#include "tests.hpp"

#ifdef WITH_XR_TESTS

#include "../../hyprctlCompat.hpp"
#include "../../Log.hpp"
#include "../../xr/xr_helpers.hpp"

#include <chrono>
#include <string>

namespace {
    bool gateUp() {
        if (XR::waitForXrState("focused", std::chrono::milliseconds(15000)))
            return true;
        return XR::waitForXrState("visible", std::chrono::milliseconds(2000));
    }
}

// xr_disable_teardown — WP11 (doc 06 §6 row 10). `hyprctl openxr disable` must stop the session
// cleanly (no crash) and, with `openxr:destroy_monitors_on_stop = 1` (the research/18 option-a
// escape hatch — default is 0 since monitors_follow_session landed, so this test sets it
// explicitly), destroy every XR-backed headless output — both a runtime-created one and the
// config-declared fixtures. `hyprctl openxr enable` must bring the session back up and
// re-materialize the declared fixtures (lazy quad binding, doc 02); the runtime-created one is
// gone for good (it was never in the declared set). The default keep-but-unplug disposition is
// covered by xr_plugged_follow_session (plugged.cpp).
TEST_CASE(xr_disable_teardown) {
    XR_SKIP_IF_UNAVAILABLE();

    struct SGuard {
        const bool& failed;
        std::string testName;
        ~SGuard() {
            // Restore the default stop disposition, then always try to bring the session back
            // up for any XR tests that run after this one in the same shared Hyprland+Monado
            // instance (docs/openxr/06-testing.md §2.1 — one instance for the whole --xr run).
            getFromSocket("/keyword openxr:destroy_monitors_on_stop 0");
            getFromSocket("/openxr enable");
            XR::waitForXrState("focused", std::chrono::milliseconds(15000));
            if (failed)
                XR::dumpXrArtifacts(testName);
        }
    };
    SGuard guard{this->failed, name()};

    if (!gateUp()) {
        XR::logSkip(name(), "session never reached focused/visible (known env instability)");
        return;
    }

    // Opt into option-a full removal (read at stop() time; a plain keyword-set suffices).
    ASSERT(getFromSocket("/keyword openxr:destroy_monitors_on_stop 1"), std::string("ok"));

    const std::string mon = XR::monitorName(15);
    ASSERT(getFromSocket("/openxr create " + mon + " 1280x720"), std::string("ok"));
    ASSERT(XR::waitForJson(
               "j/openxr", [&](const std::string& r) { return r.contains("\"name\": \"" + mon + "\""); }, std::chrono::milliseconds(10000)),
           true);

    ASSERT(getFromSocket("/openxr disable"), std::string("ok"));
    ASSERT(XR::waitForXrState("disabled", std::chrono::milliseconds(10000)), true);

    // destroy_monitors_on_stop = 1 (set above) -> every XR output (runtime + declared) is gone.
    ASSERT(XR::waitForJson(
               "j/monitors", [&](const std::string& r) { return !r.contains("\"name\": \"" + mon + "\""); }, std::chrono::milliseconds(10000)),
           true);
    EXPECT_NOT_CONTAINS(getFromSocket("j/monitors"), "\"name\": \"XR-conf-a\"");
    EXPECT_NOT_CONTAINS(getFromSocket("j/monitors"), "\"name\": \"XR-conf-b\"");

    // Compositor still alive and responsive — the whole point of the full session state machine.
    EXPECT_CONTAINS(getFromSocket("j/monitors"), "\"activeWorkspace\"");

    ASSERT(getFromSocket("/openxr enable"), std::string("ok"));
    ASSERT(XR::waitForXrState("focused", std::chrono::milliseconds(15000)) || XR::waitForXrState("visible", std::chrono::milliseconds(2000)), true);

    // Declared fixtures re-materialize with quads bound; the runtime-created monitor is gone
    // for good (it was never declared, so nothing recreates it).
    ASSERT(XR::waitForJson(
               "j/openxr",
               [&](const std::string& r) { return r.contains("\"name\": \"XR-conf-a\"") && r.contains("\"name\": \"XR-conf-b\""); },
               std::chrono::milliseconds(10000)),
           true);
    EXPECT_NOT_CONTAINS(getFromSocket("j/openxr"), "\"name\": \"" + mon + "\"");

    NLog::green("xr_disable_teardown: disable destroyed all XR outputs cleanly; enable rebound the declared fixtures");
}

#endif // WITH_XR_TESTS
