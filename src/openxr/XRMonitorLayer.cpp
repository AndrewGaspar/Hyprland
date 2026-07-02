#include "XRMonitorLayer.hpp"
#ifdef HAVE_OPENXR

// This TU touches the swapchain teardown only (no XR platform/GL interop here — those live
// in the manager frame loop + CXRGraphics), so it needs only the base openxr.h that the
// header already pulled in, plus the monitor/buffer types.
#include <utility>

#include "../output/Monitor.hpp"
#include "../debug/log/Logger.hpp"
#include <aquamarine/buffer/Buffer.hpp>

CXRMonitorLayer::CXRMonitorLayer(const std::string& name, uint64_t seq, float sizeMeters) : m_monitorName(name), m_sizeMeters(sizeMeters), m_seq(seq) {
    ;
}

void CXRMonitorLayer::bindToMonitor(PHLMONITOR mon, std::function<void()> onGone) {
    m_monitor = mon;

    // presented: fires on the main thread after each committed frame — stash the buffer for
    // the frame thread (the WIP's proven handoff pattern, moved per-layer).
    m_presentedListener = mon->m_events.presented.listen([this]() {
        const auto pmon = m_monitor.lock();
        if (!pmon || !pmon->m_output || !pmon->m_output->state)
            return;
        auto buf = pmon->m_output->state->state().buffer;
        if (!buf)
            return;
        std::lock_guard<std::mutex> lk(m_bufMu);
        m_latestBuffer = buf; // SP keeps the buffer alive across threads
        m_haveNewFrame.store(true, std::memory_order_release);
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

void CXRMonitorLayer::destroyFrameResourcesGL(CXRGraphics& gfx) {
    // Context must be current (caller's responsibility). Uses CXRGraphics for the EGLImage
    // destructor proc + the GL delete.
    gfx.destroyLayerGL(m_lastEGLImg, m_cpuTex);
    m_lastEGLImg = nullptr;
    m_cpuTex     = 0;
    m_cpuTexSize = Vector2D{};
}

void CXRMonitorLayer::destroySwapchain() {
    if (m_swapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(m_swapchain);
        m_swapchain = XR_NULL_HANDLE;
    }
    m_swapchainImages.clear();
    m_hasContent = false;
}

#endif // HAVE_OPENXR
