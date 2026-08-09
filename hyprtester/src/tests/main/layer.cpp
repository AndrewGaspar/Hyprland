#include "../../Log.hpp"
#include "../shared.hpp"
#include "tests.hpp"
#include "../../shared.hpp"
#include "../../hyprctlCompat.hpp"
#include "../clients/build.hpp"
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <format>
#include <hyprutils/os/Process.hpp>
#include <hyprutils/memory/WeakPtr.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>
#include <thread>

using namespace Hyprutils::OS;
using namespace Hyprutils::Memory;
using namespace Hyprutils::Utils;

static bool spawnLayer(const std::string& namespace_, const std::vector<std::string>& args = {}) {
    NLog::log("{}Spawning kitty layer {}", Colors::YELLOW, namespace_);
    if (!Tests::spawnLayerKitty(namespace_, args)) {
        NLog::log("{}Error: {} layer did not spawn", Colors::RED, namespace_);
        return false;
    }
    return true;
}

static std::string getLayerLine(const std::string& layers, const std::string& target) {

    auto pos = layers.find("namespace: " + target);
    if (pos == std::string::npos)
        return "";

    auto start = layers.rfind('\n', pos);
    start      = (start == std::string::npos) ? 0 : start + 1;

    auto end = layers.find('\n', pos);

    return layers.substr(start, end - start);
}

TEST_CASE(plugin_layerrules) {

    EXPECT(spawnLayer("rule-layer"), true);

    OK(getFromSocket("/eval hl.plugin.test.add_layer_rule()"));
    OK(getFromSocket("/reload"));

    OK(getFromSocket("/eval hl.layer_rule({ match = { namespace = 'rule-layer' }, plugin_rule = 'effect' })"));

    EXPECT(spawnLayer("rule-layer"), true);

    EXPECT(spawnLayer("norule-layer"), true);

    OK(getFromSocket("/eval hl.plugin.test.check_layer_rule()"));
}

TEST_CASE(layerPointerFocusPreservedOnKeyboardRefocus) {
    static constexpr const char* LAYER_NAMESPACE = "pointer-focus-layer";

    OK(getFromSocket("/eval hl.config({ input = { follow_mouse = 0 } })"));

    OK(getFromSocket("/dispatch hl.dsp.focus({ workspace = '1' })"));
    ASSERT(!!Tests::spawnKitty("pointer_focus_ws1"), true);

    OK(getFromSocket("/dispatch hl.dsp.focus({ workspace = '2' })"));
    ASSERT(!!Tests::spawnKitty("pointer_focus_ws2"), true);

    OK(getFromSocket("/dispatch hl.dsp.focus({ workspace = '1' })"));
    OK(getFromSocket("/dispatch hl.dsp.focus({ window = 'class:pointer_focus_ws1' })"));

    ASSERT(spawnLayer(LAYER_NAMESPACE, {"--edge=top", "--layer=top", "--lines=48px", "--focus-policy=not-allowed"}), true);

    OK(getFromSocket(std::format("/eval hl.plugin.test.set_pointer_focus_layer('{}')", LAYER_NAMESPACE)));
    OK(getFromSocket(std::format("/eval hl.plugin.test.check_pointer_focus_layer('{}')", LAYER_NAMESPACE)));

    OK(getFromSocket("/dispatch hl.dsp.focus({ workspace = '2' })"));
    ASSERT_CONTAINS(getFromSocket("/activewindow"), "class: pointer_focus_ws2\n");
    OK(getFromSocket(std::format("/eval hl.plugin.test.check_pointer_focus_layer('{}')", LAYER_NAMESPACE)));
}

TEST_CASE(layerVisibilityOnFs) {

    // For default handled fullscreen

    static constexpr const char* LAYER_NAMESPACE = "bar-like-layer";

    ASSERT(spawnLayer(LAYER_NAMESPACE, {"--edge=top", "--layer=top", "--lines=48px", "--focus-policy=not-allowed"}), true);

    Tests::spawnKitty("cat");

    {
        auto str = getLayerLine(getFromSocket("/layers"), LAYER_NAMESPACE);
        EXPECT_CONTAINS(str, "a: 1")
        EXPECT_CONTAINS(getFromSocket("/activewindow"), "fullscreen: 0");
    }

    OK(getFromSocket("/dispatch hl.dsp.window.fullscreen({ mode = 'maximized', action = 'set', window = 'class:cat' })"));

    {

        auto str = getLayerLine(getFromSocket("/layers"), LAYER_NAMESPACE);
        EXPECT_CONTAINS(str, "a: 1")
        EXPECT_CONTAINS(getFromSocket("/activewindow"), "fullscreen: 1");
    }

    OK(getFromSocket("/dispatch hl.dsp.window.fullscreen({ mode = 'maximized', action = 'unset', window = 'class:cat' })"));

    {
        auto str = getLayerLine(getFromSocket("/layers"), LAYER_NAMESPACE);
        EXPECT_CONTAINS(str, "a: 1")
        EXPECT_CONTAINS(getFromSocket("/activewindow"), "fullscreen: 0");
    }

    OK(getFromSocket("/dispatch hl.dsp.window.fullscreen({ mode = 'fullscreen', action = 'set', window = 'class:cat' })"));

    {
        auto str = getLayerLine(getFromSocket("/layers"), LAYER_NAMESPACE);
        EXPECT_CONTAINS(str, "a: 0")
        EXPECT_CONTAINS(getFromSocket("/activewindow"), "fullscreen: 2");
    }

    OK(getFromSocket("/dispatch hl.dsp.window.fullscreen({ mode = 'fullscreen', action = 'unset', window = 'class:cat' })"));

    {
        auto str = getLayerLine(getFromSocket("/layers"), LAYER_NAMESPACE);
        EXPECT_CONTAINS(str, "a: 1")
        EXPECT_CONTAINS(getFromSocket("/activewindow"), "fullscreen: 0");
    }
}

TEST_CASE(windowRefocusRestoresKeyboardFocusAfterSurfaceFocusCleared) {
    static constexpr const char* WINDOW_CLASS = "keyboard_refocus_target";

    OK(getFromSocket("/dispatch hl.dsp.focus({ workspace = '1' })"));
    ASSERT(!!Tests::spawnKitty(WINDOW_CLASS), true);
    OK(getFromSocket(std::format("/dispatch hl.dsp.focus({{ window = 'class:{}' }})", WINDOW_CLASS)));
    ASSERT_CONTAINS(getFromSocket("/activewindow"), std::format("class: {}\n", WINDOW_CLASS));
    OK(getFromSocket(std::format("/eval hl.plugin.test.check_keyboard_focus_window('{}')", WINDOW_CLASS)));

    OK(getFromSocket("/eval hl.plugin.test.clear_surface_focus()"));
    OK(getFromSocket(std::format("/eval hl.plugin.test.window_soft_focus('{}')", WINDOW_CLASS)));

    OK(getFromSocket(std::format("/eval hl.plugin.test.check_keyboard_focus_window('{}')", WINDOW_CLASS)));
}

// A monitor that is repositioned by arrangeMonitors must take its layer surfaces with it.
// CLayerSurface::m_geometry is cached in GLOBAL coordinates (arrangeLayerArray anchors it to
// pMonitor->m_position), and nothing else re-derives it on a move: monitor.layoutChanged only
// reaches CSpace (windows), and CLayerSurface::onCommit re-arranges only on a layer-shell *state*
// commit, not a plain buffer commit. So before the fix, removing a monitor to the left of another
// left the right-hand monitor's wallpaper/bar stranded at the old absolute x forever — the output
// rendered empty. Live repro 2026-08-02: `hyprctl openxr destroy XR-6`/`XR-7` shifted XR-8
// 12288 -> 8192 while its wallpaper + waybar stayed at 12288, and the XR quad went flat gray.
static int monitorX(const std::string& name) {
    const auto MONS = getFromSocket("/monitors");
    const auto POS  = MONS.find("Monitor " + name + " (ID");
    if (POS == std::string::npos)
        return -1;
    const auto AT = MONS.find(" at ", POS); // "\t1920x1080@60.00000 at 1920x0"
    if (AT == std::string::npos)
        return -1;
    return std::atoi(MONS.c_str() + AT + 4); // stops at the 'x' separator
}

static int layerX(const std::string& namespace_) {
    const auto LAYERS = getFromSocket("/layers");
    const auto POS    = LAYERS.find("namespace: " + namespace_ + ",");
    if (POS == std::string::npos)
        return -1;
    auto lineStart  = LAYERS.rfind('\n', POS);
    lineStart       = (lineStart == std::string::npos) ? 0 : lineStart + 1;
    const auto XYWH = LAYERS.find("xywh: ", lineStart); // "Layer abc: xywh: 1920 0 1920 48, a: 1, ..."
    if (XYWH == std::string::npos || XYWH > POS)
        return -1;
    return std::atoi(LAYERS.c_str() + XYWH + 6);
}

TEST_CASE(layersFollowAMonitorMovedByAnotherMonitorGoingAway) {
    static constexpr const char* LEFT_MON        = "HYPRTEST-LSMOVE-L";
    static constexpr const char* RIGHT_MON       = "HYPRTEST-LSMOVE-R";
    static constexpr const char* LAYER_NAMESPACE = "lsmove-layer";

    // Two extra outputs, auto-placed to the right of the default one, in order: L then R.
    // test.lua ends with a catch-all `hl.monitor({ output = "", disabled = true })`, so an output
    // with no rule of its own is created and then immediately disabled — it must be declared first
    // (same order as crossMonitorFullscreenFocus in focus.cpp) or it never reaches /monitors.
    getFromSocket(std::format("/output remove {}", LEFT_MON));
    getFromSocket(std::format("/output remove {}", RIGHT_MON));
    for (const auto* m : {LEFT_MON, RIGHT_MON}) {
        OK(getFromSocket(std::format("/eval hl.monitor({{ output = '{}', mode = '1920x1080@60', position = 'auto-right', scale = '1' }})", m)));
    }
    OK(getFromSocket(std::format("/output create headless {}", LEFT_MON)));
    OK(getFromSocket(std::format("/output create headless {}", RIGHT_MON)));
    Tests::sync();

    CScopeGuard guard = {[&]() {
        Tests::killAllLayers();
        getFromSocket(std::format("/output remove {}", LEFT_MON));
        getFromSocket(std::format("/output remove {}", RIGHT_MON));
    }};

    // Both outputs must have materialized before anything can be focused or placed on them.
    // `output create` answers "ok" as soon as the backend accepts the request, so poll.
    std::string MONS;
    for (int i = 0; i < 100; ++i) {
        MONS = getFromSocket("/monitors");
        if (MONS.contains(LEFT_MON) && MONS.contains(RIGHT_MON))
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    NLog::log("{}monitors after create:\n{}", Colors::YELLOW, MONS);
    ASSERT_CONTAINS(MONS, LEFT_MON);
    ASSERT_CONTAINS(MONS, RIGHT_MON);

    // A layer surface with no explicit wl_output lands on the focused monitor (CLayerSurface::create).
    // Uses the in-tree layer-surface client rather than `kitty +kitten panel`: kitty is absent from
    // both this host and the hermetic container image, so a kitty-based case could not run anywhere.
    OK(getFromSocket(std::format("/dispatch hl.dsp.focus({{ monitor = '{}' }})", RIGHT_MON)));

    auto layerProc = makeUnique<CProcess>(binaryDir + "/layer-surface", std::vector<std::string>{LAYER_NAMESPACE});
    layerProc->addEnv("WAYLAND_DISPLAY", WLDISPLAY);
    layerProc->runAsync();
    CScopeGuard layerGuard = {[&]() {
        if (layerProc && Tests::processAlive(layerProc->pid()))
            Safe::signalPid(layerProc->pid(), SIGTERM);
    }};

    bool        layerUp = false;
    for (int i = 0; i < 100 && !layerUp; ++i) {
        layerUp = getFromSocket("/layers").contains(std::string{"namespace: "} + LAYER_NAMESPACE + ",");
        if (!layerUp)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ASSERT(layerUp, true);
    Tests::sync();

    const int RIGHT_X_BEFORE = monitorX(RIGHT_MON);
    const int LEFT_X         = monitorX(LEFT_MON);
    ASSERT_NOT(RIGHT_X_BEFORE, -1);
    ASSERT_NOT(LEFT_X, -1);
    // Sanity: the layer really is on RIGHT_MON, i.e. focusmonitor placed it where we expect.
    ASSERT(layerX(LAYER_NAMESPACE), RIGHT_X_BEFORE);

    // Drop the monitor to its left. arrangeMonitors re-packs the auto chain and RIGHT_MON slides left.
    OK(getFromSocket(std::format("/output remove {}", LEFT_MON)));
    Tests::sync();

    const int RIGHT_X_AFTER = monitorX(RIGHT_MON);
    ASSERT_NOT(RIGHT_X_AFTER, -1);
    ASSERT_NOT(RIGHT_X_AFTER, RIGHT_X_BEFORE); // the monitor really moved, else the test proves nothing

    // The regression: the layer must be at the monitor's NEW x, not stranded at the old one.
    EXPECT(layerX(LAYER_NAMESPACE), RIGHT_X_AFTER);
}
