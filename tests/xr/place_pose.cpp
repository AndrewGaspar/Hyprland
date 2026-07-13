#include <openxr/XRMath.hpp>

#include <gtest/gtest.h>

#include <cmath>

using namespace OpenXR;

// tests/xr/place_pose.cpp — the pure pose math behind `xrmonitor place <name> at x,y,z` (hypxrvoice
// GAP 2). placeAtFacing() builds the LOCAL_FLOOR quad pose whose center sits at the resolved gaze
// point and whose +Z (the display's outward normal, matching centerPlacement's lookAtNoRoll(from=
// quad, to=head) convention) points at the headset. Runs with no OpenXR runtime (pure math).

namespace {
    void expectVecNear(const Vec3& a, const Vec3& b, float tol = 1e-5f) {
        EXPECT_NEAR(a.x, b.x, tol);
        EXPECT_NEAR(a.y, b.y, tol);
        EXPECT_NEAR(a.z, b.z, tol);
    }
}

// The quad center lands exactly at the supplied point, regardless of orientation.
TEST(PlacePose, CenterSitsAtPoint) {
    const Vec3    point{0.5f, 1.4f, -1.2f};
    const Vec3    head{0.f, 1.6f, 0.f};
    const SXRPose W = placeAtFacing(point, head, /*headValid=*/true, Quat{});
    expectVecNear(W.pos, point);
}

// Facing: the pose's +Z axis points from the quad toward the head (so the display faces the user).
TEST(PlacePose, FacesTheHeadset) {
    const Vec3    point{0.5f, 1.4f, -1.2f};
    const Vec3    head{0.f, 1.6f, 0.f};
    const SXRPose W       = placeAtFacing(point, head, true, Quat{});
    const Vec3    fwd     = qRotate(W.rot, Vec3{0.f, 0.f, 1.f});
    const Vec3    toHead  = (head - point).normalized();
    expectVecNear(fwd, toHead, 1e-4f);
    // No roll: the pose's right axis stays in the horizontal plane (y ~ 0).
    const Vec3 right = qRotate(W.rot, Vec3{1.f, 0.f, 0.f});
    EXPECT_NEAR(right.y, 0.f, 1e-4f);
}

// Head straight in front along -Z at the same height: the quad faces +Z toward -Z ... i.e. the pose
// forward equals the direction to the head, and a monitor at the origin looking at a head on +Z is
// yawed 180° from identity.
TEST(PlacePose, HeadOnPlusZYaws180) {
    const Vec3    point{0.f, 1.5f, 0.f};
    const Vec3    head{0.f, 1.5f, 2.f}; // head on +Z
    const SXRPose W   = placeAtFacing(point, head, true, Quat{});
    const Vec3    fwd = qRotate(W.rot, Vec3{0.f, 0.f, 1.f});
    expectVecNear(fwd, Vec3{0.f, 0.f, 1.f}, 1e-4f);
}

// No head tracking: keep the supplied fallback (the quad's current) orientation, move only position.
TEST(PlacePose, InvalidHeadKeepsFallbackRot) {
    const Vec3    point{1.f, 2.f, -3.f};
    const Quat    fallback = qFromYaw(0.7f);
    const SXRPose W        = placeAtFacing(point, Vec3{0.f, 0.f, 0.f}, /*headValid=*/false, fallback);
    expectVecNear(W.pos, point);
    EXPECT_NEAR(W.rot.x, fallback.x, 1e-6f);
    EXPECT_NEAR(W.rot.y, fallback.y, 1e-6f);
    EXPECT_NEAR(W.rot.z, fallback.z, 1e-6f);
    EXPECT_NEAR(W.rot.w, fallback.w, 1e-6f);
}

// Degenerate lookAt (head directly above the point): lookAtNoRoll bails to the fallback, so the
// place keeps the quad's current facing rather than producing a rolled/NaN pose.
TEST(PlacePose, HeadDirectlyAboveKeepsFallback) {
    const Vec3    point{0.f, 1.0f, -1.0f};
    const Vec3    head{0.f, 2.0f, -1.0f}; // straight up from point
    const Quat    fallback = qFromYaw(0.3f);
    const SXRPose W        = placeAtFacing(point, head, true, fallback);
    expectVecNear(W.pos, point);
    EXPECT_NEAR(W.rot.x, fallback.x, 1e-6f);
    EXPECT_NEAR(W.rot.y, fallback.y, 1e-6f);
    EXPECT_NEAR(W.rot.z, fallback.z, 1e-6f);
    EXPECT_NEAR(W.rot.w, fallback.w, 1e-6f);
}
