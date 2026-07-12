#pragma once
#ifdef HAVE_OPENXR

#include <openxr/openxr.h> // XrInstance, XrSystemId
#include <atomic>
#include <cstdint>
#include <string>

namespace OpenXR {
    // The DRM render node the active OpenXR runtime composites on, probed via
    // XR_KHR_vulkan_enable2 (both Monado and WiVRn advertise it). This exists so
    // COpenXRManager::start() can fail closed when openxr:gpu points at a DIFFERENT physical GPU
    // than the runtime — that mismatch makes the runtime import cross-GPU buffers at
    // xrCreateSwapchain and HARD-CRASHES the graphics driver (radeonsi driUnbindContext, an
    // uncatchable SEGV that takes the whole compositor down — coredumps 8986/39318).
    //
    // Best-effort and side-effect-free: it creates a throwaway VkInstance, reads the runtime's
    // physical device DRM node, and tears everything back down. It never throws and never leaves
    // XR/Vulkan state behind. `determined == false` means "could not verify" (runtime lacks the
    // extension, Vulkan unavailable, driver lacks VK_EXT_physical_device_drm, or this build has no
    // Vulkan headers); callers then proceed with a warning rather than blocking a fine setup.
    struct SRuntimeGpu {
        bool        determined = false;
        int64_t     drmMajor   = -1;
        int64_t     drmMinor   = -1;
        std::string deviceName; // GPU name when determined, else empty
        std::string note;       // human-readable reason / detail
    };

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
