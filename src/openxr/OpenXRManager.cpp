#include "OpenXRManager.hpp"
#ifdef HAVE_OPENXR

// Include contract (doc 01): platform macros before EGL/GLES, those before the OpenXR
// platform header (which declares XrSwapchainImageOpenGLESKHR).
#define XR_USE_PLATFORM_EGL
#define XR_USE_GRAPHICS_API_OPENGL_ES

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <numeric>
#include <cstdint>
#include <chrono>
#include <cmath>
#include <format>
#include <optional>
#include <ranges>
#include <variant>

#include <sys/eventfd.h>
#include <unistd.h>
#include <wayland-server-core.h>

#include "XRIpc.hpp"
#include "XRSession.hpp"
#include "XRGraphics.hpp"
#include "XRMonitorLayer.hpp"
#include "XRInput.hpp"
#include "XRPointerDevice.hpp"
#include "../Compositor.hpp"
#include "../debug/log/Logger.hpp"
#include "../config/ConfigValue.hpp"
#include "../event/EventBus.hpp"
#include "../managers/EventManager.hpp"
#include "../managers/eventLoop/EventLoopManager.hpp"
#include "../managers/input/InputManager.hpp"
#include "../output/Monitor.hpp"
#include "../state/MonitorState.hpp"
#include "../desktop/state/FocusState.hpp"
#include "../config/shared/monitor/MonitorRuleManager.hpp"
#include "../config/shared/monitor/MonitorRule.hpp"
#include "../config/legacy/ConfigManager.hpp"
#include "../helpers/time/Time.hpp"

#include <aquamarine/backend/Backend.hpp>
#include <aquamarine/backend/Headless.hpp>
#include <aquamarine/buffer/Buffer.hpp>

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

    // Materialize any monitors declared in the config as plain headless outputs (doc 02 lazy
    // binding). Their quads bind when a session later starts. Done before start() so start()'s
    // bindExistingLayers() picks them up.
    reconcileDeclaredMonitors();

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

std::string COpenXRManager::blendModeName() const {
    // Reflect the mode the frame loop actually submits while a session exists; the OPAQUE default
    // otherwise (nothing composited).
    if (m_session)
        return OpenXR::blendModeToString(xrBlendModeFromXr(m_session->m_blendMode));
    return OpenXR::blendModeToString(OpenXR::XR_BLEND_OPAQUE);
}

bool COpenXRManager::isOverlay() const {
    return m_session && m_session->m_isOverlay;
}

std::array<COpenXRManager::SXRHandInputInfo, 2> COpenXRManager::handInputInfos() const {
    // WP-G5: reflect each hand's active device for `hyprctl openxr status`. m_input's per-hand kind
    // is an atomic mirror (main-thread safe to read); hand_grab is read from config here.
    static auto                     PHANDGRAB   = CConfigValue<std::string>("openxr:hand_grab");
    static auto                     PGRABFILTER = CConfigValue<Hyprlang::INT>("openxr:grab_filter");
    const bool                      filterOn    = *PGRABFILTER != 0;
    std::array<SXRHandInputInfo, 2> out;
    for (int h = 0; h < 2; ++h) {
        const bool hands = m_input && m_input->handInputKind((OpenXR::eXRHand)h) == OpenXR::XR_INPUT_HANDS;
        out[h].hands     = hands;
        out[h].gesture   = hands ? OpenXR::xrHandGrabName(OpenXR::xrParseHandGrab(*PHANDGRAB)) : "";
        out[h].filtered  = hands && filterOn; // WP-G6: hand move-grabs will be 1€-filtered
    }
    return out;
}

bool COpenXRManager::shouldInhibitIdle() {
    // doc 05 §6.1. FOCUSED (and only FOCUSED) inhibits: the headset is on and this session has
    // input focus. VISIBLE (e.g. runtime dashboard in front) intentionally does not inhibit.
    static auto PINHIBIT = CConfigValue<Hyprlang::INT>("openxr:inhibit_idle");
    return *PINHIBIT && m_state == XR_STATE_RUNNING_FOCUSED;
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

    // Overlay session (doc 01). Read openxr:overlay / openxr:overlay_z once, BEFORE createInstance
    // (which enables XR_EXTX_overlay only if requested AND advertised). Same semantics as
    // blend_mode: read at session start — changing requires disable/enable. Requested-but-
    // unsupported downgrades to a normal session with a WARN (never fails startup).
    {
        static auto POVERLAY          = CConfigValue<Hyprlang::INT>("openxr:overlay");
        static auto POVERLAYZ         = CConfigValue<Hyprlang::INT>("openxr:overlay_z");
        m_session->m_overlayRequested = (*POVERLAY != 0);
        m_session->m_overlayZ         = (uint32_t)std::max<int64_t>(0, (int64_t)*POVERLAYZ);
    }

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

    // Select the environment blend mode (doc 01) from openxr:blend_mode against the runtime's
    // enumerated list (getSystem filled m_blendModes in preference order). Read once at session
    // start — changing openxr:blend_mode takes effect on the next start. `auto` picks the runtime
    // preferred mode; an explicit mode the runtime doesn't advertise falls back with a WARN.
    {
        static auto      PBLEND = CConfigValue<std::string>("openxr:blend_mode");
        const auto       pick   = OpenXR::pickBlendMode(m_session->m_blendModes, *PBLEND);
        m_session->m_blendMode  = xrBlendModeToXr(pick.mode);
        if (pick.requestedUnsupported)
            Log::logger->log(Log::WARN, "[OPENXR] openxr:blend_mode '{}' is not supported by this runtime; falling back to '{}'", *PBLEND,
                             OpenXR::blendModeToString(pick.mode));
        else
            Log::logger->log(Log::DEBUG, "[OPENXR] environment blend mode: {} (openxr:blend_mode = {})", OpenXR::blendModeToString(pick.mode), *PBLEND);
    }

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

    // 8. Action system (WP6): build the action set + bindings + action spaces and attach them to
    //    the session (attach is legal only once, and must happen before the frame loop begins).
    //    A missing interaction profile is not fatal; a genuine failure leaves input disabled but
    //    the session running (quads still composite, device anchors just take the loss path).
    m_input = makeUnique<CXRInput>();
    if (!m_input->init(*m_session, m_session->m_hasHandInteraction)) {
        Log::logger->log(Log::WARN, "[OPENXR] input action system unavailable; continuing without XR input");
        m_input.reset();
    }

    // 9. Bind any layers whose monitor still exists (created while disabled — doc 02), then
    //    set up the frame->main channel and spawn the frame thread.
    bindExistingLayers();

    if (!setupFrameChannel()) {
        abortStart();
        return;
    }

    // The frame thread is the single producer for the frame->main queue; give CXRInput the
    // producer sink now that the eventfd exists and before the thread spawns.
    if (m_input)
        m_input->setEmitter([this](XRQueueItem item) { enqueue(std::move(item)); });

    m_running     = true;
    m_frameThread = std::thread([this] { frameThread(); });

    setState(XR_STATE_RUNNING_IDLE);
    Log::logger->log(Log::DEBUG, "[OPENXR] session up (runtime: {}, system: {}), frame thread started", m_runtimeName, m_systemName);

    // Monitors now come only from the config keyword / dispatcher / hyprctl (WP4). Any monitors
    // declared/created while disabled were already materialized as headless outputs and bound
    // above (bindExistingLayers); create any declared-but-missing ones now that a session is up.
    reconcileDeclaredMonitors();
    recomputeQuadActive();

    // Register the synthetic ray pointer (doc 04 §8), honoring openxr:pointer.
    ensurePointerDevice();
}

void COpenXRManager::abortStart() {
    // No frame thread has been started at any abortStart() call site, so there is nothing
    // to join. Tear down in the doc-01 order: GL -> XR handles -> EGL.
    teardownFrameChannel();
    if (m_graphics)
        m_graphics->destroyGL();
    if (m_input) {
        m_input->destroy(); // action spaces are session children — destroy before the session
        m_input.reset();
    }
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

    // Remove the ray pointer first (main thread) so no further input routes while tearing down.
    removePointerDevice();

    // Teardown ordering (doc 01): join the frame thread BEFORE touching any EGL/XR object.
    m_running = false;
    if (m_frameThread.joinable())
        m_frameThread.join();

    teardownFrameChannel();

    // Teardown ordering (doc 01). The frame thread is gone, so no removal barrier is needed
    // (path C): destroy per-layer frame resources directly.
    // 2. GL cleanup with the context current — per-layer EGLImage/staging tex, then shared.
    if (m_graphics && m_graphics->m_xrContext != EGL_NO_CONTEXT) {
        CXRGraphics::CScopedGLContext ctx(*m_graphics);
        for (auto& l : m_layers)
            l->destroyFrameResourcesGL(*m_graphics);
    }
    if (m_graphics)
        m_graphics->destroyGL();

    // 3. Per-layer swapchains (context NOT current), then the action system, then XR handles.
    for (auto& l : m_layers)
        l->destroySwapchain();
    if (m_input) {
        m_input->destroy(); // action spaces are session children — destroy before the session
        m_input.reset();
    }
    if (m_session)
        m_session->destroy();

    // 5. EGL/GBM.
    if (m_graphics)
        m_graphics->destroyEGL();

    // 6. Monitor disposition per openxr:destroy_monitors_on_stop.
    teardownLayers();

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
    m_queue.reset(); // no producer/consumer yet — safe to clear any stale items from a prior run
    m_queueOverflowed.store(false);

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
    // The frame thread is joined before teardownFrameChannel runs, so this is single-threaded.
    m_queue.reset();
}

void COpenXRManager::wakeMain() {
    if (m_eventFd < 0)
        return;
    const uint64_t        one = 1;
    [[maybe_unused]] auto _   = write(m_eventFd, &one, sizeof(one));
}

void COpenXRManager::enqueue(XRQueueItem item) {
    // Frame thread (single producer). Full-queue policy (doc 04 §7.2): MOTION_ABS/AXIS may be
    // dropped (regenerated next frame); BUTTON/FRAME and every SXRStateEvent must not be — log
    // once if that ever happens (1024-deep with per-dispatch drain cannot realistically fill).
    bool droppable = false;
    if (auto* ie = std::get_if<SXRInputEvent>(&item))
        droppable = ie->type == eXRInputEventType::MOTION_ABS || ie->type == eXRInputEventType::AXIS;

    if (!m_queue.push(std::move(item))) {
        if (!droppable && !m_queueOverflowed.exchange(true))
            Log::logger->log(Log::ERR, "[OPENXR] frame->main event queue overflow; a non-droppable event was lost");
        return;
    }
    wakeMain();
}

void COpenXRManager::reportState(eXRManagerState s) {
    // Frame thread: SESSION_STATE event carrying the already-mapped manager state.
    SXRStateEvent ev;
    ev.type = eXRStateEventType::SESSION_STATE;
    ev.a    = (int32_t)s;
    enqueue(ev);
}

void COpenXRManager::dispatchStateEvent(const SXRStateEvent& e) {
    switch (e.type) {
        case eXRStateEventType::SESSION_STATE: setState((eXRManagerState)e.a); break;
        case eXRStateEventType::LAYER_REMOVED:
            // Removal barrier step 3: the frame thread released a layer's resources and acked.
            finalizeLayerRemoval(e.str);
            break;
        case eXRStateEventType::GRAB:
            // The frame thread already resolved the monitor name at grab-begin/end time (it
            // carries a race with the layer-removed ack otherwise — doc 05 §5). Post verbatim.
            if (!e.str.empty() && g_pEventManager)
                g_pEventManager->postEvent(SHyprIPCEvent{"xrmonitorgrab", e.str + (e.a ? ",1" : ",0")});
            break;
        case eXRStateEventType::TRACKING: Log::logger->log(Log::DEBUG, "[OPENXR] device-lock tracking {} (monitor {})", e.a ? "gained" : "lost", e.str); break;
        case eXRStateEventType::SCHEDULE_FRAMES: {
            // Pacing on behalf of the frame thread (see the frame loop): scheduleFrame() is
            // main-thread-only (aquamarine idle-callback list has no lock).
            std::scoped_lock lock(m_layersMu);
            for (auto& l : m_layers) {
                if (l->m_pendingRemoval.load(std::memory_order_acquire))
                    continue;
                if (auto mon = l->m_monitor.lock())
                    mon->scheduleFrame();
            }
            break;
        }
    }
}

void COpenXRManager::dispatchInputEvent(const SXRInputEvent& e) {
    // Main thread: replay a frame-thread pointer event onto the synthetic device (doc 04 §8).
    // Hover bookkeeping (m_hovered / last-hovered) is kept up to date even when the pointer device
    // is absent (openxr:pointer = 0) so status JSON and selection still reflect the ray.
    switch (e.type) {
        case eXRInputEventType::MOTION_ABS: {
            if (e.monitorID < 0) {
                setHoveredMonitor(""); // ray left all quads
                break;
            }
            auto layer = layerByMonitorID(e.monitorID);
            if (!layer)
                break; // monitor died in flight
            setHoveredMonitor(layer->m_monitorName);
            if (m_pointerDevice) {
                // m_boundOutput routes the 0-1 absolute onto this monitor's box (doc 04 §8).
                m_pointerDevice->m_boundOutput = layer->m_monitorName;
                m_pointerDevice->m_pointerEvents.motionAbsolute.emit(IPointer::SMotionAbsoluteEvent{.timeMs = e.timeMs, .absolute = e.uv, .device = m_pointerDevice});
            }
            break;
        }
        case eXRInputEventType::BUTTON:
            if (m_pointerDevice)
                m_pointerDevice->m_pointerEvents.button.emit(IPointer::SButtonEvent{
                    .timeMs = e.timeMs, .button = e.button, .state = e.pressed ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED, .mouse = true});
            break;
        case eXRInputEventType::AXIS:
            if (m_pointerDevice)
                m_pointerDevice->m_pointerEvents.axis.emit(
                    IPointer::SAxisEvent{.timeMs = e.timeMs, .source = WL_POINTER_AXIS_SOURCE_CONTINUOUS, .axis = e.axis, .delta = e.axisDelta, .deltaDiscrete = 0, .mouse = true});
            break;
        case eXRInputEventType::FRAME:
            if (m_pointerDevice)
                m_pointerDevice->m_pointerEvents.frame.emit();
            break;
    }
}

void COpenXRManager::setHoveredMonitor(const std::string& name) {
    if (m_curHoveredMonitor == name)
        return;
    if (!m_curHoveredMonitor.empty())
        if (auto prev = layerByName(m_curHoveredMonitor))
            prev->m_hovered = false;
    m_curHoveredMonitor = name;
    if (!name.empty()) {
        if (auto l = layerByName(name))
            l->m_hovered = true;
        m_lastHoveredMonitor = name; // selection rule 2 (doc 05 §3.2) — persists as "last"
    }
}

PXRLAYER COpenXRManager::layerByMonitorID(MONITORID id) {
    std::scoped_lock lock(m_layersMu);
    for (auto& l : m_layers) {
        if (l->m_pendingRemoval.load(std::memory_order_acquire))
            continue;
        if (auto mon = l->m_monitor.lock(); mon && mon->m_id == id)
            return l;
    }
    return nullptr;
}

void COpenXRManager::ensurePointerDevice() {
    static auto PPOINTER = CConfigValue<Hyprlang::INT>("openxr:pointer");
    if (!*PPOINTER || !m_running.load() || m_pointerDevice || !g_pInputManager)
        return;
    m_pointerDevice = CXRPointerDevice::create();
    g_pInputManager->newMouse(m_pointerDevice); // -> setupMouse -> attachPointer + destroy listener
    Log::logger->log(Log::DEBUG, "[OPENXR] ray pointer device registered");
}

void COpenXRManager::removePointerDevice() {
    if (!m_pointerDevice)
        return;
    m_pointerDevice->destroy(); // fires m_events.destroy -> InputManager + PointerManager detach
    m_pointerDevice.reset();
    m_curHoveredMonitor.clear();
    Log::logger->log(Log::DEBUG, "[OPENXR] ray pointer device removed");
}

void COpenXRManager::onFrameChannelReadable() {
    // Main thread (single consumer): consume the eventfd counter, then drain the ring to empty.
    uint64_t              v = 0;
    [[maybe_unused]] auto _ = read(m_eventFd, &v, sizeof(v));

    XRQueueItem           item;
    while (m_queue.pop(item)) {
        if (auto* se = std::get_if<SXRStateEvent>(&item))
            dispatchStateEvent(*se);
        else if (auto* ie = std::get_if<SXRInputEvent>(&item))
            dispatchInputEvent(*ie);
    }

    // EXITING / LOSS_PENDING: the frame loop has already exited. Defer stop() out of this
    // fd callback (doLater is main-thread-safe) so we don't remove the event source from
    // within its own dispatch. stop() decides DISABLED vs UNAVAILABLE from m_instanceLost.
    if (m_frameRequestedTeardown.exchange(false) && g_pEventLoopManager)
        g_pEventLoopManager->doLater([this] { stop(); });
}

void COpenXRManager::frameThread() {
    // The frame thread exclusively owns the EGL context while running. It snapshots the
    // layer set once per frame, blits each layer's latest presented buffer into its
    // swapchain, and submits one XrCompositionLayerQuad per layer (doc 01 loop / doc 02).
    eXRManagerState lastReported  = XR_STATE_RUNNING_IDLE;
    int64_t         lastPredicted = 0; // XrTime (ns) of the previous frame, for the solve dt

    while (m_running.load()) {
        m_session->pollEvents();

        // WP-G5: forward an interaction-profile change to the input system so it re-reads each
        // hand's active device (hands vs controllers) on the next sample().
        if (m_input && m_session->m_interactionProfileChanged) {
            m_session->m_interactionProfileChanged = false;
            m_input->notifyInteractionProfileChanged();
        }

        // Recenter (doc 03 §6): re-express every anchor across a reference-space change.
        if (m_session->m_recenterPending) {
            const OpenXR::SXRPose M      = m_session->m_recenterPose;
            const bool            valid  = m_session->m_recenterPoseValid;
            m_session->m_recenterPending = false;
            if (valid) {
                std::scoped_lock lock(m_layersMu);
                for (auto& l : m_layers)
                    l->m_anchor.onReferenceSpaceChanged(M);
            }
        }

        if (m_session->m_exitRequested) {
            // Signal the main thread to run teardown; the loop exits so the join is instant.
            m_frameRequestedTeardown.store(true);
            wakeMain();
            break;
        }

        const eXRManagerState mapped = mapSessionState((int)m_session->m_xrState);
        if (mapped != lastReported) {
            reportState(mapped);
            lastReported = mapped;
        }

        // Snapshot the layer set: bound + non-pending-removal layers become the active set;
        // pending-removal layers are collected for frame-side teardown (removal barrier
        // step 2). Both under m_layersMu (doc 00 handoff table).
        std::vector<PXRLAYER> active;
        std::vector<PXRLAYER> toRemove;
        {
            std::scoped_lock lock(m_layersMu);
            active.reserve(m_layers.size());
            for (auto& l : m_layers) {
                if (l->m_pendingRemoval.load(std::memory_order_acquire))
                    toRemove.push_back(l);
                else
                    active.push_back(l);
            }
        }

        // Removal barrier step 2: this layer is no longer submitted/blitted. Destroy its
        // frame-side resources (once), DROP our refs, then ack to main. Order matters: main
        // erases the layer on ack, and the last shared_ptr must die on the main thread —
        // ~CXRMonitorLayer releases hyprutils WPs/listeners, which must not be released
        // concurrently with main-thread refcount traffic (see XRMonitorLayer.hpp).
        std::vector<std::string> removedNames;
        for (auto& l : toRemove) {
            if (l->m_removalAcked.exchange(true))
                continue;
            {
                CXRGraphics::CScopedGLContext ctx(*m_graphics);
                l->destroyFrameResourcesGL(*m_graphics);
            }
            l->destroySwapchain(); // context NOT current
            removedNames.push_back(l->m_monitorName);
        }
        toRemove.clear(); // drop refs BEFORE the acks — main may finalize (and erase) immediately
        for (auto& name : removedNames)
            reportLayerRemoved(name);

        // DEVIATION from doc 01's frame-loop pseudocode (which gates the pump on the state
        // already being SYNCHRONIZED/VISIBLE/FOCUSED): the runtime only advances
        // READY -> SYNCHRONIZED -> VISIBLE once the app STARTS submitting frames, so gating
        // the pump on those states deadlocks the session at READY (confirmed against
        // Monado's null compositor). We therefore pump the frame lifecycle as soon as the
        // session is running (m_sessionBegan). xrWaitFrame blocks until the next frame time,
        // so this needs no throttle. Reported to WP13 for a doc-01 amendment.
        if (!m_session->m_sessionBegan) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50)); // WIP idle throttle
            continue;
        }

        const bool visible = m_session->m_xrState == XR_SESSION_STATE_VISIBLE || m_session->m_xrState == XR_SESSION_STATE_FOCUSED;

        // Pacing (doc 02): drive compositor renders of each visible XR monitor at the XR
        // cadence, VISIBLE/FOCUSED only — no scheduleFrame churn while idle. Routed through
        // the frame->main queue: CMonitor::scheduleFrame() lands in aquamarine's idle-callback
        // vector, which the main thread concurrently drains in CBackend::dispatchIdle with no
        // lock — calling it from this thread corrupts the heap.
        if (visible && !active.empty())
            enqueue(SXRStateEvent{.type = eXRStateEventType::SCHEDULE_FRAMES});

        XrFrameWaitInfo waitInfo = {XR_TYPE_FRAME_WAIT_INFO};
        XrFrameState    fs       = {XR_TYPE_FRAME_STATE};
        if (XR_FAILED(xrWaitFrame(m_session->m_session, &waitInfo, &fs)))
            continue;

        XrFrameBeginInfo beginInfo = {XR_TYPE_FRAME_BEGIN_INFO};
        if (XR_FAILED(xrBeginFrame(m_session->m_session, &beginInfo)))
            continue;

        // Sample controller/hand actions once per frame (doc 04 §2): xrSyncActions + per-hand
        // aim/grip pose + analog reads at the predicted display time, located in the reference
        // space. Only delivers real input while the session is FOCUSED (a success code otherwise).
        if (m_input)
            m_input->sample(fs.predictedDisplayTime, m_session->m_refSpace);

        // Per-layer: ensure a swapchain, then blit the latest presented buffer into it.
        for (auto& l : active) {
            // Mode change: recreate the swapchain at the new pixel size (doc 02).
            if (l->m_swapchainDirty.exchange(false, std::memory_order_acq_rel)) {
                Vector2D newSize;
                {
                    std::lock_guard<std::mutex> lk(l->m_bufMu);
                    newSize = l->m_pendingSize;
                }
                if (newSize.x >= 1 && newSize.y >= 1)
                    createLayerSwapchain(*l, newSize);
            }

            // First bind: create a swapchain sized to the monitor's pixel mode, as cached by
            // the main thread at bind/modeChanged (m_pendingSize under m_bufMu). The frame
            // thread must NOT lock() m_monitor — hyprutils refcounts are not atomic and racing
            // the main thread's copies corrupts them (see XRMonitorLayer.hpp).
            if (l->m_swapchain == XR_NULL_HANDLE) {
                Vector2D size;
                {
                    std::lock_guard<std::mutex> lk(l->m_bufMu);
                    size = l->m_pendingSize;
                }
                if (size.x >= 1 && size.y >= 1)
                    createLayerSwapchain(*l, size);
            }

            if (l->m_swapchain == XR_NULL_HANDLE)
                continue; // no swapchain yet (monitor has no mode) — skip this layer

            auto buf = l->takeLatestBuffer();

            // ---- WP-G2 chrome auto-hide fade (frame thread; predicted-display-time deltas, no
            // wall clock). hoverRegion/grabbedNow were written by LAST frame's processPointer
            // plumbing (end of this loop iteration) — a deliberate one-frame latency. activeNow
            // drives the envelope: fade in while the ray hovers/grabs this quad, out after the
            // hide delay. Advanced every frame regardless of whether we redraw. ----
            static auto   PFADEMS    = CConfigValue<Hyprlang::INT>("openxr:chrome_fade_ms");
            static auto   PHIDEMS    = CConfigValue<Hyprlang::INT>("openxr:chrome_hide_delay_ms");
            const uint8_t hoverReg   = l->m_hoverRegion.load(std::memory_order_acquire);
            const bool    grabbedNow = l->m_grabbedNow.load(std::memory_order_acquire);
            const bool    activeNow  = grabbedNow || hoverReg != OpenXR::XR_REGION_NONE;
            const bool    chromeOn   = l->m_chrome.hasChrome();

            const int64_t nowNs = fs.predictedDisplayTime;
            if (activeNow)
                l->m_chromeActiveNs = nowNs;
            float dtSec = 0.f;
            if (l->m_chromeUpdateNs != 0 && nowNs > l->m_chromeUpdateNs)
                dtSec = std::min(0.1f, (float)(nowNs - l->m_chromeUpdateNs) / 1e9f);
            l->m_chromeUpdateNs = nowNs;
            // m_chromeActiveNs == 0 means "never hovered/grabbed yet" -> treat as long-past so the
            // chrome starts (and stays) hidden until the first real interaction.
            const float sinceActiveSec = l->m_chromeActiveNs == 0 ? 1e9f : (float)(nowNs - l->m_chromeActiveNs) / 1e9f;
            const float newAlpha =
                chromeOn ? OpenXR::chromeFadeAdvance(l->m_chromeAlpha, activeNow, dtSec, sinceActiveSec, (float)*PFADEMS / 1000.f, (float)*PHIDEMS / 1000.f) : 0.f;
            l->m_chromeAlpha = newAlpha;

            // A chrome-only (no new desktop buffer) redraw is needed ONLY when the on-screen chrome
            // would actually differ (alpha/region/grab changed) and something is or was visible.
            // This is what keeps a static desktop with hidden chrome at zero GPU cost — the quad
            // re-presents the most recently released image every runtime frame (doc 01).
            const bool chromeVisualChanged = newAlpha != l->m_chromeDrawnAlpha || hoverReg != l->m_chromeDrawnRegion || grabbedNow != l->m_chromeDrawnGrab;
            const bool wantAnimTick        = chromeOn && l->m_hasContent && chromeVisualChanged && (newAlpha > 0.f || l->m_chromeDrawnAlpha > 0.f);

            if (!buf && !wantAnimTick && l->m_hasContent)
                continue;

            XrSwapchainImageAcquireInfo acqInfo = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            uint32_t                    imgIdx  = 0;

            // The XR context must stay current from acquire through release (KHR_opengl_es_enable
            // contract): Monado's GL client inserts a native fence into OUR command stream inside
            // xrReleaseSwapchainImage — eglCreateSyncKHR requires a current context, and without
            // one the fence fails every frame and Monado falls back to an unsynchronized path
            // that corrupts the heap in-process.
            CXRGraphics::CScopedGLContext ctx(*m_graphics);

            if (XR_FAILED(xrAcquireSwapchainImage(l->m_swapchain, &acqInfo, &imgIdx))) {
                l->retireBuffer(std::move(buf)); // never let a valid buffer SP die on this thread
                continue;
            }
            XrSwapchainImageWaitInfo waitImg = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            waitImg.timeout                  = XR_INFINITE_DURATION;
            xrWaitSwapchainImage(l->m_swapchain, &waitImg);

            if (imgIdx < l->m_swapchainImages.size()) {
                const XR_GLuint dst = l->m_swapchainImages[imgIdx];
                if (buf) {
                    m_graphics->blitBuffer(buf, *l, dst);
                    if (!l->m_hasContent)
                        Log::logger->log(Log::DEBUG, "[OPENXR] first blit landed for XR monitor '{}' ({}x{})", l->m_monitorName, (int)l->m_swapchainSize.x,
                                         (int)l->m_swapchainSize.y);
                    l->m_hasContent = true;
                    // Snapshot the fresh content (WITHOUT chrome) so an animation-only frame can
                    // restore it into a different acquired image (WP-G2). Only when chrome is
                    // enabled — the extra copy is pure overhead otherwise (zero-cost disabled path).
                    if (chromeOn)
                        m_graphics->snapshotSwapchain(*l, dst);
                } else if (l->m_hasContent) {
                    // Animation-only frame: no new desktop buffer, chrome fading — restore the last
                    // content into this (possibly different) image, then draw chrome over it.
                    m_graphics->restoreSnapshot(*l, dst);
                } else
                    m_graphics->clearTex(dst, l->m_swapchainSize, 0.0f, 0.0f, 0.0f);

                // Chrome pass (WP-G2): draw the move-bar + corner handles into the transparent
                // margin over the content. No-op when disabled or fully faded out; drawChrome never
                // touches the content rect.
                if (chromeOn && l->m_hasContent) {
                    m_graphics->drawChrome(*l, dst, newAlpha, hoverReg, grabbedNow);
                    l->m_chromeDrawnAlpha  = newAlpha;
                    l->m_chromeDrawnRegion = hoverReg;
                    l->m_chromeDrawnGrab   = grabbedNow;
                }
            }

            XrSwapchainImageReleaseInfo relInfo = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            xrReleaseSwapchainImage(l->m_swapchain, &relInfo);

            // Hand the consumed buffer back for main-thread release — its final SP dec must
            // not happen on this thread (non-atomic refcounts, see XRMonitorLayer.hpp).
            l->retireBuffer(std::move(buf));
        }

        // --- anchor solve (WP5) ---
        // Locate the head (VIEW) pose in our reference space at the predicted display time. In the
        // LOCAL fallback (no LOCAL_FLOOR) shift +floor_offset so the solver works in floor-relative
        // coordinates; the final quad pose is shifted back before submission.
        static auto     PFLOOR      = CConfigValue<Hyprlang::FLOAT>("openxr:floor_offset");
        const float     floorOffset = (float)*PFLOOR;
        // WP-G6: 1€ hand-grab carry filter parameters, read per-frame (hot-toggles). Only a hand
        // MOVE grab with grabFilter set is filtered (see CXRAnchor::solve grab override).
        static auto     PGRABFILTER   = CConfigValue<Hyprlang::INT>("openxr:grab_filter");
        static auto     PGRABFILTERMC = CConfigValue<Hyprlang::FLOAT>("openxr:grab_filter_min_cutoff");
        static auto     PGRABFILTERB  = CConfigValue<Hyprlang::FLOAT>("openxr:grab_filter_beta");
        const bool      grabFilter    = *PGRABFILTER != 0;
        const float     grabFilterMc  = (float)*PGRABFILTERMC;
        const float     grabFilterB   = (float)*PGRABFILTERB;
        OpenXR::SXRPose viewPose;
        bool            viewValid = false;
        XrSpaceLocation loc       = {XR_TYPE_SPACE_LOCATION};
        if (XR_SUCCEEDED(xrLocateSpace(m_session->m_viewSpace, m_session->m_refSpace, fs.predictedDisplayTime, &loc)) &&
            (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) && (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
            viewPose  = xrToPose(loc.pose);
            viewValid = true;
            if (!m_session->m_usingLocalFloor)
                viewPose.pos.y += floorOffset;
        }

        // dt from predicted display time deltas (monotone, no wall clock); clamped in solve().
        float dt = 0.f;
        if (lastPredicted != 0 && fs.predictedDisplayTime > lastPredicted)
            dt = (float)(fs.predictedDisplayTime - lastPredicted) / 1e9f;
        lastPredicted = fs.predictedDisplayTime;

        const OpenXR::SXRAnchorTuning tune = readAnchorTuning();

        // Feed the sampled grip poses (in the reference space) into the anchor solve. In the LOCAL
        // fallback the solver works in floor-relative coordinates, so shift grips +floor_offset the
        // same way as the view; the device-lock late-latch path (below) composes that shift back
        // out algebraically, so the grip ActionSpace is still submitted unshifted.
        std::optional<OpenXR::SXRPose> gripLeft, gripRight;
        // WP-G5: hand pinch poses, floor-shifted identically to the grips (same LOCAL-fallback
        // handling) so a pinch-anchored hand grab carries in the same reference frame as everything
        // else. Empty for controller/remote-driver sessions (no pinch pose bound).
        std::optional<OpenXR::SXRPose> pinchLeft, pinchRight;
        if (m_input) {
            if (auto g = m_input->grip(OpenXR::XR_HAND_LEFT)) {
                if (!m_session->m_usingLocalFloor)
                    g->pos.y += floorOffset;
                gripLeft = g;
            }
            if (auto g = m_input->grip(OpenXR::XR_HAND_RIGHT)) {
                if (!m_session->m_usingLocalFloor)
                    g->pos.y += floorOffset;
                gripRight = g;
            }
            if (auto p = m_input->pinch(OpenXR::XR_HAND_LEFT)) {
                if (!m_session->m_usingLocalFloor)
                    p->pos.y += floorOffset;
                pinchLeft = p;
            }
            if (auto p = m_input->pinch(OpenXR::XR_HAND_RIGHT)) {
                if (!m_session->m_usingLocalFloor)
                    p->pos.y += floorOffset;
                pinchRight = p;
            }
        }

        std::vector<OpenXR::SXRSolveResult> results(active.size());
        std::vector<bool>                   solved(active.size(), false);
        {
            std::scoped_lock lock(m_layersMu);
            m_lastVerbCtx = OpenXR::SXRVerbContext{viewPose, viewValid, gripLeft, gripRight};
            for (size_t i = 0; i < active.size(); ++i) {
                auto&      l         = active[i];
                const bool needsView = l->m_anchor.state().mode != OpenXR::XR_ANCHOR_LOCAL;
                if (viewValid || !needsView) {
                    OpenXR::SXRSolveInput in;
                    in.view       = viewPose;
                    in.dt         = dt;
                    in.gripLeft   = gripLeft;
                    in.gripRight  = gripRight;
                    in.pinchLeft  = pinchLeft;  // WP-G5: pinch-anchored hand MOVE grabs
                    in.pinchRight = pinchRight;
                    in.grabFilter          = grabFilter; // WP-G6: 1€ carry filter (hands, opt-in)
                    in.grabFilterMinCutoff = grabFilterMc;
                    in.grabFilterBeta      = grabFilterB;
                    // Aspect from the CONTENT pixel mode (not the chrome-expanded swapchain) so
                    // widthMeters/heightMeters stay CONTENT geometry — `size:` and layout
                    // serialization keep meaning content meters (WP-G1).
                    in.pxW       = (uint32_t)std::max(1.0, l->m_contentSize.x);
                    in.pxH       = (uint32_t)std::max(1.0, l->m_contentSize.y);
                    results[i]   = l->m_anchor.solve(in, tune);
                    solved[i]    = true;
                } else if (l->m_anchor.hasLastWorld()) {
                    // No head pose this frame: hold the quad at its last composed world pose.
                    results[i].space        = OpenXR::XR_SPACE_LOCAL_FLOOR;
                    results[i].pose         = l->m_anchor.lastWorld();
                    results[i].worldPose    = results[i].pose;
                    results[i].widthMeters  = l->m_anchor.state().widthMeters;
                    results[i].heightMeters = results[i].widthMeters * (float)l->m_contentSize.y / (float)std::max(1.0, l->m_contentSize.x);
                    solved[i]               = true;
                }
            }
        }

        std::vector<XrCompositionLayerQuad>              quads;
        std::vector<const XrCompositionLayerBaseHeader*> layerPtrs;
        quads.reserve(active.size());
        layerPtrs.reserve(active.size());

        // Composition order: OpenXR composites the layer array in SUBMISSION order (later
        // entries draw on top) — a quad's 3D position plays no part, so ordering must be
        // computed per frame from the solved poses: farthest-from-viewer first, so nearer
        // quads occlude farther ones. m_zOrder remains an explicit override tier; creation
        // seq breaks remaining ties.
        std::vector<size_t> order(active.size());
        std::iota(order.begin(), order.end(), 0);
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            if (active[a]->m_zOrder != active[b]->m_zOrder)
                return active[a]->m_zOrder < active[b]->m_zOrder;
            if (viewValid && solved[a] && solved[b]) {
                const float da = (results[a].worldPose.pos - viewPose.pos).length();
                const float db = (results[b].worldPose.pos - viewPose.pos).length();
                if (std::fabs(da - db) > 1e-4F)
                    return da > db; // farther composites first (ends up behind)
            }
            return active[a]->m_seq < active[b]->m_seq;
        });

        // Ray-pointer targets: the same visible quads, with their solved world pose expressed in
        // the reference frame the aim poses were sampled in (doc 04 §3). Built alongside the quad
        // array below so the two sets stay in lockstep.
        std::vector<SXRPointerTarget> pointerTargets;
        pointerTargets.reserve(active.size());

        for (size_t i : order) {
            auto& l = active[i];
            if (quads.size() >= m_session->m_maxLayerCount)
                break;
            if (!l->m_quadActive || !l->m_hasContent || !solved[i])
                continue;

            const int w = (int)l->m_swapchainSize.x;
            const int h = (int)l->m_swapchainSize.y;
            if (w <= 0 || h <= 0)
                continue;

            const OpenXR::SXRSolveResult& res = results[i];

            // Map the solver's space selector to a real XrSpace. A device-locked / grabbed quad
            // whose selector is a grip space is submitted against CXRInput's grip ActionSpace with
            // the stored grip-space offset verbatim — the runtime late-latches the controller pose
            // at display time (doc 03 §3.4), so the quad tracks 1:1 with zero added latency. Any
            // other selector (incl. a grip selector with no valid grip space) submits the world
            // pose in the reference space, with the LOCAL-fallback floor shift removed.
            XrSpace deviceSpace = XR_NULL_HANDLE;
            if (m_input) {
                if (res.space == OpenXR::XR_SPACE_GRIP_LEFT || res.space == OpenXR::XR_SPACE_GRIP_RIGHT)
                    deviceSpace = m_input->gripSpace(res.space == OpenXR::XR_SPACE_GRIP_LEFT ? OpenXR::XR_HAND_LEFT : OpenXR::XR_HAND_RIGHT);
                else if (res.space == OpenXR::XR_SPACE_PINCH_LEFT || res.space == OpenXR::XR_SPACE_PINCH_RIGHT)
                    // WP-G5: a pinch-anchored hand MOVE grab late-latches the pinch pose action space.
                    deviceSpace = m_input->pinchSpace(res.space == OpenXR::XR_SPACE_PINCH_LEFT ? OpenXR::XR_HAND_LEFT : OpenXR::XR_HAND_RIGHT);
            }

            XrSpace         quadSpace = m_session->m_refSpace;
            OpenXR::SXRPose quadPose;
            if (deviceSpace != XR_NULL_HANDLE) {
                quadSpace = deviceSpace;
                quadPose  = res.pose; // device-space offset; the grip/pinch ActionSpace is unshifted
            } else {
                quadPose = res.space == OpenXR::XR_SPACE_LOCAL_FLOOR ? res.pose : res.worldPose;
                if (!m_session->m_usingLocalFloor)
                    quadPose.pos.y -= floorOffset; // back to the LOCAL reference frame
            }

            // Chrome margins (WP-G1): the SOLVE poses the CONTENT center (`res` = content meters),
            // but the submitted quad is content + transparent margins. Grow to the full quad meters
            // (contentMeters / contentFrac) and shift the submit pose from the content center to the
            // quad geometric center so the content stays exactly where the anchor placed it — the
            // asymmetric bottom margin (which holds the move-bar) would otherwise drift layouts.
            const OpenXR::SXRChromeGeometry& chrome = l->m_chrome;
            const float                      quadW  = res.widthMeters / (chrome.contentFracW() > 0.f ? chrome.contentFracW() : 1.f);
            const float                      quadH  = res.heightMeters / (chrome.contentFracH() > 0.f ? chrome.contentFracH() : 1.f);
            const OpenXR::SXRPose            quadCenterPose = OpenXR::contentPoseToQuadCenter(quadPose, chrome, quadW, quadH);

            XrCompositionLayerQuad quad   = {XR_TYPE_COMPOSITION_LAYER_QUAD};
            quad.layerFlags               = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            quad.space                    = quadSpace;
            quad.eyeVisibility            = XR_EYE_VISIBILITY_BOTH;
            quad.subImage.swapchain       = l->m_swapchain;
            quad.subImage.imageRect       = {{0, 0}, {w, h}};
            quad.subImage.imageArrayIndex = 0;
            quad.pose                     = xrFromPose(quadCenterPose);
            quad.size                     = {quadW, quadH};
            quads.push_back(quad);

            // Ray-pointer target: hit-test against the FULL quad (content + margins) so chrome hits
            // (bar / corners) are classifiable — worldPose is the quad-center world pose in the aim
            // poses' reference frame (undo the LOCAL fallback floor shift, as the ref-frame submit
            // path above does; the chrome offset then goes from content to quad center). `chrome`
            // travels along so processPointer classifies each hit and remaps BODY hits to content
            // uv. `anchor` hands the grab machine (WP8) the live CXRAnchor, valid only for the rest
            // of this frame (backed by `active`, a local PXRLAYER vector). The monitor id comes from
            // the layer's main-thread-written cache — no m_monitor.lock() here (non-atomic
            // refcounts, see XRMonitorLayer.hpp).
            if (const auto MONID = l->m_monitorId.load(std::memory_order_acquire); MONID >= 0) {
                OpenXR::SXRPose worldRef = res.worldPose;
                if (!m_session->m_usingLocalFloor)
                    worldRef.pos.y -= floorOffset;
                SXRPointerTarget pt;
                pt.id        = MONID;
                pt.worldPose = OpenXR::contentPoseToQuadCenter(worldRef, chrome, quadW, quadH);
                pt.w         = quadW;
                pt.h         = quadH;
                pt.name      = l->m_monitorName;
                pt.anchor    = &l->m_anchor;
                pt.chrome    = chrome;
                pointerTargets.push_back(std::move(pt));
            }
        }
        for (auto& q : quads)
            layerPtrs.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&q));

        // Ray pointer (doc 04 §3-§5) + grab machine (doc 04 §6, WP8): cast every hand's aim ray
        // against the visible quads and emit motion/button/axis/frame/grab across the frame->main
        // queue. Runs after the solve so hit tests use this frame's poses; only produces events
        // while FOCUSED (aim poses are valid then). The grab machine mutates layer anchor state
        // (beginGrab/grabPushPull/grabResize/endGrab), so it runs under m_layersMu — the same
        // discipline as the solve loop above, guarding against a concurrent main-thread verb
        // (move/anchor/scale/...) touching the same CXRAnchor.
        if (m_input) {
            OpenXR::SXRSolveInput pointerSolveIn;
            pointerSolveIn.view       = viewPose;
            pointerSolveIn.dt         = dt;
            pointerSolveIn.gripLeft   = gripLeft;
            pointerSolveIn.gripRight  = gripRight;
            pointerSolveIn.pinchLeft  = pinchLeft; // WP-G5: pinch grab begin/carry/latch device pose
            pointerSolveIn.pinchRight = pinchRight;
            std::scoped_lock lock(m_layersMu);
            m_input->processPointer(pointerTargets, (uint32_t)Time::millis(Time::steadyNow()), pointerSolveIn, tune);

            // WP-G2 chrome visual-state plumbing: publish each active quad's current ray-hover
            // region + grab flag onto the layer for the NEXT frame's chrome draw pass (frame
            // thread → frame thread, plain atomics — no processPointer/grab-machine change; the
            // input path only EXPOSES its per-hand region/grab state via read-only accessors).
            for (auto& l : active) {
                const MONITORID mid = l->m_monitorId.load(std::memory_order_acquire);
                const auto reg = mid >= 0 ? m_input->chromeHoverRegion(mid) : OpenXR::XR_REGION_NONE;
                l->m_hoverRegion.store((uint8_t)reg, std::memory_order_release);
                l->m_grabbedNow.store(mid >= 0 && m_input->isMonitorGrabbed(mid), std::memory_order_release);
            }
        }

        // xrEndFrame with zero layers is valid (nothing composited yet) — doc 01.
        XrFrameEndInfo endInfo       = {XR_TYPE_FRAME_END_INFO};
        endInfo.displayTime          = fs.predictedDisplayTime;
        // Blend mode selected once at session start from openxr:blend_mode (doc 01). On
        // passthrough-capable runtimes ALPHA_BLEND composites quads over the on-device
        // passthrough underlay instead of a black void; the blit forces dst alpha to 1.0 so
        // monitors stay fully opaque against it (XRGraphics.cpp).
        endInfo.environmentBlendMode = m_session->m_blendMode;
        endInfo.layerCount           = (uint32_t)layerPtrs.size();
        endInfo.layers               = layerPtrs.empty() ? nullptr : layerPtrs.data();
        {
            // Monado's layer_commit inserts a fence via context_begin(SYNCHRONIZE), which
            // assumes the app's GL context is current (it never binds for that reason) — same
            // contract as xrReleaseSwapchainImage above.
            CXRGraphics::CScopedGLContext ctx(*m_graphics);
            xrEndFrame(m_session->m_session, &endInfo);
        }
    }
}

bool COpenXRManager::createLayerSwapchain(CXRMonitorLayer& layer, const Vector2D& size) {
    // Frame thread. Destroy any existing swapchain first (context NOT current — interop rule).
    if (layer.m_swapchain != XR_NULL_HANDLE)
        layer.destroySwapchain();

    // Chrome margins (WP-G1): expand the swapchain by a transparent margin around the content so
    // chrome (bottom move-bar + corner handles) has a place that never covers a desktop pixel.
    // `size` is the monitor's pixel mode = the inner content rect. Derive the normalized layout
    // from CONTENT meters + config, then the full px size from the same fractions — both stored on
    // the layer so the blit (px insets) and the quad-submit/classifier (fractions) share one
    // source. The anchor widthMeters read here is a benign unlocked frame-thread read (cosmetic
    // margin sizing only; content geometry is unaffected), same tolerance as the config reads.
    static auto PENABLED                   = CConfigValue<Hyprlang::INT>("openxr:chrome_enabled");
    static auto PMARGIN                     = CConfigValue<Hyprlang::FLOAT>("openxr:chrome_margin");
    static auto PBARH                       = CConfigValue<Hyprlang::FLOAT>("openxr:chrome_bar_height");
    static auto PBARWF                      = CConfigValue<Hyprlang::FLOAT>("openxr:chrome_bar_width_frac");
    static auto PCORNER                     = CConfigValue<Hyprlang::FLOAT>("openxr:chrome_corner_size");
    // Master toggle: chrome_enabled=0 collapses ALL margins to 0 -> makeChromeGeometry yields a
    // full-quad content rect (hasChrome()==false), so the swapchain is content-only and the draw
    // pass no-ops — zero visual change vs. pre-WP-G1 (the documented disable mechanism, doc §8).
    const bool                      chromeOn = *PENABLED;
    const float                     margin   = chromeOn ? (float)*PMARGIN : 0.f;
    const float                     barH     = chromeOn ? (float)*PBARH : 0.f;
    const float                     cW     = std::max(0.001f, layer.m_anchor.state().widthMeters);
    const float                     cH     = cW * (float)std::max(1.0, size.y) / (float)std::max(1.0, size.x);
    const OpenXR::SXRChromeGeometry chrome = OpenXR::makeChromeGeometry(cW, cH, margin, barH, (float)*PBARWF, (float)*PCORNER);

    // Full swapchain px = content px expanded by the same fractions (px/fraction stay consistent).
    const double   fw = chrome.contentFracW() > 0.0 ? (double)chrome.contentFracW() : 1.0;
    const double   fh = chrome.contentFracH() > 0.0 ? (double)chrome.contentFracH() : 1.0;
    const Vector2D fullSize{std::max(size.x, std::round(size.x / fw)), std::max(size.y, std::round(size.y / fh))};
    const Vector2D contentOffset{std::round(chrome.contentU0 * fullSize.x), std::round(chrome.contentV0 * fullSize.y)};

    XrSwapchainCreateInfo info = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.usageFlags            = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    info.format                = m_session->m_swapchainFormat;
    info.sampleCount           = 1;
    info.width                 = (uint32_t)fullSize.x;
    info.height                = (uint32_t)fullSize.y;
    info.faceCount             = 1;
    info.arraySize             = 1;
    info.mipCount              = 1;

    // Do NOT bind the context ourselves — Monado's context_begin calls eglMakeCurrent
    // internally; a context already current when it does so crashes AMD gallium (doc 01).
    eglMakeCurrent(m_graphics->m_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

    XrResult r = xrCreateSwapchain(m_session->m_session, &info, &layer.m_swapchain);
    if (XR_FAILED(r)) {
        Log::logger->log(Log::ERR, "[OPENXR] xrCreateSwapchain for '{}' ({}x{}) failed: {}", layer.m_monitorName, (int)fullSize.x, (int)fullSize.y, (int)r);
        layer.m_swapchain = XR_NULL_HANDLE;
        return false;
    }

    uint32_t imgCount = 0;
    xrEnumerateSwapchainImages(layer.m_swapchain, 0, &imgCount, nullptr);
    std::vector<XrSwapchainImageOpenGLESKHR> imgs(imgCount, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
    xrEnumerateSwapchainImages(layer.m_swapchain, imgCount, &imgCount, reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs.data()));
    layer.m_swapchainImages.clear();
    layer.m_swapchainImages.reserve(imgCount);
    for (auto& img : imgs)
        layer.m_swapchainImages.push_back(img.image);

    layer.m_swapchainSize   = fullSize;
    layer.m_contentSize     = size;
    layer.m_contentOffsetPx = contentOffset;
    layer.m_chrome          = chrome;
    layer.m_hasContent      = false;

    // Reset the per-layer CPU staging tex + chrome snapshot so they realloc to the new mode.
    {
        CXRGraphics::CScopedGLContext ctx(*m_graphics);
        m_graphics->destroyLayerGL(layer.m_lastEGLImg, layer.m_cpuTex, layer.m_contentTex);
    }
    layer.m_lastEGLImg     = nullptr;
    layer.m_cpuTex         = 0;
    layer.m_cpuTexSize     = Vector2D{};
    layer.m_contentTex     = 0;
    layer.m_contentTexSize = Vector2D{};
    // Chrome fade must start hidden after a (re)create so a resized/rebound quad does not flash
    // its chrome; the draw-diff trackers reset too so the first real frame draws cleanly.
    layer.m_chromeAlpha       = 0.f;
    layer.m_chromeDrawnAlpha  = 0.f;
    layer.m_chromeDrawnRegion = 0;
    layer.m_chromeDrawnGrab   = false;
    layer.m_chromeUpdateNs    = 0;
    layer.m_chromeActiveNs    = 0;

    Log::logger->log(Log::DEBUG, "[OPENXR] swapchain created for XR monitor '{}': {}x{} (content {}x{} @ +{},+{}), {} images, format 0x{:x}", layer.m_monitorName,
                     (int)fullSize.x, (int)fullSize.y, (int)size.x, (int)size.y, (int)contentOffset.x, (int)contentOffset.y, imgCount,
                     (unsigned long long)m_session->m_swapchainFormat);
    return true;
}

std::expected<PXRLAYER, std::string> COpenXRManager::createXRMonitor(const SXRMonitorParams& params) {
    // Runs on the main thread; works in EVERY manager state (doc 02 — that is what makes lazy
    // binding possible).

    // 1. Validate uniqueness (same checks as dispatchOutput, HyprCtl.cpp).
    if (params.m_name.empty())
        return std::unexpected<std::string>("monitor name must not be empty");
    for (auto const& m : State::monitorState()->allMonitors())
        if (m->m_name == params.m_name)
            return std::unexpected<std::string>("a monitor already uses that name");
    {
        std::scoped_lock lock(m_layersMu);
        for (auto& l : m_layers)
            if (l->m_monitorName == params.m_name)
                return std::unexpected<std::string>("an XR monitor with that name already exists");
    }

    // 2. Construct the layer (still unbound) and register it.
    static auto PSIZE      = CConfigValue<Hyprlang::FLOAT>("openxr:default_size");
    const float sizeMeters = params.m_sizeMeters.value_or((float)*PSIZE);
    auto        layer      = std::make_shared<CXRMonitorLayer>(params.m_name, ++m_seqCounter, sizeMeters);

    // Seed the anchoring engine (WP5). widthMeters is the live quad width; height derives from
    // the pixel mode each frame.
    OpenXR::SXRAnchorState st = params.m_anchor;
    st.widthMeters            = sizeMeters;
    // Create verb with no explicit anchor: place along the current gaze at default distance
    // (doc 05 §3.1). Falls back to a sensible default pose when there is no tracking yet.
    if (!params.m_anchorProvided) {
        static auto       PDEFDIST = CConfigValue<Hyprlang::FLOAT>("openxr:default_distance");
        const float       dist     = (float)*PDEFDIST;
        OpenXR::CXRAnchor tmp;
        st.mode = OpenXR::XR_ANCHOR_LOCAL;
        tmp.initFromState(st);
        if (tmp.applyCenter(currentVerbContext(), dist))
            st = tmp.m_state;
        else
            st.anchorPose = OpenXR::SXRPose{OpenXR::Vec3{0.f, 1.4f, -dist}, OpenXR::Quat{}}; // no tracking yet
    }
    layer->m_anchor.initFromState(st);
    layer->m_declaredAnchor = st;
    layer->m_reqResolution  = params.m_resolution;
    layer->m_reqRefresh     = params.m_refreshRate;
    {
        std::scoped_lock lock(m_layersMu);
        m_layers.push_back(layer);
    }

    // 3. Create the headless output (same recipe as dispatchOutput / the WIP).
    bool created = false;
    for (auto const& impl : g_pCompositor->m_aqBackend->getImplementations()) {
        if (impl->type() == Aquamarine::AQ_BACKEND_HEADLESS) {
            impl->createOutput(params.m_name);
            created = true;
            break;
        }
    }
    if (!created) {
        std::scoped_lock lock(m_layersMu);
        std::erase(m_layers, layer);
        return std::unexpected<std::string>("no headless backend available");
    }

    // newOutput -> monitorState()->add() -> CMonitor ctor + onConnect all run synchronously,
    // so the monitor is queryable now.
    auto mon = State::monitorState()->query().name(params.m_name).run();
    if (!mon) {
        std::scoped_lock lock(m_layersMu);
        std::erase(m_layers, layer);
        return std::unexpected<std::string>("headless output did not materialize");
    }

    // 4. Bind: cache the monitor + wire listeners. The onGone callback runs the removal
    //    barrier when the monitor is externally destroyed (path B).
    layer->bindToMonitor(mon, [this, name = params.m_name]() {
        PXRLAYER l;
        {
            std::scoped_lock lock(m_layersMu);
            for (auto& cand : m_layers)
                if (cand->m_monitorName == name) {
                    l = cand;
                    break;
                }
        }
        if (!l)
            return;
        if (m_running.load()) {
            // Frame thread is alive: hand off through the barrier (it acks -> finalize).
            l->stopMainListeners();
            l->m_pendingRemoval.store(true, std::memory_order_release);
        } else
            finalizeLayerRemoval(name); // no frame thread — clean up directly
    });

    // 5. Apply the requested pixel mode, if any. An explicit user `monitor=` rule matching this
    //    name wins (doc 02 step 5): only override the resolution when the matched rule left it at
    //    the default "preferred" (Vector2D{}). `xrmonitor=` owns existence + XR placement only.
    if (params.m_resolution && Config::monitorRuleMgr()) {
        Config::CMonitorRule rule       = Config::monitorRuleMgr()->get(mon);
        const bool           userSetRes = rule.m_resolution != Vector2D{};
        if (!userSetRes) {
            rule.m_resolution = *params.m_resolution;
            if (params.m_refreshRate)
                rule.m_refreshRate = *params.m_refreshRate;
            mon->applyMonitorRule(std::move(rule));
        } else
            Log::logger->log(Log::DEBUG, "[OPENXR] XR monitor '{}' keeps its explicit monitor= resolution", params.m_name);
    }

    // 6. Notify (doc 05 event surface).
    if (g_pEventManager)
        g_pEventManager->postEvent(SHyprIPCEvent{"xrmonitoradded", params.m_name});

    // 7. If a session is running, the frame thread creates the swapchain on its next pass (it
    //    already snapshots m_layers per frame, so no extra wakeup is needed).
    if (m_running.load())
        layer->m_swapchainDirty.store(true, std::memory_order_release);

    // 8. Layer cap: a new quad may push the oldest past maxLayerCount (doc 02 recency policy).
    recomputeQuadActive();

    Log::logger->log(Log::DEBUG, "[OPENXR] created XR monitor '{}' (seq {}, size {}m)", params.m_name, layer->m_seq, sizeMeters);
    return layer;
}

void COpenXRManager::destroyXRMonitor(const std::string& name) {
    PXRLAYER layer;
    {
        std::scoped_lock lock(m_layersMu);
        for (auto& l : m_layers)
            if (l->m_monitorName == name) {
                layer = l;
                break;
            }
    }
    if (!layer)
        return;

    if (m_running.load()) {
        // Path A: session running — removal barrier. Stop queueing buffers, flag removal; the
        // frame thread destroys frame-side resources and acks, then the main thread erases the
        // layer + destroys the output (finalizeLayerRemoval).
        layer->stopMainListeners();
        layer->m_pendingRemoval.store(true, std::memory_order_release);
    } else
        finalizeLayerRemoval(name); // no frame thread — direct teardown
}

void COpenXRManager::finalizeLayerRemoval(const std::string& name) {
    // Main thread. Erase the layer, then destroy its output if it still exists.
    PXRLAYER layer;
    {
        std::scoped_lock lock(m_layersMu);
        for (auto it = m_layers.begin(); it != m_layers.end(); ++it) {
            if ((*it)->m_monitorName == name) {
                layer = *it;
                m_layers.erase(it);
                break;
            }
        }
    }
    if (!layer)
        return;

    // Release any queued/retired presented buffers here, on the main thread — their final SP
    // dec must not happen in ~CXRMonitorLayer if that ever ran off-main (belt-and-braces; the
    // frame thread has already dropped its ref by the time it acks, see frameThread()).
    layer->releaseBuffers();

    // No frame thread (direct-teardown path): destroy any lingering frame resources. While
    // DISABLED there should be none, but be defensive.
    if (!m_running.load() && m_graphics) {
        if (m_graphics->m_xrContext != EGL_NO_CONTEXT) {
            CXRGraphics::CScopedGLContext ctx(*m_graphics);
            layer->destroyFrameResourcesGL(*m_graphics);
        }
        layer->destroySwapchain();
    }

    if (auto mon = layer->m_monitor.lock(); mon && mon->m_output)
        destroyOutputDeferred(mon->m_output); // path B (external destroy) already gone -> mon expired, skipped

    // Clear the explicit selection if it pointed at this monitor (doc 05 §3.2).
    if (m_selectedMonitor == name)
        m_selectedMonitor.clear();
    if (m_lastHoveredMonitor == name)
        m_lastHoveredMonitor.clear();

    // Freeing capacity may re-activate a suspended quad (doc 02 recency policy).
    recomputeQuadActive();

    if (g_pEventManager)
        g_pEventManager->postEvent(SHyprIPCEvent{"xrmonitorremoved", name});

    Log::logger->log(Log::DEBUG, "[OPENXR] destroyed XR monitor '{}'", name);
}

void COpenXRManager::destroyOutputDeferred(SP<Aquamarine::IOutput> output) {
    // Main thread. Works around an aquamarine headless-output lifetime bug that produced the
    // dispatchIdle -> CSignalBase::emitInternal use-after-free crash during XR monitor teardown.
    //
    // CHeadlessOutput::scheduleFrame() enqueues the output's own `framecb` (an
    // SP<std::function<void()>> that captures the output by raw `this` and holds no liveness guard)
    // into CBackend::idle.pending via addIdleEvent(). Neither CHeadlessOutput::destroy() nor
    // ~CHeadlessOutput removes that pending idle event. If the output is freed while a framecb is
    // still queued, the next CBackend::dispatchIdle() invokes it on freed memory —
    // events.frame.emit() on a dangling CSignal — which is exactly the observed crash
    // (aquamarineFDWrite -> dispatchIdle -> emitInternal). Our XR frame pacing calls
    // CMonitor::scheduleFrame() every runtime frame, so a headless XR output is almost always
    // "due" (the immediate addIdleEvent path in scheduleFrame) when it is torn down during the
    // heavy create/destroy churn of a config reload — hence the crash on most suite runs.
    //
    // We cannot remove the pending idle event (framecb is private to CHeadlessOutput) nor patch the
    // system aquamarine library, so instead we guarantee the output outlives any stale callback.
    // destroy() detaches it from the backend + monitor and (via CMonitor's destroy listener) clears
    // its frame listener, so the pending framecb becomes a harmless listener-less emit. We then keep
    // a reference alive inside a sentinel idle event, enqueued AFTER any pending framecb, which is
    // processed in the same dispatchIdle pass right after that framecb fires — dropping the last
    // reference only once the queue has drained past it.
    if (!output)
        return;

    output->destroy();

    if (!g_pCompositor || !g_pCompositor->m_aqBackend)
        return; // no backend to dispatch idle again (shutdown) — release the reference now.

    auto sentinel = makeShared<std::function<void(void)>>([output]() {}); // capture keeps `output` alive
    g_pCompositor->m_aqBackend->addIdleEvent(sentinel);
}

void COpenXRManager::bindExistingLayers() {
    // Main thread, on start(): layers created while disabled bind to their still-live monitor
    // and get marked dirty; layers whose named monitor disappeared are dropped (doc 02).
    std::vector<std::string> gone;
    {
        std::scoped_lock lock(m_layersMu);
        for (auto& l : m_layers) {
            if (l->m_monitor.lock())
                continue; // already bound
            auto mon = State::monitorState()->query().name(l->m_monitorName).run();
            if (!mon) {
                gone.push_back(l->m_monitorName);
                continue;
            }
            l->bindToMonitor(mon, [this, name = l->m_monitorName]() {
                PXRLAYER layer;
                {
                    std::scoped_lock lk(m_layersMu);
                    for (auto& cand : m_layers)
                        if (cand->m_monitorName == name) {
                            layer = cand;
                            break;
                        }
                }
                if (!layer)
                    return;
                if (m_running.load()) {
                    layer->stopMainListeners();
                    layer->m_pendingRemoval.store(true, std::memory_order_release);
                } else
                    finalizeLayerRemoval(name);
            });
            l->m_swapchainDirty.store(true, std::memory_order_release);
        }
    }
    for (auto& name : gone)
        finalizeLayerRemoval(name);
}

void COpenXRManager::teardownLayers() {
    // Main thread, during stop() after the join (frame resources already freed there).
    // Monitor disposition per openxr:destroy_monitors_on_stop.
    static auto                      PDESTROY = CConfigValue<Hyprlang::INT>("openxr:destroy_monitors_on_stop");
    const bool                       destroy  = *PDESTROY;

    std::vector<PXRLAYER> layers;
    {
        std::scoped_lock lock(m_layersMu);
        layers = m_layers;
    }

    for (auto& l : layers) {
        // Release presented-buffer refs on the main thread (no frame thread here — path C).
        l->releaseBuffers();
        if (destroy && l->m_createdByXR) {
            l->stopMainListeners();
            l->m_destroyListener.reset(); // avoid re-entering the removal path from our own destroy
            if (auto mon = l->m_monitor.lock(); mon && mon->m_output)
                destroyOutputDeferred(mon->m_output);
        } else {
            // Keep the output as a plain headless monitor; unbind the layer record so the next
            // start() re-binds it (lazy binding).
            l->m_swapchain = XR_NULL_HANDLE;
            l->m_swapchainImages.clear();
            l->m_hasContent = false;
            l->m_removalAcked.store(false);
            l->m_pendingRemoval.store(false);
        }
    }

    if (destroy) {
        std::scoped_lock lock(m_layersMu);
        std::erase_if(m_layers, [](const PXRLAYER& l) { return l->m_createdByXR; });
    }
}

void COpenXRManager::reportLayerRemoved(const std::string& name) {
    // Frame thread: enqueue the ack and wake the main thread (removal barrier step 2->3).
    SXRStateEvent ev;
    ev.type = eXRStateEventType::LAYER_REMOVED;
    ev.str  = name;
    enqueue(ev);
}

void COpenXRManager::markSwapchainsDirtyIfChromeChanged() {
    static auto PENABLED = CConfigValue<Hyprlang::INT>("openxr:chrome_enabled");
    static auto PMARGIN  = CConfigValue<Hyprlang::FLOAT>("openxr:chrome_margin");
    static auto PBARH    = CConfigValue<Hyprlang::FLOAT>("openxr:chrome_bar_height");
    static auto PBARWF   = CConfigValue<Hyprlang::FLOAT>("openxr:chrome_bar_width_frac");
    static auto PCORNER  = CConfigValue<Hyprlang::FLOAT>("openxr:chrome_corner_size");

    const std::array<double, 5> cur{(double)*PENABLED, (double)*PMARGIN, (double)*PBARH, (double)*PBARWF, (double)*PCORNER};
    const bool                  changed = !m_lastChromeGeom.has_value() || *m_lastChromeGeom != cur;
    m_lastChromeGeom                     = cur;
    if (!changed)
        return;

    // Force the frame thread to recreate every layer's swapchain, which recomputes m_contentSize/
    // m_contentOffsetPx/m_chrome from the new values (createLayerSwapchain). Without this the
    // margin px + chrome fractions stay frozen at their creation-time values.
    std::scoped_lock lock(m_layersMu);
    for (auto& l : m_layers)
        l->m_swapchainDirty.store(true, std::memory_order_release);
}

void COpenXRManager::onConfigReload() {
    static auto PENABLED = CConfigValue<Hyprlang::INT>("openxr:enabled");
    const bool  enabled  = *PENABLED;

    if (enabled && m_state == XR_STATE_DISABLED)
        start(); // start() reconciles declared monitors itself
    else if (!enabled && m_state != XR_STATE_DISABLED)
        stop();
    else
        reconcileDeclaredMonitors(); // no state edge: still reconcile the declared set

    // Hot-toggle the ray pointer device (doc 04 §8: openxr:pointer = 0 removes it live).
    if (m_running.load()) {
        static auto PPOINTER = CConfigValue<Hyprlang::INT>("openxr:pointer");
        if (*PPOINTER)
            ensurePointerDevice();
        else
            removePointerDevice();

        // WP-G2: pick up hot-edited chrome geometry (margin/bar/corner/enabled) by recreating
        // swapchains when they changed. Colors + fade/hide timings are read per-frame (no recreate).
        markSwapchainsDirtyIfChromeChanged();
    }

    // Re-run so that toggling openxr:inhibit_idle takes effect immediately (WP9 hook).
    if (g_pInputManager)
        g_pInputManager->recheckIdleInhibitorStatus();
}

// ---------------------------------------------------------------------------------------------
// WP4: reconciliation, verb funnel, selection, layer cap, status/layout serialization.
// ---------------------------------------------------------------------------------------------

namespace {
    // Note: yaw/pitch-from-quat serialization (doc 03 §7) now lives in the pure, unconditional
    // OpenXR::serializeXRMonitorLine (XRMonitorConfig.cpp) so layoutDump() and the gtest
    // round-trip test (tests/xr/parser.cpp) share one implementation.

    OpenXR::eXRAnchorMode parseAnchorModeSpec(const std::string& tok, OpenXR::eXRHand& hand, bool& ok) {
        ok = true;
        if (tok == "local")
            return OpenXR::XR_ANCHOR_LOCAL;
        if (tok == "head")
            return OpenXR::XR_ANCHOR_HEAD;
        if (tok == "body")
            return OpenXR::XR_ANCHOR_BODY;
        if (tok == "device:left") {
            hand = OpenXR::XR_HAND_LEFT;
            return OpenXR::XR_ANCHOR_DEVICE;
        }
        if (tok == "device:right") {
            hand = OpenXR::XR_HAND_RIGHT;
            return OpenXR::XR_ANCHOR_DEVICE;
        }
        ok = false;
        return OpenXR::XR_ANCHOR_LOCAL;
    }
}

OpenXR::SXRAnchorTuning COpenXRManager::readAnchorTuning() const {
    static auto             PRESP    = CConfigValue<Hyprlang::FLOAT>("openxr:leash_response");
    static auto             PANG     = CConfigValue<Hyprlang::FLOAT>("openxr:leash_deadzone_angle");
    static auto             PDIST    = CConfigValue<Hyprlang::FLOAT>("openxr:leash_deadzone_distance");
    static auto             PFOLLOW  = CConfigValue<Hyprlang::INT>("openxr:body_leash_follow_height");
    static auto             PDEFDIST = CConfigValue<Hyprlang::FLOAT>("openxr:default_distance");

    OpenXR::SXRAnchorTuning t;
    t.leashResponse    = (float)*PRESP;
    t.deadzoneAngleRad = (float)*PANG * (float)M_PI / 180.f;
    t.deadzoneDistance = (float)*PDIST;
    t.bodyFollowHeight = *PFOLLOW;
    t.defaultDistance  = (float)*PDEFDIST;
    return t;
}

OpenXR::SXRVerbContext COpenXRManager::currentVerbContext() {
    std::scoped_lock lock(m_layersMu);
    return m_lastVerbCtx;
}

PXRLAYER COpenXRManager::layerByName(const std::string& name) {
    std::scoped_lock lock(m_layersMu);
    for (auto& l : m_layers)
        if (l->m_monitorName == name && !l->m_pendingRemoval.load(std::memory_order_acquire))
            return l;
    return nullptr;
}

PXRLAYER COpenXRManager::resolveSelected() {
    // doc 05 §3.2 resolution order.
    if (!m_selectedMonitor.empty())
        if (auto l = layerByName(m_selectedMonitor))
            return l;
    // 2. Last ray-hovered (WP7 sets m_lastHoveredMonitor).
    if (!m_lastHoveredMonitor.empty())
        if (auto l = layerByName(m_lastHoveredMonitor))
            return l;
    // 3. Focused-if-XR.
    if (Desktop::focusState()) {
        if (auto mon = Desktop::focusState()->monitor())
            if (auto l = layerByName(mon->m_name))
                return l;
    }
    return nullptr;
}

void COpenXRManager::reconcileDeclaredMonitors() {
    // doc 05 §2.5: D = declared set (this parse), L = live layers created BY config
    // (m_declaredByConfig). Runtime-created layers are never touched.
    auto cm = Config::Legacy::mgr();
    if (!cm)
        return; // Lua config or no legacy manager: no declared xrmonitor set (v1 = classic only)

    const auto& declared = cm->declaredXRMonitors();

    // Snapshot current declared layer names.
    std::vector<std::string> liveDeclared;
    {
        std::scoped_lock lock(m_layersMu);
        for (auto& l : m_layers)
            if (l->m_declaredByConfig && !l->m_pendingRemoval.load(std::memory_order_acquire))
                liveDeclared.push_back(l->m_monitorName);
    }

    // D \ L -> create (declared);  D ∩ L -> diff (mode change / anchor change).
    for (const auto& d : declared) {
        auto existing = layerByName(d.m_name);
        if (existing && existing->m_declaredByConfig) {
            // D ∩ L: diff. Mode change -> apply new rule (emits modeChanged -> swapchain recreate).
            const bool modeChanged = existing->m_reqResolution != d.m_resolution || existing->m_reqRefresh != d.m_refreshRate;
            if (modeChanged && d.m_resolution && Config::monitorRuleMgr()) {
                if (auto mon = existing->m_monitor.lock()) {
                    Config::CMonitorRule rule = Config::monitorRuleMgr()->get(mon);
                    rule.m_resolution         = *d.m_resolution;
                    if (d.m_refreshRate)
                        rule.m_refreshRate = *d.m_refreshRate;
                    mon->applyMonitorRule(std::move(rule));
                }
                existing->m_reqResolution = d.m_resolution;
                existing->m_reqRefresh    = d.m_refreshRate;
            }
            // Anchor / size change -> re-anchor to the new declared state (doc 05 §2.5 / doc 03).
            static auto PSIZE             = CConfigValue<Hyprlang::FLOAT>("openxr:default_size");
            const float wantSize          = d.m_sizeMeters.value_or((float)*PSIZE);
            const bool  anchorChanged     = !(existing->m_declaredAnchor == d.m_anchor);
            const bool  sizeChanged       = existing->m_anchor.state().widthMeters != wantSize;
            const bool  anchorModeChanged = existing->m_declaredAnchor.mode != d.m_anchor.mode || existing->m_declaredAnchor.device != d.m_anchor.device;
            if (anchorChanged || sizeChanged) {
                std::scoped_lock       lock(m_layersMu);
                OpenXR::SXRAnchorState st = d.m_anchor;
                st.widthMeters            = wantSize;
                existing->m_anchor.initFromState(st);
                existing->m_declaredAnchor = st;
                existing->m_sizeMeters     = wantSize;
                if (anchorChanged && g_pEventManager && anchorModeChanged)
                    g_pEventManager->postEvent(SHyprIPCEvent{"xrmonitoranchor", d.m_name + "," + OpenXR::anchorModeToString(d.m_anchor)});
            }
            continue;
        }
        if (existing && !existing->m_declaredByConfig) {
            // Name collision with a runtime-created monitor: leave the runtime one alone.
            Log::logger->log(Log::WARN, "[OPENXR] xrmonitor '{}' declared in config but a runtime monitor already owns the name; ignoring the declaration", d.m_name);
            continue;
        }
        // D \ L: create.
        auto res = createXRMonitor(d);
        if (!res) {
            Log::logger->log(Log::WARN, "[OPENXR] failed to create declared xrmonitor '{}': {}", d.m_name, res.error());
            continue;
        }
        res.value()->m_declaredByConfig = true;
    }

    // L \ D: destroy declared layers no longer present in the declared set.
    for (const auto& name : liveDeclared) {
        const bool stillDeclared = std::ranges::any_of(declared, [&](const SXRMonitorParams& p) { return p.m_name == name; });
        if (!stillDeclared)
            destroyXRMonitor(name);
    }
}

void COpenXRManager::recomputeQuadActive() {
    // doc 02 recency policy: the newest maxLayerCount quads stay active; older ones suspend but
    // keep rendering as plain headless outputs. Compute under the lock; post events after.
    const uint32_t                            maxCount = m_session ? m_session->m_maxLayerCount : 16u;

    std::vector<std::pair<std::string, bool>> flips; // name, nowActive
    {
        std::scoped_lock                 lock(m_layersMu);

        std::vector<PXRLAYER> active;
        for (auto& l : m_layers)
            if (!l->m_pendingRemoval.load(std::memory_order_acquire))
                active.push_back(l);

        // Oldest first; suspend everything past the cap counting from the newest.
        std::sort(active.begin(), active.end(), [](const PXRLAYER& a, const PXRLAYER& b) { return a->m_seq < b->m_seq; });

        const size_t n = active.size();
        for (size_t i = 0; i < n; ++i) {
            const bool shouldBeActive = (n <= maxCount) || (i >= n - maxCount);
            if (active[i]->m_quadActive != shouldBeActive) {
                active[i]->m_quadActive = shouldBeActive;
                flips.emplace_back(active[i]->m_monitorName, shouldBeActive);
            }
        }
    }

    for (auto& [name, nowActive] : flips) {
        if (!nowActive)
            Log::logger->log(Log::WARN, "[OPENXR] XR monitor '{}' quad suspended: over the runtime layer cap ({})", name, maxCount);
        if (g_pEventManager)
            g_pEventManager->postEvent(SHyprIPCEvent{"xrmonitorquad", name + (nowActive ? ",1" : ",0")});
    }
}

std::expected<void, std::string> COpenXRManager::cmdCreate(const std::string& args) {
    auto parsed = OpenXR::parseXRMonitorCreateArgs(args);
    if (!parsed.has_value())
        return std::unexpected(parsed.error());

    // Defaults (doc 05 §3.1): mode 1920x1080@60; anchor:local placed along gaze at default
    // distance/size (WP4 keeps WP3's static pose — full placement solve is WP5).
    if (!parsed->m_resolution) {
        parsed->m_resolution = Vector2D{1920, 1080};
        if (!parsed->m_refreshRate)
            parsed->m_refreshRate = 60.f;
    }

    auto res = createXRMonitor(*parsed);
    if (!res)
        return std::unexpected(res.error());
    // Runtime-created: NOT declared, so reload reconciliation never touches it (doc 05 §2.5).
    return {};
}

std::expected<void, std::string> COpenXRManager::cmdDestroy(const std::string& target) {
    std::string name = target;
    if (target == "active" || target.empty()) {
        auto sel = resolveSelected();
        if (!sel)
            return std::unexpected<std::string>("no XR monitor selected");
        name = sel->m_monitorName;
    } else if (!layerByName(name))
        return std::unexpected<std::string>("no XR monitor named '" + name + "'");

    destroyXRMonitor(name);
    return {};
}

std::expected<void, std::string> COpenXRManager::cmdSelect(const std::string& arg) {
    if (arg.empty())
        return std::unexpected<std::string>("select: expected <name|next|prev>");

    if (arg == "next" || arg == "prev") {
        // Cycle in creation order (m_seq).
        std::vector<PXRLAYER> ordered;
        {
            std::scoped_lock lock(m_layersMu);
            for (auto& l : m_layers)
                if (!l->m_pendingRemoval.load(std::memory_order_acquire))
                    ordered.push_back(l);
        }
        if (ordered.empty())
            return std::unexpected<std::string>("no XR monitors exist");
        std::sort(ordered.begin(), ordered.end(), [](const PXRLAYER& a, const PXRLAYER& b) { return a->m_seq < b->m_seq; });

        size_t cur = 0;
        for (size_t i = 0; i < ordered.size(); ++i)
            if (ordered[i]->m_monitorName == m_selectedMonitor) {
                cur = i;
                break;
            }
        const size_t next = arg == "next" ? (cur + 1) % ordered.size() : (cur + ordered.size() - 1) % ordered.size();
        m_selectedMonitor = ordered[next]->m_monitorName;
        return {};
    }

    if (!layerByName(arg))
        return std::unexpected<std::string>("no XR monitor named '" + arg + "'");
    m_selectedMonitor = arg;
    return {};
}

std::vector<COpenXRManager::SXRMonitorInfo> COpenXRManager::monitorInfos() {
    std::vector<SXRMonitorInfo> out;
    std::scoped_lock            lock(m_layersMu);
    for (auto& l : m_layers) {
        if (l->m_pendingRemoval.load(std::memory_order_acquire))
            continue;
        const auto&    st = l->m_anchor.state();
        SXRMonitorInfo info;
        info.name       = l->m_monitorName;
        info.sizeMeters = st.widthMeters;
        info.anchorMode = OpenXR::anchorModeToString(st);
        // doc 05 §4.3: for local the pose is in LOCAL_FLOOR; for leashed/device modes it is the
        // configured offset (pos) + relative rotation (quat) in the leash/device frame. WP8
        // DEVIATION: while grabbed, `m_state.anchorPose` is a frozen snapshot of the
        // pre-grab pose (the grab override lives in a separate m_grabOffset, doc 03 §4.2, and
        // is only folded back into anchorPose on release) — report the live world-composed pose
        // instead so `hyprctl openxr status` tracks the controller during a grab, not a stale
        // value. Not explicitly specified by doc 05 (written before grab existed); this is the
        // more useful behavior for any status-polling consumer, noted for WP13 to fold into the
        // doc.
        const OpenXR::SXRPose reportPose = l->m_anchor.grabbed() && l->m_anchor.hasLastWorld() ? l->m_anchor.lastWorld() : st.anchorPose;
        info.posX                        = reportPose.pos.x;
        info.posY                        = reportPose.pos.y;
        info.posZ                        = reportPose.pos.z;
        info.quatX                       = reportPose.rot.x;
        info.quatY                       = reportPose.rot.y;
        info.quatZ                       = reportPose.rot.z;
        info.quatW                       = reportPose.rot.w;
        info.grabbed                     = l->m_anchor.grabbed(); // WP8 drives the grab machine
        switch (l->m_anchor.grabKind()) {                         // WP-G3: move vs resize
            case OpenXR::XR_GRABKIND_MOVE: info.grabKind = "move"; break;
            case OpenXR::XR_GRABKIND_RESIZE: info.grabKind = "resize"; break;
            default: info.grabKind = "none"; break;
        }
        info.hovered                     = l->m_hovered;          // WP7
        if (l->m_reqResolution) {
            info.w = (int)l->m_reqResolution->x;
            info.h = (int)l->m_reqResolution->y;
        }
        if (l->m_reqRefresh)
            info.refresh = *l->m_reqRefresh;
        if (auto mon = l->m_monitor.lock()) {
            info.id = mon->m_id;
            if (info.w == 0) {
                info.w = (int)mon->m_pixelSize.x;
                info.h = (int)mon->m_pixelSize.y;
            }
        }
        out.push_back(std::move(info));
    }
    return out;
}

std::string COpenXRManager::layoutDump() {
    // Paste-ready `xrmonitor = ...` lines (doc 05 §4.2 / doc 03 §7). Reflects the CURRENT live
    // layout, not just the parsed/declared spec:
    //  - anchor:local serializes the anchor's *live solved world pose* (CXRAnchor::lastWorld()),
    //    not the persistent m_state.anchorPose. The two are identical for LOCAL whenever the
    //    monitor isn't mid-grab (solve() sets m_lastWorld = anchorPose verbatim, doc 03 §3.1),
    //    but diverge while grabbed (the solve override composes grip * offset, doc 03 §4.2) —
    //    using lastWorld makes `hyprctl openxr layout` reproduce what the user actually sees
    //    right now even mid-grab, rather than the stale pre-grab pose. Falls back to the stored
    //    state if no solve has run yet (no session ever started for this layer).
    //  - head/body/device serialize the persistent offset (m_state.anchorPose) unchanged: that
    //    offset *is* the config representation for those modes (the live world position it
    //    produces continuously tracks the head/body/grip by design, doc 03 §3.2-3.4, so it is
    //    not itself a meaningful thing to freeze into a config line — only the offset is).
    //    applyMove/rotate/distance/center verbs already mutate this offset directly, and
    //    endGrab() re-derives it on grab release (doc 03 §4.4), so it stays live for those modes
    //    without any extra plumbing here.
    std::string              out;
    std::vector<std::string> lines;
    {
        std::scoped_lock lock(m_layersMu);
        for (auto& l : m_layers) {
            if (l->m_pendingRemoval.load(std::memory_order_acquire))
                continue;

            // Mode: prefer the requested mode; fall back to the monitor's current pixel size.
            int   w = 0, h = 0;
            float hz = 0.f;
            if (l->m_reqResolution) {
                w = (int)l->m_reqResolution->x;
                h = (int)l->m_reqResolution->y;
            } else if (auto mon = l->m_monitor.lock()) {
                w = (int)mon->m_pixelSize.x;
                h = (int)mon->m_pixelSize.y;
            }
            if (l->m_reqRefresh)
                hz = *l->m_reqRefresh;

            const auto& st = l->m_anchor.state();
            // Live substitution for LOCAL only (see comment above).
            const bool            useLive  = st.mode == OpenXR::XR_ANCHOR_LOCAL && l->m_anchor.hasLastWorld();
            const OpenXR::SXRPose livePose = useLive ? l->m_anchor.lastWorld() : st.anchorPose;

            lines.push_back(OpenXR::serializeXRMonitorLine(l->m_monitorName, Vector2D{(double)w, (double)h}, hz > 0.f ? std::optional<float>(hz) : std::nullopt, st, livePose,
                                                             st.widthMeters));
        }
    }
    for (auto& ln : lines)
        out += ln + "\n";
    return out;
}

// ---------------------------------------------------------------------------------------------
// WP5: pose-mutation verbs. One implementation, two transports (dispatcher + hyprctl). Each runs
// on the main thread, captures the frame-thread solve context, then mutates the layer's anchor
// under m_layersMu (doc 03 §5 / doc 05 §3.1).
// ---------------------------------------------------------------------------------------------

namespace {
    std::vector<std::string> splitWs(const std::string& s) {
        std::vector<std::string> out;
        size_t                   i = 0;
        while (i < s.size()) {
            while (i < s.size() && std::isspace((unsigned char)s[i]))
                ++i;
            size_t b = i;
            while (i < s.size() && !std::isspace((unsigned char)s[i]))
                ++i;
            if (i > b)
                out.push_back(s.substr(b, i - b));
        }
        return out;
    }

    std::optional<float> parseFloatArg(const std::string& s) {
        try {
            size_t idx = 0;
            float  v   = std::stof(s, &idx);
            if (idx != s.size())
                return std::nullopt;
            return v;
        } catch (...) { return std::nullopt; }
    }

    std::optional<OpenXR::Vec3> parseVec3Arg(const std::string& s) {
        std::vector<std::string> parts;
        size_t                   start = 0;
        for (size_t i = 0; i <= s.size(); ++i) {
            if (i == s.size() || s[i] == ',') {
                parts.push_back(s.substr(start, i - start));
                start = i + 1;
            }
        }
        if (parts.size() != 3)
            return std::nullopt;
        auto x = parseFloatArg(parts[0]);
        auto y = parseFloatArg(parts[1]);
        auto z = parseFloatArg(parts[2]);
        if (!x || !y || !z)
            return std::nullopt;
        return OpenXR::Vec3{*x, *y, *z};
    }
}

std::expected<void, std::string> COpenXRManager::cmdAnchor(const std::string& args) {
    const auto tokens = splitWs(args);
    if (tokens.size() < 2)
        return std::unexpected<std::string>("anchor: expected <name|active> <local|head|body|device:left|right> [offset:x,y,z]");

    const std::string   target = tokens[0];
    PXRLAYER layer  = target == "active" ? resolveSelected() : layerByName(target);
    if (!layer)
        return std::unexpected<std::string>(target == "active" ? "no XR monitor selected" : "no XR monitor named '" + target + "'");

    bool                  modeOk  = false;
    OpenXR::eXRHand       hand    = OpenXR::XR_HAND_LEFT;
    OpenXR::eXRAnchorMode newMode = parseAnchorModeSpec(tokens[1], hand, modeOk);
    if (!modeOk)
        return std::unexpected<std::string>("anchor: unknown mode '" + tokens[1] + "' (expected local|head|body|device:left|right)");

    bool         hasOffset = false;
    OpenXR::Vec3 offset;
    for (size_t i = 2; i < tokens.size(); ++i) {
        if (tokens[i].starts_with("offset:")) {
            auto v = parseVec3Arg(tokens[i].substr(7));
            if (!v)
                return std::unexpected<std::string>("anchor: bad offset '" + tokens[i] + "' (expected offset:x,y,z)");
            offset    = *v;
            hasOffset = true;
        } else
            return std::unexpected<std::string>("anchor: unexpected token '" + tokens[i] + "'");
    }

    const auto ctx  = currentVerbContext();
    const auto tune = readAnchorTuning();

    bool       modeChanged = false;
    {
        std::scoped_lock lock(m_layersMu);
        const auto&      before = layer->m_anchor.state();
        modeChanged             = before.mode != newMode || (newMode == OpenXR::XR_ANCHOR_DEVICE && before.device != hand);

        if (hasOffset) {
            OpenXR::SXRAnchorState st = layer->m_anchor.state(); // keep width
            st.mode                   = newMode;
            if (newMode == OpenXR::XR_ANCHOR_DEVICE)
                st.device = hand;
            st.anchorPose.pos = offset;
            st.anchorPose.rot = OpenXR::Quat{};
            layer->m_anchor.initFromState(st);
        } else if (!layer->m_anchor.setMode(newMode, hand, ctx, tune))
            return std::unexpected<std::string>("anchor: cannot re-anchor to that mode (head/body need head tracking, device needs a tracked controller)");
    }

    if (modeChanged && g_pEventManager)
        g_pEventManager->postEvent(SHyprIPCEvent{"xrmonitoranchor", layer->m_monitorName + "," + OpenXR::anchorModeToString(newMode, hand)});
    return {};
}

std::expected<void, std::string> COpenXRManager::cmdMove(const std::string& args) {
    const auto tokens = splitWs(args);
    if (tokens.size() != 3)
        return std::unexpected<std::string>("move: expected <dx> <dy> <dz> (meters)");
    auto dx = parseFloatArg(tokens[0]);
    auto dy = parseFloatArg(tokens[1]);
    auto dz = parseFloatArg(tokens[2]);
    if (!dx || !dy || !dz)
        return std::unexpected<std::string>("move: arguments must be numbers");

    auto layer = resolveSelected();
    if (!layer)
        return std::unexpected<std::string>("no XR monitor selected");

    const auto       ctx = currentVerbContext();
    std::scoped_lock lock(m_layersMu);
    if (!layer->m_anchor.applyMove(OpenXR::Vec3{*dx, *dy, *dz}, ctx))
        return std::unexpected<std::string>("move: head tracking (or controller for device anchors) unavailable");
    return {};
}

std::expected<void, std::string> COpenXRManager::cmdRotate(const std::string& args) {
    const auto tokens = splitWs(args);
    if (tokens.empty() || tokens.size() > 2)
        return std::unexpected<std::string>("rotate: expected <dyaw> [dpitch] (degrees)");
    auto yaw = parseFloatArg(tokens[0]);
    if (!yaw)
        return std::unexpected<std::string>("rotate: dyaw must be a number");
    float pitch = 0.f;
    if (tokens.size() == 2) {
        auto p = parseFloatArg(tokens[1]);
        if (!p)
            return std::unexpected<std::string>("rotate: dpitch must be a number");
        pitch = *p;
    }
    // Pitch clamped to ±85° (doc 05 §3.1).
    pitch = std::clamp(pitch, -85.f, 85.f);

    auto layer = resolveSelected();
    if (!layer)
        return std::unexpected<std::string>("no XR monitor selected");

    const auto       ctx     = currentVerbContext();
    constexpr float  DEG2RAD = (float)M_PI / 180.f;
    std::scoped_lock lock(m_layersMu);
    layer->m_anchor.applyRotate(*yaw * DEG2RAD, pitch * DEG2RAD, ctx);
    return {};
}

std::expected<void, std::string> COpenXRManager::cmdScale(const std::string& args) {
    const auto tokens = splitWs(args);
    if (tokens.size() != 1)
        return std::unexpected<std::string>("scale: expected <f|+d|-d>");
    const std::string& a = tokens[0];
    // Explicitly signed => additive delta in meters; bare number => multiplicative factor.
    const bool isDelta = a.front() == '+' || a.front() == '-';
    auto       f       = parseFloatArg(a);
    if (!f)
        return std::unexpected<std::string>("scale: argument must be a number");

    auto layer = resolveSelected();
    if (!layer)
        return std::unexpected<std::string>("no XR monitor selected");

    std::scoped_lock lock(m_layersMu);
    layer->m_anchor.applyScale(isDelta, *f);
    layer->m_sizeMeters = layer->m_anchor.state().widthMeters;
    return {};
}

std::expected<void, std::string> COpenXRManager::cmdDistance(const std::string& args) {
    const auto tokens = splitWs(args);
    if (tokens.size() != 1)
        return std::unexpected<std::string>("distance: expected <±m>");
    auto dm = parseFloatArg(tokens[0]);
    if (!dm)
        return std::unexpected<std::string>("distance: argument must be a number");

    auto layer = resolveSelected();
    if (!layer)
        return std::unexpected<std::string>("no XR monitor selected");

    const auto       ctx = currentVerbContext();
    std::scoped_lock lock(m_layersMu);
    if (!layer->m_anchor.applyDistance(*dm, ctx))
        return std::unexpected<std::string>("distance: head tracking unavailable or no current pose");
    return {};
}

std::expected<void, std::string> COpenXRManager::cmdCenter() {
    auto layer = resolveSelected();
    if (!layer)
        return std::unexpected<std::string>("no XR monitor selected");

    static auto      PDEFDIST = CConfigValue<Hyprlang::FLOAT>("openxr:default_distance");
    const auto       ctx      = currentVerbContext();
    std::scoped_lock lock(m_layersMu);
    if (!layer->m_anchor.applyCenter(ctx, (float)*PDEFDIST))
        return std::unexpected<std::string>("center: head tracking unavailable");
    return {};
}

#endif
