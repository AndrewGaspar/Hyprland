#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace ViewpointDemo {

    struct SVec3 {
        double x = 0.0;
        double y = 0.0;
        double z = 0.0;
    };

    struct SPortalSize {
        double widthMeters  = 0.0;
        double heightMeters = 0.0;
    };

    struct SRenderSize {
        uint32_t width  = 0;
        uint32_t height = 0;
    };

    struct SRay {
        SVec3 origin;
        SVec3 direction;
    };

    struct SFrustum {
        double left   = 0.0;
        double right  = 0.0;
        double bottom = 0.0;
        double top    = 0.0;
        double near   = 0.0;
    };

    struct SStereoViews {
        SVec3 left;
        SVec3 right;
    };

    struct SImage {
        std::span<uint32_t> pixels;
        uint32_t            width        = 0;
        uint32_t            height       = 0;
        uint32_t            stridePixels = 0;
    };

    // Builds the fixed-orientation, eye-translated off-axis frustum through the
    // physical portal edges. The gameplay/world camera remains unchanged.
    bool offAxisFrustum(const SVec3& eye, const SPortalSize& portal, double nearPlane, SFrustum& out);

    // Immutable gameplay-world impact point for the authoritative orthogonal aim
    // ray. Viewpoint samples never alter this state; they only alter its projection.
    SVec3 authoritativeAimImpact();

    // Chooses the largest per-eye render size within the requested bounds whose
    // aspect ratio exactly equals one half of a packed full-SBS destination.
    // The destination width must be even; impossible ratios fail closed.
    bool fitSBSRenderSize(uint32_t packedDestinationWidth, uint32_t packedDestinationHeight, uint32_t maximumEyeWidth, uint32_t maximumHeight, SRenderSize& out);

    // Returns the ray from a surface-relative eye through the selected pixel center
    // on the portal plane. The portal center is the origin and +Z points toward the
    // viewer. This is the ray form of the conventional asymmetric frustum:
    // l/r/b/t = near * (portal edge - eye coordinate) / eye.z.
    bool portalRay(const SVec3& eye, const SPortalSize& portal, uint32_t pixelX, uint32_t pixelY, uint32_t paneWidth, uint32_t paneHeight, SRay& out);

    // Renders one pair-latched sample into an even-width full-SBS image. Both panes are produced
    // from the same immutable scene and simulation state; only eye position changes.
    bool renderPortalSBS(const SImage& image, const SPortalSize& portal, const SStereoViews& views);

    // Renders a static, zero-disparity full-SBS calibration image for inactive or
    // stale feedback. It deliberately carries no viewpoint/sample association.
    bool renderFallbackSBS(const SImage& image);

    // Stable semantic hash over visible pixels (stride padding is ignored).
    uint64_t pixelHash(const SImage& image);

}
