#include "tests.hpp"

#ifdef WITH_XR_TESTS

#include "../../hyprctlCompat.hpp"
#include "../../Log.hpp"
#include "../../xr/xr_helpers.hpp"

#include <filesystem>
#include <string>
#include <system_error>

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

// Test — xr_gpu_mismatch_fails_closed: pointing openxr:gpu at a GPU that is NOT the one the
// runtime composites on used to take the WHOLE compositor down — the runtime imports cross-GPU
// buffers at xrCreateSwapchain and hard-crashes inside the graphics driver (radeonsi
// driUnbindContext SEGV, coredumps 8986/39318). The runtime-GPU probe (XR_KHR_vulkan_enable2)
// now catches the mismatch at start() and fails closed: `enable` reports the runtime unavailable,
// the manager lands in "unavailable", and the desktop session keeps running.
//
// Requirements to actually exercise it (SKIP otherwise, so single-GPU CI stays green):
//   * a runtime is present (XR_SKIP_IF_UNAVAILABLE),
//   * the probe determined the runtime's GPU (status.runtimeGpu non-empty) — WITHOUT a working
//     probe, forcing the wrong GPU would reintroduce the very crash we're guarding, so we refuse
//     to try,
//   * openxr:gpu is pinned to a render node, and a SECOND distinct render node exists to force.
TEST_CASE(xr_gpu_mismatch_fails_closed) {
    XR_SKIP_IF_UNAVAILABLE();

    SArtifactGuard guard{this->failed, name()};

    // The probe must be able to name the runtime's GPU, or forcing a wrong pin is unsafe.
    const std::string status0   = getFromSocket("j/openxr");
    const std::string runtimeGpu = XR::fieldAfter(status0, 0, "runtimeGpu");
    if (runtimeGpu.empty()) {
        XR::logSkip(name(), "runtime GPU could not be probed (no XR_KHR_vulkan_enable2 / Vulkan); forcing a wrong GPU would risk the crash");
        return;
    }

    // The currently-pinned (working) GPU is, by definition, the runtime's node — that's why the
    // suite is up. Any OTHER existing render node is a guaranteed cross-GPU mismatch.
    const std::string optJson   = getFromSocket("j/getoption openxr:gpu");
    const std::string currentGpu = XR::fieldAfter(optJson, 0, "str");
    if (currentGpu.find("/dev/dri/render") == std::string::npos) {
        XR::logSkip(name(), "openxr:gpu is not pinned to a render node; cannot derive a deterministically-wrong GPU");
        return;
    }

    std::string     wrongGpu;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator("/dev/dri", ec)) {
        const std::string p = e.path().string();
        if (p.find("/renderD") != std::string::npos && p != currentGpu) {
            wrongGpu = p;
            break;
        }
    }
    if (wrongGpu.empty()) {
        XR::logSkip(name(), "only one DRM render node present; cross-GPU mismatch is not reproducible on this box");
        return;
    }

    NLog::log("xr_gpu_mismatch_fails_closed: runtime GPU {} (pinned {}), forcing wrong GPU {}", runtimeGpu, currentGpu, wrongGpu);

    // Force the wrong GPU and restart the session. Pre-fix, this crashed the whole compositor;
    // the assertion is that we now survive and fail closed.
    getFromSocket("/keyword openxr:gpu " + wrongGpu);
    getFromSocket("/openxr disable");
    XR::waitForXrState("disabled", std::chrono::milliseconds(5000));

    const std::string enableReply = getFromSocket("/openxr enable");
    // Guard fired: the runtime-GPU probe rejected the cross-GPU pin (start() returns UNAVAILABLE).
    EXPECT_CONTAINS(enableReply, "unavailable");
    ASSERT(XR::waitForXrState("unavailable", std::chrono::milliseconds(10000)), true);

    // The whole point: the compositor is fully alive after refusing the bad GPU.
    EXPECT_CONTAINS(getFromSocket("j/monitors"), "\"activeWorkspace\"");
    EXPECT_CONTAINS(getFromSocket("j/openxr"), "\"state\": \"unavailable\"");

    // Restore the good GPU and bring XR back up so the rest of the group is unaffected.
    getFromSocket("/keyword openxr:gpu " + currentGpu);
    getFromSocket("/openxr disable");
    XR::waitForXrState("disabled", std::chrono::milliseconds(5000));
    getFromSocket("/openxr enable");
    // Best-effort recovery (timing varies); don't hard-fail the test on the restore leg.
    if (!XR::waitForXrState("focused", std::chrono::milliseconds(15000)))
        XR::waitForXrState("visible", std::chrono::milliseconds(3000));
}

#endif // WITH_XR_TESTS
