#include "tests.hpp"

#ifdef WITH_XR_TESTS

#include "../../hyprctlCompat.hpp"
#include "../../Log.hpp"
#include "../../xr/xr_helpers.hpp"

#include <string>

namespace {
    // RAII: dump artifacts iff the test ended up failed (docs §5.2).
    struct SArtifactGuard {
        const bool& failed;
        std::string testName;
        ~SArtifactGuard() {
            if (failed)
                XR::dumpXrArtifacts(testName);
        }
    };
}

// Test 1 — xr_session_up: the session reaches focused (or visible per the §6
// caveat) and reports Monado as the runtime.
TEST_CASE(xr_session_up) {
    XR_SKIP_IF_UNAVAILABLE();

    SArtifactGuard guard{this->failed, name()};

    // Primary gate: the session should reach FOCUSED against Monado's null
    // compositor + remote driver (WP2 verified this is reachable). Fall back to
    // documenting a VISIBLE-only plateau if FOCUSED is never seen.
    const bool reachedFocused = XR::waitForXrState("focused", std::chrono::milliseconds(20000));
    if (reachedFocused)
        NLog::green("xr_session_up: session reached FOCUSED");
    else {
        const bool reachedVisible = XR::waitForXrState("visible", std::chrono::milliseconds(2000));
        if (reachedVisible)
            NLog::red("xr_session_up: session plateaued at VISIBLE (never FOCUSED) — input tests must gate on visible; update docs §6");
        // Assert on focused: WP10's job is to verify FOCUSED is reachable.
        ASSERT(reachedFocused, true);
    }

    // Settled status must name Monado as the runtime.
    const std::string statusJson = getFromSocket("j/openxr");
    EXPECT_CONTAINS(statusJson, "\"runtimeName\"");
    EXPECT_CONTAINS(statusJson, "Monado");
    // runtimeName must be non-empty (i.e. not the unavailable empty-string).
    EXPECT_NOT_CONTAINS(statusJson, "\"runtimeName\": \"\"");
}

// Test 11 — xr_runtime_absent: openxr:enabled=1 but no runtime manifest present
// => graceful "unavailable"; enable returns a clean error; compositor still works.
// This exercises the no-runtime path, so it runs precisely when Hyprland-under-test
// was launched WITHOUT XR_RUNTIME_JSON (the no-monado invocation, docs §2.1 step 2).
TEST_CASE(xr_runtime_absent) {
    if (XR::g_ctx.runtimeProvided) {
        // A runtime is active in this invocation; the graceful-absent path can't be
        // exercised here (that's the with-monado run where xr_session_up is the gate).
        XR::logSkip(name(), "XR runtime present in this invocation; runtime-absent path not exercised");
        return;
    }

    SArtifactGuard guard{this->failed, name()};

    // No runtime => the manager must settle at "unavailable" (not crash, not hang).
    ASSERT(XR::waitForXrState("unavailable", std::chrono::milliseconds(10000)), true);

    const std::string statusJson = getFromSocket("j/openxr");
    EXPECT_CONTAINS(statusJson, "\"state\": \"unavailable\"");
    // Runtime fields empty in the unavailable state.
    EXPECT_CONTAINS(statusJson, "\"runtimeName\": \"\"");

    // enable must return a clean error string (not "ok", not a crash).
    const std::string enableReply = getFromSocket("/openxr enable");
    EXPECT_NOT(enableReply, std::string("ok"));
    EXPECT_NOT(enableReply, std::string("")); // empty reply would mean the IPC died
    EXPECT_CONTAINS(enableReply, "unavailable");

    // Still unavailable, no crash, after the failed enable.
    ASSERT(XR::waitForXrState("unavailable", std::chrono::milliseconds(3000)), true);

    // Compositor fully functional: a trivial non-XR IPC assertion. (Backend-agnostic:
    // the monitor is HEADLESS-1 under the headless backend, WAYLAND-1 when nested.)
    const std::string monitorsJson = getFromSocket("j/monitors");
    EXPECT_CONTAINS(monitorsJson, "\"activeWorkspace\"");
}

#endif // WITH_XR_TESTS
