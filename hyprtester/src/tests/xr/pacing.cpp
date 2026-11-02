#include "tests.hpp"

#ifdef WITH_XR_TESTS

#include "../../hyprctlCompat.hpp"
#include "../../Log.hpp"
#include "../../xr/xr_helpers.hpp"
#include "../shared.hpp" // Tests:: client helpers (spawnKitty / killAllWindows)

#include <hyprutils/utils/ScopeGuard.hpp>

#include <chrono>
#include <cstdint>
#include <format>
#include <string>
#include <thread>
#include <vector>

using Hyprutils::Utils::CScopeGuard;

// tests/xr/pacing.cpp — damage-gated XR pacing (docs/openxr/02-virtual-monitors.md §Frame pacing,
// AndrewGaspar/Hyprland#1). The XR tick used to composite every XR monitor on every runtime frame;
// now it only composites one that has something new. The two pure decisions are gtest-covered
// (tests/xr/pacing.cpp); what only a live session can show is that the WHOLE chain still moves
// pixels when it should and stops when it should not:
//
//   - a static monitor stops presenting while the XR tick keeps running (ticksIdle climbs),
//   - openxr:pace_static_monitors = 1 brings the every-tick behaviour back,
//   - an animation on the monitor (a workspace switch) renders, and then it goes quiet again,
//   - cursor motion over it renders,
//   - a client drawing continuously keeps getting frame callbacks (it would stall otherwise).
//
// The observable is the per-monitor `pacing` object in `hyprctl -j openxr`: cumulative
// framesPresented (desktop buffers handed to the XR layer), framesBlitted, ticksPaced, ticksIdle.

namespace {
    bool gateUp() {
        if (XR::waitForXrState("focused", std::chrono::milliseconds(15000)))
            return true;
        return XR::waitForXrState("visible", std::chrono::milliseconds(2000));
    }

    struct SArtifactGuard {
        const bool&              failed;
        std::string              testName;
        std::vector<std::string> monitorNames;
        ~SArtifactGuard() {
            getFromSocket("/keyword openxr:pace_static_monitors 0");
            for (auto& n : monitorNames)
                if (!n.empty())
                    getFromSocket("/openxr destroy " + n);
            if (failed)
                XR::dumpXrArtifacts(testName);
        }
    };

    struct SPacing {
        int64_t presented = -1;
        int64_t blitted   = -1;
        int64_t paced     = -1;
        int64_t idle      = -1;
    };

    SPacing pacingOf(const std::string& mon) {
        const std::string ST = getFromSocket("j/openxr");
        const auto        P  = XR::findAfter(ST, "\"name\": \"" + mon + "\"");
        SPacing           out;
        if (P == std::string::npos)
            return out;
        const auto Q   = XR::findAfter(ST, "\"pacing\":", P);
        auto       num = [&](const std::string& key) -> int64_t {
            const auto S = XR::fieldAfter(ST, Q, key);
            try {
                return S.empty() ? -1 : std::stoll(S);
            } catch (...) { return -1; }
        };
        out.presented = num("framesPresented");
        out.blitted   = num("framesBlitted");
        out.paced     = num("ticksPaced");
        out.idle      = num("ticksIdle");
        return out;
    }

    // Counter deltas over one window.
    SPacing deltaOver(const std::string& mon, std::chrono::milliseconds window) {
        const auto A = pacingOf(mon);
        std::this_thread::sleep_for(window);
        const auto B = pacingOf(mon);
        return {.presented = B.presented - A.presented, .blitted = B.blitted - A.blitted, .paced = B.paced - A.paced, .idle = B.idle - A.idle};
    }

    // Wait until the monitor has been quiet (no presents) for a whole `window` — workspace
    // creation, the first plug and the cursor warp all animate briefly on a fresh monitor.
    bool waitQuiet(const std::string& mon, std::chrono::milliseconds window, std::chrono::milliseconds budget) {
        const auto DEADLINE = std::chrono::steady_clock::now() + budget;
        while (std::chrono::steady_clock::now() < DEADLINE) {
            if (deltaOver(mon, window).presented == 0)
                return true;
        }
        return false;
    }
}

TEST_CASE(xr_pacing_damage_gated) {
    XR_SKIP_IF_UNAVAILABLE();
    SArtifactGuard guard{this->failed, name(), {}};

    if (!gateUp()) {
        XR::logSkip(name(), "session never reached focused/visible (known env instability)");
        return;
    }

    const std::string MON        = XR::monitorName(80);
    const std::string NAMEMARKER = "\"name\": \"" + MON + "\"";

    CScopeGuard       windowsGuard = {[&]() { Tests::killAllWindows(); }};

    // Park the pointer on the fallback output first, so the new monitor's only damage is its own
    // bring-up.
    getFromSocket("/dispatch movecursor 50 50");

    ASSERT(getFromSocket("/openxr create " + MON + " 1280x720 anchor:local pos:0,1.4,-1.5"), std::string("ok"));
    guard.monitorNames.push_back(MON);
    ASSERT(XR::waitForJson("j/openxr", [&](const std::string& r) { return r.contains(NAMEMARKER); }, std::chrono::milliseconds(10000)), true);

    // 1. The first buffer lands without any damage of ours: a fresh swapchain asks for its content
    //    (xrPlanStaleLayer -> m_wantFrame), so a static desktop never leaves the quad unsubmitted.
    if (!XR::waitForJson(
            "j/openxr",
            [&](const std::string& r) {
                const auto P = XR::findAfter(r, NAMEMARKER);
                return XR::fieldAfter(r, P, "contentPath") != "none" && XR::fieldAfter(r, P, "contentPath") != "";
            },
            std::chrono::milliseconds(10000))) {
        MARK_TEST_FAILED("the new XR monitor never received its first blit (contentPath stayed none)");
        return;
    }

    // 2. Static: once bring-up settles the monitor stops presenting, while the XR tick keeps running.
    if (!waitQuiet(MON, std::chrono::milliseconds(700), std::chrono::milliseconds(10000))) {
        const auto D = deltaOver(MON, std::chrono::milliseconds(1000));
        MARK_TEST_FAILED("a static XR monitor never went quiet: {} presents / {} paced / {} idle ticks in 1s", D.presented, D.paced, D.idle);
        return;
    }
    const auto STATIC = deltaOver(MON, std::chrono::milliseconds(1000));
    NLog::green("xr_pacing_damage_gated: static monitor: {} presents, {} blits, {} paced / {} idle ticks in 1s", STATIC.presented, STATIC.blitted, STATIC.paced, STATIC.idle);
    EXPECT(STATIC.presented <= 1, true);
    EXPECT(STATIC.blitted <= 1, true);
    // the runtime is still ticking: idle ticks are counted, at roughly the runtime rate
    EXPECT(STATIC.idle >= 10, true);

    // 3. The kill switch brings back the every-tick composite.
    ASSERT(getFromSocket("/keyword openxr:pace_static_monitors 1"), std::string("ok"));
    const auto PACEALL = deltaOver(MON, std::chrono::milliseconds(1000));
    ASSERT(getFromSocket("/keyword openxr:pace_static_monitors 0"), std::string("ok"));
    NLog::green("xr_pacing_damage_gated: pace_static_monitors=1: {} presents, {} paced ticks in 1s", PACEALL.presented, PACEALL.paced);
    EXPECT(PACEALL.presented >= 10, true);
    EXPECT(PACEALL.paced >= 10, true);
    if (!waitQuiet(MON, std::chrono::milliseconds(700), std::chrono::milliseconds(5000)))
        MARK_TEST_FAILED("the monitor did not go quiet again after pace_static_monitors returned to 0");

    // 4. An animation renders until it is done. A workspace switch on the monitor slides (and
    //    focusing the monitor warps the cursor onto it); both must reach the XR layer.
    ASSERT(getFromSocket("/dispatch focusmonitor " + MON), std::string("ok"));
    ASSERT(getFromSocket("/dispatch workspace 8080"), std::string("ok"));
    const auto ANIM = deltaOver(MON, std::chrono::milliseconds(400));
    NLog::green("xr_pacing_damage_gated: workspace switch: {} presents, {} blits in 400ms", ANIM.presented, ANIM.blitted);
    EXPECT(ANIM.presented >= 2, true);
    EXPECT(ANIM.blitted >= 1, true);
    if (!waitQuiet(MON, std::chrono::milliseconds(700), std::chrono::milliseconds(10000)))
        MARK_TEST_FAILED("the monitor did not go quiet after the workspace animation finished");

    // 5. Cursor motion over the monitor redraws it (software cursor on a headless output).
    {
        const std::string MONS = getFromSocket("j/monitors");
        const auto        P    = XR::findAfter(MONS, NAMEMARKER);
        const int         X    = std::stoi(XR::fieldAfter(MONS, P, "x").empty() ? "0" : XR::fieldAfter(MONS, P, "x"));
        const int         Y    = std::stoi(XR::fieldAfter(MONS, P, "y").empty() ? "0" : XR::fieldAfter(MONS, P, "y"));
        const auto        A    = pacingOf(MON);
        for (int i = 0; i < 10; ++i) {
            getFromSocket(std::format("/dispatch movecursor {} {}", X + 100 + i * 30, Y + 100 + i * 20));
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const auto B = pacingOf(MON);
        NLog::green("xr_pacing_damage_gated: 10 cursor moves: {} presents", B.presented - A.presented);
        EXPECT(B.presented - A.presented >= 3, true);
    }
    if (!waitQuiet(MON, std::chrono::milliseconds(700), std::chrono::milliseconds(10000)))
        MARK_TEST_FAILED("the monitor did not go quiet after the cursor stopped");

    // 6. A client drawing continuously keeps getting frame callbacks: kitty paces its redraws on
    //    wl_surface.frame, so if the gated monitor stopped answering them the output would stall
    //    and the presents would dry up. The loop ends by itself after 4s and kitty closes its
    //    window when its child exits (killing the window would hit kitty's close confirmation
    //    for a running foreground process).
    auto kitty = Tests::spawnKitty("xr_pacing", {"sh", "-c", "end=$(($(date +%s)+4)); while [ $(date +%s) -lt $end ]; do date +%s.%N; sleep 0.02; done"});
    if (!kitty) {
        XR::logSkip(name(), "kitty did not spawn (env limitation); client frame-callback leg skipped");
        return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500)); // map + open animation
    const auto CLIENT = deltaOver(MON, std::chrono::milliseconds(1000));
    NLog::green("xr_pacing_damage_gated: continuously drawing client: {} presents, {} blits in 1s", CLIENT.presented, CLIENT.blitted);
    EXPECT(CLIENT.presented >= 10, true);
    EXPECT(CLIENT.blitted >= 10, true);

    if (!XR::waitForJson("j/clients", [&](const std::string& r) { return !r.contains("\"xr_pacing\""); }, std::chrono::milliseconds(10000)))
        MARK_TEST_FAILED("the drawing kitty never closed its window after its loop ended");
    if (!waitQuiet(MON, std::chrono::milliseconds(700), std::chrono::milliseconds(10000)))
        MARK_TEST_FAILED("the monitor did not go quiet after the client closed");
}

#endif // WITH_XR_TESTS
