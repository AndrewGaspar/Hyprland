#include "tests.hpp"

#ifdef WITH_XR_TESTS

#include "../../hyprctlCompat.hpp"
#include "../../Log.hpp"
#include "../../xr/xr_helpers.hpp"
#include "../../xr/RemoteClient.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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
            getFromSocket("/keyword openxr:monitors_follow_session visible");
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
    // Pin the plug policy the way xr_plugged_follow_session does. Under the shipped `visible` default
    // on a runtime without XR_EXT_user_presence, the FIRST plug of a session is deferred behind
    // openxr:monitor_plug_settle_ms of sustained visibility — and the re-seat is armed by that plug,
    // so the whole thing under test would land a second and a half after a fixed wait. `session`
    // plugs on session existence, which makes the edge prompt and the test about anchoring rather
    // than about the blip guard. The capture gate keys on visibility, not on plugging (doc 03 §8.3),
    // so this does not change what is being exercised.
    ASSERT(getFromSocket("/keyword openxr:monitors_follow_session session"), std::string("ok"));

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
    std::vector<float> left, relLeft;
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
        relLeft = XR::parseFloatArray(XR::fieldAfter(st, p, "offset"));
        ASSERT(relLeft.size(), (size_t)3);
        NLog::log("xr_anchor_restore_across_session: placed [{:.3f}, {:.3f}, {:.3f}] -> left [{:.3f}, {:.3f}, {:.3f}], capture offset [{:.3f}, {:.3f}, {:.3f}]", placed[0], placed[1],
                  placed[2], left[0], left[1], left[2], relLeft[0], relLeft[1], relLeft[2]);
        EXPECT(dist3(relLeft, relPlaced) > 0.2f, true);
    }

    // Recycle the session: new session, new first plug, new re-seat.
    ASSERT(getFromSocket("/openxr disable"), std::string("ok"));
    ASSERT(XR::waitForXrState("disabled", std::chrono::milliseconds(10000)), true);

    // Come back somewhere ELSE, facing another way — 5 m and 85 deg from where the session died —
    // and move there WHILE THE SESSION IS DOWN, the way a person actually does it. Two reasons this
    // has to happen before the enable: it is the realistic sequence, and the re-seat fires on the
    // first plug of the new session, which lands the moment that session becomes visible. Moving the
    // head afterwards would measure a LOCAL anchor that has already been correctly seated to the old
    // spot — indistinguishable from a compositor that had quietly stopped re-seating anything.
    constexpr float NEW_YAW = -60.f * (float)M_PI / 180.f;
    const xrt_vec3  newHead{0.5f, 0.f, -2.f};
    const xrt_quat  newRot{0.f, std::sin(NEW_YAW / 2.f), 0.f, std::cos(NEW_YAW / 2.f)};
    remote->setHeadPose(newHead, newRot);
    remote->pulse();

    ASSERT(getFromSocket("/openxr enable"), std::string("ok"));
    ASSERT(XR::waitForXrState("focused", std::chrono::milliseconds(15000)) || XR::waitForXrState("visible", std::chrono::milliseconds(2000)), true);
    // Hold the user there, then wait for the PLUG rather than a wall-clock guess: the plug edge is
    // what arms the re-seat, so anything sooner reads a monitor that has not been re-seated yet and
    // anything later is dead time.
    remote->setHeadPose(newHead, newRot);
    remote->pulse();
    ASSERT(XR::waitForJson(
               "j/openxr",
               [&](const std::string& r) {
                   const auto q = XR::findAfter(r, "\"name\": \"" + mon + "\"");
                   return q != std::string::npos && XR::fieldAfter(r, q, "plugged") == "true";
               },
               std::chrono::milliseconds(20000)),
           true);
    // ...then a beat for the frame thread to consume the arming on its next valid-view frame.
    remote->pulse();
    std::this_thread::sleep_for(std::chrono::milliseconds(800));

    {
        const std::string st = getFromSocket("j/openxr");
        const auto        p  = XR::findAfter(st, "\"name\": \"" + mon + "\"");
        ASSERT_NOT(p, std::string::npos);
        const auto back = posOf(st, p);
        ASSERT(back.size(), (size_t)3);

        // Where the captured offset SHOULD land in the new head's yaw-only floor frame — the same
        // composition recenterLocalToHead performs, written out longhand so this test would notice the
        // engine and the doc disagreeing rather than following it into the same mistake.
        const float c = std::cos(NEW_YAW), sn = std::sin(NEW_YAW);
        const std::vector<float> expected{newHead.x + relLeft[0] * c + relLeft[2] * sn, relLeft[1], newHead.z - relLeft[0] * sn + relLeft[2] * c};

        // THE assertion: the room came back the way it was left, rigidly re-seated to the user.
        // Pre-fix this re-seated from the stale creation-time world pose instead.
        EXPECT_MAX_DELTA(dist3(back, expected), 0.0, 0.1);
        // ...and it really did move with them, so the re-seat demonstrably ran.
        EXPECT(dist3(back, left) > 1.f, true);
        NLog::log("xr_anchor_restore_across_session: came back at [{:.3f}, {:.3f}, {:.3f}], expected [{:.3f}, {:.3f}, {:.3f}]", back[0], back[1], back[2], expected[0], expected[1],
                  expected[2]);
    }

    NLog::green("xr_anchor_restore_across_session: ad-hoc monitor placement followed the user across a session recycle");
}

// xr_reseat_verb — doc 03 §8.4, the deliberate "bring my monitors to me".
//
// The §8.1 ladder holds monitors still in the room across a recenter, on purpose. Reported live
// 2026-08-17: the Quest's recenter pressed repeatedly, the monitors correctly staying put, and
// physically turning 180 degrees before recentering as the only workaround. `xrmonitor reseat` is
// the missing sentence.
//
// This test is where the design's real hazard is caught. The obvious implementation — replant each
// monitor's stored head-relative offset, exactly what the first plug does — is a NO-OP while the
// headset is worn, because the §8.3 capture has been re-deriving those offsets against this very
// head every frame. So the assertion that matters is not "the verb returned ok": it is that the
// monitors ACTUALLY MOVED, to the place a rigid transform onto the new facing puts them, with their
// relative layout intact. A regression to the stored-offset form fails here and nowhere else.
TEST_CASE(xr_reseat_verb) {
    XR_SKIP_IF_UNAVAILABLE();

    const std::string monL = XR::monitorName(50);
    const std::string monR = XR::monitorName(51);

    struct SReseatGuard {
        const bool& failed;
        std::string testName;
        std::string a, b;
        ~SReseatGuard() {
            if (!a.empty())
                getFromSocket("/openxr destroy " + a);
            if (!b.empty())
                getFromSocket("/openxr destroy " + b);
            getFromSocket("/keyword openxr:recenter hold");
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
    SReseatGuard guard{this->failed, name(), "", ""};

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

    // The user sits at the origin looking down -Z, with a two-monitor wall 1.5 m in front. Declared
    // poses (not ad-hoc) so the starting geometry is exact and this test is about the re-seat rather
    // than about where `create` happened to drop things.
    remote->setHeadPose(xrt_vec3{0.f, 0.f, 0.f}, xrt_quat{0.f, 0.f, 0.f, 1.f});
    remote->setControllerActive(CRemoteClient::SIDE_LEFT, false);
    remote->pulse();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    ASSERT(getFromSocket("/openxr create " + monL + " 1280x720 anchor:local pos:-0.6,1.4,-1.5 yaw:0 size:1.0"), std::string("ok"));
    guard.a = monL;
    ASSERT(getFromSocket("/openxr create " + monR + " 1280x720 anchor:local pos:0.6,1.4,-1.5 yaw:0 size:1.0"), std::string("ok"));
    guard.b = monR;
    ASSERT(XR::waitForJson(
               "j/openxr", [&](const std::string& r) { return r.contains("\"name\": \"" + monL + "\"") && r.contains("\"name\": \"" + monR + "\""); },
               std::chrono::milliseconds(10000)),
           true);

    auto poseOf = [](const std::string& st, const std::string& mon) {
        const auto p = XR::findAfter(st, "\"name\": \"" + mon + "\"");
        return p == std::string::npos ? std::vector<float>{} : posOf(st, p);
    };

    // EVERY anchor:local monitor in the session, not just ours — the re-seat is a group operation and
    // the shared test session carries the config fixture XR-conf-a as well, so the group's derived
    // seat frame (and therefore the transform) depends on it. Reading the whole set is what lets the
    // expected landing spot below be computed exactly instead of guessed at.
    auto localGroup = [](const std::string& st) {
        std::vector<std::pair<std::vector<float>, std::vector<float>>> out;
        size_t                                                         p = 0;
        while ((p = st.find("\"name\": \"", p)) != std::string::npos) {
            const size_t block = p;
            p += 9;
            if (XR::fieldAfter(st, block, "mode") != "local")
                continue;
            const auto pos = posOf(st, block), quat = quatOf(st, block);
            if (pos.size() == 3 && quat.size() == 4)
                out.emplace_back(pos, quat);
        }
        return out;
    };

    std::vector<float>                                             beforeL, beforeR;
    std::vector<std::pair<std::vector<float>, std::vector<float>>> group;
    {
        const std::string st = getFromSocket("j/openxr");
        beforeL              = poseOf(st, monL);
        beforeR              = poseOf(st, monR);
        group                = localGroup(st);
        ASSERT(beforeL.size(), (size_t)3);
        ASSERT(beforeR.size(), (size_t)3);
        ASSERT(group.size() >= 2, true);
        // They really are where they were declared — otherwise the arithmetic below proves nothing.
        EXPECT_MAX_DELTA(beforeL[0], -0.6, 0.05);
        EXPECT_MAX_DELTA(beforeL[2], -1.5, 0.05);
        EXPECT_MAX_DELTA(beforeR[0], 0.6, 0.05);
    }

    // Swivel the chair: same spot, facing 75 degrees round. Everything the user owns is now off to
    // one side, and pressing the headset's recenter would (correctly, under the default `hold`) do
    // nothing about that.
    constexpr float YAW = 75.f * (float)M_PI / 180.f;
    remote->setHeadPose(xrt_vec3{0.f, 0.f, 0.f}, xrt_quat{0.f, std::sin(YAW / 2.f), 0.f, std::cos(YAW / 2.f)});
    remote->pulse();
    std::this_thread::sleep_for(std::chrono::milliseconds(400));

    // THE verb. Its reply is the user-facing sentence, so check it says what it did. The COUNT is
    // deliberately not pinned: the shared session's monitor population is not this test's business.
    const std::string reply = getFromSocket("/openxr reseat");
    ASSERT_STARTS_WITH(reply, std::string("re-seated "));
    ASSERT_CONTAINS(reply, std::string("to the current head"));
    NLog::log("xr_reseat_verb: reseat replied '{}'", reply);
    std::this_thread::sleep_for(std::chrono::milliseconds(600)); // the frame thread consumes on its next valid-view frame

    // Where doc 03 §8.4 says the group must land, re-derived here from the observed arrangement
    // rather than borrowed from the engine — the same discipline the restore test uses, so this
    // would notice the engine and the doc disagreeing instead of following one into the other's
    // mistake. Seat frame: the group's centroid pushed back along its mean facing normal by the
    // perpendicular viewing distance; the transform maps that frame onto the head's.
    float cx = 0.f, cz = 0.f, nx = 0.f, nz = 0.f;
    for (const auto& [pos, quat] : group) {
        cx += pos[0];
        cz += pos[2];
        // +Z column of the rotation matrix: the quad's normal, the direction its viewer sits in.
        const float qx = quat[0], qy = quat[1], qz = quat[2], qw = quat[3];
        const float ex = 2.f * (qx * qz + qw * qy), ez = 1.f - 2.f * (qx * qx + qy * qy);
        const float el = std::sqrt(ex * ex + ez * ez);
        if (el < 1e-4f)
            continue;
        nx += ex / el;
        nz += ez / el;
    }
    cx /= (float)group.size();
    cz /= (float)group.size();
    const float nl = std::sqrt(nx * nx + nz * nz);
    ASSERT(nl > 1e-3f, true); // a group with no common facing has no seat frame; this one has
    nx /= nl;
    nz /= nl;
    // The head stands at the origin, so `head - centroid` is just -centroid. Clamped 0.3..5 m.
    const float dist = std::clamp(std::fabs(-cx * nx + -cz * nz), 0.3f, 5.f);
    const float vx = cx + nx * dist, vz = cz + nz * dist, vYaw = std::atan2(nx, nz);
    // headFrame ∘ inv(seat): rotate about the seat frame's origin by (headYaw - seatYaw), then put
    // that origin at the head's floor position (the origin).
    const float th = YAW - vYaw, c = std::cos(th), s = std::sin(th);
    auto        expected = [&](const std::vector<float>& p) {
        const float px = p[0] - vx, pz = p[2] - vz;
        return std::vector<float>{px * c + pz * s, p[1], -px * s + pz * c};
    };

    std::vector<float> afterL, afterR;
    {
        const std::string st = getFromSocket("j/openxr");
        afterL               = poseOf(st, monL);
        afterR               = poseOf(st, monR);
        ASSERT(afterL.size(), (size_t)3);
        ASSERT(afterR.size(), (size_t)3);

        const auto wantL = expected(beforeL), wantR = expected(beforeR);
        NLog::log("xr_reseat_verb: seat frame [{:.3f}, {:.3f}] yaw {:.1f} deg; {} [{:.3f}, {:.3f}, {:.3f}] -> [{:.3f}, {:.3f}, {:.3f}], expected [{:.3f}, {:.3f}, {:.3f}]", vx, vz,
                  vYaw * 180.f / (float)M_PI, monL, beforeL[0], beforeL[1], beforeL[2], afterL[0], afterL[1], afterL[2], wantL[0], wantL[1], wantL[2]);
        EXPECT_MAX_DELTA(dist3(afterL, wantL), 0.0, 0.1);
        EXPECT_MAX_DELTA(dist3(afterR, wantR), 0.0, 0.1);

        // THE assertion the no-op regression fails: they moved, and by the metres a 75-degree swivel
        // implies, not by the millimetre of head jitter a stored-offset replant would give.
        EXPECT(dist3(afterL, beforeL) > 1.f, true);
        EXPECT(dist3(afterR, beforeR) > 1.f, true);
        // ...and it is the LAYOUT that arrived, not two monitors piled up: the separation is intact.
        EXPECT_MAX_DELTA(dist3(afterL, afterR), dist3(beforeL, beforeR), 0.05);
    }

    // Mash it: a second press from the same spot is the identity. (The seat frame is derived from
    // the arrangement, and after the first press the head IS that frame.)
    ASSERT_STARTS_WITH(getFromSocket("/openxr reseat"), std::string("re-seated "));
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    {
        const std::string st = getFromSocket("j/openxr");
        const auto        againL = poseOf(st, monL), againR = poseOf(st, monR);
        ASSERT(againL.size(), (size_t)3);
        ASSERT(againR.size(), (size_t)3);
        EXPECT_MAX_DELTA(dist3(againL, afterL), 0.0, 0.05);
        EXPECT_MAX_DELTA(dist3(againR, afterR), 0.0, 0.05);
    }

    // The policy the status line reports, and the one that makes the headset's own recenter do this.
    EXPECT_CONTAINS(getFromSocket("/openxr"), std::string("recenter policy: hold"));
    ASSERT(getFromSocket("/keyword openxr:recenter follow"), std::string("ok"));
    EXPECT_CONTAINS(getFromSocket("/openxr"), std::string("recenter policy: follow"));
    ASSERT(getFromSocket("/keyword openxr:recenter hold"), std::string("ok"));

    NLog::green("xr_reseat_verb: the deliberate re-seat brought the layout to the user's new facing, rigidly and idempotently");
}

#endif // WITH_XR_TESTS
