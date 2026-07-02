#include <openxr/XRMath.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <limits>

using namespace OpenXR;

// tests/xr/ray_intersect.cpp — the ray -> quad intersection + UV mapping from
// docs/openxr/04-input.md §3. Pure math: builds and runs with no OpenXR runtime or headers.

namespace {
    constexpr float PI = 3.14159265358979323846f;

    // A 1.6 x 0.9 m quad, identity orientation, centered 1.5 m in front of the origin along -Z.
    SXRPose frontQuad() {
        return SXRPose{Vec3{0.f, 0.f, -1.5f}, Quat{}};
    }
}

TEST(XRRayIntersect, StraightOnCenter) {
    // Ray from origin pointing -Z hits the quad center: uv (0.5, 0.5), t == distance.
    auto hit = rayQuadIntersect(frontQuad(), Vec3{0.f, 0.f, 0.f}, Vec3{0.f, 0.f, -1.f}, 1.6f, 0.9f);
    ASSERT_TRUE(hit.hit);
    EXPECT_NEAR(hit.t, 1.5f, 1e-5f);
    EXPECT_NEAR(hit.u, 0.5f, 1e-5f);
    EXPECT_NEAR(hit.v, 0.5f, 1e-5f);
}

TEST(XRRayIntersect, UVRightIsPlusX) {
    // Aiming to the right of center (+X) must yield u > 0.5, v ~ 0.5.
    const Vec3 dir = Vec3{0.4f, 0.f, -1.5f}.normalized();
    auto       hit = rayQuadIntersect(frontQuad(), Vec3{0.f, 0.f, 0.f}, dir, 1.6f, 0.9f);
    ASSERT_TRUE(hit.hit);
    EXPECT_GT(hit.u, 0.5f);
    EXPECT_NEAR(hit.v, 0.5f, 1e-4f);
    // Exact: p.x = 0.4 at z=-1.5 -> u = 0.4/1.6 + 0.5 = 0.75.
    EXPECT_NEAR(hit.u, 0.75f, 1e-4f);
}

TEST(XRRayIntersect, UVUpIsSmallerV) {
    // Aiming up (+Y) must yield v < 0.5 (top of the surface), u ~ 0.5.
    const Vec3 dir = Vec3{0.f, 0.3f, -1.5f}.normalized();
    auto       hit = rayQuadIntersect(frontQuad(), Vec3{0.f, 0.f, 0.f}, dir, 1.6f, 0.9f);
    ASSERT_TRUE(hit.hit);
    EXPECT_LT(hit.v, 0.5f);
    EXPECT_NEAR(hit.u, 0.5f, 1e-4f);
    // Exact: p.y = 0.3 -> v = 0.5 - 0.3/0.9 = 0.16667.
    EXPECT_NEAR(hit.v, 0.5f - 0.3f / 0.9f, 1e-4f);
}

TEST(XRRayIntersect, CornersMapToUnitSquare) {
    // Bottom-right corner: p = (+0.8, -0.45) -> u = 1, v = 1.
    auto br = rayQuadIntersect(frontQuad(), Vec3{0.8f, -0.45f, 0.f}, Vec3{0.f, 0.f, -1.f}, 1.6f, 0.9f);
    ASSERT_TRUE(br.hit);
    EXPECT_NEAR(br.u, 1.f, 1e-5f);
    EXPECT_NEAR(br.v, 1.f, 1e-5f);
    // Top-left corner: p = (-0.8, +0.45) -> u = 0, v = 0.
    auto tl = rayQuadIntersect(frontQuad(), Vec3{-0.8f, 0.45f, 0.f}, Vec3{0.f, 0.f, -1.f}, 1.6f, 0.9f);
    ASSERT_TRUE(tl.hit);
    EXPECT_NEAR(tl.u, 0.f, 1e-5f);
    EXPECT_NEAR(tl.v, 0.f, 1e-5f);
}

TEST(XRRayIntersect, MissesOutsideBounds) {
    // Just past the right edge (x = 0.81 > w/2 = 0.8): miss.
    auto hit = rayQuadIntersect(frontQuad(), Vec3{0.81f, 0.f, 0.f}, Vec3{0.f, 0.f, -1.f}, 1.6f, 0.9f);
    EXPECT_FALSE(hit.hit);
}

TEST(XRRayIntersect, SlackExpandsBounds) {
    // The same off-edge ray hits once the half-extents are expanded (grab cone forgiveness).
    auto miss = rayQuadIntersect(frontQuad(), Vec3{0.85f, 0.f, 0.f}, Vec3{0.f, 0.f, -1.f}, 1.6f, 0.9f);
    EXPECT_FALSE(miss.hit);
    auto hit = rayQuadIntersect(frontQuad(), Vec3{0.85f, 0.f, 0.f}, Vec3{0.f, 0.f, -1.f}, 1.6f, 0.9f, 0.1f);
    EXPECT_TRUE(hit.hit);
}

TEST(XRRayIntersect, ParallelRayMisses) {
    // Ray traveling along +X in the quad's plane never intersects z = 0.
    auto hit = rayQuadIntersect(frontQuad(), Vec3{0.f, 0.f, 0.f}, Vec3{1.f, 0.f, 0.f}, 1.6f, 0.9f);
    EXPECT_FALSE(hit.hit);
}

TEST(XRRayIntersect, BehindRayMisses) {
    // Quad in front (-Z) but the ray points the other way (+Z): t would be negative -> miss.
    auto hit = rayQuadIntersect(frontQuad(), Vec3{0.f, 0.f, 0.f}, Vec3{0.f, 0.f, 1.f}, 1.6f, 0.9f);
    EXPECT_FALSE(hit.hit);
}

TEST(XRRayIntersect, RotatedQuadYaw90) {
    // Quad yawed 90° about +Y sits in the x = const plane, facing +X. Place it 1.5 m to the
    // right and aim +X at it: hits center.
    SXRPose q{Vec3{1.5f, 0.f, 0.f}, qFromYaw(PI / 2.f)};
    auto    hit = rayQuadIntersect(q, Vec3{0.f, 0.f, 0.f}, Vec3{1.f, 0.f, 0.f}, 1.6f, 0.9f);
    ASSERT_TRUE(hit.hit);
    EXPECT_NEAR(hit.t, 1.5f, 1e-4f);
    EXPECT_NEAR(hit.u, 0.5f, 1e-4f);
    EXPECT_NEAR(hit.v, 0.5f, 1e-4f);
}

TEST(XRRayIntersect, NearestTWinsAcrossQuads) {
    // Two coaxial quads at -1.0 and -2.0; the nearest (t = 1.0) must win occlusion.
    SXRPose near{Vec3{0.f, 0.f, -1.0f}, Quat{}};
    SXRPose far{Vec3{0.f, 0.f, -2.0f}, Quat{}};
    auto    hn = rayQuadIntersect(near, Vec3{0.f, 0.f, 0.f}, Vec3{0.f, 0.f, -1.f}, 1.6f, 0.9f);
    auto    hf = rayQuadIntersect(far, Vec3{0.f, 0.f, 0.f}, Vec3{0.f, 0.f, -1.f}, 1.6f, 0.9f);
    ASSERT_TRUE(hn.hit);
    ASSERT_TRUE(hf.hit);
    EXPECT_LT(hn.t, hf.t);
    EXPECT_NEAR(hn.t, 1.0f, 1e-5f);
}

TEST(XRRayIntersect, OffsetOriginHitsCenter) {
    // Origin not at (0,0,0): a ray from (0.5, 0.2, 0) aimed at the quad center still maps to
    // uv (0.5, 0.5).
    SXRPose     q      = frontQuad();
    const Vec3  origin = Vec3{0.5f, 0.2f, 0.f};
    const Vec3  target = q.pos; // center
    const Vec3  dir    = (target - origin).normalized();
    auto        hit    = rayQuadIntersect(q, origin, dir, 1.6f, 0.9f);
    ASSERT_TRUE(hit.hit);
    EXPECT_NEAR(hit.u, 0.5f, 1e-4f);
    EXPECT_NEAR(hit.v, 0.5f, 1e-4f);
}
