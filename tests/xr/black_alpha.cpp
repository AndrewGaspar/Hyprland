#include <openxr/XRMath.hpp>
#include <openxr/XRMonitorConfig.hpp>

#include <gtest/gtest.h>

using namespace OpenXR;

// Luma-keyed transparency, a.k.a. "black-as-alpha" (openxr:black_alpha / :black_alpha_knee,
// docs/openxr/research/archive/09-monitor-transparency.md). xrLumaKeyAlpha is the HOST-SIDE
// REFERENCE of the curve the blit fragment shader computes (CXRGraphics::initBlitGL) and the exact
// function the CPU clear paths use, so these tests pin the shape the shader must reproduce:
//
//   luma  = dot(rgb, Rec.709)
//   alpha = mix(black_alpha, 1.0, smoothstep(0.0, knee, luma))
//
// plus the premultiplication rule (rgb *= alpha) that keeps the composite correct for our
// premultiplied quads.

// ---- feature off (the shipped default) ----

TEST(XRBlackAlpha, DefaultIsFullyOpaque) {
    // black_alpha = 1.0 is the "off" switch: EVERY pixel, including pure black, stays opaque, so the
    // blit is bit-identical to the historic forced fragColor.a = 1.0.
    EXPECT_FLOAT_EQ(xrLumaKeyAlpha(0.F, 0.F, 0.F, 1.F, 0.1F), 1.F);
    EXPECT_FLOAT_EQ(xrLumaKeyAlpha(0.5F, 0.5F, 0.5F, 1.F, 0.1F), 1.F);
    EXPECT_FLOAT_EQ(xrLumaKeyAlpha(1.F, 1.F, 1.F, 1.F, 0.1F), 1.F);
    EXPECT_FALSE(xrBlackKeyActive(1.F));
}

TEST(XRBlackAlpha, OutOfRangeAlphaIsClamped) {
    // Above 1 clamps to "off"; below 0 clamps to fully transparent black.
    EXPECT_FLOAT_EQ(xrLumaKeyAlpha(0.F, 0.F, 0.F, 5.F, 0.1F), 1.F);
    EXPECT_FLOAT_EQ(xrLumaKeyAlpha(0.F, 0.F, 0.F, -3.F, 0.1F), 0.F);
}

TEST(XRBlackAlpha, ActivePredicate) {
    EXPECT_TRUE(xrBlackKeyActive(0.F));
    EXPECT_TRUE(xrBlackKeyActive(0.99F));
    EXPECT_FALSE(xrBlackKeyActive(1.F));
}

// ---- the curve ----

TEST(XRBlackAlpha, PureBlackGetsTheConfiguredAlpha) {
    EXPECT_FLOAT_EQ(xrLumaKeyAlpha(0.F, 0.F, 0.F, 0.F, 0.1F), 0.F);
    EXPECT_FLOAT_EQ(xrLumaKeyAlpha(0.F, 0.F, 0.F, 0.2F, 0.1F), 0.2F);
    EXPECT_FLOAT_EQ(xrLumaKeyAlpha(0.F, 0.F, 0.F, 0.75F, 0.5F), 0.75F);
}

TEST(XRBlackAlpha, AtOrAboveTheKneeIsFullyOpaque) {
    // The whole point of the knee: real content (text, windows, wallpaper) must not go translucent.
    EXPECT_FLOAT_EQ(xrLumaKeyAlpha(0.1F, 0.1F, 0.1F, 0.F, 0.1F), 1.F); // luma == knee
    EXPECT_FLOAT_EQ(xrLumaKeyAlpha(0.5F, 0.5F, 0.5F, 0.F, 0.1F), 1.F);
    EXPECT_FLOAT_EQ(xrLumaKeyAlpha(1.F, 1.F, 1.F, 0.F, 0.1F), 1.F);
}

TEST(XRBlackAlpha, RampIsMonotonicAndSmooth) {
    const float ba = 0.2F, knee = 0.25F;
    float       prev = xrLumaKeyAlphaFromLuma(0.F, ba, knee);
    EXPECT_FLOAT_EQ(prev, ba);
    for (int i = 1; i <= 100; ++i) {
        const float luma = (float)i / 100.F;
        const float a    = xrLumaKeyAlphaFromLuma(luma, ba, knee);
        EXPECT_GE(a, prev - 1e-6F) << "alpha must never decrease with luma (luma=" << luma << ")";
        EXPECT_GE(a, ba);
        EXPECT_LE(a, 1.F);
        prev = a;
    }
    EXPECT_FLOAT_EQ(prev, 1.F);
}

TEST(XRBlackAlpha, MidKneeIsTheSmoothstepMidpoint) {
    // smoothstep(0, knee, knee/2) == 0.5 exactly, so the alpha sits halfway between black_alpha and 1.
    const float ba = 0.2F, knee = 0.4F;
    EXPECT_NEAR(xrLumaKeyAlphaFromLuma(knee * 0.5F, ba, knee), ba + (1.F - ba) * 0.5F, 1e-6F);
}

TEST(XRBlackAlpha, SmallerKneeKeepsDarkGreysOpaque) {
    // Dark-theme guidance: a tight knee dissolves only near-black, so a #202020 background stays solid.
    const float darkGrey = 0.125F; // ~ #202020 in 0..1
    EXPECT_FLOAT_EQ(xrLumaKeyAlpha(darkGrey, darkGrey, darkGrey, 0.F, 0.05F), 1.F);
    EXPECT_LT(xrLumaKeyAlpha(darkGrey, darkGrey, darkGrey, 0.F, 0.4F), 1.F);
}

TEST(XRBlackAlpha, UsesRec709Luma) {
    // Green is by far the heaviest channel; blue the lightest. A pure-blue pixel is therefore much
    // closer to "black" than a pure-green one at the same nominal intensity.
    EXPECT_NEAR(xrRec709Luma(1.F, 0.F, 0.F), 0.2126F, 1e-6F);
    EXPECT_NEAR(xrRec709Luma(0.F, 1.F, 0.F), 0.7152F, 1e-6F);
    EXPECT_NEAR(xrRec709Luma(0.F, 0.F, 1.F), 0.0722F, 1e-6F);
    const float knee = 0.15F;
    EXPECT_LT(xrLumaKeyAlpha(0.F, 0.F, 0.1F, 0.F, knee), xrLumaKeyAlpha(0.F, 0.1F, 0.F, 0.F, knee));
}

TEST(XRBlackAlpha, ZeroKneeDegradesToAHardCutoff) {
    // knee <= 0 would divide by zero in the shader; both sides clamp it to XR_BLACK_ALPHA_KNEE_MIN,
    // which reads as a hard key: pure black transparent, anything brighter opaque.
    EXPECT_FLOAT_EQ(xrLumaKeyAlpha(0.F, 0.F, 0.F, 0.F, 0.F), 0.F);
    EXPECT_FLOAT_EQ(xrLumaKeyAlpha(0.02F, 0.02F, 0.02F, 0.F, 0.F), 1.F);
    EXPECT_FLOAT_EQ(xrLumaKeyAlpha(0.F, 0.F, 0.F, 0.F, -1.F), 0.F);
}

// ---- premultiplication (the load-bearing detail, report 09 §2.1) ----

TEST(XRBlackAlpha, OutputIsPremultiplied) {
    // Our quads carry no XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT and the runtime blends
    // src=ONE, dst=ONE_MINUS_SRC_ALPHA. rgb MUST be scaled by the same alpha — scaling alpha alone
    // leaves rgb > a and the content is added at full brightness over passthrough (additive halo).
    float r = 0.F, g = 0.F, b = 0.F, a = 0.F;
    xrLumaKeyPremultiplied(0.04F, 0.04F, 0.04F, 0.25F, 0.2F, r, g, b, a);
    EXPECT_LT(a, 1.F);
    EXPECT_GT(a, 0.25F);
    EXPECT_NEAR(r, 0.04F * a, 1e-6F);
    EXPECT_NEAR(g, 0.04F * a, 1e-6F);
    EXPECT_NEAR(b, 0.04F * a, 1e-6F);
    // Valid premultiplied means every channel <= alpha.
    EXPECT_LE(r, a + 1e-6F);
}

TEST(XRBlackAlpha, PremultipliedBlackStaysBlack) {
    // The clear/fallback path: rgb is already 0, so only alpha moves.
    float r = 1.F, g = 1.F, b = 1.F, a = 1.F;
    xrLumaKeyPremultiplied(0.F, 0.F, 0.F, 0.3F, 0.1F, r, g, b, a);
    EXPECT_FLOAT_EQ(r, 0.F);
    EXPECT_FLOAT_EQ(g, 0.F);
    EXPECT_FLOAT_EQ(b, 0.F);
    EXPECT_FLOAT_EQ(a, 0.3F);
}

TEST(XRBlackAlpha, PremultipliedIsAPassThroughWhenOff) {
    float r = 0.F, g = 0.F, b = 0.F, a = 0.F;
    xrLumaKeyPremultiplied(0.25F, 0.5F, 0.75F, 1.F, 0.1F, r, g, b, a);
    EXPECT_FLOAT_EQ(a, 1.F);
    EXPECT_FLOAT_EQ(r, 0.25F);
    EXPECT_FLOAT_EQ(g, 0.5F);
    EXPECT_FLOAT_EQ(b, 0.75F);
}

// ---- blend-mode gating (report 09 §3.1) ----

TEST(XRBlackAlpha, OnlySeeThroughBlendModesShowIt) {
    // Under OPAQUE the runtime paints black behind the layers, so a keyed monitor would just look
    // dim and dirty. COpenXRManager::publishBlackAlphaTuning forces the effective value to 1.0 there
    // (and warns once); alpha/additive are the modes where the key actually reveals the room.
    EXPECT_FALSE(blendModeShowsThrough(XR_BLEND_OPAQUE));
    EXPECT_TRUE(blendModeShowsThrough(XR_BLEND_ALPHA));
    EXPECT_TRUE(blendModeShowsThrough(XR_BLEND_ADDITIVE));
}

// ---- the gate as the manager applies it (mirrors publishBlackAlphaTuning's pure part) ----

namespace {
    // The exact resolution publishBlackAlphaTuning performs, minus the config/logging plumbing.
    float effectiveBlackAlpha(float configured, eXRBlendMode mode) {
        const float want = std::clamp(configured, 0.F, 1.F);
        return blendModeShowsThrough(mode) ? want : 1.F;
    }
}

TEST(XRBlackAlpha, GateResolution) {
    EXPECT_FLOAT_EQ(effectiveBlackAlpha(0.2F, XR_BLEND_ALPHA), 0.2F);
    EXPECT_FLOAT_EQ(effectiveBlackAlpha(0.2F, XR_BLEND_ADDITIVE), 0.2F);
    EXPECT_FLOAT_EQ(effectiveBlackAlpha(0.2F, XR_BLEND_OPAQUE), 1.F);   // ignored
    EXPECT_FLOAT_EQ(effectiveBlackAlpha(1.F, XR_BLEND_ALPHA), 1.F);     // off stays off
    EXPECT_FLOAT_EQ(effectiveBlackAlpha(-0.5F, XR_BLEND_ALPHA), 0.F);   // clamped
    EXPECT_FLOAT_EQ(effectiveBlackAlpha(2.F, XR_BLEND_ALPHA), 1.F);     // clamped
}
