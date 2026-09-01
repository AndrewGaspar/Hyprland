#pragma once

#include <cstdint>
#include <limits>
#include <type_traits>

#include "../../../helpers/memory/Memory.hpp"

namespace Config::Supplementary {
    enum ePropRefreshProp : uint16_t {
        REFRESH_LAYOUTS            = (1 << 0),
        REFRESH_INPUT_DEVICES      = (1 << 1),
        REFRESH_SCREEN_SHADER      = (1 << 2),
        REFRESH_BLUR_FB            = (1 << 3),
        REFRESH_RULES              = (1 << 4),
        REFRESH_WINDOW_STATES      = (1 << 5) | REFRESH_RULES,
        REFRESH_MONITOR_STATES     = (1 << 6) | REFRESH_LAYOUTS,
        REFRESH_CURSOR_ZOOMS       = (1 << 7),
        REFRESH_CONFIG_WATCHER     = (1 << 8),
        REFRESH_GRADIENTS_GROUPBAR = (1 << 9),

        REFRESH_ALL = std::numeric_limits<std::underlying_type_t<ePropRefreshProp>>::max(),
    };

    using PropRefreshBits = std::underlying_type_t<ePropRefreshProp>;

    class CPropRefresher {
      public:
        void            scheduleRefresh(PropRefreshBits reason);
        int             executeScheduledRefreshImmediately();

        // Which classes the refresh currently being announced actually carried.
        //
        // config.props_refreshed is a CHATTY hook: it fires for every prop refresh whatever tripped
        // it, and it carries only "was this the scheduled one". m_propsTripped is already back to 0
        // by the time the event is emitted, so a listener had no way to tell an input-device re-apply
        // (`hl.device{}`, an IME's virtual keyboard churn) apart from a monitor/rule change, and the
        // heavyweight listeners — COpenXRManager::onConfigReload() above all — ran their full pass on
        // every one of them. Latched here for the duration of the emit so a listener can filter.
        //
        // Only meaningful from inside a props_refreshed handler; between refreshes it holds the
        // classes of the LAST one (0 before the first).
        PropRefreshBits lastRefreshedProps() const;

      private:
        void            refreshProp(const bool execdAsScheduled);

        bool            m_scheduled           = false;
        uint64_t        m_scheduledRefreshSeq = 0; // 0 if no refresh event scheduled
        PropRefreshBits m_propsTripped        = 0;
        PropRefreshBits m_lastRefreshedProps  = 0;
    };

    UP<CPropRefresher>& refresher();
};
