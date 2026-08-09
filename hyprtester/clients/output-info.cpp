// output-info — dumps what a Wayland client actually sees about every output, then exits.
//
// Written for the stereo tests (research/24 §3, WP F3): the non-negotiable invariant of the SBS
// output is that a 3840x1080 panel presents as exactly ONE logical monitor of 1920x1080
// *everywhere*, and "everywhere" includes the protocol. `hyprctl monitors` is the compositor
// talking about itself; this is the only in-tree way to assert what clients are told.
//
// Output format, one line per output, stable for grepping from hyprtester:
//   output <name> mode <w>x<h>@<mHz> scale <n> logical <w>x<h> at <x>,<y>
// followed by a single "done <count>" line.

#include <cstdint>
#include <cstdio>
#include <map>
#include <print>
#include <string>

#include <wayland-client.h>
#include <wayland.hpp>
#include <xdg-output-unstable-v1.hpp>

#include <hyprutils/memory/SharedPtr.hpp>

using namespace Hyprutils::Memory;

namespace {
    struct SOutput {
        CSharedPointer<CCWlOutput>     output;
        CSharedPointer<CCZxdgOutputV1> xdgOutput;

        std::string                    name     = "?";
        int32_t                        modeW    = -1;
        int32_t                        modeH    = -1;
        int32_t                        refresh  = -1;
        int32_t                        scale    = -1;
        int32_t                        logicalW = -1;
        int32_t                        logicalH = -1;
        int32_t                        logicalX = 0;
        int32_t                        logicalY = 0;
    };

    struct SState {
        wl_display*                           display = nullptr;
        CSharedPointer<CCWlRegistry>          registry;
        CSharedPointer<CCZxdgOutputManagerV1> xdgOutputMgr;
        std::map<uint32_t, SOutput>           outputs;
    };
}

int main() {
    SState state;

    state.display = wl_display_connect(nullptr);
    if (!state.display) {
        std::println("error: failed to connect to the wayland display");
        return 1;
    }

    state.registry = makeShared<CCWlRegistry>((wl_proxy*)wl_display_get_registry(state.display));

    // Two passes: bind wl_output + the xdg-output manager first, then (after a roundtrip, so the
    // manager exists whatever order the globals arrive in) request an xdg_output per output.
    state.registry->setGlobal([&state](CCWlRegistry* r, uint32_t id, const char* name, uint32_t version) {
        const std::string NAME = name;
        if (NAME == "wl_output") {
            // v4 for the wl_output.name event — matching a monitor by name is the whole point
            const uint32_t BIND_VER = version < 4 ? version : 4;
            auto&          out      = state.outputs[id];
            out.output              = makeShared<CCWlOutput>((wl_proxy*)wl_registry_bind((wl_registry*)r->resource(), id, &wl_output_interface, BIND_VER));

            out.output->setName([&state, id](CCWlOutput*, const char* n) { state.outputs[id].name = n ? n : "?"; });
            out.output->setScale([&state, id](CCWlOutput*, int32_t factor) { state.outputs[id].scale = factor; });
            out.output->setMode([&state, id](CCWlOutput*, enum wl_output_mode flags, int32_t w, int32_t h, int32_t refresh) {
                if (!(flags & WL_OUTPUT_MODE_CURRENT))
                    return;
                auto& o   = state.outputs[id];
                o.modeW   = w;
                o.modeH   = h;
                o.refresh = refresh;
            });
        } else if (NAME == "zxdg_output_manager_v1")
            state.xdgOutputMgr = makeShared<CCZxdgOutputManagerV1>((wl_proxy*)wl_registry_bind((wl_registry*)r->resource(), id, &zxdg_output_manager_v1_interface, 3));
    });
    state.registry->setGlobalRemove([](CCWlRegistry*, uint32_t) {});

    wl_display_roundtrip(state.display);

    if (state.outputs.empty()) {
        std::println("error: no wl_output globals");
        wl_display_disconnect(state.display);
        return 1;
    }

    if (state.xdgOutputMgr) {
        for (auto& [id, out] : state.outputs) {
            out.xdgOutput = makeShared<CCZxdgOutputV1>(state.xdgOutputMgr->sendGetXdgOutput(out.output->resource()));
            out.xdgOutput->setLogicalSize([&state, id = id](CCZxdgOutputV1*, int32_t w, int32_t h) {
                state.outputs[id].logicalW = w;
                state.outputs[id].logicalH = h;
            });
            out.xdgOutput->setLogicalPosition([&state, id = id](CCZxdgOutputV1*, int32_t x, int32_t y) {
                state.outputs[id].logicalX = x;
                state.outputs[id].logicalY = y;
            });
        }

        wl_display_roundtrip(state.display);
    }

    // one more roundtrip so a late wl_output.done / xdg_output.done can't truncate the dump
    wl_display_roundtrip(state.display);

    for (const auto& [id, out] : state.outputs)
        std::println("output {} mode {}x{}@{} scale {} logical {}x{} at {},{}", out.name, out.modeW, out.modeH, out.refresh, out.scale, out.logicalW, out.logicalH, out.logicalX,
                     out.logicalY);

    std::println("done {}", state.outputs.size());
    std::fflush(stdout);

    for (auto& [id, out] : state.outputs) {
        out.xdgOutput.reset();
        out.output.reset();
    }
    state.xdgOutputMgr.reset();
    state.registry.reset();

    wl_display_disconnect(state.display);
    return 0;
}
