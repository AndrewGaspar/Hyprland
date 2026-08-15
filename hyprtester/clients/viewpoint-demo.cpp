// Native synthetic client for the experimental hypxr_viewpoint_v1 protocol.
// It renders a fixed room through a head-tracked physical portal without any
// OpenXR dependency or connection of its own.

#include "PortalRenderer.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <format>
#include <fstream>
#include <limits>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

#include <hypxr-viewpoint-v1.hpp>
#include <viewporter.hpp>
#include <wayland-client.h>
#include <wayland.hpp>
#include <xdg-shell.hpp>
#include <xdg-toplevel-tag-v1.hpp>

#include <hyprutils/memory/Casts.hpp>
#include <hyprutils/memory/SharedPtr.hpp>
#include <hyprutils/memory/UniquePtr.hpp>

using namespace Hyprutils::Memory;
using namespace ViewpointDemo;

struct SOptions {
    uint32_t                   renderWidth  = 256;
    uint32_t                   renderHeight = 144;
    bool                       fullscreen   = true;
    bool                       once         = false;
    bool                       debug        = false;
    std::optional<std::string> renderPath;
    bool                       renderFallback = false;
};

struct SSample {
    uint64_t     epoch    = 0;
    uint64_t     sample   = 0;
    uint64_t     geometry = 0;
    SStereoViews views;
};

struct SBuffer {
    ~SBuffer();

    CSharedPointer<CCWlBuffer> resource;
    void*                      mapping = nullptr;
    size_t                     size    = 0;
    uint32_t                   width   = 0;
    uint32_t                   height  = 0;
    uint32_t                   stride  = 0;
    bool                       busy    = false;
};

struct SApp {
    wl_display*                               display = nullptr;
    SOptions                                  options;

    CSharedPointer<CCWlRegistry>              registry;
    CSharedPointer<CCWlCompositor>            compositor;
    CSharedPointer<CCWlShm>                   shm;
    CSharedPointer<CCXdgWmBase>               xdgWmBase;
    CSharedPointer<CCXdgToplevelTagManagerV1> tagManager;
    CSharedPointer<CCWpViewporter>            viewporter;
    CSharedPointer<CCHypxrViewpointManagerV1> viewpointManager;

    CSharedPointer<CCWlSurface>               surface;
    CSharedPointer<CCXdgSurface>              xdgSurface;
    CSharedPointer<CCXdgToplevel>             xdgToplevel;
    CSharedPointer<CCWpViewport>              viewport;
    CSharedPointer<CCHypxrViewpointV1>        viewpoint;
    std::vector<CUniquePointer<SBuffer>>      buffers;

    bool                                      xrgb8888      = false;
    bool                                      configured    = false;
    bool                                      running       = true;
    bool                                      fallbackDirty = true;
    bool                                      active        = false;
    SFeedbackState                            feedback;
    bool                                      feedbackRequested = false;
    uint32_t                                  pendingWidth      = 0;
    uint32_t                                  pendingHeight     = 0;
    uint32_t                                  logicalWidth      = 0;
    uint32_t                                  logicalHeight     = 0;
    uint32_t                                  renderWidth       = 0;
    uint32_t                                  renderHeight      = 0;
    uint64_t                                  epoch             = 0;
    uint64_t                                  geometry          = 0;
    uint64_t                                  lastSample        = 0;
    SPortalSize                               portal;
    std::optional<SSample>                    pendingSample;
    uint64_t                                  renderedFrames = 0;
};

SBuffer::~SBuffer() {
    resource.reset();
    if (mapping)
        munmap(mapping, size);
}

template <typename... Args>
// NOLINTNEXTLINE
static void logLine(std::format_string<Args...> format, Args&&... args) {
    std::println("{}", std::format(format, std::forward<Args>(args)...));
    std::fflush(stdout);
}

template <typename... Args>
// NOLINTNEXTLINE
static void debugLine(const SApp& app, std::format_string<Args...> format, Args&&... args) {
    if (app.options.debug)
        logLine(format, std::forward<Args>(args)...);
}

static uint64_t combineWords(uint32_t high, uint32_t low) {
    return (sc<uint64_t>(high) << 32U) | low;
}

static uint32_t highWord(uint64_t value) {
    return sc<uint32_t>(value >> 32U);
}

static uint32_t lowWord(uint64_t value) {
    return sc<uint32_t>(value & 0xFFFFFFFFULL);
}

static bool parseDimension(std::string_view value, uint32_t& out) {
    uint32_t parsed         = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || end != value.data() + value.size() || parsed < 64 || parsed > 4096)
        return false;
    out = parsed;
    return true;
}

static void printHelp(const char* program) {
    std::println("Usage: {} [OPTIONS]\n"
                 "\n"
                 "Synthetic full-SBS portal for the experimental hypxr_viewpoint_v1 protocol.\n"
                 "The logical surface is the packed SBS rectangle; each buffer pane is aspect-\n"
                 "matched to one half of that destination.\n"
                 "\n"
                 "  --width N      render/one-eye width, 64..4096 (default 256)\n"
                 "  --height N     render/one-eye height, 64..4096 (default 144)\n"
                 "  --windowed     create a resizable window instead of requesting fullscreen\n"
                 "  --once         exit after the first viewpoint-associated frame\n"
                 "  --debug        print activation, coalescing, and frame identifiers\n"
                 "  --render FILE  write a deterministic active full-SBS PPM without Wayland\n"
                 "  --render-fallback FILE\n"
                 "                 write the inactive zero-disparity PPM without Wayland\n"
                 "                 (offline output is 2*width by height; default 512x144)\n"
                 "  -h, --help     show this help\n",
                 program);
}

static std::optional<SOptions> parseOptions(int argc, char** argv) {
    SOptions options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view ARG = argv[index];
        if (ARG == "-h" || ARG == "--help") {
            printHelp(argv[0]);
            return std::nullopt;
        }
        if (ARG == "--windowed") {
            options.fullscreen = false;
            continue;
        }
        if (ARG == "--once") {
            options.once = true;
            continue;
        }
        if (ARG == "--debug") {
            options.debug = true;
            continue;
        }
        if ((ARG == "--render" || ARG == "--render-fallback") && index + 1 < argc) {
            options.renderPath     = argv[++index];
            options.renderFallback = ARG == "--render-fallback";
            continue;
        }
        if ((ARG == "--width" || ARG == "--height") && index + 1 < argc) {
            uint32_t& destination = ARG == "--width" ? options.renderWidth : options.renderHeight;
            if (!parseDimension(argv[++index], destination)) {
                logLine("error: {} must be an integer from 64 through 4096", ARG);
                return std::nullopt;
            }
            continue;
        }

        logLine("error: unknown or incomplete option '{}'", ARG);
        printHelp(argv[0]);
        return std::nullopt;
    }
    return options;
}

static bool roundtrip(SApp& app) {
    return wl_display_roundtrip(app.display) >= 0;
}

static bool bindGlobals(SApp& app) {
    app.registry = makeShared<CCWlRegistry>(rc<wl_proxy*>(wl_display_get_registry(app.display)));
    app.registry->setGlobal([&app](CCWlRegistry* registry, uint32_t name, const char* interface, uint32_t version) {
        const std::string INTERFACE = interface;
        auto*             raw       = rc<wl_registry*>(registry->resource());
        if (INTERFACE == "wl_compositor")
            app.compositor = makeShared<CCWlCompositor>(rc<wl_proxy*>(wl_registry_bind(raw, name, &wl_compositor_interface, std::min(version, 6U))));
        else if (INTERFACE == "wl_shm") {
            app.shm = makeShared<CCWlShm>(rc<wl_proxy*>(wl_registry_bind(raw, name, &wl_shm_interface, std::min(version, 1U))));
            app.shm->setFormat([&app](CCWlShm*, uint32_t format) {
                if (format == WL_SHM_FORMAT_XRGB8888)
                    app.xrgb8888 = true;
            });
        } else if (INTERFACE == "xdg_wm_base")
            app.xdgWmBase = makeShared<CCXdgWmBase>(rc<wl_proxy*>(wl_registry_bind(raw, name, &xdg_wm_base_interface, std::min(version, 1U))));
        else if (INTERFACE == "xdg_toplevel_tag_manager_v1")
            app.tagManager = makeShared<CCXdgToplevelTagManagerV1>(rc<wl_proxy*>(wl_registry_bind(raw, name, &xdg_toplevel_tag_manager_v1_interface, std::min(version, 1U))));
        else if (INTERFACE == "wp_viewporter")
            app.viewporter = makeShared<CCWpViewporter>(rc<wl_proxy*>(wl_registry_bind(raw, name, &wp_viewporter_interface, std::min(version, 1U))));
        else if (INTERFACE == "hypxr_viewpoint_manager_v1")
            app.viewpointManager = makeShared<CCHypxrViewpointManagerV1>(rc<wl_proxy*>(wl_registry_bind(raw, name, &hypxr_viewpoint_manager_v1_interface, std::min(version, 1U))));
    });
    app.registry->setGlobalRemove([&app](CCWlRegistry*, uint32_t name) { debugLine(app, "global {} removed", name); });

    if (!roundtrip(app))
        return false;
    if (!roundtrip(app))
        return false;

    return app.compositor && app.shm && app.xdgWmBase && app.tagManager && app.viewporter && app.viewpointManager && app.xrgb8888;
}

static void invalidateFeedback(SApp& app, std::string_view reason) {
    logLine("viewpoint inactive fallback: {}", reason);
    app.active = false;
    app.pendingSample.reset();
    app.fallbackDirty = true;
}

static void updateFeedbackRequest(SApp& app) {
    const bool ENABLED = feedbackShouldBeEnabled(app.feedback);
    if (!app.viewpoint || ENABLED == app.feedbackRequested)
        return;

    app.feedbackRequested = ENABLED;
    app.viewpoint->sendSetEnabled(ENABLED ? 1U : 0U);
}

static void rejectFeedback(SApp& app, std::string_view reason) {
    invalidateFeedback(app, reason);
    app.feedback.stickyDisabled = true;
    updateFeedbackRequest(app);
}

static void installViewpointHandlers(SApp& app) {
    app.viewpoint->setCapabilities([&app](CCHypxrViewpointV1*, hypxrViewpointV1Layout layouts, hypxrViewpointV1Capability flags) {
        debugLine(app, "compositor capabilities layouts={:#x}, flags={:#x}", sc<uint32_t>(layouts), sc<uint32_t>(flags));
        app.viewpoint->sendSetCapabilities(HYPXR_VIEWPOINT_V1_LAYOUT_SBS, HYPXR_VIEWPOINT_V1_CAPABILITY_PAIR_LATCHED);
        const bool SBS          = (sc<uint32_t>(layouts) & sc<uint32_t>(HYPXR_VIEWPOINT_V1_LAYOUT_SBS)) != 0;
        const bool PAIR_LATCHED = (sc<uint32_t>(flags) & sc<uint32_t>(HYPXR_VIEWPOINT_V1_CAPABILITY_PAIR_LATCHED)) != 0;
        if (!SBS || !PAIR_LATCHED) {
            app.feedback.capabilitiesSupported = false;
            app.feedback.stickyDisabled        = true;
            updateFeedbackRequest(app);
            logLine("viewpoint SBS + pair-latched feedback unavailable; remaining on static zero-disparity fallback");
            return;
        }
        app.feedback.capabilitiesSupported = true;
        updateFeedbackRequest(app);
        if (!app.feedback.mappingSupported)
            debugLine(app, "viewpoint capability ready; waiting for an eligible final configure");
    });
    app.viewpoint->setActive([&app](CCHypxrViewpointV1*, uint32_t epochHigh, uint32_t epochLow, uint32_t geometryHigh, uint32_t geometryLow, uint32_t widthUm, uint32_t heightUm,
                                    hypxrViewpointV1Layout layout, hypxrViewpointV1Capability flags) {
        if (!feedbackShouldBeEnabled(app.feedback)) {
            invalidateFeedback(app, "ignored activation while feedback was deferred or disabled");
            return;
        }
        const bool PAIR_LATCHED = (sc<uint32_t>(flags) & sc<uint32_t>(HYPXR_VIEWPOINT_V1_CAPABILITY_PAIR_LATCHED)) != 0;
        if (widthUm == 0 || heightUm == 0 || layout != HYPXR_VIEWPOINT_V1_LAYOUT_SBS || !PAIR_LATCHED) {
            rejectFeedback(app, "unsupported activation contract; feedback disabled");
            return;
        }

        app.epoch      = combineWords(epochHigh, epochLow);
        app.geometry   = combineWords(geometryHigh, geometryLow);
        app.portal     = {.widthMeters = sc<double>(widthUm) / 1'000'000.0, .heightMeters = sc<double>(heightUm) / 1'000'000.0};
        app.lastSample = 0;
        app.active     = app.epoch != 0 && app.geometry != 0;
        app.pendingSample.reset();
        app.fallbackDirty = true;
        debugLine(app, "active epoch={} geometry={} portal={:.3f}m x {:.3f}m", app.epoch, app.geometry, app.portal.widthMeters, app.portal.heightMeters);
    });
    app.viewpoint->setSample([&app](CCHypxrViewpointV1*, uint32_t epochHigh, uint32_t epochLow, uint32_t sampleHigh, uint32_t sampleLow, uint32_t geometryHigh,
                                    uint32_t geometryLow, uint32_t, uint32_t, uint32_t, uint32_t, int32_t leftX, int32_t leftY, int32_t leftZ, int32_t rightX, int32_t rightY,
                                    int32_t rightZ, uint32_t viewCount, hypxrViewpointV1SampleFlag flags) {
        if (app.feedback.stickyDisabled || !app.feedback.capabilitiesSupported || !app.feedback.mappingSupported)
            return;
        const uint64_t EPOCH     = combineWords(epochHigh, epochLow);
        const uint64_t SAMPLE    = combineWords(sampleHigh, sampleLow);
        const uint64_t GEOMETRY  = combineWords(geometryHigh, geometryLow);
        const bool     POSITIONS = (sc<uint32_t>(flags) & sc<uint32_t>(HYPXR_VIEWPOINT_V1_SAMPLE_FLAG_POSITIONS_VALID)) != 0;
        if (!app.active || EPOCH != app.epoch || GEOMETRY != app.geometry || SAMPLE == 0 || SAMPLE <= app.lastSample || viewCount != 2 || !POSITIONS || leftZ <= 0 || rightZ <= 0) {
            rejectFeedback(app, "invalid or stale sample; feedback disabled");
            return;
        }

        app.lastSample    = SAMPLE;
        app.pendingSample = {
            .epoch    = EPOCH,
            .sample   = SAMPLE,
            .geometry = GEOMETRY,
            .views =
                {
                    .left  = {.x = sc<double>(leftX) / 1'000'000.0, .y = sc<double>(leftY) / 1'000'000.0, .z = sc<double>(leftZ) / 1'000'000.0},
                    .right = {.x = sc<double>(rightX) / 1'000'000.0, .y = sc<double>(rightY) / 1'000'000.0, .z = sc<double>(rightZ) / 1'000'000.0},
                },
        };
        debugLine(app, "sample {} coalesced for epoch {}", SAMPLE, EPOCH);
    });
    app.viewpoint->setInactive([&app](CCHypxrViewpointV1*, uint32_t epochHigh, uint32_t epochLow, hypxrViewpointV1InactiveReason reason) {
        debugLine(app, "inactive epoch={} reason={}", combineWords(epochHigh, epochLow), sc<uint32_t>(reason));
        invalidateFeedback(app, "compositor inactive event");
    });
}

static bool setupSurface(SApp& app) {
    app.xdgWmBase->setPing([&app](CCXdgWmBase*, uint32_t serial) { app.xdgWmBase->sendPong(serial); });
    app.surface = makeShared<CCWlSurface>(app.compositor->sendCreateSurface());
    if (!app.surface || !app.surface->resource())
        return false;

    app.xdgSurface  = makeShared<CCXdgSurface>(app.xdgWmBase->sendGetXdgSurface(app.surface->resource()));
    app.xdgToplevel = makeShared<CCXdgToplevel>(app.xdgSurface->sendGetToplevel());
    app.viewport    = makeShared<CCWpViewport>(app.viewporter->sendGetViewport(app.surface->resource()));
    app.viewpoint   = makeShared<CCHypxrViewpointV1>(app.viewpointManager->sendGetViewpoint(app.surface->resource()));
    if (!app.xdgSurface || !app.xdgToplevel || !app.viewport || !app.viewpoint || !app.xdgSurface->resource() || !app.xdgToplevel->resource() || !app.viewport->resource() ||
        !app.viewpoint->resource())
        return false;

    // Classification must precede the initial map. wp_viewport scales the
    // low-resolution full-SBS buffer to the packed logical surface rectangle.
    app.tagManager->sendSetToplevelTag(app.xdgToplevel->resource(), "stereo:sbs");
    app.xdgToplevel->sendSetTitle("HypXRland Viewpoint Portal Demo");
    app.xdgToplevel->sendSetAppId("hypxr-viewpoint-demo");
    if (app.options.fullscreen)
        app.xdgToplevel->sendSetFullscreen(nullptr);

    app.xdgToplevel->setConfigure([&app](CCXdgToplevel*, int32_t width, int32_t height, wl_array*) {
        if (width > 0)
            app.pendingWidth = sc<uint32_t>(width);
        if (height > 0)
            app.pendingHeight = sc<uint32_t>(height);
    });
    app.xdgToplevel->setClose([&app](CCXdgToplevel*) { app.running = false; });
    app.xdgSurface->setConfigure([&app](CCXdgSurface*, uint32_t serial) {
        app.xdgSurface->sendAckConfigure(serial);
        app.logicalWidth  = app.pendingWidth > 0 ? app.pendingWidth : app.options.renderWidth;
        app.logicalHeight = app.pendingHeight > 0 ? app.pendingHeight : app.options.renderHeight;
        app.pendingWidth  = 0;
        app.pendingHeight = 0;
        app.configured    = true;
        app.fallbackDirty = true;
        SRenderSize renderSize;
        if (fitSBSRenderSize(app.logicalWidth, app.logicalHeight, app.options.renderWidth, app.options.renderHeight, renderSize)) {
            app.renderWidth               = renderSize.width;
            app.renderHeight              = renderSize.height;
            app.feedback.mappingSupported = true;
        } else {
            app.renderWidth               = app.options.renderWidth;
            app.renderHeight              = app.options.renderHeight;
            app.feedback.mappingSupported = false;
            invalidateFeedback(app, "destination aspect cannot fit the bounded render buffer; feedback deferred");
        }
        updateFeedbackRequest(app);
        app.xdgSurface->sendSetWindowGeometry(0, 0, sc<int32_t>(app.logicalWidth), sc<int32_t>(app.logicalHeight));
        app.viewport->sendSetDestination(sc<int32_t>(app.logicalWidth), sc<int32_t>(app.logicalHeight));
        logLine("surface configured: logical {}x{}, aspect-matched full-SBS buffer {}x{}", app.logicalWidth, app.logicalHeight, app.renderWidth * 2U, app.renderHeight);
    });
    installViewpointHandlers(app);

    // The xdg-shell initial null commit requests the first configure. No buffer
    // and therefore no viewpoint rendered() association exists yet.
    app.surface->sendCommit();
    return true;
}

static CUniquePointer<SBuffer> createBuffer(SApp& app, uint32_t width, uint32_t height) {
    constexpr size_t BYTES_PER_PIXEL = sizeof(uint32_t);
    const size_t     STRIDE          = sc<size_t>(width) * BYTES_PER_PIXEL;
    if (width == 0 || height == 0 || STRIDE > sc<size_t>(std::numeric_limits<int32_t>::max()) || sc<size_t>(height) > std::numeric_limits<size_t>::max() / STRIDE)
        return {};

    const size_t SIZE = STRIDE * height;
    if (SIZE > sc<size_t>(std::numeric_limits<int32_t>::max()))
        return {};

    const int FD = memfd_create("hypxr-viewpoint-demo", MFD_CLOEXEC);
    if (FD < 0)
        return {};
    if (ftruncate(FD, sc<off_t>(SIZE)) < 0) {
        close(FD);
        return {};
    }

    void* mapping = mmap(nullptr, SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, FD, 0);
    if (mapping == MAP_FAILED) {
        close(FD);
        return {};
    }

    auto pool = makeShared<CCWlShmPool>(app.shm->sendCreatePool(FD, sc<int32_t>(SIZE)));
    if (!pool || !pool->resource()) {
        close(FD);
        munmap(mapping, SIZE);
        return {};
    }
    auto buffer = makeShared<CCWlBuffer>(pool->sendCreateBuffer(0, sc<int32_t>(width), sc<int32_t>(height), sc<int32_t>(STRIDE), WL_SHM_FORMAT_XRGB8888));
    close(FD);
    if (!buffer || !buffer->resource()) {
        munmap(mapping, SIZE);
        return {};
    }

    auto result      = makeUnique<SBuffer>();
    result->resource = std::move(buffer);
    result->mapping  = mapping;
    result->size     = SIZE;
    result->width    = width;
    result->height   = height;
    result->stride   = sc<uint32_t>(STRIDE);
    auto* rawResult  = result.get();
    result->resource->setRelease([rawResult](CCWlBuffer*) { rawResult->busy = false; });
    return result;
}

static SBuffer* acquireBuffer(SApp& app, uint32_t width, uint32_t height) {
    std::erase_if(app.buffers, [width, height](const CUniquePointer<SBuffer>& buffer) { return !buffer->busy && (buffer->width != width || buffer->height != height); });

    for (const auto& buffer : app.buffers) {
        if (!buffer->busy && buffer->width == width && buffer->height == height)
            return buffer.get();
    }

    const size_t CURRENT = std::count_if(app.buffers.begin(), app.buffers.end(),
                                         [width, height](const CUniquePointer<SBuffer>& buffer) { return buffer->width == width && buffer->height == height; });
    if (CURRENT >= 3)
        return nullptr;

    auto buffer = createBuffer(app, width, height);
    if (!buffer)
        return nullptr;
    auto* result = buffer.get();
    app.buffers.emplace_back(std::move(buffer));
    return result;
}

static bool commitFrame(SApp& app, SBuffer& buffer, const std::optional<SSample>& sample) {
    app.viewport->sendSetDestination(sc<int32_t>(app.logicalWidth), sc<int32_t>(app.logicalHeight));
    if (sample)
        app.viewpoint->sendRendered(highWord(sample->epoch), lowWord(sample->epoch), highWord(sample->sample), lowWord(sample->sample));

    // rendered() is intentionally adjacent to the one matching attach/commit.
    // No other wl_surface commit may be inserted into this sequence.
    app.surface->sendAttach(buffer.resource.get(), 0, 0);
    app.surface->sendDamageBuffer(0, 0, sc<int32_t>(buffer.width), sc<int32_t>(buffer.height));
    buffer.busy = true;
    app.surface->sendCommit();
    return wl_display_flush(app.display) >= 0;
}

static bool drawPending(SApp& app) {
    if (!app.configured || (!app.pendingSample && !app.fallbackDirty))
        return true;
    if (app.logicalWidth == 0 || app.logicalHeight == 0)
        return false;

    const uint32_t BUFFER_WIDTH = app.renderWidth * 2U;
    auto*          buffer       = acquireBuffer(app, BUFFER_WIDTH, app.renderHeight);
    if (!buffer)
        return true;

    SImage image = {
        .pixels       = {sc<uint32_t*>(buffer->mapping), buffer->size / sizeof(uint32_t)},
        .width        = buffer->width,
        .height       = buffer->height,
        .stridePixels = sc<uint32_t>(buffer->stride / sizeof(uint32_t)),
    };

    if (app.active && app.pendingSample) {
        const SSample SAMPLE = *app.pendingSample;
        if (SAMPLE.epoch != app.epoch || SAMPLE.geometry != app.geometry || !renderPortalSBS(image, app.portal, SAMPLE.views)) {
            rejectFeedback(app, "sample failed final render validation; feedback disabled");
            return true;
        }
        app.pendingSample.reset();
        app.fallbackDirty = false;
        if (!commitFrame(app, *buffer, SAMPLE))
            return false;
        ++app.renderedFrames;
        debugLine(app, "committed epoch={} sample={} frame={} hash={:#x}", SAMPLE.epoch, SAMPLE.sample, app.renderedFrames, pixelHash(image));
        if (app.options.once)
            app.running = false;
        return true;
    }

    if (!renderFallbackSBS(image))
        return false;
    app.fallbackDirty = false;
    debugLine(app, "committing inactive zero-disparity fallback hash={:#x}", pixelHash(image));
    return commitFrame(app, *buffer, std::nullopt);
}

static void destroyObjects(SApp& app) {
    app.buffers.clear();
    if (app.viewpoint)
        app.viewpoint->sendDestroy();
    if (app.viewport)
        app.viewport->sendDestroy();
    if (app.xdgToplevel)
        app.xdgToplevel->sendDestroy();
    if (app.xdgSurface)
        app.xdgSurface->sendDestroy();
    if (app.surface)
        app.surface->sendDestroy();
    if (app.viewpointManager)
        app.viewpointManager->sendDestroy();
    if (app.viewporter)
        app.viewporter->sendDestroy();
    if (app.tagManager)
        app.tagManager->sendDestroy();
    if (app.xdgWmBase)
        app.xdgWmBase->sendDestroy();
    if (app.shm)
        app.shm->sendRelease();
    // wl_compositor has no destructor request; the proxy is dropped with the connection.
}

static bool writePpm(const std::string& path, const SImage& image) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
        return false;

    output << "P6\n" << image.width << ' ' << image.height << "\n255\n";
    for (uint32_t y = 0; y < image.height; ++y) {
        for (uint32_t x = 0; x < image.width; ++x) {
            const uint32_t COLOR = image.pixels[sc<size_t>(y) * image.stridePixels + x];
            const char     RGB[] = {
                sc<char>((COLOR >> 16U) & 0xFFU),
                sc<char>((COLOR >> 8U) & 0xFFU),
                sc<char>(COLOR & 0xFFU),
            };
            output.write(RGB, sizeof(RGB));
        }
    }
    return output.good();
}

static bool renderOffline(const SOptions& options) {
    const uint32_t        BUFFER_WIDTH = options.renderWidth * 2U;
    std::vector<uint32_t> pixels(sc<size_t>(BUFFER_WIDTH) * options.renderHeight);
    const SImage          image = {
        .pixels       = pixels,
        .width        = BUFFER_WIDTH,
        .height       = options.renderHeight,
        .stridePixels = BUFFER_WIDTH,
    };

    const bool RENDERED = options.renderFallback ?
        renderFallbackSBS(image) :
        renderPortalSBS(image, {.widthMeters = 1.6, .heightMeters = 0.9}, {.left = {.x = -0.032, .z = 1.2}, .right = {.x = 0.032, .z = 1.2}});
    if (!RENDERED || !writePpm(*options.renderPath, image))
        return false;

    logLine("wrote {} {}x{} full-SBS PPM '{}' ({}x{} per eye, hash {:#x})", options.renderFallback ? "fallback" : "active portal", image.width, image.height, *options.renderPath,
            options.renderWidth, options.renderHeight, pixelHash(image));
    return true;
}

int main(int argc, char** argv) {
    const auto OPTIONS = parseOptions(argc, argv);
    if (!OPTIONS)
        return argc > 1 && (std::strcmp(argv[1], "-h") == 0 || std::strcmp(argv[1], "--help") == 0) ? 0 : 2;
    if (OPTIONS->renderPath)
        return renderOffline(*OPTIONS) ? 0 : 1;

    wl_display* display = wl_display_connect(nullptr);
    if (!display) {
        logLine("error: could not connect to the Wayland display");
        return 1;
    }

    int result = 0;
    {
        SApp app{};
        app.display = display;
        app.options = *OPTIONS;
        if (!bindGlobals(app)) {
            logLine("error: compositor lacks wl_shm XRGB8888, xdg-shell, viewporter, xdg-toplevel-tag-v1, or hypxr_viewpoint_v1");
            result = 1;
        } else if (!setupSurface(app)) {
            logLine("error: could not create the tagged viewpoint toplevel");
            result = 1;
        } else {
            logLine("viewpoint demo mapped; waiting for surface size and viewpoint activation (render buffer {}x{})", app.options.renderWidth * 2U, app.options.renderHeight);
            while (app.running && result == 0) {
                if (wl_display_dispatch(app.display) < 0 || !drawPending(app))
                    result = 1;
            }
        }
        destroyObjects(app);
    }

    wl_display_disconnect(display);
    return result;
}
