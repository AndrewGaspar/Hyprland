#include "tests.hpp"

#ifdef WITH_XR_TESTS

#include "../../hyprctlCompat.hpp"
#include "../../Log.hpp"
#include "../../xr/xr_helpers.hpp"
#include "../shared.hpp" // Tests:: client helpers (spawnKitty / killAllWindows / sync)

#include <hyprutils/utils/ScopeGuard.hpp>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

using Hyprutils::Utils::CScopeGuard;

// tests/xr/depth_desktop.cpp — WP X3/X4, the integration half of the DEPTH DESKTOP (research/24 §6).
//
// The pane arithmetic, the producer-specific un-map and the budget are pure and tested as such in
// tests/xr/StereoDepthPair.cpp. What cannot be tested there is the thing this WP is actually made
// of: that turning one config value on re-derives a live XR output's scanout mode, that the shipped
// Phase D producer then runs on a headless XR monitor it was never written for, and that the pair
// comes out the other end. Every step of that crosses a modeset, a rule manager, a signal, a
// main→frame publish and a swapchain reallocation, and none of it is expressible as a pure function.
//
// Two invariants are asserted at every step because they are the two ways this feature can be wrong
// while looking right:
//
//  - THE DECLARED SIZE NEVER MOVES. The pack doubles the MODE, not the desktop. If `width` ever
//    changes when depth toggles, every window on every XR monitor has just been reflowed, which is
//    a far worse bug than the feature is a feature.
//  - `stereo` AND `quads` AND `chrome` together. `stereo` is what the main thread declared, `quads`
//    is what the frame thread submitted, `chrome` is what it drew. Asserting only the first passes
//    on a build where the publish works and the submission never reads it.

namespace {
    bool gateUp() {
        if (XR::waitForXrState("focused", std::chrono::milliseconds(15000)))
            return true;
        return XR::waitForXrState("visible", std::chrono::milliseconds(2000));
    }

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

    // Bound the status JSON to one monitor's block (the xr_monitor_churn_content idiom).
    std::string blockOf(const std::string& json, const std::string& mon) {
        const std::string MARKER = "\"name\": \"" + mon + "\"";
        const size_t      POS    = json.find(MARKER);
        if (POS == std::string::npos)
            return "";
        const size_t FROM = POS + MARKER.size();
        const size_t END  = json.find("\"name\":", FROM);
        return json.substr(FROM, END == std::string::npos ? std::string::npos : END - FROM);
    }

    // Wait until this monitor's XR status reports this declaration, producer and submitted count.
    bool waitForDecl(const std::string& mon, const std::string& stereo, const std::string& producer, int quads, std::chrono::milliseconds timeout = std::chrono::milliseconds(15000)) {
        const std::string WANTSTEREO   = "\"stereo\": \"" + stereo + "\"";
        const std::string WANTPRODUCER = "\"stereoProducer\": \"" + producer + "\"";
        const std::string WANTQUADS    = "\"quads\": " + std::to_string(quads);
        return XR::waitForJson(
            "j/openxr",
            [&](const std::string& r) {
                const std::string B = blockOf(r, mon);
                return !B.empty() && B.find(WANTSTEREO) != std::string::npos && B.find(WANTPRODUCER) != std::string::npos && B.find(WANTQUADS) != std::string::npos;
            },
            timeout);
    }

    bool waitForChrome(const std::string& mon, bool live, std::chrono::milliseconds timeout = std::chrono::milliseconds(10000)) {
        const std::string WANT = std::string("\"chrome\": ") + (live ? "true" : "false");
        return XR::waitForJson(
            "j/openxr",
            [&](const std::string& r) {
                const std::string B = blockOf(r, mon);
                return !B.empty() && B.find(WANT) != std::string::npos;
            },
            timeout);
    }

    // --- `hyprctl -j monitors` field readers, the tests/main/stereo.cpp idiom ---
    //
    // Bounding the search to ONE monitor's object is not fussiness: a monitor object contains two
    // NESTED objects (activeWorkspace, specialWorkspace) which each carry their own "name", so
    // "scan forward to the next name" stops in the middle of the block and every key after it reads
    // empty. The nested objects close with "\n    }," and only the outer one closes at "\n}".
    std::string fieldIn(const std::string& json, const std::string& key) {
        const auto KEYPOS = json.find("\"" + key + "\":");
        if (KEYPOS == std::string::npos)
            return "";
        auto valStart = json.find_first_not_of(" \t", KEYPOS + key.length() + 3);
        if (valStart == std::string::npos)
            return "";
        auto valEnd = json.find_first_of(",\n", valStart);
        if (valEnd == std::string::npos)
            valEnd = json.length();
        std::string out = json.substr(valStart, valEnd - valStart);
        while (!out.empty() && (out.back() == ' ' || out.back() == '"' || out.back() == '\r'))
            out.pop_back();
        if (!out.empty() && out.front() == '"')
            out.erase(out.begin());
        return out;
    }

    std::string monitorObject(const std::string& mon) {
        const auto JSON = getFromSocket("j/monitors");
        const auto POS  = JSON.find("\"name\": \"" + mon + "\"");
        if (POS == std::string::npos)
            return "";
        const auto START = JSON.rfind('{', POS);
        const auto END   = JSON.find("\n}", POS);
        if (START == std::string::npos)
            return "";
        return JSON.substr(START, END == std::string::npos ? std::string::npos : END - START);
    }

    std::string monitorField(const std::string& mon, const std::string& key) {
        return fieldIn(monitorObject(mon), key);
    }

    bool waitForMonitorField(const std::string& mon, const std::string& key, const std::string& want, int seconds = 10) {
        for (int i = 0; i < seconds * 10; ++i) {
            if (monitorField(mon, key) == want)
                return true;
            // stereoComposites and stereoContent are written BY a frame, so a monitor that has
            // stopped repainting reports the last one. Nudge it once the natural path has had its
            // chance (the tests/main/stereo.cpp idiom, in the dispatcher the legacy config has).
            if (i == 8)
                getFromSocket("/dispatch forcerendererreload");
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return monitorField(mon, key) == want;
    }
}

// ---------------------------------------------------------------------------------------------

TEST_CASE(xr_depth_desktop_pair) {
    XR_SKIP_IF_UNAVAILABLE();
    SArtifactGuard guard{this->failed, name(), {}};

    if (!gateUp()) {
        XR::logSkip(name(), "session never reached focused/visible (known env instability)");
        return;
    }

    const std::string MON        = XR::monitorName(31);
    const std::string NAMEMARKER = "\"name\": \"" + MON + "\"";

    CScopeGuard       cfgGuard = {[&]() {
        getFromSocket("/keyword openxr:depth_desktop 1");
        getFromSocket("/keyword openxr:stereo_quad_pair 1");
        Tests::killAllWindows();
    }};

    ASSERT(getFromSocket("/keyword openxr:depth_desktop 1"), std::string("ok"));
    ASSERT(getFromSocket("/openxr create " + MON + " 1280x720"), std::string("ok"));
    guard.monitorNames.push_back(MON);
    ASSERT(XR::waitForJson(
               "j/monitors", [&](const std::string& r) { return r.contains(NAMEMARKER); }, std::chrono::milliseconds(10000)),
           true);

    // THE HEADLINE. A depth-desktop XR monitor submits a PAIR with NO stereo window anywhere near
    // it — that is the entire difference from X1, whose pair needs a fullscreen stereo client. The
    // producer field is what says which of the two it is, and it decides the pointer un-map.
    ASSERT(waitForDecl(MON, "sbs", "depth", 2), true);

    // THE INVARIANT. The declared size is PER EYE and does not move: the pack doubled the scanout
    // mode, not the desktop. `hyprctl monitors` reports paneSize(), so a `width` of 640 here would
    // mean every window on this monitor just got reflowed into half the space.
    ASSERT(waitForMonitorField(MON, "width", "1280"), true);
    EXPECT(monitorField(MON, "height"), std::string("720"));
    // ...and the SCALE with it, which is the half `width` cannot show. A physical stereo output is
    // pinned to scale 1.0 because its per-eye pixel grid is real; a virtual pack has no such grid,
    // and pinning it would silently resize every XR desktop the moment depth engaged. Captured here
    // and compared against the unpacked value at the end of the test, so this asserts the property
    // (nothing moved) rather than a number that depends on openxr:default_monitor_scale.
    const std::string SCALE_PACKED = monitorField(MON, "scale");
    EXPECT(SCALE_PACKED.empty(), false);
    ASSERT(waitForMonitorField(MON, "scanoutWidth", "2560"), true);
    EXPECT(monitorField(MON, "scanoutHeight"), std::string("720"));
    EXPECT(monitorField(MON, "stereo"), std::string("sbs"));

    // X4: the chrome is NOT suppressed. A depth desktop that hid the move-bar on every monitor
    // would have taken the primary grab affordance away from the whole session.
    ASSERT(waitForChrome(MON, true), true);

    // The §6.4.1 fast path is live: nothing on this monitor has depth yet, so the two panes are
    // identical and the compositor draws ONE composite and shows it to both eyes. The pair is still
    // submitted (the pack is a property of the mode, not of the frame), and it costs one composite.
    ASSERT(waitForMonitorField(MON, "stereoComposites", "1"), true);

    // ...and now something with depth. A focused window sits at decoration:depth_focused, so the
    // producer engages and the monitor genuinely composites twice — the panes stop being identical,
    // which is the whole feature.
    ASSERT(getFromSocket("/dispatch focusmonitor " + MON), std::string("ok"));
    auto kitty = Tests::spawnKitty("xr_depth_desktop");
    if (!kitty) {
        XR::logSkip(name(), "kitty did not spawn (env limitation)");
        return;
    }
    ASSERT(XR::waitForJson(
               "j/clients", [&](const std::string& r) { return r.contains("\"xr_depth_desktop\""); }, std::chrono::milliseconds(10000)),
           true);
    ASSERT(getFromSocket("/dispatch focuswindow class:xr_depth_desktop"), std::string("ok"));
    ASSERT(waitForMonitorField(MON, "stereoComposites", "2"), true);
    // Still exactly two quads — the second composite changed the PIXELS, not the submission.
    ASSERT(waitForDecl(MON, "sbs", "depth", 2), true);

    // THE KILL SWITCH on a packed monitor. The mode cannot change without a modeset, so the monitor
    // stays packed (`stereo` is still sbs) and ONE quad is submitted showing pane 0 — a mono desktop
    // at the right shape. Reporting `off` here would mean the pack had been dropped, which is a
    // different and much more expensive thing to do.
    ASSERT(getFromSocket("/keyword openxr:stereo_quad_pair 0"), std::string("ok"));
    ASSERT(waitForDecl(MON, "sbs", "depth", 1), true);
    EXPECT(monitorField(MON, "scanoutWidth"), std::string("2560"));
    ASSERT(getFromSocket("/keyword openxr:stereo_quad_pair 1"), std::string("ok"));
    ASSERT(waitForDecl(MON, "sbs", "depth", 2), true);

    // THE MASTER SWITCH. This one DOES drop the pack: the mode goes back to the declared size, the
    // swapchain is rebuilt at one pane, and the monitor is an ordinary quad again. The declared size
    // is unchanged across the whole round trip, which is the property that makes it safe to bind.
    ASSERT(getFromSocket("/keyword openxr:depth_desktop 0"), std::string("ok"));
    ASSERT(waitForDecl(MON, "off", "off", 1), true);
    ASSERT(waitForMonitorField(MON, "width", "1280"), true);
    EXPECT(monitorField(MON, "height"), std::string("720"));
    // THE OTHER HALF OF THE INVARIANT: same scale packed and unpacked. If these ever differ, the
    // depth toggle is a resize, and every window on the monitor moved.
    EXPECT(monitorField(MON, "scale"), SCALE_PACKED);
    // stereo/scanout* are only emitted while the monitor IS stereo, so their absence is the assertion.
    EXPECT(monitorField(MON, "scanoutWidth"), std::string(""));
    ASSERT(waitForChrome(MON, true), true); // an ordinary quad has chrome too

    // ...and back, so it is a toggle rather than a one-way door.
    ASSERT(getFromSocket("/keyword openxr:depth_desktop 1"), std::string("ok"));
    ASSERT(waitForDecl(MON, "sbs", "depth", 2), true);
    ASSERT(waitForMonitorField(MON, "width", "1280"), true);
    EXPECT(monitorField(MON, "scanoutWidth"), std::string("2560"));

    NLog::green("xr_depth_desktop_pair: pack -> pair + chrome -> second composite -> kill switch -> master switch -> back, declared size never moved");
}

// A fullscreen stereo-CONTENT window on a depth-packed monitor. The two producers meet here, and
// the precedence is not what §13's line item guessed: the DEPTH producer keeps the pair, and the
// stereo window is un-packed INSIDE the composite by Phase S's per-surface UV crop (`stereoContent`
// says so). Splitting the packed buffer a second time would halve the desktop.
//
// The picture is the same one X1 shipped — left eye sees the video's left half — but it arrives by
// the other route, and it also works while the window is merely windowed, which X1's route cannot do.
TEST_CASE(xr_depth_desktop_with_stereo_content) {
    XR_SKIP_IF_UNAVAILABLE();
    SArtifactGuard guard{this->failed, name(), {}};

    if (!gateUp()) {
        XR::logSkip(name(), "session never reached focused/visible (known env instability)");
        return;
    }

    const std::string MON        = XR::monitorName(32);
    const std::string NAMEMARKER = "\"name\": \"" + MON + "\"";

    ASSERT(getFromSocket("/keyword windowrule stereo sbs always, match:class ^(xr_depth_content)$"), std::string("ok"));
    CScopeGuard ruleGuard = {[&]() {
        getFromSocket("/keyword windowrule stereo off, match:class ^(xr_depth_content)$");
        getFromSocket("/keyword openxr:depth_desktop 1");
        Tests::killAllWindows();
    }};

    ASSERT(getFromSocket("/keyword openxr:depth_desktop 1"), std::string("ok"));
    ASSERT(getFromSocket("/openxr create " + MON + " 1280x720"), std::string("ok"));
    guard.monitorNames.push_back(MON);
    ASSERT(XR::waitForJson(
               "j/monitors", [&](const std::string& r) { return r.contains(NAMEMARKER); }, std::chrono::milliseconds(10000)),
           true);
    ASSERT(waitForDecl(MON, "sbs", "depth", 2), true);

    ASSERT(getFromSocket("/dispatch focusmonitor " + MON), std::string("ok"));
    auto kitty = Tests::spawnKitty("xr_depth_content");
    if (!kitty) {
        XR::logSkip(name(), "kitty did not spawn (env limitation)");
        return;
    }
    ASSERT(XR::waitForJson(
               "j/clients", [&](const std::string& r) { return r.contains("\"xr_depth_content\""); }, std::chrono::milliseconds(10000)),
           true);
    EXPECT_CONTAINS(getFromSocket("/clients"), "stereo: sbs");

    ASSERT(getFromSocket("/dispatch focuswindow class:xr_depth_content"), std::string("ok"));
    ASSERT(getFromSocket("/dispatch fullscreen 0"), std::string("ok"));

    // The crop reached a drawn surface: the window's packed frame is being un-packed per pane.
    ASSERT(waitForMonitorField(MON, "stereoContent", "true"), true);
    // ...and the monitor is still submitted by the DEPTH producer, as a pair. `content` here would
    // mean the packed buffer was about to be split a second time.
    ASSERT(waitForDecl(MON, "sbs", "depth", 2), true);

    // A WINDOWED stereo window keeps working, which is the thing the content pair structurally
    // cannot do: X1's coverage gate exists precisely because splitting the whole content rect for a
    // floating window would send half the desktop to each eye. Inside a depth desktop the crop is
    // per surface, so the window can be any size and the rest of the screen stays 2D.
    ASSERT(getFromSocket("/dispatch fullscreen 0"), std::string("ok"));
    ASSERT(getFromSocket("/dispatch setfloating class:xr_depth_content"), std::string("ok"));
    ASSERT(getFromSocket("/dispatch resizewindowpixel exact 400 300, class:xr_depth_content"), std::string("ok"));
    Tests::sync();
    ASSERT(waitForMonitorField(MON, "stereoContent", "true"), true);
    ASSERT(waitForDecl(MON, "sbs", "depth", 2), true);

    NLog::green("xr_depth_desktop_with_stereo_content: the depth producer owns the pair and the per-surface crop un-packs the window, fullscreen AND windowed");
}

#endif // WITH_XR_TESTS
