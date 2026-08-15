#pragma once

#include "../helpers/math/Math.hpp"

#include <hyprutils/memory/Casts.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <tuple>

namespace OpenXR {
    inline constexpr size_t XR_VIEWPOINT_MAX_SUBSURFACE_DEPTH   = 32;
    inline constexpr size_t XR_VIEWPOINT_MAX_SUBSURFACE_WATCHES = 256;

    // The sort key the eligibility walk orders viewpoints by, so that which client wins a contested
    // monitor is a deterministic function of the surfaces rather than of unordered_map iteration.
    //
    // `hasSurface` is NOT decoration. A viewpoint outlives its wl_surface (the protocol keeps the
    // object usable until the client destroys it — XRViewpointProtocol::surfaceDestroyed only drops
    // the reference), so the list routinely mixes entries with and without a surface id. Ordering
    // "by id when BOTH have one, by pointer otherwise" is NOT a strict weak ordering across such a
    // mix: with a surface-less B sitting between two live A and C by pointer, A < C by id while
    // C < B < A by pointer closes a cycle, and libstdc++'s insertion sort walks off the front of the
    // range on a cyclic comparator — memory corruption, not merely a surprising order. So the key is
    // compared as a whole tuple: surfaced viewpoints first, then by surface id, then by identity
    // (the resource address, which only ever breaks ties the earlier fields left equal).
    struct SXRViewpointOrderKey {
        bool      hasSurface = false;
        uint32_t  surfaceId  = 0;
        uintptr_t identity   = 0;

        bool      operator==(const SXRViewpointOrderKey&) const = default;
    };

    inline bool viewpointOrderBefore(const SXRViewpointOrderKey& a, const SXRViewpointOrderKey& b) {
        return std::tuple{!a.hasSurface, a.surfaceId, a.identity} < std::tuple{!b.hasSurface, b.surfaceId, b.identity};
    }

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

    inline bool viewpointSurfaceStateRequiresReevaluation(bool transform, bool scale, bool viewport, bool offset, bool bufferGeometryChanged) {
        return transform || scale || viewport || offset || bufferGeometryChanged;
    }

    inline bool viewpointSubsurfaceWatchWithinBudget(size_t depth, size_t watchCount) {
        return depth < XR_VIEWPOINT_MAX_SUBSURFACE_DEPTH && watchCount < XR_VIEWPOINT_MAX_SUBSURFACE_WATCHES;
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
