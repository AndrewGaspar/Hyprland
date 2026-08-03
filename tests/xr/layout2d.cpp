#include <openxr/XRLayout2D.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <random>

using namespace OpenXR;

// tests/xr/layout2d.cpp — WP-S1, the pure projection that derives Hyprland's 2D monitor-layout
// plane from the XR quads' 3D poses (research/archive/12-spatial-2d-layout.md). Everything here is
// pure math: no runtime, no compositor, no session.
//
// The invariants that matter are not "the numbers are these numbers" but "the emitted layout is one
// Hyprland can actually use": edge-touching within STICKS (2 px), perpendicular overlap so
// directional focus resolves, no overlaps, integral coordinates, and deterministic output. Those
// are asserted structurally by the helpers below and then re-asserted on a randomized sweep, so a
// future change to the compaction cannot quietly emit a layout with an invisible wall in it.

namespace {
    constexpr int STICKS_PX = 2; // == macros.hpp STICKS(a, b): abs(a - b) < 2

    SXRLayout2DInput mk(const std::string& name, float x, float y, float z, int w = 1920, int h = 1080, bool follow = false) {
        SXRLayout2DInput m;
        m.name        = name;
        m.pose.pos    = Vec3{x, y, z};
        m.w           = w;
        m.h           = h;
        m.followFrame = follow;
        return m;
    }

    SXRLayout2DRef refAt(float ex = 0.f, float ey = 0.f, float ez = 0.f, float yawRad = 0.f) {
        SXRLayout2DRef r;
        r.eye   = Vec3{ex, ey, ez};
        r.yaw   = yawRad;
        r.valid = true;
        return r;
    }

    const SXRLayout2DSlot* slotOf(const SXRLayout2DResult& r, const std::string& name) {
        for (const auto& s : r.slots)
            if (s.name == name)
                return &s;
        return nullptr;
    }

    int overlap1D(int a0, int a1, int b0, int b1) {
        return std::max(0, std::min(a1, b1) - std::max(a0, b0));
    }

    // Every structural guarantee the header promises, checked in one place.
    void expectValidLayout(const SXRLayout2DResult& r, const SXRLayout2DConfig& cfg) {
        // (a) no two boxes overlap in area — Hyprland only WARNS about overlaps, it never fixes
        //     them, so the projection is solely responsible for not emitting one.
        for (size_t i = 0; i < r.slots.size(); ++i)
            for (size_t j = i + 1; j < r.slots.size(); ++j) {
                const auto& a = r.slots[i];
                const auto& b = r.slots[j];
                const int   ox = overlap1D(a.x, a.x + a.w, b.x, b.x + b.w);
                const int   oy = overlap1D(a.y, a.y + a.h, b.y, b.y + b.h);
                EXPECT_TRUE(ox == 0 || oy == 0) << a.name << " overlaps " << b.name;
            }

        // (b) horizontal neighbours (same row, consecutive columns) touch EXACTLY and share enough
        //     vertical range for CMonitorQueryCore::directionLookup to find each other.
        for (const auto& a : r.slots)
            for (const auto& b : r.slots) {
                if (a.row != b.row || b.col != a.col + 1)
                    continue;
                EXPECT_EQ(a.x + a.w, b.x) << a.name << " -> " << b.name << " must be flush";
                EXPECT_LT(std::abs((a.x + a.w) - b.x), STICKS_PX);
                const int need = std::min({cfg.minOverlapPx, a.h, b.h});
                EXPECT_GE(overlap1D(a.y, a.y + a.h, b.y, b.y + b.h), need) << a.name << " / " << b.name << " share too little vertical range";
            }

        // (c) the block is normalized to a (0, 0) origin and its bounds are consistent.
        if (!r.slots.empty()) {
            int minX = r.slots[0].x, minY = r.slots[0].y, maxR = 0, maxB = 0;
            for (const auto& s : r.slots) {
                minX = std::min(minX, s.x);
                minY = std::min(minY, s.y);
                maxR = std::max(maxR, s.x + s.w);
                maxB = std::max(maxB, s.y + s.h);
            }
            EXPECT_EQ(minX, 0);
            EXPECT_EQ(minY, 0);
            EXPECT_EQ(r.width, maxR);
            EXPECT_EQ(r.height, maxB);
        }
    }

    // Rows touch vertically and overlap horizontally (so `movefocus u/d` resolves between tiers).
    void expectRowsStick(const SXRLayout2DResult& r, const SXRLayout2DConfig& cfg) {
        for (int row = 0; row + 1 < r.rows; ++row) {
            int aT = INT32_MAX, aB = INT32_MIN, aL = INT32_MAX, aR = INT32_MIN;
            int bT = INT32_MAX, bL = INT32_MAX, bR = INT32_MIN;
            for (const auto& s : r.slots) {
                if (s.row == row) {
                    aT = std::min(aT, s.y);
                    aB = std::max(aB, s.y + s.h);
                    aL = std::min(aL, s.x);
                    aR = std::max(aR, s.x + s.w);
                } else if (s.row == row + 1) {
                    bT = std::min(bT, s.y);
                    bL = std::min(bL, s.x);
                    bR = std::max(bR, s.x + s.w);
                }
            }
            ASSERT_NE(aT, INT32_MAX);
            ASSERT_NE(bT, INT32_MAX);
            EXPECT_EQ(aB, bT) << "row " << row << " must touch row " << row + 1;
            const int need = std::min({cfg.minOverlapPx, aR - aL, bR - bL});
            EXPECT_GE(overlap1D(aL, aR, bL, bR), need) << "rows " << row << "/" << row + 1 << " share too little horizontal range";
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Config enum round-trip (openxr:layout2d:vertical / :attach)
// ---------------------------------------------------------------------------------------------

// The two string knobs round-trip, and an unknown value is rejected so the caller can fall back to
// the documented default instead of silently picking whichever enumerator happens to be 0.
TEST(Layout2D, ConfigEnumsRoundTrip) {
    EXPECT_STREQ(xrLayout2DVerticalName(XR_L2D_VERT_ELEVATION), "elevation");
    EXPECT_STREQ(xrLayout2DVerticalName(XR_L2D_VERT_WORLD_HEIGHT), "world_height");
    EXPECT_STREQ(xrLayout2DAttachName(XR_L2D_ATTACH_RIGHT), "right");
    EXPECT_STREQ(xrLayout2DAttachName(XR_L2D_ATTACH_AROUND), "around");

    EXPECT_EQ(xrParseLayout2DVertical("elevation"), XR_L2D_VERT_ELEVATION);
    EXPECT_EQ(xrParseLayout2DVertical("world_height"), XR_L2D_VERT_WORLD_HEIGHT);
    EXPECT_FALSE(xrParseLayout2DVertical("Elevation").has_value());
    EXPECT_FALSE(xrParseLayout2DVertical("").has_value());
    EXPECT_FALSE(xrParseLayout2DVertical("nonsense").has_value());

    EXPECT_EQ(xrParseLayout2DAttach("right"), XR_L2D_ATTACH_RIGHT);
    EXPECT_EQ(xrParseLayout2DAttach("around"), XR_L2D_ATTACH_AROUND);
    EXPECT_FALSE(xrParseLayout2DAttach("follow-primary").has_value()); // §2.5 deferred, not silently accepted
    EXPECT_FALSE(xrParseLayout2DAttach("left").has_value());
}

// ---------------------------------------------------------------------------------------------
// The angular unwrap (§2.2)
// ---------------------------------------------------------------------------------------------

// The wrap is a true (-180, 180] fold: exact at both ends, idempotent, and never NaN.
TEST(Layout2D, WrapDegIsHalfOpenAt180) {
    EXPECT_FLOAT_EQ(xrWrapDeg180(0.f), 0.f);
    EXPECT_FLOAT_EQ(xrWrapDeg180(180.f), 180.f);   // closed end
    EXPECT_FLOAT_EQ(xrWrapDeg180(-180.f), 180.f);  // open end folds to the closed one
    EXPECT_FLOAT_EQ(xrWrapDeg180(190.f), -170.f);
    EXPECT_FLOAT_EQ(xrWrapDeg180(-190.f), 170.f);
    EXPECT_FLOAT_EQ(xrWrapDeg180(360.f), 0.f);
    EXPECT_FLOAT_EQ(xrWrapDeg180(720.f + 30.f), 30.f);
    EXPECT_FLOAT_EQ(xrWrapDeg180(-720.f - 30.f), -30.f);
    EXPECT_EQ(xrWrapDeg180(std::nanf("")), 0.f);
    EXPECT_EQ(xrWrapDeg180(std::numeric_limits<float>::infinity()), 0.f);

    for (float d = -1000.f; d <= 1000.f; d += 0.37f) {
        const float w = xrWrapDeg180(d);
        EXPECT_GT(w, -180.f - 1e-3f);
        EXPECT_LE(w, 180.f + 1e-3f);
        EXPECT_NEAR(std::fmod(std::fabs(w - d), 360.f), 0.f, 1e-2f);
    }
}

// Azimuth is right-positive: a monitor at +X is to your right when you face -Z, and that becomes a
// positive layout x. (This is the sign convention qYawOf() does NOT use — see the header.)
TEST(Layout2D, AzimuthIsRightPositive) {
    SXRLayout2DConfig cfg;
    float             az = 0, el = 0, v = 0;

    xrLayout2DAngles(mk("a", 0.f, 0.f, -2.f), refAt(), cfg, az, el, v);
    EXPECT_NEAR(az, 0.f, 1e-3f) << "straight ahead is azimuth 0";

    xrLayout2DAngles(mk("a", 2.f, 0.f, 0.f), refAt(), cfg, az, el, v);
    EXPECT_NEAR(az, 90.f, 1e-3f) << "+X is to the RIGHT";

    xrLayout2DAngles(mk("a", -2.f, 0.f, 0.f), refAt(), cfg, az, el, v);
    EXPECT_NEAR(az, -90.f, 1e-3f);
}

// Elevation is up-positive and the vertical layout coordinate is its DOWN-positive negation.
TEST(Layout2D, ElevationMapsToDownPositivePixels) {
    SXRLayout2DConfig cfg;
    cfg.pxPerDegree = 10.f;
    float az = 0, el = 0, v = 0;

    xrLayout2DAngles(mk("a", 0.f, 1.f, -1.f), refAt(), cfg, az, el, v);
    EXPECT_NEAR(el, 45.f, 1e-3f);
    EXPECT_NEAR(v, -450.f, 1e-2f) << "above eye level -> negative (higher) layout y";

    xrLayout2DAngles(mk("a", 0.f, -1.f, -1.f), refAt(), cfg, az, el, v);
    EXPECT_NEAR(el, -45.f, 1e-3f);
    EXPECT_NEAR(v, 450.f, 1e-2f);
}

// world_height mode swaps the vertical unit for metres, leaving azimuth untouched.
TEST(Layout2D, WorldHeightVerticalMode) {
    SXRLayout2DConfig cfg;
    cfg.vertical   = XR_L2D_VERT_WORLD_HEIGHT;
    cfg.pxPerMeter = 500.f;
    float az = 0, el = 0, v = 0;

    // Far away and high: elevation is small, but the metric height is not.
    xrLayout2DAngles(mk("a", 0.f, 0.8f, -8.f), refAt(), cfg, az, el, v);
    EXPECT_NEAR(az, 0.f, 1e-3f);
    EXPECT_NEAR(v, -400.f, 1e-2f);
    EXPECT_LT(std::fabs(el), 7.f);
}

// The reference yaw is what makes the layout independent of which way the user happens to face:
// rotating the reference frame by the same angle as the monitors leaves the angles unchanged.
TEST(Layout2D, ReferenceYawIsSubtracted) {
    SXRLayout2DConfig cfg;
    float             az = 0, el = 0, v = 0;

    // Monitor at +X (90 deg right of -Z). With the reference forward ALSO rotated to +X
    // (yaw = atan2(1, 0) = +pi/2 in the right-positive convention), it is straight ahead.
    xrLayout2DAngles(mk("a", 2.f, 0.f, 0.f), refAt(0, 0, 0, (float)M_PI_2), cfg, az, el, v);
    EXPECT_NEAR(az, 0.f, 1e-3f);

    // And the eye translates too.
    xrLayout2DAngles(mk("a", 3.f, 0.f, -1.f), refAt(1.f, 0.f, -1.f), cfg, az, el, v);
    EXPECT_NEAR(az, 90.f, 1e-3f);
}

// Behind the viewer: the seam of the unwrap sits directly behind the reference forward, and both
// sides of it resolve to a finite angle near +/-180 rather than to a NaN or a wrapped-around x.
TEST(Layout2D, BehindTheViewerWrapsAtTheSeam) {
    SXRLayout2DConfig cfg;
    float             az = 0, el = 0, v = 0;

    xrLayout2DAngles(mk("a", 0.f, 0.f, 2.f), refAt(), cfg, az, el, v); // directly behind
    EXPECT_NEAR(std::fabs(az), 180.f, 1e-3f);

    xrLayout2DAngles(mk("a", 0.2f, 0.f, 2.f), refAt(), cfg, az, el, v); // just right of behind
    EXPECT_GT(az, 170.f);
    EXPECT_LE(az, 180.f);

    xrLayout2DAngles(mk("a", -0.2f, 0.f, 2.f), refAt(), cfg, az, el, v); // just left of behind
    EXPECT_LT(az, -170.f);
    EXPECT_GT(az, -180.f);

    // A reference yaw that puts "behind" back in front is continuous through the seam.
    xrLayout2DAngles(mk("a", 0.f, 0.f, 2.f), refAt(0, 0, 0, (float)M_PI), cfg, az, el, v);
    EXPECT_NEAR(az, 0.f, 1e-3f);
}

// Degenerate poses must not produce NaN or an arbitrary atan2 branch.
TEST(Layout2D, DegeneratePosesAreFinite) {
    SXRLayout2DConfig cfg;
    float             az = 0, el = 0, v = 0;

    xrLayout2DAngles(mk("a", 0.f, 0.f, 0.f), refAt(), cfg, az, el, v); // exactly at the eye
    EXPECT_EQ(az, 0.f);
    EXPECT_EQ(el, 0.f);
    EXPECT_EQ(v, 0.f);

    xrLayout2DAngles(mk("a", 0.f, 2.f, 0.f), refAt(), cfg, az, el, v); // exactly overhead
    EXPECT_EQ(az, 0.f);
    EXPECT_NEAR(el, 90.f, 1e-3f);
    EXPECT_TRUE(std::isfinite(v));

    xrLayout2DAngles(mk("a", 0.f, -2.f, 0.f), refAt(), cfg, az, el, v); // exactly underfoot
    EXPECT_EQ(az, 0.f);
    EXPECT_NEAR(el, -90.f, 1e-3f);
}

// A head/body-anchored monitor is measured in its OWN follow frame — the reference is not applied,
// because its offset is already expressed about the frame origin and is constant by construction.
TEST(Layout2D, FollowFrameIgnoresTheReference) {
    SXRLayout2DConfig cfg;
    float             az = 0, el = 0, v = 0;

    // Same offset, wildly different reference: identical angles.
    const auto m = mk("hud", 1.f, 0.f, -1.f, 1920, 1080, /*follow=*/true);
    xrLayout2DAngles(m, refAt(0, 0, 0, 0.f), cfg, az, el, v);
    EXPECT_NEAR(az, 45.f, 1e-3f);
    float az2 = 0, el2 = 0, v2 = 0;
    xrLayout2DAngles(m, refAt(5.f, 3.f, -9.f, 2.1f), cfg, az2, el2, v2);
    EXPECT_FLOAT_EQ(az, az2);
    EXPECT_FLOAT_EQ(el, el2);
}

// ---------------------------------------------------------------------------------------------
// Compaction (§2.4) — the part that makes the layout usable at all
// ---------------------------------------------------------------------------------------------

// The headline case: two monitors side by side in 3D come out side by side in 2D, flush.
TEST(Layout2D, TwoSideBySideMonitorsStick) {
    SXRLayout2DConfig cfg;
    const std::vector<SXRLayout2DInput> mons{
        mk("left", -1.0f, 0.f, -1.5f),
        mk("right", 1.0f, 0.f, -1.5f),
    };
    const auto r = xrProjectLayout2D(mons, refAt(), cfg, {});
    ASSERT_EQ(r.slots.size(), 2u);
    expectValidLayout(r, cfg);

    const auto* L = slotOf(r, "left");
    const auto* R = slotOf(r, "right");
    ASSERT_TRUE(L && R);
    EXPECT_EQ(L->row, R->row) << "same elevation -> one row";
    EXPECT_LT(L->col, R->col);
    EXPECT_EQ(L->x + L->w, R->x) << "flush: no gap for the cursor to fall into";
    EXPECT_EQ(L->y, R->y);
    EXPECT_EQ(L->x, 0);
}

// The feature's actual ask: a monitor floating UP AND TO THE RIGHT lands right of, and above, its
// neighbour — while still sharing enough vertical range that `movefocus r` finds it.
TEST(Layout2D, UpperRightMonitorLandsUpperRight) {
    SXRLayout2DConfig cfg;
    cfg.minOverlapPx = 64;
    const std::vector<SXRLayout2DInput> mons{
        mk("main", 0.f, 0.f, -1.5f),
        mk("side", 1.2f, 0.25f, -1.5f), // right and a bit up (~5 deg, inside one row)
    };
    const auto r = xrProjectLayout2D(mons, refAt(), cfg, {});
    expectValidLayout(r, cfg);

    const auto* M = slotOf(r, "main");
    const auto* S = slotOf(r, "side");
    ASSERT_TRUE(M && S);
    EXPECT_EQ(M->row, S->row);
    EXPECT_EQ(M->x + M->w, S->x) << "to the right, flush";
    EXPECT_LT(S->y, M->y) << "and raised, which is the whole point";
    EXPECT_GE(overlap1D(M->y, M->y + M->h, S->y, S->y + S->h), cfg.minOverlapPx);
}

// A monitor high enough to clear row_merge_deg becomes its own tier, stacked above, and the tiers
// touch so vertical directional focus resolves.
TEST(Layout2D, HighMonitorFormsItsOwnRowAbove) {
    SXRLayout2DConfig cfg;
    cfg.rowMergeDeg = 10.f;
    const std::vector<SXRLayout2DInput> mons{
        mk("desk", 0.f, 0.f, -1.5f),
        mk("shelf", 0.f, 1.5f, -1.5f), // ~45 deg up: a separate tier
    };
    const auto r = xrProjectLayout2D(mons, refAt(), cfg, {});
    expectValidLayout(r, cfg);
    expectRowsStick(r, cfg);

    const auto* D = slotOf(r, "desk");
    const auto* S = slotOf(r, "shelf");
    ASSERT_TRUE(D && S);
    EXPECT_EQ(r.rows, 2);
    EXPECT_EQ(S->row, 0);
    EXPECT_EQ(D->row, 1);
    EXPECT_EQ(S->y + S->h, D->y) << "tiers must touch";
}

// The stagger inside a row is CLAMPED: a monitor 9 degrees higher than its neighbour would be
// staggered clean past it at 35 px/deg, which would break horizontal focus. It gets pulled back.
TEST(Layout2D, IntraRowStaggerIsClampedToKeepOverlap) {
    SXRLayout2DConfig cfg;
    cfg.rowMergeDeg  = 12.f;
    cfg.pxPerDegree  = 200.f; // absurdly large so the raw stagger far exceeds the monitor height
    cfg.minOverlapPx = 200;
    const std::vector<SXRLayout2DInput> mons{
        mk("a", -0.5f, 0.f, -1.5f, 1920, 1080),
        mk("b", 0.5f, 0.25f, -1.5f, 1920, 1080),
    };
    const auto r = xrProjectLayout2D(mons, refAt(), cfg, {});
    expectValidLayout(r, cfg);

    const auto* A = slotOf(r, "a");
    const auto* B = slotOf(r, "b");
    ASSERT_TRUE(A && B);
    EXPECT_EQ(A->row, B->row);
    EXPECT_GE(overlap1D(A->y, A->y + A->h, B->y, B->y + B->h), 200);
}

// Mismatched native sizes are preserved verbatim: the projection places by ANGLE and never rescales
// a monitor to match its angular subtense.
TEST(Layout2D, NativeSizesArePreserved) {
    SXRLayout2DConfig cfg;
    const std::vector<SXRLayout2DInput> mons{
        mk("uhd", -1.f, 0.f, -1.5f, 3840, 2160),
        mk("small", 1.f, 0.f, -1.5f, 1280, 720),
    };
    const auto r = xrProjectLayout2D(mons, refAt(), cfg, {});
    expectValidLayout(r, cfg);
    EXPECT_EQ(slotOf(r, "uhd")->w, 3840);
    EXPECT_EQ(slotOf(r, "uhd")->h, 2160);
    EXPECT_EQ(slotOf(r, "small")->w, 1280);
    EXPECT_EQ(slotOf(r, "small")->h, 720);
}

// Zero/negative sizes cannot produce a degenerate box (a 0-width monitor would make two neighbours
// share an edge with a third and confuse the adjacency lookup).
TEST(Layout2D, DegenerateSizesAreClamped) {
    SXRLayout2DConfig cfg;
    std::vector<SXRLayout2DInput> mons{mk("a", -1.f, 0.f, -1.5f, 0, 0), mk("b", 1.f, 0.f, -1.5f, -5, -5)};
    const auto                    r = xrProjectLayout2D(mons, refAt(), cfg, {});
    ASSERT_EQ(r.slots.size(), 2u);
    for (const auto& s : r.slots) {
        EXPECT_GE(s.w, 1);
        EXPECT_GE(s.h, 1);
    }
    expectValidLayout(r, cfg);
}

TEST(Layout2D, EmptyInputIsEmpty) {
    SXRLayout2DConfig cfg;
    const auto        r = xrProjectLayout2D({}, refAt(), cfg, {});
    EXPECT_TRUE(r.slots.empty());
    EXPECT_EQ(r.width, 0);
    EXPECT_EQ(r.height, 0);
    EXPECT_EQ(r.rows, 0);
}

TEST(Layout2D, SingleMonitorSitsAtTheOrigin) {
    SXRLayout2DConfig cfg;
    const auto        r = xrProjectLayout2D({mk("only", 3.f, 2.f, -1.f, 2560, 1440)}, refAt(), cfg, {});
    ASSERT_EQ(r.slots.size(), 1u);
    EXPECT_EQ(r.slots[0].x, 0);
    EXPECT_EQ(r.slots[0].y, 0);
    EXPECT_EQ(r.width, 2560);
    EXPECT_EQ(r.height, 1440);
    EXPECT_EQ(r.rows, 1);
}

// World-anchored and head-anchored monitors are merged onto ONE plane (§3): the HUD's follow-frame
// angle and the desk monitor's reference-frame angle are drawn in the same coordinates.
TEST(Layout2D, WorldAndFollowFrameMonitorsMergeOntoOnePlane) {
    SXRLayout2DConfig cfg;
    const std::vector<SXRLayout2DInput> mons{
        mk("desk", 0.f, 1.4f, -1.5f),                          // world, straight ahead of the eye
        mk("hud", 1.5f, 0.f, -1.5f, 1280, 720, /*follow=*/true), // head-anchored, to the right
    };
    const auto r = xrProjectLayout2D(mons, refAt(0.f, 1.4f, 0.f), cfg, {});
    expectValidLayout(r, cfg);
    const auto* D = slotOf(r, "desk");
    const auto* H = slotOf(r, "hud");
    ASSERT_TRUE(D && H);
    EXPECT_EQ(D->row, H->row);
    EXPECT_LT(D->col, H->col) << "the HUD is to the right of the desk monitor in both frames";
}

// ---------------------------------------------------------------------------------------------
// Hysteresis + determinism (§4)
// ---------------------------------------------------------------------------------------------

// A small bump must leave the layout BYTE-identical — the mouse mapping is a stable contract, not a
// live readout of where a quad drifted to.
TEST(Layout2D, SmallNudgeLeavesTheLayoutIdentical) {
    SXRLayout2DConfig cfg;
    cfg.reorderHysteresisDeg = 4.f;
    std::vector<SXRLayout2DInput> mons{mk("a", -1.f, 0.f, -1.5f), mk("b", 1.f, 0.f, -1.5f)};

    const auto first = xrProjectLayout2D(mons, refAt(), cfg, {});
    const auto prev  = xrLayout2DPrevOf(first);

    // Nudge "b" by ~1 degree of azimuth and a hair of elevation.
    mons[1].pose.pos = Vec3{1.03f, 0.02f, -1.5f};
    const auto again = xrProjectLayout2D(mons, refAt(), cfg, prev);

    ASSERT_EQ(first.slots.size(), again.slots.size());
    for (size_t i = 0; i < first.slots.size(); ++i) {
        EXPECT_EQ(first.slots[i].name, again.slots[i].name);
        EXPECT_EQ(first.slots[i].x, again.slots[i].x);
        EXPECT_EQ(first.slots[i].y, again.slots[i].y);
        EXPECT_EQ(first.slots[i].col, again.slots[i].col);
        EXPECT_EQ(first.slots[i].row, again.slots[i].row);
        EXPECT_FLOAT_EQ(first.slots[i].azDeg, again.slots[i].azDeg) << "the held angle is reported, not the new one";
    }
}

// ...but a real move DOES reorder: hysteresis is a dead band, not a freeze.
TEST(Layout2D, RealSwapReordersColumns) {
    SXRLayout2DConfig cfg;
    cfg.reorderHysteresisDeg = 4.f;
    std::vector<SXRLayout2DInput> mons{mk("a", -1.f, 0.f, -1.5f), mk("b", 1.f, 0.f, -1.5f)};

    const auto first = xrProjectLayout2D(mons, refAt(), cfg, {});
    EXPECT_LT(slotOf(first, "a")->col, slotOf(first, "b")->col);

    // Swap them in space.
    mons[0].pose.pos = Vec3{1.f, 0.f, -1.5f};
    mons[1].pose.pos = Vec3{-1.f, 0.f, -1.5f};
    const auto again = xrProjectLayout2D(mons, refAt(), cfg, xrLayout2DPrevOf(first));
    EXPECT_GT(slotOf(again, "a")->col, slotOf(again, "b")->col);
    expectValidLayout(again, cfg);
}

// The hold remembers the ADOPTED angle, so a monitor drifting by less than the margin every sync
// cannot accumulate unbounded error — it snaps once the total delta crosses the threshold.
TEST(Layout2D, HysteresisDoesNotAccumulateDrift) {
    SXRLayout2DConfig cfg;
    cfg.reorderHysteresisDeg = 4.f;

    std::vector<SXRLayout2DInput> mons{mk("a", 0.f, 0.f, -1.5f)};
    auto                          prev = std::vector<SXRLayout2DPrev>{};

    float                         trueAz = 0.f;
    for (int step = 0; step < 20; ++step) {
        // Move ~2 degrees per step: under the 4 degree margin individually, 40 degrees in total.
        trueAz += 2.f;
        const float rad  = trueAz / 57.29577951308232f;
        mons[0].pose.pos = Vec3{1.5f * std::sin(rad), 0.f, -1.5f * std::cos(rad)};
        const auto r     = xrProjectLayout2D(mons, refAt(), cfg, prev);
        prev             = xrLayout2DPrevOf(r);
        EXPECT_LT(std::fabs(r.slots[0].azDeg - trueAz), cfg.reorderHysteresisDeg + 1e-2f) << "step " << step << " drifted out of the dead band";
    }
}

// Prev entries for monitors that no longer exist are simply ignored; a new monitor with no prev is
// placed from its true angle.
TEST(Layout2D, StalePrevEntriesAreIgnored) {
    SXRLayout2DConfig                  cfg;
    const std::vector<SXRLayout2DPrev> prev{{"ghost", 170.f, 5000.f}, {"a", 0.f, 0.f}};
    const auto r = xrProjectLayout2D({mk("a", 0.f, 0.f, -1.5f), mk("fresh", 1.5f, 0.f, -1.5f)}, refAt(), cfg, prev);
    ASSERT_EQ(r.slots.size(), 2u);
    expectValidLayout(r, cfg);
    EXPECT_LT(slotOf(r, "a")->col, slotOf(r, "fresh")->col);
}

// Determinism: the same inputs always give the same layout, and shuffling the input order does not
// change any monitor's placement (only which index it comes back in).
TEST(Layout2D, DeterministicAndOrderIndependent) {
    SXRLayout2DConfig cfg;
    std::vector<SXRLayout2DInput> mons{
        mk("a", -1.5f, 0.1f, -1.5f, 1920, 1080), mk("b", 0.f, 0.f, -1.5f, 2560, 1440), mk("c", 1.5f, 0.05f, -1.5f, 1280, 720), mk("d", 0.f, 1.6f, -1.5f, 1920, 1080),
    };

    const auto ref1 = xrProjectLayout2D(mons, refAt(), cfg, {});
    const auto ref2 = xrProjectLayout2D(mons, refAt(), cfg, {});
    for (size_t i = 0; i < ref1.slots.size(); ++i) {
        EXPECT_EQ(ref1.slots[i].x, ref2.slots[i].x);
        EXPECT_EQ(ref1.slots[i].y, ref2.slots[i].y);
    }

    std::mt19937 rng(1234);
    for (int trial = 0; trial < 20; ++trial) {
        auto shuffled = mons;
        std::shuffle(shuffled.begin(), shuffled.end(), rng);
        const auto r = xrProjectLayout2D(shuffled, refAt(), cfg, {});
        ASSERT_EQ(r.slots.size(), ref1.slots.size());
        for (const auto& s : r.slots) {
            const auto* base = slotOf(ref1, s.name);
            ASSERT_TRUE(base);
            EXPECT_EQ(s.x, base->x) << s.name;
            EXPECT_EQ(s.y, base->y) << s.name;
            EXPECT_EQ(s.col, base->col) << s.name;
            EXPECT_EQ(s.row, base->row) << s.name;
        }
    }
}

// Identical poses (two quads stacked exactly on top of each other) still produce a valid,
// deterministic, non-overlapping layout — ties break by name.
TEST(Layout2D, IdenticalPosesTieBreakByName) {
    SXRLayout2DConfig cfg;
    const auto        r = xrProjectLayout2D({mk("zeta", 0.f, 0.f, -1.5f), mk("alpha", 0.f, 0.f, -1.5f)}, refAt(), cfg, {});
    expectValidLayout(r, cfg);
    EXPECT_LT(slotOf(r, "alpha")->col, slotOf(r, "zeta")->col);
}

// ---------------------------------------------------------------------------------------------
// Randomized sweep — the structural guarantees must hold for ANY arrangement, including the
// pathological ones (all behind you, all stacked vertically, wildly mixed sizes).
// ---------------------------------------------------------------------------------------------

TEST(Layout2D, RandomizedArrangementsAlwaysProduceAUsableLayout) {
    std::mt19937                          rng(20260803);
    std::uniform_real_distribution<float> ang(-3.14159f, 3.14159f);
    std::uniform_real_distribution<float> el(-1.4f, 1.4f);
    std::uniform_real_distribution<float> dist(0.3f, 6.f);
    std::uniform_int_distribution<int>    px(1, 4);
    std::uniform_int_distribution<int>    count(1, 9);
    std::uniform_int_distribution<int>    flag(0, 1);

    for (int trial = 0; trial < 400; ++trial) {
        SXRLayout2DConfig cfg;
        cfg.pxPerDegree          = (trial % 3 == 0) ? 8.f : (trial % 3 == 1 ? 35.f : 120.f);
        cfg.minOverlapPx         = (trial % 4) * 48;
        cfg.rowMergeDeg          = 2.f + (float)(trial % 7) * 4.f;
        cfg.reorderHysteresisDeg = (float)(trial % 5);
        cfg.vertical             = (trial % 5 == 0) ? XR_L2D_VERT_WORLD_HEIGHT : XR_L2D_VERT_ELEVATION;

        std::vector<SXRLayout2DInput> mons;
        const int                     n = count(rng);
        for (int i = 0; i < n; ++i) {
            const float a = ang(rng), e = el(rng), d = dist(rng);
            SXRLayout2DInput m;
            m.name        = "m" + std::to_string(i);
            m.pose.pos    = Vec3{d * std::cos(e) * std::sin(a), d * std::sin(e), -d * std::cos(e) * std::cos(a)};
            m.w           = 640 * px(rng);
            m.h           = 360 * px(rng);
            m.followFrame = flag(rng) == 1;
            mons.push_back(m);
        }

        const auto REF = refAt(0.f, 1.4f, 0.f, ang(rng));
        const auto r   = xrProjectLayout2D(mons, REF, cfg, {});
        ASSERT_EQ(r.slots.size(), mons.size()) << "trial " << trial;
        expectValidLayout(r, cfg);
        expectRowsStick(r, cfg);

        // Every monitor appears exactly once and keeps its native size.
        for (size_t i = 0; i < mons.size(); ++i) {
            EXPECT_EQ(r.slots[i].name, mons[i].name);
            EXPECT_EQ(r.slots[i].w, mons[i].w);
            EXPECT_EQ(r.slots[i].h, mons[i].h);
            EXPECT_TRUE(std::isfinite(r.slots[i].azDeg));
            EXPECT_TRUE(std::isfinite(r.slots[i].elDeg));
        }

        // Feeding the result back in as `prev` with unchanged poses must be a fixed point.
        const auto again = xrProjectLayout2D(mons, REF, cfg, xrLayout2DPrevOf(r));
        for (size_t i = 0; i < r.slots.size(); ++i) {
            EXPECT_EQ(r.slots[i].x, again.slots[i].x) << "trial " << trial << " monitor " << i;
            EXPECT_EQ(r.slots[i].y, again.slots[i].y) << "trial " << trial << " monitor " << i;
        }
    }
}
