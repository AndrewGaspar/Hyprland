#include "XRDmabufImport.hpp"

// Compiled unconditionally (no OpenXR headers, no HAVE_OPENXR guard) — see the header.

#include <drm_fourcc.h> // DRM_FORMAT_MOD_INVALID

const char* OpenXR::xrContentPathName(uint8_t p) {
    switch (p) {
        case XR_CONTENT_DMABUF: return "dmabuf";
        case XR_CONTENT_CPU: return "cpu";
        case XR_CONTENT_BLACK: return "black";
        default: return "none";
    }
}

bool OpenXR::shouldStashPresentedBuffer(bool forceLinear, bool bufIsDmabuf, uint64_t modifier) {
    // Only guard force-linear dmabuf outputs — everything else (same-GPU, CPU/shm buffers) is fine.
    if (!forceLinear || !bufIsDmabuf)
        return true;
    // A known non-linear tiling under force-linear is a stale pre-reconfigure buffer the foreign XR
    // GPU cannot import; skip it and wait for the reconfigured LINEAR buffer. LINEAR (0) and INVALID
    // ("implicit", could be linear) pass through.
    return modifier == DRM_FORMAT_MOD_LINEAR || modifier == DRM_FORMAT_MOD_INVALID;
}

std::vector<EGLint> OpenXR::buildDmabufImportAttribs(int width, int height, uint32_t fourcc, const std::vector<SDmabufPlaneImport>& planes, uint64_t modifier, bool withModifiers) {
    // Per-plane attrib-token tables, indexed by plane (0..3). Same layout as
    // src/render/OpenGL.cpp:626-636 (the canonical Hyprland dmabuf import).
    static const EGLint FD[4]     = {EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE1_FD_EXT, EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE3_FD_EXT};
    static const EGLint OFFSET[4] = {EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT, EGL_DMA_BUF_PLANE2_OFFSET_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT};
    static const EGLint PITCH[4]  = {EGL_DMA_BUF_PLANE0_PITCH_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT, EGL_DMA_BUF_PLANE2_PITCH_EXT, EGL_DMA_BUF_PLANE3_PITCH_EXT};
    static const EGLint MODLO[4]  = {EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT};
    static const EGLint MODHI[4]  = {EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT};

    int nPlanes = (int)planes.size();
    if (nPlanes < 1)
        nPlanes = 1;
    if (nPlanes > 4)
        nPlanes = 4;

    // Emit modifiers only when the display supports them AND the buffer has an explicit modifier.
    // DRM_FORMAT_MOD_INVALID means "implicit" — appending it as an explicit attrib is itself a
    // BAD_ATTRIBUTE on strict drivers, so omit the pair entirely (stock behavior).
    const bool emitMods = withModifiers && modifier != DRM_FORMAT_MOD_INVALID;

    std::vector<EGLint> attribs;
    attribs.reserve(7 + nPlanes * (emitMods ? 10 : 6) + 1);

    attribs.push_back(EGL_WIDTH);
    attribs.push_back((EGLint)width);
    attribs.push_back(EGL_HEIGHT);
    attribs.push_back((EGLint)height);
    attribs.push_back(EGL_LINUX_DRM_FOURCC_EXT);
    attribs.push_back((EGLint)fourcc);

    for (int p = 0; p < nPlanes; ++p) {
        const auto& pl = planes[(size_t)p];
        attribs.push_back(FD[p]);
        attribs.push_back((EGLint)pl.fd);
        attribs.push_back(OFFSET[p]);
        attribs.push_back((EGLint)pl.offset);
        attribs.push_back(PITCH[p]);
        attribs.push_back((EGLint)pl.stride);
        if (emitMods) {
            attribs.push_back(MODLO[p]);
            attribs.push_back((EGLint)(uint32_t)(modifier & 0xFFFFFFFF));
            attribs.push_back(MODHI[p]);
            attribs.push_back((EGLint)(uint32_t)(modifier >> 32));
        }
    }

    attribs.push_back(EGL_NONE);
    return attribs;
}
