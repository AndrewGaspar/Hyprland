#pragma once

// Deliberately compiles unconditionally (no OpenXR headers, no HAVE_OPENXR guard) so the
// config/parser layer can live outside the gate and hyprland_gtests can always exercise it
// (docs/openxr/07-roadmap.md conventions; parser tests in tests/xr/parser.cpp).

#include <cstdint>
#include <string>
#include <optional>
#include <expected>
#include <vector>

#include "../helpers/math/Math.hpp"
#include "XRAnchor.hpp" // OpenXR::SXRAnchorState + eXRAnchorMode/eXRHand (unconditional pure math)

namespace OpenXR {
    // Environment blend mode (doc 01). A HAVE_OPENXR-free mirror of XrEnvironmentBlendMode so the
    // selection logic (pickBlendMode) is a pure, unconditionally-compiled function that
    // hyprland_gtests can exercise without a runtime. The guarded session code converts to/from
    // the real XrEnvironmentBlendMode enum (xrBlendModeToXr / xrBlendModeFromXr in XRSession.hpp).
    enum eXRBlendMode : uint8_t {
        XR_BLEND_OPAQUE = 0, // composite over black — the classic VR "floating in a void" look
        XR_BLEND_ALPHA,      // composite over the runtime's passthrough underlay via layer alpha
        XR_BLEND_ADDITIVE,   // additive (optical see-through / additive displays)
    };

    // "opaque" | "alpha" | "additive" — the config/IPC string form (doc 05).
    std::string blendModeToString(eXRBlendMode mode);

    // Result of pickBlendMode: the chosen mode plus whether the user's explicit request could not
    // be honored (so the caller can emit the unsupported->fallback WARN — doc 01).
    struct SXRBlendModePick {
        eXRBlendMode mode                 = XR_BLEND_OPAQUE;
        bool         requestedUnsupported = false;
    };

    // Pure blend-mode selection (doc 01). `supported` is the runtime's advertised list in
    // preference order (xrEnumerateEnvironmentBlendModes returns preferred-first). `config` is the
    // openxr:blend_mode value: "auto" (or anything unrecognized) => the runtime's first-listed
    // (preferred) mode; an explicit "opaque"/"alpha"/"additive" is honored iff supported, else it
    // falls back to the preferred mode with requestedUnsupported=true. An empty supported list
    // (spec-illegal, but defended) yields XR_BLEND_OPAQUE.
    SXRBlendModePick pickBlendMode(const std::vector<eXRBlendMode>& supported, const std::string& config);

    // openxr:monitors_follow_session mode (research/18 + report-18 addendum). Governs WHEN
    // XR-created monitors behave like a plugged external display.
    enum eXRMonitorFollowMode : uint8_t {
        XR_FOLLOW_OFF = 0,  // never unplug — always-present (the pre-feature behavior). Legacy 0/false/no.
        XR_FOLLOW_SESSION,  // plug while an OpenXR session EXISTS (start()..stop()). Legacy 1/true/yes.
        XR_FOLLOW_VISIBLE,  // plug only while the session is VISIBLE/FOCUSED — a doffed/standby headset
                            // (WiVRn keeps a session alive on the shelf) reads as unplugged. Default.
    };

    // Parse the openxr:monitors_follow_session config string to the mode. Accepts the new
    // "off"|"session"|"visible" spellings AND the legacy boolean spellings for config compat:
    // 0/false/no/off -> OFF, 1/true/yes/session -> SESSION, visible/focused -> VISIBLE. Anything
    // unrecognized (including empty) -> VISIBLE (the default). Case/whitespace-insensitive. Pure.
    eXRMonitorFollowMode parseMonitorFollowMode(const std::string& v);

    // Plugged-state policy (research/18 — XR monitors behave like unplugged external monitors
    // while no session is usable). Pure and unconditional so hyprland_gtests can exercise it
    // (tests/xr/plugged.cpp). `sessionUp` is the manager's session-EXISTENCE edge (start()..stop());
    // `sessionVisible` is the VISIBLE/FOCUSED edge. Returns whether XR-created monitors should
    // currently be enabled ("plugged"): OFF => always; SESSION => sessionUp; VISIBLE => sessionVisible.
    // The anti-flap grace period around a VISIBLE->hidden drop is applied by the caller, NOT here —
    // this stays a pure instantaneous predicate.
    bool wantXRMonitorsPlugged(eXRMonitorFollowMode mode, bool sessionUp, bool sessionVisible);
}

namespace OpenXR {
    // WP5 unification: the parser now produces the canonical doc-03 SXRAnchorState directly
    // (absorbing WP4's placeholder SXRAnchorSpec). The parser stays pure/unconditional; it just
    // includes the equally-pure XRAnchor.hpp for the shared enums/state type.

    // Anchor mode -> the string used by doc 05 §4.3 (status/layout):
    // local|head|body|device:left|device:right.
    std::string anchorModeToString(eXRAnchorMode mode, eXRHand device);
    std::string anchorModeToString(const SXRAnchorState& state);

    // Inverse of parseXRMonitorLine (doc 05 §2.2 grammar / doc 03 §7 pose->text serialization
    // rules): produces one paste-ready `xrmonitor = ...` config line. Pure and unconditional so
    // it is shared by COpenXRManager::layoutDump() (the live `hyprctl openxr layout` path, which
    // resolves `pose` per doc 03 §7 — the anchor's live solved world pose for LOCAL, the
    // persistent stored offset for head/body/device) and by the round-trip unit test
    // (tests/xr/parser.cpp) that this line, reparsed through parseXRMonitorLine, reproduces an
    // equivalent SXRMonitorParams. `anchor.anchorPose` is ignored — `pose` is what gets printed.
    std::string serializeXRMonitorLine(const std::string& name, Vector2D resolution, std::optional<float> refreshHz, const SXRAnchorState& anchor, const SXRPose& pose,
                                        float sizeMeters);
}

// Parameters describing one XR monitor. Every create path (config keyword, dispatcher,
// hyprctl) funnels these into COpenXRManager::createXRMonitor (doc 02). Absent optionals fall
// back to headless/openxr:* defaults.
struct SXRMonitorParams {
    std::string             m_name;        // e.g. "XR-1"; must be unique
    std::optional<Vector2D> m_resolution;  // WxH pixel mode; absent => headless default (1920x1080)
    std::optional<float>    m_refreshRate; // @Hz part; absent => headless default (60)
    std::optional<float>    m_sizeMeters;  // quad width in meters; absent => *openxr:default_size

    // Parsed anchor as the canonical doc-03 state (WP5). anchorPose.rot is built from the
    // parsed yaw/pitch; widthMeters is left at its default here (seeded from m_sizeMeters /
    // openxr:default_size when the layer is created).
    OpenXR::SXRAnchorState m_anchor;
    // True iff an anchor-spec was explicitly given (config keyword always does; the create verb
    // may omit it, in which case the caller places the monitor along the current gaze, doc 05 §3.1).
    bool m_anchorProvided = false;
};

namespace OpenXR {
    // Pure parser for the xrmonitor config keyword value (doc 05 §2.2/§2.3):
    //   <name>, <mode>, <anchor-spec>[, <kv>]...
    // Returns the parsed params or a human-readable error. Compiled unconditionally.
    std::expected<SXRMonitorParams, std::string> parseXRMonitorLine(const std::string& args);

    // Pure parser for the `xrmonitor create` dispatcher / `hyprctl openxr create` verb (doc 05
    // §3.1): space-separated `<name> [WxH[@Hz]] [anchor-spec]`, with defaults applied by the
    // caller (mode defaults to 1920x1080@60, anchor defaults to anchor:local when absent).
    std::expected<SXRMonitorParams, std::string> parseXRMonitorCreateArgs(const std::string& args);
}
