#include <openxr/XRMath.hpp>

#include <gtest/gtest.h>

#include <cmath>

using namespace OpenXR;

// tests/xr/ray_assist.cpp — the ray-aim / cursor / hover-assist pure helpers from
// docs/openxr/research/INTERACTION.md (report 14): xrRegionIsGrabbable, xrRegionRect,
// classifyQuadRegionForgiving (cone magnetism, chrome-only), stepHoverStick (sticky hover),
// aimFilterMinCutoff (pinch-onset damping), and the endpoint-cursor pack/color/tint/size helpers.
// Pure math: builds and runs with no OpenXR runtime.

namespace {
    // 16:9 content, 0.10 m margin, 0.08 m bar (0.8 width), 0.09 m corners — the shipped defaults.
    SXRChromeGeometry defaultGeom(float contentW = 1.6f, float contentH = 0.9f) {
        return makeChromeGeometry(contentW, contentH, 0.10f, 0.08f, 0.8f, 0.09f);
    }
}

// ---- xrRegionIsGrabbable ----

TEST(XRRayAssist, GrabbablePredicate) {
    EXPECT_TRUE(xrRegionIsGrabbable(XR_REGION_BAR));
    EXPECT_TRUE(xrRegionIsGrabbable(XR_REGION_CORNER_TL));
    EXPECT_TRUE(xrRegionIsGrabbable(XR_REGION_CORNER_BR));
    EXPECT_FALSE(xrRegionIsGrabbable(XR_REGION_BODY));
    EXPECT_FALSE(xrRegionIsGrabbable(XR_REGION_MARGIN));
    EXPECT_FALSE(xrRegionIsGrabbable(XR_REGION_NONE));
}

// ---- xrRegionRect: agrees with classifyQuadHit ----

TEST(XRRayAssist, RegionRectMatchesClassify) {
    auto g = defaultGeom();
    for (eXRQuadRegion r : {XR_REGION_BAR, XR_REGION_CORNER_TL, XR_REGION_CORNER_TR, XR_REGION_CORNER_BL, XR_REGION_CORNER_BR}) {
        float u0, v0, u1, v1;
        ASSERT_TRUE(xrRegionRect(g, r, u0, v0, u1, v1)) << "region " << (int)r;
        // The rect center classifies back to the same region.
        const float cu = 0.5f * (u0 + u1), cv = 0.5f * (v0 + v1);
        EXPECT_EQ(classifyQuadHit(cu, cv, g), r) << "region " << (int)r;
    }
    // Body / margin / none have no rect.
    float a, b, c, d;
    EXPECT_FALSE(xrRegionRect(g, XR_REGION_BODY, a, b, c, d));
    EXPECT_FALSE(xrRegionRect(g, XR_REGION_MARGIN, a, b, c, d));
    EXPECT_FALSE(xrRegionRect(g, XR_REGION_NONE, a, b, c, d));
}

// ---- classifyQuadRegionForgiving: content precision preserved, margin magnetized ----

TEST(XRRayAssist, ForgivingPreservesExactHits) {
    auto g = defaultGeom();
    // Dead-center is BODY, and no slack ever turns it into a handle (content precision).
    EXPECT_EQ(classifyQuadRegionForgiving(0.5f, 0.5f, g, 0.2f, 0.2f), XR_REGION_BODY);
    // An exact bar hit stays the bar.
    float u0, v0, u1, v1;
    ASSERT_TRUE(xrRegionRect(g, XR_REGION_BAR, u0, v0, u1, v1));
    const float bu = 0.5f * (u0 + u1), bv = 0.5f * (v0 + v1);
    EXPECT_EQ(classifyQuadRegionForgiving(bu, bv, g, 0.f, 0.f), XR_REGION_BAR);
}

TEST(XRRayAssist, ForgivingMagnetizesMarginToNearestHandle) {
    auto g = defaultGeom();
    float u0, v0, u1, v1;
    ASSERT_TRUE(xrRegionRect(g, XR_REGION_BAR, u0, v0, u1, v1));
    // A point just BELOW the bar (in the margin) with zero slack stays MARGIN...
    const float bu = 0.5f * (u0 + u1);
    const float justBelow = v1 + 0.01f;
    EXPECT_EQ(classifyQuadRegionForgiving(bu, justBelow, g, 0.f, 0.f), XR_REGION_MARGIN);
    // ...but with enough v slack it snaps to the bar.
    EXPECT_EQ(classifyQuadRegionForgiving(bu, justBelow, g, 0.f, 0.05f), XR_REGION_BAR);
    // Far away (beyond the slack) stays margin.
    EXPECT_EQ(classifyQuadRegionForgiving(bu, v1 + 0.2f, g, 0.f, 0.05f), XR_REGION_MARGIN);
}

// ---- stepHoverStick: sticky region, dropout tolerance, immediate handle switch ----

TEST(XRRayAssist, HoverStickHoldsThroughBodyDrift) {
    auto             g = defaultGeom();
    SXRHoverStick    s;
    float u0, v0, u1, v1;
    ASSERT_TRUE(xrRegionRect(g, XR_REGION_BAR, u0, v0, u1, v1));
    const float bu = 0.5f * (u0 + u1), bv = 0.5f * (v0 + v1);

    // Enter the bar: adopted immediately.
    EXPECT_EQ(stepHoverStick(s, XR_REGION_BAR, 7, bu, bv, g, 0.05f, 0.05f, 2), XR_REGION_BAR);
    EXPECT_EQ(s.mon, 7);

    // Ray drifts just off the bar onto the same monitor's body, but within the exit margin -> hold.
    const float nearU = u1 + 0.005f; // just past the right edge of the bar
    EXPECT_EQ(stepHoverStick(s, XR_REGION_BODY, 7, nearU, bv, g, 0.05f, 0.05f, 2), XR_REGION_BAR);

    // Drift far past the exit margin -> release to body.
    EXPECT_EQ(stepHoverStick(s, XR_REGION_BODY, 7, u1 + 0.3f, bv, g, 0.05f, 0.05f, 2), XR_REGION_BODY);
}

TEST(XRRayAssist, HoverStickToleratesDropout) {
    auto          g = defaultGeom();
    SXRHoverStick s;
    float u0, v0, u1, v1;
    ASSERT_TRUE(xrRegionRect(g, XR_REGION_CORNER_TL, u0, v0, u1, v1));
    const float cu = 0.5f * (u0 + u1), cv = 0.5f * (v0 + v1);
    EXPECT_EQ(stepHoverStick(s, XR_REGION_CORNER_TL, 3, cu, cv, g, 0.05f, 0.05f, 2), XR_REGION_CORNER_TL);

    // Two missed frames (ray off every quad) are tolerated (maxMiss = 2).
    EXPECT_EQ(stepHoverStick(s, XR_REGION_NONE, -1, 0, 0, g, 0.05f, 0.05f, 2), XR_REGION_CORNER_TL);
    EXPECT_EQ(stepHoverStick(s, XR_REGION_NONE, -1, 0, 0, g, 0.05f, 0.05f, 2), XR_REGION_CORNER_TL);
    // Third miss releases.
    EXPECT_EQ(stepHoverStick(s, XR_REGION_NONE, -1, 0, 0, g, 0.05f, 0.05f, 2), XR_REGION_NONE);
}

TEST(XRRayAssist, HoverStickDifferentHandleWinsImmediately) {
    auto          g = defaultGeom();
    SXRHoverStick s;
    float u0, v0, u1, v1;
    ASSERT_TRUE(xrRegionRect(g, XR_REGION_BAR, u0, v0, u1, v1));
    stepHoverStick(s, XR_REGION_BAR, 1, 0.5f * (u0 + u1), 0.5f * (v0 + v1), g, 0.05f, 0.05f, 2);
    // Landing a corner handle switches instantly (no hold lag on a fresh grabbable).
    ASSERT_TRUE(xrRegionRect(g, XR_REGION_CORNER_BR, u0, v0, u1, v1));
    EXPECT_EQ(stepHoverStick(s, XR_REGION_CORNER_BR, 1, 0.5f * (u0 + u1), 0.5f * (v0 + v1), g, 0.05f, 0.05f, 2), XR_REGION_CORNER_BR);
}

// ---- aimFilterMinCutoff: pinch-onset damping band ----

TEST(XRRayAssist, AimFilterPinchOnsetDamping) {
    // Below the onset floor: no damping.
    EXPECT_FLOAT_EQ(aimFilterMinCutoff(1.5f, 0.4f, 0.05f, 0.1f, 0.7f), 1.5f);
    // In the onset band: damped by the factor.
    EXPECT_FLOAT_EQ(aimFilterMinCutoff(1.5f, 0.4f, 0.4f, 0.1f, 0.7f), 1.5f * 0.4f);
    // At/above the trigger threshold (committed): back to base.
    EXPECT_FLOAT_EQ(aimFilterMinCutoff(1.5f, 0.4f, 0.9f, 0.1f, 0.7f), 1.5f);
}

// ---- endpoint cursor pack / unpack ----

TEST(XRRayAssist, CursorPackRoundTrip) {
    bool           present = false;
    eXRCursorState st      = XR_CURSOR_HIDDEN;
    float          u = 0, v = 0;
    const uint32_t w = xrPackCursor(true, XR_CURSOR_GRABBABLE, 0.25f, 0.75f);
    xrUnpackCursor(w, present, st, u, v);
    EXPECT_TRUE(present);
    EXPECT_EQ(st, XR_CURSOR_GRABBABLE);
    EXPECT_NEAR(u, 0.25f, 1e-3f);
    EXPECT_NEAR(v, 0.75f, 1e-3f);

    // Absent cursor.
    xrUnpackCursor(xrPackCursor(false, XR_CURSOR_IDLE, 0.f, 0.f), present, st, u, v);
    EXPECT_FALSE(present);

    // Clamping out-of-range uv.
    xrUnpackCursor(xrPackCursor(true, XR_CURSOR_PRESS, -0.5f, 1.9f), present, st, u, v);
    EXPECT_NEAR(u, 0.f, 1e-3f);
    EXPECT_NEAR(v, 1.f, 1e-3f);
}

// ---- cursor color / tint ----

TEST(XRRayAssist, CursorColorSelection) {
    EXPECT_EQ(xrCursorColorFor(XR_CURSOR_IDLE, 1, 2, 3, 4), 1u);
    EXPECT_EQ(xrCursorColorFor(XR_CURSOR_GRABBABLE, 1, 2, 3, 4), 2u);
    EXPECT_EQ(xrCursorColorFor(XR_CURSOR_PRESS, 1, 2, 3, 4), 3u);
    EXPECT_EQ(xrCursorColorFor(XR_CURSOR_GRAB, 1, 2, 3, 4), 4u);
    EXPECT_EQ(xrCursorColorFor(XR_CURSOR_HIDDEN, 1, 2, 3, 4), 1u);
}

TEST(XRRayAssist, CursorTintLeftHandCooler) {
    const uint32_t c = 0xFF804020; // a=ff r=80 g=40 b=20
    // Right hand: identity.
    EXPECT_EQ(xrCursorTint(c, 1, true), c);
    // Disabled: identity for either hand.
    EXPECT_EQ(xrCursorTint(c, 0, false), c);
    // Left hand: red pulled down, blue pushed up, alpha + green preserved.
    const uint32_t l = xrCursorTint(c, 0, true);
    EXPECT_EQ((l >> 24) & 0xFF, 0xFFu);          // alpha preserved
    EXPECT_EQ((l >> 8) & 0xFF, 0x40u);           // green preserved
    EXPECT_LT((l >> 16) & 0xFF, 0x80u);          // red down
    EXPECT_GT(l & 0xFF, 0x20u);                  // blue up
}

// ---- cursor sizing ----

TEST(XRRayAssist, CursorRadiusMetricRound) {
    float ru = 0, rv = 0;
    // 0.02 m cursor on a 2 m x 1 m quad -> ru = 0.005, rv = 0.01 (round in meters).
    xrCursorRadiusUV(0.02f, 2.0f, 1.0f, false, 1.6f, ru, rv);
    EXPECT_NEAR(ru, 0.005f, 1e-5f);
    EXPECT_NEAR(rv, 0.010f, 1e-5f);
    // Pressed grows by the press scale.
    float ru2 = 0, rv2 = 0;
    xrCursorRadiusUV(0.02f, 2.0f, 1.0f, true, 1.6f, ru2, rv2);
    EXPECT_NEAR(ru2, 0.005f * 1.6f, 1e-5f);
    EXPECT_NEAR(rv2, 0.010f * 1.6f, 1e-5f);
    // Degenerate quad meters -> zero radius (no divide-by-zero).
    float ru3 = 1, rv3 = 1;
    xrCursorRadiusUV(0.02f, 0.f, 0.f, false, 1.6f, ru3, rv3);
    EXPECT_FLOAT_EQ(ru3, 0.f);
    EXPECT_FLOAT_EQ(rv3, 0.f);
}
