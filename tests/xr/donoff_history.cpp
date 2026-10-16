#include "donoff_trace.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <random>
#include <type_traits>

// tests/xr/donoff_history.cpp — research/30 §8 / §13.1: the tiered history. Exactness of the
// summaries (integer time fields bit-exact, float sums within rounding) is the property that lets a
// T4 bucket answer what brute force over its raw frames would.

using namespace OpenXR;
using namespace DonOffTest;

TEST(DonOffHistory, FootprintAndPodness) {
    static_assert(std::is_trivially_copyable_v<CXRPoseHistory>);
    EXPECT_EQ(sizeof(SXRHistSample), 352u);
    EXPECT_LT(sizeof(SXRHistSummary), 660u);
    EXPECT_LT(sizeof(CXRPoseHistory), 3'700'000u);
}

TEST(DonOffHistory, TierSpansAt90Hz) {
    STrace tr;
    tr.hold(Vec3{0.F, 1.6F, 0.F}, 0.F, 0.F, 300.F); // 5 minutes
    const auto& t1 = tr.h->tier(0);
    ASSERT_GT(t1.size(), 1000u);
    for (size_t i = 0; i < t1.size(); ++i) {
        const int64_t span = t1.at(i).t1Ns - t1.at(i).t0Ns;
        EXPECT_GE(span, XR_HIST_TIER_RES_NS[0]);
        EXPECT_LT(span, XR_HIST_TIER_RES_NS[0] + 12'000'000);
    }
    const auto& t2 = tr.h->tier(1);
    EXPECT_GE(t2.size(), 140u); // 300 s / 2 s
    EXPECT_EQ(tr.h->t0().size(), XR_HIST_T0_CAP);
}

struct SBrute {
    uint64_t                                        n = 0, worn = 0, level = 0, still = 0, dwell = 0;
    double                                          wc = 0, ws = 0, w = 0;
    std::map<uint32_t, std::pair<uint64_t, double>> mon; // id -> (n, Σx)
};

static void bruteAdd(SBrute& b, const SXRHistSample& s, int64_t dt) {
    const uint64_t us = static_cast<uint64_t>(dt / 1000);
    ++b.n;
    if (s.flags & XR_HF_WORN)
        b.worn += us;
    if (s.flags & XR_HF_LEVEL)
        b.level += us;
    if (s.flags & XR_HF_STILL)
        b.still += us;
    if (s.flags & XR_HF_IN_DWELL)
        b.dwell += us;
    if ((s.flags & XR_HF_LEVEL) && (s.flags & XR_HF_TRUSTED) && (s.flags & XR_HF_UPRIGHT)) {
        const double wt  = xrStillnessWeight(dt, s.angSpeed);
        const double yaw = qYawOf(s.headRot, 0.F);
        b.wc += wt * std::cos(yaw);
        b.ws += wt * std::sin(yaw);
        b.w += wt;
    }
    for (size_t i = 0; i < s.nMonitors; ++i) {
        auto& e = b.mon[s.monitors[i].id];
        ++e.first;
        e.second += s.monitors[i].pos.x;
    }
}

TEST(DonOffHistory, MergeIsExactAcrossTiersIncludingWrapAndMonitorChurn) {
    // ~10 minutes of random motion around yaw = ±π (the wrap), random wear and stillness, with a
    // monitor created at 1/3 and another destroyed at 2/3 of the stream.
    STrace                                tr;
    std::mt19937                          rng(1234);
    std::uniform_real_distribution<float> u(-1.F, 1.F);
    SBrute                                brute;
    const int                             N   = 90 * 600;
    float                                 yaw = 3.1F;
    tr.nMon                                   = 3;
    for (uint32_t i = 0; i < 3; ++i) {
        tr.mons[i].id  = i + 1;
        tr.mons[i].pos = Vec3{static_cast<float>(i), 1.5F, -1.5F};
    }
    int64_t prevT = 0;
    for (int i = 0; i < N; ++i) {
        if (i == N / 3) {
            tr.mons[3].id  = 4;
            tr.mons[3].pos = Vec3{4.F, 1.5F, -1.F};
            tr.nMon        = 4;
        }
        if (i == 2 * N / 3) { // monitor 2 destroyed: compact
            tr.mons[1] = tr.mons[3];
            tr.nMon    = 3;
        }
        if (i % 900 == 0)
            tr.worn = u(rng) > -0.6F;
        yaw += u(rng) * (i % 400 < 200 ? 0.002F : 0.03F); // alternating still / moving stretches
        tr.step(SXRPose{Vec3{0.F, 1.6F, 0.F}, headRot(yaw, 0.1F * u(rng))});
        const auto&   s  = tr.h->t0().back(0);
        const int64_t dt = i == 0 ? 0 : xrHistDtNs(prevT, s.tNs);
        bruteAdd(brute, s, dt);
        prevT = s.tNs;
    }
    tr.h->endSession(tr.tNs);

    // every tier, summed over its closed buckets, equals brute force
    for (size_t k = 0; k < XR_HIST_TIERS; ++k) {
        SXRHistSummary all;
        const auto&    ring = tr.h->tier(k);
        if (ring.count > ring.size())
            continue; // T1 overflowed (it only spans ~4 min) — the upper tiers still cover it all
        for (size_t i = 0; i < ring.size(); ++i) {
            xrHistMerge(all, ring.at(i));
        }
        EXPECT_EQ(all.n, brute.n) << "tier " << k;
        EXPECT_EQ(all.wornUs, brute.worn) << "tier " << k;
        EXPECT_EQ(all.levelUs, brute.level) << "tier " << k;
        EXPECT_EQ(all.stillUs, brute.still) << "tier " << k;
        EXPECT_EQ(all.dwellUs, brute.dwell) << "tier " << k;
        float yawT = 0.F, concT = 0.F;
        ASSERT_TRUE(xrHistMeanYaw(all, yawT, concT));
        const double yawB  = std::atan2(brute.ws, brute.wc);
        const double concB = std::sqrt(brute.wc * brute.wc + brute.ws * brute.ws) / brute.w;
        EXPECT_NEAR(std::remainder(yawT - yawB, 2.0 * M_PI), 0.0, 2e-3) << "tier " << k;
        EXPECT_NEAR(concT, concB, 2e-3) << "tier " << k;
        EXPECT_NEAR(all.yawW, brute.w, brute.w * 1e-3) << "tier " << k;
        ASSERT_EQ(all.nMonitors, brute.mon.size()) << "tier " << k;
        for (size_t i = 0; i < all.nMonitors; ++i) {
            const auto& m = all.monitors[i];
            ASSERT_TRUE(brute.mon.contains(m.id));
            EXPECT_EQ(m.n, brute.mon[m.id].first) << "tier " << k << " id " << m.id;
            EXPECT_NEAR(m.posSum.x, brute.mon[m.id].second, 1e-2 * std::max(1.0, brute.mon[m.id].second)) << "tier " << k;
        }
        EXPECT_EQ(all.monitorsDropped, 0);
    }
}

TEST(DonOffHistory, MonitorOverflowKeepsTheBusiestAndSaysSo) {
    SXRHistSummary a, b;
    a.n = b.n   = 1;
    a.nMonitors = 8;
    for (uint32_t i = 0; i < 8; ++i) {
        a.monitors[i].id = i + 1;
        a.monitors[i].n  = 10 + i;
    }
    b.nMonitors      = 2;
    b.monitors[0].id = 3; // merges into an existing id
    b.monitors[0].n  = 5;
    b.monitors[1].id = 99; // a ninth: the smallest (id 1, n 10) loses to n 50
    b.monitors[1].n  = 50;
    xrHistMerge(a, b);
    EXPECT_EQ(a.monitorsDropped, 1);
    EXPECT_EQ(a.nMonitors, 8);
    bool has99 = false, has1 = false;
    for (size_t i = 0; i < a.nMonitors; ++i) {
        has99 |= a.monitors[i].id == 99;
        has1 |= a.monitors[i].id == 1;
        if (a.monitors[i].id == 3)
            EXPECT_EQ(a.monitors[i].n, 12u + 5u);
    }
    EXPECT_TRUE(has99);
    EXPECT_FALSE(has1);
}

TEST(DonOffHistory, GenChangeClosesBucketsEarlyAndNeverMixesFrames) {
    STrace tr;
    tr.hold(Vec3{0.F, 1.6F, 0.F}, 0.F, 0.F, 1.1F);
    tr.gen = 2;
    tr.hold(Vec3{5.F, 1.6F, 5.F}, 1.F, 0.F, 1.1F);
    tr.h->endSession(tr.tNs);
    const auto& t1       = tr.h->tier(0);
    bool        sawEarly = false;
    for (size_t i = 0; i < t1.size(); ++i) {
        const auto& b = t1.at(i);
        // a bucket's AABB never spans both positions
        EXPECT_LT(b.posMax.x - b.posMin.x, 1.F);
        if (b.closedEarly == 1)
            sawEarly = true;
    }
    EXPECT_TRUE(sawEarly);
    // and the T2 bucket of gen 1 is separate from gen 2's
    const auto& t2 = tr.h->tier(1);
    ASSERT_GE(t2.size(), 2u);
    EXPECT_NE(t2.at(0).gen, t2.at(t2.size() - 1).gen);
}

TEST(DonOffHistory, EpisodeOpensAfterDwellMinAndClosesOnWalk) {
    STrace tr;
    tr.hold(Vec3{0.F, 1.6F, 0.F}, 0.F, 0.F, 5.F);
    EXPECT_FALSE(tr.h->runningEpisodeOpen());
    tr.hold(Vec3{0.F, 1.6F, 0.F}, 0.F, 0.F, 5.F);
    EXPECT_TRUE(tr.h->runningEpisodeOpen());
    EXPECT_TRUE(tr.h->t0().back(0).flags & XR_HF_IN_DWELL);
    // walk 3 m away and stay: closes as a walk after 1.5 s outside
    tr.move(Vec3{0.F, 1.6F, 0.F}, 0.F, 0.F, Vec3{3.F, 1.6F, 0.F}, 0.F, 0.F, 1.F);
    tr.hold(Vec3{3.F, 1.6F, 0.F}, 0.F, 0.F, 2.F);
    ASSERT_EQ(tr.h->episodes().size(), 1u);
    const auto& ep = tr.h->episodes().back(0);
    EXPECT_EQ(ep.endReason, XR_EP_WALK);
    EXPECT_NEAR(ep.seatPos.x, 0.F, 0.05F);
    EXPECT_NEAR(ep.seatYaw, 0.F, 1.F * DEG);
    EXPECT_GT(ep.yawConcentration, 0.99F);
    EXPECT_EQ(ep.dof, 6);
}

TEST(DonOffHistory, EpisodeRecordsTheArrangementItClosedOn) {
    STrace tr;
    tr.nMon        = 1;
    tr.mons[0].id  = 7;
    tr.mons[0].pos = Vec3{0.F, 1.5F, -1.5F};
    tr.hold(Vec3{0.F, 1.6F, 0.F}, 0.F, 0.F, 10.F);
    tr.h->endSession(tr.tNs);
    ASSERT_EQ(tr.h->episodes().size(), 1u);
    const auto& ep = tr.h->episodes().back(0);
    EXPECT_EQ(ep.endReason, XR_EP_SESSION);
    ASSERT_EQ(ep.nMonitors, 1);
    EXPECT_EQ(ep.arrangement[0].id, 7u);
}

TEST(DonOffHistory, StillnessWeightingFavoursRestingGaze) {
    // half the dwell resting at yaw 0, half sweeping fast between 0 and 60°: the unweighted mean
    // would sit near 15°, the stillness-weighted one stays near the rest.
    STrace tr;
    for (int rep = 0; rep < 4; ++rep) {
        tr.hold(Vec3{0.F, 1.6F, 0.F}, 0.F, 0.F, 3.F);
        tr.move(Vec3{0.F, 1.6F, 0.F}, 0.F, 0.F, Vec3{0.F, 1.6F, 0.F}, 60.F * DEG, 0.F, 0.75F);
        tr.move(Vec3{0.F, 1.6F, 0.F}, 60.F * DEG, 0.F, Vec3{0.F, 1.6F, 0.F}, 0.F, 0.F, 0.75F);
    }
    tr.h->endSession(tr.tNs);
    ASSERT_EQ(tr.h->episodes().size(), 1u);
    const auto& ep = tr.h->episodes().back(0);
    EXPECT_LT(std::fabs(ep.seatYaw), 6.F * DEG);
}

TEST(DonOffFeatures, UprightTurnsFalseBeforeThePitchReachesVertical) {
    // R2 regression: sweep pitch 0 → 120°. No sample past upY < 0.5 may claim to be upright, and every
    // non-upright one has a yaw that would be mirrored if anyone read it.
    SXRHeadFeatures f;
    SXRPose         prev{Vec3{0.F, 1.6F, 0.F}, headRot(0.F, 0.F)};
    bool            sawFlipped = false;
    for (int i = 0; i <= 120; ++i) {
        const SXRPose cur{Vec3{0.F, 1.6F, 0.F}, headRot(0.F, static_cast<float>(i) * DEG)};
        f = xrHeadFeatures(prev, cur, 1.F / 90.F, f, true, i > 0);
        if (i < 60)
            EXPECT_TRUE(f.upright) << i;
        if (i > 60)
            EXPECT_FALSE(f.upright) << i;
        if (!f.upright && std::fabs(qYawOf(cur.rot, 0.F)) > 170.F * DEG)
            sawFlipped = true;
        EXPECT_EQ(f.upright, xrHeadUpright(cur.rot));
        prev = cur;
    }
    EXPECT_TRUE(sawFlipped); // the hazard is real, and only non-upright samples carry it
}

TEST(DonOffFeatures, DeadStillOnlyWhenTrulyMotionless) {
    SXRHeadFeatures f;
    SXRPose         p{Vec3{0.F, 1.F, 0.F}, Quat{}};
    for (int i = 0; i < 300; ++i) {
        f = xrHeadFeatures(p, p, 1.F / 90.F, f, true, i > 0);
    }
    EXPECT_TRUE(f.deadStill);
    STrace tr; // the tremor in hold() keeps a worn head above the floor
    tr.hold(Vec3{0.F, 1.6F, 0.F}, 0.F, 0.F, 4.F);
    EXPECT_FALSE(tr.f.deadStill);
    EXPECT_TRUE(tr.f.still);
}
