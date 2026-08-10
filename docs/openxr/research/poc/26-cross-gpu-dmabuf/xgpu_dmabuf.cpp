// xgpu_dmabuf — cross-GPU dma-buf image sharing probe.
//
// Answers, with code rather than literature, whether a Vulkan image allocated on
// one GPU can be exported as a dma-buf and imported+sampled by a *different
// vendor's* GPU on this machine, at OpenXR swapchain shapes; and what it costs.
//
// Written for HypXRland research report 26 (WiVRn multi-GPU client render).
//
// Build:  cmake -S . -B build && cmake --build build -j8
// Run:    ./build/xgpu_dmabuf [--width W] [--height H] [--iters N] [--format N]
//
// It is read-only with respect to the system: it creates its own VkInstance and
// VkDevices, touches no compositor, no X/Wayland surface, no DRM master.

#include <vulkan/vulkan.h>

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <algorithm>
#include <string>
#include <vector>

#include <unistd.h>

// ---------------------------------------------------------------------------
// plumbing
// ---------------------------------------------------------------------------

static const char* vkres(VkResult r) {
    switch (r) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_NOT_READY: return "VK_NOT_READY";
        case VK_TIMEOUT: return "VK_TIMEOUT";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
        case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
        case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
        case VK_ERROR_INVALID_EXTERNAL_HANDLE: return "VK_ERROR_INVALID_EXTERNAL_HANDLE";
        case VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT:
            return "VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT";
        case VK_ERROR_UNKNOWN: return "VK_ERROR_UNKNOWN";
        default: return "VK_ERROR_<other>";
    }
}

#define VK_CHECK(x)                                                                      \
    do {                                                                                 \
        VkResult _r = (x);                                                               \
        if (_r != VK_SUCCESS) {                                                          \
            std::fprintf(stderr, "FATAL %s:%d: %s -> %s\n", __FILE__, __LINE__, #x,      \
                         vkres(_r));                                                     \
            std::exit(1);                                                                \
        }                                                                                \
    } while (0)

#define VK_TRY(x, out)                                                                   \
    do {                                                                                 \
        (out) = (x);                                                                     \
        if ((out) != VK_SUCCESS)                                                         \
            std::fprintf(stderr, "  [soft-fail] %s -> %s\n", #x, vkres(out));            \
    } while (0)

// DRM fourcc modifier decoding (subset; enough to name what these two drivers offer).
static std::string modName(uint64_t m) {
    char buf[128];
    if (m == 0)
        return "DRM_FORMAT_MOD_LINEAR";
    if (m == 0x00ffffffffffffffULL)
        return "DRM_FORMAT_MOD_INVALID";
    const uint8_t vendor = (uint8_t)(m >> 56);
    const char*   vname  = "?";
    switch (vendor) {
        case 0x01: vname = "INTEL"; break;
        case 0x02: vname = "AMD"; break;
        case 0x03: vname = "NVIDIA"; break;
        case 0x04: vname = "SAMSUNG"; break;
        case 0x06: vname = "ARM"; break;
        case 0x08: vname = "BROADCOM"; break;
        case 0x0a: vname = "AMLOGIC"; break;
    }
    if (vendor == 0x03) {
        // NVIDIA block-linear: bits 0..3 = height in GOBs (log2), bit 4 = "block linear"
        const uint64_t payload = m & 0x00ffffffffffffffULL;
        if ((payload & 0x10) == 0x10) {
            std::snprintf(buf, sizeof buf, "NVIDIA_BLOCK_LINEAR_2D(h=%u gob) [0x%016" PRIx64 "]",
                          (unsigned)(payload & 0xf), m);
            return buf;
        }
    }
    if (vendor == 0x02) {
        // AMD/AMDGPU modifier: TILE_VERSION in bits 5..7, TILE in 0..4
        const uint64_t tile    = m & 0x1f;
        const uint64_t version = (m >> 5) & 0x7;
        const char*    vv      = "?";
        switch (version) {
            case 1: vv = "GFX9"; break;
            case 2: vv = "GFX10"; break;
            case 3: vv = "GFX10_RBPLUS"; break;
            case 4: vv = "GFX11"; break;
            case 5: vv = "GFX12"; break;
        }
        std::snprintf(buf, sizeof buf, "AMD(ver=%s tile=%u) [0x%016" PRIx64 "]", vv,
                      (unsigned)tile, m);
        return buf;
    }
    std::snprintf(buf, sizeof buf, "%s [0x%016" PRIx64 "]", vname, m);
    return buf;
}

static const char* fmtName(VkFormat f) {
    switch (f) {
        case VK_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
        case VK_FORMAT_R8G8B8A8_SRGB: return "R8G8B8A8_SRGB";
        case VK_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
        case VK_FORMAT_B8G8R8A8_SRGB: return "B8G8R8A8_SRGB";
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return "A2B10G10R10_UNORM_PACK32";
        case VK_FORMAT_R16G16B16A16_SFLOAT: return "R16G16B16A16_SFLOAT";
        default: return "<fmt>";
    }
}

// ---------------------------------------------------------------------------
// device context
// ---------------------------------------------------------------------------

struct Gpu {
    VkPhysicalDevice                 phys = VK_NULL_HANDLE;
    VkDevice                         dev  = VK_NULL_HANDLE;
    VkQueue                          q    = VK_NULL_HANDLE;
    uint32_t                         qfi  = 0;
    VkCommandPool                    pool = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties mem{};
    VkPhysicalDeviceProperties       props{};
    uint8_t                          uuid[VK_UUID_SIZE]{};
    uint8_t                          driverUuid[VK_UUID_SIZE]{};
    int64_t                          renderMinor = -1;
    int64_t                          primaryMinor = -1;
    std::string                      driverName;
    bool                             hasModifier = false;
    bool                             hasDmaBuf   = false;
    bool                             hasForeign  = false;
    bool                             hasSemFd    = false;
    bool                             hasFenceFd  = false;
    VkQueryPool                      tsPool = VK_NULL_HANDLE;
    float                            tsPeriodNs = 0.f;

    PFN_vkGetMemoryFdKHR                        GetMemoryFdKHR                  = nullptr;
    PFN_vkGetMemoryFdPropertiesKHR              GetMemoryFdPropertiesKHR        = nullptr;
    PFN_vkGetImageDrmFormatModifierPropertiesEXT GetImageDrmFormatModifierProps = nullptr;
    PFN_vkGetSemaphoreFdKHR                     GetSemaphoreFdKHR               = nullptr;
    PFN_vkImportSemaphoreFdKHR                  ImportSemaphoreFdKHR            = nullptr;

    const char* tag() const { return props.deviceName; }
};

static uint32_t pickMemType(const Gpu& g, uint32_t typeBits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < g.mem.memoryTypeCount; i++)
        if ((typeBits & (1u << i)) && (g.mem.memoryTypes[i].propertyFlags & want) == want)
            return i;
    // fall back to any allowed type rather than returning an invalid index
    for (uint32_t i = 0; i < g.mem.memoryTypeCount; i++)
        if (typeBits & (1u << i))
            return i;
    return UINT32_MAX;
}

// Order candidate memory types: DEVICE_LOCAL-only first (VRAM), then the rest.
static std::vector<uint32_t> memTypeOrder(const Gpu& g, uint32_t typeBits) {
    std::vector<uint32_t> devLocal, other;
    for (uint32_t i = 0; i < g.mem.memoryTypeCount; i++) {
        if (!(typeBits & (1u << i)))
            continue;
        if (g.mem.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
            devLocal.push_back(i);
        else
            other.push_back(i);
    }
    devLocal.insert(devLocal.end(), other.begin(), other.end());
    return devLocal;
}

static std::string memTypeDesc(const Gpu& g, uint32_t i) {
    if (i >= g.mem.memoryTypeCount)
        return "<invalid>";
    const auto f = g.mem.memoryTypes[i].propertyFlags;
    const auto h = g.mem.memoryTypes[i].heapIndex;
    std::string s = "type" + std::to_string(i) + " heap" + std::to_string(h) + " ";
    if (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) s += "DEVICE_LOCAL|";
    if (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) s += "HOST_VISIBLE|";
    if (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) s += "HOST_COHERENT|";
    if (f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) s += "HOST_CACHED|";
    if (f & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) s += "LAZY|";
    if (s.back() == '|') s.pop_back();
    const bool heapDevLocal =
        (g.mem.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0;
    s += heapDevLocal ? " (VRAM heap)" : " (sysmem heap)";
    return s;
}

static VkCommandBuffer beginCmd(const Gpu& g) {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool        = g.pool;
    ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cb;
    VK_CHECK(vkAllocateCommandBuffers(g.dev, &ai, &cb));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cb, &bi));
    return cb;
}

static void endCmdWait(const Gpu& g, VkCommandBuffer cb) {
    VK_CHECK(vkEndCommandBuffer(cb));
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers    = &cb;
    VK_CHECK(vkQueueSubmit(g.q, 1, &si, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(g.q));
    vkFreeCommandBuffers(g.dev, g.pool, 1, &cb);
}

static void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout oldL, VkImageLayout newL,
                    VkAccessFlags srcA, VkAccessFlags dstA, uint32_t srcQ = VK_QUEUE_FAMILY_IGNORED,
                    uint32_t dstQ = VK_QUEUE_FAMILY_IGNORED) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout                   = oldL;
    b.newLayout                   = newL;
    b.srcAccessMask               = srcA;
    b.dstAccessMask               = dstA;
    b.srcQueueFamilyIndex         = srcQ;
    b.dstQueueFamilyIndex         = dstQ;
    b.image                       = img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &b);
}

// ---------------------------------------------------------------------------
// modifier queries
// ---------------------------------------------------------------------------

struct ModInfo {
    uint64_t mod;
    uint32_t planeCount;
    bool     exportable;   // can be exported as dma-buf with our usage
    bool     importable;   // can be imported from dma-buf with our usage
    bool     dedicatedOnly;
    VkExtent3D maxExtent;
};

static std::vector<ModInfo> queryModifiers(const Gpu& g, VkFormat fmt, VkImageUsageFlags usage) {
    std::vector<ModInfo> out;
    if (!g.hasModifier)
        return out;

    VkDrmFormatModifierPropertiesListEXT list{
        VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT};
    VkFormatProperties2 fp{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, &list};
    vkGetPhysicalDeviceFormatProperties2(g.phys, fmt, &fp);
    if (!list.drmFormatModifierCount)
        return out;
    std::vector<VkDrmFormatModifierPropertiesEXT> mods(list.drmFormatModifierCount);
    list.pDrmFormatModifierProperties = mods.data();
    vkGetPhysicalDeviceFormatProperties2(g.phys, fmt, &fp);

    for (auto& m : mods) {
        ModInfo mi{};
        mi.mod        = m.drmFormatModifier;
        mi.planeCount = m.drmFormatModifierPlaneCount;

        // Ask whether an image with this modifier + usage can be created at all,
        // and whether dma-buf export / import are advertised for it.
        for (int dir = 0; dir < 2; dir++) {
            VkPhysicalDeviceImageDrmFormatModifierInfoEXT modInfo{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT};
            modInfo.drmFormatModifier = m.drmFormatModifier;
            modInfo.sharingMode       = VK_SHARING_MODE_EXCLUSIVE;

            VkPhysicalDeviceExternalImageFormatInfo extInfo{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO, &modInfo};
            extInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

            VkPhysicalDeviceImageFormatInfo2 ifi{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, &extInfo};
            ifi.format = fmt;
            ifi.type   = VK_IMAGE_TYPE_2D;
            ifi.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
            ifi.usage  = usage;
            ifi.flags  = 0;

            VkExternalImageFormatProperties efp{
                VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
            VkImageFormatProperties2 ifp{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, &efp};

            if (vkGetPhysicalDeviceImageFormatProperties2(g.phys, &ifi, &ifp) != VK_SUCCESS)
                continue;

            const auto f = efp.externalMemoryProperties.externalMemoryFeatures;
            if (dir == 0) {
                mi.exportable = (f & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) != 0;
                mi.importable = (f & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) != 0;
                mi.dedicatedOnly =
                    (f & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT) != 0;
                mi.maxExtent = ifp.imageFormatProperties.maxExtent;
            }
            break;
        }
        out.push_back(mi);
    }
    std::sort(out.begin(), out.end(), [](const ModInfo& a, const ModInfo& b) {
        return a.mod < b.mod;
    });
    return out;
}

static void dumpModifiers(const Gpu& g, VkFormat fmt, VkImageUsageFlags usage) {
    auto mods = queryModifiers(g, fmt, usage);
    std::printf("  %-46s %s : %zu modifiers\n", g.tag(), fmtName(fmt), mods.size());
    for (auto& m : mods) {
        std::printf("      %-58s planes=%u exp=%d imp=%d ded=%d max=%ux%u\n",
                    modName(m.mod).c_str(), m.planeCount, (int)m.exportable, (int)m.importable,
                    (int)m.dedicatedOnly, m.maxExtent.width, m.maxExtent.height);
    }
}

// ---------------------------------------------------------------------------
// images
// ---------------------------------------------------------------------------

struct Img {
    VkImage        img  = VK_NULL_HANDLE;
    VkDeviceMemory mem  = VK_NULL_HANDLE;
    uint64_t       mod  = 0;
    uint32_t       planes = 1;
    VkDeviceSize   size = 0;
    uint32_t       memTypeIdx = UINT32_MAX;
    std::vector<VkSubresourceLayout> layouts;
};

// Create an image on `g` with tiling=DRM_FORMAT_MODIFIER restricted to `mods`,
// backed by exportable (dma-buf) dedicated memory.
static bool createExportable(const Gpu& g, uint32_t w, uint32_t h, VkFormat fmt,
                             VkImageUsageFlags usage, const std::vector<uint64_t>& mods, Img& out,
                             int* outFd) {
    VkImageDrmFormatModifierListCreateInfoEXT modList{
        VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT};
    modList.drmFormatModifierCount    = (uint32_t)mods.size();
    modList.pDrmFormatModifiers       = mods.data();

    VkFormat            fmtList[1] = {fmt};
    VkImageFormatListCreateInfo fl{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO, &modList};
    fl.viewFormatCount = 1;
    fl.pViewFormats    = fmtList;

    VkExternalMemoryImageCreateInfo emi{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, &fl};
    emi.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &emi};
    ici.imageType     = VK_IMAGE_TYPE_2D;
    ici.format        = fmt;
    ici.extent        = {w, h, 1};
    ici.mipLevels     = 1;
    ici.arrayLayers   = 1;
    ici.samples       = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling        = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    ici.usage         = usage;
    ici.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkResult r;
    VK_TRY(vkCreateImage(g.dev, &ici, nullptr, &out.img), r);
    if (r != VK_SUCCESS)
        return false;

    VkMemoryDedicatedRequirements dedReq{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    VkMemoryRequirements2         mr{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, &dedReq};
    VkImageMemoryRequirementsInfo2 mri{VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2};
    mri.image = out.img;
    vkGetImageMemoryRequirements2(g.dev, &mri, &mr);
    out.size = mr.memoryRequirements.size;

    VkMemoryDedicatedAllocateInfo dai{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    dai.image = out.img;
    VkExportMemoryAllocateInfo ema{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, &dai};
    ema.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    // Drivers differ on WHICH memory types can back a dma-buf export (NVIDIA in particular
    // refuses some VRAM types). Try every allowed type, VRAM-first, and report which one won —
    // that tells us where the shared buffer physically lives.
    r = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    for (uint32_t mt : memTypeOrder(g, mr.memoryRequirements.memoryTypeBits)) {
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &ema};
        mai.allocationSize  = mr.memoryRequirements.size;
        mai.memoryTypeIndex = mt;
        r                   = vkAllocateMemory(g.dev, &mai, nullptr, &out.mem);
        if (r == VK_SUCCESS) {
            out.memTypeIdx = mt;
            break;
        }
    }
    if (r != VK_SUCCESS) {
        std::fprintf(stderr, "  [soft-fail] no exportable memory type worked (bits=0x%x): %s\n",
                     mr.memoryRequirements.memoryTypeBits, vkres(r));
        vkDestroyImage(g.dev, out.img, nullptr);
        out.img = VK_NULL_HANDLE;
        return false;
    }
    VK_CHECK(vkBindImageMemory(g.dev, out.img, out.mem, 0));

    VkImageDrmFormatModifierPropertiesEXT mp{
        VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT};
    VK_CHECK(g.GetImageDrmFormatModifierProps(g.dev, out.img, &mp));
    out.mod = mp.drmFormatModifier;

    // plane count for the chosen modifier
    auto all = queryModifiers(g, fmt, usage);
    out.planes = 1;
    for (auto& m : all)
        if (m.mod == out.mod)
            out.planes = m.planeCount;

    static const VkImageAspectFlagBits kPlane[4] = {
        VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT, VK_IMAGE_ASPECT_MEMORY_PLANE_1_BIT_EXT,
        VK_IMAGE_ASPECT_MEMORY_PLANE_2_BIT_EXT, VK_IMAGE_ASPECT_MEMORY_PLANE_3_BIT_EXT};
    out.layouts.resize(out.planes);
    for (uint32_t i = 0; i < out.planes; i++) {
        VkImageSubresource sr{};
        sr.aspectMask = kPlane[i];
        vkGetImageSubresourceLayout(g.dev, out.img, &sr, &out.layouts[i]);
    }

    if (outFd) {
        VkMemoryGetFdInfoKHR gfi{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
        gfi.memory     = out.mem;
        gfi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        VK_TRY(g.GetMemoryFdKHR(g.dev, &gfi, outFd), r);
        if (r != VK_SUCCESS)
            return false;
    }
    return true;
}

// Import a dma-buf fd on `g` as an image with an explicit modifier + plane layouts.
static bool importImage(const Gpu& g, uint32_t w, uint32_t h, VkFormat fmt,
                        VkImageUsageFlags usage, uint64_t mod,
                        const std::vector<VkSubresourceLayout>& layouts, int fd, Img& out) {
    VkImageDrmFormatModifierExplicitCreateInfoEXT ex{
        VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT};
    ex.drmFormatModifier           = mod;
    ex.drmFormatModifierPlaneCount = (uint32_t)layouts.size();
    ex.pPlaneLayouts               = layouts.data();

    VkExternalMemoryImageCreateInfo emi{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, &ex};
    emi.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &emi};
    ici.imageType     = VK_IMAGE_TYPE_2D;
    ici.format        = fmt;
    ici.extent        = {w, h, 1};
    ici.mipLevels     = 1;
    ici.arrayLayers   = 1;
    ici.samples       = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling        = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    ici.usage         = usage;
    ici.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkResult r;
    VK_TRY(vkCreateImage(g.dev, &ici, nullptr, &out.img), r);
    if (r != VK_SUCCESS)
        return false;
    out.mod    = mod;
    out.planes = (uint32_t)layouts.size();

    VkMemoryRequirements2          mr{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
    VkImageMemoryRequirementsInfo2 mri{VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2};
    mri.image = out.img;
    vkGetImageMemoryRequirements2(g.dev, &mri, &mr);

    VkMemoryFdPropertiesKHR fdp{VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    VK_TRY(g.GetMemoryFdPropertiesKHR(g.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd,
                                      &fdp),
           r);
    if (r != VK_SUCCESS) {
        vkDestroyImage(g.dev, out.img, nullptr);
        out.img = VK_NULL_HANDLE;
        return false;
    }

    const uint32_t bits = mr.memoryRequirements.memoryTypeBits & fdp.memoryTypeBits;
    if (!bits) {
        std::fprintf(stderr,
                     "  [soft-fail] no common memory type: imageBits=0x%x fdBits=0x%x\n",
                     mr.memoryRequirements.memoryTypeBits, fdp.memoryTypeBits);
        vkDestroyImage(g.dev, out.img, nullptr);
        out.img = VK_NULL_HANDLE;
        return false;
    }
    VkMemoryDedicatedAllocateInfo dai{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    dai.image = out.img;
    VkImportMemoryFdInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, &dai};
    imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    imp.fd         = fd;  // ownership transfers to Vulkan on success

    r = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    for (uint32_t mt : memTypeOrder(g, bits)) {
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp};
        mai.allocationSize  = mr.memoryRequirements.size;
        mai.memoryTypeIndex = mt;
        r                   = vkAllocateMemory(g.dev, &mai, nullptr, &out.mem);
        if (r == VK_SUCCESS) {
            out.memTypeIdx = mt;
            break;
        }
        // An import consumes the fd only on success; keep trying other types.
    }
    if (r != VK_SUCCESS) {
        std::fprintf(stderr, "  [soft-fail] import vkAllocateMemory failed for all types "
                             "(bits=0x%x): %s\n",
                     bits, vkres(r));
        vkDestroyImage(g.dev, out.img, nullptr);
        out.img = VK_NULL_HANDLE;
        return false;
    }
    VK_TRY(vkBindImageMemory(g.dev, out.img, out.mem, 0), r);
    if (r != VK_SUCCESS)
        return false;
    return true;
}

static Img createLocal(const Gpu& g, uint32_t w, uint32_t h, VkFormat fmt,
                       VkImageUsageFlags usage, VkImageTiling tiling = VK_IMAGE_TILING_OPTIMAL) {
    Img o{};
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType     = VK_IMAGE_TYPE_2D;
    ici.format        = fmt;
    ici.extent        = {w, h, 1};
    ici.mipLevels     = 1;
    ici.arrayLayers   = 1;
    ici.samples       = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling        = tiling;
    ici.usage         = usage;
    ici.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(g.dev, &ici, nullptr, &o.img));
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(g.dev, o.img, &mr);
    VkResult r = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    for (uint32_t mt : memTypeOrder(g, mr.memoryTypeBits)) {
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize  = mr.size;
        mai.memoryTypeIndex = mt;
        r                   = vkAllocateMemory(g.dev, &mai, nullptr, &o.mem);
        if (r == VK_SUCCESS) {
            o.memTypeIdx = mt;
            break;
        }
    }
    if (r != VK_SUCCESS) {
        std::fprintf(stderr, "  [soft-fail] createLocal alloc failed on %s: %s\n", g.tag(),
                     vkres(r));
        vkDestroyImage(g.dev, o.img, nullptr);
        o.img = VK_NULL_HANDLE;
        return o;
    }
    VK_CHECK(vkBindImageMemory(g.dev, o.img, o.mem, 0));
    o.size = mr.size;
    return o;
}

struct Buf {
    VkBuffer       buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    void*          ptr = nullptr;
    VkDeviceSize   size = 0;
};

static Buf createHostBuf(const Gpu& g, VkDeviceSize size, VkBufferUsageFlags usage) {
    Buf b{};
    b.size = size;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size  = size;
    bci.usage = usage;
    VK_CHECK(vkCreateBuffer(g.dev, &bci, nullptr, &b.buf));
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(g.dev, b.buf, &mr);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize  = mr.size;
    mai.memoryTypeIndex = pickMemType(g, mr.memoryTypeBits,
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VK_CHECK(vkAllocateMemory(g.dev, &mai, nullptr, &b.mem));
    VK_CHECK(vkBindBufferMemory(g.dev, b.buf, b.mem, 0));
    VK_CHECK(vkMapMemory(g.dev, b.mem, 0, VK_WHOLE_SIZE, 0, &b.ptr));
    return b;
}

// deterministic test pattern
static inline uint32_t pat(uint32_t x, uint32_t y) {
    const uint8_t r = (uint8_t)(x * 7 + 13);
    const uint8_t gg = (uint8_t)(y * 11 + 29);
    const uint8_t b = (uint8_t)((x ^ y) * 5 + 61);
    return (uint32_t)r | ((uint32_t)gg << 8) | ((uint32_t)b << 16) | 0xff000000u;
}

// ---------------------------------------------------------------------------
// instance / devices
// ---------------------------------------------------------------------------

static bool hasExt(const std::vector<VkExtensionProperties>& v, const char* n) {
    for (auto& e : v)
        if (!std::strcmp(e.extensionName, n))
            return true;
    return false;
}

static void initGpu(VkInstance inst, VkPhysicalDevice pd, Gpu& g) {
    g.phys = pd;

    VkPhysicalDeviceDrmPropertiesEXT drm{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT};
    VkPhysicalDeviceDriverProperties drv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES,
                                         &drm};
    VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES, &drv};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id};
    vkGetPhysicalDeviceProperties2(pd, &p2);
    g.props = p2.properties;
    std::memcpy(g.uuid, id.deviceUUID, VK_UUID_SIZE);
    std::memcpy(g.driverUuid, id.driverUUID, VK_UUID_SIZE);
    g.driverName   = drv.driverName;
    g.renderMinor  = drm.hasRender ? drm.renderMinor : -1;
    g.primaryMinor = drm.hasPrimary ? drm.primaryMinor : -1;
    g.tsPeriodNs   = g.props.limits.timestampPeriod;

    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> exts(n);
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, exts.data());

    g.hasModifier = hasExt(exts, VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
    g.hasDmaBuf   = hasExt(exts, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
    g.hasForeign  = hasExt(exts, VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);
    g.hasSemFd    = hasExt(exts, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);
    g.hasFenceFd  = hasExt(exts, VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME);

    vkGetPhysicalDeviceMemoryProperties(pd, &g.mem);

    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qf.data());
    g.qfi = 0;
    for (uint32_t i = 0; i < qn; i++)
        if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            g.qfi = i;
            break;
        }

    std::vector<const char*> want;
    auto add = [&](const char* e) {
        if (hasExt(exts, e))
            want.push_back(e);
    };
    add(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
    add(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
    add(VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
    add(VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME);
    add(VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);
    add(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);
    add(VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME);

    float prio = 1.f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = g.qfi;
    qci.queueCount       = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount    = 1;
    dci.pQueueCreateInfos       = &qci;
    dci.enabledExtensionCount   = (uint32_t)want.size();
    dci.ppEnabledExtensionNames = want.data();
    VK_CHECK(vkCreateDevice(pd, &dci, nullptr, &g.dev));
    vkGetDeviceQueue(g.dev, g.qfi, 0, &g.q);

    VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpi.queueFamilyIndex = g.qfi;
    cpi.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VK_CHECK(vkCreateCommandPool(g.dev, &cpi, nullptr, &g.pool));

    VkQueryPoolCreateInfo qpi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qpi.queryType  = VK_QUERY_TYPE_TIMESTAMP;
    qpi.queryCount = 2;
    vkCreateQueryPool(g.dev, &qpi, nullptr, &g.tsPool);

    g.GetMemoryFdKHR = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(g.dev, "vkGetMemoryFdKHR");
    g.GetMemoryFdPropertiesKHR =
        (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(g.dev, "vkGetMemoryFdPropertiesKHR");
    g.GetImageDrmFormatModifierProps = (PFN_vkGetImageDrmFormatModifierPropertiesEXT)
        vkGetDeviceProcAddr(g.dev, "vkGetImageDrmFormatModifierPropertiesEXT");
    g.GetSemaphoreFdKHR =
        (PFN_vkGetSemaphoreFdKHR)vkGetDeviceProcAddr(g.dev, "vkGetSemaphoreFdKHR");
    g.ImportSemaphoreFdKHR =
        (PFN_vkImportSemaphoreFdKHR)vkGetDeviceProcAddr(g.dev, "vkImportSemaphoreFdKHR");
    (void)inst;
}

static void printGpu(const Gpu& g) {
    char u[64], du[64];
    for (int i = 0; i < 16; i++) {
        std::snprintf(u + i * 2, 3, "%02x", g.uuid[i]);
        std::snprintf(du + i * 2, 3, "%02x", g.driverUuid[i]);
    }
    std::printf("  %-46s vendor=0x%04x driver=%-24s\n", g.props.deviceName, g.props.vendorID,
                g.driverName.c_str());
    std::printf("      deviceUUID=%s driverUUID=%s\n", u, du);
    std::printf("      renderD%ld card%ld  tsPeriod=%.1fns  modifier=%d dmabuf=%d foreign=%d "
                "semfd=%d fencefd=%d\n",
                (long)g.renderMinor, (long)(g.primaryMinor), g.tsPeriodNs, (int)g.hasModifier,
                (int)g.hasDmaBuf, (int)g.hasForeign, (int)g.hasSemFd, (int)g.hasFenceFd);
    std::printf("      memory heaps:\n");
    for (uint32_t i = 0; i < g.mem.memoryHeapCount; i++)
        std::printf("        heap[%u] %6.0f MiB flags=0x%x%s\n", i,
                    g.mem.memoryHeaps[i].size / 1048576.0, g.mem.memoryHeaps[i].flags,
                    (g.mem.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? " (VRAM)"
                                                                                   : " (sysmem)");
    std::printf("      memory types:\n");
    for (uint32_t i = 0; i < g.mem.memoryTypeCount; i++)
        std::printf("        %s\n", memTypeDesc(g, i).c_str());
}

// ---------------------------------------------------------------------------
// external sync capability probe
// ---------------------------------------------------------------------------

static void probeSync(const Gpu& g) {
    struct { VkExternalSemaphoreHandleTypeFlagBits t; const char* n; } sems[] = {
        {VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT, "SEMAPHORE OPAQUE_FD"},
        {VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT, "SEMAPHORE SYNC_FD"},
    };
    for (auto& s : sems) {
        for (int timeline = 0; timeline < 2; timeline++) {
            VkSemaphoreTypeCreateInfo st{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
            st.semaphoreType = timeline ? VK_SEMAPHORE_TYPE_TIMELINE : VK_SEMAPHORE_TYPE_BINARY;
            VkPhysicalDeviceExternalSemaphoreInfo si{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO, &st};
            si.handleType = s.t;
            VkExternalSemaphoreProperties sp{VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
            vkGetPhysicalDeviceExternalSemaphoreProperties(g.phys, &si, &sp);
            if (timeline && s.t == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT &&
                !sp.externalSemaphoreFeatures)
                continue;
            std::printf("      %-20s %-8s exportable=%d importable=%d (export=0x%x compat=0x%x)\n",
                        s.n, timeline ? "timeline" : "binary",
                        (sp.externalSemaphoreFeatures &
                         VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT) != 0,
                        (sp.externalSemaphoreFeatures &
                         VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT) != 0,
                        sp.exportFromImportedHandleTypes, sp.compatibleHandleTypes);
        }
    }
    struct { VkExternalFenceHandleTypeFlagBits t; const char* n; } fences[] = {
        {VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_FD_BIT, "FENCE OPAQUE_FD"},
        {VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT, "FENCE SYNC_FD"},
    };
    for (auto& f : fences) {
        VkPhysicalDeviceExternalFenceInfo fi{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_FENCE_INFO};
        fi.handleType = f.t;
        VkExternalFenceProperties fp{VK_STRUCTURE_TYPE_EXTERNAL_FENCE_PROPERTIES};
        vkGetPhysicalDeviceExternalFenceProperties(g.phys, &fi, &fp);
        std::printf("      %-20s %-8s exportable=%d importable=%d (export=0x%x compat=0x%x)\n",
                    f.n, "",
                    (fp.externalFenceFeatures & VK_EXTERNAL_FENCE_FEATURE_EXPORTABLE_BIT) != 0,
                    (fp.externalFenceFeatures & VK_EXTERNAL_FENCE_FEATURE_IMPORTABLE_BIT) != 0,
                    fp.exportFromImportedHandleTypes, fp.compatibleHandleTypes);
    }
}

// Try an ACTUAL cross-vendor binary semaphore handoff via sync_fd.
static void trySemaphoreHandoff(const Gpu& src, const Gpu& dst) {
    std::printf("  cross-vendor semaphore handoff %s -> %s:\n", src.tag(), dst.tag());
    if (!src.hasSemFd || !dst.hasSemFd) {
        std::printf("      SKIP (external_semaphore_fd missing on one side)\n");
        return;
    }
    for (int which = 0; which < 2; which++) {
        const auto ht = which == 0 ? VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT
                                   : VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
        const char* hn = which == 0 ? "SYNC_FD" : "OPAQUE_FD";

        VkExportSemaphoreCreateInfo esci{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
        esci.handleTypes = ht;
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &esci};
        VkSemaphore sem = VK_NULL_HANDLE;
        VkResult    r;
        VK_TRY(vkCreateSemaphore(src.dev, &sci, nullptr, &sem), r);
        if (r != VK_SUCCESS) {
            std::printf("      %-10s create on src FAILED\n", hn);
            continue;
        }
        // A sync_fd export requires the semaphore to have a pending signal.
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores    = &sem;
        VK_TRY(vkQueueSubmit(src.q, 1, &si, VK_NULL_HANDLE), r);

        VkSemaphoreGetFdInfoKHR gi{VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR};
        gi.semaphore  = sem;
        gi.handleType = ht;
        int fd        = -1;
        VK_TRY(src.GetSemaphoreFdKHR(src.dev, &gi, &fd), r);
        if (r != VK_SUCCESS) {
            std::printf("      %-10s export from src FAILED (%s)\n", hn, vkres(r));
            vkQueueWaitIdle(src.q);
            vkDestroySemaphore(src.dev, sem, nullptr);
            continue;
        }

        VkSemaphoreCreateInfo sci2{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkSemaphore           sem2 = VK_NULL_HANDLE;
        VK_CHECK(vkCreateSemaphore(dst.dev, &sci2, nullptr, &sem2));
        VkImportSemaphoreFdInfoKHR ii{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR};
        ii.semaphore  = sem2;
        ii.handleType = ht;
        ii.fd         = fd;
        ii.flags      = (ht == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT)
                            ? VK_SEMAPHORE_IMPORT_TEMPORARY_BIT
                            : 0;
        VK_TRY(dst.ImportSemaphoreFdKHR(dst.dev, &ii), r);
        if (r != VK_SUCCESS) {
            std::printf("      %-10s IMPORT INTO DST FAILED (%s)  <-- cross-vendor sync blocked\n",
                        hn, vkres(r));
            ::close(fd);
        } else {
            // Actually wait on it from the dst queue to see if it is honoured.
            VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            VkSubmitInfo         wsi{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            wsi.waitSemaphoreCount = 1;
            wsi.pWaitSemaphores    = &sem2;
            wsi.pWaitDstStageMask  = &stage;
            VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            VkFence           fence;
            VK_CHECK(vkCreateFence(dst.dev, &fci, nullptr, &fence));
            VK_TRY(vkQueueSubmit(dst.q, 1, &wsi, fence), r);
            VkResult wr = vkWaitForFences(dst.dev, 1, &fence, VK_TRUE, 2ull * 1000000000ull);
            std::printf("      %-10s IMPORT OK, cross-device wait -> %s\n", hn, vkres(wr));
            vkDestroyFence(dst.dev, fence, nullptr);
        }
        vkQueueWaitIdle(src.q);
        vkQueueWaitIdle(dst.q);
        vkDestroySemaphore(dst.dev, sem2, nullptr);
        vkDestroySemaphore(src.dev, sem, nullptr);
    }
}

// Does a cross-vendor sync_fd wait actually ORDER anything, or does it return immediately?
// A capability probe that says "import OK" proves nothing: we must show the waiter is held
// for as long as the signaller's real GPU work takes.
static void semOrderingTest(const Gpu& src, const Gpu& dst, uint32_t w, uint32_t h) {
    std::printf("  ordering test %s -> %s (sync_fd):\n", src.tag(), dst.tag());
    if (!src.hasSemFd || !dst.hasSemFd) {
        std::printf("      SKIP\n");
        return;
    }
    const VkFormat f = VK_FORMAT_R8G8B8A8_UNORM;
    Img a = createLocal(src, w, h, f,
                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    Img b = createLocal(src, w, h, f, VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    if (!a.img || !b.img) {
        std::printf("      SKIP (alloc failed)\n");
        return;
    }
    {
        VkCommandBuffer cb = beginCmd(src);
        barrier(cb, a.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0,
                VK_ACCESS_TRANSFER_READ_BIT);
        barrier(cb, b.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                VK_ACCESS_TRANSFER_WRITE_BIT);
        endCmdWait(src, cb);
    }

    // A deliberately long GPU job on src (hundreds of full-frame copies).
    const uint32_t  kJob = 400;
    VkCommandBuffer cb   = beginCmd(src);
    VkImageCopy     ic{};
    ic.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ic.srcSubresource.layerCount = 1;
    ic.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ic.dstSubresource.layerCount = 1;
    ic.extent                    = {w, h, 1};
    for (uint32_t i = 0; i < kJob; i++) {
        vkCmdCopyImage(cb, a.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b.img,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 1, &mb, 0, nullptr, 0, nullptr);
    }
    VK_CHECK(vkEndCommandBuffer(cb));

    VkExportSemaphoreCreateInfo esci{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
    esci.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &esci};
    VkSemaphore           sem;
    VK_CHECK(vkCreateSemaphore(src.dev, &sci, nullptr, &sem));

    const auto   t0 = std::chrono::steady_clock::now();
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount   = 1;
    si.pCommandBuffers      = &cb;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores    = &sem;
    VK_CHECK(vkQueueSubmit(src.q, 1, &si, VK_NULL_HANDLE));

    VkSemaphoreGetFdInfoKHR gi{VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR};
    gi.semaphore  = sem;
    gi.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
    int      fd   = -1;
    VkResult r;
    VK_TRY(src.GetSemaphoreFdKHR(src.dev, &gi, &fd), r);
    if (r != VK_SUCCESS) {
        std::printf("      export failed\n");
        return;
    }

    VkSemaphoreCreateInfo sci2{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore           sem2;
    VK_CHECK(vkCreateSemaphore(dst.dev, &sci2, nullptr, &sem2));
    VkImportSemaphoreFdInfoKHR ii{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR};
    ii.semaphore  = sem2;
    ii.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
    ii.fd         = fd;
    ii.flags      = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT;
    VK_TRY(dst.ImportSemaphoreFdKHR(dst.dev, &ii), r);
    if (r != VK_SUCCESS) {
        std::printf("      import failed: %s\n", vkres(r));
        ::close(fd);
        return;
    }
    const auto t1 = std::chrono::steady_clock::now();

    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo         wsi{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    wsi.waitSemaphoreCount = 1;
    wsi.pWaitSemaphores    = &sem2;
    wsi.pWaitDstStageMask  = &stage;
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence           fence;
    VK_CHECK(vkCreateFence(dst.dev, &fci, nullptr, &fence));
    VK_CHECK(vkQueueSubmit(dst.q, 1, &wsi, fence));
    VkResult wr = vkWaitForFences(dst.dev, 1, &fence, VK_TRUE, 10ull * 1000000000ull);
    const auto t2 = std::chrono::steady_clock::now();

    // How long did the src job actually take?
    VK_CHECK(vkQueueWaitIdle(src.q));
    const auto t3 = std::chrono::steady_clock::now();

    const double importMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    const double waitMs   = std::chrono::duration<double, std::milli>(t2 - t0).count();
    const double jobMs    = std::chrono::duration<double, std::milli>(t3 - t0).count();
    std::printf("      src job (%u full-frame copies) took %.1f ms; dst wait returned at %.1f ms "
                "(%s)\n",
                kJob, jobMs, waitMs, vkres(wr));
    std::printf("      import cost %.2f ms; ORDERING %s\n", importMs,
                waitMs > jobMs * 0.8 ? "HONOURED (waiter held for the signaller's work)"
                                     : "*** NOT HONOURED — waiter returned early ***");

    vkDestroyFence(dst.dev, fence, nullptr);
    vkDestroySemaphore(dst.dev, sem2, nullptr);
    vkDestroySemaphore(src.dev, sem, nullptr);
    vkFreeCommandBuffers(src.dev, src.pool, 1, &cb);
    vkDestroyImage(src.dev, a.img, nullptr);
    vkFreeMemory(src.dev, a.mem, nullptr);
    vkDestroyImage(src.dev, b.img, nullptr);
    vkFreeMemory(src.dev, b.mem, nullptr);
}

// ---------------------------------------------------------------------------
// the actual share test
// ---------------------------------------------------------------------------

struct ShareResult {
    bool     ok        = false;
    bool     created   = false;
    bool     exported  = false;
    bool     imported  = false;
    bool     verified  = false;
    uint64_t mod       = 0;
    uint32_t badPixels = 0;
    uint32_t firstBadX = 0, firstBadY = 0;
    uint32_t gotPixel = 0, wantPixel = 0;
    std::string note;
};

static void fillPattern(const Gpu& g, Img& img, uint32_t w, uint32_t h, bool foreignRelease) {
    Buf staging = createHostBuf(g, (VkDeviceSize)w * h * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    uint32_t* p = (uint32_t*)staging.ptr;
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++)
            p[(size_t)y * w + x] = pat(x, y);

    VkCommandBuffer cb = beginCmd(g);
    barrier(cb, img.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
            VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy bic{};
    bic.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bic.imageSubresource.layerCount = 1;
    bic.imageExtent                 = {w, h, 1};
    vkCmdCopyBufferToImage(cb, staging.buf, img.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
    if (foreignRelease)
        barrier(cb, img.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, 0, g.qfi, VK_QUEUE_FAMILY_FOREIGN_EXT);
    endCmdWait(g, cb);

    vkDestroyBuffer(g.dev, staging.buf, nullptr);
    vkUnmapMemory(g.dev, staging.mem);
    vkFreeMemory(g.dev, staging.mem, nullptr);
}

static uint32_t verifyPattern(const Gpu& g, Img& img, uint32_t w, uint32_t h, bool foreignAcquire,
                              ShareResult& res) {
    Buf readback = createHostBuf(g, (VkDeviceSize)w * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);

    VkCommandBuffer cb = beginCmd(g);
    if (foreignAcquire)
        barrier(cb, img.img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0,
                VK_ACCESS_TRANSFER_READ_BIT, VK_QUEUE_FAMILY_FOREIGN_EXT, g.qfi);
    else
        barrier(cb, img.img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy bic{};
    bic.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bic.imageSubresource.layerCount = 1;
    bic.imageExtent                 = {w, h, 1};
    vkCmdCopyImageToBuffer(cb, img.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buf, 1,
                           &bic);
    endCmdWait(g, cb);

    uint32_t* p   = (uint32_t*)readback.ptr;
    uint32_t  bad = 0;
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            const uint32_t want = pat(x, y);
            const uint32_t got  = p[(size_t)y * w + x];
            if (want != got) {
                if (!bad) {
                    res.firstBadX = x;
                    res.firstBadY = y;
                    res.gotPixel  = got;
                    res.wantPixel = want;
                }
                bad++;
            }
        }
    }
    vkDestroyBuffer(g.dev, readback.buf, nullptr);
    vkUnmapMemory(g.dev, readback.mem);
    vkFreeMemory(g.dev, readback.mem, nullptr);
    return bad;
}

static ShareResult tryShare(const Gpu& src, const Gpu& dst, uint32_t w, uint32_t h, VkFormat fmt,
                            const std::vector<uint64_t>& srcMods, VkImageUsageFlags usage) {
    ShareResult res;
    Img         a{}, b{};
    int         fd = -1;

    if (!createExportable(src, w, h, fmt, usage, srcMods, a, &fd)) {
        res.note = "export-side image creation or fd export failed";
        return res;
    }
    res.created  = true;
    res.exported = fd >= 0;
    res.mod      = a.mod;
    std::printf("\n        export ok: mod=%s size=%.2f MiB src-mem=%s fd=%d\n",
                modName(a.mod).c_str(), a.size / 1048576.0,
                memTypeDesc(src, a.memTypeIdx).c_str(), fd);
    if (a.mod != 0 && !srcMods.empty() && srcMods[0] != a.mod)
        std::printf("        NOTE: driver returned a DIFFERENT modifier than requested\n");
    for (size_t i = 0; i < a.layouts.size(); i++)
        std::printf("        plane[%zu] offset=%" PRIu64 " rowPitch=%" PRIu64 " size=%" PRIu64
                    "\n",
                    i, a.layouts[i].offset, a.layouts[i].rowPitch, a.layouts[i].size);

    fillPattern(src, a, w, h, /*foreignRelease=*/true);

    if (!importImage(dst, w, h, fmt, usage, a.mod, a.layouts, fd, b)) {
        res.note = "import into dst failed";
        vkDestroyImage(src.dev, a.img, nullptr);
        vkFreeMemory(src.dev, a.mem, nullptr);
        if (fd >= 0)
            ::close(fd);
        return res;
    }
    res.imported = true;
    std::printf("        import ok: dst-mem=%s\n        result: ",
                memTypeDesc(dst, b.memTypeIdx).c_str());

    res.badPixels = verifyPattern(dst, b, w, h, /*foreignAcquire=*/true, res);
    res.verified  = res.badPixels == 0;
    res.ok        = res.verified;

    vkDestroyImage(dst.dev, b.img, nullptr);
    vkFreeMemory(dst.dev, b.mem, nullptr);
    vkDestroyImage(src.dev, a.img, nullptr);
    vkFreeMemory(src.dev, a.mem, nullptr);
    return res;
}

// ---------------------------------------------------------------------------
// benchmarks
// ---------------------------------------------------------------------------

// One-time queue-family ownership acquire of a dma-buf image from VK_QUEUE_FAMILY_FOREIGN_EXT.
static void acquireForeign(const Gpu& g, VkImage img) {
    VkCommandBuffer cb = beginCmd(g);
    barrier(cb, img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0,
            VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_QUEUE_FAMILY_FOREIGN_EXT,
            g.qfi);
    endCmdWait(g, cb);
}

static double benchImageCopy(const Gpu& g, VkImage srcImg, VkImageLayout srcLayout, VkImage dstImg,
                             uint32_t w, uint32_t h, uint32_t iters, bool srcForeign) {
    // Pre-transition dst once.
    {
        VkCommandBuffer cb = beginCmd(g);
        barrier(cb, dstImg, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                VK_ACCESS_TRANSFER_WRITE_BIT);
        endCmdWait(g, cb);
    }
    VkImageCopy ic{};
    ic.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ic.srcSubresource.layerCount = 1;
    ic.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ic.dstSubresource.layerCount = 1;
    ic.extent                    = {w, h, 1};

    // warmup + timed
    double best = 1e30;
    for (uint32_t pass = 0; pass < 3; pass++) {
        VkCommandBuffer cb = beginCmd(g);
        if (srcForeign)
            barrier(cb, srcImg, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0,
                    VK_ACCESS_TRANSFER_READ_BIT, VK_QUEUE_FAMILY_FOREIGN_EXT, g.qfi);
        else
            barrier(cb, srcImg, srcLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        for (uint32_t i = 0; i < iters; i++) {
            vkCmdCopyImage(cb, srcImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dstImg,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
            VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr, 0,
                                 nullptr);
        }
        // put src back so the next pass's barrier is legal
        barrier(cb, srcImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_WRITE_BIT);

        VK_CHECK(vkEndCommandBuffer(cb));
        auto         t0 = std::chrono::steady_clock::now();
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers    = &cb;
        VK_CHECK(vkQueueSubmit(g.q, 1, &si, VK_NULL_HANDLE));
        VK_CHECK(vkQueueWaitIdle(g.q));
        auto t1 = std::chrono::steady_clock::now();
        vkFreeCommandBuffers(g.dev, g.pool, 1, &cb);
        if (pass == 0)
            continue;  // warmup
        const double ms =
            std::chrono::duration<double, std::milli>(t1 - t0).count() / (double)iters;
        best = std::min(best, ms);
        srcForeign = false;  // after first acquire it's ours
    }
    return best;
}

// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);  // keep stdout/stderr interleaving readable
    setvbuf(stderr, nullptr, _IOLBF, 0);
    uint32_t W = 2064, H = 2208, ITERS = 20;
    int      fmtSel = -1;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--width") && i + 1 < argc)
            W = (uint32_t)std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--height") && i + 1 < argc)
            H = (uint32_t)std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--iters") && i + 1 < argc)
            ITERS = (uint32_t)std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--format") && i + 1 < argc)
            fmtSel = std::atoi(argv[++i]);
    }

    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    ai.pApplicationName = "xgpu_dmabuf";
    ai.apiVersion       = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &ai;
    VkInstance inst;
    VK_CHECK(vkCreateInstance(&ici, nullptr, &inst));

    uint32_t n = 0;
    vkEnumeratePhysicalDevices(inst, &n, nullptr);
    std::vector<VkPhysicalDevice> pds(n);
    vkEnumeratePhysicalDevices(inst, &n, pds.data());

    std::printf("=== 1. physical devices (%u) ===\n", n);
    std::vector<Gpu> gpus(n);
    for (uint32_t i = 0; i < n; i++) {
        initGpu(inst, pds[i], gpus[i]);
        printGpu(gpus[i]);
    }
    if (n < 2) {
        std::printf("\nFATAL: need >= 2 physical devices; check VK_DRIVER_FILES/VK_ICD_FILENAMES "
                    "is not pinning one ICD.\n");
        return 2;
    }

    // Identify NVIDIA / AMD
    int nv = -1, amd = -1;
    for (uint32_t i = 0; i < n; i++) {
        if (gpus[i].props.vendorID == 0x10de && nv < 0)
            nv = (int)i;
        if (gpus[i].props.vendorID == 0x1002 && amd < 0)
            amd = (int)i;
    }
    std::printf("\n  NVIDIA index=%d  AMD index=%d\n", nv, amd);
    if (nv < 0 || amd < 0) {
        std::printf("FATAL: expected one NVIDIA and one AMD device\n");
        return 2;
    }
    Gpu& NV = gpus[nv];
    Gpu& AMD = gpus[amd];

    std::printf("\n=== 2. external sync capabilities ===\n");
    for (Gpu* g : {&NV, &AMD}) {
        std::printf("  %s:\n", g->tag());
        probeSync(*g);
    }

    const VkImageUsageFlags kUsage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                     VK_IMAGE_USAGE_SAMPLED_BIT |
                                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    std::vector<VkFormat> formats = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB,
                                     VK_FORMAT_B8G8R8A8_UNORM,
                                     VK_FORMAT_A2B10G10R10_UNORM_PACK32};
    if (fmtSel >= 0 && fmtSel < (int)formats.size())
        formats = {formats[fmtSel]};

    std::printf("\n=== 3. DRM format modifier lists (usage = TRANSFER_SRC|DST|SAMPLED|COLOR_ATT) "
                "===\n");
    for (VkFormat f : formats) {
        dumpModifiers(NV, f, kUsage);
        dumpModifiers(AMD, f, kUsage);
        std::printf("\n");
    }

    std::printf("=== 4. cross-vendor semaphore handoff ===\n");
    trySemaphoreHandoff(NV, AMD);
    trySemaphoreHandoff(AMD, NV);
    std::printf("\n  --- does the cross-vendor wait actually ORDER anything? ---\n");
    semOrderingTest(NV, AMD, 2048, 2048);
    semOrderingTest(AMD, NV, 2048, 2048);

    std::printf("\n=== 5. dma-buf image share (%ux%u) ===\n", W, H);
    for (VkFormat f : formats) {
        for (int dir = 0; dir < 2; dir++) {
            Gpu& S = dir == 0 ? NV : AMD;
            Gpu& D = dir == 0 ? AMD : NV;
            std::printf("\n  --- %s: %s --> %s ---\n", fmtName(f), S.tag(), D.tag());

            auto sm = queryModifiers(S, f, kUsage);
            auto dm = queryModifiers(D, f, kUsage);
            if (sm.empty() || dm.empty()) {
                std::printf("      SKIP: modifier list empty on one side (src=%zu dst=%zu)\n",
                            sm.size(), dm.size());
                continue;
            }

            // (a) intersection: exportable on src AND importable on dst
            std::vector<uint64_t> both;
            for (auto& a : sm) {
                if (!a.exportable)
                    continue;
                for (auto& b : dm)
                    if (b.mod == a.mod && b.importable)
                        both.push_back(a.mod);
            }
            std::printf("      modifier intersection (src-exportable AND dst-importable): %zu\n",
                        both.size());
            for (uint64_t m : both)
                std::printf("        %s\n", modName(m).c_str());

            // (b) try every intersecting modifier, best (non-linear) first
            std::vector<uint64_t> order = both;
            std::sort(order.begin(), order.end(), [](uint64_t a, uint64_t b) {
                if ((a == 0) != (b == 0))
                    return b == 0;  // non-linear first
                return a < b;
            });

            bool anyOk = false;
            for (uint64_t m : order) {
                std::printf("      TRY modifier %s ... ", modName(m).c_str());
                std::fflush(stdout);
                ShareResult r = tryShare(S, D, W, H, f, {m}, kUsage);
                if (r.ok) {
                    std::printf("OK  (all %u px match)\n", W * H);
                    anyOk = true;
                } else if (r.imported) {
                    std::printf("IMPORTED but %u/%u px WRONG (first bad (%u,%u) got=%08x "
                                "want=%08x)\n",
                                r.badPixels, W * H, r.firstBadX, r.firstBadY, r.gotPixel,
                                r.wantPixel);
                } else {
                    std::printf("FAIL (%s)\n", r.note.c_str());
                }
            }
            if (order.empty())
                std::printf("      no intersecting modifier to try\n");

            // (c) LINEAR-only fallback, even if not in the "intersection" (drivers sometimes
            //     under-report import caps but accept the import anyway).
            bool linearInBoth = false;
            for (uint64_t m : both)
                if (m == 0)
                    linearInBoth = true;
            if (!linearInBoth) {
                std::printf("      FALLBACK forced DRM_FORMAT_MOD_LINEAR ... ");
                std::fflush(stdout);
                ShareResult r = tryShare(S, D, W, H, f, {0}, kUsage);
                if (r.ok)
                    std::printf("OK\n");
                else if (r.imported)
                    std::printf("IMPORTED but %u px wrong\n", r.badPixels);
                else
                    std::printf("FAIL (%s)\n", r.note.c_str());
                anyOk = anyOk || r.ok;
            }
            (void)anyOk;
        }
    }

    // -----------------------------------------------------------------------
    // 5b. Row-pitch alignment probe.
    //
    // The NVIDIA->AMD LINEAR import fails with INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT even
    // though both drivers advertise LINEAR as exportable+importable. Hypothesis: NVIDIA hands
    // back a TIGHT rowPitch (width*bpp) while RADV requires the linear stride to be aligned.
    // Sweep widths and print each side's natural pitch to prove or kill it, then test the
    // padded-width workaround.
    std::printf("\n=== 5b. LINEAR row-pitch alignment probe (why NVIDIA->AMD fails) ===\n");
    {
        const VkFormat f = VK_FORMAT_R8G8B8A8_UNORM;
        std::printf("  width  NV pitch  AMD pitch  NV->AMD import\n");
        const uint32_t widths[] = {1832, 1920, 2016, 2048, 2064, 2080, 2112, 2144, 2160, 2560};
        for (uint32_t w : widths) {
            uint64_t nvPitch = 0, amdPitch = 0;
            Img      na{}, aa{};
            int      nfd = -1;
            if (createExportable(NV, w, H, f, kUsage, {0}, na, &nfd) && !na.layouts.empty())
                nvPitch = na.layouts[0].rowPitch;
            if (createExportable(AMD, w, H, f, kUsage, {0}, aa, nullptr) && !aa.layouts.empty())
                amdPitch = aa.layouts[0].rowPitch;

            const char* verdict = "n/a";
            if (na.img && nfd >= 0) {
                Img imp{};
                if (importImage(AMD, w, H, f, kUsage, 0, na.layouts, nfd, imp)) {
                    verdict = "OK";
                    vkDestroyImage(AMD.dev, imp.img, nullptr);
                    vkFreeMemory(AMD.dev, imp.mem, nullptr);
                } else {
                    verdict = "REJECTED";
                    ::close(nfd);
                }
            }
            std::printf("  %-6u %-9" PRIu64 " %-10" PRIu64 " %s%s\n", w, nvPitch, amdPitch,
                        verdict,
                        (nvPitch && amdPitch && nvPitch == amdPitch) ? "   (pitches match)" : "");
            if (na.img) {
                vkDestroyImage(NV.dev, na.img, nullptr);
                vkFreeMemory(NV.dev, na.mem, nullptr);
            }
            if (aa.img) {
                vkDestroyImage(AMD.dev, aa.img, nullptr);
                vkFreeMemory(AMD.dev, aa.mem, nullptr);
            }
        }

        // The workaround: allocate on NVIDIA at a PADDED width so its tight pitch equals the
        // pitch RADV wants, import the padded image on AMD, and treat the left W columns as
        // the real content.
        std::printf("\n  padded-width workaround for %ux%u:\n", W, H);
        Img probe{};
        uint64_t wantPitch = 0;
        if (createExportable(AMD, W, H, VK_FORMAT_R8G8B8A8_UNORM, kUsage, {0}, probe, nullptr) &&
            !probe.layouts.empty())
            wantPitch = probe.layouts[0].rowPitch;
        if (probe.img) {
            vkDestroyImage(AMD.dev, probe.img, nullptr);
            vkFreeMemory(AMD.dev, probe.mem, nullptr);
        }
        const uint32_t padW = wantPitch ? (uint32_t)(wantPitch / 4) : W;
        std::printf("    AMD wants rowPitch=%" PRIu64 " for width %u  ->  padded width = %u "
                    "(+%u px, +%.1f%% memory)\n",
                    wantPitch, W, padW, padW - W, 100.0 * (padW - W) / W);
        if (padW != W) {
            ShareResult r =
                tryShare(NV, AMD, padW, H, VK_FORMAT_R8G8B8A8_UNORM, {0}, kUsage);
            std::printf("    padded NVIDIA->AMD share: %s\n",
                        r.ok ? "OK (pixel-exact)"
                             : (r.imported ? "imported but pixels wrong" : r.note.c_str()));
        }
    }

    // -----------------------------------------------------------------------
    std::printf("\n=== 6. cost of the shapes at %ux%u (per-eye; VR needs 2 of these @90Hz) ===\n",
                W, H);
    {
        const VkFormat f = VK_FORMAT_R8G8B8A8_UNORM;
        // Baseline: same-device OPTIMAL->OPTIMAL copy on each GPU (the "zero-copy would avoid
        // this" reference).
        for (Gpu* g : {&NV, &AMD}) {
            Img a = createLocal(*g, W, H, f, VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                                 VK_IMAGE_USAGE_TRANSFER_DST_BIT);
            Img b = createLocal(*g, W, H, f, VK_IMAGE_USAGE_TRANSFER_DST_BIT);
            {
                VkCommandBuffer cb = beginCmd(*g);
                barrier(cb, a.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0,
                        VK_ACCESS_MEMORY_WRITE_BIT);
                endCmdWait(*g, cb);
            }
            double ms = benchImageCopy(*g, a.img, VK_IMAGE_LAYOUT_GENERAL, b.img, W, H, ITERS,
                                       false);
            std::printf("  local OPTIMAL->OPTIMAL copy on %-40s %7.3f ms (%.1f GB/s eff)\n",
                        g->tag(), ms, (double)W * H * 4 * 2 / (ms * 1e-3) / 1e9);
            vkDestroyImage(g->dev, a.img, nullptr);
            vkFreeMemory(g->dev, a.mem, nullptr);
            vkDestroyImage(g->dev, b.img, nullptr);
            vkFreeMemory(g->dev, b.mem, nullptr);
        }

        // What forcing LINEAR costs the COMPOSITOR on its own GPU: today the server allocates
        // swapchain images VK_IMAGE_TILING_OPTIMAL; a cross-GPU-shareable swapchain must be
        // LINEAR. Measure the compositor GPU reading each.
        for (Gpu* g : {&AMD, &NV}) {
            for (int lin = 0; lin < 2; lin++) {
                Img src{};
                if (lin) {
                    if (!createExportable(*g, W, H, f, kUsage, {0}, src, nullptr))
                        continue;
                    acquireForeign(*g, src.img);
                } else {
                    src = createLocal(*g, W, H, f,
                                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                          VK_IMAGE_USAGE_TRANSFER_DST_BIT);
                    if (!src.img)
                        continue;
                    VkCommandBuffer cb = beginCmd(*g);
                    barrier(cb, src.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0,
                            VK_ACCESS_MEMORY_WRITE_BIT);
                    endCmdWait(*g, cb);
                }
                Img dst = createLocal(*g, W, H, f, VK_IMAGE_USAGE_TRANSFER_DST_BIT);
                if (dst.img) {
                    double ms = benchImageCopy(*g, src.img, VK_IMAGE_LAYOUT_GENERAL, dst.img, W,
                                               H, ITERS, false);
                    std::printf("  compositor reads own %-7s swapchain image on %-40s %7.3f ms\n",
                                lin ? "LINEAR" : "OPTIMAL", g->tag(), ms);
                    vkDestroyImage(g->dev, dst.img, nullptr);
                    vkFreeMemory(g->dev, dst.mem, nullptr);
                }
                vkDestroyImage(g->dev, src.img, nullptr);
                vkFreeMemory(g->dev, src.mem, nullptr);
            }
        }

        // THE REAL SHAPE. Monado's server allocates the swapchain images and exports them;
        // the client imports and RENDERS INTO them. So for "compositor on AMD, game on NVIDIA"
        // the buffer is AMD-allocated, and the per-frame cost is NVIDIA *writing* into it
        // (the game's blit at xrReleaseSwapchainImage) plus AMD *reading* it at composite.
        std::printf("\n  --- the real shape: server(exporter) allocates, client(importer) renders "
                    "into it ---\n");
        for (int dir = 0; dir < 2; dir++) {
            Gpu& SRV = dir == 0 ? AMD : NV;  // allocates + composites (the WiVRn server)
            Gpu& CLI = dir == 0 ? NV : AMD;  // imports + renders (the game)

            // Pitch-match if the importer is picky (see 5b).
            uint32_t BW = W;
            {
                Img probe{};
                if (createExportable(SRV, W, H, f, kUsage, {0}, probe, nullptr) &&
                    !probe.layouts.empty()) {
                    // importer needs its own natural pitch to agree
                    Img cprobe{};
                    if (createExportable(CLI, W, H, f, kUsage, {0}, cprobe, nullptr) &&
                        !cprobe.layouts.empty() &&
                        cprobe.layouts[0].rowPitch != probe.layouts[0].rowPitch)
                        BW = (W + 63) & ~63u;
                    if (cprobe.img) {
                        vkDestroyImage(CLI.dev, cprobe.img, nullptr);
                        vkFreeMemory(CLI.dev, cprobe.mem, nullptr);
                    }
                }
                if (probe.img) {
                    vkDestroyImage(SRV.dev, probe.img, nullptr);
                    vkFreeMemory(SRV.dev, probe.mem, nullptr);
                }
            }

            Img shared{}, cliView{};
            int fd = -1;
            if (!createExportable(SRV, BW, H, f, kUsage, {0}, shared, &fd)) {
                std::printf("  server=%s: LINEAR export failed\n", SRV.tag());
                continue;
            }
            {
                VkCommandBuffer cb = beginCmd(SRV);
                barrier(cb, shared.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0,
                        VK_ACCESS_MEMORY_WRITE_BIT, SRV.qfi, VK_QUEUE_FAMILY_FOREIGN_EXT);
                endCmdWait(SRV, cb);
            }
            if (!importImage(CLI, BW, H, f, kUsage, shared.mod, shared.layouts, fd, cliView)) {
                std::printf("  server=%s client=%s: IMPORT FAILED (w=%u)\n", SRV.tag(), CLI.tag(),
                            BW);
                vkDestroyImage(SRV.dev, shared.img, nullptr);
                vkFreeMemory(SRV.dev, shared.mem, nullptr);
                continue;
            }
            acquireForeign(CLI, cliView.img);

            std::printf("  server=%-22s client=%-22s (w=%u, buffer in %s)\n", SRV.tag(), CLI.tag(),
                        BW, memTypeDesc(SRV, shared.memTypeIdx).c_str());

            // client writes into the shared image (the game's release-time blit)
            Img cliLocal = createLocal(CLI, BW, H, f, VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                                          VK_IMAGE_USAGE_TRANSFER_DST_BIT);
            if (cliLocal.img) {
                VkCommandBuffer cb = beginCmd(CLI);
                barrier(cb, cliLocal.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0,
                        VK_ACCESS_MEMORY_WRITE_BIT);
                endCmdWait(CLI, cb);
                double ms = benchImageCopy(CLI, cliLocal.img, VK_IMAGE_LAYOUT_GENERAL,
                                           cliView.img, BW, H, ITERS, false);
                std::printf("    CLIENT writes into shared image   %7.3f ms/eye  "
                            "(%.1f ms both eyes = %.0f%% of an 11.1 ms frame)\n",
                            ms, ms * 2, ms * 2 / 11.1 * 100.0);
                vkDestroyImage(CLI.dev, cliLocal.img, nullptr);
                vkFreeMemory(CLI.dev, cliLocal.mem, nullptr);
            }
            // server reads it back at composite time
            Img srvLocal = createLocal(SRV, BW, H, f, VK_IMAGE_USAGE_TRANSFER_DST_BIT);
            if (srvLocal.img) {
                double ms = benchImageCopy(SRV, shared.img, VK_IMAGE_LAYOUT_GENERAL, srvLocal.img,
                                           BW, H, ITERS, true);
                std::printf("    SERVER reads shared image         %7.3f ms/eye  "
                            "(%.1f ms both eyes = %.0f%% of an 11.1 ms frame)\n",
                            ms, ms * 2, ms * 2 / 11.1 * 100.0);
                vkDestroyImage(SRV.dev, srvLocal.img, nullptr);
                vkFreeMemory(SRV.dev, srvLocal.mem, nullptr);
            }

            vkDestroyImage(CLI.dev, cliView.img, nullptr);
            vkFreeMemory(CLI.dev, cliView.mem, nullptr);
            vkDestroyImage(SRV.dev, shared.img, nullptr);
            vkFreeMemory(SRV.dev, shared.mem, nullptr);
        }
    }

    std::printf("\ndone.\n");
    return 0;
}
