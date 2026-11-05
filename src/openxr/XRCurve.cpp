#include "XRCurve.hpp"

// Compiled unconditionally (no OpenXR headers, no HAVE_OPENXR guard) — see the header.

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <format>

#include <hyprutils/memory/Casts.hpp>

using namespace OpenXR;
using Hyprutils::Memory::sc;

static std::string lowerTrim(std::string_view v) {
    while (!v.empty() && std::isspace(sc<unsigned char>(v.front())))
        v.remove_prefix(1);
    while (!v.empty() && std::isspace(sc<unsigned char>(v.back())))
        v.remove_suffix(1);
    std::string out(v);
    std::ranges::transform(out, out.begin(), [](unsigned char c) { return sc<char>(std::tolower(c)); });
    return out;
}

std::expected<SXRCurveSpec, std::string> OpenXR::parseXRCurve(std::string_view value) {
    const std::string V = lowerTrim(value);
    if (V == "flat" || V == "off" || V == "false" || V == "none" || V == "no")
        return SXRCurveSpec{};
    if (V == "auto" || V == "on" || V == "true" || V == "yes")
        return SXRCurveSpec{.mode = XR_CURVE_AUTO};

    float      r     = 0.F;
    const auto BEGIN = V.data();
    const auto END   = V.data() + V.size();
    const auto RES   = std::from_chars(BEGIN, END, r);
    if (V.empty() || RES.ec != std::errc{} || RES.ptr != END || !std::isfinite(r))
        return std::unexpected(std::format("curve: expected auto, flat or a radius in metres, got '{}'", value));
    // A zero radius is OpenXR's own spelling of an infinite (flat) cylinder.
    if (r == 0.F)
        return SXRCurveSpec{};
    if (r < XR_CURVE_RADIUS_MIN || r > XR_CURVE_RADIUS_MAX)
        return std::unexpected(std::format("curve: radius must be between {} and {} metres, got {}", XR_CURVE_RADIUS_MIN, XR_CURVE_RADIUS_MAX, r));
    return SXRCurveSpec{.mode = XR_CURVE_RADIUS, .radius = r};
}

std::string OpenXR::xrCurveName(const SXRCurveSpec& spec) {
    switch (spec.mode) {
        case XR_CURVE_AUTO: return "auto";
        case XR_CURVE_RADIUS: return std::format("{}", spec.radius);
        default: return "flat";
    }
}

float OpenXR::xrCurveBaseRadius(const SXRCurveSpec& spec, float autoRadius, float fallback) {
    float r = 0.F;
    switch (spec.mode) {
        case XR_CURVE_RADIUS: r = spec.radius; break;
        case XR_CURVE_AUTO: r = autoRadius > 0.F ? autoRadius : fallback; break;
        default: return 0.F;
    }
    if (!xrFinite(r) || r <= 0.F)
        r = XR_CURVE_RADIUS_MIN;
    return std::clamp(r, XR_CURVE_RADIUS_MIN, XR_CURVE_RADIUS_MAX);
}

float OpenXR::xrCurveAutoRadius(const Vec3& head, const Vec3& centre) {
    const float d = (centre - head).length();
    if (!xrFinite(d))
        return XR_CURVE_RADIUS_MIN;
    return std::clamp(d, XR_CURVE_RADIUS_MIN, XR_CURVE_RADIUS_MAX);
}

float OpenXR::xrCurveEffectiveRadius(float radius, float widthMeters, float maxAngle) {
    if (!(radius > 0.F) || !xrFinite(radius))
        return 0.F;
    float r = std::max(radius, XR_CURVE_RADIUS_MIN);
    if (maxAngle > 0.F && widthMeters > 0.F)
        r = std::max(r, widthMeters / maxAngle);
    return r;
}

float OpenXR::xrCurveCentralAngle(float radius, float widthMeters) {
    return radius > 0.F ? widthMeters / radius : 0.F;
}

// The axis centre (at the surface centre's height) of the cylinder a surface lies on.
static Vec3 cylinderCentre(const SXRPose& Q, float radius) {
    return Q.pos + qRotate(Q.rot, Vec3{0.F, 0.F, radius});
}

SXRCylinderGeom OpenXR::xrCylinderFromSurface(const SXRPose& Q, float w, float h, float radius) {
    SXRCylinderGeom g;
    g.pose         = SXRPose{cylinderCentre(Q, radius), Q.rot};
    g.radius       = radius;
    g.centralAngle = xrCurveCentralAngle(radius, w);
    g.aspectRatio  = h > 0.F ? w / h : 1.F;
    return g;
}

// The exit (larger) root of a ray against an infinite vertical cylinder of `radius` about the local
// +Y axis — the root at which the ray leaves the cylinder, i.e. meets its INSIDE face. Ray already
// in cylinder-local space. nullopt when it misses or the inside face is behind the origin.
static std::optional<float> cylinderExitT(const Vec3& lo, const Vec3& ld, float radius) {
    const float a = ld.x * ld.x + ld.z * ld.z;
    if (a < 1e-12F)
        return std::nullopt; // parallel to the axis: never meets the curved face
    const float b    = 2.F * (lo.x * ld.x + lo.z * ld.z);
    const float c    = lo.x * lo.x + lo.z * lo.z - radius * radius;
    const float disc = b * b - 4.F * a * c;
    if (disc < 0.F)
        return std::nullopt;
    const float t = (-b + std::sqrt(disc)) / (2.F * a);
    if (!(t > 0.F))
        return std::nullopt;
    return t;
}

SXRQuadHit OpenXR::rayCylinderIntersect(const SXRPose& Q, const Vec3& o, const Vec3& d, float w, float h, float radius, float slack) {
    if (!(radius > 0.F))
        return rayQuadIntersect(Q, o, d, w, h, slack);

    SXRQuadHit out;
    const Quat qi = qInverse(Q.rot);
    const Vec3 lo = qRotate(qi, o - cylinderCentre(Q, radius)); // cylinder-local: axis = +Y through the origin
    const Vec3 ld = qRotate(qi, d);

    const auto T = cylinderExitT(lo, ld, radius);
    if (!T)
        return out;

    const Vec3  p     = lo + ld * *T;
    const float theta = std::atan2(p.x, -p.z); // 0 at the surface centre, + towards +X
    const float arc   = theta * radius;
    // The angular bound may not reach round to the back of the cylinder however large the slack.
    const float halfArc = std::min(w * 0.5F + slack, XR_CURVE_MAX_ANGLE * radius);
    if (std::fabs(arc) > halfArc || std::fabs(p.y) > h * 0.5F + slack)
        return out;

    out.hit = true;
    out.t   = *T;
    out.u   = arc / w + 0.5F;
    out.v   = 0.5F - p.y / h;
    return out;
}

SXRQuadHit OpenXR::rayMonitorIntersect(const SXRPose& Q, const Vec3& o, const Vec3& d, float w, float h, float radius, float slack) {
    return radius > 0.F ? rayCylinderIntersect(Q, o, d, w, h, radius, slack) : rayQuadIntersect(Q, o, d, w, h, slack);
}

std::optional<float> OpenXR::rayMonitorSurfaceT(const SXRPose& Q, const Vec3& o, const Vec3& d, float radius) {
    const Quat qi = qInverse(Q.rot);
    if (radius > 0.F)
        return cylinderExitT(qRotate(qi, o - cylinderCentre(Q, radius)), qRotate(qi, d), radius);

    const Vec3 lo = qRotate(qi, o - Q.pos);
    const Vec3 ld = qRotate(qi, d);
    if (std::fabs(ld.z) < 1e-6F)
        return std::nullopt;
    const float t = -lo.z / ld.z;
    if (t <= 0.F)
        return std::nullopt;
    return t;
}

Vec3 OpenXR::monitorPointFromUV(const SXRPose& Q, float w, float h, float radius, float u, float v) {
    if (!(radius > 0.F))
        return quadPointFromUV(Q, w, h, u, v);

    const float y     = (0.5F - v) * h;
    const float half  = w * 0.5F / radius;
    const float theta = (u - 0.5F) * w / radius;
    Vec3        local;
    if (std::fabs(theta) <= half)
        local = Vec3{radius * std::sin(theta), y, -radius * std::cos(theta)};
    else {
        // Past an end: continue along that end's outward tangent by the arc-length overshoot.
        const float s     = theta > 0.F ? 1.F : -1.F;
        const float edge  = s * half;
        const float extra = (std::fabs(theta) - half) * radius;
        local             = Vec3{radius * std::sin(edge), y, -radius * std::cos(edge)} + Vec3{std::cos(edge), 0.F, std::sin(edge)} * (s * extra);
    }
    return cylinderCentre(Q, radius) + qRotate(Q.rot, local);
}

Vec3 OpenXR::monitorNormalAtUV(const SXRPose& Q, float w, float radius, float u) {
    if (!(radius > 0.F))
        return qRotate(Q.rot, Vec3{0.F, 0.F, 1.F});
    const float half  = w * 0.5F / radius;
    const float theta = std::clamp((u - 0.5F) * w / radius, -half, half);
    return qRotate(Q.rot, Vec3{-std::sin(theta), 0.F, std::cos(theta)});
}

SXRPose OpenXR::contentPoseToSurfaceCenter(const SXRPose& contentPose, const SXRChromeGeometry& g, float quadW, float quadH, float radius) {
    if (!(radius > 0.F))
        return contentPoseToQuadCenter(contentPose, g, quadW, quadH);

    // The content centre sits at angle thetaC on the surface (and yC up the axis). The surface's own
    // frame is the content frame turned back by thetaC about the axis; its centre is then found from
    // the shared axis: content = axis + S * (R sin thetaC, yC, -R cos thetaC).
    const float xLocal = (g.contentCenterU() - 0.5F) * quadW;
    const float yLocal = (0.5F - g.contentCenterV()) * quadH;
    const float thetaC = xLocal / radius;
    const Quat  S      = qMul(contentPose.rot, qFromYaw(thetaC));
    const Vec3  axis   = contentPose.pos - qRotate(S, Vec3{radius * std::sin(thetaC), yLocal, -radius * std::cos(thetaC)});
    return SXRPose{axis + qRotate(S, Vec3{0.F, 0.F, -radius}), S};
}

const char* OpenXR::xrCurveFlatReasonName(eXRCurveFlatReason r) {
    switch (r) {
        case XR_CURVEFLAT_UNSUPPORTED: return "runtime lacks XR_KHR_composition_layer_cylinder";
        case XR_CURVEFLAT_DEPTH: return "depth desktop stays flat";
        case XR_CURVEFLAT_VIEWPOINT: return "viewpoint portal stays flat";
        default: return "";
    }
}
