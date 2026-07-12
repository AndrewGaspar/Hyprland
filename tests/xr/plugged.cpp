#include <openxr/XRMonitorConfig.hpp>

#include <gtest/gtest.h>

using namespace OpenXR;

// wantXRMonitorsPlugged (XRMonitorConfig.cpp) is the pure plugged-state policy behind
// research/18 (XR monitors behave like unplugged external monitors while no OpenXR session
// exists). COpenXRManager::setMonitorsPlugged and createXRMonitor both consult it; these tests
// pin the full truth table so the compositor-side wiring can rely on it blindly.

// ---- monitors_follow_session = true (the default): plugged tracks session existence ----

TEST(XRPluggedPolicy, FollowSessionUnpluggedWhileSessionless) {
    // The fishfood login case: xrmonitor= lines materialize at init() with no headset in sight.
    EXPECT_FALSE(wantXRMonitorsPlugged(/*followSession*/ true, /*sessionUp*/ false));
}

TEST(XRPluggedPolicy, FollowSessionPluggedWhileSessionUp) {
    EXPECT_TRUE(wantXRMonitorsPlugged(/*followSession*/ true, /*sessionUp*/ true));
}

// ---- monitors_follow_session = false: the pre-feature always-present behavior ----

TEST(XRPluggedPolicy, OptOutPluggedWhileSessionless) {
    EXPECT_TRUE(wantXRMonitorsPlugged(/*followSession*/ false, /*sessionUp*/ false));
}

TEST(XRPluggedPolicy, OptOutPluggedWhileSessionUp) {
    EXPECT_TRUE(wantXRMonitorsPlugged(/*followSession*/ false, /*sessionUp*/ true));
}
