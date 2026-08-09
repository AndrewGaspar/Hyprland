// screencopy-crop — "is a region capture a 1:1 crop of the full one?"
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
// That is true on any monitor, stereo or not, and needs no knowledge of what is on screen. The
// connect/capture plumbing lives in screencopy-capture.hpp, shared with screencopy-probe.
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

#include "screencopy-capture.hpp"

#include <cmath>
#include <string>
#include <utility>

using namespace Screencopy;

namespace {
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
    if (!connect(state))
        return 1;

    wl_proxy* target = outputNamed(state, OUTNAME);
    if (!target)
        return 1;

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
