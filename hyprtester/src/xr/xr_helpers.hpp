#pragma once

#ifdef WITH_XR_TESTS

#include <chrono>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

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
        std::string    runtimeManifest;      // XR_RUNTIME_JSON handed to Hyprland (and any XR client the tests spawn)
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

    // How many distinct PHYSICAL GPUs this process can reach through /dev/dri, counted the way the
    // compositor's cross-GPU decision counts them (DRM::sameGpu → drmDevicesEqual): every node is
    // collapsed onto its sysfs parent device, so one GPU's card node and render node count ONCE.
    // < 2 ⇒ no cross-GPU path exists here and the tests that pin cross-GPU behaviour must SKIP.
    size_t drmGpuCount();

    // Dump monitors.json / openxr.json / monado.log tail / hyprland.log tail into
    // hyprtester/artifacts/<run-id>/<testName>/. Called on failure paths only.
    void dumpXrArtifacts(const std::string& testName);

    // ---- tiny ad-hoc JSON scraping (WP11) --------------------------------------------------
    // hyprtester has no JSON library. These handle exactly the flat shapes hyprctl's status
    // JSON produces (quoted strings, bracketed float arrays, bare numbers/bools) — good enough
    // for polling/asserting on specific fields without pulling in a dependency. Not a general
    // parser: don't feed it arbitrary JSON.

    // Byte offset of `marker` at/after `from` in `hay`, or std::string::npos.
    size_t findAfter(const std::string& hay, const std::string& marker, size_t from = 0);

    // The raw value text following `"key":` at/after `from` (scope the search by passing the
    // offset of an enclosing marker, e.g. a `"name": "..."` block start). Quoted strings are
    // returned unquoted; bracketed arrays are returned with brackets (see parseFloatArray);
    // bare numbers/bools are trimmed. Empty string if `key` isn't found at/after `from`.
    std::string fieldAfter(const std::string& json, size_t from, const std::string& key);

    // Parse a "[a, b, c]" array (as returned by fieldAfter for pos:/quat: fields) into floats.
    std::vector<float> parseFloatArray(const std::string& arr);

    // std::stof that returns `fallback` instead of throwing on bad input.
    float toFloatOr(const std::string& s, float fallback);

} // namespace XR

#endif // WITH_XR_TESTS
