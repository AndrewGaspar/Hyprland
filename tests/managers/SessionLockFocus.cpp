#include <managers/SessionLockManager.hpp>

#include <gtest/gtest.h>

#include <optional>
#include <vector>

// Which lock surface should hold keyboard focus.
//
// The bug this covers, observed live 2026-09-07: the session was locked with the headless FALLBACK
// as the only output, the headset was re-donned, four XR outputs plugged and the FALLBACK left. The
// lock screen rendered on the XR quads but ate every keystroke, because nothing ever handed
// keyboard focus to a surviving lock surface:
//
//   * the destroy handler's focus clear was `focusState()->surface().reset()`, which resets a
//     TEMPORARY (surface() returns by value) — so removeSessionLockSurface()'s "focus is already
//     gone" gate saw the dying surface as a live focus holder and skipped the handoff;
//   * nothing assigned focus when a lock surface mapped — the only path was the pointer pass, which
//     picks by cursor position and clears focus outright when the cursor's monitor has no mapped
//     lock surface (the normal state while a batch of outputs is still coming up).
//
// The compositor-side plumbing (seat, focus state, protocol objects) is not reachable from a gtest,
// so what is pinned here is the POLICY: given the lock surfaces that exist and the monitor the
// compositor considers focused, which one takes the keyboard. Everything above it is the two call
// sites that feed it — SessionLockManager::refocusKeyboard() building the candidate list, and the
// map/destroy/surface-death events that call it.

using namespace SessionLock;

namespace {
    SFocusCandidate live(uint64_t monitorID) {
        return SFocusCandidate{.monitorID = monitorID, .mapped = true, .focusable = true};
    }

    SFocusCandidate unmapped(uint64_t monitorID) {
        return SFocusCandidate{.monitorID = monitorID, .mapped = false, .focusable = true};
    }

    SFocusCandidate dead(uint64_t monitorID) {
        return SFocusCandidate{.monitorID = monitorID, .mapped = true, .focusable = false};
    }
}

TEST(SessionLockFocus, NoCandidatesPicksNothing) {
    EXPECT_FALSE(pickFocusCandidate({}, std::nullopt).has_value());
    EXPECT_FALSE(pickFocusCandidate({}, 3).has_value());
}

TEST(SessionLockFocus, PrefersTheFocusedMonitor) {
    // the FALLBACK→XR case: four fresh lock surfaces, focus should land on the monitor the
    // compositor is focused on, not on whichever surface happens to be first in creation order.
    const std::vector<SFocusCandidate> CANDIDATES = {live(7), live(8), live(9), live(10)};

    EXPECT_EQ(pickFocusCandidate(CANDIDATES, 9), 2u);
    EXPECT_EQ(pickFocusCandidate(CANDIDATES, 7), 0u);
    EXPECT_EQ(pickFocusCandidate(CANDIDATES, 10), 3u);
}

TEST(SessionLockFocus, FallsBackToTheFirstUsableWhenTheFocusedMonitorHasNone) {
    // the monitor the compositor is focused on is the one that just went away, so its lock surface
    // is no longer in the list at all. Any live surface beats no focus.
    const std::vector<SFocusCandidate> CANDIDATES = {live(8), live(9)};

    EXPECT_EQ(pickFocusCandidate(CANDIDATES, 7), 0u);
    EXPECT_EQ(pickFocusCandidate(CANDIDATES, std::nullopt), 0u);
}

TEST(SessionLockFocus, SkipsUnmappedSurfaces) {
    // the plug storm: surfaces are created in one batch and map one by one. An unmapped surface has
    // no frame and must not take the keyboard, but it must not block a mapped sibling either.
    EXPECT_EQ(pickFocusCandidate({unmapped(7), live(8)}, std::nullopt), 1u);
    EXPECT_EQ(pickFocusCandidate({unmapped(7), live(8)}, 7), 1u);
    EXPECT_FALSE(pickFocusCandidate({unmapped(7), unmapped(8)}, std::nullopt).has_value());
}

TEST(SessionLockFocus, SkipsSurfacesWhoseWlSurfaceIsGone) {
    // a lock surface whose wl_surface died under a live role object (SessionLock.cpp's
    // surfaceDestroy path) is still in the list but cannot be focused.
    EXPECT_EQ(pickFocusCandidate({dead(7), live(8)}, std::nullopt), 1u);
    EXPECT_EQ(pickFocusCandidate({dead(7), live(8)}, 7), 1u);
    EXPECT_FALSE(pickFocusCandidate({dead(7), dead(8)}, 8).has_value());
}

TEST(SessionLockFocus, PreferredMonitorMustAlsoBeUsable) {
    // the focused monitor's surface exists but is not up yet — take a usable one rather than
    // nothing, since "nothing" is exactly the state that eats keystrokes for the rest of the lock.
    EXPECT_EQ(pickFocusCandidate({live(7), unmapped(9), live(8)}, 9), 0u);
    EXPECT_EQ(pickFocusCandidate({live(7), dead(9), live(8)}, 9), 0u);
}

TEST(SessionLockFocus, LastSurfaceDyingLeavesNothingToPick) {
    // FALLBACK's lock surface dies while it is the only one (the 10:17:24 unplug): there is
    // genuinely nothing to hand focus to, and the map handler has to be the one that re-arms it.
    EXPECT_FALSE(pickFocusCandidate({}, 5).has_value());
}
