#pragma once

// XRLayout2D — the pure projection that derives Hyprland's native 2D monitor-layout plane from the
// 3D spatial arrangement of the XR quads (docs/openxr/research/archive/12-spatial-2d-layout.md,
// WP-S1). A monitor floating to your upper-right in the headset must sit upper-right in the 2D
// layout, so the cursor leaves XR-main's right edge and lands on XR-side exactly where your eyes
// expect, and `movefocus r` goes where you point.
//
// Compiled UNCONDITIONALLY (no OpenXR headers, no HAVE_OPENXR guard), exactly like its pure
// siblings XRMath.hpp / XRAnchor.hpp / XRRule.hpp, so hyprland_gtests can always exercise it
// (tests/xr/layout2d.cpp). No threads, no clocks, no compositor types, no config lookups: the
// caller (COpenXRManager::syncLayout2D) snapshots poses under m_layersMu, reads config on the MAIN
// thread, and passes plain values in.
//
// THE MODEL (report 12 §2, settled):
//
//   1. VIEWER-CENTRIC UNWRAP (§2.2). Treat the reference eye as the origin and unroll the sphere of
//      monitors around it: each quad centre becomes (azimuth, elevation); azimuth -> layout x and
//      elevation -> layout y through one PX_PER_DEG constant. Centres are positioned BY ANGLE;
//      every monitor keeps its NATIVE logical pixel size (a 4K and a 720p panel at the same
//      distance subtend the same angle but are wildly different boxes — we place them where they
//      ARE and let them keep how BIG they are).
//
//   2. COMPACTION (§2.4). The raw unwrap has gaps and fractional overlaps, which Hyprland cannot
//      use: `movefocus`/monitor adjacency needs the facing edges within STICKS (2 px) AND a
//      perpendicular overlap (MonitorQueryCore::directionLookup + macros.hpp STICKS), and the
//      pointer is clamped to the UNION of monitor boxes so any gap is a visible cursor jump. So the
//      unwrap is normalized into rows/columns and emitted edge-touching, gap-free and overlap-free
//      — the headless equivalent of what GNOME mutter and KDE KScreen do to a dragged arrangement.
//
//   3. HYSTERESIS (§4). A monitor whose angles moved less than `reorderHysteresisDeg` since the last
//      sync keeps its PREVIOUS angles for layout purposes, so a slightly bumped quad reshuffles
//      nothing. Because the remembered value is the one that was actually adopted, a slow drift is
//      bounded by the threshold rather than accumulating forever.
//
// Everything here is deterministic and integral: identical inputs give a byte-identical layout, and
// every coordinate is a whole pixel.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "XRMath.hpp"

namespace OpenXR {

    // ---- config (report 12 §5; the caller maps openxr:layout2d:* onto this) ----

    // How a monitor's row placement is derived (§2.2). ELEVATION keeps the whole map a clean
    // spherical unwrap (both axes are angles, one constant, symmetric behaviour) and is the default;
    // WORLD_HEIGHT is metric and can feel better when radii differ a lot (a low close reference
    // monitor vs a high far status display).
    enum eXRLayout2DVertical : uint8_t {
        XR_L2D_VERT_ELEVATION = 0,
        XR_L2D_VERT_WORLD_HEIGHT,
    };

    // Where the XR block attaches to the physical monitors (§2.5). Physical outputs have no room
    // pose, so we can only arrange XR monitors AMONG THEMSELVES and attach the block at a seam.
    enum eXRLayout2DAttach : uint8_t {
        XR_L2D_ATTACH_RIGHT = 0, // flush right of everything already placed (matches today)
        XR_L2D_ATTACH_AROUND,    // the XR block IS the layout (headset-only session)
    };

    const char*                        xrLayout2DVerticalName(eXRLayout2DVertical v);
    const char*                        xrLayout2DAttachName(eXRLayout2DAttach a);
    std::optional<eXRLayout2DVertical> xrParseLayout2DVertical(const std::string& s);
    std::optional<eXRLayout2DAttach>   xrParseLayout2DAttach(const std::string& s);

    struct SXRLayout2DConfig {
        // Angular -> pixel scale for CENTRE placement (§2.3). 35 px/deg is a 1080p reference monitor
        // (1.6 m wide at 1.5 m subtends ~56 deg, 1920/56 ~= 34), so two quads that are angularly
        // adjacent in 3D come out nearly edge-touching before compaction and the compaction only
        // nudges by a few px. The exact value matters little — compaction is authoritative for
        // adjacency — it mostly sets how proportional the pre-compaction spacing looks.
        float               pxPerDegree          = 35.F;
        // Only used when vertical == WORLD_HEIGHT: metres of world height -> layout px.
        float               pxPerMeter           = 1000.F;
        eXRLayout2DVertical vertical             = XR_L2D_VERT_ELEVATION;
        // Minimum perpendicular overlap forced between neighbours (§2.4). Hyprland's directional
        // lookup needs INTERSECTLEN > 0; a real minimum keeps the cursor from threading a 1 px seam.
        int                 minOverlapPx         = 64;
        // Elevation window within which monitors share a row.
        float               rowMergeDeg          = 10.F;
        // Angular margin a monitor must cross before the layout is allowed to change (§4).
        float               reorderHysteresisDeg = 4.F;
    };

    // ---- the reference frame (§3) ----

    // World/`local` monitors are measured against a LATCHED "desk orientation", never the live head
    // yaw: turning your chair must not re-map your mouse. `yaw` is the reference forward's azimuth
    // in the SAME right-positive convention the projection uses, i.e. atan2(fwd.x, -fwd.z) — NOT
    // qYawOf(), which is the negation of that (it measures yaw left-positive about +Y). Mixing the
    // two mirrors the whole layout, so the caller computes this straight from the forward vector.
    struct SXRLayout2DRef {
        Vec3  eye;
        float yaw   = 0.F;
        bool  valid = false;
    };

    // ---- inputs ----

    struct SXRLayout2DInput {
        std::string name;
        // World (LOCAL_FLOOR) quad centre when followFrame == false; the persistent follow-frame
        // offset (CXRAnchor::state().anchorPose) when true. Only .pos is read — a quad's own
        // orientation does not affect WHERE it is around you.
        SXRPose     pose;
        // head/body-anchored monitors are measured in their own (already stable) follow frame and
        // merged onto the same plane: the 2D map represents the arrangement AS SEEN FROM the
        // reference pose, and when the user faces the reference forward the desk frame and the view
        // frame coincide (§3, "merge onto one plane").
        bool        followFrame = false;
        // LOGICAL size in layout pixels (CMonitor::size(), i.e. pixels/scale after transform) — the
        // unit CMonitorPositionController::arrange works in. Clamped to >= 1.
        int         w           = 1920;
        int         h           = 1080;
    };

    // Per-monitor result of the previous sync, fed back in for hysteresis (§4).
    struct SXRLayout2DPrev {
        std::string name;
        float       azDeg = 0.F;
        float       vPx   = 0.F;
    };

    // ---- output ----

    struct SXRLayout2DSlot {
        std::string name;
        int         col = 0, row = 0;
        int         x = 0, y = 0; // top-left in the block's own coordinates (block origin = 0,0)
        int         w = 0, h = 0;
        float       azDeg = 0.F; // the (possibly hysteresis-held) angles the placement used
        float       elDeg = 0.F;
        float       vPx   = 0.F; // the vertical coordinate those angles produced, pre-compaction
    };

    struct SXRLayout2DResult {
        std::vector<SXRLayout2DSlot> slots;
        int                          width = 0, height = 0; // bounding box of the block
        int                          rows  = 0;
    };

    // ---- the pure pieces (each individually gtest-covered) ----

    // Wrap to (-180, 180]. The unwrap's seam therefore sits DIRECTLY BEHIND the reference forward,
    // which is the natural place for the discontinuity of a 360-degree ring flattened onto a line.
    float xrWrapDeg180(float deg);

    // Angular coordinates of ONE monitor about the reference eye. Writes the azimuth (degrees,
    // right-positive: +az is to your right, which becomes +x), the elevation (degrees, up-positive)
    // and the vertical layout coordinate in px (down-positive, so it is directly comparable to a
    // layout y). Degenerate inputs — a quad exactly at the eye, or exactly overhead — resolve to
    // az = 0 rather than a NaN or an arbitrary atan2 branch.
    void  xrLayout2DAngles(const SXRLayout2DInput& mon, const SXRLayout2DRef& ref, const SXRLayout2DConfig& cfg, float& azDegOut, float& elDegOut, float& vPxOut);

    // The whole projection: unwrap -> hysteresis -> rows -> columns -> compaction. The result's
    // origin is (0, 0); the caller translates the block to the attach seam (§2.5). `prev` may be
    // empty (first sync / thawed after a config change), in which case no angle is held.
    //
    // GUARANTEES (pinned by tests/xr/layout2d.cpp):
    //   * every coordinate is an integer pixel;
    //   * no two monitors' boxes overlap;
    //   * horizontally adjacent monitors touch EXACTLY (gap 0, so STICKS holds) and overlap
    //     vertically by at least min(minOverlapPx, both heights);
    //   * vertically adjacent ROWS touch exactly and their x-spans overlap by at least
    //     min(minOverlapPx, both row widths);
    //   * the output is deterministic (ties broken by name) and order-independent in `mons`.
    SXRLayout2DResult xrProjectLayout2D(const std::vector<SXRLayout2DInput>& mons, const SXRLayout2DRef& ref, const SXRLayout2DConfig& cfg, const std::vector<SXRLayout2DPrev>& prev);

    // Convenience: the feedback state to carry into the next call.
    std::vector<SXRLayout2DPrev> xrLayout2DPrevOf(const SXRLayout2DResult& r);
}
