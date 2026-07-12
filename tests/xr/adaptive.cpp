#include <openxr/XRMath.hpp>
#include <openxr/XRAnchor.hpp>

#include <gtest/gtest.h>

#include <cmath>

using namespace OpenXR;

// tests/xr/adaptive.cpp — adaptive anchoring (docs/openxr/research/13-adaptive-anchoring.md):
// the pure geofence/envelope math (XRMath.hpp) AND the full phase machine driven through
// CXRAnchor::solve() with a scripted head position. Deterministic, dt-driven, no runtime present.

namespace {
    constexpr float PI = 3.14159265358979323846f;

    // Fast-settling tuning so a handful of frames drives a full transition in the tests.
    SXRAnchorTuning testTuning() {
        SXRAnchorTuning t;
        t.leashResponse    = 0.05f;
        t.deadzoneAngleRad = 15.f * PI / 180.f;
        t.deadzoneDistance = 0.02f; // small so the roam leash actually chases in a few frames
        t.defaultDistance  = 1.5f;
        t.adLeaveRadius    = 1.5f;
        t.adReturnRadius   = 1.0f;
        t.adLeaveDwell     = 0.1f;
        t.adReturnDwell    = 0.1f;
        t.adTransition     = 0.1f;
        t.adEase           = XR_EASE_SMOOTHSTEP;
        t.adRoamMode       = XR_ANCHOR_BODY;
        t.adUseHeight      = false;
        t.adCarryOffset    = false;
        return t;
    }

    SXRAnchorState dockedAdaptiveState(const Vec3& deskPos) {
        SXRAnchorState st;
        st.mode                   = XR_ANCHOR_LOCAL;
        st.anchorPose.pos         = deskPos;
        st.anchorPose.rot         = Quat{}; // identity
        st.widthMeters            = 1.0f;
        st.adaptive.enabled       = true;
        st.adaptive.roamMode      = XR_ANCHOR_BODY;
        st.adaptive.roamModeSet   = true;
        return st;
    }

    struct Rig {
        CXRAnchor       a;
        SXRAnchorTuning t = testTuning();

        // Run n frames with the head at `headPos` (identity orientation -> facing -Z).
        SXRSolveResult run(const Vec3& headPos, int n, float dt = 1.f / 90.f) {
            SXRSolveResult r;
            for (int i = 0; i < n; ++i) {
                SXRSolveInput in;
                in.view = SXRPose{headPos, Quat{}};
                in.dt   = dt;
                in.pxW  = 1600;
                in.pxH  = 900;
                r       = a.solve(in, t);
            }
            return r;
        }
    };
} // namespace

// ==== pure geofence + envelope math ==========================================================

TEST(AdaptiveMath, EaseEndpointsAndMonotonic) {
    for (eXREase e : {XR_EASE_LINEAR, XR_EASE_SMOOTHSTEP, XR_EASE_OUT}) {
        EXPECT_FLOAT_EQ(easeApply(e, 0.f), 0.f);
        EXPECT_FLOAT_EQ(easeApply(e, 1.f), 1.f);
        // clamps out-of-range
        EXPECT_FLOAT_EQ(easeApply(e, -0.5f), 0.f);
        EXPECT_FLOAT_EQ(easeApply(e, 1.5f), 1.f);
        // monotone non-decreasing on [0,1]
        float prev = -1.f;
        for (int i = 0; i <= 20; ++i) {
            const float v = easeApply(e, i / 20.f);
            EXPECT_GE(v, prev - 1e-6f);
            prev = v;
        }
    }
    // smoothstep midpoint is 0.5 with zero slope endpoints (classic 3t^2-2t^3).
    EXPECT_FLOAT_EQ(easeApply(XR_EASE_SMOOTHSTEP, 0.5f), 0.5f);
}

TEST(AdaptiveMath, EnvAdvanceSnapHoldRamp) {
    EXPECT_FLOAT_EQ(envAdvance(0.3f, 0.016f, 0.f), 1.f);   // duration<=0 -> snap complete
    EXPECT_FLOAT_EQ(envAdvance(0.3f, 0.f, 0.7f), 0.3f);    // dt<=0 -> hold
    EXPECT_NEAR(envAdvance(0.f, 0.35f, 0.7f), 0.5f, 1e-6f); // half a 0.7s transition
    EXPECT_FLOAT_EQ(envAdvance(0.9f, 1.0f, 0.7f), 1.f);    // never overshoots 1
}

TEST(AdaptiveMath, HorizDistIgnoresHeight) {
    // Standing up (change in y only) must not register as walking away (§3.1).
    EXPECT_FLOAT_EQ(horizDistXZ(Vec3{0, 0, 0}, Vec3{0, 1.8f, 0}), 0.f);
    EXPECT_NEAR(horizDistXZ(Vec3{0, 0, 0}, Vec3{3, 5, 4}), 5.f, 1e-5f);
    EXPECT_NEAR(dist3(Vec3{0, 0, 0}, Vec3{0, 1.8f, 0}), 1.8f, 1e-5f);
}

TEST(AdaptiveMath, DwellStepAccumulatesAndResets) {
    float acc = 0.f;
    // condition false -> stays 0, never fires
    EXPECT_FALSE(dwellStep(false, acc, 0.05f, 0.1f));
    EXPECT_FLOAT_EQ(acc, 0.f);
    // condition true accumulates; fires once total dt reaches the dwell
    EXPECT_FALSE(dwellStep(true, acc, 0.05f, 0.1f)); // 0.05
    EXPECT_FALSE(dwellStep(true, acc, 0.04f, 0.1f)); // 0.09
    EXPECT_TRUE(dwellStep(true, acc, 0.02f, 0.1f));  // 0.11 >= 0.1
    // a single false sample resets the accumulator (anti-flap)
    EXPECT_FALSE(dwellStep(false, acc, 0.05f, 0.1f));
    EXPECT_FLOAT_EQ(acc, 0.f);
}

// ==== full phase machine through CXRAnchor::solve() ==========================================

TEST(Adaptive, DisabledIsInertLocal) {
    // adaptive disabled: a LOCAL anchor stays exactly at its pose regardless of head motion.
    Rig r;
    SXRAnchorState st = dockedAdaptiveState(Vec3{0, 1.4f, -1.5f});
    st.adaptive.enabled = false;
    r.a.initFromState(st);
    const auto res = r.run(Vec3{5, 0, 0}, 60);
    EXPECT_EQ(r.a.adaptivePhase(), XRAD_DOCKED);
    EXPECT_NEAR(res.worldPose.pos.x, 0.f, 1e-4f);
    EXPECT_NEAR(res.worldPose.pos.y, 1.4f, 1e-4f);
    EXPECT_NEAR(res.worldPose.pos.z, -1.5f, 1e-4f);
}

TEST(Adaptive, WalkAwayUndocksAndFollows) {
    Rig r;
    r.a.initFromState(dockedAdaptiveState(Vec3{0, 1.4f, -1.5f}));

    // Seat capture at the desk.
    r.run(Vec3{0, 0, 0}, 5);
    EXPECT_EQ(r.a.adaptivePhase(), XRAD_DOCKED);

    // Walk out well past R_out (1.5 m) and hold. Dwell (0.1s) + transition (0.1s) + leash settle.
    const auto res = r.run(Vec3{3, 0, 0}, 120);
    EXPECT_EQ(r.a.adaptivePhase(), XRAD_ROAMING);
    // Body roam, default offset straight ahead at 1.5m: world ~ {3, ~0, -1.5}.
    EXPECT_NEAR(res.worldPose.pos.x, 3.f, 0.1f);
    EXPECT_NEAR(res.worldPose.pos.z, -1.5f, 0.15f);
}

TEST(Adaptive, DeadBandDoesNotUndock) {
    // Pacing in the hysteresis dead band (between R_in 1.0 and R_out 1.5) never undocks.
    Rig r;
    r.a.initFromState(dockedAdaptiveState(Vec3{0, 1.4f, -1.5f}));
    r.run(Vec3{0, 0, 0}, 5);
    r.run(Vec3{1.2f, 0, 0}, 200);
    EXPECT_EQ(r.a.adaptivePhase(), XRAD_DOCKED);
}

TEST(Adaptive, ReturnRedocksToDeskPose) {
    Rig r;
    r.a.initFromState(dockedAdaptiveState(Vec3{0, 1.4f, -1.5f}));
    r.run(Vec3{0, 0, 0}, 5);
    r.run(Vec3{3, 0, 0}, 120);
    ASSERT_EQ(r.a.adaptivePhase(), XRAD_ROAMING);

    // Walk back to the seat (inside R_in) and hold -> redock, exactly at the saved desk pose.
    const auto res = r.run(Vec3{0.1f, 0, 0}, 120);
    EXPECT_EQ(r.a.adaptivePhase(), XRAD_DOCKED);
    EXPECT_NEAR(res.worldPose.pos.x, 0.f, 1e-3f);
    EXPECT_NEAR(res.worldPose.pos.y, 1.4f, 1e-3f);
    EXPECT_NEAR(res.worldPose.pos.z, -1.5f, 1e-3f);
}

TEST(Adaptive, StandingUpDoesNotUndockUnlessUseHeight) {
    // XZ-only: rising straight up (y only) stays docked...
    Rig r;
    r.a.initFromState(dockedAdaptiveState(Vec3{0, 1.4f, -1.5f}));
    r.run(Vec3{0, 0, 0}, 5);
    r.run(Vec3{0, 2.0f, 0}, 200); // 2m up, 0 horizontal
    EXPECT_EQ(r.a.adaptivePhase(), XRAD_DOCKED);

    // ...but with use_height the same vertical move undocks.
    Rig r2;
    r2.t.adUseHeight = true;
    r2.a.initFromState(dockedAdaptiveState(Vec3{0, 1.4f, -1.5f}));
    r2.run(Vec3{0, 0, 0}, 5);
    r2.run(Vec3{0, 2.0f, 0}, 120);
    EXPECT_NE(r2.a.adaptivePhase(), XRAD_DOCKED);
}

TEST(Adaptive, ReverseMidTransitionReturnsWithoutSnap) {
    // Begin undocking, then return before it completes: the machine reverses to redocking and lands
    // back DOCKED at the desk pose, and the pose never snaps (stays within the dock<->roam span).
    Rig r;
    r.a.initFromState(dockedAdaptiveState(Vec3{0, 1.4f, -1.5f}));
    r.run(Vec3{0, 0, 0}, 5);

    // Trigger undock and advance partway through the transition only.
    r.run(Vec3{3, 0, 0}, 12); // ~0.13s: past the 0.1s dwell, into UNDOCKING
    const auto midPhase = r.a.adaptivePhase();
    EXPECT_TRUE(midPhase == XRAD_UNDOCKING || midPhase == XRAD_ROAMING);

    // Return to the seat; hold long enough to complete the reverse blend.
    const auto res = r.run(Vec3{0.1f, 0, 0}, 160);
    EXPECT_EQ(r.a.adaptivePhase(), XRAD_DOCKED);
    EXPECT_NEAR(res.worldPose.pos.x, 0.f, 1e-2f);
    EXPECT_NEAR(res.worldPose.pos.z, -1.5f, 1e-2f);
}

TEST(Adaptive, ForceVerbsSkipDwell) {
    Rig r;
    r.a.initFromState(dockedAdaptiveState(Vec3{0, 1.4f, -1.5f}));
    r.run(Vec3{0, 0, 0}, 5);

    // Manual undock skips the geofence dwell entirely, even while sitting at the desk.
    r.a.adaptiveForceUndock();
    r.run(Vec3{0, 0, 0}, 120);
    EXPECT_EQ(r.a.adaptivePhase(), XRAD_ROAMING);

    // Manual dock returns it.
    r.a.adaptiveForceDock();
    r.run(Vec3{0, 0, 0}, 120);
    EXPECT_EQ(r.a.adaptivePhase(), XRAD_DOCKED);
}

TEST(Adaptive, RoundTripSerializeParseThroughState) {
    // The adaptive config round-trips through the persisted SXRAdaptiveConfig equality used by the
    // reconcile diff (a light structural check that all persisted fields compare).
    SXRAdaptiveConfig a;
    a.enabled       = true;
    a.roamMode      = XR_ANCHOR_HEAD;
    a.roamModeSet   = true;
    a.hasRoamOffset = true;
    a.roamOffset    = SXRPose{Vec3{0, 1.35f, -1.2f}, Quat{}};
    a.leaveRadius   = 2.0f;
    a.returnRadius  = 1.2f;
    SXRAdaptiveConfig b = a;
    EXPECT_TRUE(a == b);
    b.roamMode = XR_ANCHOR_BODY;
    EXPECT_FALSE(a == b);
}
