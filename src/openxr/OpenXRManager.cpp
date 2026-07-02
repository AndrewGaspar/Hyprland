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
#include <cstring>
#include <cstdint>
#include <chrono>

#include <sys/eventfd.h>
#include <unistd.h>
#include <wayland-server-core.h>

#include "XRIpc.hpp"
#include "XRSession.hpp"
#include "XRGraphics.hpp"
#include "XRMonitorLayer.hpp"
#include "../Compositor.hpp"
#include "../debug/log/Logger.hpp"
#include "../config/ConfigValue.hpp"
#include "../event/EventBus.hpp"
#include "../managers/EventManager.hpp"
#include "../managers/eventLoop/EventLoopManager.hpp"
#include "../managers/input/InputManager.hpp"
#include "../output/Monitor.hpp"
#include "../state/MonitorState.hpp"
#include "../config/shared/monitor/MonitorRuleManager.hpp"
#include "../config/shared/monitor/MonitorRule.hpp"

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

    // 8. Bind any layers whose monitor still exists (created while disabled — doc 02), then
    //    set up the frame->main channel and spawn the frame thread.
    bindExistingLayers();

    if (!setupFrameChannel()) {
        abortStart();
        return;
    }

    m_running = true;
    m_frameThread = std::thread([this] { frameThread(); });

    setState(XR_STATE_RUNNING_IDLE);
    Log::logger->log(Log::DEBUG, "[OPENXR] session up (runtime: {}, system: {}), frame thread started", m_runtimeName, m_systemName);

    // WP3: create one hard-coded test monitor so a live quad is submitted end-to-end. The
    // full create funnel (config keyword / dispatcher / hyprctl) is WP4; this exercises the
    // same createXRMonitor path with fixed params.
    if (m_layers.empty()) {
        SXRMonitorParams params;
        params.m_name       = "XR-1";
        params.m_resolution = Vector2D{1920, 1080};
        auto res            = createXRMonitor(params);
        if (!res)
            Log::logger->log(Log::WARN, "[OPENXR] WP3 test monitor create failed: {}", res.error());
    }
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

    // 3. Per-layer swapchains (context NOT current), then XR handles.
    for (auto& l : m_layers)
        l->destroySwapchain();
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
    m_removedLayerNames.clear();
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
    std::vector<std::string>     removed;
    {
        std::scoped_lock lock(m_pendingMu);
        pending.swap(m_pendingStates);
        removed.swap(m_removedLayerNames);
    }
    for (auto s : pending)
        setState(s);

    // Removal barrier step 3: the frame thread has released a layer's resources and acked.
    for (auto& name : removed)
        finalizeLayerRemoval(name);

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
                        Log::logger->log(Log::DEBUG, "[OPENXR] first blit landed for XR monitor '{}' ({}x{})", l->m_monitorName, (int)l->m_swapchainSize.x, (int)l->m_swapchainSize.y);
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

        // Static pose for WP3 (anchoring is WP5): centered ~1.5m in front, ~1.4m high. In the
        // LOCAL fallback (no LOCAL_FLOOR) subtract the assumed eye height so it stays roughly
        // eye-level relative to the head origin.
        static auto  PDIST  = CConfigValue<Hyprlang::FLOAT>("openxr:default_distance");
        static auto  PFLOOR = CConfigValue<Hyprlang::FLOAT>("openxr:floor_offset");
        const float  dist   = (float)*PDIST;
        const float  yFloor = 1.4f;
        const float  y      = m_session->m_usingLocalFloor ? yFloor : (yFloor - (float)*PFLOOR);

        std::vector<XrCompositionLayerQuad>              quads;
        std::vector<const XrCompositionLayerBaseHeader*> layerPtrs;
        quads.reserve(active.size());
        layerPtrs.reserve(active.size());

        for (auto& l : active) {
            if (quads.size() >= m_session->m_maxLayerCount)
                break;
            if (!l->m_quadActive || !l->m_hasContent)
                continue;

            const int w = (int)l->m_swapchainSize.x;
            const int h = (int)l->m_swapchainSize.y;
            if (w <= 0 || h <= 0)
                continue;

            XrCompositionLayerQuad quad = {XR_TYPE_COMPOSITION_LAYER_QUAD};
            quad.layerFlags             = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            quad.space                  = m_session->m_refSpace;
            quad.eyeVisibility          = XR_EYE_VISIBILITY_BOTH;
            quad.subImage.swapchain     = l->m_swapchain;
            quad.subImage.imageRect     = {{0, 0}, {w, h}};
            quad.subImage.imageArrayIndex = 0;
            quad.pose.orientation       = {0, 0, 0, 1};
            quad.pose.position          = {0.0f, y, -dist};
            quad.size                   = {l->m_sizeMeters, l->m_sizeMeters * (float)h / (float)w};
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
    static auto PSIZE       = CConfigValue<Hyprlang::FLOAT>("openxr:default_size");
    const float sizeMeters  = params.m_sizeMeters.value_or((float)*PSIZE);
    auto        layer       = makeShared<CXRMonitorLayer>(params.m_name, ++m_seqCounter, sizeMeters);
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

    // 5. Apply the requested pixel mode, if any (DEVIATION: the "explicit user monitor= rule
    //    wins" subtlety of doc 02 step 5 is deferred to WP4 with the full keyword grammar).
    if (params.m_resolution && Config::monitorRuleMgr()) {
        Config::CMonitorRule rule = Config::monitorRuleMgr()->get(mon);
        rule.m_resolution         = *params.m_resolution;
        if (params.m_refreshRate)
            rule.m_refreshRate = *params.m_refreshRate;
        mon->applyMonitorRule(std::move(rule));
    }

    // 6. Notify (doc 05 event surface; full payload lands with WP4).
    if (g_pEventManager)
        g_pEventManager->postEvent(SHyprIPCEvent{"xrmonitoradded", params.m_name});

    // 7. If a session is running, the frame thread creates the swapchain on its next pass (it
    //    already snapshots m_layers per frame, so no extra wakeup is needed).
    if (m_running.load())
        layer->m_swapchainDirty.store(true, std::memory_order_release);

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
    static auto PDESTROY = CConfigValue<Hyprlang::INT>("openxr:destroy_monitors_on_stop");
    const bool  destroy  = *PDESTROY;

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
            l->m_swapchain    = XR_NULL_HANDLE;
            l->m_swapchainImages.clear();
            l->m_hasContent   = false;
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
    {
        std::scoped_lock lock(m_pendingMu);
        m_removedLayerNames.push_back(name);
    }
    if (m_eventFd >= 0) {
        const uint64_t one = 1;
        [[maybe_unused]] auto _ = write(m_eventFd, &one, sizeof(one));
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
