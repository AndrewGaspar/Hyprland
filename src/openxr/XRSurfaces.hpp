#pragma once

// XRSurfaces — research/32 C0-C2: the pure plane model and the "drop a window onto a table" math.
//
// Compiled UNCONDITIONALLY (no HAVE_OPENXR guard, no OpenXR headers), like XRMath/XRAnchor, so every
// decision the feature makes is gtested without a runtime (tests/xr/surfaces.cpp). No clocks, no
// config, no logging, no allocation on the frame-thread entry points: planes are fixed-capacity POD
// records, so the frame thread copies them around with zero hyprutils refcount ops by construction
// (XRMonitorLayer.hpp threading rule).
//
// THE PLANE CONVENTION (canonical, whatever the source): a plane is the local X-Y plane of its pose,
// local +Z is its normal — the side content rests against — and its polygon is in plane-local X-Y
// metres. Poses are in the SOLVER frame (LOCAL_FLOOR metres; the LOCAL fallback is floor-shifted by
// the caller exactly like the head and grips). A source re-locates its planes against the reference
// space on every poll; nothing here caches a world coordinate across a reference-space change — the
// frame thread re-expresses its snapshot with xrPlanesReexpress on a recenter, and a poll taken in a
// space that has since died is refused by epoch (CXRSurfaceSources / COpenXRManager).

#include "XRMath.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace OpenXR {
    struct SXRVec2 {
        float x = 0.F, y = 0.F;
    };

    // Semantic label. XR_EXT_plane_detection's PLATFORM is what research/32 calls TABLE; synthetic
    // planes may say either.
    enum eXRPlaneLabel : uint8_t {
        XR_PLANE_LABEL_UNKNOWN = 0,
        XR_PLANE_LABEL_FLOOR,
        XR_PLANE_LABEL_WALL,
        XR_PLANE_LABEL_CEILING,
        XR_PLANE_LABEL_TABLE,
        XR_PLANE_LABEL_PLATFORM,
    };

    // Orientation class, always derived from the plane's normal against gravity (xrClassifyPlane),
    // never trusted from a label.
    enum eXRPlaneOrient : uint8_t {
        XR_PLANE_UP = 0,   // horizontal, normal up: floors, tables — what a window can stand on
        XR_PLANE_DOWN,     // horizontal, normal down: ceilings
        XR_PLANE_VERTICAL, // walls
        XR_PLANE_ARBITRARY,
    };

    enum eXRPlaneSource : uint8_t {
        XR_PLANE_SRC_SYNTHETIC = 0, // config-declared (openxr:surfaces_synthetic)
        XR_PLANE_SRC_DETECTION,     // XR_EXT_plane_detection
    };

    // How a released window was put on a surface.
    enum eXRSnapKind : uint8_t {
        XR_SNAP_NONE = 0,
        XR_SNAP_STAND, // bottom edge on the surface, facing the user, tilted back
        XR_SNAP_FLAT,  // lying on the surface, top edge away from the user
        XR_SNAP_WALL,  // flush against a vertical surface
    };

    constexpr size_t XR_PLANE_MAX_VERTS = 64;
    constexpr size_t XR_PLANE_MAX       = 32;

    // Horizontal = normal within this of vertical; vertical = normal within this of horizontal.
    constexpr float XR_PLANE_CLASS_TOL_RAD = 20.F * 3.14159265F / 180.F;

    struct SXRPlane {
        uint64_t                                id = 0;
        SXRPose                                 pose;
        float                                   extentX = 0.F, extentY = 0.F; // full extents along local X / Y
        std::array<SXRVec2, XR_PLANE_MAX_VERTS> poly{};                       // outer contour, plane-local
        uint8_t                                 nPoly      = 0;               // 0 = the extents rectangle
        eXRPlaneLabel                           label      = XR_PLANE_LABEL_UNKNOWN;
        eXRPlaneOrient                          orient     = XR_PLANE_ARBITRARY;
        eXRPlaneSource                          source     = XR_PLANE_SRC_SYNTHETIC;
        uint32_t                                generation = 0; // the source's detection generation
    };

    // What the frame thread holds: the planes of the newest accepted poll, in the CURRENT space.
    struct SXRPlaneSnapshot {
        std::array<SXRPlane, XR_PLANE_MAX> planes{};
        uint8_t                            n          = 0;
        uint32_t                           generation = 0; // bumps on every publish
    };

    const char*   xrPlaneLabelName(eXRPlaneLabel l);
    const char*   xrPlaneOrientName(eXRPlaneOrient o);
    const char*   xrPlaneSourceName(eXRPlaneSource s);
    const char*   xrSnapKindName(eXRSnapKind k);
    eXRPlaneLabel xrParsePlaneLabel(const std::string& s, bool& ok);

    // ---- geometry ----
    Vec3           xrPlaneNormal(const SXRPlane& p);
    eXRPlaneOrient xrClassifyPlane(const Vec3& normal);
    // A world point in plane-local coordinates: x,y in the plane, z = signed height along the normal.
    Vec3 xrPlaneLocal(const SXRPlane& p, const Vec3& world);
    Vec3 xrPlaneWorld(const SXRPlane& p, const SXRVec2& local, float height = 0.F);
    // Crossing-number test; winding-agnostic. A degenerate polygon (< 3 vertices) contains nothing.
    bool xrPointInPolygon(const SXRVec2& pt, const SXRVec2* poly, size_t n);
    // Inside the plane's contour (or its extents rectangle when it has no contour).
    bool xrPlaneContains(const SXRPlane& p, const SXRVec2& local);

    struct SXRPlaneRayHit {
        bool    hit    = false; // the ray meets the infinite plane ahead of its origin
        bool    inside = false; // ...inside the polygon
        float   t      = 0.F;
        Vec3    point;
        SXRVec2 local;
    };
    // `dir` need not be unit; t is in units of |dir|. A ray parallel to the plane never hits.
    SXRPlaneRayHit xrRayPlane(const SXRPlane& p, const Vec3& origin, const Vec3& dir);

    // ---- plane-relative storage (research/32 §5 C2) ----
    SXRPose xrPoseToPlane(const SXRPlane& p, const SXRPose& world);
    SXRPose xrPoseFromPlane(const SXRPlane& p, const SXRPose& local);

    // Re-express every plane across a reference-space change, `m` being the new origin in the old
    // space — the transform CXRAnchor::onReferenceSpaceChanged applies to anchor:local poses.
    void xrPlanesReexpress(SXRPlaneSnapshot& s, const SXRPose& m);
    // Find a plane by id; nullptr when the snapshot no longer has it.
    const SXRPlane* xrFindPlane(const SXRPlaneSnapshot& s, uint64_t id);

    // ---- the snap decision ----
    struct SXRSurfaceTuning {
        float snapDist     = 0.08F;   // enter: the window's bottom edge within this of the surface (m)
        float exitDist     = 0.12F;   // a previewed snap holds until it is this far (hysteresis, m)
        float maxPenetrate = 0.25F;   // ...also counts while pushed this far THROUGH the surface, or
                                      // deeper while the window's center is still above it (m)
        float standTiltRad = 0.209F;  // ~12 deg back-tilt of a standing window
        float lift         = 0.002F;  // gap kept between the window and the surface (m)
        float flatTiltCos  = 0.7071F; // a carried window whose normal is within 45 deg of the
                                      // surface normal lies flat (the wrist-tilt modifier)
        bool walls = true;            // snap flush onto vertical planes
    };

    // The carried window, as the snap sees it. `content` is the CONTENT center pose (what the anchor
    // solves); `bottomExt` is how far below the content center, along the window's own up axis, the
    // VISIBLE bottom edge sits — content half-height plus the bottom chrome margin and move-bar — so
    // the bar is what rests on the table, the same edge the user grabbed.
    struct SXRSurfaceQuery {
        SXRPose content;
        float   halfWidth = 0.5F; // full visible half-width (m)
        float   bottomExt = 0.3F;
        Vec3    head;
        bool    headValid = false;
        bool    haveRay   = false; // the aim (hand) or gaze ray, to prefer the plane it points at
        Vec3    rayOrigin, rayDir;
        bool    flat = false; // modifier: lie flat instead of standing
    };

    // Why a plane did not qualify (or XR_SURF_OK), for the carry trace (research/32 diagnosability).
    enum eXRSurfaceVerdict : uint8_t {
        XR_SURF_OK = 0,
        XR_SURF_ORIENT,       // ceilings, slopes, and walls with openxr:surfaces_walls off
        XR_SURF_TOO_HIGH,      // the window is farther above it than the entry distance
        XR_SURF_TOO_DEEP,      // pushed through too far: its center is below the surface as well
        XR_SURF_OUTSIDE,       // the bottom edge (or center, for a wall) is not over the plane
        XR_SURF_NONFINITE,
    };
    const char* xrSurfaceVerdictName(eXRSurfaceVerdict v);

    // What an evaluation saw: the numbers the carry trace logs.
    struct SXRSurfaceDiag {
        eXRSurfaceVerdict verdict   = XR_SURF_OK;
        float             lowest    = 0.F; // lowest visible corner above the plane (a wall: the center's distance)
        float             centerH   = 0.F; // the content center above the plane
        bool              contained = false;
    };

    struct SXRSurfaceCandidate {
        bool        ok      = false;
        size_t      index   = 0;
        uint64_t    planeId = 0;
        eXRSnapKind kind    = XR_SNAP_NONE;
        float       dist    = 0.F; // |signed distance| that qualified it (m)
        bool        rayHit  = false;
        Vec3        contact; // on the plane: the point the window's bottom edge (or center) lands on
        SXRPose     pose;    // the snapped CONTENT pose
    };

    // One plane, against an explicit entry distance (snapDist, or exitDist for the sticky plane).
    // An upward plane qualifies when the window's lowest corner is within `enterDist` above it and
    // either no deeper than maxPenetrate below it or the window still straddles it (its content
    // center is above the surface): a large window lowered THROUGH a table is on that table, however
    // far its bottom edge went. `diag`, when given, says what was seen and why it did not qualify.
    SXRSurfaceCandidate xrSurfaceEvaluate(const SXRPlane& p, const SXRSurfaceQuery& q, const SXRSurfaceTuning& t, float enterDist, SXRSurfaceDiag* diag = nullptr);
    // Every plane; a horizontal surface beats a wall, a higher horizontal surface beats a lower one
    // (both qualifying means the bottom edge is over both: the floor under a table is under the
    // table), then the plane the ray points at beats one it does not (it breaks ties between
    // surfaces at about the same height), then the nearest. `stickyId` (0 = none) is the plane already previewed: it is judged against
    // exitDist so a preview does not flicker at its threshold.
    SXRSurfaceCandidate xrSurfacePick(const SXRPlane* planes, size_t n, const SXRSurfaceQuery& q, const SXRSurfaceTuning& t, uint64_t stickyId);
    // The snapped CONTENT pose for a kind, contact and plane.
    SXRPose xrSnapPose(eXRSnapKind kind, const SXRPlane& p, const Vec3& contact, const SXRSurfaceQuery& q, const SXRSurfaceTuning& t);

    // The per-anchor arm the frame thread refreshes while a window is carried (CXRAnchor holds one).
    // It carries everything the RELEASE needs to re-decide against the latched release pose without
    // reaching back into the snapshot: the candidate plane itself (a copy), the query parameters
    // and the tuning. POD.
    struct SXRSurfaceArm {
        bool             havePlane = false;
        bool             preview   = false; // within the threshold: the solve shows the snapped pose
        SXRPlane         plane;
        SXRSurfaceQuery  query; // geometry of the window; `content` is refreshed per decision
        SXRSurfaceTuning tuning;
        SXRPose          previewPose;
        eXRSnapKind      previewKind = XR_SNAP_NONE;
    };

    // The release decision: re-evaluate the armed plane against the LATCHED release pose (the
    // WP-G4 ring has already rewound past the release jerk), with the exit distance if the snap was
    // being previewed. Returns a candidate with ok=false when the window should simply stay where
    // it was let go.
    SXRSurfaceCandidate xrSurfaceRelease(const SXRSurfaceArm& arm, const SXRPose& releaseContent, const Vec3& head, bool headValid);

    // What a released, snapped window remembers (CXRMonitorLayer::m_surfaceAttach). `local` is the
    // content pose in the plane's frame; `placed` is the world pose last applied, which is how the
    // frame thread notices that something else (a verb, a re-seat, a grab) has moved the window since.
    struct SXRSurfaceAttach {
        bool        valid     = false;
        bool        planeLost = false; // the plane vanished from the snapshot: holding the world pose
        uint64_t    planeId   = 0;
        eXRSnapKind kind      = XR_SNAP_NONE;
        SXRPose     local;
        SXRPose     placed;
    };

    enum eXRAttachStep : uint8_t {
        XR_ATTACH_HOLD = 0, // nothing to do (unchanged, or the plane is gone and we hold the pose)
        XR_ATTACH_FOLLOW,   // the plane was refined: move the window with it
        XR_ATTACH_DETACH,   // the plane jumped (a SLAM reset, a re-anchor): drop the relation, keep the pose
    };
    constexpr float XR_ATTACH_FOLLOW_MAX_M   = 0.25F;
    constexpr float XR_ATTACH_FOLLOW_MAX_RAD = 20.F * 3.14159265F / 180.F;
    constexpr float XR_ATTACH_EPS_M          = 0.0005F;

    // Pure follow step. `plane` is null when the snapshot no longer has the attached plane.
    eXRAttachStep xrAttachStep(const SXRSurfaceAttach& a, const SXRPlane* plane, SXRPose& out);

    // ---- sources ----
    // Synthetic planes (C0), `openxr:surfaces_synthetic`. Planes separated by ';', each:
    //     <label> pos:<x>,<y>,<z> size:<w>,<d> [yaw:<deg>]
    // label: table | platform | floor | ceiling | wall | plane. pos is the plane CENTER in LOCAL_FLOOR
    // metres; size is the extent along the plane's width and depth (height, for a wall); yaw turns it
    // about world up (degrees, + = counter-clockwise seen from above). A wall's normal faces +Z at
    // yaw 0, i.e. a wall in front of a user looking down -Z faces them. e.g.
    //     "table pos:0,0.74,-0.7 size:1.2,0.7; wall pos:0,1.2,-2.5 size:3,2.4"
    // Ids are 1..n in declaration order, with bit 63 set so they can never collide with a runtime id.
    std::expected<std::vector<SXRPlane>, std::string> xrParseSyntheticPlanes(const std::string& spec);
    constexpr uint64_t                                XR_PLANE_SYNTHETIC_ID_BIT = 1ULL << 63;

    // XR_EXT_plane_detection hands us a pose whose plane-in-local-frame convention the spec leaves
    // to prose. We assume the X-Y plane / +Z normal convention every other OpenXR plane API uses,
    // and VERIFY it against the orientation the runtime declared: when the declared class only fits
    // if +Y were the normal, the pose is rotated so +Z becomes that normal and the contour is
    // re-expressed with it (a proper rotation, (u,v) -> (u,-v)... see the .cpp). `swapped` reports it.
    // `declared` is the runtime's orientation enum mapped onto ours (ARBITRARY = no claim).
    SXRPlane xrCanonicalPlane(const SXRPose& rawPose, eXRPlaneOrient declared, const SXRVec2* rawPoly, size_t nRaw, float extentX, float extentY, bool& swapped);
}
