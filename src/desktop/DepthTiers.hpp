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
// WP D1 note: nothing renders from these values yet. D2 is the producer.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
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
}
