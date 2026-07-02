#pragma once
#ifdef HAVE_OPENXR

#include <cstdint>
#include <string>

#include "../helpers/memory/Memory.hpp"
#include "../helpers/signal/Signal.hpp"

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

    // "disabled" | "unavailable" | "starting" | "idle" | "visible" | "focused" | "stopping"
    static const char*  stateToString(eXRManagerState state);

  private:
    void                setState(eXRManagerState newState);
    void                onConfigReload();

    eXRManagerState     m_state = XR_STATE_DISABLED;
    bool                m_active = false; // derived: state ∈ {visible, focused}

    // Populated from xrInstanceProperties/xrSystemProperties once a session exists (WP2).
    std::string         m_runtimeName;
    std::string         m_systemName;

    UP<CXRIpc>          m_ipc;

    CHyprSignalListener m_configReloadListener;
    CHyprSignalListener m_propsRefreshedListener;
};

inline UP<COpenXRManager> g_pOpenXRManager;

#endif
