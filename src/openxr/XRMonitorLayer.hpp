#pragma once
#ifdef HAVE_OPENXR

// This header uses XrSwapchain (an OpenXR handle) but must be includable in TUs that do not
// pull in the GL/EGL platform headers — so it includes only the base openxr.h (no platform
// macros needed) and uses the WIP forward-declaration trick for the GL/EGL types (doc 01).
#include <openxr/openxr.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "XRGraphics.hpp"      // XR_GLuint / XR_EGLImageKHR aliases + CXRGraphics
#include "XRStereoPair.hpp"    // OpenXR::Stereo::SPaneGeom / SPairDecl (pure, unguarded)
#include "XRMonitorConfig.hpp" // SXRMonitorParams / OpenXR::SXRAnchorSpec (unguarded)
#include "XRRule.hpp"          // SXREffects / SXRResolvedEffects / SXRFxEnv (unguarded)
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

    // MAIN THREAD ONLY. Resolve + publish m_stereoPairDecl for this monitor (WP X1/X3). Called from
    // the `presented` listener, and directly on a config change so the kill switch applies at once.
    void publishStereoPairLayout(const PHLMONITOR& mon);
    void publishStereoMode(const PHLMONITOR& mon);
    // MAIN THREAD ONLY (WP X3, §5.4). The XR ray cursor's depth, as a fraction of one pane's width.
    void publishCursorDisparity(const PHLMONITOR& mon, bool depthPaired);

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
    // skipXrCall: the runtime IPC is dead — null the swapchain handle without xrDestroySwapchain
    // (a doomed round-trip; xrDestroyInstance reaps it). Frees the local image list + content state.
    void destroySwapchain(bool skipXrCall = false);

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
    // report-20 issue E: true iff an explicit user `monitor=NAME,...` rule set this output's
    // resolution. Captured before the manager registers its own declared-mode rule, then refreshed
    // from field provenance on config reload, so adding/removing a user mode applies live without
    // confusing the manager's durable mode for user configuration. When false, the manager owns a
    // persistent rule carrying the xrmonitor-declared mode so it survives plug/unplug/reload.
    bool                    m_userProvidedMode = false;
    bool                    m_hovered = false; // last ray-hovered (WP7 sets this; status field)

    // ---- 2D-plane sync (report 12 WP-S2) — MAIN THREAD ONLY, under COpenXRManager::m_layersMu ----
    // m_l2dUserPinned mirrors m_userProvidedMode for POSITION: true iff an explicit user
    // `monitor=NAME,...,<x>x<y>,...` rule gave this output a layout offset when it was created. Such a
    // monitor is excluded from the projection entirely — the user's rule keeps it where they put it,
    // arrange() places it first (explicitPosition wins), and the auto-placed XR block attaches clear
    // of it. Rule-field provenance keeps this distinct from the offset the sync engine writes and
    // lets a config reload add or remove a user pin live.
    bool                    m_l2dUserPinned = false;
    // The placement the projection last assigned, kept so registerDeclaredMonitorRule can carry it
    // into the persistent monitor rule (durability across a rule refresh) and so `hyprctl openxr
    // status` can show WHY a monitor sits where it does.
    bool                    m_l2dPlaced = false;
    Vector2D                m_l2dOffset;
    int                     m_l2dCol = 0, m_l2dRow = 0;
    float                   m_l2dAzDeg = 0.f, m_l2dElDeg = 0.f;

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
    Vector2D                 m_contentSize;    // the DESKTOP buffer we blit, px (the monitor's pixel mode — double-wide while depth-packed)
    Vector2D                 m_contentOffsetPx;// top-left of the content rect within ONE PANE, px
    OpenXR::SXRChromeGeometry m_chrome;        // normalized layout of ONE PANE (single source of truth)
    // WP X4: how the swapchain is divided. `panes` is 1 for every ordinary monitor, in which case
    // paneFull == m_swapchainSize, paneContent == m_contentSize and every pane-aware expression below
    // is the identity. 2 while the monitor is depth-packed, and then the swapchain holds two
    // independently-margined panes so each eye's quad carries its own chrome ring.
    OpenXR::Stereo::SPaneGeom m_paneGeom;
    XR_GLuint             m_cpuTex = 0;           // CPU-fallback staging tex, sized to mode
    Vector2D              m_cpuTexSize;           // size m_cpuTex was allocated at
    XR_EGLImageKHR        m_lastEGLImg = nullptr; // last dmabuf EGLImage (destroyed on next blit)
    bool                  m_hasContent = false;   // at least one successful blit since (re)create
    bool                  m_quadActive = true;    // false while suspended by the layer cap

    // WP-L2 import-failure log-once state (frame thread only; the blit is single-threaded per layer).
    // The dmabuf import used to spam one EGL error per frame on a cross-GPU black session — we now log
    // one line per changed (fourcc, modifier, EGL-error) signature. See XRGraphics.cpp blitBuffer.
    bool     m_importFailLogged = false;
    uint32_t m_importFailFourcc = 0;
    uint64_t m_importFailMod    = 0;
    unsigned m_importFailEgl    = 0;

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

    // ---- gaze-selection visual-state contract (research/16 §3.3) — extends the WP-G2 contract ----
    // m_gazeSelected: this quad is the dwell-stable gazed-at candidate (whole-quad highlight —
    // "look here to grab"). m_gazeCarried: this quad is being gaze-carried right now. WRITTEN by the
    // frame loop's gazeSelectPass (right after processPointer), READ by the chrome draw pass at the
    // top of the next frame's blit loop — the same one-frame-latency plain-atomic discipline as
    // m_hoverRegion (never a hyprutils refcount op). They light the chrome fade envelope (hover color
    // for a candidate, grab color while carried) so the whole monitor glows as the gaze feedback,
    // since a gaze ray has no cursor of its own for pre-grab selection.
    std::atomic<bool>    m_gazeSelected{false};
    std::atomic<bool>    m_gazeCarried{false};
    // Gaze cursor: one packed word (OpenXR::xrPackCursor) for the gaze point on the CARRIED quad,
    // written by gazeSelectPass and drawn by drawCursor over content + chrome (distinct color). Same
    // one-frame-latency contract; m_gazeCursorDrawn is the redraw-diff of what was last drawn.
    std::atomic<uint32_t> m_gazeCursorPacked{0};
    uint32_t              m_gazeCursorDrawn = 0; // frame-thread only (redraw diff)

    // ---- endpoint-cursor visual-state contract (report 14 Stage A1) — extends the WP-G2 contract ----
    // One packed word per hand (OpenXR::xrPackCursor: present(1) state(3) u(14) v(14)), WRITTEN by the
    // frame loop right after processPointer (from CXRInput::cursorMon/cursorUV/cursorState, matched by
    // monitor id) and READ by the drawCursor pass at the TOP of the next frame's blit loop — the same
    // deliberate one-frame latency + plain-atomic discipline as m_hoverRegion (never a refcount op).
    std::array<std::atomic<uint32_t>, 2> m_cursorPacked{}; // per-hand [L,R]
    // Full quad meters, cached frame-thread during the quad build for next frame's drawCursor to size
    // the cursor to a constant physical diameter (openxr:cursor_size). Frame-thread only.
    float                m_quadWMeters = 0.f;
    float                m_quadHMeters = 0.f;
    // What drawCursor last rendered per hand (redraw diff — an animation-only frame redraws when the
    // cursor changed even with no new desktop buffer). Frame-thread only.
    std::array<uint32_t, 2> m_cursorDrawn{0, 0};

    // Adaptive anchoring (research/13 §5): the last adaptive phase PUBLISHED by the frame thread —
    // both the "previous" value the frame thread edge-detects against (to emit the terminal
    // dock/undock events exactly once) and a status-readable mirror. Written frame-thread after the
    // solve, read main-thread (status JSON) under m_layersMu. Plain atomic — never a refcount op.
    std::atomic<uint8_t> m_adPhase{0 /* OpenXR::XRAD_DOCKED */};

    // WP-L2 observability: which blit path last produced this layer's content (OpenXR::eXRContentPath
    // — none/dmabuf/cpu/black). Written frame-thread in CXRGraphics::blitBuffer, read main-thread for
    // `hyprctl openxr status` (contentPath field) under m_layersMu. Plain atomic — never a refcount op.
    // "black" pinpoints a silent black-quad session (e.g. cross-GPU import failure) in one command.
    std::atomic<uint8_t> m_contentPath{0 /* OpenXR::XR_CONTENT_NONE */};

    // ---- situational transparency (doc 05 §xrrule; report 09) ----
    //
    // MAIN THREAD ONLY (under COpenXRManager::m_layersMu). m_manualFx is the sticky manual override
    // set by `hyprctl openxr alpha|blackalpha <name> <v>` and cleared by `auto` — it is the TOP
    // precedence layer, above the rules, mirroring the shipped hand_input "manual over auto"
    // pattern. m_fxResolved is the last full resolution (defaults -> rules -> manual) kept for
    // `hyprctl openxr status` provenance, and the three envelopes ease the LIVE values toward it so
    // no effect change ever pops (COpenXRManager::advanceEffectEnvelopes on an 8ms main-thread tick).
    OpenXR::SXREffects         m_manualFx;
    OpenXR::SXRResolvedEffects m_fxResolved;
    OpenXR::SXRFxEnv           m_fxAlphaEnv{1.F, 1.F, 1.F};
    OpenXR::SXRFxEnv           m_fxBlackAlphaEnv{1.F, 1.F, 1.F};
    OpenXR::SXRFxEnv           m_fxKneeEnv{0.1F, 0.1F, 1.F};

    // The EASED values the frame loop applies this frame: uniform monitor alpha (a premultiplied
    // multiply over the whole composed swapchain image, so chrome ghosts WITH the content — report
    // 09 §3.3) and the per-monitor luma-key parameters handed to blitBuffer/clearTex. Written by the
    // main thread's envelope tick, read once per frame by the blit loop. Plain atomics — never a
    // hyprutils refcount op, never a string (XRMonitorLayer.hpp threading rule + task #25).
    std::atomic<float> m_fxAlpha{1.F};
    std::atomic<float> m_fxBlackAlpha{1.F};
    std::atomic<float> m_fxKnee{0.1F};
    // What the uniform fade last RENDERED into the swapchain (frame thread only, redraw diff): a
    // change means an animation-only frame must recompose even with no new desktop buffer.
    float              m_fxAlphaDrawn = 1.F;

    // Stereo declaration for this monitor (research/24 §5.1 + §6, WP X1/X3), as ONE published word
    // (OpenXR::Stereo::packDecl): which producer made the panes, how the image splits, whether to
    // submit a pair, and the monitor pixel mode all that describes.
    //
    // The MAIN thread resolves it — it takes a fullscreen-controller lookup, a window's rule fold and
    // the monitor's live packing state, none of which the frame thread may touch — and stores it here
    // from the `presented` listener, i.e. at the moment it hands over the very buffer it describes.
    // The frame thread loads it once per frame and turns it into imageRects + eyeVisibility per eye.
    //
    // A plain word rather than a struct because the frame thread must never read anything whose
    // backing store the main thread can reallocate under it; the same reason m_handGrabMode is a
    // uint8_t. ONE word rather than several atomics because the mode travels with the split: a
    // monitor mid-mode-change would otherwise let the frame thread pair up an image the declaration
    // is not talking about, and spend a frame showing each eye half of a mono desktop.
    std::atomic<uint64_t> m_stereoPairDecl{0};

    // The structural half of m_stereoPairDecl. A depth desktop's two panes belong to the monitor
    // MODE, not to whichever frame happened to be presented most recently. Updated only at bind /
    // modeChanged and folded into m_stereoPairDecl by publishStereoPairLayout(). This prevents an
    // OpenXR focus/visibility bounce from transiently publishing a mono declaration for a buffer
    // that is still physically packed SBS.
    std::atomic<uint64_t> m_stereoModeDecl{0};

    // The XR ray cursor's depth disparity (§5.4, WP X3): the eye-0 shift as a fraction of ONE PANE's
    // width, for whatever the pointer is currently over. MAIN thread publishes the target (it needs
    // the hovered view and the depth ladder, both main-thread-only); the FRAME thread eases toward it
    // with OpenXR::Stereo::easeCursorDisparity so crossing a window edge does not snap the cursor
    // between depths. Zero on every monitor that is not depth-paired, which makes the whole feature a
    // multiply by zero for everyone else.
    std::atomic<float>    m_cursorDisparityTarget{0.F};
    float                 m_cursorDisparity      = 0.F; // frame thread only (the eased value)
    float                 m_cursorDisparityDrawn = 0.F; // frame thread only (redraw diff)

    // How many composition layers this monitor submitted LAST FRAME: 0 (nothing — no content yet,
    // suspended by the layer cap, or a pair the budget refused), 1 (an ordinary quad) or 2 (a
    // stereo pair). Starts at zero, is cleared at every session/view boundary, and is zeroed for
    // every active layer at the top of quad assembly so it can never report a stale count. Frame
    // thread → main thread (status + cursor eligibility), so `hyprctl openxr` can
    // answer "is the pair actually being submitted" rather than only "was it declared" — the two
    // differ when the budget refuses a pair, which is the one failure mode with no visual tell.
    std::atomic<uint8_t> m_quadsSubmitted{0};

    // Whether the chrome draw pass ran for this monitor LAST FRAME (WP X4). Frame thread → main
    // thread (status only). It exists because chrome suppression is a frame-thread decision with no
    // other outside view: X1 suppresses chrome on a CONTENT pair and X4 restores it on a DEPTH pair,
    // and "my XR monitor stopped being grabbable" is otherwise a bug report with nothing to look at.
    // False also for chrome_enabled = 0 / chrome_margin = 0, which is the honest answer to
    // "is there chrome on this quad".
    std::atomic<bool>    m_chromeLive{false};

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
