#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>

namespace OpenXR {
    inline constexpr size_t XR_VIEWPOINT_LEDGER_CAPACITY = 64;

    // Main-thread ledger of samples actually emitted to one client activation. Only identifiers
    // present here may be used by rendered(); consuming is one-shot, and old entries age out at a
    // fixed bound so a stalled or hostile client cannot grow compositor state.
    class CXRViewpointIssuedSampleLedger {
      public:
        explicit CXRViewpointIssuedSampleLedger(size_t capacity = XR_VIEWPOINT_LEDGER_CAPACITY);

        bool   issue(uint64_t sample);
        bool   consume(uint64_t sample);
        void   clear();
        size_t size() const;

      private:
        size_t               m_capacity = XR_VIEWPOINT_LEDGER_CAPACITY;
        std::deque<uint64_t> m_samples;
    };

    // Epoch zero is reserved for "never active" on the wire. Overflow fails closed rather than
    // aliasing an activation whose buffers or messages may still exist.
    bool nextViewpointEpoch(uint64_t current, uint64_t& next);
}
