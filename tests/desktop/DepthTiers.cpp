#include <desktop/DepthTiers.hpp>
#include <desktop/rule/windowRule/WindowRule.hpp>
#include <desktop/rule/windowRule/WindowRuleEffectContainer.hpp>
#include <desktop/rule/layerRule/LayerRule.hpp>
#include <desktop/rule/layerRule/LayerRuleEffectContainer.hpp>

#include <gtest/gtest.h>

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
// What is NOT tested here, because D1 does not ship it: any rendering. Depth is inert until D2.

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
