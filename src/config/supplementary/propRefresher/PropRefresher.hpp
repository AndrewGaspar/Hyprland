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
        REFRESH_MONITOR_STATES_OWN = (1 << 6), // see below — the monitor pass WITHOUT the implied layout pass
        REFRESH_MONITOR_STATES     = REFRESH_MONITOR_STATES_OWN | REFRESH_LAYOUTS,
        REFRESH_CURSOR_ZOOMS       = (1 << 7),
        REFRESH_CONFIG_WATCHER     = (1 << 8),
        REFRESH_GRADIENTS_GROUPBAR = (1 << 9),

        REFRESH_ALL = std::numeric_limits<std::underlying_type_t<ePropRefreshProp>>::max(),
    };

    // A note on the two COMPOSITE classes, because their `&` tests do not mean what they look like.
    //
    // REFRESH_MONITOR_STATES and REFRESH_WINDOW_STATES OR IN the class they imply, so a plain
    // `tripped & REFRESH_MONITOR_STATES` is a bit-SUPERSET test: a bare REFRESH_LAYOUTS satisfies it.
    // For window states that is the intent — REFRESH_RULES has no handler of its own, and the window
    // pass IS its handler. For monitor states it is not: REFRESH_LAYOUTS has its own block right
    // below, so a bare layout refresh (a runtime `gaps_in` retune under the Lua config) used to run
    // the monitor pass as well — scheduleReload(), ensureVRR(), ensurePersistentWorkspacesPresent()
    // and a second recalculateMonitor() per monitor, none of which a gaps change needs. Test
    // REFRESH_MONITOR_STATES_OWN when you mean "the monitor class itself was tripped".

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
