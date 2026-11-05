#pragma once

// XRCurve — curved (cylindrical) XR monitors: the `curve` config value and the pure geometry
// behind it (docs/openxr/02-virtual-monitors.md "Curved monitors", research/VISUALS.md C7).
//
// THE MODEL. A monitor keeps exactly the handle it always had: the anchor solves the pose of the
// CONTENT CENTRE (pos + yaw/pitch), and `size:` is the content width in metres. A curved monitor
// bends that same rectangle around a vertical cylinder whose axis sits R metres IN FRONT of the
// centre, along the centre's own normal (+Z, the side the viewer is on) — so the centre point,
// the facing and the width the user configured are all unchanged, and only the shape is new:
//
//   - the axis is the pose's +Y (pitch tilts the cylinder, it does not shear it);
//   - width is ARC LENGTH: centralAngle = width / R, aspectRatio = width / height (the
//     XrCompositionLayerCylinderKHR contract — its height is radius * centralAngle / aspectRatio);
//   - u runs along the arc (u = 0.5 at the centre, growing towards +X), v down the axis, exactly
//     as rayQuadIntersect's convention, so every uv-space consumer (chrome classification, the
//     content remap, cursor drawing, hover hysteresis in metres-per-uv) is shape-agnostic.
//
// Because the axis is derived from the pose, a rigid drag of the pose moves axis and arc together
// as one piece; nothing ever slides around the arc. "auto" picks R = the distance from the head to
// the monitor centre at placement time — the head then sits on the axis and every column is
// equidistant — and freezes it until something re-places the monitor around the head (a re-seat,
// `xrmonitor center`, or `xrmonitor curve auto`). The FRAME thread resolves it (it owns the head
// pose); see COpenXRManager's solve loop.
//
// A pose and full-surface dimensions (`SXRPose Q, float w, float h, float radius`) describe a
// monitor SURFACE everywhere below: Q is the surface centre (u = v = 0.5) facing its viewer, and
// radius <= 0 means flat. Every function degenerates to the exact flat code path for radius <= 0,
// so a flat monitor is bit-identical to before.
//
// Pure + unconditional (no OpenXR headers, no HAVE_OPENXR guard), like XRMath.hpp, so
// hyprland_gtests exercises it directly (tests/xr/curve.cpp).

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

#include "XRMath.hpp"

namespace OpenXR {
    enum eXRCurveMode : uint8_t {
        XR_CURVE_FLAT = 0, // the default: a flat quad
        XR_CURVE_AUTO,     // centred on the head, R = head -> monitor distance at placement
        XR_CURVE_RADIUS,   // an explicit radius in metres
    };

    struct SXRCurveSpec {
        eXRCurveMode mode   = XR_CURVE_FLAT;
        float        radius = 0.F; // metres; meaningful iff mode == XR_CURVE_RADIUS

        bool         operator==(const SXRCurveSpec&) const = default;
    };

    // Bounds. The minimum radius matches the nearest placement distance any verb accepts
    // (XR_DISTANCE_MIN): a tighter cylinder would sit closer to the axis than anything can be put.
    // The maximum explicit radius is where a curve stops being distinguishable from flat at monitor
    // sizes (a 4 m arc at 100 m bows by 2 cm). The central angle is capped at 180 degrees: past
    // that the arc's edges turn back towards the viewer and wrap around behind them.
    constexpr float XR_CURVE_RADIUS_MIN = 0.3F;
    constexpr float XR_CURVE_RADIUS_MAX = 100.F;
    constexpr float XR_CURVE_MAX_ANGLE  = 3.14159265358979323846F;

    // Parse a `curve` value: "flat" / "off" / "false" / "none" / "0" -> flat; "auto" / "on" /
    // "true" -> auto; a number in [XR_CURVE_RADIUS_MIN, XR_CURVE_RADIUS_MAX] -> that radius.
    // Case-insensitive. Errors name the accepted forms.
    std::expected<SXRCurveSpec, std::string> parseXRCurve(std::string_view value);
    // The canonical spelling parseXRCurve round-trips: "flat", "auto", or the radius (shortest
    // round-trip decimal).
    std::string xrCurveName(const SXRCurveSpec& spec);

    // The radius the geometry is built on, before the angle clamp: 0 for flat; the explicit radius;
    // or for auto the frozen `autoRadius`, falling back to `fallback` while it is still unresolved
    // (<= 0 — no head pose has been seen since the monitor was placed). Clamped to the bounds.
    float xrCurveBaseRadius(const SXRCurveSpec& spec, float autoRadius, float fallback);
    // The auto radius for a monitor centred at `centre` seen from `head` — their distance, clamped.
    float xrCurveAutoRadius(const Vec3& head, const Vec3& centre);
    // The radius actually rendered for a surface `widthMeters` wide: never below the minimum, and
    // raised just enough that the central angle stays within `maxAngle` (a too-wide monitor flattens
    // a little rather than wrapping). 0 (flat) stays 0.
    float xrCurveEffectiveRadius(float radius, float widthMeters, float maxAngle = XR_CURVE_MAX_ANGLE);
    // width / radius; 0 for flat.
    float xrCurveCentralAngle(float radius, float widthMeters);

    // XrCompositionLayerCylinderKHR's parameters for a surface: the layer pose is ON THE AXIS (at the
    // surface centre's height), facing the surface centre down its -Z, which is the runtime's
    // convention for where the visible arc is centred.
    struct SXRCylinderGeom {
        SXRPose pose;
        float   radius       = 0.F;
        float   centralAngle = 0.F;
        float   aspectRatio  = 1.F;
    };
    SXRCylinderGeom xrCylinderFromSurface(const SXRPose& Q, float w, float h, float radius);

    // ---- the shape-aware hit test: ray vs monitor surface -> hit, uv, distance ----

    // Ray vs a curved surface. Only the INSIDE face is hit (the side the axis is on — the side the
    // runtime draws the image for); bounds are the arc (|arc length| <= w/2 + slack) and the height
    // (|y| <= h/2 + slack). `slack` is metres along the surface, the same forgiveness rayQuadIntersect
    // gives the grab cone / magnet / gaze hysteresis. radius <= 0 is rayQuadIntersect exactly.
    SXRQuadHit rayCylinderIntersect(const SXRPose& Q, const Vec3& o, const Vec3& d, float w, float h, float radius, float slack = 0.F);
    // THE entry point every hit test goes through: flat or curved by `radius`.
    SXRQuadHit rayMonitorIntersect(const SXRPose& Q, const Vec3& o, const Vec3& d, float w, float h, float radius, float slack = 0.F);
    // The unbounded ray parameter to the surface (its plane, or its infinite cylinder's inside face),
    // for sizing an angular slack before the bounded test. nullopt when the ray never reaches it.
    std::optional<float> rayMonitorSurfaceT(const SXRPose& Q, const Vec3& o, const Vec3& d, float radius);

    // The inverse of rayMonitorIntersect's uv map: the world point at (u,v). NOT clamped, like
    // quadPointFromUV, because cursor crossing aims through the overshoot past an edge. Past the arc's
    // ends the point continues along the TANGENT at that end (arc-length overshoot -> the same
    // distance along the tangent) rather than around the cylinder, so an overshoot keeps pointing
    // outwards instead of wrapping behind the viewer. Round-trips with rayMonitorIntersect for any
    // in-bounds uv seen from inside the cylinder.
    Vec3 monitorPointFromUV(const SXRPose& Q, float w, float h, float radius, float u, float v);
    // The unit surface normal at u (towards the viewer / the axis); the pose's +Z when flat.
    Vec3 monitorNormalAtUV(const SXRPose& Q, float w, float radius, float u);

    // contentPoseToQuadCenter for either shape: the content centre is a point ON the surface, so a
    // horizontal content offset is an angle about the axis on a curve, not a chord. The vertical
    // offset is along the axis either way. Same chrome geometry, same full-surface metres.
    SXRPose contentPoseToSurfaceCenter(const SXRPose& contentPose, const SXRChromeGeometry& g, float quadW, float quadH, float radius);

    // Can a viewer at `eye` see the WHOLE arc of a curved surface? The runtime draws a cylinder layer
    // by intersecting each eye ray with the infinite cylinder and sampling only the FAR (exit)
    // intersection (Monado layer.comp do_cylinder; the gfx mesh path draws the same inside face), so
    // an arc point is visible exactly when the ray from the eye LEAVES the cylinder there — i.e. when
    // the eye is on the axis side of that point's tangent plane: dot(eye_h, n_out(theta)) < R in the
    // cylinder's horizontal plane. Inside the cylinder that always holds; behind the axis it holds for
    // any arc up to 180 degrees; behind the monitor, or out beyond an end, part of the arc would go
    // transparent. `margin` (metres) shrinks R for the test so both eyes (+/- IPD/2 from the head) and a
    // flicker band are covered. Always true for flat.
    bool xrCurveFullyVisibleFrom(const SXRPose& Q, float w, float radius, const Vec3& eye, float margin = 0.F);

    // Why a monitor that asked for a curve is being submitted flat (status + the one-time log).
    enum eXRCurveFlatReason : uint8_t {
        XR_CURVEFLAT_NONE = 0,    // not flattened (flat by config, or actually curved)
        XR_CURVEFLAT_UNSUPPORTED, // the runtime does not offer XR_KHR_composition_layer_cylinder
        XR_CURVEFLAT_VIEWPOINT,   // the viewpoint portal stays flat in v1
        XR_CURVEFLAT_OUTSIDE,     // the viewer is where the runtime would not draw the whole arc
    };
    const char* xrCurveFlatReasonName(eXRCurveFlatReason r);
}
