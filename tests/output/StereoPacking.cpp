#include <output/StereoPacking.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

// WP F3 — unit tests for the stereo output pack (research/24 §3).
//
// These exercise the very functions CMonitor and IHyprRenderer call (Monitor.cpp's accessors and
// the derivation sites delegate to this header), so a regression here is a regression there.
//
// The load-bearing invariants, in order of how badly they break things:
//   1. stereo:off is the identity — every function must return exactly the stock value.
//   2. the logical size derives from ONE PANE, and m_size * scale == m_transformedSize survives.
//   3. the pack lives strictly below m_transformedSize, so damage submitted to the output is the
//      pane damage folded into every destination box.

using namespace Monitor::Stereo;
using namespace Config;

namespace {
    // every transform, so the pane/mode axis swap is checked against rotation rather than assumed
    constexpr std::array<wl_output_transform, 8> ALL_TRANSFORMS = {
        WL_OUTPUT_TRANSFORM_NORMAL,  WL_OUTPUT_TRANSFORM_90,         WL_OUTPUT_TRANSFORM_180,         WL_OUTPUT_TRANSFORM_270,
        WL_OUTPUT_TRANSFORM_FLIPPED, WL_OUTPUT_TRANSFORM_FLIPPED_90, WL_OUTPUT_TRANSFORM_FLIPPED_180, WL_OUTPUT_TRANSFORM_FLIPPED_270,
    };

    constexpr std::array<eMonitorStereoMode, 2> ALL_MODES = {STEREO_OFF, STEREO_SBS};

    // the stock (pre-stereo) derivation, verbatim from Monitor.cpp before WP F1. Test 1 above is
    // "deriveGeometry(..., STEREO_OFF) == this", which is the whole no-regression promise.
    Vector2D stockLogicalSize(const Vector2D& pixelSize, wl_output_transform transform, float scale) {
        const Vector2D XFMD = transform % 2 == 1 ? Vector2D{pixelSize.y, pixelSize.x} : pixelSize;
        return (XFMD / scale).round();
    }

    Vector2D stockTransformedSize(const Vector2D& pixelSize, wl_output_transform transform) {
        return transform % 2 == 1 ? Vector2D{pixelSize.y, pixelSize.x} : pixelSize;
    }

    // rect-set equality, since CRegion has no operator==
    std::vector<std::array<int, 4>> rectsOf(const CRegion& r) {
        std::vector<std::array<int, 4>> out;
        for (const auto& box : r.getRects())
            out.push_back({box.x1, box.y1, box.x2, box.y2});
        std::ranges::sort(out);
        return out;
    }
}

// --- the divisor, the pane count, the pane size ---

TEST(StereoPacking, divisorOffIsIdentity) {
    EXPECT_EQ(packDivisor(STEREO_OFF), Vector2D(1, 1));
    EXPECT_EQ(paneCount(STEREO_OFF), 1);
}

TEST(StereoPacking, divisorSbsIsTwoWide) {
    EXPECT_EQ(packDivisor(STEREO_SBS), Vector2D(2, 1));
    EXPECT_EQ(paneCount(STEREO_SBS), 2);
}

TEST(StereoPacking, paneSizeOffIsThePixelSize) {
    for (const auto& SIZE : {Vector2D{3840, 1080}, Vector2D{1920, 1080}, Vector2D{2560, 1440}, Vector2D{1367, 769}})
        EXPECT_EQ(paneSize(SIZE, STEREO_OFF), SIZE);
}

TEST(StereoPacking, paneSizeSbsHalvesTheWidthOnly) {
    EXPECT_EQ(paneSize({3840, 1080}, STEREO_SBS), Vector2D(1920, 1080));
    EXPECT_EQ(paneSize({2560, 1440}, STEREO_SBS), Vector2D(1280, 1440));
    EXPECT_EQ(paneSize({1920, 1080}, STEREO_SBS), Vector2D(960, 1080));
}

// --- the destination boxes (where each pane lands in the scanout buffer) ---

TEST(StereoPacking, paneDestBoxOffIsTheWholeBuffer) {
    const auto BOX = paneDestBox({3840, 1080}, STEREO_OFF, 0);
    EXPECT_EQ(BOX.pos(), Vector2D(0, 0));
    EXPECT_EQ(BOX.size(), Vector2D(3840, 1080));
}

TEST(StereoPacking, paneDestBoxSbsIsTwoHalves) {
    const auto LEFT  = paneDestBox({3840, 1080}, STEREO_SBS, 0);
    const auto RIGHT = paneDestBox({3840, 1080}, STEREO_SBS, 1);

    EXPECT_EQ(LEFT.pos(), Vector2D(0, 0));
    EXPECT_EQ(LEFT.size(), Vector2D(1920, 1080));
    EXPECT_EQ(RIGHT.pos(), Vector2D(1920, 0));
    EXPECT_EQ(RIGHT.size(), Vector2D(1920, 1080));

    // the two boxes must exactly tile the mode — no gap, no overlap, nothing left over
    EXPECT_EQ(LEFT.pos().x + LEFT.size().x, RIGHT.pos().x);
    EXPECT_EQ(RIGHT.pos().x + RIGHT.size().x, 3840);
}

// --- the sanitize predicate (a mode that does not divide must drop the packing) ---

TEST(StereoPacking, modeDividesOffAlwaysTrue) {
    EXPECT_TRUE(modeDivides({3841, 1080}, STEREO_OFF)); // an odd mode is fine when nothing is packed
    EXPECT_TRUE(modeDivides({1, 1}, STEREO_OFF));
}

TEST(StereoPacking, modeDividesSbsRejectsOddWidth) {
    EXPECT_TRUE(modeDivides({3840, 1080}, STEREO_SBS));
    EXPECT_FALSE(modeDivides({3841, 1080}, STEREO_SBS)); // 1920.5 px panes
    EXPECT_FALSE(modeDivides({1, 1080}, STEREO_SBS));    // 0.5 px panes
    EXPECT_FALSE(modeDivides({0, 0}, STEREO_SBS));       // degenerate mode, never a stereo output
}

// --- the second sanitize predicate: the pack must be on the mode it was configured for ---
//
// (§3.4 item 15.) Divisibility is not enough: a 3840x1080 SBS pack on a display that fell back to
// 2560x1440 divides perfectly and is still one eye's half stretched across the panel. Both the
// applyMonitorRule sanitizer and the wlr-output-management write-back guard ask this predicate,
// which is what keeps them from disagreeing. The mode REQUEST forms it has to survive are the ones
// Parser.cpp produces — Vector2D() for `preferred`, (-1,-1)/(-1,-2)/(-1,-3) for
// highrr/highres/maxwidth — none of which is a resolution (see tests/config/MonitorParser.cpp,
// which drives the same predicate with values from the real parser).

TEST(StereoPacking, modeIsAsRequestedExplicitResolutionIsAContract) {
    EXPECT_TRUE(modeIsAsRequested({3840, 1080}, {3840, 1080}));
    EXPECT_FALSE(modeIsAsRequested({2560, 1440}, {3840, 1080})); // the fallback landed elsewhere
    EXPECT_FALSE(modeIsAsRequested({1920, 1080}, {3840, 1080})); // ... including on the pane itself
    // an explicit request is checked against the mode even when the search reported no fallback:
    // the custom-mode retry and the requestedModes loop can both land off-request quietly
    EXPECT_FALSE(modeIsAsRequested({2560, 1440}, {3840, 1080}, false));
}

TEST(StereoPacking, modeIsAsRequestedPreferredHasNoModeToCompare) {
    // `preferred` asks for whatever the display prefers, so any committed mode is as requested...
    EXPECT_TRUE(modeIsAsRequested({3840, 1080}, Vector2D()));
    EXPECT_TRUE(modeIsAsRequested({2560, 1440}, Vector2D()));
    // ... unless the search gave up on the request entirely and took any mode it could commit
    EXPECT_FALSE(modeIsAsRequested({2560, 1440}, Vector2D(), true));
}

TEST(StereoPacking, modeIsAsRequestedSentinelsAreNotResolutions) {
    // highrr / highres / maxwidth. Comparing a real mode against these would be false forever,
    // which would drop the pack on every stereo output configured with one of them.
    for (const auto& SENTINEL : {Vector2D(-1, -1), Vector2D(-1, -2), Vector2D(-1, -3)}) {
        EXPECT_TRUE(modeIsAsRequested({3840, 1080}, SENTINEL)) << SENTINEL.x << "," << SENTINEL.y;
        EXPECT_FALSE(modeIsAsRequested({3840, 1080}, SENTINEL, true)) << SENTINEL.x << "," << SENTINEL.y;
    }
}

TEST(StereoPacking, modeIsAsRequestedGuardsAModeWriteBack) {
    // the wlr-output-management shape: a GUI writes a mode over a monitor that is already packed.
    const Vector2D COMMITTED = {3840, 1080};
    EXPECT_TRUE(modeIsAsRequested(COMMITTED, COMMITTED));     // the GUI kept the mode
    EXPECT_FALSE(modeIsAsRequested({1920, 1080}, COMMITTED)); // it picked the pane's size
    EXPECT_FALSE(modeIsAsRequested({2560, 1440}, COMMITTED)); // or anything else
    EXPECT_TRUE(modeIsAsRequested({2560, 1440}, Vector2D())); // nothing committed yet, no opinion
}

// --- the mode-list watch: the pack must never OUTLIVE the mode it was validated on ---
//
// (§3.4 item 15b, task #142.) modeIsAsRequested above is an apply-time predicate, and it is right,
// but it only ever runs when a rule is applied. Live on 2026-08-09 the XREAL fell from its 3D
// personality (3840x1080-only EDID) to its 2D one (1920x1080-only) on a USB re-enumeration under a
// connector that kept its name, with the rule unchanged. Nothing re-applied it — aquamarine's
// IOutput has no modes-changed signal, m_output->modes is read nowhere but applyMonitorRule, and
// ensureMonitorStatus skips a monitor whose rule did not change — so `hyprctl monitors` reported a
// live availableModes of 1920x1080 next to a frozen `stereo: sbs, scanoutWidth: 3840`, and the
// compositor kept packing two panes into a scanout the panel had stopped splitting.
//
// The invariant these pin: the pack is never live while the panel does not advertise the packed
// mode. Everything else the watch does (the re-adopt) is a convenience on top of that.

namespace {
    const std::vector<Vector2D> THREE_D_PERSONALITY = {{3840, 1080}}; // XREAL after `xreal-ctl mode 3d`
    const std::vector<Vector2D> TWO_D_PERSONALITY   = {{1920, 1080}}; // ... and after it falls back
}

TEST(StereoPacking, watchLeavesAHealthyPackAlone) {
    EXPECT_EQ(watchAction(STEREO_SBS, STEREO_SBS, {3840, 1080}, {3840, 1080}, THREE_D_PERSONALITY), STEREO_WATCH_NOTHING);
}

TEST(StereoPacking, watchDropsAPackWhoseModeVanished) {
    // the live #142 state: packed at 3840, panel now offers 1920 only
    EXPECT_EQ(watchAction(STEREO_SBS, STEREO_SBS, {3840, 1080}, {3840, 1080}, TWO_D_PERSONALITY), STEREO_WATCH_DROP);
    // and with nothing advertised at all (a connector mid-re-enumeration) — still not a mode we can
    // pack, so still a drop rather than a guess
    EXPECT_EQ(watchAction(STEREO_SBS, STEREO_SBS, {3840, 1080}, {3840, 1080}, {}), STEREO_WATCH_DROP);
}

TEST(StereoPacking, watchDropsRegardlessOfHowTheModeWasRequested) {
    // the drop is a safety action: it is about the PANEL, not about the request form, so a pack
    // configured with `preferred` or a sentinel is guarded exactly as tightly as an explicit mode.
    for (const auto& REQUEST : {Vector2D(), Vector2D(-1, -1), Vector2D(-1, -2), Vector2D(-1, -3), Vector2D(3840, 1080)}) {
        EXPECT_EQ(watchAction(STEREO_SBS, STEREO_SBS, {3840, 1080}, REQUEST, TWO_D_PERSONALITY), STEREO_WATCH_DROP) << REQUEST.x << "," << REQUEST.y;
    }
}

TEST(StereoPacking, watchNeverDropsACustomModeline) {
    // a user modeline is legitimately absent from the advertised list; reading that as a fall would
    // drop the pack on every `monitor = ..., modeline ...` stereo output, forever.
    EXPECT_EQ(watchAction(STEREO_SBS, STEREO_SBS, {3840, 1080}, {3840, 1080}, TWO_D_PERSONALITY, /* onCustomMode */ true), STEREO_WATCH_NOTHING);
}

TEST(StereoPacking, watchIsInertOnAMonitorThatIsNotStereo) {
    // stereo:off must cost nothing and decide nothing — the whole feature's no-regression promise
    for (const auto& ADVERTISED : {THREE_D_PERSONALITY, TWO_D_PERSONALITY, std::vector<Vector2D>{}}) {
        EXPECT_EQ(watchAction(STEREO_OFF, STEREO_OFF, {1920, 1080}, {1920, 1080}, ADVERTISED), STEREO_WATCH_NOTHING);
        EXPECT_EQ(watchAction(STEREO_OFF, STEREO_OFF, {1920, 1080}, Vector2D(), ADVERTISED), STEREO_WATCH_NOTHING);
    }
}

TEST(StereoPacking, watchReAdoptsWhenTheConfiguredModeComesBack) {
    // stereo-by-default: the rule stays in the config, so when the glasses return to their 3D
    // personality the desktop must follow them back without the user reloading anything.
    EXPECT_EQ(watchAction(STEREO_OFF, STEREO_SBS, {1920, 1080}, {3840, 1080}, THREE_D_PERSONALITY), STEREO_WATCH_READOPT);
    // ... and not while they are still down
    EXPECT_EQ(watchAction(STEREO_OFF, STEREO_SBS, {1920, 1080}, {3840, 1080}, TWO_D_PERSONALITY), STEREO_WATCH_NOTHING);
    // ... and not when we are already committed on it (that is a pack that sanitize dropped for a
    // reason of its own — divisibility — and a timer must not fight it)
    EXPECT_EQ(watchAction(STEREO_OFF, STEREO_SBS, {3840, 1080}, {3840, 1080}, THREE_D_PERSONALITY), STEREO_WATCH_NOTHING);
}

TEST(StereoPacking, watchDoesNotChaseANonResolutionRequest) {
    // `preferred`/`highrr`/`highres`/`maxwidth` mean "whatever the mode search picks". Re-adopting
    // re-modesets the panel, and a timer has no business deciding that on a request with no mode in
    // it — those recover on the next reload, as the docs say.
    for (const auto& REQUEST : {Vector2D(), Vector2D(-1, -1), Vector2D(-1, -2), Vector2D(-1, -3)}) {
        EXPECT_EQ(watchAction(STEREO_OFF, STEREO_SBS, {1920, 1080}, REQUEST, THREE_D_PERSONALITY), STEREO_WATCH_NOTHING) << REQUEST.x << "," << REQUEST.y;
    }
}

TEST(StereoPacking, watchReplaysTheLivePersonalityFall) {
    // the whole #142 sequence, as the watch sees it one tick at a time.
    const Vector2D RULE_MODE = {3840, 1080};

    // 1. healthy: rule applied, panel in 3D personality, pack live on the mode it was validated on
    auto advertised = THREE_D_PERSONALITY;
    auto pack       = STEREO_SBS;
    auto committed  = RULE_MODE;
    EXPECT_EQ(watchAction(pack, STEREO_SBS, committed, RULE_MODE, advertised), STEREO_WATCH_NOTHING);

    // 2. the fall. Same connector, same rule, new mode list — and this is the tick that used to not
    //    exist, which is the whole bug.
    advertised = TWO_D_PERSONALITY;
    ASSERT_EQ(watchAction(pack, STEREO_SBS, committed, RULE_MODE, advertised), STEREO_WATCH_DROP);

    // 3. applyMonitorRule re-runs: the search lands on 1920 and sanitizeStereoMode drops the pack
    //    because the committed mode is not the requested one. THE invariant — the pack is not live
    //    while the committed width differs from the stereo-configured width.
    committed = {1920, 1080};
    pack      = modeIsAsRequested(committed, RULE_MODE) ? STEREO_SBS : STEREO_OFF;
    ASSERT_EQ(pack, STEREO_OFF);
    EXPECT_NE(committed.x, RULE_MODE.x);

    // 4. settled: no further re-modeset while the glasses stay down (no timer storm)
    EXPECT_EQ(watchAction(pack, STEREO_SBS, committed, RULE_MODE, advertised), STEREO_WATCH_NOTHING);

    // 5. the glasses come back
    advertised = THREE_D_PERSONALITY;
    ASSERT_EQ(watchAction(pack, STEREO_SBS, committed, RULE_MODE, advertised), STEREO_WATCH_READOPT);

    // 6. re-applied, sanitize passes, the pack is back and the watch goes quiet again
    committed = RULE_MODE;
    pack      = modeIsAsRequested(committed, RULE_MODE) ? STEREO_SBS : STEREO_OFF;
    ASSERT_EQ(pack, STEREO_SBS);
    EXPECT_EQ(watchAction(pack, STEREO_SBS, committed, RULE_MODE, advertised), STEREO_WATCH_NOTHING);
}

// --- the derivation: stereo:off must be bit-identical to the stock expression ---

TEST(StereoPacking, deriveGeometryOffIsBitIdenticalToStock) {
    for (const auto TRANSFORM : ALL_TRANSFORMS) {
        for (const float SCALE : {1.F, 1.5F, 2.F, 1.25F}) {
            for (const auto& MODE : {Vector2D{3840, 1080}, Vector2D{1920, 1080}, Vector2D{2560, 1440}}) {
                const auto GEOM = deriveGeometry(MODE, STEREO_OFF, TRANSFORM, SCALE);
                EXPECT_EQ(GEOM.logicalSize, stockLogicalSize(MODE, TRANSFORM, SCALE)) << "transform " << TRANSFORM << " scale " << SCALE;
                EXPECT_EQ(GEOM.transformedSize, stockTransformedSize(MODE, TRANSFORM)) << "transform " << TRANSFORM << " scale " << SCALE;
            }
        }
    }
}

// --- the derivation: stereo derives from the PANE, across every transform ---

TEST(StereoPacking, deriveGeometrySbsDerivesFromThePane) {
    // 3840x1080 sbs at scale 1: one logical 1920x1080 monitor, whatever the rotation
    for (const auto TRANSFORM : ALL_TRANSFORMS) {
        const auto     GEOM     = deriveGeometry({3840, 1080}, STEREO_SBS, TRANSFORM, 1.F);
        const bool     ROTATED  = TRANSFORM % 2 == 1;
        const Vector2D EXPECTED = ROTATED ? Vector2D{1080, 1920} : Vector2D{1920, 1080};

        EXPECT_EQ(GEOM.transformedSize, EXPECTED) << "transform " << TRANSFORM;
        EXPECT_EQ(GEOM.logicalSize, EXPECTED) << "transform " << TRANSFORM;

        // and it is never the mode — the phantom-3840-desktop bug this whole framing exists to avoid
        EXPECT_NE(GEOM.transformedSize, ROTATED ? Vector2D(1080, 3840) : Vector2D(3840, 1080)) << "transform " << TRANSFORM;
    }
}

TEST(StereoPacking, deriveGeometrySbsAcrossScales) {
    // scale divides the PANE (§3.4 item 11): 1920/scale, never 3840/scale
    EXPECT_EQ(deriveGeometry({3840, 1080}, STEREO_SBS, WL_OUTPUT_TRANSFORM_NORMAL, 1.5F).logicalSize, Vector2D(1280, 720));
    EXPECT_EQ(deriveGeometry({3840, 1080}, STEREO_SBS, WL_OUTPUT_TRANSFORM_NORMAL, 2.F).logicalSize, Vector2D(960, 540));
    EXPECT_EQ(deriveGeometry({3840, 1080}, STEREO_SBS, WL_OUTPUT_TRANSFORM_NORMAL, 1.F).logicalSize, Vector2D(1920, 1080));

    // the transformedSize is scale-independent in every case (it is the buffer, not the desktop)
    for (const float SCALE : {1.F, 1.5F, 2.F})
        EXPECT_EQ(deriveGeometry({3840, 1080}, STEREO_SBS, WL_OUTPUT_TRANSFORM_NORMAL, SCALE).transformedSize, Vector2D(1920, 1080));
}

// The invariant the whole "put the pack below m_transformedSize" framing buys: logical × scale is
// the buffer again. Under the rejected framing (logical shrinks, transformedSize stays the mode)
// this fails for stereo, and with it the damage ring, the layer clip box and the cursor box.
TEST(StereoPacking, logicalTimesScaleIsTheTransformedSize) {
    for (const auto MODE : ALL_MODES) {
        for (const auto TRANSFORM : ALL_TRANSFORMS) {
            for (const float SCALE : {1.F, 1.5F, 2.F}) {
                const auto GEOM = deriveGeometry({3840, 1080}, MODE, TRANSFORM, SCALE);
                EXPECT_EQ(GEOM.logicalSize * SCALE, GEOM.transformedSize) << "mode " << stereoModeToString(MODE) << " transform " << TRANSFORM << " scale " << SCALE;
            }
        }
    }
}

// --- the m_createdByUser back-computation is the exact inverse ---

TEST(StereoPacking, backComputeModeRoundTrips) {
    for (const auto MODE : ALL_MODES) {
        for (const auto TRANSFORM : ALL_TRANSFORMS) {
            const Vector2D PIXELSIZE = {3840, 1080};
            const auto     GEOM      = deriveGeometry(PIXELSIZE, MODE, TRANSFORM, 1.F);
            EXPECT_EQ(backComputeMode(GEOM.transformedSize, TRANSFORM, MODE), PIXELSIZE) << "mode " << stereoModeToString(MODE) << " transform " << TRANSFORM;
        }
    }
}

// --- the fractional-scale validation base (§3.4 item 11) ---

TEST(StereoPacking, scaleValidationOffIsTheModeOverScale) {
    EXPECT_EQ(scaleValidationSize({3840, 1080}, STEREO_OFF, 2.F), Vector2D(1920, 540));
    EXPECT_EQ(scaleValidationSize({3840, 1080}, STEREO_OFF, 1.F), Vector2D(3840, 1080));
}

// The regression this guards: validating the MODE instead of the PANE accepts a scale that
// produces half-pixel logical sizes. 2550/2 = 1275 (integral, accepted) but the pane is
// 1275/2 = 637.5 — fractional, and the desktop would be laid out on half a pixel.
TEST(StereoPacking, scaleValidationUsesThePaneNotTheMode) {
    const Vector2D MODE = {2550, 1080};

    const Vector2D PANEBASED = scaleValidationSize(MODE, STEREO_SBS, 2.F);
    EXPECT_EQ(PANEBASED, Vector2D(637.5, 540.0));
    EXPECT_NE(PANEBASED.x, std::round(PANEBASED.x)); // correctly rejected by the scale check

    const Vector2D MODEBASED = MODE / 2.F;           // what the pre-F1 expression computed
    EXPECT_EQ(MODEBASED.x, std::round(MODEBASED.x)); // would have been wrongly accepted
    EXPECT_NE(PANEBASED, MODEBASED);
}

TEST(StereoPacking, scaleValidationAcceptsTheCleanStereoCase) {
    const Vector2D LOGICAL = scaleValidationSize({3840, 1080}, STEREO_SBS, 1.5F);
    EXPECT_EQ(LOGICAL, Vector2D(1280, 720));
    EXPECT_EQ(LOGICAL.x, std::round(LOGICAL.x));
    EXPECT_EQ(LOGICAL.y, std::round(LOGICAL.y));
}

// --- the two-half damage fold ---

TEST(StereoPacking, foldPaneDamageOffIsIdentity) {
    CRegion damage;
    damage.add(CBox{10, 20, 100, 50});
    damage.add(CBox{500, 600, 30, 30});

    EXPECT_EQ(rectsOf(foldPaneDamage(damage, {3840, 1080}, STEREO_OFF)), rectsOf(damage));
}

TEST(StereoPacking, foldPaneDamageDuplicatesIntoBothHalves) {
    CRegion damage;
    damage.add(CBox{10, 20, 100, 50});

    const auto FOLDED = foldPaneDamage(damage, {3840, 1080}, STEREO_SBS);
    const auto RECTS  = rectsOf(FOLDED);

    ASSERT_EQ(RECTS.size(), 2u);
    EXPECT_EQ(RECTS[0], (std::array<int, 4>{10, 20, 110, 70}));       // left pane
    EXPECT_EQ(RECTS[1], (std::array<int, 4>{1930, 20, 2030, 70}));    // right pane, + paneW
    EXPECT_EQ(FOLDED.copy().getExtents().size(), Vector2D(2020, 50)); // spans both halves
}

// The property the pack actually needs: a full-pane repaint must submit the FULL scanout buffer as
// damage. If the fold were missing, the right eye would keep the previous frame forever.
TEST(StereoPacking, foldPaneDamageFullPaneCoversTheWholeScanout) {
    CRegion damage;
    damage.add(CBox{0, 0, 1920, 1080});

    const auto FOLDED = foldPaneDamage(damage, {3840, 1080}, STEREO_SBS);
    const auto BOX    = FOLDED.copy().getExtents();

    EXPECT_EQ(BOX.pos(), Vector2D(0, 0));
    EXPECT_EQ(BOX.size(), Vector2D(3840, 1080));
    EXPECT_EQ(rectsOf(FOLDED).size(), 1u); // the two halves merge into one rect covering the mode
}

TEST(StereoPacking, foldPaneDamageEmptyStaysEmpty) {
    const CRegion EMPTY;
    EXPECT_TRUE(foldPaneDamage(EMPTY, {3840, 1080}, STEREO_SBS).empty());
}

// A pane-space rect always lands inside the scanout buffer after the fold — a fold that overflowed
// the mode would be submitting damage aquamarine has no buffer for.
TEST(StereoPacking, foldPaneDamageStaysInsideTheMode) {
    const Vector2D MODE = {3840, 1080};

    CRegion        damage;
    damage.add(CBox{1900, 1000, 20, 80}); // the bottom-right corner of the pane

    const auto BOX = foldPaneDamage(damage, MODE, STEREO_SBS).copy().getExtents();
    EXPECT_GE(BOX.pos().x, 0);
    EXPECT_GE(BOX.pos().y, 0);
    EXPECT_LE(BOX.pos().x + BOX.size().x, MODE.x);
    EXPECT_LE(BOX.pos().y + BOX.size().y, MODE.y);
}

// The one invariant that spans the two independent implementations of the pack: CHyprOpenGLImpl's
// blit loop writes pane i at paneDestBox(i) (as a viewport, with the same origin installed as the
// glScissor offset), and CHyprRenderer submits foldPaneDamage() to the output. If those two ever
// disagreed the compositor would tell the display that pixels changed somewhere it did not draw —
// tearing on one eye, or a stale eye, depending on which way the disagreement went. Neither side
// is observable from a client (every capture protocol is sized at the pane by design), so this is
// the boundary where they are checked against each other.
TEST(StereoPacking, foldedDamageLandsExactlyOnThePaneDestinations) {
    const Vector2D MODE = {3840, 1080};
    const CBox     RECT = {40, 60, 100, 50}; // one pane-space rect, small enough not to merge

    CRegion        damage;
    damage.add(RECT);

    std::vector<std::array<int, 4>> expected;
    for (int i = 0; i < paneCount(STEREO_SBS); ++i) {
        // the blit loop's destination for pane i — also the scissor offset it installs
        const auto ORIGIN = paneDestBox(MODE, STEREO_SBS, i).pos();
        expected.push_back({sc<int>(RECT.x + ORIGIN.x), sc<int>(RECT.y + ORIGIN.y), sc<int>(RECT.x + ORIGIN.x + RECT.width), sc<int>(RECT.y + ORIGIN.y + RECT.height)});
    }
    std::ranges::sort(expected);

    EXPECT_EQ(rectsOf(foldPaneDamage(damage, MODE, STEREO_SBS)), expected);
}
