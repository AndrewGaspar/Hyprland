#include "tests.hpp"

#ifdef WITH_XR_TESTS

#include "../../hyprctlCompat.hpp"
#include "../../Log.hpp"
#include "../../xr/xr_helpers.hpp"
#include "../../xr/RemoteClient.hpp"

#include <chrono>
#include <string>
#include <thread>

// xr_adaptive_geofence — WP-A6 (docs/openxr/research/13-adaptive-anchoring.md §7).
//
// The payoff of keeping the geofence pure + dt-driven: the whole trigger is scriptable from the
// Monado remote driver's head POSITION with no headset. Walk the scripted head out past the leave
// radius, assert the monitor undocks (status phase -> roaming); walk it back inside the return
// radius, assert it re-docks (phase -> docked). Thresholds are shrunk + sped up via hot keywords so
// the whole thing runs in a few seconds. The FEEL (radii/dwell/easing taste) still needs a live
// Quest — this only proves the machine fires end-to-end through the real frame loop + IPC.

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
            if (XR::g_ctx.remote) {
                using namespace MonadoWire;
                XR::g_ctx.remote->setHeadPose(xrt_vec3{0.f, 0.f, 0.f}, xrt_quat{0.f, 0.f, 0.f, 1.f});
                XR::g_ctx.remote->pulse();
            }
            // Restore adaptive thresholds to their defaults so later tests are unaffected.
            getFromSocket("/keyword openxr:adaptive_leave_radius 1.5");
            getFromSocket("/keyword openxr:adaptive_return_radius 1.0");
            getFromSocket("/keyword openxr:adaptive_leave_dwell_ms 400");
            getFromSocket("/keyword openxr:adaptive_return_dwell_ms 800");
            getFromSocket("/keyword openxr:adaptive_transition_ms 700");
            if (!monitorName.empty())
                getFromSocket("/openxr destroy " + monitorName);
            if (failed)
                XR::dumpXrArtifacts(testName);
        }
    };
}

TEST_CASE(xr_adaptive_geofence) {
    XR_SKIP_IF_UNAVAILABLE();

    const std::string mon = XR::monitorName(20);
    SArtifactGuard     guard{this->failed, name(), ""};

    if (!gateUp()) {
        XR::logSkip(name(), "session never reached focused/visible (known env instability)");
        return;
    }

    using namespace MonadoWire;
    auto* remote = XR::g_ctx.remote;
    if (!remote) {
        XR::logSkip(name(), "no remote client available");
        return;
    }

    // Fast, tight geofence so the walk-out/walk-in cycle completes in seconds.
    getFromSocket("/keyword openxr:adaptive_leave_radius 1.0");
    getFromSocket("/keyword openxr:adaptive_return_radius 0.5");
    getFromSocket("/keyword openxr:adaptive_leave_dwell_ms 150");
    getFromSocket("/keyword openxr:adaptive_return_dwell_ms 150");
    getFromSocket("/keyword openxr:adaptive_transition_ms 200");

    // Baseline: head at the origin (the desk seat).
    remote->setHeadPose(xrt_vec3{0.f, 0.f, 0.f}, xrt_quat{0.f, 0.f, 0.f, 1.f});
    remote->pulse();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // An adaptive anchor:local monitor that body-follows when we walk away.
    ASSERT(getFromSocket("/openxr create " + mon + " 1280x720 anchor:local pos:0,1.4,-1.5 adaptive:on roam:body"), std::string("ok"));
    guard.monitorName = mon;
    ASSERT(XR::waitForJson(
               "j/openxr", [&](const std::string& r) { return r.contains("\"name\": \"" + mon + "\""); }, std::chrono::milliseconds(10000)),
           true);
    ASSERT(getFromSocket("/openxr select " + mon), std::string("ok"));

    // Let the seat capture at the origin; expect it docked + adaptive enabled.
    remote->pulse();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    {
        const std::string st = getFromSocket("j/openxr");
        const auto        p  = XR::findAfter(st, "\"name\": \"" + mon + "\"");
        EXPECT(XR::fieldAfter(st, p, "enabled"), std::string("true"));
        EXPECT(XR::fieldAfter(st, p, "phase"), std::string("docked"));
    }

    // Poll status while holding a scripted head position, pulsing to keep the pose fresh + the
    // session alive; return true once the monitor reports `wantPhase`.
    auto holdAndAwaitPhase = [&](const xrt_vec3& headPos, const std::string& wantPhase, int budgetMs) -> bool {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budgetMs);
        while (std::chrono::steady_clock::now() < deadline) {
            remote->setHeadPose(headPos, xrt_quat{0.f, 0.f, 0.f, 1.f});
            remote->pulse();
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
            const std::string st = getFromSocket("j/openxr");
            const auto        p  = XR::findAfter(st, "\"name\": \"" + mon + "\"");
            if (XR::fieldAfter(st, p, "phase") == wantPhase)
                return true;
        }
        return false;
    };

    // Walk out ~2 m along +X (well past the 1.0 m leave radius) -> undock + follow.
    if (!holdAndAwaitPhase(xrt_vec3{2.0f, 0.f, 0.f}, "roaming", 6000))
        MARK_TEST_FAILED("adaptive monitor never reached 'roaming' after the head walked past the leave radius");
    else
        NLog::green("xr_adaptive_geofence: monitor undocked -> roaming after walking past the leave radius");

    // Walk back to the seat (inside the 0.5 m return radius) -> re-dock.
    if (!holdAndAwaitPhase(xrt_vec3{0.f, 0.f, 0.f}, "docked", 6000))
        MARK_TEST_FAILED("adaptive monitor never re-docked after the head returned to the seat");
    else
        NLog::green("xr_adaptive_geofence: monitor re-docked after returning to the seat");
}

#endif // WITH_XR_TESTS
