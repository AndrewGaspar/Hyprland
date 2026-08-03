#pragma once
#ifdef HAVE_OPENXR

#include <array>
#include <cstdint>
#include <string>
#include <atomic>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../helpers/memory/Memory.hpp"
#include "../helpers/signal/Signal.hpp"
#include "../helpers/time/Time.hpp" // Time::steady_tp (presence blip window)
#include "../desktop/DesktopTypes.hpp" // PHLMONITOR (applyCrossGpuLinear)
#include "XRMonitorConfig.hpp"
#include "XRRule.hpp"     // situational transparency: SXRRule / SXRRuleContext / SXREffects (unguarded)
#include "XRLayout2D.hpp" // 2D-plane sync: the pure projection (report 12, unguarded)
#include "XRInput.hpp" // SXRInputEvent / SXRStateEvent / XRQueueItem / CXRQueue / CXRInput

struct wl_event_source;

namespace Aquamarine {
    class IOutput;
}

// Top-level lifecycle state of the OpenXR integration. Kept verbatim from
// docs/openxr/00-overview.md — do not reorder or renumber.
enum eXRManagerState : uint8_t {
    XR_STATE_DISABLED = 0,    // openxr:enabled == 0, or stopped; no XR objects exist
    XR_STATE_UNAVAILABLE,     // start attempted: no runtime / xrCreateInstance or system lookup failed,
                              // or instance loss. NO auto-retry polling — user re-enables explicitly.
    XR_STATE_STARTING,        // start() in progress on the main thread
    XR_STATE_RUNNING_IDLE,    // session exists; XrSessionState IDLE/READY/SYNCHRONIZED/STOPPING
    XR_STATE_RUNNING_VISIBLE, // XrSessionState VISIBLE — quads composited, pacing active
    XR_STATE_RUNNING_FOCUSED, // XrSessionState FOCUSED — input active, idle-inhibit active
    XR_STATE_STOPPING,        // stop() in progress: joining frame thread, tearing down
};

class CXRIpc;
class CXRSession;
class CXRGraphics;
class CXRMonitorLayer;
class CXRPointerDevice;
class CEventLoopTimer;

// XR monitor layers cross the frame thread via std::shared_ptr (atomic control block), NOT the
// codebase-standard hyprutils SP whose refcount is a plain int (see the thread-safety rule in
// XRMonitorLayer.hpp, where this alias is also defined identically for the layer TU). Declared
// here against the incomplete type so this lightweight header need not pull in the XR headers.
using PXRLAYER = std::shared_ptr<CXRMonitorLayer>;

// The single main-thread entry point the rest of Hyprland touches for OpenXR.
// Owns the lifecycle state machine and funnels the three enable/disable entry points
// (startup check, config hot-toggle, hyprctl) into start()/stop().
class COpenXRManager {
  public:
    COpenXRManager();
    ~COpenXRManager();

    // Constructed at STAGE_LATE (after XWayland). init() registers the config-reload
    // listeners + the hyprctl command, then honors openxr:enabled on startup.
    void init();

    // Idempotent main-thread lifecycle methods — all enable/disable paths funnel here.
    void               start();
    void               stop();

    eXRManagerState    state() const;
    const std::string& runtimeName() const;
    const std::string& systemName() const;
    // Human-readable description of the GPU the runtime composites on, as resolved by the
    // wrong-GPU probe at start() ("<device name> (drm <maj>:<min>)"), or empty when it could not
    // be determined (runtime lacks XR_KHR_vulkan_enable2, no Vulkan, etc.). Surfaced in status so
    // the user can see which GPU to point openxr:gpu at.
    const std::string& runtimeGpu() const;
    // The openxr:runtime_json override currently applied to the XR session (WP-XR1) — the OpenXR
    // runtime manifest path forced into XR_RUNTIME_JSON for this session, or empty when no override is
    // set (the loader default / active_runtime.json is used). Surfaced in `hyprctl openxr status` so
    // the XREAL flat/XR toggle can confirm which runtime the session handshook against.
    const std::string& runtimeJson() const;
    // Currently-active environment blend mode as "opaque"|"alpha"|"additive" (doc 05 §4.3). The
    // selected mode while a session exists; "opaque" (the default) otherwise.
    std::string        blendModeName() const;
    // Whether the current session is an XR_EXTX_overlay session (doc 01). Actual state, not the
    // openxr:overlay request — false when no session exists or the runtime lacked the extension.
    bool               isOverlay() const;

    // ---- luma-keyed transparency, "black-as-alpha" (openxr:black_alpha, report 09) ----
    // Live state for `hyprctl openxr status`: the CONFIGURED alpha-for-black, the EFFECTIVE one the
    // frame thread is applying (== configured while the blend mode shows through, 1.0 = off otherwise),
    // and the luma knee. Readable from any thread (plain atomics).
    struct SXRBlackAlpha {
        float configured = 1.F;
        float effective  = 1.F;
        float knee       = 0.1F;
        bool  active     = false; // effective < 1 -> the key is actually keying
        bool  gatedOff   = false; // configured < 1 but the blend mode is opaque -> ignored
    };
    SXRBlackAlpha blackAlphaStatus() const;

    // ---- situational per-monitor transparency: the `xrrule` engine (doc 05 §xrrule, report 09) ----
    // MAIN THREAD ONLY (rule evaluation touches config strings, compiled regexes and PHLWINDOW/
    // PHLMONITOR refcounts — none of which the frame thread may ever see).
    //
    // reloadXRRules() re-snapshots CConfigManager's declared rule list and re-evaluates; called from
    // onConfigReload() and from the dynamic `hyprctl keyword xrrule` path.
    // requestEffectEval() is the COALESCED re-evaluation entry point every trigger uses (focus /
    // fullscreen / title / anchor state / monitor add-remove / manual verb): it defers ONE evaluation
    // to the end of the current event-loop iteration, so a burst of events costs a single pass.
    void                             reloadXRRules();
    void                             requestEffectEval();
    // The deferred body requestEffectEval() schedules. Public only because the doLater callback
    // reaches it through g_pOpenXRManager (never a captured `this` — see requestEffectEval).
    void                             onEffectEvalDue();
    // `hyprctl openxr alpha <name|active> <0..1|auto>` / `blackalpha <name|active> <0..1|off|auto>`.
    // Sets (or clears, with `auto`) the manual override — the TOP precedence layer, sticky until
    // cleared, mirroring the shipped `handinput` manual-over-auto latch.
    std::expected<void, std::string> cmdAlpha(const std::string& args);
    std::expected<void, std::string> cmdBlackAlpha(const std::string& args);

    // Idle-inhibit predicate (doc 05 §6.1 + research/20 phase 2). MAIN THREAD ONLY — it reads the
    // STRING config value openxr:inhibit_idle, which must never happen off-main.
    // CInputManager::recheckIdleInhibitorStatus() is the sole writer of the inhibit bit; it consults
    // this before its final setInhibit(false). See OpenXR::wantXRIdleInhibit for the mode contract.
    bool                       shouldInhibitIdle();
    // The parsed openxr:inhibit_idle mode, and its "off"|"focused"|"present" string form for
    // `hyprctl openxr status`. Main thread only (string config read), same as shouldInhibitIdle().
    OpenXR::eXRIdleInhibitMode idleInhibitMode();
    std::string                idleInhibitModeName();

    // Monitor create/destroy funnel (main thread). createXRMonitor works in EVERY manager
    // state (including DISABLED) so monitors created without a session become plain headless
    // outputs whose quads bind lazily on start() (doc 02). WP3 exercises this with a single
    // hard-coded test monitor; the config keyword/dispatcher/hyprctl surfaces are WP4.
    std::expected<PXRLAYER, std::string> createXRMonitor(const SXRMonitorParams& params);
    void                                 destroyXRMonitor(const std::string& name);

    // Plugged-state applicator (research/18 WP-M1/M2): drive every XR-created monitor to the given
    // final `plugged` state. Unplugging drives CMonitor::onDisconnect() (full workspace evacuation),
    // plugging drives onConnect(true) (name-keyed workspace return) — the exact functions the rule
    // manager uses for `monitor=...,disable` toggles. Never destroys an output (that path is the
    // aquamarine framecb UAF, destroyOutputDeferred). Main thread only. Idempotent; records the
    // applied intent in m_monitorsPlugged so updateMonitorsPlugged() can reason about the grace edge.
    // Most callers want updateMonitorsPlugged() (which decides the target from mode+state); this is
    // the raw apply, also used by createXRMonitor's own per-monitor gate.
    void setMonitorsPlugged(bool plugged);

    // Plugged-state decision funnel (research/18 + report-18 addendum). Reads
    // openxr:monitors_follow_session (off|session|visible) and the current session facts, computes
    // the desired plugged state, and applies it via setMonitorsPlugged() — EXCEPT that under the
    // `visible` mode a VISIBLE->hidden drop (headset doffed/standby while the session persists) is
    // deferred by openxr:monitor_unplug_grace_ms so a doff-and-straight-back-on does not evacuate
    // workspaces. `allowGrace` enables that deferral (true only on the live session-state edge;
    // false forces the steady state immediately — start()/stop()/reload). Cancels any pending grace
    // timer whenever it settles the state. Main thread only. start()/stop()/onConfigReload() and the
    // frame->main session-state dispatch all funnel here.
    void updateMonitorsPlugged(bool allowGrace);

    // Milliseconds until the pending grace-period unplug fires, or -1 when none is armed. Cheap
    // read of the timer for `hyprctl openxr status` observability. Main thread only.
    int  monitorUnplugPendingMs() const;
    // The active openxr:monitors_follow_session mode as "off"|"session"|"visible" (status).
    std::string monitorFollowModeName() const;
    // `hyprctl openxr status` presence field (report-19): "unsupported" (no XR_EXT_user_presence /
    // device can't report it) | "unknown" (supported, no event yet) | "yes"/"no" (donned/doffed).
    std::string presenceStatusString() const;
    // `hyprctl openxr status` raw visibility field (report-20 issue D): "yes" while the session is
    // VISIBLE/FOCUSED, "no" while it exists but is not visible (doffed/standby), "n/a" with no session.
    // Surfaced alongside presence so the combined plug gate (needs both) is diagnosable in one command.
    std::string visibleStatusString() const;
    // `hyprctl openxr status` dormant re-probe field (report-17 WP-L3 / report-20 issue B1): while
    // UNAVAILABLE with a re-probe armed, "runtime" (no runtime yet) or "headset" (runtime up, headset
    // undonned); empty otherwise. reprobePendingMs(): ms until the next probe, or -1 when none armed.
    std::string reprobeWaitString() const;
    int         reprobePendingMs() const;
    // Whether the event-driven inotify watch on the runtime socket dir is currently armed (don-the-
    // headset dead-air fix). True only while dormant in UNAVAILABLE with openxr:reprobe_watch on and
    // $XDG_RUNTIME_DIR resolvable. Surfaced so `hyprctl openxr status` makes the behavior diagnosable.
    bool        reprobeWatchArmed() const;

    // --- IPC verb funnel (main thread). ONE implementation, two transports: the xrmonitor
    // dispatcher and the hyprctl openxr subcommands both call these (doc 05 §3/§4). Return
    // empty-expected on success, an error string otherwise. ---
    std::expected<void, std::string> cmdCreate(const std::string& args);    // runtime-owned monitor
    std::expected<void, std::string> cmdDestroy(const std::string& target); // <name>|active
    std::expected<void, std::string> cmdSelect(const std::string& arg);     // <name>|next|prev

    // --- pose-mutation verbs (WP5, doc 03 §5 / doc 05 §3.1). Main thread; take m_layersMu. ---
    std::expected<void, std::string> cmdAnchor(const std::string& args);   // <name|active> <mode-spec>
    std::expected<void, std::string> cmdMove(const std::string& args);     // <dx> <dy> <dz>
    std::expected<void, std::string> cmdRotate(const std::string& args);   // <dyaw> [dpitch]
    std::expected<void, std::string> cmdScale(const std::string& args);    // <f|+d|-d>
    std::expected<void, std::string> cmdDistance(const std::string& args); // <±m>
    std::expected<void, std::string> cmdCenter();                          // (none)
    // hypxrvoice GAP 2: place a named monitor at a resolved LOCAL_FLOOR point, re-anchoring to
    // `local` and MOVING the quad so its center sits at that point (facing the headset by default).
    // The point is exactly what `hyprctl openxr gaze` returns, so a voice daemon can drop a monitor
    // where the user was looking when they said "here". Syntax: <name|active> at <x>,<y>,<z>.
    std::expected<void, std::string> cmdPlace(const std::string& args);

    // --- adaptive anchoring verbs (research/13 §6.3). Main thread; take m_layersMu. Operate on the
    // selected monitor (like move/center). ---
    std::expected<void, std::string> cmdAdaptive(const std::string& args); // on|off|toggle
    std::expected<void, std::string> cmdDock(const std::string& args);     // [here]
    std::expected<void, std::string> cmdUndock();                          // (none)
    std::expected<void, std::string> cmdRoam(const std::string& args);     // head|body

    // --- gaze grab + conditional hand input (research/16). Main thread; take m_layersMu. ---
    // gazegrab with NO arg TOGGLES: grabs the dwell-stable gazed-at monitor, or releases the current
    // gaze carry. With a monitor name (hypxrvoice GAP 1) it begins a carry on the NAMED monitor
    // regardless of the live dwell candidate — a voice daemon resolves "this" at the WORD's timestamp
    // and grabs the correct monitor seconds later. "active" resolves the selected monitor.
    std::expected<void, std::string> cmdGazeGrab(const std::string& arg = ""); // (none) toggle | <name> targeted
    std::expected<void, std::string> cmdGazeRelease();                     // (none) — explicit release
    std::expected<void, std::string> cmdGazePush(const std::string& args); // <±m> (default gaze_dist_step)
    std::expected<void, std::string> cmdHandInput(const std::string& args); // on|off|auto|toggle

    // Snapshot of one XR monitor for `hyprctl openxr status` (doc 05 §4.3). Main thread.
    struct SXRMonitorInfo {
        std::string name;
        int64_t     id = -1;
        int         w = 0, h = 0; // current pixel mode (normal-format line only)
        float       refresh    = 0.f;
        float       sizeMeters = 1.6f;
        std::string anchorMode = "local";
        float       posX = 0.f, posY = 0.f, posZ = 0.f;
        float       quatX = 0.f, quatY = 0.f, quatZ = 0.f, quatW = 1.f;
        bool        grabbed  = false;
        std::string grabKind = "none"; // WP-G3: "none" | "move" | "resize" (which grab owns it)
        bool        hovered  = false;
        // report 14: the ray-hover REGION the frame thread last published for this quad — "none" |
        // "body" | "bar" | "corner-*" | "margin". Sticky-hover-stabilized; scriptable for tests.
        std::string region   = "none";
        bool        plugged  = false; // research/18: the headless output is currently enabled
        // WP-L2: which blit path last filled this layer's swapchain — "none" | "dmabuf" | "cpu" |
        // "black". "black" flags a silent black-quad session (e.g. cross-GPU import failure).
        std::string contentPath = "none";
        // Cross-GPU: true iff this output's buffers are allocated LINEAR so the XR GPU can import
        // them (set when the XR runtime GPU differs from the buffer allocator; openxr:force_linear).
        bool        linear = false;
        // Adaptive anchoring (research/13 §6.4).
        bool        adaptiveEnabled  = false;
        std::string adaptivePhase    = "docked"; // docked | undocking | roaming | redocking
        std::string adaptiveRoamMode = "body";   // head | body
        float       adaptiveSeatDist = 0.f;      // current XZ distance from the desk seat (m)
        float       adaptiveT        = 0.f;      // transition envelope parameter [0,1]
        // Situational transparency (doc 05 §xrrule). The LIVE (eased) values the frame loop is
        // applying, their resolved TARGETS, and the provenance of each — "default" | "rule" |
        // "manual" — so `hyprctl openxr status` answers "why is this monitor ghosted" outright.
        // anchorState is the state the rules matched against ("docked"|"follow"|"carried").
        float       fxAlpha          = 1.f;
        float       fxAlphaTarget    = 1.f;
        std::string fxAlphaSrc       = "default";
        float       fxBlackAlpha     = 1.f;
        float       fxBlackAlphaTarget = 1.f;
        std::string fxBlackAlphaSrc  = "default";
        float       fxKnee           = 0.1f;
        std::string fxKneeSrc        = "default";
        bool        fxTransitioning  = false;
        std::string anchorState      = "docked";
        // 2D-plane sync (report 12): the slot the projection assigned and the angles it used, so
        // `hyprctl openxr status` explains where the mouse will cross. source is
        // "auto" (the projection placed it) | "pinned" (an explicit user monitor= offset owns it) |
        // "off" (2D-plane sync disabled, or nothing placed yet — the historic append-right).
        std::string l2dSource = "off";
        int         l2dCol = 0, l2dRow = 0;
        double      l2dX = 0.0, l2dY = 0.0;
        float       l2dAzDeg = 0.f, l2dElDeg = 0.f;
    };
    std::vector<SXRMonitorInfo> monitorInfos();

    // ---- 2D-plane sync (report 12 WP-S2): derive the layout plane from the 3D arrangement ----
    // MAIN THREAD ONLY. requestLayout2DSync() is the DEBOUNCED entry point every trigger uses
    // (monitor add/remove/plug, grab RELEASE — never grab begin, adaptive dock/undock, the pose
    // verbs, config reload): it re-arms a single timer, so a burst of releases costs one relayout.
    // syncLayout2D() is the pass itself; force=true runs it even while frozen (the explicit verb).
    void                             requestLayout2DSync();
    void                             syncLayout2D(bool force = false);
    // Re-latch the "desk orientation" reference frame (§3a) from the current head pose: the frame
    // world-anchored monitors' azimuths are measured against. Turning your chair must NOT re-map
    // your mouse, so this happens only at well-defined moments (session start, `center`/recenter,
    // an explicit `sync-layout`), never per-frame. No-op without a valid tracked view.
    void                             latchLayout2DReference();
    // `hyprctl openxr sync-layout [freeze|thaw]` / `xrmonitor sync`.
    std::expected<void, std::string> cmdSyncLayout(const std::string& args);

    struct SXRLayout2DStatus {
        bool        enabled  = false;
        bool        frozen   = false;
        bool        refValid = false;
        float       refYawDeg = 0.f;
        int         placed = 0, rows = 0, width = 0, height = 0;
        std::string vertical = "elevation";
        std::string attach   = "right";
        float       pxPerDegree = 35.f;
    };
    SXRLayout2DStatus layout2DStatus();

    // WP-G5: per-hand active input device for `hyprctl openxr status`. Reads CXRInput's atomic
    // interaction-profile mirror (main-thread safe) + the openxr:hand_grab mode. `hands` is true
    // when that hand is on the ext/hand_interaction_ext profile; `gesture` is the hand_grab mode
    // string then (else empty). Both default to controller when no session/input exists.
    struct SXRHandInputInfo {
        bool        hands    = false;
        std::string gesture  = "";    // "pinch" | "grasp" | "both" when hands, else ""
        bool        filtered = false; // WP-G6: this hand's move-grabs run the 1€ carry filter
                                      // (openxr:grab_filter on AND hands active)
    };
    std::array<SXRHandInputInfo, 2> handInputInfos() const;

    // research/16 Part A: conditional hand-input policy state for `hyprctl openxr status`. `mode` is
    // the openxr:hand_input policy (on|off|auto, live-mutable via `xrmonitor handinput`); `state` is
    // the resolved gate: active | gated (keyboard) | gated (manual) | off. Readable on either thread.
    struct SXRHandInputStatus {
        std::string mode  = "auto";
        std::string state = "off";
    };
    SXRHandInputStatus handInputStatus() const;

    // research/16 Part B: gaze grab status for `hyprctl openxr status`.
    struct SXRGazeStatus {
        std::string source        = "view"; // openxr:gaze_source
        int64_t     hoveredMonitor = -1;    // dwell-stable candidate id (-1 = looking at passthrough)
        std::string hoveredName;            // resolved name of hoveredMonitor ("" if none)
        bool        carrying = false;       // a gaze carry is active
        std::string carryMonitor;           // its monitor name ("" if none)
        float       dist = 0.f;             // carry distance in meters (0 when not carrying)
    };
    SXRGazeStatus gazeStatus();

    // hypxrvoice GAP 3: the concrete monitor `active` resolves to right now — explicit select > last
    // ray-hovered > focused-if-XR (resolveSelected()->m_monitorName), or "" if none. Lets a voice
    // daemon name the target in feedback without replicating the resolution order. Main thread.
    std::string   selectedName();

    // hypxrvoice WP-V1 (docs/openxr/research/VOICE-CONTROL.md): a timestamped head-pose + gaze
    // candidate resolved from the rolling ring (m_poseRing). `gazeSampleNow()` returns the newest
    // sample; `gazeSampleAt(ms)` returns the nearest sample to a monotonic-clock timestamp so a
    // voice daemon can ask "where was the head pointed when the user started speaking?" (speech
    // recognition latency means pose-at-parse-time is a systematic bug). Names are resolved from
    // the stored MONITORID on THIS (main) thread. `ok` is false only when the ring is empty (no XR
    // frame has run). See the CLOCK CONTRACT comment on SXRPoseRing in XRMath.hpp.
    struct SXRGazeSample {
        bool        ok        = false; // a sample exists
        int64_t     timestampMs = 0;   // capture time (CLOCK_MONOTONIC ms)
        bool        viewValid = false; // head pose was locatable at capture
        OpenXR::Vec3 headPos;          // LOCAL_FLOOR meters
        OpenXR::Quat headRot;
        OpenXR::Vec3 headForward;      // -Z of headRot, unit
        int64_t     gazeMonitorId = -1;
        std::string gazeName;          // resolved name of gazeMonitorId ("" if none/gone)
        bool        selected = false;  // a dwell-stable monitor is currently gazed at
        float       dwell    = 0.F;    // seconds toward the pending dwell switch
        // hypxrvoice GAP 4: the gaze-ray/quad intersection on the selected monitor, LOCAL_FLOOR
        // meters — the same space `openxr place <name> at x,y,z` consumes, so a voice daemon can
        // feed it straight back. `hitValid` implies `selected`, but not the converse: a mid-dwell
        // sample still names the old monitor while the ray has already left its quad.
        bool         hitValid = false;
        OpenXR::Vec3 hitPoint;
        float        hitDist  = 0.F;   // meters along the gaze ray to hitPoint
        // Only meaningful for gazeSampleAt(): how the query resolved against the ring.
        bool        matched          = false; // resolved via a timestamp query (vs "now")
        int64_t     requestedTimestampMs = 0;
        int64_t     matchedTimestampMs   = 0; // the ring entry actually returned
        int64_t     ageMs                = 0; // requested - matched (>0 = matched sample is older)
    };
    SXRGazeSample gazeSampleNow();
    SXRGazeSample gazeSampleAt(int64_t requestedTimestampMs);

    // `hyprctl openxr layout`: paste-ready `xrmonitor = ...` lines for every live XR monitor.
    std::string layoutDump();

    // Reconcile the declared (`xrmonitor` keyword) set against live layers (doc 05 §2.5). Runs
    // from onConfigReload() and from init() so declared monitors materialize even while disabled.
    void reconcileDeclaredMonitors();

    // WP-G2: on a config reload, if the chrome GEOMETRY vars (enabled/margin/bar_height/
    // bar_width_frac/corner_size) changed, mark every layer's swapchain dirty so the frame thread
    // recreates it — the chrome margin px + m_chrome fractions are frozen at swapchain creation, so
    // this is what makes chrome_* hot-reloadable (the WP-G1 follow-up). Colors + fade/hide timings
    // are read per-frame and need no recreate. Main thread; takes m_layersMu.
    void markSwapchainsDirtyIfChromeChanged();

    // Re-check `openxr:enabled` (and other hot-live vars) against the current lifecycle state
    // and start()/stop() accordingly. Normally reached via the config.reloaded/props_refreshed
    // listeners registered in init() — but a bare `hyprctl keyword openxr:enabled 0/1` under the
    // legacy config parser fires neither event (see CConfigManager::parseKeyword's special-case
    // cluster), so that path calls this directly. Idempotent; safe to call redundantly.
    void onConfigReload();

    // "disabled" | "unavailable" | "starting" | "idle" | "visible" | "focused" | "stopping"
    static const char* stateToString(eXRManagerState state);

  private:
    void setState(eXRManagerState newState);

    // --- plugged-state grace timer (report-18 addendum). Main thread only. ---
    // Session facts derived from m_state for the plugged predicate.
    bool sessionExists() const;  // m_state ∈ {idle, visible, focused}
    bool sessionVisible() const; // m_state ∈ {visible, focused}
    // Arm/reschedule the one-shot grace timer to fire in `ms`; lazily created + added to the loop.
    void armUnplugTimer(int ms);
    // Disarm the grace timer without firing (keeps the timer object; removed only in stop()/dtor).
    void cancelUnplugTimer();
    // Grace-timer callback body: re-evaluate and unplug iff we still want unplugged.
    void onUnplugGraceExpired();

    // report-19 plug decision. monitorsShouldBePluggedNow(): the pure predicate (mode + session +
    // presence facts) AND the first-plug blip guard — the single source of truth for the plugged
    // target, shared by updateMonitorsPlugged(), createXRMonitor()'s per-monitor gate, and the timer
    // callbacks. visibleSustainedMs(): how long the session has been continuously VISIBLE (0 if not).
    bool          monitorsShouldBePluggedNow() const;
    int64_t       visibleSustainedMs() const;
    int           plugSettleMs() const; // openxr:monitor_plug_settle_ms, clamped >= 0
    // Fallback first-plug settle timer (mirrors the unplug grace timer).
    void          armPlugSettleTimer(int ms);
    void          cancelPlugSettleTimer();
    void          onPlugSettleExpired();
    // Reset the per-session presence/plug bookkeeping (start()/session end).
    void          resetPresenceState();

    // --- dormant re-probe (report-17 WP-L3 / report-20 issue B1). Main thread only. ---
    // Why the last start() attempt landed in UNAVAILABLE, so the re-probe can distinguish "no runtime
    // yet" (grow the backoff) from "runtime up, headset undonned" (gentle fixed cadence + status hint).
    enum eXRProbeWait : uint8_t {
        XR_WAIT_NONE = 0, // not waiting (not dormant, or reprobe disabled)
        XR_WAIT_RUNTIME,  // xrCreateInstance / system lookup failed -> waiting for the runtime/server
        XR_WAIT_HEADSET,  // xrGetSystem returned FORM_FACTOR_UNAVAILABLE -> runtime up, headset undonned
    };
    // Arm the re-probe timer (if UNAVAILABLE + openxr:enabled + openxr:reprobe) using the backoff for
    // the current attempt (RUNTIME) or the fixed cadence (HEADSET). Called on entering UNAVAILABLE.
    void          maybeArmReprobe();
    void          armReprobeTimer(int ms);
    // Disarm the re-probe timer. resetBackoff clears the consecutive-failure count (on a real start()
    // success / user disable); a mere STARTING transition disarms without resetting so the backoff
    // keeps growing across failed attempts.
    void          cancelReprobe(bool resetBackoff);
    // Timer body: re-attempt start() while still UNAVAILABLE + enabled + reprobe.
    void          onReprobeExpired();

    // --- event-driven re-probe: inotify watch on the runtime socket dir (don-the-headset dead-air) ---
    // While dormant in UNAVAILABLE (and openxr:reprobe + openxr:reprobe_watch on) we inotify-watch
    // $XDG_RUNTIME_DIR (plus its wivrn/ subdir) for the runtime materializing. Live evidence (boot
    // 2026-07-14) reshaped this: WiVRn pre-creates comp_ipc at SERVICE start and the don-time signal
    // is the forked compositor server creating/rewriting wivrn.pid — so the triggers are sockets AND
    // pid files, the mask includes MODIFY, and the watch is armed in both wait modes. The timer path
    // stays fully intact as the fallback. All main-thread (the inotify fd rides the wl_event_loop,
    // like the frame->main eventfd). Watched paths + trigger logic are the pure OpenXR::xrReprobe* set.
    void setupReprobeWatch();    // arm: build the inotify fd, add the existing dirs, hook the loop
    void teardownReprobeWatch(); // disarm: remove watches, close the fd, cancel the debounce
    // inotify_add_watch(spec.dir) if it exists and isn't already watched; record it for dispatch/teardown.
    // checkTriggerNow stats for spec.triggerNames right away (used when a watch is (re)added mid-churn —
    // nested dir just appeared / watched dir recreated — to close the created-before-the-add race).
    void addReprobeWatchDir(const OpenXR::SXRReprobeWatch& spec, bool checkTriggerNow);
    void onReprobeWatchReadable();  // drain inotify: nested-dir adds, IN_IGNORED re-adds, trigger matches
    void triggerWatchedProbe();     // arm the debounce one-shot (coalesces a burst of trigger events)
    void onWatchDebounceExpired();  // reset the backoff to attempt 0 and start() immediately
    // Recent-relevant-activity window (live-evidence bug 1): trigger/subdir/nested-dir events stamp
    // m_lastWatchActivity; while it is fresher than XR_REPROBE_ACTIVITY_WINDOW_MS, maybeArmReprobe
    // caps the delay at the base interval (the runtime is materializing — poll hard).
    void markWatchActivity();
    bool watchActivityRecent() const;
    // Is a known runtime IPC socket (monado_comp_ipc / wivrn/comp_ipc) present on disk? Refines the
    // degraded-runtime HEADSET classification in start(): WiVRn's client lib answers extension
    // enumeration even with no server, so only enumerate+socket together mean "service up".
    bool runtimeSocketPresent() const;

    // Aborts an in-progress start(), tearing down whatever was created, and lands in
    // UNAVAILABLE. Safe to call at any failure point in start()/the bring-up continuation.
    void abortStart();

    // --- Task #89 phase 2: keep ALL long-blocking OpenXR work off the compositor MAIN thread. ---
    // The runtime FIRST-CONTACT handshake (xrCreateInstance + xrGetSystem) and the direct-mode session
    // BRING-UP (xrCreateSession .. input->init) are the two blocking legs. Blocker B (the handshake) is
    // run FULLY async: beginHandshake() spawns a detached worker and returns; the worker marshals its
    // result back through the handshake eventfd to onHandshakeComplete() so the main thread NEVER parks
    // (the 30s reprobe against a cold WiVRn socket used to stall the desktop 3-8s each cycle). Blocker A
    // (bring-up) uses the runBoundedHandshake idiom: the main thread parks in a bounded wait while the
    // helper does the work, abandoning on timeout (XR_BRINGUP_TIMEOUT_MS).

    // Shared claim block for the async first-contact handshake. Decides exactly once, under its mutex,
    // who owns `sess`: the main thread (worker finished; adopted in onHandshakeComplete) or the worker
    // (main abandoned via abandonHandshake() -> worker deletes sess + clears m_handshakeInFlight). Full
    // definition in the .cpp. shared_ptr member with an incomplete type — the dtor (in the .cpp) sees it.
    struct SHandshakeClaim;

    // Spawn the detached first-contact worker (createInstance + getSystem) and return immediately. Main.
    void beginHandshake(CXRSession* sess);
    // Main (handshake eventfd): drain the fd and, if the pending handshake finished, run its completion.
    void onHandshakeChannelReadable();
    // Main: adopt `sess`, classify (missing runtime / degraded / no headset), else continue to bring-up.
    void onHandshakeComplete(std::shared_ptr<SHandshakeClaim> claim);
    // Main: abandon an in-flight handshake (stop/dtor/abortStart) — transfer `sess` to the worker (still
    // running) for off-main destruction, or destroy it here if the worker already finished. Idempotent.
    void abandonHandshake();

    // Post-handshake, main: create graphics, initEGL + GPU verify (both bounded/local), then run the
    // bounded bring-up and, on success, spawn the frame thread. `sess` is raw-owned here (not yet in a
    // UP) so a timed-out bring-up can hand it + graphics to the helper for self-cleanup.
    void continueBringup(CXRSession* sess);

    // Outcome of the bounded session bring-up (steps 4-8), run on a helper while the main thread parks.
    enum eBringupResult {
        BRINGUP_OK,      // all steps done in time; main adopted the input and owns sess/graphics
        BRINGUP_FAILED,  // a fatal step returned false; main tears the raws down (synchronous)
        BRINGUP_TIMEOUT, // exceeded XR_BRINGUP_TIMEOUT_MS; ownership of sess/graphics moved to the helper
    };
    // Runs steps 4-8 on a helper thread; the main thread parks up to XR_BRINGUP_TIMEOUT_MS. On OK it
    // adopts the created CXRInput into m_input. On timeout it hands sess/graphics(+partial input) to the
    // helper (which self-cleans on late return) and latches m_bringupInFlight. Main thread only.
    eBringupResult runBoundedBringup(CXRSession* sess, CXRGraphics* gfx);

    // Handshake completion channel (eventfd on the wl_event_loop), created in init(), torn down in the
    // dtor — mirrors the frame->main channel. Persistent because the manager is a session-lifetime
    // singleton; a detached worker may signal it at any time.
    bool setupHandshakeChannel();
    void teardownHandshakeChannel();

    // The XR frame thread body (owns the EGL context + XR frame loop while running).
    void frameThread();

    // --- layer management ---
    // Frame thread: (re)create a layer's swapchain at the given pixel size. Returns false on
    // failure (leaves m_swapchain == XR_NULL_HANDLE).
    bool createLayerSwapchain(CXRMonitorLayer& layer, const Vector2D& size);
    // Main thread: bind still-existing layers on start() and drop those whose monitor
    // disappeared while disabled (doc 02 lazy binding).
    void bindExistingLayers();
    // Main thread (report-20 issue E): register a persistent monitor rule carrying the xrmonitor-
    // declared pixel mode, so it survives plug/unplug/reload (onConnect/ensureMonitorStatus otherwise
    // re-derive the mode from the rule manager, which has no XR entry and falls back to the headless
    // default). No-op when the layer has no declared resolution or the user supplied their own mode
    // (layer->m_userProvidedMode). Idempotent; re-asserted from createXRMonitor + reconcileDeclaredMonitors.
    void registerDeclaredMonitorRule(const PHLMONITOR& mon, const PXRLAYER& layer);
    // Main thread: re-install the requested-mode rule for EVERY live layer that asked for a pixel
    // mode, declared or runtime-created. A config reparse clears the whole rule manager
    // (CConfigManager::reload -> monitorRuleMgr()->clear()), and reconcileDeclaredMonitors only walks
    // the `xrmonitor =` set — so a monitor born from `hyprctl openxr create NAME 2560x1440@60`
    // silently lost its mode on the next reload and fell back to the user's `monitor = NAME,
    // preferred` line / the headless default 1920x1080 (live 2026-08-01). Idempotent, and still a
    // no-op for any layer whose mode the user pinned themselves (m_userProvidedMode).
    void reassertMonitorModeRules();
    // Main thread: decide (openxr:force_linear + the XR EGL node vs this output's buffer allocator
    // node) whether the XR-bound headless output must allocate LINEAR buffers for cross-GPU import,
    // set CMonitor::m_forceLinearSwapchain accordingly, and reconfigure the swapchain if it changed.
    // No-op unless a session is up (the XR render node is only known once EGL is initialized). Runs
    // at bind time (bindExistingLayers / createXRMonitor) — never on the frame thread.
    void applyCrossGpuLinear(const PHLMONITOR& mon);
    // Main thread: destroy all layer frame-side + monitor resources during stop() (path C —
    // no frame thread; barrier not needed). Honors openxr:destroy_monitors_on_stop.
    void teardownLayers();
    // Frame thread: enqueue a "layer removed" ack + wake main (removal barrier step 2).
    void reportLayerRemoved(const std::string& name);
    // Main thread: erase the acked layer + destroy its output (removal barrier step 3).
    void finalizeLayerRemoval(const std::string& name);
    // Main thread: destroy a headless XR output while keeping it alive until aquamarine has
    // drained its idle-callback queue once, so a still-pending frame callback cannot fire on
    // freed memory (works around an aquamarine lifetime bug — see the .cpp).
    void destroyOutputDeferred(SP<Aquamarine::IOutput> output);

    // --- selection + layer cap (main thread) ---
    // Resolve the "the" monitor per doc 05 §3.2: explicit selection > last ray-hovered (WP7) >
    // focused-if-XR. Returns null if none resolves.
    PXRLAYER            resolveSelected();
    PXRLAYER            layerByName(const std::string& name);
    PXRLAYER            layerByMonitorID(MONITORID id);
    // Main-thread hover bookkeeping driven by the ray pointer (doc 04 §3/§9): mark `name` (or "")
    // as the owner's currently-hovered XR monitor, updating layer.m_hovered flags and the
    // last-ray-hovered selection candidate.
    void                setHoveredMonitor(const std::string& name);

    // --- synthetic ray pointer (doc 04 §8, WP7). Main thread. ---
    // Create the CXRPointerDevice + register it via g_pInputManager->newMouse, iff openxr:pointer
    // is set and a session is running. Idempotent. removePointerDevice() destroys it live.
    void ensurePointerDevice();
    void removePointerDevice();
    // Recompute m_quadActive under the runtime layer cap (doc 02 recency policy): newest
    // maxLayerCount quads active, older suspended; posts xrmonitorquad on flips. Under m_layersMu.
    void recomputeQuadActive();

    // --- frame->main channel: the general SPSC ring + eventfd on the wayland loop (doc 04 §7).
    // All frame->main crossings (session-state transitions, layer-removed acks, and — from WP7
    // on — pointer input events) travel through m_queue. ---
    bool                   setupFrameChannel();
    void                   teardownFrameChannel();
    void                   enqueue(XRQueueItem item);                  // frame thread: push + wake main
    void                   wakeMain();                                 // frame thread: write the eventfd
    void                   reportState(eXRManagerState s);             // frame thread: SESSION_STATE event
    void                   onFrameChannelReadable();                   // main thread: drain + dispatch
    void                   dispatchStateEvent(const SXRStateEvent& e); // main thread
    void                   dispatchInputEvent(const SXRInputEvent& e); // main thread (WP7 sink)

    static eXRManagerState mapSessionState(int xrSessionState);

    // --- anchoring (WP5) ---
    // Read the leash/placement tuning from config (hot-live; called per frame + per verb).
    OpenXR::SXRAnchorTuning readAnchorTuning() const;

    // Adaptive-anchoring STRING options (openxr:adaptive_roam_mode / adaptive_transition_ease) are
    // parsed to plain enums on the MAIN thread and published here as atomics, so the per-frame
    // readAnchorTuning() (frame thread) never dereferences a CConfigValue<const char*>: a config
    // reload can rebuild/free the underlying string, dangling the cached pointer -> SIGSEGV.
    // (Numeric CConfigValues are still read directly on the frame thread — a torn read of a live
    // number is a tolerated benign race; a dangling string pointer is not. See readAnchorTuning.)
    // Refreshed from start() and onConfigReload(), both main-thread. Ints hold OpenXR enum values.
    void              publishAdaptiveStringTuning();      // main thread only
    std::atomic<int>  m_adRoamMode{OpenXR::XR_ANCHOR_BODY};
    std::atomic<int>  m_adEase{OpenXR::XR_EASE_SMOOTHSTEP};

    // ---- grab STRING options parsed to atomics for the frame thread (task #25 latent-crash fix) ----
    // openxr:hand_grab / openxr:hand_grab_anywhere (read in CXRInput::processPointer) and openxr:grab_filter_scope
    // (read in frameThread's per-frame solve) are STRING config values. Dereferencing a
    // CConfigValue<std::string> on the frame thread races a concurrent main-thread config reload that
    // rebuilds/frees the backing std::string — a use-after-free / torn read of heap-allocated string
    // storage that corrupts the allocator's metadata and aborts ("corrupted double-linked list") at a
    // later, unrelated free (typically compositor teardown). Parse them to enums on the main thread here
    // and read these atomics on the frame thread instead — identical to publishAdaptiveStringTuning /
    // publishHandInputPolicy. Refreshed from start() + onConfigReload() (+ the legacy-keyword special-
    // cases in ConfigManager.cpp, which fire neither reloaded nor props_refreshed).
    void                 publishGrabStringTuning();        // main thread only
    std::atomic<uint8_t> m_handGrabMode{OpenXR::XR_HANDGRAB_BOTH};            // openxr:hand_grab (shipped default "both")
    std::atomic<uint8_t> m_handGrabAnyMode{OpenXR::XR_HANDGRAB_ANY_GRASP};    // openxr:hand_grab_anywhere (default "grasp")
    std::atomic<bool>    m_grabFilterScopeAll{true};                         // openxr:grab_filter_scope != "hands" (default "all")

    // ---- luma-keyed transparency ("black-as-alpha", openxr:black_alpha — report 09) ----
    // openxr:black_alpha / :black_alpha_knee are NUMERIC configs, so the frame thread could read them
    // directly — but the EFFECTIVE value also depends on the session's environment blend mode (which
    // lives on m_session, main-thread-owned) and must be clamped, and a value that is ignored under an
    // opaque blend mode should say so exactly once. So they are resolved on the MAIN thread here and
    // published as plain atomics the frame loop reads once per frame, the same publish pattern as
    // publishAdaptiveStringTuning/publishGrabStringTuning. Refreshed from start() (both before the
    // frame thread launches AND right after the blend mode is picked), onConfigReload(), and the
    // legacy-keyword special-case in ConfigManager.cpp (a bare `hyprctl keyword` fires neither
    // config.reloaded nor props_refreshed). A CHANGE also damages every XR monitor so a static desktop
    // re-blits through the new key instead of sitting on the previously-keyed swapchain image.
    // modeOverride: use this blend mode for the gate instead of m_session's (start() calls it with the
    // freshly-picked mode before the session object is adopted). nullopt + no session = gate closed.
    void               publishBlackAlphaTuning(std::optional<OpenXR::eXRBlendMode> modeOverride = std::nullopt); // main thread only
    std::atomic<float> m_blackAlpha{1.F};         // EFFECTIVE alpha for pure black; 1.0 = feature off
    std::atomic<float> m_blackAlphaKnee{0.1F};    // luma at which content is fully opaque
    std::atomic<float> m_blackAlphaConfigured{1.F};
    // Main thread only: the configured value we have already warned about being ignored under an
    // opaque blend mode (-1 = nothing warned / re-armed), so the WARN fires once per config set.
    float              m_blackAlphaWarnedFor = -1.F;

    // ---- situational per-monitor transparency: the `xrrule` engine (doc 05 §xrrule) ----
    // MAIN THREAD ONLY, all of it. m_xrRules is a snapshot of CConfigManager's declared list taken at
    // reload (config order is load-bearing: later matches override earlier ones per effect).
    // m_rulesUseTitle short-circuits the window-title trigger when no rule actually looks at titles —
    // titles change on every tab switch and we refuse to pay for that unless it can matter.
    std::vector<OpenXR::SXRRule> m_xrRules;
    bool                         m_rulesUseTitle    = false;
    bool                         m_effectEvalQueued = false; // one deferred evaluation is already pending
    bool                         m_fxKeyGateWarned  = false; // warned once that a rule's blackalpha is gated off

    // Re-resolve every layer's effects from its OWN context tuple and retarget its envelopes. Never
    // call directly from a trigger — go through requestEffectEval() so bursts coalesce.
    void                   evaluateMonitorEffects();
    // The context tuple for one layer: its name, its anchor state, and the class/title/fullscreen of
    // its focused window (the fullscreen window on the monitor's active workspace if any, else that
    // workspace's last-focused window — doc 05 §xrrule).
    OpenXR::SXRRuleContext buildRuleContext(const PXRLAYER& layer);
    // docked | follow | carried, folded from the layer's live anchor mode + grab + adaptive phase.
    OpenXR::eXRAnchorState layerAnchorState(const PXRLAYER& layer);
    // Publish one layer's CURRENT (eased) effect values to the atomics the frame loop reads.
    void                   publishLayerEffects(const PXRLAYER& layer);
    // 8ms main-thread envelope tick: advance every layer's transition, republish, and damage the
    // monitors whose LUMA KEY is mid-transition (the key is baked into the blit, so a static desktop
    // needs a fresh buffer to re-key — the uniform alpha needs no damage at all, it is a post pass
    // over the already-composed image). Self-disarming once every envelope has settled.
    void                onEffectEnvelopeTick();
    void                armEffectEnvelopeTimer();
    SP<CEventLoopTimer> m_fxTimer;
    Time::steady_tp     m_fxLastTick{};
    bool                m_fxTickRunning = false; // false = the next tick seeds dt instead of using it

    // Re-evaluation triggers (doc 05 §xrrule). Anchor-state changes ride the EXISTING frame->main
    // GRAB/ADAPTIVE state events (dispatchStateEvent), so they need no listener of their own.
    CHyprSignalListener m_fxWindowActiveListener;
    CHyprSignalListener m_fxWindowFullscreenListener;
    CHyprSignalListener m_fxWindowTitleListener;
    CHyprSignalListener m_fxWindowCloseListener;
    CHyprSignalListener m_fxWorkspaceActiveListener;
    CHyprSignalListener m_fxMonitorAddedListener;
    CHyprSignalListener m_fxMonitorRemovedListener;

    // ---- 2D-plane sync (report 12 WP-S2) — MAIN THREAD ONLY ----
    // m_l2dRef is the LATCHED desk orientation (§3a). m_l2dPrev is the previous placement's angles,
    // fed back into the projection so a slightly bumped quad changes nothing (§4 hysteresis).
    // m_l2dTimer is the debounce: every trigger RE-ARMS it, so the pass runs once at the end of a
    // burst rather than once per event. m_l2dFrozen is the `sync-layout freeze` latch.
    OpenXR::SXRLayout2DRef               m_l2dRef;
    // Set by the FRAME thread when the runtime hands us a reference-space change (the user pressed
    // recenter): the LOCAL_FLOOR origin moved, every anchor was re-expressed into the new space, and
    // our latched eye/yaw are still in the OLD one. A plain atomic bool is the whole handoff — no
    // refcounts, no strings, no lock (XRMonitorLayer.hpp threading rule). syncLayout2D tests-and-
    // clears it and re-latches from a fresh head pose.
    std::atomic<bool>                    m_l2dRefStale{false};
    std::vector<OpenXR::SXRLayout2DPrev> m_l2dPrev;
    SP<CEventLoopTimer>                  m_l2dTimer;
    bool                                 m_l2dFrozen = false;
    int                                  m_l2dPlacedCount = 0, m_l2dRows = 0, m_l2dWidth = 0, m_l2dHeight = 0;
    void                                 onLayout2DSyncDue();
    // Hand every monitor the engine placed back to the ordinary auto (append-right) path — the
    // feature was switched off. Idempotent.
    void                                 releaseLayout2DPlacements();
    OpenXR::SXRLayout2DConfig            readLayout2DConfig();     // reads STRING config: main thread only
    OpenXR::eXRLayout2DAttach            readLayout2DAttach();     // ditto
    bool                                 layout2DEnabled();

    // ---- conditional hand input (research/16 Part A) ----
    // m_handPolicy is the openxr:hand_input baseline (config + `handinput on|off|auto`); m_handForce
    // is a runtime manual latch set ONLY by `handinput toggle` and consulted ONLY in AUTO. Both are
    // atomics: the main-thread dispatcher/config writes, the frame thread (gating gate before
    // processPointer) and either thread (status) read. m_handPolicyConfigStr caches the last config
    // string so a reload that did NOT change openxr:hand_input preserves a runtime dispatcher change.
    enum eXRHandPolicy : uint8_t { HANDPOL_AUTO = 0, HANDPOL_ON, HANDPOL_OFF };
    enum eXRHandForce : uint8_t { HANDFORCE_NONE = 0, HANDFORCE_ON, HANDFORCE_OFF };
    enum eXRHandInputState : uint8_t { HANDIN_ACTIVE = 0, HANDIN_GATED_KBD, HANDIN_GATED_MANUAL, HANDIN_OFF };
    std::atomic<uint8_t> m_handPolicy{HANDPOL_AUTO};
    std::atomic<uint8_t> m_handForce{HANDFORCE_NONE};
    std::string          m_handPolicyConfigStr; // main thread; last openxr:hand_input value applied
    // Any layer currently roaming (adaptive phase != DOCKED): the "away from the seat" OR-term of the
    // AUTO gate. Written frame-thread each solve, read main-thread (status) + frame-thread (gate).
    std::atomic<bool>    m_anyRoaming{false};
    // Re-read openxr:hand_input into m_handPolicy with change detection (main thread). Called from
    // start() + onConfigReload(); an actual change resets the manual force latch.
    void                 publishHandInputPolicy();
    // Resolve the effective hand-input gate (usable on either thread — reads atomics + the static
    // keyboard-recency signal + config numerics). handInputEnabled() == (state == HANDIN_ACTIVE).
    eXRHandInputState    handInputState() const;
    bool                 handInputEnabled() const {
        return handInputState() == HANDIN_ACTIVE;
    }

    // ---- gaze grab (research/16 Part B) ----
    // frame -> main: the dwell-stable gazed-at monitor id (the m_monitorId atomic pattern). Written
    // release-store after the gaze pass; read acquire-load in the dispatchers + status.
    std::atomic<int64_t> m_gazeHoveredId{-1};
    // The monitor currently gaze-carried (main-thread string; "" = none). Set by cmdGazeGrab, cleared
    // by cmdGazeRelease/toggle + on that monitor's destroy. O(1) toggle/release + status.
    std::string          m_gazeCarryMonitor;
    // Shared "begin a gaze carry on this layer" tail for both the argument-less dwell grab and the
    // targeted `gazegrab <name>` (hypxrvoice GAP 1). Acquires the frame context, takes m_layersMu, and
    // enforces the mutual-exclusion + placed-yet gates. Idempotent when already carrying that exact
    // monitor; errors cleanly when a DIFFERENT monitor is already carried. Main thread.
    std::expected<void, std::string> beginGazeCarry(PXRLAYER layer);
    // Frame-thread-only gaze state: the 1€ pose pre-filter, the dwell/hysteresis selector, and the
    // most recent nearest-hit id + uv (for the gaze cursor). Touched only on the frame thread.
    OpenXR::SXROneEuroPose m_gazeFilter;
    OpenXR::SXRGazeSelect  m_gazeSel;
    int64_t                m_gazeHitId = -1;
    Vector2D               m_gazeHitUV;
    // hypxrvoice GAP 4: the ray/quad intersection that belongs to the DWELL-STABLE candidate, in
    // LOCAL_FLOOR meters. Computed by gazeSelectPass from the intersections it already performs and
    // copied into the pose ring by recordPoseSample (same thread, plain values). The MAIN thread must
    // never re-derive this — it would have to read live quad poses off refcounted layers.
    bool                   m_gazeHitValid = false;
    OpenXR::Vec3           m_gazeHitPoint;
    float                  m_gazeHitDist = 0.F;
    // The gaze-hover + selection + gaze-cursor pass (frame thread). Casts the (optionally filtered)
    // gaze ray over the pointer targets, advances the dwell selector, publishes m_gazeHoveredId + the
    // per-layer m_gazeSelected highlight, and packs the gaze cursor onto the carried layer. `active`
    // is the frame's visible-layer snapshot (same vector processPointer consumed).
    void gazeSelectPass(const std::vector<SXRPointerTarget>& targets, const std::vector<PXRLAYER>& active, const OpenXR::SXRPose& view, bool viewValid, float dt);

    // hypxrvoice WP-V1: the rolling head-pose + gaze-candidate ring. ~91s at 90Hz (8192 * 1/90s;
    // longer at lower refresh) — sized so the earliest deictic word of a long utterance is still
    // in the window after ASR + intent latency, at full frame-rate resolution (~512KB; a `gaze at`
    // older than the window clamps to the oldest sample SILENTLY, so generosity beats a knob here).
    // Written once per frame by the frame thread (recordPoseSample), read by main-thread IPC
    // queries (gazeSampleNow/At), both under m_poseRingMu — a plain std::mutex, deliberately NOT
    // m_layersMu (a status query must not contend with the solve/grab critical section). The
    // sample is a plain POD (no strings, no refcounts) so the frame-thread write is refcount- and
    // string-free (XRMonitorLayer.hpp rules). Capacity is a power of two (mask indexing).
    static constexpr size_t         XR_POSE_RING_CAP = 8192;
    OpenXR::SXRPoseRing<XR_POSE_RING_CAP> m_poseRing;
    std::mutex                      m_poseRingMu;
    // Frame thread, called once per frame after gazeSelectPass. Stamps Time::steadyNow() and packs
    // the current head pose + the dwell-selector state into the ring. viewValid propagates so a
    // query can tell head-tracked frames from dropouts. Reads m_gazeHoveredId (its own atomic) and
    // the frame-thread-only m_gazeSel/m_gazeHitId (same thread) — no config, no refcount.
    void recordPoseSample(const OpenXR::SXRPose& view, bool viewValid);

    // Copy of the most recent frame-thread solve inputs, so verbs (main thread) get a view/grip
    // context without blocking the frame thread. Written by the frame thread under m_layersMu.
    OpenXR::SXRVerbContext currentVerbContext();

    eXRManagerState        m_state  = XR_STATE_DISABLED;
    bool                   m_active = false; // derived: state ∈ {visible, focused}

    // Plugged-state bookkeeping (report-18 addendum). m_monitorsPlugged is the last plugged intent
    // setMonitorsPlugged() applied to the session-following XR monitors, so updateMonitorsPlugged()
    // only arms the grace timer on a real plugged->unplugged edge (never to unplug something already
    // unplugged). m_unplugTimer is the one-shot anti-flap grace timer for the VISIBLE->hidden drop.
    bool                m_monitorsPlugged = false;
    SP<CEventLoopTimer> m_unplugTimer;

    // User-presence gating (report-19). m_userPresenceSupported is latched at start() from
    // CXRSession::m_supportsUserPresence (the ext advertised AND the device supports presence); when
    // true the `visible` mode gates on presence, not raw visibility. m_presenceKnown flips true on
    // the first presence event of a session (before it, `visible` reads as ABSENT so a doffed
    // session-create sprint never plugs). m_userPresent is the last don/doff state. All main thread.
    bool                m_userPresenceSupported = false;
    bool                m_presenceKnown         = false;
    bool                m_userPresent           = false;

    // First-plug blip guard for the no-presence fallback (report-19). m_everPlugged: have we plugged
    // at least once this session (after that the anti-flap grace governs, not the blip gate).
    // m_visibleSince: when the session last became VISIBLE (for the sustained-visibility check).
    // m_plugSettleTimer: one-shot that re-runs the funnel once the blip window elapses so the first
    // plug lands without an external edge to trigger it.
    bool                       m_everPlugged = false;
    std::optional<Time::steady_tp> m_visibleSince;
    SP<CEventLoopTimer>        m_plugSettleTimer;

    // Recenter-on-plug (report-20 issue C). m_recenteredThisSession: gate so a session re-seats its
    // anchor:local monitors exactly once, on the FIRST presence-confirmed plug (a brief doff+don in
    // the same session must NOT re-seat). Reset per session in resetPresenceState(). m_recenterArmed
    // is set on that first plug (main thread) and consumed by the frame thread, which owns the head
    // pose — it re-seats when a valid view is available (so a plug while the view is momentarily
    // invalid still recenters on the next good frame). Atomic: main writes, frame reads/clears.
    bool                m_recenteredThisSession = false;
    std::atomic<bool>   m_recenterArmed{false};

    // Dormant re-probe timer (report-17 WP-L3 / report-20 issue B1). m_reprobeTimer re-attempts
    // start() while UNAVAILABLE; m_reprobeAttempt counts consecutive failures (drives the backoff);
    // m_probeWait records why we are waiting (status + cadence choice).
    SP<CEventLoopTimer> m_reprobeTimer;
    int                 m_reprobeAttempt = 0;
    eXRProbeWait        m_probeWait      = XR_WAIT_NONE;

    // Event-driven re-probe (don-the-headset dead-air fix). The inotify fd rides the wl_event_loop
    // exactly like m_eventFd/m_eventSource; armed on entering UNAVAILABLE, torn down on leaving. All
    // main-thread. m_watchSpecs is the pure-derived watch set (from OpenXR::xrReprobeWatchDirs);
    // m_watchByWd maps a live inotify watch descriptor back to its spec for event dispatch;
    // m_watchDebounceTimer coalesces a burst of create events + waits out the server's accept() gap.
    int                                        m_watchInotifyFd = -1;
    wl_event_source*                           m_watchSource    = nullptr;
    std::vector<OpenXR::SXRReprobeWatch>       m_watchSpecs;
    std::unordered_map<int, OpenXR::SXRReprobeWatch> m_watchByWd;
    SP<CEventLoopTimer>                        m_watchDebounceTimer;
    // Per-dormant-PERIOD bookkeeping (survives the per-probe watch teardown/re-arm; cleared only by
    // cancelReprobe(resetBackoff=true) — real success / user disable). m_lastWatchActivity: last
    // relevant inotify event, drives the fast-cadence activity window. m_watchEverFired /
    // m_watchMissLogged: the timer-probe silent-miss guard (a trigger path existing on disk while the
    // watch has never fired is logged once — WARN on RUNTIME wait, DEBUG on the expected WiVRn
    // pre-don HEADSET wait).
    std::optional<Time::steady_tp>             m_lastWatchActivity;
    bool                                       m_watchEverFired  = false;
    bool                                       m_watchMissLogged = false;

    // Populated from xrInstanceProperties/xrSystemProperties once a session exists.
    std::string       m_runtimeName;
    std::string       m_systemName;
    // Set by the wrong-GPU probe in start() (see runtimeGpu()); empty when undeterminable.
    std::string       m_runtimeGpu;
    // The openxr:runtime_json override active for this session (see runtimeJson()); empty = none.
    std::string       m_runtimeJson;

    UP<CXRIpc>        m_ipc;
    UP<CXRSession>    m_session;
    UP<CXRGraphics>   m_graphics;
    UP<CXRInput>      m_input; // OpenXR action system (frame-thread sampling), created in start()

    // Synthetic ray pointer (main thread). Registered on start() when openxr:pointer is set;
    // driven by the frame->main queue drain (dispatchInputEvent). Destroyed on stop()/toggle.
    SP<CXRPointerDevice> m_pointerDevice;

    std::thread       m_frameThread;
    std::atomic<bool> m_running{false};

    // Set true whenever a first-contact handshake worker is in flight (beginHandshake) OR an abandoned
    // handshake worker is still running (blocked in xrCreateInstance against a wedged runtime). While set,
    // start() defers — never runs a second concurrent handshake (the OpenXR loader's global init is not
    // re-entrant). Cleared on the main thread in onHandshakeComplete, or on the worker thread if the
    // handshake was abandoned. Read on the main thread.
    std::atomic<bool> m_handshakeInFlight{false};

    // Task #89 phase 2 blocker A: latched while an ABANDONED bring-up helper is still running (blocked in
    // an XR IPC against a wedged runtime, then self-cleaning sess/graphics off-main). While set, start()
    // defers so a reprobe never races the loader / a live-but-doomed XrInstance. The helper clears it.
    std::atomic<bool> m_bringupInFlight{false};

    // Task #89 phase 2 blocker B: the in-flight async handshake's claim (main-thread owned), plus the
    // completion eventfd + its wl_event_loop source. The worker signals m_handshakeFd; the source calls
    // onHandshakeChannelReadable() on the main thread. Non-null m_pendingHandshake == a handshake awaiting
    // its completion callback.
    std::shared_ptr<SHandshakeClaim> m_pendingHandshake;
    int                              m_handshakeFd     = -1;
    wl_event_source*                 m_handshakeSource = nullptr;

    // XR monitor layers. m_layers is written on the main thread and snapshotted per frame by
    // the frame thread, both under m_layersMu (doc 00 handoff table). std::shared_ptr, NOT
    // hyprutils SP: the snapshot copies cross threads and only shared_ptr's refcount is atomic
    // (see the thread-safety rule in XRMonitorLayer.hpp). The frame thread additionally drops
    // its toRemove refs BEFORE acking a removal, so finalizeLayerRemoval always holds the last
    // ref and ~CXRMonitorLayer (which releases hyprutils WPs/listeners) runs on main.
    std::vector<PXRLAYER>            m_layers;
    std::mutex                       m_layersMu;
    uint64_t                         m_seqCounter = 0;

    // Latest solve context captured by the frame thread (under m_layersMu) for main-thread verbs.
    OpenXR::SXRVerbContext m_lastVerbCtx;

    // Selected-monitor state (doc 05 §3.2). Explicit selection wins; cleared when destroyed.
    std::string m_selectedMonitor;
    // Last ray-hovered XR monitor (doc 05 §3.2 rule 2 / doc 04 §9), set by the pointer drain.
    std::string m_lastHoveredMonitor;
    // The owner's currently-hovered XR monitor (main-thread mirror), so m_hovered flags can be
    // cleared when the ray moves off / onto a different quad.
    std::string m_curHoveredMonitor;

    // Frame->main channel state (doc 04 §7.2): lock-free SPSC ring drained by an eventfd on the
    // wayland event loop. Single producer = frame thread, single consumer = main thread.
    int                 m_eventFd     = -1;
    wl_event_source*    m_eventSource = nullptr;
    CXRQueue            m_queue;
    std::atomic<bool>   m_queueOverflowed{false}; // logged-once guard for a lost non-droppable item
    std::atomic<bool>   m_frameRequestedTeardown{false};

    // Last-seen chrome geometry config tuple (enabled, margin, bar_height, bar_width_frac,
    // corner_size), to detect a hot-reload change (WP-G2 chrome hot-reload fix). Empty until first
    // compared.
    std::optional<std::array<double, 5>> m_lastChromeGeom;

    CHyprSignalListener m_configReloadListener;
    CHyprSignalListener m_propsRefreshedListener;
};

inline UP<COpenXRManager> g_pOpenXRManager;

#endif
