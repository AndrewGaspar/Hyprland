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
//
// ---------------------------------------------------------------------------------------------
//
// The SECOND verb was added for the depth desktop (research/24 §6, WP D4), which needs a different
// question answered: not "is this the right image" but "did the picture MOVE, and by how much".
//
//   screencopy-crop --centroid <output-name> [attempts]
//
// It captures the output and reports the mass-weighted centroid of everything that is not the
// background, where the background is the corner pixel:
//
//   mass(x, y) = |R − bgR| + |G − bgG| + |B − bgB|
//
// Why a centroid rather than an edge finder: a rigid translation of the foreground moves the
// centroid by EXACTLY the translation, sub-pixel included, because the first moment of a
// convolution is the convolution of the first moments — antialiasing, linear filtering and the
// rounded corners of a window all wash out. The one precondition is a uniform background (set
// `misc:disable_hyprland_logo` and `disable_splash_rendering`), which is why the corner count is
// reported: `corners 4` means all four agree with the background pixel and the measurement means
// what it says. Two captures are compared and `stable 1` reported only when they agree to 0.01 px,
// so a frame caught mid-animation is retried rather than measured.
//
// Prints:  full <w>x<h> ... / bg <0xRRGGBB> corners <n> / mass <m> / centroid <x> <y>
//          stable 0|1 / result ok|error
// Exit:    0 when a stable measurement was taken, 1 otherwise.

#include <sys/mman.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <utility>
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

    // --- the --centroid verb (research/24 §6, WP D4) ---

    struct SCentroid {
        double   x = 0.0, y = 0.0, mass = 0.0;
        uint32_t bg      = 0;
        int      corners = 0;
    };

    // The mass-weighted centre of everything that is not the background. `bg` is the top-left
    // pixel; `corners` counts how many of the four corners agree with it, which is the caller's
    // proof that the background really is uniform and the measurement is a translation and not a
    // change of scenery.
    SCentroid centroidOf(const SImage& img) {
        SCentroid out;
        if (!img.w || !img.h)
            return out;

        out.bg = img.at(0, 0);
        for (const auto& [CX, CY] : {std::pair{0u, 0u}, std::pair{img.w - 1, 0u}, std::pair{0u, img.h - 1}, std::pair{img.w - 1, img.h - 1}}) {
            if (img.at(CX, CY) == out.bg)
                ++out.corners;
        }

        const auto CHANNEL = [](uint32_t px, int shift) { return static_cast<int>((px >> shift) & 0xFF); };

        double     sumX = 0.0, sumY = 0.0, sum = 0.0;
        for (uint32_t y = 0; y < img.h; ++y) {
            for (uint32_t x = 0; x < img.w; ++x) {
                const uint32_t PX = img.at(x, y);
                if (PX == out.bg)
                    continue;

                const double MASS = std::abs(CHANNEL(PX, 0) - CHANNEL(out.bg, 0)) + std::abs(CHANNEL(PX, 8) - CHANNEL(out.bg, 8)) + std::abs(CHANNEL(PX, 16) - CHANNEL(out.bg, 16));

                sum += MASS;
                sumX += MASS * x;
                sumY += MASS * y;
            }
        }

        out.mass = sum;
        if (sum > 0.0) {
            out.x = sumX / sum;
            out.y = sumY / sum;
        }
        return out;
    }

    int runCentroid(SState& state, wl_proxy* target, int attempts) {
        SCentroid measured;
        SImage    image;
        bool      stable = false;

        // two captures of the same scene: a centroid that moved between them means something on
        // screen is still animating, and measuring it would be measuring the wrong frame
        const auto AGREES = [](const SCentroid& a, const SCentroid& b) { return std::abs(a.x - b.x) < 0.01 && std::abs(a.y - b.y) < 0.01; };

        for (int attempt = 0; attempt < attempts && !stable; ++attempt) {
            const auto FIRST = capture(state, target, false, 0, 0, 0, 0);
            if (!FIRST)
                continue;

            const auto SECOND = capture(state, target, false, 0, 0, 0, 0);
            if (!SECOND)
                continue;

            image    = *SECOND;
            measured = centroidOf(*SECOND);
            stable   = AGREES(centroidOf(*FIRST), measured);
        }

        if (measured.mass <= 0.0) {
            clientLog("result error");
            return 1;
        }

        clientLog("full {}x{} fmt {} stride {} flags {}", image.w, image.h, image.fmt, image.stride, image.flags);
        clientLog("bg 0x{:06x} corners {}", measured.bg, measured.corners);
        clientLog("mass {:.0f}", measured.mass);
        clientLog("centroid {:.3f} {:.3f}", measured.x, measured.y);
        clientLog("stable {}", stable ? 1 : 0);
        clientLog("result ok");
        return stable ? 0 : 1;
    }
}

int main(int argc, char** argv) {
    const bool CENTROID = argc > 1 && std::string(argv[1]) == "--centroid";

    if ((CENTROID && argc < 3) || (!CENTROID && argc < 6)) {
        clientLog("usage: screencopy-crop <output-name> <x> <y> <w> <h> [attempts]");
        clientLog("       screencopy-crop --centroid <output-name> [attempts]");
        return 1;
    }

    const std::string OUTNAME  = CENTROID ? argv[2] : argv[1];
    const int32_t     X        = CENTROID ? 0 : std::stoi(argv[2]);
    const int32_t     Y        = CENTROID ? 0 : std::stoi(argv[3]);
    const int32_t     W        = CENTROID ? 0 : std::stoi(argv[4]);
    const int32_t     H        = CENTROID ? 0 : std::stoi(argv[5]);
    const int         ATTEMPTS = CENTROID ? (argc > 3 ? std::stoi(argv[3]) : 3) : (argc > 6 ? std::stoi(argv[6]) : 3);

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

    if (CENTROID) {
        const int RET = runCentroid(state, target, ATTEMPTS);
        wl_display_disconnect(state.display);
        return RET;
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
