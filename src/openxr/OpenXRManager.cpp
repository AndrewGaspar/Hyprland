#include "OpenXRManager.hpp"
#ifdef HAVE_OPENXR

#include <openxr/openxr.h>

#include <cstring>

#include "XRIpc.hpp"
#include "../debug/log/Logger.hpp"
#include "../config/ConfigValue.hpp"
#include "../event/EventBus.hpp"
#include "../managers/EventManager.hpp"
#include "../managers/input/InputManager.hpp"

COpenXRManager::COpenXRManager() = default;

COpenXRManager::~COpenXRManager() {
    if (m_state != XR_STATE_DISABLED)
        stop();
}

void COpenXRManager::init() {
    // Register the hyprctl "openxr" command surface. Kept out of HyprCtl.cpp so the
    // XR footprint outside src/openxr/ stays at zero.
    m_ipc = makeUnique<CXRIpc>();
    m_ipc->registerCommands();

    // React to reloads. `hyprctl keyword openxr:enabled 1` fires props_refreshed rather than
    // a full reload, so listen to both; onConfigReload() is idempotent.
    m_configReloadListener   = Event::bus()->m_events.config.reloaded.listen([this] { onConfigReload(); });
    m_propsRefreshedListener = Event::bus()->m_events.config.props_refreshed.listen([this] { onConfigReload(); });

    // Honor openxr:enabled at startup.
    static auto PENABLED = CConfigValue<Hyprlang::INT>("openxr:enabled");
    if (*PENABLED)
        start();
}

const char* COpenXRManager::stateToString(eXRManagerState state) {
    switch (state) {
        case XR_STATE_DISABLED: return "disabled";
        case XR_STATE_UNAVAILABLE: return "unavailable";
        case XR_STATE_STARTING: return "starting";
        case XR_STATE_RUNNING_IDLE: return "idle";
        case XR_STATE_RUNNING_VISIBLE: return "visible";
        case XR_STATE_RUNNING_FOCUSED: return "focused";
        case XR_STATE_STOPPING: return "stopping";
    }
    return "disabled";
}

eXRManagerState COpenXRManager::state() const {
    return m_state;
}

const std::string& COpenXRManager::runtimeName() const {
    return m_runtimeName;
}

const std::string& COpenXRManager::systemName() const {
    return m_systemName;
}

void COpenXRManager::setState(eXRManagerState newState) {
    if (m_state == newState)
        return;

    m_state = newState;

    if (g_pEventManager)
        g_pEventManager->postEvent(SHyprIPCEvent{"openxrsessionstate", stateToString(newState)});

    // openxractive is a derived boolean: active ⇔ state ∈ {visible, focused}. Post only on flip.
    const bool nowActive = newState == XR_STATE_RUNNING_VISIBLE || newState == XR_STATE_RUNNING_FOCUSED;
    if (nowActive != m_active) {
        m_active = nowActive;
        if (g_pEventManager)
            g_pEventManager->postEvent(SHyprIPCEvent{"openxractive", nowActive ? "1" : "0"});
    }

    // Idle-inhibit is rechecked on every session-state transition (the hook itself lands in WP9).
    if (g_pInputManager)
        g_pInputManager->recheckIdleInhibitorStatus();
}

void COpenXRManager::start() {
    // Idempotent: only DISABLED/UNAVAILABLE may (re)start. UNAVAILABLE retries from scratch.
    if (m_state != XR_STATE_DISABLED && m_state != XR_STATE_UNAVAILABLE)
        return;

    setState(XR_STATE_STARTING);

    m_runtimeName.clear();
    m_systemName.clear();

    // WP1: availability probe only. Full instance/system/session creation over EGL/GBM and
    // the frame thread land in WP2 (docs/openxr/01-session-graphics.md). A successful probe
    // still ends in UNAVAILABLE here because there is no session code to bring RUNNING up yet.
    XrInstanceCreateInfo ci{};
    ci.type = XR_TYPE_INSTANCE_CREATE_INFO;
    std::strncpy(ci.applicationInfo.applicationName, "Hyprland", XR_MAX_APPLICATION_NAME_SIZE - 1);
    ci.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;

    XrInstance     instance = XR_NULL_HANDLE;
    const XrResult result   = xrCreateInstance(&ci, &instance);

    if (XR_FAILED(result)) {
        Log::logger->log(Log::WARN, "[OPENXR] no runtime available (xrCreateInstance -> {}), state -> unavailable", static_cast<int>(result));
        setState(XR_STATE_UNAVAILABLE);
        return;
    }

    XrInstanceProperties props{};
    props.type = XR_TYPE_INSTANCE_PROPERTIES;
    if (XR_SUCCEEDED(xrGetInstanceProperties(instance, &props)))
        Log::logger->log(Log::DEBUG, "[OPENXR] runtime available: {}; session support lands in WP2, state -> unavailable", props.runtimeName);
    else
        Log::logger->log(Log::DEBUG, "[OPENXR] runtime available; session support lands in WP2, state -> unavailable");

    xrDestroyInstance(instance);

    // No session support yet — do not advertise a runtime name while UNAVAILABLE.
    setState(XR_STATE_UNAVAILABLE);
}

void COpenXRManager::stop() {
    if (m_state == XR_STATE_DISABLED)
        return;

    setState(XR_STATE_STOPPING);

    // WP1: nothing to tear down yet (no frame thread / session / monitors). The teardown
    // ordering invariant and openxr:destroy_monitors_on_stop handling land in WP2+.
    m_runtimeName.clear();
    m_systemName.clear();

    setState(XR_STATE_DISABLED);
}

void COpenXRManager::onConfigReload() {
    static auto PENABLED = CConfigValue<Hyprlang::INT>("openxr:enabled");
    const bool  enabled  = *PENABLED;

    if (enabled && m_state == XR_STATE_DISABLED)
        start();
    else if (!enabled && m_state != XR_STATE_DISABLED)
        stop();

    // Re-run so that toggling openxr:inhibit_idle takes effect immediately (WP9 hook).
    if (g_pInputManager)
        g_pInputManager->recheckIdleInhibitorStatus();
}

#endif
