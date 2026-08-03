#pragma once

// XRRule — situational per-monitor transparency: the `xrrule` keyword's rule model, parser,
// matcher and effect resolver (docs/openxr/research/archive/09-monitor-transparency.md).
//
// Compiled UNCONDITIONALLY (no OpenXR headers, no HAVE_OPENXR guard), exactly like its pure
// siblings XRMath.hpp / XRAnchor.hpp / XRMonitorConfig.hpp, so the config layer can live outside
// the gate and hyprland_gtests can always exercise it (tests/xr/xrrule.cpp).
//
// THE MODEL (settled design):
//   * the MONITOR is the unit of EFFECT — windows are only a source of CONDITIONS;
//   * per pixel, final_alpha = uniform_alpha * lumakey_alpha(pixel) (report 09 §2.1: both are
//     premultiplied, so rgb is scaled by the same factor — see xrComposeEffectAlpha below);
//   * three precedence layers, each resolved PER EFFECT: defaults -> rules (config order, later
//     match wins) -> manual override (sticky until cleared to auto), mirroring the shipped
//     hand_input "manual over auto" pattern.
//
// Everything here is pure: no threads, no clocks, no compositor types. The manager evaluates it on
// the MAIN thread and publishes the resolved numbers to the frame thread as plain atomics
// (XRMonitorLayer.hpp threading rule).

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <expected>
#include <vector>

#include "XRMath.hpp" // easeApply / envAdvance (the shared transition envelope) + the luma-key math

namespace re2 {
    class RE2;
}

namespace OpenXR {

    // ---- the per-monitor situation a rule is matched against ----

    // How a monitor is currently anchored, collapsed to the three states a user reasons about.
    // Resolved on the main thread from the layer's live anchor + grab + adaptive phase; see
    // COpenXRManager::layerAnchorState for the exact mapping (and doc 05 §xrrule for the contract).
    enum eXRAnchorState : uint8_t {
        XR_ANCHORSTATE_DOCKED = 0, // world-fixed where you left it (anchor:local, adaptive docked)
        XR_ANCHORSTATE_FOLLOW,     // leashed to you (anchor:head/body/device, or adaptive roaming)
        XR_ANCHORSTATE_CARRIED,    // being moved right now (hand grab, resize grab or gaze carry)
    };

    const char*                     xrAnchorStateName(eXRAnchorState s);
    std::optional<eXRAnchorState>   xrParseAnchorState(const std::string& s);

    // One monitor's context tuple. Every monitor is evaluated with its OWN tuple — that is what
    // makes `monitor:` a filter rather than a selector.
    struct SXRRuleContext {
        std::string    monitorName;
        eXRAnchorState anchorState = XR_ANCHORSTATE_DOCKED;
        // The monitor's "focused window" (doc 05 §xrrule): the fullscreen window on the monitor's
        // active workspace if there is one, else that workspace's last-focused window. hasFocus is
        // false when the monitor shows no window at all — a focusclass:/focustitle: condition can
        // then never match.
        bool           hasFocus   = false;
        std::string    focusClass;
        std::string    focusTitle;
        bool           fullscreen = false; // that window is fullscreen (false when hasFocus is false)
    };

    // ---- effects ----

    // The effect set a rule (or a manual override, or a resolution result) can carry. Every field
    // is optional so precedence is resolved PER EFFECT: a later rule that only sets `blackalpha`
    // leaves an earlier rule's `alpha` standing.
    struct SXREffects {
        std::optional<float> alpha;      // uniform monitor alpha, 0..1 (1 = fully opaque)
        std::optional<float> blackAlpha; // luma key: alpha given to pure black, 0..1 (1 = keying off)
        std::optional<float> blackKnee;  // luma key: luma at which content is fully opaque
        bool                 empty() const {
            return !alpha && !blackAlpha && !blackKnee;
        }
    };

    // Where a resolved value came from — surfaced verbatim by `hyprctl openxr status` so "why is
    // this monitor ghosted" is answerable in one command.
    enum eXREffectSource : uint8_t {
        XR_EFFSRC_DEFAULT = 0, // openxr:black_alpha / the 1.0 opaque default
        XR_EFFSRC_RULE,        // an `xrrule` matched
        XR_EFFSRC_MANUAL,      // `hyprctl openxr alpha|blackalpha <name> <v>` (sticky until `auto`)
    };
    const char* xrEffectSourceName(eXREffectSource s);

    // A fully resolved per-monitor effect set: concrete values plus their provenance.
    struct SXRResolvedEffects {
        float           alpha         = 1.F;
        float           blackAlpha    = 1.F;
        float           blackKnee     = 0.1F;
        eXREffectSource alphaSrc      = XR_EFFSRC_DEFAULT;
        eXREffectSource blackAlphaSrc = XR_EFFSRC_DEFAULT;
        eXREffectSource blackKneeSrc  = XR_EFFSRC_DEFAULT;
    };

    // ---- one rule ----

    // Compiled conditions. Regexes are RE2 (the engine Hyprland's own windowrules use) held by
    // shared_ptr so rules stay copyable — they are copied from the config manager's declared list
    // into the XR manager's snapshot. Main thread only (RE2 matching itself is thread-safe, but
    // the rule vector's lifetime is not).
    struct SXRRuleConds {
        std::shared_ptr<const re2::RE2> monitorRe;
        std::shared_ptr<const re2::RE2> focusClassRe;
        std::shared_ptr<const re2::RE2> focusTitleRe;
        std::optional<eXRAnchorState>   anchorState;
        std::optional<bool>             fullscreen;

        bool                            empty() const {
            return !monitorRe && !focusClassRe && !focusTitleRe && !anchorState && !fullscreen;
        }
    };

    struct SXRRule {
        SXREffects   effects;
        SXRRuleConds conds;
        std::string  raw; // the source line, for `hyprctl openxr rules` / logging
    };

    // Parse one `xrrule = <effects>, <conditions>` line (the value after the `=`).
    //
    //   effects    space-separated `name value` pairs: alpha <0..1>, blackalpha <0..1|off>,
    //              blackalpha_knee <v>. At least one is required.
    //   conditions space-separated `key:value` pairs, ALL of which must match: monitor:<regex>,
    //              anchorstate:<docked|follow|carried>, focusclass:<regex>, focustitle:<regex>,
    //              fullscreen:<0|1>. Omitted = wildcard; the whole list may be omitted.
    //
    // Values containing spaces may be double-quoted (`focustitle:"My Window"`). Malformed input
    // returns a human-readable error the config parser reports as a config error.
    std::expected<SXRRule, std::string> parseXRRuleLine(const std::string& args);

    // Does this rule's condition set match this monitor's context? (Empty condition set = always.)
    bool                                xrRuleMatches(const SXRRule& rule, const SXRRuleContext& ctx);

    // The full precedence fold: defaults -> rules in config order (later match overrides per
    // effect) -> manual override. `defaults` supplies the baseline values (alpha is 1.0; blackAlpha
    // / blackKnee come from openxr:black_alpha / :black_alpha_knee, which is what keeps every
    // pre-xrrule config behaving EXACTLY as before).
    SXRResolvedEffects xrResolveEffects(const SXREffects& defaults, const std::vector<SXRRule>& rules, const SXRRuleContext& ctx, const SXREffects& manual);

    // ---- effect composition ----

    // The per-pixel alpha the blit must produce: the luma key's alpha for this pixel, scaled by the
    // monitor's uniform alpha. Both live in the same premultiplied budget, so the shader writes
    // rgb * a (report 09 §2.1: scaling alpha alone leaves rgb > a and the runtime's
    // src=ONE,dst=ONE_MINUS_SRC_ALPHA blend adds the content at full brightness — the additive-halo
    // bug). This is the host-side reference for the composition the GL pipeline performs as
    // (blit luma key) x (uniform fade pass).
    inline float xrComposeEffectAlpha(float luma, float uniformAlpha, float blackAlpha, float knee) {
        return std::clamp(uniformAlpha, 0.F, 1.F) * xrLumaKeyAlphaFromLuma(luma, blackAlpha, knee);
    }

    // Is the uniform fade doing anything at all? (Cheap guard: 1.0 = no fade pass at all.)
    inline bool xrUniformAlphaActive(float alpha) {
        return alpha < 1.F;
    }

    // ---- transition envelope ----

    // Main-thread tick period for the effect envelopes. Finer than any headset frame interval
    // (8ms < 13.9ms at 72Hz), so the eased value the frame loop samples never steps visibly; the
    // timer is disarmed entirely whenever nothing is in flight.
    inline constexpr int XR_FX_TICK_MS = 8;

    // One eased scalar. Every effect change rides one of these so nothing ever pops: on retarget the
    // envelope restarts from the CURRENT value (so an interrupted transition never jumps back), and
    // the parameter stays LINEAR with the ease applied at sample time — the exact shape adaptive
    // anchoring's transition envelope uses (XRMath.hpp envAdvance/easeApply, §4.3).
    struct SXRFxEnv {
        float from = 1.F;
        float to   = 1.F;
        float t    = 1.F; // linear parameter, 1 = settled

        float value(eXREase ease = XR_EASE_SMOOTHSTEP) const {
            return from + (to - from) * easeApply(ease, t);
        }
        bool settled() const {
            return t >= 1.F;
        }
        // Retarget. No-op when the target is unchanged, so a re-evaluation that resolves the same
        // numbers (the common case — most events change nothing) never restarts an envelope.
        void retarget(float target, eXREase ease = XR_EASE_SMOOTHSTEP) {
            if (target == to)
                return;
            from = value(ease);
            to   = target;
            t    = 0.F;
        }
        // Advance one tick. durationSec <= 0 snaps.
        void advance(float dtSec, float durationSec) {
            if (t < 1.F)
                t = envAdvance(t, dtSec, durationSec);
        }
        // Jump straight to a value with no transition (session start / monitor creation).
        void set(float v) {
            from = to = v;
            t         = 1.F;
        }
    };
}
