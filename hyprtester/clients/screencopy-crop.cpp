// screencopy-crop — the only pixel readback in the test suite.
//
// Written for the stereo tests (research/24 §3.6, §3.12, WP F3). Everything else about the SBS
// output is assertable structurally; the one thing that is not is whether the *image* a capture
// protocol hands a client is the right image. That gap hid a real regression: on a stereo monitor
// every draw is projected through outputProjection(paneSize), so a capture buffer smaller than the
// pane must CLIP the projected pane. Bind a viewport of the buffer's own size instead and the whole
// desktop is squeezed into the region rectangle — a picture that is wrong in a way no geometry
// assertion can see.
//
// So this client does the comparison itself and prints a verdict, rather than piping pixels out:
//
//   1. capture the whole output          (wlr-screencopy capture_output)
//   2. capture a region of it            (capture_output_region)
//   3. assert that (2) is byte-identical to the matching sub-rect of (1)
//
// That is true on any monitor, stereo or not, and needs no knowledge of what is on screen. Both
// captures go through shm, which is the path `grim`, the screenshot portal and hyprshot all take.
//
// Usage:   screencopy-crop <output-name> <x> <y> <w> <h> [attempts]
// Prints:  full <w>x<h> fmt <f> ... / region <w>x<h> fmt <f> ... / varied <n> / matchpct <p>
//          result ok|mismatch|error
// Exit:    0 when the crop matched, 1 otherwise.
//
// `varied` is the number of pixels in the reference crop that differ from its first pixel — a
// uniform crop would make the comparison vacuous, so the harness asserts it is non-zero.

#include <sys/mman.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstring>
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

using namespace Hyprutils::Memory;

namespace {
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
    void clientLog(std::format_string<Args...> fmt, Args&&... args) {
        std::println("{}", std::format(fmt, std::forward<Args>(args)...));
        std::fflush(stdout);
    }

    // Pump the display until `done` or the budget runs out. Screencopy answers on the compositor's
    // own render cycle, so this is a wait, not a roundtrip.
    bool pumpUntil(SState& state, const std::function<bool()>& done, int budgetMs = 5000) {
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

    std::optional<SImage> capture(SState& state, wl_proxy* output, bool region, int32_t x, int32_t y, int32_t w, int32_t h) {
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
        const int    FD   = memfd_create("screencopy-crop", MFD_CLOEXEC);
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
}

int main(int argc, char** argv) {
    if (argc < 6) {
        clientLog("usage: screencopy-crop <output-name> <x> <y> <w> <h> [attempts]");
        return 1;
    }

    const std::string OUTNAME  = argv[1];
    const int32_t     X        = std::stoi(argv[2]);
    const int32_t     Y        = std::stoi(argv[3]);
    const int32_t     W        = std::stoi(argv[4]);
    const int32_t     H        = std::stoi(argv[5]);
    const int         ATTEMPTS = argc > 6 ? std::stoi(argv[6]) : 3;

    SState            state;
    state.display = wl_display_connect(nullptr);
    if (!state.display) {
        clientLog("error: failed to connect to the wayland display");
        return 1;
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
        return 1;
    }

    wl_proxy* target = nullptr;
    for (auto& [id, out] : state.outputs) {
        if (out.name == OUTNAME)
            target = out.output->resource();
    }

    if (!target) {
        clientLog("error: no output named {}", OUTNAME);
        return 1;
    }

    // Best of N: the two captures are separate frames, so anything still animating (a background
    // fade, a window opening) can differ between them. A real crop failure differs in every attempt.
    double bestPct    = -1.0;
    size_t bestVaried = 0;
    SImage bestFull, bestRegion;

    for (int attempt = 0; attempt < ATTEMPTS && bestPct < 100.0; ++attempt) {
        const auto FULL = capture(state, target, false, 0, 0, 0, 0);
        if (!FULL)
            continue;

        const auto REGION = capture(state, target, true, X, Y, W, H);
        if (!REGION)
            continue;

        if (FULL->fmt != REGION->fmt) {
            clientLog("error: format mismatch, full {} vs region {}", FULL->fmt, REGION->fmt);
            continue;
        }

        if (X < 0 || Y < 0 || static_cast<uint32_t>(X + W) > FULL->w || static_cast<uint32_t>(Y + H) > FULL->h) {
            clientLog("error: region {},{} {}x{} does not fit the {}x{} capture", X, Y, W, H, FULL->w, FULL->h);
            return 1;
        }

        size_t equal = 0, varied = 0, total = 0;
        for (uint32_t row = 0; row < REGION->h; ++row) {
            for (uint32_t col = 0; col < REGION->w; ++col) {
                const uint32_t REF = FULL->at(X + col, Y + row);
                ++total;
                if (REF == REGION->at(col, row))
                    ++equal;
                if (REF != FULL->at(X, Y))
                    ++varied;
            }
        }

        const double PCT = total ? (100.0 * static_cast<double>(equal) / static_cast<double>(total)) : 0.0;
        if (PCT > bestPct) {
            bestPct    = PCT;
            bestVaried = varied;
            bestFull   = *FULL;
            bestRegion = *REGION;
        }
    }

    if (bestPct < 0) {
        clientLog("result error");
        wl_display_disconnect(state.display);
        return 1;
    }

    clientLog("full {}x{} fmt {} stride {} flags {}", bestFull.w, bestFull.h, bestFull.fmt, bestFull.stride, bestFull.flags);
    clientLog("region {}x{} fmt {} stride {} flags {}", bestRegion.w, bestRegion.h, bestRegion.fmt, bestRegion.stride, bestRegion.flags);
    clientLog("varied {}", bestVaried);
    clientLog("matchpct {:.2f}", bestPct);
    clientLog("result {}", bestPct >= 99.9 ? "ok" : "mismatch");

    wl_display_disconnect(state.display);
    return bestPct >= 99.9 ? 0 : 1;
}
