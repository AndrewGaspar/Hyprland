#include <openxr/XRMath.hpp>
#include <openxr/XRAnchor.hpp>

#include <gtest/gtest.h>

#include <cmath>

using namespace OpenXR;

// tests/xr/grab_ring.cpp — WP-G4 release-latching ring + velocity-rejection pure math
// (research 04-grabbable-borders.md §4/§5.4). Compiles and runs with no OpenXR runtime present.

namespace {
    void expectVecNear(const Vec3& a, const Vec3& b, float tol = 1e-4f) {
        EXPECT_NEAR(a.x, b.x, tol);
        EXPECT_NEAR(a.y, b.y, tol);
        EXPECT_NEAR(a.z, b.z, tol);
    }

    SXRPose poseAt(float x, float y = 0.f, float z = 0.f) {
        return SXRPose{Vec3{x, y, z}, Quat{0.f, 0.f, 0.f, 1.f}};
    }

    // Push a steady 90 Hz (~11 ms) carry: pos.x ramps from x0 by dx per frame, `n` frames,
    // starting at time t0. Returns the last timestamp pushed.
    uint32_t pushSteady(SXRGrabRing& r, float x0, float dx, int n, uint32_t t0, uint32_t dtMs = 11) {
        uint32_t t = t0;
        for (int i = 0; i < n; ++i) {
            r.push(poseAt(x0 + dx * (float)i), t);
            t += dtMs;
        }
        return t - dtMs;
    }
}

// ---- ring bookkeeping ----------------------------------------------------------------------

TEST(GrabRing, SizeAndWrap) {
    SXRGrabRing r;
    EXPECT_EQ(r.size(), 0u);
    for (int i = 0; i < 10; ++i)
        r.push(poseAt((float)i), (uint32_t)(i * 11));
    EXPECT_EQ(r.size(), 10u);
    EXPECT_NEAR(r.at(0).world.pos.x, 9.f, 1e-5f); // newest
    EXPECT_NEAR(r.at(9).world.pos.x, 0.f, 1e-5f); // oldest

    // Overflow past CAP: size saturates, oldest drops off, newest is correct.
    SXRGrabRing r2;
    const int   N = SXRGrabRing::CAP + 50;
    for (int i = 0; i < N; ++i)
        r2.push(poseAt((float)i), (uint32_t)(i * 11));
    EXPECT_EQ(r2.size(), SXRGrabRing::CAP);
    EXPECT_NEAR(r2.at(0).world.pos.x, (float)(N - 1), 1e-3f);
    EXPECT_NEAR(r2.at(SXRGrabRing::CAP - 1).world.pos.x, (float)(N - (int)SXRGrabRing::CAP), 1e-3f);

    r.reset();
    EXPECT_EQ(r.size(), 0u);
}

TEST(GrabRing, PushComputesSpeed) {
    SXRGrabRing r;
    r.push(poseAt(0.f), 0);
    EXPECT_NEAR(r.at(0).linSpeed, 0.f, 1e-6f); // first sample has no predecessor
    r.push(poseAt(0.1f), 100);                 // 0.1 m over 0.1 s = 1 m/s
    EXPECT_NEAR(r.at(0).linSpeed, 1.f, 1e-4f);
    r.push(poseAt(0.1f), 200); // no motion
    EXPECT_NEAR(r.at(0).linSpeed, 0.f, 1e-6f);
}

// ---- sampleBack interpolation --------------------------------------------------------------

TEST(GrabRing, SampleBackInterpolates) {
    SXRGrabRing r;
    // Samples at t=0,100,200,300 ms; x = 0,1,2,3.
    for (int i = 0; i < 4; ++i)
        r.push(poseAt((float)i), (uint32_t)(i * 100));

    // now=300, latency=150 -> target=150ms -> halfway between x=1(t100) and x=2(t200) = 1.5.
    expectVecNear(r.sampleBack(300, 150).pos, Vec3{1.5f, 0.f, 0.f});
    // exact sample time
    expectVecNear(r.sampleBack(300, 100).pos, Vec3{2.f, 0.f, 0.f});
    // latency 0 -> newest
    expectVecNear(r.sampleBack(300, 0).pos, Vec3{3.f, 0.f, 0.f});
    // latency beyond history -> clamps to oldest
    expectVecNear(r.sampleBack(300, 100000).pos, Vec3{0.f, 0.f, 0.f});
}

// ---- lastCalm picks the pre-jerk sample ----------------------------------------------------

TEST(GrabRing, LastCalmSkipsTheJerk) {
    SXRGrabRing r;
    // 8 steady frames (dx = 0.001 m/frame @ 11ms -> ~0.09 m/s, calm), ending at x=0.007.
    uint32_t t = pushSteady(r, 0.f, 0.001f, 8, 0);
    const float calmX = r.at(0).world.pos.x;
    // Then a sharp 0.2 m jerk over the next frame (11ms -> ~18 m/s).
    t += 11;
    r.push(poseAt(calmX + 0.2f), t);

    // Newest sample is the jerk; lastCalm(thresh between calm and jerk) must return the pre-jerk pose.
    const uint32_t now = t;
    SXRPose        cp  = r.lastCalm(0.6f, now, XR_GRAB_MAX_REWIND_MS);
    EXPECT_NEAR(cp.pos.x, calmX, 1e-4f); // NOT calmX + 0.2

    // If the newest is itself calm, lastCalm returns it (no rewind).
    r.push(poseAt(calmX + 0.2f + 0.001f), t + 11); // small step from the jerked pose = calm again
    SXRPose cp2 = r.lastCalm(0.6f, t + 11, XR_GRAB_MAX_REWIND_MS);
    EXPECT_NEAR(cp2.pos.x, calmX + 0.2f + 0.001f, 1e-4f);
}

TEST(GrabRing, LastCalmWindowBound) {
    SXRGrabRing r;
    // Entire history is a fast throw; lastCalm must not rewind past the window and returns the
    // furthest-back in-window sample rather than something older.
    uint32_t t = 0;
    for (int i = 0; i < 20; ++i) {
        r.push(poseAt(0.05f * (float)i), t); // 0.05 m / 11 ms ~ 4.5 m/s, all "fast"
        t += 11;
    }
    const uint32_t now = t - 11;
    // window 33 ms back from now -> only the last ~3 samples are in-window.
    SXRPose cp = r.lastCalm(0.6f, now, 33);
    // furthest-back in-window is ~33ms before newest = 3 frames back.
    EXPECT_LT(cp.pos.x, r.at(0).world.pos.x); // rewound at least a bit
    EXPECT_GE(cp.pos.x, r.at(0).world.pos.x - 0.05f * 4.f);
}

// ---- release-window / carry statistics (the relative-rejection primitives) -----------------

TEST(GrabRing, ReleasePeakAndCarryTypical) {
    // 30 calm frames at ~0.18 m/s (dx 0.002 @ 11ms), then a 0.3 m jerk (~27 m/s) on the next frame.
    SXRGrabRing r;
    uint32_t    t = pushSteady(r, 0.f, 0.002f, 30, 0);
    t += 11;
    const float calmX = 0.002f * 29.f;
    r.push(poseAt(calmX + 0.3f), t);
    const uint32_t now = t;

    // Peak over the 80 ms release window is the jerk; the typical carry pace (older than 80 ms;
    // lower-trimmed mean = mean of the faster half) is ~0.18 for a uniform calm carry.
    EXPECT_GT(r.releasePeakSpeed(now, XR_GRAB_RELEASE_WINDOW_MS), 20.f);
    EXPECT_NEAR(r.carryTypicalSpeed(now, XR_GRAB_RELEASE_WINDOW_MS, XR_GRAB_MAX_REWIND_MS), 0.1818f, 0.02f);

    // A uniformly fast carry: peak ≈ typical carry (both ~2.7 m/s), so the ratio is ~1.
    SXRGrabRing f;
    pushSteady(f, 0.f, 0.03f, 40, 0);
    const uint32_t fnow = 39 * 11;
    const float    peak = f.releasePeakSpeed(fnow, XR_GRAB_RELEASE_WINDOW_MS);
    const float    typ  = f.carryTypicalSpeed(fnow, XR_GRAB_RELEASE_WINDOW_MS, XR_GRAB_MAX_REWIND_MS);
    EXPECT_NEAR(peak, 2.727f, 0.1f);
    EXPECT_NEAR(typ, 2.727f, 0.1f);
    EXPECT_LT(peak / typ, 1.5f); // nowhere near a K=3 outlier

    // Rest-then-flick: stationary just-grabbed samples must NOT drag the typical pace to ~0 (the
    // median failure mode that motivated the trimmed mean). 18 still frames then 12 flick frames
    // at ~2.7 m/s: the faster-half mean blends the flick with a few zeros but stays comfortably
    // within a factor K=3 of the flick pace.
    SXRGrabRing rf;
    uint32_t    tt = pushSteady(rf, 0.f, 0.f, 18, 0); // at rest right after the grab
    tt             = pushSteady(rf, 0.03f, 0.03f, 12, tt + 11);
    const float rfPeak = rf.releasePeakSpeed(tt, XR_GRAB_RELEASE_WINDOW_MS);
    const float rfTyp  = rf.carryTypicalSpeed(tt, XR_GRAB_RELEASE_WINDOW_MS, XR_GRAB_MAX_REWIND_MS);
    EXPECT_GT(rfTyp, 0.5f);         // NOT ~0 (a median over 18 zeros + a few flick frames would be 0)
    EXPECT_LT(rfPeak / rfTyp, 3.f); // not an outlier at the default K
}

// ---- pickReleasePose truth table (RELATIVE velocity rejection) ------------------------------
//
// velRejectRatio is now a RATIO K; the denominator is the lower-trimmed mean (faster-half mean) of
// the carry speeds, floored at XR_GRAB_CARRY_SPEED_FLOOR. Truth table:
//   carry                 release        ratio vs K=3   outlier?  result
//   calm  (0.18)          jerk  (27)     ~150           yes       lastCalm -> pre-jerk
//   still (0, floored)    jerk  (27)     ~540           yes       lastCalm -> pre-jerk
//   fast  (2.7)           fast  (2.7)    ~1             no        sampleBack (latency) — kept
//   rest then fast flick  fast  (2.7)    <3             no        sampleBack (latency) — kept
//   calm  (0.18)          calm  (0.18)   ~1             no        sampleBack (latency)
//   any                   any            K=0 (disabled) —         sampleBack (latency)

TEST(GrabRing, PickReleaseRelativeOutlier) {
    // Calm carry (0.18 m/s) then a 0.3 m jerk -> a clear outlier that must rewind past the jerk.
    SXRGrabRing r;
    uint32_t    t     = pushSteady(r, 0.f, 0.002f, 30, 0);
    const float calmX = r.at(0).world.pos.x;
    t += 11;
    r.push(poseAt(calmX + 0.3f), t);
    const uint32_t now = t;

    // (1) K disabled (0): pure latency rewind. latency 0 -> newest (the jerk).
    expectVecNear(pickReleasePose(r, now, 0, 0.f).pos, Vec3{calmX + 0.3f, 0.f, 0.f});
    // (2) K disabled, latency 33 ms rewinds a few frames past the 11 ms jerk -> pre-jerk region.
    EXPECT_LT(pickReleasePose(r, now, 33, 0.f).pos.x, calmX + 0.1f);
    // (3) K=3 and the jerk is a ~150x outlier: lastCalm -> pre-jerk pose, regardless of latency.
    EXPECT_NEAR(pickReleasePose(r, now, 0, 3.f).pos.x, calmX, 1e-2f);
    EXPECT_LT(pickReleasePose(r, now, 0, 3.f).pos.x, calmX + 0.1f); // NOT the jerk
    // (4) K enormous (1000): the jerk is no longer a relative outlier -> latency path (0 -> jerk).
    expectVecNear(pickReleasePose(r, now, 0, 1000.f).pos, Vec3{calmX + 0.3f, 0.f, 0.f});

    // Empty ring -> identity (caller falls back to release-frame endGrab).
    SXRGrabRing empty;
    expectVecNear(pickReleasePose(empty, 100, 50, 3.f).pos, Vec3{0.f, 0.f, 0.f});
}

TEST(GrabRing, PickReleaseStillPanelFistOpen) {
    // A perfectly still carry (0 m/s) then a jerk: typical carry speed is 0, floored to 0.05, so the jerk
    // is still an outlier and rewinds to the last still (pre-jerk) sample.
    SXRGrabRing r;
    pushSteady(r, 0.5f, 0.f, 30, 0); // held still at x=0.5
    uint32_t t = 29 * 11 + 11;
    r.push(poseAt(0.5f + 0.3f), t); // 0.3 m jerk
    const SXRPose got = pickReleasePose(r, t, 0, 3.f);
    EXPECT_NEAR(got.pos.x, 0.5f, 1e-3f); // pre-jerk still pose, not 0.8
}

TEST(GrabRing, PickReleaseUniformFastNoRewind) {
    // A deliberate fast flick carried and released at a uniform ~2.7 m/s. The release window speed
    // matches the carry pace (ratio ~1), so it must NOT rewind — it takes the latency path and lands
    // near where the hand let go, NOT walked back toward the grab start.
    SXRGrabRing r;
    pushSteady(r, 0.f, 0.03f, 40, 0);
    const uint32_t now     = 39 * 11;
    const SXRPose  got     = pickReleasePose(r, now, 100, 3.f);
    const SXRPose  latency = r.sampleBack(now, 100);
    expectVecNear(got.pos, latency.pos);           // took the latency path
    EXPECT_GT(got.pos.x, 0.03f * 40.f * 0.6f);     // landed near the end, not rewound to the start
}

TEST(GrabRing, PickReleaseRestThenFlickNoRewind) {
    // Grab at rest, then one continuous fast flick released while still moving — the realistic
    // "deliberate fast move" (nobody carries at constant speed from t=0). The stationary samples
    // stay in the 500 ms carry window; a plain-median denominator would be ~0 and misfire a rewind
    // back toward the grab point. The trimmed-mean denominator keeps the flick.
    SXRGrabRing r;
    uint32_t    t = pushSteady(r, 0.f, 0.f, 18, 0);          // ~200 ms at rest after the grab
    t             = pushSteady(r, 0.03f, 0.03f, 12, t + 11); // ~130 ms flick at ~2.7 m/s
    const SXRPose got     = pickReleasePose(r, t, 100, 3.f);
    const SXRPose latency = r.sampleBack(t, 100);
    expectVecNear(got.pos, latency.pos); // took the latency path (no outlier rewind)
    EXPECT_GT(got.pos.x, 0.05f);         // landed into the flick, not back at the rest pose (x=0)
}

TEST(GrabRing, PickReleaseCalmReleaseNoRewind) {
    // A wholly calm carry + calm release: K must NOT trip; result equals the latency sample.
    SXRGrabRing r;
    pushSteady(r, 0.f, 0.002f, 40, 0);
    const uint32_t now     = 39 * 11;
    const SXRPose  latched = pickReleasePose(r, now, 100, 3.f);
    const SXRPose  ref     = r.sampleBack(now, 100);
    expectVecNear(latched.pos, ref.pos);
}

// ---- endGrab(world,...) overload equals the release-frame endGrab per anchor mode ----------

namespace {
    SXRAnchorTuning tuning() {
        SXRAnchorTuning t;
        t.leashResponse    = 0.35f;
        t.deadzoneAngleRad = 0.2618f;
        t.deadzoneDistance = 0.25f;
        t.defaultDistance  = 1.5f;
        return t;
    }

    SXRSolveInput solveInput(const SXRPose& grip) {
        SXRSolveInput in;
        in.view      = SXRPose{Vec3{0.f, 1.6f, 0.f}, Quat{0.f, 0.f, 0.f, 1.f}};
        in.dt        = 1.f / 90.f;
        in.gripLeft  = grip;
        in.gripRight = grip;
        in.pxW       = 1600;
        in.pxH       = 900;
        return in;
    }
}

// The new endGrab(releaseWorld,...) with releaseWorld = grip ∘ offset must produce byte-identical
// persistent state to the legacy release-frame endGrab(in,...) — proving the overload is a pure
// generalization for EVERY anchor mode.
TEST(GrabEndOverload, EqualsReleaseFrameForEachMode) {
    const eXRAnchorMode modes[] = {XR_ANCHOR_LOCAL, XR_ANCHOR_HEAD, XR_ANCHOR_BODY, XR_ANCHOR_DEVICE};
    for (auto mode : modes) {
        SXRAnchorState st;
        st.mode        = mode;
        st.device      = XR_HAND_LEFT;
        st.anchorPose  = SXRPose{Vec3{0.3f, 1.4f, -1.5f}, Quat{0.f, 0.f, 0.f, 1.f}};
        st.widthMeters = 1.6f;

        const SXRPose grip = SXRPose{Vec3{0.1f, 1.2f, -0.4f}, qNormalize(Quat{0.f, 0.2f, 0.f, 1.f})};
        const auto    in   = solveInput(grip);
        const auto    tune = tuning();

        // Anchor A: legacy path.
        CXRAnchor a;
        a.initFromState(st);
        a.solve(in, tune); // seed lastWorld
        a.beginGrab(XR_HAND_LEFT, grip);
        a.solve(in, tune); // carry frame -> lastWorld = grip ∘ offset
        const SXRPose W = a.lastWorld();
        a.endGrab(in, tune);

        // Anchor B: explicit-world overload with the same world pose.
        CXRAnchor b;
        b.initFromState(st);
        b.solve(in, tune);
        b.beginGrab(XR_HAND_LEFT, grip);
        b.solve(in, tune);
        b.endGrab(W, in, tune);

        EXPECT_TRUE(a.state() == b.state()) << "mode " << (int)mode;
        expectVecNear(a.lastWorld().pos, b.lastWorld().pos, 1e-4f);
    }
}

// A latched (pre-jerk) release must leave the LOCAL anchor at the pre-jerk world pose, not the
// jerked one — the end-to-end WP-G4 guarantee at the math layer.
TEST(GrabEndOverload, LatchedLocalReleaseLandsPreJerk) {
    SXRAnchorState st;
    st.mode       = XR_ANCHOR_LOCAL;
    st.anchorPose = SXRPose{Vec3{0.f, 1.4f, -1.5f}, Quat{0.f, 0.f, 0.f, 1.f}};

    CXRAnchor anchor;
    anchor.initFromState(st);
    const SXRPose grip = SXRPose{Vec3{0.f, 1.2f, -0.5f}, Quat{0.f, 0.f, 0.f, 1.f}};
    const auto    in   = solveInput(grip);
    const auto    tune = tuning();
    anchor.solve(in, tune);
    anchor.beginGrab(XR_HAND_LEFT, grip);
    anchor.solve(in, tune);

    // Simulate the carry ring: 20 calm frames (~0.45 m/s) near the current world pose, then a jerk.
    // The span exceeds the 80 ms release window so the carry statistic is well-formed.
    const SXRPose base = anchor.lastWorld();
    SXRGrabRing   ring;
    uint32_t      t = 0;
    for (int i = 0; i < 20; ++i) {
        ring.push(SXRPose{Vec3{base.pos.x + 0.005f * i, base.pos.y, base.pos.z}, base.rot}, t);
        t += 11;
    }
    const SXRPose preJerk = ring.at(0).world;
    t += 11;
    ring.push(SXRPose{Vec3{preJerk.pos.x + 0.2f, base.pos.y, base.pos.z}, base.rot}, t); // jerk

    const SXRPose releaseWorld = pickReleasePose(ring, t, 0, 3.f); // relative outlier trips -> pre-jerk
    anchor.endGrab(releaseWorld, in, tune);

    EXPECT_NEAR(anchor.state().anchorPose.pos.x, preJerk.pos.x, 1e-3f);      // latched
    EXPECT_GT(std::fabs(anchor.state().anchorPose.pos.x - (preJerk.pos.x + 0.2f)), 0.1f); // not jerked
}
