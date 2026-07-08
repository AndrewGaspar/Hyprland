#pragma once
#ifdef HAVE_OPENXR

// This header uses XrSwapchain (an OpenXR handle) but must be includable in TUs that do not
// pull in the GL/EGL platform headers — so it includes only the base openxr.h (no platform
// macros needed) and uses the WIP forward-declaration trick for the GL/EGL types (doc 01).
#include <openxr/openxr.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "XRGraphics.hpp"      // XR_GLuint / XR_EGLImageKHR aliases + CXRGraphics
#include "XRMonitorConfig.hpp" // SXRMonitorParams / OpenXR::SXRAnchorSpec (unguarded)
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
//
// THREAD-SAFETY RULE (load-bearing, learned from a live UAF crash): hyprutils
// CSharedPointer/CWeakPointer refcounts are plain unsigned ints — NOT atomic. Any inc/dec
// from the frame thread races the main thread's copies of the same object and silently
// corrupts the count (observed: CMonitor destroyed while still owned by the monitor state
// vectors). Therefore:
//   - The frame thread must never copy, destroy, or lock() a hyprutils SP/WP whose impl the
//     main thread also touches. Monitor facts it needs are cached below as plain values
//     (m_monitorId, m_pendingSize) written by the main thread.
//   - Buffer SPs are handed BACK to the main thread for their final release (retireBuffer/
//     releaseBuffers) — a move never touches the refcount.
//   - The layer itself crosses threads via std::shared_ptr (atomic control block), and the
//     manager guarantees the destructor runs on the main thread (the dtor releases hyprutils
//     WPs/listeners).
class CXRMonitorLayer {
  public:
    CXRMonitorLayer(const std::string& name, uint64_t seq, float sizeMeters);
    ~CXRMonitorLayer() = default;

    // ---- main thread ----
    // Connect the headless CMonitor: cache the weak ref + wire the presented/modeChanged/
    // destroy listeners. onGone is invoked (main thread) when the monitor is externally
    // destroyed so the manager can run the removal barrier.
    void bindToMonitor(PHLMONITOR mon, std::function<void()> onGone);
    // Stop queueing new buffers/mode changes (removal barrier step 1).
    void stopMainListeners();

    // ---- frame thread ----
    // Grab the latest presented buffer, if any (nulls m_haveNewFrame). Returns null when no
    // new frame is pending. The returned SP is MOVED out (no refcount op); the caller must
    // hand it back via retireBuffer() when done — never let it die on the frame thread.
    SP<Aquamarine::IBuffer> takeLatestBuffer();
    // Hand a consumed buffer back for main-thread release (moved into m_retiredBuffers).
    void retireBuffer(SP<Aquamarine::IBuffer>&& buf);
    // ---- main thread ----
    // Release the queued + retired buffer refs here, on the main thread (called by the
    // presented listener each frame and by the manager's removal/teardown paths).
    void releaseBuffers();
    // Delete per-layer GL objects (m_lastEGLImg, m_cpuTex). REQUIRES the EGL context current.
    void destroyFrameResourcesGL(CXRGraphics& gfx);
    // Destroy the XrSwapchain. REQUIRES the context NOT current (interop rule, doc 01).
    void destroySwapchain();

    // ---- main thread ----
    std::string         m_monitorName;         // key; survives monitor teardown
    PHLMONITORREF       m_monitor;             // weak ref to the headless output's CMonitor
    CHyprSignalListener m_presentedListener;   // mon->m_events.presented
    CHyprSignalListener m_modeChangedListener; // mon->m_events.modeChanged
    CHyprSignalListener m_destroyListener;     // mon->m_events.destroy (external destroy)
    bool                m_createdByXR = true;  // false for xrmonitor-adopted pre-existing outputs

    // WP4 lifecycle/state (main thread, under COpenXRManager::m_layersMu):
    // m_declaredByConfig == true iff this monitor came from an `xrmonitor` keyword. Only these
    // are touched by reload reconciliation (doc 05 §2.5); runtime-created ones are left alone.
    bool m_declaredByConfig = false;
    // Live anchoring engine (WP5). solve() runs on the frame thread; verbs mutate it on the main
    // thread under COpenXRManager::m_layersMu. m_declaredAnchor is the last state DECLARED by the
    // config keyword, kept separate from the live (spring-mutating) engine for reconcile diffs.
    OpenXR::CXRAnchor       m_anchor;
    OpenXR::SXRAnchorState  m_declaredAnchor;
    std::optional<Vector2D> m_reqResolution;   // last requested pixel mode (for reconcile diff)
    std::optional<float>    m_reqRefresh;      // last requested refresh (for reconcile diff)
    bool                    m_hovered = false; // last ray-hovered (WP7 sets this; status field)

    // ---- main -> frame handoff (doc 00 table) ----
    std::mutex              m_bufMu;
    SP<Aquamarine::IBuffer> m_latestBuffer;          // written under m_bufMu on presented
    std::vector<SP<Aquamarine::IBuffer>>
                            m_retiredBuffers;        // under m_bufMu; frame moves consumed buffers in, main releases (releaseBuffers)
    Vector2D                m_pendingSize;           // written under m_bufMu on bind + mode change (the monitor's pixel size)
    std::atomic<int64_t>    m_monitorId{-1};         // MONITORID, cached at bind — the frame thread must not lock() m_monitor
    std::atomic<bool>       m_haveNewFrame{false};   // release-store after buffer write
    std::atomic<bool>       m_swapchainDirty{false}; // set on mode change / (re)bind
    std::atomic<bool>       m_pendingRemoval{false}; // removal barrier flag
    std::atomic<bool>       m_removalAcked{false};   // frame thread acked removal once

    // ---- quad params: main writes under COpenXRManager::m_layersMu, frame copies per frame ----
    float    m_sizeMeters = 1.6f; // quad width (m); height = width * pxH/pxW
    int      m_zOrder     = 0;    // xrEndFrame submission order (back->front)
    uint64_t m_seq        = 0;    // creation sequence, monotonic (cap policy)

    // ---- frame thread only ----
    XrSwapchain           m_swapchain = XR_NULL_HANDLE;
    std::vector<uint32_t> m_swapchainImages;      // GLuints from XrSwapchainImageOpenGLESKHR
    Vector2D              m_swapchainSize;        // size the swapchain was created at (FULL: content + chrome margins)
    // Chrome margins (WP-G1): the swapchain is content + a transparent alpha margin; the desktop
    // blits into the inner content rect and the margin holds the move-bar / corner handles. These
    // are set alongside m_swapchainSize in createLayerSwapchain and read on the frame thread by the
    // blit (px insets) and the quad-submit/pointer path (m_chrome fractions). See XRMath.hpp §8.
    Vector2D                 m_contentSize;    // inner content rect size, px (the monitor's pixel mode)
    Vector2D                 m_contentOffsetPx;// top-left of the content rect within the swapchain, px
    OpenXR::SXRChromeGeometry m_chrome;        // normalized full-quad layout (single source of truth)
    XR_GLuint             m_cpuTex = 0;           // CPU-fallback staging tex, sized to mode
    Vector2D              m_cpuTexSize;           // size m_cpuTex was allocated at
    XR_EGLImageKHR        m_lastEGLImg = nullptr; // last dmabuf EGLImage (destroyed on next blit)
    bool                  m_hasContent = false;   // at least one successful blit since (re)create
    bool                  m_quadActive = true;    // false while suspended by the layer cap

    // ---- chrome content snapshot (WP-G2, frame thread only) ----
    // A persistent RGBA copy of the last fully-composited swapchain image (content @ alpha 1 +
    // transparent margin), kept so an ANIMATION-ONLY frame (chrome fading with NO new desktop
    // buffer) can re-blit the content into a freshly-acquired swapchain image before drawing the
    // chrome over it — every acquired image gets a complete, correct render. Deliberately a GL
    // texture we own (not the retired IBuffer, whose dmabuf the compositor may recycle, nor
    // m_lastEGLImg): zero hyprutils refcount traffic, self-contained teardown.
    XR_GLuint m_contentTex = 0;
    Vector2D  m_contentTexSize; // size m_contentTex was allocated at (== m_swapchainSize)

    // ---- chrome visual-state contract (WP-G2) — the interface future chrome consumers read ----
    // hoverRegion/grabbedNow are WRITTEN by the frame loop right AFTER CXRInput::processPointer
    // (from CXRInput's read-only chromeHoverRegion()/isMonitorGrabbed() accessors, queried by
    // monitor id) and READ by the chrome draw pass at the TOP of the next frame's blit loop — a
    // deliberate one-frame latency, all on the frame thread. Atomics per the §5.5 contract (never
    // a hyprutils refcount op). grabbedNow is true for EITHER grab kind (MOVE from bar/body AND
    // corner RESIZE, WP-G3): any active manipulation shows the grab color on all chrome elements.
    // While a hand grabs it casts no ray, so its hover contribution clears; grabbedNow alone holds
    // the chrome visible for the duration of the manipulation.
    std::atomic<uint8_t> m_hoverRegion{0 /* OpenXR::XR_REGION_NONE */}; // region the ray last classified on THIS quad (any hand)
    std::atomic<bool>    m_grabbedNow{false};                          // this quad has an active MOVE or RESIZE grab

    // Fade-envelope state (frame thread only; only the blit loop touches these). Alpha is advanced
    // every frame from predicted-display-time deltas via OpenXR::chromeFadeAdvance; the *Drawn*
    // trackers record what was last rendered so a static (no-new-buffer) frame can decide whether a
    // chrome-only redraw is actually needed.
    float    m_chromeAlpha       = 0.F; // current fade alpha [0,1]
    int64_t  m_chromeUpdateNs    = 0;   // predictedDisplayTime of the last fade advance
    int64_t  m_chromeActiveNs    = 0;   // predictedDisplayTime the quad was last hovered/grabbed
    float    m_chromeDrawnAlpha  = 0.F; // alpha last rendered into the swapchain (redraw diff)
    uint8_t  m_chromeDrawnRegion = 0;   // hover region last rendered (redraw diff)
    bool     m_chromeDrawnGrab   = false;
};

// Layers deliberately use std::shared_ptr instead of the codebase-standard hyprutils SP:
// copies genuinely cross the frame thread (the per-frame snapshot), and only shared_ptr's
// control block is atomic. See the thread-safety rule above.
using PXRLAYER = std::shared_ptr<CXRMonitorLayer>;

#endif // HAVE_OPENXR
