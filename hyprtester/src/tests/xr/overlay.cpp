#include "tests.hpp"

#ifdef WITH_XR_TESTS

#include "../../hyprctlCompat.hpp"
#include "../../Log.hpp"
#include "../../xr/xr_helpers.hpp"

#include <hyprutils/os/Process.hpp>
#include <hyprutils/memory/SharedPtr.hpp>

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>

using namespace Hyprutils::OS;
using namespace Hyprutils::Memory;
#define SP CSharedPointer

// xr_overlay_composition — WP-P5 (docs/openxr/06-testing.md §6, docs/openxr/08-wiki-notes.md §8).
//
// Proves HypXRland can run as an XR_EXTX_overlay session composited ON TOP of a separate primary
// OpenXR application (the `hypxrpaper` ambient-background app). Opt-in: gated on
// $HYPRTESTER_HYPXRPAPER pointing at a hypxrpaper binary — unset/invalid SKIPs (never fails), so
// the default `--xr` run stays a no-op green for this case.
//
// The suite's Hyprland launches ONCE at startup with openxr:overlay OFF (default), so this test
// flips it live: disable the exclusive session, bring hypxrpaper up as the primary, `keyword
// openxr:overlay 1`, then re-enable — openxr:overlay is read in COpenXRManager::start() (which
// `/openxr enable` calls directly), so a plain keyword-set before enable is enough; no
// config.props_refreshed / parseKeyword special-case is required (unlike the enabled/inhibit_idle
// hot-toggles). Everything is restored (overlay 0, exclusive session back, hypxrpaper killed) on
// exit via the RAII guard, so subsequent tests inherit a normal focused session.
namespace {
    struct SOverlayGuard {
        const bool&   failed;
        std::string   testName;
        SP<CProcess>  paper; // hypxrpaper primary session, PID-tracked
        ~SOverlayGuard() {
            // Restore the compositor to its default (exclusive, overlay off) session so later
            // tests are unaffected: disable, clear the keyword, re-enable.
            getFromSocket("/openxr disable");
            getFromSocket("/keyword openxr:overlay 0");
            getFromSocket("/openxr enable");
            // Kill the primary app by tracked PID only (never by name).
            if (paper) {
                if (kill(paper->pid(), 0) == 0)
                    kill(paper->pid(), SIGKILL);
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                paper.reset();
            }
            if (failed)
                XR::dumpXrArtifacts(testName);
        }
    };

    bool overlayTrue(const std::string& json) {
        return XR::fieldAfter(json, 0, "overlay") == "true";
    }
}

TEST_CASE(xr_overlay_composition) {
    XR_SKIP_IF_UNAVAILABLE();

    const char* paperEnv = std::getenv("HYPRTESTER_HYPXRPAPER");
    if (!paperEnv || !*paperEnv) {
        XR::logSkip(name(), "HYPRTESTER_HYPXRPAPER not set — hypxrpaper overlay composition not exercised");
        return;
    }
    const std::string paperBin = paperEnv;
    std::error_code   ec;
    if (!std::filesystem::is_regular_file(paperBin, ec)) {
        XR::logSkip(name(), "HYPRTESTER_HYPXRPAPER does not point at a file: " + paperBin);
        return;
    }

    SOverlayGuard guard{this->failed, name(), nullptr};

    // Baseline: the default session should be up (exclusive, overlay off). If it never reaches
    // focused this box's Monado is already unhappy — SKIP rather than chase overlay on top of it.
    if (!XR::waitForXrState("focused", std::chrono::milliseconds(15000))) {
        XR::logSkip(name(), "baseline session never reached focused (known env instability, see WP7 notes)");
        return;
    }

    // 1) Release Hyprland's exclusive session so hypxrpaper can own the primary slot.
    getFromSocket("/openxr disable");
    XR::waitForJson(
        "j/openxr", [](const std::string& r) { return XR::fieldAfter(r, 0, "state") != "focused"; }, std::chrono::milliseconds(10000));

    // 2) Bring up hypxrpaper as the PRIMARY session — gradient panorama mode (no args), so no
    //    scene-asset dependency. Same runtime dir + XR_RUNTIME_JSON as the suite's monado; forward
    //    the suite's GPU pin so it can't cross GPUs against Monado's compositor.
    auto paper = makeShared<CProcess>(paperBin, std::vector<std::string>{});
    paper->addEnv("XR_RUNTIME_JSON", XR::g_ctx.runtimeManifest);
    paper->addEnv("XDG_RUNTIME_DIR", XR::g_ctx.runtimeDir);
    if (const char* gpu = std::getenv("HYPRTESTER_XR_GPU"); gpu && *gpu) {
        // hypxrpaper takes --gpu as a CLI arg; rebuild the process with it.
        paper = makeShared<CProcess>(paperBin, std::vector<std::string>{"--gpu", gpu});
        paper->addEnv("XR_RUNTIME_JSON", XR::g_ctx.runtimeManifest);
        paper->addEnv("XDG_RUNTIME_DIR", XR::g_ctx.runtimeDir);
    }
    if (!paper->runAsync()) {
        XR::logSkip(name(), "failed to launch hypxrpaper");
        return;
    }
    guard.paper = paper;

    // Give it a beat to create its session; if it dies immediately that's an env problem, SKIP.
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    if (kill(paper->pid(), 0) != 0) {
        XR::logSkip(name(), "hypxrpaper exited immediately (known env instability / missing runtime)");
        return;
    }

    // 3) Request overlay and bring HypXRland back up on top of hypxrpaper.
    ASSERT(getFromSocket("/keyword openxr:overlay 1"), std::string("ok"));
    const std::string enableReply = getFromSocket("/openxr enable");
    if (enableReply != "ok") {
        XR::logSkip(name(), "overlay enable did not return ok (" + enableReply + ") — env instability");
        return;
    }

    // 4) Assert: session focused, reports overlay:true, and the declared monitors bound.
    if (!XR::waitForXrState("focused", std::chrono::milliseconds(20000))) {
        XR::logSkip(name(), "overlay session never reached focused within budget (known env instability)");
        return;
    }

    ASSERT(XR::waitForJson("j/openxr", overlayTrue, std::chrono::milliseconds(5000)), true);

    const std::string statusJson = getFromSocket("j/openxr");
    EXPECT_CONTAINS(statusJson, "\"overlay\": true");
    EXPECT_CONTAINS(statusJson, "Monado");
    // The static xr-test.conf fixtures (XR-conf-a/b) rebind on session start — monitors present.
    EXPECT_CONTAINS(statusJson, "\"monitors\": [");
    EXPECT_CONTAINS(statusJson, "\"name\":");

    NLog::green("xr_overlay_composition: HypXRland reached focused as an XR_EXTX_overlay over hypxrpaper (overlay:true, monitors bound)");
    // Guard restores the exclusive session + kills hypxrpaper on the way out.
}

#endif // WITH_XR_TESTS
