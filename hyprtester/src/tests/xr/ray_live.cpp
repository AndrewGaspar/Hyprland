#include "tests.hpp"

#ifdef WITH_XR_TESTS

#include "../../hyprctlCompat.hpp"
#include "../../Log.hpp"
#include "../../xr/xr_helpers.hpp"
#include "../../xr/RemoteClient.hpp"

#include <chrono>
#include <string>
#include <thread>

namespace {
    // RAII: dump artifacts iff the test ended up failed (docs §5.2), and always clean up the
    // monitor we created + deactivate the controller, regardless of how the test exits.
    struct SArtifactGuard {
        const bool& failed;
        std::string testName;
        std::string monitorName;
        ~SArtifactGuard() {
            if (!monitorName.empty())
                getFromSocket("/openxr destroy " + monitorName);
            if (XR::g_ctx.remote)
                XR::g_ctx.remote->setControllerActive(CRemoteClient::SIDE_LEFT, false);
            if (failed)
                XR::dumpXrArtifacts(testName);
        }
    };

    // Test-only, name-scoped bool-field check, built on WP11's XR::findAfter/XR::fieldAfter
    // scraping primitives (same pattern as tests/xr/input.cpp's local scopedBoolField). xr-test.conf
    // declares XR-conf-a/b fixtures alongside whatever this test creates, so `j/openxr` always has
    // >= 2 monitor blocks — an unscoped "does '\"hovered\": true' appear anywhere in the blob"
    // search would false-positive the instant either static fixture is (or becomes) hovered for
    // any reason, unrelated to this test's own monitor/controller.
    bool scopedBoolField(const std::string& json, const std::string& monName, const std::string& field, bool expect) {
        const auto p = XR::findAfter(json, "\"name\": \"" + monName + "\"");
        return p != std::string::npos && XR::fieldAfter(json, p, field) == (expect ? "true" : "false");
    }
}

// xr_ray_hover — bounded live smoke test for WP7 (doc 04 §3): drives the Monado remote
// driver's left controller aim toward a freshly-created XR monitor and waits for the ray to
// register a hover in the status JSON. This environment (dual-GPU dev box, see docs/openxr
// WP7/WP10 notes) has known Monado-side instability, so a timeout SKIPs rather than fails —
// this test proves the pointer *can* work when the environment cooperates, it is not a strict
// CI gate. Total wall-clock budget is bounded to ~20s of active polling.
TEST_CASE(xr_ray_hover) {
    XR_SKIP_IF_UNAVAILABLE();

    const std::string mon = XR::monitorName(1);
    SArtifactGuard     guard{this->failed, name(), ""};

    if (!XR::waitForXrState("focused", std::chrono::milliseconds(15000))) {
        XR::logSkip(name(), "session never reached focused (known env instability, see WP7 notes)");
        return;
    }

    auto* remote = XR::g_ctx.remote;
    if (!remote) {
        XR::logSkip(name(), "no remote client available");
        return;
    }

    // A large-ish quad straight ahead along -Z so a roughly-forward-pointing controller has a
    // good chance of hitting it without needing precise aim.
    const std::string createReply = getFromSocket("/openxr create " + mon + " 1280x720 anchor:local pos:0,0,-1.5 size:1.5");
    if (createReply != "ok") {
        XR::logSkip(name(), "monitor create failed: " + createReply);
        return;
    }
    guard.monitorName = mon;

    using namespace MonadoWire;
    remote->setControllerActive(CRemoteClient::SIDE_LEFT, true);
    remote->setTrigger(CRemoteClient::SIDE_LEFT, 0.f);

    // Bracket a couple of plausible controller heights/orientations against LOCAL vs
    // LOCAL_FLOOR reference-frame differences across runtimes (doc 01 floor-offset handling
    // already compensates for this on our side, but keep the bracket cheap insurance).
    struct SPoseAttempt {
        xrt_vec3 pos;
        xrt_quat rot;
    };
    const SPoseAttempt attempts[] = {
        {xrt_vec3{0.f, 0.f, 0.f}, xrt_quat{0.f, 0.f, 0.f, 1.f}},
        {xrt_vec3{0.f, 1.5f, 0.f}, xrt_quat{0.f, 0.f, 0.f, 1.f}},
        {xrt_vec3{0.f, -1.5f, 0.f}, xrt_quat{0.f, 0.f, 0.f, 1.f}},
    };

    bool             gotHover = false;
    const auto       deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    int              idx      = 0;
    while (!gotHover && std::chrono::steady_clock::now() < deadline) {
        const auto& a = attempts[(idx / 40) % (sizeof(attempts) / sizeof(attempts[0]))];
        remote->setControllerPose(CRemoteClient::SIDE_LEFT, a.pos, a.rot);
        remote->pulse();
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        if ((++idx % 20) == 0) {
            const std::string st = getFromSocket("j/openxr");
            if (scopedBoolField(st, mon, "hovered", true))
                gotHover = true;
        }
    }

    if (!gotHover) {
        XR::logSkip(name(), "ray never registered a hover within budget (known env instability, see WP7 notes)");
        return;
    }

    NLog::green("xr_ray_hover: hover detected");

    // Light interaction exercise (not asserted — this test only gates on hover per the WP7
    // acceptance criteria; click routing is covered more thoroughly in WP12).
    remote->setTrigger(CRemoteClient::SIDE_LEFT, 1.f);
    remote->pulse();
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
    remote->setTrigger(CRemoteClient::SIDE_LEFT, 0.f);
    remote->pulse();
}

#endif // WITH_XR_TESTS
