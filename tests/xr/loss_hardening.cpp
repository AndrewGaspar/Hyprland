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
    // The reconnect handshake (xrCreateInstance + xrGetSystem) runs off the main thread with this
    // ceiling. Generous enough that a healthy cold start never abandons, bounded enough that a wedged
    // runtime cannot keep the (main-thread) probe waiting forever had it not been moved off-thread.
    EXPECT_GE(XR_HANDSHAKE_TIMEOUT_MS, 1000); // a cold wivrn+encoder start can take a beat
    EXPECT_LE(XR_HANDSHAKE_TIMEOUT_MS, 30000);
}
