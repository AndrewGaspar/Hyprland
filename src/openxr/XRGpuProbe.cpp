#include "XRGpuProbe.hpp"
#ifdef HAVE_OPENXR

#include "../debug/log/Logger.hpp"

#ifdef HAVE_XR_VULKAN_PROBE

// The probe needs the Vulkan-flavoured OpenXR structs (XrVulkanInstanceCreateInfoKHR etc.), which
// openxr_platform.h only declares under XR_USE_GRAPHICS_API_VULKAN — so Vulkan headers must come
// first. This TU is the ONLY place Hyprland pulls in Vulkan; libvulkan is dlopen'd at runtime (no
// hard link dependency) so a box without a Vulkan ICD simply falls back to "could not verify".
#define XR_USE_GRAPHICS_API_VULKAN
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <dlfcn.h>
#include <atomic>

namespace OpenXR {

SRuntimeGpu probeRuntimeRenderNode(XrInstance instance, XrSystemId systemId, const std::atomic<bool>* abandon) {
    SRuntimeGpu out;
    auto abandoned = [&] { return abandon && abandon->load(std::memory_order_acquire); };

    // Resolve ONLY the device-query entry point of XR_KHR_vulkan_enable2. We deliberately do NOT
    // use xrCreateVulkanInstanceKHR: routing VkInstance creation through the runtime makes Monado
    // (and likely WiVRn) spin up its full Vulkan compositor synchronously. And even a PLAIN
    // vkCreateInstance can deadlock against the runtime's own in-process Vulkan usage (observed
    // hanging indefinitely against Monado's null compositor). So this whole function is designed
    // to be run on a THROWAWAY thread the caller abandons on a timeout (never on the main thread):
    // `abandon` lets it bail before any XrInstance call once the caller has moved on, so a late
    // unblock can never touch a torn-down XR instance. Vulkan objects here are all our own.
    PFN_xrGetVulkanGraphicsDevice2KHR pGetDev = nullptr;
    if (XR_FAILED(xrGetInstanceProcAddr(instance, "xrGetVulkanGraphicsDevice2KHR", reinterpret_cast<PFN_xrVoidFunction*>(&pGetDev))) || !pGetDev) {
        out.note = "runtime does not expose xrGetVulkanGraphicsDevice2KHR";
        return out;
    }

    // Best-effort precondition: the spec wants xrGetVulkanGraphicsRequirements2KHR called before the
    // device query. It is a pure getter (version bounds — no Vulkan objects created, does not block).
    // Ignore its result; the device query below is what we actually need.
    PFN_xrGetVulkanGraphicsRequirements2KHR pGetReqs = nullptr;
    if (!abandoned() && XR_SUCCEEDED(xrGetInstanceProcAddr(instance, "xrGetVulkanGraphicsRequirements2KHR", reinterpret_cast<PFN_xrVoidFunction*>(&pGetReqs))) && pGetReqs) {
        XrGraphicsRequirementsVulkanKHR reqs = {XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
        pGetReqs(instance, systemId, &reqs);
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

    // Our own minimal VkInstance. apiVersion 1.1 makes VkPhysicalDeviceProperties2 and the DRM
    // property struct core-queryable; no instance extensions are needed just to read properties.
    VkApplicationInfo app = {};
    app.sType             = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName  = "Hyprland-XR-gpu-probe";
    app.apiVersion        = VK_API_VERSION_1_1;

    VkInstanceCreateInfo ici = {};
    ici.sType                = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo     = &app;

    VkInstance vkInst = VK_NULL_HANDLE;
    if (pCreateInstance(&ici, nullptr, &vkInst) != VK_SUCCESS || vkInst == VK_NULL_HANDLE) {
        dlclose(vklib);
        out.note = "vkCreateInstance failed";
        return out;
    }

    auto pDestroy = reinterpret_cast<PFN_vkDestroyInstance>(vkGIPA(vkInst, "vkDestroyInstance"));
    auto pProps2  = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(vkGIPA(vkInst, "vkGetPhysicalDeviceProperties2"));

    // The caller moved on (timeout): do NOT touch the XR instance — it may be torn down. Clean up
    // our own Vulkan objects and bail.
    if (abandoned()) {
        if (pDestroy)
            pDestroy(vkInst, nullptr);
        dlclose(vklib);
        out.note = "probe abandoned (timed out)";
        return out;
    }

    XrVulkanGraphicsDeviceGetInfoKHR gdi = {XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
    gdi.systemId                         = systemId;
    gdi.vulkanInstance                   = vkInst;

    VkPhysicalDevice phys = VK_NULL_HANDLE;
    if (XR_FAILED(pGetDev(instance, &gdi, &phys)) || phys == VK_NULL_HANDLE) {
        if (pDestroy)
            pDestroy(vkInst, nullptr);
        dlclose(vklib);
        out.note = "xrGetVulkanGraphicsDevice2KHR failed";
        return out;
    }

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

    if (pDestroy)
        pDestroy(vkInst, nullptr);
    dlclose(vklib);
    return out;
}

}

#else // !HAVE_XR_VULKAN_PROBE — built without Vulkan headers; the probe is a no-op stub.

namespace OpenXR {
SRuntimeGpu probeRuntimeRenderNode(XrInstance, XrSystemId, const std::atomic<bool>*) {
    SRuntimeGpu out;
    out.note = "built without Vulkan headers (XR GPU probe unavailable)";
    return out;
}
}

#endif // HAVE_XR_VULKAN_PROBE
#endif // HAVE_OPENXR
