#include <openxr/XRMath.hpp>
#include <openxr/XRAnchor.hpp>

#include <gtest/gtest.h>

#include <cmath>

using namespace OpenXR;

// tests/xr/anchor_math.cpp — the 12 named cases from docs/openxr/03-anchoring.md §9. Compiles
// and runs with no OpenXR runtime or headers present (pure math).

namespace {
    constexpr float PI = 3.14159265358979323846f;

    Quat            normQ(float x, float y, float z, float w) {
        return qNormalize(Quat{x, y, z, w});
    }

    void expectVecNear(const Vec3& a, const Vec3& b, float tol = 1e-5f) {
        EXPECT_NEAR(a.x, b.x, tol);
        EXPECT_NEAR(a.y, b.y, tol);
        EXPECT_NEAR(a.z, b.z, tol);
    }

    SXRAnchorTuning defaultTuning() {
        SXRAnchorTuning t;
        t.leashResponse    = 0.35f;
        t.deadzoneAngleRad = 15.f * PI / 180.f;
        t.deadzoneDistance = 0.25f;
        t.bodyFollowHeight = false;
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
}

// 1 --------------------------------------------------------------------------------------------
TEST(XRAnchorMath, MathComposeInverseIdentity) {
    const SXRPose poses[] = {
        {{1.f, 2.f, -3.f}, normQ(0.1f, 0.5f, -0.2f, 0.8f)},
        {{-0.4f, 1.1f, 0.7f}, normQ(0.3f, -0.1f, 0.6f, 0.7f)},
        {{5.f, -2.f, 4.f}, normQ(-0.2f, 0.2f, 0.9f, 0.3f)},
    };
    for (const auto& P : poses) {
        const SXRPose id = poseCompose(poseInverse(P), P);
        expectVecNear(id.pos, Vec3{0, 0, 0}, 1e-4f);
        // qAngleBetween of a near-identity quat carries float acos noise; 2e-3 rad is ~0.1deg.
        EXPECT_NEAR(qAngleBetween(id.rot, Quat{}), 0.f, 2e-3f);
    }
    // qRotate(qMul(a,b), v) == qRotate(a, qRotate(b, v))
    const Quat a = normQ(0.2f, 0.3f, -0.1f, 0.9f);
    const Quat b = normQ(-0.5f, 0.1f, 0.2f, 0.8f);
    const Vec3 v{0.7f, -1.2f, 0.4f};
    expectVecNear(qRotate(qMul(a, b), v), qRotate(a, qRotate(b, v)), 1e-5f);
}

// 2 --------------------------------------------------------------------------------------------
TEST(XRAnchorMath, MathLookAtNoRoll) {
    const Vec3 froms[] = {{0, 0, 0}, {1, 1, 1}, {-2, 0.5f, 3}};
    const Vec3 tos[]   = {{0, 0, -1}, {2, 1, -1}, {-2, 0.5f, -1}};
    for (int i = 0; i < 3; ++i) {
        const Quat q = lookAtNoRoll(froms[i], tos[i], Quat{});
        // +Z points at `to`
        const Vec3 zdir = qRotate(q, Vec3{0, 0, 1});
        expectVecNear(zdir, (tos[i] - froms[i]).normalized(), 1e-4f);
        // +X is horizontal (roll removed)
        const Vec3 xdir = qRotate(q, Vec3{1, 0, 0});
        EXPECT_NEAR(xdir.y, 0.f, 1e-4f);
    }
    // near-vertical: returns fallback
    const Quat fb = normQ(0.1f, 0.2f, 0.3f, 0.9f);
    const Quat r  = lookAtNoRoll(Vec3{0, 0, 0}, Vec3{0, 1, 0}, fb);
    EXPECT_NEAR(qAngleBetween(r, fb), 0.f, 2e-3f); // float acos noise on a self-comparison
}

// 3 --------------------------------------------------------------------------------------------
TEST(XRAnchorMath, YawExtraction) {
    for (float deg : {0.f, 45.f, -45.f, 90.f, -90.f, 179.f}) {
        const float th = deg * PI / 180.f;
        EXPECT_NEAR(qYawOf(qFromYaw(th), 0.f), th, 1e-4f);
        // composed yaw∘pitch still extracts the yaw
        const Quat rot = qMul(qFromYaw(th), qFromPitch(20.f * PI / 180.f));
        EXPECT_NEAR(qYawOf(rot, 0.f), th, 1e-4f);
    }
    // §7 serialization round-trip recovers yaw and pitch
    const float yawIn = 30.f * PI / 180.f, pitchIn = -12.f * PI / 180.f;
    const Quat  rot = qMul(qFromYaw(yawIn), qFromPitch(pitchIn));
    const Vec3  f   = qRotate(rot, Vec3{0, 0, -1});
    EXPECT_NEAR(std::asin(std::clamp(f.y, -1.f, 1.f)), pitchIn, 1e-4f);
    EXPECT_NEAR(std::atan2(-f.x, -f.z), yawIn, 1e-4f);
}

// 4 --------------------------------------------------------------------------------------------
TEST(XRAnchorMath, SpringConvergesNoOvershoot) {
    const auto     tune = defaultTuning();
    CXRAnchor      a;
    SXRAnchorState st;
    st.mode           = XR_ANCHOR_HEAD;
    st.anchorPose.pos = Vec3{0, 0, -1}; // 1 m in front in view space
    a.initFromState(st);

    // seed at view A (spring settles onto the target, velocity 0)
    const SXRPose viewA{{0, 1.5f, 0}, Quat{}};
    a.solve(viewInput(viewA), tune);

    // new target 1 m away along +X -> forces chasing
    const SXRPose viewB{{1.f, 1.5f, 0}, Quat{}};
    const Vec3    Tb   = viewB.pos + st.anchorPose.pos; // identity rot
    const float   err0 = (a.lastWorld().pos - Tb).length();

    // Critically-damped from rest: strictly monotone decay, no overshoot. The spring re-latches
    // (freezes) once within XR_LEASH_SETTLE_POS of the target, so the residual settles at that
    // threshold — NOTE: doc 03 §9.4's <0.001 target is unreachable given the 0.01 re-latch; we
    // assert convergence to the settle threshold, which is the physically meaningful outcome.
    float prev    = err0;
    float t       = 0.f;
    bool  settled = false;
    for (int i = 0; i < 400; ++i) {
        const auto  res = a.solve(viewInput(viewB), tune);
        const Vec3  d   = res.worldPose.pos - Tb;
        const float err = d.length();
        EXPECT_LE(err, prev + 1e-6f); // monotone non-increasing
        EXPECT_LE(d.x, 1e-5f);        // started negative (Ta.x - Tb.x = -1), never crosses 0
        prev = err;
        t += 1.f / 90.f;
        if (err <= XR_LEASH_SETTLE_POS) {
            settled = true;
            break;
        }
    }
    EXPECT_TRUE(settled);
    EXPECT_LE(prev, XR_LEASH_SETTLE_POS + 1e-4f);
    EXPECT_LT(t, 5.f * tune.leashResponse);
}

// 5 --------------------------------------------------------------------------------------------
TEST(XRAnchorMath, SpringStableLargeDt) {
    const auto     tune = defaultTuning();
    CXRAnchor      a;
    SXRAnchorState st;
    st.mode           = XR_ANCHOR_HEAD;
    st.anchorPose.pos = Vec3{0, 0, -1};
    a.initFromState(st);

    a.solve(viewInput({{0, 1.5f, 0}, Quat{}}), tune); // seed
    const SXRPose viewB{{1.f, 1.5f, 0}, Quat{}};
    const Vec3    Tb = viewB.pos + st.anchorPose.pos;

    const float   before = (a.lastWorld().pos - Tb).length();
    const auto    res    = a.solve(viewInput(viewB, 0.1f), tune); // one big step
    const Vec3    d      = res.worldPose.pos - Tb;
    EXPECT_LT(d.length(), before); // moved closer
    EXPECT_LE(d.x, 1e-5f);         // no overshoot past the target (exact integrator)
}

// 6 --------------------------------------------------------------------------------------------
TEST(XRAnchorMath, DeadzoneHoldRelease) {
    const auto     tune = defaultTuning();
    CXRAnchor      a;
    SXRAnchorState st;
    st.mode           = XR_ANCHOR_HEAD;
    st.anchorPose.pos = Vec3{0, 0, -1};
    a.initFromState(st);

    const SXRPose view0{{0, 1.5f, 0}, Quat{}};
    a.solve(viewInput(view0), tune);
    const Vec3 held = a.lastWorld().pos;

    // tiny head move within the deadzone -> position frozen
    const SXRPose viewTiny{{0.05f, 1.5f, 0}, Quat{}};
    const auto    resTiny = a.solve(viewInput(viewTiny), tune);
    expectVecNear(resTiny.worldPose.pos, held, 1e-6f);

    // exceed the deadzone -> chase to convergence
    const SXRPose viewFar{{1.0f, 1.5f, 0}, Quat{}};
    const Vec3    Tfar = viewFar.pos + st.anchorPose.pos;
    for (int i = 0; i < 400; ++i) {
        a.solve(viewInput(viewFar), tune);
        if ((a.lastWorld().pos - Tfar).length() < 0.005f)
            break;
    }
    EXPECT_LT((a.lastWorld().pos - Tfar).length(), 0.01f);

    // re-latched: another tiny move produces zero motion
    const Vec3    settled = a.lastWorld().pos;
    const SXRPose viewFar2{{1.03f, 1.5f, 0}, Quat{}};
    const auto    resHold = a.solve(viewInput(viewFar2), tune);
    expectVecNear(resHold.worldPose.pos, settled, 1e-6f);
}

// 7 --------------------------------------------------------------------------------------------
TEST(XRAnchorMath, BodyYawNearVerticalHysteresis) {
    const auto     tune = defaultTuning();
    CXRAnchor      a;
    SXRAnchorState st;
    st.mode           = XR_ANCHOR_BODY;
    st.anchorPose.pos = Vec3{0, 1.4f, -1.2f};
    a.initFromState(st);

    // level gaze, yaw 0 -> body yaw 0
    a.solve(viewInput({{0, 1.6f, 0}, Quat{}}), tune);

    // look near-straight-down (horiz < HOLD) while yawing 90deg: yaw must HOLD at 0
    const Quat    rotDownYawed = qMul(qFromYaw(90.f * PI / 180.f), qFromPitch(-85.f * PI / 180.f));
    const SXRPose viewDown{{0, 1.6f, 0}, rotDownYawed};
    for (int i = 0; i < 50; ++i)
        a.solve(viewInput(viewDown), tune);
    EXPECT_NEAR(qYawOf(a.lastWorld().rot, 0.f), 0.f, 1e-2f); // held, not 90

    // pure head pitch never disturbs the target position (deadzone independence)
    const Vec3 heldPos = a.lastWorld().pos;
    expectVecNear(a.solve(viewInput(viewDown), tune).worldPose.pos, heldPos, 1e-5f);

    // return to level with yaw 90 (horiz > RESUME) -> yaw resumes toward 90
    const SXRPose viewLevelYawed{{0, 1.6f, 0}, qFromYaw(90.f * PI / 180.f)};
    for (int i = 0; i < 400; ++i)
        a.solve(viewInput(viewLevelYawed), tune);
    EXPECT_NEAR(qYawOf(a.lastWorld().rot, 0.f), 90.f * PI / 180.f, 1e-2f);
}

// 8 --------------------------------------------------------------------------------------------
TEST(XRAnchorMath, GrabRoundTripIdentity) {
    const auto    tune = defaultTuning();
    const SXRPose view{{0, 1.5f, 0}, qFromYaw(20.f * PI / 180.f)};
    const SXRPose gripL{{-0.2f, 1.1f, -0.4f}, qFromYaw(10.f * PI / 180.f)};
    const SXRPose gripR{{0.3f, 1.0f, -0.5f}, qFromPitch(15.f * PI / 180.f)};

    struct Case {
        eXRAnchorMode mode;
        eXRHand       device;
        eXRHand       grabHand;
    };
    const Case cases[] = {
        {XR_ANCHOR_LOCAL, XR_HAND_LEFT, XR_HAND_LEFT},   {XR_ANCHOR_HEAD, XR_HAND_LEFT, XR_HAND_LEFT},
        {XR_ANCHOR_BODY, XR_HAND_LEFT, XR_HAND_LEFT},    {XR_ANCHOR_DEVICE, XR_HAND_LEFT, XR_HAND_LEFT}, // same-hand anchor
        {XR_ANCHOR_DEVICE, XR_HAND_RIGHT, XR_HAND_LEFT},                                                 // opposite-hand anchor
    };

    for (const auto& c : cases) {
        CXRAnchor      a;
        SXRAnchorState st;
        st.mode           = c.mode;
        st.device         = c.device;
        st.anchorPose.pos = c.mode == XR_ANCHOR_LOCAL ? Vec3{0.5f, 1.4f, -1.2f} : Vec3{0.1f, 0.0f, -0.6f};
        a.initFromState(st);

        SXRSolveInput in = viewInput(view);
        in.gripLeft      = gripL;
        in.gripRight     = gripR;

        const SXRPose w0 = a.solve(in, tune).worldPose; // establish m_lastWorld

        a.beginGrab(c.grabHand, c.grabHand == XR_HAND_LEFT ? gripL : gripR);
        a.endGrab(in, tune);
        const SXRPose w1 = a.solve(in, tune).worldPose;

        expectVecNear(w1.pos, w0.pos, 1e-4f);
        EXPECT_NEAR(qAngleBetween(w1.rot, w0.rot), 0.f, 1e-3f);

        if (c.mode == XR_ANCHOR_HEAD) {
            // the spring must not kick: next frame stays put
            const SXRPose w2 = a.solve(in, tune).worldPose;
            expectVecNear(w2.pos, w1.pos, 1e-5f);
        }
    }
}

// 9 --------------------------------------------------------------------------------------------
TEST(XRAnchorMath, GrabOffsetFollows) {
    const auto     tune = defaultTuning();
    const SXRPose  view{{0, 1.5f, 0}, Quat{}};

    CXRAnchor      a;
    SXRAnchorState st;
    st.mode           = XR_ANCHOR_LOCAL;
    st.anchorPose.pos = Vec3{0.4f, 1.3f, -1.0f};
    a.initFromState(st);

    SXRSolveInput in = viewInput(view);
    const SXRPose oldGrip{{-0.2f, 1.1f, -0.4f}, qFromYaw(10.f * PI / 180.f)};
    in.gripLeft = oldGrip;

    const SXRPose oldWorld = a.solve(in, tune).worldPose;
    a.beginGrab(XR_HAND_LEFT, oldGrip);

    // translate + rotate the grip by a known delta
    const SXRPose newGrip{{0.3f, 1.4f, -0.7f}, qMul(qFromYaw(35.f * PI / 180.f), qFromPitch(20.f * PI / 180.f))};
    in.gripLeft     = newGrip;
    const SXRPose w = a.solve(in, tune).worldPose;

    const SXRPose expected = poseCompose(newGrip, poseCompose(poseInverse(oldGrip), oldWorld));
    expectVecNear(w.pos, expected.pos, 1e-4f);
    EXPECT_NEAR(qAngleBetween(w.rot, expected.rot), 0.f, 1e-4f);
}

// 9b ------------------------------------------------------------------------------------------
// §4.2 amendment: head/body-anchored quads re-evaluate orientation continuously while carried
// (position rigid to the grip; orientation faces the viewer regardless of wrist rotation).
TEST(XRAnchorMath, GrabContinuousFacing) {
    const auto    tune = defaultTuning();
    const SXRPose view{{0, 1.6f, 0}, Quat{}};

    for (const auto mode : {XR_ANCHOR_HEAD, XR_ANCHOR_BODY}) {
        CXRAnchor      a;
        SXRAnchorState st;
        st.mode           = mode;
        st.anchorPose.pos = Vec3{0, 0, -1.2f};
        a.initFromState(st);

        SXRSolveInput in = viewInput(view);
        in.dt            = 0.016f;
        const SXRPose grip0{{0.1f, 1.2f, -0.5f}, qFromYaw(0.f)};
        in.gripLeft = grip0;
        a.solve(in, tune); // one settled frame so lastWorld exists
        a.beginGrab(XR_HAND_LEFT, grip0);

        // carry the quad off to the left with an arbitrary wrist yaw+pitch
        const SXRPose grip1{{-0.9f, 1.3f, -0.6f}, qMul(qFromYaw(80.f * PI / 180.f), qFromPitch(-30.f * PI / 180.f))};
        in.gripLeft     = grip1;
        const SXRPose w = a.solve(in, tune).worldPose;

        Quat expectFace = lookAtNoRoll(w.pos, view.pos, Quat{});
        if (mode == XR_ANCHOR_BODY)
            expectFace = qFromYaw(qYawOf(expectFace, 0.f)); // body stays upright: yaw-only facing
        EXPECT_NEAR(qAngleBetween(w.rot, expectFace), 0.f, 1e-3f);
    }
}

// 10 -------------------------------------------------------------------------------------------
TEST(XRAnchorMath, VerbMath) {
    const auto tune = defaultTuning();

    // applyMove LOCAL: exact view-relative displacement qRotate(view.rot, (dx,dy,-dz))
    {
        const SXRPose  view{{0, 1.5f, 0}, qFromYaw(30.f * PI / 180.f)};
        CXRAnchor      a;
        SXRAnchorState st;
        st.mode           = XR_ANCHOR_LOCAL;
        st.anchorPose.pos = Vec3{0.2f, 1.3f, -1.1f};
        a.initFromState(st);
        const SXRPose  w0 = a.solve(viewInput(view), tune).worldPose;
        SXRVerbContext ctx;
        ctx.view      = view;
        ctx.viewValid = true;
        ASSERT_TRUE(a.applyMove(Vec3{0.1f, 0.2f, 0.3f}, ctx));
        const SXRPose w1 = a.solve(viewInput(view), tune).worldPose;
        expectVecNear(w1.pos - w0.pos, qRotate(view.rot, Vec3{0.1f, 0.2f, -0.3f}), 1e-5f);
    }

    // applyDistance clamps to [0.3, 5.0]
    {
        const SXRPose  view{{0, 1.5f, 0}, Quat{}};
        SXRVerbContext ctx;
        ctx.view      = view;
        ctx.viewValid = true;

        CXRAnchor      a;
        SXRAnchorState st;
        st.mode           = XR_ANCHOR_LOCAL;
        st.anchorPose.pos = Vec3{0, 1.5f, -1.0f}; // 1 m in front
        a.initFromState(st);
        a.solve(viewInput(view), tune);
        ASSERT_TRUE(a.applyDistance(+100.f, ctx)); // clamp to 5.0
        EXPECT_NEAR((a.solve(viewInput(view), tune).worldPose.pos - view.pos).length(), 5.0f, 1e-4f);
        a.solve(viewInput(view), tune);
        ASSERT_TRUE(a.applyDistance(-100.f, ctx)); // clamp to 0.3
        EXPECT_NEAR((a.solve(viewInput(view), tune).worldPose.pos - view.pos).length(), 0.3f, 1e-4f);
    }

    // applyScale: factor + delta, clamped to [0.2, 4.0]
    {
        CXRAnchor      a;
        SXRAnchorState st;
        st.widthMeters = 1.0f;
        a.initFromState(st);
        a.applyScale(false, 2.0f); // factor
        EXPECT_FLOAT_EQ(a.state().widthMeters, 2.0f);
        a.applyScale(true, 0.5f); // +delta
        EXPECT_FLOAT_EQ(a.state().widthMeters, 2.5f);
        a.applyScale(false, 100.f);
        EXPECT_FLOAT_EQ(a.state().widthMeters, 4.0f); // clamp hi
        a.applyScale(true, -100.f);
        EXPECT_FLOAT_EQ(a.state().widthMeters, 0.2f); // clamp lo
    }

    // applyCenter: places at defaultDistance along view forward, +Z faces the head
    {
        const SXRPose  view{{0, 1.5f, 0}, qFromYaw(50.f * PI / 180.f)};
        SXRVerbContext ctx;
        ctx.view      = view;
        ctx.viewValid = true;
        CXRAnchor      a;
        SXRAnchorState st;
        st.mode = XR_ANCHOR_LOCAL;
        a.initFromState(st);
        ASSERT_TRUE(a.applyCenter(ctx, tune.defaultDistance));
        const SXRPose w   = a.solve(viewInput(view), tune).worldPose;
        const Vec3    fwd = qRotate(view.rot, Vec3{0, 0, -1});
        expectVecNear(w.pos, view.pos + fwd * tune.defaultDistance, 1e-4f);
        expectVecNear(qRotate(w.rot, Vec3{0, 0, 1}), (view.pos - w.pos).normalized(), 1e-4f);
    }

    // applyRotate: LOCAL in place (pos unchanged); HEAD orbits (distance to head unchanged)
    {
        const SXRPose  view{{0, 1.5f, 0}, Quat{}};
        SXRVerbContext ctx;
        ctx.view      = view;
        ctx.viewValid = true;

        CXRAnchor      loc;
        SXRAnchorState ls;
        ls.mode           = XR_ANCHOR_LOCAL;
        ls.anchorPose.pos = Vec3{0.3f, 1.4f, -1.2f};
        loc.initFromState(ls);
        const Vec3 before = loc.state().anchorPose.pos;
        loc.applyRotate(25.f * PI / 180.f, 10.f * PI / 180.f, ctx);
        expectVecNear(loc.state().anchorPose.pos, before, 1e-6f);           // in place
        EXPECT_GT(qAngleBetween(loc.state().anchorPose.rot, Quat{}), 0.1f); // rotated

        CXRAnchor      head;
        SXRAnchorState hs;
        hs.mode           = XR_ANCHOR_HEAD;
        hs.anchorPose.pos = Vec3{0, 0, -1.2f};
        head.initFromState(hs);
        const float distBefore = head.state().anchorPose.pos.length();
        head.applyRotate(40.f * PI / 180.f, 0.f, ctx);
        EXPECT_NEAR(head.state().anchorPose.pos.length(), distBefore, 1e-5f);        // orbit: dist kept
        EXPECT_GT((head.state().anchorPose.pos - Vec3{0, 0, -1.2f}).length(), 0.1f); // moved
    }
}

// 11 -------------------------------------------------------------------------------------------
TEST(XRAnchorMath, DeviceTrackingLoss) {
    const auto     tune = defaultTuning();
    const SXRPose  view{{0, 1.5f, 0}, Quat{}};
    const SXRPose  grip{{0.2f, 1.1f, -0.3f}, qFromYaw(15.f * PI / 180.f)};

    CXRAnchor      a;
    SXRAnchorState st;
    st.mode           = XR_ANCHOR_DEVICE;
    st.device         = XR_HAND_LEFT;
    st.anchorPose.pos = Vec3{0, 0.05f, -0.1f};
    a.initFromState(st);

    SXRSolveInput in = viewInput(view);
    in.gripLeft      = grip;
    const auto r0    = a.solve(in, tune);
    EXPECT_EQ(r0.space, XR_SPACE_GRIP_LEFT);
    const SXRPose expectedWorld = poseCompose(grip, st.anchorPose);
    expectVecNear(r0.worldPose.pos, expectedWorld.pos, 1e-5f);

    // tracking lost
    in.gripLeft   = std::nullopt;
    const auto r1 = a.solve(in, tune);
    EXPECT_EQ(r1.space, XR_SPACE_LOCAL_FLOOR);
    expectVecNear(r1.pose.pos, expectedWorld.pos, 1e-5f); // parked at last composed world

    // tracking restored
    in.gripLeft   = grip;
    const auto r2 = a.solve(in, tune);
    EXPECT_EQ(r2.space, XR_SPACE_GRIP_LEFT);
    expectVecNear(r2.pose.pos, st.anchorPose.pos, 1e-5f); // stored offset again
}

// 12 -------------------------------------------------------------------------------------------
TEST(XRAnchorMath, ReferenceSpaceChange) {
    const auto    tune = defaultTuning();
    const SXRPose M{{0.5f, 0.1f, -0.3f}, qFromYaw(20.f * PI / 180.f)}; // new origin in old space

    // LOCAL: world pose re-expressed so that M ∘ newPose == oldPose
    {
        CXRAnchor      a;
        SXRAnchorState st;
        st.mode           = XR_ANCHOR_LOCAL;
        st.anchorPose.pos = Vec3{1.0f, 1.4f, -1.5f};
        st.anchorPose.rot = qFromYaw(35.f * PI / 180.f);
        a.initFromState(st);
        const SXRPose oldWorld = a.solve(viewInput({{0, 1.5f, 0}, Quat{}}), tune).worldPose;

        a.onReferenceSpaceChanged(M);
        const SXRPose newWorld   = a.solve(viewInput({{0, 1.5f, 0}, Quat{}}), tune).worldPose;
        const SXRPose recomposed = poseCompose(M, newWorld);
        expectVecNear(recomposed.pos, oldWorld.pos, 1e-4f);
        EXPECT_NEAR(qAngleBetween(recomposed.rot, oldWorld.rot), 0.f, 1e-4f);
    }

    // HEAD: stored offset unaffected
    {
        CXRAnchor      a;
        SXRAnchorState st;
        st.mode           = XR_ANCHOR_HEAD;
        st.anchorPose.pos = Vec3{0.1f, 0, -1.0f};
        a.initFromState(st);
        a.solve(viewInput({{0, 1.5f, 0}, Quat{}}), tune);
        const Vec3 before = a.state().anchorPose.pos;
        a.onReferenceSpaceChanged(M);
        expectVecNear(a.state().anchorPose.pos, before, 1e-6f);
    }

    // BODY: bodyHeight shifted by the new origin's y
    {
        CXRAnchor      a;
        SXRAnchorState st;
        st.mode           = XR_ANCHOR_BODY;
        st.anchorPose.pos = Vec3{0, 1.2f, -1.2f};
        a.initFromState(st);
        a.solve(viewInput({{0, 1.6f, 0}, Quat{}}), tune); // captures bodyHeight = 1.6
        const float before   = a.state().bodyHeight;
        const float expected = poseCompose(poseInverse(M), SXRPose{Vec3{0, before, 0}, Quat{}}).pos.y;
        a.onReferenceSpaceChanged(M);
        EXPECT_NEAR(a.state().bodyHeight, expected, 1e-5f);
    }
}

// 13 (WP8 extension) ----------------------------------------------------------------------------
// grabPushPull / grabResize (doc 03 §4.3, driven by the doc 04 §6 grab machine's thumbstick
// handling): distance clamp 0.3-5.0 m, width clamp 0.2-4.0 m, direction preserved.
TEST(XRAnchorMath, GrabPushPullResizeClamps) {
    const auto     tune = defaultTuning();
    const SXRPose  view{{0, 1.5f, 0}, Quat{}};
    const SXRPose  grip{{0, 1.2f, -0.3f}, Quat{}};

    CXRAnchor      a;
    SXRAnchorState st;
    st.mode           = XR_ANCHOR_LOCAL;
    st.anchorPose.pos = Vec3{0.f, 1.4f, -1.0f}; // 1.0 m in front of the grip along -Z
    st.widthMeters    = 1.6f;
    a.initFromState(st);

    SXRSolveInput in = viewInput(view);
    in.gripLeft      = grip;

    const SXRPose world0 = a.solve(in, tune).worldPose;
    a.beginGrab(XR_HAND_LEFT, grip);

    // grip -> quad offset direction should be preserved through push/pull.
    const Vec3 dir0 = (world0.pos - grip.pos).normalized();

    // Push far past the max: clamps at 5.0 m from the grip.
    a.grabPushPull(100.f);
    {
        const SXRPose w = a.solve(in, tune).worldPose;
        EXPECT_NEAR((w.pos - grip.pos).length(), 5.0f, 1e-4f);
        expectVecNear((w.pos - grip.pos).normalized(), dir0, 1e-4f);
    }

    // Pull far past the min: clamps at 0.3 m from the grip.
    a.grabPushPull(-100.f);
    {
        const SXRPose w = a.solve(in, tune).worldPose;
        EXPECT_NEAR((w.pos - grip.pos).length(), 0.3f, 1e-4f);
        expectVecNear((w.pos - grip.pos).normalized(), dir0, 1e-4f);
    }

    // A small, in-range push moves the distance by exactly the delta.
    a.grabResize(0.f); // no-op sanity: width unaffected by push/pull
    const float distBefore = (a.solve(in, tune).worldPose.pos - grip.pos).length();
    a.grabPushPull(0.2f);
    EXPECT_NEAR((a.solve(in, tune).worldPose.pos - grip.pos).length(), distBefore + 0.2f, 1e-4f);

    // grabResize: width clamps at 4.0 m / 0.2 m, unaffected by push/pull state.
    a.grabResize(100.f);
    EXPECT_NEAR(a.state().widthMeters, 4.0f, 1e-4f);
    a.grabResize(-100.f);
    EXPECT_NEAR(a.state().widthMeters, 0.2f, 1e-4f);
    a.grabResize(0.5f);
    EXPECT_NEAR(a.state().widthMeters, 0.7f, 1e-4f);

    // endGrab re-anchors LOCAL to the final grabbed world pose (round trip already covered by
    // GrabRoundTripIdentity; here just check the mode stays LOCAL and the pose matches solve()'s
    // last grabbed world pose).
    const SXRPose lastGrabbedWorld = a.solve(in, tune).worldPose;
    a.endGrab(in, tune);
    EXPECT_EQ(a.state().mode, XR_ANCHOR_LOCAL);
    expectVecNear(a.state().anchorPose.pos, lastGrabbedWorld.pos, 1e-4f);
}

// ---- report-20 issue C: recenter-on-plug (recenterLocalToHead) ----

TEST(XRAnchorRecenter, LocalReseatsHeadRelativePreservingHeightAndDistance) {
    // Declared "1.5m up, 1.5m in front, facing origin" (identity rot = faces +Z toward a user at the
    // origin looking down -Z). The runtime origin is arbitrary; the head is off at (10, 1.6, 5) facing
    // +X (yaw = -90deg: -Z rotates to +X). After recenter the monitor must sit 1.5m in front of the
    // head along its facing, at the configured floor height 1.5, facing back at the head.
    SXRAnchorState decl;
    decl.mode           = XR_ANCHOR_LOCAL;
    decl.anchorPose.pos = {0.f, 1.5f, -1.5f};
    decl.anchorPose.rot = Quat{}; // identity

    CXRAnchor a;
    a.initFromState(decl);

    // Head at (10,1.6,5), yaw = -90deg so forward (-Z) points +X.
    const float yaw = -90.f * PI / 180.f;
    SXRPose     head{{10.f, 1.6f, 5.f}, qFromYaw(yaw)};

    a.recenterLocalToHead(head, decl);

    const SXRPose& W = a.state().anchorPose;
    // Configured height preserved (floor-relative), independent of head y.
    EXPECT_NEAR(W.pos.y, 1.5f, 1e-4f);
    // 1.5m in front of the head along +X (yaw -90): head XZ (10,5) + (1.5, 0) = (11.5, 5).
    EXPECT_NEAR(W.pos.x, 11.5f, 1e-4f);
    EXPECT_NEAR(W.pos.z, 5.f, 1e-4f);
    // Distance from head is exactly the configured horizontal 1.5m (heights differ by 0.1).
    const float dxz = std::sqrt((W.pos.x - head.pos.x) * (W.pos.x - head.pos.x) + (W.pos.z - head.pos.z) * (W.pos.z - head.pos.z));
    EXPECT_NEAR(dxz, 1.5f, 1e-4f);
    // Facing: the quad's +Z (its normal toward the viewer) now points back toward the head (-X).
    const Vec3 normal = qRotate(W.rot, Vec3{0.f, 0.f, 1.f});
    EXPECT_NEAR(normal.x, -1.f, 1e-4f);
    EXPECT_NEAR(normal.y, 0.f, 1e-4f);
    EXPECT_NEAR(normal.z, 0.f, 1e-4f);
}

TEST(XRAnchorRecenter, MultiMonitorGroupTransformedRigidly) {
    // Two monitors with distinct declared offsets, recentered with the SAME head pose, must keep the
    // exact vector between them (rigid transform), just rotated/translated into the head frame.
    SXRAnchorState left, right;
    left.mode            = XR_ANCHOR_LOCAL;
    left.anchorPose.pos  = {-1.f, 1.5f, -1.5f};
    left.anchorPose.rot  = Quat{};
    right.mode           = XR_ANCHOR_LOCAL;
    right.anchorPose.pos = {1.f, 1.5f, -1.5f};
    right.anchorPose.rot = Quat{};

    const Vec3 declSep = right.anchorPose.pos - left.anchorPose.pos; // (2,0,0)

    CXRAnchor la, ra;
    la.initFromState(left);
    ra.initFromState(right);

    SXRPose head{{3.f, 1.7f, -2.f}, qFromYaw(40.f * PI / 180.f)};
    la.recenterLocalToHead(head, left);
    ra.recenterLocalToHead(head, right);

    const Vec3  sep = ra.state().anchorPose.pos - la.state().anchorPose.pos;
    // Rigid: separation length is preserved (2m), and it is the declared separation rotated by yaw.
    EXPECT_NEAR(sep.length(), declSep.length(), 1e-4f);
    const Vec3 expected = qRotate(qFromYaw(40.f * PI / 180.f), declSep);
    expectVecNear(sep, expected, 1e-4f);
}

TEST(XRAnchorRecenter, NonLocalModesUnaffected) {
    // head/body/device anchors are already user-relative; recenter must be a no-op for them.
    SXRAnchorState st;
    st.mode           = XR_ANCHOR_HEAD;
    st.anchorPose.pos = {0.f, 0.f, -1.5f};
    CXRAnchor a;
    a.initFromState(st);
    const SXRPose before = a.state().anchorPose;

    a.recenterLocalToHead(SXRPose{{5.f, 1.6f, 5.f}, qFromYaw(1.f)}, st);
    expectVecNear(a.state().anchorPose.pos, before.pos, 1e-6f);
    EXPECT_EQ(a.state().mode, XR_ANCHOR_HEAD);
}

// ---- research/22 §4.3: reconstructing a reference-space change the runtime refused to describe ----
//
// monado — so WiVRn, so every session on this machine — pushes
// XrEventDataReferenceSpaceChangePending with pose_valid = false and an identity pose. The handler
// used to drop exactly those, which left every anchor holding coordinates in a frame that no longer
// existed and flung the monitors across the room by the whole frame shift. These pin the head-pair
// reconstruction that recovers the delta, and the ladder deciding when it is trustworthy.

TEST(XRAnchorRecenter, HeadPairReconstructsTheWithheldFrameChange) {
    // Ground truth at the magnitude actually measured live: the latched head frame moved 8.25 m and
    // ~155 deg of yaw across one recenter, inside a single session, with the user at the same desk.
    const SXRPose M{{8.25f, 0.f, -0.42f}, qFromYaw(155.f * PI / 180.f)};

    // One physical head, expressed on both sides of the swap.
    const SXRPose headOld{{1.2f, 1.62f, -0.4f}, qFromYaw(20.f * PI / 180.f)};
    const SXRPose headNew = poseCompose(poseInverse(M), headOld);

    const SXRPose solved = solveReferenceSpaceChangeFromHead(headOld, headNew);
    expectVecNear(solved.pos, M.pos, 1e-3f);
    EXPECT_NEAR(qAngleBetween(solved.rot, M.rot), 0.f, 1e-4f);
}

TEST(XRAnchorRecenter, HeadPairSolveIgnoresPitchAndRoll) {
    // The head keeps moving between the two samples; only its YAW may enter the solve, because both
    // LOCAL_FLOOR frames are gravity-aligned and the true delta is 4-DoF. Pitch/roll leaking in would
    // tilt the whole monitor group off the horizon.
    const SXRPose M{{2.f, 0.f, -1.f}, qFromYaw(0.9f)};
    const SXRPose headOld{{0.4f, 1.6f, 0.2f}, qMul(qFromYaw(0.3f), qFromPitch(0.4f))};
    const SXRPose headNewTrue = poseCompose(poseInverse(M), headOld);
    // Same position, but the head has pitched down and rolled since the pre-change sample.
    const SXRPose headNew{headNewTrue.pos, qMul(qMul(qFromYaw(qYawOf(headNewTrue.rot, 0.f)), qFromPitch(-0.7f)), qFromAxisAngle(Vec3{0.f, 0.f, 1.f}, 0.25f))};

    const SXRPose solved = solveReferenceSpaceChangeFromHead(headOld, headNew);
    EXPECT_NEAR(qAngleBetween(solved.rot, M.rot), 0.f, 1e-4f);
    expectVecNear(solved.pos, M.pos, 1e-4f);
}

TEST(XRAnchorRecenter, HeadPairSolveInventsNoYawWhenYawIsUnobservable) {
    // A head staring straight up has no yaw to read (doc 03 §1.5). The solve must fall back to a pure
    // translation rather than let qYawOf's fallback fabricate a rotation and spin the desktop.
    const SXRPose up{{0.5f, 1.6f, 0.2f}, qFromPitch(PI / 2.f)};
    const SXRPose upMoved{{-1.f, 1.6f, 3.f}, qFromPitch(PI / 2.f)};

    const SXRPose solved = solveReferenceSpaceChangeFromHead(up, upMoved);
    EXPECT_NEAR(qAngleBetween(solved.rot, Quat{}), 0.f, 1e-5f);
    expectVecNear(solved.pos, up.pos - upMoved.pos, 1e-5f);
}

TEST(XRAnchorRecenter, ReconstructedChangeHoldsALocalMonitorWhereItIsInTheRoom) {
    // End to end: the monitor must occupy the same physical spot after the recenter.
    const auto    tune = defaultTuning();
    const SXRPose M{{8.25f, 0.f, -0.42f}, qFromYaw(155.f * PI / 180.f)};
    const SXRPose headOld{{1.2f, 1.62f, -0.4f}, qFromYaw(20.f * PI / 180.f)};
    const SXRPose headNew = poseCompose(poseInverse(M), headOld);

    CXRAnchor      a;
    SXRAnchorState st;
    st.mode           = XR_ANCHOR_LOCAL;
    st.anchorPose.pos = Vec3{1.0f, 1.4f, -1.5f};
    st.anchorPose.rot = qFromYaw(35.f * PI / 180.f);
    a.initFromState(st);
    const SXRPose oldWorld = a.solve(viewInput(headOld), tune).worldPose;

    a.onReferenceSpaceChanged(solveReferenceSpaceChangeFromHead(headOld, headNew));
    const SXRPose newWorld = a.solve(viewInput(headNew), tune).worldPose;

    // Re-composing the new coordinates through the TRUE delta must land back on the old ones.
    const SXRPose recomposed = poseCompose(M, newWorld);
    expectVecNear(recomposed.pos, oldWorld.pos, 1e-3f);
    EXPECT_NEAR(qAngleBetween(recomposed.rot, oldWorld.rot), 0.f, 1e-3f);

    // And the numbers really did move: without the reconstruction the monitor would have stayed at
    // `oldWorld`'s coordinates, which is now metres away in the room. That gap IS the reported bug.
    EXPECT_GT((newWorld.pos - oldWorld.pos).length(), 5.f);
}

TEST(XRAnchorRecenter, FixLadderPicksTheOnlyTrustworthySource) {
    // A runtime that actually fills poseInPreviousSpace always wins, however old the head sample is.
    EXPECT_EQ(xrRecenterFix(true, false, 0), eXRRecenterFix::XR_RECENTER_APPLY_RUNTIME_POSE);
    EXPECT_EQ(xrRecenterFix(true, true, 10'000'000'000), eXRRecenterFix::XR_RECENTER_APPLY_RUNTIME_POSE);

    // No runtime pose: one frame of tracking (~11 ms at 90 Hz) straddling the change is the good case.
    EXPECT_EQ(xrRecenterFix(false, true, 11'000'000), eXRRecenterFix::XR_RECENTER_SOLVE_FROM_HEAD);
    EXPECT_EQ(xrRecenterFix(false, true, XR_RECENTER_HEAD_MAX_AGE_NS), eXRRecenterFix::XR_RECENTER_SOLVE_FROM_HEAD);

    // Past the bound the head sample proves nothing about where the room went: the wearer may have
    // taken the headset off and walked away, which is precisely the doff/re-don case.
    EXPECT_EQ(xrRecenterFix(false, true, XR_RECENTER_HEAD_MAX_AGE_NS + 1), eXRRecenterFix::XR_RECENTER_RESEAT_TO_HEAD);
    EXPECT_EQ(xrRecenterFix(false, false, 0), eXRRecenterFix::XR_RECENTER_RESEAT_TO_HEAD);
    EXPECT_EQ(xrRecenterFix(false, true, -1), eXRRecenterFix::XR_RECENTER_RESEAT_TO_HEAD); // clock went backwards
}

TEST(XRAnchorRecenter, NoOpGuardSkipsAnUnchangedFrame) {
    const SXRPose head{{1.f, 1.6f, -2.f}, qFromYaw(0.7f)};
    EXPECT_TRUE(xrRecenterIsNoOp(SXRPose{}));
    EXPECT_TRUE(xrRecenterIsNoOp(solveReferenceSpaceChangeFromHead(head, head)));
    EXPECT_FALSE(xrRecenterIsNoOp(SXRPose{Vec3{0.f, 0.f, 0.05f}, Quat{}}));
    EXPECT_FALSE(xrRecenterIsNoOp(SXRPose{Vec3{}, qFromYaw(0.1f)}));
}
