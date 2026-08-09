// Regression guard for the 2026-08-09 "compositor hang" — which was not a hang.
//
// hyprtester's preTestCleanup() calls Tests::killAllLayers(), which used to read pids straight out
// of Hyprland's `/layers` reply and hand them to kill(2). CLayerSurface::getPID() answers -1 for a
// layer surface whose wl_client is already gone, and kill(-1, SIGTERM) is not "signal nothing" —
// it signals every process the invoking user owns. Twice that day a hyprtester run launched from
// inside the developer's live desktop SIGTERM'd that desktop out from under them ~12s in: uwsm,
// dbus-broker, the compositor, the session daemons and the harness's own shell all took the signal
// inside the same millisecond.
//
// These tests pin the two halves of the fix: the wire really can hand us a non-process pid, and the
// guard refuses to signal one. Nothing here ever sends a real signal to a real process.

#include "../../hyprtester/src/SafeKill.hpp"

#include <gtest/gtest.h>

TEST(HarnessSafeKill, validPidRejectsEveryBroadcastForm) {
    // the three forms kill(2) reads as "more than one process"
    EXPECT_FALSE(Safe::validPid(-1));    // every process we may signal
    EXPECT_FALSE(Safe::validPid(0));     // our own process group
    EXPECT_FALSE(Safe::validPid(-4242)); // the process group 4242

    EXPECT_TRUE(Safe::validPid(1));
    EXPECT_TRUE(Safe::validPid(getpid()));
}

TEST(HarnessSafeKill, signalPidRefusesBroadcasts) {
    // Signal 0 delivers nothing, so a regression here cannot hurt the test runner — but a
    // regression WOULD make these return true.
    EXPECT_FALSE(Safe::signalPid(-1, 0));
    EXPECT_FALSE(Safe::signalPid(0, 0));
    EXPECT_FALSE(Safe::signalPid(-4242, 0));

    // ...while a real, single process still gets through
    EXPECT_TRUE(Safe::signalPid(getpid(), 0));
}

TEST(HarnessSafeKill, pidAliveIsFalseForNonProcesses) {
    // the raw `kill(pid, 0) == 0` form answers TRUE for both of these
    EXPECT_FALSE(Safe::pidAlive(-1));
    EXPECT_FALSE(Safe::pidAlive(0));

    EXPECT_TRUE(Safe::pidAlive(getpid()));
}

TEST(HarnessSafeKill, layersReplyCanCarryADeadLayersMinusOne) {
    // Shape copied from HyprCtl.cpp's non-JSON `/layers` formatter. The middle entry is a layer
    // whose client has gone away — the case that fired.
    const std::string REPLY = "Monitor eDP-1:\n"
                              "\tLayer level 2 (top):\n"
                              "\t\tLayer 5f0a: xywh: 0 0 2560 40, a: 1, namespace: waybar, pid: 4711\n"
                              "\t\tLayer 5f0b: xywh: 0 0 0 0, a: 0, namespace: notifications, pid: -1\n"
                              "\t\tLayer 5f0c: xywh: 0 0 2560 1440, a: 1, namespace: swaybg, pid: 4712\n";

    const auto        PIDS = Safe::pidsFromReply(REPLY, "pid: ");
    ASSERT_EQ(PIDS.size(), 3u);
    EXPECT_EQ(PIDS[0], 4711);
    EXPECT_EQ(PIDS[1], -1);
    EXPECT_EQ(PIDS[2], 4712);

    // ...and the -1 is exactly the one the guard drops
    EXPECT_TRUE(Safe::validPid(PIDS[0]));
    EXPECT_FALSE(Safe::validPid(PIDS[1]));
    EXPECT_TRUE(Safe::validPid(PIDS[2]));
}

TEST(HarnessSafeKill, pidsFromReplyStopsAtTheFieldNotTheReply) {
    // The old parser passed find('\n') — an absolute offset — as substr()'s COUNT, so the token ran
    // to the end of the reply and only parsed because stoi() happens to stop at the first
    // non-digit. Pin that the field ends where the field ends.
    const auto PIDS = Safe::pidsFromReply("pid: 12, namespace: 999\npid: 34\n", "pid: ");
    ASSERT_EQ(PIDS.size(), 2u);
    EXPECT_EQ(PIDS[0], 12);
    EXPECT_EQ(PIDS[1], 34);
}

TEST(HarnessSafeKill, pidsFromReplyReportsUnparseableFieldsAsNonProcesses) {
    // A truncated or garbled reply must not silently vanish: it comes back as -1, which the guard
    // then refuses. Dropping it here instead would let a future caller assume "empty means clean".
    const auto PIDS = Safe::pidsFromReply("pid: \npid: abc\npid: 7\n", "pid: ");
    ASSERT_EQ(PIDS.size(), 3u);
    EXPECT_EQ(PIDS[0], -1);
    EXPECT_EQ(PIDS[1], -1);
    EXPECT_EQ(PIDS[2], 7);

    EXPECT_TRUE(Safe::pidsFromReply("Monitor eDP-1:\n\tLayer level 0 (background):\n", "pid: ").empty());
    EXPECT_TRUE(Safe::pidsFromReply("pid: 7", "").empty());
}
