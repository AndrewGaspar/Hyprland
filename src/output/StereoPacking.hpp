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

    // sanitize predicate: the committed mode must divide cleanly into >= 1x1 panes, or the
    // packing is dropped loudly rather than deriving fractional pane sizes (§3.4 item 1).
    inline bool modeDivides(const Vector2D& pixelSize, Config::eMonitorStereoMode mode) {
        const Vector2D PANE = paneSize(pixelSize, mode);
        return PANE == PANE.floor() && PANE.x >= 1 && PANE.y >= 1;
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
