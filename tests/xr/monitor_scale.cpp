#include <openxr/XRMonitorConfig.hpp>

#include <config/values/ConfigValues.hpp>
#include <config/values/types/FloatValue.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

using namespace OpenXR;

// Compositor-owned default scale for XR-created monitors (task #129).
//
// A headless output has no EDID, so CMonitor::getDefaultScale()'s PPI heuristic reads a 1920x1080 XR
// quad as a tiny dense panel and returns 2.0 — legible on a desk, unusably cramped through a headset.
// A monitor a voice command or keybind minted seconds ago (`hyprctl openxr create` -> XR-2, XR-3, …)
// has no `monitor =` line to correct that, which is why configs grew XR-2..XR-8 workaround blocks
// with a cliff at XR-9. xrDefaultMonitorScale is the whole policy: the manager calls it from
// registerDeclaredMonitorRule (the persistent-rule machinery that already makes the declared MODE
// durable across plug/unplug and reload) and writes the result into the rule's scale field.
//
// Everything the feature decides is here. The manager side is plumbing: read the live rule for this
// monitor, ask this function, write the answer back, and let the ordinary rule pipeline apply it.

namespace {
    constexpr float DEFAULT_SCALE = 1.25F; // the shipped openxr:default_monitor_scale default

    // The rule field as the compositor hands it to us: -1 is CMonitorRule's "auto" default, and any
    // `monitor = NAME, ..., auto` line leaves it there too.
    constexpr float RULE_AUTO = -1.F;

    // The mode `hyprctl openxr create` gives a monitor when the args don't say otherwise.
    const Vector2D HEADLESS_DEFAULT_MODE{1920, 1080};

    // The common case: an XR-created, non-stereo monitor at the headless default mode, whose matching
    // rule carries the given scale.
    std::optional<float> forCreatedMonitor(float ruleScale, float configured = DEFAULT_SCALE, const Vector2D& mode = HEADLESS_DEFAULT_MODE) {
        return xrDefaultMonitorScale(/*createdByXR*/ true, xrRuleScaleIsExplicit(ruleScale), /*stereoOutput*/ false, mode, /*skipScaleChecks*/ false, configured);
    }
}

// ---- the auto sentinel: which rule scales count as "the user asked for one" ----

TEST(XRDefaultMonitorScale, AutoSentinelMatchesTheConsumer) {
    // CMonitor::applyMonitorRule decides `autoScale` with `RULE->m_scale <= 0.1`. If these two ever
    // disagree we would either overwrite a scale the user set or hand applyMonitorRule a value it
    // still treats as auto — so pin the threshold from both sides.
    EXPECT_FALSE(xrRuleScaleIsExplicit(RULE_AUTO)); // CMonitorRule's default
    EXPECT_FALSE(xrRuleScaleIsExplicit(0.F));
    EXPECT_FALSE(xrRuleScaleIsExplicit(XR_RULE_SCALE_AUTO_MAX));
    EXPECT_TRUE(xrRuleScaleIsExplicit(1.F));
    EXPECT_TRUE(xrRuleScaleIsExplicit(1.25F));
    EXPECT_TRUE(xrRuleScaleIsExplicit(2.F));
}

// ---- the point of the feature: a created monitor with no rule gets the configured value ----

TEST(XRDefaultMonitorScale, CreatedMonitorWithNoUserScaleGetsTheConfiguredDefault) {
    // `hyprctl openxr create XR-2` with no `monitor = XR-2, ...` line anywhere: the rule manager hands
    // back its hardcoded fallback (scale -1 = auto), and we fill it in instead of letting the PPI
    // heuristic land on 2.0.
    const auto got = forCreatedMonitor(RULE_AUTO);
    ASSERT_TRUE(got.has_value());
    EXPECT_FLOAT_EQ(*got, DEFAULT_SCALE);
}

TEST(XRDefaultMonitorScale, ARuleThatSetsOnlyAModeStillGetsTheDefault) {
    // `monitor = XR-2, 2560x1440@90, auto, auto` claims the MODE but says nothing about scale. The
    // mode ladder (doc 05 §3.1) hands that monitor its resolution; the scale is still ours to fill.
    const auto got = forCreatedMonitor(RULE_AUTO);
    ASSERT_TRUE(got.has_value());
    EXPECT_FLOAT_EQ(*got, DEFAULT_SCALE);
}

TEST(XRDefaultMonitorScale, ARetunedDefaultIsWhatTheNextCreateGets) {
    // The value is read live at every call (a CConfigValue read in registerDeclaredMonitorRule), not
    // frozen at the first create — so `hyprctl keyword openxr:default_monitor_scale 1.5` is what the
    // next created monitor gets, and a reload re-derives it for the ones that already exist.
    EXPECT_FLOAT_EQ(*forCreatedMonitor(RULE_AUTO, 1.25F), 1.25F);
    EXPECT_FLOAT_EQ(*forCreatedMonitor(RULE_AUTO, 1.5F), 1.5F);
    EXPECT_FLOAT_EQ(*forCreatedMonitor(RULE_AUTO, 2.F), 2.F);
}

// ---- precedence: an explicit user scale outranks the compositor default ----

TEST(XRDefaultMonitorScale, ExplicitUserScaleWins) {
    // `monitor = XR-2, preferred, auto, 2` — the documented way to set an XR monitor's scale. We must
    // return nullopt so the caller leaves the rule's own value in place.
    EXPECT_FALSE(forCreatedMonitor(2.F).has_value());
    EXPECT_FALSE(forCreatedMonitor(1.F).has_value());
    EXPECT_FALSE(forCreatedMonitor(1.6666F).has_value());
    // Including a user scale that happens to equal the default — nothing to write either way.
    EXPECT_FALSE(forCreatedMonitor(DEFAULT_SCALE).has_value());
}

TEST(XRDefaultMonitorScale, ExplicitUserScaleSurvivesPlugCyclesAndReloads) {
    // Both durability paths re-run registerDeclaredMonitorRule against the LIVE rule for the monitor:
    //   * a plug cycle (unplug/replug, ensureMonitorStatus refresh) — the rule manager still holds the
    //     rule, scale and all;
    //   * a config reload — CConfigManager::reload() clears the rule manager, the file is re-parsed,
    //     and reassertMonitorModeRules() re-derives from the user's freshly parsed `monitor =` line.
    // In every one of those the input is the same "user set 2.0", so the decision must be stable: a
    // policy that flipped on any of them would silently take the monitor to 1.25 on the next refresh.
    for (int refresh = 0; refresh < 5; ++refresh)
        EXPECT_FALSE(forCreatedMonitor(2.F).has_value()) << "refresh #" << refresh;
}

TEST(XRDefaultMonitorScale, OurOwnAnswerIsIdempotent) {
    // We write the result into the rule, and the very next refresh reads that rule back. The answer
    // must therefore read as explicit, or each pass would rewrite the field forever (and, worse, a
    // returned auto-sentinel would loop through applyMonitorRule's PPI guess).
    const auto first = forCreatedMonitor(RULE_AUTO);
    ASSERT_TRUE(first.has_value());
    EXPECT_TRUE(xrRuleScaleIsExplicit(*first));
    EXPECT_FALSE(forCreatedMonitor(*first).has_value());
}

// ---- opt-out: default 0 hands the decision back to Hyprland ----

TEST(XRDefaultMonitorScale, ZeroOptsOutAndKeepsThePPIGuess) {
    // openxr:default_monitor_scale = 0 means "don't touch it" — the rule keeps its auto sentinel and
    // getDefaultScale() decides, exactly as before this feature existed.
    EXPECT_FALSE(forCreatedMonitor(RULE_AUTO, 0.F).has_value());
    EXPECT_FALSE(forCreatedMonitor(RULE_AUTO, XR_RULE_SCALE_AUTO_MAX).has_value());
    // ...and opting out never resurrects a user's explicit scale decision either way.
    EXPECT_FALSE(forCreatedMonitor(2.F, 0.F).has_value());
}

// ---- the two monitors this must never touch ----

TEST(XRDefaultMonitorScale, AdoptedRealMonitorIsNeverRescaled) {
    // An `xrmonitor` line may ADOPT a pre-existing output (createdByXR == false). That is one of the
    // user's real monitors, with a real EDID behind getDefaultScale() — mirroring the desktop into a
    // headset must not renegotiate the desktop's own scale.
    EXPECT_FALSE(xrDefaultMonitorScale(false, /*ruleScaleExplicit*/ false, /*stereo*/ false, HEADLESS_DEFAULT_MODE, false, DEFAULT_SCALE).has_value());
    EXPECT_FALSE(xrDefaultMonitorScale(false, /*ruleScaleExplicit*/ true, /*stereo*/ false, HEADLESS_DEFAULT_MODE, false, DEFAULT_SCALE).has_value());
}

TEST(XRDefaultMonitorScale, StereoOutputIsNeverRescaled) {
    // No cross-talk with the stereo auto-scale rule (research/24 §3.8): a `stereo:sbs` output is
    // pinned to 1.0 by getDefaultScale() because the pack needs one buffer pixel per physical pixel
    // per eye. Writing an EXPLICIT scale here would defeat that pin (applyMonitorRule only calls
    // getDefaultScale when the rule's scale is auto) and would additionally trip its "stereo output
    // with scale != 1.0" warning. XR-created monitors are never stereo outputs today; this keeps them
    // that way by construction rather than by coincidence. Note the mode divides cleanly here, so the
    // stereo bit is the only thing declining it.
    EXPECT_FALSE(xrDefaultMonitorScale(true, false, /*stereo*/ true, {3840, 1080}, false, DEFAULT_SCALE).has_value());
    EXPECT_FALSE(xrDefaultMonitorScale(true, false, /*stereo*/ true, {3840, 1080}, false, 1.F).has_value());
    EXPECT_TRUE(xrDefaultMonitorScale(true, false, /*stereo*/ false, {3840, 1080}, false, DEFAULT_SCALE).has_value());
}

// ---- the divisor gate: never hand applyMonitorRule a scale it will complain about ----

TEST(XRDefaultMonitorScale, TheDefaultIsCleanOnTheModesXRMonitorsActuallyUse) {
    // 1.25 divides every 16:9 mode into whole logical pixels, which is why it is the shipped default.
    for (const Vector2D& mode : {Vector2D{1920, 1080}, Vector2D{2560, 1440}, Vector2D{1280, 720}, Vector2D{1600, 900}, Vector2D{3840, 2160}, Vector2D{800, 600}})
        EXPECT_TRUE(forCreatedMonitor(RULE_AUTO, DEFAULT_SCALE, mode).has_value()) << mode.x << "x" << mode.y;
}

TEST(XRDefaultMonitorScale, AModeTheDefaultCannotDivideDeclinesInsteadOfErroring) {
    // 1024/1.25 = 819.2. applyMonitorRule treats a non-auto scale as the USER's, so it would snap to
    // the nearest clean divisor, log an ERR and raise a red "Invalid scale passed to monitor"
    // notification — blaming the user for a number the compositor picked. Decline instead: the PPI
    // guess stands, which is exactly where such a monitor already was. (hyprtester's own
    // `xrmonitor = XR-conf-b, 1024x768` is this case.)
    EXPECT_FALSE(forCreatedMonitor(RULE_AUTO, DEFAULT_SCALE, {1024, 768}).has_value());
    EXPECT_FALSE(forCreatedMonitor(RULE_AUTO, DEFAULT_SCALE, {1366, 768}).has_value());
    // Only one axis needs to fail.
    EXPECT_FALSE(forCreatedMonitor(RULE_AUTO, DEFAULT_SCALE, {1920, 768}).has_value());
    // ...and a retuned default that does divide such a mode is honored on it.
    EXPECT_FLOAT_EQ(*forCreatedMonitor(RULE_AUTO, 2.F, {1024, 768}), 2.F);
}

TEST(XRDefaultMonitorScale, AnUnknownModeDeclines) {
    // Before a mode is knowable there is nothing to divide, so claim nothing. The negative sentinels
    // `highres` / `highrr` / `maxwidth` leave in the rule land here too.
    EXPECT_FALSE(forCreatedMonitor(RULE_AUTO, DEFAULT_SCALE, {0, 0}).has_value());
    EXPECT_FALSE(forCreatedMonitor(RULE_AUTO, DEFAULT_SCALE, {-1, -1}).has_value());
    EXPECT_FALSE(forCreatedMonitor(RULE_AUTO, DEFAULT_SCALE, {1920, 0}).has_value());
}

TEST(XRDefaultMonitorScale, DisableScaleChecksBypassesTheDivisorGate) {
    // With debug:disable_scale_checks the consumer accepts any scale without complaint, so the gate
    // has nothing to protect against and the configured value applies verbatim.
    const auto got = xrDefaultMonitorScale(true, false, false, {1024, 768}, /*skipScaleChecks*/ true, DEFAULT_SCALE);
    ASSERT_TRUE(got.has_value());
    EXPECT_FLOAT_EQ(*got, DEFAULT_SCALE);
    // The rungs above it still outrank it, though.
    EXPECT_FALSE(xrDefaultMonitorScale(true, /*ruleScaleExplicit*/ true, false, {1024, 768}, true, DEFAULT_SCALE).has_value());
}

// ---- which rules count as "the user pinned a scale on THIS output" ----

namespace {
    // Just enough of Config::CMonitorRule for xrPinnedRuleScale, in the rule manager's declaration
    // order. The real matcher is CMonitor::matchesStaticSelector; `desc:` selectors are prefix
    // matches against the EDID description, plain ones are exact name matches.
    struct SFakeRule {
        std::string m_name;
        float       m_scale = -1.F;
    };

    float pinnedFor(const std::string& monitorName, const std::string& monitorDesc, const std::vector<SFakeRule>& rules) {
        return xrPinnedRuleScale(rules, [&](const std::string& sel) {
            if (sel.starts_with("desc:"))
                return monitorDesc.starts_with(sel.substr(5));
            return sel == monitorName;
        });
    }
}

TEST(XRPinnedRuleScale, NoRuleAtAllReadsAsAuto) {
    EXPECT_FALSE(xrRuleScaleIsExplicit(pinnedFor("XR-2", "", {})));
}

TEST(XRPinnedRuleScale, ANamelessCatchAllIsNotAnOpinionAboutThisMonitor) {
    // The hazard this exists for. `monitor = , preferred, auto, 1` is what almost every config
    // carries, and CMonitorRuleManager::get() hands it back for ANY output nothing else matches — so
    // reading the merged rule's scale would read a catch-all as "the user set XR-2 to 1.0" and
    // disable the XR default for everybody who has one.
    EXPECT_FALSE(xrRuleScaleIsExplicit(pinnedFor("XR-2", "", {{"", 1.F}})));
    // A named rule for a DIFFERENT output is not an opinion about this one either.
    EXPECT_FALSE(xrRuleScaleIsExplicit(pinnedFor("XR-2", "", {{"", 1.F}, {"eDP-1", 1.5F}, {"XR-main", 2.F}})));
}

TEST(XRPinnedRuleScale, ANamedRuleWins) {
    EXPECT_FLOAT_EQ(pinnedFor("XR-2", "", {{"", 1.F}, {"XR-2", 1.6F}}), 1.6F);
    // ...whichever order the file declares them in.
    EXPECT_FLOAT_EQ(pinnedFor("XR-2", "", {{"XR-2", 1.6F}, {"", 1.F}}), 1.6F);
}

TEST(XRPinnedRuleScale, LaterRulesOutrankEarlierOnes) {
    // Same ranking CMonitorRuleManager::get() uses (it walks its list in reverse and takes the first
    // match), so a `hyprctl keyword monitor XR-2,...` issued at runtime beats the config's line.
    EXPECT_FLOAT_EQ(pinnedFor("XR-2", "", {{"XR-2", 1.25F}, {"XR-2", 2.F}}), 2.F);
    EXPECT_FALSE(xrRuleScaleIsExplicit(pinnedFor("XR-2", "", {{"XR-2", 2.F}, {"XR-2", -1.F}})));
}

TEST(XRPinnedRuleScale, DescSelectorsAlsoPin) {
    // An XR output has no EDID description, so this mostly matters for the adopted case — but a rule
    // that matches by description is every bit as explicit as one matching by name.
    EXPECT_FLOAT_EQ(pinnedFor("DP-5", "Nreal Air 2 Ultra 0x88888800", {{"desc:Nreal Air 2", 1.F}}), 1.F);
    EXPECT_FALSE(xrRuleScaleIsExplicit(pinnedFor("XR-2", "", {{"desc:Nreal Air 2", 1.F}})));
}

// ---- the config declaration itself ----

TEST(XRDefaultMonitorScale, ConfigVarIsDeclaredWithTheDocumentedDefault) {
    // The var is declared exactly once, in getConfigValues(). This pins the name docs/scripts use,
    // the shipped default the tests above assume, and the range — 0 must stay reachable, since that
    // is the opt-out.
    const auto it = std::ranges::find_if(Config::Values::CONFIG_VALUES, [](const auto& v) { return std::string_view(v->name()) == "openxr:default_monitor_scale"; });
    ASSERT_NE(it, Config::Values::CONFIG_VALUES.end()) << "openxr:default_monitor_scale is not declared";

    const auto asFloat = dynamic_cast<Config::Values::CFloatValue*>(it->get());
    ASSERT_NE(asFloat, nullptr) << "openxr:default_monitor_scale must be a FLOAT (hot-tunable, no string reads on the frame thread)";
    EXPECT_FLOAT_EQ(asFloat->defaultVal(), DEFAULT_SCALE);
    ASSERT_TRUE(asFloat->m_min.has_value());
    EXPECT_FLOAT_EQ(*asFloat->m_min, 0.F);
    ASSERT_TRUE(asFloat->m_max.has_value());
    EXPECT_GE(*asFloat->m_max, 2.F);
}
