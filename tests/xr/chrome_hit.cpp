#include <openxr/XRMath.hpp>

#include <gtest/gtest.h>

#include <cmath>

using namespace OpenXR;

// tests/xr/chrome_hit.cpp — the transparent-chrome-margin geometry from
// docs/openxr/research/04-grabbable-borders.md §8 (WP-G1): makeChromeGeometry + classifyQuadHit +
// remapToContentUV + contentPoseToQuadCenter. Pure math: builds and runs with no OpenXR runtime.

namespace {
    // Representative 16:9 content, 0.04 m margin, 0.05 m bar (0.6 width), 0.06 m corners.
    SXRChromeGeometry defaultGeom(float contentW = 1.6f, float contentH = 0.9f) {
        return makeChromeGeometry(contentW, contentH, 0.04f, 0.05f, 0.6f, 0.06f);
    }
}

// ---- makeChromeGeometry ----

TEST(XRChrome, ContentRectInsetSymmetricHoriz) {
    auto g = defaultGeom();
    // Horizontal margins are symmetric: content is centered on u.
    EXPECT_NEAR(g.contentCenterU(), 0.5f, 1e-5f);
    // Content occupies less than the full quad on both axes.
    EXPECT_GT(g.contentU0, 0.f);
    EXPECT_LT(g.contentU1, 1.f);
    EXPECT_GT(g.contentV0, 0.f);
    EXPECT_LT(g.contentV1, 1.f);
}

TEST(XRChrome, BottomMarginLargerThanTop) {
    auto g = defaultGeom();
    // The bar lives in the bottom margin -> content sits ABOVE the quad center (centerV < 0.5).
    EXPECT_LT(g.contentCenterV(), 0.5f);
    const float topMargin = g.contentV0;          // fraction above content
    const float botMargin = 1.f - g.contentV1;    // fraction below content
    EXPECT_GT(botMargin, topMargin);
}

TEST(XRChrome, DisabledWhenZeroMarginZeroBar) {
    auto g = makeChromeGeometry(1.6f, 0.9f, 0.f, 0.f, 0.6f, 0.06f);
    EXPECT_NEAR(g.contentU0, 0.f, 1e-6f);
    EXPECT_NEAR(g.contentV0, 0.f, 1e-6f);
    EXPECT_NEAR(g.contentU1, 1.f, 1e-6f);
    EXPECT_NEAR(g.contentV1, 1.f, 1e-6f);
    // Anything classifies BODY when chrome is disabled.
    EXPECT_EQ(classifyQuadHit(0.f, 0.f, g), XR_REGION_BODY);
    EXPECT_EQ(classifyQuadHit(1.f, 1.f, g), XR_REGION_BODY);
    EXPECT_EQ(classifyQuadHit(0.5f, 0.5f, g), XR_REGION_BODY);
}

TEST(XRChrome, DegenerateContentReturnsIdentity) {
    auto g = makeChromeGeometry(0.f, 0.f, 0.04f, 0.05f, 0.6f, 0.06f);
    EXPECT_FALSE(g.hasContentRect() && (g.contentU0 > 0.f)); // identity full-quad rect
    EXPECT_NEAR(g.contentFracW(), 1.f, 1e-6f);
}

// ---- classifyQuadHit ----

TEST(XRChrome, CenterIsBody) {
    auto g = defaultGeom();
    EXPECT_EQ(classifyQuadHit(0.5f, g.contentCenterV(), g), XR_REGION_BODY);
    // A point clearly inside the content rect.
    EXPECT_EQ(classifyQuadHit(g.contentCenterU(), g.contentV0 + 0.01f, g), XR_REGION_BODY);
}

TEST(XRChrome, CornersClassifyAsCorners) {
    auto g = defaultGeom();
    // Just outside each content corner, within the corner handle band (diagonal into margin).
    const float du = g.cornerU * 0.5f, dv = g.cornerV * 0.5f;
    EXPECT_EQ(classifyQuadHit(g.contentU0 - du, g.contentV0 - dv, g), XR_REGION_CORNER_TL);
    EXPECT_EQ(classifyQuadHit(g.contentU1 + du, g.contentV0 - dv, g), XR_REGION_CORNER_TR);
    EXPECT_EQ(classifyQuadHit(g.contentU0 - du, g.contentV1 + dv, g), XR_REGION_CORNER_BL);
    EXPECT_EQ(classifyQuadHit(g.contentU1 + du, g.contentV1 + dv, g), XR_REGION_CORNER_BR);
}

TEST(XRChrome, BarClassifiesAsBar) {
    auto g = defaultGeom();
    // Center of the bar: content-center u, just below the content bottom edge.
    const float barMidU = 0.5f * (g.barU0 + g.barU1);
    const float barMidV = 0.5f * (g.barV0 + g.barV1);
    EXPECT_EQ(classifyQuadHit(barMidU, barMidV, g), XR_REGION_BAR);
    // The bar is centered under the content.
    EXPECT_NEAR(barMidU, g.contentCenterU(), 1e-5f);
}

TEST(XRChrome, DeadMarginClassifiesAsMargin) {
    auto g = defaultGeom();
    // Top-center margin strip (above content, away from corners) is dead margin.
    EXPECT_EQ(classifyQuadHit(0.5f, g.contentV0 * 0.5f, g), XR_REGION_MARGIN);
    // Far bottom-left away from bar/corner.
    EXPECT_EQ(classifyQuadHit(0.5f, 0.999f, g), XR_REGION_MARGIN);
}

TEST(XRChrome, BodyEdgesInclusive) {
    auto g = defaultGeom();
    // Exactly on the content edge counts as body (no click pixel is stolen at the seam).
    EXPECT_EQ(classifyQuadHit(g.contentU0, g.contentCenterV(), g), XR_REGION_BODY);
    EXPECT_EQ(classifyQuadHit(g.contentU1, g.contentCenterV(), g), XR_REGION_BODY);
}

// ---- remapToContentUV ----

TEST(XRChrome, RemapContentCenterIsHalf) {
    auto  g = defaultGeom();
    float cu = 0.f, cv = 0.f;
    ASSERT_TRUE(remapToContentUV(g.contentCenterU(), g.contentCenterV(), g, cu, cv));
    EXPECT_NEAR(cu, 0.5f, 1e-5f);
    EXPECT_NEAR(cv, 0.5f, 1e-5f);
}

TEST(XRChrome, RemapContentCornersAreZeroOne) {
    auto  g = defaultGeom();
    float cu = 0.f, cv = 0.f;
    ASSERT_TRUE(remapToContentUV(g.contentU0, g.contentV0, g, cu, cv));
    EXPECT_NEAR(cu, 0.f, 1e-5f);
    EXPECT_NEAR(cv, 0.f, 1e-5f);
    ASSERT_TRUE(remapToContentUV(g.contentU1, g.contentV1, g, cu, cv));
    EXPECT_NEAR(cu, 1.f, 1e-5f);
    EXPECT_NEAR(cv, 1.f, 1e-5f);
}

TEST(XRChrome, RemapOutsideContentFails) {
    auto  g = defaultGeom();
    float cu = 0.f, cv = 0.f;
    // A margin point (above content) is not a valid content uv.
    EXPECT_FALSE(remapToContentUV(0.5f, g.contentV0 * 0.5f, g, cu, cv));
}

TEST(XRChrome, RemapPreservesClickPixel) {
    // The KEY invariant: a BODY hit at full-quad uv maps to the SAME content uv the desktop was
    // blitted at, so a click lands on the exact same pixel as before chrome margins existed.
    auto g = defaultGeom();
    // Pick a content-uv target (e.g. 0.25, 0.75), find its full-quad uv, remap back.
    const float wantCU = 0.25f, wantCV = 0.75f;
    const float fullU  = g.contentU0 + wantCU * g.contentFracW();
    const float fullV  = g.contentV0 + wantCV * g.contentFracH();
    ASSERT_EQ(classifyQuadHit(fullU, fullV, g), XR_REGION_BODY);
    float cu = 0.f, cv = 0.f;
    ASSERT_TRUE(remapToContentUV(fullU, fullV, g, cu, cv));
    EXPECT_NEAR(cu, wantCU, 1e-5f);
    EXPECT_NEAR(cv, wantCV, 1e-5f);
}

// ---- metric evenness across aspect ratios ----

TEST(XRChrome, MarginMetricallyEvenAcrossAspect) {
    // The margin is a fixed number of METERS, so its uv fraction scales inversely with the quad
    // extent on each axis but is the SAME metric thickness on all four sides regardless of aspect.
    const float margin = 0.04f;
    for (auto [w, h] : {std::pair{1.6f, 0.9f}, std::pair{1.0f, 1.0f}, std::pair{0.5f, 2.0f}}) {
        auto        g     = makeChromeGeometry(w, h, margin, 0.f, 0.f, 0.f); // no bar -> symmetric
        const float quadW = w + 2.f * margin;
        const float quadH = h + 2.f * margin;
        // Left margin in meters == top margin in meters == the config margin.
        EXPECT_NEAR(g.contentU0 * quadW, margin, 1e-4f);
        EXPECT_NEAR(g.contentV0 * quadH, margin, 1e-4f);
        EXPECT_NEAR((1.f - g.contentU1) * quadW, margin, 1e-4f);
        // With no bar, vertical is symmetric too.
        EXPECT_NEAR(g.contentCenterV(), 0.5f, 1e-5f);
    }
}

TEST(XRChrome, CornersMetricallySquare) {
    // A corner handle is cornerSize meters on BOTH axes, even on a very non-square quad.
    const float corner = 0.03f, margin = 0.04f;
    auto        g      = makeChromeGeometry(0.5f, 2.0f, margin, 0.f, 0.f, corner);
    const float quadW  = 0.5f + 2.f * margin;
    const float quadH  = 2.0f + 2.f * margin;
    EXPECT_NEAR(g.cornerU * quadW, corner, 1e-4f);
    EXPECT_NEAR(g.cornerV * quadH, corner, 1e-4f);
}

TEST(XRChrome, CornerClampedToMargin) {
    // cornerSize is clamped to the margin so a handle never eats into the content/BODY.
    const float margin = 0.02f;
    auto        g      = makeChromeGeometry(1.6f, 0.9f, margin, 0.f, 0.f, 0.10f /* > margin */);
    const float quadW  = 1.6f + 2.f * margin;
    EXPECT_NEAR(g.cornerU * quadW, margin, 1e-4f);
}

// ---- contentPoseToQuadCenter ----

TEST(XRChrome, PoseOffsetIdentityWhenSymmetric) {
    // No bar -> symmetric margins -> content center == quad center -> pose unchanged.
    auto        g     = makeChromeGeometry(1.6f, 0.9f, 0.04f, 0.f, 0.f, 0.f);
    const float quadW = 1.6f + 0.08f, quadH = 0.9f + 0.08f;
    SXRPose     in{Vec3{0.2f, 1.3f, -1.5f}, Quat{}};
    auto        out = contentPoseToQuadCenter(in, g, quadW, quadH);
    EXPECT_NEAR(out.pos.x, in.pos.x, 1e-5f);
    EXPECT_NEAR(out.pos.y, in.pos.y, 1e-5f);
    EXPECT_NEAR(out.pos.z, in.pos.z, 1e-5f);
}

TEST(XRChrome, PoseOffsetPushesQuadDownForBottomBar) {
    // Bottom-heavy margin (bar) -> quad center sits BELOW the content center. With identity
    // orientation, the quad center's y is lower than the content pose's y by half the extra margin.
    auto        g     = defaultGeom();
    const float quadW = 1.6f / g.contentFracW();
    const float quadH = 0.9f / g.contentFracH();
    SXRPose     in{Vec3{0.f, 1.5f, -1.5f}, Quat{}};
    auto        out = contentPoseToQuadCenter(in, g, quadW, quadH);
    EXPECT_NEAR(out.pos.x, 0.f, 1e-5f);
    EXPECT_LT(out.pos.y, in.pos.y); // quad center below content center
    // Magnitude: (0.5 - contentCenterV) * quadH.
    const float expectDrop = (0.5f - g.contentCenterV()) * quadH;
    EXPECT_NEAR(in.pos.y - out.pos.y, expectDrop, 1e-4f);
}

TEST(XRChrome, PoseOffsetRoundTripPlacesContentAtAnchor) {
    // The whole point: submitting the quad at contentPoseToQuadCenter(anchor) puts the CONTENT
    // center exactly back at the anchor. Reconstruct content center = quadCenter + R*localOffset.
    auto        g     = defaultGeom();
    const float quadW = 1.6f / g.contentFracW();
    const float quadH = 0.9f / g.contentFracH();
    SXRPose     anchor{Vec3{0.3f, 1.4f, -2.0f}, qFromYaw(0.5f)};
    auto        quad = contentPoseToQuadCenter(anchor, g, quadW, quadH);
    // content center within quad, quad-local:
    const float xLocal      = (g.contentCenterU() - 0.5f) * quadW;
    const float yLocal      = (0.5f - g.contentCenterV()) * quadH;
    const Vec3  contentBack = quad.pos + qRotate(quad.rot, Vec3{xLocal, yLocal, 0.f});
    EXPECT_NEAR(contentBack.x, anchor.pos.x, 1e-4f);
    EXPECT_NEAR(contentBack.y, anchor.pos.y, 1e-4f);
    EXPECT_NEAR(contentBack.z, anchor.pos.z, 1e-4f);
}
