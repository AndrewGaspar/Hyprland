#include <openxr/XRMonitorConfig.hpp>

#include <gtest/gtest.h>

#include <vector>

using namespace OpenXR;

// pickBlendMode (XRMonitorConfig.cpp) is the pure, HAVE_OPENXR-free selection COpenXRManager::start()
// runs against the runtime's enumerated env-blend-mode list (doc 01). These tests exercise every
// branch without a runtime.

// ---- auto => the runtime's first-listed (preferred) mode ----

TEST(XRBlendMode, AutoTakesRuntimePreferred) {
    // WiVRn-style: alpha preferred first, then opaque.
    auto p = pickBlendMode({XR_BLEND_ALPHA, XR_BLEND_OPAQUE}, "auto");
    EXPECT_EQ(p.mode, XR_BLEND_ALPHA);
    EXPECT_FALSE(p.requestedUnsupported);
}

TEST(XRBlendMode, AutoOpaqueOnlyRuntime) {
    // Monado null-compositor advertises OPAQUE only.
    auto p = pickBlendMode({XR_BLEND_OPAQUE}, "auto");
    EXPECT_EQ(p.mode, XR_BLEND_OPAQUE);
    EXPECT_FALSE(p.requestedUnsupported);
}

TEST(XRBlendMode, UnrecognizedConfigActsLikeAuto) {
    auto p = pickBlendMode({XR_BLEND_ADDITIVE, XR_BLEND_OPAQUE}, "garbage");
    EXPECT_EQ(p.mode, XR_BLEND_ADDITIVE);
    EXPECT_FALSE(p.requestedUnsupported);
}

TEST(XRBlendMode, EmptyConfigActsLikeAuto) {
    auto p = pickBlendMode({XR_BLEND_ALPHA, XR_BLEND_OPAQUE}, "");
    EXPECT_EQ(p.mode, XR_BLEND_ALPHA);
    EXPECT_FALSE(p.requestedUnsupported);
}

// ---- explicit + supported => honored, no fallback ----

TEST(XRBlendMode, ExplicitSupportedNonPreferred) {
    // opaque is supported but not the preferred (first) one — still honored exactly.
    auto p = pickBlendMode({XR_BLEND_ALPHA, XR_BLEND_OPAQUE}, "opaque");
    EXPECT_EQ(p.mode, XR_BLEND_OPAQUE);
    EXPECT_FALSE(p.requestedUnsupported);
}

TEST(XRBlendMode, ExplicitAlphaSupported) {
    auto p = pickBlendMode({XR_BLEND_OPAQUE, XR_BLEND_ALPHA}, "alpha");
    EXPECT_EQ(p.mode, XR_BLEND_ALPHA);
    EXPECT_FALSE(p.requestedUnsupported);
}

TEST(XRBlendMode, ExplicitAdditiveSupported) {
    auto p = pickBlendMode({XR_BLEND_ADDITIVE}, "additive");
    EXPECT_EQ(p.mode, XR_BLEND_ADDITIVE);
    EXPECT_FALSE(p.requestedUnsupported);
}

// ---- explicit + unsupported => fall back to preferred, flag set ----

TEST(XRBlendMode, ExplicitUnsupportedFallsBackToPreferred) {
    // User asked for alpha but the runtime only offers opaque (the on-box Monado case).
    auto p = pickBlendMode({XR_BLEND_OPAQUE}, "alpha");
    EXPECT_EQ(p.mode, XR_BLEND_OPAQUE);
    EXPECT_TRUE(p.requestedUnsupported);
}

TEST(XRBlendMode, ExplicitUnsupportedFallsBackToNonOpaquePreferred) {
    // Preferred is additive; requested alpha is not advertised -> fall back to additive.
    auto p = pickBlendMode({XR_BLEND_ADDITIVE, XR_BLEND_OPAQUE}, "alpha");
    EXPECT_EQ(p.mode, XR_BLEND_ADDITIVE);
    EXPECT_TRUE(p.requestedUnsupported);
}

// ---- empty list edge case (spec-illegal, defended) ----

TEST(XRBlendMode, EmptyListAutoDefaultsOpaque) {
    auto p = pickBlendMode({}, "auto");
    EXPECT_EQ(p.mode, XR_BLEND_OPAQUE);
    EXPECT_FALSE(p.requestedUnsupported);
}

TEST(XRBlendMode, EmptyListExplicitDefaultsOpaqueWithFallback) {
    auto p = pickBlendMode({}, "alpha");
    EXPECT_EQ(p.mode, XR_BLEND_OPAQUE);
    EXPECT_TRUE(p.requestedUnsupported);
}

// ---- string form ----

TEST(XRBlendMode, ToString) {
    EXPECT_EQ(blendModeToString(XR_BLEND_OPAQUE), "opaque");
    EXPECT_EQ(blendModeToString(XR_BLEND_ALPHA), "alpha");
    EXPECT_EQ(blendModeToString(XR_BLEND_ADDITIVE), "additive");
}
