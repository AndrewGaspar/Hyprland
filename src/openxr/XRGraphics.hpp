#pragma once
#ifdef HAVE_OPENXR

#include <EGL/egl.h> // EGLDisplay, EGLContext, EGLConfig, EGL_NO_DISPLAY, EGL_NO_CONTEXT
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
    // Delete a layer's per-layer GL objects (EGLImage + staging tex). Context must be current.
    void destroyLayerGL(XR_EGLImageKHR img, XR_GLuint cpuTex);

    // RAII guard: ctor eglMakeCurrent(m_xrContext), dtor eglMakeCurrent(EGL_NO_CONTEXT).
    // The ONLY sanctioned way GL work is issued — see doc 01 "EGL context ownership".
    struct CScopedGLContext {
        explicit CScopedGLContext(CXRGraphics& gfx);
        ~CScopedGLContext();
        CScopedGLContext(const CScopedGLContext&)            = delete;
        CScopedGLContext& operator=(const CScopedGLContext&) = delete;
        CXRGraphics& m_gfx;
    };

    EGLDisplay         m_eglDisplay  = EGL_NO_DISPLAY;
    EGLContext         m_xrContext   = EGL_NO_CONTEXT; // owned by the frame thread while running
    EGLConfig          m_config      = nullptr;
    struct gbm_device* m_gbmOwned    = nullptr;        // set iff we opened our own GBM device
    int                m_gbmFd       = -1;
    bool               m_ownsDisplay = false;          // false = shared-display fallback (do not terminate)

    // Shared blit resources (all in the m_xrContext share group).
    XR_GLuint m_blitProg = 0; // external-OES -> FBO program
    XR_GLuint m_blitVAO  = 0; // dummy VAO for the fullscreen triangle
    XR_GLuint m_extTex   = 0; // GL_TEXTURE_EXTERNAL_OES, rebound per DMA-BUF blit

  private:
    // Picks m_eglDisplay (+ m_gbmOwned/m_gbmFd when we own it). WIP GPU-selection port,
    // default adapted to "match Hyprland's primary GPU render node".
    bool selectDisplay(const std::string& gpuOverride);
};

#endif // HAVE_OPENXR
