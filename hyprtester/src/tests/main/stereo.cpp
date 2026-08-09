#include "tests.hpp"
#include "../../shared.hpp"
#include "../../hyprctlCompat.hpp"
#include "../shared.hpp"
#include "../clients/build.hpp"

#include <hyprutils/os/Process.hpp>
#include <hyprutils/memory/SharedPtr.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace Hyprutils::OS;
using namespace Hyprutils::Memory;
using namespace Hyprutils::Utils;

// Stereo SBS output — the integration half of WP F3 (research/24 §3, §3.12).
//
// The user invariant these tests defend: a physical 3840x1080 output configured `stereo:sbs`
// presents as EXACTLY ONE logical monitor of 1920x1080 to everything above the scanout — hyprctl,
// the wl_output/xdg_output protocol, the layout — with no phantom monitors, and `stereo:off` is
// indistinguishable from a build without the feature.
//
// Runs fully headless on the host: no container, no XR runtime, no kitty (the window client is
// the in-tree hyprtester/clients/xdg-interactive, the output dump is clients/output-info).
//
// Known gap (§3.12): hyprtester has no pixel readback, so "the two panes hold the same image" is
// not asserted here. The damage assertion below is the closest structural proxy — it proves the
// compositor submitted both halves — and the pane geometry itself is unit-tested in
// tests/output/StereoPacking.cpp.

namespace {
    constexpr const char* STEREO_MON  = "HYPRTEST-STEREO";
    constexpr const char* CONTROL_MON = "HYPRTEST-STEREO-CTL";
    constexpr const char* STEREO_MODE = "3840x1080@60";
    // the control is the pane of the stereo monitor, as an ordinary output: same desktop, no pack
    constexpr const char* CONTROL_MODE = "1920x1080@60";

    // --- tiny JSON field readers (j/monitors is pretty-printed, one key per line) ---

    // the raw token after "key": , up to the next , or newline. "" when absent.
    std::string fieldIn(const std::string& json, const std::string& key) {
        const auto KEYPOS = json.find("\"" + key + "\":");
        if (KEYPOS == std::string::npos)
            return "";

        auto valStart = json.find_first_not_of(" \t", KEYPOS + key.length() + 3);
        if (valStart == std::string::npos)
            return "";

        auto valEnd = json.find_first_of(",\n", valStart);
        if (valEnd == std::string::npos)
            valEnd = json.length();

        std::string out = json.substr(valStart, valEnd - valStart);
        while (!out.empty() && (out.back() == ' ' || out.back() == '"' || out.back() == '\r'))
            out.pop_back();
        if (!out.empty() && out.front() == '"')
            out.erase(out.begin());
        return out;
    }

    // ONE monitor's object, so a field read can never spill into the next monitor's fields.
    // The nested workspace objects close with "\n    }," — only the outer object closes at "\n}".
    std::string monitorObject(const std::string& name) {
        const auto JSON = getFromSocket("j/monitors");
        const auto POS  = JSON.find("\"name\": \"" + name + "\"");
        if (POS == std::string::npos)
            return "";

        const auto START = JSON.rfind('{', POS);
        const auto END   = JSON.find("\n}", POS);
        if (START == std::string::npos)
            return "";
        return JSON.substr(START, END == std::string::npos ? std::string::npos : END - START);
    }

    std::string monitorField(const std::string& name, const std::string& key) {
        return fieldIn(monitorObject(name), key);
    }

    int monitorCount() {
        return Tests::countOccurrences(getFromSocket("j/monitors"), "\"description\":");
    }

    // add/replace the monitor rule. Single quotes so the lua needs no escaping (layer.cpp precedent).
    // A null `scale` omits the key entirely, which is how a rule asks for an AUTO scale.
    std::string declareMonitor(const char* name, const char* mode, const char* stereo, const char* scale = "1") {
        std::string spec = std::format("hl.monitor({{ output = '{}', mode = '{}', position = 'auto-right'", name, mode);
        if (scale)
            spec += std::format(", scale = '{}'", scale);
        if (stereo)
            spec += std::format(", stereo = '{}'", stereo);
        spec += " })";
        return getFromSocket("/eval " + spec);
    }

    // Wait for a rule change to reach the monitor. The rule manager applies scheduled reloads from
    // the render pre-check, so a monitor that is not currently repainting needs a nudge —
    // force_renderer_reload re-applies every rule synchronously (Actions::forceRendererReload).
    bool waitForMonitorField(const std::string& name, const std::string& key, const std::string& want, int seconds = 8) {
        for (int i = 0; i < seconds * 10; ++i) {
            if (monitorField(name, key) == want)
                return true;
            if (i == 5) // the natural (damage-driven) path had its chance; make it deterministic
                getFromSocket("/dispatch hl.dsp.force_renderer_reload()");
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return monitorField(name, key) == want;
    }

    bool waitForMonitorPresent(const std::string& name, bool present, int seconds = 10) {
        for (int i = 0; i < seconds * 10; ++i) {
            if (getFromSocket("/monitors").contains(name) == present)
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return getFromSocket("/monitors").contains(name) == present;
    }

    // --- clients ---

    // What a CLIENT is told about the outputs (wl_output.mode + xdg_output.logical_size), which is
    // the half of the invariant `hyprctl monitors` cannot speak for.
    std::string clientOutputInfo() {
        CProcess proc(binaryDir + "/output-info", {});
        proc.addEnv("WAYLAND_DISPLAY", WLDISPLAY);
        if (!proc.runSync())
            return "";
        return proc.stdOut();
    }

    // The suite's only pixel readback (§3.12's stated gap): clients/screencopy-crop captures the whole
    // output and then a region of it over wlr-screencopy, and reports whether the region is
    // byte-identical to the matching sub-rect of the full frame. Returns its stdout.
    std::string screencopyCrop(const std::string& monitor, int x, int y, int w, int h) {
        CProcess proc(binaryDir + "/screencopy-crop", {monitor, std::to_string(x), std::to_string(y), std::to_string(w), std::to_string(h)});
        proc.addEnv("WAYLAND_DISPLAY", WLDISPLAY);
        if (!proc.runSync() && proc.stdOut().empty())
            return "";
        return proc.stdOut();
    }

    // "varied <n>" — pixels in the reference crop that differ from its first pixel. Zero means the
    // captured area was flat, which would make the crop comparison pass for the wrong reason.
    int variedIn(const std::string& dump) {
        const auto POS = dump.find("varied ");
        if (POS == std::string::npos)
            return -1;
        try {
            return std::stoi(dump.substr(POS + 7));
        } catch (...) { return -1; }
    }

    // the line for one output, or "" — e.g. "output HYPRTEST-STEREO mode 1920x1080@60000 scale 1 logical 1920x1080 at 1920,0"
    std::string outputInfoLine(const std::string& dump, const std::string& name) {
        std::istringstream iss(dump);
        for (std::string line; std::getline(iss, line);) {
            if (line.starts_with("output " + name + " "))
                return line;
        }
        return "";
    }

    // An xdg-toplevel window, held open by a stdin pipe (child-window.cpp precedent: the client
    // polls stdin, so the write end must stay open or it spins on EOF).
    struct SWindowClient {
        CSharedPointer<CProcess> proc;
        int                      stdinWrite = -1;

        SWindowClient() {
            int pipeFds[2];
            if (pipe(pipeFds) != 0)
                return;

            proc       = makeShared<CProcess>(binaryDir + "/xdg-interactive", std::vector<std::string>{});
            stdinWrite = pipeFds[1];
            proc->setStdinFD(pipeFds[0]);
            proc->addEnv("WAYLAND_DISPLAY", WLDISPLAY);
            proc->runAsync();
        }

        ~SWindowClient() {
            if (proc && Tests::processAlive(proc->pid()))
                kill(proc->pid(), SIGTERM);
            if (stdinWrite >= 0)
                close(stdinWrite);
        }

        bool waitForWindow(int before) {
            if (!proc)
                return false;

            for (int i = 0; i < 100; ++i) {
                if (Tests::windowCount() == before + 1)
                    return true;
                if (!Tests::processAlive(proc->pid()))
                    return false;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            return false;
        }
    };

    // "size": [1880, 1040] -> "1880,1040"
    std::string activeWindowSize() {
        const auto JSON = getFromSocket("j/activewindow");
        const auto POS  = JSON.find("\"size\":");
        if (POS == std::string::npos)
            return "";

        const auto OPEN  = JSON.find('[', POS);
        const auto CLOSE = JSON.find(']', OPEN);
        if (OPEN == std::string::npos || CLOSE == std::string::npos)
            return "";

        std::string out;
        for (char c : JSON.substr(OPEN + 1, CLOSE - OPEN - 1)) {
            if (c != ' ')
                out += c;
        }
        return out;
    }

    int widthOf(const std::string& size) {
        try {
            return std::stoi(size);
        } catch (...) { return -1; }
    }

    // --- the log (the only in-process view of what damage was submitted) ---

    std::string readHyprlandLog() {
        const char* xdg = std::getenv("XDG_RUNTIME_DIR");
        if (!xdg || HIS.empty())
            return "";

        // Logger.cpp writes hyprlandd.log for HYPRLAND_DEBUG (Debug) builds, hyprland.log
        // otherwise — exactly one of them. Read the FIRST that exists rather than concatenating,
        // so a byte offset into this string stays meaningful.
        const std::string base = std::string(xdg) + "/hypr/" + HIS + "/";
        for (const char* fn : {"hyprlandd.log", "hyprland.log"}) {
            std::ifstream f(base + fn);
            if (!f)
                continue;
            std::stringstream ss;
            ss << f.rdbuf();
            return ss.str();
        }
        return "";
    }

    // widest "Damage: Stereo fold on <name> (...): extents xy: X, Y wh: W, H" in the log.
    // Pane damage can never exceed the pane width, so W > paneW is positive proof the fold ran.
    int widestStereoFold(const std::string& log, const std::string& monitor) {
        const std::string MARKER = "Damage: Stereo fold on " + monitor + " ";
        int               widest = -1;

        for (size_t pos = log.find(MARKER); pos != std::string::npos; pos = log.find(MARKER, pos + MARKER.length())) {
            const auto EOL = log.find('\n', pos);
            const auto WH  = log.find("wh: ", pos);
            if (WH == std::string::npos || (EOL != std::string::npos && WH > EOL))
                continue;

            try {
                widest = std::max(widest, std::stoi(log.substr(WH + 4)));
            } catch (...) { continue; }
        }

        return widest;
    }
}

// stereoSbsOneLogicalMonitor — the structural contract of the flat SBS presenter.
//
// Catches, in order: a stereo output reported at its packed mode; a phantom second monitor; a
// client told the packed mode over wl_output/xdg_output; a window laid out on 3840 instead of
// 1920; a leaked hardware cursor plane; a missing direct-scanout/solitary block; a missing damage
// fold (the right eye going stale).
TEST_CASE(stereoSbsOneLogicalMonitor) {
    // Declare the rules BEFORE creating the outputs: test.lua ends with a catch-all
    // `hl.monitor({ output = "", disabled = true })`, so an output with no rule of its own is
    // created and immediately disabled (layer.cpp precedent).
    getFromSocket(std::format("/output remove {}", STEREO_MON));
    getFromSocket(std::format("/output remove {}", CONTROL_MON));

    OK(declareMonitor(STEREO_MON, STEREO_MODE, "sbs"));
    OK(declareMonitor(CONTROL_MON, CONTROL_MODE, nullptr));

    const int MONITORS_BEFORE = monitorCount();

    OK(getFromSocket(std::format("/output create headless {}", STEREO_MON)));
    OK(getFromSocket(std::format("/output create headless {}", CONTROL_MON)));

    CScopeGuard guard = {[&]() {
        Tests::killAllWindows();
        getFromSocket("/eval hl.config({ debug = { log_damage = false } })");
        getFromSocket(std::format("/output remove {}", STEREO_MON));
        getFromSocket(std::format("/output remove {}", CONTROL_MON));
    }};

    ASSERT(waitForMonitorPresent(STEREO_MON, true), true);
    ASSERT(waitForMonitorPresent(CONTROL_MON, true), true);
    Tests::sync();

    // === 1. exactly ONE logical monitor, at the per-eye pane size ===

    ASSERT(waitForMonitorField(STEREO_MON, "width", "1920"), true);
    EXPECT(monitorField(STEREO_MON, "height"), std::string("1080"));

    // no phantom: the pack added exactly the two outputs we created, not four
    EXPECT(monitorCount(), MONITORS_BEFORE + 2);
    EXPECT(Tests::countOccurrences(getFromSocket("j/monitors"), std::string("\"name\": \"") + STEREO_MON + "\""), 1);

    // the packing is discoverable rather than a mystery (§3.4 item 12)
    EXPECT(monitorField(STEREO_MON, "stereo"), std::string("sbs"));
    EXPECT(monitorField(STEREO_MON, "scanoutWidth"), std::string("3840"));
    EXPECT(monitorField(STEREO_MON, "scanoutHeight"), std::string("1080"));
    // scale is untouched by the pack — the desktop is 1920 logical at scale 1, not 3840 at scale 2
    EXPECT(monitorField(STEREO_MON, "scale"), std::string("1.00"));

    // the control output must carry none of it
    EXPECT(monitorField(CONTROL_MON, "width"), std::string("1920"));
    EXPECT(monitorField(CONTROL_MON, "stereo"), std::string(""));
    EXPECT(monitorField(CONTROL_MON, "scanoutWidth"), std::string(""));

    // === 2. the mandatory blocks (F1 items 7, 8, 10) ===

    // a packed scanout frame can never be a client buffer, and the solitary shortcut would leave
    // the second pane stale — both report STEREO as the blocking reason. The quotes matter: they
    // pin the match to the JSON reason token rather than the monitor's own name.
    EXPECT_CONTAINS(monitorObject(STEREO_MON), "\"STEREO\"");
    EXPECT_NOT_CONTAINS(monitorObject(CONTROL_MON), "\"STEREO\"");
    // a hardware cursor plane draws ONE cursor into the packed buffer -> one-eyed pointer (§3.7)
    EXPECT(monitorField(STEREO_MON, "hardwareCursorsInUse"), std::string("false"));

    // === 3. what CLIENTS are told (wl_output.mode + xdg_output.logical_size) ===

    const auto DUMP        = clientOutputInfo();
    const auto STEREO_LINE = outputInfoLine(DUMP, STEREO_MON);
    if (STEREO_LINE.empty())
        NLog::log("{}output-info dump was:\n{}", Colors::YELLOW, DUMP);
    ASSERT_NOT(STEREO_LINE, std::string(""));

    EXPECT_CONTAINS(STEREO_LINE, "mode 1920x1080@");
    EXPECT_CONTAINS(STEREO_LINE, "logical 1920x1080");
    EXPECT_CONTAINS(STEREO_LINE, "scale 1 ");
    // the packed mode must never leak to a client — a fullscreen client that believed it could
    // scan out at 3840 is exactly risk 12 in the report
    EXPECT_NOT_CONTAINS(STEREO_LINE, "3840");

    // === 4. a window on it is laid out on 1920, exactly like the plain 1920 control ===
    //
    // log_damage is enabled around the stereo window only: it is the sole way to observe the
    // submitted damage from outside the process, and it is loud.

    OK(getFromSocket("/eval hl.config({ debug = { log_damage = true } })"));
    const size_t LOGMARK = readHyprlandLog().size(); // only count folds caused by THIS window

    std::string  stereoWindowSize, controlWindowSize;
    {
        OK(getFromSocket(std::format("/dispatch hl.dsp.focus({{ monitor = '{}' }})", STEREO_MON)));
        const int     BEFORE = Tests::windowCount();
        SWindowClient client;
        ASSERT(client.waitForWindow(BEFORE), true);
        Tests::sync();
        stereoWindowSize = activeWindowSize();
    }
    Tests::waitUntilWindowsN(0);
    Tests::sync();

    // === 5. the damage fold reached both halves of the scanout buffer (§3.4 item 6) ===
    //
    // Pane damage is bounded by the pane (1920). Anything wider can only come from the fold, so
    // this is positive proof rather than a smoke test. Without it the right eye freezes.
    //
    // Polled, and nudged with a full re-render each round: the log is written by the compositor
    // asynchronously, and a headless monitor only repaints when something damages it.
    int WIDEST = -1;
    for (int i = 0; i < 50 && WIDEST <= 1920; ++i) {
        const auto LOG = readHyprlandLog();
        WIDEST         = widestStereoFold(LOG.substr(std::min(LOGMARK, LOG.size())), STEREO_MON);
        if (WIDEST > 1920)
            break;

        getFromSocket("/dispatch hl.dsp.force_renderer_reload()"); // damages every monitor
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    OK(getFromSocket("/eval hl.config({ debug = { log_damage = false } })"));

    if (WIDEST <= 1920)
        NLog::log("{}no stereo fold wider than a pane found in the log after the client commit (widest: {})", Colors::YELLOW, WIDEST);
    EXPECT(WIDEST > 1920, true);

    {
        OK(getFromSocket(std::format("/dispatch hl.dsp.focus({{ monitor = '{}' }})", CONTROL_MON)));
        const int     BEFORE = Tests::windowCount();
        SWindowClient client;
        ASSERT(client.waitForWindow(BEFORE), true);
        Tests::sync();
        controlWindowSize = activeWindowSize();
    }
    Tests::waitUntilWindowsN(0);

    ASSERT_NOT(stereoWindowSize, std::string(""));
    // the whole point: the stereo output lays out exactly like an ordinary 1920x1080 one
    EXPECT(stereoWindowSize, controlWindowSize);
    // and it is derived from the pane, not the packed mode (which would be ~3840 wide)
    EXPECT(widthOf(stereoWindowSize) > 1000 && widthOf(stereoWindowSize) <= 1920, true);
}

// stereoAutoScaleStaysAtOne — the rule with no `scale` key at all.
//
// An auto scale must not guess a scale for a stereo output. The guess reads the pixel density of
// the PACKED mode (3840 wide, ~1.8x the pane's horizontal density) and the fractional-scale
// validator downstream divides the pane, so 1920/2 = 960 sails through: the monitor would come up
// as a 960x540 desktop, below the resolution it presents per eye, with no warning anywhere (the
// scale warning is deliberately only for explicit scales). research/24 §3.8 wants 1.0 here.
TEST_CASE(stereoAutoScaleStaysAtOne) {
    getFromSocket(std::format("/output remove {}", STEREO_MON));

    OK(declareMonitor(STEREO_MON, STEREO_MODE, "sbs", nullptr /* no scale key => auto */));
    OK(getFromSocket(std::format("/output create headless {}", STEREO_MON)));

    CScopeGuard guard = {[&]() { getFromSocket(std::format("/output remove {}", STEREO_MON)); }};

    ASSERT(waitForMonitorPresent(STEREO_MON, true), true);
    ASSERT(waitForMonitorField(STEREO_MON, "scale", "1.00"), true);

    // the invariant, restated for the auto case: one logical monitor at the presented per-eye size
    EXPECT(monitorField(STEREO_MON, "width"), std::string("1920"));
    EXPECT(monitorField(STEREO_MON, "height"), std::string("1080"));
    EXPECT(monitorField(STEREO_MON, "scanoutWidth"), std::string("3840"));
    EXPECT(monitorField(STEREO_MON, "stereo"), std::string("sbs"));
}

// stereoRegionCaptureIsACrop — the one thing structural assertions cannot see: the IMAGE.
//
// A region capture must be a 1:1 crop of the monitor's own capture. On a stereo monitor every draw
// is projected through outputProjection(paneSize), so a capture buffer smaller than the pane has to
// CLIP that projection; give the pass a viewport of the buffer's own size instead and the whole
// desktop is squeezed into the region rectangle. Both captures go through the shm path, which is
// what grim / the screenshot portal / hyprshot use.
//
// The plain monitor is checked with the same client and the same rectangle, which is what makes
// this a stereo assertion rather than a screencopy smoke test.
TEST_CASE(stereoRegionCaptureIsACrop) {
    // a rectangle in the bottom-right corner: offset on BOTH axes (a crop that ignored the offset
    // would still match at the origin) and guaranteed structure — it straddles the tiled window's
    // corner, its border and the gap behind it.
    constexpr int CROP_X = 1600, CROP_Y = 800, CROP_W = 320, CROP_H = 280;

    getFromSocket(std::format("/output remove {}", STEREO_MON));
    getFromSocket(std::format("/output remove {}", CONTROL_MON));

    OK(declareMonitor(STEREO_MON, STEREO_MODE, "sbs"));
    OK(declareMonitor(CONTROL_MON, CONTROL_MODE, nullptr));

    OK(getFromSocket(std::format("/output create headless {}", STEREO_MON)));
    OK(getFromSocket(std::format("/output create headless {}", CONTROL_MON)));

    CScopeGuard guard = {[&]() {
        Tests::killAllWindows();
        getFromSocket(std::format("/output remove {}", STEREO_MON));
        getFromSocket(std::format("/output remove {}", CONTROL_MON));
    }};

    ASSERT(waitForMonitorPresent(STEREO_MON, true), true);
    ASSERT(waitForMonitorPresent(CONTROL_MON, true), true);
    ASSERT(waitForMonitorField(STEREO_MON, "width", "1920"), true);

    const auto cropCheck = [&](const char* monitor) {
        OK(getFromSocket(std::format("/dispatch hl.dsp.focus({{ monitor = '{}' }})", monitor)));

        // the window (borders, rounding, the gap behind it) is what puts structure under the crop
        // rectangle; it must be gone again before the next monitor's turn, so it lives in its own
        // scope — waitUntilWindowsN(0) can only succeed after the client has been reaped.
        std::string DUMP;
        {
            const int     BEFORE = Tests::windowCount();
            SWindowClient client;
            ASSERT(client.waitForWindow(BEFORE), true);
            Tests::sync();

            DUMP = screencopyCrop(monitor, CROP_X, CROP_Y, CROP_W, CROP_H);
        }
        Tests::waitUntilWindowsN(0);
        Tests::sync();

        if (!DUMP.contains("result ok"))
            NLog::log("{}screencopy-crop on {} said:\n{}", Colors::YELLOW, monitor, DUMP);

        // the monitor capture is the LOGICAL view — one pane, never the packed frame (§3.6)
        EXPECT_CONTAINS(DUMP, "full 1920x1080");
        EXPECT_CONTAINS(DUMP, std::format("region {}x{}", CROP_W, CROP_H));
        // a flat capture would make the comparison below vacuous
        EXPECT(variedIn(DUMP) > 0, true);
        EXPECT_CONTAINS(DUMP, "result ok");
    };

    cropCheck(STEREO_MON);
    cropCheck(CONTROL_MON); // the same assertion on an ordinary output, as the control
}

// stereoLiveToggleRestoresState — the hot-reload ordering risk.
//
// A stereo flip changes the logical size derivation, the render-resource sizes and the matrices,
// all recomputed inside applyMonitorRule in a specific order (the old pane must be captured before
// the new mode is adopted, or the render resources are never reset). off -> on -> off must not
// crash, must not leak the pack, and must land back on a monitor indistinguishable from the one we
// started with.
TEST_CASE(stereoLiveToggleRestoresState) {
    getFromSocket(std::format("/output remove {}", STEREO_MON));

    OK(declareMonitor(STEREO_MON, STEREO_MODE, nullptr));
    OK(getFromSocket(std::format("/output create headless {}", STEREO_MON)));

    CScopeGuard guard = {[&]() { getFromSocket(std::format("/output remove {}", STEREO_MON)); }};

    ASSERT(waitForMonitorPresent(STEREO_MON, true), true);
    ASSERT(waitForMonitorField(STEREO_MON, "width", "3840"), true);

    // The fields that must survive a round trip. Deliberately excludes the volatile ones
    // (solitary/directScanoutTo pointers, focus, workspace ids); everything geometric or
    // stereo-derived is here.
    static const std::vector<std::string> FIELDS = {"width", "height", "refreshRate", "x", "y", "scale", "transform", "disabled", "mirrorOf", "hardwareCursorsInUse", "stereo"};

    const auto                            snapshot = [&]() {
        std::string out;
        const auto  OBJ = monitorObject(STEREO_MON);
        for (const auto& f : FIELDS)
            out += f + "=" + fieldIn(OBJ, f) + ";";
        return out;
    };

    const std::string BEFORE = snapshot();
    NLog::log("{}stereo:off snapshot: {}", Colors::YELLOW, BEFORE);
    EXPECT_CONTAINS(BEFORE, "width=3840;");
    EXPECT_CONTAINS(BEFORE, "stereo=;"); // no stereo key at all when off — byte-identical to stock

    // two full cycles, because a leak in the resource reset shows up on the SECOND flip
    for (int cycle = 0; cycle < 2; ++cycle) {
        NLog::log("{}stereo toggle cycle {}", Colors::YELLOW, cycle);

        OK(declareMonitor(STEREO_MON, STEREO_MODE, "sbs"));
        ASSERT(waitForMonitorField(STEREO_MON, "width", "1920"), true);
        EXPECT(monitorField(STEREO_MON, "stereo"), std::string("sbs"));
        EXPECT(monitorField(STEREO_MON, "scanoutWidth"), std::string("3840"));
        Tests::sync();

        OK(declareMonitor(STEREO_MON, STEREO_MODE, "off"));
        ASSERT(waitForMonitorField(STEREO_MON, "width", "3840"), true);
        Tests::sync();

        EXPECT(snapshot(), BEFORE); // cycle N left nothing behind
    }

    // a mode that cannot be split must refuse the pack loudly instead of deriving 960.5px panes
    OK(declareMonitor(STEREO_MON, "1921x1080@60", "sbs"));
    getFromSocket("/dispatch hl.dsp.force_renderer_reload()");
    Tests::sync();
    if (monitorField(STEREO_MON, "width") == "1921")
        EXPECT(monitorField(STEREO_MON, "stereo"), std::string("")); // packing dropped, monitor intact
    else
        NLog::log("{}backend did not take the odd 1921 mode (width is {}) — odd-mode sanitize covered by the gtests", Colors::YELLOW, monitorField(STEREO_MON, "width"));

    // and the compositor is still alive and answering (a crash here is the whole point of the test)
    EXPECT_CONTAINS(getFromSocket("/version"), "Hyprland");
}
