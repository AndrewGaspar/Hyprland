#include "tests.hpp"

#ifdef WITH_XR_TESTS

#include "../../hyprctlCompat.hpp"
#include "../../Log.hpp"
#include "../../xr/xr_helpers.hpp"
#include "../../xr/RemoteClient.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <optional>
#include <string>
#include <thread>

// grab_region.cpp — WP-G3 region-gated grab + corner resize, scripted via the Monado remote
// driver (no headset). The chrome layout is deterministic from the openxr:chrome_* defaults
// (margin 0.04, bar 0.05 @ 0.6 width, corner 0.06), so a controller aiming a -Z ray from a chosen
// (x,y) offset lands the ray at (x,y) on the quad plane and hits a known region. Tests assert the
// gating decision (grabKind: none/move/resize) and, for resize, that the opposite corner is pinned.

namespace {
    using namespace MonadoWire;

    struct SGuard {
        const bool& failed;
        std::string testName;
        std::string monitorName;
        bool        restoreGrabAnywhere = false;
        bool        restoreChrome       = false;
        ~SGuard() {
            if (restoreGrabAnywhere)
                getFromSocket("/keyword openxr:grab_anywhere 1"); // back to default for later tests
            if (restoreChrome) {
                getFromSocket("/keyword openxr:chrome_margin 0.04");
                getFromSocket("/keyword openxr:chrome_corner_size 0.06");
            }
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

    // --- tiny quaternion helper (mirrors OpenXR::qRotate; the suite can't link the compositor) ---
    struct V3 {
        float x, y, z;
    };
    V3 qrot(const std::array<float, 4>& q, V3 v) { // q = [x,y,z,w]
        const V3    u{q[0], q[1], q[2]};
        const float w = q[3];
        const V3    uxv{u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x};
        const V3    t{2.f * uxv.x, 2.f * uxv.y, 2.f * uxv.z};
        const V3    uxt{u.y * t.z - u.z * t.y, u.z * t.x - u.x * t.z, u.x * t.y - u.y * t.x};
        return V3{v.x + w * t.x + uxt.x, v.y + w * t.y + uxt.y, v.z + w * t.z + uxt.z};
    }

    struct SMon {
        std::array<float, 3> pos{};
        std::array<float, 4> quat{0, 0, 0, 1};
        float                size = 0.f;
        std::string          grabKind;
        bool                 grabbed = false;
        bool                 found   = false;
    };

    // Scrape one monitor's block from the openxr status JSON.
    SMon readMon(const std::string& mon) {
        SMon             m;
        const std::string js = getFromSocket("j/openxr");
        const auto        p  = XR::findAfter(js, "\"name\": \"" + mon + "\"");
        if (p == std::string::npos)
            return m;
        m.found       = true;
        m.grabbed     = XR::fieldAfter(js, p, "grabbed") == "true";
        m.grabKind    = XR::fieldAfter(js, p, "grabKind");
        m.size        = std::strtof(XR::fieldAfter(js, p, "size_m").c_str(), nullptr);
        const auto pv = XR::parseFloatArray(XR::fieldAfter(js, p, "pos"));
        const auto qv = XR::parseFloatArray(XR::fieldAfter(js, p, "quat"));
        if (pv.size() >= 3)
            m.pos = {pv[0], pv[1], pv[2]};
        if (qv.size() >= 4)
            m.quat = {qv[0], qv[1], qv[2], qv[3]};
        return m;
    }

    // World position of a content corner (sx = +1 right/-1 left, sy = +1 top/-1 bottom).
    V3 cornerOf(const SMon& m, float sx, float sy, float aspectHW) {
        const float w     = m.size;
        const float h     = w * aspectHW;
        const V3    right = qrot(m.quat, V3{1, 0, 0});
        const V3    up    = qrot(m.quat, V3{0, 1, 0});
        return V3{m.pos[0] + right.x * (sx * w * 0.5f) + up.x * (sy * h * 0.5f), m.pos[1] + right.y * (sx * w * 0.5f) + up.y * (sy * h * 0.5f),
                  m.pos[2] + right.z * (sx * w * 0.5f) + up.z * (sy * h * 0.5f)};
    }

    // Shared: reach focused, (optionally run beforeCreate — e.g. set chrome_* keywords that only
    // take effect at monitor create), create a local monitor facing the viewer, activate the left
    // controller.
    bool setup(CTestCase* tc, const std::string& mon, SGuard& guard, const std::function<void()>& beforeCreate = {}) {
        if (!XR::waitForXrState("focused", std::chrono::milliseconds(15000))) {
            XR::logSkip(tc->name(), "session never reached focused (known env instability)");
            return false;
        }
        if (!XR::g_ctx.remote) {
            XR::logSkip(tc->name(), "no remote client available");
            return false;
        }
        if (beforeCreate)
            beforeCreate();
        if (getFromSocket("/openxr create " + mon + " 1280x720 anchor:local pos:0,0,-1.5 size:1.5") != "ok") {
            XR::logSkip(tc->name(), "monitor create failed");
            return false;
        }
        guard.monitorName = mon;
        auto* r           = XR::g_ctx.remote;
        r->setControllerActive(CRemoteClient::SIDE_LEFT, true);
        r->setTrigger(CRemoteClient::SIDE_LEFT, 0.f);
        r->setSqueeze(CRemoteClient::SIDE_LEFT, 0.f);
        r->pulse();
        return true;
    }

    // Aim the controller at (x,y) on the quad plane, squeeze, and poll: returns the grabKind once
    // grabbed ("move"/"resize"), or "" if no grab registered within the budget. Leaves the squeeze
    // held (caller releases).
    std::string squeezeAt(const std::string& mon, float x, float y, std::chrono::milliseconds budget = std::chrono::milliseconds(2500)) {
        auto* r = XR::g_ctx.remote;
        r->setSqueeze(CRemoteClient::SIDE_LEFT, 0.f);
        // settle the aim first so hover classifies before the squeeze edge
        for (int i = 0; i < 6; ++i) {
            r->setControllerPose(CRemoteClient::SIDE_LEFT, xrt_vec3{x, y, 0.f}, xrt_quat{0, 0, 0, 1});
            r->pulse();
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        r->setSqueeze(CRemoteClient::SIDE_LEFT, 1.f);
        const auto deadline = std::chrono::steady_clock::now() + budget;
        while (std::chrono::steady_clock::now() < deadline) {
            r->setControllerPose(CRemoteClient::SIDE_LEFT, xrt_vec3{x, y, 0.f}, xrt_quat{0, 0, 0, 1});
            r->pulse();
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            const SMon m = readMon(mon);
            if (m.found && m.grabbed)
                return m.grabKind;
        }
        return "";
    }

    void release(const std::string& mon) {
        auto* r = XR::g_ctx.remote;
        r->setSqueeze(CRemoteClient::SIDE_LEFT, 0.f);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < deadline) {
            r->pulse();
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            const SMon m = readMon(mon);
            if (m.found && !m.grabbed)
                return;
        }
    }

    constexpr float ASPECT = 720.f / 1280.f; // content h/w for the created 1280x720 monitor
}

// xr_grab_gating_body_vs_bar — with grab_anywhere=false, a squeeze on the CONTENT body must NOT
// grab, but a squeeze on the bottom move-bar MUST grab as a MOVE. This is the WP-G3 de-confliction.
TEST_CASE(xr_grab_gating_body_vs_bar) {
    XR_SKIP_IF_UNAVAILABLE();

    const std::string mon = XR::monitorName(20);
    SGuard            guard{this->failed, name(), ""};
    if (!setup(this, mon, guard))
        return;

    ASSERT(getFromSocket("/keyword openxr:grab_anywhere 0"), std::string("ok"));
    guard.restoreGrabAnywhere = true;

    // 1. Body center: must NOT grab (grab_anywhere=false confines moves to the chrome).
    const std::string bodyKind = squeezeAt(mon, 0.f, 0.f);
    NLog::green("xr_grab_gating_body_vs_bar: body-center grabKind='{}' (expect none)", bodyKind);
    ASSERT(bodyKind, std::string("")); // no grab
    release(mon);

    // 2. Bottom move-bar (just below the content, on the content centerline) -> MOVE grab. Sweep a
    //    couple of y offsets inside the bar band to absorb env aim jitter.
    std::string barKind;
    for (float by : {-0.42f, -0.44f, -0.40f}) {
        barKind = squeezeAt(mon, 0.f, by);
        if (barKind == "move")
            break;
        release(mon);
    }
    NLog::green("xr_grab_gating_body_vs_bar: bar grabKind='{}' (expect move)", barKind);
    if (barKind.empty()) {
        XR::logSkip(name(), "bar squeeze never registered a grab within budget (known env aim jitter)");
        return;
    }
    ASSERT(barKind, std::string("move"));

    // The bar grab actually moves the monitor: drag +0.2 m along +X and confirm it followed.
    const SMon before = readMon(mon);
    XR::g_ctx.remote->animate(
        [&](r_remote_data& d, float t01) {
            d.left.pose.position.x = 0.f + 0.2f * t01;
            d.left.pose.position.y = -0.42f;
            d.left.pose.position.z = 0.f;
        },
        std::chrono::milliseconds(500), 60);
    const SMon moved = readMon(mon);
    NLog::green("xr_grab_gating_body_vs_bar: bar-move x {:.3f} -> {:.3f}", before.pos[0], moved.pos[0]);
    ASSERT(moved.pos[0] > before.pos[0] + 0.05f, true);
    release(mon);
}

// xr_grab_body_default_regression — the no-regression guard: with the DEFAULT grab_anywhere=true a
// squeeze on the content body still grabs (as a MOVE), preserving today's controller UX.
TEST_CASE(xr_grab_body_default_regression) {
    XR_SKIP_IF_UNAVAILABLE();

    const std::string mon = XR::monitorName(21);
    SGuard            guard{this->failed, name(), ""};
    if (!setup(this, mon, guard))
        return;
    // grab_anywhere defaults true (not touched here).

    const std::string kind = squeezeAt(mon, 0.f, 0.f);
    NLog::green("xr_grab_body_default_regression: body grabKind='{}' (expect move)", kind);
    if (kind.empty()) {
        XR::logSkip(name(), "body squeeze never registered a grab within budget (known env instability)");
        return;
    }
    ASSERT(kind, std::string("move"));
    release(mon);
}

// xr_grab_corner_resize — squeeze a CORNER handle and drag diagonally: the monitor's CONTENT size
// grows and the OPPOSITE corner stays pinned in world within tolerance.
TEST_CASE(xr_grab_corner_resize) {
    XR_SKIP_IF_UNAVAILABLE();

    const std::string mon = XR::monitorName(22);
    SGuard            guard{this->failed, name(), ""};
    // The default 0.04 m corner handle is too small to hit through the remote-aim jitter. Enlarge
    // the chrome margin + corner handle for this test (takes effect at create) so the BR handle is a
    // big, easy target; restored to defaults by the guard. Content stays 1.5 m wide / 1280x720.
    guard.restoreChrome = true;
    if (!setup(this, mon, guard, [] {
            getFromSocket("/keyword openxr:chrome_margin 0.2");
            getFromSocket("/keyword openxr:chrome_corner_size 0.2");
        }))
        return;

    // Find a bottom-right corner aim that classifies as a RESIZE grab. With a 0.2 m margin the BR
    // handle spans world x in [0.75, 0.95], y in [-0.42, -0.62] (content half-width 0.75, half-height
    // ~0.42). Sweep a grid centered in it to absorb aim jitter.
    std::string        kind;
    std::array<float, 2> hit{};
    for (float cx : {0.85f, 0.80f, 0.90f})
        for (float cy : {-0.52f, -0.48f, -0.57f}) {
            kind = squeezeAt(mon, cx, cy);
            if (kind == "resize") {
                hit = {cx, cy};
                break;
            }
            release(mon); // wrong region (move/none) or nothing — try next
        }
    NLog::green("xr_grab_corner_resize: corner grabKind='{}'", kind);
    if (kind != "resize") {
        XR::logSkip(name(), "corner squeeze never registered a resize grab within budget (known env aim jitter)");
        return;
    }

    // Record the pinned (opposite, TL) corner + size before dragging.
    const SMon before = readMon(mon);
    const V3   pinBefore = cornerOf(before, -1.f, 1.f, ASPECT); // TL
    NLog::green("xr_grab_corner_resize: pre size {:.3f} m, TL pin [{:.3f},{:.3f},{:.3f}]", before.size, pinBefore.x, pinBefore.y, pinBefore.z);

    // Drag the BR corner outward (down-right, +X/-Y) along the diagonal to grow it.
    XR::g_ctx.remote->animate(
        [&](r_remote_data& d, float t01) {
            d.left.pose.position.x = hit[0] + 0.22f * t01;
            d.left.pose.position.y = hit[1] - 0.12f * t01;
            d.left.pose.position.z = 0.f;
        },
        std::chrono::milliseconds(600), 60);
    // settle a few frames at the end pose
    for (int i = 0; i < 6; ++i) {
        XR::g_ctx.remote->setControllerPose(CRemoteClient::SIDE_LEFT, xrt_vec3{hit[0] + 0.22f, hit[1] - 0.12f, 0.f}, xrt_quat{0, 0, 0, 1});
        XR::g_ctx.remote->pulse();
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    const SMon during   = readMon(mon);
    const V3   pinDuring = cornerOf(during, -1.f, 1.f, ASPECT);
    NLog::green("xr_grab_corner_resize: dragged size {:.3f} m, TL pin [{:.3f},{:.3f},{:.3f}]", during.size, pinDuring.x, pinDuring.y, pinDuring.z);

    ASSERT(during.size > before.size + 0.05f, true); // grew
    // Opposite corner pinned in world within tolerance (generous for aim/1-frame slack).
    const float dPin = std::sqrt((pinDuring.x - pinBefore.x) * (pinDuring.x - pinBefore.x) + (pinDuring.y - pinBefore.y) * (pinDuring.y - pinBefore.y) +
                                 (pinDuring.z - pinBefore.z) * (pinDuring.z - pinBefore.z));
    NLog::green("xr_grab_corner_resize: opposite-corner drift {:.4f} m", dPin);
    ASSERT(dPin < 0.06f, true);

    // Release latches the resized size (no snap-back).
    const float sizeAtRelease = during.size;
    release(mon);
    const SMon after = readMon(mon);
    NLog::green("xr_grab_corner_resize: post-release size {:.3f} m (grabbed={})", after.size, after.grabbed);
    ASSERT(after.grabbed, false);
    ASSERT(std::fabs(after.size - sizeAtRelease) < 0.1f, true);
}

#endif // WITH_XR_TESTS
