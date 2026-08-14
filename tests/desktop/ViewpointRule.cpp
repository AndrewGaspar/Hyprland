#include <desktop/rule/windowRule/WindowRule.hpp>
#include <desktop/rule/windowRule/WindowRuleEffectContainer.hpp>

#include <gtest/gtest.h>
#include <hyprutils/memory/Casts.hpp>

#include <optional>

using namespace Desktop::Rule;
using namespace Hyprutils::Memory;

static std::optional<bool> viewpointRule(const std::string& raw) {
    CWindowRule rule("test");
    if (!rule.addEffect(WINDOW_RULE_EFFECT_VIEWPOINT, raw))
        return std::nullopt;

    const auto& effects = rule.effects();
    if (effects.size() != 1 || !std::holds_alternative<bool>(effects[0].value))
        return std::nullopt;

    return std::get<bool>(effects[0].value);
}

TEST(ViewpointRule, IsNamedAndParsesAsAnExplicitBooleanPolicy) {
    EXPECT_EQ(windowEffects()->get("viewpoint"), std::optional{CWindowRuleEffectContainer::storageType{WINDOW_RULE_EFFECT_VIEWPOINT}});
    EXPECT_EQ(windowEffects()->get(sc<CWindowRuleEffectContainer::storageType>(WINDOW_RULE_EFFECT_VIEWPOINT)), "viewpoint");

    EXPECT_EQ(viewpointRule("1"), std::optional{true});
    EXPECT_EQ(viewpointRule("on"), std::optional{true});
    EXPECT_EQ(viewpointRule("0"), std::optional{false});
    EXPECT_EQ(viewpointRule("off"), std::optional{false});
}
