#include <openxr/XRMath.hpp>
#include <openxr/XRAnchor.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <initializer_list>
#include <vector>

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

// ---- doc 03 §8.3: cross-session restore ----
//
// The remaining monitor lottery, reported live 2026-08-16. A wivrn-server restart recycles the XR
// session and the new one gets a brand-new LOCAL_FLOOR origin. Config-DECLARED monitors survived it,
// because a declared `pos:` is head-relative by construction and the first plug re-seats it around
// the wearer. AD-HOC monitors (`hyprctl openxr create XR-3`) did not: what got stored as their
// "declared" anchor was the world pose the create-time head happened to occupy, so the first plug of
// the new session composed a dead frame's coordinates into the head frame and threw them across the
// room — the user's words were "spun way off, outside my house".
//
// The evidence log names the exact arithmetic. Session one latched its head frame at
// eye [4.23, 1.04, 5.75]; XR-3 and XR-4 were created in that frame at 10:00:25/10:00:30; at 10:01:40
// a recenter moved the origin ("reconstructed a 7.13m / 14.7 deg frame change from the head"), which
// re-expressed every LIVE anchor but not the frozen declared ones. Session two came up at 20:43 with
// eye [4.41, 1.00, 6.11] and re-seated the ad-hoc monitors from those 7-metre stale offsets.
//
// The fix names each monitor's placement relative to the WEARER instead of the origin. These pin
// that form's two load-bearing properties — it is reference-space independent, and re-planting it
// rigidly preserves the constellation — plus the fallback ladder for everything degenerate.

namespace {
    // The wearer's frame in the old (now dead) session, from the live log.
    const SXRPose OLD_HEAD{{4.23f, 1.04f, 5.75f}, qFromYaw(-7.9f * PI / 180.f)};
    // The next session's origin lands somewhere else entirely, and so does the user.
    const SXRPose NEW_HEAD{{-3.10f, 0.98f, 11.40f}, qFromYaw(137.f * PI / 180.f)};

    // Re-seat an anchor the way the frame loop does, from an offset in the wearer's frame.
    SXRPose reseatFrom(const SXRPose& head, const SXRPose& offset) {
        SXRAnchorState seat;
        seat.mode       = XR_ANCHOR_LOCAL;
        seat.anchorPose = offset;
        CXRAnchor a;
        a.initFromState(seat);
        a.recenterLocalToHead(head, seat);
        return a.state().anchorPose;
    }
}

TEST(XRAnchorRestore, HeadFrameCaptureRoundTripsExactly) {
    // xrPoseInHeadFrame is the inverse of the composition recenterLocalToHead performs. If those two
    // ever drift the restore would nudge every monitor a little on each session, so pin the identity
    // on poses with real rotation and off-axis positions.
    const SXRPose worlds[] = {
        {{4.1f, 1.2f, 4.3f}, qFromYaw(0.4f)},
        {{-2.f, 0.35f, 0.f}, qMul(qFromYaw(-2.2f), qFromPitch(0.3f))},
        {{0.f, 2.6f, -9.f}, Quat{}},
    };
    // 2e-3 rad (~0.1 deg) on the angle for the reason MathComposeInverseIdentity gives: qAngleBetween
    // of a near-identity quat is 2*acos(|dot|), whose derivative blows up there, so float noise lands
    // around 6e-4. The positions below are held to a tenth of a millimetre.
    for (const auto& W : worlds) {
        const SXRPose rel = xrPoseInHeadFrame(OLD_HEAD, W);
        const SXRPose back = reseatFrom(OLD_HEAD, rel);
        expectVecNear(back.pos, W.pos, 1e-4f);
        EXPECT_NEAR(qAngleBetween(back.rot, W.rot), 0.f, 2e-3f);
    }
}

TEST(XRAnchorRestore, CaptureIsReferenceSpaceIndependent) {
    // THE property that makes the captured offset durable: it names the monitor relative to the
    // wearer, and both of them move together when the origin does. Take a capture, then apply the
    // exact frame change the live log recorded (7.13 m / 14.7 deg) to BOTH the head and the monitor,
    // and re-capture — the offset must not have budged. A LOCAL anchorPose, by contrast, is 7 m
    // different, which is precisely why replaying one into a new session is the bug.
    const SXRPose M{{7.13f, 0.f, 0.4f}, qFromYaw(14.7f * PI / 180.f)};

    CXRAnchor      a;
    SXRAnchorState st;
    st.mode           = XR_ANCHOR_LOCAL;
    st.anchorPose.pos = {4.1f, 1.2f, 4.25f};
    st.anchorPose.rot = qFromYaw(0.6f);
    a.initFromState(st);

    const SXRPose relBefore = xrPoseInHeadFrame(OLD_HEAD, a.state().anchorPose);

    a.onReferenceSpaceChanged(M);
    const SXRPose headAfter = poseCompose(poseInverse(M), OLD_HEAD);
    const SXRPose relAfter  = xrPoseInHeadFrame(headAfter, a.state().anchorPose);

    expectVecNear(relAfter.pos, relBefore.pos, 1e-3f);
    EXPECT_NEAR(qAngleBetween(relAfter.rot, relBefore.rot), 0.f, 2e-3f);
    // ...while the raw LOCAL coordinates moved by the whole frame change.
    EXPECT_GT((a.state().anchorPose.pos - st.anchorPose.pos).length(), 5.f);
}

TEST(XRAnchorRestore, ConstellationSurvivesASessionRestart) {
    // The reported case, end to end. Two monitors in the old session: a config-DECLARED one 1.5 m in
    // front at 1.2 m height, and an AD-HOC one the user placed 2 m to its right. A wivrn restart
    // gives a new origin AND finds the user standing somewhere else facing another way.
    SXRAnchorState declA;
    declA.mode           = XR_ANCHOR_LOCAL;
    declA.anchorPose.pos = {0.f, 1.2f, -1.5f};
    declA.anchorPose.rot = Quat{};

    // Where they actually sat in the old session's LOCAL_FLOOR.
    const SXRPose worldA = reseatFrom(OLD_HEAD, declA.anchorPose);
    const SXRPose worldB = reseatFrom(OLD_HEAD, SXRPose{Vec3{2.f, 1.2f, -1.5f}, Quat{}});
    // Sanity: they really are 2 m apart, and really are metres from the runtime's origin.
    EXPECT_NEAR((worldB.pos - worldA.pos).length(), 2.f, 1e-4f);
    EXPECT_GT(worldA.pos.length(), 5.f);

    // Capture (what the frame loop does on every frame the user is wearing the headset).
    const SXRPose relA = xrPoseInHeadFrame(OLD_HEAD, worldA);
    const SXRPose relB = xrPoseInHeadFrame(OLD_HEAD, worldB);

    // New session, new origin, new head: restore the constellation.
    const SXRPose newA = reseatFrom(NEW_HEAD, relA);
    const SXRPose newB = reseatFrom(NEW_HEAD, relB);

    // 1. B is still exactly 2 m to A's right — along the NEW head's right axis, because the whole
    //    group was replanted rigidly rather than each monitor re-derived on its own.
    const Vec3 sep = newB.pos - newA.pos;
    EXPECT_NEAR(sep.length(), 2.f, 1e-4f);
    expectVecNear(sep, qRotate(qFromYaw(137.f * PI / 180.f), Vec3{2.f, 0.f, 0.f}), 1e-4f);

    // 2. The group faces the user: A sits 1.5 m in front of the new head at its configured 1.2 m
    //    height, with its normal (+Z) pointing back at them.
    const Vec3 toA = newA.pos - NEW_HEAD.pos;
    EXPECT_NEAR(newA.pos.y, 1.2f, 1e-4f);
    EXPECT_NEAR(std::sqrt(toA.x * toA.x + toA.z * toA.z), 1.5f, 1e-4f);
    const Vec3 normalA = qRotate(newA.rot, Vec3{0.f, 0.f, 1.f});
    const Vec3 fwdNew  = qRotate(NEW_HEAD.rot, Vec3{0.f, 0.f, -1.f});
    expectVecNear(normalA, Vec3{-fwdNew.x, 0.f, -fwdNew.z}, 1e-4f);

    // 3. And the whole head-relative geometry is bit-preserved: every monitor sits where it sat
    //    relative to the wearer, which is the user-visible promise.
    expectVecNear(xrPoseInHeadFrame(NEW_HEAD, newA).pos, relA.pos, 1e-4f);
    expectVecNear(xrPoseInHeadFrame(NEW_HEAD, newB).pos, relB.pos, 1e-4f);

    // 4. The counterfactual that IS the bug: the ad-hoc monitor's "declared" anchor is its raw old
    //    world pose, and re-seating from that composes a dead frame's coordinates into the new head
    //    frame — putting it metres away instead of the 2.5 m it belongs at.
    const SXRPose bugB = reseatFrom(NEW_HEAD, worldB);
    EXPECT_GT((bugB.pos - NEW_HEAD.pos).length(), 5.f);
    EXPECT_LT((newB.pos - NEW_HEAD.pos).length(), 3.f);
}

TEST(XRAnchorRestore, UntouchedDeclaredMonitorRestoresToItsDeclaredRig) {
    // The strict-generalization pin: for a declared monitor the user never moved, the captured offset
    // IS the declared offset, so the restore path and the legacy declared path agree to the float.
    // That is what keeps the behavior doc 03 §8.2 promises for `xrmonitor` lines intact.
    SXRAnchorState decl;
    decl.mode           = XR_ANCHOR_LOCAL;
    decl.anchorPose.pos = {0.6f, 1.35f, -1.8f};
    decl.anchorPose.rot = qFromYaw(-0.35f);

    const SXRPose world = reseatFrom(OLD_HEAD, decl.anchorPose);
    const SXRPose rel   = xrPoseInHeadFrame(OLD_HEAD, world);
    expectVecNear(rel.pos, decl.anchorPose.pos, 1e-4f);
    EXPECT_NEAR(qAngleBetween(rel.rot, decl.anchorPose.rot), 0.f, 2e-3f);

    const SXRPose viaRestore  = reseatFrom(NEW_HEAD, rel);
    const SXRPose viaDeclared = reseatFrom(NEW_HEAD, decl.anchorPose);
    expectVecNear(viaRestore.pos, viaDeclared.pos, 1e-4f);
    EXPECT_NEAR(qAngleBetween(viaRestore.rot, viaDeclared.rot), 0.f, 2e-3f);
}

TEST(XRAnchorRestore, GrabMovedDeclaredMonitorKeepsThePlacementAcrossASession) {
    // Docs are silent on whether a grab-moved DECLARED monitor resets to its config rig across a
    // session restart. It does not, deliberately: the within-session ladder (§8.1) already holds a
    // moved monitor where the user put it, and a reload does not clobber live geometry either. So the
    // moved offset — not the declared one — is what comes back.
    SXRAnchorState decl;
    decl.mode           = XR_ANCHOR_LOCAL;
    decl.anchorPose.pos = {0.f, 1.2f, -1.5f};

    CXRAnchor a;
    a.initFromState(decl);
    a.recenterLocalToHead(OLD_HEAD, decl);

    // The user grabs it and parks it up and to the left (a completed grab lands in anchorPose).
    const SXRPose moved = reseatFrom(OLD_HEAD, SXRPose{Vec3{-1.1f, 1.7f, -2.2f}, qFromYaw(0.5f)});
    a.placeLocalAt(moved);

    const SXRPose rel = xrPoseInHeadFrame(OLD_HEAD, a.state().anchorPose);
    expectVecNear(rel.pos, Vec3{-1.1f, 1.7f, -2.2f}, 1e-4f);

    const SXRPose restored = reseatFrom(NEW_HEAD, rel);
    expectVecNear(xrPoseInHeadFrame(NEW_HEAD, restored).pos, Vec3{-1.1f, 1.7f, -2.2f}, 1e-4f);
    // Not the declared rig it started from.
    EXPECT_GT((restored.pos - reseatFrom(NEW_HEAD, decl.anchorPose).pos).length(), 0.5f);
}

TEST(XRAnchorRestore, CaptureIgnoresHeadPitchAndRoll) {
    // The capture frame is yaw-only, so what the user happened to be looking at when the session died
    // — the floor, the ceiling, head tilted — must not tip the whole room on restore.
    const SXRPose world{{4.1f, 1.2f, 4.25f}, qFromYaw(0.6f)};
    const SXRPose level = xrPoseInHeadFrame(OLD_HEAD, world);

    const SXRPose tilted{OLD_HEAD.pos, qMul(qMul(qFromYaw(qYawOf(OLD_HEAD.rot, 0.f)), qFromPitch(-1.1f)), qFromAxisAngle(Vec3{0.f, 0.f, 1.f}, 0.4f))};
    const SXRPose fromTilted = xrPoseInHeadFrame(tilted, world);

    expectVecNear(fromTilted.pos, level.pos, 1e-4f);
    EXPECT_NEAR(qAngleBetween(fromTilted.rot, level.rot), 0.f, 2e-3f);
}

TEST(XRAnchorRestore, ReseatSourceLadderCoversTheDegenerateCases) {
    // A monitor that has been placed under real tracking replays that placement...
    EXPECT_EQ(xrReseatSource(XR_ANCHOR_LOCAL, true), XR_RESEAT_RESTORED);
    // ...and one that has not falls back to its declared rig. That covers a session that was never
    // donned, a session with no head sample by the time it ended, and a monitor created while the
    // headset was off — in all three the config/creation-time rig is the only honest answer, and it
    // is exactly what shipped before this change.
    EXPECT_EQ(xrReseatSource(XR_ANCHOR_LOCAL, false), XR_RESEAT_DECLARED);

    // head/body/device anchors are already expressed against the user's moving frames. A re-seat is a
    // no-op for them (NonLocalModesUnaffected), so they never consult a captured offset even if one
    // is somehow left over from a period when the monitor was local.
    for (const bool valid : {false, true}) {
        EXPECT_EQ(xrReseatSource(XR_ANCHOR_HEAD, valid), XR_RESEAT_DECLARED);
        EXPECT_EQ(xrReseatSource(XR_ANCHOR_BODY, valid), XR_RESEAT_DECLARED);
        EXPECT_EQ(xrReseatSource(XR_ANCHOR_DEVICE, valid), XR_RESEAT_DECLARED);
    }
}

TEST(XRAnchorRestore, DeclaredRigIsRecognisedAsARigNotAPlacement) {
    // The capture must not record a monitor that is still sitting at its raw `xrmonitor` pose: that
    // pose is relative to the runtime's ARBITRARY origin (the thing §8.2 exists to rescue it from),
    // so remembering it would defeat the rescue on the next session. "Still at its declaration" is a
    // question about provenance — is this pose literally the copy it was assigned from — so the test
    // is bit-equality, with no epsilon that would misjudge a monitor deliberately parked a
    // millimetre away.
    const SXRPose declared{{0.f, 1.5f, -1.5f}, qFromYaw(0.2f)};
    EXPECT_TRUE(xrPoseIdentical(declared, declared));
    EXPECT_TRUE(xrPoseIdentical(declared, SXRPose{{0.f, 1.5f, -1.5f}, qFromYaw(0.2f)}));

    // A re-seat moves it out of the origin-relative rig and into the room, so capture resumes.
    const SXRPose seated = reseatFrom(OLD_HEAD, declared);
    EXPECT_FALSE(xrPoseIdentical(seated, declared));

    // ...as does the smallest deliberate nudge.
    EXPECT_FALSE(xrPoseIdentical(declared, SXRPose{{0.001f, 1.5f, -1.5f}, qFromYaw(0.2f)}));
    EXPECT_FALSE(xrPoseIdentical(declared, SXRPose{{0.f, 1.5f, -1.5f}, qFromYaw(0.2001f)}));
}

TEST(XRAnchorRestore, UnrestorableMonitorLandsInFrontOfTheUserAnyway) {
    // The safe floor under the whole feature. A monitor created with no tracking gets the eye-height
    // default (0, 1.4, -default_distance) as its anchor and no capture; the declared fallback then
    // reads that as head-relative and puts it 1.5 m in front of whoever plugs in — never metres away.
    SXRAnchorState untracked;
    untracked.mode           = XR_ANCHOR_LOCAL;
    untracked.anchorPose.pos = {0.f, 1.4f, -1.5f};

    ASSERT_EQ(xrReseatSource(untracked.mode, /*restoreValid=*/false), XR_RESEAT_DECLARED);
    const SXRPose W    = reseatFrom(NEW_HEAD, untracked.anchorPose);
    const Vec3    toW  = W.pos - NEW_HEAD.pos;
    EXPECT_NEAR(std::sqrt(toW.x * toW.x + toW.z * toW.z), 1.5f, 1e-4f);
    EXPECT_NEAR(W.pos.y, 1.4f, 1e-4f);
}

// ---- doc 03 §8.4: the deliberate re-seat ("bring my monitors to me") ----
//
// The §8.1 ladder deliberately holds monitors still in the room across a reference-space change,
// which is right — it is what ended the monitor lottery — and it left the user with no way to say
// the other thing. Reported live 2026-08-17: pressing the Quest's recenter repeatedly, the monitors
// correctly staying put, and physically turning 180 degrees before recentering as the only
// workaround ("if I pivoted only slightly, everything would land exactly where it already was").
// The answer is a one-shot verb (`xrmonitor reseat`) running the SAME rigid re-seat the first plug
// of a session runs. These pin the two pure decisions that verb is made of — which monitors it
// moves and when it must refuse — plus how it composes with the §8.3 capture.

TEST(XRReseatSet, OnlyLocalMonitorsMove) {
    // Same set as recenter-on-plug, and for the same reason: LOCAL is the only mode whose pose is
    // named against the runtime's origin. A head/body/device-anchored monitor already rides the
    // user, so re-seating it would be a no-op with extra steps (recenterLocalToHead self-guards too).
    EXPECT_TRUE(xrReseatEligible(XR_ANCHOR_LOCAL, /*pendingRemoval=*/false));
    EXPECT_FALSE(xrReseatEligible(XR_ANCHOR_HEAD, false));
    EXPECT_FALSE(xrReseatEligible(XR_ANCHOR_BODY, false));
    EXPECT_FALSE(xrReseatEligible(XR_ANCHOR_DEVICE, false));
}

TEST(XRReseatSet, PendingRemovalIsNeverEligible) {
    // A layer mid-removal-barrier belongs to that path. Counting it would also make the verb's
    // "re-seated N monitors" a claim about a monitor that is on its way out.
    EXPECT_FALSE(xrReseatEligible(XR_ANCHOR_LOCAL, /*pendingRemoval=*/true));
    EXPECT_FALSE(xrReseatEligible(XR_ANCHOR_HEAD, true));
}

TEST(XRReseatGate, ReadyNeedsSessionHeadAndMonitors) {
    EXPECT_EQ(xrReseatBlock(/*sessionUp=*/true, /*haveHeadSample=*/true, /*headValid=*/true, /*headAgeMs=*/11, /*eligibleCount=*/1), XR_RESEAT_READY);
    EXPECT_EQ(xrReseatBlock(true, true, true, 0, 6), XR_RESEAT_READY);
    // Exactly at the limit still counts as fresh (inclusive bound, like xrRecenterFix's).
    EXPECT_EQ(xrReseatBlock(true, true, true, XR_RESEAT_HEAD_MAX_AGE_MS, 1), XR_RESEAT_READY);
}

TEST(XRReseatGate, NoSessionOutranksEverything) {
    // Asked with the session down, the answer is "there is no session", not "no monitors" — the
    // message the user gets has to name the thing they can actually do something about.
    EXPECT_EQ(xrReseatBlock(/*sessionUp=*/false, false, false, -1, 0), XR_RESEAT_NO_SESSION);
    EXPECT_EQ(xrReseatBlock(false, true, true, 5, 3), XR_RESEAT_NO_SESSION);
}

TEST(XRReseatGate, RefusesWithoutALiveHead) {
    // No frame rendered yet: the pose ring is empty.
    EXPECT_EQ(xrReseatBlock(true, /*haveHeadSample=*/false, false, -1, 2), XR_RESEAT_NO_HEAD);
    // The newest sample exists but the view was not locatable that frame (tracking lost / headset off).
    EXPECT_EQ(xrReseatBlock(true, true, /*headValid=*/false, 5, 2), XR_RESEAT_NO_HEAD);
    // Tracked, but nothing has been published for over a second — "the current head" would be a
    // guess, and flinging the whole group at a guess is the class of bug this all exists to end.
    EXPECT_EQ(xrReseatBlock(true, true, true, XR_RESEAT_HEAD_MAX_AGE_MS + 1, 2), XR_RESEAT_NO_HEAD);
    // A sample stamped in the future proves nothing either (same treatment as xrRecenterFix).
    EXPECT_EQ(xrReseatBlock(true, true, true, -1, 2), XR_RESEAT_NO_HEAD);
}

TEST(XRReseatGate, CleanNoOpWithNothingToMove) {
    // A session whose monitors are all head-leashed is a legitimate configuration, not a failure —
    // but the verb must say so rather than report success on zero monitors.
    EXPECT_EQ(xrReseatBlock(true, true, true, 11, /*eligibleCount=*/0), XR_RESEAT_NO_MONITORS);
}

TEST(XRReseatRestore, ReseatThenCaptureIsAFixedPoint) {
    // THE interaction between the deliberate re-seat and the §8.3 placement capture, which run in
    // that order on the same frame. The re-seat plants headFrame(new) ∘ offset; the capture then
    // measures inv(headFrame(new)) ∘ pose off the very same head. Those are exact inverses, so the
    // stored placement comes back unchanged: a re-seat does not fight the capture, it hands the
    // capture its own offset back. That is what makes the verb safe to bind to a key and mash, and
    // what makes a `follow`-policy recenter idempotent instead of cumulative.
    const SXRPose offset = xrPoseInHeadFrame(OLD_HEAD, SXRPose{{4.9f, 1.35f, 3.2f}, qFromYaw(0.6f)});

    SXRPose       captured = offset;
    for (int i = 0; i < 3; ++i) {
        const SXRPose seated = reseatFrom(NEW_HEAD, captured);
        captured             = xrPoseInHeadFrame(NEW_HEAD, seated); // the capture the frame loop takes right after
        expectVecNear(captured.pos, offset.pos, 1e-4f);
        EXPECT_NEAR(qAngleBetween(captured.rot, offset.rot), 0.f, 2e-3f);
    }

    // ...so re-seating from the re-captured offset lands the monitor in the same place every time,
    // and mashing the keybind does not walk the group across the room.
    expectVecNear(reseatFrom(NEW_HEAD, captured).pos, reseatFrom(NEW_HEAD, offset).pos, 1e-4f);
}

TEST(XRReseatRestore, GroupGeometrySurvivesADeliberateReseat) {
    // The user's actual ask: bring the LAYOUT to my current facing — not three monitors piled on
    // top of each other. The same head for every monitor makes it a rigid transform, so every
    // pairwise separation is preserved. This is §8.2's property, asserted again for the on-demand
    // path because that is the one a keybind fires dozens of times a day.
    const SXRPose worlds[] = {
        {{3.4f, 1.30f, 5.1f}, qFromYaw(0.10f)},
        {{4.6f, 1.45f, 5.6f}, qFromYaw(-0.55f)},
        {{2.2f, 0.95f, 6.0f}, qFromYaw(0.80f)},
    };

    SXRPose seated[3];
    for (int i = 0; i < 3; ++i)
        seated[i] = reseatFrom(NEW_HEAD, xrPoseInHeadFrame(OLD_HEAD, worlds[i]));

    for (int i = 0; i < 3; ++i)
        for (int j = i + 1; j < 3; ++j) {
            const Vec3 before = worlds[j].pos - worlds[i].pos;
            const Vec3 after  = seated[j].pos - seated[i].pos;
            EXPECT_NEAR(after.length(), before.length(), 1e-4f);
            // Heights are absolute (the head frame is built at y = 0), so the group keeps its
            // vertical arrangement rather than being flattened onto the new eye height.
            EXPECT_NEAR(after.y, before.y, 1e-4f);
        }
}

// ---- doc 03 §8.4: the group seat frame ----
//
// THE reason the deliberate re-seat cannot be the §8.2 one. The §8.3 capture re-derives every stored
// offset every frame while the headset is worn, so "replant the stored offset around the current
// head" is hf(H) ∘ inv(hf(H)) ∘ pose — the monitor's own pose, back. A verb built on that would land
// everything exactly where it already was, which is the complaint that asked for the feature.
//
// So the deliberate re-seat derives its reference from the ARRANGEMENT: the frame a viewer would
// have to occupy to see the group head-on. Move that frame onto the head and the whole group comes
// with it, rigidly. These pin the derivation, and the round trip through recenterLocalToHead that
// the frame loop actually performs.

namespace {
    // Exactly what the frame loop's GROUP branch does: derive the seat frame, then re-express every
    // monitor in it and re-plant that in the head's frame.
    std::vector<SXRPose> groupReseat(const std::vector<SXRPose>& worlds, const SXRPose& head) {
        const auto           seat = xrGroupSeatFrame(worlds.data(), worlds.size(), head);
        std::vector<SXRPose> out;
        if (!seat.valid)
            return worlds; // the frame loop leaves the group alone
        for (const auto& W : worlds) {
            SXRAnchorState live;
            live.mode       = XR_ANCHOR_LOCAL;
            live.anchorPose = W;
            CXRAnchor a;
            a.initFromState(live);

            SXRAnchorState replant;
            replant.mode       = XR_ANCHOR_LOCAL;
            replant.anchorPose = xrPoseInHeadFrame(seat.frame, W);
            a.recenterLocalToHead(head, replant);
            out.push_back(a.state().anchorPose);
        }
        return out;
    }

    // A flat row of three monitors 1.5 m in front of a viewer at the origin facing -Z, all facing
    // back at them (+Z normal, identity rotation). The layout `append right` produces.
    std::vector<SXRPose> wallAt(float z, float y = 1.4f) {
        return {
            {{-1.f, y, z}, Quat{}},
            {{0.f, y, z}, Quat{}},
            {{1.f, y, z}, Quat{}},
        };
    }

    // A toed-in arc of radius r: each monitor sits at yaw a from the viewer's forward and faces back.
    std::vector<SXRPose> arcAt(float r, std::initializer_list<float> anglesDeg) {
        std::vector<SXRPose> out;
        for (const float deg : anglesDeg) {
            const float a = deg * PI / 180.f;
            out.push_back(SXRPose{{-r * std::sin(a), 1.4f, -r * std::cos(a)}, qFromYaw(a)});
        }
        return out;
    }
}

TEST(XRGroupSeat, WallSeatIsWhereItsViewerStands) {
    // The seat frame of a wall is dead in front of it, at the distance the viewer is currently
    // viewing it from — so a user already sitting square to their monitors gets the identity, and
    // the verb honestly reports "re-seated 3 monitors" that did not need to move.
    const auto    monitors = wallAt(-1.5f);
    const SXRPose head{{0.f, 1.6f, 0.f}, Quat{}};

    const auto    seat = xrGroupSeatFrame(monitors.data(), monitors.size(), head);
    ASSERT_TRUE(seat.valid);
    expectVecNear(seat.frame.pos, Vec3{0.f, 0.f, 0.f}, 1e-4f);
    EXPECT_NEAR(qYawOf(seat.frame.rot, 999.f), 0.f, 1e-4f);

    const auto after = groupReseat(monitors, head);
    for (size_t i = 0; i < monitors.size(); ++i)
        expectVecNear(after[i].pos, monitors[i].pos, 1e-4f);
}

TEST(XRGroupSeat, ArcSeatIsItsFocus) {
    // A toed-in arc names its own viewing point exactly: the mean normal points at the focus and
    // the perpendicular distance is the radius. This is the arrangement a HypXRland desk actually
    // ends up in after a few grab-moves.
    const auto    monitors = arcAt(1.5f, {-30.f, 0.f, 30.f});
    const SXRPose head{{0.f, 1.62f, 0.f}, Quat{}};

    const auto    seat = xrGroupSeatFrame(monitors.data(), monitors.size(), head);
    ASSERT_TRUE(seat.valid);
    expectVecNear(seat.frame.pos, Vec3{0.f, 0.f, 0.f}, 1e-3f);
    EXPECT_NEAR(qYawOf(seat.frame.rot, 999.f), 0.f, 1e-3f);
}

TEST(XRGroupSeat, PivotInPlaceBringsTheGroupToTheNewFacing) {
    // THE reported case, in its mild form: the user swivels their chair and wants the layout to
    // come round. Note what the §8.2 re-seat would have done here — nothing, because the capture had
    // already re-measured every offset against this very head.
    const auto    monitors = arcAt(1.5f, {-25.f, 0.f, 25.f});
    const float   pivot    = 90.f * PI / 180.f;
    const SXRPose head{{0.f, 1.6f, 0.f}, qFromYaw(pivot)};

    const auto    after = groupReseat(monitors, head);
    ASSERT_EQ(after.size(), monitors.size());

    // Each monitor is now the same distance from the head as before (the head did not move, and the
    // transform is a rotation about it)...
    for (size_t i = 0; i < after.size(); ++i) {
        const Vec3 b = monitors[i].pos - head.pos, a = after[i].pos - head.pos;
        EXPECT_NEAR(std::sqrt(a.x * a.x + a.z * a.z), std::sqrt(b.x * b.x + b.z * b.z), 1e-3f);
    }
    // ...and the middle one now sits along the head's new forward, which is what "bring it to my
    // current facing" means. Forward at yaw 90 deg is -X. Compared horizontally: the group keeps its
    // absolute heights (the seat frame is on the floor), so the monitor is below eye level and a 3D
    // direction would carry that tilt.
    const Vec3 toMiddle = Vec3{after[1].pos.x - head.pos.x, 0.f, after[1].pos.z - head.pos.z}.normalized();
    EXPECT_NEAR(toMiddle.x, -1.f, 1e-3f);
    EXPECT_NEAR(toMiddle.z, 0.f, 1e-3f);
    // The relative layout is untouched: separations are preserved exactly.
    for (size_t i = 0; i < after.size(); ++i)
        for (size_t j = i + 1; j < after.size(); ++j)
            EXPECT_NEAR((after[j].pos - after[i].pos).length(), (monitors[j].pos - monitors[i].pos).length(), 1e-3f);
}

TEST(XRGroupSeat, ReseatIsAFixedPoint) {
    // Mash the keybind: the second press must be the identity. After a re-seat the head IS the
    // group's seat frame, so the derivation returns it and the transform collapses. Without the
    // perpendicular projection this would creep on every press.
    const auto    monitors = arcAt(1.6f, {-40.f, -10.f, 20.f});
    const SXRPose head{{2.f, 1.55f, -3.f}, qFromYaw(1.1f)};

    const auto    once  = groupReseat(monitors, head);
    const auto    twice = groupReseat(once, head);
    for (size_t i = 0; i < once.size(); ++i) {
        expectVecNear(twice[i].pos, once[i].pos, 1e-3f);
        EXPECT_NEAR(qAngleBetween(twice[i].rot, once[i].rot), 0.f, 2e-3f);
    }
}

TEST(XRGroupSeat, SlidingAlongAWallIsNotWalkingAwayFromIt) {
    // The viewing distance is measured PERPENDICULARLY to the group's facing. Standing 3 m to one
    // side of a wall you are 1.5 m from is still a 1.5 m viewing distance, so the wall comes round
    // to 1.5 m in front of you rather than being flung 3.4 m away.
    const auto    monitors = wallAt(-1.5f);
    const SXRPose head{{3.f, 1.6f, 0.f}, qFromYaw(0.3f)};

    const auto    seat = xrGroupSeatFrame(monitors.data(), monitors.size(), head);
    ASSERT_TRUE(seat.valid);
    EXPECT_NEAR(seat.frame.pos.z, 0.f, 1e-4f); // 1.5 m out from the wall, not 3.4 m

    const auto after = groupReseat(monitors, head);
    const Vec3 toMid{after[1].pos.x - head.pos.x, 0.f, after[1].pos.z - head.pos.z};
    EXPECT_NEAR(toMid.length(), 1.5f, 1e-3f);
}

TEST(XRGroupSeat, ViewingDistanceIsClamped) {
    // Re-seating from across the room must not park the group across the room. Same clamp every
    // other placement verb uses.
    const auto    monitors = wallAt(-1.5f);
    const SXRPose faraway{{0.f, 1.6f, 20.f}, Quat{}};
    const auto    seat = xrGroupSeatFrame(monitors.data(), monitors.size(), faraway);
    ASSERT_TRUE(seat.valid);
    EXPECT_NEAR(seat.frame.pos.z, -1.5f + XR_DISTANCE_MAX, 1e-4f);

    // ...and standing inside them does not put them on the tip of your nose.
    const SXRPose inside{{0.f, 1.6f, -1.55f}, Quat{}};
    const auto    close = xrGroupSeatFrame(monitors.data(), monitors.size(), inside);
    ASSERT_TRUE(close.valid);
    EXPECT_NEAR(std::fabs(close.frame.pos.z + 1.5f), XR_DISTANCE_MIN, 1e-4f);
}

TEST(XRGroupSeat, SingleMonitorWorks) {
    // The most common arrangement of all. One monitor 2 m to the user's left, facing them; a re-seat
    // puts it 2 m dead ahead, still facing them.
    const std::vector<SXRPose> one{{{-2.f, 1.4f, 0.f}, qFromYaw(-90.f * PI / 180.f)}};
    const SXRPose              head{{0.f, 1.6f, 0.f}, Quat{}};

    const auto                 after = groupReseat(one, head);
    ASSERT_EQ(after.size(), size_t{1});
    expectVecNear(after[0].pos, Vec3{0.f, 1.4f, -2.f}, 1e-3f);
    // Its normal points back at the head (+Z of the quad toward the viewer).
    const Vec3 normal = qRotate(after[0].rot, Vec3{0.f, 0.f, 1.f});
    EXPECT_NEAR(normal.z, 1.f, 1e-3f);
}

TEST(XRGroupSeat, NoCommonFacingHasNoSeat) {
    // A ring of monitors surrounding the user has no "in front of" to be brought around to — their
    // normals cancel. The frame loop must leave the group alone rather than invent a frame, and the
    // status line has to be able to say so.
    const std::vector<SXRPose> ring{
        {{0.f, 1.4f, -1.5f}, Quat{}},
        {{0.f, 1.4f, 1.5f}, qFromYaw(PI)},
    };
    const SXRPose head{{0.f, 1.6f, 0.f}, Quat{}};
    EXPECT_FALSE(xrGroupSeatFrame(ring.data(), ring.size(), head).valid);

    // An empty group is likewise unanswerable (the verb's own gate catches this first).
    EXPECT_FALSE(xrGroupSeatFrame(nullptr, 0, head).valid);
}

TEST(XRGroupSeat, MonitorsLyingFlatDoNotVoteOnFacing) {
    // A quad pitched to face the ceiling has no horizontal normal; letting its degenerate direction
    // into the mean would swing the whole group somewhere arbitrary. It still MOVES with the group
    // (it is in the rigid transform), it just does not get a say in where the front is.
    std::vector<SXRPose> monitors = wallAt(-1.5f);
    monitors.push_back(SXRPose{{0.f, 2.4f, -1.f}, qFromPitch(-90.f * PI / 180.f)}); // a ceiling panel

    const SXRPose head{{0.f, 1.6f, 0.f}, Quat{}};
    const auto    seat = xrGroupSeatFrame(monitors.data(), monitors.size(), head);
    ASSERT_TRUE(seat.valid);
    EXPECT_NEAR(qYawOf(seat.frame.rot, 999.f), 0.f, 1e-4f);

    const auto after = groupReseat(monitors, head);
    ASSERT_EQ(after.size(), monitors.size());
    // The ceiling panel travelled with everything else, keeping its offset from the wall.
    EXPECT_NEAR((after[3].pos - after[1].pos).length(), (monitors[3].pos - monitors[1].pos).length(), 1e-3f);
}
