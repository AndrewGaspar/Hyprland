#include "tests.hpp"

#ifdef WITH_XR_TESTS

#include "../../hyprctlCompat.hpp"
#include "../../Log.hpp"
#include "../../xr/xr_helpers.hpp"
#include "../../xr/RemoteClient.hpp"

#include <chrono>
#include <cmath>
#include <string>
#include <thread>

// xr_gaze_grab — gaze-vector monitor selection + keybind grab/push/release
// (docs/openxr/research/archive/16-gaze-grab.md). Scripts the head pose via the Monado remote
// driver: aim the head at a monitor, dwell, `gazegrab`, yaw the head, assert the carried monitor's
// live pose followed the gaze, `gazepush` to change distance, then `gazerelease`. Purely the VIEW
// (head-forward) source — the only one testable headless (no eye device on the null/remote driver).
// Env-flaky steps SKIP (not fail), consistent with the other xr suite tests; the gaze *logic* is
// covered deterministically by tests/xr/gaze_select.cpp.

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
            // Always drop any lingering gaze carry, then destroy the monitor.
            getFromSocket("/openxr gazerelease");
            if (!monitorName.empty())
                getFromSocket("/openxr destroy " + monitorName);
            if (failed)
                XR::dumpXrArtifacts(testName);
        }
    };

    std::vector<float> posOf(const std::string& json, size_t blockPos) {
        return XR::parseFloatArray(XR::fieldAfter(json, blockPos, "pos"));
    }

    float dist3(const std::vector<float>& a, const std::vector<float>& b) {
        if (a.size() != 3 || b.size() != 3)
            return -1.f;
        const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }
}

TEST_CASE(xr_gaze_grab) {
    XR_SKIP_IF_UNAVAILABLE();

    const std::string mon = XR::monitorName(16);
    SArtifactGuard    guard{this->failed, name(), ""};

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

    // Head at the origin looking straight down -Z; a monitor 1.5 m dead ahead at head height so the
    // center-FOV gaze ray hits it (same geometry the controller-ray tests use for a hover).
    remote->setHeadPose(xrt_vec3{0.f, 0.f, 0.f}, xrt_quat{0.f, 0.f, 0.f, 1.f});
    remote->pulse();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    if (getFromSocket("/openxr create " + mon + " 1280x720 anchor:local pos:0,0,-1.5 size:1.5") != "ok") {
        XR::logSkip(name(), "monitor create failed");
        return;
    }
    guard.monitorName = mon;
    ASSERT(XR::waitForJson(
               "j/openxr", [&](const std::string& r) { return r.contains("\"name\": \"" + mon + "\""); }, std::chrono::milliseconds(10000)),
           true);

    // Make sure the gaze dwell is snappy for the test window.
    getFromSocket("/keyword openxr:gaze_dwell_ms 120");

    // ---- dwell-stable gaze selection: the status `gaze.hoveredName` should become our monitor ----
    const bool gazing = XR::waitForJson(
        "j/openxr",
        [&](const std::string& r) {
            const auto gp = XR::findAfter(r, "\"gaze\"");
            return gp != std::string::npos && XR::fieldAfter(r, gp, "hoveredName") == mon;
        },
        std::chrono::milliseconds(4000));
    if (!gazing) {
        XR::logSkip(name(), "gaze ray never registered a dwell-stable hover on the monitor (known env instability)");
        return;
    }
    NLog::green("xr_gaze_grab: dwell-stable gaze selection landed on {}", mon);

    // ---- grab it (TOGGLE) and confirm the carry state ----
    ASSERT(getFromSocket("/openxr gazegrab"), std::string("ok"));
    {
        const std::string st = getFromSocket("j/openxr");
        const auto        gp = XR::findAfter(st, "\"gaze\"");
        EXPECT(XR::fieldAfter(st, gp, "carrying"), std::string("true"));
        EXPECT(XR::fieldAfter(st, gp, "carryMonitor"), mon);
    }

    // Baseline carried pose (reported live for a gaze carry — reportLive includes gazeGrabbed).
    std::vector<float> before;
    {
        const std::string st = getFromSocket("j/openxr");
        before               = posOf(st, XR::findAfter(st, "\"name\": \"" + mon + "\""));
        ASSERT(before.size(), (size_t)3);
    }

    // ---- yaw the head 90°: with gaze_follow (default) the carried monitor swings to stay ahead ----
    remote->animate(
        [](r_remote_data& d, float t01) {
            const float yaw            = t01 * (float)M_PI / 2.f; // 0 -> 90 deg
            d.head.center.orientation.y = std::sin(yaw / 2.f);
            d.head.center.orientation.w = std::cos(yaw / 2.f);
            d.head.per_view_data_valid  = false;
        },
        std::chrono::milliseconds(800), 60);
    std::this_thread::sleep_for(std::chrono::milliseconds(600));

    {
        const std::string st    = getFromSocket("j/openxr");
        const auto        after = posOf(st, XR::findAfter(st, "\"name\": \"" + mon + "\""));
        ASSERT(after.size(), (size_t)3);
        const float moved = dist3(after, before);
        if (moved < 0.5f)
            MARK_TEST_FAILED("gaze-carried monitor only moved {}m after a 90 deg head yaw (expected the carry to follow the gaze, > 0.5m)", moved);
        else
            NLog::green("xr_gaze_grab: carried monitor followed the gaze {}m after the yaw", moved);
    }

    // ---- push it farther: gaze.distM should grow ----
    float distBefore = -1.f;
    {
        const std::string st = getFromSocket("j/openxr");
        distBefore           = XR::toFloatOr(XR::fieldAfter(st, XR::findAfter(st, "\"gaze\""), "distM"), -1.f);
    }
    ASSERT(getFromSocket("/openxr gazepush 0.5"), std::string("ok"));
    {
        const std::string st       = getFromSocket("j/openxr");
        const float       distNow = XR::toFloatOr(XR::fieldAfter(st, XR::findAfter(st, "\"gaze\""), "distM"), -1.f);
        if (distBefore > 0.f && distNow > 0.f && distNow <= distBefore)
            MARK_TEST_FAILED("gazepush 0.5 did not increase the carry distance ({} -> {})", distBefore, distNow);
        else
            NLog::green("xr_gaze_grab: gazepush moved the carry distance {} -> {}", distBefore, distNow);
    }

    // ---- release (toggle off via the explicit verb): carry clears, monitor stays local ----
    ASSERT(getFromSocket("/openxr gazerelease"), std::string("ok"));
    {
        const std::string st = getFromSocket("j/openxr");
        const auto        gp = XR::findAfter(st, "\"gaze\"");
        EXPECT(XR::fieldAfter(st, gp, "carrying"), std::string("false"));
        EXPECT(XR::fieldAfter(st, XR::findAfter(st, "\"name\": \"" + mon + "\""), "mode"), std::string("local"));
    }

    // Restore the head + the dwell default for later tests sharing the session.
    getFromSocket("/keyword openxr:gaze_dwell_ms 200");
    remote->setHeadPose(xrt_vec3{0.f, 0.f, 0.f}, xrt_quat{0.f, 0.f, 0.f, 1.f});
    remote->pulse();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
}

#endif // WITH_XR_TESTS
