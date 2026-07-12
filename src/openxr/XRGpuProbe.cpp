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
