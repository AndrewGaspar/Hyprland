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

// The decision now takes a resolved (gpusKnown, sameGpu) pair — the caller compares the physical
// devices with DRM::sameGpu (drmDevicesEqual), which is node-type agnostic. That replaces the old
// raw major/minor compare, which mis-read a single-GPU box as cross-GPU because the XR *render* node
// and the allocator *card* node of one GPU never share a device number (the NVIDIA all-black bug).

// ---- shouldForceLinear: ON / OFF ignore the devices ----

TEST(XRForceLinear, OnAlwaysForces) {
    // Same GPU, but ON forces regardless.
    EXPECT_TRUE(shouldForceLinear(XR_LINEAR_ON, true, true));
    // Even with unresolved devices.
    EXPECT_TRUE(shouldForceLinear(XR_LINEAR_ON, false, true));
}

TEST(XRForceLinear, OffNeverForces) {
    // Different GPU, but OFF never forces (debug/escape hatch).
    EXPECT_FALSE(shouldForceLinear(XR_LINEAR_OFF, true, false));
}

// ---- shouldForceLinear: AUTO ----

TEST(XRForceLinear, AutoForcesOnCrossGpu) {
    // Genuine split: XR on NVIDIA, buffers allocated on the AMD iGPU. DRM::sameGpu → false.
    EXPECT_TRUE(shouldForceLinear(XR_LINEAR_AUTO, true, false));
}

TEST(XRForceLinear, AutoLeavesSameGpuNative) {
    // Single-GPU: same physical device -> keep native tiling (linear costs compositing throughput,
    // and NVIDIA can't even produce a linear scanout buffer, which black-screened the panel).
    EXPECT_FALSE(shouldForceLinear(XR_LINEAR_AUTO, true, true));
}

TEST(XRForceLinear, AutoSameGpuAcrossNodeTypesStaysNative) {
    // Regression for the NVIDIA all-black bug: the XR render node (226:128) and the allocator card
    // node (226:1) are the SAME GPU. The caller's DRM::sameGpu resolves that to sameGpu=true, so even
    // though their device numbers differ, AUTO must NOT force linear.
    EXPECT_FALSE(shouldForceLinear(XR_LINEAR_AUTO, true, true));
}

TEST(XRForceLinear, AutoUnknownDevicesStayNative) {
    // Either device unresolved (shared-display fallback, unstat-able/unopenable fd) -> can't confirm
    // cross-GPU -> stay native.
    EXPECT_FALSE(shouldForceLinear(XR_LINEAR_AUTO, false, true));
    EXPECT_FALSE(shouldForceLinear(XR_LINEAR_AUTO, false, false));
}
