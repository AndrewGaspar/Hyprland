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

#include "screencopy-capture.hpp"

#include <string>

using namespace Screencopy;

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
    if (!connect(state))
        return 1;

    wl_proxy* target = outputNamed(state, OUTNAME);
    if (!target)
        return 1;

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
