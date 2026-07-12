#include <openxr/XRMonitorConfig.hpp>

#include <gtest/gtest.h>

using namespace OpenXR;

// parseForceLinearMode + shouldForceLinear (XRMonitorConfig.cpp) are the pure decision behind the
// cross-GPU black-screen fix (research/17 addendum): when the XR EGL context lives on a different
// GPU than the compositor allocates the headless output's buffers on, that output must allocate
// LINEAR buffers so the XR GPU can import them. COpenXRManager::applyCrossGpuLinear feeds the live
// DRM node numbers into this; the tests pin the truth table so the wiring can rely on it blindly.

// ---- parseForceLinearMode ----

TEST(XRForceLinear, ParseAuto) {
    EXPECT_EQ(parseForceLinearMode("auto"), XR_LINEAR_AUTO);
    EXPECT_EQ(parseForceLinearMode(""), XR_LINEAR_AUTO);        // empty -> default
    EXPECT_EQ(parseForceLinearMode("nonsense"), XR_LINEAR_AUTO); // unrecognized -> default
}

TEST(XRForceLinear, ParseOn) {
    EXPECT_EQ(parseForceLinearMode("on"), XR_LINEAR_ON);
    EXPECT_EQ(parseForceLinearMode("true"), XR_LINEAR_ON);
    EXPECT_EQ(parseForceLinearMode("1"), XR_LINEAR_ON);
    EXPECT_EQ(parseForceLinearMode("yes"), XR_LINEAR_ON);
}

TEST(XRForceLinear, ParseOff) {
    EXPECT_EQ(parseForceLinearMode("off"), XR_LINEAR_OFF);
    EXPECT_EQ(parseForceLinearMode("false"), XR_LINEAR_OFF);
    EXPECT_EQ(parseForceLinearMode("0"), XR_LINEAR_OFF);
    EXPECT_EQ(parseForceLinearMode("no"), XR_LINEAR_OFF);
}

// ---- shouldForceLinear: ON / OFF ignore the nodes ----

TEST(XRForceLinear, OnAlwaysForces) {
    // Same GPU, but ON forces regardless.
    EXPECT_TRUE(shouldForceLinear(XR_LINEAR_ON, true, 226, 128, true, 226, 128));
    // Even with unknown nodes.
    EXPECT_TRUE(shouldForceLinear(XR_LINEAR_ON, false, -1, -1, false, -1, -1));
}

TEST(XRForceLinear, OffNeverForces) {
    // Different GPU, but OFF never forces (debug/escape hatch).
    EXPECT_FALSE(shouldForceLinear(XR_LINEAR_OFF, true, 226, 128, true, 226, 129));
}

// ---- shouldForceLinear: AUTO ----

TEST(XRForceLinear, AutoForcesOnCrossGpu) {
    // The live fishfood case: XR on NVIDIA renderD128 (226:128), buffers on AMD renderD129 (226:129).
    EXPECT_TRUE(shouldForceLinear(XR_LINEAR_AUTO, true, 226, 128, true, 226, 129));
}

TEST(XRForceLinear, AutoLeavesSameGpuNative) {
    // Single-GPU: identical nodes -> keep native tiling (linear costs compositing throughput).
    EXPECT_FALSE(shouldForceLinear(XR_LINEAR_AUTO, true, 226, 128, true, 226, 128));
}

TEST(XRForceLinear, AutoDiffersOnMinorOnly) {
    EXPECT_TRUE(shouldForceLinear(XR_LINEAR_AUTO, true, 226, 128, true, 226, 130));
}

TEST(XRForceLinear, AutoDiffersOnMajorOnly) {
    EXPECT_TRUE(shouldForceLinear(XR_LINEAR_AUTO, true, 225, 0, true, 226, 0));
}

TEST(XRForceLinear, AutoUnknownXrNodeStaysNative) {
    // Shared-display fallback: XR render node unknown -> can't confirm cross-GPU -> stay native.
    EXPECT_FALSE(shouldForceLinear(XR_LINEAR_AUTO, false, -1, -1, true, 226, 129));
}

TEST(XRForceLinear, AutoUnknownAllocatorStaysNative) {
    EXPECT_FALSE(shouldForceLinear(XR_LINEAR_AUTO, true, 226, 128, false, -1, -1));
}
