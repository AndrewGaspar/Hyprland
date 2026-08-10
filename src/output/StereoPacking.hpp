#pragma once

// Stereo output packing — the pure geometry (research/24 §3).
//
// A stereo monitor scans out a mode assembled from N identical per-eye PANES; the compositor
// presents ONE logical monitor at pane size, and the pack is a final-blit scanout detail. These
// are the exact expressions CMonitor and the renderer run — kept header-only and state-free so
// tests/output/StereoPacking.cpp exercises the same code, not a copy (WP F3). Every function is
// the identity when mode == STEREO_OFF, which is what keeps stereo:off bit-identical to stock.

#include "../config/shared/monitor/MonitorRule.hpp"
#include "../helpers/math/Math.hpp"

#include <algorithm>
#include <span>

namespace Monitor::Stereo {

    // {columns, rows} of identical panes packed into the scanout mode. {1,1} when off;
    // F5 layouts (hsbs/tab/htab) are new divisors and destination boxes, not new mechanisms.
    inline Vector2D packDivisor(Config::eMonitorStereoMode mode) {
        switch (mode) {
            case Config::STEREO_SBS: return {2, 1};
            default: return {1, 1};
        }
    }

    inline int paneCount(Config::eMonitorStereoMode mode) {
        const auto DIV = packDivisor(mode);
        return static_cast<int>(DIV.x * DIV.y);
    }

    // ONE pane of the mode-sized scanout buffer — the size the whole render pass above the
    // final blit works at. == pixelSize when off.
    inline Vector2D paneSize(const Vector2D& pixelSize, Config::eMonitorStereoMode mode) {
        return pixelSize / packDivisor(mode);
    }

    // pane idx's destination box inside the mode-sized scanout buffer, row-major.
    inline CBox paneDestBox(const Vector2D& pixelSize, Config::eMonitorStereoMode mode, int idx) {
        const auto PANE = paneSize(pixelSize, mode);
        const auto DIV  = packDivisor(mode);
        const int  COL  = idx % static_cast<int>(DIV.x);
        const int  ROW  = idx / static_cast<int>(DIV.x);
        return {COL * PANE.x, ROW * PANE.y, PANE.x, PANE.y};
    }

    // THE MODE A STEREO RULE ASKS THE DISPLAY FOR — and the one place the two kinds of stereo
    // output differ (research/24 §6.2's "this needs no change to the monitor's mode", WP X3).
    //
    // A PHYSICAL stereo output (the `monitor = …, stereo:sbs` token, WP F1) names a mode the panel
    // really has: 3840x1080 on an XREAL in 3D mode. The rule's resolution IS the packed mode and
    // the logical desktop is derived by halving it — the panel's mode is the fixed quantity.
    //
    // A VIRTUAL pack has no panel. A headless/XR output invents its own scanout, so the fixed
    // quantity is the other one: the user declares the size they want to WORK at (an `xrmonitor`
    // at 2560x1440 stays 2560x1440 per eye) and the packed mode is derived by doubling it. Halving
    // a declared XR size instead would silently shrink every XR desktop the moment the depth
    // producer engaged, which is the wrong semantics for an output that has no panel to respect.
    //
    // Everything downstream (deriveGeometry, sanitizeStereoMode, backComputeMode) is identical for
    // both — only the request differs, and it differs here, once.
    inline Vector2D requestedMode(const Vector2D& ruleResolution, Config::eMonitorStereoMode mode, bool virtualPack) {
        if (!virtualPack || mode == Config::STEREO_OFF)
            return ruleResolution;
        // `preferred` (0,0) and the highrr/highres/maxwidth sentinels (-1,-N) are mode REQUESTS, not
        // resolutions — there is nothing to double, and doubling a sentinel would invent a mode.
        if (ruleResolution.x <= 0 || ruleResolution.y <= 0)
            return ruleResolution;
        return ruleResolution * packDivisor(mode);
    }

    // sanitize predicate: the committed mode must divide cleanly into >= 1x1 panes, or the
    // packing is dropped loudly rather than deriving fractional pane sizes (§3.4 item 1).
    inline bool modeDivides(const Vector2D& pixelSize, Config::eMonitorStereoMode mode) {
        const Vector2D PANE = paneSize(pixelSize, mode);
        return PANE == PANE.floor() && PANE.x >= 1 && PANE.y >= 1;
    }

    // sanitize predicate 2 (§3.4 item 15): the pack is only valid on the mode it was CONFIGURED
    // for. A display that is not in side-by-side mode shows one eye's half stretched across the
    // whole panel, so a mode fallback landing anywhere else has to drop the packing.
    //
    // `requestedMode` is a mode REQUEST, not a resolution: `preferred` is Vector2D() and
    // highrr/highres/maxwidth are the (-1,-1)/(-1,-2)/(-1,-3) sentinels (Parser.cpp parseMode) —
    // none of which can be compared against a committed mode. A rule that names a resolution is a
    // contract; the rest ask for "whatever the mode search picks", and only the emergency
    // any-available-mode fallback (`searchFellBack`) breaks that contract.
    inline bool modeIsAsRequested(const Vector2D& committedMode, const Vector2D& requestedMode, bool searchFellBack = false) {
        if (requestedMode.x > 0 && requestedMode.y > 0)
            return committedMode == requestedMode;
        return !searchFellBack;
    }

    // §3.4 item 15b — the pack is validated when a rule is applied, but the hazard is CONTINUOUS.
    // A display can swap its mode list under a connector that never disconnects: the XREAL falls
    // from its 3D personality (3840x1080-only EDID) to its 2D one (1920x1080-only) on a USB
    // re-enumeration and keeps the same connector. Nothing in the tree signals that — aquamarine's
    // IOutput has no modes-changed event, `m_output->modes` is read only inside applyMonitorRule,
    // and CMonitorRuleManager::ensureMonitorStatus skips a monitor whose RULE did not change. So
    // m_pixelSize and m_stereoMode freeze at the last good apply while the panel stops splitting,
    // and the compositor packs two panes into a mode the display no longer presents side by side.
    //
    // A packed monitor therefore has to re-ask. This is the whole decision, as arithmetic over the
    // connector's advertised mode list, so it is one gtest away from the code that runs.
    enum eStereoWatchAction : uint8_t {
        STEREO_WATCH_NOTHING = 0,
        STEREO_WATCH_DROP,   // the pack is live on a mode the panel no longer offers
        STEREO_WATCH_READOPT // the pack is off, the rule still wants it, and its mode came back
    };

    inline bool modeAdvertised(const Vector2D& mode, const std::span<const Vector2D> advertised) {
        return std::ranges::find(advertised, mode) != advertised.end();
    }

    // `packMode` is what the monitor is packing RIGHT NOW (CMonitor::m_stereoMode, already
    // sanitized); `ruleMode`/`requestedMode` are what the active rule asks for. `onCustomMode` is
    // the one legitimate way a committed mode is absent from the advertised list (a user modeline),
    // and it must never be read as a fall.
    //
    // READOPT is deliberately narrower than DROP. Dropping is a safety action and applies to every
    // request form. Re-adopting re-modesets the panel, so it only fires when the rule NAMES a
    // resolution and that exact resolution is back — `preferred`/`highrr`/`highres`/`maxwidth` ask
    // for "whatever the search picks", which is not a thing a timer may decide to chase.
    inline eStereoWatchAction watchAction(Config::eMonitorStereoMode packMode, Config::eMonitorStereoMode ruleMode, const Vector2D& committedMode, const Vector2D& requestedMode,
                                          const std::span<const Vector2D> advertised, bool onCustomMode = false) {
        if (packMode != Config::STEREO_OFF)
            return !onCustomMode && !modeAdvertised(committedMode, advertised) ? STEREO_WATCH_DROP : STEREO_WATCH_NOTHING;

        if (ruleMode == Config::STEREO_OFF || requestedMode.x <= 0 || requestedMode.y <= 0)
            return STEREO_WATCH_NOTHING;

        return modeAdvertised(requestedMode, advertised) && committedMode != requestedMode ? STEREO_WATCH_READOPT : STEREO_WATCH_NOTHING;
    }

    // the size the fractional-scale check must validate — the PANE over the scale, never the mode
    // over the scale (§3.4 item 11). == pixelSize / scale when off.
    inline Vector2D scaleValidationSize(const Vector2D& pixelSize, Config::eMonitorStereoMode mode, float scale) {
        return paneSize(pixelSize, mode) / scale;
    }

    struct SDerivedGeometry {
        Vector2D logicalSize;     // CMonitor::m_size
        Vector2D transformedSize; // CMonitor::m_transformedSize — ONE pane, NOT the mode
    };

    // research/24 §3.2: the logical size derives from ONE PANE, not the mode. The pack sits
    // strictly below transformedSize (like zoom/mirror, a final-blit stage), so every
    // logical↔buffer round-trip in the tree (m_size * m_scale == m_transformedSize) stays true.
    // Bit-identical to the stock derivation when off.
    inline SDerivedGeometry deriveGeometry(const Vector2D& pixelSize, Config::eMonitorStereoMode mode, wl_output_transform transform, float scale) {
        const Vector2D PANE = paneSize(pixelSize, mode);
        const Vector2D XFMD = transform % 2 == 1 ? Vector2D{PANE.y, PANE.x} : PANE;
        return {.logicalSize = (XFMD / scale).round(), .transformedSize = XFMD};
    }

    // the m_createdByUser dance: back-compute the MODE from the (pane) transformedSize —
    // un-transform the pane, then re-pack (×{1,1} when off). Inverse of deriveGeometry.
    inline Vector2D backComputeMode(const Vector2D& transformedSize, wl_output_transform transform, Config::eMonitorStereoMode mode) {
        CBox transformedBox = {0, 0, transformedSize.x, transformedSize.y};
        transformedBox.transform(Math::wlTransformToHyprutils(Math::invertTransform(transform)), transformedSize.x, transformedSize.y);
        return Vector2D(transformedBox.width, transformedBox.height) * packDivisor(mode);
    }

    // the two-half damage fold (§3.4 item 6): frame damage is in pane-buffer coordinates; the
    // scanout buffer holds the same pane in every destination box, so fold the damage into each
    // before submitting to the output — D ∪ (D + paneW). Identity when off.
    inline CRegion foldPaneDamage(const CRegion& paneDamage, const Vector2D& pixelSize, Config::eMonitorStereoMode mode) {
        if (mode == Config::STEREO_OFF)
            return paneDamage;

        CRegion folded;
        for (int i = 0; i < paneCount(mode); ++i)
            folded.add(paneDamage.copy().translate(paneDestBox(pixelSize, mode, i).pos()));
        return folded;
    }
}
