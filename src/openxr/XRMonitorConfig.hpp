#pragma once

// Deliberately compiles unconditionally (no OpenXR headers) so the config/parser layer can
// live outside the HAVE_OPENXR gate — see docs/openxr/07-roadmap.md conventions. For WP3
// this only holds the SXRMonitorParams struct that the createXRMonitor funnel consumes; the
// full config keyword + dispatcher grammar/parser lands in WP4.

#include <string>
#include <optional>

#include "../helpers/math/Math.hpp"

// Parameters describing one XR monitor. Every create path (config keyword, dispatcher,
// hyprctl) funnels these into COpenXRManager::createXRMonitor (doc 02). Absent optionals
// fall back to headless/openxr:* defaults.
struct SXRMonitorParams {
    std::string             m_name;        // e.g. "XR-1"; must be unique
    std::optional<Vector2D> m_resolution;  // WxH pixel mode; absent => headless default (1920x1080)
    std::optional<float>    m_refreshRate; // @Hz part; absent => headless default
    std::optional<float>    m_sizeMeters;  // quad width in meters; absent => *openxr:default_size
    // NOTE: the parsed anchor state (SXRAnchorState) is added in WP5; WP3 uses a static pose.
};
