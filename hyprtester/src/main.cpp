
// This is a tester for Hyprland. It will launch the built binary in ./build/Hyprland
// in headless mode and test various things.
// for now it's quite basic and limited, but will be expanded in the future.

// NOTE: This tester has to be ran from its directory!!

// Some TODO:
// - Add a plugin built alongside so that we can do more detailed tests (e.g. simulating keystrokes)
// - test coverage
// - maybe figure out a way to do some visual tests too?

// Required runtime deps for checks:
// - kitty
// - xeyes

#define INCLUDED_FROM_MAIN 1 // Prevent macro redefinition warnings from includes of "tests/*/tests.hpp"

#include "shared.hpp"
#include "hyprctlCompat.hpp"
#include "tests/main/tests.hpp"
#include "tests/clients/tests.hpp"
#include "tests/misc/tests.hpp"
#include "tests/shared.hpp"

#ifdef WITH_XR_TESTS
#include "tests/xr/tests.hpp"
#include "xr/MonadoOrchestrator.hpp"
#include "xr/RemoteClient.hpp"
#include "xr/xr_helpers.hpp"
#endif

#include <hyprutils/os/Process.hpp>
#include <hyprutils/memory/WeakPtr.hpp>
#include <hyprutils/memory/Casts.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <memory>
#include <print>
#include <string>
#include <string_view>
#include <span>
#include <thread>
#include <vector>

#include "Log.hpp"

using namespace Hyprutils::OS;
using namespace Hyprutils::Memory;
using Path = std::filesystem::path;

#define SP CSharedPointer

namespace {
    struct SSettings {
        Path                     configPath;
        Path                     binaryPath;
        Path                     pluginPath;
        std::vector<std::string> requestedTests;
        bool                     xrMode = false;
    };

    struct STestsRunResult {
        unsigned long long       total;
        std::vector<std::string> failedNames;
    };
}

static SP<CProcess> hyprlandProc;

static bool         launchHyprland(Path configPath, Path binaryPath, const std::vector<std::pair<std::string, std::string>>& env = {}, bool headlessOnly = true) {
    NLog::yellow("Launching Hyprland");
    hyprlandProc = makeShared<CProcess>(binaryPath, std::vector<std::string>{"--config", configPath});
    if (headlessOnly)
        hyprlandProc->addEnv("HYPRLAND_HEADLESS_ONLY", "1");
    for (const auto& [k, v] : env)
        hyprlandProc->addEnv(k, v);

    NLog::yellow("Launched async process");

    return hyprlandProc->runAsync();
}

static bool hyprlandAlive() {
    NLog::yellow("hyprlandAlive");
    return kill(hyprlandProc->pid(), 0) == 0 || errno != ESRCH;
}

[[noreturn]] static void helpAndDie(int exit_code) {
    NLog::log("usage: hyprtester [--OPTION [VALUE]]... [TEST_NAMES].\n");
    NLog::log(R"(Arguments:
    --help              -h         - Show this message again
    --config FILE       -c FILE    - Specify config file to use (default: './test.lua')
    --binary FILE       -b FILE    - Specify Hyprland binary to use (default: '../build/Hyprland')
    --plugin FILE       -p FILE    - Specify the location of the test plugin (default: './')
    [TEST_NAMES]                   - Specify list of tests to run (separated by spaces).
                                     If omitted, all tests will run.)");

    std::exit(exit_code);
}

static Path validatePathOrDie(Path path) {
    try {
        if (!std::filesystem::is_regular_file(path)) {
            throw std::exception();
        }
    } catch (...) {
        std::println(stderr, "[ ERROR ] File '{}' is not accessible or not a regular file", path.string());
        helpAndDie(EXIT_FAILURE);
    }
    return path;
}

static SSettings parseSettings(const std::span<const char*> args) {
    static const auto cwd = std::filesystem::current_path();
    SSettings         settings{};

    for (auto it = args.begin(); it < args.end(); it++) {
        std::string_view value = *it;
        if (value == "--config" || value == "-c") {
            if (std::next(it) == args.end()) {
                helpAndDie(EXIT_FAILURE);
            }

            settings.configPath = validatePathOrDie(*std::next(it));
            it++;
        } else if (value == "--binary" || value == "-b") {
            if (std::next(it) == args.end()) {
                helpAndDie(EXIT_FAILURE);
            }

            settings.binaryPath = validatePathOrDie(*std::next(it));
            it++;
        } else if (value == "--plugin" || value == "-p") {
            if (std::next(it) == args.end()) {
                helpAndDie(EXIT_FAILURE);
            }

            settings.pluginPath = validatePathOrDie(*std::next(it));
            it++;
        } else if (value == "--xr") {
            settings.xrMode = true;
        } else if (value == "--help" || value == "-h") {
            helpAndDie(EXIT_SUCCESS);
        } else if (!value.starts_with("-")) {
            settings.requestedTests.emplace_back(value);
        } else {
            std::println(stderr, "[ ERROR ] Unknown option '{}' !", *it);
            helpAndDie(EXIT_FAILURE);
        }
    }

    // Default options
    if (settings.configPath.empty())
        settings.configPath = validatePathOrDie(cwd / (settings.xrMode ? "xr-test.conf" : "test.lua"));
    if (settings.binaryPath.empty())
        settings.binaryPath = validatePathOrDie(cwd / "../build/Hyprland");
    if (settings.pluginPath.empty())
        settings.pluginPath = cwd;

    return settings;
}

// Half of the resets go through test.lua's `hl.dsp.*` dispatchers; a classic hyprlang config
// (--xr's xr-test.conf, the stereo front-end run's stereo-legacy.conf) has none of them, so those
// runs skip that half.
static bool preTestCleanup(bool nonLuaConfig = false) {
    bool failed = false;

    if (!Tests::killAllWindows()) {
        NLog::red("Internal failure: failed to kill all windows");
        failed = true;
    }
    if (!Tests::killAllLayers()) {
        NLog::red("Internal failure: failed to kill all layers");
        failed = true;
    }
    if (getFromSocket("/reload") != "ok") {
        NLog::red("Internal failure: failed to reload");
        failed = true;
    }

    // The remaining resets use Lua dispatchers (hl.dsp.*) that only exist under the
    // standard test.lua config; a classic hyprlang config has none of them, so skip them.
    if (nonLuaConfig)
        return !failed;

    if (!getFromSocket("/activeworkspace").contains("workspace ID 1 (1)")) {
        if (getFromSocket("/dispatch hl.dsp.focus({ workspace = '1' })") != "ok") {
            NLog::red("Internal failure: failed to switch to workspace 1");
            failed = true;
        }
    }
    if (getFromSocket("/dispatch hl.dsp.cursor.move({ x = 960, y = 540 })") != "ok") {
        NLog::red("Internal failure: failed to reset cursor position");
        failed = true;
    }

    return !failed;
}

static STestsRunResult runTests(std::vector<std::shared_ptr<CTestCase>>& testCases, bool nonLuaConfig = false) {
    struct STestsRunResult res{.total = testCases.size(), .failedNames = {}};

    for (auto& tc : testCases) {
        // Clean up before every test
        NLog::yellow("Cleaning up");

        if (!preTestCleanup(nonLuaConfig)) { // damn it, something really went wrong
            if (nonLuaConfig) {
                NLog::red("pre-test cleanup failed; continuing (best-effort)");
            } else
                std::exit(1);
        }

        NLog::log("{}Running test {}", Colors::BLUE, tc->name());
        tc->test();

        if (tc->failed) {
            NLog::red("Test failed!: {}", tc->name());
            res.failedNames.emplace_back(std::format("{}:{}", tc->groupName(), tc->name()));
        } else
            NLog::green("Test passed: {}", tc->name());
    }

    return res;
}

static void cleanupAndReport(const STestsRunResult& tInfo, bool nonLuaConfig = false) {
    NLog::green("dispatching exit");
    getFromSocket(nonLuaConfig ? "/dispatch exit" : "/dispatch hl.dsp.exit()");

    NLog::log("\nSummary:\n\tPASSED: {}{}{}/{}", Colors::GREEN, tInfo.total - tInfo.failedNames.size(), Colors::RESET, tInfo.total);
    NLog::log("\tFAILED: {}{}{}/{}", Colors::RED, tInfo.failedNames.size(), Colors::RESET, tInfo.total);
    if (!tInfo.failedNames.empty()) {
        NLog::red("Failed tests:");
        for (const auto& name : tInfo.failedNames) {
            NLog::red("\t- {}", name);
        }
    }

    kill(hyprlandProc->pid(), SIGKILL);
    hyprlandProc.reset();
}

#ifdef WITH_XR_TESTS

// Poll for the Hyprland-under-test to register an instance (its lock lands under
// XDG_RUNTIME_DIR/hypr). Fills HIS/WLDISPLAY. Returns false if it dies or times out.
static bool waitForHyprlandInstance(int timeoutSec) {
    for (int i = 0; i < timeoutSec * 10; ++i) {
        if (!hyprlandProc || (kill(hyprlandProc->pid(), 0) != 0 && errno == ESRCH)) {
            NLog::red("Hyprland process died during startup");
            return false;
        }
        const auto INSTANCES = instances();
        if (!INSTANCES.empty()) {
            HIS       = INSTANCES.back().id;
            WLDISPLAY = INSTANCES.back().wlSocket;
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

static void killHyprlandProc() {
    if (hyprlandProc) {
        if (kill(hyprlandProc->pid(), 0) == 0)
            kill(hyprlandProc->pid(), SIGKILL);
        // reap: CProcess detaches on runAsync, so just give it a moment
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        hyprlandProc.reset();
    }
}

// The whole `--xr` run: bring up Monado (optional), bring up a Hyprland with the
// XR runtime pointed at it, run the xr test group, tear everything down.
static int runXrSuite(const SSettings& settings) {
    static CMonadoOrchestrator orchestrator;
    static CRemoteClient       remote;

    // Isolated-but-shared XDG_RUNTIME_DIR for BOTH monado and Hyprland (docs §3.1).
    // We create it and point our own env at it so getFromSocket()/instances() also
    // resolve against it (they read $XDG_RUNTIME_DIR live).
    const std::string origRuntimeDir = getenv("XDG_RUNTIME_DIR") ? getenv("XDG_RUNTIME_DIR") : "";
    const std::string origWayland    = getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "";
    const std::string runDir         = "/tmp/hyprtester-xr-" + std::to_string(getpid());

    std::error_code ec;
    std::filesystem::remove_all(runDir, ec);
    std::filesystem::create_directories(runDir, ec);
    chmod(runDir.c_str(), 0700);
    setenv("XDG_RUNTIME_DIR", runDir.c_str(), 1);

    XR::g_ctx.runId      = std::format("xr-{}-{}", getpid(), sc<long long>(std::time(nullptr)));
    XR::g_ctx.runtimeDir = runDir;

    // Launch through a generated wrapper config so machine-specific knobs stay out of the
    // tracked xr-test.conf (docs §6.5): the tracked config, then an optional untracked
    // hyprtester/xr-test-local.conf, then an optional $HYPRTESTER_XR_GPU pin (dual-GPU boxes:
    // Hyprland's XR GPU must match the one monado's compositor picks, or swapchain creation
    // crosses GPUs and crashes inside monado).
    const Path launchConfig = Path(runDir) / "xr-wrapper.conf";
    {
        const std::string localConf = std::string(HYPRTESTER_SOURCE_ROOT) + "/hyprtester/xr-test-local.conf";
        std::ofstream     wrap(launchConfig);
        wrap << "source = " << std::filesystem::absolute(settings.configPath).string() << "\n";
        if (std::filesystem::is_regular_file(localConf))
            wrap << "source = " << localConf << "\n";
        if (const auto* gpu = getenv("HYPRTESTER_XR_GPU"); gpu && *gpu)
            wrap << "openxr:gpu = " << gpu << "\n";
    }

    const bool monadoUp = orchestrator.start();

    std::vector<std::pair<std::string, std::string>> hlEnv;

    if (monadoUp) {
        XR::g_ctx.monadoLog       = orchestrator.logPath();
        XR::g_ctx.runtimeProvided = true;
        XR::g_ctx.runtimeManifest = orchestrator.runtimeManifest();
        hlEnv.emplace_back("XR_RUNTIME_JSON", orchestrator.runtimeManifest());

        // Validate the remote wire ABI (runtime half of drift protection, §4.2).
        const int fd = orchestrator.takeRemoteFd();
        if (fd >= 0 && remote.connectAndValidate(fd)) {
            XR::g_ctx.remote    = &remote;
            XR::g_ctx.available = true;
        } else {
            XR::g_ctx.wireMismatch = true; // suite SKIPs (not fails)
            NLog::yellow("XR: remote wire validation failed — suite will SKIP");
        }
    } else {
        // No monado: launch Hyprland WITHOUT XR_RUNTIME_JSON so xr_runtime_absent can
        // assert the graceful-unavailable path; all other xr tests SKIP.
        XR::g_ctx.runtimeProvided = false;
        XR::g_ctx.available       = false;
        XR::g_ctx.skipReason      = "monado-service not found";
    }

    // --- Bring up Hyprland. First try the stock headless-only path in the isolated
    // runtime dir; if that fails to come up, fall back to nesting under the host
    // Wayland session (symlink its socket in) — nested startup is occasionally racy,
    // so retry it. (See WP2 notes / docs §6 caveat.)
    bool up = launchHyprland(launchConfig, settings.binaryPath, hlEnv, /*headlessOnly*/ true) && waitForHyprlandInstance(15);

    if (!up) {
        NLog::yellow("XR: stock headless launch did not come up; trying nested-Wayland fallback");
        killHyprlandProc();

        if (!origRuntimeDir.empty() && !origWayland.empty()) {
            for (const std::string suffix : {std::string(""), std::string(".lock")}) {
                const std::string src = origRuntimeDir + "/" + origWayland + suffix;
                const std::string dst = runDir + "/" + origWayland + suffix;
                std::filesystem::remove(dst, ec);
                if (std::filesystem::exists(src))
                    std::filesystem::create_symlink(src, dst, ec);
            }
        }

        auto nestedEnv = hlEnv;
        if (!origWayland.empty())
            nestedEnv.emplace_back("WAYLAND_DISPLAY", origWayland);

        for (int attempt = 0; attempt < 2 && !up; ++attempt) {
            NLog::yellow("XR: nested launch attempt {}", attempt + 1);
            up = launchHyprland(launchConfig, settings.binaryPath, nestedEnv, /*headlessOnly*/ false) && waitForHyprlandInstance(15);
            if (!up)
                killHyprlandProc();
        }
    }

    if (!up) {
        NLog::red("XR: Hyprland failed to launch in both headless and nested modes");
        orchestrator.teardown(true);
        NLog::red("XR: run dir preserved: {}", runDir);
        return 1;
    }

    NLog::green("XR: Hyprland up (HIS={})", HIS);

    // Select tests: requested subset, else the whole xr group.
    std::vector<std::shared_ptr<CTestCase>> cases;
    if (!settings.requestedTests.empty()) {
        for (const auto& t : settings.requestedTests) {
            if (testCases.contains(t))
                cases.push_back(testCases.at(t));
            else {
                NLog::red("ERROR: Unknown test name '{}'", t);
                killHyprlandProc();
                orchestrator.teardown(false);
                std::filesystem::remove_all(runDir, ec);
                return EXIT_FAILURE;
            }
        }
    } else
        cases = xrTestCases;

    STestsRunResult result = runTests(cases, /*nonLuaConfig*/ true);

    // Report + teardown.
    NLog::green("dispatching exit");
    getFromSocket("/dispatch exit");
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    killHyprlandProc();

    const bool anyFailed = !result.failedNames.empty();
    orchestrator.teardown(anyFailed);

    NLog::log("\nSummary:\n\tPASSED: {}{}{}/{}", Colors::GREEN, result.total - result.failedNames.size(), Colors::RESET, result.total);
    NLog::log("\tFAILED: {}{}{}/{}", Colors::RED, result.failedNames.size(), Colors::RESET, result.total);
    if (anyFailed) {
        NLog::red("Failed tests:");
        for (const auto& n : result.failedNames)
            NLog::red("\t- {}", n);
        NLog::red("XR: run dir preserved for inspection: {}", runDir);
    } else
        std::filesystem::remove_all(runDir, ec);

    return anyFailed ? 1 : 0;
}

#endif // WITH_XR_TESTS

int main(int argc, char** argv, char** envp) {

    std::span<const char*>                  args{const_cast<const char**>(argv + 1), sc<std::size_t>(argc - 1)};
    const SSettings                         settings = parseSettings(args);

#ifdef WITH_XR_TESTS
    if (settings.xrMode)
        return runXrSuite(settings);
#else
    if (settings.xrMode) {
        NLog::red("--xr requires building with -DWITH_XR_TESTS=ON");
        return EXIT_FAILURE;
    }
#endif

    std::vector<std::shared_ptr<CTestCase>> requestedTestCases;
    for (auto& test : settings.requestedTests) {
        if (testCases.contains(test)) {
            requestedTestCases.push_back(testCases.at(test));
        } else {
            NLog::red("ERROR: Unknown test name '{}'", Colors::RED, test);
            return EXIT_FAILURE;
        }
    }
    if (requestedTestCases.empty()) {
        // When no tests are explicitly requested, run all tests.
        // For convenience of log inspection, run tests group by group.
        requestedTestCases = miscTestCases;
        std::ranges::copy(clientTestCases, std::back_inserter(requestedTestCases));
        std::ranges::copy(mainTestCases, std::back_inserter(requestedTestCases));
    }

    // Which front-end parsed the config decides what the harness may dispatch: the `hl.dsp.*`
    // resets exist only under test.lua. `-c ./stereo-legacy.conf` runs the same tests against the
    // classic monitor= / monitorv2 front-ends (research/24 §3.10).
    const bool NONLUACONFIG = settings.configPath.extension() != ".lua";

    NLog::yellow("launching hl");
    if (!launchHyprland(settings.configPath, settings.binaryPath)) {
        NLog::red("well it failed");
        return 1;
    }

    // hyprland has launched, let's check if it's alive after 10s
    std::this_thread::sleep_for(std::chrono::milliseconds(10000));
    NLog::yellow("slept for 10s");
    if (!hyprlandAlive()) {
        NLog::red("Hyprland failed to launch!");
        return 1;
    }

    // wonderful, we are in. Let's get the instance signature.
    NLog::yellow("trying to get INSTANCES");
    const auto INSTANCES = instances();
    if (INSTANCES.empty()) {
        NLog::red("Hyprland failed to launch (2)");
        return 1;
    }

    HIS       = INSTANCES.back().id;
    WLDISPLAY = INSTANCES.back().wlSocket;

    NLog::yellow("trying to get create headless output");
    const auto CREATE_HEADLESS_2 = getFromSocket("/output create headless HEADLESS-2");
    if (CREATE_HEADLESS_2 != "ok" && CREATE_HEADLESS_2 != "Name already taken") {
        NLog::red("Failed to create HEADLESS-2: {}", CREATE_HEADLESS_2);
        getFromSocket("/dispatch hl.dsp.exit()");
        return 1;
    }

    NLog::yellow("trying to load plugin");
    if (const auto R = getFromSocket(std::format("/plugin load {}", settings.pluginPath.string())); R != "ok") {
        NLog::red("Failed to load the test plugin: {}", R);
        getFromSocket("/dispatch hl.dsp.exit()");
        return 1;
    }

    NLog::yellow("Loaded plugin");

    STestsRunResult result = runTests(requestedTestCases, NONLUACONFIG);

    cleanupAndReport(result, NONLUACONFIG);

    return result.failedNames.size() > 0;
}
