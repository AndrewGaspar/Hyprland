// screencopy-probe — "what colour is the pixel at x,y?"
//
// The second half of the suite's pixel readback, written for WP S2 (research/24 §5.3). The per-window
// stereo producer changes nothing structural: the window keeps its box, its geometry, its input
// region. The ONLY observable difference between a cropped window and an uncropped one is which
// texels ended up on the screen — so the producer cannot be tested at all without reading pixels.
//
// What makes that testable on a stereo output is §3.6: a monitor capture is sized at the PANE and is
// taken pre-pack, from the composite of eye 0. So a client whose buffer carries four distinct
// quadrants (xdg-interactive --paint) tells you exactly which crop ran, by comparing probed points
// to each other:
//
//   uncropped      all four window quadrants differ
//   sbs/hsbs eye 0 left half stretched across the box  -> top-left == top-right, bottom-left == bottom-right
//   tab/htab eye 0 top half stretched across the box   -> top-left == bottom-left, top-right == bottom-right
//
// Comparisons, never absolute colours: the frame has been through the client's shm buffer, the
// compositor's composite, whatever colour management the monitor carries and the capture's own
// format. Every one of those is a transform that preserves "these two pixels are the same" and none
// of them preserves "this pixel is 0xFF0000". The assertion is in the harness; this client only
// reports.
//
// Usage:   screencopy-probe <output-name> <x> <y> [<x> <y> ...]
// Prints:  size <w>x<h> fmt <f>
//          px <i> <x> <y> <rrggbb>          (one line per point, in argument order)
//          result ok|error
// Exit:    0 when every point was inside the capture, 1 otherwise.

#include "screencopy-capture.hpp"

#include <string>
#include <utility>
#include <vector>

using namespace Screencopy;

int main(int argc, char** argv) {
    if (argc < 4 || (argc % 2) != 0) {
        clientLog("usage: screencopy-probe <output-name> <x> <y> [<x> <y> ...]");
        return 1;
    }

    const std::string                OUTNAME = argv[1];

    std::vector<std::pair<int, int>> points;
    for (int i = 2; i + 1 < argc; i += 2)
        points.emplace_back(std::stoi(argv[i]), std::stoi(argv[i + 1]));

    SState state;
    if (!connect(state))
        return 1;

    wl_proxy* target = outputNamed(state, OUTNAME);
    if (!target)
        return 1;

    const auto FRAME = capture(state, target, false, 0, 0, 0, 0);
    if (!FRAME) {
        clientLog("result error");
        wl_display_disconnect(state.display);
        return 1;
    }

    clientLog("size {}x{} fmt {}", FRAME->w, FRAME->h, FRAME->fmt);

    bool ok = true;
    for (size_t i = 0; i < points.size(); ++i) {
        const auto& [X, Y] = points[i];
        if (X < 0 || Y < 0 || static_cast<uint32_t>(X) >= FRAME->w || static_cast<uint32_t>(Y) >= FRAME->h) {
            clientLog("error: point {},{} is outside the {}x{} capture", X, Y, FRAME->w, FRAME->h);
            ok = false;
            continue;
        }

        clientLog("px {} {} {} {:06x}", i, X, Y, FRAME->at(X, Y));
    }

    clientLog("result {}", ok ? "ok" : "error");

    wl_display_disconnect(state.display);
    return ok ? 0 : 1;
}
