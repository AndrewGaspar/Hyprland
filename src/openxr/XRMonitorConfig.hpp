#pragma once

// Deliberately compiles unconditionally (no OpenXR headers, no HAVE_OPENXR guard) so the
// config/parser layer can live outside the gate and hyprland_gtests can always exercise it
// (docs/openxr/07-roadmap.md conventions; parser tests in tests/xr/parser.cpp).

#include <cstdint>
#include <string>
#include <optional>
#include <expected>
#include <vector>

#include "../helpers/math/Math.hpp"
#include "XRAnchor.hpp" // OpenXR::SXRAnchorState + eXRAnchorMode/eXRHand (unconditional pure math)

namespace OpenXR {
    // Environment blend mode (doc 01). A HAVE_OPENXR-free mirror of XrEnvironmentBlendMode so the
    // selection logic (pickBlendMode) is a pure, unconditionally-compiled function that
    // hyprland_gtests can exercise without a runtime. The guarded session code converts to/from
    // the real XrEnvironmentBlendMode enum (xrBlendModeToXr / xrBlendModeFromXr in XRSession.hpp).
    enum eXRBlendMode : uint8_t {
        XR_BLEND_OPAQUE = 0, // composite over black — the classic VR "floating in a void" look
        XR_BLEND_ALPHA,      // composite over the runtime's passthrough underlay via layer alpha
        XR_BLEND_ADDITIVE,   // additive (optical see-through / additive displays)
    };

    // "opaque" | "alpha" | "additive" — the config/IPC string form (doc 05).
    std::string blendModeToString(eXRBlendMode mode);

    // Result of pickBlendMode: the chosen mode plus whether the user's explicit request could not
    // be honored (so the caller can emit the unsupported->fallback WARN — doc 01).
    struct SXRBlendModePick {
        eXRBlendMode mode                 = XR_BLEND_OPAQUE;
        bool         requestedUnsupported = false;
    };

    // Pure blend-mode selection (doc 01). `supported` is the runtime's advertised list in
    // preference order (xrEnumerateEnvironmentBlendModes returns preferred-first). `config` is the
    // openxr:blend_mode value: "auto" (or anything unrecognized) => the runtime's first-listed
    // (preferred) mode; an explicit "opaque"/"alpha"/"additive" is honored iff supported, else it
    // falls back to the preferred mode with requestedUnsupported=true. An empty supported list
    // (spec-illegal, but defended) yields XR_BLEND_OPAQUE.
    SXRBlendModePick pickBlendMode(const std::vector<eXRBlendMode>& supported, const std::string& config);

    // openxr:monitors_follow_session mode (research/18 + report-18 addendum). Governs WHEN
    // XR-created monitors behave like a plugged external display.
    enum eXRMonitorFollowMode : uint8_t {
        XR_FOLLOW_OFF = 0,  // never unplug — always-present (the pre-feature behavior). Legacy 0/false/no.
        XR_FOLLOW_SESSION,  // plug while an OpenXR session EXISTS (start()..stop()). Legacy 1/true/yes.
        XR_FOLLOW_VISIBLE,  // plug only while the session is VISIBLE/FOCUSED — a doffed/standby headset
                            // (WiVRn keeps a session alive on the shelf) reads as unplugged. Default.
    };

    // Parse the openxr:monitors_follow_session config string to the mode. Accepts the new
    // "off"|"session"|"visible" spellings AND the legacy boolean spellings for config compat:
    // 0/false/no/off -> OFF, 1/true/yes/session -> SESSION, visible/focused -> VISIBLE. Anything
    // unrecognized (including empty) -> VISIBLE (the default). Case/whitespace-insensitive. Pure.
    eXRMonitorFollowMode parseMonitorFollowMode(const std::string& v);

    // Cross-GPU linear-buffer policy (research/17 addendum). When the XR EGL context lives on a
    // DIFFERENT physical GPU than the one the compositor allocates the headless output's buffers on
    // (hybrid: desktop on the AMD iGPU, WiVRn/runtime on the NVIDIA dGPU via openxr:gpu), the XR
    // context cannot import the AMD-vendor-tiled buffers — NVIDIA's EGL rejects the foreign tiling
    // modifier with EGL_BAD_ATTRIBUTE and the quad goes black. The multi-GPU-standard fix is to make
    // that output's swapchain allocate DRM_FORMAT_MOD_LINEAR buffers (importable by any GPU). This
    // mirrors openxr:force_linear = auto | on | off.
    enum eForceLinearMode : uint8_t {
        XR_LINEAR_AUTO = 0, // force linear iff a cross-GPU split is positively detected (default)
        XR_LINEAR_ON,       // always force linear on XR-bound outputs
        XR_LINEAR_OFF,      // never force (leave native tiling; for debugging / same-GPU-only setups)
    };
    // "auto" (or anything unrecognized) => XR_LINEAR_AUTO; "on"/"true"/"1" => ON; "off"/"false"/"0" => OFF.
    eForceLinearMode parseForceLinearMode(const std::string& s);

    // Pure decision (tests/xr/force_linear.cpp): should the XR-bound headless output allocate LINEAR
    // buffers? `*Valid` say whether each DRM node's device numbers are known. AUTO forces linear only
    // when BOTH nodes are known AND they differ (a genuine cross-GPU split) — an unknown node stays
    // native (same-GPU is the common case and linear costs compositing throughput). ON/OFF ignore the
    // nodes. No OpenXR/aquamarine types so hyprland_gtests can exercise it without a runtime.
    bool shouldForceLinear(eForceLinearMode mode, bool xrValid, int64_t xrMajor, int64_t xrMinor, bool allocValid, int64_t allocMajor, int64_t allocMinor);

    // Plugged-state policy (research/18 + report-18/19 addenda — XR monitors behave like unplugged
    // external monitors while the headset is not being worn). Pure and unconditional so
    // hyprland_gtests can exercise it (tests/xr/plugged.cpp). Instantaneous predicate — the anti-flap
    // grace and the first-plug blip guard are the caller's concern (see xrDeferFirstPlug).
    //   OFF     => always plugged.
    //   SESSION => sessionUp (a session exists; WiVRn-on-the-shelf counts).
    //   VISIBLE => gate on the CONJUNCTION of BOTH available real signals (report-20 issue D):
    //     * sessionVisible must be true. WiVRn drops VISIBLE->SYNCHRONIZED reliably on doff, so
    //       visibility is the reliable doff signal even when presence sticks 'present' in standby.
    //     * AND, when presenceSupported (XR_EXT_user_presence advertised AND supported): userPresent,
    //       but only once presenceKnown (>=1 presence event seen) — before the first event we read as
    //       ABSENT so the session-create sprint never plugs.
    //     * otherwise (no presence ext): visibility alone is the signal.
    //   Requiring BOTH means a doff (visibility drop) unplugs even if presence is stuck, and a
    //   presence-absent unplugs even if a stale VISIBLE bit lingers — the fix for the never-unplugs
    //   standby bug. The first-plug settle (xrDeferFirstPlug) still guards the create-time blip.
    bool wantXRMonitorsPlugged(eXRMonitorFollowMode mode, bool sessionUp, bool sessionVisible, bool presenceSupported, bool presenceKnown, bool userPresent);

    // First-plug settle guard (report-20 issue D). Whether a would-be plug must be DEFERRED because it
    // is the first plug of the session and visibility has not yet been sustained past the session-start
    // blip window. Applies to the visibility side REGARDLESS of presence support: at session creation a
    // presence-capable runtime can report 'present' within a millisecond (indistinguishable from a
    // spurious blip), so requiring visibility to be sustained is the safety margin before the FIRST
    // plug. Any subsequent plug (everPlugged) never defers — later edges use the anti-flap grace.
    // Pure/gtestable; the caller supplies how long visibility has been continuously true.
    bool xrDeferFirstPlug(bool everPlugged, int64_t visibleSustainedMs, int64_t blipMs);

    // Re-probe backoff schedule (report-17 WP-L3 / report-20 issue B1). Milliseconds to wait before
    // the Nth consecutive re-probe of an absent runtime while dormant in UNAVAILABLE. `attempt` is
    // 0-based; doubling from baseMs, clamped to capMs. Pure/gtestable so the manager's dormant timer
    // (which reads config + owns the CEventLoopTimer) can rely on it. HEADSET-wait (runtime up, headset
    // undonned) uses a fixed gentle cadence instead and does not consult this.
    int64_t xrReprobeBackoffMs(int attempt, int64_t baseMs, int64_t capMs);

    // ---- event-driven re-probe: inotify watch-path derivation (don-the-headset dead-air fix) ----
    // Live evidence (boot 2026-07-14, instance c41d16e2*_1784014116): WiVRn's main server creates
    // and LISTENS on $XDG_RUNTIME_DIR/wivrn/comp_ipc at service startup (create_listen_socket in
    // wivrn server/main.cpp) — the socket exists long before the headset is donned, and it answers
    // IPC in a degraded "no headset" mode that advertises no EGL/GLES extensions (so probes fail at
    // the required-extension check while connect() succeeds). When the headset connects, the main
    // server FORKS the real compositor server, which INHERITS the listen socket — donning produces
    // ZERO filesystem events at the socket path. What the forked server DOES touch is the pid file:
    // monado u_process.c pidfile_open/pidfile_write on XRT_IPC_SERVICE_PID_FILENAME —
    // $XDG_RUNTIME_DIR/wivrn.pid for WiVRn (server/CMakeLists.txt), monado.pid for stock monado —
    // created on the first don of a boot (IN_CREATE) and truncated+rewritten on later dons
    // (pidfile_close leaves the file behind; IN_MODIFY). So the trigger set is sockets AND pid
    // files, and the manager's mask must include IN_MODIFY|IN_CLOSE_WRITE, not just IN_CREATE.
    //
    // A single directory to inotify-watch, plus the basenames whose events there mean something.
    // triggerNames: a create/move-in/modify of one of these = the runtime (or its real server) is
    //   materializing -> probe immediately.
    // subdirNames: creating one of these is a nested socket directory appearing -> start watching it
    //   too (its own socket lands inside a moment later; see xrReprobeWatchDirs for the pairing).
    struct SXRReprobeWatch {
        std::string              dir;          // absolute directory to watch
        std::vector<std::string> triggerNames; // basenames whose create/move-in/modify here triggers a probe
        std::vector<std::string> subdirNames;  // basenames whose creation here means "also watch dir/<name>"
    };

    // Derive the watch set from the value of $XDG_RUNTIME_DIR (pass the raw env string; may be empty).
    // Returns {} when runtimeDir is empty — without XDG_RUNTIME_DIR the socket location is unknown and
    // the caller falls back to the timer alone. Otherwise returns, in order:
    //   [0] $XDG_RUNTIME_DIR       triggers={"monado_comp_ipc","monado.pid","wivrn.pid"}, subdirs={"wivrn"}
    //   [1] $XDG_RUNTIME_DIR/wivrn triggers={"comp_ipc"}
    // Sources (all resolve via monado u_file_get_runtime_dir == $XDG_RUNTIME_DIR):
    //   - monado CMakeLists.txt: XRT_IPC_MSG_SOCK_FILENAME "monado_comp_ipc", XRT_IPC_SERVICE_PID_FILENAME "monado.pid"
    //   - WiVRn  server/CMakeLists.txt: XRT_IPC_MSG_SOCK_FILENAME "wivrn/comp_ipc", XRT_IPC_SERVICE_PID_FILENAME "wivrn.pid"
    // The [1] entry is emitted unconditionally (pure); the manager only inotify-adds it once the
    // directory exists, and adds it dynamically when the "wivrn" subdir creation event fires on [0].
    std::vector<SXRReprobeWatch> xrReprobeWatchDirs(const std::string& runtimeDir);

    // Pure predicates over a watch spec + an event basename, so the trigger decision is gtestable:
    bool xrReprobeTriggerMatch(const SXRReprobeWatch& w, const std::string& name); // name is a probe trigger
    bool xrReprobeSubdirMatch(const SXRReprobeWatch& w, const std::string& name);  // name is a nested dir -> watch it too

    // Debounce between a trigger event and the probe: the socket/server can exist a beat before it is
    // accept()ing usefully, so a probe fired the same instant may still fail. On a matching event the
    // manager resets the backoff to attempt 0 (so if the debounced probe DOES fail it falls back to
    // the FAST end of the schedule, not the grown delay) and arms this one-shot.
    inline constexpr int XR_REPROBE_WATCH_DEBOUNCE_MS = 150;

    // Backoff-policy fix (live-evidence bug 1): the next probe delay, replacing the raw backoff call.
    //   headsetWait     — HEADSET-class wait (xrGetSystem FORM_FACTOR_UNAVAILABLE, or the runtime was
    //                     reachable but lacked the required extensions — WiVRn's pre-don degraded
    //                     mode): fixed base cadence, never grows. This is the leg that turned the
    //                     live boot's dead-air into 30s: the degraded-mode failure was classified as
    //                     "no runtime" and the backoff grew to the cap.
    //   activityRecent  — relevant filesystem activity was seen in the watched dirs within
    //                     XR_REPROBE_ACTIVITY_WINDOW_MS: the runtime is materializing; poll hard
    //                     (base cadence) instead of honoring a grown backoff.
    //   otherwise       — the plain xrReprobeBackoffMs schedule.
    int64_t xrReprobeDelayMs(bool headsetWait, bool activityRecent, int attempt, int64_t baseMs, int64_t capMs);

    // How long after the last relevant watch event the backoff stays capped at the base interval.
    inline constexpr int64_t XR_REPROBE_ACTIVITY_WINDOW_MS = 60000;

    // The known runtime IPC socket paths under $XDG_RUNTIME_DIR ({} when runtimeDir is empty):
    //   $RT/monado_comp_ipc  and  $RT/wivrn/comp_ipc
    // Used by the manager to refine the degraded-runtime wait classification: WiVRn's CLIENT lib
    // answers xrEnumerateInstanceExtensionProperties with a degraded list even when NO server exists
    // (verified in an isolated $XDG_RUNTIME_DIR), so "enumerate answered" alone cannot distinguish
    // "service up, headset undonned" (poll at base forever) from "service not running" (back off).
    // A LISTENING server always has its socket on disk — stat'ing these is the missing bit.
    std::vector<std::string> xrRuntimeSocketPaths(const std::string& runtimeDir);

    // ---- session-loss hardening (freeze audit, 2026-07-14) ----
    // A live wivrn restart under a two-client session froze the desktop. The frame loop must never make
    // a BLOCKING xr call that cannot time out: if it wedges, the main thread's join() in stop() wedges
    // with it and the compositor stops painting the desktop. These bound every such wait.

    // Ceiling for xrWaitSwapchainImage. On a healthy runtime the image is ready ~immediately, so a wait
    // this long means the runtime is wedged/dying — we treat it as loss (drop the session, reprobe)
    // rather than block. NEVER XR_INFINITE_DURATION. Value is nanoseconds (XrDuration). 2s is far above
    // any normal jitter (won't false-drop on a Wi-Fi hiccup) yet bounds the worst-case frame-thread
    // stall — and therefore the worst-case main-thread join — to ~2s instead of forever.
    inline constexpr long long XR_SWAPCHAIN_WAIT_TIMEOUT_NS = 2'000'000'000LL;

    // Consecutive xrWaitFrame/xrBeginFrame hard-failures the frame loop tolerates before latching loss.
    // pollEvents at the top of each iteration normally classifies a dead runtime within one iteration;
    // this is the backstop for a failure code that isn't INSTANCE_LOST/SESSION_LOST, so the loop can
    // never busy-spin on a dead runtime forever.
    inline constexpr int XR_MAX_FRAME_FAIL_STREAK = 8;

    // Bounded wait (ms) for the reconnect handshake (xrCreateInstance + xrGetSystem) that start() runs
    // on a helper thread. The instant wivrn's socket reappears during a restart the event-driven
    // reprobe fires start(); if the just-appeared server is not yet answering, that FIRST-contact
    // handshake can block on the socket. Running it off the main thread with this ceiling keeps the
    // compositor painting; on timeout start() abandons the attempt (UNAVAILABLE) and reprobes later.
    // Generous so a healthy cold start never abandons.
    inline constexpr int XR_HANDSHAKE_TIMEOUT_MS = 5000;
}

namespace OpenXR {
    // WP5 unification: the parser now produces the canonical doc-03 SXRAnchorState directly
    // (absorbing WP4's placeholder SXRAnchorSpec). The parser stays pure/unconditional; it just
    // includes the equally-pure XRAnchor.hpp for the shared enums/state type.

    // Anchor mode -> the string used by doc 05 §4.3 (status/layout):
    // local|head|body|device:left|device:right.
    std::string anchorModeToString(eXRAnchorMode mode, eXRHand device);
    std::string anchorModeToString(const SXRAnchorState& state);

    // Inverse of parseXRMonitorLine (doc 05 §2.2 grammar / doc 03 §7 pose->text serialization
    // rules): produces one paste-ready `xrmonitor = ...` config line. Pure and unconditional so
    // it is shared by COpenXRManager::layoutDump() (the live `hyprctl openxr layout` path, which
    // resolves `pose` per doc 03 §7 — the anchor's live solved world pose for LOCAL, the
    // persistent stored offset for head/body/device) and by the round-trip unit test
    // (tests/xr/parser.cpp) that this line, reparsed through parseXRMonitorLine, reproduces an
    // equivalent SXRMonitorParams. `anchor.anchorPose` is ignored — `pose` is what gets printed.
    std::string serializeXRMonitorLine(const std::string& name, Vector2D resolution, std::optional<float> refreshHz, const SXRAnchorState& anchor, const SXRPose& pose,
                                        float sizeMeters);
}

// Parameters describing one XR monitor. Every create path (config keyword, dispatcher,
// hyprctl) funnels these into COpenXRManager::createXRMonitor (doc 02). Absent optionals fall
// back to headless/openxr:* defaults.
struct SXRMonitorParams {
    std::string             m_name;        // e.g. "XR-1"; must be unique
    std::optional<Vector2D> m_resolution;  // WxH pixel mode; absent => headless default (1920x1080)
    std::optional<float>    m_refreshRate; // @Hz part; absent => headless default (60)
    std::optional<float>    m_sizeMeters;  // quad width in meters; absent => *openxr:default_size

    // Parsed anchor as the canonical doc-03 state (WP5). anchorPose.rot is built from the
    // parsed yaw/pitch; widthMeters is left at its default here (seeded from m_sizeMeters /
    // openxr:default_size when the layer is created).
    OpenXR::SXRAnchorState m_anchor;
    // True iff an anchor-spec was explicitly given (config keyword always does; the create verb
    // may omit it, in which case the caller places the monitor along the current gaze, doc 05 §3.1).
    bool m_anchorProvided = false;
};

namespace OpenXR {
    // Pure parser for the xrmonitor config keyword value (doc 05 §2.2/§2.3):
    //   <name>, <mode>, <anchor-spec>[, <kv>]...
    // Returns the parsed params or a human-readable error. Compiled unconditionally.
    std::expected<SXRMonitorParams, std::string> parseXRMonitorLine(const std::string& args);

    // Pure parser for the `xrmonitor create` dispatcher / `hyprctl openxr create` verb (doc 05
    // §3.1): space-separated `<name> [WxH[@Hz]] [anchor-spec]`, with defaults applied by the
    // caller (mode defaults to 1920x1080@60, anchor defaults to anchor:local when absent).
    std::expected<SXRMonitorParams, std::string> parseXRMonitorCreateArgs(const std::string& args);
}
