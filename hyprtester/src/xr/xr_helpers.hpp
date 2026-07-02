#pragma once

#ifdef WITH_XR_TESTS

#include <chrono>
#include <functional>
#include <string>

class CRemoteClient;

using namespace std::chrono_literals;

namespace XR {

    // Process-wide XR test context, populated by main() before the suite runs.
    struct SXrContext {
        bool           available       = false; // orchestrator up AND remote wire validated
        bool           wireMismatch    = false; // vendored header vs service ABI drift (§4.2)
        bool           runtimeProvided = false; // XR_RUNTIME_JSON was passed to Hyprland-under-test
        std::string    skipReason;           // human-readable reason when !available
        std::string    runtimeDir;           // the isolated, shared XDG_RUNTIME_DIR
        std::string    monadoLog;            // <run-dir>/monado.log
        std::string    runId;                // xr-<pid>-<unixtime>
        CRemoteClient* remote = nullptr;     // shared, connected client (null if unavailable)
    };

    inline SXrContext g_ctx;

    // Returns true if the suite cannot run and the caller should SKIP; fills reason.
    // xr_runtime_absent is the one test exempt from this (it wants the unavailable path).
    bool shouldSkip(std::string& reasonOut);

    // Emit a TAP-style skip line (counts as a pass in the summary).
    void logSkip(const std::string& testName, const std::string& reason);

    // Poll getFromSocket(cmd) until pred(reply) or timeout. cmd is a raw socket
    // command, e.g. "j/openxr".
    bool waitForJson(const std::string& cmd, std::function<bool(const std::string&)> pred, std::chrono::milliseconds timeout = 5000ms, std::chrono::milliseconds interval = 100ms);

    // Specialization: the "state" field of the openxr status JSON reaches `state`.
    bool waitForXrState(const std::string& state, std::chrono::milliseconds timeout = 10000ms);

    // Unique per-run monitor name: XR-t<pid>-<n>.
    std::string monitorName(int n);

    // Dump monitors.json / openxr.json / monado.log tail / hyprland.log tail into
    // hyprtester/artifacts/<run-id>/<testName>/. Called on failure paths only.
    void dumpXrArtifacts(const std::string& testName);

} // namespace XR

#endif // WITH_XR_TESTS
