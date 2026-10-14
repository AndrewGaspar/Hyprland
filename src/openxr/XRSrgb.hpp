#pragma once

namespace OpenXR {
    // XR monitor swapchains are GL_SRGB8_ALPHA8 (XRSession::chooseSwapchainFormat), so the runtime
    // DECODES their texels sRGB->linear when it samples them (Monado/WiVRn create the Vulkan view
    // with the image's own *_SRGB format). The bytes we write must therefore be sRGB-ENCODED — and
    // the presented desktop buffer already is (XRGB8888 holds sRGB-encoded values, sampled raw).
    //
    // OpenGL ES, however, unconditionally ENCODES linear->sRGB on every write to an sRGB attachment
    // (shader output, glClear, glBlitFramebuffer). Left at that default, the already-encoded desktop
    // was encoded a second time (a mid-grey 128 stored as 188): lifted shadows, pale/washed-out
    // colors versus the same desktop on a plain 2D output.
    //
    // GL_EXT_sRGB_write_control turns that write-side encode off, making every write to the sRGB
    // swapchain a raw byte copy (and a blit from it a raw read, so the chrome snapshot round-trips
    // losslessly). Call with the XR context current; it is per-context state, set once.
    // Returns true when raw writes are in effect.
    bool disableSrgbWriteEncoding();
}
