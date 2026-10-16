#include <openxr/XRDonSettle.hpp>
#include <openxr/XRDonDoff.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <functional>

// tests/xr/donoff_settle.cpp — research/30 §5.2 / §13.1: the don settle detector over synthetic
// head traces. Features (angular / linear speed) are derived exactly as the frame thread derives
// them (xrHeadFeatures, EMA'd), so the thresholds are exercised through the same filter.

using namespace OpenXR;

static constexpr float DEG = 3.14159265F / 180.F;

static Quat            headRot(float yaw, float pitch, float roll = 0.F) {
    return qMul(qFromYaw(yaw), qMul(qFromPitch(pitch), qFromAxisAngle(Vec3{0.F, 0.F, -1.F}, roll)));
}

struct SDriver {
    CXRSettleDetector det;
    SXRHeadFeatures   f;
    SXRPose           prev;
    bool              prevValid  = false;
    int64_t           tEdge      = 5'000'000'000LL;
    int64_t           t          = 5'000'000'000LL;
    float             hz         = 90.F;
    bool              posTracked = true;
    bool              trusted    = true;
    SXRSettleResult   result;
    bool              done = false;

    explicit SDriver(SXRSettleParams p) {
        det.reset(tEdge, p);
    }

    // feed `seconds` of head poses from `fn(tSinceSegmentStart)`; stops at the first decision
    void run(float seconds, const std::function<SXRPose(float)>& fn) {
        const float dt = 1.F / hz;
        const int   n  = static_cast<int>(std::lround(seconds * hz));
        for (int i = 0; i < n && !done; ++i) {
            const SXRPose head = fn(static_cast<float>(i) * dt);
            t += static_cast<int64_t>(std::llround(static_cast<double>(dt) * 1e9));
            f         = xrHeadFeatures(prev, head, dt, f, posTracked, prevValid);
            prev      = head;
            prevValid = true;
            const auto r =
                det.step(SXRSettleInput{.tNs = t, .head = head, .valid = true, .trusted = trusted, .posTracked = posTracked, .angSpeed = f.angSpeed, .linSpeed = f.linSpeed});
            if (r.state == XR_SETTLE_WAITING)
                continue;
            result = r;
            done   = true;
        }
    }

    float sinceEdgeS() const {
        return static_cast<float>(static_cast<double>(t - tEdge) * 1e-9);
    }
};

// a worn head is never dead still: sub-degree sway around a facing
static SXRPose sway(float yaw, float pitch, float tS, Vec3 pos = {0.F, 1.2F, 0.F}) {
    const float s = 0.5F * DEG * std::sin(tS * 2.F * 3.14159265F * 0.7F);
    return SXRPose{pos, headRot(yaw + s, pitch + 0.3F * s)};
}

// putting the glasses on: lifted from below the eye line, then fitted with a 150°/s wobble
static void donMotion(SDriver& d) {
    // continuous across the segments (a jump between frames would be a 1000s-of-°/s spike no head makes)
    d.run(0.3F, [](float t) { return SXRPose{{0.F, 0.9F + t, 0.F}, headRot(25.F * DEG * t / 0.3F, -40.F * DEG * (1.F - t / 0.3F))}; });
    d.run(1.F, [](float t) { return SXRPose{{0.F, 1.2F, 0.F}, headRot(25.F * DEG + 25.F * DEG * std::sin(t * 2.F * 3.14159265F), 0.F)}; });
}

TEST(DonOffSettle, CleanDonSettlesAfterTheHoldAtTheHoldMean) {
    SDriver d{SXRSettleParams{.mode = XR_SETTLE_POSE}};
    donMotion(d);
    ASSERT_FALSE(d.done);
    const float stillFrom = d.sinceEdgeS();
    d.run(3.F, [](float t) { return sway(25.F * DEG, -5.F * DEG, t, Vec3{0.1F, 1.2F, -0.05F}); });
    ASSERT_TRUE(d.done);
    EXPECT_EQ(d.result.state, XR_SETTLE_SETTLED);
    EXPECT_EQ(d.result.rung, XR_RUNG_SETTLED);
    // the hold (500 ms) after the fitting's angular speed has decayed through the EMA
    EXPECT_GE(d.sinceEdgeS() - stillFrom, 0.5F);
    EXPECT_LE(d.sinceEdgeS() - stillFrom, 0.75F);
    ASSERT_TRUE(d.result.havePose);
    EXPECT_NEAR(qYawOf(d.result.pose.rot, 99.F), 25.F * DEG, 1.F * DEG);
    EXPECT_NEAR(d.result.pose.pos.x, 0.1F, 1e-3F);
    EXPECT_NEAR(d.result.pose.pos.z, -0.05F, 1e-3F);
    // the hydration rotation is yaw-only
    EXPECT_NEAR(xrHeadUpY(d.result.pose.rot), 1.F, 1e-4F);
}

TEST(DonOffSettle, AnAdjustmentMidHoldRestartsIt) {
    SDriver d{SXRSettleParams{.mode = XR_SETTLE_POSE}};
    d.run(0.4F, [](float t) { return sway(0.F, 0.F, t); });
    ASSERT_FALSE(d.done);
    // "push the glasses up the nose": a quick pitch nod
    d.run(0.15F, [](float t) { return SXRPose{{0.F, 1.2F, 0.F}, headRot(0.F, -15.F * DEG * std::sin(t / 0.15F * 3.14159265F))}; });
    ASSERT_FALSE(d.done);
    const float nodEnd = d.sinceEdgeS();
    d.run(2.F, [](float t) { return sway(0.F, 0.F, t); });
    ASSERT_TRUE(d.done);
    EXPECT_EQ(d.result.state, XR_SETTLE_SETTLED);
    EXPECT_GE(d.sinceEdgeS() - nodEnd, 0.5F);
}

TEST(DonOffSettle, StillAtTheEdgeStillWaitsTheHold) {
    SDriver d{SXRSettleParams{.mode = XR_SETTLE_POSE}};
    d.run(2.F, [](float t) { return sway(0.F, 0.F, t); });
    ASSERT_TRUE(d.done);
    EXPECT_NEAR(d.sinceEdgeS(), 0.5F, 0.03F);
}

TEST(DonOffSettle, RecliningUserGetsRungTwoAtTheTimeout) {
    SDriver d{SXRSettleParams{.mode = XR_SETTLE_POSE}};
    d.run(4.F, [](float t) { return sway(-30.F * DEG, 50.F * DEG, t); });
    ASSERT_TRUE(d.done);
    EXPECT_EQ(d.result.state, XR_SETTLE_TIMEOUT);
    EXPECT_EQ(d.result.rung, XR_RUNG_LAST_UPRIGHT);
    EXPECT_EQ(d.result.block, XR_SBLK_NOT_LEVEL);
    EXPECT_NEAR(d.sinceEdgeS(), 3.F, 0.02F);
    ASSERT_TRUE(d.result.havePose);
    EXPECT_NEAR(qYawOf(d.result.pose.rot, 99.F), -30.F * DEG, 1.F * DEG);
}

TEST(DonOffSettle, UpsideDownNeverYieldsAPose) {
    // R2 regression: a pitch sweep past vertical, then held there — no yaw is ever read from it
    SDriver d{SXRSettleParams{.mode = XR_SETTLE_POSE}};
    d.run(1.F, [](float t) { return SXRPose{{0.F, 1.2F, 0.F}, headRot(0.F, 120.F * DEG * t)}; });
    d.run(3.F, [](float t) { return sway(0.F, 120.F * DEG, t); });
    ASSERT_TRUE(d.done);
    EXPECT_EQ(d.result.state, XR_SETTLE_TIMEOUT);
    // the first ~0.5 s of the sweep is upright but at most briefly level and fast: no usable 300 ms
    // window, and the reclined rung needs |pitch| <= 60° — which the sweep passed through
    EXPECT_NE(d.result.rung, XR_RUNG_SETTLED);
    EXPECT_NE(d.result.rung, XR_RUNG_BEST_WINDOW);
}

TEST(DonOffSettle, HeldInAHandUpsideDownIsRungThree) {
    SDriver d{SXRSettleParams{.mode = XR_SETTLE_POSE}};
    d.run(4.F, [](float t) { return SXRPose{{0.F, 0.8F, 0.F}, qMul(qFromAxisAngle(Vec3{0.F, 0.F, 1.F}, 3.14159265F), headRot(0.3F * std::sin(t * 3.F), 0.F))}; });
    ASSERT_TRUE(d.done);
    EXPECT_EQ(d.result.state, XR_SETTLE_TIMEOUT);
    EXPECT_EQ(d.result.rung, XR_RUNG_IN_PLACE);
    EXPECT_FALSE(d.result.havePose);
    EXPECT_EQ(d.result.block, XR_SBLK_NOT_UPRIGHT);
}

TEST(DonOffSettle, KeptMovingButLookedForwardOnceIsRungOne) {
    SDriver d{SXRSettleParams{.mode = XR_SETTLE_POSE}};
    // fidgeting (±80°/s) except one calm 0.45 s look at yaw 10°; continuous across the segments
    auto fidget = [](float t) { return SXRPose{{0.F, 1.2F, 0.F}, headRot(10.F * DEG + 20.F * DEG * std::sin(t * 4.F), 0.F)}; };
    d.run(1.571F, fidget);
    d.run(0.45F, [](float) { return SXRPose{{0.F, 1.2F, 0.F}, headRot(10.F * DEG, 0.F)}; });
    d.run(3.F, fidget);
    ASSERT_TRUE(d.done);
    EXPECT_EQ(d.result.state, XR_SETTLE_TIMEOUT);
    EXPECT_EQ(d.result.rung, XR_RUNG_BEST_WINDOW);
    ASSERT_TRUE(d.result.havePose);
    EXPECT_NEAR(qYawOf(d.result.pose.rot, 99.F), 10.F * DEG, 3.F * DEG);
}

TEST(DonOffSettle, SlamGlideSettlesOnlyAfterTheGlide) {
    // XREAL SLAM re-acquisition: the position glides 0.3 m over 1.5 s with the head still
    SDriver d{SXRSettleParams{.mode = XR_SETTLE_POSE, .needPos = true}};
    d.run(1.5F, [](float t) { return sway(0.F, 0.F, t, Vec3{0.2F * t, 1.2F, 0.F}); });
    ASSERT_FALSE(d.done);
    const float glideEnd = d.sinceEdgeS();
    d.run(2.F, [](float t) { return sway(0.F, 0.F, t, Vec3{0.3F, 1.2F, 0.F}); });
    ASSERT_TRUE(d.done);
    EXPECT_EQ(d.result.state, XR_SETTLE_SETTLED);
    EXPECT_GE(d.sinceEdgeS() - glideEnd, 0.5F);
    EXPECT_NEAR(d.result.pose.pos.x, 0.3F, 0.01F);
}

TEST(DonOffSettle, ExpectedSixDofWaitsForPositionTracking) {
    // the 3DoF hold window of a SLAM re-acquire: rotation good, POSITION_TRACKED clear
    SDriver d{SXRSettleParams{.mode = XR_SETTLE_POSE, .needPos = true}};
    d.posTracked = false;
    d.run(1.8F, [](float t) { return sway(0.F, 0.F, t); });
    ASSERT_FALSE(d.done);
    d.posTracked = true;
    d.run(2.F, [](float t) { return sway(0.F, 0.F, t); });
    ASSERT_TRUE(d.done);
    EXPECT_EQ(d.result.state, XR_SETTLE_SETTLED);
    EXPECT_GE(d.sinceEdgeS(), 2.3F);
}

TEST(DonOffSettle, ThreeDofRuntimeDoesNotWaitForPosition) {
    // needPos false (no 6DoF ever seen): a 3DoF XREAL or a runtime without the bit settles normally
    SDriver d{SXRSettleParams{.mode = XR_SETTLE_POSE, .needPos = false}};
    d.posTracked = false;
    d.run(2.F, [](float t) { return sway(0.F, 0.F, t); });
    ASSERT_TRUE(d.done);
    EXPECT_EQ(d.result.state, XR_SETTLE_SETTLED);
    EXPECT_LT(d.sinceEdgeS(), 0.6F);
}

TEST(DonOffSettle, PositionThatNeverComesTimesOutWithAPose) {
    SDriver d{SXRSettleParams{.mode = XR_SETTLE_POSE, .needPos = true}};
    d.posTracked = false;
    d.run(4.F, [](float t) { return sway(5.F * DEG, 0.F, t); });
    ASSERT_TRUE(d.done);
    EXPECT_EQ(d.result.state, XR_SETTLE_TIMEOUT);
    EXPECT_EQ(d.result.block, XR_SBLK_NO_POSITION);
    // rung 1 does not need position: the head was calm and level, so the room still comes back
    EXPECT_EQ(d.result.rung, XR_RUNG_BEST_WINDOW);
    EXPECT_NEAR(qYawOf(d.result.pose.rot, 99.F), 5.F * DEG, 1.F * DEG);
}

TEST(DonOffSettle, TrackingModeIgnoresMotionAndWaitsForPosition) {
    SDriver d{SXRSettleParams{.mode = XR_SETTLE_TRACKING, .needPos = true}};
    d.posTracked = false;
    auto turning = [](float t) { return SXRPose{{0.F, 1.2F, 0.F}, headRot(60.F * DEG * t, 0.F)}; };
    d.run(1.8F, turning);
    ASSERT_FALSE(d.done);
    d.posTracked = true;
    d.run(1.F, turning);
    ASSERT_TRUE(d.done);
    EXPECT_EQ(d.result.state, XR_SETTLE_SETTLED);
    EXPECT_FALSE(d.result.havePose);
    EXPECT_NEAR(d.sinceEdgeS(), 1.8F + 0.25F, 0.03F);
}

TEST(DonOffSettle, TrackingModeTimesOutInPlace) {
    SDriver d{SXRSettleParams{.mode = XR_SETTLE_TRACKING, .timeoutNs = 2'000'000'000LL, .needPos = true}};
    d.posTracked = false;
    d.run(3.F, [](float t) { return sway(0.F, 0.F, t); });
    ASSERT_TRUE(d.done);
    EXPECT_EQ(d.result.state, XR_SETTLE_TIMEOUT);
    EXPECT_EQ(d.result.rung, XR_RUNG_IN_PLACE);
    EXPECT_NEAR(d.sinceEdgeS(), 2.F, 0.02F);
}

TEST(DonOffSettle, UntrustedHeadNeverSettles) {
    SDriver d{SXRSettleParams{.mode = XR_SETTLE_POSE}};
    d.trusted = false;
    d.run(4.F, [](float t) { return sway(0.F, 0.F, t); });
    ASSERT_TRUE(d.done);
    EXPECT_EQ(d.result.state, XR_SETTLE_TIMEOUT);
    EXPECT_EQ(d.result.rung, XR_RUNG_IN_PLACE);
    EXPECT_EQ(d.result.block, XR_SBLK_NO_VIEW);
}

TEST(DonOffSettle, RollBeyondTwentyDegreesIsNotLevel) {
    SDriver d{SXRSettleParams{.mode = XR_SETTLE_POSE, .timeoutNs = 1'500'000'000LL}};
    d.run(2.F, [](float t) { return SXRPose{{0.F, 1.2F, 0.F}, headRot(0.5F * DEG * std::sin(t * 4.F), 0.F, 30.F * DEG)}; });
    ASSERT_TRUE(d.done);
    EXPECT_EQ(d.result.state, XR_SETTLE_TIMEOUT);
    EXPECT_EQ(d.result.block, XR_SBLK_NOT_LEVEL);
    EXPECT_NEAR(std::fabs(xrHeadRoll(headRot(0.F, 0.F, 30.F * DEG))), 30.F * DEG, 1e-3F);
}

TEST(DonOffSettle, FadeIsASmoothstep) {
    EXPECT_FLOAT_EQ(xrDonFadeAlpha(0, 180'000'000LL), 0.F);
    EXPECT_FLOAT_EQ(xrDonFadeAlpha(90'000'000LL, 180'000'000LL), 0.5F);
    EXPECT_FLOAT_EQ(xrDonFadeAlpha(180'000'000LL, 180'000'000LL), 1.F);
    EXPECT_FLOAT_EQ(xrDonFadeAlpha(500'000'000LL, 180'000'000LL), 1.F);
    EXPECT_FLOAT_EQ(xrDonFadeAlpha(0, 0), 1.F);
    EXPECT_LT(xrDonFadeAlpha(20'000'000LL, 180'000'000LL), 0.05F);
}
