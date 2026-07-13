#include <openxr/XRMath.hpp>

#include <gtest/gtest.h>

#include <cstdlib>

using namespace OpenXR;

// tests/xr/pose_ring.cpp — the timestamped head-pose / gaze history ring (hypxrvoice WP-V1,
// docs/openxr/research/VOICE-CONTROL.md). Pure POD + index/binary-search math: builds and runs
// with no OpenXR runtime, exactly like its pure-math siblings (gaze_select.cpp etc.).

namespace {
    SXRPoseSample mk(int64_t ts, int64_t gaze = -1) {
        SXRPoseSample s;
        s.timestampMs   = ts;
        s.gazeMonitorId = gaze;
        s.viewValid     = true;
        s.headPos       = Vec3{(float)ts, 0.F, 0.F};
        return s;
    }
} // namespace

TEST(PoseRing, EmptyReturnsFalseAndLeavesOutUntouched) {
    SXRPoseRing<8> ring;
    EXPECT_TRUE(ring.empty());
    EXPECT_EQ(ring.size(), 0u);

    SXRPoseSample out = mk(999, 7); // sentinel
    EXPECT_FALSE(poseRingNearest(ring, 100, out));
    EXPECT_EQ(out.timestampMs, 999); // untouched
    EXPECT_EQ(out.gazeMonitorId, 7);
}

TEST(PoseRing, PushSizeAndNewest) {
    SXRPoseRing<8> ring;
    for (int i = 0; i < 5; ++i)
        ring.push(mk(i * 10));
    EXPECT_FALSE(ring.empty());
    EXPECT_EQ(ring.size(), 5u);
    EXPECT_EQ(ring.newest().timestampMs, 40);
    EXPECT_EQ(ring.at(0).timestampMs, 0);   // oldest
    EXPECT_EQ(ring.at(4).timestampMs, 40);  // newest
}

TEST(PoseRing, OverflowKeepsMostRecentWindowInOrder) {
    SXRPoseRing<4> ring; // capacity 4
    for (int i = 0; i < 10; ++i)
        ring.push(mk(i * 10)); // pushed 0..90
    EXPECT_EQ(ring.size(), 4u);
    // Live window is the last 4: 60,70,80,90 oldest->newest.
    EXPECT_EQ(ring.at(0).timestampMs, 60);
    EXPECT_EQ(ring.at(1).timestampMs, 70);
    EXPECT_EQ(ring.at(2).timestampMs, 80);
    EXPECT_EQ(ring.at(3).timestampMs, 90);
    EXPECT_EQ(ring.newest().timestampMs, 90);
}

TEST(PoseRing, NearestExactHit) {
    SXRPoseRing<16> ring;
    for (int i = 0; i < 8; ++i)
        ring.push(mk(i * 100, i)); // ts 0,100,...,700 ; gaze == i
    SXRPoseSample out;
    ASSERT_TRUE(poseRingNearest(ring, 300, out));
    EXPECT_EQ(out.timestampMs, 300);
    EXPECT_EQ(out.gazeMonitorId, 3);
}

TEST(PoseRing, NearestInteriorRoundsToClosest) {
    SXRPoseRing<16> ring;
    for (int i = 0; i < 8; ++i)
        ring.push(mk(i * 100, i));
    SXRPoseSample out;
    // 320 -> closer to 300
    ASSERT_TRUE(poseRingNearest(ring, 320, out));
    EXPECT_EQ(out.timestampMs, 300);
    // 380 -> closer to 400
    ASSERT_TRUE(poseRingNearest(ring, 380, out));
    EXPECT_EQ(out.timestampMs, 400);
}

TEST(PoseRing, NearestTieResolvesToNewer) {
    SXRPoseRing<16> ring;
    ring.push(mk(100, 1));
    ring.push(mk(300, 3));
    SXRPoseSample out;
    ASSERT_TRUE(poseRingNearest(ring, 200, out)); // equidistant -> newer (300)
    EXPECT_EQ(out.timestampMs, 300);
    EXPECT_EQ(out.gazeMonitorId, 3);
}

TEST(PoseRing, ClampBelowOldest) {
    SXRPoseRing<16> ring;
    for (int i = 0; i < 5; ++i)
        ring.push(mk(100 + i * 10));
    SXRPoseSample out;
    ASSERT_TRUE(poseRingNearest(ring, 0, out)); // older than everything
    EXPECT_EQ(out.timestampMs, 100);            // clamped to oldest
}

TEST(PoseRing, ClampAboveNewest) {
    SXRPoseRing<16> ring;
    for (int i = 0; i < 5; ++i)
        ring.push(mk(100 + i * 10)); // newest 140
    SXRPoseSample out;
    ASSERT_TRUE(poseRingNearest(ring, 9999, out)); // newer than everything
    EXPECT_EQ(out.timestampMs, 140);               // clamped to newest
}

TEST(PoseRing, NearestWorksAcrossWraparound) {
    SXRPoseRing<4> ring;
    for (int i = 0; i < 10; ++i)
        ring.push(mk(i * 10)); // live window 60,70,80,90 spanning a wrap
    SXRPoseSample out;
    ASSERT_TRUE(poseRingNearest(ring, 74, out));
    EXPECT_EQ(out.timestampMs, 70);
    ASSERT_TRUE(poseRingNearest(ring, 76, out));
    EXPECT_EQ(out.timestampMs, 80);
    // clamp below the (now-evicted) start: 0 is older than the retained 60
    ASSERT_TRUE(poseRingNearest(ring, 0, out));
    EXPECT_EQ(out.timestampMs, 60);
}

TEST(PoseRing, SingleSampleAlwaysMatches) {
    SXRPoseRing<8> ring;
    ring.push(mk(500, 2));
    SXRPoseSample out;
    ASSERT_TRUE(poseRingNearest(ring, 0, out));
    EXPECT_EQ(out.timestampMs, 500);
    ASSERT_TRUE(poseRingNearest(ring, 500, out));
    EXPECT_EQ(out.timestampMs, 500);
    ASSERT_TRUE(poseRingNearest(ring, 100000, out));
    EXPECT_EQ(out.timestampMs, 500);
    EXPECT_EQ(out.gazeMonitorId, 2);
}

TEST(PoseRing, AgeSignConventionMatchesIpc) {
    // The IPC layer computes ageMs = requested - matched. A request BEHIND the newest sample
    // (voice latency: user spoke 1.5s ago) resolves to an older entry, so ageMs > 0.
    SXRPoseRing<512> ring;
    for (int i = 0; i < 200; ++i)
        ring.push(mk(1000 + i * 11)); // ~90Hz-ish spacing, newest = 1000 + 199*11 = 3189
    SXRPoseSample out;
    const int64_t requested = 1700; // ~1.5s before the newest
    ASSERT_TRUE(poseRingNearest(ring, requested, out));
    const int64_t age = requested - out.timestampMs;
    EXPECT_LE(std::abs((long long)age), 6); // within half a frame of the request
    EXPECT_LT(out.timestampMs, 3189);       // definitely an older sample, not the newest
}

TEST(PoseRing, CapacityIsFiveSecondsAtNinetyHz) {
    // Design intent: ~5s of history at 90Hz. 512 samples / 90Hz = 5.68s.
    SXRPoseRing<512> ring;
    for (int i = 0; i < 1000; ++i)
        ring.push(mk(i));
    EXPECT_EQ(ring.size(), 512u);
}
