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
            // WP8 fills this: post the xrmonitorgrab socket2 event here.
            break;
        case eXRStateEventType::TRACKING: Log::logger->log(Log::DEBUG, "[OPENXR] device-lock tracking {} (monitor {})", e.a ? "gained" : "lost", e.str); break;
    }
}

void COpenXRManager::dispatchInputEvent(const SXRInputEvent& e) {
    // WP6 handoff point (WP7 replaces this with CXRPointerDevice signal emission). For now just
    // prove that action state crossed the queue onto the main thread.
    switch (e.type) {
        case eXRInputEventType::BUTTON:
            Log::logger->log(Log::DEBUG, "[OPENXR] input event crossed queue: BUTTON 0x{:x} {} (monitor {})", e.button, e.pressed ? "press" : "release", (long long)e.monitorID);
            break;
        case eXRInputEventType::MOTION_ABS:
        case eXRInputEventType::AXIS:
        case eXRInputEventType::FRAME:
            // Coalesced/positional events: no logging (would be noisy). Sink until WP7.
            break;
    }
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
        std::vector<SP<CXRMonitorLayer>> active;
        std::vector<SP<CXRMonitorLayer>> toRemove;
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
        // frame-side resources (once) and ack to main, which erases it + destroys the output.
        for (auto& l : toRemove) {
            if (l->m_removalAcked.exchange(true))
                continue;
            {
                CXRGraphics::CScopedGLContext ctx(*m_graphics);
                l->destroyFrameResourcesGL(*m_graphics);
            }
            l->destroySwapchain(); // context NOT current
            reportLayerRemoved(l->m_monitorName);
        }

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
        // cadence. This is the single sanctioned cross-thread monitor call, VISIBLE/FOCUSED
        // only — no scheduleFrame churn while idle.
        if (visible) {
            for (auto& l : active) {
                if (auto mon = l->m_monitor.lock())
                    mon->scheduleFrame();
            }
        }

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

            // First bind: create a swapchain sized to the monitor's pixel mode.
            if (l->m_swapchain == XR_NULL_HANDLE) {
                Vector2D size;
                if (auto mon = l->m_monitor.lock())
                    size = mon->m_pixelSize;
                if (size.x >= 1 && size.y >= 1)
                    createLayerSwapchain(*l, size);
            }

            if (l->m_swapchain == XR_NULL_HANDLE)
                continue; // no swapchain yet (monitor has no mode) — skip this layer

            auto buf = l->takeLatestBuffer();

            // Skip the blit when no new buffer arrived: a quad re-presents the most recently
            // released swapchain image every runtime frame (doc 01).
            if (!buf && l->m_hasContent)
                continue;

            XrSwapchainImageAcquireInfo acqInfo = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            uint32_t                    imgIdx  = 0;
            if (XR_FAILED(xrAcquireSwapchainImage(l->m_swapchain, &acqInfo, &imgIdx)))
                continue;
            XrSwapchainImageWaitInfo waitImg = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            waitImg.timeout                  = XR_INFINITE_DURATION;
            xrWaitSwapchainImage(l->m_swapchain, &waitImg);

            if (imgIdx < l->m_swapchainImages.size()) {
                CXRGraphics::CScopedGLContext ctx(*m_graphics); // bind ONLY around GL work
                if (buf) {
                    m_graphics->blitBuffer(buf, *l, l->m_swapchainImages[imgIdx]);
                    if (!l->m_hasContent)
                        Log::logger->log(Log::DEBUG, "[OPENXR] first blit landed for XR monitor '{}' ({}x{})", l->m_monitorName, (int)l->m_swapchainSize.x,
                                         (int)l->m_swapchainSize.y);
                    l->m_hasContent = true;
                } else
                    m_graphics->clearTex(l->m_swapchainImages[imgIdx], l->m_swapchainSize, 0.0f, 0.0f, 0.0f);
            }

            XrSwapchainImageReleaseInfo relInfo = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            xrReleaseSwapchainImage(l->m_swapchain, &relInfo);
        }

        // Build the quad layer array (sorted back->front by z-order then creation seq),
        // truncated to maxLayerCount. Storage must outlive xrEndFrame, so reserve up front.
        std::sort(active.begin(), active.end(), [](const SP<CXRMonitorLayer>& a, const SP<CXRMonitorLayer>& b) {
            if (a->m_zOrder != b->m_zOrder)
                return a->m_zOrder < b->m_zOrder;
            return a->m_seq < b->m_seq;
        });

        // --- anchor solve (WP5) ---
        // Locate the head (VIEW) pose in our reference space at the predicted display time. In the
        // LOCAL fallback (no LOCAL_FLOOR) shift +floor_offset so the solver works in floor-relative
        // coordinates; the final quad pose is shifted back before submission.
        static auto     PFLOOR      = CConfigValue<Hyprlang::FLOAT>("openxr:floor_offset");
        const float     floorOffset = (float)*PFLOOR;
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
                    in.view      = viewPose;
                    in.dt        = dt;
                    in.gripLeft  = gripLeft;
                    in.gripRight = gripRight;
                    in.pxW       = (uint32_t)std::max(1.0, l->m_swapchainSize.x);
                    in.pxH       = (uint32_t)std::max(1.0, l->m_swapchainSize.y);
                    results[i]   = l->m_anchor.solve(in, tune);
                    solved[i]    = true;
                } else if (l->m_anchor.hasLastWorld()) {
                    // No head pose this frame: hold the quad at its last composed world pose.
                    results[i].space        = OpenXR::XR_SPACE_LOCAL_FLOOR;
                    results[i].pose         = l->m_anchor.lastWorld();
                    results[i].worldPose    = results[i].pose;
                    results[i].widthMeters  = l->m_anchor.state().widthMeters;
                    results[i].heightMeters = results[i].widthMeters * (float)l->m_swapchainSize.y / (float)std::max(1.0, l->m_swapchainSize.x);
                    solved[i]               = true;
                }
            }
        }

        std::vector<XrCompositionLayerQuad>              quads;
        std::vector<const XrCompositionLayerBaseHeader*> layerPtrs;
        quads.reserve(active.size());
        layerPtrs.reserve(active.size());

        for (size_t i = 0; i < active.size(); ++i) {
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
            XrSpace gripSpace = XR_NULL_HANDLE;
            if (m_input && (res.space == OpenXR::XR_SPACE_GRIP_LEFT || res.space == OpenXR::XR_SPACE_GRIP_RIGHT))
                gripSpace = m_input->gripSpace(res.space == OpenXR::XR_SPACE_GRIP_LEFT ? OpenXR::XR_HAND_LEFT : OpenXR::XR_HAND_RIGHT);

            XrSpace         quadSpace = m_session->m_refSpace;
            OpenXR::SXRPose quadPose;
            if (gripSpace != XR_NULL_HANDLE) {
                quadSpace = gripSpace;
                quadPose  = res.pose; // grip-space offset; the grip ActionSpace is unshifted
            } else {
                quadPose = res.space == OpenXR::XR_SPACE_LOCAL_FLOOR ? res.pose : res.worldPose;
                if (!m_session->m_usingLocalFloor)
                    quadPose.pos.y -= floorOffset; // back to the LOCAL reference frame
            }

            XrCompositionLayerQuad quad   = {XR_TYPE_COMPOSITION_LAYER_QUAD};
            quad.layerFlags               = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            quad.space                    = quadSpace;
            quad.eyeVisibility            = XR_EYE_VISIBILITY_BOTH;
            quad.subImage.swapchain       = l->m_swapchain;
            quad.subImage.imageRect       = {{0, 0}, {w, h}};
            quad.subImage.imageArrayIndex = 0;
            quad.pose                     = xrFromPose(quadPose);
            quad.size                     = {res.widthMeters, res.heightMeters};
            quads.push_back(quad);
        }
        for (auto& q : quads)
            layerPtrs.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&q));

        // xrEndFrame with zero layers is valid (nothing composited yet) — doc 01.
        XrFrameEndInfo endInfo       = {XR_TYPE_FRAME_END_INFO};
        endInfo.displayTime          = fs.predictedDisplayTime;
        endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        endInfo.layerCount           = (uint32_t)layerPtrs.size();
        endInfo.layers               = layerPtrs.empty() ? nullptr : layerPtrs.data();
        xrEndFrame(m_session->m_session, &endInfo);
    }
}

bool COpenXRManager::createLayerSwapchain(CXRMonitorLayer& layer, const Vector2D& size) {
    // Frame thread. Destroy any existing swapchain first (context NOT current — interop rule).
    if (layer.m_swapchain != XR_NULL_HANDLE)
        layer.destroySwapchain();

    XrSwapchainCreateInfo info = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.usageFlags            = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    info.format                = m_session->m_swapchainFormat;
    info.sampleCount           = 1;
    info.width                 = (uint32_t)size.x;
    info.height                = (uint32_t)size.y;
    info.faceCount             = 1;
    info.arraySize             = 1;
    info.mipCount              = 1;

    // Do NOT bind the context ourselves — Monado's context_begin calls eglMakeCurrent
    // internally; a context already current when it does so crashes AMD gallium (doc 01).
    eglMakeCurrent(m_graphics->m_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

    XrResult r = xrCreateSwapchain(m_session->m_session, &info, &layer.m_swapchain);
    if (XR_FAILED(r)) {
        Log::logger->log(Log::ERR, "[OPENXR] xrCreateSwapchain for '{}' ({}x{}) failed: {}", layer.m_monitorName, (int)size.x, (int)size.y, (int)r);
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

    layer.m_swapchainSize = size;
    layer.m_hasContent    = false;

    // Reset the per-layer CPU staging tex so it reallocs to the new mode on the next blit.
    {
        CXRGraphics::CScopedGLContext ctx(*m_graphics);
        m_graphics->destroyLayerGL(layer.m_lastEGLImg, layer.m_cpuTex);
    }
    layer.m_lastEGLImg = nullptr;
    layer.m_cpuTex     = 0;
    layer.m_cpuTexSize = Vector2D{};

    Log::logger->log(Log::DEBUG, "[OPENXR] swapchain created for XR monitor '{}': {}x{}, {} images, format 0x{:x}", layer.m_monitorName, (int)size.x, (int)size.y, imgCount,
                     (unsigned long long)m_session->m_swapchainFormat);
    return true;
}

std::expected<SP<CXRMonitorLayer>, std::string> COpenXRManager::createXRMonitor(const SXRMonitorParams& params) {
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
    auto        layer      = makeShared<CXRMonitorLayer>(params.m_name, ++m_seqCounter, sizeMeters);

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
        SP<CXRMonitorLayer> l;
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
    SP<CXRMonitorLayer> layer;
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
    SP<CXRMonitorLayer> layer;
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
        mon->m_output->destroy(); // path B (external destroy) already gone -> mon expired, skipped

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
                SP<CXRMonitorLayer> layer;
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

    std::vector<SP<CXRMonitorLayer>> layers;
    {
        std::scoped_lock lock(m_layersMu);
        layers = m_layers;
    }

    for (auto& l : layers) {
        if (destroy && l->m_createdByXR) {
            l->stopMainListeners();
            l->m_destroyListener.reset(); // avoid re-entering the removal path from our own destroy
            if (auto mon = l->m_monitor.lock(); mon && mon->m_output)
                mon->m_output->destroy();
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
        std::erase_if(m_layers, [](const SP<CXRMonitorLayer>& l) { return l->m_createdByXR; });
    }
}

void COpenXRManager::reportLayerRemoved(const std::string& name) {
    // Frame thread: enqueue the ack and wake the main thread (removal barrier step 2->3).
    SXRStateEvent ev;
    ev.type = eXRStateEventType::LAYER_REMOVED;
    ev.str  = name;
    enqueue(ev);
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

    // Re-run so that toggling openxr:inhibit_idle takes effect immediately (WP9 hook).
    if (g_pInputManager)
        g_pInputManager->recheckIdleInhibitorStatus();
}

// ---------------------------------------------------------------------------------------------
// WP4: reconciliation, verb funnel, selection, layer cap, status/layout serialization.
// ---------------------------------------------------------------------------------------------

namespace {
    // Derive yaw/pitch degrees from a stored quat for serialization (doc 03 §7).
    void quatToYawPitchDeg(const OpenXR::Quat& q, float& yawDeg, float& pitchDeg) {
        const OpenXR::Vec3 f       = OpenXR::qRotate(q, OpenXR::Vec3{0.f, 0.f, -1.f});
        const float        pitch   = std::asin(std::clamp(f.y, -1.f, 1.f));
        const float        yaw     = (f.x * f.x + f.z * f.z < 1e-8f) ? 0.f : std::atan2(-f.x, -f.z);
        constexpr float    RAD2DEG = 180.f / (float)M_PI;
        yawDeg                     = yaw * RAD2DEG;
        pitchDeg                   = pitch * RAD2DEG;
    }

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

SP<CXRMonitorLayer> COpenXRManager::layerByName(const std::string& name) {
    std::scoped_lock lock(m_layersMu);
    for (auto& l : m_layers)
        if (l->m_monitorName == name && !l->m_pendingRemoval.load(std::memory_order_acquire))
            return l;
    return nullptr;
}

SP<CXRMonitorLayer> COpenXRManager::resolveSelected() {
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

        std::vector<SP<CXRMonitorLayer>> active;
        for (auto& l : m_layers)
            if (!l->m_pendingRemoval.load(std::memory_order_acquire))
                active.push_back(l);

        // Oldest first; suspend everything past the cap counting from the newest.
        std::sort(active.begin(), active.end(), [](const SP<CXRMonitorLayer>& a, const SP<CXRMonitorLayer>& b) { return a->m_seq < b->m_seq; });

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
        std::vector<SP<CXRMonitorLayer>> ordered;
        {
            std::scoped_lock lock(m_layersMu);
            for (auto& l : m_layers)
                if (!l->m_pendingRemoval.load(std::memory_order_acquire))
                    ordered.push_back(l);
        }
        if (ordered.empty())
            return std::unexpected<std::string>("no XR monitors exist");
        std::sort(ordered.begin(), ordered.end(), [](const SP<CXRMonitorLayer>& a, const SP<CXRMonitorLayer>& b) { return a->m_seq < b->m_seq; });

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
        // configured offset (pos) + relative rotation (quat) in the leash/device frame.
        info.posX    = st.anchorPose.pos.x;
        info.posY    = st.anchorPose.pos.y;
        info.posZ    = st.anchorPose.pos.z;
        info.quatX   = st.anchorPose.rot.x;
        info.quatY   = st.anchorPose.rot.y;
        info.quatZ   = st.anchorPose.rot.z;
        info.quatW   = st.anchorPose.rot.w;
        info.grabbed = l->m_anchor.grabbed(); // WP8 drives the grab machine
        info.hovered = l->m_hovered;          // WP7
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
    // Paste-ready `xrmonitor = ...` lines (doc 05 §4.2). WP4 serializes the stored (static)
    // anchor spec + mode + size, which round-trips through the WP4 parser. TODO(WP5): emit the
    // live solved pose from the anchoring engine rather than the parsed spec.
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

            std::string mode = std::format("{}x{}", w, h);
            if (hz > 0.f)
                mode += std::format("@{:.0f}", hz);

            const auto& st = l->m_anchor.state();
            const auto& p  = st.anchorPose.pos;
            std::string anchor;
            switch (st.mode) {
                case OpenXR::XR_ANCHOR_LOCAL: anchor = std::format("anchor:local pos:{:.3f},{:.3f},{:.3f}", p.x, p.y, p.z); break;
                case OpenXR::XR_ANCHOR_HEAD: anchor = std::format("anchor:head offset:{:.3f},{:.3f},{:.3f}", p.x, p.y, p.z); break;
                case OpenXR::XR_ANCHOR_BODY: anchor = std::format("anchor:body offset:{:.3f},{:.3f},{:.3f}", p.x, p.y, p.z); break;
                case OpenXR::XR_ANCHOR_DEVICE:
                    anchor = std::format("anchor:device:{} offset:{:.3f},{:.3f},{:.3f}", st.device == OpenXR::XR_HAND_RIGHT ? "right" : "left", p.x, p.y, p.z);
                    break;
            }
            // Rotation serialization (doc 03 §7): derive yaw/pitch from the stored quat. head
            // prints no rotation (lookAt-driven); body prints yaw only (pitch/roll forced to 0);
            // local/device print yaw and pitch (pitch omitted when |pitch| < 0.05°). Roll is not
            // representable and is intentionally dropped (documented v1 limitation).
            float yawDeg = 0.f, pitchDeg = 0.f;
            quatToYawPitchDeg(st.anchorPose.rot, yawDeg, pitchDeg);
            if (st.mode != OpenXR::XR_ANCHOR_HEAD)
                anchor += std::format(" yaw:{:.1f}", yawDeg);
            if ((st.mode == OpenXR::XR_ANCHOR_LOCAL || st.mode == OpenXR::XR_ANCHOR_DEVICE) && std::fabs(pitchDeg) >= 0.05f)
                anchor += std::format(" pitch:{:.1f}", pitchDeg);

            lines.push_back(std::format("xrmonitor = {}, {}, {}, size:{:.2f}", l->m_monitorName, mode, anchor, st.widthMeters));
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
    SP<CXRMonitorLayer> layer  = target == "active" ? resolveSelected() : layerByName(target);
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
