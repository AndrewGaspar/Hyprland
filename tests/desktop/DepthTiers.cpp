#include <desktop/DepthTiers.hpp>
#include <desktop/rule/windowRule/WindowRule.hpp>
#include <desktop/rule/windowRule/WindowRuleEffectContainer.hpp>
#include <desktop/rule/layerRule/LayerRule.hpp>
#include <desktop/rule/layerRule/LayerRuleEffectContainer.hpp>

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
