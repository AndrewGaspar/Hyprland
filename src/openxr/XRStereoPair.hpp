#pragma once

// The OpenXR quad PAIR — research/24 §5.1(a), §5.2, WP X1. The pure half.
//
// Phase F gave a flat output two panes by packing them into the two halves of a scanout buffer.
// This is the other presenter of the same primitive (§2): on an XR monitor there is no scanout
// buffer to pack, there is a quad, and OpenXR already knows how to show one image to one eye.
// So instead of producing anything, we submit the SAME swapchain image TWICE — once with
// eyeVisibility LEFT and once with RIGHT — and let each quad's subImage.imageRect select that
// eye's half of the content the client already packed.
//
// That is the whole mechanism, and its consequence is the reason X1 is small: there is NO
// producer. No second composite, no double-wide swapchain, no reallocation when stereo engages
// or leaves. The compositor renders the monitor exactly as it does today; a fullscreen stereo
// client's packed frame IS the content rect, and the runtime's sampler does the split it was
// going to do anyway. (The depth desktop needs the real producer — two panes with genuinely
// different pixels — and that is X3's double-wide swapchain, not this.)
//
// Everything here is a pure function so tests/xr/StereoPair.cpp exercises the expressions the
// frame thread runs rather than a copy of them (the StereoPacking.hpp / StereoContent.hpp
// discipline). Nothing here allocates, locks, touches a refcount or reads config — it is called
// from the frame thread, where all three are forbidden (XRMonitorLayer.hpp's thread-safety rule).

#include "../render/StereoContent.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace OpenXR::Stereo {

    using Render::Stereo::eContentLayout;

    // What the MAIN thread observed about one XR monitor, and the only three facts the policy
    // needs. Gathered where windows may be touched; the answer crosses to the frame thread as a
    // single published byte (CXRMonitorLayer::m_stereoPairLayout).
    struct SPairQuery {
        // The resolved content declaration of the window that owns this output — already through
        // the rule fold, the client's tag and the fullscreen gate (CWindow::stereoLayout()).
        eContentLayout declared = Render::Stereo::CONTENT_OFF;

        // Does that window's box cover the whole output? THE load-bearing gate, and the reason it
        // is asked separately from `declared`: the pair splits the monitor's ENTIRE content rect,
        // not one window's texture. A stereo window that is merely `always`-declared and floating
        // would otherwise send the left half of the *desktop* to the left eye — bars, wallpaper and
        // all — which is the "the compositor broke" failure mode §4.3 exists to prevent. Per-window
        // stereo on an otherwise-mono XR monitor needs the real producer (§5.3) and is X3/X4 work.
        bool coversOutput = false;

        // openxr:stereo_quad_pair. The kill switch: if a runtime turns out to mishandle
        // eyeVisibility or imageRect, this restores the single-quad flatten in one keyword.
        bool enabled = true;
    };

    // THE ACTIVATION PREDICATE. Returns the layout to split the content rect with, or CONTENT_OFF
    // for "submit one ordinary quad, exactly as before".
    //
    // CONTENT_AUTO cannot reach a presenter — it is a delegation to the client's tag that the
    // declaration fold already resolved — so viewCount() rejects it along with mono. That is not a
    // formality: an unresolved AUTO reaching here would split the screen on a layout nobody chose.
    inline eContentLayout resolvePairLayout(const SPairQuery& query) {
        if (!query.enabled || !query.coversOutput)
            return Render::Stereo::CONTENT_OFF;

        if (Render::Stereo::viewCount(query.declared) < 2)
            return Render::Stereo::CONTENT_OFF;

        return query.declared;
    }

    inline bool pairActive(eContentLayout layout) {
        return Render::Stereo::viewCount(layout) >= 2;
    }

    // How many composition layers this monitor costs this frame.
    inline size_t quadsFor(eContentLayout layout) {
        return pairActive(layout) ? 2 : 1;
    }

    // §F2's cost note, as the invariant it protects: the layer budget must be checked for the
    // WHOLE pair, never one quad at a time. A pair that fits half-way is submitted LEFT-eye-only —
    // one eye sees the desktop and the other sees nothing, which is not a degraded picture but a
    // nauseating one. So a monitor whose pair does not fit is skipped entirely and the next
    // (cheaper) monitor may still be submitted.
    inline bool submissionFits(size_t quadsSoFar, uint32_t maxLayerCount, eContentLayout layout) {
        return quadsSoFar + quadsFor(layout) <= static_cast<size_t>(maxLayerCount);
    }

    // A rectangle of swapchain pixels, in the coordinate system XrSwapchainSubImage::imageRect uses
    // for an OpenGL(ES) swapchain: **origin bottom-left**, the same convention glViewport takes and
    // therefore the same one CXRGraphics::blitBuffer already computes as `contentGL`.
    //
    // That origin is not a guess. A GL client submits every layer through comp_gl_client.c, which
    // sets `flip_y`; the compositor's set_post_transform_rect then rewrites the normalized rect to
    // (x, y + h, w, -h), so layer_quad.vert samples the quad's TOP row at (offset.y + extent.h) and
    // its BOTTOM row at offset.y. Bottom-left. WiVRn's squasher carries the same helper verbatim.
    //
    // It matters for exactly one thing — which half is which on an over-under pack — and getting it
    // backwards swaps the eyes, which is uncomfortable rather than obviously broken. Hence the note.
    struct SImageRect {
        int32_t x = 0, y = 0, w = 0, h = 0;

        bool    operator==(const SImageRect&) const = default;
    };

    // ONE eye's rectangle inside the swapchain's content rect. `content` is the blit's destination
    // (CXRMonitorLayer::m_contentSize at m_contentOffsetPx, y already flipped to GL). Identity for
    // a mono layout, so the caller needs no branch.
    //
    // Eye 0 is the LEFT eye and takes the left half (sbs/hsbs) or the TOP half (tab/htab) — the
    // left-first convention of Matroska's StereoMode, ISOBMFF's st3d, cropForEye() and the flat
    // presenter's pane order. "Top" is the high-y end here, because y counts up from the bottom.
    //
    // An odd content width or height drops its middle row/column rather than overlapping the panes
    // or reading one pixel past the rect: both eyes always get exactly floor(n/2), and the right/
    // bottom pane is anchored to the far edge so the two halves stay symmetric about the centre.
    inline SImageRect paneImageRect(const SImageRect& content, eContentLayout layout, int eye) {
        if (!pairActive(layout))
            return content;

        const int32_t VIEW = std::clamp(eye, 0, 1);
        SImageRect    out  = content;

        if (Render::Stereo::splitsHorizontally(layout)) {
            const int32_t HALF = std::max<int32_t>(0, content.w / 2);
            out.w              = HALF;
            out.x              = VIEW == 0 ? content.x : content.x + content.w - HALF;
        } else {
            const int32_t HALF = std::max<int32_t>(0, content.h / 2);
            out.h              = HALF;
            // Eye 0 is the TOP half, and top is the HIGH y end in this bottom-left space.
            out.y              = VIEW == 0 ? content.y + content.h - HALF : content.y;
        }

        return out;
    }
}
