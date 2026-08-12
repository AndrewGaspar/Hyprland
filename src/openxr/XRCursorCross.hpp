#pragma once

// XRCursorCross — pure math for RAY-CAST cursor edge crossing between XR monitors (task #139).
//
// WHY THIS EXISTS, on top of the 2D-plane sync (XRLayout2D.hpp, report 12):
//
//   The 2D sync answers "which monitor is topologically next to this one" by unwrapping every quad
//   centre to (azimuth, elevation) about a latched reference eye and compacting the result into a
//   gap-free grid. That is the right model for Hyprland's own primitives — `movefocus r`, monitor
//   adjacency, the union box the pointer is clamped to — and it must keep owning them.
//
//   But TOPOLOGICAL adjacency and VISUAL adjacency are not the same relation, and they diverge
//   exactly where a headset is more expressive than a desk: monitors at different DEPTHS (a near
//   quad partly in front of a far one), different ELEVATIONS (a quad you have to look up at), or
//   yawed toward you (a cockpit wrap). Compaction has already thrown away depth by then — a centre
//   0.8 m away and a centre 3 m away with the same azimuth land in the same column — so the cursor
//   leaves an edge and reappears somewhere your eyes did not predict. Live report, 2026-08-09:
//   "it feels slightly hinky ... I almost feel like if we treated the mouse movement as logically a
//   ray casting through 3d space _when pushing past the boundary of the monitor_ would feel closer
//   to expected behavior."
//
//   So: at the INSTANT the cursor pushes past a monitor edge — and only then — we stop asking the
//   layout and ask the room. The cursor's overshoot is a point on the source quad's own (extended)
//   plane; the ray from the CURRENT HEAD POSITION through that point is, by construction, the line
//   of sight the user's eye is already following; the first quad that line meets is what they see
//   over there, whatever the layout thinks. The cursor is warped to the 2D coordinates of that
//   3D hit point, so it reappears at the place in space the ray met, not at an edge midpoint.
//
//   The 2D sync is untouched and remains the FALLBACK: no hit, stale head pose, dead session, or a
//   non-XR monitor on either end and the crossing is exactly what it was before.
//
// Compiled UNCONDITIONALLY (no OpenXR headers, no HAVE_OPENXR guard), like its pure siblings
// XRMath.hpp / XRLayout2D.hpp / XRAnchor.hpp, so hyprland_gtests always exercises the real
// expressions rather than a copy (tests/xr/cursor_cross.cpp). No threads, no clocks, no compositor
// types, no config lookups: COpenXRManager::redirectCursorCrossing snapshots quad poses under
// m_layersMu, reads the head pose from the pose ring, resolves live monitor boxes on the MAIN
// thread, and passes plain values in.
//
// COORDINATE CONTRACTS (both borrowed, deliberately, from code that already ships):
//   - Quad UV is rayQuadIntersect's: u = 0 at the quad's LEFT edge and 1 at the right, v = 0 at the
//     TOP and 1 at the bottom. That is also the orientation of a monitor's logical box, so the
//     UV <-> layout-pixel map is a plain affine scale with no flip.
//   - Poses/points are LOCAL_FLOOR metres, the same space `openxr place <name> at x,y,z` consumes.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "XRMath.hpp"
#include "../helpers/math/Math.hpp"

namespace OpenXR {

    // openxr:cursor_crossing. RAYCAST is the default (the feel the live session asked for); LAYOUT
    // is the escape hatch back to pure 2D-layout adjacency, and is what every fallback path below
    // degenerates to anyway, so a user who dislikes the raycast feel gets bit-identical behaviour.
    enum eXRCursorCrossing : uint8_t {
        XR_CURSORCROSS_RAYCAST = 0,
        XR_CURSORCROSS_LAYOUT,
    };

    inline eXRCursorCrossing xrParseCursorCrossing(const std::string& s) {
        if (s == "layout")
            return XR_CURSORCROSS_LAYOUT;
        return XR_CURSORCROSS_RAYCAST; // default + explicit "raycast"
    }

    inline const char* xrCursorCrossingName(eXRCursorCrossing m) {
        switch (m) {
            case XR_CURSORCROSS_LAYOUT: return "layout";
            default: return "raycast";
        }
    }

    // ---- tunables (all reasoned about in docs/openxr/05-configuration.md § cursor_crossing) ----

    // Angular forgiveness on the in-bounds test, in degrees of visual angle about the head. Two
    // monitors that LOOK adjacent are essentially never mathematically edge-to-edge — hand-placed
    // quads leave centimetre gaps — and a ray threading such a gap would miss everything and fall
    // back to the layout, which is precisely the behaviour we are here to replace. An ANGULAR
    // margin (rather than metres or a size fraction) is what makes the forgiveness feel uniform:
    // it is the same on-screen slack for a small near quad and a big far one, and it is the same
    // trick the grab cone (XR_GRAB_CONE_DEG = 5) and the gaze hysteresis already use. 4 deg is a
    // touch tighter than the grab cone — crossing is a constant, low-effort action, so the margin
    // must bridge a gap without letting an edge-push snag a monitor that is plainly elsewhere. At a
    // 2 m viewing distance it is ~14 cm of slack, roughly a tenth of a default 1.6 m-wide quad.
    // A tolerated hit only ever runs when NO quad was hit squarely, and its UV is clamped into
    // bounds, so the cursor lands on the target's nearest edge — never outside it.
    constexpr float XR_CROSS_TOLERANCE_DEG = 4.0F;

    // How far past the source edge the exit point is allowed to travel, in units of the source
    // monitor (0.5 = half a monitor width/height). Continued outward pushes accumulate up to this
    // limit; capping them keeps the ray aimed beside the monitor instead of sweeping the room.
    constexpr float XR_CROSS_MAX_OVERSHOOT_UV = 0.5F;

    // Forget a partial edge push after this much inactivity. A mouse reports continued pressure at
    // hundreds of Hz, so 250 ms leaves ample room for a deliberate shove while preventing a miss
    // from priming an unrelated crossing much later.
    constexpr int64_t XR_CROSS_PUSH_TIMEOUT_MS = 250;

    // Head-pose freshness budget. The ring is written once per frame (~11 ms at 90 Hz), so 200 ms
    // is ~18 frames: generous enough that a compositor hitch or a dropped frame still crosses by
    // ray, tight enough that a session which has actually stopped rendering falls back to the
    // layout instead of casting from where the head USED to be. (A doffed headset also unplugs its
    // monitors, so they leave the layout entirely — this budget is about hitches, not doffing.)
    constexpr int64_t XR_CROSS_POSE_MAX_AGE_MS = 200;

    // Rebuild budget for the quad-geometry snapshot, in ms. The snapshot costs one m_layersMu
    // acquisition, and while the cursor is pushed against an edge whose ray misses we are asked on
    // EVERY motion event (up to 1 kHz). One rebuild per frame is plenty — quads do not move faster
    // than they are drawn — and it bounds our contention with the frame thread's solve.
    constexpr int64_t XR_CROSS_GEOM_TTL_MS = 8;

    // Inset, in layout px, of the entry point from the target monitor's box edges. CPointerManager
    // ::closestValid re-clamps whatever we return, and CBox::inside is STRICT, so a point sitting
    // exactly on a shared boundary is "outside" every box and gets snapped by nearest-edge search —
    // possibly onto the neighbour we just decided against. One pixel of inset makes the landing
    // unambiguous and is invisible.
    constexpr double XR_CROSS_ENTRY_INSET_PX = 1.0;

    // ---- types ----

    // One XR monitor's live CONTENT quad (the desktop pixels — NOT the chrome-expanded full quad
    // the ray pointer hit-tests, whose margins are transparent and carry no cursor coordinates).
    struct SXRCrossQuad {
        int64_t id = -1;       // MONITORID, the handle back to the CMonitor and its layout box
        SXRPose pose;          // content-quad centre, LOCAL_FLOOR metres
        float   wMeters = 0.F; // content width
        float   hMeters = 0.F; // content height (= wMeters * contentPxH / contentPxW)
    };

    struct SXRCrossPick {
        bool    ok        = false;
        int64_t id        = -1;  // winning monitor
        float   u         = 0.F; // hit point in the winner's quad UV, always clamped to [0,1]
        float   v         = 0.F;
        float   t         = 0.F;   // distance along the ray, metres
        bool    tolerated = false; // true = won on the angular margin, not a square hit
    };

    enum eXRCrossEdge : uint8_t {
        XR_CROSSEDGE_NONE = 0,
        XR_CROSSEDGE_LEFT,
        XR_CROSSEDGE_RIGHT,
        XR_CROSSEDGE_TOP,
        XR_CROSSEDGE_BOTTOM,
    };

    // MAIN-THREAD gesture state. The cursor itself is clamped back into the layout after every
    // missed cast, so without remembering this normalized pressure every small event starts again
    // at the edge and only a single hard flick can ever sweep the ray across a visible gap.
    struct SXRCrossPushState {
        int64_t      sourceId    = -1;
        eXRCrossEdge edge        = XR_CROSSEDGE_NONE;
        double       overshootUV = 0.0;
        int64_t      lastPushMs  = -1;

        void         reset() {
            sourceId    = -1;
            edge        = XR_CROSSEDGE_NONE;
            overshootUV = 0.0;
            lastPushMs  = -1;
        }
    };

    // ---- 2D <-> quad UV ----

    // Where a layout point sits in a monitor's box, as a fraction of it. AFFINE and deliberately
    // unclamped: the whole mechanism depends on a point PAST the edge mapping past [0,1], since
    // that overshoot is what sweeps the ray outward.
    inline Vector2D xrBoxUV(const CBox& box, const Vector2D& p) {
        const double w = box.w > 0.0 ? box.w : 1.0;
        const double h = box.h > 0.0 ? box.h : 1.0;
        return Vector2D{(p.x - box.x) / w, (p.y - box.y) / h};
    }

    // The exit point of a crossing, in source-quad UV, with the overshoot budget applied.
    inline Vector2D xrCrossExitUV(const CBox& box, const Vector2D& p, float maxOvershoot) {
        const double   lo = -(double)maxOvershoot;
        const double   hi = 1.0 + (double)maxOvershoot;
        const Vector2D uv = xrBoxUV(box, p);
        return Vector2D{std::clamp(uv.x, lo, hi), std::clamp(uv.y, lo, hi)};
    }

    // Add one relative-motion event to a continued edge push. A miss intentionally leaves `state`
    // alive: CPointerManager will perform its ordinary 2D-layout fallback and, if that fallback
    // clamps the cursor back to this edge, the next outward event advances the ray instead of
    // starting over. If the fallback actually reaches another monitor, sourceId changes and the
    // gesture resets on the next event.
    inline std::optional<Vector2D> xrAccumulateCrossPush(SXRCrossPushState& state, int64_t sourceId, const CBox& box, const Vector2D& oldPos, const Vector2D& newPos, int64_t nowMs,
                                                         int64_t timeoutMs, float maxOvershoot) {
        if (sourceId < 0 || !(box.w > 0.0) || !(box.h > 0.0) || !(maxOvershoot > 0.F)) {
            state.reset();
            return std::nullopt;
        }

        if (state.edge != XR_CROSSEDGE_NONE && (state.sourceId != sourceId || nowMs < state.lastPushMs || nowMs - state.lastPushMs > std::max<int64_t>(0, timeoutMs)))
            state.reset();

        const Vector2D delta  = newPos - oldPos;
        const bool     inward = (state.edge == XR_CROSSEDGE_LEFT && delta.x > 0.0) || (state.edge == XR_CROSSEDGE_RIGHT && delta.x < 0.0) ||
            (state.edge == XR_CROSSEDGE_TOP && delta.y > 0.0) || (state.edge == XR_CROSSEDGE_BOTTOM && delta.y < 0.0);
        if (inward)
            state.reset();

        const double left   = std::max(0.0, (box.x - newPos.x) / box.w);
        const double right  = std::max(0.0, (newPos.x - (box.x + box.w)) / box.w);
        const double top    = std::max(0.0, (box.y - newPos.y) / box.h);
        const double bottom = std::max(0.0, (newPos.y - (box.y + box.h)) / box.h);

        // At a corner, keep the gesture on its current edge while that edge is still crossed. This
        // prevents tiny diagonal jitter from alternating edges and throwing accumulated intent
        // away. A new gesture chooses the edge with the greatest normalized excursion.
        eXRCrossEdge edge     = XR_CROSSEDGE_NONE;
        double       amount   = 0.0;
        const auto   consider = [&](eXRCrossEdge candidate, double excursion) {
            if (excursion > amount) {
                edge   = candidate;
                amount = excursion;
            }
        };

        if (state.edge == XR_CROSSEDGE_LEFT && left > 0.0)
            edge = state.edge, amount = left;
        else if (state.edge == XR_CROSSEDGE_RIGHT && right > 0.0)
            edge = state.edge, amount = right;
        else if (state.edge == XR_CROSSEDGE_TOP && top > 0.0)
            edge = state.edge, amount = top;
        else if (state.edge == XR_CROSSEDGE_BOTTOM && bottom > 0.0)
            edge = state.edge, amount = bottom;
        else {
            consider(XR_CROSSEDGE_LEFT, left);
            consider(XR_CROSSEDGE_RIGHT, right);
            consider(XR_CROSSEDGE_TOP, top);
            consider(XR_CROSSEDGE_BOTTOM, bottom);
        }

        if (edge == XR_CROSSEDGE_NONE)
            return std::nullopt;

        if (state.edge != edge) {
            state.reset();
            state.sourceId = sourceId;
            state.edge     = edge;
        }

        state.overshootUV = std::clamp(state.overshootUV + amount, 0.0, (double)maxOvershoot);
        state.lastPushMs  = nowMs;

        Vector2D uv = xrCrossExitUV(box, newPos, maxOvershoot);
        switch (edge) {
            case XR_CROSSEDGE_LEFT: uv.x = -state.overshootUV; break;
            case XR_CROSSEDGE_RIGHT: uv.x = 1.0 + state.overshootUV; break;
            case XR_CROSSEDGE_TOP: uv.y = -state.overshootUV; break;
            case XR_CROSSEDGE_BOTTOM: uv.y = 1.0 + state.overshootUV; break;
            default: break;
        }
        return uv;
    }

    // The layout point a quad-UV hit corresponds to, inset from the box edges (see the constant).
    // u/v are expected in [0,1]; they are clamped anyway so a caller cannot land us outside.
    inline Vector2D xrCrossEntryPoint(const CBox& box, float u, float v, double insetPx) {
        const double cu = std::clamp((double)u, 0.0, 1.0);
        const double cv = std::clamp((double)v, 0.0, 1.0);
        Vector2D     p{box.x + cu * box.w, box.y + cv * box.h};
        // Never inset past the middle: a box thinner than 2*inset would otherwise invert.
        const double ix = std::min(insetPx, box.w / 2.0);
        const double iy = std::min(insetPx, box.h / 2.0);
        p.x             = std::clamp(p.x, box.x + ix, box.x + box.w - ix);
        p.y             = std::clamp(p.y, box.y + iy, box.y + box.h - iy);
        return p;
    }

    // ---- target selection ----

    // Cast from `eye` through `through` and pick the monitor the user is looking at over there.
    //
    // Two passes, and the ORDER is the whole policy: a square hit always beats a tolerated one, so
    // the angular margin can only ever rescue a crossing that would otherwise have found nothing —
    // it can never steal a crossing from a monitor the ray actually went through. Within a pass the
    // NEAREST hit wins, because the nearest quad along that line is the one that OCCLUDES the rest:
    // it is literally what the user sees in the direction they just pushed. (Note we do not require
    // the hit to be farther than the source plane. A quad hanging in front of the source, off to
    // the side, is visible in that direction and is the honest answer.)
    //
    // `sourceId` is excluded: the source's own plane is hit at the exit point by construction, so
    // including it would trivially and uselessly re-select the monitor we are leaving.
    inline SXRCrossPick xrPickCrossTarget(const Vec3& eye, const Vec3& through, const std::vector<SXRCrossQuad>& quads, int64_t sourceId, float toleranceDeg) {
        SXRCrossPick out;

        const Vec3  delta = through - eye;
        const float len   = delta.length();
        if (len < 1e-5F)
            return out; // degenerate: the exit point is at the eye, no direction to cast
        const Vec3  dir     = delta * (1.F / len);
        const float tanTol  = std::tan(std::clamp(toleranceDeg, 0.F, 30.F) * (float)M_PI / 180.F);

        for (int pass = 0; pass < 2; ++pass) {
            float bestT = std::numeric_limits<float>::max();
            for (const auto& q : quads) {
                if (q.id == sourceId || q.id < 0)
                    continue;
                if (!(q.wMeters > 0.F) || !(q.hMeters > 0.F))
                    continue;
                // Pass 1 is exact; pass 2 expands the half-extents by the angular margin subtended
                // at THIS quad's distance, so the forgiveness is constant in the user's view.
                const float slack = pass == 0 ? 0.F : tanTol * (q.pose.pos - eye).length();
                const SXRQuadHit hit = rayQuadIntersect(q.pose, eye, dir, q.wMeters, q.hMeters, slack);
                if (!hit.hit || hit.t >= bestT)
                    continue;
                bestT         = hit.t;
                out.ok        = true;
                out.id        = q.id;
                out.t         = hit.t;
                out.u         = std::clamp(hit.u, 0.F, 1.F);
                out.v         = std::clamp(hit.v, 0.F, 1.F);
                out.tolerated = pass == 1;
            }
            if (out.ok)
                return out;
        }
        return out;
    }
}
