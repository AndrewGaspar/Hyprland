#include <openxr/XRSurfaces.hpp>
#include <openxr/XRPlaneSource.hpp>
#include <openxr/XRAnchor.hpp>

#include <gtest/gtest.h>
#include <hyprutils/memory/Casts.hpp>

#include <cmath>
#include <format>

using namespace OpenXR;
using Hyprutils::Memory::sc;

// tests/xr/surfaces.cpp — research/32 C0-C2: the pure plane model and the snap math. No runtime.

static constexpr float DEG = 3.14159265F / 180.F;

static void            expectVecNear(const Vec3& a, const Vec3& b, float tol = 1e-4F) {
    EXPECT_NEAR(a.x, b.x, tol);
    EXPECT_NEAR(a.y, b.y, tol);
    EXPECT_NEAR(a.z, b.z, tol);
}

static SXRPlane table(float y = 0.74F, float z = -0.7F, float w = 1.2F, float d = 0.7F, float yawDeg = 0.F) {
    auto res = xrParseSyntheticPlanes(std::format("table pos:0,{},{} size:{},{} yaw:{}", y, z, w, d, yawDeg));
    EXPECT_TRUE(res.has_value());
    return res->at(0);
}

// A window carried upright in front of the user, its visible bottom edge `gap` above the table top.
static SXRSurfaceQuery carried(const SXRPlane& t, float gap, float x = 0.F, float z = -0.7F) {
    SXRSurfaceQuery q;
    q.halfWidth   = 0.5F;
    q.bottomExt   = 0.35F;
    q.head        = Vec3{0.F, 1.6F, 0.F};
    q.headValid   = true;
    q.content.pos = Vec3{x, t.pose.pos.y + gap + q.bottomExt, z};
    q.content.rot = qFromYaw(0.F); // upright, facing +Z (the user)
    return q;
}

// ---- polygon / ray ----

TEST(Surfaces, PointInPolygonSquareAndConcave) {
    const SXRVec2 sq[] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    EXPECT_TRUE(xrPointInPolygon({0, 0}, sq, 4));
    EXPECT_TRUE(xrPointInPolygon({0.99F, -0.99F}, sq, 4));
    EXPECT_FALSE(xrPointInPolygon({1.01F, 0}, sq, 4));
    EXPECT_FALSE(xrPointInPolygon({0, 0}, sq, 2)); // degenerate contains nothing
    // an L shape: the notch is outside
    const SXRVec2 l[] = {{0, 0}, {2, 0}, {2, 1}, {1, 1}, {1, 2}, {0, 2}};
    EXPECT_TRUE(xrPointInPolygon({0.5F, 1.5F}, l, 6));
    EXPECT_FALSE(xrPointInPolygon({1.5F, 1.5F}, l, 6));
    // winding-agnostic
    const SXRVec2 cw[] = {{-1, 1}, {1, 1}, {1, -1}, {-1, -1}};
    EXPECT_TRUE(xrPointInPolygon({0.2F, 0.3F}, cw, 4));
}

TEST(Surfaces, RayHitsTableInsideAndMissesOutside) {
    const auto t   = table();
    const auto hit = xrRayPlane(t, Vec3{0.F, 1.6F, 0.F}, Vec3{0.F, -0.86F, -0.7F});
    ASSERT_TRUE(hit.hit);
    EXPECT_TRUE(hit.inside);
    EXPECT_NEAR(hit.point.y, 0.74F, 1e-4F);
    EXPECT_NEAR(hit.point.z, -0.7F, 1e-3F);
    // pointing past the far edge: hits the infinite plane, outside the polygon
    const auto far = xrRayPlane(t, Vec3{0.F, 1.6F, 0.F}, Vec3{0.F, -0.86F, -2.F});
    EXPECT_TRUE(far.hit);
    EXPECT_FALSE(far.inside);
    // parallel and away
    EXPECT_FALSE(xrRayPlane(t, Vec3{0.F, 1.6F, 0.F}, Vec3{0.F, 0.F, -1.F}).hit);
    EXPECT_FALSE(xrRayPlane(t, Vec3{0.F, 1.6F, 0.F}, Vec3{0.F, 1.F, 0.F}).hit);
}

// ---- parsing the synthetic provider ----

TEST(Surfaces, SyntheticParseAndOrientation) {
    auto res = xrParseSyntheticPlanes("table pos:0,0.74,-0.7 size:1.2,0.7; wall pos:0,1.2,-2.5 size:3,2.4 ; ceiling pos:0,2.6,0 size:4,4");
    ASSERT_TRUE(res.has_value()) << res.error();
    ASSERT_EQ(res->size(), 3U);
    const auto& t = (*res)[0];
    EXPECT_EQ(t.label, XR_PLANE_LABEL_TABLE);
    EXPECT_EQ(t.orient, XR_PLANE_UP);
    expectVecNear(xrPlaneNormal(t), Vec3{0.F, 1.F, 0.F});
    EXPECT_NE(t.id & XR_PLANE_SYNTHETIC_ID_BIT, 0ULL);
    // the depth (local y) of a table runs along world z at yaw 0
    expectVecNear(xrPlaneWorld(t, SXRVec2{0.F, 0.35F}), Vec3{0.F, 0.74F, -1.05F});
    EXPECT_TRUE(xrPlaneContains(t, {0.59F, 0.34F}));
    EXPECT_FALSE(xrPlaneContains(t, {0.61F, 0.F}));

    const auto& w = (*res)[1];
    EXPECT_EQ(w.orient, XR_PLANE_VERTICAL);
    expectVecNear(xrPlaneNormal(w), Vec3{0.F, 0.F, 1.F});
    EXPECT_EQ((*res)[2].orient, XR_PLANE_DOWN);
    EXPECT_NE((*res)[0].id, (*res)[1].id);
}

TEST(Surfaces, SyntheticParseErrors) {
    EXPECT_TRUE(xrParseSyntheticPlanes("").has_value());
    EXPECT_TRUE(xrParseSyntheticPlanes(" ; ").value().empty());
    EXPECT_FALSE(xrParseSyntheticPlanes("sofa pos:0,0,0 size:1,1").has_value());
    EXPECT_FALSE(xrParseSyntheticPlanes("table pos:0,0 size:1,1").has_value());
    EXPECT_FALSE(xrParseSyntheticPlanes("table pos:0,0,0").has_value());
    EXPECT_FALSE(xrParseSyntheticPlanes("table pos:0,0,0 size:-1,1").has_value());
    EXPECT_FALSE(xrParseSyntheticPlanes("table pos:0,0,0 size:1,1 tilt:3").has_value());
    EXPECT_FALSE(xrParseSyntheticPlanes("table pos:0,a,0 size:1,1").has_value());
}

TEST(Surfaces, SyntheticYawTurnsTheTable) {
    const auto t = table(0.74F, -1.F, 2.F, 0.4F, 90.F);
    // turned a quarter: the 2 m width now runs along world z
    EXPECT_TRUE(xrPlaneContains(t, SXRVec2{xrPlaneLocal(t, Vec3{0.F, 0.74F, -1.9F}).x, xrPlaneLocal(t, Vec3{0.F, 0.74F, -1.9F}).y}));
    const Vec3 sideways = xrPlaneLocal(t, Vec3{0.9F, 0.74F, -1.F});
    EXPECT_FALSE(xrPlaneContains(t, SXRVec2{sideways.x, sideways.y}));
}

TEST(Surfaces, SyntheticSourceRelocatesWithTheFloorShift) {
    CXRSyntheticPlaneSource src;
    ASSERT_TRUE(src.setSpec("table pos:0,0.74,-0.7 size:1.2,0.7"));
    std::vector<SXRPlane> out;
    SXRPlanePollContext   ctx;
    ASSERT_EQ(src.poll(ctx, out), XR_PLANE_POLL_UPDATED);
    ASSERT_EQ(out.size(), 1U);
    EXPECT_NEAR(out[0].pose.pos.y, 0.74F, 1e-6F);
    EXPECT_EQ(src.poll(ctx, out), XR_PLANE_POLL_UNCHANGED);
    ctx.floorShift = 1.6F; // the LOCAL fallback
    ASSERT_EQ(src.poll(ctx, out), XR_PLANE_POLL_UPDATED);
    EXPECT_NEAR(out[0].pose.pos.y, 2.34F, 1e-5F);
    EXPECT_FALSE(src.setSpec("table nonsense"));
    EXPECT_EQ(src.planeCount(), 0U);
    EXPECT_NE(src.status().find("invalid"), std::string::npos);
}

// ---- the snap ----

TEST(Surfaces, SnapEntersWithinDistanceAndStands) {
    const auto       t = table();
    SXRSurfaceTuning tun;
    // 5 cm above: snaps
    auto q = carried(t, 0.05F);
    auto c = xrSurfacePick(&t, 1, q, tun, 0);
    ASSERT_TRUE(c.ok);
    EXPECT_EQ(c.kind, XR_SNAP_STAND);
    EXPECT_NEAR(c.contact.y, 0.74F, 1e-4F);
    // 20 cm above: does not
    EXPECT_FALSE(xrSurfacePick(&t, 1, carried(t, 0.20F), tun, 0).ok);
    // pushed 10 cm through the top still counts; 40 cm through does not
    EXPECT_TRUE(xrSurfacePick(&t, 1, carried(t, -0.10F), tun, 0).ok);
    EXPECT_FALSE(xrSurfacePick(&t, 1, carried(t, -0.40F), tun, 0).ok);
    // off the table's edge: no
    EXPECT_FALSE(xrSurfacePick(&t, 1, carried(t, 0.02F, 1.5F), tun, 0).ok);
}

TEST(Surfaces, StandPoseRestsBottomEdgeFacesUserTiltsBack) {
    const auto       t = table();
    SXRSurfaceTuning tun;
    tun.standTiltRad = 12.F * DEG;
    const auto q     = carried(t, 0.03F, 0.2F);
    const auto c     = xrSurfacePick(&t, 1, q, tun, 0);
    ASSERT_TRUE(c.ok);
    // the visible bottom edge sits on the table (plus the lift)
    const Vec3 up     = qRotate(c.pose.rot, Vec3{0.F, 1.F, 0.F});
    const Vec3 bottom = c.pose.pos - up * q.bottomExt;
    EXPECT_NEAR(bottom.y, 0.74F + tun.lift, 1e-4F);
    EXPECT_NEAR(bottom.x, 0.2F, 1e-4F);
    // faces the user (horizontal part of the normal points at the head)
    const Vec3 n      = qRotate(c.pose.rot, Vec3{0.F, 0.F, 1.F});
    const Vec3 toHead = (q.head - c.contact);
    EXPECT_GT(n.x * toHead.x + n.z * toHead.z, 0.F);
    EXPECT_NEAR(std::atan2(n.x, n.z), std::atan2(toHead.x, toHead.z), 1e-3F);
    // tilted back by the configured angle: the normal leans up, the top edge leans away
    EXPECT_NEAR(std::asin(n.y), 12.F * DEG, 1e-3F);
    EXPECT_GT(up.y, 0.97F);
    // no roll
    EXPECT_NEAR(qRotate(c.pose.rot, Vec3{1.F, 0.F, 0.F}).y, 0.F, 1e-4F);
}

TEST(Surfaces, FlatWithModifierOrWristTilt) {
    const auto       t = table();
    SXRSurfaceTuning tun;
    auto             q = carried(t, 0.03F);
    q.flat             = true;
    auto c             = xrSurfacePick(&t, 1, q, tun, 0);
    ASSERT_TRUE(c.ok);
    EXPECT_EQ(c.kind, XR_SNAP_FLAT);
    // lies on the table, face up, near edge on the contact, top edge away from the user
    expectVecNear(qRotate(c.pose.rot, Vec3{0.F, 0.F, 1.F}), Vec3{0.F, 1.F, 0.F});
    EXPECT_NEAR(c.pose.pos.y, 0.74F + tun.lift, 1e-4F);
    EXPECT_LT(c.pose.pos.z, c.contact.z); // farther from the user than where the edge landed
    EXPECT_LT(qRotate(c.pose.rot, Vec3{0.F, 1.F, 0.F}).z, -0.99F);

    // no modifier, but carried tilted face-up past 45 deg: flat too
    auto q2        = carried(t, 0.02F);
    q2.content.rot = qMul(qFromYaw(0.F), qFromPitch(-60.F * DEG));
    q2.content.pos = Vec3{0.F, 0.74F + 0.05F, -0.7F};
    auto c2        = xrSurfacePick(&t, 1, q2, tun, 0);
    ASSERT_TRUE(c2.ok);
    EXPECT_EQ(c2.kind, XR_SNAP_FLAT);
}

TEST(Surfaces, HysteresisHoldsTheStickyPlane) {
    const auto       t = table();
    SXRSurfaceTuning tun; // enter 8 cm, exit 12 cm
    const auto       q = carried(t, 0.10F);
    EXPECT_FALSE(xrSurfacePick(&t, 1, q, tun, 0).ok);
    EXPECT_TRUE(xrSurfacePick(&t, 1, q, tun, t.id).ok);
    EXPECT_FALSE(xrSurfacePick(&t, 1, carried(t, 0.13F), tun, t.id).ok);
}

TEST(Surfaces, PrefersTheTableOverAWallAndTheRayHitPlane) {
    // two patches of one table 2 cm apart (within the same-surface band, so the ray may choose)
    auto res = xrParseSyntheticPlanes("wall pos:0,1,-0.75 size:3,3; table pos:0,0.74,-0.7 size:1.2,0.7; table pos:0,0.72,-0.7 size:1.2,0.7");
    ASSERT_TRUE(res.has_value());
    SXRSurfaceTuning tun;
    auto             q = carried((*res)[1], 0.03F);
    q.content.pos.z    = -0.72F; // 3 cm in front of the wall too
    auto c             = xrSurfacePick(res->data(), res->size(), q, tun, 0);
    ASSERT_TRUE(c.ok);
    EXPECT_NE(c.kind, XR_SNAP_WALL);
    EXPECT_EQ(c.planeId, (*res)[1].id); // the nearer of the two stacked tables
    // the ray points at the lower one: it wins despite being farther
    q.haveRay   = true;
    q.rayOrigin = Vec3{0.F, 1.6F, 0.F};
    q.rayDir    = Vec3{0.F, -0.9F, -0.7F};
    // make only the lower table reachable by the ray: shrink the upper one away from the ray hit
    (*res)[1].extentX = (*res)[1].extentY = 0.F;
    (*res)[1].nPoly                       = 4;
    (*res)[1].poly                        = {};
    (*res)[1].poly[0]                     = {-0.6F, 0.2F};
    (*res)[1].poly[1]                     = {0.6F, 0.2F};
    (*res)[1].poly[2]                     = {0.6F, 0.35F};
    (*res)[1].poly[3]                     = {-0.6F, 0.35F};
    q.content.pos.z                       = -0.98F; // still over both
    auto c2                               = xrSurfacePick(res->data(), res->size(), q, tun, 0);
    ASSERT_TRUE(c2.ok);
    EXPECT_TRUE(c2.rayHit);
    EXPECT_EQ(c2.planeId, (*res)[2].id);
}

TEST(Surfaces, WallSnapsFlushFacingTheRoom) {
    auto res = xrParseSyntheticPlanes("wall pos:0,1.2,-2 size:3,2.4");
    ASSERT_TRUE(res.has_value());
    SXRSurfaceTuning tun;
    SXRSurfaceQuery  q;
    q.head        = Vec3{0.3F, 1.6F, 0.F};
    q.headValid   = true;
    q.content.pos = Vec3{0.4F, 1.3F, -1.95F};
    q.content.rot = qFromYaw(20.F * DEG);
    auto c        = xrSurfacePick(res->data(), 1, q, tun, 0);
    ASSERT_TRUE(c.ok);
    EXPECT_EQ(c.kind, XR_SNAP_WALL);
    expectVecNear(qRotate(c.pose.rot, Vec3{0.F, 0.F, 1.F}), Vec3{0.F, 0.F, 1.F});
    EXPECT_NEAR(c.pose.pos.z, -2.F + tun.lift, 1e-4F);
    EXPECT_NEAR(c.pose.pos.y, 1.3F, 1e-4F); // keeps the height it was carried at
    tun.walls = false;
    EXPECT_FALSE(xrSurfacePick(res->data(), 1, q, tun, 0).ok);
}

TEST(Surfaces, ReleaseUsesTheLatchedPoseAndTheArmedPlane) {
    const auto    t = table();
    SXRSurfaceArm arm;
    arm.havePlane = true;
    arm.plane     = t;
    arm.query     = carried(t, 0.F);
    // previewing: the exit distance applies to the release pose
    arm.preview = true;
    auto rel    = carried(t, 0.11F).content;
    auto c      = xrSurfaceRelease(arm, rel, Vec3{0.F, 1.6F, 0.F}, true);
    EXPECT_TRUE(c.ok);
    EXPECT_EQ(c.planeId, t.id);
    // not previewing: only the entry distance
    arm.preview = false;
    EXPECT_FALSE(xrSurfaceRelease(arm, rel, Vec3{0.F, 1.6F, 0.F}, true).ok);
    // no plane armed: never snaps
    arm.havePlane = false;
    EXPECT_FALSE(xrSurfaceRelease(arm, carried(t, 0.01F).content, Vec3{}, false).ok);
}

// ---- plane-relative storage + re-locate ----

TEST(Surfaces, PlaneRelativeRoundTrip) {
    const auto    t = table(0.74F, -0.7F, 1.2F, 0.7F, 33.F);
    const SXRPose w{Vec3{0.3F, 1.1F, -0.6F}, qMul(qFromYaw(0.4F), qFromPitch(-0.2F))};
    const auto    back = xrPoseFromPlane(t, xrPoseToPlane(t, w));
    expectVecNear(back.pos, w.pos);
    EXPECT_LT(qAngleBetween(back.rot, w.rot), 1e-3F);
}

TEST(Surfaces, ReexpressKeepsPlanesInTheRoomAcrossARecenter) {
    SXRPlaneSnapshot s;
    s.planes[0]       = table();
    s.n               = 1;
    const Vec3 corner = xrPlaneWorld(s.planes[0], SXRVec2{0.6F, 0.35F});
    // the new origin sits 1 m to the right, turned 90 deg, in the old space
    const SXRPose M{Vec3{1.F, 0.F, 0.F}, qFromYaw(90.F * DEG)};
    xrPlanesReexpress(s, M);
    // the same physical corner, expressed in the new space, is inv(M) applied to the old coordinates
    const Vec3 expect = poseCompose(poseInverse(M), SXRPose{corner, Quat{}}).pos;
    expectVecNear(xrPlaneWorld(s.planes[0], SXRVec2{0.6F, 0.35F}), expect);
    // and an anchor re-expressed the same way keeps its plane-relative offset bit for bit in meaning
    EXPECT_EQ(xrFindPlane(s, s.planes[0].id), &s.planes[0]);
    EXPECT_EQ(xrFindPlane(s, 12345), nullptr);
}

TEST(Surfaces, AttachFollowsRefinementDetachesOnJumpHoldsWhenLost) {
    auto             t = table();
    SXRSurfaceAttach a;
    a.valid   = true;
    a.planeId = t.id;
    a.placed  = SXRPose{Vec3{0.1F, 1.1F, -0.7F}, qFromYaw(0.F)};
    a.local   = xrPoseToPlane(t, a.placed);
    SXRPose out;
    EXPECT_EQ(xrAttachStep(a, &t, out), XR_ATTACH_HOLD); // unchanged
    // refined 2 cm up: follow
    t.pose.pos.y += 0.02F;
    ASSERT_EQ(xrAttachStep(a, &t, out), XR_ATTACH_FOLLOW);
    EXPECT_NEAR(out.pos.y, 1.12F, 1e-4F);
    // jumped 1 m: detach (a SLAM reset re-anchored the plane — do not drag the window along)
    t.pose.pos.z += 1.F;
    EXPECT_EQ(xrAttachStep(a, &t, out), XR_ATTACH_DETACH);
    // plane gone: hold the world pose
    EXPECT_EQ(xrAttachStep(a, nullptr, out), XR_ATTACH_HOLD);
}

TEST(Surfaces, CanonicalPlaneAcceptsBothNormalConventions) {
    // +Z normal convention, horizontal-up declared: untouched
    const SXRPose upZ{Vec3{0.F, 0.7F, -1.F}, qFromPitch(-90.F * DEG)};
    const SXRVec2 poly[]  = {{-0.5F, -0.2F}, {0.5F, -0.2F}, {0.5F, 0.3F}, {-0.5F, 0.3F}};
    bool          swapped = true;
    auto          p       = xrCanonicalPlane(upZ, XR_PLANE_UP, poly, 4, 1.F, 0.5F, swapped);
    EXPECT_FALSE(swapped);
    EXPECT_EQ(p.orient, XR_PLANE_UP);
    EXPECT_EQ(p.nPoly, 4);
    // +Y normal convention (identity pose for a floor-like plane): rotated so +Z is up, contour
    // (u, v) meaning local (u, 0, v) stays the same world point
    const SXRPose upY{Vec3{0.F, 0.7F, -1.F}, Quat{}};
    auto          q = xrCanonicalPlane(upY, XR_PLANE_UP, poly, 4, 1.F, 0.5F, swapped);
    EXPECT_TRUE(swapped);
    EXPECT_EQ(q.orient, XR_PLANE_UP);
    expectVecNear(xrPlaneNormal(q), Vec3{0.F, 1.F, 0.F});
    for (size_t i = 0; i < 4; ++i)
        expectVecNear(xrPlaneWorld(q, q.poly[i]), upY.pos + Vec3{poly[i].x, 0.F, poly[i].y});
    // no claim: trust +Z
    auto r = xrCanonicalPlane(upY, XR_PLANE_ARBITRARY, poly, 4, 1.F, 0.5F, swapped);
    EXPECT_FALSE(swapped);
    EXPECT_EQ(r.orient, XR_PLANE_VERTICAL);
}

// ---- the merger: synthetic first, the runtime source after, UNAVAILABLE drops its planes ----

class CFakeRuntimeSource : public IXRPlaneSource {
  public:
    eXRPlanePoll          next = XR_PLANE_POLL_UNCHANGED;
    std::vector<SXRPlane> planes;
    bool                  shut = false;
    const char*           name() const override {
        return "fake";
    }
    eXRPlanePoll poll(const SXRPlanePollContext&, std::vector<SXRPlane>& out) override {
        if (next == XR_PLANE_POLL_UPDATED)
            out = planes;
        return next;
    }
    std::string status() const override {
        return "fake";
    }
    void shutdown(bool) override {
        shut = true;
    }
};

TEST(Surfaces, SourcesMergeRuntimeAndSyntheticAndDropUnavailable) {
    CXRSurfaceSources src;
    ASSERT_TRUE(src.setSyntheticSpec("table pos:0,0.74,-0.7 size:1.2,0.7"));
    auto     fakeUP = makeUnique<CFakeRuntimeSource>();
    auto*    fake   = fakeUP.get();
    SXRPlane floor;
    floor.id     = 42;
    floor.source = XR_PLANE_SRC_DETECTION;
    fake->planes = {floor};
    fake->next   = XR_PLANE_POLL_UPDATED;
    src.setRuntimeSource(std::move(fakeUP));

    SXRPlaneSnapshot    s;
    SXRPlanePollContext ctx;
    ASSERT_TRUE(src.poll(ctx, s));
    ASSERT_EQ(s.n, 2);
    EXPECT_EQ(s.planes[0].source, XR_PLANE_SRC_SYNTHETIC);
    EXPECT_EQ(s.planes[1].id, 42U);
    const auto gen = s.generation;

    // nothing new from either: no publish
    fake->next = XR_PLANE_POLL_UNCHANGED;
    EXPECT_FALSE(src.poll(ctx, s));
    // a reference-space change: everything is re-published (a new generation)
    src.invalidate();
    ASSERT_TRUE(src.poll(ctx, s));
    EXPECT_GT(s.generation, gen);
    // the runtime source becomes unavailable: its planes go, the synthetic ones stay
    fake->next = XR_PLANE_POLL_UNAVAILABLE;
    ASSERT_TRUE(src.poll(ctx, s));
    EXPECT_EQ(s.n, 1);
    // session end: shut down and detached
    src.detachRuntime(false);
    EXPECT_NE(src.runtimeStatus().find("none"), std::string::npos);
}

// ---- the anchor: preview while carried, snap on release ----

static SXRSolveInput solveAt(const SXRPose& grip) {
    SXRSolveInput in;
    in.view     = SXRPose{Vec3{0.F, 1.6F, 0.F}, Quat{}};
    in.dt       = 1.F / 90.F;
    in.gripLeft = grip;
    in.pxW      = 1600;
    in.pxH      = 900;
    return in;
}

// A local monitor at eye height, grabbed by the left grip held 0.6 m in front of it.
static CXRAnchor grabbedAnchor(SXRPose& grip) {
    CXRAnchor      a;
    SXRAnchorState st;
    st.mode        = XR_ANCHOR_LOCAL;
    st.anchorPose  = SXRPose{Vec3{0.F, 1.2F, -0.7F}, Quat{}};
    st.widthMeters = 1.F;
    a.initFromState(st);
    grip = SXRPose{Vec3{0.F, 1.2F, -0.1F}, Quat{}};
    a.solve(solveAt(grip), {});
    a.beginGrab(XR_HAND_LEFT, grip);
    return a;
}

static SXRSurfaceArm armFor(const CXRAnchor& a, const SXRPlane& t) {
    SXRSurfaceArm arm;
    arm.query.content   = a.carryWorld();
    arm.query.halfWidth = 0.5F;
    arm.query.bottomExt = 0.35F;
    arm.query.head      = Vec3{0.F, 1.6F, 0.F};
    arm.query.headValid = true;
    const auto c        = xrSurfacePick(&t, 1, arm.query, arm.tuning, 0);
    arm.havePlane       = true;
    arm.plane           = t;
    arm.preview         = c.ok;
    arm.previewPose     = c.pose;
    arm.previewKind     = c.kind;
    return arm;
}

TEST(Surfaces, AnchorPreviewsWhileCarriedAndSnapsOnRelease) {
    const auto t = table(0.74F);
    SXRPose    grip;
    CXRAnchor  a = grabbedAnchor(grip);

    // carry it down to 4 cm above the table: bottom edge at 0.78
    grip.pos.y = 0.78F + 0.35F;
    a.solve(solveAt(grip), {});
    const SXRPose carried = a.carryWorld();
    EXPECT_NEAR(carried.pos.y, 1.13F, 1e-4F);
    a.setSurfaceArm(armFor(a, t));
    ASSERT_TRUE(a.surfaceArm().preview);

    // the next solve SHOWS the snapped pose, in LOCAL_FLOOR, while the carry keeps tracking the grip
    const auto res = a.solve(solveAt(grip), {});
    EXPECT_EQ(res.space, XR_SPACE_LOCAL_FLOOR);
    expectVecNear(res.worldPose.pos, a.surfaceArm().previewPose.pos);
    expectVecNear(a.carryWorld().pos, carried.pos);
    EXPECT_GT(std::fabs(a.lastWorld().pos.y - carried.pos.y), 1e-3F);

    // release from the carried pose: snapped, local, and the snap is reported exactly once
    a.endGrab(a.carryWorld(), solveAt(grip), {});
    EXPECT_FALSE(a.grabbed());
    EXPECT_EQ(a.state().mode, XR_ANCHOR_LOCAL);
    SXRSurfaceCandidate snap;
    ASSERT_TRUE(a.takeSurfaceSnap(snap));
    EXPECT_FALSE(a.takeSurfaceSnap(snap));
    EXPECT_EQ(snap.planeId, t.id);
    EXPECT_EQ(snap.kind, XR_SNAP_STAND);
    expectVecNear(a.state().anchorPose.pos, snap.pose.pos);
    // the bottom edge rests on the table
    const Vec3 up = qRotate(a.state().anchorPose.rot, Vec3{0.F, 1.F, 0.F});
    EXPECT_NEAR((a.state().anchorPose.pos - up * 0.35F).y, 0.74F + 0.002F, 1e-3F);
    EXPECT_FALSE(a.surfaceArm().havePlane);
}

TEST(Surfaces, AnchorReleaseFarAwayOrAbortDoesNotSnap) {
    const auto t = table(0.74F);
    {
        SXRPose   grip;
        CXRAnchor a = grabbedAnchor(grip);
        grip.pos.y  = 0.78F + 0.35F;
        a.solve(solveAt(grip), {});
        a.setSurfaceArm(armFor(a, t));
        // the release jerk carried the latched pose 30 cm up: no snap, it stays where it was let go
        SXRPose rel = a.carryWorld();
        rel.pos.y += 0.30F;
        a.endGrab(rel, solveAt(grip), {});
        SXRSurfaceCandidate snap;
        EXPECT_FALSE(a.takeSurfaceSnap(snap));
        expectVecNear(a.state().anchorPose.pos, rel.pos);
    }
    {
        SXRPose   grip;
        CXRAnchor a = grabbedAnchor(grip);
        grip.pos.y  = 0.78F + 0.35F;
        a.solve(solveAt(grip), {});
        a.setSurfaceArm(armFor(a, t));
        // `openxr view off` aborts the grab: the hand never let go, nothing snaps
        EXPECT_TRUE(a.abortGrab(solveAt(grip), {}));
        SXRSurfaceCandidate snap;
        EXPECT_FALSE(a.takeSurfaceSnap(snap));
        EXPECT_FALSE(a.surfaceArm().havePlane);
    }
}

TEST(Surfaces, GazeCarryReleaseSnapsFromTheCarriedPose) {
    const auto     t = table(0.74F);
    CXRAnchor      a;
    SXRAnchorState st;
    st.mode       = XR_ANCHOR_LOCAL;
    st.anchorPose = SXRPose{Vec3{0.F, 1.08F, -0.7F}, Quat{}}; // tilted toward the head: bottom edge ~5 cm up
    a.initFromState(st);
    SXRSolveInput in = solveAt(SXRPose{});
    // look at the spot the monitor is at, so the carry keeps it there
    in.view.rot = lookAtNoRoll(in.view.pos, in.view.pos + (in.view.pos - st.anchorPose.pos), Quat{});
    a.solve(in, {});
    a.beginGazeGrab(in.view, true);
    a.solve(in, {});
    a.setSurfaceArm(armFor(a, t));
    ASSERT_TRUE(a.surfaceArm().preview);
    a.solve(in, {});
    a.endGazeGrab(in, {});
    EXPECT_FALSE(a.gazeGrabbed());
    SXRSurfaceCandidate snap;
    ASSERT_TRUE(a.takeSurfaceSnap(snap));
    EXPECT_EQ(snap.planeId, t.id);
}

// ---- the live repro (2026-10-16, XREAL Air 2 Ultra + Monado XR_EXT_plane_detection) ----
// Status showed a PLATFORM plane 0x42 at [0.65, 1.13, -0.54], 1.68 x 1.23 m, floor patches at
// y ~0.70, and XR-main (1.32 m wide, 2560x1600) anchored local at ~[0.3, 1.45, -1.4]. Its visible
// bottom edge hung ~0.27 m BELOW the table top from the start, so once over the table the window
// was always "in" it — and the old fixed 0.25 m penetration limit turned exactly that away.

static const Vec3 LIVE_HEAD{0.25F, 1.55F, 0.05F};

// Plane 0x42 as XRPlaneDetection builds it from a Monado location: a +Z-normal pose (Basalt's
// z-up plane frame through xr_world), the outer contour counter-clockwise about +Z.
static SXRPlane liveTable() {
    const SXRPose raw{Vec3{0.65F, 1.13F, -0.54F}, qFromPitch(-90.F * DEG)};
    const SXRVec2 contour[] = {{-0.84F, -0.615F}, {0.84F, -0.615F}, {0.84F, 0.615F}, {-0.84F, 0.615F}};
    bool          swapped   = true;
    SXRPlane      p         = xrCanonicalPlane(raw, XR_PLANE_UP, contour, 4, 1.68F, 1.23F, swapped);
    EXPECT_FALSE(swapped);
    p.id     = 0x42;
    p.label  = XR_PLANE_LABEL_PLATFORM;
    p.source = XR_PLANE_SRC_DETECTION;
    return p;
}

static SXRPlane liveFloor() {
    const SXRPose raw{Vec3{0.3F, 0.70F, -1.0F}, qFromPitch(-90.F * DEG)};
    const SXRVec2 contour[] = {{-2.F, -2.F}, {2.F, -2.F}, {2.F, 2.F}, {-2.F, 2.F}};
    bool          swapped   = true;
    SXRPlane      p         = xrCanonicalPlane(raw, XR_PLANE_UP, contour, 4, 4.F, 4.F, swapped);
    p.id                    = 0x7;
    p.label                 = XR_PLANE_LABEL_FLOOR;
    p.source                = XR_PLANE_SRC_DETECTION;
    return p;
}

// XR-main as the frame pass queries it: 1.32 m x 0.825 m of content plus the default chrome
// (0.10 m margin; the bottom margin also holds the 0.08 m bar), facing the head, gazed at.
static constexpr float LIVE_BOTTOM_EXT = 0.825F * 0.5F + 0.18F;

static SXRSurfaceQuery liveWindow(const Vec3& center) {
    SXRSurfaceQuery q;
    q.content   = SXRPose{center, lookAtNoRoll(center, LIVE_HEAD, Quat{})};
    q.halfWidth = (1.32F + 0.2F) * 0.5F;
    q.bottomExt = LIVE_BOTTOM_EXT;
    q.head      = LIVE_HEAD;
    q.headValid = true;
    q.haveRay   = true;
    q.rayOrigin = LIVE_HEAD;
    q.rayDir    = (center - LIVE_HEAD).normalized();
    return q;
}

TEST(Surfaces, LiveTableTakesTheBigWindowLoweredThroughIt) {
    const auto       t = liveTable();
    SXRSurfaceTuning tun;

    // Carried over the table where the user had it: center 0.32 m above the top, bottom edge
    // ~0.27 m under it. The old rule (lowest < -maxPenetrate) said no; the window straddles the table.
    SXRSurfaceDiag d;
    const auto     c = xrSurfaceEvaluate(t, liveWindow(Vec3{0.5F, 1.45F, -0.8F}), tun, tun.snapDist, &d);
    EXPECT_LT(d.lowest, -tun.maxPenetrate);
    EXPECT_GT(d.centerH, 0.F);
    EXPECT_TRUE(d.contained);
    ASSERT_TRUE(c.ok) << xrSurfaceVerdictName(d.verdict);
    EXPECT_EQ(c.kind, XR_SNAP_STAND);
    EXPECT_NEAR(c.contact.y, 1.13F, 1e-3F);

    // Lowered through the table in 1 cm steps, as the frame pass sees it (sticky once previewed):
    // the preview starts with the bottom edge 8 cm above the top and holds until the window's
    // center goes under it — no gap in between.
    uint64_t sticky = 0;
    float    first = -1.F, last = -1.F;
    bool     gap = false;
    for (int i = 0; i <= 100; ++i) {
        const float y  = 1.95F - 0.01F * sc<float>(i);
        const auto  pc = xrSurfacePick(&t, 1, liveWindow(Vec3{0.5F, y, -0.8F}), tun, sticky);
        if (pc.ok) {
            if (first < 0.F)
                first = y;
            else if (sticky == 0)
                gap = true;
            last = y;
        }
        sticky = pc.ok ? pc.planeId : 0;
    }
    EXPECT_FALSE(gap);
    EXPECT_NEAR(first, 1.13F + 0.08F + LIVE_BOTTOM_EXT, 0.04F); // less the window's pitch toward the head
    EXPECT_NEAR(last, 1.13F, 0.02F);

    // ...and a release anywhere in there lands it on the table, bottom edge on the top.
    SXRSurfaceArm arm;
    arm.havePlane = true;
    arm.plane     = t;
    arm.query     = liveWindow(Vec3{0.5F, 1.30F, -0.8F});
    const auto r  = xrSurfaceRelease(arm, arm.query.content, LIVE_HEAD, true);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.planeId, 0x42U);
    const Vec3 up = qRotate(r.pose.rot, Vec3{0.F, 1.F, 0.F});
    EXPECT_NEAR((r.pose.pos - up * LIVE_BOTTOM_EXT).y, 1.13F + tun.lift, 1e-3F);

    // Under the table (center below its top, and deeper than maxPenetrate): not on it.
    SXRSurfaceDiag under;
    EXPECT_FALSE(xrSurfaceEvaluate(t, liveWindow(Vec3{0.5F, 1.05F, -0.8F}), tun, tun.snapDist, &under).ok);
    EXPECT_EQ(under.verdict, XR_SURF_TOO_DEEP);
}

TEST(Surfaces, LiveTableBeatsTheFloorUnderIt) {
    const SXRPlane   planes[] = {liveFloor(), liveTable()};
    SXRSurfaceTuning tun;
    // Bottom edge 2 cm over the floor patch, center well above the table top: both qualify and the
    // floor is the nearer, but it is under the table.
    const auto q = liveWindow(Vec3{0.5F, 0.72F + LIVE_BOTTOM_EXT, -0.8F});
    ASSERT_TRUE(xrSurfaceEvaluate(planes[0], q, tun, tun.snapDist).ok);
    const auto c = xrSurfacePick(planes, 2, q, tun, 0);
    ASSERT_TRUE(c.ok);
    EXPECT_EQ(c.planeId, 0x42U);
    // Past the table's far edge only the floor is under it.
    const auto off = xrSurfacePick(planes, 2, liveWindow(Vec3{0.5F, 0.72F + LIVE_BOTTOM_EXT, -1.6F}), tun, 0);
    ASSERT_TRUE(off.ok);
    EXPECT_EQ(off.planeId, 0x7U);
}

TEST(Surfaces, LiveKeyboardNudgeThroughTheTableSettlesOnIt) {
    // The logged sequence: SUPER+ALT+minus x10 (`gazepush -0.1`) with NO carry active — a nudge of
    // the gazed-at monitor along the head->monitor ray — pulling XR-main from beyond the table's far
    // edge in over it, then SUPER+SHIFT+G. Each nudge is now a one-step carry the frame pass settles
    // (requestSurfaceSettle -> takeSurfaceSettle -> pick -> surfaceSettleAt, as below).
    const auto       t = liveTable();
    SXRSurfaceTuning tun;
    CXRAnchor        a;
    SXRAnchorState   st;
    st.mode        = XR_ANCHOR_LOCAL;
    st.widthMeters = 1.32F;
    st.anchorPose  = SXRPose{Vec3{0.3F, 1.45F, -1.4F}, lookAtNoRoll(Vec3{0.3F, 1.45F, -1.4F}, LIVE_HEAD, Quat{})};
    a.initFromState(st);
    SXRSolveInput in;
    in.view = SXRPose{LIVE_HEAD, lookAtNoRoll(LIVE_HEAD, LIVE_HEAD + (LIVE_HEAD - st.anchorPose.pos), Quat{})};
    in.dt   = 1.F / 90.F;
    in.pxW  = 2560;
    in.pxH  = 1600;
    a.solve(in, {});
    const SXRVerbContext ctx{in.view, true, std::nullopt, std::nullopt};

    // Where it started it is not over the table (whose far edge is at z ~ -1.16).
    SXRSurfaceDiag d0;
    EXPECT_FALSE(xrSurfaceEvaluate(t, liveWindow(a.state().anchorPose.pos), tun, tun.snapDist, &d0).ok);
    EXPECT_EQ(d0.verdict, XR_SURF_OUTSIDE);

    int steps = 0;
    for (; steps < 10; ++steps) {
        ASSERT_TRUE(a.applyDistance(-0.1F, ctx));
        a.requestSurfaceSettle();
        a.solve(in, {});
        // the frame pass: once per request, the anchor's own world pose against the planes
        ASSERT_TRUE(a.takeSurfaceSettle());
        EXPECT_FALSE(a.takeSurfaceSettle());
        SXRSurfaceQuery q = liveWindow(a.state().anchorPose.pos);
        q.content         = a.state().anchorPose;
        if (const auto c = xrSurfacePick(&t, 1, q, tun, 0); c.ok) {
            a.surfaceSettleAt(c);
            break;
        }
    }
    ASSERT_LT(steps, 10) << "ten nudges through the table never settled the window on it";
    EXPECT_LE(steps, 4);
    SXRSurfaceCandidate snap;
    ASSERT_TRUE(a.takeSurfaceSnap(snap));
    EXPECT_FALSE(a.takeSurfaceSnap(snap));
    EXPECT_EQ(snap.planeId, 0x42U);
    EXPECT_EQ(snap.kind, XR_SNAP_STAND);
    EXPECT_EQ(a.state().mode, XR_ANCHOR_LOCAL);
    expectVecNear(a.state().anchorPose.pos, snap.pose.pos);
    const Vec3 up = qRotate(a.state().anchorPose.rot, Vec3{0.F, 1.F, 0.F});
    EXPECT_NEAR((a.state().anchorPose.pos - up * LIVE_BOTTOM_EXT).y, 1.13F + tun.lift, 1e-3F);
}

// ---- clutter (the live set: 21 planes, duplicates, "unknown" patches 1.6-2.3 m up, sofa arms) ----

// A detected upward patch, centred at `c`, w x d metres, as the runtime reports it.
static SXRPlane detectedPatch(uint64_t id, const Vec3& c, float w, float d) {
    const SXRVec2 contour[] = {{-w * 0.5F, -d * 0.5F}, {w * 0.5F, -d * 0.5F}, {w * 0.5F, d * 0.5F}, {-w * 0.5F, d * 0.5F}};
    bool          swapped   = false;
    SXRPlane      p         = xrCanonicalPlane(SXRPose{c, qFromPitch(-90.F * DEG)}, XR_PLANE_UP, contour, 4, w, d, swapped);
    p.id                    = id;
    p.source                = XR_PLANE_SRC_DETECTION;
    return p;
}

TEST(Surfaces, ClutterSmallAndHighDetectedPatchesAreIgnored) {
    const SXRSurfaceQuery q = liveWindow(Vec3{0.5F, 1.45F, -0.8F}); // head at y 1.55

    // the real table is not clutter
    EXPECT_EQ(xrPlaneClutter(liveTable(), q), XR_SURF_OK);
    EXPECT_EQ(xrPlaneClutter(liveFloor(), q), XR_SURF_OK);
    EXPECT_NEAR(xrPlaneArea(liveTable()), 1.68F * 1.23F, 1e-4F);

    // too small: by area, and by a short side under 20 cm even with enough area (a sofa arm)
    EXPECT_EQ(xrPlaneClutter(detectedPatch(1, Vec3{0.F, 0.9F, -1.F}, 0.25F, 0.25F), q), XR_SURF_CLUTTER_SMALL);
    EXPECT_EQ(xrPlaneClutter(detectedPatch(2, Vec3{0.F, 0.9F, -1.F}, 0.9F, 0.15F), q), XR_SURF_CLUTTER_SMALL);
    EXPECT_EQ(xrPlaneClutter(detectedPatch(3, Vec3{0.F, 0.9F, -1.F}, 0.4F, 0.3F), q), XR_SURF_OK);

    // at or above eye level: the "unknown" patches 1.6-2.3 m up, and one just under the eyes
    EXPECT_EQ(xrPlaneClutter(detectedPatch(4, Vec3{0.F, 2.1F, -1.F}, 1.F, 1.F), q), XR_SURF_CLUTTER_HIGH);
    EXPECT_EQ(xrPlaneClutter(detectedPatch(5, Vec3{0.F, 1.50F, -1.F}, 1.F, 1.F), q), XR_SURF_CLUTTER_HIGH);
    EXPECT_EQ(xrPlaneClutter(detectedPatch(6, Vec3{0.F, 1.40F, -1.F}, 1.F, 1.F), q), XR_SURF_OK);
    // ...a rule about the head: without one, only the size rules apply
    SXRSurfaceQuery noHead = q;
    noHead.headValid       = false;
    EXPECT_EQ(xrPlaneClutter(detectedPatch(4, Vec3{0.F, 2.1F, -1.F}, 1.F, 1.F), noHead), XR_SURF_OK);

    // a declared plane is taken as declared, however small or high
    auto tiny = xrParseSyntheticPlanes("table pos:0,2.1,-1 size:0.1,0.1");
    ASSERT_TRUE(tiny.has_value());
    EXPECT_EQ(xrPlaneClutter(tiny->at(0), q), XR_SURF_OK);
}

TEST(Surfaces, ClutterNeverWinsThePick) {
    SXRSurfaceTuning tun;
    // A narrow patch 4 cm above the table right under the window's bottom edge (which the window's
    // pitch toward the head puts at z ~ -0.92) — nearer than the table, and higher — is clutter:
    // the table under the window is still where it goes.
    const SXRPlane planes[] = {detectedPatch(9, Vec3{0.5F, 1.17F, -0.92F}, 0.9F, 0.15F), liveTable()};
    const auto     q        = liveWindow(Vec3{0.5F, 1.17F + LIVE_BOTTOM_EXT, -0.8F});
    ASSERT_TRUE(xrSurfaceEvaluate(planes[1], q, tun, tun.snapDist).ok);
    SXRSurfaceDiag d;
    ASSERT_TRUE(xrSurfaceEvaluate(planes[0], q, tun, tun.snapDist, &d).ok); // geometrically it would
    const auto c = xrSurfacePick(planes, 2, q, tun, 0);
    ASSERT_TRUE(c.ok);
    EXPECT_EQ(c.planeId, 0x42U);
    // with only clutter around, nothing snaps
    EXPECT_FALSE(xrSurfacePick(planes, 1, q, tun, 0).ok);
}
