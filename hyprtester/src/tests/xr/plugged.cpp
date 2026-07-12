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
#include <stdexcept>
#include <string>
#include <sys/poll.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace Hyprutils::OS;
using namespace Hyprutils::Memory;
#define SP CSharedPointer

// research/18 WP-M4 — plugged-state round-trip on the session-EXISTENCE edge. These tests pin
// openxr:monitors_follow_session = session so the round-trip is deterministic regardless of the
// null/remote runtime's visibility behavior: `openxr disable` UNPLUGS the XR monitors (workspaces
// evacuate to the remaining monitor, layers + anchors persist), `openxr enable` re-plugs them
// (workspaces return by name). The suite's Hyprland boots WITH the runtime available, so the
// sessionless state is driven via the disable/enable edge — the same setMonitorsPlugged() path
// init() takes on a runtime-less boot.
//
// report-18 addendum: the SHIPPED default is `visible` (a doffed/standby headset reads as
// unplugged, after openxr:monitor_unplug_grace_ms). That visibility gating + the anti-flap grace
// cannot be exercised here — the monado null/remote driver cannot script headset don/doff
// visibility transitions — so they are covered purely by the pure-logic gtests in tests/xr/
// plugged.cpp (wantXRMonitorsPlugged / parseMonitorFollowMode truth tables). These suite tests
// therefore pin `session` mode, which shares the exact same apply path (updateMonitorsPlugged ->
// setMonitorsPlugged -> onConnect/onDisconnect).
namespace {
    bool gateUp() {
        if (XR::waitForXrState("focused", std::chrono::milliseconds(15000)))
            return true;
        return XR::waitForXrState("visible", std::chrono::milliseconds(2000));
    }

    // Minimal windowed client (the pointer-scroll test client maps one toplevel), modeled on
    // input.cpp's CXRClient: it exists so the evacuated workspace holds a window and therefore
    // survives the unplug->replug cycle (an empty inactive workspace would be culled).
    class CPluggedClient {
      public:
        explicit CPluggedClient(const std::string& monitorName) {
            if (getFromSocket("/dispatch focusmonitor " + monitorName) != "ok")
                throw std::runtime_error("focusmonitor failed");

            m_proc = makeShared<CProcess>(binaryDir + "/pointer-scroll", std::vector<std::string>{});
            m_proc->addEnv("WAYLAND_DISPLAY", WLDISPLAY);

            int pipeFdsIn[2], pipeFdsOut[2];
            if (pipe(pipeFdsIn) != 0 || pipe(pipeFdsOut) != 0)
                throw std::runtime_error("pipe failed");

            m_writeFd = CFileDescriptor(pipeFdsIn[1]);
            m_proc->setStdinFD(pipeFdsIn[0]);
            m_readFd = CFileDescriptor(pipeFdsOut[0]);
            m_proc->setStdoutFD(pipeFdsOut[1]);

            const int before = Tests::windowCount();
            m_proc->runAsync();
            close(pipeFdsIn[0]);
            close(pipeFdsOut[1]);

            struct pollfd fds = {.fd = m_readFd.get(), .events = POLLIN};
            if (poll(&fds, 1, 2000) != 1 || !(fds.revents & POLLIN))
                throw std::runtime_error("client did not start");
            std::array<char, 256> buf{};
            if (read(m_readFd.get(), buf.data(), buf.size() - 1) == -1)
                throw std::runtime_error("client read failed");
            if (!std::string{buf.data()}.contains("started"))
                throw std::runtime_error("client did not report started");

            int counter = 0;
            while (Tests::processAlive(m_proc->pid()) && Tests::windowCount() == before) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (++counter > 50)
                    throw std::runtime_error("client window took too long to open");
            }
        }

        ~CPluggedClient() {
            if (m_proc) {
                std::string cmd = "exit\n";
                write(m_writeFd.get(), cmd.c_str(), cmd.length());
                kill(m_proc->pid(), SIGKILL); // tracked PID only — never by name
                m_proc.reset();
            }
        }

        pid_t pid() const {
            return m_proc ? m_proc->pid() : -1;
        }

      private:
        SP<CProcess>    m_proc;
        CFileDescriptor m_readFd, m_writeFd;
    };
}

// xr_plugged_follow_session — the WP-M4 evacuation/return round-trip on the declared fixture
// XR-conf-a, with a real window riding the workspace.
TEST_CASE(xr_plugged_follow_session) {
    XR_SKIP_IF_UNAVAILABLE();

    struct SGuard {
        const bool& failed;
        std::string testName;
        ~SGuard() {
            // Always leave the shared instance with a running session AND the shipped default mode
            // for later tests.
            getFromSocket("/openxr enable");
            XR::waitForXrState("focused", std::chrono::milliseconds(15000));
            getFromSocket("/keyword openxr:monitors_follow_session visible");
            if (failed)
                XR::dumpXrArtifacts(testName);
        }
    };
    SGuard guard{this->failed, name()};

    if (!gateUp()) {
        XR::logSkip(name(), "session never reached focused/visible (known env instability)");
        return;
    }

    // Pin existence-gating (see the file header) so the disable/enable round-trip below is
    // deterministic; the session is focused here, so this keeps the monitors plugged.
    ASSERT(getFromSocket("/keyword openxr:monitors_follow_session session"), std::string("ok"));

    // (a) session up => the declared fixture is PLUGGED: in the default j/monitors list, and
    //     status reports plugged: true.
    ASSERT(XR::waitForJson(
               "j/monitors", [](const std::string& r) { return r.contains("\"name\": \"XR-conf-a\""); }, std::chrono::milliseconds(10000)),
           true);
    {
        const std::string status = getFromSocket("j/openxr");
        const auto        posA   = XR::findAfter(status, "\"name\": \"XR-conf-a\"");
        ASSERT_NOT(posA, std::string::npos);
        EXPECT(XR::fieldAfter(status, posA, "plugged"), std::string("true"));
    }

    // (a2) nudge the anchor away from its DECLARED pose so the persistence check below proves
    //      live anchor STATE survives the cycle (a re-seed from the declaration would lose the
    //      move; reconcile only re-anchors when the declared spec itself changed).
    std::string posMoved;
    ASSERT(getFromSocket("/openxr select XR-conf-a"), std::string("ok"));
    ASSERT(getFromSocket("/openxr move 0.25 0 0"), std::string("ok"));
    {
        const std::string status = getFromSocket("j/openxr");
        const auto        posA   = XR::findAfter(status, "\"name\": \"XR-conf-a\"");
        ASSERT_NOT(posA, std::string::npos);
        posMoved = XR::fieldAfter(status, posA, "pos");
        ASSERT_NOT(posMoved, std::string(""));
    }

    // (b) park a window on XR-conf-a's active workspace and remember that workspace's id.
    //     (getMonitorData emits the monitor's own "id" BEFORE "name", so the first "id" after
    //     the name marker is activeWorkspace.id.) The client is BEST-EFFORT: some headless host
    //     environments cannot spawn the test clients at all (the same pre-existing limitation
    //     that SKIPs xr_scroll/xr_menu_right_click there) — without it the unplug/replug and
    //     evacuation assertions below still run in full; only the window-riding workspace-return
    //     check (which needs a window to keep the workspace alive while parked) is gated on it.
    std::string wsId;
    {
        const std::string mons = getFromSocket("j/monitors");
        const auto        posA = XR::findAfter(mons, "\"name\": \"XR-conf-a\"");
        ASSERT_NOT(posA, std::string::npos);
        wsId = XR::fieldAfter(mons, posA, "id");
        ASSERT_NOT(wsId, std::string(""));
    }

    std::unique_ptr<CPluggedClient> client;
    try {
        client = std::make_unique<CPluggedClient>("XR-conf-a");
    } catch (const std::exception& e) {
        NLog::yellow("xr_plugged_follow_session: no windowed client ({}) — running the round-trip without the workspace-return check", e.what());
    }
    const int windowsWithClient = Tests::windowCount();

    // (c) UNPLUG: stop the session. The declared monitors must leave the default monitor list
    //     (workspace placement set) but keep their layer records; the workspace evacuates.
    ASSERT(getFromSocket("/openxr disable"), std::string("ok"));
    ASSERT(XR::waitForXrState("disabled", std::chrono::milliseconds(10000)), true);

    ASSERT(XR::waitForJson(
               "j/monitors", [](const std::string& r) { return !r.contains("\"name\": \"XR-conf-a\"") && !r.contains("\"name\": \"XR-conf-b\""); },
               std::chrono::milliseconds(10000)),
           true);

    // Lingering-but-disabled in `monitors all` (research/18 §5 — same as `monitor=...,disable`).
    {
        const std::string all  = getFromSocket("j/monitors all");
        const auto        posA = XR::findAfter(all, "\"name\": \"XR-conf-a\"");
        ASSERT_NOT(posA, std::string::npos);
        EXPECT(XR::fieldAfter(all, posA, "disabled"), std::string("true"));
    }

    // Layers persist across the unplug (option-b keep) and report plugged: false.
    {
        const std::string status = getFromSocket("j/openxr");
        const auto        posA   = XR::findAfter(status, "\"name\": \"XR-conf-a\"");
        ASSERT_NOT(posA, std::string::npos);
        EXPECT(XR::fieldAfter(status, posA, "plugged"), std::string("false"));
    }

    // Workspaces evacuated: nothing may still sit on an unplugged monitor; the client (if any)
    // survived the evacuation.
    ASSERT(XR::waitForJson(
               "j/workspaces", [](const std::string& r) { return !r.contains("\"monitor\": \"XR-conf-a\"") && !r.contains("\"monitor\": \"XR-conf-b\""); },
               std::chrono::milliseconds(10000)),
           true);
    if (client) {
        EXPECT(Tests::windowCount(), windowsWithClient);
        EXPECT(Tests::processAlive(client->pid()), true);
    }

    // (d) REPLUG: start the session again. The monitors re-enter j/monitors and — when a window
    //     kept it alive — the remembered workspace returns to XR-conf-a by name.
    ASSERT(getFromSocket("/openxr enable"), std::string("ok"));
    ASSERT(XR::waitForXrState("focused", std::chrono::milliseconds(15000)) || XR::waitForXrState("visible", std::chrono::milliseconds(2000)), true);

    ASSERT(XR::waitForJson(
               "j/monitors", [](const std::string& r) { return r.contains("\"name\": \"XR-conf-a\"") && r.contains("\"name\": \"XR-conf-b\""); },
               std::chrono::milliseconds(10000)),
           true);
    {
        const std::string status = getFromSocket("j/openxr");
        const auto        posA   = XR::findAfter(status, "\"name\": \"XR-conf-a\"");
        ASSERT_NOT(posA, std::string::npos);
        EXPECT(XR::fieldAfter(status, posA, "plugged"), std::string("true"));
        // Anchor state survived the whole unplug/replug cycle (research/18 §6.2: option (b)
        // keeps the layer, so the verb-moved pose — NOT the declared one — must still be live).
        EXPECT(XR::fieldAfter(status, posA, "pos"), posMoved);
    }

    if (client) {
        // The workspace held its window through the cycle and returned by name (m_lastMonitor
        // tag + remembered-workspace map, Monitor.cpp onConnect).
        ASSERT(XR::waitForJson(
                   "j/monitors",
                   [&](const std::string& r) {
                       const auto posA = XR::findAfter(r, "\"name\": \"XR-conf-a\"");
                       return posA != std::string::npos && XR::fieldAfter(r, posA, "id") == wsId;
                   },
                   std::chrono::milliseconds(10000)),
               true);
        EXPECT(Tests::windowCount(), windowsWithClient);
        NLog::green("xr_plugged_follow_session: unplug evacuated workspace {} (window intact), replug returned it to XR-conf-a", wsId);
    } else
        NLog::green("xr_plugged_follow_session: unplug/replug round-trip verified (no windowed client in this environment — workspace-return check not exercised)");

    // Undo the (a2) nudge so later tests see the declared pose.
    getFromSocket("/openxr select XR-conf-a");
    getFromSocket("/openxr move -0.25 0 0");
}

// xr_plugged_create_while_sessionless — research/18 WP-M3: a monitor created while NO session
// exists (here: after `openxr disable`; same code path as an xrmonitor declared on a
// runtime-less boot) comes up UNPLUGGED, and plugs in on the next session start.
TEST_CASE(xr_plugged_create_while_sessionless) {
    XR_SKIP_IF_UNAVAILABLE();

    const std::string mon = XR::monitorName(60);

    struct SGuard {
        const bool& failed;
        std::string testName;
        std::string monitorName;
        ~SGuard() {
            getFromSocket("/openxr enable");
            XR::waitForXrState("focused", std::chrono::milliseconds(15000));
            if (!monitorName.empty())
                getFromSocket("/openxr destroy " + monitorName);
            getFromSocket("/keyword openxr:monitors_follow_session visible");
            if (failed)
                XR::dumpXrArtifacts(testName);
        }
    };
    SGuard guard{this->failed, name(), ""};

    if (!gateUp()) {
        XR::logSkip(name(), "session never reached focused/visible (known env instability)");
        return;
    }

    // Pin existence-gating (see the file header) so a sessionless create is deterministically
    // unplugged and a session start plugs it in, independent of the null runtime's visibility.
    ASSERT(getFromSocket("/keyword openxr:monitors_follow_session session"), std::string("ok"));

    ASSERT(getFromSocket("/openxr disable"), std::string("ok"));
    ASSERT(XR::waitForXrState("disabled", std::chrono::milliseconds(10000)), true);

    ASSERT(getFromSocket("/openxr create " + mon + " 1280x720"), std::string("ok"));
    guard.monitorName = mon;

    // Sessionless create => unplugged: absent from j/monitors, disabled in `monitors all`,
    // layer present with plugged: false.
    EXPECT_NOT_CONTAINS(getFromSocket("j/monitors"), "\"name\": \"" + mon + "\"");
    {
        const std::string all = getFromSocket("j/monitors all");
        const auto        pos = XR::findAfter(all, "\"name\": \"" + mon + "\"");
        ASSERT_NOT(pos, std::string::npos);
        EXPECT(XR::fieldAfter(all, pos, "disabled"), std::string("true"));
    }
    {
        const std::string status = getFromSocket("j/openxr");
        const auto        pos    = XR::findAfter(status, "\"name\": \"" + mon + "\"");
        ASSERT_NOT(pos, std::string::npos);
        EXPECT(XR::fieldAfter(status, pos, "plugged"), std::string("false"));
    }

    // Session start plugs it in.
    ASSERT(getFromSocket("/openxr enable"), std::string("ok"));
    ASSERT(XR::waitForXrState("focused", std::chrono::milliseconds(15000)) || XR::waitForXrState("visible", std::chrono::milliseconds(2000)), true);
    ASSERT(XR::waitForJson(
               "j/monitors", [&](const std::string& r) { return r.contains("\"name\": \"" + mon + "\""); }, std::chrono::milliseconds(10000)),
           true);
    {
        const std::string status = getFromSocket("j/openxr");
        const auto        pos    = XR::findAfter(status, "\"name\": \"" + mon + "\"");
        ASSERT_NOT(pos, std::string::npos);
        EXPECT(XR::fieldAfter(status, pos, "plugged"), std::string("true"));
    }

    ASSERT(getFromSocket("/openxr destroy " + mon), std::string("ok"));
    guard.monitorName.clear();
    ASSERT(XR::waitForJson(
               "j/monitors", [&](const std::string& r) { return !r.contains("\"name\": \"" + mon + "\""); }, std::chrono::milliseconds(10000)),
           true);

    NLog::green("xr_plugged_create_while_sessionless: sessionless create came up unplugged; session start plugged it in");
}

#endif // WITH_XR_TESTS
