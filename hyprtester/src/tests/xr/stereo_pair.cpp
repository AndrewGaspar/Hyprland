#include "tests.hpp"

#ifdef WITH_XR_TESTS

#include "../../hyprctlCompat.hpp"
#include "../../Log.hpp"
#include "../../xr/xr_helpers.hpp"
#include "../shared.hpp" // Tests:: client helpers (spawnKitty / killAllWindows / sync)

#include <hyprutils/utils/ScopeGuard.hpp>

#include <chrono>
#include <string>
#include <vector>

using Hyprutils::Utils::CScopeGuard;

// tests/xr/stereo_pair.cpp — WP X2, the integration half of the OpenXR quad pair (research/24
// §5.1, WP X1). The pane arithmetic and the activation policy are pure and tested as such in
// tests/xr/StereoPair.cpp; what CANNOT be tested there is the wiring: does a windowrule written at
// runtime actually reach a live XR monitor's submission, across the main→frame publish, and does
// the kill switch actually take it back.
//
// The observable is `hyprctl -j openxr`'s per-monitor `stereo` and `quads`. They are deliberately
// two fields and the test asserts both every time: `stereo` is what the MAIN thread declared and
// `quads` is what the FRAME thread last submitted, so asserting only the first would pass on a
// build where the publish works and the submission never reads it — which is precisely the bug a
// pure test cannot catch.
//
// The rule carries `always`, which switches off §4.3's window-level fullscreen gate. That is on
// purpose: with the window-level gate out of the way, what remains under test is the XR-side
// COVERAGE gate, the one this WP added, and the one that stops a floating stereo window from
// sending half the desktop to each eye.
//
// WP X3 note: this whole case runs with `openxr:depth_desktop = 0`. That is not a workaround, it is
// the CONTENT producer's actual domain — a depth-packed monitor already composites once per eye, so
// its pair belongs to the depth producer and the packed buffer must not be split a second time
// (tests/xr/depth_desktop.cpp covers that side, including a stereo window living inside it). X1's
// route is what an XR monitor does when it is NOT producing panes of its own, and turning the depth
// desktop off is how you ask for it.

namespace {
    // Same session gate the rest of the XR group uses: focused, with a short visible fallback so a
    // dead session fails fast instead of burning the per-test budget.
    bool gateUp() {
        if (XR::waitForXrState("focused", std::chrono::milliseconds(15000)))
            return true;
        return XR::waitForXrState("visible", std::chrono::milliseconds(2000));
    }

    // RAII: dump artifacts iff the test failed, and always destroy the monitors we created.
    struct SArtifactGuard {
        const bool&              failed;
        std::string              testName;
        std::vector<std::string> monitorNames;
        ~SArtifactGuard() {
            for (auto& n : monitorNames)
                if (!n.empty())
                    getFromSocket("/openxr destroy " + n);
            if (failed)
                XR::dumpXrArtifacts(testName);
        }
    };

    // Bound the status JSON to one monitor's block. "name" opens a block and the next "name" opens
    // the following one, so scanning between them is exact (the xr_monitor_churn_content idiom).
    std::string blockOf(const std::string& json, const std::string& mon) {
        const std::string MARKER = "\"name\": \"" + mon + "\"";
        const size_t      POS    = json.find(MARKER);
        if (POS == std::string::npos)
            return "";
        const size_t FROM = POS + MARKER.size();
        const size_t END  = json.find("\"name\":", FROM);
        return json.substr(FROM, END == std::string::npos ? std::string::npos : END - FROM);
    }

    // Wait until this monitor reports exactly this declaration AND this submitted quad count.
    bool waitForPair(const std::string& mon, const std::string& stereo, int quads, std::chrono::milliseconds timeout = std::chrono::milliseconds(10000)) {
        const std::string WANTSTEREO = "\"stereo\": \"" + stereo + "\"";
        const std::string WANTQUADS  = "\"quads\": " + std::to_string(quads);
        return XR::waitForJson(
            "j/openxr",
            [&](const std::string& r) {
                const std::string B = blockOf(r, mon);
                return !B.empty() && B.find(WANTSTEREO) != std::string::npos && B.find(WANTQUADS) != std::string::npos;
            },
            timeout);
    }
}

TEST_CASE(xr_stereo_quad_pair) {
    XR_SKIP_IF_UNAVAILABLE();
    SArtifactGuard guard{this->failed, name(), {}};

    if (!gateUp()) {
        XR::logSkip(name(), "session never reached focused/visible (known env instability)");
        return;
    }

    const std::string MON        = XR::monitorName(30);
    const std::string NAMEMARKER = "\"name\": \"" + MON + "\"";

    // The rule goes in BEFORE the client maps, so the declaration is there the first time the fold
    // runs and the test is not racing a re-evaluation.
    ASSERT(getFromSocket("/keyword windowrule stereo sbs always, match:class ^(xr_stereo_pair)$"), std::string("ok"));
    // The CONTENT producer's domain is an XR monitor that is NOT producing panes of its own — see
    // the header note. Set this BEFORE the monitor is created so it is never packed at all.
    ASSERT(getFromSocket("/keyword openxr:depth_desktop 0"), std::string("ok"));

    CScopeGuard ruleGuard = {[&]() {
        // `off` is the one layout that wins outright — leave nothing behind for later cases.
        getFromSocket("/keyword windowrule stereo off, match:class ^(xr_stereo_pair)$");
        getFromSocket("/keyword openxr:stereo_quad_pair 1");
        getFromSocket("/keyword openxr:depth_desktop 1");
        Tests::killAllWindows();
    }};

    ASSERT(getFromSocket("/openxr create " + MON + " 1280x720"), std::string("ok"));
    guard.monitorNames.push_back(MON);
    ASSERT(XR::waitForJson(
               "j/monitors", [&](const std::string& r) { return r.contains(NAMEMARKER); }, std::chrono::milliseconds(10000)),
           true);

    // A fresh XR monitor with nothing on it is mono, and must cost exactly one quad — the "costs
    // nothing when unused" baseline every later assertion is measured against.
    ASSERT(waitForPair(MON, "off", 1), true);

    ASSERT(getFromSocket("/dispatch focusmonitor " + MON), std::string("ok"));
    auto kitty = Tests::spawnKitty("xr_stereo_pair");
    if (!kitty) {
        XR::logSkip(name(), "kitty did not spawn (env limitation)");
        return;
    }
    ASSERT(XR::waitForJson(
               "j/clients", [&](const std::string& r) { return r.contains("\"xr_stereo_pair\""); }, std::chrono::milliseconds(10000)),
           true);

    // The DECLARATION reached the window — this is Phase S's half, and asserting it separately is
    // what tells "my rule never matched" apart from "my rule matched and the XR gate held it".
    EXPECT_CONTAINS(getFromSocket("/clients"), "stereo: sbs");

    // THE COVERAGE GATE. A small floating window is declared stereo (with `always`, so nothing at
    // the window level is holding it) and still must not pair, because the pair splits the whole
    // content rect and this window is not the whole content rect.
    ASSERT(getFromSocket("/dispatch setfloating class:xr_stereo_pair"), std::string("ok"));
    ASSERT(getFromSocket("/dispatch resizewindowpixel exact 400 300, class:xr_stereo_pair"), std::string("ok"));
    Tests::sync();
    ASSERT(waitForPair(MON, "off", 1), true);

    // Now let it own the output: the declaration engages and the monitor costs a PAIR.
    ASSERT(getFromSocket("/dispatch focuswindow class:xr_stereo_pair"), std::string("ok"));
    ASSERT(getFromSocket("/dispatch fullscreen 0"), std::string("ok"));
    ASSERT(waitForPair(MON, "sbs", 2), true);
    // ...and this pair belongs to the CONTENT producer, which is what decides the pointer un-map.
    // Reading `depth` here would mean the cursor is about to be un-mapped with the identity on an
    // image where each pane is half a packed frame — §5.6's half-a-screen bug.
    EXPECT_CONTAINS(blockOf(getFromSocket("j/openxr"), MON), "\"stereoProducer\": \"content\"");

    // THE KILL SWITCH, which is the whole reason it exists: a runtime is showing something wrong
    // and one keyword must take it back — without moving the window, without a reload, and without
    // waiting for the desktop to repaint (publishStereoPairTuning damages the monitor for exactly
    // this). Both fields must fall back together; `stereo: off, quads: 2` would mean the frame
    // thread never re-read the publish.
    ASSERT(getFromSocket("/keyword openxr:stereo_quad_pair 0"), std::string("ok"));
    ASSERT(waitForPair(MON, "off", 1), true);

    // ...and it comes back, so the switch is a toggle rather than a one-way door.
    ASSERT(getFromSocket("/keyword openxr:stereo_quad_pair 1"), std::string("ok"));
    ASSERT(waitForPair(MON, "sbs", 2), true);

    // Leaving fullscreen un-pairs it: the pair follows the coverage, not the rule.
    ASSERT(getFromSocket("/dispatch fullscreen 0"), std::string("ok"));
    ASSERT(waitForPair(MON, "off", 1), true);

    NLog::green("xr_stereo_quad_pair: declaration -> pair -> kill switch -> pair -> un-pair, all observable in status");
}

#endif // WITH_XR_TESTS
