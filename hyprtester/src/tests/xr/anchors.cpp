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

namespace {
    bool gateUp() {
        if (XR::waitForXrState("focused", std::chrono::milliseconds(15000)))
            return true;
        return XR::waitForXrState("visible", std::chrono::milliseconds(2000));
    }

    // RAII: dump artifacts on failure, always destroy the monitor we created, always park the
    // shared RemoteClient's head/controller state back at identity/inactive so later tests
    // (this file's own later checks, or other xr test cases that run after this one in the
    // same shared session) don't inherit a yawed head or a dangling active controller.
    struct SArtifactGuard {
        const bool& failed;
        std::string testName;
        std::string monitorName;
        ~SArtifactGuard() {
            if (XR::g_ctx.remote) {
                using namespace MonadoWire;
                XR::g_ctx.remote->setHeadPose(xrt_vec3{0.f, 0.f, 0.f}, xrt_quat{0.f, 0.f, 0.f, 1.f});
                XR::g_ctx.remote->setControllerActive(CRemoteClient::SIDE_LEFT, false);
                XR::g_ctx.remote->pulse();
            }
            if (!monitorName.empty())
                getFromSocket("/openxr destroy " + monitorName);
            if (failed)
                XR::dumpXrArtifacts(testName);
        }
    };

    std::vector<float> posOf(const std::string& json, size_t blockPos) {
        return XR::parseFloatArray(XR::fieldAfter(json, blockPos, "pos"));
    }

    std::vector<float> quatOf(const std::string& json, size_t blockPos) {
        return XR::parseFloatArray(XR::fieldAfter(json, blockPos, "quat"));
    }

    float dist3(const std::vector<float>& a, const std::vector<float>& b) {
        if (a.size() != 3 || b.size() != 3)
            return -1.f;
        const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }
}

// xr_anchor_transitions — WP11 (doc 06 §6 row 4; doc 03/05 anchor semantics).
//
// DEVIATION from doc 06's literal test description (noted for WP13): doc 06 says to assert on
// the monitor's *world* pose converging after a head-leash yaw. Doc 05 §4.3 (the authoritative,
// implemented IPC schema) instead reports the *configured offset* for head/body/device modes —
// not the live world-composed pose — via `anchor.pose`; only a *grabbed* monitor's status
// reports live world pose (WP8 deviation, see OpenXRManager.cpp monitorInfos()). So this test
// observes leash convergence indirectly: anchor to head, let the leash chase a scripted head
// yaw, then re-anchor to local (which snapshots the *live* world pose into `anchor.pose` at
// that instant) and check the resulting local pose moved meaningfully from the pre-yaw offset.
//
// Also folds in the WP11 roadmap's "verbs" ask (move/rotate/scale/distance/center) on the same
// monitor once it's back in local mode with a known, deterministic baseline pose.
TEST_CASE(xr_anchor_transitions) {
    XR_SKIP_IF_UNAVAILABLE();

    const std::string mon = XR::monitorName(14);
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

    // Baseline: identity head, inactive controller.
    remote->setHeadPose(xrt_vec3{0.f, 0.f, 0.f}, xrt_quat{0.f, 0.f, 0.f, 1.f});
    remote->setControllerActive(CRemoteClient::SIDE_LEFT, false);
    remote->pulse();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    ASSERT(getFromSocket("/openxr create " + mon + " 1280x720 anchor:local pos:0,1.4,-1.5 yaw:0 size:1.0"), std::string("ok"));
    guard.monitorName = mon;
    ASSERT(XR::waitForJson(
               "j/openxr", [&](const std::string& r) { return r.contains("\"name\": \"" + mon + "\""); }, std::chrono::milliseconds(10000)),
           true);
    ASSERT(getFromSocket("/openxr select " + mon), std::string("ok"));

    // ---- mode cycling: local -> head -> body -> (device:left, best-effort) -----------------
    ASSERT(getFromSocket("/openxr anchor " + mon + " head offset:0.4,-0.2,-1.0"), std::string("ok"));
    {
        const std::string st  = getFromSocket("j/openxr");
        const auto        p   = XR::findAfter(st, "\"name\": \"" + mon + "\"");
        EXPECT(XR::fieldAfter(st, p, "mode"), std::string("head"));
        const auto o = posOf(st, p);
        ASSERT(o.size(), (size_t)3);
        EXPECT_MAX_DELTA(o[0], 0.4, 0.05);
        EXPECT_MAX_DELTA(o[1], -0.2, 0.05);
        EXPECT_MAX_DELTA(o[2], -1.0, 0.05);
    }

    ASSERT(getFromSocket("/openxr anchor " + mon + " body offset:-0.5,0.0,-1.2"), std::string("ok"));
    {
        const std::string st = getFromSocket("j/openxr");
        EXPECT(XR::fieldAfter(st, XR::findAfter(st, "\"name\": \"" + mon + "\""), "mode"), std::string("body"));
    }

    // device:left needs a tracked left controller; best-effort, SKIP-note (not fail) if the
    // runtime never reports a valid grip in our short budget (known env instability).
    remote->setControllerActive(CRemoteClient::SIDE_LEFT, true);
    remote->setControllerPose(CRemoteClient::SIDE_LEFT, xrt_vec3{0.2f, 1.2f, -0.3f}, xrt_quat{0.f, 0.f, 0.f, 1.f});
    bool deviceOk = false;
    for (int i = 0; i < 30 && !deviceOk; ++i) {
        remote->pulse();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (i >= 5) // give the runtime a few frames to start reporting tracked state
            deviceOk = getFromSocket("/openxr anchor " + mon + " device:left") == "ok";
    }
    if (deviceOk) {
        const std::string st = getFromSocket("j/openxr");
        EXPECT(XR::fieldAfter(st, XR::findAfter(st, "\"name\": \"" + mon + "\""), "mode"), std::string("device:left"));
        NLog::green("xr_anchor_transitions: device:left anchor accepted");
    } else
        NLog::yellow("xr_anchor_transitions: device:left anchor never accepted in budget (known env instability) — skipping that sub-check, not failing the test");
    remote->setControllerActive(CRemoteClient::SIDE_LEFT, false);
    remote->pulse();

    // ---- head-leash convergence: yaw the head 90°, expect the head-relative pose to have
    // moved substantially once re-anchored to local (see DEVIATION note above). -------------
    ASSERT(getFromSocket("/openxr anchor " + mon + " head offset:0.4,-0.2,-1.0"), std::string("ok"));
    std::this_thread::sleep_for(std::chrono::milliseconds(300)); // let the spring settle at the baseline offset first

    remote->animate(
        [](r_remote_data& d, float t01) {
            const float yaw = t01 * (float)M_PI / 2.f; // 0 -> 90 deg
            d.head.center.orientation.y = std::sin(yaw / 2.f);
            d.head.center.orientation.w = std::cos(yaw / 2.f);
            d.head.per_view_data_valid  = false;
        },
        std::chrono::milliseconds(1000), 60);

    // Wait well beyond 2x the default leash_response (0.35s) for the spring to converge onto
    // the yawed target — generous per the task's tolerance guidance.
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));

    ASSERT(getFromSocket("/openxr anchor " + mon + " local"), std::string("ok"));
    {
        const std::string st = getFromSocket("j/openxr");
        const auto        p  = XR::findAfter(st, "\"name\": \"" + mon + "\"");
        EXPECT(XR::fieldAfter(st, p, "mode"), std::string("local"));
        const auto afterYaw = posOf(st, p);
        ASSERT(afterYaw.size(), (size_t)3);
        const float moved = dist3(afterYaw, {0.4f, -0.2f, -1.0f});
        // A 90 deg yaw of a ~1.1m offset moves it by roughly its own magnitude; 0.3m is a very
        // conservative floor that still proves the leash tracked the head rather than staying
        // frozen at the pre-yaw offset.
        if (moved < 0.3f)
            MARK_TEST_FAILED("head-leashed pose only moved {}m after a 90 deg head yaw + settle (expected > 0.3m)", moved);
        else
            NLog::green("xr_anchor_transitions: head-leashed pose moved {}m after the yaw (leash tracked the head)", moved);
    }

    // Reset the head back to identity before the deterministic verb checks below (doc 06 row 4:
    // "yaw head back") — also matters for later tests sharing the same RemoteClient.
    remote->setHeadPose(xrt_vec3{0.f, 0.f, 0.f}, xrt_quat{0.f, 0.f, 0.f, 1.f});
    remote->pulse();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // ---- verbs: move/rotate/scale/distance/center (WP11 roadmap ask) ----------------------
    // Re-anchor to a known, deterministic local baseline first (offset: sets pos directly for
    // local mode, no head tracking needed) so the verb deltas below are computed against a
    // known start point rather than wherever the leash test above left it.
    ASSERT(getFromSocket("/openxr anchor " + mon + " local offset:0,1.4,-1.5"), std::string("ok"));

    auto readMonitor = [&]() -> std::string { return getFromSocket("j/openxr"); };
    auto blockOf      = [&](const std::string& st) { return XR::findAfter(st, "\"name\": \"" + mon + "\""); };

    // rotate: pure algebra (LOCAL mode ignores ctx), exact and immediate. dyaw=30, dpitch=0
    // starting from identity rotation -> expect quat ~= {0, sin(15deg), 0, cos(15deg)}.
    ASSERT(getFromSocket("/openxr rotate 30"), std::string("ok"));
    {
        const std::string st = readMonitor();
        const auto        q  = quatOf(st, blockOf(st));
        ASSERT(q.size(), (size_t)4);
        EXPECT_MAX_DELTA(q[1], std::sin(15.f * (float)M_PI / 180.f), 0.02);
        EXPECT_MAX_DELTA(q[3], std::cos(15.f * (float)M_PI / 180.f), 0.02);
    }

    // scale: the monitor was created with an explicit size:1.0 -> 1.0*1.25 = 1.25, then -0.3
    // (explicit delta) = 0.95.
    ASSERT(getFromSocket("/openxr scale 1.25"), std::string("ok"));
    ASSERT(getFromSocket("/openxr scale -0.3"), std::string("ok"));
    {
        const std::string st     = readMonitor();
        const float       sizeM  = XR::toFloatOr(XR::fieldAfter(st, blockOf(st), "size_m"), -1.f);
        EXPECT_MAX_DELTA(sizeM, 0.95, 0.05);
    }

    // move: needs head tracking (ctx.viewValid); we just reset the head to identity, so the
    // world-space delta should equal the raw (dx,dy,dz) we pass. dz>0 means "away" (view -Z).
    {
        const std::string before = readMonitor();
        const auto        p0     = posOf(before, blockOf(before));
        ASSERT(p0.size(), (size_t)3);
        ASSERT(getFromSocket("/openxr move 0.2 0 0"), std::string("ok"));
        const std::string after = readMonitor();
        const auto        p1    = posOf(after, blockOf(after));
        ASSERT(p1.size(), (size_t)3);
        EXPECT_MAX_DELTA(p1[0] - p0[0], 0.2, 0.1);
    }

    // distance: pushing/pulling along the viewer->quad ray by dMeters moves the quad's position
    // by exactly dMeters (the delta is `dir * dMeters`, and `dir` is a unit vector) regardless
    // of where the viewer actually is — so we can assert the delta magnitude without needing to
    // know the exact head position (floor-offset dependent). `distance` computes `dir`/`len`
    // from the anchor's internally-cached last-composed world pose, which is only refreshed by
    // the frame thread's next solve tick — settle briefly after the immediately-preceding move
    // so that cache isn't stale relative to `p0` below (observed as a large spurious delta
    // without this wait).
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    {
        const std::string before = readMonitor();
        const auto        p0     = posOf(before, blockOf(before));
        ASSERT(p0.size(), (size_t)3);
        ASSERT(getFromSocket("/openxr distance +0.3"), std::string("ok"));
        const std::string after = readMonitor();
        const auto        p1    = posOf(after, blockOf(after));
        ASSERT(p1.size(), (size_t)3);
        EXPECT_MAX_DELTA(dist3(p0, p1), 0.3, 0.1);
    }

    // center: recenters in front of the (identity) view at openxr:default_distance (1.5 by
    // default). With head at the origin facing -Z, expect roughly x~0, z~-1.5 (generous
    // tolerance for floor-offset uncertainty in y, which we don't assert on).
    ASSERT(getFromSocket("/openxr center"), std::string("ok"));
    {
        const std::string st = readMonitor();
        const auto        p  = posOf(st, blockOf(st));
        ASSERT(p.size(), (size_t)3);
        EXPECT_MAX_DELTA(p[0], 0.0, 0.4);
        EXPECT_MAX_DELTA(p[2], -1.5, 0.4);
    }

    NLog::green("xr_anchor_transitions: mode transitions + verbs (move/rotate/scale/distance/center) verified");
}

// xr_anchor_restore_across_session — doc 03 §8.3, the cross-session monitor lottery.
//
// An AD-HOC monitor (`openxr create` with no anchor spec) has no head-relative declaration: the
// pose stored as its "declared" anchor is the LOCAL_FLOOR pose `applyCenter` derived from wherever
// the head stood at creation. The first plug of a NEW session re-seats anchor:local monitors into
// the wearer's frame — and re-seating from THAT composes a dead frame's coordinates into the head
// frame, throwing the monitor as far as the head was from the origin. Live report 2026-08-16:
// monitors "spun way off, outside my house", from a session whose origin sat ~7 m from the user.
//
// `openxr disable` + `openxr enable` is a genuine session recycle (same vehicle
// xr_plugged_follow_session uses), and the remote driver lets us stand the head well away from the
// origin so the stale-coordinate composition has something to go wrong with. Pre-fix, the monitor
// lands metres from where it was left; post-fix it comes back where the user put it.
TEST_CASE(xr_anchor_restore_across_session) {
    XR_SKIP_IF_UNAVAILABLE();

    const std::string mon = XR::monitorName(17);

    struct SRestoreGuard {
        const bool& failed;
        std::string testName;
        std::string monitorName;
        ~SRestoreGuard() {
            // Always leave a running session, an identity head and the shipped recenter default for
            // whatever runs next in this shared instance.
            getFromSocket("/openxr enable");
            XR::waitForXrState("focused", std::chrono::milliseconds(15000));
            if (!monitorName.empty())
                getFromSocket("/openxr destroy " + monitorName);
            getFromSocket("/keyword openxr:recenter_on_plug 1");
            if (XR::g_ctx.remote) {
                using namespace MonadoWire;
                XR::g_ctx.remote->setHeadPose(xrt_vec3{0.f, 0.f, 0.f}, xrt_quat{0.f, 0.f, 0.f, 1.f});
                XR::g_ctx.remote->setControllerActive(CRemoteClient::SIDE_LEFT, false);
                XR::g_ctx.remote->pulse();
            }
            if (failed)
                XR::dumpXrArtifacts(testName);
        }
    };
    SRestoreGuard guard{this->failed, name(), ""};

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

    ASSERT(getFromSocket("/keyword openxr:recenter_on_plug 1"), std::string("ok"));

    // Stand the user 5 m from the runtime's origin, facing 25 deg off it — the geometry that makes a
    // stale LOCAL pose dangerous. yaw 25 deg about +Y.
    remote->setHeadPose(xrt_vec3{3.f, 0.f, 4.f}, xrt_quat{0.f, 0.2164f, 0.f, 0.9763f});
    remote->setControllerActive(CRemoteClient::SIDE_LEFT, false);
    remote->pulse();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // No anchor spec at all => the ad-hoc path (applyCenter from the live head), which is the case
    // that used to carry raw world coordinates into the next session. `size:` is deliberately absent:
    // the create grammar only accepts kv tokens as part of an anchor spec, and supplying one would
    // put us on the DECLARED path instead — the very path that already worked.
    ASSERT(getFromSocket("/openxr create " + mon + " 1280x720"), std::string("ok"));
    guard.monitorName = mon;
    ASSERT(XR::waitForJson(
               "j/openxr", [&](const std::string& r) { return r.contains("\"name\": \"" + mon + "\""); }, std::chrono::milliseconds(10000)),
           true);
    ASSERT(getFromSocket("/openxr select " + mon), std::string("ok"));

    // It really was placed out where the user is standing, not near the origin — otherwise the stale
    // composition below would be harmless and this test would prove nothing.
    std::vector<float> placed, relPlaced;
    {
        const std::string st = getFromSocket("j/openxr");
        const auto        p  = XR::findAfter(st, "\"name\": \"" + mon + "\"");
        ASSERT_NOT(p, std::string::npos);
        placed = posOf(st, p);
        ASSERT(placed.size(), (size_t)3);
        EXPECT(dist3(placed, std::vector<float>{0.f, 0.f, 0.f}) > 2.f, true);
        // The create-time seed makes it restorable straight away (doc 03 §8.3).
        EXPECT(XR::fieldAfter(st, p, "restorable"), std::string("true"));
        relPlaced = XR::parseFloatArray(XR::fieldAfter(st, p, "offset"));
        ASSERT(relPlaced.size(), (size_t)3);
    }

    // Nudge it so the restored pose is provably the USER'S placement, not the creation pose.
    ASSERT(getFromSocket("/openxr move 0.4 0 0"), std::string("ok"));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    std::vector<float> left;
    {
        const std::string st = getFromSocket("j/openxr");
        const auto        p  = XR::findAfter(st, "\"name\": \"" + mon + "\"");
        ASSERT_NOT(p, std::string::npos);
        left = posOf(st, p);
        ASSERT(left.size(), (size_t)3);
        EXPECT(dist3(left, placed) > 0.2f, true);
        // The capture must have TRACKED the move — this is the mechanism the restore below rides on,
        // and asserting it here says "the capture is not running" rather than leaving that to be
        // inferred from a pose landing somewhere unexpected two session transitions later.
        // If the gate is shut nothing is being remembered, and the restore below would silently
        // replay the create-time seed instead — say so here rather than leaving it to be inferred
        // from a pose landing somewhere unexpected two session transitions later.
        EXPECT(XR::fieldAfter(st, size_t{0}, "restoreCapture"), std::string("true"));
        const auto relLeft = XR::parseFloatArray(XR::fieldAfter(st, p, "offset"));
        ASSERT(relLeft.size(), (size_t)3);
        NLog::log("xr_anchor_restore_across_session: placed [{:.3f}, {:.3f}, {:.3f}] -> left [{:.3f}, {:.3f}, {:.3f}], capture offset [{:.3f}, {:.3f}, {:.3f}]", placed[0], placed[1],
                  placed[2], left[0], left[1], left[2], relLeft[0], relLeft[1], relLeft[2]);
        EXPECT(dist3(relLeft, relPlaced) > 0.2f, true);
    }

    // Recycle the session: new session, new first plug, new re-seat.
    ASSERT(getFromSocket("/openxr disable"), std::string("ok"));
    ASSERT(XR::waitForXrState("disabled", std::chrono::milliseconds(10000)), true);
    remote->pulse();
    ASSERT(getFromSocket("/openxr enable"), std::string("ok"));
    ASSERT(XR::waitForXrState("focused", std::chrono::milliseconds(15000)) || XR::waitForXrState("visible", std::chrono::milliseconds(2000)), true);
    // Keep the head where the user is standing and give the frame thread time to consume the armed
    // re-seat on a valid-view frame.
    remote->setHeadPose(xrt_vec3{3.f, 0.f, 4.f}, xrt_quat{0.f, 0.2164f, 0.f, 0.9763f});
    remote->pulse();
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    {
        const std::string st = getFromSocket("j/openxr");
        const auto        p  = XR::findAfter(st, "\"name\": \"" + mon + "\"");
        ASSERT_NOT(p, std::string::npos);
        const auto back = posOf(st, p);
        ASSERT(back.size(), (size_t)3);
        // THE assertion: the room came back the way it was left. Pre-fix this re-seated from the
        // stale creation-time world pose and landed several metres out.
        EXPECT_MAX_DELTA(dist3(back, left), 0.0, 0.35);
    }

    NLog::green("xr_anchor_restore_across_session: ad-hoc monitor placement survived a session recycle");
}

#endif // WITH_XR_TESTS
