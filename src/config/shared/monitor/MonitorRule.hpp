#pragma once

#include <string>
#include <optional>
#include <cstdint>
#include <xf86drmMode.h>

#include "../../../helpers/math/Math.hpp"
#include "../../../helpers/cm/ColorManagement.hpp"
#include "../../../helpers/CMType.hpp"
#include "../../../helpers/TransferFunction.hpp"
#include "../../../desktop/reserved/ReservedArea.hpp"

namespace Config {
    // Enum for the different types of auto directions, e.g. auto-left, auto-up.
    enum eAutoDirs : uint8_t {
        DIR_AUTO_NONE = 0, /* None will be treated as right. */
        DIR_AUTO_UP,
        DIR_AUTO_DOWN,
        DIR_AUTO_LEFT,
        DIR_AUTO_RIGHT,
        DIR_AUTO_CENTER_UP,
        DIR_AUTO_CENTER_DOWN,
        DIR_AUTO_CENTER_LEFT,
        DIR_AUTO_CENTER_RIGHT
    };

    enum eMonitorRuleComparisonResult : uint8_t {
        COMPARISON_FULL_MATCH = 0, // nothing is different
        COMPARISON_SOFT_MISMATCH,  // only minor differences not needing a modeset
        COMPARISON_NO_MATCH        // needs a modeset
    };

    // HypXRland stereo output (research/24 §3): the physical mode is packed from N identical
    // per-eye panes and the compositor presents ONE logical monitor at pane size. The pack is a
    // final-blit scanout detail (like scale/transform, but Hyprland-private). STEREO_SBS packs two
    // horizontal panes; further layouts (hsbs/tab/htab, WP F5) are data — a different pack divisor
    // and destination boxes — not new mechanisms.
    enum eMonitorStereoMode : uint8_t {
        STEREO_OFF = 0,
        STEREO_SBS, // full side-by-side: mode = 2 panes wide, 1 tall
    };

    constexpr const char* stereoModeToString(eMonitorStereoMode mode) {
        switch (mode) {
            case STEREO_SBS: return "sbs";
            default: return "off";
        }
    }

    class CMonitorRule {
      public:
        CMonitorRule()  = default;
        ~CMonitorRule() = default;

        eMonitorRuleComparisonResult compare(const CMonitorRule& other) const;

        eAutoDirs                    m_autoDir       = DIR_AUTO_NONE;
        std::string                  m_name          = "";
        Vector2D                     m_resolution    = Vector2D();
        Vector2D                     m_offset        = Vector2D(-INT32_MAX, -INT32_MAX);
        float                        m_scale         = -1;
        float                        m_refreshRate   = 60; // Hz
        bool                         m_disabled      = false;
        // HypXRland (XREAL V2.2 leasable-on-demand direct mode): when set via the `lease` monitor-rule
        // flag, this named DESKTOP output is NOT configured as a desktop and is instead offered to
        // drm-lease-v1 clients (monado direct mode owns the flip). Default false ⇒ ordinary desktop
        // monitor, byte-identical to stock. Toggling it reconfigures the output (see MonitorRuleManager).
        bool                         m_lease         = false;
        // Stereo pane packing for this output (research/24 §3.10). For a PHYSICAL stereo output the
        // rule's resolution is the MODE that is scanned out and the logical desktop is derived from
        // it (mode / pack) — that is what the `monitor = …, stereo:sbs` token means.
        eMonitorStereoMode           m_stereo        = STEREO_OFF;
        // …and the inversion, for an output with no panel (research/24 WP X3). When set, m_resolution
        // is ONE PANE and the scanned-out mode is DERIVED from it (pane * pack divisor). This is how
        // an XR monitor becomes a depth-desktop producer without its declared size changing meaning:
        // an `xrmonitor` at 2560x1440 keeps 2560x1440 per eye and scans out 5120x1440. Never set by
        // the `stereo:` token — a real panel's mode is not ours to invent (Monitor::Stereo::requestedMode).
        bool                         m_stereoVirtualMode = false;
        wl_output_transform          m_transform     = WL_OUTPUT_TRANSFORM_NORMAL;
        std::string                  m_mirrorOf      = "";
        bool                         m_enable10bit   = false;
        NCMType::eCMType             m_cmType        = NCMType::CM_SRGB;
        NTransferFunction::eTF       m_sdrEotf       = NTransferFunction::TF_DEFAULT;
        float                        m_sdrSaturation = 1.F; // SDR -> HDR
        float                        m_sdrBrightness = 1.F; // SDR -> HDR
        Desktop::CReservedArea       m_reservedArea;
        std::string                  m_iccFile;

        int                          m_supportsWideColor = 0;    // 0 - auto, 1 - force enable, -1 - force disable
        int                          m_supportsHDR       = 0;    // 0 - auto, 1 - force enable, -1 - force disable
        float                        m_sdrMinLuminance   = 0.2F; // SDR -> HDR
        int                          m_sdrMaxLuminance   = 80;   // SDR -> HDR

        // Incorrect values will result in reduced luminance range or incorrect tonemapping. Shouldn't damage the HW. Use with care in case of a faulty monitor firmware.
        float              m_minLuminance    = -1.F; // >= 0 overrides EDID
        int                m_maxLuminance    = -1;   // >= 0 overrides EDID
        int                m_maxAvgLuminance = -1;   // >= 0 overrides EDID

        drmModeModeInfo    m_drmMode = {};
        std::optional<int> m_vrr;
    };
};
