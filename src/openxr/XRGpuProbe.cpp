#include "XRGpuProbe.hpp"
#ifdef HAVE_OPENXR

#include "../debug/log/Logger.hpp"

// Include contract (doc 01): every XR_USE_* macro must be defined before openxr_platform.h, and
// each one's own headers before that. This TU asks the runtime TWO different "which GPU" questions
// and openxr_platform.h has an include guard, so both macros are set up front, once:
//   * XR_USE_PLATFORM_EGL   — for PFN_xrEglGetProcAddressMNDX, which XR_MND_query_egl_device reuses.
//   * XR_USE_GRAPHICS_API_VULKAN — for XrVulkanInstanceCreateInfoKHR etc., only where Vulkan headers
//     were found at configure time (HAVE_XR_VULKAN_PROBE). This TU is the ONLY place Hyprland pulls
//     in Vulkan; libvulkan is dlopen'd at runtime (no hard link dependency) so a box without a
//     Vulkan ICD simply loses the fallback probe.
#define XR_USE_PLATFORM_EGL
#ifdef HAVE_XR_VULKAN_PROBE
#define XR_USE_GRAPHICS_API_VULKAN
#endif

#include <EGL/egl.h>
#include <EGL/eglext.h>
#ifdef HAVE_XR_VULKAN_PROBE
#include <vulkan/vulkan.h>
#endif
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <dlfcn.h>
#include <atomic>
#include <sys/stat.h>
#include <sys/sysmacros.h>

#ifndef EGL_DRM_RENDER_NODE_FILE_EXT
#define EGL_DRM_RENDER_NODE_FILE_EXT 0x3377
#endif
#ifndef EGL_DRM_DEVICE_FILE_EXT
#define EGL_DRM_DEVICE_FILE_EXT 0x3233
#endif
#ifndef EGL_RENDERER_EXT
#define EGL_RENDERER_EXT 0x335F
#endif

namespace OpenXR {

/*
 * XR_MND_query_egl_device — a Monado vendor extension that is not in the OpenXR SDK headers we
 * build against, so its two structs and its entry point are declared here, matching Monado's
 * src/external/openxr_includes/openxr/XR_MND_query_egl_device.h field for field.
 */
static constexpr XrStructureType XR_TYPE_SYSTEM_EGL_DEVICE_GET_INFO_MND_ = (XrStructureType)1000445001;
static constexpr XrStructureType XR_TYPE_SYSTEM_EGL_DEVICE_MND_          = (XrStructureType)1000445002;

struct SXrSystemEGLDeviceGetInfoMND {
    XrStructureType             type;
    const void*                 next;
    XrSystemId                  systemId;
    PFN_xrEglGetProcAddressMNDX getProcAddress;
};

struct SXrSystemEGLDeviceMND {
    XrStructureType type;
    void*           next;
    EGLDeviceEXT    eglDevice;
};

using PFN_xrGetSystemEGLDeviceMND_t = XrResult(XRAPI_PTR*)(XrInstance, const SXrSystemEGLDeviceGetInfoMND*, SXrSystemEGLDeviceMND*);
using PFNEGLQUERYDEVICESTRINGEXTPROC_t = const char* (*)(EGLDeviceEXT, EGLint);

SRuntimeGpu probeRuntimeEglDevice(XrInstance instance, XrSystemId systemId, const std::atomic<bool>* abandon) {
    SRuntimeGpu out;
    out.probe      = "EGL device query (XR_MND_query_egl_device)";
    auto abandoned = [&] { return abandon && abandon->load(std::memory_order_acquire); };

    // The instance must have enabled XR_MND_query_egl_device; the loader/runtime otherwise answers
    // XR_ERROR_FUNCTION_UNSUPPORTED and we fall back to the Vulkan probe.
    PFN_xrGetSystemEGLDeviceMND_t pGetEglDevice = nullptr;
    if (XR_FAILED(xrGetInstanceProcAddr(instance, "xrGetSystemEGLDeviceMND", reinterpret_cast<PFN_xrVoidFunction*>(&pGetEglDevice))) || !pGetEglDevice) {
        out.note = "runtime does not expose xrGetSystemEGLDeviceMND (XR_MND_query_egl_device)";
        return out;
    }

    if (abandoned()) {
        out.note = "probe abandoned (timed out)";
        return out;
    }

    // Answered in-process: the runtime calls back through `getProcAddress` to enumerate OUR EGL
    // devices and hands back the one whose UUID matches its compositor's. No IPC, no Vulkan — but
    // that callback is a full glvnd EGL device enumeration, which touches every installed vendor
    // driver, so this runs on the caller's throwaway thread all the same.
    SXrSystemEGLDeviceGetInfoMND info = {XR_TYPE_SYSTEM_EGL_DEVICE_GET_INFO_MND_, nullptr, systemId, eglGetProcAddress};
    SXrSystemEGLDeviceMND       dev   = {XR_TYPE_SYSTEM_EGL_DEVICE_MND_, nullptr, EGL_NO_DEVICE_EXT};

    const XrResult res = pGetEglDevice(instance, &info, &dev);
    if (XR_FAILED(res) || dev.eglDevice == EGL_NO_DEVICE_EXT) {
        out.note = std::string("xrGetSystemEGLDeviceMND failed (") + std::to_string((int)res) + ")";
        return out;
    }

    auto eglQueryDeviceStringEXT_fn = (PFNEGLQUERYDEVICESTRINGEXTPROC_t)eglGetProcAddress("eglQueryDeviceStringEXT");
    if (!eglQueryDeviceStringEXT_fn) {
        out.note = "EGL_EXT_device_query unavailable (no eglQueryDeviceStringEXT)";
        return out;
    }

    // Prefer the render node, fall back to the primary/card node — the same order (and the same
    // DRM major:minor identity) CXRGraphics::selectDisplay used to pick the XR context's device,
    // so the two are directly comparable.
    //
    // No eglGetError() clearing here any more: the EGL error latch is PER-THREAD, and this function
    // now always runs on a throwaway thread that is about to exit, so a latched EGL_BAD_ATTRIBUTE
    // from a failed query dies with the thread. It never was the main thread's error to inherit —
    // it only looked that way while this ran on the main thread.
    bool        primaryOnly = false;
    const char* nodePath    = eglQueryDeviceStringEXT_fn(dev.eglDevice, EGL_DRM_RENDER_NODE_FILE_EXT);
    if (!nodePath) {
        nodePath    = eglQueryDeviceStringEXT_fn(dev.eglDevice, EGL_DRM_DEVICE_FILE_EXT);
        primaryOnly = nodePath != nullptr;
    }
    if (!nodePath) {
        out.note = "the runtime's EGL device reports no DRM node (EGL_EXT_device_drm unsupported)";
        return out;
    }

    struct stat st = {};
    if (stat(nodePath, &st) != 0) {
        out.note = std::string("could not stat the runtime's DRM node ") + nodePath;
        return out;
    }

    // EGL_EXT_device_query_name is optional; the node path is a perfectly good name without it.
    const char* renderer = eglQueryDeviceStringEXT_fn(dev.eglDevice, EGL_RENDERER_EXT);

    out.determined = true;
    out.drmMajor   = (int64_t)major(st.st_rdev);
    out.drmMinor   = (int64_t)minor(st.st_rdev);
    out.deviceName = renderer ? renderer : nodePath;
    if (primaryOnly)
        out.note = "runtime reported only a primary DRM node";
    return out;
}

}

#ifdef HAVE_XR_VULKAN_PROBE

namespace OpenXR {

SRuntimeGpu probeRuntimeRenderNode(XrInstance instance, XrSystemId systemId, const std::atomic<bool>* abandon) {
    SRuntimeGpu out;
    out.probe = "Vulkan device query (XR_KHR_vulkan_enable2)";
    auto abandoned = [&] { return abandon && abandon->load(std::memory_order_acquire); };

    // Resolve the XR_KHR_vulkan_enable2 entry points. xrGetVulkanGraphicsDevice2KHR is the query we
    // need. xrCreateVulkanInstanceKHR is how the SPEC says a vulkan_enable2 app creates its
    // VkInstance (the runtime merges in the instance extensions it needs to match its physical
    // device). Strategy (see the two attempts below): create the instance through the runtime FIRST
    // — BOTH Monado and WiVRn's xrGetVulkanGraphicsDevice2KHR validate that the instance came from
    // xrCreateVulkanInstanceKHR (a plain vkCreateInstance instance makes Monado log
    // XR_ERROR_VALIDATION_FAILURE and makes WiVRn fail the query outright, the observed live bug) —
    // and fall back to a plain vkCreateInstance only for a runtime that lacks the entry point.
    //
    // Routing instance creation through the runtime can make it spin up Vulkan synchronously, and
    // even a plain vkCreateInstance can deadlock against the runtime's own in-process Vulkan usage —
    // so this whole function is designed to run on a THROWAWAY thread the caller abandons on a
    // timeout (never on the main thread): `abandon` lets it bail before any XrInstance call once the
    // caller has moved on, so a late unblock can never touch a torn-down XR instance.
    PFN_xrGetVulkanGraphicsDevice2KHR pGetDev = nullptr;
    if (XR_FAILED(xrGetInstanceProcAddr(instance, "xrGetVulkanGraphicsDevice2KHR", reinterpret_cast<PFN_xrVoidFunction*>(&pGetDev))) || !pGetDev) {
        out.note = "runtime does not expose xrGetVulkanGraphicsDevice2KHR";
        return out;
    }
    PFN_xrCreateVulkanInstanceKHR pCreateXrVkInst = nullptr;
    if (XR_FAILED(xrGetInstanceProcAddr(instance, "xrCreateVulkanInstanceKHR", reinterpret_cast<PFN_xrVoidFunction*>(&pCreateXrVkInst))))
        pCreateXrVkInst = nullptr;

    // Best-effort precondition: the spec requires xrGetVulkanGraphicsRequirements2KHR before the
    // instance/device calls. It is a pure getter (version bounds — no Vulkan objects created, does
    // not block). We also use its minApiVersionSupported to pick a VkApplicationInfo::apiVersion the
    // runtime will accept (WiVRn requires a high-enough Vulkan version).
    uint32_t                                wantApiVersion = VK_API_VERSION_1_1;
    PFN_xrGetVulkanGraphicsRequirements2KHR pGetReqs       = nullptr;
    if (!abandoned() && XR_SUCCEEDED(xrGetInstanceProcAddr(instance, "xrGetVulkanGraphicsRequirements2KHR", reinterpret_cast<PFN_xrVoidFunction*>(&pGetReqs))) && pGetReqs) {
        XrGraphicsRequirementsVulkanKHR reqs = {XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
        if (XR_SUCCEEDED(pGetReqs(instance, systemId, &reqs))) {
            const uint32_t reqApi = VK_MAKE_API_VERSION(0, XR_VERSION_MAJOR(reqs.minApiVersionSupported), XR_VERSION_MINOR(reqs.minApiVersionSupported), 0);
            if (reqApi > wantApiVersion)
                wantApiVersion = reqApi;
        }
    }

    void* vklib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!vklib)
        vklib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!vklib) {
        out.note = "libvulkan not available (dlopen failed)";
        return out;
    }
    auto vkGIPA = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(vklib, "vkGetInstanceProcAddr"));
    auto pCreateInstance = vkGIPA ? reinterpret_cast<PFN_vkCreateInstance>(vkGIPA(nullptr, "vkCreateInstance")) : nullptr;
    if (!vkGIPA || !pCreateInstance) {
        dlclose(vklib);
        out.note = "libvulkan missing vkGetInstanceProcAddr/vkCreateInstance";
        return out;
    }

    // Our own minimal VkInstance. apiVersion is the max of 1.1 (makes VkPhysicalDeviceProperties2 and
    // the DRM property struct core-queryable) and the runtime's advertised minimum; no instance
    // extensions are requested — the runtime adds any it needs via xrCreateVulkanInstanceKHR.
    VkApplicationInfo app = {};
    app.sType             = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName  = "Hyprland-XR-gpu-probe";
    app.apiVersion        = wantApiVersion;

    VkInstanceCreateInfo ici = {};
    ici.sType                = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo     = &app;

    if (abandoned()) {
        dlclose(vklib);
        out.note = "probe abandoned (timed out)";
        return out;
    }

    // vkDestroyInstance is an instance-level command: it must be resolved from a live VkInstance
    // (vkGetInstanceProcAddr(NULL, ...) returns NULL for it), so it is fetched per created instance.
    auto destroyInst = [&](VkInstance& inst) {
        if (inst == VK_NULL_HANDLE)
            return;
        if (auto d = reinterpret_cast<PFN_vkDestroyInstance>(vkGIPA(inst, "vkDestroyInstance")))
            d(inst, nullptr);
        inst = VK_NULL_HANDLE;
    };

    // Run xrGetVulkanGraphicsDevice2KHR against a given VkInstance. Returns the physical device or
    // VK_NULL_HANDLE. Does not touch the XR instance once abandoned.
    auto queryDevice = [&](VkInstance inst) -> VkPhysicalDevice {
        if (abandoned() || inst == VK_NULL_HANDLE)
            return VK_NULL_HANDLE;
        XrVulkanGraphicsDeviceGetInfoKHR gdi = {XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
        gdi.systemId                         = systemId;
        gdi.vulkanInstance                   = inst;
        VkPhysicalDevice phys                = VK_NULL_HANDLE;
        if (XR_FAILED(pGetDev(instance, &gdi, &phys)))
            return VK_NULL_HANDLE;
        return phys;
    };

    // Attempt 1 — the SPEC path: create the VkInstance through the runtime. xrCreateVulkanInstanceKHR
    // merges in the instance extensions the runtime needs to match its physical device, and BOTH
    // Monado and WiVRn require it — their xrGetVulkanGraphicsDevice2KHR validates that the instance
    // was created this way (Monado logs XR_ERROR_VALIDATION_FAILURE / "vk_get_instance_proc_addr ==
    // NULL" and WiVRn simply returns failure for a foreign plain-vkCreateInstance instance). So this
    // is tried first; the plain fallback below only covers a runtime that lacks the entry point.
    VkInstance       vkInst = VK_NULL_HANDLE;
    VkPhysicalDevice phys   = VK_NULL_HANDLE;
    if (pCreateXrVkInst && !abandoned()) {
        XrVulkanInstanceCreateInfoKHR xci = {XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
        xci.systemId                      = systemId;
        xci.pfnGetInstanceProcAddr        = vkGIPA;
        xci.vulkanCreateInfo              = &ici;
        VkResult   vkRes                  = VK_SUCCESS;
        const auto xrRes                  = pCreateXrVkInst(instance, &xci, &vkInst, &vkRes);
        if (XR_SUCCEEDED(xrRes) && vkRes == VK_SUCCESS && vkInst != VK_NULL_HANDLE)
            phys = queryDevice(vkInst);
        else {
            vkInst   = VK_NULL_HANDLE;
            out.note = std::string("xrCreateVulkanInstanceKHR failed (xr ") + std::to_string((int)xrRes) + ", vk " + std::to_string((int)vkRes) + ")";
        }
    }

    // Attempt 2 — fallback: a plain vkCreateInstance, for a runtime that does NOT advertise
    // xrCreateVulkanInstanceKHR (spec-illegal for vulkan_enable2, but defended). Only reached when the
    // spec path was unavailable or its instance did not yield a device.
    if (phys == VK_NULL_HANDLE && vkInst == VK_NULL_HANDLE && !pCreateXrVkInst && !abandoned()) {
        if (pCreateInstance(&ici, nullptr, &vkInst) == VK_SUCCESS && vkInst != VK_NULL_HANDLE)
            phys = queryDevice(vkInst);
        else {
            vkInst = VK_NULL_HANDLE;
            if (out.note.empty())
                out.note = "vkCreateInstance failed";
        }
    }

    if (phys == VK_NULL_HANDLE) {
        destroyInst(vkInst);
        dlclose(vklib);
        if (out.note.empty())
            out.note = abandoned() ? "probe abandoned (timed out)" : "xrGetVulkanGraphicsDevice2KHR failed";
        return out;
    }

    auto pProps2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(vkGIPA(vkInst, "vkGetPhysicalDeviceProperties2"));

    if (pProps2) {
        // VK_EXT_physical_device_drm (a physical-device property; queryable without enabling the
        // device extension) gives the DRM major/minor of the render (preferred) or primary node.
        VkPhysicalDeviceDrmPropertiesEXT drm = {};
        drm.sType                            = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT;
        VkPhysicalDeviceProperties2 props2   = {};
        props2.sType                         = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        props2.pNext                         = &drm;
        pProps2(phys, &props2);

        out.deviceName = props2.properties.deviceName;
        if (drm.hasRender) {
            out.determined = true;
            out.drmMajor   = (int64_t)drm.renderMajor;
            out.drmMinor   = (int64_t)drm.renderMinor;
        } else if (drm.hasPrimary) {
            out.determined = true;
            out.drmMajor   = (int64_t)drm.primaryMajor;
            out.drmMinor   = (int64_t)drm.primaryMinor;
            out.note       = "runtime reported only a primary DRM node";
        } else {
            out.note = "driver did not report a DRM node (VK_EXT_physical_device_drm unsupported)";
        }
    } else {
        out.note = "vkGetPhysicalDeviceProperties2 unavailable";
    }

    destroyInst(vkInst);
    dlclose(vklib);
    return out;
}

}

#else // !HAVE_XR_VULKAN_PROBE — built without Vulkan headers; the FALLBACK probe is a no-op stub.
      // The EGL device query above is unaffected and remains the guard's primary answer.

namespace OpenXR {
SRuntimeGpu probeRuntimeRenderNode(XrInstance, XrSystemId, const std::atomic<bool>*) {
    SRuntimeGpu out;
    out.probe = "Vulkan device query (XR_KHR_vulkan_enable2)";
    out.note  = "built without Vulkan headers (XR GPU probe unavailable)";
    return out;
}
}

#endif // HAVE_XR_VULKAN_PROBE
#endif // HAVE_OPENXR
