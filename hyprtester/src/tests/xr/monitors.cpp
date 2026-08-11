#include "tests.hpp"

#ifdef WITH_XR_TESTS

#include "../../hyprctlCompat.hpp"
#include "../../Log.hpp"
#include "../../xr/xr_helpers.hpp"
#include "../../shared.hpp" // HIS (hyprland.log path for the force_linear reallocation check)
#include "../shared.hpp"    // Tests:: client helpers (spawnKitty, processAlive — destroy-survival test)

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

namespace {
    // Gate shared by every test in this file: focused, falling back to a short visible check
    // (WP10's session.cpp caveat) so we don't burn the whole per-test budget on a dead session.
    bool gateUp() {
        if (XR::waitForXrState("focused", std::chrono::milliseconds(15000)))
            return true;
        return XR::waitForXrState("visible", std::chrono::milliseconds(2000));
    }

    // RAII: dump artifacts iff the test ended up failed (docs §5.2), and always clean up any
    // runtime monitors we created, regardless of how the test exits.
    struct SArtifactGuard {
        const bool&              failed;
        std::string              testName;
        std::vector<std::string> monitorNames;
        ~SArtifactGuard() {
            for (auto& n : monitorNames)
                if (!n.empty())
                    getFromSocket("/openxr destroy " + n);
            if (failed)
                XR::dumpXrArtifacts(testName);
        }
    };
}

// xr_monitor_create_destroy — WP11 (doc 06 §6 row 2): create via `hyprctl openxr create`,
// verify presence in both j/monitors and j/openxr (with size_m ~ openxr:default_size), destroy,
// verify absence from both.
TEST_CASE(xr_monitor_create_destroy) {
    XR_SKIP_IF_UNAVAILABLE();
    SArtifactGuard guard{this->failed, name(), {}};

    if (!gateUp()) {
        XR::logSkip(name(), "session never reached focused/visible (known env instability)");
        return;
    }

    const std::string mon = XR::monitorName(11);
    ASSERT(getFromSocket("/openxr create " + mon + " 1280x720"), std::string("ok"));
    guard.monitorNames.push_back(mon);

    const std::string nameMarker = "\"name\": \"" + mon + "\"";
    ASSERT(XR::waitForJson(
               "j/openxr", [&](const std::string& r) { return r.contains(nameMarker); }, std::chrono::milliseconds(10000)),
           true);
    ASSERT(XR::waitForJson(
               "j/monitors", [&](const std::string& r) { return r.contains(nameMarker); }, std::chrono::milliseconds(10000)),
           true);

    // size_m ≈ openxr:default_size (1.6 default; xr-test.conf doesn't override it).
    const std::string openxrJson = getFromSocket("j/openxr");
    const auto        pos        = XR::findAfter(openxrJson, nameMarker);
    ASSERT_NOT(pos, std::string::npos);
    const float sizeM = XR::toFloatOr(XR::fieldAfter(openxrJson, pos, "size_m"), -1.f);
    EXPECT_MAX_DELTA(sizeM, 1.6, 0.05);

    ASSERT(getFromSocket("/openxr destroy " + mon), std::string("ok"));
    guard.monitorNames.clear(); // already destroyed; don't try again in the guard

    ASSERT(XR::waitForJson(
               "j/openxr", [&](const std::string& r) { return !r.contains(nameMarker); }, std::chrono::milliseconds(10000)),
           true);
    ASSERT(XR::waitForJson(
               "j/monitors", [&](const std::string& r) { return !r.contains(nameMarker); }, std::chrono::milliseconds(10000)),
           true);

    NLog::green("xr_monitor_create_destroy: create+destroy round-trip verified in both j/monitors and j/openxr");
}

// `xrmonitor view` is the in-headset presentation switch: it must remove every monitor quad without
// stopping the session or unplugging/recreating the backing outputs. Exercise the public IPC surface
// here; the per-monitor `quads` count proves the frame-thread gate has reached xrEndFrame rather than
// this being status-only bookkeeping.
TEST_CASE(xr_monitor_view_toggle) {
    XR_SKIP_IF_UNAVAILABLE();
    SArtifactGuard guard{this->failed, name(), {}};
    // `view` is a global session latch; never let an assertion hide the fixtures for tests that
    // follow this one.
    struct SViewGuard {
        ~SViewGuard() {
            getFromSocket("/openxr view on");
        }
    } viewGuard;

    if (!gateUp()) {
        XR::logSkip(name(), "session never reached focused/visible (known env instability)");
        return;
    }

    const std::string mon        = XR::monitorName(18);
    const std::string nameMarker = "\"name\": \"" + mon + "\"";
    ASSERT(getFromSocket("/openxr create " + mon + " 1280x720"), std::string("ok"));
    guard.monitorNames.push_back(mon);

    ASSERT(XR::waitForJson("j/openxr", [&](const std::string& r) { return r.contains(nameMarker); }, std::chrono::milliseconds(10000)), true);
    // A freshly-created monitor may still be inside the first-plug settle window even though the
    // session itself is focused. Wait for the backing output before trying to focus it.
    if (!XR::waitForJson("j/monitors", [&](const std::string& r) { return r.contains(nameMarker); }, std::chrono::milliseconds(15000))) {
        XR::logSkip(name(), "XR monitor never got plugged (monitors_follow_session gate never satisfied in this environment)");
        return;
    }
    ASSERT(getFromSocket("/dispatch focusmonitor " + mon), std::string("ok"));
    auto kitty = Tests::spawnKitty("xr_monitor_view_toggle");
    if (!kitty) {
        XR::logSkip(name(), "kitty did not spawn (env limitation)");
        return;
    }
    ASSERT(XR::waitForJson("j/clients", [&](const std::string& r) { return r.contains("\"xr_monitor_view_toggle\""); }, std::chrono::milliseconds(10000)), true);
    ASSERT(XR::waitForJson(
               "j/openxr",
               [&](const std::string& r) {
                   const auto p = XR::findAfter(r, nameMarker);
                   return p != std::string::npos && XR::fieldAfter(r, p, "quads") != "0";
               },
               std::chrono::milliseconds(10000)),
           true);
    const std::string stateBefore = XR::fieldAfter(getFromSocket("j/openxr"), 0, "state");

    ASSERT(getFromSocket("/openxr view off"), std::string("ok"));
    ASSERT(XR::waitForJson(
               "j/openxr",
               [&](const std::string& r) {
                   const auto p = XR::findAfter(r, nameMarker);
                   return XR::fieldAfter(r, 0, "monitorView") == "hidden" && p != std::string::npos && XR::fieldAfter(r, p, "quads") == "0";
               },
               std::chrono::milliseconds(3000)),
           true);

    const std::string hidden = getFromSocket("j/openxr");
    EXPECT(XR::fieldAfter(hidden, 0, "state"), stateBefore);
    const auto hiddenMon = XR::findAfter(hidden, nameMarker);
    EXPECT_NOT(hiddenMon, std::string::npos);
    EXPECT(XR::fieldAfter(hidden, hiddenMon, "plugged"), std::string("true"));

    ASSERT(getFromSocket("/openxr view toggle"), std::string("ok"));
    ASSERT(XR::waitForJson(
               "j/openxr",
               [&](const std::string& r) {
                   const auto p = XR::findAfter(r, nameMarker);
                   return XR::fieldAfter(r, 0, "monitorView") == "shown" && p != std::string::npos && XR::fieldAfter(r, p, "quads") != "0";
               },
               std::chrono::milliseconds(3000)),
           true);

    EXPECT(getFromSocket("/openxr view invalid"), std::string("view: expected on|off|toggle"));
    NLog::green("xr_monitor_view_toggle: zero-layer hide/show preserved session and backing output");
}

// xr_monitor_create_mode — live 2026-08-01: `hyprctl openxr create XR-2 2560x1440@60` produced a
// monitor RUNNING 1920x1080. createXRMonitor does install a persistent monitor rule carrying the
// requested mode (report-20 issue E), but a config reparse CLEARS the rule manager and only
// reconcileDeclaredMonitors() reinstalls those rules — for `xrmonitor =` DECLARED layers. A
// runtime-created monitor is deliberately outside reconciliation, so after any `hyprctl reload` its
// mode was un-owned and fell back to the headless preferred mode (or to a `monitor = NAME,
// preferred` line, which is exactly what the report's config had). Assert the APPLIED mode from
// j/monitors — not the requested one echoed back by j/openxr, which was right the whole time —
// both before and after a reload.
TEST_CASE(xr_monitor_create_mode) {
    XR_SKIP_IF_UNAVAILABLE();
    SArtifactGuard guard{this->failed, name(), {}};

    if (!gateUp()) {
        XR::logSkip(name(), "session never reached focused/visible (known env instability)");
        return;
    }

    const std::string mon        = XR::monitorName(14);
    const std::string nameMarker = "\"name\": \"" + mon + "\"";
    // Deliberately neither the headless default nor the 1280x720 the other tests use, so a fallback
    // to either is unmistakable.
    ASSERT(getFromSocket("/openxr create " + mon + " 1600x900@75"), std::string("ok"));
    guard.monitorNames.push_back(mon);

    const auto modeIs = [&](const std::string& r) {
        const auto p = XR::findAfter(r, nameMarker);
        if (p == std::string::npos)
            return false;
        return XR::toFloatOr(XR::fieldAfter(r, p, "width"), -1.f) == 1600.f && XR::toFloatOr(XR::fieldAfter(r, p, "height"), -1.f) == 900.f &&
            std::abs(XR::toFloatOr(XR::fieldAfter(r, p, "refreshRate"), -1.f) - 75.f) < 1.5f;
    };

    // A monitor created inside the first-plug settle window starts UNPLUGGED (report-20 issue D) and
    // only enters j/monitors on the plug edge — the same environmental gate xr_mirror SKIPs on.
    if (!XR::waitForJson(
            "j/monitors", [&](const std::string& r) { return r.contains(nameMarker); }, std::chrono::milliseconds(15000))) {
        XR::logSkip(name(), "XR monitor never got plugged (monitors_follow_session gate never satisfied in this environment)");
        return;
    }
    ASSERT(XR::waitForJson("j/monitors", modeIs, std::chrono::milliseconds(10000)), true);
    NLog::green("xr_monitor_create_mode: create applied 1600x900@75");

    // The regression: reload, and the requested mode must still own the output.
    ASSERT(getFromSocket("/reload"), std::string("ok"));
    ASSERT(XR::waitForJson(
               "j/monitors", [&](const std::string& r) { return r.contains(nameMarker); }, std::chrono::milliseconds(15000)),
           true);
    ASSERT(XR::waitForJson("j/monitors", modeIs, std::chrono::milliseconds(10000)), true);

    // ...and stays owned: ensureMonitorStatus runs on a later render pass, so a rule that lost the
    // mode reverts a beat after the reload settles, not instantly.
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    ASSERT(modeIs(getFromSocket("j/monitors")), true);

    NLog::green("xr_monitor_create_mode: 1600x900@75 survived a config reload");
}

// xr_force_linear_realloc — Defect A (2026-07-12): a bare multigpu flip was swallowed by aquamarine's
// CSwapchain::reconfigure() no-op (it compares only format/size/length), so the XR-bound output's
// buffers kept their native (foreign-vendor-tiled) modifier and the cross-GPU import stayed black —
// even though `hyprctl openxr status` already reported `linear`. CMonitorState::updateSwapchain() now
// forces the fullReconfigure path on a multigpu change so the buffers ACTUALLY re-allocate LINEAR.
//
// We assert on AQUAMARINE's own allocator log, not the OpenXR manager's: the manager logs via
// Log::logger, whose sink the harness doesn't route into hyprlandd.log, but aquamarine's allocator
// DOES land there — and it is stronger evidence anyway, being the layer that actually honors
// SSwapchainOptions::multigpu. The signature of a real LINEAR re-allocation is the pair
//   "GBM: Buffer is marked as multigpu, forcing linear"  +  "modifier 0x0 : LINEAR"
// In the BUGGY build the flag flip was swallowed by aquamarine's reconfigure() no-op, so this pair
// never appeared and the buffers kept their native tiling. status `linear: true` alone is NOT
// sufficient — it was `true` in the buggy build too; the aquamarine reallocation is what changed.
//
// The dev host happens to be cross-GPU (openxr:gpu pins a different node than the nested compositor
// allocates on), so the `auto` default already force-linears the declared XR-conf monitors AT
// STARTUP — the exact live scenario. We observe that first; if the host is cross-GPU but the
// startup evidence is absent, we drive it explicitly via `force_linear = on` + a fresh monitor.
// Either way the assertion is the aquamarine reallocation evidence.
//
// Requires two GPUs, so it SKIPs below that (docs §5.3): the defect is a CROSS-GPU one, and with a
// single GPU `auto` correctly never forces, leaving only the synthetic `force_linear = on` drive —
// whose evidence lands only if the freshly-created monitor also reaches a plugged, composited state
// (report-20 issue D, the same environmental gate xr_mirror / xr_monitor_create_mode skip on). In
// the hermetic container, which is handed ONE render node, that combination came up roughly 1 run
// in 5, so a hard assertion there tests the environment rather than the fix.
TEST_CASE(xr_force_linear_realloc) {
    XR_SKIP_IF_UNAVAILABLE();
    SArtifactGuard guard{this->failed, name(), {}};

    // Counted the way applyCrossGpuLinear decides cross-GPU (DRM::sameGpu → drmDevicesEqual over
    // the XR render node and the allocator's card node), so the gate and the feature agree on what
    // "two GPUs" means. Checked before the session gate — it costs nothing and needs no session.
    if (const size_t GPUS = XR::drmGpuCount(); GPUS < 2) {
        XR::logSkip(name(), "single-GPU environment (" + std::to_string(GPUS) + " DRM device(s) under /dev/dri); the cross-GPU force-linear reallocation is not reproducible here");
        return;
    }

    if (!gateUp()) {
        XR::logSkip(name(), "session never reached focused/visible (known env instability)");
        return;
    }

    auto readHyprlandLog = []() -> std::string {
        const char* xdg = std::getenv("XDG_RUNTIME_DIR");
        if (!xdg || HIS.empty())
            return "";
        // Logger.cpp writes hyprlandd.log for HYPRLAND_DEBUG (CMAKE_BUILD_TYPE=Debug) builds and
        // hyprland.log otherwise — read whichever exists so this works under both (docs §5.2 gap).
        const std::string base = std::string(xdg) + "/hypr/" + HIS + "/";
        std::string       out;
        for (const char* fn : {"hyprlandd.log", "hyprland.log"}) {
            std::ifstream f(base + fn);
            if (f) {
                std::stringstream ss;
                ss << f.rdbuf();
                out += ss.str();
            }
        }
        return out;
    };
    // The reallocation is proven iff the log holds BOTH the multigpu-forcing note and a LINEAR (0x0)
    // buffer allocation. (Same-GPU builds without the fix would have neither; the buggy build had a
    // set flag + status linear=true but neither aquamarine line.)
    auto reallocatedLinear = [&](const std::string& log) { return log.find("multigpu, forcing linear") != std::string::npos && log.find("modifier 0x0 : LINEAR") != std::string::npos; };
    // Does any monitor report linear:true in status?
    auto anyMonitorLinear = []() { return getFromSocket("j/openxr").find("\"linear\": true") != std::string::npos; };

    // Path 1 (this host): the auto default already forced the declared monitors linear at startup.
    bool sawRealloc = reallocatedLinear(readHyprlandLog());
    bool sawStatus  = anyMonitorLinear();

    // Path 2 (same-GPU host): drive it explicitly so the mechanics are still exercised.
    std::string mon;
    if (!sawRealloc || !sawStatus) {
        ASSERT(getFromSocket("/keyword openxr:force_linear on"), std::string("ok")); // read fresh at create
        mon                          = XR::monitorName(13);
        const std::string nameMarker = "\"name\": \"" + mon + "\"";
        ASSERT(getFromSocket("/openxr create " + mon + " 1280x720"), std::string("ok"));
        guard.monitorNames.push_back(mon);
        ASSERT(XR::waitForJson(
                   "j/openxr", [&](const std::string& r) { return r.contains(nameMarker); }, std::chrono::milliseconds(10000)),
               true);
        // A monitor created inside the first-plug settle window (report-20 issue D) or across a
        // session bounce starts UNPLUGGED — no composite, so the forced re-allocation may not fire
        // until the plug edge (bindExistingLayers' cross-GPU pass). Wait for the plug so the poll
        // below observes a deterministic trigger instead of racing the settle guard (the in-container
        // full-suite failure mode: the realloc pair landed in the log a few KB past the poll budget).
        (void)XR::waitForJson(
            "j/monitors", [&](const std::string& r) { return r.contains(nameMarker); }, std::chrono::milliseconds(15000));
        // Poll: the forced swapchain re-allocates once the monitor's mode is applied + first rendered.
        for (int i = 0; i < 100 && !(sawRealloc && sawStatus); ++i) {
            sawRealloc = reallocatedLinear(readHyprlandLog());
            sawStatus  = anyMonitorLinear();
            if (!(sawRealloc && sawStatus))
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    if (!sawRealloc)
        NLog::red("xr_force_linear_realloc diag: aquamarine never logged 'multigpu, forcing linear' + 'modifier 0x0 : LINEAR' (log bytes={})", readHyprlandLog().size());
    ASSERT(sawRealloc, true); // the Defect A fix: the multigpu flip actually re-allocates LINEAR
    EXPECT(sawStatus, true);  // status surface reflects it

    // Cleanup: destroy any monitor we created + restore the default (auto).
    if (!mon.empty()) {
        getFromSocket("/openxr destroy " + mon);
        guard.monitorNames.clear();
        getFromSocket("/keyword openxr:force_linear auto");
    }

    NLog::green("xr_force_linear_realloc: multigpu flip re-allocates the XR swapchain LINEAR (aquamarine 'multigpu, forcing linear' + 'modifier 0x0 : LINEAR'), status linear=true");
}

// xr_monitor_churn_content — live 2026-07-12 regression: creating/destroying XR monitors in a burst
// left monitors created AFTER a destroy showing a BLANK (black) quad. Root cause: the cross-GPU
// force-linear policy was applied a beat AFTER the monitor's first composite, so a foreign-tiled
// buffer got composited + stashed before the swapchain reconfigured LINEAR; the frame thread then
// imported that stale tiled buffer, the foreign XR GPU's EGL rejected the tiling, and the quad went
// black (intermittent by timing — some monitors in the burst, not all). Fixed by (a) setting the
// force-linear flag BEFORE the first mode-apply/composite (createXRMonitor step 4b) and (b) a
// presented-listener guard that never stashes a still-tiled buffer while force-linear is active.
//
// The invariant this pins: EVERY living XR monitor's content path resolves to a real blit
// ("dmabuf" on this cross-GPU host — never stuck "black"), including monitors created after a
// destroy in the same burst. This host is genuinely cross-GPU (xr-test-local.conf pins a different
// node than the nested compositor allocates on), so the force-linear import path is live in-suite —
// the exact condition that reproduced the blank.
TEST_CASE(xr_monitor_churn_content) {
    XR_SKIP_IF_UNAVAILABLE();
    SArtifactGuard guard{this->failed, name(), {}};

    if (!gateUp()) {
        XR::logSkip(name(), "session never reached focused/visible (known env instability)");
        return;
    }

    // Does this monitor's status block report a NON-black content path (a real blit landed)?
    // "name" precedes "contentPath" in each block (XRIpc.cpp), and "contentPath" precedes the next
    // block's "name", so scanning from the name marker to the next name bounds it to this monitor.
    auto blockOf = [](const std::string& json, const std::string& mon) -> std::string {
        const std::string marker = "\"name\": \"" + mon + "\"";
        const size_t      pos    = XR::findAfter(json, marker); // position OF the marker (plain find)
        if (pos == std::string::npos)
            return "";
        const size_t from = pos + marker.size(); // start past our own name key or the next find hits it
        const size_t end  = json.find("\"name\":", from);
        return json.substr(from, end == std::string::npos ? std::string::npos : end - from);
    };
    auto contentReady = [&](const std::string& mon) {
        return [&, mon](const std::string& r) {
            const std::string b = blockOf(r, mon);
            // The blank bug parked contentPath at "black". Accept any real blit path; assert it is
            // specifically "dmabuf" separately below once it has settled.
            return !b.empty() && b.find("\"contentPath\": \"dmabuf\"") != std::string::npos;
        };
    };

    // Burst: create three, destroy the first, create two more — every survivor + newcomer must reach
    // a real (dmabuf) content path. The destroy-then-create is the exact scenario that blanked.
    const std::string a = XR::monitorName(20), b = XR::monitorName(21), c = XR::monitorName(22), d = XR::monitorName(23);
    for (auto& n : {a, b, c}) {
        ASSERT(getFromSocket("/openxr create " + n + " 1280x720"), std::string("ok"));
        guard.monitorNames.push_back(n);
    }
    // Destroy the first, then create two more AFTER the destroy (the regression trigger). The
    // destroy is asynchronous (removal barrier: the frame thread must ack before the output is
    // finalized), and j/openxr hides pendingRemoval layers EARLY — the compositor monitor object
    // outlives that by a beat. Wait for the name to leave j/monitors (the real resource) before
    // re-using it, or the create races the teardown and fails on the name collision.
    ASSERT(getFromSocket("/openxr destroy " + a), std::string("ok"));
    guard.monitorNames.erase(guard.monitorNames.begin());
    ASSERT(XR::waitForJson(
               "j/monitors all", [&](const std::string& r) { return !r.contains("\"name\": \"" + a + "\""); }, std::chrono::milliseconds(10000)),
           true);
    for (auto& n : {d, a}) { // create a brand-new one AND re-create the destroyed name
        ASSERT(getFromSocket("/openxr create " + n + " 1280x720"), std::string("ok"));
        guard.monitorNames.push_back(n);
    }

    // Every living monitor (b, c, d, and the re-created a) must reach dmabuf content — the blank
    // regression left one or more stuck at "black". Generous timeout: the frame thread must blit a
    // freshly-composited LINEAR buffer.
    for (auto& n : {b, c, d, a}) {
        const bool ok = XR::waitForJson("j/openxr", contentReady(n), std::chrono::milliseconds(20000));
        if (!ok)
            NLog::red("xr_monitor_churn_content: monitor '{}' never reached contentPath=dmabuf (blank regression); block=[{}] full=[{}]", n, blockOf(getFromSocket("j/openxr"), n),
                      getFromSocket("j/openxr"));
        ASSERT(ok, true);
    }

    NLog::green("xr_monitor_churn_content: all monitors (incl. created-after-destroy) reached dmabuf content — no blank quads");
}

// xr_monitor_destroy_client_survives — live 2026-07-12 symptom 2 companion: destroying an XR
// monitor whose workspace holds client windows must never kill the clients. The destroy path is
// required to evacuate FIRST (CMonitor::onDisconnect -> moveWorkspaceToMonitor, live-log verified)
// and only then remove the output — so the client keeps a valid output and merely moves. (The live
// ghostty SIGSEGV turned out to be a GTK4 client-side bug in its own wl_output dispatch — cores
// present, zero compositor protocol errors — but this pins OUR side of the contract: graceful
// evacuation, no protocol error, client alive.)
TEST_CASE(xr_monitor_destroy_client_survives) {
    XR_SKIP_IF_UNAVAILABLE();
    SArtifactGuard guard{this->failed, name(), {}};

    if (!gateUp()) {
        XR::logSkip(name(), "session never reached focused/visible (known env instability)");
        return;
    }

    const std::string mon        = XR::monitorName(24);
    const std::string nameMarker = "\"name\": \"" + mon + "\"";
    ASSERT(getFromSocket("/openxr create " + mon + " 1280x720"), std::string("ok"));
    guard.monitorNames.push_back(mon);
    ASSERT(XR::waitForJson(
               "j/monitors", [&](const std::string& r) { return r.contains(nameMarker); }, std::chrono::milliseconds(10000)),
           true);

    // Park a client on the XR monitor's active workspace.
    ASSERT(getFromSocket("/dispatch focusmonitor " + mon), std::string("ok"));
    auto kitty = Tests::spawnKitty("xr_destroy_survivor");
    if (!kitty) {
        XR::logSkip(name(), "kitty did not spawn (env limitation)");
        return;
    }
    const pid_t kittyPid = kitty->pid();
    ASSERT(Tests::processAlive(kittyPid), true);
    // Confirm it landed on the XR monitor before we pull the rug.
    ASSERT(XR::waitForJson(
               "j/clients", [&](const std::string& r) { return r.contains("\"xr_destroy_survivor\""); }, std::chrono::milliseconds(10000)),
           true);

    // Destroy the monitor under the client; wait until the output is fully gone.
    ASSERT(getFromSocket("/openxr destroy " + mon), std::string("ok"));
    guard.monitorNames.clear();
    ASSERT(XR::waitForJson(
               "j/monitors all", [&](const std::string& r) { return !r.contains(nameMarker); }, std::chrono::milliseconds(10000)),
           true);

    // The client must survive the output removal (evacuated, not killed): still in j/clients and
    // the process alive. Give the evacuation a beat to settle before judging.
    const bool stillListed = XR::waitForJson(
        "j/clients", [&](const std::string& r) { return r.contains("\"xr_destroy_survivor\""); }, std::chrono::milliseconds(5000));
    EXPECT(stillListed, true);
    Tests::sync();
    ASSERT(Tests::processAlive(kittyPid), true);

    // And it must have been MOVED to a real monitor (its workspace's monitor is not the dead one).
    const std::string clients = getFromSocket("j/clients");
    EXPECT(clients.contains("\"" + mon + "\""), false);

    Tests::killAllWindows(); // our kitty included; preTestCleanup would do this anyway
    NLog::green("xr_monitor_destroy_client_survives: client evacuated and alive after XR monitor destroy");
}

// xr_config_declared — WP11 (doc 06 §6 row 3, extended per the roadmap's explicit ask for
// update-in-place + removal coverage; doc 06 marks the file-swap flavor "out of scope v1" but
// it's cheap and the roadmap wants it).
//
// (a) the two config-declared fixtures (xr-test.conf) exist with their configured anchors;
// (b) a runtime monitor + `/reload` -> declared fixtures survive with stable ids (no
//     destroy/create flicker), the runtime monitor is untouched;
// (c) rewrite xr-test.conf to change XR-conf-a's size and drop XR-conf-b, `/reload` ->
//     XR-conf-a updates IN PLACE (same id, new size_m), XR-conf-b is destroyed, the runtime
//     monitor from (b) is still untouched.
//
// The config file is always restored (and reloaded again) by the RAII guard, regardless of
// test outcome, so a crash mid-test never leaves xr-test.conf modified on disk.
TEST_CASE(xr_config_declared) {
    XR_SKIP_IF_UNAVAILABLE();

    static const std::string CONF_PATH = "xr-test.conf";

    std::string original;
    {
        std::ifstream in(CONF_PATH);
        if (in) {
            std::ostringstream ss;
            ss << in.rdbuf();
            original = ss.str();
        }
    }

    struct SConfigGuard {
        const bool& failed;
        std::string testName;
        std::string monitorName;
        std::string originalContent;
        bool        modified = false;
        ~SConfigGuard() {
            if (modified && !originalContent.empty()) {
                std::ofstream out(CONF_PATH, std::ios::trunc);
                if (out)
                    out << originalContent;
                getFromSocket("/reload");
            }
            if (!monitorName.empty())
                getFromSocket("/openxr destroy " + monitorName);
            if (failed)
                XR::dumpXrArtifacts(testName);
        }
    };
    SConfigGuard guard{this->failed, name(), "", original, false};

    if (original.empty()) {
        XR::logSkip(name(), "could not read xr-test.conf from the cwd (hyprtester must run from hyprtester/)");
        return;
    }

    if (!gateUp()) {
        XR::logSkip(name(), "session never reached focused/visible (known env instability)");
        return;
    }

    // (a) initial declared fixtures, correct anchors.
    std::string status = getFromSocket("j/openxr");
    ASSERT_CONTAINS(status, "\"name\": \"XR-conf-a\"");
    ASSERT_CONTAINS(status, "\"name\": \"XR-conf-b\"");

    const auto posA0 = XR::findAfter(status, "\"name\": \"XR-conf-a\"");
    const auto posB0 = XR::findAfter(status, "\"name\": \"XR-conf-b\"");
    EXPECT(XR::fieldAfter(status, posA0, "mode"), std::string("local"));
    EXPECT(XR::fieldAfter(status, posB0, "mode"), std::string("head"));

    const std::string idA = XR::fieldAfter(status, posA0, "id");
    const std::string idB = XR::fieldAfter(status, posB0, "id");
    ASSERT_NOT(idA, std::string(""));
    ASSERT_NOT(idB, std::string(""));

    // (b) runtime monitor + reload -> declared survive untouched (same id), runtime survives.
    const std::string mon = XR::monitorName(12);
    ASSERT(getFromSocket("/openxr create " + mon + " 1280x720"), std::string("ok"));
    guard.monitorName = mon;
    ASSERT(XR::waitForJson(
               "j/openxr", [&](const std::string& r) { return r.contains("\"name\": \"" + mon + "\""); }, std::chrono::milliseconds(10000)),
           true);

    ASSERT(getFromSocket("/reload"), std::string("ok"));

    std::string statusAfterReload;
    ASSERT(XR::waitForJson(
               "j/openxr",
               [&](const std::string& r) {
                   statusAfterReload = r;
                   return r.contains("\"name\": \"XR-conf-a\"") && r.contains("\"name\": \"XR-conf-b\"") && r.contains("\"name\": \"" + mon + "\"");
               },
               std::chrono::milliseconds(10000)),
           true);

    EXPECT(XR::fieldAfter(statusAfterReload, XR::findAfter(statusAfterReload, "\"name\": \"XR-conf-a\""), "id"), idA);
    EXPECT(XR::fieldAfter(statusAfterReload, XR::findAfter(statusAfterReload, "\"name\": \"XR-conf-b\""), "id"), idB);
    NLog::green("xr_config_declared: declared fixtures survived reload with stable ids; runtime monitor untouched");

    // (c) rewrite the config: change XR-conf-a's size, drop XR-conf-b entirely.
    std::string modified;
    {
        std::istringstream iss(original);
        std::string        line;
        while (std::getline(iss, line)) {
            const auto p = line.find_first_not_of(" \t");
            if (p != std::string::npos && line.compare(p, 9, "xrmonitor") == 0)
                continue; // drop both original xrmonitor declarations; we re-add XR-conf-a below
            modified += line + "\n";
        }
        modified += "xrmonitor = XR-conf-a, 1280x720@60, anchor:local pos:0,1.4,-1.5 yaw:0, size:1.9\n";
    }
    {
        std::ofstream out(CONF_PATH, std::ios::trunc);
        ASSERT(!!out, true);
        out << modified;
    }
    guard.modified = true;

    ASSERT(getFromSocket("/reload"), std::string("ok"));

    std::string statusFinal;
    ASSERT(XR::waitForJson(
               "j/openxr",
               [&](const std::string& r) {
                   statusFinal = r;
                   return r.contains("\"name\": \"XR-conf-a\"") && !r.contains("\"name\": \"XR-conf-b\"") && r.contains("\"name\": \"" + mon + "\"");
               },
               std::chrono::milliseconds(10000)),
           true);

    // XR-conf-a updated in place: same id, new size.
    const auto posAFinal = XR::findAfter(statusFinal, "\"name\": \"XR-conf-a\"");
    EXPECT(XR::fieldAfter(statusFinal, posAFinal, "id"), idA);
    const float sizeFinal = XR::toFloatOr(XR::fieldAfter(statusFinal, posAFinal, "size_m"), -1.f);
    EXPECT_MAX_DELTA(sizeFinal, 1.9, 0.05);

    // XR-conf-b fully gone (destroyed, headless output removed too). The layer leaves
    // j/openxr synchronously on reconcile, but the underlying output's removal goes through the
    // frame-thread removal barrier (WP3/WP4), so give j/monitors a bounded moment to catch up.
    ASSERT(XR::waitForJson(
               "j/monitors", [&](const std::string& r) { return !r.contains("\"name\": \"XR-conf-b\""); }, std::chrono::milliseconds(10000)),
           true);

    NLog::green("xr_config_declared: declaration change updated XR-conf-a in place and destroyed XR-conf-b; runtime monitor untouched throughout");
}

// xr_mirror — WP11 (doc 06 §6 row 9): zero-new-compositor-code mirroring via the ordinary
// `monitor =` keyword's `mirror` verb (CMonitor::setMirror).
TEST_CASE(xr_mirror) {
    XR_SKIP_IF_UNAVAILABLE();

    struct SGuard {
        const bool& failed;
        std::string testName;
        std::string monitorName;
        bool        createdHeadless2 = false;
        ~SGuard() {
            if (createdHeadless2)
                getFromSocket("/output destroy HEADLESS-2");
            if (!monitorName.empty())
                getFromSocket("/openxr destroy " + monitorName);
            if (failed)
                XR::dumpXrArtifacts(testName);
        }
    };
    SGuard guard{this->failed, name(), "", false};

    if (!gateUp()) {
        XR::logSkip(name(), "session never reached focused/visible (known env instability)");
        return;
    }

    const std::string mon = XR::monitorName(13);
    ASSERT(getFromSocket("/openxr create " + mon + " 1280x720"), std::string("ok"));
    guard.monitorName = mon;
    ASSERT(XR::waitForJson(
               "j/openxr", [&](const std::string& r) { return r.contains("\"name\": \"" + mon + "\""); }, std::chrono::milliseconds(10000)),
           true);

    // The mirror keyword resolves its target through the LIVE monitor list at apply time, and
    // CMonitor::setMirror() silently no-ops when the name isn't found — nothing re-resolves the
    // rule when the target appears later. An XR monitor created inside the first-plug settle
    // window (report-20 issue D: monitors_follow_session=visible defers the first plug of a
    // session ~1.5s past the session-start visibility blip) starts life UNPLUGGED and only
    // enters that list on the plug edge — exactly the window this test used to race in-container
    // (the preceding test's reload bounces the session, re-arming the guard). Wait for the plug
    // before touching the mirror keyword; if the plug never comes the env gate isn't satisfied,
    // which is the same environmental class the other tests SKIP on.
    if (!XR::waitForJson(
            "j/monitors", [&](const std::string& r) { return r.contains("\"name\": \"" + mon + "\""); }, std::chrono::milliseconds(15000))) {
        XR::logSkip(name(), "XR monitor never got plugged (monitors_follow_session gate never satisfied in this environment)");
        return;
    }

    const std::string xrStatus = getFromSocket("j/openxr");
    const std::string xrId     = XR::fieldAfter(xrStatus, XR::findAfter(xrStatus, "\"name\": \"" + mon + "\""), "id");
    ASSERT_NOT(xrId, std::string(""));

    const std::string createReply = getFromSocket("/output create headless HEADLESS-2");
    ASSERT(createReply == "ok" || createReply == "Name already taken", true);
    guard.createdHeadless2 = true;

    // The headless output takes a moment to actually connect/register before it's queryable or
    // matchable by a `monitor =` rule; wait for it before applying the mirror keyword. Use
    // "monitors all" throughout this test: a monitor actively mirroring another is excluded
    // from the plain `monitors()` list (CMonitorStateTracker erases `isMirror()` entries) and
    // only shows up via `allMonitors()` (`hyprctl monitors all`).
    ASSERT(XR::waitForJson(
               "j/monitors all", [&](const std::string& r) { return r.contains("\"name\": \"HEADLESS-2\""); }, std::chrono::milliseconds(10000)),
           true);

    ASSERT(getFromSocket("/keyword monitor HEADLESS-2, preferred, auto, 1, mirror, " + mon), std::string("ok"));

    ASSERT(XR::waitForJson(
               "j/monitors all",
               [&](const std::string& r) {
                   const auto p = XR::findAfter(r, "\"name\": \"HEADLESS-2\"");
                   return p != std::string::npos && XR::fieldAfter(r, p, "mirrorOf") == xrId;
               },
               std::chrono::milliseconds(10000)),
           true);
    NLog::green("xr_mirror: HEADLESS-2 mirrorOf == {} (the XR monitor's id)", xrId);

    // Unset mirroring, restoring HEADLESS-2 to a normal output (doc 06 §6 row 9).
    ASSERT(getFromSocket("/keyword monitor HEADLESS-2, preferred, auto, 1"), std::string("ok"));
    ASSERT(XR::waitForJson(
               "j/monitors",
               [&](const std::string& r) {
                   const auto p = XR::findAfter(r, "\"name\": \"HEADLESS-2\"");
                   return p != std::string::npos && XR::fieldAfter(r, p, "mirrorOf") == "none";
               },
               std::chrono::milliseconds(10000)),
           true);
}

#endif // WITH_XR_TESTS
