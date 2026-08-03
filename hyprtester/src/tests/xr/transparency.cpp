#include "tests.hpp"

#ifdef WITH_XR_TESTS

#include "../../hyprctlCompat.hpp"
#include "../../Log.hpp"
#include "../../xr/xr_helpers.hpp"

#include <chrono>
#include <string>
#include <thread>

// xr_transparency_rules — situational per-monitor transparency end to end (doc 05 §3.5).
//
// The rule engine's decisions are pinned by pure gtests (tests/xr/xrrule.cpp); what CANNOT be
// covered there is the wiring: does an `xrrule` keyword reach the manager, does it get evaluated
// against a REAL monitor's context, does the eased value land in `hyprctl openxr status`, and does
// the manual override actually outrank the rule and hand control back on `auto`. That whole chain
// (config keyword -> declared list -> snapshot -> evaluation -> envelope -> atomics -> IPC) is what
// this exercises against a live session. It needs no headset and no passthrough: `alpha` is not
// blend-mode gated, so it behaves identically under the null compositor's opaque mode.

namespace {
    bool gateUp() {
        if (XR::waitForXrState("focused", std::chrono::milliseconds(15000)))
            return true;
        return XR::waitForXrState("visible", std::chrono::milliseconds(2000));
    }

    struct SArtifactGuard {
        const bool& failed;
        std::string testName;
        std::string monitorName;
        ~SArtifactGuard() {
            // A dynamic `hyprctl keyword xrrule` APPENDS (there is no name to replace), so the only
            // way to drop the rule again is a reload — leaving it would leak into later tests.
            getFromSocket("/reload");
            getFromSocket("/keyword openxr:transparency_blend_ms 600");
            if (!monitorName.empty())
                getFromSocket("/openxr destroy " + monitorName);
            if (failed)
                XR::dumpXrArtifacts(testName);
        }
    };
}

TEST_CASE(xr_transparency_rules) {
    XR_SKIP_IF_UNAVAILABLE();

    const std::string mon = XR::monitorName(24);
    SArtifactGuard    guard{this->failed, name(), ""};

    if (!gateUp()) {
        XR::logSkip(name(), "session never reached focused/visible (known env instability)");
        return;
    }

    // Snap transitions so the LIVE value equals the target immediately — the envelope shape itself
    // is gtest-covered; here we only care that the resolved number reaches the frame loop.
    getFromSocket("/keyword openxr:transparency_blend_ms 0");

    ASSERT(getFromSocket("/openxr create " + mon + " 1280x720 anchor:local pos:0,1.4,-1.5"), std::string("ok"));
    guard.monitorName = mon;
    ASSERT(XR::waitForJson(
               "j/openxr", [&](const std::string& r) { return r.contains("\"name\": \"" + mon + "\""); }, std::chrono::milliseconds(10000)),
           true);

    auto field = [&](const std::string& key) -> std::string {
        const std::string st = getFromSocket("j/openxr");
        const auto        p  = XR::findAfter(st, "\"name\": \"" + mon + "\"");
        return XR::fieldAfter(st, p, key);
    };
    auto awaitField = [&](const std::string& key, const std::string& want, int budgetMs) -> bool {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budgetMs);
        while (std::chrono::steady_clock::now() < deadline) {
            if (field(key) == want)
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
        }
        return false;
    };

    // 1. Baseline: no rule, no override -> fully opaque, provenance "default".
    EXPECT(field("alphaSource"), std::string("default"));
    EXPECT(field("alphaTarget"), std::string("1.000"));

    // 2. A rule that names this monitor applies (config keyword -> evaluation -> status).
    ASSERT(getFromSocket("/keyword xrrule alpha 0.500, monitor:^" + mon + "$"), std::string("ok"));
    if (!awaitField("alphaSource", "rule", 4000))
        MARK_TEST_FAILED("xrrule never took effect: alphaSource never became 'rule'");
    else {
        EXPECT(field("alphaTarget"), std::string("0.500"));
        EXPECT(field("alpha"), std::string("0.500")); // blend_ms 0 -> the eased value is there already
        NLog::green("xr_transparency_rules: xrrule applied (alpha 0.5, source rule)");
    }

    // 3. The manual override outranks the rule.
    ASSERT(getFromSocket("/openxr alpha " + mon + " 0.8"), std::string("ok"));
    if (!awaitField("alphaSource", "manual", 4000))
        MARK_TEST_FAILED("manual alpha override never won over the rule");
    else {
        EXPECT(field("alphaTarget"), std::string("0.800"));
        NLog::green("xr_transparency_rules: manual override outranks the rule");
    }

    // 4. `auto` clears it and hands the monitor straight back to the rule.
    ASSERT(getFromSocket("/openxr alpha " + mon + " auto"), std::string("ok"));
    if (!awaitField("alphaSource", "rule", 4000))
        MARK_TEST_FAILED("`alpha auto` did not restore rule control");
    else {
        EXPECT(field("alphaTarget"), std::string("0.500"));
        NLog::green("xr_transparency_rules: `auto` restored rule control");
    }

    // 5. The luma-key override is a separate effect: setting it must not disturb alpha. `off` is
    //    used deliberately — it resolves to 1.0 under every blend mode, so this assertion holds on
    //    the null compositor's opaque mode too (a numeric key would be gated off there).
    ASSERT(getFromSocket("/openxr blackalpha " + mon + " off"), std::string("ok"));
    if (!awaitField("blackAlphaSource", "manual", 4000))
        MARK_TEST_FAILED("manual blackalpha override never registered");
    else {
        EXPECT(field("blackAlphaTarget"), std::string("1.000"));
        EXPECT(field("alphaSource"), std::string("rule")); // alpha untouched — per-effect precedence
        NLog::green("xr_transparency_rules: blackalpha override is per-effect (alpha untouched)");
    }

    // 6. Bad input is rejected, not silently clamped.
    EXPECT(getFromSocket("/openxr alpha " + mon + " 5").starts_with("alpha:"), true);
    EXPECT(getFromSocket("/openxr alpha nosuchmonitor 0.5").starts_with("no XR monitor named"), true);
}

#endif // WITH_XR_TESTS
