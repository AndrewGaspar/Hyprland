#include "donoff_trace.hpp"

#include <gtest/gtest.h>

#include <random>

// tests/xr/donoff_seat.cpp — research/30 §6 / §13.1: motion onset, seat selection and intrinsic
// plausibility over synthetic traces modelled on the 2026-10-13..15 live log (§1.2).

using namespace OpenXR;
using namespace DonOffTest;

static const Vec3 SEAT{0.F, 1.6F, 0.F};

// the head-relative offset a commit would write for monitor `i`
static Vec3 committedOffset(const SXRSeatVerdict& v, const SXRArrangementView& arr, size_t i) {
    return xrPoseInHeadFrame(v.seatNow, arr.m[i].world).pos;
}

TEST(DonOffSeat, StillSeatCommitsTheDwell) {
    STrace tr;
    tr.hold(SEAT, 0.F, -5.F * DEG, 60.F);
    const auto arr = desk();
    const auto v   = xrSelectSeat(*tr.h, tr.tNs, arr, SXRSeatParams{});
    ASSERT_TRUE(v.ok);
    EXPECT_EQ(v.chosen.src, XR_SEAT_DWELL);
    EXPECT_NEAR(qYawOf(v.seatNow.rot, 0.F), 0.F, 1.F * DEG);
    EXPECT_EQ(v.primary, 1u);
    EXPECT_LT(committedOffset(v, arr, 0).z, -1.4F);
}

// The 09:39:06 reconstruction: a 60 s dwell facing -z, then a 1.1 s lift that pitches through 110°
// (the glasses lifted up and back), then the presence edge 400 ms later. The old code staged the
// last frame — mirrored. The dwell must win, and every offset must stay in front (z < 0).
TEST(DonOffSeat, LiftAndFlipDoffKeepsTheArrangementInFront) {
    STrace tr;
    tr.hold(SEAT, 0.F, -5.F * DEG, 60.F);
    const int64_t liftStart = tr.tNs;
    tr.move(SEAT, 0.F, -5.F * DEG, SEAT + Vec3{0.F, 0.25F, 0.05F}, 0.F, 110.F * DEG, 1.1F);
    tr.hold(SEAT + Vec3{0.F, 0.25F, 0.05F}, 0.F, 110.F * DEG, 0.4F);
    const auto arr = desk();
    const auto v   = xrSelectSeat(*tr.h, tr.tNs, arr, SXRSeatParams{});
    ASSERT_TRUE(v.ok);
    EXPECT_EQ(v.chosen.src, XR_SEAT_DWELL);
    ASSERT_TRUE(v.onset.found);
    EXPECT_LE(v.onset.tOnsetNs, liftStart + 200'000'000);
    EXPECT_GE(v.onset.tOnsetNs, liftStart - 200'000'000);
    for (size_t i = 0; i < arr.n; ++i) {
        EXPECT_LT(committedOffset(v, arr, i).z, 0.F) << i;
    }
    // the seat is the dwell, not the lifted head: within 5 cm of where the user sat
    EXPECT_NEAR(v.seatNow.pos.y, SEAT.y, 0.05F);
    EXPECT_NEAR(v.seatNow.pos.z, SEAT.z, 0.05F);
}

// The 22:43:56 reconstruction: no presence edge; the last 3 s parked on the forehead (pitch 100°),
// then the session ends.
TEST(DonOffSeat, ForeheadParkThenSessionEnd) {
    STrace tr;
    tr.hold(SEAT, 0.F, 0.F, 45.F);
    tr.move(SEAT, 0.F, 0.F, SEAT + Vec3{0.F, 0.1F, 0.F}, 0.F, 100.F * DEG, 0.6F);
    tr.hold(SEAT + Vec3{0.F, 0.1F, 0.F}, 0.F, 100.F * DEG, 3.F);
    const auto arr = desk();
    const auto v   = xrSelectSeat(*tr.h, tr.tNs, arr, SXRSeatParams{});
    ASSERT_TRUE(v.ok);
    EXPECT_EQ(v.chosen.src, XR_SEAT_DWELL);
    EXPECT_LT(committedOffset(v, arr, 0).z, -1.4F);
    EXPECT_NEAR(qYawOf(v.seatNow.rot, 0.F), 0.F, 2.F * DEG);
}

// R7: the user glances at the right-hand monitor for the last couple of seconds before the doff.
TEST(DonOffSeat, SideGlanceAtDoffDoesNotRotateTheSeat) {
    STrace tr;
    tr.gazeEval = true;
    tr.hold(SEAT, 0.F, 0.F, 60.F, 1);
    tr.move(SEAT, 0.F, 0.F, SEAT, -40.F * DEG, 0.F, 0.5F);
    tr.hold(SEAT, -40.F * DEG, 0.F, 2.F, 2);
    tr.move(SEAT, -40.F * DEG, 0.F, SEAT + Vec3{0.F, 0.2F, 0.F}, -40.F * DEG, 100.F * DEG, 0.8F);
    const auto arr = desk();
    const auto v   = xrSelectSeat(*tr.h, tr.tNs, arr, SXRSeatParams{});
    ASSERT_TRUE(v.ok);
    EXPECT_EQ(v.chosen.src, XR_SEAT_DWELL);
    EXPECT_LT(std::fabs(qYawOf(v.seatNow.rot, 0.F)), 3.F * DEG);
    EXPECT_EQ(v.primary, 1u);
}

// A slow, fumbling two-handed doff and a fast one find different onsets, and both keep the dwell.
TEST(DonOffSeat, SlowAndFastDoffsFindTheirOwnOnsets) {
    for (const float fumble : {0.3F, 3.F}) {
        STrace tr;
        tr.hold(SEAT, 0.F, 0.F, 30.F);
        const int64_t motion = tr.tNs;
        const int     n      = static_cast<int>(fumble * tr.hz);
        for (int i = 0; i < n; ++i) {
            const float ph = static_cast<float>(i) / tr.hz;
            tr.step(SXRPose{SEAT + Vec3{0.F, 0.05F * ph, 0.F}, headRot(25.F * DEG * std::sin(ph * 9.F), -30.F * DEG * std::fabs(std::sin(ph * 5.F)))});
        }
        tr.move(SEAT + Vec3{0.F, 0.05F * fumble, 0.F}, 0.F, 0.F, SEAT + Vec3{0.F, 0.3F, 0.F}, 0.F, 120.F * DEG, 0.5F);
        const auto arr = desk();
        const auto v   = xrSelectSeat(*tr.h, tr.tNs, arr, SXRSeatParams{});
        ASSERT_TRUE(v.ok) << fumble;
        EXPECT_EQ(v.chosen.src, XR_SEAT_DWELL) << fumble;
        ASSERT_TRUE(v.onset.found) << fumble;
        EXPECT_NEAR(static_cast<double>(v.onset.tOnsetNs - motion) * 1e-9, 0.0, 0.25) << fumble;
        EXPECT_LT(std::fabs(qYawOf(v.seatNow.rot, 0.F)), 2.F * DEG) << fumble;
    }
}

// S3: stand up, walk 6 m while wearing, stand 12 s facing away from the desk, doff. The kitchen
// episode is refused (the monitors are behind it and out of reach); the desk episode wins.
TEST(DonOffSeat, WalkAwayWhileWornCommitsTheDesk) {
    STrace     tr;
    const Vec3 kitchen{0.F, 1.6F, 6.F};
    tr.hold(SEAT, 0.F, 0.F, 30.F);
    tr.move(SEAT, 0.F, 0.F, kitchen, 3.14159F, 0.F, 4.F);
    tr.hold(kitchen, 3.14159F, 0.F, 12.F);
    tr.move(kitchen, 3.14159F, 0.F, kitchen + Vec3{0.F, 0.2F, 0.F}, 3.14159F, 110.F * DEG, 0.8F);
    const auto arr = desk();
    const auto v   = xrSelectSeat(*tr.h, tr.tNs, arr, SXRSeatParams{});
    ASSERT_TRUE(v.ok);
    EXPECT_EQ(v.chosen.src, XR_SEAT_EPISODE_OLDER);
    EXPECT_NEAR(v.seatNow.pos.z, 0.F, 0.1F);
    ASSERT_GE(v.nRefused, 1);
    EXPECT_TRUE(v.refused[0].reason == XR_SEATREF_P3_FACING || v.refused[0].reason == XR_SEATREF_P4_REACH);
    EXPECT_LT(committedOffset(v, arr, 0).z, -1.4F);
}

TEST(DonOffSeat, ThreeDofSelectsFromYawAndLevelOnly) {
    STrace tr;
    tr.posTracked = false;
    tr.hold(SEAT, 0.3F, 0.F, 40.F);
    tr.move(SEAT, 0.3F, 0.F, SEAT, 0.3F, 115.F * DEG, 0.9F);
    const auto arr = desk(Vec3{0.F, 0.F, 0.F}, 0.3F);
    const auto v   = xrSelectSeat(*tr.h, tr.tNs, arr, SXRSeatParams{});
    ASSERT_TRUE(v.ok);
    EXPECT_EQ(v.chosen.dof, 3);
    EXPECT_NEAR(qYawOf(v.seatNow.rot, 0.F), 0.3F, 2.F * DEG);
}

TEST(DonOffSeat, SpreadAttentionFallsBackToTheGroupFrame) {
    // the user turns a full circle again and again: no direction dominates (R̄ < 0.4)
    STrace tr;
    float  yaw = 0.F;
    for (int i = 0; i < 90 * 40; ++i) {
        yaw += 2.F * 3.14159F / (90.F * 4.F); // one turn per 4 s at a constant 90°/s
        tr.step(SXRPose{SEAT, headRot(yaw, 0.F)});
    }
    auto arr           = desk();
    arr.groupSeatValid = false;
    auto v             = xrSelectSeat(*tr.h, tr.tNs, arr, SXRSeatParams{});
    EXPECT_FALSE(v.ok);
    arr.groupSeatValid = true;
    arr.groupSeat      = SXRPose{Vec3{0.F, 0.F, 0.F}, qFromYaw(0.F)};
    v                  = xrSelectSeat(*tr.h, tr.tNs, arr, SXRSeatParams{});
    ASSERT_TRUE(v.ok);
    EXPECT_TRUE(v.chosen.yawFromGroup);
    EXPECT_NEAR(qYawOf(v.seatNow.rot, 0.F), 0.F, 1e-3F);
}

// The 10-14/10-15 lock-in: a flipped memory (even with dwell provenance) never vetoes a real dwell.
TEST(DonOffSeat, BadMemoryNeverBlocksADwell) {
    STrace tr;
    tr.hold(SEAT, 0.F, 0.F, 30.F);
    auto arr = desk();
    for (size_t i = 0; i < arr.n; ++i) {
        arr.m[i].memValid  = true;
        arr.m[i].memSrc    = XR_SEAT_DWELL;
        arr.m[i].memOffset = SXRPose{Vec3{0.70F, 1.64F, 1.78F}, Quat{}}; // the flipped capture
    }
    const auto v = xrSelectSeat(*tr.h, tr.tNs, arr, SXRSeatParams{});
    ASSERT_TRUE(v.ok);
    EXPECT_EQ(v.chosen.src, XR_SEAT_DWELL);
    EXPECT_LT(committedOffset(v, arr, 0).z, 0.F);
}

TEST(DonOffSeat, WeakWitnessMayNotOverwriteAStrongMemoryItDisagreesWith) {
    // only 4 s worn: no dwell, so the QUIET window is the only witness
    STrace tr;
    tr.hold(SEAT, 120.F * DEG, 0.F, 4.F);
    tr.move(SEAT, 120.F * DEG, 0.F, SEAT, 120.F * DEG, 110.F * DEG, 0.8F);
    auto arr = desk(Vec3{0.F, 0.F, 0.F}, 120.F * DEG);
    // memory says the primary is straight ahead... of a seat 120° away from this one
    for (size_t i = 0; i < arr.n; ++i) {
        arr.m[i].memValid  = true;
        arr.m[i].memSrc    = XR_SEAT_RESEAT;
        arr.m[i].memOffset = SXRPose{Vec3{1.3F, 1.5F, 0.75F}, Quat{}}; // bearing ~120°
    }
    auto v = xrSelectSeat(*tr.h, tr.tNs, arr, SXRSeatParams{});
    EXPECT_FALSE(v.ok);
    ASSERT_GE(v.nRefused, 1);
    EXPECT_EQ(v.refused[v.nRefused - 1].reason, XR_SEATREF_RANK);
    // the same memory with legacy (weak) provenance: the quiet window replaces it
    for (size_t i = 0; i < arr.n; ++i) {
        arr.m[i].memSrc = XR_SEAT_LEGACY;
    }
    v = xrSelectSeat(*tr.h, tr.tNs, arr, SXRSeatParams{});
    ASSERT_TRUE(v.ok);
    EXPECT_EQ(v.chosen.src, XR_SEAT_QUIET);
}

// D3, as a property: for random arrangements, a seat yawed 180° ± 30° at the same position is
// always refused by P3, and the un-flipped seat is accepted.
TEST(DonOffSeat, FlippedSeatIsAlwaysRefusedByFacing) {
    std::mt19937                          rng(42);
    std::uniform_real_distribution<float> u(-1.F, 1.F);
    STrace                                empty;
    for (int it = 0; it < 1000; ++it) {
        const Vec3  seatPos{u(rng) * 3.F, 1.6F, u(rng) * 3.F};
        const float seatYaw = u(rng) * 3.14159F;
        auto        arr     = desk(Vec3{seatPos.x, 0.F, seatPos.z}, seatYaw);
        // perturb the arc a little
        for (size_t i = 0; i < arr.n; ++i) {
            arr.m[i].world.pos += Vec3{u(rng) * 0.1F, 0.F, u(rng) * 0.1F};
        }
        SXRSeatCandidate good;
        good.src              = XR_SEAT_DWELL;
        good.gen              = 1;
        good.haveYaw          = true;
        good.yawConcentration = 0.9F;
        good.durUs            = 60'000'000;
        good.seat             = SXRPose{seatPos, qFromYaw(seatYaw)};
        SXRSeatCandidate bad  = good;
        bad.seat.rot          = qFromYaw(seatYaw + 3.14159F + u(rng) * 30.F * DEG);
        float    d = 0.F, b = 0.F;
        uint32_t prim = 0;
        SXRPose  now;
        EXPECT_EQ(xrSeatPlausible(good, *empty.h, arr, SXRSeatParams{}, d, prim, b, now), XR_SEATREF_OK) << it;
        EXPECT_EQ(xrSeatPlausible(bad, *empty.h, arr, SXRSeatParams{}, d, prim, b, now), XR_SEATREF_P3_FACING) << it;
    }
}

TEST(DonOffSeat, KnownRecenterBridgesAndUnknownRefuses) {
    STrace tr;
    tr.hold(SEAT, 0.F, 0.F, 20.F);
    tr.h->endSession(tr.tNs); // close the episode in gen 1
    auto arr = desk();
    arr.gen  = 2;
    // gen 2 = the origin moved 1 m along +x (P_new = inv(M) ∘ P_old): the desk is now at x = -1
    SXRGenRecord g;
    g.gen    = 2;
    g.bridge = XR_BRIDGE_UNKNOWN;
    tr.h->recordGen(g);
    auto v = xrSelectSeat(*tr.h, tr.tNs, arr, SXRSeatParams{});
    EXPECT_FALSE(v.ok);
    ASSERT_GE(v.nRefused, 1);
    EXPECT_EQ(v.refused[0].reason, XR_SEATREF_P2_FRAME);

    STrace tr2;
    tr2.hold(SEAT, 0.F, 0.F, 20.F);
    tr2.h->endSession(tr2.tNs);
    g.bridge = XR_BRIDGE_KNOWN;
    g.M      = SXRPose{Vec3{1.F, 0.F, 0.F}, Quat{}};
    tr2.h->recordGen(g);
    auto arr2 = desk(Vec3{-1.F, 0.F, 0.F});
    arr2.gen  = 2;
    v         = xrSelectSeat(*tr2.h, tr2.tNs, arr2, SXRSeatParams{});
    ASSERT_TRUE(v.ok);
    EXPECT_NEAR(v.seatNow.pos.x, -1.F, 0.02F);
    EXPECT_NEAR(committedOffset(v, arr2, 0).x, 0.F, 0.05F);
}

TEST(DonOffSeat, NothingToRememberWithNoEligibleMonitor) {
    STrace tr;
    tr.hold(SEAT, 0.F, 0.F, 20.F);
    auto arr = desk();
    for (size_t i = 0; i < arr.n; ++i) {
        arr.m[i].eligible = false;
    }
    const auto v = xrSelectSeat(*tr.h, tr.tNs, arr, SXRSeatParams{});
    EXPECT_FALSE(v.ok);
    ASSERT_GE(v.nRefused, 1);
    EXPECT_EQ(v.refused[0].reason, XR_SEATREF_NO_MONITORS);
}

TEST(DonOffSeat, CutAtTheOnsetMatchesBruteForce) {
    // the seat after cutting the take-off motion equals the plain mean of the pre-onset dwell
    STrace tr;
    tr.hold(SEAT + Vec3{0.1F, 0.F, 0.F}, 0.2F, 0.F, 20.F);
    tr.move(SEAT + Vec3{0.1F, 0.F, 0.F}, 0.2F, 0.F, SEAT + Vec3{0.4F, 0.1F, 0.F}, 0.2F, 20.F * DEG, 1.F); // a level-ish lift that WOULD contaminate
    tr.move(SEAT + Vec3{0.4F, 0.1F, 0.F}, 0.2F, 20.F * DEG, SEAT + Vec3{0.4F, 0.4F, 0.F}, 0.2F, 110.F * DEG, 0.5F);
    const auto v = xrSelectSeat(*tr.h, tr.tNs, desk(), SXRSeatParams{});
    ASSERT_TRUE(v.ok);
    EXPECT_NEAR(v.seatNow.pos.x, 0.1F, 0.02F);
    EXPECT_NEAR(qYawOf(v.seatNow.rot, 0.F), 0.2F, 1.F * DEG);
}
