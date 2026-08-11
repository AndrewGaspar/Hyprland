#pragma once
#ifdef HAVE_OPENXR

#include <openxr/openxr.h> // XrInstance, XrSystemId
#include <atomic>
#include <cstdint>
#include <string>

namespace OpenXR {
    // The DRM render node the active OpenXR runtime COMPOSITES on. This exists so
    // COpenXRManager::start() can fail closed when openxr:gpu points at a DIFFERENT physical GPU
    // than the runtime — that mismatch makes the runtime import cross-GPU buffers at
    // xrCreateSwapchain and HARD-CRASHES the graphics driver (radeonsi driUnbindContext, an
    // uncatchable SEGV that takes the whole compositor down — coredumps 8986/39318).
    //
    // Two probes answer it; see each function below for which question it really asks. Both are
    // best-effort and side-effect-free, never throw, and never leave XR/EGL/Vulkan state behind.
    // `determined == false` means "could not verify" (runtime lacks the extension, Vulkan/EGL
    // unavailable, driver reports no DRM node, or this build has no Vulkan headers); callers then
    // proceed with a warning rather than blocking a fine setup.
    struct SRuntimeGpu {
        bool        determined = false;
        int64_t     drmMajor   = -1;
        int64_t     drmMinor   = -1;
        std::string deviceName; // GPU name (or node path) when determined, else empty
        std::string note;       // human-readable reason / detail
        std::string probe;      // which query answered, for the log/status line
    };

    // ASK THIS ONE. The compositor's own device, asked as an EGL question:
    // XR_MND_query_egl_device's xrGetSystemEGLDeviceMND returns the EGLDeviceEXT an EGL client is
    // meant to build its context on, which is by construction the device the runtime's compositor
    // renders on — a GL/EGL client wraps the native compositor and imports its swapchain images by
    // an OPAQUE_FD, importable on the exporting device and on no other. That makes it the only
    // query whose answer is the thing this guard needs to know, and it stays correct on a runtime
    // deliberately splitting the compositor GPU from the GPU it suggests to Vulkan applications
    // (WiVRn's cross-GPU `client-gpu` mode: game on the dGPU, composite+encode on the iGPU).
    //
    // The instance must have been created with XR_MND_query_egl_device enabled (the entry point is
    // otherwise XR_ERROR_FUNCTION_UNSUPPORTED). Safe on the main thread: the runtime answers it
    // in-process from data it already has (it enumerates EGL devices through the getProcAddress we
    // hand it and matches by UUID) — no IPC round-trip, no Vulkan, nothing that can deadlock.
    SRuntimeGpu probeRuntimeEglDevice(XrInstance instance, XrSystemId systemId);

    // FALLBACK ONLY. xrGetVulkanGraphicsDevice2KHR — "which GPU should a VULKAN APPLICATION
    // render on". On a stock single-GPU runtime that is also the compositor's GPU, which is why
    // this was the original guard and why it is still the right answer against runtimes that do
    // not split the two. On a runtime that DOES split them it answers the client GPU, which is not
    // what this guard asks — so only reach for it when the EGL query above is unavailable.
    //
    // Must only be called when the instance was created with XR_KHR_vulkan_enable2 enabled (its
    // entry points are otherwise XR_ERROR_FUNCTION_UNSUPPORTED). MUST run on a throwaway thread,
    // NEVER on the compositor main thread: it calls vkCreateInstance, which can deadlock
    // indefinitely against the runtime's own in-process Vulkan usage. The caller waits with a
    // timeout and, on timeout, sets `*abandon` and moves on; the probe then bails before any
    // XrInstance call so a late unblock cannot touch a torn-down instance (its own Vulkan objects
    // are always cleaned up). Pass nullptr only if you can guarantee the instance outlives the call.
    SRuntimeGpu probeRuntimeRenderNode(XrInstance instance, XrSystemId systemId, const std::atomic<bool>* abandon);
}

#endif // HAVE_OPENXR
