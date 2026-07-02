#pragma once
#ifdef HAVE_OPENXR

// This header uses XrSwapchain (an OpenXR handle) but must be includable in TUs that do not
// pull in the GL/EGL platform headers — so it includes only the base openxr.h (no platform
// macros needed) and uses the WIP forward-declaration trick for the GL/EGL types (doc 01).
#include <openxr/openxr.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "XRGraphics.hpp" // XR_GLuint / XR_EGLImageKHR aliases + CXRGraphics
#include "../helpers/memory/Memory.hpp"
#include "../helpers/signal/Signal.hpp"
#include "../helpers/math/Math.hpp"
#include "../desktop/DesktopTypes.hpp" // PHLMONITOR / PHLMONITORREF

namespace Aquamarine {
    class IBuffer;
}

// One CXRMonitorLayer per XR monitor, owned by COpenXRManager::m_layers (guarded by
// m_layersMu). It ferries a headless output's presented buffers into an XrSwapchain and is
// submitted as one XrCompositionLayerQuad per frame. Thread ownership is annotated per field
// and is load-bearing (doc 02 / doc 00 handoff table).
class CXRMonitorLayer {
  public:
    CXRMonitorLayer(const std::string& name, uint64_t seq, float sizeMeters);
    ~CXRMonitorLayer() = default;

    // ---- main thread ----
    // Connect the headless CMonitor: cache the weak ref + wire the presented/modeChanged/
    // destroy listeners. onGone is invoked (main thread) when the monitor is externally
    // destroyed so the manager can run the removal barrier.
    void                    bindToMonitor(PHLMONITOR mon, std::function<void()> onGone);
    // Stop queueing new buffers/mode changes (removal barrier step 1).
    void                    stopMainListeners();

    // ---- frame thread ----
    // Grab the latest presented buffer, if any (nulls m_haveNewFrame). Returns null when no
    // new frame is pending.
    SP<Aquamarine::IBuffer> takeLatestBuffer();
    // Delete per-layer GL objects (m_lastEGLImg, m_cpuTex). REQUIRES the EGL context current.
    void                    destroyFrameResourcesGL(CXRGraphics& gfx);
    // Destroy the XrSwapchain. REQUIRES the context NOT current (interop rule, doc 01).
    void                    destroySwapchain();

    // ---- main thread ----
    std::string         m_monitorName;         // key; survives monitor teardown
    PHLMONITORREF       m_monitor;             // weak ref to the headless output's CMonitor
    CHyprSignalListener m_presentedListener;   // mon->m_events.presented
    CHyprSignalListener m_modeChangedListener; // mon->m_events.modeChanged
    CHyprSignalListener m_destroyListener;     // mon->m_events.destroy (external destroy)
    bool                m_createdByXR = true;  // false for xrmonitor-adopted pre-existing outputs

    // ---- main -> frame handoff (doc 00 table) ----
    std::mutex              m_bufMu;
    SP<Aquamarine::IBuffer> m_latestBuffer;             // written under m_bufMu on presented
    Vector2D                m_pendingSize;              // written under m_bufMu on mode change
    std::atomic<bool>       m_haveNewFrame{false};      // release-store after buffer write
    std::atomic<bool>       m_swapchainDirty{false};    // set on mode change / (re)bind
    std::atomic<bool>       m_pendingRemoval{false};    // removal barrier flag
    std::atomic<bool>       m_removalAcked{false};      // frame thread acked removal once

    // ---- quad params: main writes under COpenXRManager::m_layersMu, frame copies per frame ----
    float    m_sizeMeters = 1.6f; // quad width (m); height = width * pxH/pxW
    int      m_zOrder     = 0;    // xrEndFrame submission order (back->front)
    uint64_t m_seq        = 0;    // creation sequence, monotonic (cap policy)

    // ---- frame thread only ----
    XrSwapchain           m_swapchain = XR_NULL_HANDLE;
    std::vector<uint32_t> m_swapchainImages; // GLuints from XrSwapchainImageOpenGLESKHR
    Vector2D              m_swapchainSize;    // size the swapchain was created at
    XR_GLuint             m_cpuTex     = 0;   // CPU-fallback staging tex, sized to mode
    Vector2D              m_cpuTexSize;       // size m_cpuTex was allocated at
    XR_EGLImageKHR        m_lastEGLImg = nullptr; // last dmabuf EGLImage (destroyed on next blit)
    bool                  m_hasContent = false;   // at least one successful blit since (re)create
    bool                  m_quadActive = true;    // false while suspended by the layer cap
};

#endif // HAVE_OPENXR
