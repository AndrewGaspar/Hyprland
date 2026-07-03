#include "MonadoOrchestrator.hpp"

#ifdef WITH_XR_TESTS

#include "../Log.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <csignal>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace Hyprutils::OS;
using namespace Hyprutils::Memory;

CMonadoOrchestrator::CMonadoOrchestrator() = default;

CMonadoOrchestrator::~CMonadoOrchestrator() {
    // Best-effort safety net; main() calls teardown() explicitly.
    if (m_remoteFd >= 0) {
        close(m_remoteFd);
        m_remoteFd = -1;
    }
}

bool CMonadoOrchestrator::resolveBinary() {
    // Resolution order (docs §3.1): $HYPRTESTER_MONADO_SERVICE -> the vendored
    // submodule build (subprojects/monado/build, see scripts/build-monado.sh) ->
    // monado-service in PATH.
    auto tryPath = [](const std::string& p) -> bool { return !p.empty() && std::filesystem::is_regular_file(p) && ::access(p.c_str(), X_OK) == 0; };

    // An explicit override is authoritative: if set, we use it exclusively (resolve
    // or fail — no silent fallback to a different monado). This also lets tests force
    // the unavailable leg with HYPRTESTER_MONADO_SERVICE=/nonexistent.
    if (const auto* env = getenv("HYPRTESTER_MONADO_SERVICE")) {
        if (tryPath(env)) {
            m_binary = env;
        } else {
            NLog::yellow("MonadoOrchestrator: HYPRTESTER_MONADO_SERVICE='{}' is not an executable — treating monado as unavailable", env);
            return false;
        }
    } else if (const std::string known = HYPRTESTER_SOURCE_ROOT "/subprojects/monado/build/src/xrt/targets/service/monado-service"; tryPath(known)) {
        m_binary = known;
    } else if (const auto* path = getenv("PATH"); path) {
        std::string              paths = path;
        std::string::size_type   start = 0;
        while (start <= paths.size()) {
            const auto        end = paths.find(':', start);
            const std::string dir = paths.substr(start, end == std::string::npos ? std::string::npos : end - start);
            if (!dir.empty() && tryPath(dir + "/monado-service")) {
                m_binary = dir + "/monado-service";
                break;
            }
            if (end == std::string::npos)
                break;
            start = end + 1;
        }
    }

    if (m_binary.empty())
        return false;

    // Manifest: $HYPRTESTER_MONADO_MANIFEST override, else derive from the binary path —
    // a build tree (<build>/src/xrt/targets/service/monado-service) carries
    // <build>/openxr_monado-dev.json; an installed binary (<prefix>/bin/monado-service)
    // carries <prefix>/share/openxr/1/openxr_monado.json.
    const std::string suffix = "/src/xrt/targets/service/monado-service";
    if (const auto* env = getenv("HYPRTESTER_MONADO_MANIFEST"); env)
        m_manifest = env;
    else if (m_binary.size() > suffix.size() && m_binary.ends_with(suffix))
        m_manifest = m_binary.substr(0, m_binary.size() - suffix.size()) + "/openxr_monado-dev.json";
    else
        m_manifest = std::filesystem::path(m_binary).parent_path().parent_path().string() + "/share/openxr/1/openxr_monado.json";

    if (!std::filesystem::is_regular_file(m_manifest))
        NLog::yellow("MonadoOrchestrator: runtime manifest not found at {} (XR_RUNTIME_JSON will still be passed)", m_manifest);

    return true;
}

bool CMonadoOrchestrator::pollReadiness() {
    const std::string ipcSock  = m_runtimeDir + "/monado_comp_ipc";
    const auto        deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);

    bool ipcUp = false;

    while (std::chrono::steady_clock::now() < deadline) {
        // Condition 1: monado_comp_ipc unix socket accepts (then close).
        if (!ipcUp) {
            const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
            if (fd >= 0) {
                sockaddr_un addr = {};
                addr.sun_family  = AF_UNIX;
                strncpy(addr.sun_path, ipcSock.c_str(), sizeof(addr.sun_path) - 1);
                if (connect(fd, reinterpret_cast<sockaddr*>(&addr), SUN_LEN(&addr)) == 0)
                    ipcUp = true;
                close(fd);
            }
        }

        // Condition 2: TCP 127.0.0.1:4242 accepts — keep this fd for RemoteClient.
        if (ipcUp) {
            const int fd = socket(AF_INET, SOCK_STREAM, 0);
            if (fd >= 0) {
                sockaddr_in addr = {};
                addr.sin_family  = AF_INET;
                addr.sin_port    = htons(4242);
                inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
                if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
                    m_remoteFd = fd;
                    return true;
                }
                close(fd);
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    return false;
}

bool CMonadoOrchestrator::start() {
    if (!resolveBinary()) {
        NLog::yellow("MonadoOrchestrator: monado-service not found (checked $HYPRTESTER_MONADO_SERVICE, the known build path, and $PATH) — XR suite will SKIP");
        return false;
    }

    NLog::yellow("MonadoOrchestrator: using service binary {}", m_binary);

    // Isolated-but-shared XDG_RUNTIME_DIR (docs §3.1): the caller (runXrSuite)
    // creates it, mode 0700, and points our own XDG_RUNTIME_DIR at it before
    // calling start() — so this service and the Hyprland-under-test share it.
    if (const auto* xdg = getenv("XDG_RUNTIME_DIR"); xdg)
        m_runtimeDir = xdg;
    if (m_runtimeDir.empty() || !std::filesystem::is_directory(m_runtimeDir)) {
        NLog::red("MonadoOrchestrator: XDG_RUNTIME_DIR '{}' not a directory", m_runtimeDir);
        return false;
    }

    m_logPath = m_runtimeDir + "/monado.log";

    m_proc    = makeUnique<CProcess>(m_binary, std::vector<std::string>{});
    m_proc->addEnv("XRT_COMPOSITOR_NULL", "true");
    m_proc->addEnv("P_OVERRIDE_ACTIVE_CONFIG", "remote");
    m_proc->addEnv("XRT_NO_STDIN", "1");
    m_proc->addEnv("XDG_RUNTIME_DIR", m_runtimeDir);
    if (const auto* vk = getenv("HYPRTESTER_VK_DRIVER_FILES"); vk)
        m_proc->addEnv("VK_DRIVER_FILES", vk);

    // Redirect service stdout/stderr to <run-dir>/monado.log for artifact capture.
    const int logFd = open(m_logPath.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (logFd >= 0) {
        m_proc->setStdoutFD(logFd);
        m_proc->setStderrFD(logFd);
    }

    if (!m_proc->runAsync()) {
        NLog::red("MonadoOrchestrator: failed to launch monado-service");
        if (logFd >= 0)
            close(logFd);
        return false;
    }
    if (logFd >= 0)
        close(logFd); // child holds its own dup

    NLog::yellow("MonadoOrchestrator: launched monado-service (pid {}), polling readiness...", m_proc->pid());

    if (!pollReadiness()) {
        NLog::red("MonadoOrchestrator: monado-service did not become ready within 10s — XR suite will SKIP");
        // dump a short tail for diagnosis
        std::ifstream logIn(m_logPath);
        if (logIn) {
            NLog::yellow("--- monado.log tail ---");
            std::string              line;
            std::vector<std::string> lines;
            while (std::getline(logIn, line))
                lines.push_back(line);
            const size_t from = lines.size() > 30 ? lines.size() - 30 : 0;
            for (size_t i = from; i < lines.size(); ++i)
                NLog::log("{}", lines[i]);
        }
        teardown(true);
        return false;
    }

    NLog::green("MonadoOrchestrator: monado-service ready (IPC + TCP 4242)");
    m_available = true;
    return true;
}

void CMonadoOrchestrator::teardown(bool keepArtifacts) {
    if (m_remoteFd >= 0) {
        close(m_remoteFd);
        m_remoteFd = -1;
    }

    if (m_proc) {
        const pid_t pid = m_proc->pid();
        if (pid > 0 && kill(pid, 0) == 0) {
            kill(pid, SIGTERM);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (std::chrono::steady_clock::now() < deadline) {
                if (kill(pid, 0) != 0 && errno == ESRCH)
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            if (kill(pid, 0) == 0)
                kill(pid, SIGKILL);
            waitpid(pid, nullptr, 0);
        }
        m_proc.reset();
    }

    // The run dir is owned by runXrSuite (it created it); it handles removal /
    // preservation. We only note it here for visibility.
    (void)keepArtifacts;

    m_available = false;
}

#endif // WITH_XR_TESTS
