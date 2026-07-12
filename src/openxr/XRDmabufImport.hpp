#pragma once

// Deliberately compiles unconditionally (no OpenXR headers, no HAVE_OPENXR guard) so the EGL
// dmabuf-import attrib construction is a pure, unit-testable function. It needs only EGL + libdrm
// headers (both always-present build deps), NOT the OpenXR runtime — mirroring the XRMonitorConfig
// pattern (pickBlendMode). See docs/openxr/research/17-late-runtime-lifecycle.md §4.2/§4.5 (WP-L2):
// NVIDIA's EGL rejects modifier-less dmabuf imports with EGL_BAD_ATTRIBUTE even for LINEAR buffers,
// so the cross-GPU (desktop on AMD, XR context on NVIDIA) blit must pass explicit per-plane
// EGL_DMA_BUF_PLANE<i>_MODIFIER_LO/HI_EXT — exactly as src/render/OpenGL.cpp:626-652 does.

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <cstdint>
#include <vector>

namespace OpenXR {
    // Which blit path last produced a layer's swapchain content, for `hyprctl openxr status`
    // (WP-L2 mini-L6 observability). A DMABUF value means the zero-copy import worked; BLACK means
    // both the dmabuf import and the CPU staging fallback failed and the quad is an opaque-black
    // clear — the exact cross-GPU black-screen state this WP diagnoses.
    enum eXRContentPath : uint8_t {
        XR_CONTENT_NONE = 0, // no blit landed yet
        XR_CONTENT_DMABUF,   // zero-copy dmabuf EGLImage import
        XR_CONTENT_CPU,      // CPU data-pointer staging upload
        XR_CONTENT_BLACK,    // both paths failed -> opaque black clear
    };
    // "none" | "dmabuf" | "cpu" | "black" (unknown -> "none").
    const char* xrContentPathName(uint8_t p);

    // One dmabuf plane's import parameters (from Aquamarine::SDMABUFAttrs).
    struct SDmabufPlaneImport {
        int      fd     = -1;
        uint32_t offset = 0;
        uint32_t stride = 0;
    };

    // Build the attrib list for eglCreateImageKHR(..., EGL_LINUX_DMA_BUF_EXT, ..., attribs). Always
    // emits EGL_WIDTH/HEIGHT, EGL_LINUX_DRM_FOURCC_EXT, and per-plane FD/OFFSET/PITCH. Appends the
    // per-plane MODIFIER_LO/HI_EXT pair IFF withModifiers is true AND modifier is not
    // DRM_FORMAT_MOD_INVALID (an INVALID modifier means "implicit" — the modifier attribs must be
    // omitted, matching stock Hyprland behavior). Result is EGL_NONE-terminated. planes is clamped
    // to [1, 4]; each plane must have a valid entry in `planes`.
    std::vector<EGLint> buildDmabufImportAttribs(int width, int height, uint32_t fourcc, const std::vector<SDmabufPlaneImport>& planes, uint64_t modifier, bool withModifiers);
}
