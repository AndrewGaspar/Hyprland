#pragma once

// The shared half of the suite's pixel readback: connect, find an output by name, and pull one
// wlr-screencopy frame into host memory over shm (the path grim, the screenshot portal and hyprshot
// all take). Two clients sit on top of it and differ only in what they assert about the pixels:
//
//   screencopy-crop  — is a region capture a 1:1 crop of the full one? (WP F3, research/24 §3.6)
//   screencopy-probe — what colour is the pixel at x,y?               (WP S2, research/24 §5.3)
//
// Header-only because hyprtester's clientNew() builds one .cpp per executable; each client is its
// own TU, so there is nothing to link.

#include <sys/mman.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <print>
#include <string>
#include <thread>
#include <vector>

#include <wayland-client.h>
#include <wayland.hpp>
#include <wlr-screencopy-unstable-v1.hpp>

#include <hyprutils/memory/SharedPtr.hpp>

namespace Screencopy {
    using namespace Hyprutils::Memory;

    constexpr int BYTES_PER_PIXEL = 4;

    struct SOutput {
        CSharedPointer<CCWlOutput> output;
        std::string                name = "?";
    };

    struct SState {
        wl_display*                               display = nullptr;
        CSharedPointer<CCWlRegistry>              registry;
        CSharedPointer<CCWlShm>                   shm;
        CSharedPointer<CCZwlrScreencopyManagerV1> screencopy;
        std::map<uint32_t, SOutput>               outputs;
    };

    struct SImage {
        uint32_t             fmt    = 0;
        uint32_t             w      = 0;
        uint32_t             h      = 0;
        uint32_t             stride = 0;
        uint32_t             flags  = 0;
        std::vector<uint8_t> px;

        // pixel (x, y) with the alpha byte masked off: capture buffers are commonly X-formats where
        // the fourth byte is undefined, and comparing it would produce noise, not signal.
        uint32_t at(uint32_t x, uint32_t y) const {
            uint32_t v = 0;
            std::memcpy(&v, px.data() + static_cast<size_t>(y) * stride + static_cast<size_t>(x) * BYTES_PER_PIXEL, sizeof(v));
            return v & 0x00FFFFFF;
        }
    };

    template <typename... Args>
    //NOLINTNEXTLINE
    inline void clientLog(std::format_string<Args...> fmt, Args&&... args) {
        std::println("{}", std::format(fmt, std::forward<Args>(args)...));
        std::fflush(stdout);
    }

    // Pump the display until `done` or the budget runs out. Screencopy answers on the compositor's
    // own render cycle, so this is a wait, not a roundtrip.
    inline bool pumpUntil(SState& state, const std::function<bool()>& done, int budgetMs = 5000) {
        for (int waited = 0; waited < budgetMs; waited += 10) {
            if (done())
                return true;
            if (wl_display_roundtrip(state.display) < 0)
                return false;
            if (done())
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return done();
    }

    inline std::optional<SImage> capture(SState& state, wl_proxy* output, bool region, int32_t x, int32_t y, int32_t w, int32_t h) {
        auto frame =
            makeShared<CCZwlrScreencopyFrameV1>(region ? state.screencopy->sendCaptureOutputRegion(0, output, x, y, w, h) : state.screencopy->sendCaptureOutput(0, output));
        if (!frame->resource()) {
            clientLog("error: no screencopy frame");
            return std::nullopt;
        }

        SImage image;
        bool   gotBuffer = false, ready = false, failed = false;

        frame->setBuffer([&](CCZwlrScreencopyFrameV1*, uint32_t fmt, uint32_t width, uint32_t height, uint32_t stride) {
            image.fmt    = fmt;
            image.w      = width;
            image.h      = height;
            image.stride = stride;
            gotBuffer    = true;
        });
        frame->setFlags([&](CCZwlrScreencopyFrameV1*, zwlrScreencopyFrameV1Flags flags) { image.flags = static_cast<uint32_t>(flags); });
        frame->setReady([&](CCZwlrScreencopyFrameV1*, uint32_t, uint32_t, uint32_t) { ready = true; });
        frame->setFailed([&](CCZwlrScreencopyFrameV1*) { failed = true; });

        if (!pumpUntil(state, [&]() { return gotBuffer || failed; }) || failed || !gotBuffer) {
            clientLog("error: no buffer event for the {} capture", region ? "region" : "full");
            frame->sendDestroy();
            return std::nullopt;
        }

        if (image.stride < image.w * BYTES_PER_PIXEL) {
            clientLog("error: stride {} is not 4 bytes per pixel at width {}", image.stride, image.w);
            frame->sendDestroy();
            return std::nullopt;
        }

        const size_t SIZE = static_cast<size_t>(image.stride) * image.h;
        const int    FD   = memfd_create("screencopy", MFD_CLOEXEC);
        if (FD < 0 || ftruncate(FD, SIZE) != 0) {
            clientLog("error: failed to allocate a {} byte shm buffer", SIZE);
            frame->sendDestroy();
            return std::nullopt;
        }

        void* data = mmap(nullptr, SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, FD, 0);
        if (data == MAP_FAILED) {
            close(FD);
            clientLog("error: failed to mmap the shm buffer");
            frame->sendDestroy();
            return std::nullopt;
        }
        std::memset(data, 0, SIZE);

        auto pool   = makeShared<CCWlShmPool>(state.shm->sendCreatePool(FD, SIZE));
        auto buffer = makeShared<CCWlBuffer>(pool->sendCreateBuffer(0, image.w, image.h, image.stride, image.fmt));
        pool->sendDestroy();

        frame->sendCopy(buffer->resource());

        const bool GOT = pumpUntil(state, [&]() { return ready || failed; }) && ready && !failed;
        if (GOT) {
            image.px.resize(SIZE);
            std::memcpy(image.px.data(), data, SIZE);
        } else
            clientLog("error: the {} capture never became ready", region ? "region" : "full");

        munmap(data, SIZE);
        close(FD);
        buffer->sendDestroy();
        frame->sendDestroy();
        wl_display_roundtrip(state.display);

        if (!GOT)
            return std::nullopt;

        return image;
    }

    // connect + bind wl_shm, wl_output (v4, for wl_output.name) and the screencopy manager.
    inline bool connect(SState& state) {
        state.display = wl_display_connect(nullptr);
        if (!state.display) {
            clientLog("error: failed to connect to the wayland display");
            return false;
        }

        state.registry = makeShared<CCWlRegistry>((wl_proxy*)wl_display_get_registry(state.display));
        state.registry->setGlobal([&state](CCWlRegistry* r, uint32_t id, const char* name, uint32_t version) {
            const std::string NAME = name;
            if (NAME == "wl_output") {
                const uint32_t BIND_VER = version < 4 ? version : 4; // v4 for wl_output.name
                auto&          out      = state.outputs[id];
                out.output              = makeShared<CCWlOutput>((wl_proxy*)wl_registry_bind((wl_registry*)r->resource(), id, &wl_output_interface, BIND_VER));
                out.output->setName([&state, id](CCWlOutput*, const char* n) { state.outputs[id].name = n ? n : "?"; });
            } else if (NAME == "wl_shm")
                state.shm = makeShared<CCWlShm>((wl_proxy*)wl_registry_bind((wl_registry*)r->resource(), id, &wl_shm_interface, 1));
            else if (NAME == "zwlr_screencopy_manager_v1")
                state.screencopy = makeShared<CCZwlrScreencopyManagerV1>((wl_proxy*)wl_registry_bind((wl_registry*)r->resource(), id, &zwlr_screencopy_manager_v1_interface, 1));
        });
        state.registry->setGlobalRemove([](CCWlRegistry*, uint32_t) {});

        wl_display_roundtrip(state.display);
        wl_display_roundtrip(state.display); // wl_output.name arrives on the second pass

        if (!state.shm || !state.screencopy) {
            clientLog("error: missing wl_shm or zwlr_screencopy_manager_v1");
            return false;
        }

        return true;
    }

    inline wl_proxy* outputNamed(SState& state, const std::string& name) {
        for (auto& [id, out] : state.outputs) {
            if (out.name == name)
                return out.output->resource();
        }

        clientLog("error: no output named {}", name);
        return nullptr;
    }
}
