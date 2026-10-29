#include "tests.hpp"

#ifdef WITH_XR_TESTS

#include "../../hyprctlCompat.hpp"
#include "../../Log.hpp"
#include "../../xr/xr_helpers.hpp"
#include "../../xr/RemoteClient.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <format>
#include <functional>
#include <optional>
#include <string>
#include <thread>

// research/32 C0-C2 end to end, on the synthetic plane provider: grab a monitor with a scripted
// controller, lower it toward a declared table until the surfaces code previews the snap, let go,
// and check it now stands on the table, plane-relative, with the snap reported in status. The
// sibling proves the gate: with openxr:surfaces off the very same motion lands nothing.

struct SSurfaceGuard {
    const bool& failed;
    std::string testName;
    std::string monitorName;
    ~SSurfaceGuard() {
        getFromSocket("/eval hl.config({ openxr = { surfaces = false } })");
        getFromSocket("/eval hl.config({ openxr = { grab_filter = true } })"); // the shipped default
        if (!monitorName.empty())
            getFromSocket("/openxr destroy " + monitorName);
        if (XR::g_ctx.remote) {
            XR::g_ctx.remote->setSqueeze(CRemoteClient::SIDE_LEFT, 0.f);
            XR::g_ctx.remote->setTrigger(CRemoteClient::SIDE_LEFT, 0.f);
            XR::g_ctx.remote->setControllerActive(CRemoteClient::SIDE_LEFT, false);
            XR::g_ctx.remote->pulse();
        }
        if (failed)
            XR::dumpXrArtifacts(testName);
    }
};

static size_t monitorBlock(const std::string& json, const std::string& mon) {
    return XR::findAfter(json, "\"name\": \"" + mon + "\"");
}

static std::optional<std::array<float, 3>> surfaceMonitorPos(const std::string& json, const std::string& mon) {
    const auto p = monitorBlock(json, mon);
    if (p == std::string::npos)
        return std::nullopt;
    const auto v = XR::parseFloatArray(XR::fieldAfter(json, p, "pos"));
    if (v.size() < 3)
        return std::nullopt;
    return std::array<float, 3>{v[0], v[1], v[2]};
}

static std::string monitorField(const std::string& json, const std::string& mon, const std::string& key) {
    const auto p = monitorBlock(json, mon);
    return p == std::string::npos ? std::string{} : XR::fieldAfter(json, p, key);
}

// Reach focused, create a local monitor, hover its body/bar with the left controller and squeeze.
static bool surfaceGrab(CTestCase* tc, const std::string& mon, SSurfaceGuard& guard, MonadoWire::xrt_vec3& hoverOut) {
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
    // A rigid, unfiltered carry: the controller's motion is the monitor's motion.
    getFromSocket("/eval hl.config({ openxr = { grab_filter = false } })");
    if (getFromSocket("/openxr create " + mon + " 1280x720 anchor:local pos:0,0,-1.5 size:1.5") != "ok") {
        logSkip("monitor create failed");
        return false;
    }
    guard.monitorName = mon;

    remote->setControllerActive(CRemoteClient::SIDE_LEFT, true);
    remote->setTrigger(CRemoteClient::SIDE_LEFT, 0.f);
    remote->setSqueeze(CRemoteClient::SIDE_LEFT, 0.f);

    const MonadoWire::xrt_vec3 attempts[] = {{0.f, 0.f, 0.f}, {0.f, 1.5f, 0.f}, {0.f, -1.5f, 0.f}};
    bool                       gotHover   = false;
    const auto                 deadline   = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    int                        idx        = 0;
    while (!gotHover && std::chrono::steady_clock::now() < deadline) {
        const auto& a = attempts[(idx / 40) % 3];
        remote->setControllerPose(CRemoteClient::SIDE_LEFT, a, MonadoWire::xrt_quat{0.f, 0.f, 0.f, 1.f});
        remote->pulse();
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        if ((++idx % 20) == 0 && monitorField(getFromSocket("j/openxr"), mon, "hovered") == "true") {
            gotHover = true;
            hoverOut = a;
        }
    }
    if (!gotHover) {
        logSkip("ray never registered a hover within budget (known env instability)");
        return false;
    }

    remote->setSqueeze(CRemoteClient::SIDE_LEFT, 1.f);
    const auto gDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < gDeadline) {
        remote->pulse();
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        if (monitorField(getFromSocket("j/openxr"), mon, "grabbed") == "true")
            return true;
    }
    logSkip("grab never registered within budget (known env instability)");
    return false;
}

// Lower the controller (and so the carried monitor) in small steps until `stop` says so or `depth`
// metres are used up. Returns how far it went.
static float lowerUntil(const MonadoWire::xrt_vec3& from, float depth, const std::function<bool()>& stop) {
    auto*       remote = XR::g_ctx.remote;
    const float step   = 0.004F;
    float       d      = 0.F;
    for (int i = 0; d < depth; ++i) {
        d += step;
        remote->setControllerPose(CRemoteClient::SIDE_LEFT, MonadoWire::xrt_vec3{from.x, from.y - d, from.z}, MonadoWire::xrt_quat{0.f, 0.f, 0.f, 1.f});
        remote->pulse();
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        if (i % 3 == 0 && stop())
            break;
    }
    return d;
}

static bool awaitReleased(const std::string& mon) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (XR::g_ctx.remote)
            XR::g_ctx.remote->pulse();
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        if (monitorField(getFromSocket("j/openxr"), mon, "grabbed") == "false")
            return true;
    }
    return false;
}

TEST_CASE(xr_surface_snap_synthetic_table) {
    XR_SKIP_IF_UNAVAILABLE();

    const std::string    mon = XR::monitorName(76);
    SSurfaceGuard        guard{this->failed, name(), ""};
    MonadoWire::xrt_vec3 hover{};
    if (!surfaceGrab(this, mon, guard, hover))
        return;

    const auto start = surfaceMonitorPos(getFromSocket("j/openxr"), mon);
    ASSERT(start.has_value(), true);
    // A 3x3 m table 0.9 m below the monitor's center: its bottom edge (half the 0.84 m content plus
    // the chrome margin and bar) starts ~0.35 m above it.
    const float tableY = (*start)[1] - 0.9F;
    EXPECT(getFromSocket(std::format("/eval hl.config({{ openxr = {{ surfaces_synthetic = 'table pos:{:.3f},{:.3f},{:.3f} size:3,3' }} }})", (*start)[0], tableY, (*start)[2])), "ok");
    EXPECT(getFromSocket("/eval hl.config({ openxr = { surfaces = true } })"), "ok");
    ASSERT(XR::waitForJson("j/openxr", [](const std::string& j) { return j.find("\"source\": \"synthetic\"") != std::string::npos; }, std::chrono::milliseconds(5000)), true);
    NLog::green("xr_surface_snap_synthetic_table: table published at y {:.3f}", tableY);

    // Carry it down until the snap previews (the monitor is shown standing on the table).
    bool        previewed = false;
    const float went      = lowerUntil(hover, 0.7F, [&] {
        previewed = monitorField(getFromSocket("j/openxr"), mon, "preview") == "true";
        return previewed;
    });
    NLog::green("xr_surface_snap_synthetic_table: lowered {:.3f} m, preview {}", went, previewed);
    ASSERT(previewed, true);
    // ...while still a few centimetres above it: the carried pose itself has not reached the table
    ASSERT(went < 0.40F, true);

    XR::g_ctx.remote->setSqueeze(CRemoteClient::SIDE_LEFT, 0.f);
    ASSERT(awaitReleased(mon), true);

    const std::string st = getFromSocket("j/openxr");
    EXPECT(monitorField(st, mon, "attached"), "true");
    EXPECT(monitorField(st, mon, "kind"), "stand");
    EXPECT(monitorField(st, mon, "mode"), "local");
    const auto snapAt = XR::findAfter(st, "\"lastSnap\"");
    ASSERT(snapAt != std::string::npos, true);
    EXPECT(XR::fieldAfter(st, snapAt, "valid"), "true");
    EXPECT(XR::fieldAfter(st, snapAt, "monitor"), mon);
    EXPECT(XR::fieldAfter(st, snapAt, "label"), "table");
    const auto contact = XR::parseFloatArray(XR::fieldAfter(st, snapAt, "contact"));
    ASSERT(contact.size(), (size_t)3);
    NLog::green("xr_surface_snap_synthetic_table: contact [{:.3f}, {:.3f}, {:.3f}]", contact[0], contact[1], contact[2]);
    ASSERT(std::fabs(contact[1] - tableY) < 0.005F, true); // the bottom edge sits ON the plane

    // It stands on the table: above it by about its own lower half, and it stays there.
    const auto rest = surfaceMonitorPos(st, mon);
    ASSERT(rest.has_value(), true);
    ASSERT((*rest)[1] > tableY + 0.3F, true);
    ASSERT((*rest)[1] < tableY + 0.7F, true);
    for (int i = 0; i < 20; ++i) {
        XR::g_ctx.remote->pulse();
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    const auto later = surfaceMonitorPos(getFromSocket("j/openxr"), mon);
    ASSERT(later.has_value(), true);
    ASSERT(std::fabs((*later)[1] - (*rest)[1]) < 0.002F, true);
    EXPECT(monitorField(getFromSocket("j/openxr"), mon, "attached"), "true");
    NLog::green("xr_surface_snap_synthetic_table: monitor stands on the table at y {:.3f}", (*rest)[1]);
}

TEST_CASE(xr_surface_off_never_snaps) {
    XR_SKIP_IF_UNAVAILABLE();

    const std::string    mon = XR::monitorName(77);
    SSurfaceGuard        guard{this->failed, name(), ""};
    MonadoWire::xrt_vec3 hover{};
    getFromSocket("/eval hl.config({ openxr = { surfaces = false } })");
    if (!surfaceGrab(this, mon, guard, hover))
        return;

    const auto start = surfaceMonitorPos(getFromSocket("j/openxr"), mon);
    ASSERT(start.has_value(), true);
    const float tableY = (*start)[1] - 0.9F;
    EXPECT(getFromSocket(std::format("/eval hl.config({{ openxr = {{ surfaces_synthetic = 'table pos:{:.3f},{:.3f},{:.3f} size:3,3' }} }})", (*start)[0], tableY, (*start)[2])), "ok");

    // The same descent, all the way to where the table is: no preview, ever.
    bool previewed = false;
    lowerUntil(hover, 0.35F, [&] {
        previewed = previewed || monitorField(getFromSocket("j/openxr"), mon, "preview") == "true";
        return false;
    });
    ASSERT(previewed, false);
    const auto carried = surfaceMonitorPos(getFromSocket("j/openxr"), mon);
    XR::g_ctx.remote->setSqueeze(CRemoteClient::SIDE_LEFT, 0.f);
    ASSERT(awaitReleased(mon), true);
    const std::string st = getFromSocket("j/openxr");
    EXPECT(monitorField(st, mon, "attached"), "false");
    // the monitor stays where the hand let it go
    const auto rest = surfaceMonitorPos(st, mon);
    ASSERT(rest.has_value() && carried.has_value(), true);
    ASSERT(std::fabs((*rest)[1] - (*carried)[1]) < 0.05F, true);
    EXPECT_CONTAINS(st, "\"surfaces\": { \"enabled\": false");
}

// ---- the keyboard gaze path (live repro 2026-10-16) ----
// The live user moved a big monitor with SUPER+SHIFT+G (`gazegrab`) and SUPER+ALT+±
// (`gazepush ±0.1`) toward them over a table whose top was BETWEEN the monitor's center and its
// bottom edge: deeper "in" the table than the old fixed 25 cm penetration limit, and — for the
// pushes made without a carry — never evaluated at all. Both legs are scripted here with the head
// still and straight ahead, against a declared table that starts just short of the monitor and
// runs toward the head, its top 0.30 m under the monitor's center (the bottom edge ~0.30 m under it).

struct SGazeSurfaceGuard {
    const bool& failed;
    std::string testName;
    std::string monitorName;
    ~SGazeSurfaceGuard() {
        getFromSocket("/openxr gazerelease");
        getFromSocket("/eval hl.config({ openxr = { surfaces = false } })");
        getFromSocket("/eval hl.config({ openxr = { gaze_dwell_ms = 200 } })");
        if (!monitorName.empty())
            getFromSocket("/openxr destroy " + monitorName);
        if (XR::g_ctx.remote) {
            XR::g_ctx.remote->setHeadPose(MonadoWire::xrt_vec3{0.f, 0.f, 0.f}, MonadoWire::xrt_quat{0.f, 0.f, 0.f, 1.f});
            XR::g_ctx.remote->pulse();
        }
        if (failed)
            XR::dumpXrArtifacts(testName);
    }
};

// Focused, head at the origin looking down -Z, a 1.5 m local monitor 1.5 m ahead.
static bool gazeSurfaceSetup(CTestCase* tc, const std::string& mon, SGazeSurfaceGuard& guard) {
    if (!XR::waitForXrState("focused", std::chrono::milliseconds(15000))) {
        XR::logSkip(tc->name(), "session never reached focused (known env instability)");
        return false;
    }
    if (!XR::g_ctx.remote) {
        XR::logSkip(tc->name(), "no remote client available");
        return false;
    }
    XR::g_ctx.remote->setHeadPose(MonadoWire::xrt_vec3{0.f, 0.f, 0.f}, MonadoWire::xrt_quat{0.f, 0.f, 0.f, 1.f});
    XR::g_ctx.remote->pulse();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    getFromSocket("/eval hl.config({ openxr = { surfaces = false } })");
    if (getFromSocket("/openxr create " + mon + " 1280x720 anchor:local pos:0,0,-1.5 size:1.5") != "ok") {
        XR::logSkip(tc->name(), "monitor create failed");
        return false;
    }
    guard.monitorName = mon;
    return XR::waitForJson("j/openxr", [&](const std::string& j) { return surfaceMonitorPos(j, mon).has_value(); }, std::chrono::milliseconds(10000));
}

// The table: top 0.30 m under `at`'s center, from 0.35 m nearer the head than `at` to 1.15 m nearer.
static bool declareTableInFrontOf(const std::array<float, 3>& at, float& tableY) {
    tableY                 = at[1] - 0.30F;
    const std::string spec = std::format("table pos:{:.3f},{:.3f},{:.3f} size:3,0.8", at[0], tableY, at[2] + 0.75F);
    if (getFromSocket("/eval hl.config({ openxr = { surfaces_synthetic = '" + spec + "' } })") != "ok")
        return false;
    if (getFromSocket("/eval hl.config({ openxr = { surfaces = true } })") != "ok")
        return false;
    return XR::waitForJson("j/openxr", [](const std::string& j) { return j.find("\"source\": \"synthetic\"") != std::string::npos; }, std::chrono::milliseconds(5000));
}

static void pulseFrames(int n) {
    for (int i = 0; i < n; ++i) {
        if (XR::g_ctx.remote)
            XR::g_ctx.remote->pulse();
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
}

static bool expectSnappedOnTable(const std::string& mon, float tableY) {
    const bool attached = XR::waitForJson("j/openxr", [&](const std::string& j) { return monitorField(j, mon, "attached") == "true"; }, std::chrono::milliseconds(3000));
    if (!attached)
        return false;
    const std::string st     = getFromSocket("j/openxr");
    const auto        snapAt = XR::findAfter(st, "\"lastSnap\"");
    if (snapAt == std::string::npos || XR::fieldAfter(st, snapAt, "monitor") != mon || XR::fieldAfter(st, snapAt, "kind") != "stand")
        return false;
    const auto contact = XR::parseFloatArray(XR::fieldAfter(st, snapAt, "contact"));
    return contact.size() == 3 && std::fabs(contact[1] - tableY) < 0.005F && monitorField(st, mon, "mode") == "local";
}

TEST_CASE(xr_surface_gaze_carry_push_through_table) {
    XR_SKIP_IF_UNAVAILABLE();

    const std::string mon = XR::monitorName(78);
    SGazeSurfaceGuard guard{this->failed, name(), ""};
    if (!gazeSurfaceSetup(this, mon, guard))
        return;

    // SUPER+SHIFT+G, by name (no dwell needed): the carry starts where the monitor is — once the
    // frame loop has placed it ("monitor not placed yet" until its first solve).
    std::string grabReply;
    for (int i = 0; i < 60 && grabReply != "ok"; ++i) {
        grabReply = getFromSocket("/openxr gazegrab " + mon);
        if (grabReply != "ok")
            pulseFrames(5);
    }
    if (grabReply != "ok") {
        XR::logSkip(name(), "gazegrab never succeeded: " + grabReply + " (known env instability)");
        return;
    }
    pulseFrames(10);
    const auto carried = surfaceMonitorPos(getFromSocket("j/openxr"), mon);
    ASSERT(carried.has_value(), true);
    float tableY = 0.F;
    ASSERT(declareTableInFrontOf(*carried, tableY), true);
    pulseFrames(10);
    EXPECT(monitorField(getFromSocket("j/openxr"), mon, "preview"), "false"); // not over it yet

    // SUPER+ALT+minus while carrying: the carry comes toward the head, over the table, its bottom edge
    // ~0.30 m deep in it. That is a window standing in a table: it previews...
    bool previewed = false;
    for (int i = 0; i < 4 && !previewed; ++i) {
        ASSERT(getFromSocket("/openxr gazepush -0.2"), std::string("ok"));
        pulseFrames(8);
        previewed = monitorField(getFromSocket("j/openxr"), mon, "preview") == "true";
    }
    ASSERT(previewed, true);
    NLog::green("xr_surface_gaze_carry_push_through_table: carried monitor previews on the table at y {:.3f}", tableY);

    // ...and the keyboard release (G again) stands it on the table.
    ASSERT(getFromSocket("/openxr gazegrab"), std::string("ok"));
    ASSERT(expectSnappedOnTable(mon, tableY), true);
}

TEST_CASE(xr_surface_gaze_nudge_settles_on_table) {
    XR_SKIP_IF_UNAVAILABLE();

    const std::string mon = XR::monitorName(79);
    SGazeSurfaceGuard guard{this->failed, name(), ""};
    if (!gazeSurfaceSetup(this, mon, guard))
        return;

    // No carry: SUPER+ALT+minus nudges the monitor the head dwells on.
    getFromSocket("/eval hl.config({ openxr = { gaze_dwell_ms = 120 } })");
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
    const auto start = surfaceMonitorPos(getFromSocket("j/openxr"), mon);
    ASSERT(start.has_value(), true);
    float tableY = 0.F;
    ASSERT(declareTableInFrontOf(*start, tableY), true);
    pulseFrames(10);
    EXPECT(monitorField(getFromSocket("j/openxr"), mon, "attached"), "false");

    // Two nudges bring it over the table, which it then stands on — a one-step carry and release.
    bool attached = false;
    for (int i = 0; i < 4 && !attached; ++i) {
        ASSERT(getFromSocket("/openxr gazepush -0.2"), std::string("ok"));
        pulseFrames(8);
        attached = monitorField(getFromSocket("j/openxr"), mon, "attached") == "true";
    }
    ASSERT(attached, true);
    ASSERT(expectSnappedOnTable(mon, tableY), true);
    NLog::green("xr_surface_gaze_nudge_settles_on_table: nudged monitor stands on the table at y {:.3f}", tableY);
}

#endif // WITH_XR_TESTS
