#pragma once

// Deliberately compiles unconditionally (no OpenXR headers, no HAVE_OPENXR guard) so the
// config/parser layer can live outside the gate and hyprland_gtests can always exercise it
// (docs/openxr/07-roadmap.md conventions; parser tests in tests/xr/parser.cpp).

#include <string>
#include <optional>
#include <expected>

#include "../helpers/math/Math.hpp"
#include "XRAnchor.hpp" // OpenXR::SXRAnchorState + eXRAnchorMode/eXRHand (unconditional pure math)

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
