#include <openxr/XRMath.hpp>

#include <gtest/gtest.h>

#include <cmath>

using namespace OpenXR;

// tests/xr/gaze_hit.cpp — hypxrvoice GAP 4 (docs/openxr/05-configuration.md §5, `hyprctl openxr
// gaze`): the gaze reply's optional `hitPoint` / `hitDistM`. Two pure pieces are covered here:
//
//   1. pickGazeHitT() — which of the (at most two) intersections the selection pass performed
//      belongs to the DWELL-STABLE candidate that the reply actually names. Getting this wrong
//      reports a point on the WRONG quad during a dwell transition, which is precisely when a
//      voice daemon resolving "put it where I was looking" reads the sample.
//   2. the ray -> point round trip: rayQuadIntersect's `t` turned back into a LOCAL_FLOOR point
//      must land ON the quad it hit (that point is what `openxr place <name> at x,y,z` consumes).
//
// Pure math: builds and runs with no OpenXR runtime.

namespace {
    void expectVecNear(const Vec3& a, const Vec3& b, float tol = 1e-4f) {
        EXPECT_NEAR(a.x, b.x, tol);
        EXPECT_NEAR(a.y, b.y, tol);
        EXPECT_NEAR(a.z, b.z, tol);
    }
}

// Steady state: the ray is on the selected monitor and the nearest hit IS the selection.
TEST(XRGazeHit, SteadyStateUsesTheNearestHit) {
    const auto p = pickGazeHitT(/*stable=*/3, /*rawId=*/3, /*rawValid=*/true, /*rawT=*/2.5f,
                                /*prevStable=*/3, /*prevValid=*/true, /*prevT=*/2.5f);
    EXPECT_TRUE(p.valid);
    EXPECT_NEAR(p.t, 2.5f, 1e-6f);
}

// Passthrough: nothing is selected, so there is no hit to report even if the ray grazed something.
TEST(XRGazeHit, NoSelectionNeverReportsAHit) {
    const auto p = pickGazeHitT(/*stable=*/-1, /*rawId=*/7, /*rawValid=*/true, /*rawT=*/1.f,
                                /*prevStable=*/-1, /*prevValid=*/false, /*prevT=*/0.f);
    EXPECT_FALSE(p.valid);
}

// Mid-dwell, ray has moved onto a NEW monitor: the reply still names the OLD one (the switch has
// not committed), so the point must be the old monitor's intersection, not the new one's.
TEST(XRGazeHit, MidDwellReportsTheStillSelectedMonitorsHit) {
    const auto p = pickGazeHitT(/*stable=*/1, /*rawId=*/2, /*rawValid=*/true, /*rawT=*/1.2f,
                                /*prevStable=*/1, /*prevValid=*/true, /*prevT=*/3.4f);
    EXPECT_TRUE(p.valid);
    EXPECT_NEAR(p.t, 3.4f, 1e-6f);
}

// Mid-dwell, ray has left every quad (looking away must persist dwellSec before the selection
// clears): still selected, but nothing was hit -> no point. The daemon falls back to projection.
TEST(XRGazeHit, MidDwellLookingAwayReportsNoHit) {
    const auto p = pickGazeHitT(/*stable=*/1, /*rawId=*/-1, /*rawValid=*/false, /*rawT=*/0.f,
                                /*prevStable=*/1, /*prevValid=*/false, /*prevT=*/0.f);
    EXPECT_FALSE(p.valid);
}

// Commit frame: stepGazeSelect just moved the selection to rawId. The nearest hit is the new
// selection's, and the (now stale) previous stable must not win.
TEST(XRGazeHit, CommitFrameUsesTheNewlySelectedMonitorsHit) {
    const auto p = pickGazeHitT(/*stable=*/2, /*rawId=*/2, /*rawValid=*/true, /*rawT=*/1.2f,
                                /*prevStable=*/1, /*prevValid=*/true, /*prevT=*/3.4f);
    EXPECT_TRUE(p.valid);
    EXPECT_NEAR(p.t, 1.2f, 1e-6f);
}

// An occluding nearer quad: the nearest hit is some other monitor while the sticky (hysteresis)
// selection is still the farther one — report the SELECTED monitor's own intersection.
TEST(XRGazeHit, StickySelectionBehindANearerQuad) {
    const auto p = pickGazeHitT(/*stable=*/5, /*rawId=*/9, /*rawValid=*/true, /*rawT=*/0.8f,
                                /*prevStable=*/5, /*prevValid=*/true, /*prevT=*/2.0f);
    EXPECT_TRUE(p.valid);
    EXPECT_NEAR(p.t, 2.0f, 1e-6f);
}

// The reported point is origin + dir * t, and it must lie ON the quad that was hit. Quad 2m ahead
// of a floor-standing head, ray aimed slightly right/up of its center.
TEST(XRGazeHit, RayParameterRoundTripsToAPointOnTheQuad) {
    const SXRPose quad{Vec3{0.f, 1.5f, -2.f}, Quat{}}; // faces +Z, i.e. back at the viewer
    const Vec3    origin{0.f, 1.5f, 0.f};

    // Aim at a point 0.3m right and 0.2m up on the quad's surface.
    const Vec3 target{0.3f, 1.7f, -2.f};
    const Vec3 dir = (target - origin).normalized();
    const auto hit = rayQuadIntersect(quad, origin, dir, /*w=*/1.6f, /*h=*/0.9f);

    ASSERT_TRUE(hit.hit);
    const Vec3 point = origin + dir * hit.t;
    expectVecNear(point, target, 1e-3f);
    // Distance is the ray parameter for a unit direction — the `hitDistM` the reply publishes.
    EXPECT_NEAR(hit.t, (target - origin).length(), 1e-3f);
    // And the point is coplanar with the quad.
    EXPECT_NEAR(point.z, quad.pos.z, 1e-3f);
}

// A miss produces no usable t: the pass must not publish a point for a ray that cleared the quad.
TEST(XRGazeHit, MissProducesNoHit) {
    const SXRPose quad{Vec3{0.f, 1.5f, -2.f}, Quat{}};
    const Vec3    origin{0.f, 1.5f, 0.f};
    const Vec3    dir = (Vec3{3.f, 1.5f, -2.f} - origin).normalized(); // way off to the right
    const auto    hit = rayQuadIntersect(quad, origin, dir, 1.6f, 0.9f);
    EXPECT_FALSE(hit.hit);

    const auto p = pickGazeHitT(/*stable=*/1, /*rawId=*/-1, /*rawValid=*/false, /*rawT=*/0.f,
                                /*prevStable=*/1, /*prevValid=*/hit.hit, /*prevT=*/hit.t);
    EXPECT_FALSE(p.valid);
}
