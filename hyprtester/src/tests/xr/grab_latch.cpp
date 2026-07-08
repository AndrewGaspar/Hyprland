#include "tests.hpp"

#ifdef WITH_XR_TESTS

#include "../../hyprctlCompat.hpp"
#include "../../Log.hpp"
#include "../../xr/xr_helpers.hpp"
#include "../../xr/RemoteClient.hpp"

#include <chrono>
#include <cmath>
#include <optional>
#include <string>
#include <thread>

namespace {
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

    // The world pos [x,y,z] of monitorName's block in the openxr status JSON, name-scoped
    // (xr-test.conf always carries other static monitor blocks — see grab_live.cpp).
    std::optional<std::array<float, 3>> monitorPos(const std::string& json, const std::string& monitorName) {
        const auto namePos = XR::findAfter(json, "\"name\": \"" + monitorName + "\"");
        if (namePos == std::string::npos)
            return std::nullopt;
        const std::string arr = XR::fieldAfter(json, namePos, "pos");
        const auto        v   = XR::parseFloatArray(arr);
        if (v.size() < 3)
            return std::nullopt;
        return std::array<float, 3>{v[0], v[1], v[2]};
    }

    bool scopedGrabbed(const std::string& json, const std::string& monitorName, bool expect) {
        const auto p = XR::findAfter(json, "\"name\": \"" + monitorName + "\"");
        return p != std::string::npos && XR::fieldAfter(json, p, "grabbed") == (expect ? "true" : "false");
    }

    float dist3(const std::array<float, 3>& a, const std::array<float, 3>& b) {
        const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    // Shared setup: reach focused, create a local-anchored monitor, activate the left controller,
    // find a hover pose, and squeeze-grab. Returns false (with a SKIP already logged) on any
    // known-flaky env failure upstream of an actual grab. On success, fills hoverPos + monitor.
    bool beginScriptedGrab(CTestCase* tc, const std::string& mon, SArtifactGuard& guard, MonadoWire::xrt_vec3& hoverPosOut) {
        auto logSkip = [&](const std::string& r) { XR::logSkip(tc->name(), r); };

        if (!XR::waitForXrState("focused", std::chrono::milliseconds(15000))) {
            logSkip("session never reached focused (known env instability)");
            return false;
        }
        auto* remote = XR::g_ctx.remote;
        if (!remote) {
            logSkip("no remote client available");
            return false;
        }
        if (getFromSocket("/openxr create " + mon + " 1280x720 anchor:local pos:0,0,-1.5 size:1.5") != "ok") {
            logSkip("monitor create failed");
            return false;
        }
        guard.monitorName = mon;

        using namespace MonadoWire;
        remote->setControllerActive(CRemoteClient::SIDE_LEFT, true);
        remote->setTrigger(CRemoteClient::SIDE_LEFT, 0.f);
        remote->setSqueeze(CRemoteClient::SIDE_LEFT, 0.f);

        const struct {
            xrt_vec3 pos;
            xrt_quat rot;
        } attempts[] = {
            {xrt_vec3{0.f, 0.f, 0.f}, xrt_quat{0.f, 0.f, 0.f, 1.f}},
            {xrt_vec3{0.f, 1.5f, 0.f}, xrt_quat{0.f, 0.f, 0.f, 1.f}},
            {xrt_vec3{0.f, -1.5f, 0.f}, xrt_quat{0.f, 0.f, 0.f, 1.f}},
        };

        bool       gotHover = false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        int        idx      = 0;
        while (!gotHover && std::chrono::steady_clock::now() < deadline) {
            const auto& a = attempts[(idx / 40) % (sizeof(attempts) / sizeof(attempts[0]))];
            remote->setControllerPose(CRemoteClient::SIDE_LEFT, a.pos, a.rot);
            remote->pulse();
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            if ((++idx % 20) == 0) {
                const std::string st = getFromSocket("j/openxr");
                const auto         p = XR::findAfter(st, "\"name\": \"" + mon + "\"");
                if (p != std::string::npos && XR::fieldAfter(st, p, "hovered") == "true") {
                    gotHover     = true;
                    hoverPosOut  = a.pos;
                }
            }
        }
        if (!gotHover) {
            logSkip("ray never registered a hover within budget (known env instability)");
            return false;
        }

        remote->setSqueeze(CRemoteClient::SIDE_LEFT, 1.f);
        bool       grabbed  = false;
        const auto gDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!grabbed && std::chrono::steady_clock::now() < gDeadline) {
            remote->pulse();
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            if (scopedGrabbed(getFromSocket("j/openxr"), mon, true))
                grabbed = true;
        }
        if (!grabbed) {
            logSkip("grab never registered within budget (known env instability)");
            return false;
        }
        return true;
    }

    // Poll until the monitor reports released (grabbed:false), then return its settled world pos.
    std::optional<std::array<float, 3>> awaitReleasedPos(const std::string& mon) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            if (XR::g_ctx.remote)
                XR::g_ctx.remote->pulse();
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            const std::string st = getFromSocket("j/openxr");
            if (scopedGrabbed(st, mon, false))
                return monitorPos(st, mon);
        }
        return std::nullopt;
    }
}

// xr_grab_release_latch — THE money test for WP-G4 (research 04-grabbable-borders.md §6-G4).
// Scripts a controller grab: a steady carry, then a sharp 20 cm pose jerk on the final frames
// AS the squeeze is released (the controller analogue of the fist-open lurch). Asserts the
// grabbed monitor's final re-anchored world pose matches the PRE-JERK pose within tolerance —
// i.e. the release-latch rewound past the jerk — not the jerked pose. Fully controller-
// scriptable via the Monado remote driver; no headset needed.
TEST_CASE(xr_grab_release_latch) {
    XR_SKIP_IF_UNAVAILABLE();

    const std::string mon = XR::monitorName(7);
    SArtifactGuard     guard{this->failed, name(), ""};
    MonadoWire::xrt_vec3 hoverPos{};
    if (!beginScriptedGrab(this, mon, guard, hoverPos))
        return;
    NLog::green("xr_grab_release_latch: grab began");

    auto* remote = XR::g_ctx.remote;

    // 1. Steady carry: translate +0.3 m along +X over 500 ms, then hold a few frames to settle.
    remote->animate(
        [&](MonadoWire::r_remote_data& d, float t01) {
            d.left.pose.position.x = hoverPos.x + 0.3f * t01;
            d.left.pose.position.y = hoverPos.y;
            d.left.pose.position.z = hoverPos.z;
        },
        std::chrono::milliseconds(500), 60);
    const MonadoWire::xrt_vec3 steady{hoverPos.x + 0.3f, hoverPos.y, hoverPos.z};
    for (int i = 0; i < 8; ++i) {
        remote->setControllerPose(CRemoteClient::SIDE_LEFT, steady, MonadoWire::xrt_quat{0.f, 0.f, 0.f, 1.f});
        remote->pulse();
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    // Record the PRE-JERK carried pose (status reports lastWorld() while grabbed).
    const auto preJerk = monitorPos(getFromSocket("j/openxr"), mon);
    ASSERT(preJerk.has_value(), true);
    NLog::green("xr_grab_release_latch: pre-jerk pos [{:.3f},{:.3f},{:.3f}]", (*preJerk)[0], (*preJerk)[1], (*preJerk)[2]);

    // 2. Sharp 0.20 m jerk in +Y over ~60 ms, releasing the squeeze partway through the jerk so
    //    the release edge is detected while the ring's newest samples are the fast jerk (the fist-
    //    open analogue). This is what the latch must reject.
    remote->animate(
        [&](MonadoWire::r_remote_data& d, float t01) {
            d.left.pose.position.x = steady.x;
            d.left.pose.position.y = steady.y + 0.20f * t01;
            d.left.pose.position.z = steady.z;
            if (t01 > 0.5f) {
                d.left.squeeze_value.x = 0.f;
                d.left.squeeze_force.x = 0.f;
            }
        },
        std::chrono::milliseconds(60), 120);
    remote->setSqueeze(CRemoteClient::SIDE_LEFT, 0.f);

    const auto finalPos = awaitReleasedPos(mon);
    ASSERT(finalPos.has_value(), true);
    NLog::green("xr_grab_release_latch: final pos  [{:.3f},{:.3f},{:.3f}]", (*finalPos)[0], (*finalPos)[1], (*finalPos)[2]);

    // The jerked pose would be ~0.20 m away in +Y from pre-jerk; the latched pose must be close to
    // pre-jerk instead. Allow generous slack for a residual frame of jerk motion + spring settle.
    const std::array<float, 3> jerked{(*preJerk)[0], (*preJerk)[1] + 0.20f, (*preJerk)[2]};
    const float                dPre  = dist3(*finalPos, *preJerk);
    const float                dJerk = dist3(*finalPos, jerked);
    NLog::green("xr_grab_release_latch: dist-to-preJerk {:.3f} m, dist-to-jerk {:.3f} m", dPre, dJerk);

    ASSERT(dPre < 0.08f, true);    // latched near where the hand actually was before the jerk
    ASSERT(dPre < dJerk, true);    // and unambiguously closer to pre-jerk than to the jerk
    EXPECT_CONTAINS(getFromSocket("j/openxr"), "\"mode\": \"local\"");
    NLog::green("xr_grab_release_latch: release latched past the jerk");
}

// xr_grab_calm_release_noregress — the no-regression half: a calm grab + calm release must land
// the monitor where the hand left it (the latch/velocity-reject must not rewind a clean release).
TEST_CASE(xr_grab_calm_release_noregress) {
    XR_SKIP_IF_UNAVAILABLE();

    const std::string mon = XR::monitorName(8);
    SArtifactGuard     guard{this->failed, name(), ""};
    MonadoWire::xrt_vec3 hoverPos{};
    if (!beginScriptedGrab(this, mon, guard, hoverPos))
        return;
    NLog::green("xr_grab_calm_release_noregress: grab began");

    auto* remote = XR::g_ctx.remote;

    // Calm carry +0.25 m along +X, settle, then release WITHOUT any jerk.
    remote->animate(
        [&](MonadoWire::r_remote_data& d, float t01) {
            d.left.pose.position.x = hoverPos.x + 0.25f * t01;
            d.left.pose.position.y = hoverPos.y;
            d.left.pose.position.z = hoverPos.z;
        },
        std::chrono::milliseconds(500), 60);
    const MonadoWire::xrt_vec3 settled{hoverPos.x + 0.25f, hoverPos.y, hoverPos.z};
    for (int i = 0; i < 10; ++i) {
        remote->setControllerPose(CRemoteClient::SIDE_LEFT, settled, MonadoWire::xrt_quat{0.f, 0.f, 0.f, 1.f});
        remote->pulse();
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    const auto whereLeft = monitorPos(getFromSocket("j/openxr"), mon);
    ASSERT(whereLeft.has_value(), true);

    remote->setSqueeze(CRemoteClient::SIDE_LEFT, 0.f);
    const auto finalPos = awaitReleasedPos(mon);
    ASSERT(finalPos.has_value(), true);

    const float d = dist3(*finalPos, *whereLeft);
    NLog::green("xr_grab_calm_release_noregress: settle->release drift {:.3f} m", d);
    ASSERT(d < 0.05f, true); // a clean release stays put (no spurious rewind)
}

#endif // WITH_XR_TESTS
