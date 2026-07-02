#pragma once

// XRQueue — a lock-free single-producer / single-consumer ring buffer used for the OpenXR
// frame-thread -> main-thread event handoff (docs/openxr/04-input.md §7.2). Compiled
// UNCONDITIONALLY (no HAVE_OPENXR guard, no OpenXR headers) so the ring can be unit-tested by
// hyprland_gtests (tests/xr/event_queue.cpp) with a trivial payload type.
//
// Capacity must be a power of two; one slot is reserved to disambiguate full vs. empty, so a
// ring of capacity CAP holds up to CAP-1 live items at once. The producer owns m_head, the
// consumer owns m_tail. Ordering: the producer writes the item then release-stores head; the
// consumer acquire-loads head, reads the item, then release-stores tail.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace OpenXR {
    template <typename T, size_t CAP>
    class CXRSPSCRing {
        static_assert(CAP >= 2 && (CAP & (CAP - 1)) == 0, "CAP must be a power of two >= 2");

      public:
        // Producer thread only. Returns false (dropping the item) when the ring is full.
        bool push(T item) {
            const uint32_t head = m_head.load(std::memory_order_relaxed);
            const uint32_t next = (head + 1) & MASK;
            if (next == m_tail.load(std::memory_order_acquire))
                return false; // full
            m_buf[head] = std::move(item);
            m_head.store(next, std::memory_order_release);
            return true;
        }

        // Consumer thread only. Returns false when the ring is empty.
        bool pop(T& out) {
            const uint32_t tail = m_tail.load(std::memory_order_relaxed);
            if (tail == m_head.load(std::memory_order_acquire))
                return false; // empty
            out = std::move(m_buf[tail]);
            m_tail.store((tail + 1) & MASK, std::memory_order_release);
            return true;
        }

        bool empty() const {
            return m_head.load(std::memory_order_acquire) == m_tail.load(std::memory_order_acquire);
        }

        // NOT thread-safe: only call when no producer/consumer is running (e.g. during teardown
        // after the frame thread is joined).
        void reset() {
            m_head.store(0, std::memory_order_relaxed);
            m_tail.store(0, std::memory_order_relaxed);
        }

      private:
        static constexpr uint32_t MASK = static_cast<uint32_t>(CAP - 1);
        std::array<T, CAP>        m_buf{};
        std::atomic<uint32_t>     m_head{0}; // producer writes
        std::atomic<uint32_t>     m_tail{0}; // consumer writes
    };
}
