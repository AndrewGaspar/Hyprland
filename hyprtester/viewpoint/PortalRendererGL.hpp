#pragma once

// GPU twin of the CPU raymarcher in PortalRenderer.cpp. The fragment shader is a
// faithful translation of trace()/fallbackPixel(): same geometry, same colors,
// same portal projection, same reticle. It runs in 32-bit floats where the CPU
// reference runs in doubles, so the two agree to within a small per-channel
// tolerance rather than bit-exactly — see docs/openxr/10-view-dependent-surfaces.md
// §13.1. The CPU path therefore remains the only deterministic one.
//
// Two EGL back ends share one renderer:
//   - onSurface() drives a live wl_egl_window (zero-copy dmabuf, the device is
//     whatever Mesa picks from the compositor's dmabuf feedback — never hand-picked);
//   - offscreen() drives EGL_MESA_platform_surfaceless plus an FBO, so --render-gpu,
//     --bench-gpu, and the CPU/GPU comparison run with no compositor at all.

#include "PortalRenderer.hpp"

#include <memory>
#include <string>

struct wl_display;
struct wl_surface;

namespace ViewpointDemo {

    class CPortalRendererGL {
      public:
        CPortalRendererGL();
        ~CPortalRendererGL();

        CPortalRendererGL(const CPortalRendererGL&)            = delete;
        CPortalRendererGL(CPortalRendererGL&&)                 = delete;
        CPortalRendererGL& operator=(const CPortalRendererGL&) = delete;
        CPortalRendererGL& operator=(CPortalRendererGL&&)      = delete;

        // Both factories return nullptr and fill `error` with one human-readable
        // line; no partial state survives a failure. `packedWidth` is the full-SBS
        // width and must be even, exactly as for renderPortalSBS().
        static std::unique_ptr<CPortalRendererGL> offscreen(uint32_t packedWidth, uint32_t height, std::string& error);
        static std::unique_ptr<CPortalRendererGL> onSurface(wl_display* display, wl_surface* surface, uint32_t packedWidth, uint32_t height, std::string& error);

        // Grid-line antialiasing. On (the live default) the shader box-filters the
        // grid over the pixel footprint, which is what removes the shimmer the CPU
        // path has. Off reproduces the CPU's hard threshold so the two images can be
        // compared; it is the only mode the tolerance harness asserts on.
        void setAntialiasGrid(bool enabled);
        bool antialiasGrid() const;

        bool resize(uint32_t packedWidth, uint32_t height);

        // Same acceptance contract as renderPortalSBS(): a rejected scene draws
        // nothing and reports false.
        bool drawPortal(const SPortalSize& portal, const SStereoViews& views);
        bool drawFallback();

        // Offscreen only: copies the drawable into `image` as XRGB8888, top row
        // first, matching the CPU renderer's memory layout.
        bool readback(const SImage& image);

        bool finish();

        // Window only: eglSwapBuffers(). The caller must have already sent the
        // matching viewpoint rendered() request — the swap performs the attach and
        // the commit that the association binds to.
        bool present();

        uint32_t    packedWidth() const;
        uint32_t    height() const;
        std::string description() const;

      private:
        struct SState;
        std::unique_ptr<SState> m_state;
    };

}
