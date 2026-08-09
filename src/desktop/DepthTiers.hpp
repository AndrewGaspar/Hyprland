#pragma once

// Depth as a decoration axis — the pure ladder math (research/24 §7.2, §7.3, §8.2).
//
// `depth` is a unitless 0..1 height above the wallpaper plane. It is NOT a distance: §7.2's
// `decoration:depth_scale` turns it into metres of rise (0.12 m at depth 1.0), and only the D2
// producer converts that into a per-pane pixel shift (§8.1). Keeping the ladder here — header-only
// and state-free — means tests/desktop/DepthTiers.cpp exercises the very expressions CWindow and
// CLayerSurface run, not a copy of them (the StereoPacking.hpp pattern from WP F1/F3).
//
// The one design fact worth restating from §8.2, because every default here follows from it: what
// the eye can fuse foveally is the STEP between two things at once (~0.1° ≈ 3 px total at our
// defaults), not the absolute offset (~1° is fine). So this is a LADDER of a few tiers with small
// gaps, never a continuum — 0.0 wallpaper, 0.2 ordinary window, 0.6 focused, 0.8 bar/overlay.
//
// WP D1 shipped the ladder; WP D2 added the §8.1 half below it — the producer that turns a rung
// into pixels of horizontal disparity. Everything here is still a pure function of its arguments.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <numbers>
#include <optional>

namespace Desktop::Depth {

    // The wallpaper plane and the ceiling. Depth is deliberately one-sided: §8.2 point 2 (Shibata
    // et al.) measured crossed (toward-viewer) disparities as the more comfortable direction, so
    // things rise off the page and nothing is ever pushed behind it.
    inline constexpr float MIN = 0.F;
    inline constexpr float MAX = 1.F;

    inline float           clamp(float depth) {
        return std::clamp(depth, MIN, MAX);
    }

    // The zero-config ladder, i.e. `decoration:depth_*` read into one struct so the resolution
    // below stays a pure function. Defaults here mirror ConfigValues.cpp; they exist so the test
    // can pin the shipped ladder without a config manager.
    struct STiers {
        float focused   = 0.6F; // the focused window rises — the rise IS the focus indicator (§7.3)
        float unfocused = 0.2F; // one small step off the page, so the page is the wallpaper
        float layers    = 0.8F; // top/overlay layer surfaces: waybar, mako, walker
    };

    // zwlr_layer_shell_v1 layer values, spelled out so this header needs no protocol include.
    inline constexpr uint32_t LAYER_BACKGROUND = 0;
    inline constexpr uint32_t LAYER_BOTTOM     = 1;
    inline constexpr uint32_t LAYER_TOP        = 2;
    inline constexpr uint32_t LAYER_OVERLAY    = 3;

    // §7.3: a fullscreen window IS the plane — raising it just moves the whole panel, so it drops
    // to 0 rather than carrying the focused tier.
    inline float windowTier(const STiers& tiers, bool focused, bool fullscreen) {
        if (fullscreen)
            return MIN;
        return clamp(focused ? tiers.focused : tiers.unfocused);
    }

    // §7.3: background/bottom (the wallpaper) stay pinned at 0 — anything else destroys the sense
    // of a *page*. top/overlay are the "hovering above the page" tier.
    inline float layerTier(const STiers& tiers, uint32_t layer) {
        if (layer < LAYER_TOP)
            return MIN;
        return clamp(tiers.layers);
    }

    // §7.2: "rules are for tuning; the feature should work with none". So an explicit
    // `windowrule = depth <z>` / `layerrule = depth <z>` beats the tier — including on a fullscreen
    // window or a wallpaper layer, where the tier would otherwise pin 0 — but it is still clamped
    // into the comfort range.
    inline float resolve(const std::optional<float>& ruleOverride, float tier) {
        return ruleOverride.has_value() ? clamp(*ruleOverride) : tier;
    }

    // §7.2: `decoration:depth_scale` is the comfort knob — metres of rise at depth 1.0. This is the
    // only place depth becomes physical; D2's disparity formula (§8.1) starts from this number.
    inline float riseMetres(float depth, float depthScale) {
        return clamp(depth) * depthScale;
    }

    // §8.2's binding constraint expressed as something a test can assert: the largest gap between
    // two adjacent rungs the user can see side by side. Ordered 0 (wallpaper) → unfocused →
    // focused → layers, which is the ladder as shipped.
    inline float largestLadderStep(const STiers& tiers) {
        const float RUNGS[] = {MIN, clamp(tiers.unfocused), clamp(tiers.focused), clamp(tiers.layers)};

        float       largest = 0.F;
        for (size_t i = 1; i < std::size(RUNGS); ++i)
            largest = std::max(largest, std::abs(RUNGS[i] - RUNGS[i - 1]));

        return largest;
    }

    // ---------------------------------------------------------------------------------------
    // §8.1 — from a rung on the ladder to pixels of horizontal disparity. This is WP D2's core
    // arithmetic and the reason the ladder is unitless: a depth is a *style*, and only here does
    // it meet a screen.
    // ---------------------------------------------------------------------------------------

    // The geometry of the virtual screen ONE eye sees. The defaults reproduce §8.1's worked table
    // exactly (openxr:default_size 1.6 m at openxr:default_distance 1.5 m, P = 1920 → 1200 px/m),
    // which is also a fair description of birdbath glasses; `decoration:depth_distance` and
    // `decoration:depth_screen_width` move it to a user's own desk. Interocular is not a config
    // key — §8.1 says "use 0.063 m" and a wrong value here is a comfort hazard, not a preference.
    struct SGeometry {
        float distanceM    = 1.5F;   // D — eye to the screen plane
        float screenWidthM = 1.6F;   // W — how wide ONE pane is perceived to be
        float interocularM = 0.063F; // b
    };

    // p = b · (1 − D/d) with d = D − rise. Metres, and NEGATIVE in front of the screen, which is
    // §8.1's sign convention and the only direction we ever go (§8.2 point 2).
    inline float parallaxMetres(float riseM, const SGeometry& geo) {
        // nothing may rise onto the bridge of the nose: hold the element 1 cm short of the eye so
        // the ratio stays finite for absurd depth_scale values
        const float DIST = std::max(geo.distanceM - riseM, 0.01F);
        return geo.interocularM * (1.F - geo.distanceM / DIST);
    }

    // |Δ| = |p/2| · (P/W) — the magnitude of ONE pane's shift, in pane pixels.
    inline float shiftMagnitudePx(float riseM, const SGeometry& geo, float panePixels) {
        if (geo.screenWidthM <= 0.F || panePixels <= 0.F)
            return 0.F;

        return std::abs(parallaxMetres(riseM, geo) / 2.F) * (panePixels / geo.screenWidthM);
    }

    // §8.2 budget 1, the 1° rule, as a hard ceiling rather than a warning: whatever a user asks
    // for with `depth_scale`, no element is ever pushed past one degree of angular disparity.
    // ≈15.7 px per pane at the shipped defaults, against the ≈3.3 px depth 1.0 actually asks for.
    inline float comfortCeilingPx(const SGeometry& geo, float panePixels, float degrees = 1.F) {
        if (geo.screenWidthM <= 0.F || panePixels <= 0.F)
            return 0.F;

        const float PMAX = geo.distanceM * std::tan(degrees * (std::numbers::pi_v<float> / 180.F));
        return (PMAX / 2.F) * (panePixels / geo.screenWidthM);
    }

    // §8.1: "left pane +, right pane − for d < D". Pane 0 is the left eye in every layout the
    // pack knows (StereoPacking::paneDestBox is row-major and sbs puts pane 0 at x = 0).
    inline float eyeSign(int paneIdx) {
        return paneIdx == 0 ? 1.F : -1.F;
    }

    // §6.3 pt 2, the eye-AGNOSTIC magnitude. Both panes exist in the same frame, so anything asking
    // "where might this be drawn" — damage, visibility culling — has to answer [box − s, box + s].
    // Every per-pane shift is this number times an eye sign, so a damage box grown by it on each
    // side provably covers every pane's draw; that identity is the whole reason it lives here
    // rather than being computed twice.
    inline float damageSpreadPx(float depth, float depthScale, const SGeometry& geo, float panePixels) {
        return std::min(shiftMagnitudePx(riseMetres(depth, depthScale), geo, panePixels), comfortCeilingPx(geo, panePixels));
    }

    // §6.1's v1 recommendation, the frame-violation clamp. A raised element whose LEFT or RIGHT
    // edge sits at the panel edge is cut differently in the two eyes, and the sliver visible to
    // only one eye is the severe artifact (retinal rivalry) — so the shift may not exceed the
    // element's own margin from the frame. `slackPx` is the sliver we accept anyway
    // (`decoration:depth_edge_slack`): with a hard zero, a full-width bar — the exact element the
    // design most wants floating — could never leave the page at all.
    //
    // Both edges bound the shift, not just the one the pane moves toward: the pair moves the
    // element BOTH ways at once, so whichever margin is smaller is the one that governs.
    inline float clampToFrame(float shiftPx, double boxX, double boxW, double frameW, float slackPx) {
        const double MARGIN = std::min(boxX, frameW - (boxX + boxW));
        const float  LIMIT  = std::max(static_cast<float>(std::max(MARGIN, 0.0)), std::max(slackPx, 0.F));
        return std::clamp(shiftPx, -LIMIT, LIMIT);
    }

    // The whole chain in one expression, so what ships and what the tests assert cannot drift.
    // Returns the SIGNED shift for pane `paneIdx`, in pane pixels, before the edge clamp (which
    // needs the element's box and therefore lives at the call site).
    inline float paneShiftPx(float depth, float depthScale, const SGeometry& geo, float panePixels, int paneIdx) {
        return eyeSign(paneIdx) * damageSpreadPx(depth, depthScale, geo, panePixels);
    }
}
