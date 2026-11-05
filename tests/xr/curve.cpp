#include <openxr/XRCurve.hpp>
#include <openxr/XRDefaults.hpp>
#include <openxr/XRMonitorConfig.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <optional>
#include <utility>

using namespace OpenXR;

// tests/xr/curve.cpp — curved (cylindrical) XR monitors: the `curve` value, the radius/angle
// derivation and clamps, ray -> cylinder hits and their uv, the uv -> point inverse, and the
// promise that a flat monitor takes exactly the old code path. Pure math, no runtime.

static constexpr float PI = 3.14159265358979323846F;

// A surface whose centre is 1.5 m straight ahead of the origin, facing it.
static SXRPose ahead(float dist = 1.5F) {
    return SXRPose{Vec3{0.F, 0.F, -dist}, Quat{}};
}

// Unit direction at yaw `yawRad` (positive = towards -X, i.e. turning left) in the horizontal plane.
static Vec3 dirYaw(float yawRad) {
    return Vec3{-std::sin(yawRad), 0.F, -std::cos(yawRad)};
}

// ---- the config value ----

TEST(XRCurve, ParseKeywordsAndRadius) {
    EXPECT_EQ(parseXRCurve("auto")->mode, XR_CURVE_AUTO);
    EXPECT_EQ(parseXRCurve(" Auto ")->mode, XR_CURVE_AUTO);
    EXPECT_EQ(parseXRCurve("true")->mode, XR_CURVE_AUTO);
    EXPECT_EQ(parseXRCurve("flat")->mode, XR_CURVE_FLAT);
    EXPECT_EQ(parseXRCurve("off")->mode, XR_CURVE_FLAT);
    EXPECT_EQ(parseXRCurve("false")->mode, XR_CURVE_FLAT);
    EXPECT_EQ(parseXRCurve("0")->mode, XR_CURVE_FLAT); // OpenXR's own infinite-radius spelling

    const auto R = parseXRCurve("2.5");
    ASSERT_TRUE(R.has_value());
    EXPECT_EQ(R->mode, XR_CURVE_RADIUS);
    EXPECT_FLOAT_EQ(R->radius, 2.5F);
}

TEST(XRCurve, ParseRejectsJunkAndOutOfRange) {
    EXPECT_FALSE(parseXRCurve("").has_value());
    EXPECT_FALSE(parseXRCurve("bendy").has_value());
    EXPECT_FALSE(parseXRCurve("2.5m").has_value());
    EXPECT_FALSE(parseXRCurve("-1").has_value());
    EXPECT_FALSE(parseXRCurve("0.1").has_value());  // tighter than XR_CURVE_RADIUS_MIN
    EXPECT_FALSE(parseXRCurve("1000").has_value()); // flatter than XR_CURVE_RADIUS_MAX
    EXPECT_FALSE(parseXRCurve("nan").has_value());
}

TEST(XRCurve, NameRoundTrips) {
    for (const char* s : {"flat", "auto", "2.5", "1.9", "0.3"}) {
        const auto SPEC = parseXRCurve(s);
        ASSERT_TRUE(SPEC.has_value()) << s;
        EXPECT_EQ(xrCurveName(*SPEC), s);
        EXPECT_EQ(*parseXRCurve(xrCurveName(*SPEC)), *SPEC);
    }
}

// ---- radius / angle derivation ----

TEST(XRCurve, BaseRadiusPerMode) {
    EXPECT_EQ(xrCurveBaseRadius(SXRCurveSpec{}, 1.9F, 1.5F), 0.F);
    EXPECT_FLOAT_EQ(xrCurveBaseRadius(SXRCurveSpec{.mode = XR_CURVE_RADIUS, .radius = 2.5F}, 1.9F, 1.5F), 2.5F);
    // auto: the frozen radius once resolved, the fallback before
    EXPECT_FLOAT_EQ(xrCurveBaseRadius(SXRCurveSpec{.mode = XR_CURVE_AUTO}, 1.9F, 1.5F), 1.9F);
    EXPECT_FLOAT_EQ(xrCurveBaseRadius(SXRCurveSpec{.mode = XR_CURVE_AUTO}, 0.F, 1.5F), 1.5F);
    // never below the minimum, whatever the fallback
    EXPECT_FLOAT_EQ(xrCurveBaseRadius(SXRCurveSpec{.mode = XR_CURVE_AUTO}, 0.F, 0.F), XR_CURVE_RADIUS_MIN);
}

TEST(XRCurve, AutoRadiusIsHeadDistanceClamped) {
    EXPECT_NEAR(xrCurveAutoRadius(Vec3{0.F, 1.2F, 0.F}, Vec3{0.F, 1.2F, -1.9F}), 1.9F, 1e-5F);
    EXPECT_NEAR(xrCurveAutoRadius(Vec3{0.F, 0.F, 0.F}, Vec3{3.F, 0.F, -4.F}), 5.F, 1e-5F);
    EXPECT_FLOAT_EQ(xrCurveAutoRadius(Vec3{}, Vec3{0.F, 0.F, -0.05F}), XR_CURVE_RADIUS_MIN);
}

TEST(XRCurve, EffectiveRadiusClampsTheAngle) {
    // A 3.0 m arc at R 1.9 is ~90 degrees: untouched.
    EXPECT_FLOAT_EQ(xrCurveEffectiveRadius(1.9F, 3.0F), 1.9F);
    EXPECT_NEAR(xrCurveCentralAngle(1.9F, 3.0F) * 180.F / PI, 90.47F, 0.05F);
    // A 4 m arc at R 1.0 would be 229 degrees: R grows until it is exactly 180.
    const float R = xrCurveEffectiveRadius(1.0F, 4.0F);
    EXPECT_NEAR(R, 4.0F / PI, 1e-5F);
    EXPECT_NEAR(xrCurveCentralAngle(R, 4.0F), PI, 1e-5F);
    // flat stays flat; the minimum holds
    EXPECT_EQ(xrCurveEffectiveRadius(0.F, 3.0F), 0.F);
    EXPECT_FLOAT_EQ(xrCurveEffectiveRadius(0.1F, 0.2F), XR_CURVE_RADIUS_MIN);
}

TEST(XRCurve, CylinderLayerParameters) {
    const auto G = xrCylinderFromSurface(ahead(1.9F), 3.0F, 0.84F, 1.9F);
    // The layer pose sits on the axis: the head, for an auto curve seen from the origin.
    EXPECT_NEAR(G.pose.pos.x, 0.F, 1e-5F);
    EXPECT_NEAR(G.pose.pos.z, 0.F, 1e-5F);
    EXPECT_NEAR(G.centralAngle, 3.0F / 1.9F, 1e-5F);
    EXPECT_NEAR(G.aspectRatio, 3.0F / 0.84F, 1e-5F);
    // and its height formula (radius * angle / aspect) gives back the surface height
    EXPECT_NEAR(G.radius * G.centralAngle / G.aspectRatio, 0.84F, 1e-5F);
}

// ---- ray -> cylinder ----

TEST(XRCurve, HitAtCentre) {
    const auto HIT = rayMonitorIntersect(ahead(), Vec3{}, Vec3{0.F, 0.F, -1.F}, 2.0F, 0.6F, 1.5F);
    ASSERT_TRUE(HIT.hit);
    EXPECT_NEAR(HIT.t, 1.5F, 1e-5F);
    EXPECT_NEAR(HIT.u, 0.5F, 1e-5F);
    EXPECT_NEAR(HIT.v, 0.5F, 1e-5F);
}

TEST(XRCurve, EveryColumnIsEquidistantFromTheAxis) {
    // The point of `auto`: from the axis, every direction across the arc is the same distance away.
    // A flat quad of the same width is 1/cos(yaw) farther at the side.
    const float W = 2.4F, H = 0.7F, R = 1.5F;
    for (float yawDeg : {-40.F, -20.F, 0.F, 15.F, 40.F}) {
        const float yaw = yawDeg * PI / 180.F;
        const auto  HIT = rayMonitorIntersect(ahead(R), Vec3{}, dirYaw(yaw), W, H, R);
        ASSERT_TRUE(HIT.hit) << yawDeg;
        EXPECT_NEAR(HIT.t, R, 1e-4F) << yawDeg;
        // u is arc length: yawing LEFT (+yaw) moves towards u = 0
        EXPECT_NEAR(HIT.u, 0.5F - yaw * R / W, 1e-4F) << yawDeg;
        EXPECT_NEAR(HIT.v, 0.5F, 1e-4F) << yawDeg;
    }
}

TEST(XRCurve, EdgeAndOutsideTheArc) {
    const float W = 2.4F, H = 0.7F, R = 1.5F;
    const float halfAngle = W * 0.5F / R; // ~45.8 deg
    // Just inside the right end hits near u = 1; just outside misses, slack rescues it.
    const auto IN = rayMonitorIntersect(ahead(R), Vec3{}, dirYaw(-(halfAngle - 0.01F)), W, H, R);
    ASSERT_TRUE(IN.hit);
    EXPECT_NEAR(IN.u, 1.F - 0.01F * R / W, 1e-4F);
    const auto OUT = rayMonitorIntersect(ahead(R), Vec3{}, dirYaw(-(halfAngle + 0.02F)), W, H, R);
    EXPECT_FALSE(OUT.hit);
    const auto SLACK = rayMonitorIntersect(ahead(R), Vec3{}, dirYaw(-(halfAngle + 0.02F)), W, H, R, 0.05F);
    ASSERT_TRUE(SLACK.hit);
    EXPECT_GT(SLACK.u, 1.F);
    // Above the top edge misses.
    const auto UP = rayMonitorIntersect(ahead(R), Vec3{}, Vec3{0.F, 0.5F, -1.F}.normalized(), W, H, R);
    EXPECT_FALSE(UP.hit);
}

TEST(XRCurve, BehindAndOutsideFaceMiss) {
    const float W = 2.4F, H = 0.7F, R = 1.5F;
    // Facing away from the monitor: the arc is behind the ray.
    EXPECT_FALSE(rayMonitorIntersect(ahead(R), Vec3{}, Vec3{0.F, 0.F, 1.F}, W, H, R).hit);
    // Standing BEHIND the monitor looking at it: that is the outside (convex) face — no hit, and the
    // far side of the cylinder is outside the arc.
    EXPECT_FALSE(rayMonitorIntersect(ahead(R), Vec3{0.F, 0.F, -3.F}, Vec3{0.F, 0.F, 1.F}, W, H, R).hit);
    // Parallel to the axis never meets the curved face.
    EXPECT_FALSE(rayMonitorIntersect(ahead(R), Vec3{0.F, 0.F, -1.F}, Vec3{0.F, 1.F, 0.F}, W, H, R).hit);
}

TEST(XRCurve, FromOffAxisViewerStillHitsTheInsideFace) {
    // Leaning 0.4 m right of the axis: still inside the cylinder, still the arc.
    const float W = 2.4F, H = 0.7F, R = 1.5F;
    const auto  HIT = rayMonitorIntersect(ahead(R), Vec3{0.4F, 0.F, 0.F}, Vec3{0.F, 0.F, -1.F}, W, H, R);
    ASSERT_TRUE(HIT.hit);
    EXPECT_NEAR(HIT.t, std::sqrt(R * R - 0.16F), 1e-4F);
    EXPECT_GT(HIT.u, 0.5F);
    // Stepped back beyond the axis (outside the cylinder): the ray enters through the far side and
    // the hit is still the inside face of the arc.
    const auto BACK = rayMonitorIntersect(ahead(R), Vec3{0.F, 0.F, 1.0F}, Vec3{0.F, 0.F, -1.F}, W, H, R);
    ASSERT_TRUE(BACK.hit);
    EXPECT_NEAR(BACK.t, 2.5F, 1e-4F);
    EXPECT_NEAR(BACK.u, 0.5F, 1e-4F);
}

TEST(XRCurve, PitchTiltsTheAxis) {
    // Pitched down 20 deg about the centre: from the (tilted) axis the hit is still R away and v
    // follows the tilted vertical.
    const float   R = 1.5F, W = 2.0F, H = 0.8F;
    const float   pitch = -20.F * PI / 180.F;
    const SXRPose P{Vec3{0.F, -R * std::sin(-pitch), -R * std::cos(pitch)}, qFromPitch(pitch)};
    const auto    HIT = rayMonitorIntersect(P, Vec3{}, poseForward(P.rot), W, H, R);
    ASSERT_TRUE(HIT.hit);
    EXPECT_NEAR(HIT.t, R, 1e-4F);
    EXPECT_NEAR(HIT.u, 0.5F, 1e-4F);
    EXPECT_NEAR(HIT.v, 0.5F, 1e-4F);
}

// ---- uv <-> point ----

TEST(XRCurve, UVPointRoundTrip) {
    const float   W = 3.0F, H = 0.84F, R = 1.9F;
    const SXRPose P{Vec3{0.3F, 1.2F, -1.7F}, qMul(qFromYaw(0.4F), qFromPitch(-0.1F))};
    const Vec3    eye = xrCylinderFromSurface(P, W, H, R).pose.pos; // on the axis
    for (float u : {0.02F, 0.25F, 0.5F, 0.8F, 0.98F}) {
        for (float v : {0.05F, 0.5F, 0.9F}) {
            const Vec3 pt  = monitorPointFromUV(P, W, H, R, u, v);
            const auto HIT = rayMonitorIntersect(P, eye, (pt - eye).normalized(), W, H, R);
            ASSERT_TRUE(HIT.hit) << u << "," << v;
            EXPECT_NEAR(HIT.u, u, 1e-4F);
            EXPECT_NEAR(HIT.v, v, 1e-4F);
            EXPECT_NEAR(HIT.t, (pt - eye).length(), 1e-4F);
        }
    }
}

TEST(XRCurve, PointsLieOnTheCylinderAndNormalsFaceTheAxis) {
    const float   W = 2.4F, H = 0.7F, R = 1.5F;
    const SXRPose P = ahead(R);
    for (float u : {0.F, 0.3F, 0.5F, 1.F}) {
        const Vec3 pt = monitorPointFromUV(P, W, H, R, u, 0.5F);
        EXPECT_NEAR(std::sqrt(pt.x * pt.x + pt.z * pt.z), R, 1e-4F) << u;
        const Vec3 n = monitorNormalAtUV(P, W, R, u);
        EXPECT_NEAR(n.length(), 1.F, 1e-5F);
        // the normal points from the surface point straight at the (origin) axis
        EXPECT_NEAR(n.dot((Vec3{} - pt).normalized()), 1.F, 1e-4F) << u;
    }
}

TEST(XRCurve, OvershootContinuesAlongTheTangent) {
    // Past the end the inverse leaves the cylinder along the end's tangent, so a big overshoot on a
    // wide arc keeps heading outwards instead of curling round behind the viewer.
    const float   W = 4.0F, H = 0.7F, R = 4.0F / PI; // a 180-degree arc
    const SXRPose P    = ahead(R);
    const Vec3    end  = monitorPointFromUV(P, W, H, R, 1.F, 0.5F);
    const Vec3    over = monitorPointFromUV(P, W, H, R, 1.5F, 0.5F);
    EXPECT_NEAR((over - end).length(), 0.5F * W, 1e-4F);
    // the right end of a 180-degree arc is at x = +R beside the viewer; the tangent there runs +z
    EXPECT_NEAR(end.x, R, 1e-4F);
    EXPECT_NEAR(over.x, R, 1e-4F);
    EXPECT_GT(over.z, end.z);
}

TEST(XRCurve, ContentPoseToSurfaceCenterKeepsTheAxis) {
    // With symmetric side margins the content centre and the surface centre share a column, and the
    // vertical chrome offset is along the axis — so the surface centre is just shifted along +Y.
    const auto    G     = makeChromeGeometry(2.0F, 0.6F, 0.05F, 0.04F, 0.3F, 0.05F);
    const float   quadW = 2.0F / G.contentFracW(), quadH = 0.6F / G.contentFracH();
    const SXRPose C     = ahead(1.5F);
    const SXRPose flat  = contentPoseToQuadCenter(C, G, quadW, quadH);
    const SXRPose curve = contentPoseToSurfaceCenter(C, G, quadW, quadH, 1.5F);
    EXPECT_NEAR(curve.pos.x, flat.pos.x, 1e-5F);
    EXPECT_NEAR(curve.pos.y, flat.pos.y, 1e-5F);
    EXPECT_NEAR(curve.pos.z, flat.pos.z, 1e-5F);

    // An asymmetric (hypothetical) horizontal offset is an ANGLE on the curve: the content centre
    // must land back on the same cylinder at its uv.
    SXRChromeGeometry skew = G;
    skew.contentU0 += 0.05F;
    skew.contentU1 += 0.05F;
    const SXRPose S  = contentPoseToSurfaceCenter(C, skew, quadW, quadH, 1.5F);
    const Vec3    at = monitorPointFromUV(S, quadW, quadH, 1.5F, skew.contentCenterU(), skew.contentCenterV());
    EXPECT_NEAR((at - C.pos).length(), 0.F, 1e-4F);
    const Vec3 axis = xrCylinderFromSurface(S, quadW, quadH, 1.5F).pose.pos; // same (vertical) axis
    EXPECT_NEAR(axis.x, 0.F, 1e-4F);
    EXPECT_NEAR(axis.z, 0.F, 1e-4F);
}

// ---- what Monado actually draws ----

// A line-for-line port of Monado's compute-renderer cylinder lookup (src/xrt/compositor/shaders/
// layer.comp, do_cylinder): ray in the layer's model space -> the texture coordinate it samples, or
// nothing (transparent). Only the FAR intersection is used, longitude is linear in angle, and the
// vertical coordinate is height in radii.
static std::optional<std::pair<float, float>> monadoCylinderSample(const Vec3& ro, const Vec3& rdIn, float radius, float centralAngle, float aspectRatio) {
    const Vec3  rd   = rdIn.normalized();
    const float card = rd.y;
    const float caoc = ro.y;
    const float a    = 1.F - card * card;
    const float b    = ro.dot(rd) - caoc * card;
    const float c    = ro.dot(ro) - caoc * caoc - radius * radius;
    float       h    = b * b - a * c;
    if (h < 0.F)
        return std::nullopt;
    h             = std::sqrt(h);
    const float t = (-b + h) / a;
    if (t < 0.F)
        return std::nullopt;
    const Vec3  dir  = (ro + rd * t).normalized();
    const float lon  = std::atan2(dir.x, -dir.z) / (2.F * PI) + 0.5F;
    const float y    = -dir.y / std::sqrt(1.F - dir.y * dir.y);
    const float chan = centralAngle / (2.F * PI);
    const float hgt  = centralAngle / aspectRatio;
    const float uhan = 0.5F + chan / 2.F, lhan = 0.5F - chan / 2.F;
    if (!(y < hgt / 2.F && y > -hgt / 2.F && lon < uhan && lon > lhan))
        return std::nullopt;
    return std::pair{(lon - lhan) / (uhan - lhan), (y + hgt / 2.F) / hgt};
}

TEST(XRCurve, HitTestMatchesMonadoCylinderSampling) {
    // XReal-ish: a 3.0 x 0.84 m arc at R 1.9, the head at eye height on the axis. Eyes at +/- IPD/2,
    // then a 6DoF lean, then a step back beyond the axis.
    const float   W = 3.0F, H = 0.84F, R = 1.9F;
    const SXRPose P{Vec3{0.F, 1.2F, -R}, Quat{}};
    const auto    CYL  = xrCylinderFromSurface(P, W, H, R);
    const Quat    INV  = qInverse(CYL.pose.rot);
    const Vec3    HEAD = CYL.pose.pos;
    for (const Vec3& eye : {HEAD + Vec3{-0.032F, 0.F, 0.F}, HEAD + Vec3{0.032F, 0.F, 0.F}, HEAD + Vec3{0.25F, -0.1F, 0.4F}, HEAD + Vec3{0.F, 0.05F, 0.9F}}) {
        ASSERT_TRUE(xrCurveFullyVisibleFrom(P, W, R, eye, 0.F));
        for (float u : {0.03F, 0.3F, 0.5F, 0.77F, 0.97F}) {
            for (float v : {0.1F, 0.5F, 0.85F}) {
                const Vec3 target = monitorPointFromUV(P, W, H, R, u, v);
                const Vec3 dir    = (target - eye).normalized();
                const auto OURS   = rayMonitorIntersect(P, eye, dir, W, H, R);
                const auto THEIRS = monadoCylinderSample(qRotate(INV, eye - CYL.pose.pos), qRotate(INV, dir), CYL.radius, CYL.centralAngle, CYL.aspectRatio);
                ASSERT_TRUE(OURS.hit);
                ASSERT_TRUE(THEIRS.has_value());
                EXPECT_NEAR(OURS.u, THEIRS->first, 1e-4F);
                EXPECT_NEAR(OURS.v, THEIRS->second, 1e-4F);
                EXPECT_NEAR(OURS.u, u, 1e-4F);
                EXPECT_NEAR(OURS.v, v, 1e-4F);
            }
        }
    }
}

TEST(XRCurve, VisibilityMatchesWhereMonadoDrawsTheArc) {
    const float   W = 3.0F, H = 0.84F, R = 1.9F;
    const SXRPose P{Vec3{0.F, 1.2F, -R}, Quat{}};
    const auto    CYL = xrCylinderFromSurface(P, W, H, R);
    const Quat    INV = qInverse(CYL.pose.rot);
    // Does the runtime draw EVERY sampled column of the arc for this eye?
    auto drawsWholeArc = [&](const Vec3& eye) {
        for (float u = 0.02F; u < 0.99F; u += 0.04F) {
            const Vec3 dir = (monitorPointFromUV(P, W, H, R, u, 0.5F) - eye).normalized();
            if (!monadoCylinderSample(qRotate(INV, eye - CYL.pose.pos), qRotate(INV, dir), CYL.radius, CYL.centralAngle, CYL.aspectRatio))
                return false;
        }
        return true;
    };
    const Vec3 HEAD = CYL.pose.pos;
    // inside / behind the axis: drawn, and we agree
    for (const Vec3& eye : {HEAD, HEAD + Vec3{1.2F, 0.F, 0.5F}, HEAD + Vec3{0.F, 0.F, 3.F}}) {
        EXPECT_TRUE(drawsWholeArc(eye));
        EXPECT_TRUE(xrCurveFullyVisibleFrom(P, W, R, eye));
    }
    // behind the monitor, and out beyond its right end: Monado would drop part of it, and we say so
    // (so the frame loop submits a quad instead of a half-invisible cylinder)
    for (const Vec3& eye : {Vec3{0.F, 1.2F, -2.5F}, HEAD + Vec3{2.6F, 0.F, -1.6F}}) {
        EXPECT_FALSE(drawsWholeArc(eye));
        EXPECT_FALSE(xrCurveFullyVisibleFrom(P, W, R, eye));
    }
    // the margin makes the test conservative near the wall
    EXPECT_TRUE(xrCurveFullyVisibleFrom(P, W, R, HEAD + Vec3{0.F, 0.F, -1.85F}, 0.F));
    EXPECT_FALSE(xrCurveFullyVisibleFrom(P, W, R, HEAD + Vec3{0.F, 0.F, -1.85F}, 0.1F));
    // flat is always "visible"
    EXPECT_TRUE(xrCurveFullyVisibleFrom(P, W, 0.F, Vec3{0.F, 0.F, -9.F}));
}

// ---- flat stays flat ----

TEST(XRCurve, FlatIsTheQuadPathExactly) {
    const SXRPose P{Vec3{0.2F, 1.1F, -1.4F}, qFromYaw(0.3F)};
    const Vec3    o{0.05F, 1.2F, 0.F};
    for (float yaw : {-0.3F, 0.F, 0.25F}) {
        const Vec3 d = dirYaw(yaw);
        const auto A = rayQuadIntersect(P, o, d, 1.6F, 0.9F, 0.02F);
        const auto B = rayMonitorIntersect(P, o, d, 1.6F, 0.9F, 0.F, 0.02F);
        EXPECT_EQ(A.hit, B.hit);
        EXPECT_EQ(A.t, B.t);
        EXPECT_EQ(A.u, B.u);
        EXPECT_EQ(A.v, B.v);
    }
    const Vec3 a = quadPointFromUV(P, 1.6F, 0.9F, 1.2F, -0.1F);
    const Vec3 b = monitorPointFromUV(P, 1.6F, 0.9F, 0.F, 1.2F, -0.1F);
    EXPECT_EQ(a.x, b.x);
    EXPECT_EQ(a.y, b.y);
    EXPECT_EQ(a.z, b.z);
}

TEST(XRCurve, LargeRadiusConvergesOnFlat) {
    // As R -> infinity the cylinder flattens onto the quad: same uv, same distance.
    const SXRPose P = ahead(1.5F);
    const Vec3    o{0.1F, 0.05F, 0.F};
    const Vec3    d = Vec3{0.3F, -0.1F, -1.F}.normalized();
    const auto    Q = rayQuadIntersect(P, o, d, 1.6F, 0.9F);
    const auto    C = rayMonitorIntersect(P, o, d, 1.6F, 0.9F, 5000.F);
    ASSERT_TRUE(Q.hit);
    ASSERT_TRUE(C.hit);
    EXPECT_NEAR(C.u, Q.u, 1e-3F);
    EXPECT_NEAR(C.v, Q.v, 1e-3F);
    EXPECT_NEAR(C.t, Q.t, 1e-3F);
}

TEST(XRCurve, SurfaceTMatchesTheBoundedHit) {
    const float R = 1.5F;
    const auto  T = rayMonitorSurfaceT(ahead(R), Vec3{}, dirYaw(0.3F), R);
    ASSERT_TRUE(T.has_value());
    EXPECT_NEAR(*T, R, 1e-4F);
    EXPECT_FALSE(rayMonitorSurfaceT(ahead(R), Vec3{}, Vec3{0.F, 1.F, 0.F}, R).has_value());
    // flat: the plane distance
    const auto TF = rayMonitorSurfaceT(ahead(R), Vec3{}, Vec3{0.F, 0.F, -1.F}, 0.F);
    ASSERT_TRUE(TF.has_value());
    EXPECT_NEAR(*TF, R, 1e-5F);
}

// ---- config surface: the classic `curve:` token, create args, layout round-trip, xr_defaults ----

TEST(XRCurveConfig, XRMonitorLineToken) {
    const auto P = parseXRMonitorLine("XR-wide, 5120x1440@60, anchor:local pos:0,1.2,-1.9 size:3.0 curve:auto");
    ASSERT_TRUE(P.has_value()) << P.error();
    ASSERT_TRUE(P->m_curve.has_value());
    EXPECT_EQ(P->m_curve->mode, XR_CURVE_AUTO);

    const auto R = parseXRMonitorLine("XR-wide, 5120x1440, anchor:head offset:0,0,-1.9 curve:2.5");
    ASSERT_TRUE(R.has_value()) << R.error();
    EXPECT_EQ(R->m_curve->mode, XR_CURVE_RADIUS);
    EXPECT_FLOAT_EQ(R->m_curve->radius, 2.5F);

    // An explicit flat is a VALUE (it overrides a curving profile), not an absence.
    const auto F = parseXRMonitorLine("XR-1, 1920x1080, anchor:local pos:0,1.2,-1.5 curve:flat");
    ASSERT_TRUE(F.has_value());
    ASSERT_TRUE(F->m_curve.has_value());
    EXPECT_EQ(F->m_curve->mode, XR_CURVE_FLAT);

    // Unset stays unset (the defaults ladder decides).
    EXPECT_FALSE(parseXRMonitorLine("XR-1, 1920x1080, anchor:local pos:0,1.2,-1.5")->m_curve.has_value());

    const auto BAD = parseXRMonitorLine("XR-1, 1920x1080, anchor:local pos:0,1.2,-1.5 curve:wobbly");
    ASSERT_FALSE(BAD.has_value());
    EXPECT_NE(BAD.error().find("curve"), std::string::npos);
}

TEST(XRCurveConfig, CreateArgsTakeCurveAnywhere) {
    const auto A = parseXRMonitorCreateArgs("XR-wide 5120x1440 curve:auto");
    ASSERT_TRUE(A.has_value()) << A.error();
    EXPECT_EQ(A->m_name, "XR-wide");
    EXPECT_EQ(A->m_curve->mode, XR_CURVE_AUTO);
    EXPECT_FALSE(A->m_anchorProvided);

    const auto B = parseXRMonitorCreateArgs("curve:1.9 XR-wide 5120x1440 anchor:local pos:0,1.2,-1.9");
    ASSERT_TRUE(B.has_value()) << B.error();
    EXPECT_FLOAT_EQ(B->m_curve->radius, 1.9F);
    EXPECT_TRUE(B->m_anchorProvided);

    EXPECT_FALSE(parseXRMonitorCreateArgs("XR-wide curve:auto curve:flat").has_value());
    EXPECT_FALSE(parseXRMonitorCreateArgs("XR-wide curve:").has_value());
}

TEST(XRCurveConfig, LayoutLineRoundTrips) {
    SXRAnchorState anchor;
    anchor.mode = XR_ANCHOR_LOCAL;
    const SXRPose pose{Vec3{0.F, 1.2F, -1.9F}, Quat{}};
    for (const char* c : {"auto", "2.5"}) {
        const auto        SPEC = *parseXRCurve(c);
        const std::string LINE = serializeXRMonitorValue("XR-wide", Vector2D(5120, 1440), 60.F, anchor, pose, 3.0F, SPEC);
        EXPECT_NE(LINE.find(std::string(" curve:") + c), std::string::npos) << LINE;
        const auto BACK = parseXRMonitorLine(LINE);
        ASSERT_TRUE(BACK.has_value()) << BACK.error();
        ASSERT_TRUE(BACK->m_curve.has_value());
        EXPECT_EQ(*BACK->m_curve, SPEC);
    }
    // The paste-ready Lua line wraps the same value.
    EXPECT_NE(serializeXRMonitorLine("XR-wide", Vector2D(5120, 1440), 60.F, anchor, pose, 3.0F, *parseXRCurve("auto")).find(" curve:auto\")"), std::string::npos);
    // A flat monitor's line is byte-identical to before the option existed.
    const std::string FLAT = serializeXRMonitorLine("XR-1", Vector2D(1920, 1080), 60.F, anchor, pose, 1.6F);
    EXPECT_EQ(FLAT.find("curve"), std::string::npos);
    EXPECT_EQ(FLAT, serializeXRMonitorLine("XR-1", Vector2D(1920, 1080), 60.F, anchor, pose, 1.6F, SXRCurveSpec{}));
}

TEST(XRCurveConfig, DefaultsLadderResolvesCurveWithProvenance) {
    SXRSystemContext ctx;
    ctx.bound      = true;
    ctx.systemName = "Monado: XREAL Air 2 Ultra";

    std::vector<SXRDefaultsBlock> blocks;
    SXRDefaultsBlock              base;
    base.size = 1.32F;
    blocks.push_back(base);
    SXRDefaultsBlock wide;
    wide.profile    = "ultrawide";
    wide.resolution = Vector2D{5120, 1440};
    wide.size       = 3.0F;
    wide.curve      = SXRCurveSpec{.mode = XR_CURVE_AUTO};
    blocks.push_back(wide);

    // no profile: globals say flat
    const auto PLAIN = resolveXRDefaults(blocks, "", ctx, {});
    ASSERT_TRUE(PLAIN.has_value());
    EXPECT_EQ(PLAIN->curve.value.mode, XR_CURVE_FLAT);
    EXPECT_EQ(PLAIN->curve.source, XR_DEFSRC_GLOBAL);

    // the profile curves it
    const auto UW = resolveXRDefaults(blocks, "ultrawide", ctx, {});
    ASSERT_TRUE(UW.has_value());
    EXPECT_EQ(UW->curve.value.mode, XR_CURVE_AUTO);
    EXPECT_EQ(UW->curve.source, XR_DEFSRC_PROFILE);
    EXPECT_EQ(UW->curve.block, 1);

    // an explicit value (even flat) outranks the profile
    const auto EX = resolveXRDefaults(blocks, "ultrawide", ctx, {}, SXRExplicitFields{.curve = SXRCurveSpec{}});
    ASSERT_TRUE(EX.has_value());
    EXPECT_EQ(EX->curve.value.mode, XR_CURVE_FLAT);
    EXPECT_EQ(EX->curve.source, XR_DEFSRC_EXPLICIT);
}

TEST(XRCurveConfig, DefaultsBlockValidatesRadius) {
    SXRDefaultsBlock b;
    b.curve = SXRCurveSpec{.mode = XR_CURVE_RADIUS, .radius = 2.F};
    EXPECT_TRUE(validateXRDefaultsBlock(b).has_value());
    b.curve->radius = 0.05F;
    EXPECT_FALSE(validateXRDefaultsBlock(b).has_value());
}
