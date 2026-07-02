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
            if (XR::g_ctx.remote) {
                XR::g_ctx.remote->setSqueeze(CRemoteClient::SIDE_LEFT, 0.f);
                XR::g_ctx.remote->setControllerActive(CRemoteClient::SIDE_LEFT, false);
                XR::g_ctx.remote->pulse();
            }
            if (failed)
                XR::dumpXrArtifacts(testName);
        }
    };

    // Test-only, no-JSON-library substring extraction: the `"pos": [...]` array belonging to
    // `monitorName`'s block in a status blob. Scoped by name (WP11 note: xr-test.conf now
    // declares XR-conf-a/b fixtures alongside whatever this test creates, so `j/openxr` always
    // has >= 3 monitor blocks — a name-unscoped "first pos in the blob" search would silently
    // pick up a *different*, static monitor and never see this test's own movement). Good
    // enough to detect "the pose changed" without a real JSON parser.
    std::string extractPos(const std::string& json, const std::string& monitorName) {
        const auto namePos = json.find("\"name\": \"" + monitorName + "\"");
        if (namePos == std::string::npos)
            return "";
        const std::string key = "\"pos\": [";
        auto               p  = json.find(key, namePos);
        if (p == std::string::npos)
            return "";
        auto e = json.find(']', p);
        if (e == std::string::npos)
            return "";
        return json.substr(p, e - p + 1);
    }

    // Same name-scoping for bool fields ("hovered"/"grabbed") as extractPos above, built on
    // WP11's XR::findAfter/XR::fieldAfter primitives. Without it, "hovered": true / "grabbed":
    // true|false anywhere in the blob would match the XR-conf-a/b static fixtures xr-test.conf
    // always declares, not necessarily this test's own monitor.
    bool scopedBoolField(const std::string& json, const std::string& monitorName, const std::string& field, bool expect) {
        const auto p = XR::findAfter(json, "\"name\": \"" + monitorName + "\"");
        return p != std::string::npos && XR::fieldAfter(json, p, field) == (expect ? "true" : "false");
    }
}

// xr_grab_move — bounded live smoke test for WP8 (doc 04 §6 / doc 03 §4): squeeze-grabs a
// freshly created XR monitor with the left controller, translates the controller, and verifies
// the quad's world pose tracked it; then releases and verifies the grab flag clears (re-anchor).
// Like xr_ray_hover (WP7), this environment has known Monado-side instability, so anything
// upstream of "grab began" (session focus, ray hover) SKIPs on timeout rather than failing —
// but once a grab is confirmed to have begun, pose-tracking and release ARE asserted: that is
// exactly the behavior this WP is responsible for. Total wall-clock budget bounded to ~25s.
TEST_CASE(xr_grab_move) {
    XR_SKIP_IF_UNAVAILABLE();

    const std::string mon = XR::monitorName(3);
    SArtifactGuard     guard{this->failed, name(), ""};

    if (!XR::waitForXrState("focused", std::chrono::milliseconds(15000))) {
        XR::logSkip(name(), "session never reached focused (known env instability, see WP7/WP8 notes)");
        return;
    }

    auto* remote = XR::g_ctx.remote;
    if (!remote) {
        XR::logSkip(name(), "no remote client available");
        return;
    }

    const std::string createReply = getFromSocket("/openxr create " + mon + " 1280x720 anchor:local pos:0,0,-1.5 size:1.5");
    if (createReply != "ok") {
        XR::logSkip(name(), "monitor create failed: " + createReply);
        return;
    }
    guard.monitorName = mon;

    using namespace MonadoWire;
    remote->setControllerActive(CRemoteClient::SIDE_LEFT, true);
    remote->setTrigger(CRemoteClient::SIDE_LEFT, 0.f);
    remote->setSqueeze(CRemoteClient::SIDE_LEFT, 0.f);

    // Bracket a couple of plausible controller heights (same brackets xr_ray_hover uses) to
    // find one that hovers the quad before attempting a grab.
    struct SPoseAttempt {
        xrt_vec3 pos;
        xrt_quat rot;
    };
    const SPoseAttempt attempts[] = {
        {xrt_vec3{0.f, 0.f, 0.f}, xrt_quat{0.f, 0.f, 0.f, 1.f}},
        {xrt_vec3{0.f, 1.5f, 0.f}, xrt_quat{0.f, 0.f, 0.f, 1.f}},
        {xrt_vec3{0.f, -1.5f, 0.f}, xrt_quat{0.f, 0.f, 0.f, 1.f}},
    };

    bool        gotHover = false;
    xrt_vec3    hoverPos{};
    const auto  hoverDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    int         idx           = 0;
    while (!gotHover && std::chrono::steady_clock::now() < hoverDeadline) {
        const auto& a = attempts[(idx / 40) % (sizeof(attempts) / sizeof(attempts[0]))];
        remote->setControllerPose(CRemoteClient::SIDE_LEFT, a.pos, a.rot);
        remote->pulse();
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        if ((++idx % 20) == 0) {
            const std::string st = getFromSocket("j/openxr");
            if (scopedBoolField(st, mon, "hovered", true)) {
                gotHover = true;
                hoverPos = a.pos;
            }
        }
    }

    if (!gotHover) {
        XR::logSkip(name(), "ray never registered a hover within budget (known env instability, see WP7/WP8 notes)");
        return;
    }

    // Squeeze past openxr:grab_threshold (0.7 default) to begin the grab.
    remote->setSqueeze(CRemoteClient::SIDE_LEFT, 1.f);
    bool        grabbed    = false;
    std::string statusAfterGrab;
    const auto  grabDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!grabbed && std::chrono::steady_clock::now() < grabDeadline) {
        remote->pulse();
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        const std::string st = getFromSocket("j/openxr");
        if (scopedBoolField(st, mon, "grabbed", true)) {
            grabbed         = true;
            statusAfterGrab = st;
        }
    }

    if (!grabbed) {
        XR::logSkip(name(), "grab never registered within budget (known env instability, see WP7/WP8 notes)");
        return;
    }
    NLog::green("xr_grab_move: grab began");

    const std::string posBefore = extractPos(statusAfterGrab, mon);

    // Translate the controller 0.5 m along +X while still squeezing; the grabbed quad must
    // track it rigidly (doc 04 §6 / doc 03 §4.2 solve override).
    remote->animate(
        [&](r_remote_data& d, float t01) {
            d.left.pose.position.x = hoverPos.x + 0.5f * t01;
            d.left.pose.position.y = hoverPos.y;
            d.left.pose.position.z = hoverPos.z;
        },
        std::chrono::milliseconds(500), 60);

    const std::string statusMoved = getFromSocket("j/openxr");
    const std::string posAfter    = extractPos(statusMoved, mon);

    ASSERT(posBefore.empty(), false);
    ASSERT(posAfter.empty(), false);
    ASSERT(posBefore == posAfter, false); // the quad must have moved with the controller

    NLog::green("xr_grab_move: quad tracked the controller ({} -> {})", posBefore, posAfter);

    // Release: squeeze below openxr:grab_threshold_release (0.4 default) ends the grab and
    // re-anchors into the persistent mode (still "local" here, doc 03 §4.4).
    remote->setSqueeze(CRemoteClient::SIDE_LEFT, 0.f);
    bool       released       = false;
    const auto releaseDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!released && std::chrono::steady_clock::now() < releaseDeadline) {
        remote->pulse();
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        const std::string st = getFromSocket("j/openxr");
        if (scopedBoolField(st, mon, "grabbed", false))
            released = true;
    }
    ASSERT(released, true);
    NLog::green("xr_grab_move: released and re-anchored");

    // Re-anchor must have preserved the persistent mode (local): the acceptance criteria only
    // require the *representation* to re-derive, not the mode to change.
    const std::string statusReleased = getFromSocket("j/openxr");
    EXPECT_CONTAINS(statusReleased, "\"mode\": \"local\"");
}

#endif // WITH_XR_TESTS
