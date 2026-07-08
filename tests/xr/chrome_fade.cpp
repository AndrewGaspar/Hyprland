#include <openxr/XRMath.hpp>

#include <gtest/gtest.h>

#include <cmath>

using namespace OpenXR;

// tests/xr/chrome_fade.cpp — WP-G2 chrome auto-hide fade envelope (pure math) +
// SXRChromeGeometry::hasChrome. Compiles and runs with no OpenXR runtime present.

namespace {
    constexpr float FADE = 0.15f; // openxr:chrome_fade_ms default 150ms
    constexpr float HIDE = 1.5f;  // openxr:chrome_hide_delay_ms default 1500ms

    // Run the envelope for `n` frames of `dt` seconds, holding `active`/`sinceActive` fixed.
    float run(float start, bool active, float sinceActive, int n, float dt) {
        float a = start;
        for (int i = 0; i < n; ++i)
            a = chromeFadeAdvance(a, active, dt, sinceActive, FADE, HIDE);
        return a;
    }
}

TEST(ChromeFade, FadesInWhileActive) {
    // One 60Hz-ish frame moves alpha by dt/fade = 0.05/0.15 ≈ 0.333.
    EXPECT_NEAR(chromeFadeAdvance(0.f, true, 0.05f, 0.f, FADE, HIDE), 1.f / 3.f, 1e-5f);
    // Reaches full opacity within ~fade seconds and never overshoots.
    EXPECT_FLOAT_EQ(run(0.f, true, 0.f, 10, 0.05f), 1.f);
}

TEST(ChromeFade, HoldsVisibleDuringHideDelay) {
    // Not active, but still within the hide-delay grace -> target stays 1, alpha holds.
    EXPECT_FLOAT_EQ(run(1.f, false, HIDE * 0.5f, 20, 0.05f), 1.f);
}

TEST(ChromeFade, FadesOutAfterHideDelay) {
    // Past the hide delay and not active -> ramps to 0. Args: (cur, active, dtSec, sinceActiveSec, ...).
    EXPECT_NEAR(chromeFadeAdvance(1.f, false, 0.05f, HIDE + 0.01f, FADE, HIDE), 1.f - 1.f / 3.f, 1e-5f);
    EXPECT_FLOAT_EQ(run(1.f, false, HIDE + 1.f, 10, 0.05f), 0.f);
}

TEST(ChromeFade, ActiveBeatsHideDelay) {
    // activeNow forces target 1 even if sinceActive is huge (caller resets sinceActive to 0 when
    // active; this guards the precedence regardless).
    EXPECT_GT(chromeFadeAdvance(0.f, true, 0.05f, 999.f, FADE, HIDE), 0.f);
}

TEST(ChromeFade, ClampsToUnitRange) {
    EXPECT_LE(run(0.9f, true, 0.f, 5, 0.05f), 1.f);
    EXPECT_GE(run(0.1f, false, HIDE + 1.f, 5, 0.05f), 0.f);
}

TEST(ChromeFade, ZeroFadeSnaps) {
    EXPECT_FLOAT_EQ(chromeFadeAdvance(0.f, true, 0.05f, 0.f, 0.f, HIDE), 1.f);
    EXPECT_FLOAT_EQ(chromeFadeAdvance(1.f, false, 0.05f, HIDE + 1.f, 0.f, HIDE), 0.f);
}

TEST(ChromeFade, ZeroDtHolds) {
    // No time elapsed -> alpha unchanged (not snapped to target).
    EXPECT_FLOAT_EQ(chromeFadeAdvance(0.4f, true, 0.f, 0.f, FADE, HIDE), 0.4f);
    EXPECT_FLOAT_EQ(chromeFadeAdvance(0.4f, false, 0.f, HIDE + 1.f, FADE, HIDE), 0.4f);
}

TEST(ChromeGeometry, HasChromeReflectsElements) {
    // Chrome disabled (margin 0 + bar 0): full-quad content rect, no drawable elements.
    SXRChromeGeometry off = makeChromeGeometry(1.6f, 0.9f, 0.f, 0.f, 0.6f, 0.06f);
    EXPECT_FALSE(off.hasChrome());
    EXPECT_TRUE(off.hasContentRect());

    // A margin with corner handles -> chrome present.
    SXRChromeGeometry corners = makeChromeGeometry(1.6f, 0.9f, 0.08f, 0.f, 0.f, 0.06f);
    EXPECT_TRUE(corners.hasChrome());

    // A bar alone (no margin, no corners) -> chrome present.
    SXRChromeGeometry bar = makeChromeGeometry(1.6f, 0.9f, 0.f, 0.05f, 0.6f, 0.f);
    EXPECT_TRUE(bar.hasChrome());
}
