#include <openxr/XRMonitorConfig.hpp>

#include <gtest/gtest.h>

using namespace OpenXR;

// Session-loss / freeze hardening (2026-07-14 live-restart audit). A wivrn restart under a live
// two-client session froze the desktop: the frame thread must never make a BLOCKING xr call that
// cannot time out, because the main thread's join() in stop() blocks with it and the compositor
// stops painting. These pin the bounds that keep every such wait finite so a careless bump (e.g.
// pushing the swapchain wait back toward XR_INFINITE_DURATION) is caught here.

TEST(loss_hardening, swapchain_wait_is_bounded_not_infinite) {
    // Nanoseconds. Must be strictly positive (a real ceiling, never XR_INFINITE_DURATION == INT64_MAX),
    // above any normal per-frame jitter (won't false-drop a session on a brief network hiccup), and
    // still small enough to bound the worst-case frame-thread stall — and hence the main-thread join.
    EXPECT_GT(XR_SWAPCHAIN_WAIT_TIMEOUT_NS, 0LL);
    EXPECT_GE(XR_SWAPCHAIN_WAIT_TIMEOUT_NS, 100'000'000LL); // >= 100ms: above normal jitter
    EXPECT_LE(XR_SWAPCHAIN_WAIT_TIMEOUT_NS, 10'000'000'000LL); // <= 10s: still a hard bound on the freeze
}

TEST(loss_hardening, frame_fail_streak_is_a_small_positive_backstop) {
    // The loop tolerates a few consecutive wait/begin failures (pollEvents normally classifies loss
    // within one iteration) but must latch loss well before it could busy-spin indefinitely.
    EXPECT_GT(XR_MAX_FRAME_FAIL_STREAK, 0);
    EXPECT_LE(XR_MAX_FRAME_FAIL_STREAK, 64);
}

TEST(loss_hardening, handshake_timeout_is_bounded_and_generous) {
    // Task #89 phase 2 (blocker B): the reconnect handshake (xrCreateInstance + xrGetSystem) now runs
    // FULLY off the main thread and marshals its result back — the main thread never parks on it, so this
    // constant is no longer an abandon deadline but the "runtime was slow to answer" WARN threshold.
    // Generous enough that a healthy cold start never trips the warning, bounded enough to stay meaningful.
    EXPECT_GE(XR_HANDSHAKE_TIMEOUT_MS, 1000); // a cold wivrn+encoder start can take a beat
    EXPECT_LE(XR_HANDSHAKE_TIMEOUT_MS, 30000);
}

TEST(loss_hardening, bringup_timeout_is_bounded_and_generous) {
    // Task #89 phase 2 (blocker A): direct-mode session bring-up (xrCreateSession .. input->init) runs on a
    // helper thread while the main thread parks in a bounded wait; on timeout the main thread abandons the
    // helper and falls back to UNAVAILABLE, so a cross-process deadlock with the runtime on a sick leased
    // connector no longer freezes the desktop. Generous enough that a healthy `xr --direct` never abandons,
    // bounded enough that the main thread's park can never exceed it.
    EXPECT_GE(XR_BRINGUP_TIMEOUT_MS, 1000);
    EXPECT_LE(XR_BRINGUP_TIMEOUT_MS, 30000);
    // Bring-up is the slower, headset-present leg; its ceiling should be at least the handshake threshold.
    EXPECT_GE(XR_BRINGUP_TIMEOUT_MS, XR_HANDSHAKE_TIMEOUT_MS);
}
