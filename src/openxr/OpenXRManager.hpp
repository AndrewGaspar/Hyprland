#pragma once
#ifdef HAVE_OPENXR

#include <cstdint>
#include <string>
#include <atomic>
#include <expected>
#include <mutex>
#include <thread>
#include <vector>

#include "../helpers/memory/Memory.hpp"
#include "../helpers/signal/Signal.hpp"
#include "XRMonitorConfig.hpp"
#include "XRInput.hpp" // SXRInputEvent / SXRStateEvent / XRQueueItem / CXRQueue / CXRInput

struct wl_event_source;

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

    // Monitor create/destroy funnel (main thread). createXRMonitor works in EVERY manager
    // state (including DISABLED) so monitors created without a session become plain headless
    // outputs whose quads bind lazily on start() (doc 02). WP3 exercises this with a single
    // hard-coded test monitor; the config keyword/dispatcher/hyprctl surfaces are WP4.
    std::expected<SP<CXRMonitorLayer>, std::string> createXRMonitor(const SXRMonitorParams& params);
    void                                            destroyXRMonitor(const std::string& name);

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
        bool        grabbed = false;
        bool        hovered = false;
    };
    std::vector<SXRMonitorInfo> monitorInfos();

    // `hyprctl openxr layout`: paste-ready `xrmonitor = ...` lines for every live XR monitor.
    std::string layoutDump();

    // Reconcile the declared (`xrmonitor` keyword) set against live layers (doc 05 §2.5). Runs
    // from onConfigReload() and from init() so declared monitors materialize even while disabled.
    void reconcileDeclaredMonitors();

    // "disabled" | "unavailable" | "starting" | "idle" | "visible" | "focused" | "stopping"
    static const char* stateToString(eXRManagerState state);

  private:
    void setState(eXRManagerState newState);
    void onConfigReload();

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

    // --- selection + layer cap (main thread) ---
    // Resolve the "the" monitor per doc 05 §3.2: explicit selection > last ray-hovered (WP7) >
    // focused-if-XR. Returns null if none resolves.
    SP<CXRMonitorLayer> resolveSelected();
    SP<CXRMonitorLayer> layerByName(const std::string& name);
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

    std::thread       m_frameThread;
    std::atomic<bool> m_running{false};

    // XR monitor layers. m_layers is written on the main thread and snapshotted per frame by
    // the frame thread, both under m_layersMu (doc 00 handoff table).
    std::vector<SP<CXRMonitorLayer>> m_layers;
    std::mutex                       m_layersMu;
    uint64_t                         m_seqCounter = 0;

    // Latest solve context captured by the frame thread (under m_layersMu) for main-thread verbs.
    OpenXR::SXRVerbContext m_lastVerbCtx;

    // Selected-monitor state (doc 05 §3.2). Explicit selection wins; cleared when destroyed.
    std::string m_selectedMonitor;
    // WP7 hook: the ray pointer will set this to the last-hovered XR monitor name.
    std::string m_lastHoveredMonitor;

    // Frame->main channel state (doc 04 §7.2): lock-free SPSC ring drained by an eventfd on the
    // wayland event loop. Single producer = frame thread, single consumer = main thread.
    int                 m_eventFd     = -1;
    wl_event_source*    m_eventSource = nullptr;
    CXRQueue            m_queue;
    std::atomic<bool>   m_queueOverflowed{false}; // logged-once guard for a lost non-droppable item
    std::atomic<bool>   m_frameRequestedTeardown{false};

    CHyprSignalListener m_configReloadListener;
    CHyprSignalListener m_propsRefreshedListener;
};

inline UP<COpenXRManager> g_pOpenXRManager;

#endif
