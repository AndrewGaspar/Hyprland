#include <openxr/XRMath.hpp>
#include <openxr/XRAnchor.hpp>

#include <gtest/gtest.h>

#include <cmath>

using namespace OpenXR;

// tests/xr/gaze_select.cpp — the pure gaze-grab + conditional-hand-input logic from
// docs/openxr/research/archive/16-gaze-grab.md (Part A gating + Part B gaze selection/carry).
// Pure math: builds and runs with no OpenXR runtime.

namespace {
    constexpr float PI = 3.14159265358979323846f;

    void expectVecNear(const Vec3& a, const Vec3& b, float tol = 1e-4f) {
        EXPECT_NEAR(a.x, b.x, tol);
        EXPECT_NEAR(a.y, b.y, tol);
        EXPECT_NEAR(a.z, b.z, tol);
    }

    SXRAnchorTuning defaultTuning() {
        SXRAnchorTuning t;
        t.leashResponse    = 0.35f;
        t.deadzoneAngleRad = 15.f * PI / 180.f;
        t.deadzoneDistance = 0.25f;
        t.defaultDistance  = 1.5f;
        return t;
    }

    SXRSolveInput viewInput(const SXRPose& view, float dt = 1.f / 90.f) {
        SXRSolveInput in;
        in.view = view;
        in.dt   = dt;
        in.pxW  = 1600;
        in.pxH  = 900;
        return in;
    }

    // A LOCAL anchor placed 1.5 m in front of the origin at eye height, facing the viewer.
    CXRAnchor localAnchorAt(const Vec3& pos) {
        SXRAnchorState st;
        st.mode              = XR_ANCHOR_LOCAL;
        st.widthMeters       = 1.6f;
        st.anchorPose.pos    = pos;
        st.anchorPose.rot    = Quat{}; // identity
        CXRAnchor a;
        a.initFromState(st);
        return a;
    }
}

// ================= Part B: gaze selection dwell/hysteresis state machine =====================

TEST(XRGazeSelect, DwellRequiredToAcquire) {
    SXRGazeSelect s;
    const float   dwell = 0.2f; // 200 ms
    const float   dt    = 1.f / 90.f;

    // Look at monitor 5; must dwell 200ms before it becomes the stable candidate.
    int64_t stable = -1;
    for (float t = 0.f; t < 0.2f - dt; t += dt)
        stable = stepGazeSelect(s, 5, dt, dwell);
    EXPECT_EQ(stable, -1) << "acquired before the dwell elapsed";

    // Keep looking: it commits once the accumulator crosses the dwell.
    for (int i = 0; i < 5; ++i)
        stable = stepGazeSelect(s, 5, dt, dwell);
    EXPECT_EQ(stable, 5);
}

TEST(XRGazeSelect, SaccadePastAMonitorNeverSelectsIt) {
    SXRGazeSelect s;
    const float   dwell = 0.2f;
    const float   dt    = 1.f / 90.f;

    // Rest on 1 long enough to select it.
    for (int i = 0; i < 30; ++i)
        stepGazeSelect(s, 1, dt, dwell);
    ASSERT_EQ(s.stable, 1);

    // A brief flick across monitor 2 (< dwell) then back to 1 must never select 2.
    int64_t stable = 1;
    for (int i = 0; i < 3; ++i) { // ~33 ms on 2
        stable = stepGazeSelect(s, 2, dt, dwell);
        EXPECT_EQ(stable, 1);
    }
    for (int i = 0; i < 3; ++i)
        stable = stepGazeSelect(s, 1, dt, dwell);
    EXPECT_EQ(stable, 1);
}

TEST(XRGazeSelect, SwitchRequiresSustainedDwellOnNewTarget) {
    SXRGazeSelect s;
    const float   dwell = 0.2f;
    const float   dt    = 1.f / 90.f;
    for (int i = 0; i < 30; ++i)
        stepGazeSelect(s, 1, dt, dwell);
    ASSERT_EQ(s.stable, 1);

    // Sustained look at 2 for > dwell switches the selection.
    int64_t stable = 1;
    for (int i = 0; i < 30; ++i)
        stable = stepGazeSelect(s, 2, dt, dwell);
    EXPECT_EQ(stable, 2);
}

TEST(XRGazeSelect, LookingAwayClearsAfterDwell) {
    SXRGazeSelect s;
    const float   dwell = 0.2f;
    const float   dt    = 1.f / 90.f;
    for (int i = 0; i < 30; ++i)
        stepGazeSelect(s, 7, dt, dwell);
    ASSERT_EQ(s.stable, 7);

    // Passthrough (-1) must persist for the dwell before the selection clears (hysteresis).
    int64_t stable = 7;
    for (int i = 0; i < 3; ++i) {
        stable = stepGazeSelect(s, -1, dt, dwell);
        EXPECT_EQ(stable, 7) << "cleared too eagerly on a brief look-away";
    }
    for (int i = 0; i < 30; ++i)
        stable = stepGazeSelect(s, -1, dt, dwell);
    EXPECT_EQ(stable, -1);
}

TEST(XRGazeSelect, ZeroDwellIsInstant) {
    SXRGazeSelect s;
    EXPECT_EQ(stepGazeSelect(s, 3, 1.f / 90.f, 0.f), 3);
    EXPECT_EQ(stepGazeSelect(s, 9, 1.f / 90.f, 0.f), 9);
    EXPECT_EQ(stepGazeSelect(s, -1, 1.f / 90.f, 0.f), -1);
}

// ================= Part B: gaze carry solve =================================================

TEST(XRGazeCarry, GrabPlacesQuadOnGazeRayAtGrabDistanceFacingViewer) {
    auto        tune = defaultTuning();
    // Monitor sits 1.5 m in front of the origin (−Z).
    CXRAnchor   a    = localAnchorAt(Vec3{0.f, 1.5f, -1.5f});
    const SXRPose view{{0.f, 1.5f, 0.f}, Quat{}}; // at origin, looking −Z

    // Seed one solve so lastWorld is valid.
    a.solve(viewInput(view), tune);
    ASSERT_TRUE(a.hasLastWorld());

    // Grab at the current distance (1.5 m). Follow mode.
    a.beginGazeGrab(view, /*follow=*/true);
    EXPECT_TRUE(a.gazeGrabbed());
    EXPECT_NEAR(a.gazeDist(), 1.5f, 1e-4f);

    // Solve while looking straight ahead: the quad is at view + (-Z)*1.5 = (0,1.5,-1.5).
    auto res = a.solve(viewInput(view), tune);
    EXPECT_EQ(res.space, XR_SPACE_LOCAL_FLOOR);
    expectVecNear(res.worldPose.pos, Vec3{0.f, 1.5f, -1.5f});

    // Now turn the head 90° to the right (look toward −X). Follow => the quad swings to stay
    // centred: view forward becomes (-1,0,0), so pos = (−1.5, 1.5, 0).
    const float yaw = PI / 2.f; // yaw about +Y; forward -Z rotates toward -X
    const Quat  qYaw = qFromYaw(yaw);
    const SXRPose viewTurned{{0.f, 1.5f, 0.f}, qYaw};
    auto res2 = a.solve(viewInput(viewTurned), tune);
    expectVecNear(res2.worldPose.pos, Vec3{-1.5f, 1.5f, 0.f}, 1e-3f);
}

TEST(XRGazeCarry, PushPullClampsToDistanceRange) {
    auto        tune = defaultTuning();
    CXRAnchor   a    = localAnchorAt(Vec3{0.f, 1.5f, -1.5f});
    const SXRPose view{{0.f, 1.5f, 0.f}, Quat{}};
    a.solve(viewInput(view), tune);
    a.beginGazeGrab(view, true);

    a.gazePushPull(1.0f); // 1.5 -> 2.5
    EXPECT_NEAR(a.gazeDist(), 2.5f, 1e-4f);
    a.gazePushPull(100.f); // clamps to XR_DISTANCE_MAX (5.0)
    EXPECT_NEAR(a.gazeDist(), XR_DISTANCE_MAX, 1e-4f);
    a.gazePushPull(-100.f); // clamps to XR_DISTANCE_MIN (0.3)
    EXPECT_NEAR(a.gazeDist(), XR_DISTANCE_MIN, 1e-4f);

    a.gazeSetDist(2.0f);
    EXPECT_NEAR(a.gazeDist(), 2.0f, 1e-4f);

    // The solved distance follows m_gazeDist.
    auto res = a.solve(viewInput(view), tune);
    EXPECT_NEAR((res.worldPose.pos - view.pos).length(), 2.0f, 1e-3f);
}

TEST(XRGazeCarry, FreezeModeKeepsQuadPutWhenLookingAway) {
    auto        tune = defaultTuning();
    CXRAnchor   a    = localAnchorAt(Vec3{0.f, 1.5f, -1.5f});
    const SXRPose view{{0.f, 1.5f, 0.f}, Quat{}};
    a.solve(viewInput(view), tune);
    a.beginGazeGrab(view, /*follow=*/false);

    // Look 90° away: freeze mode keeps the frozen origin+direction, so the quad stays in front.
    const SXRPose viewTurned{{0.f, 1.5f, 0.f}, qFromYaw(PI / 2.f)};
    auto res = a.solve(viewInput(viewTurned), tune);
    expectVecNear(res.worldPose.pos, Vec3{0.f, 1.5f, -1.5f}, 1e-3f);
}

TEST(XRGazeCarry, ReleaseReanchorsLocalWithoutMoving) {
    auto        tune = defaultTuning();
    CXRAnchor   a    = localAnchorAt(Vec3{0.f, 1.5f, -1.5f});
    const SXRPose view{{0.f, 1.5f, 0.f}, Quat{}};
    a.solve(viewInput(view), tune);
    a.beginGazeGrab(view, true);
    a.gazeSetDist(2.5f);
    // Carry it to a new spot.
    auto carried = a.solve(viewInput(view), tune);
    const Vec3 whereItSits = carried.worldPose.pos;

    // Release: LOCAL anchor should now sit exactly where it was let go.
    SXRSolveInput in = viewInput(view);
    a.endGazeGrab(in, tune);
    EXPECT_FALSE(a.gazeGrabbed());
    EXPECT_EQ(a.state().mode, XR_ANCHOR_LOCAL);
    expectVecNear(a.state().anchorPose.pos, whereItSits, 1e-3f);

    // A subsequent solve keeps it there (not carried anymore).
    auto after = a.solve(viewInput(view), tune);
    expectVecNear(after.worldPose.pos, whereItSits, 1e-3f);
}

TEST(XRGazeCarry, HandGrabOverrideWinsOverGaze) {
    auto        tune = defaultTuning();
    CXRAnchor   a    = localAnchorAt(Vec3{0.f, 1.5f, -1.5f});
    const SXRPose view{{0.f, 1.5f, 0.f}, Quat{}};
    a.solve(viewInput(view), tune);

    // Both a hand grab and a gaze grab set (should not happen via the manager, but the solve must
    // resolve deterministically: the hand-grab override returns first).
    SXRPose grip{{0.3f, 1.3f, -0.4f}, Quat{}};
    a.beginGrab(XR_HAND_RIGHT, grip);
    a.beginGazeGrab(view, true);

    SXRSolveInput in = viewInput(view);
    in.gripRight     = grip;
    auto res         = a.solve(in, tune);
    // Hand grab uses a grip space selector, not LOCAL_FLOOR — proves the hand override ran.
    EXPECT_EQ(res.space, XR_SPACE_GRIP_RIGHT);
}

// ================= poseForward sanity =======================================================

TEST(XRGazeCarry, PoseForwardIsMinusZ) {
    expectVecNear(poseForward(Quat{}), Vec3{0.f, 0.f, -1.f});
    // 90° yaw: forward points toward −X.
    expectVecNear(poseForward(qFromYaw(PI / 2.f)), Vec3{-1.f, 0.f, 0.f}, 1e-4f);
}

// ================= Part A: conditional hand-input gate truth table ===========================

TEST(XRHandGate, PolicyOnAlwaysActive) {
    for (bool away : {false, true})
        for (bool roam : {false, true})
            for (uint8_t force : {XR_HANDFORCE_NONE, XR_HANDFORCE_ON, XR_HANDFORCE_OFF})
                EXPECT_EQ(handInputGate(XR_HANDPOL_ON, force, away, roam, true), XR_HANDGATE_ACTIVE);
}

TEST(XRHandGate, PolicyOffAlwaysOff) {
    for (bool away : {false, true})
        for (bool roam : {false, true})
            for (uint8_t force : {XR_HANDFORCE_NONE, XR_HANDFORCE_ON, XR_HANDFORCE_OFF})
                EXPECT_EQ(handInputGate(XR_HANDPOL_OFF, force, away, roam, true), XR_HANDGATE_OFF);
}

TEST(XRHandGate, AutoGatesAtKeyboardEnablesAway) {
    // At the keyboard (not away, not roaming) -> gated by keyboard.
    EXPECT_EQ(handInputGate(XR_HANDPOL_AUTO, XR_HANDFORCE_NONE, false, false, true), XR_HANDGATE_KBD);
    // Away from the keyboard -> active.
    EXPECT_EQ(handInputGate(XR_HANDPOL_AUTO, XR_HANDFORCE_NONE, true, false, true), XR_HANDGATE_ACTIVE);
}

TEST(XRHandGate, AutoRoamOrTerm) {
    // Typing but roaming, with roam-enables ON -> active (wander-the-house override).
    EXPECT_EQ(handInputGate(XR_HANDPOL_AUTO, XR_HANDFORCE_NONE, false, true, true), XR_HANDGATE_ACTIVE);
    // Same but roam-enables OFF -> still gated by keyboard.
    EXPECT_EQ(handInputGate(XR_HANDPOL_AUTO, XR_HANDFORCE_NONE, false, true, false), XR_HANDGATE_KBD);
}

TEST(XRHandGate, AutoManualForceLatchWins) {
    // Manual force ON while typing -> active (the key-chord "let me use hands now").
    EXPECT_EQ(handInputGate(XR_HANDPOL_AUTO, XR_HANDFORCE_ON, false, false, true), XR_HANDGATE_ACTIVE);
    // Manual force OFF while away -> gated (manual) despite being away.
    EXPECT_EQ(handInputGate(XR_HANDPOL_AUTO, XR_HANDFORCE_OFF, true, true, true), XR_HANDGATE_MANUAL);
}
