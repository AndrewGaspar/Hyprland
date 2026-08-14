#pragma once

// XRViewpoint — pure surface-relative viewpoint geometry for view-dependent XR content.
//
// This header is compiled unconditionally: it deliberately has no OpenXR, Wayland, renderer, or
// client-projection dependencies. The compositor supplies a content surface's world pose and
// physical size plus one or two world-space view positions. The result is the small geometry
// contract a future transport can expose to a cooperating client.
//
// Surface-local coordinates use the same convention as XRMath's quad helpers: origin at the
// content center, +X toward surface-right, +Y up, and +Z out of the surface toward its viewer.
// Units are meters. View order is caller-defined and preserved (for stereo: left, then right).

#include "XRMath.hpp"

#include <array>
#include <cstddef>

namespace OpenXR {
    inline constexpr float XR_VIEWPOINT_Z_EPSILON = 1e-4F;

    struct SXRViewpointGeometry {
        bool                valid        = false;
        size_t              viewCount    = 0;
        float               widthMeters  = 0.F;
        float               heightMeters = 0.F;
        std::array<Vec3, 2> viewPositions;
    };

    // Transform one or two world-space view positions into `contentWorldPose`'s local frame. The
    // content pose and all view positions must already be expressed in the SAME solver/world
    // reference frame. In particular, applying or removing HypXRland's LOCAL-fallback floor offset
    // is the caller's responsibility; this pure geometry helper does no reference-space conversion.
    //
    // `valid` is true only when the count is 1..2, the content pose and dimensions are finite, the
    // dimensions are positive, every requested view position is finite, and every transformed view
    // lies strictly farther than XR_VIEWPOINT_Z_EPSILON in front of the content plane. Finite local
    // positions are retained verbatim when a viewer is at or behind the plane; their Z is never
    // clamped into a seemingly usable frustum. A structurally invalid count, size, pose, or
    // orientation returns the default geometry (including viewCount == 0 and zero dimensions).
    SXRViewpointGeometry surfaceRelativeViewpoint(const SXRPose& contentWorldPose, float widthMeters, float heightMeters, const std::array<Vec3, 2>& viewWorldPositions,
                                                  size_t viewCount);
}
