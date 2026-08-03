#include <openxr/XRRule.hpp>

#include <gtest/gtest.h>

using namespace OpenXR;

// Situational per-monitor transparency: the `xrrule` rule engine (doc 05 §xrrule, report 09).
//
// Everything the feature decides is pure and lives here — the flat-string parser, the per-monitor
// condition match, the three-layer precedence fold (defaults -> rules -> manual), the effect
// composition math (uniform alpha x luma key) and the transition envelope. The manager only feeds
// it a context tuple and publishes the numbers, so these tests pin the actual behavior.

namespace {
    SXRRule mustParse(const std::string& s) {
        auto r = parseXRRuleLine(s);
        EXPECT_TRUE(r.has_value()) << "expected '" << s << "' to parse, got: " << (r.has_value() ? "" : r.error());
        return r.value_or(SXRRule{});
    }

    SXRRuleContext ctxFor(const std::string& name, eXRAnchorState st = XR_ANCHORSTATE_DOCKED) {
        SXRRuleContext c;
        c.monitorName = name;
        c.anchorState = st;
        return c;
    }

    SXRRuleContext ctxWindow(const std::string& name, const std::string& cls, const std::string& title, bool fs = false, eXRAnchorState st = XR_ANCHORSTATE_DOCKED) {
        SXRRuleContext c = ctxFor(name, st);
        c.hasFocus       = true;
        c.focusClass     = cls;
        c.focusTitle     = title;
        c.fullscreen     = fs;
        return c;
    }

    SXREffects defaults(float black = 1.F, float knee = 0.1F) {
        SXREffects d;
        d.alpha      = 1.F;
        d.blackAlpha = black;
        d.blackKnee  = knee;
        return d;
    }
}

// ---- parsing: effects ----

TEST(XRRuleParse, SingleAlphaEffectNoConditions) {
    // No comma at all = effects only = matches every monitor ("omitted condition is a wildcard").
    const auto r = mustParse("alpha 0.55");
    ASSERT_TRUE(r.effects.alpha.has_value());
    EXPECT_FLOAT_EQ(*r.effects.alpha, 0.55F);
    EXPECT_FALSE(r.effects.blackAlpha.has_value());
    EXPECT_TRUE(r.conds.empty());
    EXPECT_TRUE(xrRuleMatches(r, ctxFor("anything")));
}

TEST(XRRuleParse, MultipleEffects) {
    const auto r = mustParse("alpha 1.0 blackalpha off blackalpha_knee 0.25, monitor:XR-main");
    ASSERT_TRUE(r.effects.alpha.has_value());
    EXPECT_FLOAT_EQ(*r.effects.alpha, 1.F);
    ASSERT_TRUE(r.effects.blackAlpha.has_value());
    EXPECT_FLOAT_EQ(*r.effects.blackAlpha, 1.F); // `off` == every pixel opaque == keying disabled
    ASSERT_TRUE(r.effects.blackKnee.has_value());
    EXPECT_FLOAT_EQ(*r.effects.blackKnee, 0.25F);
}

TEST(XRRuleParse, KneeIsClampedToTheShaderSafeMinimum) {
    const auto r = mustParse("blackalpha_knee 0.0");
    ASSERT_TRUE(r.effects.blackKnee.has_value());
    EXPECT_FLOAT_EQ(*r.effects.blackKnee, XR_BLACK_ALPHA_KNEE_MIN);
}

TEST(XRRuleParse, MalformedEffects) {
    EXPECT_FALSE(parseXRRuleLine("").has_value());                     // nothing at all
    EXPECT_FALSE(parseXRRuleLine(", monitor:XR-main").has_value());    // conditions but no effect
    EXPECT_FALSE(parseXRRuleLine("alpha").has_value());                // missing value
    EXPECT_FALSE(parseXRRuleLine("alpha 0.5 blackalpha").has_value()); // missing trailing value
    EXPECT_FALSE(parseXRRuleLine("opacity 0.5").has_value());          // unknown effect
    EXPECT_FALSE(parseXRRuleLine("alpha nope").has_value());           // not a number
    EXPECT_FALSE(parseXRRuleLine("alpha 1.5").has_value());            // out of range
    EXPECT_FALSE(parseXRRuleLine("alpha -0.2").has_value());           // out of range
    EXPECT_FALSE(parseXRRuleLine("alpha 0.5 alpha 0.6").has_value());  // given twice
}

// ---- parsing: conditions ----

TEST(XRRuleParse, AllConditionKinds) {
    const auto r = mustParse("alpha 0.4, monitor:^XR- anchorstate:follow focusclass:(mpv|vlc) focustitle:YouTube fullscreen:1");
    EXPECT_TRUE((bool)r.conds.monitorRe);
    EXPECT_TRUE((bool)r.conds.focusClassRe);
    EXPECT_TRUE((bool)r.conds.focusTitleRe);
    ASSERT_TRUE(r.conds.anchorState.has_value());
    EXPECT_EQ(*r.conds.anchorState, XR_ANCHORSTATE_FOLLOW);
    ASSERT_TRUE(r.conds.fullscreen.has_value());
    EXPECT_TRUE(*r.conds.fullscreen);
}

TEST(XRRuleParse, QuotedConditionValueKeepsItsSpaces) {
    // The tokenizer is whitespace-based, so a value that needs a space must be quoted.
    const auto r = mustParse(R"(alpha 0.5, focustitle:"Mozilla Firefox")");
    ASSERT_TRUE((bool)r.conds.focusTitleRe);
    EXPECT_TRUE(xrRuleMatches(r, ctxWindow("XR-main", "firefox", "Reddit — Mozilla Firefox")));
    EXPECT_FALSE(xrRuleMatches(r, ctxWindow("XR-main", "firefox", "MozillaFirefox")));
}

TEST(XRRuleParse, MalformedConditions) {
    EXPECT_FALSE(parseXRRuleLine("alpha 0.5, notakey:x").has_value());
    EXPECT_FALSE(parseXRRuleLine("alpha 0.5, monitor").has_value());          // not key:value
    EXPECT_FALSE(parseXRRuleLine("alpha 0.5, monitor:").has_value());         // empty value
    EXPECT_FALSE(parseXRRuleLine("alpha 0.5, anchorstate:parked").has_value());
    EXPECT_FALSE(parseXRRuleLine("alpha 0.5, fullscreen:maybe").has_value());
    EXPECT_FALSE(parseXRRuleLine("alpha 0.5, monitor:XR- monitor:foo").has_value()); // given twice
}

TEST(XRRuleParse, InvalidRegexIsAConfigError) {
    auto r = parseXRRuleLine("alpha 0.5, monitor:[unterminated");
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("invalid regex"), std::string::npos) << r.error();
}

// ---- matching ----

TEST(XRRuleMatch, MonitorRegexIsAPartialMatch) {
    // PartialMatch (search), not FullMatch: `^XR-` is a prefix test and `(mpv|vlc)` finds a
    // substring. Anchor both ends for an exact match. Documented divergence from windowrules.
    const auto r = mustParse("alpha 0.5, monitor:^XR-");
    EXPECT_TRUE(xrRuleMatches(r, ctxFor("XR-main")));
    EXPECT_TRUE(xrRuleMatches(r, ctxFor("XR-media")));
    EXPECT_FALSE(xrRuleMatches(r, ctxFor("DP-1")));

    const auto exact = mustParse("alpha 0.5, monitor:^XR-main$");
    EXPECT_TRUE(xrRuleMatches(exact, ctxFor("XR-main")));
    EXPECT_FALSE(xrRuleMatches(exact, ctxFor("XR-main2")));
}

TEST(XRRuleMatch, EveryConditionMustHold) {
    const auto r = mustParse("alpha 0.5, monitor:^XR- anchorstate:follow");
    EXPECT_TRUE(xrRuleMatches(r, ctxFor("XR-main", XR_ANCHORSTATE_FOLLOW)));
    EXPECT_FALSE(xrRuleMatches(r, ctxFor("XR-main", XR_ANCHORSTATE_DOCKED)));  // wrong state
    EXPECT_FALSE(xrRuleMatches(r, ctxFor("DP-1", XR_ANCHORSTATE_FOLLOW)));     // wrong monitor
}

TEST(XRRuleMatch, AnchorStateCarried) {
    const auto r = mustParse("alpha 0.6, anchorstate:carried");
    EXPECT_TRUE(xrRuleMatches(r, ctxFor("XR-main", XR_ANCHORSTATE_CARRIED)));
    EXPECT_FALSE(xrRuleMatches(r, ctxFor("XR-main", XR_ANCHORSTATE_FOLLOW)));
}

TEST(XRRuleMatch, FocusConditionsNeedAFocusedWindow) {
    const auto r = mustParse("blackalpha off, focusclass:^steam_app_");
    EXPECT_TRUE(xrRuleMatches(r, ctxWindow("XR-main", "steam_app_620", "Portal 2")));
    EXPECT_FALSE(xrRuleMatches(r, ctxWindow("XR-main", "kitty", "zsh")));
    EXPECT_FALSE(xrRuleMatches(r, ctxFor("XR-main"))); // empty monitor: cannot match
}

TEST(XRRuleMatch, FullscreenZeroMatchesAnEmptyMonitor) {
    // fullscreen: is a property of the monitor's SITUATION, not a claim that a window exists.
    const auto no  = mustParse("alpha 0.5, fullscreen:0");
    const auto yes = mustParse("alpha 0.5, fullscreen:1");
    EXPECT_TRUE(xrRuleMatches(no, ctxFor("XR-main")));
    EXPECT_FALSE(xrRuleMatches(yes, ctxFor("XR-main")));
    EXPECT_TRUE(xrRuleMatches(yes, ctxWindow("XR-main", "mpv", "video", /*fs=*/true)));
    EXPECT_FALSE(xrRuleMatches(no, ctxWindow("XR-main", "mpv", "video", /*fs=*/true)));
}

TEST(XRRuleMatch, EachMonitorIsEvaluatedWithItsOwnTuple) {
    // The point of `monitor:` being a FILTER: one rule set, different outcomes per monitor.
    const std::vector<SXRRule> rules{mustParse("blackalpha off, monitor:XR-media"), mustParse("alpha 0.55, anchorstate:follow")};

    const auto media = xrResolveEffects(defaults(0.2F), rules, ctxFor("XR-media", XR_ANCHORSTATE_DOCKED), {});
    EXPECT_FLOAT_EQ(media.blackAlpha, 1.F); // keying off here
    EXPECT_FLOAT_EQ(media.alpha, 1.F);

    const auto main = xrResolveEffects(defaults(0.2F), rules, ctxFor("XR-main", XR_ANCHORSTATE_FOLLOW), {});
    EXPECT_FLOAT_EQ(main.blackAlpha, 0.2F); // default key still on
    EXPECT_FLOAT_EQ(main.alpha, 0.55F);     // walking rule applied
}

// ---- precedence ----

TEST(XRRuleResolve, DefaultsWhenNothingMatches) {
    const auto res = xrResolveEffects(defaults(0.2F, 0.15F), {mustParse("alpha 0.5, monitor:^DP-")}, ctxFor("XR-main"), {});
    EXPECT_FLOAT_EQ(res.alpha, 1.F);
    EXPECT_FLOAT_EQ(res.blackAlpha, 0.2F);
    EXPECT_FLOAT_EQ(res.blackKnee, 0.15F);
    EXPECT_EQ(res.alphaSrc, XR_EFFSRC_DEFAULT);
    EXPECT_EQ(res.blackAlphaSrc, XR_EFFSRC_DEFAULT);
}

TEST(XRRuleResolve, LaterRuleWinsPerEffectNotWholesale) {
    // The load-bearing precedence detail: rule 2 only sets blackalpha, so rule 1's alpha SURVIVES.
    const std::vector<SXRRule> rules{mustParse("alpha 0.4 blackalpha 0.3, monitor:.*"), mustParse("blackalpha off, monitor:.*")};
    const auto                 res = xrResolveEffects(defaults(), rules, ctxFor("XR-main"), {});
    EXPECT_FLOAT_EQ(res.alpha, 0.4F);
    EXPECT_FLOAT_EQ(res.blackAlpha, 1.F);
    EXPECT_EQ(res.alphaSrc, XR_EFFSRC_RULE);
    EXPECT_EQ(res.blackAlphaSrc, XR_EFFSRC_RULE);
}

TEST(XRRuleResolve, ConfigOrderDecidesBetweenTwoMatchingRules) {
    const std::vector<SXRRule> a{mustParse("alpha 0.3, monitor:.*"), mustParse("alpha 0.9, monitor:.*")};
    const std::vector<SXRRule> b{mustParse("alpha 0.9, monitor:.*"), mustParse("alpha 0.3, monitor:.*")};
    EXPECT_FLOAT_EQ(xrResolveEffects(defaults(), a, ctxFor("XR-main"), {}).alpha, 0.9F);
    EXPECT_FLOAT_EQ(xrResolveEffects(defaults(), b, ctxFor("XR-main"), {}).alpha, 0.3F);
}

TEST(XRRuleResolve, ManualOverrideOutranksEveryRule) {
    const std::vector<SXRRule> rules{mustParse("alpha 0.55, anchorstate:follow")};
    SXREffects                 manual;
    manual.alpha   = 0.8F;
    const auto res = xrResolveEffects(defaults(), rules, ctxFor("XR-main", XR_ANCHORSTATE_FOLLOW), manual);
    EXPECT_FLOAT_EQ(res.alpha, 0.8F);
    EXPECT_EQ(res.alphaSrc, XR_EFFSRC_MANUAL);
}

TEST(XRRuleResolve, ClearingTheManualOverrideFallsBackToTheRules) {
    // `hyprctl openxr alpha <mon> auto` clears the optional; the very next resolution is rule-driven.
    const std::vector<SXRRule> rules{mustParse("alpha 0.55, anchorstate:follow")};
    const auto                 res = xrResolveEffects(defaults(), rules, ctxFor("XR-main", XR_ANCHORSTATE_FOLLOW), SXREffects{});
    EXPECT_FLOAT_EQ(res.alpha, 0.55F);
    EXPECT_EQ(res.alphaSrc, XR_EFFSRC_RULE);
}

TEST(XRRuleResolve, ManualIsPerEffect) {
    // A manual alpha must not freeze the luma key: blackalpha keeps following the rules.
    const std::vector<SXRRule> rules{mustParse("blackalpha 0.25, monitor:.*")};
    SXREffects                 manual;
    manual.alpha   = 0.7F;
    const auto res = xrResolveEffects(defaults(), rules, ctxFor("XR-main"), manual);
    EXPECT_FLOAT_EQ(res.alpha, 0.7F);
    EXPECT_EQ(res.alphaSrc, XR_EFFSRC_MANUAL);
    EXPECT_FLOAT_EQ(res.blackAlpha, 0.25F);
    EXPECT_EQ(res.blackAlphaSrc, XR_EFFSRC_RULE);
}

TEST(XRRuleResolve, ShippedExampleRulesResolveAsAdvertised) {
    // The three documented examples, evaluated together in config order.
    const std::vector<SXRRule> rules{
        mustParse("alpha 1.0 blackalpha off, monitor:.* anchorstate:docked focusclass:^steam_app_ fullscreen:1"),
        mustParse("blackalpha off, monitor:XR-media focusclass:(mpv|vlc)"),
        mustParse("alpha 0.55, anchorstate:follow"),
    };

    // Fullscreen game on a docked monitor: fully opaque, no keying.
    auto game = xrResolveEffects(defaults(0.2F), rules, ctxWindow("XR-main", "steam_app_620", "Portal 2", true, XR_ANCHORSTATE_DOCKED), {});
    EXPECT_FLOAT_EQ(game.alpha, 1.F);
    EXPECT_FLOAT_EQ(game.blackAlpha, 1.F);

    // Walking with a terminal up: ghosted, key still on.
    auto walk = xrResolveEffects(defaults(0.2F), rules, ctxWindow("XR-main", "kitty", "zsh", false, XR_ANCHORSTATE_FOLLOW), {});
    EXPECT_FLOAT_EQ(walk.alpha, 0.55F);
    EXPECT_FLOAT_EQ(walk.blackAlpha, 0.2F);

    // mpv on the media monitor while walking: BOTH match — alpha from the walking rule, key off
    // from the media rule. Per-effect precedence in one shot.
    auto media = xrResolveEffects(defaults(0.2F), rules, ctxWindow("XR-media", "mpv", "video", false, XR_ANCHORSTATE_FOLLOW), {});
    EXPECT_FLOAT_EQ(media.alpha, 0.55F);
    EXPECT_FLOAT_EQ(media.blackAlpha, 1.F);
}

// ---- effect composition (the per-pixel math the GL pipeline reproduces) ----

TEST(XRRuleCompose, UniformAlphaTimesLumaKey) {
    // final_alpha = uniform x lumakey(pixel). The blit bakes the key; fadeTex multiplies the whole
    // composed image by the uniform — the product is what the runtime sees.
    const float knee = 0.1F;
    for (float luma : {0.F, 0.05F, 0.1F, 0.5F, 1.F}) {
        const float key = xrLumaKeyAlphaFromLuma(luma, 0.2F, knee);
        EXPECT_FLOAT_EQ(xrComposeEffectAlpha(luma, 0.5F, 0.2F, knee), 0.5F * key);
    }
}

TEST(XRRuleCompose, EitherHalfAloneIsIdentity) {
    // Key off (1.0): the uniform alpha is the whole story. Uniform 1.0: the key is.
    EXPECT_FLOAT_EQ(xrComposeEffectAlpha(0.F, 0.55F, 1.F, 0.1F), 0.55F);
    EXPECT_FLOAT_EQ(xrComposeEffectAlpha(1.F, 0.55F, 0.2F, 0.1F), 0.55F);
    EXPECT_FLOAT_EQ(xrComposeEffectAlpha(0.F, 1.F, 0.2F, 0.1F), 0.2F);
    EXPECT_FLOAT_EQ(xrComposeEffectAlpha(0.F, 1.F, 1.F, 0.1F), 1.F); // both off -> fully opaque
}

TEST(XRRuleCompose, BlackPixelUnderBothEffects) {
    // A pure-black pixel on a half-faded monitor with a 0.2 key: 0.5 * 0.2 = 0.1.
    EXPECT_FLOAT_EQ(xrComposeEffectAlpha(0.F, 0.5F, 0.2F, 0.1F), 0.1F);
    EXPECT_TRUE(xrUniformAlphaActive(0.5F));
    EXPECT_FALSE(xrUniformAlphaActive(1.F));
}

// ---- transition envelope ----

TEST(XRRuleEnvelope, SettledByDefault) {
    SXRFxEnv e;
    EXPECT_TRUE(e.settled());
    EXPECT_FLOAT_EQ(e.value(), 1.F);
}

TEST(XRRuleEnvelope, RetargetThenEaseToTheTarget) {
    SXRFxEnv e;
    e.set(1.F);
    e.retarget(0.55F);
    EXPECT_FALSE(e.settled());
    EXPECT_FLOAT_EQ(e.value(), 1.F); // t = 0 -> still at the start

    // Half the duration: strictly between, and (smoothstep) exactly halfway at t = 0.5.
    e.advance(0.3F, 0.6F);
    EXPECT_NEAR(e.value(), 0.775F, 1e-5F);
    e.advance(0.3F, 0.6F);
    EXPECT_TRUE(e.settled());
    EXPECT_FLOAT_EQ(e.value(), 0.55F);
}

TEST(XRRuleEnvelope, RetargetToTheSameValueIsANoOp) {
    // Most re-evaluations resolve the same numbers; they must not restart an in-flight transition.
    SXRFxEnv e;
    e.set(1.F);
    e.retarget(0.55F);
    e.advance(0.3F, 0.6F);
    const float mid = e.value();
    e.retarget(0.55F);
    EXPECT_FLOAT_EQ(e.value(), mid);
    EXPECT_FALSE(e.settled());
}

TEST(XRRuleEnvelope, InterruptedTransitionRestartsFromWhereItIsNow) {
    // Walk -> stop walking mid-fade: the value must continue from the current point, never jump.
    SXRFxEnv e;
    e.set(1.F);
    e.retarget(0.55F);
    e.advance(0.3F, 0.6F);
    const float mid = e.value();
    e.retarget(1.F);
    EXPECT_FLOAT_EQ(e.value(), mid); // no discontinuity at the retarget instant
    e.advance(0.6F, 0.6F);
    EXPECT_TRUE(e.settled());
    EXPECT_FLOAT_EQ(e.value(), 1.F);
}

TEST(XRRuleEnvelope, ZeroDurationSnaps) {
    SXRFxEnv e;
    e.set(1.F);
    e.retarget(0.F);
    e.advance(0.001F, 0.F);
    EXPECT_TRUE(e.settled());
    EXPECT_FLOAT_EQ(e.value(), 0.F);
}

TEST(XRRuleEnvelope, NoTimeElapsedHolds) {
    SXRFxEnv e;
    e.set(1.F);
    e.retarget(0.5F);
    e.advance(0.F, 0.6F);
    EXPECT_FALSE(e.settled());
    EXPECT_FLOAT_EQ(e.value(), 1.F);
}

// ---- names (the status surface) ----

TEST(XRRuleNames, SourceAndAnchorStateStrings) {
    EXPECT_STREQ(xrEffectSourceName(XR_EFFSRC_DEFAULT), "default");
    EXPECT_STREQ(xrEffectSourceName(XR_EFFSRC_RULE), "rule");
    EXPECT_STREQ(xrEffectSourceName(XR_EFFSRC_MANUAL), "manual");
    EXPECT_STREQ(xrAnchorStateName(XR_ANCHORSTATE_DOCKED), "docked");
    EXPECT_STREQ(xrAnchorStateName(XR_ANCHORSTATE_FOLLOW), "follow");
    EXPECT_STREQ(xrAnchorStateName(XR_ANCHORSTATE_CARRIED), "carried");
    EXPECT_EQ(xrParseAnchorState("FOLLOW"), XR_ANCHORSTATE_FOLLOW);
    EXPECT_FALSE(xrParseAnchorState("sideways").has_value());
}
