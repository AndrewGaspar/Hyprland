#pragma once

// Per-HMD XR monitor defaults (`hl.xr_defaults`), named shape profiles, the `when` matcher shared
// with `hl.xr_monitor`, and the `XR-<n>` name minting used by a name-less `create` (doc 05 §3.3).
//
// Deliberately compiles unconditionally (no OpenXR headers, no HAVE_OPENXR guard), like
// XRMonitorConfig.hpp: everything here is a pure function of plain data so hyprland_gtests can
// exercise it without a runtime (tests/xr/xr_defaults.cpp). The manager owns the impure halves —
// building the HMD context from the live session, and applying a resolution to a live monitor.

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../helpers/math/Math.hpp"

namespace re2 {
    class RE2;
}

namespace OpenXR {
    // ---- the HMD context a `when` is evaluated against ----

    // A DRM connector currently leased out through wp_drm_lease_v1 — in practice, the direct-mode
    // HMD panel a Monado service holds (XREAL). Both description spellings are kept, exactly as a
    // `desc:` monitor rule matches against both (CMonitor::matchesStaticSelector).
    struct SXRLeasedDisplay {
        std::string description;
        std::string shortDescription;

        bool        operator==(const SXRLeasedDisplay&) const = default;
    };

    // What is known about the headset the XR session bound. Built on the main thread at session
    // bring-up (COpenXRManager::refreshXRSystemContext) and kept after the session ends, so a
    // shelved headset does not reshape the desktop; it changes only when a session binds a
    // DIFFERENT system. `bound == false` means no session has bound a system yet since the
    // compositor started, and then only `when`-less blocks/declarations match.
    struct SXRSystemContext {
        bool                          bound = false;
        std::string                   systemName; // XrSystemProperties::systemName
        std::vector<SXRLeasedDisplay> leasedDisplays;
        bool                          eyeGaze      = false; // runtime advertises XR_EXT_eye_gaze_interaction
        bool                          handTracking = false; // XR_EXT_hand_tracking enabled AND the system supports it
        bool                          passthrough  = false; // the runtime offers the ALPHA blend mode

        bool                          operator==(const SXRSystemContext&) const = default;
    };

    // One-line human form, for the log and `hyprctl openxr status`.
    std::string describeXRSystemContext(const SXRSystemContext& ctx);

    // ---- `when` ----

    // A predicate over SXRSystemContext. Every set key must match (AND). An empty `when` matches
    // always — including before any session has bound a system; a non-empty one never matches then.
    struct SXRWhen {
        // `hmd`, verbatim. "desc:<prefix>" matches a leased connector's EDID description (the same
        // prefix semantics as a `desc:` monitor rule); anything else is an RE2 regex SEARCHED in
        // the OpenXR systemName (so "Quest 3" matches "Meta Quest 3 on WiVRn").
        std::optional<std::string>      hmd;
        std::string                     hmdDesc; // trimmed prefix, non-empty iff hmd is a desc: form
        std::shared_ptr<const re2::RE2> hmdRe;   // set iff hmd is a regex form

        std::optional<bool>             eyeGaze;
        std::optional<bool>             handTracking;
        std::optional<bool>             passthrough;

        bool                            empty() const;
        // Canonical `key=value ...` text. Identity for same-name xr_monitor declarations and the
        // form `hyprctl openxr status` prints.
        std::string describe() const;
    };

    // Set `hmd`. Errors on an empty value, an empty `desc:` prefix, or an invalid regex.
    std::expected<void, std::string> setXRWhenHmd(SXRWhen& when, const std::string& value);
    // Set a boolean capability predicate by its config key (eye_gaze, hand_tracking, passthrough).
    // Unknown keys are an error naming the valid ones.
    std::expected<void, std::string> setXRWhenCapability(SXRWhen& when, std::string_view key, bool value);
    // The keys setXRWhenCapability accepts, for error messages.
    inline constexpr const char* XR_WHEN_VALID_KEYS = "hmd, eye_gaze, hand_tracking, passthrough";

    bool                         matchXRWhen(const SXRWhen& when, const SXRSystemContext& ctx);

    // ---- xr_defaults blocks + the resolver ----

    struct SXRDefaultsBlock {
        std::string             profile; // empty = a base block, applies to every monitor
        SXRWhen                 when;
        std::optional<Vector2D> resolution;
        std::optional<float>    refreshRate; // only meaningful with resolution; absent = 60
        std::optional<float>    scale;       // monitor scale; 0 = hand back to the PPI heuristic
        std::optional<float>    size;        // quad width, meters
        std::optional<float>    distance;    // initial placement distance, meters
    };

    // Field validation shared by every front end. scale >= 0 (0 = PPI guess); size/distance > 0.
    std::expected<void, std::string> validateXRDefaultsBlock(const SXRDefaultsBlock& block);

    // Where a resolved field's value came from, highest-precedence last.
    enum eXRDefaultSource : uint8_t {
        XR_DEFSRC_GLOBAL = 0, // openxr:default_* / the headless 1920x1080@60
        XR_DEFSRC_BLOCK,      // a matching base xr_defaults block
        XR_DEFSRC_PROFILE,    // a matching block of the requested profile
        XR_DEFSRC_EXPLICIT,   // the xr_monitor declaration / create args
    };
    const char* xrDefaultSourceName(eXRDefaultSource s);

    // The base underneath every block (openxr:default_* + the headless mode).
    struct SXRDefaultsGlobals {
        Vector2D resolution  = {1920, 1080};
        float    refreshRate = 60.F;
        float    scale       = 1.25F;
        float    size        = 1.6F;
        float    distance    = 1.5F;
    };

    // Values the declaration / create args gave explicitly. They outrank every block.
    struct SXRExplicitFields {
        std::optional<Vector2D> resolution;
        std::optional<float>    refreshRate;
        std::optional<float>    size;

        bool                    operator==(const SXRExplicitFields&) const = default;
    };

    template <typename T>
    struct SXRResolvedField {
        T                value{};
        eXRDefaultSource source = XR_DEFSRC_GLOBAL;
        int              block  = -1; // index into the block list, -1 for global/explicit

        bool             operator==(const SXRResolvedField&) const = default;
    };

    struct SXRResolvedDefaults {
        SXRResolvedField<Vector2D> resolution;
        SXRResolvedField<float>    refreshRate; // always rides with resolution (same source)
        SXRResolvedField<float>    scale;
        SXRResolvedField<float>    size;
        SXRResolvedField<float>    distance;

        bool                       operator==(const SXRResolvedDefaults&) const = default;
    };

    // Does any block carry this profile name? (Matching or not — an existing profile whose blocks do
    // not match the current HMD is not an error, it just contributes nothing.)
    bool xrDefaultsHasProfile(const std::vector<SXRDefaultsBlock>& blocks, const std::string& profile);

    // The resolver. Layers, lowest first: globals; matching base blocks in config order (field-wise,
    // last wins); matching blocks of `profile` in config order (field-wise, last wins); explicit.
    // An unknown non-empty profile is an error. Pure.
    std::expected<SXRResolvedDefaults, std::string> resolveXRDefaults(const std::vector<SXRDefaultsBlock>& blocks, const std::string& profile, const SXRSystemContext& ctx,
                                                                      const SXRDefaultsGlobals& globals, const SXRExplicitFields& explicitFields = {});

    // The pixel mode a monitor should REQUEST (install into its XR-owned monitor rule) for a
    // resolution: the resolved mode, except that a purely global one is left unrequested (nullopt —
    // monitor rules / the headless default decide, the pre-xr_defaults declaration behavior) unless
    // pinGlobalMode (a runtime `create`, which always pinned 1920x1080@60).
    std::pair<std::optional<Vector2D>, std::optional<float>> xrRequestedMode(const SXRResolvedDefaults& resolved, bool pinGlobalMode);

    // ---- name minting ----

    // The lowest `XR-<n>`, n >= 2, not in `taken` (live monitor names + every declared name).
    std::string mintXRMonitorName(const std::vector<std::string>& taken);

    // Strict "WxH" / "WxH@Hz" shape test (digits only). Used where a token may be a name OR a mode.
    bool looksLikeXRMode(std::string_view token);
}
