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
#include <vector>

// docs/openxr/research/30-donoff-intent.md §13.2 — don/doff intent, Phases 0/1, end to end against
// the remote driver. The remote driver has no XR_EXT_user_presence, so these ride the
// visibility/session edges (the doff is `openxr disable`, a session end), and it never reports
// TRACKED, so the history's trust rule falls back to VALID exactly as it does live on such runtimes.
//
// A scripted head is perfectly motionless between pulses, which the history would rightly call a
// headset lying on a desk (dead-still, §4.1). So every "hold" here sways the head by under a degree,
// the way a worn head always does.

static bool gateUp() {
    if (XR::waitForXrState("focused", std::chrono::milliseconds(15000)))
        return true;
    return XR::waitForXrState("visible", std::chrono::milliseconds(2000));
}

static std::vector<float> restoreOffsetOf(const std::string& json, size_t blockPos) {
    return XR::parseFloatArray(XR::fieldAfter(json, blockPos, "offset"));
}

static MonadoWire::xrt_quat yawPitch(float yaw, float pitch) {
    // q = qYaw(yaw) * qPitch(pitch) — yaw about +Y, then pitch about the head's +X
    const float cy = std::cos(yaw / 2.f), sy = std::sin(yaw / 2.f), cp = std::cos(pitch / 2.f), sp = std::sin(pitch / 2.f);
    return MonadoWire::xrt_quat{cy * sp, sy * cp, -sy * sp, cy * cp};
}

// hold a facing for `ms`, with a sub-degree sway so the head is never dead-still
static void swayHold(CRemoteClient* remote, float yaw, float pitch, int ms) {
    remote->animate(
        [yaw, pitch, ms](MonadoWire::r_remote_data& d, float t01) {
            const float t              = t01 * static_cast<float>(ms) * 1e-3f;
            const float sway           = 0.6f * static_cast<float>(M_PI) / 180.f * std::sin(t * 2.f * static_cast<float>(M_PI) * 0.7f);
            d.head.center.orientation  = yawPitch(yaw + sway, pitch + 0.3f * sway);
            d.head.center.position     = MonadoWire::xrt_vec3{0.f, 0.f, 0.f};
            d.head.per_view_data_valid = false;
        },
        std::chrono::milliseconds(ms), 60);
}

// the take-off: pitch up and back through vertical, the motion that used to mirror the memory
static void liftAndFlip(CRemoteClient* remote) {
    remote->animate(
        [](MonadoWire::r_remote_data& d, float t01) {
            d.head.center.orientation  = yawPitch(0.f, t01 * 110.f * static_cast<float>(M_PI) / 180.f);
            d.head.center.position     = MonadoWire::xrt_vec3{0.f, 0.2f * t01, 0.f};
            d.head.per_view_data_valid = false;
        },
        std::chrono::milliseconds(500), 60);
    remote->animate(
        [](MonadoWire::r_remote_data& d, float) {
            d.head.center.orientation  = yawPitch(0.f, 110.f * static_cast<float>(M_PI) / 180.f);
            d.head.per_view_data_valid = false;
        },
        std::chrono::milliseconds(400), 60);
}

struct SDonOffGuard {
    const bool& failed;
    std::string testName;
    std::string monitorName;
    ~SDonOffGuard() {
        getFromSocket("/keyword openxr:placement_memory legacy");
        getFromSocket("/openxr enable");
        XR::waitForXrState("focused", std::chrono::milliseconds(15000));
        if (!monitorName.empty())
            getFromSocket("/openxr destroy " + monitorName);
        getFromSocket("/keyword openxr:monitors_follow_session visible");
        if (XR::g_ctx.remote) {
            XR::g_ctx.remote->setHeadPose(MonadoWire::xrt_vec3{0.f, 0.f, 0.f}, MonadoWire::xrt_quat{0.f, 0.f, 0.f, 1.f});
            XR::g_ctx.remote->setControllerActive(CRemoteClient::SIDE_LEFT, false);
            XR::g_ctx.remote->pulse();
        }
        if (failed)
            XR::dumpXrArtifacts(testName);
    }
};

// create an ad-hoc monitor straight ahead (yaw 0) and wait for it to exist
static bool createAhead(CRemoteClient* remote, const std::string& mon) {
    remote->setHeadPose(MonadoWire::xrt_vec3{0.f, 0.f, 0.f}, MonadoWire::xrt_quat{0.f, 0.f, 0.f, 1.f});
    remote->setControllerActive(CRemoteClient::SIDE_LEFT, false);
    remote->pulse();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    if (getFromSocket("/openxr create " + mon + " 1280x720") != "ok")
        return false;
    return XR::waitForJson("j/openxr", [&](const std::string& r) { return r.contains("\"name\": \"" + mon + "\""); }, std::chrono::milliseconds(10000));
}

// A dwell facing the monitor, the lift-and-flip take-off, then the session ends. Returns the
// status JSON read after the session is down.
static std::string dwellThenFlipThenEnd(CRemoteClient* remote, const std::string& mon) {
    // No previous memory: the legacy drift gate would otherwise refuse a flipped capture by
    // comparing it with the create-time seed, and this would not test the upright guard at all.
    // A FIRST commit only ever had the 8 m reach bound (R4), which is how the flip got in.
    getFromSocket("/openxr forget " + mon);
    swayHold(remote, 0.f, 0.f, 10500);
    liftAndFlip(remote);
    getFromSocket("/openxr disable");
    XR::waitForXrState("disabled", std::chrono::milliseconds(10000));
    return getFromSocket("j/openxr");
}

// The 09:39:06 regression, under the shipped `legacy` memory: the staging gate now refuses a head
// pitched past upright, so the take-off can no longer mirror the memory; and the shadow selection
// ran and chose the dwell.
TEST_CASE(xr_doff_legacy_upright_and_shadow) {
    XR_SKIP_IF_UNAVAILABLE();
    const std::string mon = XR::monitorName(31);
    SDonOffGuard      guard{this->failed, name(), ""};
    if (!gateUp()) {
        XR::logSkip(name(), "session never reached focused/visible (known env instability)");
        return;
    }
    auto* remote = XR::g_ctx.remote;
    if (!remote) {
        XR::logSkip(name(), "no remote client available");
        return;
    }
    ASSERT(getFromSocket("/keyword openxr:monitors_follow_session session"), std::string("ok"));
    ASSERT(getFromSocket("/keyword openxr:placement_memory legacy"), std::string("ok"));
    ASSERT(createAhead(remote, mon), true);
    guard.monitorName = mon;

    const std::string st = dwellThenFlipThenEnd(remote, mon);
    const auto        p  = XR::findAfter(st, "\"name\": \"" + mon + "\"");
    ASSERT_NOT(p, std::string::npos);
    const auto off = restoreOffsetOf(st, p);
    ASSERT(off.size(), (size_t)3);
    NLog::log("xr_doff_legacy_upright_and_shadow: remembered offset [{:.3f}, {:.3f}, {:.3f}], shadow seat {}", off[0], off[1], off[2], XR::fieldAfter(st, 0, "seatSource"));
    // in front of the seat, not mirrored behind it
    EXPECT(off[2] < -0.3f, true);
    EXPECT(std::fabs(off[0]) < 0.3f, true);
    // the shadow selection ran at the session-end edge and found the dwell
    EXPECT(XR::fieldAfter(st, 0, "shadow"), std::string("true"));
    EXPECT(XR::fieldAfter(st, 0, "seatSource"), std::string("dwell"));
    NLog::green("xr_doff_legacy_upright_and_shadow: no flip under legacy; shadow chose the dwell");
}

// Under placement_memory = intent the memory IS the dwell, with dwell provenance.
TEST_CASE(xr_doff_intent_commits_dwell) {
    XR_SKIP_IF_UNAVAILABLE();
    const std::string mon = XR::monitorName(32);
    SDonOffGuard      guard{this->failed, name(), ""};
    if (!gateUp()) {
        XR::logSkip(name(), "session never reached focused/visible (known env instability)");
        return;
    }
    auto* remote = XR::g_ctx.remote;
    if (!remote) {
        XR::logSkip(name(), "no remote client available");
        return;
    }
    ASSERT(getFromSocket("/keyword openxr:monitors_follow_session session"), std::string("ok"));
    ASSERT(getFromSocket("/keyword openxr:placement_memory intent"), std::string("ok"));
    ASSERT(createAhead(remote, mon), true);
    guard.monitorName = mon;

    const std::string st = dwellThenFlipThenEnd(remote, mon);
    const auto        p  = XR::findAfter(st, "\"name\": \"" + mon + "\"");
    ASSERT_NOT(p, std::string::npos);
    const auto off = restoreOffsetOf(st, p);
    ASSERT(off.size(), (size_t)3);
    NLog::log("xr_doff_intent_commits_dwell: remembered offset [{:.3f}, {:.3f}, {:.3f}] ({})", off[0], off[1], off[2], XR::fieldAfter(st, p, "source"));
    EXPECT(off[2] < -0.3f, true);
    EXPECT(std::fabs(off[0]) < 0.3f, true);
    EXPECT(XR::fieldAfter(st, p, "source"), std::string("dwell"));
    EXPECT(XR::fieldAfter(st, 0, "shadow"), std::string("false"));
    EXPECT(XR::fieldAfter(st, 0, "seatSource"), std::string("dwell"));
    NLog::green("xr_doff_intent_commits_dwell: the dwell became the memory");
}

// research/30 §7 / D4: a deliberate re-seat writes the memory immediately (one keypress repairs a bad
// memory), and `forget` drops it.
TEST_CASE(xr_reseat_commits_memory_and_forget) {
    XR_SKIP_IF_UNAVAILABLE();
    const std::string mon = XR::monitorName(33);
    SDonOffGuard      guard{this->failed, name(), ""};
    if (!gateUp()) {
        XR::logSkip(name(), "session never reached focused/visible (known env instability)");
        return;
    }
    auto* remote = XR::g_ctx.remote;
    if (!remote) {
        XR::logSkip(name(), "no remote client available");
        return;
    }
    ASSERT(getFromSocket("/keyword openxr:monitors_follow_session session"), std::string("ok"));
    ASSERT(createAhead(remote, mon), true);
    guard.monitorName = mon;
    {
        const std::string st = getFromSocket("j/openxr");
        const auto        p  = XR::findAfter(st, "\"name\": \"" + mon + "\"");
        ASSERT_NOT(p, std::string::npos);
        EXPECT(XR::fieldAfter(st, p, "source"), std::string("created"));
    }

    // turn around: the monitor is now behind the user; the re-seat brings it round and that
    // placement becomes the memory at once — no doff, no gate
    swayHold(remote, static_cast<float>(M_PI), 0.f, 600);
    const auto r = getFromSocket("/openxr reseat");
    EXPECT(r.starts_with("re-seated"), true);
    swayHold(remote, static_cast<float>(M_PI), 0.f, 600);
    {
        const std::string st = getFromSocket("j/openxr");
        const auto        p  = XR::findAfter(st, "\"name\": \"" + mon + "\"");
        ASSERT_NOT(p, std::string::npos);
        const auto off = restoreOffsetOf(st, p);
        ASSERT(off.size(), (size_t)3);
        NLog::log("xr_reseat_commits_memory_and_forget: after re-seat memory [{:.3f}, {:.3f}, {:.3f}] ({})", off[0], off[1], off[2], XR::fieldAfter(st, p, "source"));
        EXPECT(XR::fieldAfter(st, p, "source"), std::string("reseat"));
        EXPECT(off[2] < -0.3f, true);
        EXPECT(std::fabs(off[0]) < 0.3f, true);
    }

    EXPECT(getFromSocket("/openxr forget " + mon).starts_with("forgot 1"), true);
    {
        const std::string st = getFromSocket("j/openxr");
        const auto        p  = XR::findAfter(st, "\"name\": \"" + mon + "\"");
        ASSERT_NOT(p, std::string::npos);
        EXPECT(XR::fieldAfter(st, p, "restorable"), std::string("false"));
        EXPECT(XR::fieldAfter(st, p, "source"), std::string("none"));
    }
    EXPECT(getFromSocket("/openxr forget no-such-monitor").starts_with("forget: no XR monitor"), true);
    // the diagnostic dump answers
    EXPECT(getFromSocket("/openxr donoff").starts_with("don/doff: "), true);
    NLog::green("xr_reseat_commits_memory_and_forget: re-seat committed immediately; forget dropped it");
}

#endif
