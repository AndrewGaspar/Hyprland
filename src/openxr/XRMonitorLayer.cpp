#include "XRMonitorLayer.hpp"
#ifdef HAVE_OPENXR

// This TU touches the swapchain teardown only (no XR platform/GL interop here — those live
// in the manager frame loop + CXRGraphics), so it needs only the base openxr.h that the
// header already pulled in, plus the monitor/buffer types.
#include <utility>

#include "../output/Monitor.hpp"
#include "../debug/log/Logger.hpp"
#include "XRDmabufImport.hpp" // OpenXR::shouldStashPresentedBuffer (cross-GPU stale-buffer guard)
#include "XRStereoPair.hpp"   // WP X1: the stereo quad-pair policy (pure)
#include "../config/ConfigValue.hpp"
#include "../desktop/view/Window.hpp"
#include "../managers/fullscreen/FullscreenController.hpp"
#include "../render/Renderer.hpp"     // WP X3: cursorDepthDamageSpread (main thread only)
#include "../pointer/PointerManager.hpp"
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
        // WP X1: resolve the stereo declaration for the frame we are about to hand over, BEFORE the
        // buffer checks below — a frame the cross-GPU guard drops must not leave the frame thread
        // splitting the previous (differently-declared) image.
        publishStereoPairLayout(pmon);
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

// MAIN THREAD ONLY (research/24 §5.1, WP X1). Resolve whether this monitor's next frame should be
// submitted as a pair of eye-cropped quads, and publish the answer as one byte for the frame thread.
//
// Everything expensive or thread-hostile happens here: the fullscreen-controller lookup, the
// window's rule/tag fold, the string-backed config. The frame thread gets an integer.
//
// The COVERAGE test is what makes the pair safe, and it is asked of the window's live geometry
// rather than of the declaration: CWindow::stereoLayout() already applied §4.3's fullscreen gate,
// but `always` and a client's own tag both legitimately SKIP that gate, and a floating declared
// window must not split the desktop. This is the same size/position comparison the solitary check
// uses (Monitor.cpp's SC_TRANSFORM) — deliberately not m_solitaryClient itself, which additionally
// drops out for notifications, drag-and-drop, fadeouts and overlay layers. Those are transient, and
// since engaging the pair can change the quad's ASPECT (§5.2), letting a toast flatten the panel
// would make it visibly change shape and back.
// WP X3 adds the SECOND producer above this, and it takes precedence, so read the two branches in
// order. A monitor that is PACKED (the depth desktop: CMonitor composited it once per eye into a
// double-wide buffer) has already dealt with every stereo window on it — Phase S's per-surface UV
// crop ran inside each composite, so a fullscreen 3D film and a windowed one both come out correct,
// in panes. Splitting the packed buffer a second time as if it were client content would halve the
// desktop. So when the pack is on, the DEPTH producer owns the pair and the CONTENT query is not
// even asked; when it is off, X1's path below is untouched, byte for byte.
void CXRMonitorLayer::publishStereoPairLayout(const PHLMONITOR& mon) {
    static auto              PPAIR  = CConfigValue<Hyprlang::INT>("openxr:stereo_quad_pair");
    const bool               ENABLED = *PPAIR != 0;

    OpenXR::Stereo::SPairDecl decl;
    decl.modeW = (uint32_t)std::max(0.0, mon->m_pixelSize.x);
    decl.modeH = (uint32_t)std::max(0.0, mon->m_pixelSize.y);

    if (mon->isStereo() && mon->stereoPaneCount() >= 2) {
        // THE DEPTH DESKTOP (research/24 §6, WP X3). The layout is a property of the PACK, not of the
        // kill switch: the buffer is laid out side-by-side whether or not we submit two quads, and the
        // quad's aspect is derived from one pane either way. `submit` carries the switch, so turning
        // it off degrades to ONE quad showing pane 0 — a mono desktop at the right shape — instead of
        // one quad showing a doubled side-by-side image.
        decl.producer = OpenXR::Stereo::PRODUCER_DEPTH;
        decl.layout   = Render::Stereo::CONTENT_SBS;
        decl.submit   = ENABLED;
    } else {
        OpenXR::Stereo::SPairQuery query;
        query.enabled = ENABLED;

        if (const auto FSWINDOW = Fullscreen::controller()->getFullscreenWindow(mon)) {
            query.declared     = FSWINDOW->stereoLayout();
            query.coversOutput = FSWINDOW->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT) == mon->m_size &&
                FSWINDOW->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT) == mon->m_position;
        }

        decl.layout   = OpenXR::Stereo::resolvePairLayout(query);
        decl.producer = OpenXR::Stereo::pairActive(decl.layout) ? OpenXR::Stereo::PRODUCER_CONTENT : OpenXR::Stereo::PRODUCER_NONE;
        decl.submit   = decl.producer == OpenXR::Stereo::PRODUCER_CONTENT;
        // X1 never carried a mode and its swapchain never changes size when the pair engages, so the
        // describes() guard has nothing to protect there. Leave it unarmed rather than inventing a
        // constraint that could only ever mis-fire.
        decl.modeW = decl.modeH = 0;
    }

    m_stereoPairDecl.store(OpenXR::Stereo::packDecl(decl), std::memory_order_release);
    publishCursorDisparity(mon, decl.producer == OpenXR::Stereo::PRODUCER_DEPTH);
}

// MAIN THREAD (research/24 §5.4, WP X3). The XR ray cursor must sit at the depth of whatever it is
// over, or it reads as pointing THROUGH a raised window — stereo cinema's "subtitle behind the
// object". The number itself is main-thread-only work (the hovered view, the depth ladder, the
// comfort ceiling), so it is resolved here and published as a fraction of ONE PANE's width; the
// frame thread eases toward it and turns it into pixels.
void CXRMonitorLayer::publishCursorDisparity(const PHLMONITOR& mon, bool depthPaired) {
    float target = 0.F;

    // Only while this monitor is actually showing two different panes, and only while the pointer is
    // on it: the hovered view is a global, and lending monitor B's hover depth to monitor A's ray
    // cursor would float it over nothing. In practice they agree — pointing a ray at a monitor is
    // what moves the pointer there — so this is a guard, not a policy.
    if (depthPaired && g_pHyprRenderer && mon->m_size.x > 0 && mon->logicalBox().containsPoint(Pointer::mgr()->position())) {
        const double SHIFTLOGICAL = g_pHyprRenderer->cursorDepthDamageSpread(mon);
        target                    = (float)(SHIFTLOGICAL / mon->m_size.x);
    }

    m_cursorDisparityTarget.store(target, std::memory_order_release);
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

void CXRMonitorLayer::destroySwapchain(bool skipXrCall) {
    if (m_swapchain != XR_NULL_HANDLE) {
        if (!skipXrCall)
            xrDestroySwapchain(m_swapchain);
        m_swapchain = XR_NULL_HANDLE;
    }
    m_swapchainImages.clear();
    m_hasContent = false;
    m_contentPath.store(0 /* OpenXR::XR_CONTENT_NONE */, std::memory_order_relaxed);
}

#endif // HAVE_OPENXR
