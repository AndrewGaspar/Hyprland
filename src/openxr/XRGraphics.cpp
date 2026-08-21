#include "XRGraphics.hpp"
#ifdef HAVE_OPENXR

// EGL/GLES headers only — this TU never touches OpenXR, so it needs no platform macros.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

#include <gbm.h>
#include <xf86drm.h>

#include "../Compositor.hpp"
#include "../render/OpenGL.hpp"
#include "../debug/log/Logger.hpp"
#include "../config/ConfigValue.hpp"
#include "XRMonitorLayer.hpp"
#include "XRDmabufImport.hpp"
#include "XRBoundedCall.hpp" // OpenXR::runBoundedProbe — the throwaway-thread pattern for driver calls
#include "XRMath.hpp" // OpenXR::xrLumaKeyAlpha / xrLumaKeyPremultiplied (the luma-key reference curve)
#include <aquamarine/buffer/Buffer.hpp>

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
    // Save the binding that is current on THIS thread so the dtor can restore it verbatim (WP-L1,
    // doc 17 §5). On the main thread this is Hyprland's renderer context (+ its surfaces); on the
    // frame thread nothing is current between bursts, so this captures EGL_NO_CONTEXT.
    m_savedDisplay = eglGetCurrentDisplay();
    m_savedContext = eglGetCurrentContext();
    m_savedDraw    = eglGetCurrentSurface(EGL_DRAW);
    m_savedRead    = eglGetCurrentSurface(EGL_READ);
    eglMakeCurrent(m_gfx.m_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, m_gfx.m_xrContext);
}

CXRGraphics::CScopedGLContext::~CScopedGLContext() {
    // Restore the pre-burst binding instead of blindly unbinding. This both keeps the interop
    // contract (the XR context must NOT stay current across a runtime GL call — Monado/WiVRn bind
    // it themselves; a lingering XR context crashes AMD gallium, doc 01) AND repairs the main
    // thread's binding so Hyprland's renderer is not left running against an unbound/stale context
    // (doc 17 §5 "bug a"). Never restore the XR context itself: it is only ever transiently current
    // inside this scope, and re-binding it would violate the interop contract and pin it to this
    // thread (blocking the frame thread from claiming it). Fall back to a clean unbind otherwise.
    if (m_savedContext != EGL_NO_CONTEXT && m_savedContext != m_gfx.m_xrContext && m_savedDisplay != EGL_NO_DISPLAY)
        eglMakeCurrent(m_savedDisplay, m_savedDraw, m_savedRead, m_savedContext);
    else
        eglMakeCurrent(m_gfx.m_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

CXRGraphics::~CXRGraphics() {
    destroyGL();
    destroyEGL();
}

// Result of the BOUNDED EGL device scan below. Plain data only — it crosses a thread boundary and
// may be discarded wholesale if that thread is abandoned, so it owns everything it names (the node
// paths are copied out of the driver-owned strings on the worker).
namespace {
    struct SEglDeviceScan {
        bool                     entryPoints = false; // the three EXT entry points resolved
        void*                    getPlatformDisplay = nullptr; // PFNEGLGETPLATFORMDISPLAYEXTPROC, resolved on the worker
        size_t                   deviceCount = 0;     // how many devices the driver reported
        std::vector<std::string> nodePaths;           // DRM node per device we actually queried, in order
        bool                     pinShortCircuit = false; // stopped early because the pin matched
    };
}

/*
 * WHAT A PIN CAN AND CANNOT AVOID — measured on a dual-vendor box (NVIDIA RTX 5070 + AMD 890M,
 * glvnd with 10_nvidia.json + 50_mesa.json), instrumenting /proc/self/maps and /proc/self/fd:
 *
 *   after eglGetProcAddress   libEGL_nvidia LOADED, libnvidia-eglcore LOADED, mesa LOADED
 *   after eglQueryDevicesEXT(0, nullptr, &n)   -> /dev/nvidiactl, /dev/nvidia0 OPEN (x3)
 *   after the per-device string queries        -> /dev/dri/renderD128 OPEN
 *
 * Read that top line again: the vendor libraries are all loaded by the FIRST eglGetProcAddress, and
 * the NVIDIA KERNEL DRIVER is contacted by the COUNT-ONLY enumeration call — before a single
 * EGLDeviceEXT handle exists for us to filter on. libglvnd has to do this: eglQueryDevicesEXT is not
 * tied to a display, so libEGL must ask every vendor and merge the answers, and eglGetProcAddress
 * must load every vendor to discover who implements the extension at all.
 *
 * THEREFORE: openxr:gpu cannot keep us out of a wedged vendor's driver. There is no EGL API to say
 * "only this vendor" — the sole lever is the __EGL_VENDOR_LIBRARY_FILENAMES environment variable,
 * which is deliberately NOT used here (it is session-scoped and would be inherited by every client
 * app the compositor spawns — a much larger blast radius than the problem).
 *
 * What the pin CAN do, and does below, is bound the damage after that point: stop the per-device
 * string queries as soon as the pinned node is found, so a non-matching vendor is asked for as
 * little as possible, as late as possible. And what actually protects the user is the other two
 * guards: the kernel-taint tripwire refuses bring-up BEFORE any of this runs (doc 01), and this
 * whole scan is bounded on a throwaway thread so a driver that never returns costs XR, not the
 * desktop.
 */
static SEglDeviceScan scanEglDevices(const std::string& targetNode, const std::atomic<bool>& abandon) {
    using PFNEGLGETPLATFORMDISPLAYEXTPROC_t = EGLDisplay (*)(EGLenum, void*, const EGLint*);
    using PFNEGLQUERYDEVICESEXTPROC_t       = EGLBoolean (*)(EGLint, EGLDeviceEXT*, EGLint*);
    using PFNEGLQUERYDEVICESTRINGEXTPROC_t  = const char* (*)(EGLDeviceEXT, EGLint);

    SEglDeviceScan out;

    // This is the call that loads every glvnd vendor. It is the first thing on the worker, and
    // nothing on the main thread resolves these entry points, so a vendor that hangs here hangs
    // only this abandonable thread.
    auto getPlatformDisplay = (PFNEGLGETPLATFORMDISPLAYEXTPROC_t)eglGetProcAddress("eglGetPlatformDisplayEXT");
    auto queryDevices       = (PFNEGLQUERYDEVICESEXTPROC_t)eglGetProcAddress("eglQueryDevicesEXT");
    auto queryDeviceString  = (PFNEGLQUERYDEVICESTRINGEXTPROC_t)eglGetProcAddress("eglQueryDeviceStringEXT");
    if (!getPlatformDisplay || !queryDevices || !queryDeviceString)
        return out;

    out.entryPoints        = true;
    out.getPlatformDisplay = (void*)getPlatformDisplay;
    if (abandon.load(std::memory_order_acquire))
        return out;

    EGLint numDevs = 0;
    queryDevices(0, nullptr, &numDevs); // contacts every vendor's kernel driver — see above
    if (numDevs <= 0 || abandon.load(std::memory_order_acquire))
        return out;

    std::vector<EGLDeviceEXT> devs(numDevs);
    queryDevices(numDevs, devs.data(), &numDevs);
    out.deviceCount = (size_t)std::max(0, numDevs);
    devs.resize(out.deviceCount);

    for (auto dev : devs) {
        if (abandon.load(std::memory_order_acquire))
            return out;

        // Prefer the render-node path, fall back to the primary/card path. No eglGetError() dance:
        // the EGL error latch is PER-THREAD and this thread is thrown away, so a failed query
        // cannot leave anything behind for the main thread's EGL bookkeeping to trip over.
        const char* path = queryDeviceString(dev, EGL_DRM_RENDER_NODE_FILE_EXT);
        if (!path)
            path = queryDeviceString(dev, EGL_DRM_DEVICE_FILE_EXT);
        out.nodePaths.emplace_back(path ? path : "");

        // Pin-aware: the caller wants exactly this node, so stop touching further devices.
        if (path && !targetNode.empty() && targetNode == path) {
            out.pinShortCircuit = true;
            break;
        }
    }

    return out;
}

bool CXRGraphics::selectDisplay(const std::string& gpuOverride) {
    using PFNEGLGETPLATFORMDISPLAYEXTPROC_t = EGLDisplay (*)(EGLenum, void*, const EGLint*);

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

    // BOUNDED. Enumeration enters every installed GPU vendor driver (see scanEglDevices), and a
    // wedged one blocks forever — on the main thread that is a frozen desktop, not a failed XR
    // start. A timeout is treated exactly like an enumeration failure: refuse cleanly, let the
    // re-probe try again later.
    // targetNode is captured BY VALUE, not by reference. On a timeout this frame returns while the
    // worker is still running — a captured reference would dangle into a destroyed local the moment
    // the abandoned thread unblocked and compared it against a device path.
    int        blockedMs = 0;
    const auto scanned =
        OpenXR::runBoundedProbe([targetNode](const std::atomic<bool>& abandon) { return scanEglDevices(targetNode, abandon); }, OpenXR::XR_PROBE_TIMEOUT_MS, &blockedMs);
    // The bound abandons the WORKER; the caller still parks for the full duration. On the main
    // thread that IS the desktop hitch, so name it rather than leaving it invisible.
    if (blockedMs >= OpenXR::XR_MAIN_THREAD_STALL_WARN_MS)
        Log::logger->log(Log::WARN, "[OPENXR] EGL device enumeration blocked the compositor main thread for {}ms (a GPU driver was slow to answer)", blockedMs);
    if (!scanned.has_value()) {
        Log::logger->log(Log::ERR,
                         "[OPENXR] EGL device enumeration did not complete within {}ms — a GPU driver is not responding. Refusing to start XR (the desktop is unaffected; "
                         "the enumeration thread is abandoned). If the kernel has oopsed this boot, reboot.",
                         OpenXR::XR_PROBE_TIMEOUT_MS);
        return false;
    }
    const auto& scan = *scanned;

    if (scan.entryPoints && scan.deviceCount > 0) {
        Log::logger->log(Log::DEBUG, "[OPENXR] found {} EGL devices; queried {}{}", scan.deviceCount, scan.nodePaths.size(),
                         scan.pinShortCircuit ? " (stopped at the pinned node)" : "");

        auto eglGetPlatformDisplayEXT_fn = (PFNEGLGETPLATFORMDISPLAYEXTPROC_t)scan.getPlatformDisplay;

        for (const auto& node : scan.nodePaths) {
            if (node.empty())
                continue;
            const char* path = node.c_str();

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
            // Record the node's DRM device numbers so start() can verify it matches the runtime's
            // GPU before handing the runtime a (possibly cross-GPU) EGL binding.
            struct stat st;
            if (fstat(fd, &st) == 0) {
                m_selectedNode.path  = path;
                m_selectedNode.major = (int64_t)major(st.st_rdev);
                m_selectedNode.minor = (int64_t)minor(st.st_rdev);
                m_selectedNode.valid = true;
            }
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

    // Whether we may pass explicit dmabuf modifiers on import. On a hybrid box (desktop on the AMD
    // iGPU, this XR context on the NVIDIA dGPU per openxr:gpu) NVIDIA's EGL rejects modifier-less
    // imports with EGL_BAD_ATTRIBUTE even for LINEAR — the black-screen root cause (research/17 §4.2,
    // WP-L2). blitBuffer appends the per-plane MODIFIER_LO/HI attribs when this is set.
    if (const char* exts = eglQueryString(m_eglDisplay, EGL_EXTENSIONS))
        m_hasModifiers = std::strstr(exts, "EGL_EXT_image_dma_buf_import_modifiers") != nullptr;
    Log::logger->log(Log::DEBUG, "[OPENXR] EGL context created (dmabuf import modifiers: {})", m_hasModifiers ? "yes" : "no");
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

    // Vertex shader: fullscreen triangle from gl_VertexID, no VBO needed. V is flipped:
    // OpenXR GL swapchain images have a bottom-left origin (quad row 0 renders at the quad's
    // bottom), while Wayland buffers are top-left — sampling 1-y puts the screen's top row in
    // the texture's last row so the quad displays upright (matches rayQuadIntersect's v=0=top).
    const char* vsSrc = R"(
        #version 300 es
        out vec2 vUV;
        void main() {
            vec2 pos;
            if (gl_VertexID == 0)      pos = vec2(-1.0, -1.0);
            else if (gl_VertexID == 1) pos = vec2( 3.0, -1.0);
            else                       pos = vec2(-1.0,  3.0);
            vUV = vec2(pos.x * 0.5 + 0.5, 0.5 - pos.y * 0.5);
            gl_Position = vec4(pos, 0.0, 1.0);
        }
    )";

    // Fragment shader: sample the source texture and derive the output alpha.
    //
    // Default (uBlackKey.x >= 1.0): alpha is pinned to 1.0 — Hyprland monitor buffers are typically
    // XRGB (undefined alpha), and under XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND (passthrough) garbage
    // alpha would punch see-through holes in monitors (doc 01). rgb * 1.0 is a no-op, so the
    // feature-off path is bit-identical to the old `fragColor.a = 1.0`.
    //
    // Luma key / "black-as-alpha" (uBlackKey.x < 1.0, openxr:black_alpha): alpha comes from the
    // pixel's own Rec.709 luma — pure black gets uBlackKey.x, anything at/above the knee
    // (uBlackKey.y) stays fully opaque, smoothstep between. The result is written PREMULTIPLIED
    // (rgb *= a) because the quad carries no XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT and the
    // runtime blends src=ONE, dst=ONE_MINUS_SRC_ALPHA; scaling alpha alone would leave rgb > a and
    // add the content at full brightness over passthrough (additive halo — report 09 §2.1). Keep
    // this curve in sync with OpenXR::xrLumaKeyAlpha (XRMath.hpp), which the gtests pin.
    //
    // Two programs are built from one body: samplerExternalOES for the DMA-BUF path, sampler2D for
    // the CPU-staging path (which otherwise uses a plain glBlitFramebuffer).
    const char* fsBody = R"(
        in vec2 vUV;
        out vec4 fragColor;
        uniform vec2 uBlackKey; // x = alpha for pure black (1.0 = keying off), y = luma knee
        void main() {
            vec3 rgb = texture(uTex, vUV).rgb;
            float a = 1.0;
            if (uBlackKey.x < 1.0) {
                float luma = dot(rgb, vec3(0.2126, 0.7152, 0.0722));
                a = mix(uBlackKey.x, 1.0, smoothstep(0.0, max(uBlackKey.y, 0.001), luma));
            }
            fragColor = vec4(rgb * a, a);
        }
    )";
    const std::string fsExtSrc = std::string(R"(
        #version 300 es
        #extension GL_OES_EGL_image_external_essl3 : require
        precision highp float;
        uniform samplerExternalOES uTex;
    )") + fsBody;
    const std::string fs2DSrc = std::string(R"(
        #version 300 es
        precision highp float;
        uniform sampler2D uTex;
    )") + fsBody;

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

    auto linkProgram = [&](const char* fsSrc) -> GLuint {
        GLuint vs = compileShader(GL_VERTEX_SHADER, vsSrc);
        GLuint fs = compileShader(GL_FRAGMENT_SHADER, fsSrc);
        if (!vs || !fs) {
            glDeleteShader(vs);
            glDeleteShader(fs);
            return 0;
        }
        GLuint prog = glCreateProgram();
        glAttachShader(prog, vs);
        glAttachShader(prog, fs);
        glLinkProgram(prog);
        glDeleteShader(vs);
        glDeleteShader(fs);

        GLint linked = 0;
        glGetProgramiv(prog, GL_LINK_STATUS, &linked);
        if (!linked) {
            char log[512] = {};
            glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
            Log::logger->log(Log::ERR, "[OPENXR] shader link error: {}", log);
            glDeleteProgram(prog);
            return 0;
        }
        return prog;
    };

    m_blitProg = linkProgram(fsExtSrc.c_str());
    if (!m_blitProg)
        return false;

    // The sampler2D twin is only used by the CPU-staging fallback WITH luma keying on. A failure here
    // is not fatal — that path then keeps its plain glBlitFramebuffer (opaque) behavior.
    m_blitProg2D = linkProgram(fs2DSrc.c_str());
    if (!m_blitProg2D)
        Log::logger->log(Log::WARN, "[OPENXR] sampler2D blit program failed to build — the CPU fallback path will ignore openxr:black_alpha");

    // Uniform per-monitor alpha (doc 05 §xrrule): a flat-color program used ONLY as a multiply pass
    // over the finished swapchain image (see fadeTex). Textureless — the fade is pure blend state.
    const char* fsFadeSrc = R"(
        #version 300 es
        precision highp float;
        out vec4 fragColor;
        uniform float uAlpha;
        void main() { fragColor = vec4(uAlpha, uAlpha, uAlpha, uAlpha); }
    )";
    m_fadeProg            = linkProgram(fsFadeSrc);
    if (!m_fadeProg)
        Log::logger->log(Log::WARN, "[OPENXR] fade program failed to build — per-monitor `alpha` effects will be ignored (monitors stay opaque)");

    glGenVertexArrays(1, &m_blitVAO);

    Log::logger->log(Log::DEBUG, "[OPENXR] blit GL resources initialized");
    return true;
}

void CXRGraphics::blitBuffer(const SP<Aquamarine::IBuffer>& buf, CXRMonitorLayer& layer, XR_GLuint dstTex, float blackAlpha, float knee) {
    const int dstW = (int)layer.m_swapchainSize.x; // FULL swapchain (content + chrome margins)
    const int dstH = (int)layer.m_swapchainSize.y;

    // Luma key ("black-as-alpha", openxr:black_alpha). Already gated on the blend mode + clamped on
    // the main thread — here it is just two numbers. Only the CONTENT is keyed; the chrome margins are
    // drawn separately by drawChrome and stay exactly as configured (a ghosted monitor must keep a
    // grabbable bar). blackAlpha >= 1 short-circuits every path back to the old opaque behavior.
    const bool  keyed   = OpenXR::xrBlackKeyActive(blackAlpha);
    const float keyBA   = std::clamp(blackAlpha, 0.f, 1.f);
    const float keyKnee = std::max(knee, OpenXR::XR_BLACK_ALPHA_KNEE_MIN);

    // Chrome margins (WP-G1): the desktop content blits into the INNER content rect; the surrounding
    // margin is left fully transparent (alpha 0) so it never covers anything under XR passthrough.
    // m_contentSize/m_contentOffsetPx are px within the full swapchain (top-left origin); the GL
    // swapchain image has a BOTTOM-left origin (the shader / blit already flip v), so the content
    // rect's GL-space bottom edge is dstH - top - height. Content-less layers (no chrome computed
    // yet) fall back to the full swapchain.
    //
    // WP X4: on a depth-packed monitor the presented buffer holds TWO panes and the swapchain holds
    // two INDEPENDENTLY MARGINED panes, so the two destinations are not contiguous — a pane's right
    // margin and its neighbour's left margin sit between them. That is the whole reason the draws
    // below run in a loop: one blit cannot land a contiguous source into two separated rects.
    // PANES == 1 for every ordinary monitor and the loop body is then byte-for-byte the shipped path.
    const int PANES = std::clamp(layer.m_paneGeom.panes, 1, 2);

    OpenXR::Stereo::SImageRect dest[2]{};
    int                        paneCount = PANES;
    if (layer.m_paneGeom.paneContent.x >= 1 && layer.m_paneGeom.paneContent.y >= 1 && layer.m_paneGeom.paneFull.x >= 1) {
        for (int i = 0; i < PANES; ++i)
            dest[i] = OpenXR::Stereo::paneContentDestGL(layer.m_paneGeom, i);
    } else {
        // Content-less layer (no chrome geometry computed yet): the whole image, exactly as before.
        paneCount = 1;
        dest[0]   = {0, 0, dstW, dstH};
    }

    // Place the WHOLE source across `paneCount` pane-widths and scissor to pane i, so pane i of the
    // source lands exactly on dest[i]. Used by both shader paths (a fullscreen triangle cannot take
    // a source sub-rect, but a viewport wider than the scissor is the same thing).
    auto viewportForPane = [&](int i) {
        glEnable(GL_SCISSOR_TEST);
        glScissor(dest[i].x, dest[i].y, (GLsizei)dest[i].w, (GLsizei)dest[i].h);
        glViewport(dest[i].x - i * dest[i].w, dest[i].y, (GLsizei)(dest[i].w * paneCount), (GLsizei)dest[i].h);
    };

    // Clear the WHOLE swapchain image to premultiplied-transparent (0,0,0,0). Because the quad is
    // submitted premultiplied (no XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT), bilinear
    // sampling at the content/margin seam blends content(rgb,1) with (0,0,0,0) staying valid
    // premultiplied — no dark halo. Content pixels below then overwrite the inner rect at alpha 1.
    auto clearMargin = [&]() {
        GLuint fbo = 0;
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dstTex, 0);
        glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearColor(0.f, 0.f, 0.f, 0.f);
        glClear(GL_COLOR_BUFFER_BIT);
        glDeleteFramebuffers(1, &fbo);
    };

    // --- 1. DMA-BUF path (primary) ---
    auto dmab = buf->dmabuf();
    if (dmab.success && s_eglCreateImage && s_eglDestroyImage && s_glImageTarget2D) {
        // Destroy the layer's previous EGLImage first (per-layer so removal teardown is
        // self-contained).
        if (layer.m_lastEGLImg != nullptr) {
            s_eglDestroyImage(m_eglDisplay, layer.m_lastEGLImg);
            layer.m_lastEGLImg = nullptr;
        }

        // Build the import attrib list via the pure helper (gtested). It appends explicit per-plane
        // MODIFIER_LO/HI when the display advertises EGL_EXT_image_dma_buf_import_modifiers AND the
        // buffer carries a non-INVALID modifier — the cross-GPU black-screen fix (research/17 §4.2,
        // WP-L2). When the ext is absent it emits the legacy modifier-less list (no vendor regressed).
        std::vector<OpenXR::SDmabufPlaneImport> planes;
        planes.reserve(dmab.planes);
        for (int p = 0; p < dmab.planes && p < 4; p++)
            planes.push_back({dmab.fds[p], dmab.offsets[p], dmab.strides[p]});
        std::vector<EGLint> attribs = OpenXR::buildDmabufImportAttribs((int)buf->size.x, (int)buf->size.y, dmab.format, planes, dmab.modifier, m_hasModifiers);

        layer.m_lastEGLImg = s_eglCreateImage(m_eglDisplay, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attribs.data());
        if (layer.m_lastEGLImg != nullptr) {
            layer.m_contentPath.store(OpenXR::XR_CONTENT_DMABUF, std::memory_order_relaxed);
            layer.m_importFailLogged = false; // reset so a later failure logs afresh
            clearMargin(); // transparent margin first

            glBindTexture(GL_TEXTURE_EXTERNAL_OES, m_extTex);
            s_glImageTarget2D(GL_TEXTURE_EXTERNAL_OES, (GLeglImageOES)layer.m_lastEGLImg);

            GLuint fbo = 0;
            glGenFramebuffers(1, &fbo);
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dstTex, 0);
            // Draw the content into the INNER content rect(s) only (the viewport confines the
            // fullscreen triangle); the shader pins content alpha to 1.0 (or luma-keys it when
            // openxr:black_alpha < 1). Margins stay transparent.
            glUseProgram(m_blitProg);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_EXTERNAL_OES, m_extTex);
            glUniform1i(glGetUniformLocation(m_blitProg, "uTex"), 0);
            glUniform2f(glGetUniformLocation(m_blitProg, "uBlackKey"), keyBA, keyKnee);
            glBindVertexArray(m_blitVAO);
            for (int i = 0; i < paneCount; ++i) {
                viewportForPane(i);
                glDrawArrays(GL_TRIANGLES, 0, 3);
            }
            glBindVertexArray(0);
            glDisable(GL_SCISSOR_TEST);
            glDeleteFramebuffers(1, &fbo);
            return;
        }
        // Import failed. Log ONCE per (fourcc, modifier, EGL-error) signature — a cross-GPU black
        // session used to emit ~30 of these per second (43k in one 23-min run, research/17 §4.1). The
        // one line names everything needed to diagnose it: format, modifier, plane count, whether we
        // even passed modifiers, and the EGL error (WP-L2 mini-L6 observability).
        const unsigned eglErr = (unsigned)eglGetError();
        if (!layer.m_importFailLogged || layer.m_importFailFourcc != dmab.format || layer.m_importFailMod != dmab.modifier || layer.m_importFailEgl != eglErr) {
            layer.m_importFailLogged = true;
            layer.m_importFailFourcc = dmab.format;
            layer.m_importFailMod    = dmab.modifier;
            layer.m_importFailEgl    = eglErr;
            Log::logger->log(Log::WARN, "[OPENXR] dmabuf import failed for XR monitor {}: fourcc 0x{:x} modifier 0x{:x} planes {} (modifiers {}) EGL 0x{:x} — falling back (cross-GPU? see openxr:gpu)", layer.m_monitorName, dmab.format, dmab.modifier, dmab.planes, m_hasModifiers ? "passed" : "unavailable", eglErr);
        }
    }

    // --- 2. CPU data-pointer fallback ---
    if (buf->caps() & Aquamarine::BUFFER_CAPABILITY_DATAPTR) {
        auto [data, stride, size] = buf->beginDataPtr(0);
        if (data) {
            // Allocate/realloc the per-layer staging texture at the ACTUAL source mode (the
            // WIP hard-coded 1920x1080 here, corrupting any other mode — doc 01 fix).
            if (layer.m_cpuTex == 0 || layer.m_cpuTexSize != buf->size) {
                if (layer.m_cpuTex == 0)
                    glGenTextures(1, &layer.m_cpuTex);
                glBindTexture(GL_TEXTURE_2D, layer.m_cpuTex);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)buf->size.x, (GLsizei)buf->size.y, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                layer.m_cpuTexSize = buf->size;
            }

            clearMargin(); // transparent margin first

            glBindTexture(GL_TEXTURE_2D, layer.m_cpuTex);
            // DRM_FORMAT_XRGB8888 is BGRX in memory; GL_BGRA_EXT swaps to RGBA correctly.
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)buf->size.x, (GLsizei)buf->size.y, GL_BGRA_EXT, GL_UNSIGNED_BYTE, data);
            buf->endDataPtr();

            if (keyed && m_blitProg2D) {
                // Luma-keying needs a per-pixel alpha, which glBlitFramebuffer cannot produce — draw
                // the staging texture through the sampler2D twin of the dmabuf shader instead. Same
                // geometry (the vertex shader's v flip == the blit's inverted source Y) and same
                // GL_LINEAR minification, so this is pixel-equivalent to the blit path plus the key.
                GLuint fbo = 0;
                glGenFramebuffers(1, &fbo);
                glBindFramebuffer(GL_FRAMEBUFFER, fbo);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dstTex, 0);
                glUseProgram(m_blitProg2D);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, layer.m_cpuTex);
                glUniform1i(glGetUniformLocation(m_blitProg2D, "uTex"), 0);
                glUniform2f(glGetUniformLocation(m_blitProg2D, "uBlackKey"), keyBA, keyKnee);
                glBindVertexArray(m_blitVAO);
                for (int i = 0; i < paneCount; ++i) {
                    viewportForPane(i);
                    glDrawArrays(GL_TRIANGLES, 0, 3);
                }
                glBindVertexArray(0);
                glDisable(GL_SCISSOR_TEST);
                glDeleteFramebuffers(1, &fbo);
                layer.m_contentPath.store(OpenXR::XR_CONTENT_CPU, std::memory_order_relaxed);
                return;
            }

            GLuint srcFBO = 0, dstFBO = 0;
            glGenFramebuffers(1, &srcFBO);
            glGenFramebuffers(1, &dstFBO);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, srcFBO);
            glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, layer.m_cpuTex, 0);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dstFBO);
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dstTex, 0);
            // Source Y inverted: same top-left -> bottom-left origin flip as the dmabuf shader.
            // Dst is the INNER content rect(s) (transparent margins already cleared around them).
            // This path CAN take a source sub-rect, so each pane reads its own half directly.
            const GLint SRCW = (GLint)buf->size.x / paneCount;
            for (int i = 0; i < paneCount; ++i)
                glBlitFramebuffer(SRCW * i, (GLint)buf->size.y, SRCW * (i + 1), 0, dest[i].x, dest[i].y, dest[i].x + dest[i].w, dest[i].y + dest[i].h, GL_COLOR_BUFFER_BIT,
                                  GL_LINEAR);
            // Force dst alpha opaque within the CONTENT rects only (same reason as the dmabuf
            // shader's fragColor.a = 1.0): the XRGB source has undefined alpha which would punch
            // holes under ALPHA_BLEND. Scissor so the transparent margins keep alpha 0.
            glEnable(GL_SCISSOR_TEST);
            glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
            glClearColor(0.f, 0.f, 0.f, 1.f);
            for (int i = 0; i < paneCount; ++i) {
                glScissor(dest[i].x, dest[i].y, (GLsizei)dest[i].w, (GLsizei)dest[i].h);
                glClear(GL_COLOR_BUFFER_BIT);
            }
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glDisable(GL_SCISSOR_TEST);
            glDeleteFramebuffers(1, &srcFBO);
            glDeleteFramebuffers(1, &dstFBO);
            layer.m_contentPath.store(OpenXR::XR_CONTENT_CPU, std::memory_order_relaxed);
            return;
        }
    }

    // --- 3. Clear fallback (black in production; WIP used cyan as a debug sentinel) ---
    // Transparent margin, opaque-black content rect (both blit paths failed). `hyprctl openxr status`
    // now reports contentPath "black" so this silent-black-quad state is diagnosable in one command.
    // Under the luma key this flat black is exactly the color the key is meant to dissolve, so it
    // honors it too (premultiplied: rgb is 0, so only alpha moves).
    layer.m_contentPath.store(OpenXR::XR_CONTENT_BLACK, std::memory_order_relaxed);
    clearMargin();
    {
        float cr = 0.f, cg = 0.f, cb = 0.f, ca = 1.f;
        OpenXR::xrLumaKeyPremultiplied(0.f, 0.f, 0.f, keyBA, keyKnee, cr, cg, cb, ca);
        GLuint fbo = 0;
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dstTex, 0);
        glEnable(GL_SCISSOR_TEST);
        glClearColor(cr, cg, cb, ca);
        for (int i = 0; i < paneCount; ++i) {
            glScissor(dest[i].x, dest[i].y, (GLsizei)dest[i].w, (GLsizei)dest[i].h);
            glClear(GL_COLOR_BUFFER_BIT);
        }
        glDisable(GL_SCISSOR_TEST);
        glDeleteFramebuffers(1, &fbo);
    }
}

void CXRGraphics::clearTex(XR_GLuint dstTex, const Vector2D& size, float r, float g, float b, float blackAlpha, float knee) {
    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dstTex, 0);
    glViewport(0, 0, (GLsizei)size.x, (GLsizei)size.y);
    // Alpha pinned to 1.0 by default: a cleared quad (no content / clear fallback) must be fully
    // opaque so it does not become see-through under XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND
    // passthrough (doc 01). With the luma key on (openxr:black_alpha < 1) the SAME rule the content
    // gets applies to the flat fill instead — a black placeholder quad dissolves like a black
    // desktop would — and the fill is written premultiplied so the composite stays correct.
    float cr = r, cg = g, cb = b, ca = 1.f;
    OpenXR::xrLumaKeyPremultiplied(r, g, b, blackAlpha, knee, cr, cg, cb, ca);
    glClearColor(cr, cg, cb, ca);
    glClear(GL_COLOR_BUFFER_BIT);
    glDeleteFramebuffers(1, &fbo);
}

void CXRGraphics::fadeTex(XR_GLuint dstTex, const Vector2D& size, float alpha) {
    // Uniform per-monitor alpha (doc 05 §xrrule / report 09 §2.1 + §3.3). Runs LAST, over the fully
    // composed image (content + chrome + cursors), so a ghosted monitor ghosts ENTIRELY — a solid
    // move-bar under a near-invisible screen looks broken.
    //
    // The image is already valid premultiplied — content is (rgb*a, a), the margin is (0,0,0,0), the
    // chrome fills are (rgb*ea, ea). Multiplying ALL FOUR channels by the same f keeps it valid
    // premultiplied and is exactly the crossfade the runtime's src=ONE, dst=ONE_MINUS_SRC_ALPHA
    // blend then resolves to `content*f + background*(1-f)`. Scaling alpha alone would leave rgb > a
    // and add the content at full brightness (the additive-halo bug).
    //
    // Implemented as blend state, not a texture read: draw a flat (f,f,f,f) fullscreen triangle with
    // glBlendFunc(GL_ZERO, GL_SRC_COLOR), i.e. dst = dst * src — one op, no sampling, no copy.
    if (!m_fadeProg || alpha >= 1.F || size.x < 1 || size.y < 1)
        return;

    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dstTex, 0);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glViewport(0, 0, (GLsizei)size.x, (GLsizei)size.y);

    glEnable(GL_BLEND);
    glBlendFunc(GL_ZERO, GL_SRC_COLOR);
    glUseProgram(m_fadeProg);
    glUniform1f(glGetUniformLocation(m_fadeProg, "uAlpha"), std::clamp(alpha, 0.f, 1.f));
    glBindVertexArray(m_blitVAO);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    glDisable(GL_BLEND); // every other pass here writes unblended (clear/scissor fills, blits)

    glDeleteFramebuffers(1, &fbo);
}

// ---- WP-G2: chrome snapshot + move-bar/corner-handle draw pass ----

void CXRGraphics::snapshotSwapchain(CXRMonitorLayer& layer, XR_GLuint srcTex) {
    const Vector2D size = layer.m_swapchainSize;
    if (size.x < 1 || size.y < 1)
        return;

    // (Re)allocate the persistent snapshot at the full swapchain size.
    if (layer.m_contentTex == 0 || layer.m_contentTexSize != size) {
        if (layer.m_contentTex == 0)
            glGenTextures(1, &layer.m_contentTex);
        glBindTexture(GL_TEXTURE_2D, layer.m_contentTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)size.x, (GLsizei)size.y, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        layer.m_contentTexSize = size;
    }

    // Copy srcTex (the swapchain image we just wrote content into) -> snapshot, 1:1 (no flip; both
    // share the swapchain's bottom-left origin). Preserves the content-alpha-1 / margin-alpha-0 layout.
    GLuint srcFBO = 0, dstFBO = 0;
    glGenFramebuffers(1, &srcFBO);
    glGenFramebuffers(1, &dstFBO);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, srcFBO);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, srcTex, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dstFBO);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, layer.m_contentTex, 0);
    glDisable(GL_SCISSOR_TEST);
    glBlitFramebuffer(0, 0, (GLint)size.x, (GLint)size.y, 0, 0, (GLint)size.x, (GLint)size.y, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glDeleteFramebuffers(1, &srcFBO);
    glDeleteFramebuffers(1, &dstFBO);
}

bool CXRGraphics::restoreSnapshot(CXRMonitorLayer& layer, XR_GLuint dstTex) {
    const Vector2D size = layer.m_swapchainSize;
    if (layer.m_contentTex == 0 || layer.m_contentTexSize != size || size.x < 1 || size.y < 1)
        return false;

    GLuint srcFBO = 0, dstFBO = 0;
    glGenFramebuffers(1, &srcFBO);
    glGenFramebuffers(1, &dstFBO);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, srcFBO);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, layer.m_contentTex, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dstFBO);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dstTex, 0);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBlitFramebuffer(0, 0, (GLint)size.x, (GLint)size.y, 0, 0, (GLint)size.x, (GLint)size.y, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glDeleteFramebuffers(1, &srcFBO);
    glDeleteFramebuffers(1, &dstFBO);
    return true;
}

void CXRGraphics::drawChrome(CXRMonitorLayer& layer, XR_GLuint dstTex, float alpha, uint8_t hoverRegion, bool grabbed, int pane) {
    const OpenXR::SXRChromeGeometry& g = layer.m_chrome;
    if (alpha <= 0.f || !g.hasChrome())
        return;

    // WP X4: the chrome fractions describe ONE PANE, which on a mono monitor is the whole swapchain
    // and on a depth-packed one is its half. Drawing into the pane's sub-rect is the entire change —
    // the geometry, the hit classifier and the grab machine all keep working in per-pane uv, because
    // that is the space the submitted quad already lives in (each eye's quad IS one margined pane).
    const auto PR = OpenXR::Stereo::paneFullRect(layer.m_paneGeom, pane);
    const int  PX = layer.m_paneGeom.paneFull.x >= 1 ? PR.x : 0;
    const int  W  = layer.m_paneGeom.paneFull.x >= 1 ? PR.w : (int)layer.m_swapchainSize.x;
    const int  H  = layer.m_paneGeom.paneFull.y >= 1 ? PR.h : (int)layer.m_swapchainSize.y;
    if (W <= 0 || H <= 0)
        return;

    // Chrome colors (ARGB ints, hot-reloadable per-frame cached reads — benign frame-thread race,
    // same tolerance as the other openxr:* config reads in this path).
    static auto PIDLE  = CConfigValue<Config::INTEGER>("openxr:chrome_col_idle");
    static auto PHOVER = CConfigValue<Config::INTEGER>("openxr:chrome_col_hover");
    static auto PGRAB  = CConfigValue<Config::INTEGER>("openxr:chrome_col_grab");

    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dstTex, 0);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glEnable(GL_SCISSOR_TEST);

    // Fill a uv rect (top-left origin, v down) with a PREMULTIPLIED chrome color scaled by the fade
    // alpha. The swapchain image has a bottom-left origin, so flip v -> GL y. glClear on a scissor
    // rect is a hard overwrite (plain filled rect — v1 shape; L-brackets deferred), which is exactly
    // what we want in the transparent margin. rgb is stored premultiplied by (colorAlpha*fade) so the
    // TEXTURE_SOURCE_ALPHA composite is correct with no edge halo.
    auto fillRect = [&](float u0, float v0, float u1, float v1, uint32_t argb) {
        if (u1 <= u0 || v1 <= v0)
            return;
        const float ca = ((argb >> 24) & 0xff) / 255.f;
        const float cr = ((argb >> 16) & 0xff) / 255.f;
        const float cg = ((argb >> 8) & 0xff) / 255.f;
        const float cb = (argb & 0xff) / 255.f;
        const float ea = ca * alpha; // effective (fade-scaled) alpha
        const int   x0 = PX + (int)std::lround(u0 * W);
        const int   x1 = PX + (int)std::lround(u1 * W);
        const int   glY0 = (int)std::lround((1.f - v1) * H); // v1 is the lower (in-screen) edge -> smaller GL y
        const int   glY1 = (int)std::lround((1.f - v0) * H);
        if (x1 <= x0 || glY1 <= glY0)
            return;
        glScissor(x0, glY0, x1 - x0, glY1 - glY0);
        glClearColor(cr * ea, cg * ea, cb * ea, ea); // premultiplied
        glClear(GL_COLOR_BUFFER_BIT);
    };

    // Per-element color: grabbed overrides everything; else the hovered element gets the hover
    // color and the rest stay idle. hoverRegion is the ray's region on THIS quad (bar / a specific
    // corner / body / margin). Body/margin hover still shows the whole chrome (idle) as an invite.
    const uint32_t idle  = (uint32_t)(int64_t)*PIDLE;
    const uint32_t hover = (uint32_t)(int64_t)*PHOVER;
    const uint32_t grab  = (uint32_t)(int64_t)*PGRAB;
    auto colFor = [&](OpenXR::eXRQuadRegion elem) -> uint32_t {
        if (grabbed)
            return grab;
        return ((OpenXR::eXRQuadRegion)hoverRegion == elem) ? hover : idle;
    };

    // Move-bar.
    fillRect(g.barU0, g.barV0, g.barU1, g.barV1, colFor(OpenXR::XR_REGION_BAR));

    // Corner handles: filled squares in the margin just OUTSIDE each content corner (matches
    // OpenXR::classifyQuadHit's corner bands exactly).
    if (g.cornerU > 0.f && g.cornerV > 0.f) {
        fillRect(g.contentU0 - g.cornerU, g.contentV0 - g.cornerV, g.contentU0, g.contentV0, colFor(OpenXR::XR_REGION_CORNER_TL));
        fillRect(g.contentU1, g.contentV0 - g.cornerV, g.contentU1 + g.cornerU, g.contentV0, colFor(OpenXR::XR_REGION_CORNER_TR));
        fillRect(g.contentU0 - g.cornerU, g.contentV1, g.contentU0, g.contentV1 + g.cornerV, colFor(OpenXR::XR_REGION_CORNER_BL));
        fillRect(g.contentU1, g.contentV1, g.contentU1 + g.cornerU, g.contentV1 + g.cornerV, colFor(OpenXR::XR_REGION_CORNER_BR));
    }

    glDisable(GL_SCISSOR_TEST);
    glDeleteFramebuffers(1, &fbo);
}

void CXRGraphics::drawCursor(CXRMonitorLayer& layer, XR_GLuint dstTex, uint32_t packedL, uint32_t packedR, uint32_t packedGaze, int pane, float disparityContentFrac) {
    static auto PCURSOR   = CConfigValue<Hyprlang::INT>("openxr:cursor");
    static auto PGAZECUR  = CConfigValue<Hyprlang::INT>("openxr:gaze_cursor");
    const bool  handsOn   = *PCURSOR != 0;
    const bool  gazeOn    = *PGAZECUR != 0 && packedGaze != 0;
    if (!handsOn && !gazeOn)
        return;

    // WP X3/X4: draw into ONE PANE's sub-rect (identity on a mono monitor — the pane is the whole
    // image), shifted by this pane's share of the cursor's depth disparity (§5.4).
    const auto PR = OpenXR::Stereo::paneFullRect(layer.m_paneGeom, pane);
    const int  PX = layer.m_paneGeom.paneFull.x >= 1 ? PR.x : 0;
    const int  W  = layer.m_paneGeom.paneFull.x >= 1 ? PR.w : (int)layer.m_swapchainSize.x;
    const int  H  = layer.m_paneGeom.paneFull.y >= 1 ? PR.h : (int)layer.m_swapchainSize.y;
    if (W <= 0 || H <= 0)
        return;

    // The disparity arrives as a fraction of the pane's CONTENT width (that is the space the depth
    // ladder computes in); the cursor's uv is over the pane's FULL quad. contentFracW is exactly the
    // ratio between the two, and it is the same number the chrome geometry is built from.
    const float DU = disparityContentFrac * (layer.m_chrome.contentFracW() > 0.f ? layer.m_chrome.contentFracW() : 1.f);

    static auto PSIZE  = CConfigValue<Hyprlang::FLOAT>("openxr:cursor_size");
    static auto PPRESS = CConfigValue<Hyprlang::FLOAT>("openxr:cursor_press_scale");
    static auto PTINT  = CConfigValue<Hyprlang::INT>("openxr:cursor_per_hand_tint");
    static auto PCIDLE = CConfigValue<Config::INTEGER>("openxr:cursor_col_idle");
    static auto PCGRAB = CConfigValue<Config::INTEGER>("openxr:cursor_col_grabbable");
    static auto PCPRES = CConfigValue<Config::INTEGER>("openxr:cursor_col_press");
    static auto PCGRB2 = CConfigValue<Config::INTEGER>("openxr:cursor_col_grab");
    static auto PGAZECOL = CConfigValue<Config::INTEGER>("openxr:gaze_cursor_col");

    const float    diamM      = (float)*PSIZE;
    const float    pressScale = (float)*PPRESS;
    const bool     tint       = *PTINT != 0;
    const uint32_t colIdle    = (uint32_t)(int64_t)*PCIDLE;
    const uint32_t colGrab    = (uint32_t)(int64_t)*PCGRAB;
    const uint32_t colPress   = (uint32_t)(int64_t)*PCPRES;
    const uint32_t colGrb2    = (uint32_t)(int64_t)*PCGRB2;

    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dstTex, 0);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glEnable(GL_SCISSOR_TEST);

    // Draw one dot from a packed cursor word with an explicit ARGB color. Opaque (alpha 1) so it
    // never punches an alpha hole under passthrough; rgb dimmed by the color's alpha so a low-alpha
    // config color reads dimmer (same rule as clearTex). The press-grow scale only applies to hands.
    auto drawDot = [&](uint32_t packed, uint32_t argb, bool allowPressGrow) {
        bool                   present = false;
        OpenXR::eXRCursorState st      = OpenXR::XR_CURSOR_HIDDEN;
        float                  u = 0.f, v = 0.f;
        OpenXR::xrUnpackCursor(packed, present, st, u, v);
        if (!present || st == OpenXR::XR_CURSOR_HIDDEN)
            return;

        float ru = 0.f, rv = 0.f;
        OpenXR::xrCursorRadiusUV(diamM, layer.m_quadWMeters, layer.m_quadHMeters, allowPressGrow && st == OpenXR::XR_CURSOR_PRESS, pressScale, ru, rv);
        if (ru <= 0.f || rv <= 0.f)
            return;

        const float ca = ((argb >> 24) & 0xff) / 255.f;
        const float cr = ((argb >> 16) & 0xff) / 255.f * ca;
        const float cg = ((argb >> 8) & 0xff) / 255.f * ca;
        const float cb = (argb & 0xff) / 255.f * ca;

        // uv rect (top-left origin, v down) clamped to the quad; flip v -> GL y (bottom-left origin).
        // DU is this pane's depth shift — the dot floats at the depth of what it is over.
        const float uc  = u + DU;
        const float fu0 = std::clamp(uc - ru, 0.f, 1.f), fu1 = std::clamp(uc + ru, 0.f, 1.f);
        const float fv0 = std::clamp(v - rv, 0.f, 1.f), fv1 = std::clamp(v + rv, 0.f, 1.f);
        const int   x0   = PX + (int)std::lround(fu0 * W);
        const int   x1   = PX + (int)std::lround(fu1 * W);
        const int   glY0 = (int)std::lround((1.f - fv1) * H);
        const int   glY1 = (int)std::lround((1.f - fv0) * H);
        if (x1 <= x0 || glY1 <= glY0)
            return;
        glScissor(x0, glY0, x1 - x0, glY1 - glY0);
        glClearColor(cr, cg, cb, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    };

    if (handsOn) {
        const uint32_t packed[2] = {packedL, packedR};
        for (int hand = 0; hand < 2; ++hand) {
            bool                   present = false;
            OpenXR::eXRCursorState st      = OpenXR::XR_CURSOR_HIDDEN;
            float                  u = 0.f, v = 0.f;
            OpenXR::xrUnpackCursor(packed[hand], present, st, u, v);
            if (!present || st == OpenXR::XR_CURSOR_HIDDEN)
                continue;
            const uint32_t argb = OpenXR::xrCursorTint(OpenXR::xrCursorColorFor(st, colIdle, colGrab, colPress, colGrb2), hand, tint);
            drawDot(packed[hand], argb, /*allowPressGrow=*/true);
        }
    }
    // Gaze cursor (research/16 §3.3): a single fixed color, drawn last so it reads on top.
    if (gazeOn)
        drawDot(packedGaze, (uint32_t)(int64_t)*PGAZECOL, /*allowPressGrow=*/false);

    glDisable(GL_SCISSOR_TEST);
    glDeleteFramebuffers(1, &fbo);
}

void CXRGraphics::destroyLayerGL(XR_EGLImageKHR img, XR_GLuint cpuTex, XR_GLuint contentTex) {
    if (img != nullptr && s_eglDestroyImage)
        s_eglDestroyImage(m_eglDisplay, img);
    if (cpuTex) {
        GLuint t = cpuTex;
        glDeleteTextures(1, &t);
    }
    if (contentTex) {
        GLuint t = contentTex;
        glDeleteTextures(1, &t);
    }
}

void CXRGraphics::destroyGL() {
    if (m_xrContext == EGL_NO_CONTEXT)
        return;

    // GL objects must be deleted with the XR context current. Go through CScopedGLContext so this
    // teardown burst (main-thread stop()/abortStart(), or the dtor) SAVES and RESTORES the caller's
    // EGL binding instead of dropping to EGL_NO_CONTEXT (WP-L1, doc 17 §5): the stop path is
    // precisely where the old unconditional unbind left Hyprland's renderer running against an
    // unbound context on the same GPU — a candidate for the on-toggle host-monitor corruption.
    CScopedGLContext ctx(*this);
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
    if (m_blitProg2D) {
        glDeleteProgram(m_blitProg2D);
        m_blitProg2D = 0;
    }
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
