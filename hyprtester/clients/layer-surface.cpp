// Minimal zwlr_layer_shell_v1 client for the test suite. The other layer tests drive
// `kitty +kitten panel`, which needs kitty on the box — the host does not have it and the
// hermetic container image does not ship it either, so a test that must inspect layer geometry
// had nowhere to run. This is that missing piece: a top-anchored bar with a real SHM buffer,
// no wl_output of its own (so it lands on the focused monitor, per CLayerSurface::create).
//
// Usage: layer-surface <namespace> [height]
// Prints "mapped" once the surface has been configured and a buffer committed, then idles
// until killed. The harness polls `hyprctl layers` for the namespace (see
// hyprtester/src/tests/main/layer.cpp).

#include <sys/mman.h>
#include <sys/poll.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <print>
#include <string>

#include <wayland-client.h>
#include <wayland.hpp>
#include <wlr-layer-shell-unstable-v1.hpp>

#include <hyprutils/memory/SharedPtr.hpp>

using namespace Hyprutils::Memory;

struct SWlState {
    wl_display*                          display = nullptr;
    CSharedPointer<CCWlRegistry>         registry;
    CSharedPointer<CCWlCompositor>       compositor;
    CSharedPointer<CCWlShm>              shm;
    CSharedPointer<CCZwlrLayerShellV1>   layerShell;
    CSharedPointer<CCWlSurface>          surface;
    CSharedPointer<CCZwlrLayerSurfaceV1> layerSurface;
    CSharedPointer<CCWlBuffer>           buffer;
};

template <typename... Args>
//NOLINTNEXTLINE
static void clientLog(std::format_string<Args...> fmt, Args&&... args) {
    std::println("{}", std::format(fmt, std::forward<Args>(args)...));
    std::fflush(stdout);
}

static bool bindRegistry(SWlState& state) {
    state.registry = makeShared<CCWlRegistry>((wl_proxy*)wl_display_get_registry(state.display));

    state.registry->setGlobal([&state](CCWlRegistry* r, uint32_t id, const char* name, uint32_t version) {
        const std::string NAME = name;
        if (NAME == "wl_compositor")
            state.compositor = makeShared<CCWlCompositor>((wl_proxy*)wl_registry_bind((wl_registry*)r->resource(), id, &wl_compositor_interface, 4));
        else if (NAME == "wl_shm")
            state.shm = makeShared<CCWlShm>((wl_proxy*)wl_registry_bind((wl_registry*)r->resource(), id, &wl_shm_interface, 1));
        else if (NAME == "zwlr_layer_shell_v1")
            state.layerShell = makeShared<CCZwlrLayerShellV1>((wl_proxy*)wl_registry_bind((wl_registry*)r->resource(), id, &zwlr_layer_shell_v1_interface, 1));
    });
    state.registry->setGlobalRemove([](CCWlRegistry* r, uint32_t id) {});

    wl_display_roundtrip(state.display);

    if (!state.compositor || !state.shm || !state.layerShell) {
        clientLog("Failed to get wl_compositor/wl_shm/zwlr_layer_shell_v1");
        return false;
    }

    return true;
}

// A single-colour XRGB8888 buffer. Anonymous shm, unlinked immediately — the pool keeps the fd.
static CSharedPointer<CCWlBuffer> makeBuffer(SWlState& state, int32_t w, int32_t h) {
    const int32_t STRIDE = w * 4;
    const int32_t SIZE   = STRIDE * h;

    const int     FD = memfd_create("layer-surface-buf", MFD_CLOEXEC);
    if (FD < 0 || ftruncate(FD, SIZE) != 0) {
        clientLog("Failed to allocate the shm buffer");
        return nullptr;
    }

    void* data = mmap(nullptr, SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, FD, 0);
    if (data == MAP_FAILED) {
        close(FD);
        clientLog("Failed to mmap the shm buffer");
        return nullptr;
    }
    std::memset(data, 0x40, SIZE);
    munmap(data, SIZE);

    auto pool   = makeShared<CCWlShmPool>(state.shm->sendCreatePool(FD, SIZE));
    auto buffer = makeShared<CCWlBuffer>(pool->sendCreateBuffer(0, w, h, STRIDE, WL_SHM_FORMAT_XRGB8888));
    pool->sendDestroy();
    close(FD);

    return buffer;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        clientLog("usage: layer-surface <namespace> [height]");
        return -1;
    }

    const std::string NAMESPACE = argv[1];
    const int32_t     HEIGHT    = argc > 2 ? std::stoi(argv[2]) : 48;

    SWlState          state;

    state.display = wl_display_connect(nullptr);
    if (!state.display) {
        clientLog("Failed to connect to wayland display");
        return -1;
    }

    if (!bindRegistry(state))
        return -1;

    state.surface = makeShared<CCWlSurface>(state.compositor->sendCreateSurface());

    // nullptr output: the compositor puts us on the focused monitor, which is what the test wants.
    state.layerSurface =
        makeShared<CCZwlrLayerSurfaceV1>(state.layerShell->sendGetLayerSurface(state.surface->resource(), nullptr, ZWLR_LAYER_SHELL_V1_LAYER_TOP, NAMESPACE.c_str()));
    if (!state.layerSurface->resource()) {
        clientLog("Failed to create the layer surface");
        return -1;
    }

    // Anchored to the whole top edge, so width follows the monitor and x follows its position —
    // exactly the geometry the regression under test gets wrong.
    state.layerSurface->sendSetSize(0, HEIGHT);
    state.layerSurface->sendSetAnchor((zwlrLayerSurfaceV1Anchor)(ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT));
    state.layerSurface->sendSetExclusiveZone(0);

    bool mapped = false;
    state.layerSurface->setConfigure([&state, &mapped, HEIGHT](CCZwlrLayerSurfaceV1* p, uint32_t serial, uint32_t w, uint32_t h) {
        p->sendAckConfigure(serial);

        if (mapped)
            return;

        const int32_t W = w > 0 ? (int32_t)w : 1;
        state.buffer    = makeBuffer(state, W, h > 0 ? (int32_t)h : HEIGHT);
        if (!state.buffer)
            return;

        state.surface->sendAttach(state.buffer.get(), 0, 0);
        state.surface->sendDamageBuffer(0, 0, INT32_MAX, INT32_MAX);
        state.surface->sendCommit();
        mapped = true;
        clientLog("mapped");
    });
    state.layerSurface->setClosed([](CCZwlrLayerSurfaceV1* p) { clientLog("closed"); });

    state.surface->sendCommit(); // triggers the initial configure

    struct pollfd fds[1] = {{.fd = wl_display_get_fd(state.display), .events = POLLIN}};
    while (poll(fds, 1, 50) != -1) {
        wl_display_flush(state.display);

        if (fds[0].revents & POLLIN) {
            if (wl_display_prepare_read(state.display) == 0) {
                wl_display_read_events(state.display);
                wl_display_dispatch_pending(state.display);
            } else if (wl_display_dispatch(state.display) < 0)
                break;
        }

        int ret = 0;
        do {
            ret = wl_display_dispatch_pending(state.display);
            wl_display_flush(state.display);
        } while (ret > 0);
    }

    wl_display* display = state.display;
    state               = {};
    wl_display_disconnect(display);
    return 0;
}
