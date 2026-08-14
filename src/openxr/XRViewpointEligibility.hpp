#pragma once

#include "../helpers/math/Math.hpp"

#include <hyprutils/memory/Casts.hpp>

#include <cmath>
#include <cstdint>

namespace OpenXR {
    enum eXRViewpointRuntimeState : uint8_t {
        XR_VIEWPOINT_RUNTIME_UNKNOWN = 0,
        XR_VIEWPOINT_RUNTIME_VALID,
        XR_VIEWPOINT_RUNTIME_INVALID,
    };

    struct SXRViewpointRuntimeTransition {
        eXRViewpointRuntimeState state   = XR_VIEWPOINT_RUNTIME_UNKNOWN;
        bool                     changed = false;
    };

    inline SXRViewpointRuntimeTransition viewpointRuntimeTransition(eXRViewpointRuntimeState previous, bool valid) {
        const auto next = valid ? XR_VIEWPOINT_RUNTIME_VALID : XR_VIEWPOINT_RUNTIME_INVALID;
        return {.state = next, .changed = next != previous};
    }

    inline bool viewpointActivationUnchanged(uint32_t existingSurfaceId, uint32_t surfaceId, uint64_t existingEpoch, uint64_t existingToken, uint64_t resourceToken,
                                             uint32_t existingWidthUM, uint32_t widthUM, uint32_t existingHeightUM, uint32_t heightUM, bool anchorSame) {
        return existingSurfaceId == surfaceId && existingEpoch != 0 && existingToken == resourceToken && existingWidthUM == widthUM && existingHeightUM == heightUM && anchorSame;
    }

    // The v1 visual path accepts exactly one compositor mapping: a full-SBS buffer scaled as a
    // whole to the monitor's logical rectangle. Each eye occupies one half of that destination, so
    // the packed buffer and destination have the same aspect. No source crop, buffer scale,
    // transform, or implicit sizing is accepted; explicit aspect-preserving destination scaling is.
    inline bool viewpointSBSBufferMapping(const Vector2D& bufferSize, const Vector2D& surfaceSize, const Vector2D& monitorSize, bool hasSource, bool hasDestination,
                                          const Vector2D& destination, bool normalTransform, int scale) {
        if (hasSource || !hasDestination || !normalTransform || scale != 1 || monitorSize.x <= 0 || monitorSize.y <= 0 || bufferSize.x <= 0 || bufferSize.y <= 0 ||
            std::floor(bufferSize.x) != bufferSize.x || std::floor(bufferSize.y) != bufferSize.y || std::floor(monitorSize.x) != monitorSize.x ||
            std::floor(monitorSize.y) != monitorSize.y)
            return false;
        const int64_t BUFFERW = Hyprutils::Memory::sc<int64_t>(bufferSize.x);
        const int64_t BUFFERH = Hyprutils::Memory::sc<int64_t>(bufferSize.y);
        const int64_t DESTW   = Hyprutils::Memory::sc<int64_t>(monitorSize.x);
        const int64_t DESTH   = Hyprutils::Memory::sc<int64_t>(monitorSize.y);
        return destination == monitorSize && surfaceSize == monitorSize && BUFFERW % 2 == 0 && BUFFERW * DESTH == BUFFERH * DESTW;
    }
}
