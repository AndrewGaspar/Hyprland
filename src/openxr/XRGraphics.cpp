#include "XRGraphics.hpp"
#ifdef HAVE_OPENXR

// EGL/GLES headers only — this TU never touches OpenXR, so it needs no platform macros.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>

#include <cstdio>
#include <cstring>
#include <vector>
#include <fcntl.h>
#include <unistd.h>

#include <gbm.h>
#include <xf86drm.h>

#include "../Compositor.hpp"
#include "../render/OpenGL.hpp"
#include "../debug/log/Logger.hpp"

// EGL/GL extension procs — loaded once in initEGL. Kept at file scope (the WIP pattern);
// the per-layer DMA-BUF blit that consumes them lands in WP3.
using PFNEGLCREATEIMAGEKHRPROC_t        = EGLImageKHR (*)(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint*);
using PFNEGLDESTROYIMAGEKHRPROC_t       = EGLBoolean (*)(EGLDisplay, EGLImageKHR);
using PFNGLEGLIMAGETARGETTEX2DOESPROC_t = void (*)(GLenum, GLeglImageOES);

static PFNEGLCREATEIMAGEKHRPROC_t        s_eglCreateImage  = nullptr;
static PFNEGLDESTROYIMAGEKHRPROC_t       s_eglDestroyImage = nullptr;
static PFNGLEGLIMAGETARGETTEX2DOESPROC_t s_glImageTarget2D = nullptr;

#ifndef EGL_DRM_RENDER_NODE_FILE_EXT
#define EGL_DRM_RENDER_NODE_FILE_EXT 0x3377
#endif
#ifndef EGL_DRM_DEVICE_FILE_EXT
#define EGL_DRM_DEVICE_FILE_EXT 0x3233
#endif

CXRGraphics::CScopedGLContext::CScopedGLContext(CXRGraphics& gfx) : m_gfx(gfx) {
    eglMakeCurrent(m_gfx.m_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, m_gfx.m_xrContext);
}

CXRGraphics::CScopedGLContext::~CScopedGLContext() {
    // Always leave the context UNBOUND — Monado's compositor thread binds it itself, and
    // leaving it current across an XR interop call crashes AMD gallium (doc 01).
    eglMakeCurrent(m_gfx.m_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

CXRGraphics::~CXRGraphics() {
    destroyGL();
    destroyEGL();
}

bool CXRGraphics::selectDisplay(const std::string& gpuOverride) {
    using PFNEGLGETPLATFORMDISPLAYEXTPROC_t = EGLDisplay (*)(EGLenum, void*, const EGLint*);
    using PFNEGLQUERYDEVICESEXTPROC_t       = EGLBoolean (*)(EGLint, EGLDeviceEXT*, EGLint*);
    using PFNEGLQUERYDEVICESTRINGEXTPROC_t  = const char* (*)(EGLDeviceEXT, EGLint);
    auto eglGetPlatformDisplayEXT_fn = (PFNEGLGETPLATFORMDISPLAYEXTPROC_t)eglGetProcAddress("eglGetPlatformDisplayEXT");
    auto eglQueryDevicesEXT_fn       = (PFNEGLQUERYDEVICESEXTPROC_t)eglGetProcAddress("eglQueryDevicesEXT");
    auto eglQueryDeviceStringEXT_fn  = (PFNEGLQUERYDEVICESTRINGEXTPROC_t)eglGetProcAddress("eglQueryDeviceStringEXT");

    // Determine the DRM render-node path we want to open an EGL display on.
    //
    // Default (changed from the WIP, which hard-coded the AMD/Mesa PCI vendor 0x1002 and
    // skipped NVIDIA 0x10de): match HYPRLAND'S PRIMARY GPU render node. The WIP's real
    // constraint is that cross-GPU DMA-BUF import crashes Monado in driUnbindContext when
    // importing buffers produced on a different GPU — so the correct general default is to
    // put the XR EGL context on the SAME node Hyprland renders on. openxr:gpu overrides
    // this for hybrid setups where the runtime lives on the other GPU.
    std::string targetNode = gpuOverride;
    bool        overridden = !gpuOverride.empty();
    if (!overridden) {
        if (char* name = drmGetRenderDeviceNameFromFd(g_pCompositor->m_drmRenderNode.fd)) {
            targetNode = name;
            free(name);
        }
    }

    if (targetNode.empty())
        Log::logger->log(Log::WARN, "[OPENXR] could not resolve Hyprland's render node; will scan EGL devices without a target");
    else
        Log::logger->log(Log::DEBUG, "[OPENXR] target XR render node: {}{}", targetNode, overridden ? " (openxr:gpu override)" : " (Hyprland primary)");

    if (eglGetPlatformDisplayEXT_fn && eglQueryDevicesEXT_fn && eglQueryDeviceStringEXT_fn) {
        EGLint numDevs = 0;
        eglQueryDevicesEXT_fn(0, nullptr, &numDevs);
        std::vector<EGLDeviceEXT> devs(numDevs);
        eglQueryDevicesEXT_fn(numDevs, devs.data(), &numDevs);
        Log::logger->log(Log::DEBUG, "[OPENXR] found {} EGL devices", numDevs);

        for (auto dev : devs) {
            // Prefer the render-node path, fall back to the primary/card path.
            const char* path = eglQueryDeviceStringEXT_fn(dev, EGL_DRM_RENDER_NODE_FILE_EXT);
            if (!path)
                path = eglQueryDeviceStringEXT_fn(dev, EGL_DRM_DEVICE_FILE_EXT);
            if (!path)
                continue;

            // If we have a target node, only accept the device that matches it.
            if (!targetNode.empty() && targetNode != path)
                continue;

            // Use EGL_PLATFORM_GBM_KHR (the normal Mesa rendering path).
            // EGL_PLATFORM_DEVICE_EXT is headless/compute-only and leaves gallium
            // pipe_context state partially uninitialised, causing driUnbindContext to crash.
            int fd = open(path, O_RDWR | O_CLOEXEC);
            if (fd < 0)
                continue;
            struct gbm_device* gbm = gbm_create_device(fd);
            if (!gbm) {
                close(fd);
                continue;
            }
            EGLDisplay tryDpy = eglGetPlatformDisplayEXT_fn(EGL_PLATFORM_GBM_KHR, gbm, nullptr);
            if (tryDpy == EGL_NO_DISPLAY) {
                gbm_device_destroy(gbm);
                close(fd);
                continue;
            }
            Log::logger->log(Log::DEBUG, "[OPENXR] using GBM EGL display from {} for the XR context", path);
            m_eglDisplay  = tryDpy;
            m_gbmOwned    = gbm;
            m_gbmFd       = fd;
            m_ownsDisplay = true;
            return true;
        }
    }

    // A configured openxr:gpu that matched no EGL device is a misconfiguration: fail loudly
    // rather than silently rendering XR on the wrong GPU (doc 01).
    if (overridden) {
        Log::logger->log(Log::ERR, "[OPENXR] openxr:gpu '{}' did not match any EGL device — refusing to start", gpuOverride);
        return false;
    }

    // Last-resort fallback: reuse Hyprland's own EGL display. May crash on cross-GPU
    // DMA-BUF import, but worth trying (eglInitialize is refcounted, so re-initializing an
    // already-initialized display is fine).
    if (Render::GL::g_pHyprOpenGL) {
        Log::logger->log(Log::WARN, "[OPENXR] no matching EGL device found, falling back to Hyprland's display (cross-GPU import may fail)");
        m_eglDisplay  = Render::GL::g_pHyprOpenGL->m_eglDisplay;
        m_ownsDisplay = false;
        return true;
    }

    Log::logger->log(Log::ERR, "[OPENXR] no EGL display available");
    return false;
}

bool CXRGraphics::initEGL(const std::string& gpuOverride) {
    if (!selectDisplay(gpuOverride))
        return false;

    if (m_eglDisplay == EGL_NO_DISPLAY) {
        Log::logger->log(Log::ERR, "[OPENXR] failed to get an EGL display (0x{:x})", (unsigned)eglGetError());
        return false;
    }

    EGLint major = 0, minor = 0;
    if (!eglInitialize(m_eglDisplay, &major, &minor)) {
        Log::logger->log(Log::ERR, "[OPENXR] eglInitialize failed (0x{:x})", (unsigned)eglGetError());
        return false;
    }
    Log::logger->log(Log::DEBUG, "[OPENXR] EGL {}.{} initialized", major, minor);

    eglBindAPI(EGL_OPENGL_ES_API);

    // Pick an EGL config. Try progressively more permissive filters.
    // Note: the default EGL_SURFACE_TYPE is EGL_WINDOW_BIT, which excludes pbuffer-only
    // configs on the EGL device platform — so we must specify it explicitly.
    EGLConfig    cfg = nullptr;
    EGLint       n   = 0;
    const EGLint cfgAttempts[][9] = {
        // GBM: window+pbuffer, GLES3, RGBA8
        {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, (EGLint)(EGL_WINDOW_BIT | EGL_PBUFFER_BIT), EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_NONE},
        // Device/NVIDIA: pbuffer only, GLES3, RGBA8
        {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_NONE},
        // Any surface type, GLES3, RGBA8
        {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, 0, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_NONE},
        // Any surface type, any GLES3 config
        {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, 0, EGL_NONE, 0, EGL_NONE, 0, EGL_NONE},
    };
    for (auto& attrs : cfgAttempts) {
        if (eglChooseConfig(m_eglDisplay, attrs, &cfg, 1, &n) && n > 0)
            break;
        cfg = nullptr;
    }
    if (!cfg) {
        // Manually scan all configs (works around Mesa eglChooseConfig quirks on the
        // shared-display fallback).
        EGLint numAll = 0;
        eglGetConfigs(m_eglDisplay, nullptr, 0, &numAll);
        std::vector<EGLConfig> all(numAll);
        eglGetConfigs(m_eglDisplay, all.data(), numAll, &numAll);
        for (auto& c : all) {
            EGLint rt = 0;
            eglGetConfigAttrib(m_eglDisplay, c, EGL_RENDERABLE_TYPE, &rt);
            if (rt & EGL_OPENGL_ES3_BIT) {
                cfg = c;
                Log::logger->log(Log::DEBUG, "[OPENXR] manually selected a GLES3 config (eglChooseConfig workaround)");
                break;
            }
        }
    }
    if (!cfg) {
        Log::logger->log(Log::ERR, "[OPENXR] no suitable EGL config found (0x{:x})", (unsigned)eglGetError());
        return false;
    }
    m_config = cfg;
    Log::logger->log(Log::DEBUG, "[OPENXR] EGL config selected");

    const EGLint ctxAttrs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    m_xrContext             = eglCreateContext(m_eglDisplay, m_config, EGL_NO_CONTEXT, ctxAttrs);
    if (m_xrContext == EGL_NO_CONTEXT) {
        Log::logger->log(Log::ERR, "[OPENXR] eglCreateContext failed (0x{:x})", (unsigned)eglGetError());
        return false;
    }

    // Load EGL/GL extension procs once (consumed by the WP3 blit).
    s_eglCreateImage  = (PFNEGLCREATEIMAGEKHRPROC_t)eglGetProcAddress("eglCreateImageKHR");
    s_eglDestroyImage = (PFNEGLDESTROYIMAGEKHRPROC_t)eglGetProcAddress("eglDestroyImageKHR");
    s_glImageTarget2D = (PFNGLEGLIMAGETARGETTEX2DOESPROC_t)eglGetProcAddress("glEGLImageTargetTexture2DOES");

    Log::logger->log(Log::DEBUG, "[OPENXR] EGL context created");
    return true;
}

bool CXRGraphics::initBlitGL() {
    // Runs on the main thread before the frame thread starts, so m_xrContext is available.
    CScopedGLContext ctx(*this);

    // GL_TEXTURE_EXTERNAL_OES for the DMA-BUF path (rebound per blit in WP3).
    glGenTextures(1, &m_extTex);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, m_extTex);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Vertex shader: fullscreen triangle from gl_VertexID, no VBO needed.
    const char* vsSrc = R"(
        #version 300 es
        out vec2 vUV;
        void main() {
            vec2 pos;
            if (gl_VertexID == 0)      pos = vec2(-1.0, -1.0);
            else if (gl_VertexID == 1) pos = vec2( 3.0, -1.0);
            else                       pos = vec2(-1.0,  3.0);
            vUV = pos * 0.5 + 0.5;
            gl_Position = vec4(pos, 0.0, 1.0);
        }
    )";

    // Fragment shader: sample the external OES texture (DMA-BUF import).
    const char* fsSrc = R"(
        #version 300 es
        #extension GL_OES_EGL_image_external_essl3 : require
        precision mediump float;
        uniform samplerExternalOES uTex;
        in vec2 vUV;
        out vec4 fragColor;
        void main() {
            fragColor = texture(uTex, vUV);
        }
    )";

    auto compileShader = [](GLenum type, const char* src) -> GLuint {
        GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &src, nullptr);
        glCompileShader(s);
        GLint ok = 0;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[512] = {};
            glGetShaderInfoLog(s, sizeof(log), nullptr, log);
            Log::logger->log(Log::ERR, "[OPENXR] shader compile error: {}", log);
            glDeleteShader(s);
            return 0;
        }
        return s;
    };

    GLuint vs = compileShader(GL_VERTEX_SHADER, vsSrc);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, fsSrc);
    if (!vs || !fs) {
        glDeleteShader(vs);
        glDeleteShader(fs);
        return false;
    }

    m_blitProg = glCreateProgram();
    glAttachShader(m_blitProg, vs);
    glAttachShader(m_blitProg, fs);
    glLinkProgram(m_blitProg);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint linked = 0;
    glGetProgramiv(m_blitProg, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[512] = {};
        glGetProgramInfoLog(m_blitProg, sizeof(log), nullptr, log);
        Log::logger->log(Log::ERR, "[OPENXR] shader link error: {}", log);
        glDeleteProgram(m_blitProg);
        m_blitProg = 0;
        return false;
    }

    glGenVertexArrays(1, &m_blitVAO);

    Log::logger->log(Log::DEBUG, "[OPENXR] blit GL resources initialized");
    return true;
}

void CXRGraphics::destroyGL() {
    if (m_xrContext == EGL_NO_CONTEXT)
        return;

    // GL objects must be deleted with the context current — briefly make it so.
    eglMakeCurrent(m_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, m_xrContext);
    if (m_extTex) {
        glDeleteTextures(1, &m_extTex);
        m_extTex = 0;
    }
    if (m_blitVAO) {
        glDeleteVertexArrays(1, &m_blitVAO);
        m_blitVAO = 0;
    }
    if (m_blitProg) {
        glDeleteProgram(m_blitProg);
        m_blitProg = 0;
    }
    eglMakeCurrent(m_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

void CXRGraphics::destroyEGL() {
    if (m_xrContext != EGL_NO_CONTEXT) {
        eglDestroyContext(m_eglDisplay, m_xrContext);
        m_xrContext = EGL_NO_CONTEXT;
    }
    // Only terminate a display we own — never tear down Hyprland's shared display.
    if (m_ownsDisplay && m_eglDisplay != EGL_NO_DISPLAY)
        eglTerminate(m_eglDisplay);
    m_eglDisplay = EGL_NO_DISPLAY;
    m_config     = nullptr;

    // The display depends on the GBM device — destroy it last.
    if (m_gbmOwned) {
        gbm_device_destroy(m_gbmOwned);
        m_gbmOwned = nullptr;
    }
    if (m_gbmFd >= 0) {
        close(m_gbmFd);
        m_gbmFd = -1;
    }
    m_ownsDisplay = false;
}

#endif // HAVE_OPENXR
