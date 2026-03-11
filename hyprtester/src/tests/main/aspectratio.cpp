#include <cmath>

#include "../shared.hpp"
#include "../../shared.hpp"
#include "../../hyprctlCompat.hpp"
#include "tests.hpp"

#include <hyprutils/string/VarList2.hpp>

using namespace Hyprutils::String;

static int ret = 0;

// Monitor: 1920x1080, gaps_out=20, gaps_in=5, border_size=2
// Single tiled window on a plain workspace (no smart gaps):
//   calcPos (pre-reserved):  {20, 20}
//   calcSize (pre-reserved): {1880, 1040}
//   After border (RESERVED): realPos:{22,22}  realSize:{1876,1036}
//
// 1:1 ratio applied before RESERVED:
//   constrainedSize.x = 1040, centred → calcPos.x = 20+420=440
//   After RESERVED: realPos:{442,22}  realSize:{1036,1036}
// (identical result to layout:single_window_aspect_ratio 1:1)

struct WindowSize {
    int w = 0, h = 0;
};

static WindowSize parseSize(const std::string& winInfo) {
    auto pos = winInfo.find("size:");
    if (pos == std::string::npos)
        return {};
    auto data = winInfo.substr(pos + 5);
    data      = data.substr(0, data.find('\n'));
    CVarList2 parts(std::move(data), 0, ',');
    try {
        return {std::stoi(std::string{parts[0]}), std::stoi(std::string{parts[1]})};
    } catch (...) { return {}; }
}

struct WindowPos {
    int x = 0, y = 0;
};

static WindowPos parsePos(const std::string& winInfo) {
    auto pos = winInfo.find("at:");
    if (pos == std::string::npos)
        return {};
    auto data = winInfo.substr(pos + 3);
    data      = data.substr(0, data.find('\n'));
    CVarList2 parts(std::move(data), 0, ',');
    try {
        return {std::stoi(std::string{parts[0]}), std::stoi(std::string{parts[1]})};
    } catch (...) { return {}; }
}

static bool spawnKitty(const std::string& class_ = "") {
    if (!Tests::spawnKitty(class_)) {
        NLog::log("{}Error: kitty ({}) did not spawn", Colors::RED, class_.empty() ? "default" : class_);
        ret = 1;
        return false;
    }
    return true;
}

// ──────────────────────────────────────────────────────────────────────────────
// Test 1: basic 1:1 tiled via windowrule applied to a NEW window
// ──────────────────────────────────────────────────────────────────────────────
static void testTiledNewWindow() {
    NLog::log("{}aspectratio: tiled – rule applied to new window", Colors::GREEN);

    OK(getFromSocket("/keyword windowrule match:class kitty, aspect_ratio 1:1"));

    if (!spawnKitty())
        return;

    {
        auto win = getFromSocket("/activewindow");
        EXPECT_CONTAINS(win, "at: 442,22");
        EXPECT_CONTAINS(win, "size: 1036,1036");
    }

    // getprop should echo back 1.0
    {
        auto prop = getFromSocket("/getprop active aspect_ratio");
        EXPECT_CONTAINS(prop, "1");
    }

    Tests::killAllWindows();
    OK(getFromSocket("/reload"));
}

// ──────────────────────────────────────────────────────────────────────────────
// Test 2: rule applied to an EXISTING window (the key regression case)
// The window must be repositioned immediately without a close/reopen cycle.
// ──────────────────────────────────────────────────────────────────────────────
static void testTiledExistingWindow() {
    NLog::log("{}aspectratio: tiled – rule applied to existing window (regression)", Colors::GREEN);

    // Spawn FIRST, then add the rule — window must reposition immediately.
    if (!spawnKitty())
        return;

    {
        auto win = getFromSocket("/activewindow");
        auto sz  = parseSize(win);
        NLog::log("{}Before rule: size {},{}", Colors::YELLOW, sz.w, sz.h);
        EXPECT_MAX_DELTA(sz.w, 1876, 2);
        EXPECT_MAX_DELTA(sz.h, 1036, 2);
    }

    // Add the rule AFTER the window already exists
    OK(getFromSocket("/keyword windowrule match:class kitty, aspect_ratio 1:1"));

    {
        auto win = getFromSocket("/activewindow");
        auto sz  = parseSize(win);
        auto p   = parsePos(win);
        NLog::log("{}After rule: at {},{} size {},{}", Colors::YELLOW, p.x, p.y, sz.w, sz.h);
        EXPECT_MAX_DELTA(sz.w, 1036, 2);
        EXPECT_MAX_DELTA(sz.h, 1036, 2);
        EXPECT(sz.w, sz.h);
        EXPECT_MAX_DELTA(p.x, 442, 2);
        EXPECT_MAX_DELTA(p.y, 22, 2);
    }

    Tests::killAllWindows();
    OK(getFromSocket("/reload"));
}

// ──────────────────────────────────────────────────────────────────────────────
// Test 3: 4:3 ratio – rendered dimensions must be in the right ratio
// ──────────────────────────────────────────────────────────────────────────────
static void testTiled4x3() {
    NLog::log("{}aspectratio: tiled – 4:3 ratio", Colors::GREEN);

    OK(getFromSocket("/keyword windowrule match:class kitty, aspect_ratio 4:3"));

    if (!spawnKitty())
        return;

    {
        auto win = getFromSocket("/activewindow");
        auto sz  = parseSize(win);
        auto p   = parsePos(win);
        NLog::log("{}4:3 tiled: at {},{} size {},{}", Colors::YELLOW, p.x, p.y, sz.w, sz.h);

        // Height equals the full slot height — width was constrained
        EXPECT_MAX_DELTA(sz.h, 1036, 2);

        // Width/height ratio close to 4/3
        const double ratio = static_cast<double>(sz.w) / sz.h;
        NLog::log("{}Ratio: {} (expected ~1.333)", Colors::YELLOW, ratio);
        EXPECT_MAX_DELTA(ratio, 4.0 / 3.0, 0.005);

        // Must be narrower than the full slot and centred (left margin > 22)
        EXPECT(sz.w < 1876, true);
        EXPECT(p.x > 22, true);
    }

    Tests::killAllWindows();
    OK(getFromSocket("/reload"));
}

// ──────────────────────────────────────────────────────────────────────────────
// Test 4: setprop (dispatcher) and unset → window returns to full slot
// ──────────────────────────────────────────────────────────────────────────────
static void testSetpropAndUnset() {
    NLog::log("{}aspectratio: setprop + unset via dispatcher", Colors::GREEN);

    if (!spawnKitty())
        return;

    // Apply 1:1 via setprop dispatcher (float notation)
    OK(getFromSocket("/dispatch setprop active aspect_ratio 1.0"));

    {
        auto win = getFromSocket("/activewindow");
        auto sz  = parseSize(win);
        NLog::log("{}After setprop 1.0: size {},{}", Colors::YELLOW, sz.w, sz.h);
        EXPECT_MAX_DELTA(sz.w, 1036, 2);
        EXPECT_MAX_DELTA(sz.h, 1036, 2);
    }

    // Ratio = 0 disables the constraint
    OK(getFromSocket("/dispatch setprop active aspect_ratio 0"));

    {
        auto win = getFromSocket("/activewindow");
        auto sz  = parseSize(win);
        NLog::log("{}After unset (0): size {},{}", Colors::YELLOW, sz.w, sz.h);
        EXPECT_MAX_DELTA(sz.w, 1876, 2);
        EXPECT_MAX_DELTA(sz.h, 1036, 2);
    }

    Tests::killAllWindows();
}

// ──────────────────────────────────────────────────────────────────────────────
// Test 5: floating window – constraint applies to rendered size,
//         logical (layout) size is preserved
// ──────────────────────────────────────────────────────────────────────────────
static void testFloating() {
    NLog::log("{}aspectratio: floating window", Colors::GREEN);

    if (!spawnKitty())
        return;

    OK(getFromSocket("/dispatch setfloating"));
    // Resize to a wide box: width > height, so for 1:1 width gets constrained
    OK(getFromSocket("/dispatch resizewindowpixel exact 800 450, activewindow"));

    {
        auto sz = parseSize(getFromSocket("/activewindow"));
        NLog::log("{}Float before rule: size {},{}", Colors::YELLOW, sz.w, sz.h);
        EXPECT_MAX_DELTA(sz.w, 800, 2);
        EXPECT_MAX_DELTA(sz.h, 450, 2);
    }

    // 1:1 → height (450) is the short side → constrain width to 450
    OK(getFromSocket("/dispatch setprop active aspect_ratio 1:1"));

    {
        auto win = getFromSocket("/activewindow");
        auto sz  = parseSize(win);
        NLog::log("{}Float after 1:1: size {},{}", Colors::YELLOW, sz.w, sz.h);
        EXPECT_MAX_DELTA(sz.w, 450, 2);
        EXPECT_MAX_DELTA(sz.h, 450, 2);
        EXPECT(sz.w, sz.h);
    }

    // Square box + 4:3 rule: arTarget > arCur → constrain height
    // arTarget=1.333, arCur=1.0 → constrainedSize.y = 800/1.333 = 600
    OK(getFromSocket("/dispatch resizewindowpixel exact 800 800, activewindow"));
    OK(getFromSocket("/dispatch setprop active aspect_ratio 4:3"));

    {
        auto win = getFromSocket("/activewindow");
        auto sz  = parseSize(win);
        NLog::log("{}Float 4:3 in 800x800: size {},{}", Colors::YELLOW, sz.w, sz.h);
        EXPECT_MAX_DELTA(sz.w, 800, 2);
        EXPECT_MAX_DELTA(sz.h, 600, 2);
        const double ratio = static_cast<double>(sz.w) / sz.h;
        EXPECT_MAX_DELTA(ratio, 4.0 / 3.0, 0.005);
    }

    // Unset → rendered size returns to full logical box
    OK(getFromSocket("/dispatch setprop active aspect_ratio 0"));

    {
        auto sz = parseSize(getFromSocket("/activewindow"));
        NLog::log("{}Float after unset: size {},{}", Colors::YELLOW, sz.w, sz.h);
        EXPECT_MAX_DELTA(sz.w, 800, 2);
        EXPECT_MAX_DELTA(sz.h, 800, 2);
    }

    Tests::killAllWindows();
}

// ──────────────────────────────────────────────────────────────────────────────
// Test 6: fullscreen – constraint must NOT apply
// ──────────────────────────────────────────────────────────────────────────────
static void testNoFullscreen() {
    NLog::log("{}aspectratio: fullscreen – constraint must be bypassed", Colors::GREEN);

    OK(getFromSocket("/keyword windowrule match:class kitty, aspect_ratio 1:1"));

    if (!spawnKitty())
        return;

    // Confirm the rule is active in tiled mode
    {
        auto sz = parseSize(getFromSocket("/activewindow"));
        NLog::log("{}Tiled with rule: size {},{}", Colors::YELLOW, sz.w, sz.h);
        EXPECT_MAX_DELTA(sz.w, 1036, 2);
        EXPECT_MAX_DELTA(sz.h, 1036, 2);
    }

    // Go fullscreen — constraint must not apply
    OK(getFromSocket("/dispatch fullscreen 0 set"));

    {
        auto win = getFromSocket("/activewindow");
        auto sz  = parseSize(win);
        NLog::log("{}Fullscreen with 1:1 rule: size {},{}", Colors::YELLOW, sz.w, sz.h);
        EXPECT_MAX_DELTA(sz.w, 1920, 2);
        EXPECT_MAX_DELTA(sz.h, 1080, 2);
    }

    // Exit fullscreen — constraint must be restored
    OK(getFromSocket("/dispatch fullscreenstate 0 0"));

    {
        auto sz = parseSize(getFromSocket("/activewindow"));
        NLog::log("{}Back to tiled: size {},{}", Colors::YELLOW, sz.w, sz.h);
        EXPECT_MAX_DELTA(sz.w, 1036, 2);
        EXPECT_MAX_DELTA(sz.h, 1036, 2);
    }

    Tests::killAllWindows();
    OK(getFromSocket("/reload"));
}

// ──────────────────────────────────────────────────────────────────────────────
// Test 7: getprop / setprop round-trip with W:H notation
// ──────────────────────────────────────────────────────────────────────────────
static void testGetSetPropRoundTrip() {
    NLog::log("{}aspectratio: getprop/setprop round-trip", Colors::GREEN);

    if (!spawnKitty())
        return;

    OK(getFromSocket("/dispatch setprop active aspect_ratio 4:3"));
    {
        auto prop = getFromSocket("/getprop active aspect_ratio");
        NLog::log("{}getprop after 4:3: {}", Colors::YELLOW, prop);
        EXPECT_CONTAINS(prop, "1.3");
    }

    OK(getFromSocket("/dispatch setprop active aspect_ratio 16:9"));
    {
        auto prop = getFromSocket("/getprop active aspect_ratio");
        NLog::log("{}getprop after 16:9: {}", Colors::YELLOW, prop);
        EXPECT_CONTAINS(prop, "1.7");
    }

    OK(getFromSocket("/dispatch setprop active aspect_ratio 0"));
    {
        auto prop = getFromSocket("/getprop active aspect_ratio");
        NLog::log("{}getprop after 0: {}", Colors::YELLOW, prop);
        EXPECT_CONTAINS(prop, "0");
    }

    Tests::killAllWindows();
}

// ──────────────────────────────────────────────────────────────────────────────
// Entry point
// ──────────────────────────────────────────────────────────────────────────────
static bool test() {
    NLog::log("{}Testing aspect_ratio window rule", Colors::GREEN);

    // Switch to a dedicated workspace; do NOT reload first (that would reset it).
    // Ensure the headless monitor has the right resolution (headless may default to 0x0).
    // Use "highres" to pick the best available mode rather than requesting a specific refresh.
    // The headless output may have 0x0 resolution. Try to create a properly-sized one.
    getFromSocket("/output remove HEADLESS-2");
    getFromSocket("/output create headless");
    getFromSocket("/keyword monitor HEADLESS-2,1920x1080,0x0,1");
    {
        auto mons = getFromSocket("/monitors all");
        auto h2 = mons.find("HEADLESS-2");
        if (h2 != std::string::npos) {
            auto end = mons.find("\n\n", h2);
            NLog::log("{}Monitor: {}", Colors::CYAN, mons.substr(h2, end != std::string::npos ? end - h2 : 200));
        }
    }

    OK(getFromSocket("/dispatch workspace 15"));

    testTiledNewWindow();
    testTiledExistingWindow();
    testTiled4x3();
    testSetpropAndUnset();
    testFloating();
    testNoFullscreen();
    testGetSetPropRoundTrip();

    NLog::log("{}Cleaning up aspect_ratio tests", Colors::YELLOW);
    Tests::killAllWindows();
    OK(getFromSocket("/dispatch workspace 1"));
    OK(getFromSocket("/reload"));

    return !ret;
}

REGISTER_TEST_FN(test)
