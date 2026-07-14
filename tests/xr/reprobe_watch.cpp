#include <openxr/XRMonitorConfig.hpp>

#include <gtest/gtest.h>

using namespace OpenXR;

// Pure watch-path derivation + trigger predicates behind the event-driven re-probe (don-the-headset
// dead-air fix). The manager (COpenXRManager::setupReprobeWatch / onReprobeWatchReadable) owns the
// inotify fd + the CEventLoopTimer debounce; these functions decide WHICH directories to watch and
// WHICH just-created basenames mean "the runtime socket appeared" / "a nested socket dir appeared".
// Evidence pinned here: monado CMakeLists.txt default socket "monado_comp_ipc", WiVRn
// server/CMakeLists.txt "wivrn/comp_ipc", both under $XDG_RUNTIME_DIR (monado u_file_get_runtime_dir).

TEST(reprobe_watch, empty_runtime_dir_yields_no_watches) {
    // No $XDG_RUNTIME_DIR -> the socket location is unresolvable -> caller falls back to the timer.
    EXPECT_TRUE(xrReprobeWatchDirs("").empty());
}

TEST(reprobe_watch, derives_runtime_dir_and_wivrn_subdir) {
    const auto w = xrReprobeWatchDirs("/run/user/1000");
    ASSERT_EQ(w.size(), 2u);
    // [0] the runtime dir itself: monado's socket lands directly here; WiVRn's subdir is created here.
    EXPECT_EQ(w[0].dir, "/run/user/1000");
    EXPECT_TRUE(xrReprobeSocketMatch(w[0], "monado_comp_ipc"));
    EXPECT_TRUE(xrReprobeSubdirMatch(w[0], "wivrn"));
    EXPECT_FALSE(xrReprobeSocketMatch(w[0], "wivrn"));       // wivrn is a dir, not the socket
    EXPECT_FALSE(xrReprobeSubdirMatch(w[0], "monado_comp_ipc"));
    // [1] the WiVRn subdir: comp_ipc appears inside it once the headset connects.
    EXPECT_EQ(w[1].dir, "/run/user/1000/wivrn");
    EXPECT_TRUE(xrReprobeSocketMatch(w[1], "comp_ipc"));
    EXPECT_TRUE(w[1].subdirNames.empty());
}

TEST(reprobe_watch, ignores_unrelated_churn) {
    // $XDG_RUNTIME_DIR sees constant unrelated churn (wayland-1, pipewire-0, ...). None of it triggers.
    const auto w = xrReprobeWatchDirs("/run/user/1000");
    ASSERT_EQ(w.size(), 2u);
    for (const char* junk : {"wayland-1", "pipewire-0", "bus", "dbus-1", ".flock", "comp_ipc"}) {
        // note: "comp_ipc" is the WiVRn socket name but only matters INSIDE the wivrn subdir ([1]),
        // never at the runtime-dir level ([0]) — no false probe from a stray comp_ipc in $XDG_RUNTIME_DIR.
        EXPECT_FALSE(xrReprobeSocketMatch(w[0], junk)) << junk;
        EXPECT_FALSE(xrReprobeSubdirMatch(w[0], junk)) << junk;
    }
    // Inside the wivrn subdir, only comp_ipc is the socket.
    EXPECT_FALSE(xrReprobeSocketMatch(w[1], "wayland-1"));
    EXPECT_FALSE(xrReprobeSocketMatch(w[1], "monado_comp_ipc"));
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
