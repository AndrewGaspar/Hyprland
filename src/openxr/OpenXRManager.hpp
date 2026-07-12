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
#include <vector>

#include "../helpers/memory/Memory.hpp"
#include "../helpers/signal/Signal.hpp"
#include "XRMonitorConfig.hpp"
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
    // Currently-active environment blend mode as "opaque"|"alpha"|"additive" (doc 05 §4.3). The
    // selected mode while a session exists; "opaque" (the default) otherwise.
    std::string        blendModeName() const;
    // Whether the current session is an XR_EXTX_overlay session (doc 01). Actual state, not the
    // openxr:overlay request — false when no session exists or the runtime lacked the extension.
    bool               isOverlay() const;

    // Idle-inhibit predicate (doc 05 §6.1). Main thread only. True iff openxr:inhibit_idle is
    // set AND the session currently has input focus (FOCUSED). CInputManager::recheckIdleInhibitorStatus()
    // is the sole writer of the inhibit bit; it consults this before its final setInhibit(false).
    bool               shouldInhibitIdle();

    // Monitor create/destroy funnel (main thread). createXRMonitor works in EVERY manager
    // state (including DISABLED) so monitors created without a session become plain headless
    // outputs whose quads bind lazily on start() (doc 02). WP3 exercises this with a single
    // hard-coded test monitor; the config keyword/dispatcher/hyprctl surfaces are WP4.
    std::expected<PXRLAYER, std::string> createXRMonitor(const SXRMonitorParams& params);
    void                                 destroyXRMonitor(const std::string& name);

    // Plugged-state edge (research/18 WP-M1/M2): make every XR-created monitor behave like a
    // plugged/unplugged external monitor. `sessionUp` is the session-EXISTENCE edge (the
    // start()/stop() boundary — never VISIBLE/FOCUSED, which flaps on the proximity sensor).
    // With openxr:monitors_follow_session (default on), unplugging drives CMonitor::onDisconnect()
    // (full workspace evacuation) and plugging drives onConnect(true) (name-keyed workspace
    // return) — the exact functions the rule manager uses for `monitor=...,disable` toggles.
    // Never destroys an output (that path is the aquamarine framecb UAF, destroyOutputDeferred).
    // Main thread only. Idempotent — future session-lifecycle code (research/17) can call it on
    // any re-probed edge.
    void setMonitorsPlugged(bool sessionUp);

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

    // --- adaptive anchoring verbs (research/13 §6.3). Main thread; take m_layersMu. Operate on the
    // selected monitor (like move/center). ---
    std::expected<void, std::string> cmdAdaptive(const std::string& args); // on|off|toggle
    std::expected<void, std::string> cmdDock(const std::string& args);     // [here]
    std::expected<void, std::string> cmdUndock();                          // (none)
    std::expected<void, std::string> cmdRoam(const std::string& args);     // head|body

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
        bool        plugged  = false; // research/18: the headless output is currently enabled
        // Adaptive anchoring (research/13 §6.4).
        bool        adaptiveEnabled  = false;
        std::string adaptivePhase    = "docked"; // docked | undocking | roaming | redocking
        std::string adaptiveRoamMode = "body";   // head | body
        float       adaptiveSeatDist = 0.f;      // current XZ distance from the desk seat (m)
        float       adaptiveT        = 0.f;      // transition envelope parameter [0,1]
    };
    std::vector<SXRMonitorInfo> monitorInfos();

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

    // Aborts an in-progress start(), tearing down whatever was created, and lands in
    // UNAVAILABLE. Safe to call at any failure point in start().
    void abortStart();

    // The XR frame thread body (owns the EGL context + XR frame loop while running).
    void frameThread();

    // --- layer management ---
    // Frame thread: (re)create a layer's swapchain at the given pixel size. Returns false on
    // failure (leaves m_swapchain == XR_NULL_HANDLE).
    bool createLayerSwapchain(CXRMonitorLayer& layer, const Vector2D& size);
    // Main thread: bind still-existing layers on start() and drop those whose monitor
    // disappeared while disabled (doc 02 lazy binding).
    void bindExistingLayers();
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
    // Copy of the most recent frame-thread solve inputs, so verbs (main thread) get a view/grip
    // context without blocking the frame thread. Written by the frame thread under m_layersMu.
    OpenXR::SXRVerbContext currentVerbContext();

    eXRManagerState        m_state  = XR_STATE_DISABLED;
    bool                   m_active = false; // derived: state ∈ {visible, focused}

    // Populated from xrInstanceProperties/xrSystemProperties once a session exists.
    std::string       m_runtimeName;
    std::string       m_systemName;

    UP<CXRIpc>        m_ipc;
    UP<CXRSession>    m_session;
    UP<CXRGraphics>   m_graphics;
    UP<CXRInput>      m_input; // OpenXR action system (frame-thread sampling), created in start()

    // Synthetic ray pointer (main thread). Registered on start() when openxr:pointer is set;
    // driven by the frame->main queue drain (dispatchInputEvent). Destroyed on stop()/toggle.
    SP<CXRPointerDevice> m_pointerDevice;

    std::thread       m_frameThread;
    std::atomic<bool> m_running{false};

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
