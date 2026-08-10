#include <openxr/XRCursorCross.hpp>

#include <gtest/gtest.h>

#include <cmath>

using namespace OpenXR;

// tests/xr/cursor_cross.cpp — ray-cast cursor edge crossing (task #139, src/openxr/XRCursorCross.hpp).
// Pure math: no OpenXR runtime, no compositor, no config. These are the exact expressions
// COpenXRManager::redirectCursorCrossing runs, not a copy.
//
// Scene convention (shared with tests/xr/ray_intersect.cpp): the user stands at the LOCAL_FLOOR
// origin looking down -Z. An identity-rotation quad's local +X is world +X (right) and its local
// +Y is world +Y (up), so it faces the user when placed at negative Z.

namespace {
    constexpr float PI = 3.14159265358979323846f;

    // A 1.6 x 0.9 m quad centred `dist` metres ahead, offset `x` right and `y` up, facing the user.
    SXRCrossQuad quadAt(int64_t id, float x, float y, float dist) {
        SXRCrossQuad q;
        q.id      = id;
        q.pose    = SXRPose{Vec3{x, y, -dist}, Quat{}};
        q.wMeters = 1.6f;
        q.hMeters = 0.9f;
        return q;
    }

    // A 1920x1080 layout box at (x,y).
    CBox boxAt(double x, double y) {
        return CBox{x, y, 1920.0, 1080.0};
    }

    const Vec3 EYE{0.f, 0.f, 0.f};
}

// --- config parsing -----------------------------------------------------------------------------

TEST(XRCursorCross, ParseDefaultsToRaycast) {
    // Anything that is not exactly "layout" is the default feel — including junk, so a typo in the
    // config never silently disables the behaviour the user asked for.
    EXPECT_EQ(xrParseCursorCrossing("raycast"), XR_CURSORCROSS_RAYCAST);
    EXPECT_EQ(xrParseCursorCrossing(""), XR_CURSORCROSS_RAYCAST);
    EXPECT_EQ(xrParseCursorCrossing("Layout"), XR_CURSORCROSS_RAYCAST); // case-sensitive, like its siblings
    EXPECT_EQ(xrParseCursorCrossing("nonsense"), XR_CURSORCROSS_RAYCAST);
    EXPECT_EQ(xrParseCursorCrossing("layout"), XR_CURSORCROSS_LAYOUT);
}

TEST(XRCursorCross, ModeNamesRoundTrip) {
    EXPECT_STREQ(xrCursorCrossingName(XR_CURSORCROSS_RAYCAST), "raycast");
    EXPECT_STREQ(xrCursorCrossingName(XR_CURSORCROSS_LAYOUT), "layout");
    EXPECT_EQ(xrParseCursorCrossing(xrCursorCrossingName(XR_CURSORCROSS_LAYOUT)), XR_CURSORCROSS_LAYOUT);
    EXPECT_EQ(xrParseCursorCrossing(xrCursorCrossingName(XR_CURSORCROSS_RAYCAST)), XR_CURSORCROSS_RAYCAST);
}

// --- quadPointFromUV: the inverse of the hit test ------------------------------------------------

TEST(XRCursorCross, QuadPointFromUVRoundTripsWithTheHitTest) {
    // The whole mechanism rests on this being rayQuadIntersect's exact inverse: a UV turned into a
    // world point and cast back at must reproduce the UV. Checked on a rotated quad so a wrong sign
    // or a swapped axis cannot hide.
    const SXRPose quad{Vec3{1.2f, 1.4f, -2.f}, qFromYaw(-0.6f)};
    for (float u : {0.f, 0.25f, 0.5f, 0.9f, 1.f}) {
        for (float v : {0.f, 0.33f, 0.5f, 1.f}) {
            const Vec3 p   = quadPointFromUV(quad, 1.6f, 0.9f, u, v);
            const Vec3 dir = (p - EYE).normalized();
            const auto hit = rayQuadIntersect(quad, EYE, dir, 1.6f, 0.9f, 1e-3f);
            ASSERT_TRUE(hit.hit) << "u=" << u << " v=" << v;
            EXPECT_NEAR(hit.u, u, 1e-3f) << "u=" << u << " v=" << v;
            EXPECT_NEAR(hit.v, v, 1e-3f) << "u=" << u << " v=" << v;
        }
    }
}

TEST(XRCursorCross, QuadPointFromUVCentreAndCorners) {
    const SXRPose quad{Vec3{0.f, 0.f, -1.5f}, Quat{}};
    const Vec3    c = quadPointFromUV(quad, 1.6f, 0.9f, 0.5f, 0.5f);
    EXPECT_NEAR(c.x, 0.f, 1e-5f);
    EXPECT_NEAR(c.y, 0.f, 1e-5f);
    // u = 1 is the RIGHT edge, v = 0 is the TOP edge (rayQuadIntersect's convention).
    const Vec3 tr = quadPointFromUV(quad, 1.6f, 0.9f, 1.f, 0.f);
    EXPECT_NEAR(tr.x, 0.8f, 1e-5f);
    EXPECT_NEAR(tr.y, 0.45f, 1e-5f);
}

TEST(XRCursorCross, QuadPointFromUVIsUnclampedSoOvershootLeavesTheQuad) {
    // A UV past 1 must land past the edge on the EXTENDED plane — that is what sweeps the ray.
    const SXRPose quad{Vec3{0.f, 0.f, -1.5f}, Quat{}};
    const Vec3    past = quadPointFromUV(quad, 1.6f, 0.9f, 1.25f, 0.5f);
    EXPECT_NEAR(past.x, 1.2f, 1e-5f); // (1.25 - 0.5) * 1.6
    EXPECT_NEAR(past.z, -1.5f, 1e-5f);
}

// --- 2D box <-> UV -------------------------------------------------------------------------------

TEST(XRCursorCross, BoxUVIsAffineAndUnclamped) {
    const CBox b = boxAt(2560.0, 100.0);
    EXPECT_NEAR(xrBoxUV(b, Vector2D{2560.0, 100.0}).x, 0.0, 1e-9);
    EXPECT_NEAR(xrBoxUV(b, Vector2D{2560.0 + 960.0, 100.0 + 540.0}).x, 0.5, 1e-9);
    EXPECT_NEAR(xrBoxUV(b, Vector2D{2560.0 + 960.0, 100.0 + 540.0}).y, 0.5, 1e-9);
    // Past the right edge: strictly greater than 1, by exactly the overshoot fraction.
    EXPECT_NEAR(xrBoxUV(b, Vector2D{2560.0 + 1920.0 + 192.0, 100.0}).x, 1.1, 1e-9);
    // Before the left edge: negative.
    EXPECT_NEAR(xrBoxUV(b, Vector2D{2560.0 - 192.0, 100.0}).x, -0.1, 1e-9);
}

TEST(XRCursorCross, ExitUVCapsOvershootButKeepsTheInteriorAxis) {
    const CBox b = boxAt(0.0, 0.0);
    // A hard flick 4 monitor-widths to the right, at 1/4 height. The crossing axis is capped at the
    // budget; the axis the cursor did NOT leave through is untouched, so the ray keeps its height.
    const Vector2D uv = xrCrossExitUV(b, Vector2D{1920.0 * 5.0, 270.0}, XR_CROSS_MAX_OVERSHOOT_UV);
    EXPECT_NEAR(uv.x, 1.0 + (double)XR_CROSS_MAX_OVERSHOOT_UV, 1e-9);
    EXPECT_NEAR(uv.y, 0.25, 1e-9);
}

TEST(XRCursorCross, ExitUVCapsBothDirections) {
    const CBox b = boxAt(0.0, 0.0);
    const auto up = xrCrossExitUV(b, Vector2D{960.0, -1080.0 * 3.0}, 0.5f);
    EXPECT_NEAR(up.y, -0.5, 1e-9);
    EXPECT_NEAR(up.x, 0.5, 1e-9);
}

TEST(XRCursorCross, ExitUVLeavesASmallOvershootAlone) {
    // The common case: one motion event pushes a couple of px past the edge. The cap must not
    // quantize that — the ray is meant to graze the edge, not jump to the budget.
    const CBox     b  = boxAt(0.0, 0.0);
    const Vector2D uv = xrCrossExitUV(b, Vector2D{1922.0, 540.0}, XR_CROSS_MAX_OVERSHOOT_UV);
    EXPECT_GT(uv.x, 1.0);
    EXPECT_LT(uv.x, 1.01);
}

TEST(XRCursorCross, EntryPointMapsUVBackToLayoutPixels) {
    const CBox     b = boxAt(3000.0, 500.0);
    const Vector2D p = xrCrossEntryPoint(b, 0.5f, 0.25f, XR_CROSS_ENTRY_INSET_PX);
    EXPECT_NEAR(p.x, 3000.0 + 960.0, 1e-9);
    EXPECT_NEAR(p.y, 500.0 + 270.0, 1e-9);
}

TEST(XRCursorCross, EntryPointInsetsFromTheBoxEdges) {
    // A tolerated hit lands at u = 0 exactly. Left there, the point sits ON the shared boundary,
    // where CBox::inside is false for every box and closestValid's nearest-edge snap could hand it
    // back to the neighbour we just decided against.
    const CBox     b  = boxAt(3000.0, 500.0);
    const Vector2D tl = xrCrossEntryPoint(b, 0.f, 0.f, XR_CROSS_ENTRY_INSET_PX);
    EXPECT_NEAR(tl.x, 3000.0 + XR_CROSS_ENTRY_INSET_PX, 1e-9);
    EXPECT_NEAR(tl.y, 500.0 + XR_CROSS_ENTRY_INSET_PX, 1e-9);
    const Vector2D br = xrCrossEntryPoint(b, 1.f, 1.f, XR_CROSS_ENTRY_INSET_PX);
    EXPECT_NEAR(br.x, 3000.0 + 1920.0 - XR_CROSS_ENTRY_INSET_PX, 1e-9);
    EXPECT_NEAR(br.y, 500.0 + 1080.0 - XR_CROSS_ENTRY_INSET_PX, 1e-9);
}

TEST(XRCursorCross, EntryPointNeverInvertsATinyBox) {
    // A box narrower than twice the inset must still land inside itself, not fold through it.
    const CBox     b = CBox{10.0, 10.0, 1.0, 1.0};
    const Vector2D p = xrCrossEntryPoint(b, 1.f, 1.f, XR_CROSS_ENTRY_INSET_PX);
    EXPECT_GE(p.x, 10.0);
    EXPECT_LE(p.x, 11.0);
    EXPECT_GE(p.y, 10.0);
    EXPECT_LE(p.y, 11.0);
}

TEST(XRCursorCross, EntryPointClampsAnOutOfRangeUV) {
    const CBox     b = boxAt(0.0, 0.0);
    const Vector2D p = xrCrossEntryPoint(b, 1.7f, -0.4f, XR_CROSS_ENTRY_INSET_PX);
    EXPECT_NEAR(p.x, 1920.0 - XR_CROSS_ENTRY_INSET_PX, 1e-9);
    EXPECT_NEAR(p.y, XR_CROSS_ENTRY_INSET_PX, 1e-9);
}

// --- target selection ----------------------------------------------------------------------------

TEST(XRCursorCross, PicksTheQuadTheRayActuallyMeets) {
    // Source dead ahead; a neighbour immediately to its right, edge to edge (both 1.6 m wide at
    // 2 m, so centres 1.6 m apart touch). Push past the source's right edge at mid height: the ray
    // enters the neighbour at its LEFT edge, which is exactly where the eye expects the cursor.
    const auto                src  = quadAt(1, 0.f, 0.f, 2.f);
    const auto                nbr  = quadAt(2, 1.6f, 0.f, 2.f);
    std::vector<SXRCrossQuad> all{src, nbr};

    const Vec3 through = quadPointFromUV(src.pose, src.wMeters, src.hMeters, 1.001f, 0.5f);
    const auto pick    = xrPickCrossTarget(EYE, through, all, src.id, XR_CROSS_TOLERANCE_DEG);
    ASSERT_TRUE(pick.ok);
    EXPECT_EQ(pick.id, 2);
    EXPECT_FALSE(pick.tolerated);
    EXPECT_NEAR(pick.u, 0.f, 0.02f);
    EXPECT_NEAR(pick.v, 0.5f, 0.02f);
}

TEST(XRCursorCross, NeverPicksTheSourceItself) {
    // The source's own plane is met at the exit point by construction. With no other candidate the
    // answer must be "nothing there", so the caller falls back to the 2D layout.
    const auto                src = quadAt(1, 0.f, 0.f, 2.f);
    std::vector<SXRCrossQuad> all{src};
    const Vec3                through = quadPointFromUV(src.pose, src.wMeters, src.hMeters, 1.001f, 0.5f);
    EXPECT_FALSE(xrPickCrossTarget(EYE, through, all, src.id, XR_CROSS_TOLERANCE_DEG).ok);
}

TEST(XRCursorCross, MissFallsBackRatherThanGuessing) {
    // Push RIGHT while the only neighbour is far to the LEFT. Nothing is over there, so no pick —
    // the layout's answer stands.
    const auto                src = quadAt(1, 0.f, 0.f, 2.f);
    const auto                far = quadAt(2, -3.2f, 0.f, 2.f);
    std::vector<SXRCrossQuad> all{src, far};
    const Vec3                through = quadPointFromUV(src.pose, src.wMeters, src.hMeters, 1.001f, 0.5f);
    EXPECT_FALSE(xrPickCrossTarget(EYE, through, all, src.id, XR_CROSS_TOLERANCE_DEG).ok);
}

TEST(XRCursorCross, NearestHitWinsAmongCandidates) {
    // Two quads stacked along the same line of sight to the right of the source. The NEAR one wins:
    // it is the one that occludes the other, i.e. the one the user can see.
    const auto                src = quadAt(1, 0.f, 0.f, 2.f);
    const auto                near_ = quadAt(2, 0.9f, 0.f, 1.f);  // ~42 deg right at 1 m
    const auto                far_  = quadAt(3, 1.8f, 0.f, 2.f);  // ~42 deg right at 2 m
    std::vector<SXRCrossQuad> all{src, far_, near_};              // far listed first on purpose

    const Vec3 dirPoint = Vec3{0.9f, 0.f, -1.f}; // straight at the near quad's centre
    const auto pick     = xrPickCrossTarget(EYE, dirPoint, all, src.id, XR_CROSS_TOLERANCE_DEG);
    ASSERT_TRUE(pick.ok);
    EXPECT_EQ(pick.id, 2);
    EXPECT_NEAR(pick.t, std::sqrt(0.9f * 0.9f + 1.f), 1e-3f);
}

TEST(XRCursorCross, DepthDisagreementIsTheWholePoint) {
    // The case the 2D sync gets wrong. Two neighbours at the SAME azimuth band but different
    // depths: a small-looking far quad at 4 m and a near quad at 1.2 m, both to the right. The
    // unwrap projects centres by angle alone and cannot separate them; the ray can, and picks the
    // one that is actually in the way.
    const auto                src  = quadAt(1, 0.f, 0.f, 2.f);
    auto                      far_ = quadAt(2, 2.0f, 0.f, 4.f);
    auto                      near_ = quadAt(3, 0.6f, 0.f, 1.2f);
    std::vector<SXRCrossQuad> all{src, far_, near_};

    const Vec3 through = quadPointFromUV(src.pose, src.wMeters, src.hMeters, 1.05f, 0.5f);
    const auto pick    = xrPickCrossTarget(EYE, through, all, src.id, XR_CROSS_TOLERANCE_DEG);
    ASSERT_TRUE(pick.ok);
    EXPECT_EQ(pick.id, 3) << "the near quad occludes the far one along that line of sight";
}

TEST(XRCursorCross, ElevationSeparatesNeighbours) {
    // A quad parked well ABOVE the source's right edge is not "to the right" at mid height — the
    // ray passes under it and finds nothing. Pushing near the TOP of the source does reach it.
    const auto                src  = quadAt(1, 0.f, 0.f, 2.f);
    const auto                up_  = quadAt(2, 1.6f, 1.2f, 2.f);
    std::vector<SXRCrossQuad> all{src, up_};

    const Vec3 mid = quadPointFromUV(src.pose, src.wMeters, src.hMeters, 1.05f, 0.5f);
    EXPECT_FALSE(xrPickCrossTarget(EYE, mid, all, src.id, XR_CROSS_TOLERANCE_DEG).ok);

    // Same push, but aimed at the source's top-right: now the neighbour's lower-left corner is on
    // the line, and the cursor lands there rather than in its middle.
    const Vec3 high = quadPointFromUV(src.pose, src.wMeters, src.hMeters, 1.05f, -0.75f);
    const auto pick = xrPickCrossTarget(EYE, high, all, src.id, XR_CROSS_TOLERANCE_DEG);
    ASSERT_TRUE(pick.ok);
    EXPECT_EQ(pick.id, 2);
    EXPECT_LT(pick.u, 0.5f);
    EXPECT_GT(pick.v, 0.5f);
}

TEST(XRCursorCross, ToleranceBridgesAVisualGap) {
    // Two quads with a hand-placed gap between them: 1.6 m wide at 2 m, centres 1.75 m apart, so a
    // 15 cm gap (~4.3 deg at that distance). A ray grazing the source's right edge threads it and
    // misses squarely — with the angular margin it lands on the neighbour's left edge instead,
    // which is what the user meant by pushing that way.
    const auto                src = quadAt(1, 0.f, 0.f, 2.f);
    const auto                nbr = quadAt(2, 1.75f, 0.f, 2.f);
    std::vector<SXRCrossQuad> all{src, nbr};
    const Vec3                through = quadPointFromUV(src.pose, src.wMeters, src.hMeters, 1.001f, 0.5f);

    EXPECT_FALSE(xrPickCrossTarget(EYE, through, all, src.id, /*toleranceDeg=*/0.f).ok);

    const auto pick = xrPickCrossTarget(EYE, through, all, src.id, XR_CROSS_TOLERANCE_DEG);
    ASSERT_TRUE(pick.ok);
    EXPECT_EQ(pick.id, 2);
    EXPECT_TRUE(pick.tolerated);
    // A tolerated hit is clamped into bounds, so the cursor lands ON the monitor, at its near edge.
    EXPECT_GE(pick.u, 0.f);
    EXPECT_LE(pick.u, 1.f);
    EXPECT_NEAR(pick.u, 0.f, 1e-5f);
}

TEST(XRCursorCross, ASquareHitAlwaysBeatsATolerated) {
    // THE ordering policy: the margin may only rescue a crossing that found nothing. Here a distant
    // quad is squarely on the ray while a nearer one is just outside it — the near one would win on
    // distance if both passes were merged, and must not.
    // Ray direction (0.87, 0, -1): crosses z = -2 at x = 1.74 and z = -3 at x = 2.61.
    const auto                src    = quadAt(1, 0.f, 0.f, 2.f);
    auto                      grazed = quadAt(2, 2.7f, 0.f, 2.f);  // spans [1.90, 3.50]: misses by 16 cm, inside 4 deg at 3.4 m
    auto                      onRay  = quadAt(3, 2.61f, 0.f, 3.f); // spans [1.81, 3.41]: dead centre, but farther
    std::vector<SXRCrossQuad> all{src, grazed, onRay};

    const Vec3 through = Vec3{0.87f, 0.f, -1.f};
    const auto pick    = xrPickCrossTarget(EYE, through, all, src.id, XR_CROSS_TOLERANCE_DEG);
    ASSERT_TRUE(pick.ok);
    EXPECT_EQ(pick.id, 3);
    EXPECT_FALSE(pick.tolerated);
}

TEST(XRCursorCross, ZeroToleranceIsAPureIntersection) {
    const auto                src = quadAt(1, 0.f, 0.f, 2.f);
    const auto                nbr = quadAt(2, 1.6f, 0.f, 2.f);
    std::vector<SXRCrossQuad> all{src, nbr};
    const Vec3                through = quadPointFromUV(src.pose, src.wMeters, src.hMeters, 1.2f, 0.5f);
    const auto                pick    = xrPickCrossTarget(EYE, through, all, src.id, 0.f);
    ASSERT_TRUE(pick.ok);
    EXPECT_FALSE(pick.tolerated);
}

TEST(XRCursorCross, DegenerateGeometryIsIgnored) {
    const auto                src  = quadAt(1, 0.f, 0.f, 2.f);
    auto                      zero = quadAt(2, 1.6f, 0.f, 2.f);
    zero.wMeters                   = 0.f; // a monitor mid-creation with no size yet
    auto                      unbound = quadAt(3, 1.6f, 0.f, 2.f);
    unbound.id                        = -1; // never bound to a CMonitor
    std::vector<SXRCrossQuad> all{src, zero, unbound};
    const Vec3                through = quadPointFromUV(src.pose, src.wMeters, src.hMeters, 1.001f, 0.5f);
    EXPECT_FALSE(xrPickCrossTarget(EYE, through, all, src.id, XR_CROSS_TOLERANCE_DEG).ok);
}

TEST(XRCursorCross, ExitPointAtTheEyeHasNoDirection) {
    // Degenerate ray: the head is exactly on the exit point. No direction to cast, no pick.
    const auto                src = quadAt(1, 0.f, 0.f, 2.f);
    const auto                nbr = quadAt(2, 1.6f, 0.f, 2.f);
    std::vector<SXRCrossQuad> all{src, nbr};
    EXPECT_FALSE(xrPickCrossTarget(EYE, EYE, all, src.id, XR_CROSS_TOLERANCE_DEG).ok);
}

TEST(XRCursorCross, QuadsBehindTheHeadAreNotHit) {
    // rayQuadIntersect rejects t <= 0, so a quad behind the eye is never a crossing target even
    // though its infinite plane meets the line.
    const auto                src    = quadAt(1, 0.f, 0.f, 2.f);
    auto                      behind = quadAt(2, 0.f, 0.f, -2.f); // +2 m on Z: behind the user
    std::vector<SXRCrossQuad> all{src, behind};
    const Vec3                through = quadPointFromUV(src.pose, src.wMeters, src.hMeters, 1.001f, 0.5f);
    EXPECT_FALSE(xrPickCrossTarget(EYE, through, all, src.id, XR_CROSS_TOLERANCE_DEG).ok);
}

TEST(XRCursorCross, HeadPositionMovesTheAnswer) {
    // The ray is cast from where the head IS, not from a fixed origin — leaning changes which
    // monitor is "over there", which is the whole reason the pose must be fresh. Standing at the
    // origin the push finds nothing; leaning left swings the same exit point onto the neighbour.
    // (The neighbour must sit at a DIFFERENT depth from the exit point for the eye to matter at
    // all: a quad coplanar with the exit point is met at that point from every head position.)
    const auto                src = quadAt(1, 0.f, 0.f, 2.f);
    const auto                nbr = quadAt(2, 2.8f, 0.f, 4.f); // spans [2.0, 3.6] at z = -4
    std::vector<SXRCrossQuad> all{src, nbr};
    const Vec3                through = quadPointFromUV(src.pose, src.wMeters, src.hMeters, 1.05f, 0.5f);

    EXPECT_FALSE(xrPickCrossTarget(EYE, through, all, src.id, /*toleranceDeg=*/0.f).ok);

    const Vec3 leaned{-1.2f, 0.f, 0.f};
    const auto pick = xrPickCrossTarget(leaned, through, all, src.id, /*toleranceDeg=*/0.f);
    ASSERT_TRUE(pick.ok);
    EXPECT_EQ(pick.id, 2);
}

TEST(XRCursorCross, YawedNeighbourIsHitOnItsFace) {
    // A cockpit wrap: the right-hand monitor is yawed to face the user. The ray must hit its FACE
    // and produce a sane UV, not skim its plane.
    const auto   src = quadAt(1, 0.f, 0.f, 2.f);
    SXRCrossQuad nbr = quadAt(2, 1.5f, 0.f, 1.6f);
    nbr.pose.rot     = qFromYaw(-40.f * PI / 180.f); // toe in toward the user
    std::vector<SXRCrossQuad> all{src, nbr};

    const Vec3 through = quadPointFromUV(src.pose, src.wMeters, src.hMeters, 1.05f, 0.5f);
    const auto pick    = xrPickCrossTarget(EYE, through, all, src.id, XR_CROSS_TOLERANCE_DEG);
    ASSERT_TRUE(pick.ok);
    EXPECT_EQ(pick.id, 2);
    EXPECT_GE(pick.u, 0.f);
    EXPECT_LE(pick.u, 1.f);
    EXPECT_GT(pick.t, 0.f);
}

// --- the full crossing, end to end ---------------------------------------------------------------

TEST(XRCursorCross, EndToEndCrossingLandsWhereTheEyeLooks) {
    // Everything the manager composes, in order: exit UV from the 2D box -> world point on the
    // source's extended plane -> ray -> pick -> entry point in the TARGET's 2D box. The 2D layout
    // here is the compacted grid the sync produces (two 1920x1080 monitors side by side), and the
    // 3D arrangement matches, so the cursor must appear at the target's left edge at the same
    // height it left — a visually continuous crossing.
    const auto                src = quadAt(1, 0.f, 0.f, 2.f);
    const auto                nbr = quadAt(2, 1.6f, 0.f, 2.f);
    std::vector<SXRCrossQuad> all{src, nbr};

    const CBox srcBox = boxAt(0.0, 0.0);
    const CBox tgtBox = boxAt(1920.0, 0.0);

    // The cursor sat at 3/4 height and was pushed 3 px past the right edge.
    const Vector2D newPos  = Vector2D{1923.0, 810.0};
    const Vector2D exitUV  = xrCrossExitUV(srcBox, newPos, XR_CROSS_MAX_OVERSHOOT_UV);
    const Vec3     through = quadPointFromUV(src.pose, src.wMeters, src.hMeters, (float)exitUV.x, (float)exitUV.y);
    const auto     pick    = xrPickCrossTarget(EYE, through, all, src.id, XR_CROSS_TOLERANCE_DEG);
    ASSERT_TRUE(pick.ok);
    ASSERT_EQ(pick.id, 2);

    const Vector2D land = xrCrossEntryPoint(tgtBox, pick.u, pick.v, XR_CROSS_ENTRY_INSET_PX);
    // Inside the target, hard against its left edge, at the height it left.
    EXPECT_GE(land.x, tgtBox.x);
    EXPECT_LE(land.x, tgtBox.x + 8.0);
    EXPECT_NEAR(land.y, 810.0, 12.0);
}

TEST(XRCursorCross, EndToEndOvershootReachesPastAGap) {
    // Same scene with a 30 cm gap — wider than the angular margin at this distance. A gentle push
    // finds nothing (the layout decides), but a hard flick sweeps the ray far enough to land.
    const auto                src = quadAt(1, 0.f, 0.f, 2.f);
    const auto                nbr = quadAt(2, 1.9f, 0.f, 2.f);
    std::vector<SXRCrossQuad> all{src, nbr};
    const CBox                srcBox = boxAt(0.0, 0.0);

    const auto castAt = [&](double x) {
        const Vector2D uv = xrCrossExitUV(srcBox, Vector2D{x, 540.0}, XR_CROSS_MAX_OVERSHOOT_UV);
        const Vec3     p  = quadPointFromUV(src.pose, src.wMeters, src.hMeters, (float)uv.x, (float)uv.y);
        return xrPickCrossTarget(EYE, p, all, src.id, /*toleranceDeg=*/0.f);
    };

    EXPECT_FALSE(castAt(1922.0).ok);
    const auto flick = castAt(1920.0 * 2.0);
    ASSERT_TRUE(flick.ok);
    EXPECT_EQ(flick.id, 2);
}
