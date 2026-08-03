#include "tests.hpp"

#ifdef WITH_XR_TESTS

#include "../../hyprctlCompat.hpp"
#include "../../Log.hpp"
#include "../../xr/xr_helpers.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <string>
#include <thread>
#include <vector>

// xr/layout2d.cpp — 2D-plane sync end to end (report 12 WP-S2). The pure projection is exhaustively
// covered by tests/xr/layout2d.cpp (gtests); what CANNOT be tested there is the wiring: does a quad
// that lives to the left in 3D actually end up to the left in `hyprctl monitors`, and does moving it
// in 3D move its box? That needs a live session, so it lives here.

namespace {
    bool gateUp() {
        if (XR::waitForXrState("focused", std::chrono::milliseconds(15000)))
            return true;
        return XR::waitForXrState("visible", std::chrono::milliseconds(2000));
    }

    struct SGuard {
        const bool&              failed;
        std::string              testName;
        std::vector<std::string> monitorNames;
        ~SGuard() {
            // Always thaw: a test that fails mid-way while frozen would otherwise leave the sync
            // engine paused for every test after it.
            getFromSocket("/openxr sync-layout thaw");
            for (auto& n : monitorNames)
                if (!n.empty())
                    getFromSocket("/openxr destroy " + n);
            if (failed)
                XR::dumpXrArtifacts(testName);
        }
    };

    // The layout x of a monitor in `hyprctl -j monitors`, or NaN if it isn't there.
    float monitorX(const std::string& json, const std::string& name) {
        const size_t at = XR::findAfter(json, "\"name\": \"" + name + "\"");
        if (at == std::string::npos)
            return std::numeric_limits<float>::quiet_NaN();
        return XR::toFloatOr(XR::fieldAfter(json, at, "x"), std::numeric_limits<float>::quiet_NaN());
    }
}

// xr_layout2d_follows_3d — the headline contract: a monitor floating to your LEFT gets a smaller
// layout x than one floating to your right, and MOVING it in 3D moves its 2D box to match. Without
// the feature both monitors land append-right in CREATION order, so the second assertion (the order
// FLIPS after the move) is what makes this a real regression test rather than a coincidence.
TEST_CASE(xr_layout2d_follows_3d) {
    XR_SKIP_IF_UNAVAILABLE();
    SGuard guard{this->failed, name(), {}};

    if (!gateUp()) {
        XR::logSkip(name(), "session never reached focused/visible (known env instability)");
        return;
    }

    // 2D-plane sync must be ON for this to mean anything; if the shared suite config turned it off,
    // skip rather than fail.
    if (getFromSocket("/openxr status").contains("2d-plane sync: off")) {
        XR::logSkip(name(), "openxr:layout2d:enabled is off");
        return;
    }

    // Create them in an order that DISAGREES with their spatial order, so append-right and
    // spatial-order cannot both be satisfied by accident: `right` is created first.
    const std::string R = XR::monitorName(70);
    const std::string L = XR::monitorName(71);

    ASSERT(getFromSocket("/openxr create " + R + " 1280x720 anchor:local pos:1.2,1.4,-1.5 yaw:0"), std::string("ok"));
    guard.monitorNames.push_back(R);
    ASSERT(getFromSocket("/openxr create " + L + " 1280x720 anchor:local pos:-1.2,1.4,-1.5 yaw:0"), std::string("ok"));
    guard.monitorNames.push_back(L);

    // Force a sync (also re-latches the reference frame) rather than waiting on the debounce.
    ASSERT(getFromSocket("/openxr sync-layout"), std::string("ok"));

    ASSERT(XR::waitForJson(
               "j/monitors",
               [&](const std::string& r) {
                   const float xl = monitorX(r, L), xr = monitorX(r, R);
                   return xl == xl && xr == xr && xl < xr; // both present (not NaN) and left-of-right
               },
               std::chrono::milliseconds(10000)),
           true);

    // Now teleport the LEFT monitor to the far right of the other one and re-sync. Its 2D box must
    // follow — this is the whole feature, and it is the assertion append-right cannot pass. The
    // target keeps roughly the same horizontal radius (2.18 m vs 1.92 m) as the monitor it has to
    // overtake, so the two stay in the SAME elevation tier whatever height the runtime puts the eye
    // at, and the assertion is purely about azimuth ordering.
    ASSERT(getFromSocket("/openxr place " + L + " at 2.1,1.4,-0.6"), std::string("ok"));
    ASSERT(getFromSocket("/openxr sync-layout"), std::string("ok"));

    ASSERT(XR::waitForJson(
               "j/monitors",
               [&](const std::string& r) {
                   const float xl = monitorX(r, L), xr = monitorX(r, R);
                   return xl == xl && xr == xr && xl > xr;
               },
               std::chrono::milliseconds(10000)),
           true);
}

// xr_layout2d_freeze_thaw — `sync-layout freeze` must actually stop the automatic recompute (so a
// user rearranging quads doesn't get their mouse mapping yanked mid-session), and `thaw` must catch
// the layout back up. An explicit `sync-layout` still forces a pass while frozen.
TEST_CASE(xr_layout2d_freeze_thaw) {
    XR_SKIP_IF_UNAVAILABLE();
    SGuard guard{this->failed, name(), {}};

    if (!gateUp()) {
        XR::logSkip(name(), "session never reached focused/visible (known env instability)");
        return;
    }
    if (getFromSocket("/openxr status").contains("2d-plane sync: off")) {
        XR::logSkip(name(), "openxr:layout2d:enabled is off");
        return;
    }

    // TWO monitors, so "did the layout move" is a real question: with only one the block is a single
    // box at the seam and its x is the same wherever the quad floats.
    const std::string A = XR::monitorName(72); // starts LEFT
    const std::string B = XR::monitorName(73); // stays RIGHT
    ASSERT(getFromSocket("/openxr create " + A + " 1280x720 anchor:local pos:-1.2,1.4,-1.5 yaw:0"), std::string("ok"));
    guard.monitorNames.push_back(A);
    ASSERT(getFromSocket("/openxr create " + B + " 1280x720 anchor:local pos:1.2,1.4,-1.5 yaw:0"), std::string("ok"));
    guard.monitorNames.push_back(B);
    ASSERT(getFromSocket("/openxr sync-layout"), std::string("ok"));

    ASSERT(XR::waitForJson(
               "j/monitors",
               [&](const std::string& r) {
                   const float xa = monitorX(r, A), xb = monitorX(r, B);
                   return xa == xa && xb == xb && xa < xb;
               },
               std::chrono::milliseconds(10000)),
           true);
    const float before = monitorX(getFromSocket("j/monitors"), A);

    ASSERT(getFromSocket("/openxr sync-layout freeze"), std::string("ok"));
    ASSERT(getFromSocket("/openxr status").contains("frozen"), true);

    // Move A past B. The AUTOMATIC recompute (the `place` verb's trigger) must NOT fire while frozen.
    ASSERT(getFromSocket("/openxr place " + A + " at 2.1,1.4,-0.6"), std::string("ok"));
    std::this_thread::sleep_for(std::chrono::milliseconds(1200)); // well past openxr:layout2d:debounce_ms
    ASSERT(monitorX(getFromSocket("j/monitors"), A) == before, true);

    // Thawing re-arms it and the layout catches up on the debounce — A is now right of B.
    ASSERT(getFromSocket("/openxr sync-layout thaw"), std::string("ok"));
    ASSERT(XR::waitForJson(
               "j/monitors",
               [&](const std::string& r) {
                   const float xa = monitorX(r, A), xb = monitorX(r, B);
                   return xa == xa && xb == xb && xa > xb;
               },
               std::chrono::milliseconds(10000)),
           true);
}

#endif // WITH_XR_TESTS
