#include "tests.hpp"
#include "../../shared.hpp"
#include "../../hyprctlCompat.hpp"
#include "../shared.hpp"
#include "../clients/build.hpp"

#include <hyprutils/os/Process.hpp>
#include <hyprutils/memory/SharedPtr.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>

#include <chrono>
#include <csignal>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace Hyprutils::OS;
using namespace Hyprutils::Memory;
using namespace Hyprutils::Utils;

// Depth as a decoration axis — the integration half of WP D1 (research/24 §7).
//
// D1 ships STATE, not pixels: the only way to see whether a window or a layer surface is on the
// right rung of the depth ladder is `hyprctl clients` / `hyprctl layers`. That is exactly what this
// asserts, and it is the one thing the unit tests cannot: tests/desktop/DepthTiers.cpp proves the
// ladder arithmetic, this proves the arithmetic is actually WIRED — that updateDepth() runs on the
// paths that matter (map, focus, rule change, layer tier) and that the rule store the Lua front-end
// writes into is the same store the compositor reads (research/24 F8, the xrrule mistake).
//
// No kitty: both clients are in-tree (hyprtester/clients/xdg-interactive, clients/layer-surface),
// so this runs on the host and in the hermetic container alike.
//
// Deliberately NOT asserted: anything visual. There is nothing visual to assert until D2's producer
// reads m_depth — and if a future change makes depth affect rendering without going through D2,
// this test will keep passing, which is correct. It guards the state, not the picture.

namespace {
    constexpr const char* DEPTH_LAYER_NS = "depth-test-layer";

    // ConfigValues.cpp's shipped ladder; test.lua sets no decoration:depth_* overrides.
    constexpr const char* DEPTH_FOCUSED = "0.600";
    constexpr const char* DEPTH_LAYERS  = "0.800";
    constexpr const char* DEPTH_RULE    = "0.350";

    // the value of the first `"depth": <n>` at or after `from`. "" when absent. The JSON is
    // pretty-printed one key per line, so the line is the value.
    std::string depthAfter(const std::string& json, size_t from = 0) {
        const auto KEYPOS = json.find("\"depth\":", from);
        if (KEYPOS == std::string::npos)
            return "";

        const auto VALSTART = json.find_first_not_of(" \t", KEYPOS + 8);
        if (VALSTART == std::string::npos)
            return "";

        const auto VALEND = json.find_first_of(",\n", VALSTART);
        return json.substr(VALSTART, (VALEND == std::string::npos ? json.length() : VALEND) - VALSTART);
    }

    // the depth of the layer surface with the given namespace. Layer entries print depth BEFORE
    // namespace, so search backwards from the namespace to stay inside the right object.
    std::string layerDepth(const std::string& json, const std::string& ns) {
        const auto NSPOS = json.find("\"namespace\": \"" + ns + "\"");
        if (NSPOS == std::string::npos)
            return "";

        const auto KEYPOS = json.rfind("\"depth\":", NSPOS);
        if (KEYPOS == std::string::npos)
            return "";

        return depthAfter(json, KEYPOS);
    }

    // poll until the predicate holds; every step here is asynchronous (map, rule refresh, animation
    // goal propagation), so nothing may be read one-shot.
    template <typename F>
    std::string pollFor(F&& read, const std::string& want) {
        std::string got;
        for (int i = 0; i < 100; ++i) {
            got = read();
            if (got == want)
                return got;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return got;
    }
}

TEST_CASE(depthLadderIsObservable) {
    // --- a window arrives focused, so it arrives on the focused rung -----------------------------

    int pipeFds[2];
    ASSERT(pipe(pipeFds) == 0, true);

    // held open by a stdin pipe: xdg-interactive polls stdin and spins on EOF (child-window.cpp)
    auto windowProc = makeShared<CProcess>(binaryDir + "/xdg-interactive", std::vector<std::string>{});
    windowProc->setStdinFD(pipeFds[0]);
    windowProc->addEnv("WAYLAND_DISPLAY", WLDISPLAY);
    windowProc->runAsync();

    CScopeGuard windowGuard = {[&]() {
        if (windowProc && Tests::processAlive(windowProc->pid()))
            kill(windowProc->pid(), SIGTERM);
        close(pipeFds[1]);
    }};

    bool        windowUp = false;
    for (int i = 0; i < 100 && !windowUp; ++i) {
        windowUp = Tests::windowCount() == 1;
        if (!windowUp)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ASSERT(windowUp, true);
    Tests::sync();

    const auto CLIENT_DEPTH = pollFor([] { return depthAfter(getFromSocket("j/clients")); }, DEPTH_FOCUSED);
    EXPECT(CLIENT_DEPTH, std::string{DEPTH_FOCUSED});

    // --- a rule beats the tier, and it arrives through the SHARED store -------------------------

    // hl.window_rule is the Lua front-end. If depth were stored anywhere but the shared rule engine
    // this would report "ok" and change nothing — silently — which is precisely report F8's bug.
    OK(getFromSocket("/eval hl.window_rule({ name = 'depth-integration', match = { class = '.*' }, depth = 0.35 })"));

    const auto RULED_DEPTH = pollFor([] { return depthAfter(getFromSocket("j/clients")); }, DEPTH_RULE);
    EXPECT(RULED_DEPTH, std::string{DEPTH_RULE});

    // --- a top layer surface sits on the layer rung ---------------------------------------------

    auto layerProc = makeShared<CProcess>(binaryDir + "/layer-surface", std::vector<std::string>{DEPTH_LAYER_NS});
    layerProc->addEnv("WAYLAND_DISPLAY", WLDISPLAY);
    layerProc->runAsync();

    CScopeGuard layerGuard = {[&]() {
        if (layerProc && Tests::processAlive(layerProc->pid()))
            kill(layerProc->pid(), SIGTERM);
    }};

    bool        layerUp = false;
    for (int i = 0; i < 100 && !layerUp; ++i) {
        layerUp = getFromSocket("/layers").contains(std::string{"namespace: "} + DEPTH_LAYER_NS + ",");
        if (!layerUp)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ASSERT(layerUp, true);
    Tests::sync();

    const auto LAYER_DEPTH = pollFor([] { return layerDepth(getFromSocket("j/layers"), DEPTH_LAYER_NS); }, DEPTH_LAYERS);
    EXPECT(LAYER_DEPTH, std::string{DEPTH_LAYERS});
}
