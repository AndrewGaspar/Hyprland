#include "tests.hpp"
#include "../../shared.hpp"
#include "../../hyprctlCompat.hpp"
#include "../shared.hpp"
#include "../clients/build.hpp"

#include <hyprutils/os/Process.hpp>
#include <hyprutils/memory/SharedPtr.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>
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
// the in-tree hyprtester/clients/xdg-interactive, the output dump is clients/output-info, the
// pixel readback is clients/screencopy-crop).
//
// What is NOT asserted here, precisely (§3.12): the packed scanout frame itself. Every capture
// protocol is deliberately sized at the PANE (ScreenshareSession sizes SHARE_MONITOR at
// paneSize(), §3.6 — stereoRegionCaptureIsACrop asserts exactly that), so no client can ever see
// the two-pane buffer; the only readers of it are the display and the DRM commit. That leaves the
// blit loop in CHyprOpenGLImpl::end() — and the m_scissorOffset it installs — without pixel
// coverage. The damage-fold assertion below is NOT a proxy for it: the fold lives in
// CHyprRenderer (Renderer.cpp foldPaneDamage) and would still pass if the blit loop wrote
// nothing. Closing that gap needs an in-process readback of the output framebuffer (a hook in the
// test plugin) or a nested compositor whose scanout buffer is a client surface; the pane geometry
// the loop and the fold share is unit-tested in tests/output/StereoPacking.cpp.
//
// The config front-ends split across two runs: this file's `hl.monitor{ stereo = ... }` covers the
// Lua one, and stereoLegacyConfigFrontEnds (below) covers the classic monitor= / monitorv2 syntax
// under `-c ./stereo-legacy.conf`.

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

    // The same client's OTHER verb (WP D4): the mass-weighted centroid of everything on the output
    // that is not the background. A rigid translation of the foreground moves it by exactly the
    // translation, sub-pixel included, which is how a few pixels of stereo disparity become
    // measurable from outside the process.
    std::string screencopyCentroid(const std::string& monitor) {
        CProcess proc(binaryDir + "/screencopy-crop", {"--centroid", monitor});
        proc.addEnv("WAYLAND_DISPLAY", WLDISPLAY);
        if (!proc.runSync() && proc.stdOut().empty())
            return "";
        return proc.stdOut();
    }

    struct SCentroid {
        double      x = -1.0, y = -1.0, mass = 0.0;
        int         corners = 0;
        bool        stable  = false;
        std::string dump;

        bool        usable() const {
            return stable && corners == 4 && mass > 0.0 && x >= 0.0;
        }
    };

    // the token after `key `, as a double. -1 when absent.
    double doubleAfter(const std::string& dump, const std::string& key) {
        const auto POS = dump.find(key + " ");
        if (POS == std::string::npos)
            return -1.0;
        try {
            return std::stod(dump.substr(POS + key.length() + 1));
        } catch (...) { return -1.0; }
    }

    SCentroid measureCentroid(const std::string& monitor) {
        SCentroid out;
        out.dump = screencopyCentroid(monitor);

        const auto POS = out.dump.find("centroid ");
        if (POS != std::string::npos) {
            std::istringstream iss(out.dump.substr(POS + 9));
            iss >> out.x >> out.y;
        }

        out.mass    = doubleAfter(out.dump, "mass");
        out.corners = static_cast<int>(doubleAfter(out.dump, "corners"));
        out.stable  = doubleAfter(out.dump, "stable") == 1.0;
        return out;
    }

    // ...and the same measurement, repeated until two consecutive runs agree.
    //
    // The client already rejects a scene that moves between its own two captures; this rejects one
    // that moves between two RUNS of it, which is the slower kind — a client's first commit at its
    // real size, a fade timing out, a notification sliding away. Two independent runs agreeing to a
    // twentieth of a pixel is the cheapest available definition of "the picture is holding still".
    // (A notification that is simply THERE holds perfectly still and would pass; that one is
    // handled by dismissing notifications before measuring, not here.)
    SCentroid measureSettledCentroid(const std::string& monitor, int tries = 25) {
        SCentroid previous, current;

        for (int i = 0; i < tries; ++i) {
            current = measureCentroid(monitor);
            if (current.usable() && previous.usable() && std::abs(current.x - previous.x) < 0.05 && std::abs(current.y - previous.y) < 0.05)
                return current;

            previous = current;
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        NLog::log("{}the picture on {} never held still — measurements will be noisy", Colors::YELLOW, monitor);
        return current;
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

    // clients/screencopy-probe: capture the monitor and report the colour at each point. The stereo
    // CONTENT producer is invisible to every structural assertion — same box, same geometry, same
    // input region, different texels — so this is the only thing that can see it (WP S2).
    std::string screencopyProbe(const std::string& monitor, const std::vector<std::pair<int, int>>& points) {
        std::vector<std::string> args{monitor};
        for (const auto& [X, Y] : points) {
            args.push_back(std::to_string(X));
            args.push_back(std::to_string(Y));
        }

        CProcess proc(binaryDir + "/screencopy-probe", args);
        proc.addEnv("WAYLAND_DISPLAY", WLDISPLAY);
        if (!proc.runSync() && proc.stdOut().empty())
            return "";
        return proc.stdOut();
    }

    // "px <i> <x> <y> <rrggbb>" -> the colour, or "" when the line is missing
    std::string probedColour(const std::string& dump, size_t index) {
        const auto MARKER = std::format("px {} ", index);
        const auto POS    = dump.find(MARKER);
        if (POS == std::string::npos)
            return "";

        const auto EOL  = dump.find('\n', POS);
        const auto LINE = dump.substr(POS, EOL == std::string::npos ? std::string::npos : EOL - POS);
        const auto LAST = LINE.rfind(' ');
        return LAST == std::string::npos ? "" : LINE.substr(LAST + 1);
    }

    // An xdg-toplevel window, held open by a stdin pipe (child-window.cpp precedent: the client
    // polls stdin, so the write end must stay open or it spins on EOF).
    struct SWindowClient {
        CSharedPointer<CProcess> proc;
        int                      stdinWrite = -1;

        // `args` reaches clients/xdg-interactive: `--paint` fills the buffer with four distinct
        // quadrants and `--tag <t>` sets an xdg-toplevel-tag. Default (no args) is what every other
        // case in the suite uses.
        SWindowClient(std::vector<std::string> args = {}) {
            int pipeFds[2];
            if (pipe(pipeFds) != 0)
                return;

            proc       = makeShared<CProcess>(binaryDir + "/xdg-interactive", args);
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

    int intOf(const std::string& s) {
        try {
            return std::stoi(s);
        } catch (...) { return -1; }
    }

    int widthOf(const std::string& size) {
        return intOf(size);
    }

    // "key": [a, b] -> {a, b}
    std::pair<int, int> intPairIn(const std::string& json, const std::string& key) {
        const auto POS = json.find("\"" + key + "\":");
        if (POS == std::string::npos)
            return {0, 0};

        const auto OPEN  = json.find('[', POS);
        const auto CLOSE = json.find(']', OPEN);
        if (OPEN == std::string::npos || CLOSE == std::string::npos)
            return {0, 0};

        const auto INNER = json.substr(OPEN + 1, CLOSE - OPEN - 1);
        const auto COMMA = INNER.find(',');
        if (COMMA == std::string::npos)
            return {0, 0};

        try {
            return {std::stoi(INNER.substr(0, COMMA)), std::stoi(INNER.substr(COMMA + 1))};
        } catch (...) { return {0, 0}; }
    }

    struct SBox {
        int x = 0, y = 0, w = 0, h = 0;
    };

    SBox activeWindowBox() {
        const auto JSON = getFromSocket("j/activewindow");
        const auto AT   = intPairIn(JSON, "at");
        const auto SIZE = intPairIn(JSON, "size");
        return {.x = AT.first, .y = AT.second, .w = SIZE.first, .h = SIZE.second};
    }

    // THE STEREO CONTENT ASSERTION (research/24 §5.3, WP S2).
    //
    // Probe the four quadrant centres of the focused window — a client painted with four distinct
    // quadrants (xdg-interactive --paint) — and reduce them to a shape: which of the four sampled the
    // same colour as which, written as the index of the first quadrant that had each colour. The
    // monitor capture is pane-sized and taken from ONE pane's composite (§3.6), so the shape says
    // exactly which half of the client's buffer that pane sampled:
    //
    //   "abcd"  all four differ           -> no crop: the whole buffer across the whole box
    //   "aacc"  left pair, right pair      -> a side-by-side crop: the LEFT half stretched across it
    //   "abab"  top pair, bottom pair      -> an over-under crop: the TOP half stretched across it
    //
    // Relative and never absolute: the frame has been through the client's shm buffer, the
    // composite, the monitor's colour management and the capture's own format, and every one of
    // those preserves "these two pixels match" while none preserves "this pixel is red".
    //
    // ...which is also what makes it EYE-AGNOSTIC, and that is load-bearing rather than lucky. The
    // capture follows the LAST pane once the per-eye producer is running (see
    // stereoDepthDisparityMovesTheWindow), so which eye it shows depends on whether the frame took
    // the pane loop at all. A shape is invariant under that: an sbs crop reads "aacc" whether the
    // pane sampled the A/C column or the B/D one, because the signature names first occurrences
    // and not colours. What it can never read as is "abcd" — which is exactly the question.
    //
    // Points are in monitor-local coordinates, which is what screencopy captures.
    std::string paneSignature(const std::string& monitor) {
        const auto BOX = activeWindowBox();
        if (BOX.w <= 0 || BOX.h <= 0)
            return "";

        // row-major, so the probe order is top-left, top-right, bottom-left, bottom-right
        const int                        MX = intOf(monitorField(monitor, "x")), MY = intOf(monitorField(monitor, "y"));
        std::vector<std::pair<int, int>> points;
        for (const auto& fy : {0.25, 0.75}) {
            for (const auto& fx : {0.25, 0.75})
                points.emplace_back(BOX.x - MX + static_cast<int>(BOX.w * fx), BOX.y - MY + static_cast<int>(BOX.h * fy));
        }

        const auto                 DUMP = screencopyProbe(monitor, points);
        std::array<std::string, 4> colours;
        for (size_t i = 0; i < colours.size(); ++i) {
            colours[i] = probedColour(DUMP, i);
            if (colours[i].empty())
                return "";
        }

        std::string signature;
        for (size_t i = 0; i < colours.size(); ++i) {
            for (size_t j = 0; j <= i; ++j) {
                if (colours[j] == colours[i]) {
                    signature += static_cast<char>('a' + j);
                    break;
                }
            }
        }

        return signature;
    }

    // The capture is a real frame off the compositor's render cycle, so poll: the first one can
    // land before the client has painted at its configured size.
    bool waitForPaneSignature(const std::string& monitor, const std::string& want, int seconds = 8) {
        std::string last;
        for (int i = 0; i < seconds * 4; ++i) {
            last = paneSignature(monitor);
            if (last == want)
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }

        NLog::log("{}pane signature on {} was \"{}\", wanted \"{}\"", Colors::YELLOW, monitor, last, want);
        return false;
    }

    // The depth of the first window `hyprctl clients` lists, which in these tests is the only one.
    // This is WP D1's observable state and the synchronisation point for D4's pixel measurements:
    // the depth ladder is resolved on the main thread, so a capture taken after this reports the
    // new rung is a capture of the new rung.
    std::string windowDepth() {
        const auto JSON   = getFromSocket("j/clients");
        const auto KEYPOS = JSON.find("\"depth\":");
        if (KEYPOS == std::string::npos)
            return "";

        const auto VALSTART = JSON.find_first_not_of(" \t", KEYPOS + 8);
        if (VALSTART == std::string::npos)
            return "";

        const auto VALEND = JSON.find_first_of(",\n", VALSTART);
        return JSON.substr(VALSTART, (VALEND == std::string::npos ? JSON.length() : VALEND) - VALSTART);
    }

    bool waitForWindowDepth(const std::string& want, int seconds = 5) {
        for (int i = 0; i < seconds * 10; ++i) {
            if (windowDepth() == want)
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return windowDepth() == want;
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
    // this is positive proof that CHyprRenderer folded the damage — without it the right eye
    // freezes. It says nothing about the blit loop that fills the halves (see the file header):
    // the fold is computed and submitted whether or not end() drew anything.
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

        // The monitor capture is the LOGICAL view — one pane, never the packed frame (§3.6). This is
        // also the only assertion on the advertised capture SIZE (§3.4 item 14, "one fix, four
        // protocols"): wlr-screencopy is the one capture protocol with an in-tree client, and the
        // size it is told comes from CScreenshareSession's SHARE_MONITOR buffer sizing, which
        // ext-image-copy-capture, the screenshot portal and grim all reach through the same
        // CScreenshareSession — untested here only for want of clients, not for want of a fix.
        EXPECT_CONTAINS(DUMP, "full 1920x1080");
        EXPECT_CONTAINS(DUMP, std::format("region {}x{}", CROP_W, CROP_H));
        // a flat capture would make the comparison below vacuous
        EXPECT(variedIn(DUMP) > 0, true);
        EXPECT_CONTAINS(DUMP, "result ok");
    };

    cropCheck(STEREO_MON);
    cropCheck(CONTROL_MON); // the same assertion on an ordinary output, as the control
}

// stereoContentSecondComposite — WP S1's producer, live (research/24 §3.3, §5.3).
//
// The declaration half — the rule grammar, the tag, §4.3's fullscreen gate — is asserted from the
// Lua front-end in tests/main/window.cpp (stereoWindowRules). What this adds is the frame: with a
// stereo-declared window actually on a stereo output, renderMonitor stops building ONE composite
// for both panes and runs its pane loop — the same loop the depth producer runs, entered here for
// the other reason (§5.3 + §6.1: content moves UVs, depth moves geometry, either owes the second
// composite).
//
// So this case owns the "content, no depth" corner of the composition matrix, and it says so in
// two numbers that can disagree: `stereoComposites` 2 (the loop ran) with the depth ladder
// untouched, and `stereoContent` true (the crop reached a surface).
//
// The two panes' pixels are unassertable from a client (see the note at the top of this file), so
// what is otherwise proved here is narrower and still worth having: the per-eye path runs on real
// frames without crashing, hanging or losing the monitor, and the window renders at pane geometry
// while it happens. A regression that took down the compositor on any stereo desktop with an mpv
// window open would show up here as a dead socket.
TEST_CASE(stereoContentSecondComposite) {
    getFromSocket(std::format("/output remove {}", STEREO_MON));

    OK(declareMonitor(STEREO_MON, STEREO_MODE, "sbs"));
    OK(getFromSocket(std::format("/output create headless {}", STEREO_MON)));

    CScopeGuard guard = {[&]() {
        Tests::killAllWindows();
        getFromSocket(std::format("/output remove {}", STEREO_MON));
    }};

    ASSERT(waitForMonitorPresent(STEREO_MON, true), true);
    ASSERT(waitForMonitorField(STEREO_MON, "width", "1920"), true);
    Tests::sync();

    // every wayland toplevel, since the in-tree client sets no class — and `always`, because the
    // gate belongs to window.cpp's case and this one is about the producer
    OK(getFromSocket("/eval hl.window_rule({ match = { xwayland = false }, stereo = 'sbs always' })"));
    OK(getFromSocket(std::format("/dispatch hl.dsp.focus({{ monitor = '{}' }})", STEREO_MON)));

    const int     WINDOWS_BEFORE = Tests::windowCount();
    SWindowClient client;
    ASSERT(client.waitForWindow(WINDOWS_BEFORE), true);
    Tests::sync();

    // the window resolves to a packed layout, so every frame from here owes a second composite
    EXPECT_CONTAINS(getFromSocket("/activewindow"), "stereo: sbs");
    // ...and the producer says it took it. This is the assertion that the crop actually reached a
    // drawn surface: without it the case would pass just as happily on a build where the rule
    // resolved and the renderer ignored it.
    ASSERT(waitForMonitorField(STEREO_MON, "stereoContent", "true"), true);
    // ...and the frame really did cost a second composite. Nothing here has any depth (the ladder
    // is at its stock rungs and no depth rule was written), so content is the ONLY thing that can
    // have entered the pane loop — which is the "no depth, but stereo content" case.
    ASSERT(waitForMonitorField(STEREO_MON, "stereoComposites", "2"), true);
    // and it is still laid out on the PANE, not on the mode — the producer changes UVs, never boxes
    EXPECT(widthOf(activeWindowSize()) <= 1920, true);

    // let a few frames go through the per-eye path, then confirm the compositor is still there and
    // the monitor is still exactly one 1920-wide logical output
    for (int i = 0; i < 3; ++i) {
        getFromSocket(std::format("/dispatch hl.dsp.cursor.move({{ x = {}, y = 540 }})", 100 + i * 100));
        Tests::sync();
    }

    EXPECT(monitorField(STEREO_MON, "width"), std::string("1920"));
    EXPECT(monitorField(STEREO_MON, "stereo"), std::string("sbs"));
    EXPECT(monitorField(STEREO_MON, "stereoContent"), std::string("true"));
    EXPECT(Tests::countOccurrences(getFromSocket("j/monitors"), std::string("\"name\": \"") + STEREO_MON + "\""), 1);

    // and the fast path is not a claim: with the window gone the panes are identical again, so the
    // frame goes back to one composite blitted twice (research/24 §3.3's floor)
    Tests::killAllWindows();
    Tests::waitUntilWindowsN(0);
    ASSERT(waitForMonitorField(STEREO_MON, "stereoContent", "false"), true);
    ASSERT(waitForMonitorField(STEREO_MON, "stereoComposites", "1"), true);
}

// stereoTaggedWindowSamplesOneHalfPerPane — WP S2, and the only assertion in the tree that can see
// the stereo CONTENT producer at all (research/24 §4.2, §5.3, §3.12).
//
// Everything else about a stereo-declared window is identical to an ordinary one: same box, same
// geometry, same input region, same layout, same damage. The producer changes exactly one thing —
// which texels of the client's buffer a pane samples — so it is invisible to every structural
// assertion in this file, and stereoContentSecondComposite can only say that the per-eye path RAN, not
// that it sampled anything different.
//
// What closes the gap is §3.6: a monitor capture is sized at the pane and taken pre-pack, out of
// ONE pane's composite. So a client painted with four distinct quadrants tells us which half that
// composite sampled, by comparing its own quadrants to each other (paneSignature above) — a
// comparison that gives the same answer for either eye, which is why the expectations below do not
// have to know which one the capture landed on.
//
// Four cases, sharing one `stereo auto` rule that matches every wayland toplevel, so the ONLY
// difference between the stereo cases and the control is what the client itself declared:
//
//   1. a client tagged `stereo:sbs` on a stereo monitor  -> the left half, stretched  ("aacc")
//   2. a client tagged `stereo:tab` on the same monitor  -> the top half, stretched   ("abab")
//   3. an untagged client on the same monitor            -> untouched                 ("abcd")
//   4. the tagged client on a MONO monitor               -> untouched                 ("abcd")
//
// (4) is S1's documented degradation, asserted rather than assumed: a stereo window on a monitor
// that presents no pane pair shows its packed frame as-is. There is only one pane to sample into,
// so cropping half of it would be a strictly worse picture (research/24 §11, §8.6).
TEST_CASE(stereoTaggedWindowSamplesOneHalfPerPane) {
    getFromSocket(std::format("/output remove {}", STEREO_MON));
    getFromSocket(std::format("/output remove {}", CONTROL_MON));

    OK(declareMonitor(STEREO_MON, STEREO_MODE, "sbs"));
    OK(declareMonitor(CONTROL_MON, CONTROL_MODE, nullptr));

    OK(getFromSocket(std::format("/output create headless {}", STEREO_MON)));
    OK(getFromSocket(std::format("/output create headless {}", CONTROL_MON)));

    CScopeGuard guard = {[&]() {
        Tests::killAllWindows();
        // neutralise the rule for every case that runs after this one: a named rule is updated in
        // place, and `off` is the one layout that wins outright.
        getFromSocket("/eval hl.window_rule({ name = 's2-stereo-auto', stereo = 'off' })");
        getFromSocket(std::format("/output remove {}", STEREO_MON));
        getFromSocket(std::format("/output remove {}", CONTROL_MON));
    }};

    ASSERT(waitForMonitorPresent(STEREO_MON, true), true);
    ASSERT(waitForMonitorPresent(CONTROL_MON, true), true);
    ASSERT(waitForMonitorField(STEREO_MON, "width", "1920"), true);
    Tests::sync();

    // ONE rule for the whole case: tier A, and nothing else. A window is cropped here only because
    // it said so itself — which is the property the control depends on.
    OK(getFromSocket("/eval hl.window_rule({ name = 's2-stereo-auto', match = { xwayland = false }, stereo = 'auto' })"));

    const auto probeCase = [&](const char* monitor, const char* tag, const char* wantLayout, const char* wantSignature) {
        OK(getFromSocket(std::format("/dispatch hl.dsp.focus({{ monitor = '{}' }})", monitor)));

        std::vector<std::string> args{"--paint"};
        if (tag) {
            args.emplace_back("--tag");
            args.emplace_back(tag);
        }

        const int     BEFORE = Tests::windowCount();
        SWindowClient client(args);
        ASSERT(client.waitForWindow(BEFORE), true);
        Tests::sync();

        // the declaration resolved the way the case says it should — asserted first, so a signature
        // failure below is unambiguously the PRODUCER and not the rule
        EXPECT_CONTAINS(getFromSocket("/activewindow"), std::format("stereo: {}", wantLayout));
        EXPECT(waitForPaneSignature(monitor, wantSignature), true);
    };

    NLog::log("{}1. tagged stereo:sbs on the stereo output — the left half in the pane", Colors::YELLOW);
    probeCase(STEREO_MON, "stereo:sbs", "sbs", "aacc");
    Tests::killAllWindows();
    Tests::waitUntilWindowsN(0);

    NLog::log("{}2. tagged stereo:tab on the stereo output — the top half, i.e. the other axis", Colors::YELLOW);
    probeCase(STEREO_MON, "stereo:tab", "tab", "abab");
    Tests::killAllWindows();
    Tests::waitUntilWindowsN(0);

    // THE CONTROL, and the whole reason the cases above mean anything: same rule, same client, same
    // monitor, same capture — no tag. A build that cropped every window (or that ignored the eye
    // index and cropped nothing) fails exactly one of these three.
    NLog::log("{}3. untagged on the stereo output — the control: nothing is cropped", Colors::YELLOW);
    probeCase(STEREO_MON, nullptr, "off", "abcd");
    Tests::killAllWindows();
    Tests::waitUntilWindowsN(0);

    // S1's documented degradation (§11): the window still RESOLVES to sbs — the rule and the tag are
    // per-window and know nothing about outputs — but there is no pane pair to sample into, so the
    // packed frame is shown as-is rather than half of it being blown up.
    NLog::log("{}4. tagged stereo:sbs on a MONO output — the packed frame, shown as-is", Colors::YELLOW);
    probeCase(CONTROL_MON, "stereo:sbs", "sbs", "abcd");
    EXPECT(monitorField(CONTROL_MON, "stereoContent"), std::string("")); // no stereo, no content panes

    Tests::killAllWindows();
    Tests::waitUntilWindowsN(0);

    // 3 above is a control across FRAMES: nothing stereo was on screen, so the frame took the
    // single-composite fast path and no crop ever ran. This is the control WITHIN one frame — a
    // declared and an undeclared window composited together, so every frame goes through the pane
    // loop while one of the two windows must come out of it byte-identical to the first.
    //
    // That is the property the crop's placement buys (it hangs off ONE surface's UV resolution, not
    // off the monitor), and the one a per-eye pass that leaked eye state — into the pass's shared
    // render data, into a neighbouring surface, into a cached UV — would break while leaving cases
    // 1-3 green. Spawning the undeclared client second makes it the focused window without
    // unmapping the declared one, so a single probe answers it.
    NLog::log("{}5. a declared and an undeclared window in the SAME frame — only the declared one is cropped", Colors::YELLOW);
    OK(getFromSocket(std::format("/dispatch hl.dsp.focus({{ monitor = '{}' }})", STEREO_MON)));

    const int     BEFORE_TAGGED = Tests::windowCount();
    SWindowClient tagged({"--paint", "--tag", "stereo:sbs"});
    ASSERT(tagged.waitForWindow(BEFORE_TAGGED), true);
    Tests::sync();
    EXPECT(waitForPaneSignature(STEREO_MON, "aacc"), true);

    const int     BEFORE_PLAIN = Tests::windowCount();
    SWindowClient plain({"--paint"});
    ASSERT(plain.waitForWindow(BEFORE_PLAIN), true);
    Tests::sync();

    // the declared window is still on screen, so the pane loop is still running on every frame...
    EXPECT(waitForMonitorField(STEREO_MON, "stereoContent", "true"), true);
    // ...and the window that declared nothing sampled its whole buffer anyway
    EXPECT_CONTAINS(getFromSocket("/activewindow"), "stereo: off");
    EXPECT(waitForPaneSignature(STEREO_MON, "abcd"), true);
}

// stereoRuleFoldAndProvenance — which instruction wins when several apply (research/24 §4.2, §4.5).
//
// The precedence itself is a pure function and is unit-tested as a full matrix
// (tests/render/StereoContent.cpp, StereoContentResolution.*). What that cannot reach is the fold
// ABOVE it: window rules are applied in config order and the last one to set a property wins, so
// what actually reaches resolveDeclaration is the fold of every rule that matched. This case owns
// that seam, and the seam is where "I added a rule and nothing changed" comes from.
//
// Needs no stereo monitor: the resolution is per-window and `hyprctl clients` reports it directly.
TEST_CASE(stereoRuleFoldAndProvenance) {
    CScopeGuard guard = {[&]() {
        Tests::killAllWindows();
        for (const char* name : {"s2-fold-a", "s2-fold-b"})
            getFromSocket(std::format("/eval hl.window_rule({{ name = '{}', stereo = 'off' }})", name));
    }};

    const auto  ruleIs = [&](const char* name, const char* stereo) {
        OK(getFromSocket(std::format("/eval hl.window_rule({{ name = '{}', match = {{ xwayland = false }}, stereo = '{}' }})", name, stereo)));
    };

    const auto layoutOfAWindowTagged = [&](const char* tag) {
        Tests::killAllWindows();
        Tests::waitUntilWindowsN(0);

        std::vector<std::string> args;
        if (tag) {
            args.emplace_back("--tag");
            args.emplace_back(tag);
        }

        const int     BEFORE = Tests::windowCount();
        SWindowClient client(args);
        if (!client.waitForWindow(BEFORE))
            return std::string("<no window>");
        Tests::sync();

        const auto DUMP = getFromSocket("/activewindow");
        const auto POS  = DUMP.find("stereo: ");
        if (POS == std::string::npos)
            return std::string("<no field>");

        const auto EOL = DUMP.find('\n', POS);
        return DUMP.substr(POS + 8, EOL == std::string::npos ? std::string::npos : EOL - POS - 8);
    };

    // a is declared first, b second, both match -> b wins. This is the fold, and it is the ONLY
    // reason a later `windowrule = stereo off` can turn an earlier one back off.
    ruleIs("s2-fold-a", "tab always");
    ruleIs("s2-fold-b", "sbs always");
    EXPECT(layoutOfAWindowTagged(nullptr), std::string("sbs"));

    // updating the WINNER changes the answer...
    ruleIs("s2-fold-b", "htab always");
    EXPECT(layoutOfAWindowTagged(nullptr), std::string("htab"));

    // ...and updating the LOSER does not, however tempting the new value looks
    ruleIs("s2-fold-a", "sbs always");
    EXPECT(layoutOfAWindowTagged(nullptr), std::string("htab"));

    // the last rule may also be the off switch, which beats everything before it
    ruleIs("s2-fold-b", "off");
    EXPECT(layoutOfAWindowTagged(nullptr), std::string("off"));

    // --- provenance: the rule and the client can disagree, and §4.5 says the human wins ---

    ruleIs("s2-fold-a", "auto");
    ruleIs("s2-fold-b", "off");

    // `off` last: even a client that declares loudly is silenced
    EXPECT(layoutOfAWindowTagged("stereo:sbs"), std::string("off"));

    // `auto` last: the client's own declaration is honoured exactly...
    ruleIs("s2-fold-b", "auto");
    EXPECT(layoutOfAWindowTagged("stereo:htab"), std::string("htab"));
    // ...including its right to say it is NOT stereo, which is what distinguishes `stereo:mono`
    // from a client that never set a tag at all
    EXPECT(layoutOfAWindowTagged("stereo:mono"), std::string("off"));
    EXPECT(layoutOfAWindowTagged(nullptr), std::string("off"));
    // a tag outside the frozen grammar is not a declaration, and must not be guessed at
    EXPECT(layoutOfAWindowTagged("stereo:lr"), std::string("off"));
    EXPECT(layoutOfAWindowTagged("com.example.player"), std::string("off"));

    // an explicit layout after `auto` outranks the tag: the last HUMAN instruction wins, because
    // correct-looking content metadata is a liability (§4.5) and this is the user's override
    ruleIs("s2-fold-b", "tab always");
    EXPECT(layoutOfAWindowTagged("stereo:sbs"), std::string("tab"));

    // and §4.3's gate is the default: an unqualified layout on a WINDOWED window stays off, while
    // the same rule on a client that declared its own packing engages immediately
    ruleIs("s2-fold-b", "hsbs");
    EXPECT(layoutOfAWindowTagged(nullptr), std::string("off"));
    EXPECT(layoutOfAWindowTagged("stereo:sbs"), std::string("hsbs"));
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

// stereoLegacyConfigFrontEnds — the syntax users actually ship (research/24 §3.10).
//
// The rest of this file drives the Lua front-end, because the main suite runs test.lua. The token
// documented in docs/openxr/05-configuration.md §8.5 and used in example/xreal.conf is the classic
// one, and it has three separate wirings (the compact `stereo:sbs` positional token, the
// `stereo, sbs` pair, and the monitorv2 `stereo =` key) that no test reached: the gtests exercise
// CMonitorRuleParser::parseStereo, i.e. the shared leaf below all three, and `hyprctl keyword` is
// refused outright under a non-legacy parser. Unit-testing the wiring is out too — instantiating
// CConfigManager verifies (and creates) the user's real config file.
//
// So the wiring gets a second, tiny run against a classic config:
//
//   hyprtester -c ./stereo-legacy.conf -b ../build-debug/Hyprland -p <plugin> stereoLegacyConfigFrontEnds
//
// Under test.lua the case has nothing to assert and says so.
TEST_CASE(stereoLegacyConfigFrontEnds) {
    // dispatchKeyword refuses under any non-legacy parser, which is the cheapest possible probe for
    // "which front-end parsed this run's config" (and has no side effects with no arguments).
    if (getFromSocket("/keyword").contains("non-legacy")) {
        NLog::log("{}skipped: this run parses test.lua — the classic monitor= / monitorv2 stereo wiring "
                  "is covered by `hyprtester -c ./stereo-legacy.conf stereoLegacyConfigFrontEnds`",
                  Colors::YELLOW);
        return;
    }

    // one name per wiring, all declared in stereo-legacy.conf at the same 3840x1080 mode
    static const std::vector<std::pair<std::string, std::string>> WIRINGS = {
        {"HYPRTEST-LEG-POS", "the compact positional token (`, stereo:sbs`)"},
        {"HYPRTEST-LEG-KV", "the two-token positional form (`, stereo, sbs`)"},
        {"HYPRTEST-LEG-V2", "the monitorv2 `stereo =` key"},
    };
    static const std::string CONTROL = "HYPRTEST-LEG-CTL";

    CScopeGuard              guard = {[&]() {
        for (const auto& [NAME, WIRING] : WIRINGS) {
            getFromSocket(std::format("/output remove {}", NAME));
        }
        getFromSocket(std::format("/output remove {}", CONTROL));
    }};

    for (const auto& [NAME, WIRING] : WIRINGS) {
        getFromSocket(std::format("/output remove {}", NAME));
        OK(getFromSocket(std::format("/output create headless {}", NAME)));
    }
    getFromSocket(std::format("/output remove {}", CONTROL));
    OK(getFromSocket(std::format("/output create headless {}", CONTROL)));

    for (const auto& [NAME, WIRING] : WIRINGS) {
        NLog::log("{}checking {} — {}", Colors::YELLOW, NAME, WIRING);
        ASSERT(waitForMonitorPresent(NAME, true), true);

        // the pack reached the monitor: a 3840x1080 output presenting a 1920x1080 desktop
        ASSERT(waitForMonitorField(NAME, "width", "1920"), true);
        EXPECT(monitorField(NAME, "height"), std::string("1080"));
        EXPECT(monitorField(NAME, "stereo"), std::string("sbs"));
        EXPECT(monitorField(NAME, "scanoutWidth"), std::string("3840"));
        EXPECT(monitorField(NAME, "scale"), std::string("1.00"));
    }

    // the control shares the mode and omits the token: 3840 stays 3840, so a run in which every
    // rule silently packed (or none did) cannot pass
    ASSERT(waitForMonitorPresent(CONTROL, true), true);
    ASSERT(waitForMonitorField(CONTROL, "width", "3840"), true);
    EXPECT(monitorField(CONTROL, "stereo"), std::string(""));
    EXPECT(monitorField(CONTROL, "scanoutWidth"), std::string(""));

    // WP S1's content rule shares this run for the same reason the monitor token does: the classic
    // `windowrule =` handler looks the effect up by NAME, so an effect that only exists in the Lua
    // mirror's table would parse here and nowhere else (or the reverse).
    EXPECT(getFromSocket("/keyword windowrule stereo sbs always, match:class ^(mpv)$"), std::string("ok"));
    EXPECT(getFromSocket("/keyword windowrule stereo auto, match:xdg_tag ^stereo:.*"), std::string("ok"));
    // and a layout the grammar does not have is refused rather than silently ignored
    EXPECT_CONTAINS(getFromSocket("/keyword windowrule stereo lr, match:class ^(mpv)$"), "unknown layout");
}

// stereoDepthProducerFastPath — WP D2's cost model, made observable (research/24 §6.4.1).
//
// The producer composites the desktop ONCE PER PANE, which doubles the compositing work on a
// stereo output. The thing that keeps that acceptable is the fast path: when nothing on the output
// is actually off the wallpaper plane the two panes are the same image, so one composite is built
// and end()'s pack duplicates it — byte for byte the frame WP F1 shipped.
//
// `stereoComposites` in `hyprctl monitors` is the witness for which path ran, and it is the only
// one: the panes are internal (every capture protocol is sized at the pane by design, §3.12) and
// the second composite has no other outward sign. So this asserts the transition in both
// directions — empty desktop is 1, a window at the focused rung is 2, and §6.4's A/B toggle
// (`depth_scale = 0`, i.e. "same ladder, no rise") puts it back to 1 without touching a rule.
//
// WHAT THE NUMBER MEANS, precisely, because it is load-bearing here: it is counted in
// finishStereoPane as each finished pane is HANDED TO THE PACK, not predicted from the producer's
// predicate. That distinction is the whole reason this test can see anything at all — the pane
// buffer comes from a pool of eight, and when the pool is empty finishStereoPane logs a warning and
// keeps the pane it drew, so end() blits ONE composite into both halves and the frame is flat. A
// predicted `2` would report a stereo frame that was never presented, and the pixel test below
// cannot tell the difference either, because it measures the last pane and the last pane is the
// one that got duplicated. So: `2` here means two distinct composites reached the pack, and the
// log check at the end means the fallback did not fire even once during the run.
//
// The disparity ARITHMETIC is not tested here — tests/desktop/DepthTiers.cpp owns §8.1's worked
// table, the eye sign, §6.1's edge clamp and the sub-pixel warning. This test only proves the
// predicate that chooses between one composite and two is wired to the real depth state.
TEST_CASE(stereoDepthProducerFastPath) {
    Tests::killAllWindows();
    getFromSocket(std::format("/output remove {}", STEREO_MON));

    const size_t LOGMARK = readHyprlandLog().size(); // only count warnings caused by THIS test

    OK(declareMonitor(STEREO_MON, STEREO_MODE, "sbs"));
    OK(getFromSocket(std::format("/output create headless {}", STEREO_MON)));

    CScopeGuard guard = {[&]() {
        Tests::killAllWindows();
        getFromSocket("/eval hl.config({ decoration = { depth_scale = 0.12 } })");
        getFromSocket(std::format("/output remove {}", STEREO_MON));
    }};

    ASSERT(waitForMonitorPresent(STEREO_MON, true), true);
    ASSERT(waitForMonitorField(STEREO_MON, "width", "1920"), true);
    Tests::sync();

    // === 1. an empty output takes the fast path ===
    //
    // Nothing is on it, so nothing has depth, so both panes would be identical. One composite.
    ASSERT(waitForMonitorField(STEREO_MON, "stereoComposites", "1"), true);

    // === 2. a window arrives on the focused rung and the producer starts ===
    {
        OK(getFromSocket(std::format("/dispatch hl.dsp.focus({{ monitor = '{}' }})", STEREO_MON)));
        const int     BEFORE = Tests::windowCount();
        SWindowClient client;
        ASSERT(client.waitForWindow(BEFORE), true);
        Tests::sync();

        // decoration:depth_focused is 0.6, so this window is 3.3 px of disparity off the page and
        // the two panes can no longer be the same image
        EXPECT(waitForMonitorField(STEREO_MON, "stereoComposites", "2"), true);

        // === 3. §6.4's free A/B toggle: the ladder is untouched, the rise is zero ===
        //
        // The predicate asks "would anything actually MOVE", not "does anything have a depth", so
        // flattening the comfort knob is enough to buy the cheap frame back with the same desktop
        // on screen. This is the switch the ergonomics spike (D0) needs and the escape hatch for
        // anyone who wants F1's exact cost on a stereo output.
        OK(getFromSocket("/eval hl.config({ decoration = { depth_scale = 0 } })"));
        EXPECT(waitForMonitorField(STEREO_MON, "stereoComposites", "1"), true);

        OK(getFromSocket("/eval hl.config({ decoration = { depth_scale = 0.12 } })"));
        EXPECT(waitForMonitorField(STEREO_MON, "stereoComposites", "2"), true);
    }

    Tests::waitUntilWindowsN(0);

    // === 4. and the output goes back to one composite when the desktop empties ===
    EXPECT(waitForMonitorField(STEREO_MON, "stereoComposites", "1"), true);

    // the compositor survived a frame that bound a second work buffer mid-pass
    EXPECT_CONTAINS(getFromSocket("/version"), "Hyprland");

    // === 5. ...and it never ran out of them, i.e. no frame silently degraded to a flat pair ===
    //
    // The `2` above is counted at hand-over, so a frame that lost this race would have reported 1
    // and failed the wait. This says the same thing about every OTHER frame in the run, including
    // the ones nothing was waiting on.
    const std::string LOGSINCE = readHyprlandLog().substr(std::min(LOGMARK, readHyprlandLog().size()));
    EXPECT(LOGSINCE.contains("out of work buffers"), false);
}

// stereoDepthDisparityMovesTheWindow — WP D4's pixel assertion: the depth desktop, measured.
//
// stereoDepthProducerFastPath proves the producer RUNS. This proves it produces the right picture:
// a window on a rung of the depth ladder is drawn at a different horizontal position than the same
// window on the wallpaper plane, by the number of pixels §8.1's formula asks for — and by a
// different number on a different rung, which is what says the shift belongs to the ELEMENT.
//
// HOW A FEW PIXELS BECOME MEASURABLE. The client (`screencopy-crop --centroid`) reports the
// mass-weighted centroid of everything that is not the background. Under a uniform background —
// hence the logo and splash going off for the duration — the foreground is the window, its border
// and its shadow, all of which the producer translates together; and the first moment of a rigid
// translation is exact, so antialiasing, linear filtering, the rounded corners and the sub-pixel
// seam in ElementRenderer all wash out of the measurement instead of quantising it. That is what
// lets a 9.45 px disparity be checked against 9.45 rather than against "it moved".
//
// WHICH EYE THIS IS. One pane, not two: every capture protocol is sized at the pane by design
// (§3.6), and on a depth-producing output the mirror texture the capture is taken from follows the
// LAST pane — the right eye, whose sign is negative (§8.1: "left pane +, right pane −"). So the
// numbers below are the right eye's, and the left eye is its exact negation by construction —
// tests/desktop/DepthTiers.cpp asserts that antisymmetry on the shared expression both panes run.
// A capture that started showing pane 0 would flip every sign here, and this comment is the place
// to change.
//
// THE DEPTH-0 CONTROL is the same window at the same size in the same place with the rung set to
// 0: it must land back on the flat centroid, which is what makes the middle measurements a
// statement about depth rather than about capture noise.
TEST_CASE(stereoDepthDisparityMovesTheWindow) {
    // §8.1's worked table at `depth_scale = 0.30`: a rise of 0.30 m is ±9.5 px per pane on a
    // 1920-wide pane, and 0.15 m is ±4.2 px (the formula is not linear in depth — parallax goes as
    // 1 − D/d). Both are inside the 1° comfort ceiling (15.7 px) and well inside the tiled window's
    // 20 px gap, so §6.1's edge clamp does not bite.
    //
    // The measured numbers are −9.46 and −4.47 px against −9.45 and −4.20 predicted; the tolerance
    // is a pixel and a half, which leaves room for whatever the texture path rounds while staying
    // well under the 5 px gap between the two rungs.
    constexpr double DEPTH_SCALE = 0.30;
    constexpr double SHIFT_FULL  = 9.45; // depth 1.0
    constexpr double SHIFT_HALF  = 4.20; // depth 0.5
    constexpr double TOL         = 1.5;

    Tests::killAllWindows();
    getFromSocket(std::format("/output remove {}", STEREO_MON));

    OK(declareMonitor(STEREO_MON, STEREO_MODE, "sbs"));
    OK(getFromSocket(std::format("/output create headless {}", STEREO_MON)));

    CScopeGuard guard = {[&]() {
        Tests::killAllWindows();
        getFromSocket("/eval hl.config({ decoration = { depth_scale = 0.12, depth_focused = 0.6 } })");
        getFromSocket("/eval hl.config({ misc = { disable_hyprland_logo = false, disable_splash_rendering = false } })");
        getFromSocket(std::format("/output remove {}", STEREO_MON));
    }};

    ASSERT(waitForMonitorPresent(STEREO_MON, true), true);
    ASSERT(waitForMonitorField(STEREO_MON, "width", "1920"), true);

    // the measurement's one precondition: a uniform background. The wallpaper is a photograph and
    // the splash is text — both are static mass that would dilute the centroid of the thing that
    // moves, and the client reports `corners 4` so a run where this did not take cannot pass
    // quietly.
    OK(getFromSocket("/eval hl.config({ misc = { disable_hyprland_logo = true, disable_splash_rendering = true } })"));
    OK(getFromSocket(std::format("/eval hl.config({{ decoration = {{ depth_scale = {}, depth_focused = 0 }} }})", DEPTH_SCALE)));

    OK(getFromSocket(std::format("/dispatch hl.dsp.focus({{ monitor = '{}' }})", STEREO_MON)));

    const int     BEFORE = Tests::windowCount();
    SWindowClient client;
    ASSERT(client.waitForWindow(BEFORE), true);
    Tests::sync();

    // The window's depth comes from the FOCUSED tier (it is the only window, and it is focused), so
    // `decoration:depth_focused` walks it up and down the ladder without touching a rule — and
    // every step is confirmed in `hyprctl clients` before the capture is taken.
    // (plain EXPECTs, never OK/ASSERT: those return from the enclosing function, which a lambda
    // with a return value cannot do)
    const auto measureAt = [&](double depth, const char* composites) -> SCentroid {
        EXPECT(getFromSocket(std::format("/eval hl.config({{ decoration = {{ depth_focused = {} }} }})", depth)), std::string("ok"));
        EXPECT(waitForWindowDepth(std::format("{:.3f}", depth)), true);
        EXPECT(waitForMonitorField(STEREO_MON, "stereoComposites", composites), true);

        // An on-screen notification is mass like any other, it sits in the top-right corner, and it
        // outlives the test that raised it — the stereo suite alone leaves two behind (a mode that
        // cannot be split, a monitor un-stereo'd by a write-back). One in a capture is worth about
        // 6 px of centroid, i.e. most of the disparity being measured, so the screen is cleared
        // before every measurement rather than hoping the timers have run out.
        getFromSocket("/dismissnotify");
        Tests::sync();

        const auto MEASURED = measureSettledCentroid(STEREO_MON);
        if (!MEASURED.usable())
            NLog::log("{}screencopy-crop --centroid at depth {} said:\n{}", Colors::YELLOW, depth, MEASURED.dump);
        EXPECT(MEASURED.usable(), true);
        NLog::log("{}depth {}: centroid {:.3f}, {:.3f} (mass {:.0f})", Colors::YELLOW, depth, MEASURED.x, MEASURED.y, MEASURED.mass);
        return MEASURED;
    };

    // === 1. the flat reference — nothing is off the plane, so this is WP F1's frame ===
    const auto FLAT = measureAt(0.0, "1");
    ASSERT(FLAT.usable(), true); // nothing below means anything without a reference

    // === 2. depth 1.0 is 9.45 px of disparity, toward the left in the right eye ===
    const auto FULL = measureAt(1.0, "2");
    EXPECT_MAX_DELTA(FULL.x - FLAT.x, -SHIFT_FULL, TOL);

    // === 3. half as high is half as far — the shift is the ELEMENT's depth, not a frame offset ===
    //
    // This is the assertion a whole-pane translation bug cannot pass: if the producer offset the
    // pane's projection instead of each raised element, every rung would move by the same amount.
    const auto HALF = measureAt(0.5, "2");
    EXPECT_MAX_DELTA(HALF.x - FLAT.x, -SHIFT_HALF, TOL);
    EXPECT(std::abs(FULL.x - FLAT.x) > std::abs(HALF.x - FLAT.x) + 2.0, true); // the rungs are distinguishable

    // === 4. the depth-0 control lands back exactly where it started ===
    //
    // Half a pixel, not zero: the scene is a live desktop and the cursor, a fading toast or a
    // border gradient can be worth a fraction of a pixel of mass. It is an order of magnitude below
    // the shift being measured, which is all this has to be.
    const auto BACK = measureAt(0.0, "1");
    EXPECT_MAX_DELTA(BACK.x - FLAT.x, 0.0, 0.5);
    EXPECT_MAX_DELTA(BACK.y - FLAT.y, 0.0, 0.5);

    // === 5. and nothing ever moved VERTICALLY: disparity is horizontal, or it is eye strain ===
    EXPECT_MAX_DELTA(FULL.y - FLAT.y, 0.0, 0.5);
    EXPECT_MAX_DELTA(HALF.y - FLAT.y, 0.0, 0.5);
}

// stereoDepthShippedLadderIsSubPixel — the same measurement, in the regime users actually run.
//
// stereoDepthDisparityMovesTheWindow deliberately turns the comfort knob up to `depth_scale = 0.30`
// so the shift (9.45 px) is far larger than any rounding the render path might do. That makes it a
// good test of the SIGN and the PROPORTIONALITY and a useless test of §8.1's number-one risk: at
// nine pixels, quantising the disparity to whole pixels is a 5 % error and every assertion there
// still passes.
//
// The shipped ladder is the opposite regime. At `depth_scale = 0.12` the rungs are 0.61 px (depth
// 0.2) and 0.93 px (depth 0.3) — two different rungs and the SAME INTEGER. Round the shifted box
// and they render identically; everything between depth 0 and 0.3 collapses into one plane, which
// is exactly the failure §8.1 predicted. So this measures those two rungs and asserts they are
// distinguishable on screen.
//
// It is the pixel-level counterpart to DepthTiers.theSubPixelSeamKeepsTwoNearbyRungsApart, which
// asserts the same property of the expression; this one asserts it of the composited frame, i.e.
// that every drawer in the frame — surface, border, shadow, glow — really does run that expression.
TEST_CASE(stereoDepthShippedLadderIsSubPixel) {
    // §8.1 at the SHIPPED `decoration:depth_scale = 0.12`, one pane of a 3840x1080 sbs output.
    constexpr double SHIFT_LOW  = 0.61; // depth 0.2 — the shipped unfocused rung
    constexpr double SHIFT_HIGH = 0.93; // depth 0.3
    // A whole pixel of tolerance on each absolute shift, which is generous — but the assertion that
    // matters is the SEPARATION, and there a whole pixel would be meaningless: integer rounding
    // makes it exactly zero and the sub-pixel path makes it 0.32.
    constexpr double TOL_ABS = 1.0;
    constexpr double TOL_SEP = 0.15;

    Tests::killAllWindows();
    getFromSocket(std::format("/output remove {}", STEREO_MON));

    OK(declareMonitor(STEREO_MON, STEREO_MODE, "sbs"));
    OK(getFromSocket(std::format("/output create headless {}", STEREO_MON)));

    CScopeGuard guard = {[&]() {
        Tests::killAllWindows();
        getFromSocket("/eval hl.config({ decoration = { depth_scale = 0.12, depth_focused = 0.6 } })");
        getFromSocket("/eval hl.config({ misc = { disable_hyprland_logo = false, disable_splash_rendering = false } })");
        getFromSocket(std::format("/output remove {}", STEREO_MON));
    }};

    ASSERT(waitForMonitorPresent(STEREO_MON, true), true);
    ASSERT(waitForMonitorField(STEREO_MON, "width", "1920"), true);

    OK(getFromSocket("/eval hl.config({ misc = { disable_hyprland_logo = true, disable_splash_rendering = true } })"));
    OK(getFromSocket("/eval hl.config({ decoration = { depth_scale = 0.12, depth_focused = 0 } })"));

    OK(getFromSocket(std::format("/dispatch hl.dsp.focus({{ monitor = '{}' }})", STEREO_MON)));

    const int     BEFORE = Tests::windowCount();
    SWindowClient client;
    ASSERT(client.waitForWindow(BEFORE), true);
    Tests::sync();

    const auto measureAt = [&](double depth, const char* composites) -> SCentroid {
        EXPECT(getFromSocket(std::format("/eval hl.config({{ decoration = {{ depth_focused = {} }} }})", depth)), std::string("ok"));
        EXPECT(waitForWindowDepth(std::format("{:.3f}", depth)), true);
        EXPECT(waitForMonitorField(STEREO_MON, "stereoComposites", composites), true);

        getFromSocket("/dismissnotify");
        Tests::sync();

        const auto MEASURED = measureSettledCentroid(STEREO_MON);
        EXPECT(MEASURED.usable(), true);
        NLog::log("{}shipped ladder, depth {}: centroid {:.3f}, {:.3f}", Colors::YELLOW, depth, MEASURED.x, MEASURED.y);
        return MEASURED;
    };

    const auto FLAT = measureAt(0.0, "1");
    ASSERT(FLAT.usable(), true);

    const auto LOW  = measureAt(0.2, "2");
    const auto HIGH = measureAt(0.3, "2");

    // === 1. both rungs are where the formula says, to within a pixel ===
    EXPECT_MAX_DELTA(LOW.x - FLAT.x, -SHIFT_LOW, TOL_ABS);
    EXPECT_MAX_DELTA(HIGH.x - FLAT.x, -SHIFT_HIGH, TOL_ABS);

    // === 2. ...and they are NOT the same place, which is the whole assertion ===
    //
    // 0.32 px apart. A render path that rounded the disparity would put both at exactly 1 px and
    // this difference would be 0.00 — the ladder collapsed, silently, in the only regime anyone
    // ships. Note the direction as well as the magnitude: higher is further left in this pane.
    EXPECT(HIGH.x < LOW.x - TOL_SEP, true);
    EXPECT_MAX_DELTA(HIGH.x - LOW.x, -(SHIFT_HIGH - SHIFT_LOW), 0.35);

    // === 3. and neither of them moved vertically ===
    EXPECT_MAX_DELTA(LOW.y - FLAT.y, 0.0, 0.5);
    EXPECT_MAX_DELTA(HIGH.y - FLAT.y, 0.0, 0.5);
}

// stereoDepthStaysFlatOnARotatedOutput — the one geometry the disparity has no answer for.
//
// The producer turns a depth into a LOGICAL +x offset, and logical +x is the panel's horizontal
// axis under exactly one transform: the normal one. Under 90°/270° deriveGeometry transposes the
// pane, so that offset would run DOWN the panel — vertical disparity, which the eyes cannot fuse
// into depth and which §8.2 lists among the things that cause strain rather than discomfort. Under
// 180° and the flipped transforms the axis survives but reverses, which swaps the eyes and inverts
// the ladder: every window sinks BEHIND the page, against the one-sided design §8.2 point 2 chose
// deliberately.
//
// There is no correct shift to apply in either case, so the producer declines: every spread is 0,
// nothing is raised, and the output falls back to the single composite F1 shipped. This asserts
// that fallback — and, in the second half, that the SAME desktop produces two composites the moment
// the transform goes back to normal, which is what makes the first half about the transform rather
// than about some unrelated reason the producer might have been idle.
TEST_CASE(stereoDepthStaysFlatOnARotatedOutput) {
    Tests::killAllWindows();
    getFromSocket(std::format("/output remove {}", STEREO_MON));

    // 90° — the transposing case, where the disparity axis would be the panel's vertical one
    OK(getFromSocket(std::format("/eval hl.monitor({{ output = '{}', mode = '{}', position = 'auto-right', scale = '1', stereo = 'sbs', transform = 1 }})", STEREO_MON,
                                 STEREO_MODE)));
    OK(getFromSocket(std::format("/output create headless {}", STEREO_MON)));

    CScopeGuard guard = {[&]() {
        Tests::killAllWindows();
        getFromSocket(std::format("/output remove {}", STEREO_MON));
    }};

    ASSERT(waitForMonitorPresent(STEREO_MON, true), true);

    // The pack itself is untouched by the rotation — `width`/`height` are paneSize(), the scanout
    // pane, which is what the mode was split into and is transform-independent. (What the transform
    // transposes is the LOGICAL size the desktop is laid out in, which these fields do not report.)
    // Asserting it here is the control: the output really is a packed stereo output, so a `1` below
    // is the depth producer declining and not the stereo path having fallen over.
    ASSERT(waitForMonitorField(STEREO_MON, "transform", "1"), true);
    EXPECT(monitorField(STEREO_MON, "width"), std::string("1920"));
    EXPECT(monitorField(STEREO_MON, "height"), std::string("1080"));
    EXPECT(monitorField(STEREO_MON, "scanoutWidth"), std::string("3840"));
    EXPECT(monitorField(STEREO_MON, "stereo"), std::string("sbs"));

    OK(getFromSocket(std::format("/dispatch hl.dsp.focus({{ monitor = '{}' }})", STEREO_MON)));

    // a focused window on the shipped ladder — depth 0.6, i.e. the case that WOULD composite twice
    const int     BEFORE = Tests::windowCount();
    SWindowClient client;
    ASSERT(client.waitForWindow(BEFORE), true);
    Tests::sync();

    ASSERT(waitForWindowDepth("0.600"), true);

    // === 1. rotated: the window carries a depth and the output is still ONE composite ===
    //
    // Note what is NOT asserted: that depth is zero. The ladder is untouched — `hyprctl clients`
    // still reports 0.600 — because this is a property of the OUTPUT, not of the window. Move the
    // same window to an unrotated stereo monitor and it rises there.
    EXPECT(waitForMonitorField(STEREO_MON, "stereoComposites", "1"), true);

    // === 2. ...and the same desktop composites twice the moment the transform is normal ===
    OK(getFromSocket(std::format("/eval hl.monitor({{ output = '{}', mode = '{}', position = 'auto-right', scale = '1', stereo = 'sbs', transform = 0 }})", STEREO_MON,
                                 STEREO_MODE)));

    ASSERT(waitForMonitorField(STEREO_MON, "transform", "0"), true);
    EXPECT(monitorField(STEREO_MON, "width"), std::string("1920"));

    EXPECT(waitForMonitorField(STEREO_MON, "stereoComposites", "2"), true);
}

