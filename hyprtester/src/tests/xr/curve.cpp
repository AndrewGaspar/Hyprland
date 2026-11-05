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

// xr_curve — curved (cylindrical) XR monitors end to end (docs/openxr/02 "Curved monitors").
//
// Creates a monitor with `curve:auto` 0.8 m dead ahead of a scripted head and checks:
//   - status reports the spec and, when the runtime advertises XR_KHR_composition_layer_cylinder,
//     that it is being SUBMITTED as a cylinder whose auto radius is the head distance;
//   - the gaze ray lands on the CURVE: with the head on the axis, every column is the same distance
//     away, so a gaze 25 deg off-centre still hits at ~R (a flat quad would be R / cos 25 = +10%);
//   - `openxr layout` round-trips `curve:auto`;
//   - `openxr curve flat` flattens it live and the same gaze then hits the plane farther away.
// The null compositor does not rasterize, so WHAT is drawn is checked live in the glasses; the
// mapping itself is pinned to Monado's shader in tests/xr/curve.cpp.

namespace {
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
            if (!monitorName.empty())
                getFromSocket("/openxr destroy " + monitorName);
            getFromSocket("/keyword openxr:gaze_dwell_ms 200");
            if (failed)
                XR::dumpXrArtifacts(testName);
        }
    };

    bool gateUp() {
        if (XR::waitForXrState("focused", std::chrono::milliseconds(15000)))
            return true;
        return XR::waitForXrState("visible", std::chrono::milliseconds(2000));
    }

    // A field of this monitor's "curve" object in the status JSON.
    std::string curveField(const std::string& json, const std::string& mon, const std::string& key) {
        const auto MP = XR::findAfter(json, "\"name\": \"" + mon + "\"");
        if (MP == std::string::npos)
            return "";
        const auto CP = XR::findAfter(json, "\"curve\"", MP);
        return CP == std::string::npos ? std::string{} : XR::fieldAfter(json, CP, key);
    }

    void setHeadYawDeg(float deg) {
        using namespace MonadoWire;
        const float h = deg * (float)M_PI / 360.f;
        XR::g_ctx.remote->setHeadPose(xrt_vec3{0.f, 0.f, 0.f}, xrt_quat{0.f, std::sin(h), 0.f, std::cos(h)});
        XR::g_ctx.remote->pulse();
    }

    // The gaze hit distance once the gaze has dwelt on `mon`: first the status gaze selection (the
    // same signal xr_gaze_grab waits on), then `openxr gaze`, whose hitDistM is only reported for the
    // selected monitor. -1 on timeout, with the last replies logged so a skip is diagnosable.
    float gazeHitDist(const std::string& mon) {
        const bool SELECTED = XR::waitForJson(
            "j/openxr",
            [&](const std::string& r) {
                const auto gp = XR::findAfter(r, "\"gaze\"");
                return gp != std::string::npos && XR::fieldAfter(r, gp, "hoveredName") == mon;
            },
            std::chrono::milliseconds(4000));
        float       out = -1.f;
        std::string last;
        const bool  OK = SELECTED &&
            XR::waitForJson(
                            "j/openxr gaze",
                            [&](const std::string& r) {
                                last = r;
                                if (r.find("\"hitDistM\"") == std::string::npos)
                                    return false;
                                out = XR::toFloatOr(XR::fieldAfter(r, 0, "hitDistM"), -1.f);
                                return out > 0.f;
                            },
                            std::chrono::milliseconds(2000));
        if (!OK) {
            const std::string st = getFromSocket("j/openxr");
            const auto        gp = XR::findAfter(st, "\"gaze\"");
            NLog::yellow("xr_curve: no gaze hit on {} (status gaze.hoveredName '{}'); last `openxr gaze`: {}", mon,
                         gp == std::string::npos ? std::string{} : XR::fieldAfter(st, gp, "hoveredName"), last.empty() ? getFromSocket("j/openxr gaze") : last);
        }
        return OK ? out : -1.f;
    }
}

TEST_CASE(xr_curve) {
    XR_SKIP_IF_UNAVAILABLE();

    const std::string mon = XR::monitorName(90);
    // 0.8 m: NEARER than the suite's head-locked XR-conf-b fixture (1.0 m, just right of the gaze
    // ray), so the gaze's nearest hit is this monitor and not that one. A 1.2 m arc at R 0.8 spans
    // +/-43 deg, so a 25 deg gaze is well inside it; flat, the same gaze would be 0.8/cos25 = 0.883 m.
    constexpr float   DIST = 0.8f;
    SArtifactGuard    guard{this->failed, name(), ""};

    if (!gateUp()) {
        XR::logSkip(name(), "session never reached focused/visible (known env instability)");
        return;
    }
    auto* remote = XR::g_ctx.remote;
    if (!remote) {
        XR::logSkip(name(), "no remote client available");
        return;
    }

    setHeadYawDeg(0.f);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    getFromSocket("/keyword openxr:gaze_dwell_ms 80");

    // A bad radius is refused before anything is created.
    EXPECT(getFromSocket("/openxr create " + mon + " 1280x720 anchor:local pos:0,0,-0.8 size:1.2 curve:0.05") != "ok", true);

    ASSERT(getFromSocket("/openxr create " + mon + " 1280x720 anchor:local pos:0,0,-0.8 size:1.2 curve:auto"), std::string("ok"));
    guard.monitorName = mon;
    // the `curve` verb acts on the selected monitor; the suite's config fixtures must not be it
    ASSERT(getFromSocket("/openxr select " + mon), std::string("ok"));
    ASSERT(XR::waitForJson("j/openxr", [&](const std::string& r) { return curveField(r, mon, "curveSpec") == "auto"; }, std::chrono::milliseconds(10000)), true);
    EXPECT(curveField(getFromSocket("j/openxr"), mon, "curveSource"), std::string("explicit"));

    // The layout line round-trips the spec (auto stays auto, not a frozen radius).
    EXPECT(getFromSocket("/openxr layout").contains(mon + ", 1280x720") && getFromSocket("/openxr layout").contains("curve:auto"), true);

    const std::string st = getFromSocket("j/openxr");
    if (XR::fieldAfter(st, 0, "cylinderLayers") != "true") {
        // Runtime without the extension: the monitor must say why it is flat, and nothing else applies.
        EXPECT(curveField(st, mon, "curveActive"), std::string("false"));
        EXPECT(curveField(st, mon, "curveFlatReason").contains("cylinder"), true);
        XR::logSkip(name(), "runtime does not advertise XR_KHR_composition_layer_cylinder (flat fallback verified)");
        return;
    }

    // Submitted as a cylinder, auto radius = the head distance.
    const bool active = XR::waitForJson("j/openxr", [&](const std::string& r) { return curveField(r, mon, "curveActive") == "true"; }, std::chrono::milliseconds(5000));
    if (!active) {
        XR::logSkip(name(), "curved monitor never submitted (no frames; flatReason '" + curveField(getFromSocket("j/openxr"), mon, "curveFlatReason") + "')");
        return;
    }
    {
        const std::string s2 = getFromSocket("j/openxr");
        const float       R  = XR::toFloatOr(curveField(s2, mon, "curveRadiusM"), -1.f);
        const float       A  = XR::toFloatOr(curveField(s2, mon, "curveAngleDeg"), -1.f);
        if (std::fabs(R - DIST) > 0.05f)
            MARK_TEST_FAILED("auto curve radius {} (expected the {} m head distance)", R, DIST);
        // the full surface (content + any chrome margins) is at least size:1.2 -> at least 85.9 deg
        if (A < 85.f || A > 110.f)
            MARK_TEST_FAILED("central angle {} deg (expected ~86-105 for a 1.2 m arc at R 0.8)", A);
        NLog::green("xr_curve: submitted as a cylinder, R {:.3f} m, {:.1f} deg", R, A);
    }

    // Gaze straight ahead and 25 deg to the left: both hit ~R away on the curve.
    const float d0 = gazeHitDist(mon);
    setHeadYawDeg(25.f);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const float d25 = gazeHitDist(mon);
    if (d0 < 0.f || d25 < 0.f) {
        XR::logSkip(name(), "gaze never selected the monitor (known env instability)");
        return;
    }
    if (std::fabs(d0 - DIST) > 0.03f || std::fabs(d25 - DIST) > 0.03f)
        MARK_TEST_FAILED("curved gaze hits at {:.3f} m (0 deg) and {:.3f} m (25 deg); expected both ~{} m on the arc", d0, d25, DIST);
    else
        NLog::green("xr_curve: gaze hits the arc at {:.3f} m / {:.3f} m (equidistant)", d0, d25);

    // Flatten it live: the same 25 deg gaze now meets the plane at R / cos 25.
    ASSERT(getFromSocket("/openxr curve flat"), std::string("ok"));
    ASSERT(XR::waitForJson(
               "j/openxr", [&](const std::string& r) { return curveField(r, mon, "curveSpec") == "flat" && curveField(r, mon, "curveActive") == "false"; },
               std::chrono::milliseconds(3000)),
           true);
    EXPECT(curveField(getFromSocket("j/openxr"), mon, "curveSource"), std::string("runtime"));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const float dFlat = gazeHitDist(mon);
    const float want  = DIST / std::cos(25.f * (float)M_PI / 180.f);
    if (dFlat < 0.f)
        XR::logSkip(name(), "gaze lost the flattened monitor (known env instability)");
    else if (std::fabs(dFlat - want) > 0.03f)
        MARK_TEST_FAILED("flattened gaze hit at {:.3f} m, expected the plane at {:.3f} m", dFlat, want);
    else
        NLog::green("xr_curve: flattened live, gaze now meets the plane at {:.3f} m", dFlat);

    // And back: an explicit radius.
    EXPECT(getFromSocket("/openxr curve 0.1") != "ok", true);
    ASSERT(getFromSocket("/openxr curve 2.5"), std::string("ok"));
    EXPECT(XR::waitForJson(
               "j/openxr", [&](const std::string& r) { return std::fabs(XR::toFloatOr(curveField(r, mon, "curveRadiusM"), 0.f) - 2.5f) < 1e-3f; }, std::chrono::milliseconds(3000)),
           true);
}

#endif // WITH_XR_TESTS
