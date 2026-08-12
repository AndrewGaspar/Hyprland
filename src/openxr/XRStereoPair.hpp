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
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace OpenXR::Stereo {

    using Render::Stereo::eContentLayout;

    // What the MAIN thread observed about one XR monitor, and the only three facts the policy
    // needs. Gathered where windows may be touched; the answer crosses to the frame thread as a
    // single published word (CXRMonitorLayer::m_stereoPairDecl).
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

    // ------------------------------------------------------------------------------------------
    // WP X3/X4 — THE SECOND PRODUCER
    //
    // Everything above is X1: no producer at all, one image, two imageRects. The depth desktop
    // (research/24 §6) is the other kind — the compositor composites the monitor TWICE, with each
    // window shifted by its own disparity, and hands us a genuinely double-wide buffer. The
    // submission is the same two quads; almost nothing else is.
    //
    // §5.6 is emphatic about why the two must not share a code path: the CONTENT pair's quad shows
    // one HALF of a packed image, so a ray hit must be un-mapped back into the whole image; the
    // DEPTH pair's quad shows the WHOLE logical desktop (twice, from two viewpoints), so the same
    // un-map would move the cursor half a screen. "Make the mapping a property of the producer."
    // That is exactly what eProducer is, and it is why it travels in the published word rather than
    // being inferred from the layout — the two producers use the SAME layout (a full side-by-side
    // split) and differ only in what that split MEANS.
    // ------------------------------------------------------------------------------------------

    enum eProducer : uint8_t {
        PRODUCER_NONE = 0, // one ordinary quad; the whole swapchain, both eyes
        PRODUCER_CONTENT,  // X1: a fullscreen client packed both eyes into one frame. Un-map: paneUVToContentUV
        PRODUCER_DEPTH,    // X3: the compositor composited the desktop once per eye. Un-map: IDENTITY
    };

    constexpr const char* producerToString(eProducer p) {
        switch (p) {
            case PRODUCER_CONTENT: return "content";
            case PRODUCER_DEPTH: return "depth";
            default: return "off";
        }
    }

    // THE PUBLISHED DECLARATION — main thread → frame thread, as ONE 64-bit word.
    //
    // It is one word rather than four atomics for a reason that bit X1's design review: a monitor
    // whose MODE has just changed (depth engaging, a mode retry) briefly has a swapchain sized for
    // the previous mode, and a declaration that describes the new one. Split across atomics, the
    // frame thread can read "split this in half" together with an image that is not the one being
    // described, and spend a frame showing each eye half of a mono desktop. Carrying the mode IN
    // the declaration makes that unrepresentable: the frame thread checks describes() and falls
    // back to one honest quad until the two agree.
    struct SPairDecl {
        eProducer      producer = PRODUCER_NONE;
        // How the image splits. For CONTENT this is the client's packing (sbs/hsbs/tab/htab) and the
        // kill switch is already folded in (X1's resolvePairLayout returns OFF when disabled). For
        // DEPTH it is always a full side-by-side pack, because that is how the monitor is physically
        // packed, and it stays set even when the kill switch is off — the pixels are laid out that
        // way whether or not we submit two quads, and the quad's ASPECT depends on it.
        eContentLayout layout   = Render::Stereo::CONTENT_OFF;
        // Submit as a PAIR. Separate from `layout` so a DEPTH monitor can degrade to ONE quad
        // (openxr:stereo_quad_pair = 0) showing pane 0 only — a mono desktop at the right shape —
        // rather than to a doubled side-by-side image no one can look at.
        bool           submit   = false;
        // The monitor pixel mode this declaration describes (the packed, double-wide one).
        uint32_t       modeW = 0, modeH = 0;

        bool           operator==(const SPairDecl&) const = default;
    };

    // producer(4) | layout(4) | submit(1) | modeW(24) | modeH(24) — 57 bits, one release-store.
    inline uint64_t packDecl(const SPairDecl& d) {
        constexpr uint64_t DIMMASK = (1ull << 24) - 1;
        return (static_cast<uint64_t>(d.producer) & 0xF) | ((static_cast<uint64_t>(d.layout) & 0xF) << 4) | ((d.submit ? 1ull : 0ull) << 8) |
            ((static_cast<uint64_t>(d.modeW) & DIMMASK) << 9) | ((static_cast<uint64_t>(d.modeH) & DIMMASK) << 33);
    }

    inline SPairDecl unpackDecl(uint64_t word) {
        constexpr uint64_t DIMMASK  = (1ull << 24) - 1;
        const auto         PRODUCER = static_cast<uint8_t>(word & 0xF);
        const auto         LAYOUT   = static_cast<uint8_t>((word >> 4) & 0xF);
        return {
            .producer = PRODUCER > PRODUCER_DEPTH ? PRODUCER_NONE : static_cast<eProducer>(PRODUCER),
            .layout   = LAYOUT > Render::Stereo::CONTENT_AUTO ? Render::Stereo::CONTENT_OFF : static_cast<eContentLayout>(LAYOUT),
            .submit   = ((word >> 8) & 1) != 0,
            .modeW    = static_cast<uint32_t>((word >> 9) & DIMMASK),
            .modeH    = static_cast<uint32_t>((word >> 33) & DIMMASK),
        };
    }

    // Does this declaration describe the image the frame thread is holding? A declaration with no
    // mode recorded (modeW == 0) is X1's original contract — "whatever image you have" — and is
    // accepted, so a CONTENT pair keeps working exactly as it did.
    inline bool describes(const SPairDecl& d, int32_t imageW, int32_t imageH) {
        if (d.modeW == 0 || d.modeH == 0)
            return true;
        return static_cast<int32_t>(d.modeW) == imageW && static_cast<int32_t>(d.modeH) == imageH;
    }

    // How many quads. Unlike the layout-only overload above this respects `submit`, so the kill
    // switch costs one layer instead of two the moment it is thrown.
    inline size_t quadsFor(const SPairDecl& d) {
        return d.submit && pairActive(d.layout) ? 2 : 1;
    }

    inline bool submissionFits(size_t quadsSoFar, uint32_t maxLayerCount, const SPairDecl& d) {
        return quadsSoFar + quadsFor(d) <= static_cast<size_t>(maxLayerCount);
    }

    // Resolve the per-frame declaration from two kinds of main-thread state which have very
    // different lifetimes:
    //
    //  * `mode` says how the OUTPUT buffer is physically laid out. A DEPTH pack is structural and
    //    changes only on a monitor modeChanged edge.
    //  * `content` describes the window currently covering an ordinary monitor. It may change on
    //    every presentation.
    //
    // Keeping the structural declaration authoritative is load-bearing during an Android system
    // activity / OpenXR visibility bounce. Such a bounce can stop presentation between two main-
    // thread observations, but it cannot turn an already double-wide output buffer into a mono
    // one. Publishing CONTENT_OFF for that buffer makes the frame thread rebuild a one-pane
    // swapchain at the packed width and submit the raw SBS image, squeezed 2:1, in both eyes.
    inline SPairDecl resolvePublishedDecl(const SPairDecl& mode, const SPairQuery& content) {
        if (mode.producer == PRODUCER_DEPTH) {
            SPairDecl out = mode;
            out.layout    = Render::Stereo::CONTENT_SBS;
            out.submit    = content.enabled;
            return out;
        }

        const auto layout = resolvePairLayout(content);
        return {
            .producer = pairActive(layout) ? PRODUCER_CONTENT : PRODUCER_NONE,
            .layout   = layout,
            .submit   = pairActive(layout),
        };
    }

    // ---- pane geometry inside the swapchain (WP X4) ----
    //
    // X1 needed none of this: one image, and each eye's rect was a half of the content. X4's chrome
    // does need it, because chrome only works if each eye's quad carries its OWN margin ring — a
    // single ring around a double-wide content rect puts the left eye's right-hand margin inside the
    // right eye's pane. So a DEPTH-packed swapchain is TWO MARGINED PANES side by side:
    //
    //     [ margin | pane 0 | margin ][ margin | pane 1 | margin ]
    //       <------- paneFull ------->
    //
    // and each eye's imageRect is simply its whole half. A mono layer is the degenerate case
    // (panes == 1, paneFull == the swapchain), so every expression below is the identity there and
    // the non-stereo path is bit-identical to what shipped.
    struct SPaneGeom {
        int      panes = 1;         // 1 or 2
        Vector2D paneFull;          // one pane's full rect (content + its own margins), px
        Vector2D paneContent;       // one pane's content rect, px
        Vector2D contentOffsetPx;   // content's top-left inside ONE pane, px

        bool     operator==(const SPaneGeom&) const = default;
    };

    inline Vector2D swapchainSizeFor(const SPaneGeom& g) {
        return {g.paneFull.x * std::max(1, g.panes), g.paneFull.y};
    }

    // Pane `idx`'s whole rect in imageRect's bottom-left space. Since the panes are stacked
    // horizontally and span the full height, the flip is a no-op on y — but it is written out so the
    // convention is visible next to paneImageRect's note about it.
    inline SImageRect paneFullRect(const SPaneGeom& g, int idx) {
        const int32_t I = std::clamp(idx, 0, std::max(0, g.panes - 1));
        return {static_cast<int32_t>(g.paneFull.x) * I, 0, static_cast<int32_t>(g.paneFull.x), static_cast<int32_t>(g.paneFull.y)};
    }

    // Where the desktop buffer's pane `idx` is blitted, in GL (bottom-left) swapchain pixels. The
    // compositor hands over ONE contiguous double-wide buffer; the two destinations are NOT
    // contiguous (a pane's right margin and its neighbour's left margin sit between them), which is
    // the whole reason blitBuffer draws twice instead of once.
    inline SImageRect paneContentDestGL(const SPaneGeom& g, int idx) {
        const int32_t I  = std::clamp(idx, 0, std::max(0, g.panes - 1));
        const int32_t PW = static_cast<int32_t>(g.paneFull.x);
        const int32_t CW = static_cast<int32_t>(g.paneContent.x);
        const int32_t CH = static_cast<int32_t>(g.paneContent.y);
        return {
            PW * I + static_cast<int32_t>(g.contentOffsetPx.x),
            static_cast<int32_t>(g.paneFull.y) - static_cast<int32_t>(g.contentOffsetPx.y) - CH,
            CW,
            CH,
        };
    }

    // ---- the un-map, per producer (§5.6) ----
    //
    // `paneUV` is a uv inside ONE pane's CONTENT rect (the chrome classifier already removed the
    // margins). The answer is a uv in the MONITOR's logical desktop, which is what absolute pointer
    // injection wants.
    //
    // CONTENT: the pane is half of a packed image, so the uv is squeezed back into that half.
    // DEPTH:   the pane IS the whole logical desktop — identity.
    //
    // And "the disparity must be subtracted" (§5.6) turns out to have nothing to subtract, which is
    // worth stating because the first reading of that line sends you looking for a hit test. The two
    // eye quads are COINCIDENT — same pose, same size — so a ray crosses them at one point, and what
    // the user fuses at that point is the cyclopean image, which sits at ZERO disparity by
    // construction (pane 0 shifts a raised window +s, pane 1 shifts it −s). The un-shifted desktop
    // coordinate is precisely what they are pointing at. The disparity enters on the way OUT
    // instead — the ray cursor is DRAWN into each pane with the shift of whatever it is over — which
    // is the same fact from the other side.
    inline Vector2D paneUVToMonitorUV(const Vector2D& paneUV, const SPairDecl& d, int eye) {
        if (d.producer == PRODUCER_DEPTH)
            return paneUV;
        return Render::Stereo::paneUVToContentUV(paneUV, d.layout, eye);
    }

    inline Vector2D monitorUVToPaneUV(const Vector2D& monitorUV, const SPairDecl& d, int eye) {
        if (d.producer == PRODUCER_DEPTH)
            return monitorUV;
        return Render::Stereo::contentUVToPaneUV(monitorUV, d.layout, eye);
    }

    // ---- the ray cursor's depth (§5.4, WP X3) ----
    //
    // "A cursor drawn at zero disparity over a raised window sits BEHIND the thing it is pointing
    // at." So the XR ray cursor gets the disparity of whatever it is over — and §5.4 asks for a
    // 60–100 ms ease, because a window edge is a step in that quantity and a cursor that snaps
    // between depths reads as a glitch rather than as motion.
    //
    // Exponential approach rather than a linear ramp: the target moves mid-ease every time the
    // pointer crosses a second edge, and an exponential has no notion of "how far it was going", so
    // an interrupted transition needs no state at all. `tauSec` is the time constant — the ease is
    // ~95% complete after 3τ, so §5.4's 60–100 ms window is τ ≈ 0.02–0.033 s.
    // ~80 ms to 95% (3τ), the middle of §5.4's 60–100 ms recommendation. A constant rather than a
    // config knob: it is a perceptual property of a step in depth, not a preference, and one more
    // openxr:* string on the frame thread is a cost this does not earn.
    inline constexpr float CURSOR_DISPARITY_EASE_TAU_SEC = 0.027F;

    inline float easeCursorDisparity(float current, float target, float dtSec, float tauSec) {
        if (!(dtSec > 0.F) || !(tauSec > 0.F))
            return target;
        const float ALPHA = 1.F - std::exp(-dtSec / tauSec);
        return current + (target - current) * std::clamp(ALPHA, 0.F, 1.F);
    }

    // The pane-space uv offset that shift belongs at, for pane `idx`. Pane 0 is the LEFT eye and
    // takes the POSITIVE shift, matching Desktop::Depth::eyeSign — a raised thing moves right in the
    // left eye. `shiftPaneUV` is the magnitude as a fraction of the pane's width.
    inline float cursorDisparityForPane(float shiftPaneUV, int idx) {
        return idx == 0 ? shiftPaneUV : -shiftPaneUV;
    }
}
