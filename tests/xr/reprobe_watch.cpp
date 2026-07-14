#include <openxr/XRMonitorConfig.hpp>

#include <gtest/gtest.h>

using namespace OpenXR;

// Pure watch-path derivation + trigger predicates + backoff policy behind the event-driven re-probe
// (don-the-headset dead-air fix, revised after live evidence — boot 2026-07-14, instance
// c41d16e2*_1784014116). The manager (COpenXRManager::setupReprobeWatch / onReprobeWatchReadable)
// owns the inotify fd + the CEventLoopTimer debounce; these functions decide WHICH directories to
// watch, WHICH event basenames trigger a probe, and WHAT the next timer delay is.
//
// Evidence pinned here:
//   - monado CMakeLists.txt: XRT_IPC_MSG_SOCK_FILENAME "monado_comp_ipc",
//     XRT_IPC_SERVICE_PID_FILENAME "monado.pid"
//   - WiVRn server/CMakeLists.txt: XRT_IPC_MSG_SOCK_FILENAME "wivrn/comp_ipc",
//     XRT_IPC_SERVICE_PID_FILENAME "wivrn.pid"
//   - WiVRn's MAIN server creates + listens on wivrn/comp_ipc at SERVICE start (create_listen_socket
//     in server/main.cpp); the don-time signal is the FORKED compositor server creating/rewriting
//     $XDG_RUNTIME_DIR/wivrn.pid (monado u_process.c pidfile machinery) — the socket itself never
//     changes at don. Hence pid files are triggers, not just sockets.

TEST(reprobe_watch, empty_runtime_dir_yields_no_watches) {
    // No $XDG_RUNTIME_DIR -> the socket location is unresolvable -> caller falls back to the timer.
    EXPECT_TRUE(xrReprobeWatchDirs("").empty());
}

TEST(reprobe_watch, derives_runtime_dir_and_wivrn_subdir) {
    const auto w = xrReprobeWatchDirs("/run/user/1000");
    ASSERT_EQ(w.size(), 2u);
    // [0] the runtime dir itself: monado's socket + BOTH runtimes' pid files land directly here;
    // WiVRn's subdir is created here.
    EXPECT_EQ(w[0].dir, "/run/user/1000");
    EXPECT_TRUE(xrReprobeTriggerMatch(w[0], "monado_comp_ipc"));
    EXPECT_TRUE(xrReprobeSubdirMatch(w[0], "wivrn"));
    EXPECT_FALSE(xrReprobeTriggerMatch(w[0], "wivrn")); // wivrn is a dir, not a trigger file
    EXPECT_FALSE(xrReprobeSubdirMatch(w[0], "monado_comp_ipc"));
    // [1] the WiVRn subdir: comp_ipc lands inside it (at service start; watching it still covers
    // service restarts and first installs).
    EXPECT_EQ(w[1].dir, "/run/user/1000/wivrn");
    EXPECT_TRUE(xrReprobeTriggerMatch(w[1], "comp_ipc"));
    EXPECT_TRUE(w[1].subdirNames.empty());
}

TEST(reprobe_watch, pid_files_are_don_time_triggers) {
    // Live-evidence bug 2: wivrn.pid is the ONLY filesystem signal at headset-connect (birth
    // 01:40:09.219 on the evidence boot = exactly "Server started" in the wivrn journal, while
    // comp_ipc's birth matched service start 01:28:36). monado.pid is the stock-monado analogue.
    const auto w = xrReprobeWatchDirs("/run/user/1000");
    ASSERT_EQ(w.size(), 2u);
    EXPECT_TRUE(xrReprobeTriggerMatch(w[0], "wivrn.pid"));
    EXPECT_TRUE(xrReprobeTriggerMatch(w[0], "monado.pid"));
    // Pid files live at the runtime-dir ROOT, not inside wivrn/.
    EXPECT_FALSE(xrReprobeTriggerMatch(w[1], "wivrn.pid"));
    EXPECT_FALSE(xrReprobeTriggerMatch(w[1], "monado.pid"));
}

TEST(reprobe_watch, ignores_unrelated_churn) {
    // $XDG_RUNTIME_DIR sees constant unrelated churn (wayland-1, pipewire-0, ...). None of it triggers.
    const auto w = xrReprobeWatchDirs("/run/user/1000");
    ASSERT_EQ(w.size(), 2u);
    for (const char* junk : {"wayland-1", "pipewire-0", "bus", "dbus-1", ".flock", "comp_ipc", "wivrn.lock", "hypr"}) {
        // note: "comp_ipc" is the WiVRn socket name but only matters INSIDE the wivrn subdir ([1]),
        // never at the runtime-dir level ([0]) — no false probe from a stray comp_ipc in $XDG_RUNTIME_DIR.
        EXPECT_FALSE(xrReprobeTriggerMatch(w[0], junk)) << junk;
        EXPECT_FALSE(xrReprobeSubdirMatch(w[0], junk)) << junk;
    }
    // Inside the wivrn subdir, only comp_ipc is a trigger.
    EXPECT_FALSE(xrReprobeTriggerMatch(w[1], "wayland-1"));
    EXPECT_FALSE(xrReprobeTriggerMatch(w[1], "monado_comp_ipc"));
}

TEST(reprobe_watch, normalizes_trailing_slash) {
    // XDG_RUNTIME_DIR may carry a trailing slash; the watched dirs must not double it (inotify keys on
    // the exact path, and the subdir join must stay clean).
    const auto w = xrReprobeWatchDirs("/run/user/1000/");
    ASSERT_EQ(w.size(), 2u);
    EXPECT_EQ(w[0].dir, "/run/user/1000");
    EXPECT_EQ(w[1].dir, "/run/user/1000/wivrn");
}

TEST(reprobe_watch, debounce_constant_is_short) {
    // The debounce only exists to (a) coalesce a create burst and (b) let the server start accept()ing;
    // it must stay well under a human-perceptible donning delay. Pin it so a careless bump is caught.
    EXPECT_GT(XR_REPROBE_WATCH_DEBOUNCE_MS, 0);
    EXPECT_LE(XR_REPROBE_WATCH_DEBOUNCE_MS, 300);
}

// ---- backoff policy (live-evidence bug 1) ----------------------------------------------------
// The full decision table for xrReprobeDelayMs. On the evidence boot, the "runtime reachable but
// degraded" failure was misclassified as no-runtime and the backoff grew to the 30s cap; probes
// triggered mid-handshake then inherited that 30s ("attempt 25") and the user ate ~24s of dead-air
// after the real server was already up. HEADSET-class waits and recent-activity windows must pin
// the delay to the base interval.

TEST(reprobe_watch, delay_headset_wait_is_fixed_base) {
    // HEADSET wait never grows, regardless of how many attempts have failed.
    EXPECT_EQ(xrReprobeDelayMs(/*headset*/ true, /*activity*/ false, 0, 2000, 30000), 2000);
    EXPECT_EQ(xrReprobeDelayMs(true, false, 5, 2000, 30000), 2000);
    EXPECT_EQ(xrReprobeDelayMs(true, false, 25, 2000, 30000), 2000); // the evidence boot's attempt count
}

TEST(reprobe_watch, delay_recent_activity_caps_at_base) {
    // Filesystem activity in the watched dirs = the runtime is materializing -> poll hard even in
    // RUNTIME wait with a grown attempt counter.
    EXPECT_EQ(xrReprobeDelayMs(false, true, 25, 2000, 30000), 2000);
    EXPECT_EQ(xrReprobeDelayMs(false, true, 0, 2000, 30000), 2000);
    // Both flags set: still base.
    EXPECT_EQ(xrReprobeDelayMs(true, true, 25, 2000, 30000), 2000);
}

TEST(reprobe_watch, delay_quiet_runtime_wait_backs_off) {
    // No headset wait, no activity: the plain doubling schedule (unchanged behavior).
    EXPECT_EQ(xrReprobeDelayMs(false, false, 0, 2000, 30000), 2000);
    EXPECT_EQ(xrReprobeDelayMs(false, false, 1, 2000, 30000), 4000);
    EXPECT_EQ(xrReprobeDelayMs(false, false, 4, 2000, 30000), 30000); // capped
    EXPECT_EQ(xrReprobeDelayMs(false, false, 25, 2000, 30000), 30000);
}

TEST(reprobe_watch, delay_defends_degenerate_base) {
    // base <= 0 falls back to the 2000ms default on every leg (mirrors xrReprobeBackoffMs).
    EXPECT_EQ(xrReprobeDelayMs(true, false, 3, 0, 30000), 2000);
    EXPECT_EQ(xrReprobeDelayMs(false, true, 3, -5, 30000), 2000);
    EXPECT_EQ(xrReprobeDelayMs(false, false, 0, 0, 30000), 2000);
}

TEST(reprobe_watch, runtime_socket_paths) {
    // The socket-presence refinement for the degraded-runtime HEADSET classification: WiVRn's client
    // lib answers extension enumeration with a degraded list even when NO server is running (verified
    // in an isolated $XDG_RUNTIME_DIR), so "enumerate answered" alone would make a stopped service
    // poll at base forever. The manager stats these paths; only enumerate+socket = "service up".
    const auto p = xrRuntimeSocketPaths("/run/user/1000");
    ASSERT_EQ(p.size(), 2u);
    EXPECT_EQ(p[0], "/run/user/1000/monado_comp_ipc");
    EXPECT_EQ(p[1], "/run/user/1000/wivrn/comp_ipc");
    // Trailing slash normalized; empty -> empty (no $XDG_RUNTIME_DIR, nothing to stat).
    EXPECT_EQ(xrRuntimeSocketPaths("/run/user/1000/")[1], "/run/user/1000/wivrn/comp_ipc");
    EXPECT_TRUE(xrRuntimeSocketPaths("").empty());
}

TEST(reprobe_watch, activity_window_is_sane) {
    // The window must be long enough to bridge a slow runtime handshake (the evidence boot's forked
    // server took ~3s from fork to fully up, and services can restart slower) but bounded so an
    // absent runtime returns to the cheap backoff.
    EXPECT_GE(XR_REPROBE_ACTIVITY_WINDOW_MS, 10000);
    EXPECT_LE(XR_REPROBE_ACTIVITY_WINDOW_MS, 5 * 60000);
}
