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
    void                init();

    // Idempotent main-thread lifecycle methods — all enable/disable paths funnel here.
    void                start();
    void                stop();

    eXRManagerState     state() const;
    const std::string&  runtimeName() const;
    const std::string&  systemName() const;

    // Monitor create/destroy funnel (main thread). createXRMonitor works in EVERY manager
    // state (including DISABLED) so monitors created without a session become plain headless
    // outputs whose quads bind lazily on start() (doc 02). WP3 exercises this with a single
    // hard-coded test monitor; the config keyword/dispatcher/hyprctl surfaces are WP4.
    std::expected<SP<CXRMonitorLayer>, std::string> createXRMonitor(const SXRMonitorParams& params);
    void                                            destroyXRMonitor(const std::string& name);

    // "disabled" | "unavailable" | "starting" | "idle" | "visible" | "focused" | "stopping"
    static const char*  stateToString(eXRManagerState state);

  private:
    void                setState(eXRManagerState newState);
    void                onConfigReload();

    // Aborts an in-progress start(), tearing down whatever was created, and lands in
    // UNAVAILABLE. Safe to call at any failure point in start().
    void                abortStart();

    // The XR frame thread body (owns the EGL context + XR frame loop while running).
    void                frameThread();

    // --- layer management ---
    // Frame thread: (re)create a layer's swapchain at the given pixel size. Returns false on
    // failure (leaves m_swapchain == XR_NULL_HANDLE).
    bool                createLayerSwapchain(CXRMonitorLayer& layer, const Vector2D& size);
    // Main thread: bind still-existing layers on start() and drop those whose monitor
    // disappeared while disabled (doc 02 lazy binding).
    void                bindExistingLayers();
    // Main thread: destroy all layer frame-side + monitor resources during stop() (path C —
    // no frame thread; barrier not needed). Honors openxr:destroy_monitors_on_stop.
    void                teardownLayers();
    // Frame thread: enqueue a "layer removed" ack + wake main (removal barrier step 2).
    void                reportLayerRemoved(const std::string& name);
    // Main thread: erase the acked layer + destroy its output (removal barrier step 3).
    void                finalizeLayerRemoval(const std::string& name);

    // --- minimal frame->main session-state channel (WP6 replaces this with the general
    // SPSC + eventfd queue; the interface — reportState()/onFrameChannelReadable() — is
    // shaped so the internals can be swapped without touching callers). ---
    bool                setupFrameChannel();
    void                teardownFrameChannel();
    void                reportState(eXRManagerState s);  // frame thread: enqueue + wake main
    void                onFrameChannelReadable();        // main thread: drain + apply

    static eXRManagerState mapSessionState(int xrSessionState);

    eXRManagerState     m_state = XR_STATE_DISABLED;
    bool                m_active = false; // derived: state ∈ {visible, focused}

    // Populated from xrInstanceProperties/xrSystemProperties once a session exists.
    std::string         m_runtimeName;
    std::string         m_systemName;

    UP<CXRIpc>          m_ipc;
    UP<CXRSession>      m_session;
    UP<CXRGraphics>     m_graphics;

    std::thread         m_frameThread;
    std::atomic<bool>   m_running{false};

    // XR monitor layers. m_layers is written on the main thread and snapshotted per frame by
    // the frame thread, both under m_layersMu (doc 00 handoff table).
    std::vector<SP<CXRMonitorLayer>> m_layers;
    std::mutex                       m_layersMu;
    uint64_t                         m_seqCounter = 0;

    // Frame->main channel state.
    int                          m_eventFd     = -1;
    wl_event_source*             m_eventSource = nullptr;
    std::mutex                   m_pendingMu;
    std::vector<eXRManagerState> m_pendingStates;
    std::vector<std::string>     m_removedLayerNames; // frame->main layer-removed acks
    std::atomic<bool>            m_frameRequestedTeardown{false};

    CHyprSignalListener m_configReloadListener;
    CHyprSignalListener m_propsRefreshedListener;
};

inline UP<COpenXRManager> g_pOpenXRManager;

#endif
