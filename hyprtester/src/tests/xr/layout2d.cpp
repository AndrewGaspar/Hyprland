#include "tests.hpp"

#ifdef WITH_XR_TESTS

#include "../../hyprctlCompat.hpp"
#include "../../Log.hpp"
#include "../../xr/xr_helpers.hpp"

#include <algorithm>
#include <chrono>
#include <functional>
#include <limits>
#include <string>
#include <thread>
#include <vector>

// xr/layout2d.cpp — 2D-plane sync end to end (report 12 WP-S2). The pure projection is exhaustively
// covered by tests/xr/layout2d.cpp (gtests); what CANNOT be tested there is the wiring: does a quad
// that lives to the left in 3D actually end up to the left in `hyprctl monitors`, and does moving it
// in 3D move its box? That needs a live session, so it lives here.

namespace {
    bool gateUp() {
        if (XR::waitForXrState("focused", std::chrono::milliseconds(15000)))
            return true;
        return XR::waitForXrState("visible", std::chrono::milliseconds(2000));
    }

    struct SGuard {
        const bool&              failed;
        std::string              testName;
        std::vector<std::string> monitorNames;
        ~SGuard() {
            // Always thaw: a test that fails mid-way while frozen would otherwise leave the sync
            // engine paused for every test after it.
            getFromSocket("/openxr sync-layout thaw");
            for (auto& n : monitorNames)
                if (!n.empty())
                    getFromSocket("/openxr destroy " + n);
            if (failed)
                XR::dumpXrArtifacts(testName);
        }
    };

    // `sync-layout` is a VERB, not a state, and it is allowed to decline: it refuses outright ("no
    // head tracking yet — cannot latch a reference frame") until the frame loop has published a
    // view pose, and syncLayout2D() defers silently — re-arming its own debounce — whenever any
    // monitor is being carried. Both are transient, and both leave the 2D layout exactly as it was.
    // That is what a plain poll on `hyprctl monitors` cannot recover from: the layout is only ever
    // recomputed by an EVENT, so a sync that was declined at the wrong instant is not something
    // waiting will fix, and the test then burns its whole budget staring at a stale layout. Run
    // alone this race is won every time; behind thirty-odd XR tests (sessions disabled and
    // re-enabled, monitors carried, grabs released) it is the difference between "passes" and
    // "passes on rerun". So: keep asking until the verb takes.
    bool syncNow(int budgetMs = 5000) {
        const auto  deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budgetMs);
        std::string reply;
        int         tries = 0;
        while (true) {
            reply = getFromSocket("/openxr sync-layout");
            ++tries;
            if (reply == "ok") {
                if (tries > 1)
                    NLog::yellow("sync-layout was declined {} time(s) before it took", tries - 1);
                return true;
            }
            if (std::chrono::steady_clock::now() >= deadline)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        NLog::red("sync-layout never succeeded within {}ms (last reply: '{}')", budgetMs, reply);
        return false;
    }

    // The layout x of a monitor in `hyprctl -j monitors`, or NaN if it isn't there.
    float monitorX(const std::string& json, const std::string& name) {
        const size_t at = XR::findAfter(json, "\"name\": \"" + name + "\"");
        if (at == std::string::npos)
            return std::numeric_limits<float>::quiet_NaN();
        return XR::toFloatOr(XR::fieldAfter(json, at, "x"), std::numeric_limits<float>::quiet_NaN());
    }

    // "The projection has LANDED", which an ordering predicate on its own cannot establish, for two
    // reasons. First, syncLayout2D writes each monitor's rule offset and leaves the ordinary
    // arrange() pipeline to apply it on a scheduled recheck, so `hyprctl monitors` trails the engine
    // and can be read mid-move. Second — and this is what actually bit xr_layout2d_freeze_thaw —
    // "A is left of B" is equally true of the plain append-right FALLBACK whenever the monitors were
    // created in that order, so the wait can be satisfied before the engine has placed anything at
    // all (follows_3d dodges that by creating its two in the wrong order; freeze_thaw cannot, A
    // starting left IS its setup). Sampling a reference x off that unsettled layout, then freezing,
    // let the real projection land INSIDE the frozen window, and the test blamed the freeze for a
    // move it had ordered itself.
    // So landed = the engine says it placed every named monitor (layout2d.source "auto" — the half
    // an ordering predicate cannot see) AND the positions the compositor reports have stopped
    // moving. Quiescence rather than "monitors.x == layout2d.x" deliberately: how faithfully the
    // applied position tracks the engine's offset is the product's business, not this test's, and
    // the two ARE observed to disagree here — which is worth saying out loud, so it is noted rather
    // than asserted.
    constexpr int QUIET_TICKS = 6; // x100ms

    // True once every named monitor is placed by the engine; fills `xs` with the reported positions.
    bool placedAndPositions(const std::vector<std::string>& names, const std::string& mons, const std::string& status, std::vector<float>& xs) {
        bool placed = true;
        xs.clear();
        for (const auto& n : names) {
            const auto at = XR::findAfter(status, "\"name\": \"" + n + "\"");
            if (at == std::string::npos || XR::fieldAfter(status, at, "source") != "auto")
                placed = false;
            xs.push_back(monitorX(mons, n));
        }
        return placed;
    }

    // The engine's own idea of where it put a monitor, or NaN.
    float engineX(const std::string& status, const std::string& name) {
        const auto at = XR::findAfter(status, "\"name\": \"" + name + "\"");
        if (at == std::string::npos)
            return std::numeric_limits<float>::quiet_NaN();
        return XR::toFloatOr(XR::fieldAfter(status, at, "x"), std::numeric_limits<float>::quiet_NaN());
    }

    // On a timeout, say WHICH half of "landed" is missing, per monitor: "the engine never placed
    // it" and "the engine placed it somewhere the compositor has not applied" are different bugs.
    void reportLayout(const std::vector<std::string>& names) {
        const std::string mons = getFromSocket("j/monitors"), st = getFromSocket("j/openxr");
        for (const auto& n : names) {
            const auto at = XR::findAfter(st, "\"name\": \"" + n + "\"");
            if (at == std::string::npos) {
                NLog::red("  {}: not in `openxr status` at all", n);
                continue;
            }
            NLog::red("  {}: layout2d.source={} layout2d.x={} col={} row={} azDeg={} | monitors.x={}", n, XR::fieldAfter(st, at, "source"), XR::fieldAfter(st, at, "x"),
                      XR::fieldAfter(st, at, "col"), XR::fieldAfter(st, at, "row"), XR::fieldAfter(st, at, "azDeg"), monitorX(mons, n));
        }
    }

    // Wait for the projection to land on `names` and for `pred` to hold of the monitor list it
    // produced. `nudge` re-drives the sync engine once a second for the same reason syncNow exists:
    // one deferred pass must not be able to strand the whole assertion. Pass false where the
    // AUTOMATIC path is the thing under test.
    bool awaitLayout(const std::vector<std::string>& names, const std::function<bool(const std::string&)>& pred, int budgetMs = 10000, bool nudge = true) {
        const auto         deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budgetMs);
        std::vector<float> last, now;
        int                quiet = 0, i = 0;
        while (true) {
            const std::string mons = getFromSocket("j/monitors"), st = getFromSocket("j/openxr");
            const bool        placed = placedAndPositions(names, mons, st, now);
            // NaN never compares equal, so a monitor that is missing can never look quiet.
            quiet = (now == last) ? quiet + 1 : 0;
            last  = now;
            if (placed && quiet >= QUIET_TICKS && pred(mons)) {
                for (size_t k = 0; k < names.size(); ++k) {
                    const float want = engineX(st, names[k]);
                    if (want != now[k])
                        NLog::yellow("note: {} sits at x {}, but the 2D engine's own offset for it is {} — the applied layout does not match layout2d.x", names[k], now[k], want);
                }
                return true;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                NLog::red("2D layout never landed within {}ms (engine placed all: {}, quiet ticks: {}):", budgetMs, placed, quiet);
                reportLayout(names);
                return false;
            }
            if (nudge && ++i % 10 == 0) { // once a second: re-drive the edge-triggered sync engine
                NLog::yellow("2D layout has not landed yet — re-driving sync-layout");
                getFromSocket("/openxr sync-layout");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

}

// xr_layout2d_follows_3d — the headline contract: a monitor floating to your LEFT gets a smaller
// layout x than one floating to your right, and MOVING it in 3D moves its 2D box to match. Without
// the feature both monitors land append-right in CREATION order, so the second assertion (the order
// FLIPS after the move) is what makes this a real regression test rather than a coincidence.
TEST_CASE(xr_layout2d_follows_3d) {
    XR_SKIP_IF_UNAVAILABLE();
    SGuard guard{this->failed, name(), {}};

    if (!gateUp()) {
        XR::logSkip(name(), "session never reached focused/visible (known env instability)");
        return;
    }

    // 2D-plane sync must be ON for this to mean anything; if the shared suite config turned it off,
    // skip rather than fail.
    if (getFromSocket("/openxr status").contains("2d-plane sync: off")) {
        XR::logSkip(name(), "openxr:layout2d:enabled is off");
        return;
    }

    // Create them in an order that DISAGREES with their spatial order, so append-right and
    // spatial-order cannot both be satisfied by accident: `right` is created first.
    const std::string R = XR::monitorName(70);
    const std::string L = XR::monitorName(71);

    ASSERT(getFromSocket("/openxr create " + R + " 1280x720 anchor:local pos:1.2,1.4,-1.5 yaw:0"), std::string("ok"));
    guard.monitorNames.push_back(R);
    ASSERT(getFromSocket("/openxr create " + L + " 1280x720 anchor:local pos:-1.2,1.4,-1.5 yaw:0"), std::string("ok"));
    guard.monitorNames.push_back(L);

    // Force a sync (also re-latches the reference frame) rather than waiting on the debounce.
    ASSERT(syncNow(), true);

    ASSERT(awaitLayout({L, R},
                       [&](const std::string& r) {
                           const float xl = monitorX(r, L), xr = monitorX(r, R);
                           return xl == xl && xr == xr && xl < xr; // both present (not NaN) and left-of-right
                       }),
           true);

    // Now teleport the LEFT monitor to the far right of the other one and re-sync. Its 2D box must
    // follow — this is the whole feature, and it is the assertion append-right cannot pass. The
    // target keeps roughly the same horizontal radius (2.18 m vs 1.92 m) as the monitor it has to
    // overtake, so the two stay in the SAME elevation tier whatever height the runtime puts the eye
    // at, and the assertion is purely about azimuth ordering.
    ASSERT(getFromSocket("/openxr place " + L + " at 2.1,1.4,-0.6"), std::string("ok"));
    ASSERT(syncNow(), true);

    ASSERT(awaitLayout({L, R},
                       [&](const std::string& r) {
                           const float xl = monitorX(r, L), xr = monitorX(r, R);
                           return xl == xl && xr == xr && xl > xr;
                       }),
           true);
}

// xr_layout2d_freeze_thaw — `sync-layout freeze` must actually stop the automatic recompute (so a
// user rearranging quads doesn't get their mouse mapping yanked mid-session), and `thaw` must catch
// the layout back up. An explicit `sync-layout` still forces a pass while frozen.
TEST_CASE(xr_layout2d_freeze_thaw) {
    XR_SKIP_IF_UNAVAILABLE();
    SGuard guard{this->failed, name(), {}};

    if (!gateUp()) {
        XR::logSkip(name(), "session never reached focused/visible (known env instability)");
        return;
    }
    if (getFromSocket("/openxr status").contains("2d-plane sync: off")) {
        XR::logSkip(name(), "openxr:layout2d:enabled is off");
        return;
    }

    // TWO monitors, so "did the layout move" is a real question: with only one the block is a single
    // box at the seam and its x is the same wherever the quad floats.
    const std::string A = XR::monitorName(72); // starts LEFT
    const std::string B = XR::monitorName(73); // stays RIGHT
    ASSERT(getFromSocket("/openxr create " + A + " 1280x720 anchor:local pos:-1.2,1.4,-1.5 yaw:0"), std::string("ok"));
    guard.monitorNames.push_back(A);
    ASSERT(getFromSocket("/openxr create " + B + " 1280x720 anchor:local pos:1.2,1.4,-1.5 yaw:0"), std::string("ok"));
    guard.monitorNames.push_back(B);
    // Setup only, so the same "keep asking / keep nudging" treatment as follows_3d applies. The
    // frozen and thawed assertions below deliberately do NOT nudge: one asserts that nothing moves,
    // the other that the AUTOMATIC path catches up on its own.
    ASSERT(syncNow(), true);

    ASSERT(awaitLayout({A, B},
                       [&](const std::string& r) {
                           const float xa = monitorX(r, A), xb = monitorX(r, B);
                           return xa == xa && xb == xb && xa < xb;
                       }),
           true);
    // Only now — with the projection demonstrably landed — is there a number worth freezing against.
    const float before = monitorX(getFromSocket("j/monitors"), A);

    ASSERT(getFromSocket("/openxr sync-layout freeze"), std::string("ok"));
    ASSERT(getFromSocket("/openxr status").contains("frozen"), true);

    // Move A past B. The AUTOMATIC recompute (the `place` verb's trigger) must NOT fire while frozen.
    ASSERT(getFromSocket("/openxr place " + A + " at 2.1,1.4,-0.6"), std::string("ok"));
    std::this_thread::sleep_for(std::chrono::milliseconds(1200)); // well past openxr:layout2d:debounce_ms
    const float whileFrozen = monitorX(getFromSocket("j/monitors"), A);
    if (whileFrozen != before)
        MARK_TEST_FAILED("the layout moved while frozen: {} moved from x {} to x {}", A, before, whileFrozen);

    // Thawing re-arms it and the layout catches up on the debounce — A is now right of B. No nudging
    // here: that the AUTOMATIC path catches up is the behaviour under test.
    ASSERT(getFromSocket("/openxr sync-layout thaw"), std::string("ok"));
    ASSERT(awaitLayout(
               {A, B},
               [&](const std::string& r) {
                   const float xa = monitorX(r, A), xb = monitorX(r, B);
                   return xa == xa && xb == xb && xa > xb;
               },
               10000, /*nudge=*/false),
           true);
}

#endif // WITH_XR_TESTS
