#pragma once

// Deliberately compiles unconditionally (no OpenXR headers, no HAVE_OPENXR guard) so the
// config/parser layer can live outside the gate and hyprland_gtests can always exercise it
// (docs/openxr/07-roadmap.md conventions; parser tests in tests/xr/parser.cpp).

#include <string>
#include <optional>
#include <expected>

#include "../helpers/math/Math.hpp"

namespace OpenXR {
    // Anchor mode as declared by the xrmonitor keyword / create verb (doc 05 §2.2).
    // NOTE(WP5): doc 03 defines the canonical eXRAnchorMode/eXRHand + SXRAnchorState in
    // XRAnchor.hpp (which does not exist yet). WP4 only needs a parsed representation, so this
    // is the minimal parsed anchor spec; WP5 must unify it with SXRAnchorState.
    enum eXRAnchorMode : uint8_t {
        XR_ANCHOR_LOCAL = 0, // fixed in LOCAL_FLOOR
        XR_ANCHOR_HEAD,      // head leash
        XR_ANCHOR_BODY,      // body leash (yaw-only)
        XR_ANCHOR_DEVICE,    // locked to a controller grip
    };

    enum eXRHand : uint8_t {
        XR_HAND_LEFT = 0,
        XR_HAND_RIGHT,
    };

    // Parsed anchor spec. Coordinates are meters in the OpenXR convention (+X right, +Y up,
    // -Z forward). For local, m_pos is the LOCAL_FLOOR world position; for head/body/device it
    // is the offset in the respective leash/device frame. yaw/pitch are degrees.
    struct SXRAnchorSpec {
        eXRAnchorMode mode     = XR_ANCHOR_LOCAL;
        eXRHand       device   = XR_HAND_LEFT; // meaningful iff mode == XR_ANCHOR_DEVICE
        float         posX     = 0.f;
        float         posY     = 0.f;
        float         posZ     = 0.f;
        float         yawDeg   = 0.f;
        float         pitchDeg = 0.f;
        bool          hasYaw   = false;
        bool          hasPitch = false;

        bool          operator==(const SXRAnchorSpec& o) const {
            return mode == o.mode && device == o.device && posX == o.posX && posY == o.posY && posZ == o.posZ && yawDeg == o.yawDeg && pitchDeg == o.pitchDeg &&
                hasYaw == o.hasYaw && hasPitch == o.hasPitch;
        }
    };

    // Anchor mode -> the string used by doc 05 §4.3 (status/layout): local|head|body|device:left|device:right.
    std::string anchorModeToString(const SXRAnchorSpec& spec);
}

// Parameters describing one XR monitor. Every create path (config keyword, dispatcher,
// hyprctl) funnels these into COpenXRManager::createXRMonitor (doc 02). Absent optionals fall
// back to headless/openxr:* defaults.
struct SXRMonitorParams {
    std::string             m_name;        // e.g. "XR-1"; must be unique
    std::optional<Vector2D> m_resolution;  // WxH pixel mode; absent => headless default (1920x1080)
    std::optional<float>    m_refreshRate; // @Hz part; absent => headless default (60)
    std::optional<float>    m_sizeMeters;  // quad width in meters; absent => *openxr:default_size

    // Parsed anchor spec (doc 05 §2.2). WP4 stores it (and the layer keeps WP3's static pose);
    // WP5 makes it live via the anchoring engine.
    OpenXR::SXRAnchorSpec m_anchor;
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
