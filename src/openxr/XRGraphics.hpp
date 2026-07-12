#pragma once
#ifdef HAVE_OPENXR

#include <EGL/egl.h> // EGLDisplay, EGLContext, EGLConfig, EGL_NO_DISPLAY, EGL_NO_CONTEXT
#include <cstdint>
#include <string>

#include "../helpers/memory/Memory.hpp"
#include "../helpers/math/Math.hpp"

// Forward-declare GL/EGL extension types so this header compiles in TUs that do not
// pull in the GLES headers (the WIP forward-declaration trick — see doc 01).
using XR_GLuint      = unsigned int; // = GLuint
using XR_EGLImageKHR = void*;        // = EGLImageKHR

struct gbm_device;

namespace Aquamarine {
    class IBuffer;
}
class CXRMonitorLayer;

// CXRGraphics owns the GBM/EGL display + context and the shared GL blit resources used
// to push presented monitor buffers into XR swapchains. Ported from the WIP prototype
// (git branch openxr) — see docs/openxr/01-session-graphics.md.
//
// Threading: the EGL context (m_xrContext) is created on the MAIN thread inside
// COpenXRManager::start(), owned EXCLUSIVELY by the XR frame thread while it runs, and
// handed back to the main thread after the frame thread is joined (for teardown). The
// context must never be held current across an XR call that touches the runtime's GL
// interop — every GL burst is wrapped in CScopedGLContext.
class CXRGraphics {
  public:
    CXRGraphics() = default;
    ~CXRGraphics();

    // Main thread, inside start(): pick the EGL display on the right GPU, create the
    // context, load the extension procs. gpuOverride is openxr:gpu (empty = follow
    // Hyprland's primary render node). Returns false => start() fails => UNAVAILABLE.
    bool initEGL(const std::string& gpuOverride);
    // Main thread, inside start(), after the session exists: compile the blit program
    // and allocate the shared external-OES texture / VAO. (Per-layer swapchain blits
    // land in WP3.)
    bool initBlitGL();

    // Teardown is split so COpenXRManager can honor doc 01's ordering: GL objects are
    // deleted with the context current (destroyGL), then the XR handles are destroyed
    // (CXRSession::destroy), then the context/display/GBM are torn down (destroyEGL).
    void destroyGL();  // context current -> delete shared GL objects -> unbind
    void destroyEGL(); // eglDestroyContext, eglTerminate, gbm_device_destroy, close(fd)

    // Frame thread, inside a CScopedGLContext. Blit a presented monitor buffer into the
    // layer's acquired swapchain image (dstTex). Tries DMA-BUF import, then the CPU
    // data-pointer fallback, then a black clear. Uses/updates the per-layer m_lastEGLImg +
    // m_cpuTex (sized to the source mode). See doc 01 "Blit pipeline".
    void blitBuffer(const SP<Aquamarine::IBuffer>& buf, CXRMonitorLayer& layer, XR_GLuint dstTex);
    // Frame thread, inside a CScopedGLContext. Clear an image to a solid color.
    void clearTex(XR_GLuint dstTex, const Vector2D& size, float r, float g, float b);
    // Delete a layer's per-layer GL objects (EGLImage + CPU staging tex + chrome snapshot tex).
    // Context must be current.
    void destroyLayerGL(XR_EGLImageKHR img, XR_GLuint cpuTex, XR_GLuint contentTex);

    // ---- WP-G2: chrome snapshot + move-bar/corner-handle draw pass (frame thread, in a
    // CScopedGLContext). See docs/openxr/research/04-grabbable-borders.md §5.5/§8. ----
    // Copy a freshly-blitted swapchain image (srcTex, sized to layer.m_swapchainSize) into the
    // layer's persistent content snapshot texture (allocated/resized here). Called after each
    // real content blit so a later animation-only frame can restore it.
    void snapshotSwapchain(CXRMonitorLayer& layer, XR_GLuint srcTex);
    // Restore the content snapshot into a newly-acquired swapchain image (dstTex) for an
    // animation-only frame (chrome fading with no new desktop buffer). Returns false if no
    // snapshot exists yet (caller then skips the chrome-only redraw).
    bool restoreSnapshot(CXRMonitorLayer& layer, XR_GLuint dstTex);
    // Draw the auto-hiding chrome (bottom move-bar + four corner resize handles) into the margin
    // of dstTex, over already-present content. `alpha` is the fade envelope [0,1]; `hoverRegion`
    // (OpenXR::eXRQuadRegion) highlights the hovered element; `grabbed` overrides all elements to
    // the grab color. Colors come from openxr:chrome_col_* and are written PREMULTIPLIED (the quad
    // composites TEXTURE_SOURCE_ALPHA). Only touches margin pixels — never the content rect.
    void drawChrome(CXRMonitorLayer& layer, XR_GLuint dstTex, float alpha, uint8_t hoverRegion, bool grabbed);

    // RAII guard for an XR GL burst. ctor makes the XR context current; dtor RESTORES whatever
    // EGL binding was current before the burst (WP-L1, research doc 17 §5) instead of blindly
    // dropping to EGL_NO_CONTEXT. The old unconditional unbind left Hyprland's renderer to lazily
    // re-bind — survivable on the NVIDIA pin, but on a shared GPU it let the desktop renderer run
    // against a stale/empty binding, a candidate for the on-toggle host-monitor corruption. The
    // restore never re-binds the XR context itself (see the dtor) so the interop contract holds
    // (the runtime binds the XR context itself; a lingering XR context crashes AMD gallium) and
    // the frame thread can still claim it. The ONLY sanctioned way GL work is issued — see doc 01.
    struct CScopedGLContext {
        explicit CScopedGLContext(CXRGraphics& gfx);
        ~CScopedGLContext();
        CScopedGLContext(const CScopedGLContext&)            = delete;
        CScopedGLContext& operator=(const CScopedGLContext&) = delete;
        CXRGraphics& m_gfx;
        // Previously-current EGL binding, captured in the ctor, restored in the dtor.
        EGLDisplay m_savedDisplay = EGL_NO_DISPLAY;
        EGLSurface m_savedDraw    = EGL_NO_SURFACE;
        EGLSurface m_savedRead    = EGL_NO_SURFACE;
        EGLContext m_savedContext = EGL_NO_CONTEXT;
    };

    EGLDisplay         m_eglDisplay  = EGL_NO_DISPLAY;
    EGLContext         m_xrContext   = EGL_NO_CONTEXT; // owned by the frame thread while running
    EGLConfig          m_config      = nullptr;
    struct gbm_device* m_gbmOwned    = nullptr;        // set iff we opened our own GBM device
    int                m_gbmFd       = -1;
    bool               m_ownsDisplay = false;          // false = shared-display fallback (do not terminate)
    // EGL_EXT_image_dma_buf_import_modifiers present on m_eglDisplay (queried in initEGL). When true,
    // blitBuffer passes explicit per-plane dmabuf modifiers — REQUIRED for cross-GPU import on NVIDIA
    // (rejects modifier-less imports with EGL_BAD_ATTRIBUTE even for LINEAR; research/17 §4.2, WP-L2).
    // When false we keep the modifier-less attrib list so drivers without the ext are not regressed.
    bool               m_hasModifiers = false;

    // Shared blit resources (all in the m_xrContext share group).
    XR_GLuint m_blitProg = 0; // external-OES -> FBO program
    XR_GLuint m_blitVAO  = 0; // dummy VAO for the fullscreen triangle
    XR_GLuint m_extTex   = 0; // GL_TEXTURE_EXTERNAL_OES, rebound per DMA-BUF blit

    // The DRM render node the XR EGL context landed on (wrong-GPU fail-closed guard). Populated by
    // selectDisplay when we open our own GBM device; `valid` is false on the shared-display
    // fallback (no dedicated node to compare). major/minor are the node's device numbers, compared
    // in start() against the runtime's render node (OpenXR::probeRuntimeRenderNode) — a mismatch
    // means cross-GPU import, which crashes the driver, so start() fails closed.
    struct SXRRenderNode {
        std::string path;
        int64_t     major = -1;
        int64_t     minor = -1;
        bool        valid = false;
    };
    const SXRRenderNode& selectedRenderNode() const {
        return m_selectedNode;
    }

  private:
    // Picks m_eglDisplay (+ m_gbmOwned/m_gbmFd when we own it). WIP GPU-selection port,
    // default adapted to "match Hyprland's primary GPU render node".
    bool selectDisplay(const std::string& gpuOverride);

    SXRRenderNode m_selectedNode;
};

#endif // HAVE_OPENXR
