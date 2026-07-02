#include "OpenXRManager.hpp"
#ifdef HAVE_OPENXR

#include <openxr/openxr.h>

#include <cstring>
#include <cstdint>
#include <chrono>

#include <sys/eventfd.h>
#include <unistd.h>
#include <wayland-server-core.h>

#include "XRIpc.hpp"
#include "XRSession.hpp"
#include "XRGraphics.hpp"
#include "../Compositor.hpp"
#include "../debug/log/Logger.hpp"
#include "../config/ConfigValue.hpp"
#include "../event/EventBus.hpp"
#include "../managers/EventManager.hpp"
#include "../managers/eventLoop/EventLoopManager.hpp"
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
    m_frameRequestedTeardown = false;

    m_session  = makeUnique<CXRSession>();
    m_graphics = makeUnique<CXRGraphics>();

    // 1. Instance (extension checks; missing runtime / required extensions -> UNAVAILABLE).
    if (!m_session->createInstance()) {
        Log::logger->log(Log::WARN, "[OPENXR] no runtime / required extensions unavailable, state -> unavailable");
        abortStart();
        return;
    }

    // 2. System.
    if (!m_session->getSystem()) {
        Log::logger->log(Log::WARN, "[OPENXR] system lookup failed, state -> unavailable");
        abortStart();
        return;
    }

    // Publish runtime/system names for `hyprctl openxr status` as soon as we have them.
    m_runtimeName = m_session->runtimeName();
    m_systemName  = m_session->systemName();

    // 3. EGL/GBM display + context on the right GPU.
    static auto       PGPU = CConfigValue<std::string>("openxr:gpu");
    const std::string gpu  = *PGPU;
    if (!m_graphics->initEGL(gpu)) {
        Log::logger->log(Log::ERR, "[OPENXR] EGL/GBM init failed, state -> unavailable");
        abortStart();
        return;
    }

    // 4. Session (XrGraphicsBindingEGLMNDX).
    if (!m_session->createSession(*m_graphics)) {
        abortStart();
        return;
    }

    // 5. Reference + view spaces.
    if (!m_session->createSpaces(*m_graphics)) {
        abortStart();
        return;
    }

    // 6. Blit GL resources (still on the main thread, before the frame thread exists).
    if (!m_graphics->initBlitGL()) {
        abortStart();
        return;
    }

    // 7. Choose the swapchain format once (per-layer swapchains are created lazily in WP3).
    m_session->chooseSwapchainFormat();

    // 8. Frame->main channel, then spawn the frame thread and go RUNNING{idle}.
    if (!setupFrameChannel()) {
        abortStart();
        return;
    }

    m_running = true;
    m_frameThread = std::thread([this] { frameThread(); });

    setState(XR_STATE_RUNNING_IDLE);
    Log::logger->log(Log::DEBUG, "[OPENXR] session up (runtime: {}, system: {}), frame thread started", m_runtimeName, m_systemName);
}

void COpenXRManager::abortStart() {
    // No frame thread has been started at any abortStart() call site, so there is nothing
    // to join. Tear down in the doc-01 order: GL -> XR handles -> EGL.
    teardownFrameChannel();
    if (m_graphics)
        m_graphics->destroyGL();
    if (m_session)
        m_session->destroy();
    if (m_graphics)
        m_graphics->destroyEGL();
    m_graphics.reset();
    m_session.reset();

    m_runtimeName.clear();
    m_systemName.clear();

    setState(XR_STATE_UNAVAILABLE);
}

void COpenXRManager::stop() {
    if (m_state == XR_STATE_DISABLED)
        return;

    // Whether stop was ultimately triggered by instance loss decides the final state.
    const bool lost = m_session && m_session->m_instanceLost;

    setState(XR_STATE_STOPPING);

    // Teardown ordering (doc 01): join the frame thread BEFORE touching any EGL/XR object.
    m_running = false;
    if (m_frameThread.joinable())
        m_frameThread.join();

    teardownFrameChannel();

    // GL cleanup with the context current, then XR handles (context not current), then EGL.
    if (m_graphics)
        m_graphics->destroyGL();
    if (m_session)
        m_session->destroy();
    if (m_graphics)
        m_graphics->destroyEGL();

    m_graphics.reset();
    m_session.reset();

    m_runtimeName.clear();
    m_systemName.clear();

    setState(lost ? XR_STATE_UNAVAILABLE : XR_STATE_DISABLED);
}

eXRManagerState COpenXRManager::mapSessionState(int xrSessionState) {
    switch (xrSessionState) {
        case XR_SESSION_STATE_VISIBLE: return XR_STATE_RUNNING_VISIBLE;
        case XR_SESSION_STATE_FOCUSED: return XR_STATE_RUNNING_FOCUSED;
        // IDLE / READY / SYNCHRONIZED / STOPPING all map to RUNNING_IDLE (doc 01).
        default: return XR_STATE_RUNNING_IDLE;
    }
}

bool COpenXRManager::setupFrameChannel() {
    m_eventFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (m_eventFd < 0) {
        Log::logger->log(Log::ERR, "[OPENXR] eventfd() failed for the frame->main channel");
        return false;
    }

    m_eventSource = wl_event_loop_add_fd(
        g_pCompositor->m_wlEventLoop, m_eventFd, WL_EVENT_READABLE,
        [](int /*fd*/, uint32_t /*mask*/, void* data) -> int {
            static_cast<COpenXRManager*>(data)->onFrameChannelReadable();
            return 0;
        },
        this);

    if (!m_eventSource) {
        Log::logger->log(Log::ERR, "[OPENXR] wl_event_loop_add_fd failed for the frame->main channel");
        close(m_eventFd);
        m_eventFd = -1;
        return false;
    }

    return true;
}

void COpenXRManager::teardownFrameChannel() {
    if (m_eventSource) {
        wl_event_source_remove(m_eventSource);
        m_eventSource = nullptr;
    }
    if (m_eventFd >= 0) {
        close(m_eventFd);
        m_eventFd = -1;
    }
    std::scoped_lock lock(m_pendingMu);
    m_pendingStates.clear();
}

void COpenXRManager::reportState(eXRManagerState s) {
    // Frame thread: enqueue the transition and wake the main thread.
    {
        std::scoped_lock lock(m_pendingMu);
        m_pendingStates.push_back(s);
    }
    if (m_eventFd >= 0) {
        const uint64_t one = 1;
        [[maybe_unused]] auto _ = write(m_eventFd, &one, sizeof(one));
    }
}

void COpenXRManager::onFrameChannelReadable() {
    // Main thread: consume the eventfd and drain the queued transitions.
    uint64_t v = 0;
    [[maybe_unused]] auto _ = read(m_eventFd, &v, sizeof(v));

    std::vector<eXRManagerState> pending;
    {
        std::scoped_lock lock(m_pendingMu);
        pending.swap(m_pendingStates);
    }
    for (auto s : pending)
        setState(s);

    // EXITING / LOSS_PENDING: the frame loop has already exited. Defer stop() out of this
    // fd callback (doLater is main-thread-safe) so we don't remove the event source from
    // within its own dispatch. stop() decides DISABLED vs UNAVAILABLE from m_instanceLost.
    if (m_frameRequestedTeardown.exchange(false) && g_pEventLoopManager)
        g_pEventLoopManager->doLater([this] { stop(); });
}

void COpenXRManager::frameThread() {
    // The frame thread exclusively owns the EGL context while running. For WP2 we submit
    // ZERO composition layers (quad layers land in WP3) — the frame lifecycle still runs so
    // the runtime advances the session state machine, and transitions are reported to main.
    eXRManagerState lastReported = XR_STATE_RUNNING_IDLE;

    while (m_running.load()) {
        m_session->pollEvents();

        if (m_session->m_exitRequested) {
            // Signal the main thread to run teardown; the loop exits so the join is instant.
            m_frameRequestedTeardown.store(true);
            if (m_eventFd >= 0) {
                const uint64_t one = 1;
                [[maybe_unused]] auto _ = write(m_eventFd, &one, sizeof(one));
            }
            break;
        }

        const eXRManagerState mapped = mapSessionState((int)m_session->m_xrState);
        if (mapped != lastReported) {
            reportState(mapped);
            lastReported = mapped;
        }

        // DEVIATION from doc 01's frame-loop pseudocode (which gates the pump on the state
        // already being SYNCHRONIZED/VISIBLE/FOCUSED): the runtime only advances
        // READY -> SYNCHRONIZED -> VISIBLE once the app STARTS submitting frames, so gating
        // the pump on those states deadlocks the session at READY (confirmed against
        // Monado's null compositor). We therefore pump the frame lifecycle as soon as the
        // session is running (m_sessionBegan). xrWaitFrame blocks until the next frame time,
        // so this needs no throttle. Pacing (scheduleFrame) stays VISIBLE/FOCUSED-gated once
        // monitors exist (WP3). Reported to WP13 for a doc-01 amendment.
        if (!m_session->m_sessionBegan) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50)); // WIP idle throttle
            continue;
        }

        XrFrameWaitInfo waitInfo = {XR_TYPE_FRAME_WAIT_INFO};
        XrFrameState    fs       = {XR_TYPE_FRAME_STATE};
        if (XR_FAILED(xrWaitFrame(m_session->m_session, &waitInfo, &fs)))
            continue;

        XrFrameBeginInfo beginInfo = {XR_TYPE_FRAME_BEGIN_INFO};
        if (XR_FAILED(xrBeginFrame(m_session->m_session, &beginInfo)))
            continue;

        // xrEndFrame with zero layers is valid (nothing composited yet) — doc 01.
        XrFrameEndInfo endInfo       = {XR_TYPE_FRAME_END_INFO};
        endInfo.displayTime          = fs.predictedDisplayTime;
        endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        endInfo.layerCount           = 0;
        endInfo.layers               = nullptr;
        xrEndFrame(m_session->m_session, &endInfo);
    }
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
