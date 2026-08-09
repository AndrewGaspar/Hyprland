#include <desktop/DepthTiers.hpp>
#include <desktop/rule/windowRule/WindowRule.hpp>
#include <desktop/rule/windowRule/WindowRuleEffectContainer.hpp>
#include <desktop/rule/layerRule/LayerRule.hpp>
#include <desktop/rule/layerRule/LayerRuleEffectContainer.hpp>
#include <output/StereoPacking.hpp>

#include <hyprutils/math/Region.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <optional>
#include <variant>

// WP D1 — depth as a decoration axis (research/24 §7).
//
// Two things are under test and they are deliberately the two things D1 actually ships:
//
//   1. The LADDER. CWindow::updateDepth() and CLayerSurface::updateDepth() are two lines of glue
//      over Desktop::Depth::{windowTier,layerTier,resolve}, so testing the header tests them. The
//      invariants that matter are §8.2's: depth is one-sided (nothing goes behind the plane), the
//      rungs are few and close together, and an out-of-range rule is clamped rather than rejected.
//   2. The RULE PARSE. `windowrule = depth <z>` / `layerrule = depth <z>` go through the shared
//      rule engine — the same store both config front-ends write into (F8) — so parsing here is
//      parsing under Lua too.
//
// WP D2 added a third: the §8.1 DISPARITY math the producer runs — the worked table, the eye sign,
// §6.1's frame-violation clamp, and the two facts the rest of the producer is built on: that zero
// depth means an exactly-zero shift in every pane (the fast path), and that integer rounding would
// collapse the ladder (the sub-pixel seam in ElementRenderer).
//
// WP D4 finished the arithmetic with the three properties the RENDERER relies on but cannot state
// itself: the depth × eye-sign matrix over the whole ladder, that a damage box grown by the spread
// provably contains every pane's clamped draw (§6.3 pt 2 — the one place a wrong sign silently
// leaves half a window stale), and that with nothing raised the depth path hands StereoPacking
// exactly what WP F1 handed it. The pixel-level counterpart — a window that actually MOVES on a
// real headless stereo output — is hyprtester's stereoDepthDisparityMovesTheWindow.

using namespace Desktop;
using namespace Desktop::Rule;

namespace {
    // the ladder exactly as ConfigValues.cpp ships it (research/24 §7.2)
    constexpr Depth::STiers SHIPPED = {.focused = 0.6F, .unfocused = 0.2F, .layers = 0.8F};

    std::optional<float>    windowRuleDepth(const std::string& raw) {
        CWindowRule rule("test");
        if (!rule.addEffect(WINDOW_RULE_EFFECT_DEPTH, raw))
            return std::nullopt;

        const auto& EFFECTS = rule.effects();
        if (EFFECTS.size() != 1 || !std::holds_alternative<float>(EFFECTS[0].value))
            return std::nullopt;

        return std::get<float>(EFFECTS[0].value);
    }

    std::optional<float> layerRuleDepth(const std::string& raw) {
        CLayerRule rule("test");
        if (!rule.addEffect(LAYER_RULE_EFFECT_DEPTH, raw))
            return std::nullopt;

        const auto& EFFECTS = rule.effects();
        if (EFFECTS.size() != 1 || !std::holds_alternative<float>(EFFECTS[0].value))
            return std::nullopt;

        return std::get<float>(EFFECTS[0].value);
    }
}

// ---------------------------------------------------------------- clamp

TEST(DepthTiers, clampIsOneSided) {
    // §8.2 point 2: crossed (toward-viewer) disparity is the comfortable direction, so there is no
    // negative half of the range at all — a user asking to sink a window gets the wallpaper plane.
    EXPECT_FLOAT_EQ(Depth::clamp(-1.F), 0.F);
    EXPECT_FLOAT_EQ(Depth::clamp(-0.001F), 0.F);
    EXPECT_FLOAT_EQ(Depth::clamp(0.F), 0.F);
    EXPECT_FLOAT_EQ(Depth::clamp(0.42F), 0.42F);
    EXPECT_FLOAT_EQ(Depth::clamp(1.F), 1.F);
    EXPECT_FLOAT_EQ(Depth::clamp(37.F), 1.F);
}

// ---------------------------------------------------------------- window tier

TEST(DepthTiers, windowTierRisesOnFocus) {
    // "the rise IS the focus indicator" (§7.3)
    EXPECT_FLOAT_EQ(Depth::windowTier(SHIPPED, true, false), 0.6F);
    EXPECT_FLOAT_EQ(Depth::windowTier(SHIPPED, false, false), 0.2F);
    EXPECT_GT(Depth::windowTier(SHIPPED, true, false), Depth::windowTier(SHIPPED, false, false));
}

TEST(DepthTiers, windowTierFullscreenIsThePlane) {
    // a fullscreen window IS the plane — raising it would move the whole panel, not a card on it
    EXPECT_FLOAT_EQ(Depth::windowTier(SHIPPED, true, true), 0.F);
    EXPECT_FLOAT_EQ(Depth::windowTier(SHIPPED, false, true), 0.F);
}

TEST(DepthTiers, windowTierClampsAbsurdConfig) {
    // a hand-edited config cannot push a window past the ceiling
    constexpr Depth::STiers WILD = {.focused = 9.F, .unfocused = -3.F, .layers = 0.8F};
    EXPECT_FLOAT_EQ(Depth::windowTier(WILD, true, false), 1.F);
    EXPECT_FLOAT_EQ(Depth::windowTier(WILD, false, false), 0.F);
}

// ---------------------------------------------------------------- layer tier

TEST(DepthTiers, layerTierPinsTheWallpaper) {
    // background/bottom stay on the page: "anything else destroys the sense of a page" (§7.3)
    EXPECT_FLOAT_EQ(Depth::layerTier(SHIPPED, Depth::LAYER_BACKGROUND), 0.F);
    EXPECT_FLOAT_EQ(Depth::layerTier(SHIPPED, Depth::LAYER_BOTTOM), 0.F);
}

TEST(DepthTiers, layerTierRaisesBarsAndOverlays) {
    EXPECT_FLOAT_EQ(Depth::layerTier(SHIPPED, Depth::LAYER_TOP), 0.8F);
    EXPECT_FLOAT_EQ(Depth::layerTier(SHIPPED, Depth::LAYER_OVERLAY), 0.8F);
}

// ---------------------------------------------------------------- resolve (rule vs tier)

TEST(DepthTiers, resolvePrefersTheRule) {
    // §7.2: rules are for tuning, so an explicit rule beats every tier — including the two places
    // the tier would otherwise pin 0 (a fullscreen window, a wallpaper layer). Without this, a user
    // could not raise their fullscreen video player or float their wallpaper at all.
    EXPECT_FLOAT_EQ(Depth::resolve(std::nullopt, Depth::windowTier(SHIPPED, true, false)), 0.6F);
    EXPECT_FLOAT_EQ(Depth::resolve(0.35F, Depth::windowTier(SHIPPED, true, false)), 0.35F);
    EXPECT_FLOAT_EQ(Depth::resolve(0.35F, Depth::windowTier(SHIPPED, false, true)), 0.35F);
    EXPECT_FLOAT_EQ(Depth::resolve(0.35F, Depth::layerTier(SHIPPED, Depth::LAYER_BACKGROUND)), 0.35F);
}

TEST(DepthTiers, resolveClampsTheRule) {
    // the clamp is applied at parse AND at resolve, because a plugin can set the prop directly
    EXPECT_FLOAT_EQ(Depth::resolve(4.F, 0.2F), 1.F);
    EXPECT_FLOAT_EQ(Depth::resolve(-4.F, 0.2F), 0.F);
}

// ---------------------------------------------------------------- the ladder as shipped

TEST(DepthTiers, shippedLadderIsOrderedAndInRange) {
    // wallpaper < ordinary window < focused window < bar. The ORDER is the semantics: if focused
    // ever sank below unfocused, focusing a window would push it away from you.
    EXPECT_LT(0.F, SHIPPED.unfocused);
    EXPECT_LT(SHIPPED.unfocused, SHIPPED.focused);
    EXPECT_LT(SHIPPED.focused, SHIPPED.layers);
    EXPECT_LE(SHIPPED.layers, Depth::MAX);
}

TEST(DepthTiers, shippedLadderStepsStayInsideTheFovealBudget) {
    // §8.2's binding limit is foveal fusion (~0.1° ≈ 3 px total at our defaults), and it applies to
    // the STEP between two things visible at once, not the absolute offset. The shipped ladder's
    // largest step is 0.4 (unfocused→focused) — at depth_scale 0.12 m that is 4.8 cm of rise, which
    // §8.1's table puts at ~5′, i.e. inside the 1° budget with room to spare. This test exists so
    // that a future re-tune of the ladder has to argue with the number rather than drift past it.
    EXPECT_FLOAT_EQ(Depth::largestLadderStep(SHIPPED), 0.4F);

    // and the budget itself, stated as something a re-tune cannot quietly slip past: no single rung
    // may be more than half the whole span, or the "ladder, not a range" property is gone.
    EXPECT_LE(Depth::largestLadderStep(SHIPPED), Depth::MAX / 2.F);
}

TEST(DepthTiers, riseMetresIsTheComfortKnob) {
    // §7.2: depth_scale 0.12 == 12 cm of rise at depth 1.0, the row §8.1's table calls 12.5′
    EXPECT_FLOAT_EQ(Depth::riseMetres(1.F, 0.12F), 0.12F);
    EXPECT_FLOAT_EQ(Depth::riseMetres(0.5F, 0.12F), 0.06F);
    EXPECT_FLOAT_EQ(Depth::riseMetres(0.F, 0.12F), 0.F);
    // depth is clamped before it is scaled, so a rogue value cannot escape the comfort ceiling
    EXPECT_FLOAT_EQ(Depth::riseMetres(10.F, 0.12F), 0.12F);
}

// ------------------------------------------------------- §8.1 disparity (WP D2, the producer)

// The report's worked table, reproduced from the shipped expressions. If these four rows ever move
// it is because someone changed the formula or the defaults, and either is a comfort decision that
// should be made deliberately rather than discovered on a headset.
TEST(DepthTiers, disparityReproducesTheWorkedTable) {
    constexpr Depth::SGeometry GEO   = {}; // 1.5 m away, 1.6 m wide, b = 0.063 → 1200 px/m at 1920
    constexpr float            PANE  = 1920.F;
    constexpr float            TOLPX = 0.06F; // the table is quoted to one decimal place (9.45 → "9.5")

    // rise → per-pane pixels, from §8.1's table
    EXPECT_NEAR(Depth::shiftMagnitudePx(0.02F, GEO, PANE), 0.5F, TOLPX);
    EXPECT_NEAR(Depth::shiftMagnitudePx(0.05F, GEO, PANE), 1.3F, TOLPX);
    EXPECT_NEAR(Depth::shiftMagnitudePx(0.12F, GEO, PANE), 3.3F, TOLPX);
    EXPECT_NEAR(Depth::shiftMagnitudePx(0.30F, GEO, PANE), 9.5F, TOLPX);
}

TEST(DepthTiers, parallaxIsNegativeInFrontOfTheScreen) {
    // §8.1's sign convention, and §8.2 point 2's design rule in one assertion: a raised element is
    // always CROSSED (in front of the plane), and depth 0 is exactly the plane, not nearly it.
    constexpr Depth::SGeometry GEO = {};
    EXPECT_LT(Depth::parallaxMetres(0.12F, GEO), 0.F);
    EXPECT_FLOAT_EQ(Depth::parallaxMetres(0.F, GEO), 0.F);
}

TEST(DepthTiers, eyeSignPutsPaneZeroOnTheLeft) {
    // §8.1: "left pane +, right pane −". StereoPacking::paneDestBox is row-major, so pane 0 is the
    // left half of an sbs mode and therefore the left eye.
    EXPECT_FLOAT_EQ(Depth::eyeSign(0), 1.F);
    EXPECT_FLOAT_EQ(Depth::eyeSign(1), -1.F);

    constexpr Depth::SGeometry GEO = {};
    EXPECT_GT(Depth::paneShiftPx(0.6F, 0.12F, GEO, 1920.F, 0), 0.F);
    EXPECT_FLOAT_EQ(Depth::paneShiftPx(0.6F, 0.12F, GEO, 1920.F, 1), -Depth::paneShiftPx(0.6F, 0.12F, GEO, 1920.F, 0));
}

// THE FAST PATH, as arithmetic (§6.4.1). The renderer's predicate asks "would anything actually
// move"; if the answer is no it builds ONE composite and the pack duplicates it, which is the
// frame WP F1 shipped. This is the assertion the whole perf story rests on: with nothing raised,
// every pane's shift is not "small", it is exactly zero, so the two composites cannot differ.
TEST(DepthTiers, zeroDepthMovesNothingInAnyPane) {
    constexpr Depth::SGeometry GEO = {};

    for (int pane = 0; pane < 2; ++pane) {
        EXPECT_FLOAT_EQ(Depth::paneShiftPx(0.F, 0.12F, GEO, 1920.F, pane), 0.F);
        // ...and it stays exactly zero however absurd the comfort knob gets, because it is the
        // DEPTH that is zero
        EXPECT_FLOAT_EQ(Depth::paneShiftPx(0.F, 1.F, GEO, 1920.F, pane), 0.F);
    }

    // the other half of the toggle: a zeroed comfort knob flattens a fully raised desktop
    EXPECT_FLOAT_EQ(Depth::damageSpreadPx(1.F, 0.F, GEO, 1920.F), 0.F);
}

// The depth × eye-sign matrix, over the ladder the compositor actually walks. Three properties in
// one table because they are one property in the renderer: the panes disagree (or there is no
// stereo), they disagree symmetrically (or the pair has a net horizontal offset, i.e. the whole
// desktop drifts sideways instead of gaining depth), and a higher rung disagrees MORE (or the
// ladder is not a ladder).
TEST(DepthTiers, disparityIsAntisymmetricAndMonotonicAcrossTheLadder) {
    constexpr Depth::SGeometry GEO   = {};
    constexpr float            SCALE = 0.12F, PANE = 1920.F;

    // 0.0 is the wallpaper, then the three shipped rungs in order
    const float LADDER[] = {0.F, SHIPPED.unfocused, SHIPPED.focused, SHIPPED.layers, Depth::MAX};

    float       previous = -1.F;
    for (const float DEPTH : LADDER) {
        const float LEFT  = Depth::paneShiftPx(DEPTH, SCALE, GEO, PANE, 0);
        const float RIGHT = Depth::paneShiftPx(DEPTH, SCALE, GEO, PANE, 1);

        // antisymmetric: the pair is centred on the screen plane, so the two panes move by the same
        // amount in opposite directions and their MEAN is exactly zero at every rung
        EXPECT_FLOAT_EQ(LEFT + RIGHT, 0.F) << "at depth " << DEPTH;
        EXPECT_FLOAT_EQ(LEFT, -RIGHT) << "at depth " << DEPTH;
        // ...and the left eye is the positive one (§8.1's convention, pane 0 = left half of an sbs)
        EXPECT_GE(LEFT, 0.F) << "at depth " << DEPTH;

        // strictly monotonic: every rung is a visibly different place, which is what makes the
        // ladder readable at all
        EXPECT_GT(LEFT, previous) << "at depth " << DEPTH;
        previous = LEFT;
    }
}

// §8.2's binding budget (4), restated in the only unit that can be checked against a screen: the
// foveal fusion limit is ~0.1° ≈ 3 px TOTAL disparity at our defaults, and it governs the STEP
// between two things visible at once — a raised window against the wallpaper, or the bar against
// the window under it. DepthTiers.shippedLadderStepsStayInsideTheFovealBudget checks the same
// property in ladder units; this is the one that would notice a `depth_scale` re-tune.
TEST(DepthTiers, shippedLadderStepsStayUnderThreePixelsOfTotalDisparity) {
    constexpr Depth::SGeometry GEO   = {};
    constexpr float            SCALE = 0.12F, PANE = 1920.F;
    constexpr float            FOVEAL_TOTAL_PX = 3.F;

    const auto                 TOTAL = [&](float depth) { return 2.F * Depth::damageSpreadPx(depth, SCALE, GEO, PANE); };

    const float                RUNGS[] = {0.F, SHIPPED.unfocused, SHIPPED.focused, SHIPPED.layers};
    for (size_t i = 1; i < std::size(RUNGS); ++i) {
        const float STEP = TOTAL(RUNGS[i]) - TOTAL(RUNGS[i - 1]);
        EXPECT_GT(STEP, 0.F) << "rung " << i;
        EXPECT_LT(STEP, FOVEAL_TOTAL_PX) << "rung " << i << " steps " << STEP << " px, past the foveal fusion limit";
    }

    // and the whole span against §8.2 budget 1 (the 1° rule, ≈31 px total): §7.2 claims the ladder
    // uses "~20 % of the 1° budget", so hold it to a quarter
    EXPECT_LT(TOTAL(SHIPPED.layers), 2.F * Depth::comfortCeilingPx(GEO, PANE) / 4.F);
}

// The two knobs §8.1's formula exposes as config (`decoration:depth_distance`,
// `depth_screen_width`), each moving the disparity the way the geometry says it must. A user
// measuring their own desk gets a different number of pixels for the same rung, and that is the
// whole point of the keys existing — depth is a physical claim, not a pixel count.
TEST(DepthTiers, geometryKnobsScaleTheDisparity) {
    constexpr float PANE = 1920.F;

    // pixels per metre is P/W, so a screen perceived twice as wide renders the same parallax in
    // half the pixels
    constexpr Depth::SGeometry NARROW = {.distanceM = 1.5F, .screenWidthM = 1.6F};
    constexpr Depth::SGeometry WIDE   = {.distanceM = 1.5F, .screenWidthM = 3.2F};
    EXPECT_NEAR(Depth::shiftMagnitudePx(0.12F, WIDE, PANE), Depth::shiftMagnitudePx(0.12F, NARROW, PANE) / 2.F, 0.001F);

    // a screen further away needs MORE rise for the same parallax: at 3 m, 12 cm of rise is a
    // smaller fraction of the distance than it is at 1.5 m
    constexpr Depth::SGeometry FAR = {.distanceM = 3.0F, .screenWidthM = 1.6F};
    EXPECT_LT(Depth::shiftMagnitudePx(0.12F, FAR, PANE), Depth::shiftMagnitudePx(0.12F, NARROW, PANE));

    // ...and the comfort ceiling moves the other way, because 1° subtends more metres further out
    EXPECT_GT(Depth::comfortCeilingPx(FAR, PANE), Depth::comfortCeilingPx(NARROW, PANE));

    // a degenerate geometry is answered with zero rather than an infinity that would reach the
    // vertex shader
    EXPECT_FLOAT_EQ(Depth::shiftMagnitudePx(0.12F, {.screenWidthM = 0.F}, PANE), 0.F);
    EXPECT_FLOAT_EQ(Depth::shiftMagnitudePx(0.12F, NARROW, 0.F), 0.F);
    EXPECT_FLOAT_EQ(Depth::comfortCeilingPx(NARROW, 0.F), 0.F);
}

// §6.3 pt 2. The damage path has no eye — both panes are drawn in the same frame — so it uses the
// magnitude, and the property that makes that correct is that it bounds every pane's signed shift.
TEST(DepthTiers, damageSpreadCoversEveryPanesShift) {
    constexpr Depth::SGeometry GEO = {};

    for (const float DEPTH : {0.2F, 0.6F, 0.8F, 1.F}) {
        const float SPREAD = Depth::damageSpreadPx(DEPTH, 0.12F, GEO, 1920.F);
        EXPECT_GT(SPREAD, 0.F);

        for (int pane = 0; pane < 2; ++pane)
            EXPECT_LE(std::abs(Depth::paneShiftPx(DEPTH, 0.12F, GEO, 1920.F, pane)), SPREAD);
    }
}

TEST(DepthTiers, comfortCeilingBindsBeforeTheUserHurtsThemselves) {
    constexpr Depth::SGeometry GEO = {};

    // §8.2 budget 1, the 1° rule: ≈31 px total, ≈15.7 px per pane at the shipped geometry
    EXPECT_NEAR(Depth::comfortCeilingPx(GEO, 1920.F), 15.7F, 0.1F);

    // the shipped ladder is nowhere near it — ~3.3 px at depth 1.0, i.e. ~20 % of the budget, which
    // is what §7.2 claims
    EXPECT_LT(Depth::damageSpreadPx(1.F, 0.12F, GEO, 1920.F), Depth::comfortCeilingPx(GEO, 1920.F) / 4.F);

    // ...but a user who sets depth_scale to a metre is stopped at the ceiling rather than obeyed
    EXPECT_FLOAT_EQ(Depth::damageSpreadPx(1.F, 1.F, GEO, 1920.F), Depth::comfortCeilingPx(GEO, 1920.F));
}

// ------------------------------------------------------- §6.1 the frame-violation clamp

TEST(DepthTiers, clampLeavesAnInteriorElementAlone) {
    // a floating window with room on both sides moves by its full disparity
    EXPECT_FLOAT_EQ(Depth::clampToFrame(3.3F, 500, 300, 1920, 2.F), 3.3F);
    EXPECT_FLOAT_EQ(Depth::clampToFrame(-3.3F, 500, 300, 1920, 2.F), -3.3F);
}

TEST(DepthTiers, clampPinsAnEdgeAnchoredBarToTheSlack) {
    // waybar: full width, so BOTH vertical edges sit on the panel edge — §6.1's severe case, where
    // the sliver visible to one eye is what wipes out the parallax cue. It still floats, but only
    // by the sliver `decoration:depth_edge_slack` explicitly accepts.
    EXPECT_FLOAT_EQ(Depth::clampToFrame(3.3F, 0, 1920, 1920, 2.F), 2.F);
    EXPECT_FLOAT_EQ(Depth::clampToFrame(-3.3F, 0, 1920, 1920, 2.F), -2.F);

    // and with the slack at zero it does not float at all, which is the strict reading of the rule
    EXPECT_FLOAT_EQ(Depth::clampToFrame(3.3F, 0, 1920, 1920, 0.F), 0.F);
}

TEST(DepthTiers, clampIsGovernedByTheNEARERedge) {
    // the pair moves the element BOTH ways at once, so the smaller margin binds even though only
    // one pane moves toward it
    EXPECT_FLOAT_EQ(Depth::clampToFrame(9.5F, 4, 100, 1920, 0.F), 4.F);    // 4 px from the left
    EXPECT_FLOAT_EQ(Depth::clampToFrame(9.5F, 1816, 100, 1920, 0.F), 4.F); // 4 px from the right
}

TEST(DepthTiers, clampSurvivesABoxAlreadyOffTheEdge) {
    // a window dragged half off screen has a negative margin; it must clamp to the slack, not to a
    // negative limit that would invert the disparity
    EXPECT_FLOAT_EQ(Depth::clampToFrame(3.3F, -200, 400, 1920, 2.F), 2.F);
    EXPECT_FLOAT_EQ(Depth::clampToFrame(3.3F, -200, 400, 1920, 0.F), 0.F);
}

// The damage contract, end to end (§6.3 pt 2), because this is the property that decides whether
// half a stereo desktop goes stale. CHyprRenderer computes a window's damage growth
// (depthDamageSpread) and its per-pane render offset (depthRenderOffset) from the SAME clamped
// expression, so the invariant is not "the growth is generous" but "the growth is exact": a box
// grown by the spread contains both panes' draws, with nothing left over to repaint.
TEST(DepthTiers, damageGrownBySpreadContainsEveryPanesClampedDraw) {
    constexpr Depth::SGeometry GEO   = {};
    constexpr float            SCALE = 0.30F; // 9.45 px at depth 1.0 — big enough for the clamp to bite
    constexpr double           FRAME = 1920.0;
    constexpr float            SLACK = 2.F;

    struct SCase {
        double      x, w;
        float       depth;
        const char* what;
    };

    // an interior window, a window pinned to each edge, a full-width bar, and one dragged off screen
    static const SCase CASES[] = {
        {500, 300, 1.F, "interior"},       {0, 300, 1.F, "flush left"},         {1620, 300, 1.F, "flush right"},
        {0, 1920, 0.8F, "full-width bar"}, {-200, 400, 1.F, "half off screen"}, {500, 300, 0.F, "interior, no depth"},
    };

    for (const auto& C : CASES) {
        const float RAW    = Depth::damageSpreadPx(C.depth, SCALE, GEO, FRAME);
        const float SPREAD = Depth::clampToFrame(RAW, C.x, C.w, FRAME, SLACK);

        for (int pane = 0; pane < 2; ++pane) {
            const float SHIFT = Depth::clampToFrame(Depth::paneShiftPx(C.depth, SCALE, GEO, FRAME, pane), C.x, C.w, FRAME, SLACK);

            // the growth IS the magnitude of the shift — same expression, same clamp
            EXPECT_FLOAT_EQ(std::abs(SHIFT), SPREAD) << C.what << ", pane " << pane;

            // ...so the grown box contains the drawn box, on both sides
            EXPECT_LE(C.x - SPREAD, C.x + SHIFT) << C.what << ", pane " << pane;
            EXPECT_GE(C.x + C.w + SPREAD, C.x + C.w + SHIFT) << C.what << ", pane " << pane;

            // and the UNclamped spread (what an element with no box to clamp against would use)
            // is still a safe over-approximation, never an under-one
            EXPECT_LE(std::abs(SHIFT), RAW) << C.what << ", pane " << pane;
        }
    }
}

// ------------------------------------------------------- §6.4.1 the fast path, in the pack's terms

// The claim WP D2's commit message makes and D4 has to be able to defend: with nothing raised off
// the wallpaper plane, a stereo output's frame is the one WP F1 shipped, bit for bit. Two headers
// meet to make that true — Depth's spread is exactly zero, so the damage handed to Stereo's fold is
// the pane damage untouched, and the fold is F1's two halves and nothing more.
TEST(DepthTiers, fastPathHandsTheFoldExactlyWhatWPF1Did) {
    constexpr Depth::SGeometry GEO      = {};
    constexpr Vector2D         MODE     = {3840, 1080};
    constexpr float            PANEPX   = 1920.F;
    const CRegion              PANEDMG  = CBox{100, 200, 300, 400};
    const CRegion              F1FOLDED = Monitor::Stereo::foldPaneDamage(PANEDMG, MODE, Config::STEREO_SBS);

    // the two ways a desktop is flat: nothing has depth, or the comfort knob is zero (§6.4's A/B
    // toggle). Both must produce an exactly-zero spread — not a small one.
    for (const auto& [DEPTH, SCALE] : {std::pair{0.F, 0.12F}, std::pair{1.F, 0.F}, std::pair{0.F, 0.F}}) {
        const float SPREAD = Depth::damageSpreadPx(DEPTH, SCALE, GEO, PANEPX);
        ASSERT_FLOAT_EQ(SPREAD, 0.F);

        // growing a damage box by a zero spread is the identity, so the fold sees F1's region...
        CRegion grown = PANEDMG.copy();
        grown.expand(SPREAD);
        EXPECT_EQ(grown.copy().getExtents(), PANEDMG.copy().getExtents());

        // ...and produces F1's two halves
        auto folded = Monitor::Stereo::foldPaneDamage(grown, MODE, Config::STEREO_SBS);
        EXPECT_EQ(folded.copy().getExtents(), F1FOLDED.copy().getExtents());
        EXPECT_TRUE(folded.copy().subtract(F1FOLDED).empty());
        EXPECT_TRUE(F1FOLDED.copy().subtract(folded).empty());
    }

    // the negative control: once something IS raised, the fold sees a WIDER region — depth is not
    // free, and a test that could not tell the two apart would not be testing the fast path
    CRegion raised = PANEDMG.copy();
    raised.expand(Depth::damageSpreadPx(1.F, 0.12F, GEO, PANEPX));
    EXPECT_GT(Monitor::Stereo::foldPaneDamage(raised, MODE, Config::STEREO_SBS).copy().getExtents().width, F1FOLDED.copy().getExtents().width);
}

// ------------------------------------------------------- §8.1's sub-pixel warning

// The report calls the box rounding in ElementRenderer "the thing to check first" and "the number
// one implementation risk for D2". This is that risk, stated as a test rather than a worry: at the
// shipped geometry the ENTIRE ladder spans ~3 px per pane, so rounding each rung to whole pixels
// merges rungs that the design needs to be distinguishable. Hence the seam in
// IElementRenderer::renderSurface rounds on the un-shifted grid and re-applies the disparity in
// floating point — if that ever gets simplified away, the ladder quietly becomes a two-step stair.
TEST(DepthTiers, integerRoundingWouldCollapseTheLadder) {
    constexpr Depth::SGeometry GEO   = {};
    const auto                 SHIFT = [&](float d) { return Depth::paneShiftPx(d, 0.12F, GEO, 1920.F, 0); };

    // two adjacent rungs of a plausible ladder are genuinely different sub-pixel shifts...
    EXPECT_NE(SHIFT(0.2F), SHIFT(0.3F));
    EXPECT_GT(std::abs(SHIFT(0.3F) - SHIFT(0.2F)), 0.2F);

    // ...and identical once rounded to whole pixels
    EXPECT_FLOAT_EQ(std::round(SHIFT(0.2F)), std::round(SHIFT(0.3F)));

    // the shipped unfocused rung is well under one pixel — an integer shift would not render it at
    // all, which is exactly the "everything between 0 and 0.3 looks identical" §8.1 warns about
    EXPECT_LT(SHIFT(0.2F), 1.F);
    EXPECT_GT(SHIFT(0.2F), 0.F);
}

// ---------------------------------------------------------------- the sub-pixel seam itself

// The test above says the ARITHMETIC needs sub-pixel. This says the EXPRESSION the renderer runs to
// keep it does — Desktop::Depth::roundKeepingDisparity, called from IElementRenderer::drawSurface
// and from every decoration that frames a window (border, inner glow, shadow, group bar).
//
// It exists because hyprtester cannot see this: stereoDepthDisparityMovesTheWindow runs at
// `depth_scale = 0.30`, where the shift is 9.45 px and a whole-pixel quantisation error is a
// twentieth of what is being measured. The regime where the seam is LOAD-BEARING is the shipped
// one — 0.61 px unfocused, 1.91 px focused — and replacing the call with a plain CBox::round()
// fails right here instead.
TEST(DepthTiers, theSubPixelSeamKeepsTwoNearbyRungsApart) {
    constexpr Depth::SGeometry GEO   = {};
    const auto                 SHIFT = [&](float d) { return sc<double>(Depth::paneShiftPx(d, 0.12F, GEO, 1920.F, 0)); };

    // two shipped-regime rungs whose shifts share an integer part — 0.61 px and 0.93 px
    const double LOW = SHIFT(0.2F), HIGH = SHIFT(0.3F);
    ASSERT_FLOAT_EQ(std::round(LOW), std::round(HIGH)); // the precondition the seam exists for

    const auto PLACED = [&](double shift) {
        CBox box{100.0 + shift, 50.0, 500.0, 400.0}; // an integer-aligned element, disparity folded in
        Depth::roundKeepingDisparity(box, shift);
        return box;
    };

    // the two rungs land in two different places, by exactly the difference between their shifts
    EXPECT_NEAR(PLACED(HIGH).x - PLACED(LOW).x, HIGH - LOW, 1e-9);
    EXPECT_GT(std::abs(PLACED(HIGH).x - PLACED(LOW).x), 0.2);

    // ...and the element is still pixel-crisp: what was rounded is the box MINUS the disparity, so
    // the surface keeps the exact grid alignment it had with no depth at all
    for (const double SHIFTPX : {LOW, HIGH}) {
        const auto BOX = PLACED(SHIFTPX);
        EXPECT_FLOAT_EQ(BOX.x - SHIFTPX, 100.0);
        EXPECT_FLOAT_EQ(BOX.w, 500.0);
    }

    // a fractional element still rounds — the seam moves the rounding, it does not remove it
    CBox ugly{100.4 + HIGH, 50.6, 500.3, 400.2};
    Depth::roundKeepingDisparity(ugly, HIGH);
    EXPECT_FLOAT_EQ(ugly.x - HIGH, 100.0);
    EXPECT_FLOAT_EQ(ugly.y, 51.0);

    // and with no disparity it IS CBox::round(), which is what keeps the mono path bit-identical
    CBox mono{100.4, 50.6, 500.3, 400.2}, plain = mono;
    Depth::roundKeepingDisparity(mono, 0.0);
    EXPECT_EQ(mono, plain.round());
}

// The other half of the seam, and the one that would show as §6.1's severe artifact: the scissor.
// A sub-pixel box on its way into pixman is TRUNCATED (pixman_region32_init_rect takes int32), so
// scissoring to the box itself clips the column the rasteriser does cover — a one-pixel background
// sliver down a vertical edge, in ONE eye. rasterCover() rounds outward instead.
TEST(DepthTiers, theScissorCoversTheSubPixelColumnTheRasteriserDraws) {
    // 101.906 is a focused window at the shipped defaults: GL covers pixel centres 102.5..601.5,
    // i.e. columns 102..601, while the truncated rect is (101, w 500) — columns 101..600.
    const CBox SHIFTED{101.906, 50.0, 500.0, 400.0};
    const CBox COVER = Depth::rasterCover(SHIFTED);

    EXPECT_FLOAT_EQ(COVER.x, 101.0);
    EXPECT_FLOAT_EQ(COVER.x + COVER.w, 602.0); // the rightmost column the draw can touch
    EXPECT_GE(COVER.w, SHIFTED.w);

    // the truncating conversion loses that column; the cover keeps it
    EXPECT_FALSE(CRegion(SHIFTED).containsPoint({601, 60}));
    EXPECT_TRUE(CRegion(COVER).containsPoint({601, 60}));

    // the right eye leans the other way and loses the LEFT column instead — same defect, other edge
    const CBox LEANLEFT{98.094, 50.0, 500.0, 400.0};
    EXPECT_FLOAT_EQ(Depth::rasterCover(LEANLEFT).x, 98.0);

    // on a whole-pixel box — every box on an ordinary monitor — it is the identity
    const CBox WHOLE{100.0, 50.0, 500.0, 400.0};
    EXPECT_EQ(Depth::rasterCover(WHOLE), WHOLE);
}

// ...and the mirror image, which is the half that is easy to get backwards. A box SUBTRACTED from a
// draw region — a border's inner cutout (OpenGL.cpp renderBorder), a shadow's window cutout — must
// round INWARD. Truncation rounds such a hole outward, which eats a column the shader would have
// drawn: on a 1080-tall window that is a full-height 1 px gap in the border, in ONE eye, which is
// §6.1's rivalry case again. Rounding a subtracted box inward only ever costs overdraw the shader
// discards, so the direction is free to be conservative and must be.
TEST(DepthTiers, theCutoutRoundsInwardSoTheRingKeepsItsColumn) {
    // the inner edge of a border around the same focused window, disparity and all
    const CBox HOLE{101.906, 50.0, 500.0, 400.0};
    const CBox INNER = Depth::rasterInner(HOLE);

    EXPECT_FLOAT_EQ(INNER.x, 102.0);               // ceil, not trunc
    EXPECT_FLOAT_EQ(INNER.x + INNER.w, 601.0);     // floor of the right edge
    EXPECT_LE(INNER.w, HOLE.w);                    // a hole never grows

    // the column at x=101 is NOT subtracted, so the ring keeps it; truncation would have taken it
    EXPECT_TRUE(CRegion(HOLE).containsPoint({101, 60}));
    EXPECT_FALSE(CRegion(INNER).containsPoint({101, 60}));

    // it is strictly inside the cover, which is the invariant the two together have to satisfy:
    // whatever is scissored IN must never be smaller than what is punched OUT
    const CBox COVER = Depth::rasterCover(HOLE);
    EXPECT_LE(COVER.x, INNER.x);
    EXPECT_GE(COVER.x + COVER.w, INNER.x + INNER.w);

    // identity on a whole-pixel box, so the mono path is untouched here too
    const CBox WHOLE{100.0, 50.0, 500.0, 400.0};
    EXPECT_EQ(Depth::rasterInner(WHOLE), WHOLE);

    // and a box thinner than a pixel collapses rather than going negative — CBox with a negative
    // width would invert the subtraction and punch a hole somewhere else entirely
    const CBox SLIVER = Depth::rasterInner(CBox{10.4, 10.4, 0.2, 0.2});
    EXPECT_GE(SLIVER.w, 0.0);
    EXPECT_GE(SLIVER.h, 0.0);
}

// ---------------------------------------------------------------- rule parse

TEST(DepthTiers, windowRuleParsesTheSyntaxUsersWrite) {
    // `windowrule = depth 0.6, match:focus 1`
    EXPECT_EQ(windowRuleDepth("0.6"), std::optional<float>{0.6F});
    EXPECT_EQ(windowRuleDepth("0"), std::optional<float>{0.F});
    EXPECT_EQ(windowRuleDepth("1"), std::optional<float>{1.F});
    EXPECT_EQ(windowRuleDepth(".25"), std::optional<float>{0.25F});
}

TEST(DepthTiers, windowRuleClampsRatherThanRejects) {
    // a user who wrote `depth 2` wants "as high as it goes", not a config error
    EXPECT_EQ(windowRuleDepth("2"), std::optional<float>{1.F});
    EXPECT_EQ(windowRuleDepth("-1"), std::optional<float>{0.F});
}

TEST(DepthTiers, windowRuleRejectsGarbage) {
    CWindowRule rule("test");
    EXPECT_FALSE(rule.addEffect(WINDOW_RULE_EFFECT_DEPTH, "high"));
    EXPECT_FALSE(rule.addEffect(WINDOW_RULE_EFFECT_DEPTH, ""));
    EXPECT_TRUE(rule.effects().empty());
}

TEST(DepthTiers, layerRuleParsesTheSyntaxUsersWrite) {
    // `layerrule = depth 0.8, match:namespace ^(waybar)$`
    EXPECT_EQ(layerRuleDepth("0.8"), std::optional<float>{0.8F});
    EXPECT_EQ(layerRuleDepth("0"), std::optional<float>{0.F});
    EXPECT_EQ(layerRuleDepth("5"), std::optional<float>{1.F});
    EXPECT_EQ(layerRuleDepth("-5"), std::optional<float>{0.F});
}

TEST(DepthTiers, layerRuleRejectsGarbage) {
    CLayerRule rule("test");
    EXPECT_FALSE(rule.addEffect(LAYER_RULE_EFFECT_DEPTH, "up"));
    EXPECT_TRUE(rule.effects().empty());
}

// ---------------------------------------------------------------- the effect is reachable by name

TEST(DepthTiers, effectIsNamedDepthInBothFamilies) {
    // §7.1: "consistent naming across families is worth more than avoiding the duplication". Both
    // engines have to answer to the same word, or `windowrule = depth` and `layerrule = depth`
    // become two different features that happen to look alike.
    EXPECT_EQ(windowEffects()->get("depth"), std::optional{CWindowRuleEffectContainer::storageType{WINDOW_RULE_EFFECT_DEPTH}});
    EXPECT_EQ(layerEffects()->get("depth"), std::optional{CLayerRuleEffectContainer::storageType{LAYER_RULE_EFFECT_DEPTH}});

    // and the string table has to stay aligned with the enum, which is what the effect name lookup
    // in the other direction proves
    EXPECT_EQ(windowEffects()->get(sc<CWindowRuleEffectContainer::storageType>(WINDOW_RULE_EFFECT_DEPTH)), "depth");
    EXPECT_EQ(layerEffects()->get(sc<CLayerRuleEffectContainer::storageType>(LAYER_RULE_EFFECT_DEPTH)), "depth");
}
