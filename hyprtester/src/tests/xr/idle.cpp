#include "tests.hpp"

#ifdef WITH_XR_TESTS

#include "../../hyprctlCompat.hpp"
#include "../../Log.hpp"
#include "../../xr/xr_helpers.hpp"
#include "../shared.hpp"
#include "../clients/build.hpp"

#include <hyprutils/os/FileDescriptor.hpp>
#include <hyprutils/os/Process.hpp>

#include <array>
#include <chrono>
#include <csignal>
#include <optional>
#include <stdexcept>
#include <string>
#include <sys/poll.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace Hyprutils::OS;
using namespace Hyprutils::Memory;
#define SP CSharedPointer

// WP12 — xr_idle_inhibit (docs/openxr/06-testing.md §6 row 8, docs/openxr/05-ipc-config.md §6).
namespace {
    // Wraps the `idle-notify` client (WP12 addition, hyprtester/clients/idle-notify.cpp): binds
    // ext_idle_notifier_v1 with a 1000ms/obey-inhibitors notification and prints "idled"/
    // "resumed" lines asynchronously as they arrive. Unlike CXRClient in input.cpp, these events
    // are unsolicited (not query/response), so this wrapper polls-and-accumulates instead.
    class CIdleClient {
      public:
        CIdleClient() {
            m_proc = makeShared<CProcess>(binaryDir + "/idle-notify", std::vector<std::string>{});
            m_proc->addEnv("WAYLAND_DISPLAY", WLDISPLAY);

            int pipeFds[2];
            if (pipe(pipeFds) != 0)
                throw std::runtime_error("pipe failed");
            m_readFd = CFileDescriptor(pipeFds[0]);
            m_proc->setStdoutFD(pipeFds[1]);

            m_proc->runAsync();
            close(pipeFds[1]);

            m_fds = {.fd = m_readFd.get(), .events = POLLIN};

            // Wait for "started" so we know the notification request has actually been made
            // before the caller starts timing idle/inhibit windows.
            if (!waitForLine("started", std::chrono::milliseconds(3000)))
                throw std::runtime_error("idle-notify client did not report started");
        }

        ~CIdleClient() {
            if (m_proc) {
                kill(m_proc->pid(), SIGKILL);
                m_proc.reset();
            }
        }

        // Drains whatever is currently available (non-blocking) into the accumulator, then
        // returns whether `needle` has been seen so far.
        bool sawLine(const std::string& needle) {
            drainNonBlocking();
            return m_accum.contains(needle);
        }

        // Polls up to `budget` for `needle` to appear (drain+check on an interval).
        bool waitForLine(const std::string& needle, std::chrono::milliseconds budget) {
            const auto deadline = std::chrono::steady_clock::now() + budget;
            do {
                if (sawLine(needle))
                    return true;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            } while (std::chrono::steady_clock::now() < deadline);
            return sawLine(needle);
        }

        void clear() {
            m_accum.clear();
        }

      private:
        void drainNonBlocking() {
            while (poll(&m_fds, 1, 0) > 0 && (m_fds.revents & POLLIN)) {
                std::array<char, 512> buf{};
                const ssize_t         n = read(m_fds.fd, buf.data(), buf.size() - 1);
                if (n <= 0)
                    break;
                buf[n] = 0;
                m_accum += buf.data();
            }
        }

        SP<CProcess>     m_proc;
        CFileDescriptor  m_readFd;
        struct pollfd    m_fds{};
        std::string      m_accum;
    };
}

// xr_idle_inhibit — with openxr:inhibit_idle=1 and the session FOCUSED, idle must be inhibited
// (no `idled` within a generous window); flipping inhibit_idle to 0 must let idle fire; restoring
// it to 1 must produce `resumed` (either via the immediate config-reload recheck, or the next XR
// input activity — doc 05 §6.3/§6.4). Also cross-checks the WP12-added `inhibitingIdle` status
// field (docs §6/`XRIpc.cpp` — see the compositor-change note in the commit).
TEST_CASE(xr_idle_inhibit) {
    XR_SKIP_IF_UNAVAILABLE();

    struct SGuard {
        const bool& failed;
        std::string testName;
        ~SGuard() {
            // Always restore the default so later tests in this shared instance aren't affected.
            getFromSocket("/keyword openxr:inhibit_idle 1");
            if (failed)
                XR::dumpXrArtifacts(testName);
        }
    };
    SGuard guard{this->failed, name()};

    // Idle-inhibit is defined (doc 05 §6.1) as openxr:inhibit_idle && state == FOCUSED — VISIBLE
    // does not inhibit, so this test specifically needs FOCUSED, not the visible-fallback other
    // xr tests accept.
    if (!XR::waitForXrState("focused", std::chrono::milliseconds(15000))) {
        XR::logSkip(name(), "session never reached focused (known env instability; idle-inhibit is FOCUSED-only per doc 05 §6.1)");
        return;
    }

    // xr-test.conf doesn't override openxr:inhibit_idle, so it starts at its default (1). Make
    // that explicit and wait for the recheck so we have a known-good baseline regardless of test
    // order within the shared --xr run.
    ASSERT(getFromSocket("/keyword openxr:inhibit_idle 1"), std::string("ok"));
    ASSERT(XR::waitForJson(
               "j/openxr", [&](const std::string& r) { return r.contains("\"inhibitingIdle\": true"); }, std::chrono::milliseconds(5000)),
           true);

    std::optional<CIdleClient> client;
    try {
        client.emplace();
    } catch (const std::exception& e) {
        XR::logSkip(name(), std::string("could not spawn idle-notify client: ") + e.what());
        return;
    }

    // Inhibited: no "idled" within a window comfortably larger than the client's 1s timeout.
    EXPECT(client->waitForLine("idled", std::chrono::milliseconds(3000)), false);
    NLog::green("xr_idle_inhibit: inhibit_idle=1 + focused -> no idle notification within 3s");

    // Flip off: recheck fires from the config.props_refreshed listener (doc 05 §6.3).
    ASSERT(getFromSocket("/keyword openxr:inhibit_idle 0"), std::string("ok"));
    ASSERT(XR::waitForJson(
               "j/openxr", [&](const std::string& r) { return r.contains("\"inhibitingIdle\": false"); }, std::chrono::milliseconds(5000)),
           true);

    // Uninhibited: with no real input flowing, the 1s-timeout notification must fire.
    ASSERT(client->waitForLine("idled", std::chrono::milliseconds(5000)), true);
    NLog::green("xr_idle_inhibit: inhibit_idle=0 -> idle notification arrived");

    // Restore: re-inhibiting must resume the (already-idled) notification.
    client->clear();
    ASSERT(getFromSocket("/keyword openxr:inhibit_idle 1"), std::string("ok"));
    ASSERT(XR::waitForJson(
               "j/openxr", [&](const std::string& r) { return r.contains("\"inhibitingIdle\": true"); }, std::chrono::milliseconds(5000)),
           true);
    ASSERT(client->waitForLine("resumed", std::chrono::milliseconds(5000)), true);
    NLog::green("xr_idle_inhibit: restoring inhibit_idle=1 resumed the idle notification");
}

#endif // WITH_XR_TESTS
