#include "XRMonitorLayer.hpp"
#ifdef HAVE_OPENXR

// This TU touches the swapchain teardown only (no XR platform/GL interop here — those live
// in the manager frame loop + CXRGraphics), so it needs only the base openxr.h that the
// header already pulled in, plus the monitor/buffer types.
#include <utility>

#include "../output/Monitor.hpp"
#include "../debug/log/Logger.hpp"
#include "XRDmabufImport.hpp" // OpenXR::shouldStashPresentedBuffer (cross-GPU stale-buffer guard)
#include <aquamarine/buffer/Buffer.hpp>

CXRMonitorLayer::CXRMonitorLayer(const std::string& name, uint64_t seq, float sizeMeters) : m_monitorName(name), m_sizeMeters(sizeMeters), m_seq(seq) {
    ;
}

void CXRMonitorLayer::bindToMonitor(PHLMONITOR mon, std::function<void()> onGone) {
    m_monitor = mon;

    // Cache the facts the frame thread needs as plain values — it must never lock() m_monitor
    // (non-atomic hyprutils refcounts; see the thread-safety rule in the header).
    m_monitorId.store(mon->m_id, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(m_bufMu);
        m_pendingSize = mon->m_pixelSize;
    }
    m_swapchainDirty.store(true, std::memory_order_release);

    // presented: fires on the main thread after each committed frame — stash the buffer for
    // the frame thread (the WIP's proven handoff pattern, moved per-layer).
    m_presentedListener = mon->m_events.presented.listen([this]() {
        const auto pmon = m_monitor.lock();
        if (!pmon || !pmon->m_output || !pmon->m_output->state)
            return;
        auto buf = pmon->m_output->state->state().buffer;
        if (!buf)
            return;
        // Cross-GPU stale-buffer guard: on a force-linear output, never stash a still-foreign-tiled
        // buffer from a composite that raced ahead of the swapchain's LINEAR reconfigure — the frame
        // thread's dmabuf import would be rejected by the foreign XR GPU and the quad would go black
        // (live 2026-07-12 "monitor created after a destroy goes blank"). Drop it; the reconfigured
        // LINEAR composite that follows feeds the layer. Reading dmabuf() here is main-thread only.
        if (pmon->m_forceLinearSwapchain) {
            const auto dmab = buf->dmabuf();
            if (!OpenXR::shouldStashPresentedBuffer(true, dmab.success, dmab.modifier))
                return;
        }
        std::vector<SP<Aquamarine::IBuffer>> retired;
        {
            std::lock_guard<std::mutex> lk(m_bufMu);
            retired.swap(m_retiredBuffers); // release frame-consumed buffers here, on main
            m_latestBuffer = buf;           // SP keeps the buffer alive across threads
            m_haveNewFrame.store(true, std::memory_order_release);
        }
    });

    // modeChanged: record the new pixel size for the frame thread to recreate the swapchain.
    m_modeChangedListener = mon->m_events.modeChanged.listen([this]() {
        const auto pmon = m_monitor.lock();
        if (!pmon)
            return;
        const Vector2D newSize = pmon->m_pixelSize;
        {
            std::lock_guard<std::mutex> lk(m_bufMu);
            if (m_pendingSize == newSize)
                return;
            m_pendingSize = newSize;
        }
        m_swapchainDirty.store(true, std::memory_order_release);
    });

    // destroy: external monitor teardown (hyprctl output destroy, backend teardown).
    m_destroyListener = mon->m_events.destroy.listen([onGone = std::move(onGone)]() {
        if (onGone)
            onGone();
    });
}

void CXRMonitorLayer::stopMainListeners() {
    // Removal barrier step 1: no new buffers / mode changes may be queued. Keep the destroy
    // listener — the monitor may still be alive (XR-initiated destroy destroys it later).
    m_presentedListener.reset();
    m_modeChangedListener.reset();
}

SP<Aquamarine::IBuffer> CXRMonitorLayer::takeLatestBuffer() {
    if (!m_haveNewFrame.load(std::memory_order_acquire))
        return nullptr;
    std::lock_guard<std::mutex> lk(m_bufMu);
    auto buf = std::move(m_latestBuffer);
    m_latestBuffer.reset();
    m_haveNewFrame.store(false, std::memory_order_relaxed);
    return buf;
}

void CXRMonitorLayer::retireBuffer(SP<Aquamarine::IBuffer>&& buf) {
    // Frame thread. The final release of a buffer SP must happen on the main thread — the main
    // thread holds refs to the same buffer (output state, renderer) and hyprutils refcounts are
    // not atomic. A move never touches the count.
    if (!buf)
        return;
    std::lock_guard<std::mutex> lk(m_bufMu);
    m_retiredBuffers.emplace_back(std::move(buf));
}

void CXRMonitorLayer::releaseBuffers() {
    // Main thread. Pull the refs out under the lock, release them outside it.
    SP<Aquamarine::IBuffer>              latest;
    std::vector<SP<Aquamarine::IBuffer>> retired;
    {
        std::lock_guard<std::mutex> lk(m_bufMu);
        latest = std::move(m_latestBuffer);
        m_latestBuffer.reset();
        retired.swap(m_retiredBuffers);
        m_haveNewFrame.store(false, std::memory_order_relaxed);
    }
}

void CXRMonitorLayer::destroyFrameResourcesGL(CXRGraphics& gfx) {
    // Context must be current (caller's responsibility). Uses CXRGraphics for the EGLImage
    // destructor proc + the GL delete.
    gfx.destroyLayerGL(m_lastEGLImg, m_cpuTex, m_contentTex);
    m_lastEGLImg    = nullptr;
    m_cpuTex        = 0;
    m_cpuTexSize    = Vector2D{};
    m_contentTex    = 0;
    m_contentTexSize = Vector2D{};
}

void CXRMonitorLayer::destroySwapchain() {
    if (m_swapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(m_swapchain);
        m_swapchain = XR_NULL_HANDLE;
    }
    m_swapchainImages.clear();
    m_hasContent = false;
    m_contentPath.store(0 /* OpenXR::XR_CONTENT_NONE */, std::memory_order_relaxed);
}

#endif // HAVE_OPENXR
