#include "tests.hpp"

#ifdef WITH_XR_TESTS

#include "../../hyprctlCompat.hpp"
#include "../../Log.hpp"
#include "../../xr/xr_helpers.hpp"

#include <chrono>
#include <string>
#include <thread>

// xr_voice_verbs — live coverage for the hypxrvoice compositor verbs (docs COMPOSITOR-GAPS.md):
//   GAP 1  `openxr gazegrab <name>`  — begin a gaze carry on a NAMED monitor, no dwell required.
//   GAP 2  `openxr place <name> at x,y,z` — re-anchor to local, moving the center to that point.
//   GAP 3  `selected` status field.
// Unlike the dwell/ray-driven grab tests (which SKIP on "ray never registered a hover" in this
// headless env), the TARGETED gaze grab depends only on head tracking + the monitor being placed —
// so it is deterministic here and asserts, rather than skips, once the session is focused.

namespace {
    struct SArtifactGuard {
        const bool& failed;
        std::string testName;
        std::string monitorName;
        ~SArtifactGuard() {
            if (!monitorName.empty()) {
                getFromSocket("/openxr gazerelease");
                getFromSocket("/openxr destroy " + monitorName);
            }
            if (failed)
                XR::dumpXrArtifacts(testName);
        }
    };
}

TEST_CASE(xr_voice_verbs) {
    XR_SKIP_IF_UNAVAILABLE();

    const std::string mon = XR::monitorName(4);
    SArtifactGuard     guard{this->failed, name(), ""};

    if (!XR::waitForXrState("focused", std::chrono::milliseconds(15000))) {
        XR::logSkip(name(), "session never reached focused (known env instability)");
        return;
    }

    const std::string createReply = getFromSocket("/openxr create " + mon + " 1280x720 anchor:local pos:0,0,-1.5 size:1.5");
    if (createReply != "ok") {
        XR::logSkip(name(), "monitor create failed: " + createReply);
        return;
    }
    guard.monitorName = mon;

    // Wait until the monitor has a solved pose (hasLastWorld) — a gaze carry needs it placed.
    if (!XR::waitForJson(
            "j/openxr", [&](const std::string& r) { return r.contains("\"name\": \"" + mon + "\""); }, std::chrono::milliseconds(10000))) {
        XR::logSkip(name(), "created monitor never appeared in status");
        return;
    }

    // --- GAP 1: targeted gaze grab on the NAMED monitor (no dwell / hover needed). ---
    // A carry needs the monitor PLACED (hasLastWorld) — that is set by the first solve after create,
    // which can lag a frame or two behind the monitor appearing in status. Poll until placed.
    std::string grabReply;
    const auto  grabDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do {
        grabReply = getFromSocket("/openxr gazegrab " + mon);
        if (grabReply == "ok")
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    } while (std::chrono::steady_clock::now() < grabDeadline);
    ASSERT(grabReply, std::string("ok"));

    const bool carrying = XR::waitForJson(
        "j/openxr", [&](const std::string& r) { return r.contains("\"carrying\": true") && r.contains("\"carryMonitor\": \"" + mon + "\""); }, std::chrono::milliseconds(5000));
    ASSERT(carrying, true);
    NLog::green("xr_voice_verbs: targeted gazegrab {} began a carry", mon);

    // A targeted grab on a nonexistent monitor errors cleanly (does not touch the live carry).
    const std::string badReply = getFromSocket("/openxr gazegrab no-such-monitor-xyz");
    EXPECT_CONTAINS(badReply, "no XR monitor named");

    // --- Release the carry. ---
    const std::string relReply = getFromSocket("/openxr gazerelease");
    ASSERT(relReply, std::string("ok"));
    const bool released = XR::waitForJson(
        "j/openxr", [&](const std::string& r) { return r.contains("\"carrying\": false"); }, std::chrono::milliseconds(5000));
    ASSERT(released, true);

    // --- GAP 2: place the monitor at a resolved LOCAL_FLOOR point. ---
    const std::string placeReply = getFromSocket("/openxr place " + mon + " at 0.5,1.4,-1.2");
    ASSERT(placeReply, std::string("ok"));

    // The status pose for a placed (non-grabbed) local monitor reports the anchor pose verbatim:
    // its center must sit at the requested point (formatted {:.3f}).
    const bool placed = XR::waitForJson(
        "j/openxr",
        [&](const std::string& r) {
            const auto p = XR::findAfter(r, "\"name\": \"" + mon + "\"");
            if (p == std::string::npos)
                return false;
            const auto posKey = r.find("\"pos\": [", p);
            if (posKey == std::string::npos)
                return false;
            const auto end = r.find(']', posKey);
            const auto seg = r.substr(posKey, end - posKey);
            return seg.find("0.500") != std::string::npos && seg.find("1.400") != std::string::npos && seg.find("-1.200") != std::string::npos;
        },
        std::chrono::milliseconds(5000));
    ASSERT(placed, true);
    NLog::green("xr_voice_verbs: place moved {} to 0.5,1.4,-1.2", mon);

    // Placing re-anchors to local: the mode must read local.
    const std::string afterPlace = getFromSocket("j/openxr");
    EXPECT_CONTAINS(afterPlace, "\"mode\": \"local\"");

    // --- GAP 3: the status carries a top-level `selected` field. ---
    EXPECT_CONTAINS(afterPlace, "\"selected\":");
}

#endif // WITH_XR_TESTS
