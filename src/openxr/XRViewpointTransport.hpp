#pragma once

// XRViewpointTransport — protocol-neutral encoded samples and their frame->main mailbox.
//
// This is deliberately OpenXR- and Wayland-header-free. The encoded sample is a fixed-size POD
// made only of explicit-width integer fields; a future protocol binding may map those fields onto
// its own messages without making the pure transport contract depend on that binding.

#include "XRViewpoint.hpp"

#include <array>
#include <cstdint>
#include <mutex>
#include <type_traits>

namespace OpenXR {
    inline constexpr double   XR_VIEWPOINT_UM_PER_METER = 1'000'000.0;
    inline constexpr int32_t  XR_VIEWPOINT_Z_EPSILON_UM = 100;
    inline constexpr uint32_t XR_VIEWPOINT_SAMPLE_VALID = 1U << 0;

    // Explicit high/low representation of a complete uint64 value. Word order is semantic, not a
    // byte serialization: no host-endian or Wayland binding assumption is made here.
    struct SXRViewpointU64 {
        uint32_t hi = 0;
        uint32_t lo = 0;

        bool     operator==(const SXRViewpointU64&) const = default;
    };

    struct SXRViewpointPositionUM {
        int32_t x = 0;
        int32_t y = 0;
        int32_t z = 0;

        bool    operator==(const SXRViewpointPositionUM&) const = default;
    };

    // One canonical encoded sample. A sample is valid as a WHOLE or not at all: there are no
    // per-eye validity bits, so a two-view sample can never expose one current eye and one stale or
    // malformed eye. Unused view slots remain zero.
    struct SXRViewpointEncodedSample {
        SXRViewpointU64                       serial;
        SXRViewpointU64                       geometryId;
        uint32_t                              widthUM  = 0;
        uint32_t                              heightUM = 0;
        std::array<SXRViewpointPositionUM, 2> viewPositions;
        uint32_t                              viewCount = 0;
        uint32_t                              flags     = 0;

        bool                                  operator==(const SXRViewpointEncodedSample&) const = default;
    };

    static_assert(std::is_trivially_copyable_v<SXRViewpointEncodedSample>);
    static_assert(std::is_standard_layout_v<SXRViewpointEncodedSample>);
    static_assert(sizeof(SXRViewpointEncodedSample) == 56);

    SXRViewpointU64 splitViewpointU64(uint64_t value);
    uint64_t        joinViewpointU64(const SXRViewpointU64& value);

    // Round meters to the nearest micrometer (half away from zero). A failure zeroes `out` and
    // returns false; no nonfinite or out-of-range input is saturated into a believable position.
    bool   encodeViewpointPositionUM(double meters, int32_t& out);
    double decodeViewpointPositionUM(int32_t micrometers);

    // Dimensions are finite, strictly positive unsigned micrometers. Values that round to zero or
    // beyond uint32_t fail closed instead of becoming a zero-sized or saturated rectangle.
    bool   encodeViewpointDimensionUM(double meters, uint32_t& out);
    double decodeViewpointDimensionUM(uint32_t micrometers);

    bool   encodedViewpointSampleValid(const SXRViewpointEncodedSample& sample);

    // Encode/decode the stage-zero geometry as one atomic semantic unit. Every output is reset to a
    // safe default on failure. IDs retain the full uint64 range, including zero and UINT64_MAX.
    bool encodeViewpointSample(const SXRViewpointGeometry& geometry, uint64_t serial, uint64_t geometryId, SXRViewpointEncodedSample& out);
    bool decodeViewpointSample(const SXRViewpointEncodedSample& sample, SXRViewpointGeometry& geometry, uint64_t& serial, uint64_t& geometryId);

    struct SXRViewpointPublishResult {
        bool     accepted   = false;
        bool     shouldWake = false; // true only for the empty -> pending edge
        uint64_t generation = 0;     // mailbox-local, unrelated to the sample serial

        bool     operator==(const SXRViewpointPublishResult&) const = default;
    };

    struct SXRViewpointMailboxRead {
        SXRViewpointEncodedSample sample;
        uint64_t                  generation = 0;
        uint64_t                  superseded = 0; // older pending samples replaced by this value

        bool                      operator==(const SXRViewpointMailboxRead&) const = default;
    };

    static_assert(std::is_trivially_copyable_v<SXRViewpointPublishResult>);
    static_assert(std::is_trivially_copyable_v<SXRViewpointMailboxRead>);

    // A bounded one-sample SPSC mailbox. The frame-thread producer overwrites the pending value;
    // the main-thread consumer receives only the newest complete POD. A mutex makes the payload
    // copy data-race-free (a seqlock around non-atomic fields would not). No strings, allocations,
    // Wayland resources, Hyprland refcounts, or XR event-queue entries cross this boundary.
    class CXRViewpointSampleMailbox {
      public:
        // `initialGeneration` permits carrying a counter across an owning activation and makes the
        // overflow boundary testable. Once UINT64_MAX is reached, further publishes fail closed;
        // an activation must replace the mailbox rather than wrap and alias an older generation.
        explicit CXRViewpointSampleMailbox(uint64_t initialGeneration = 0);

        SXRViewpointPublishResult publish(const SXRViewpointEncodedSample& sample);
        bool                      consumeLatest(SXRViewpointMailboxRead& out);
        // Discard the sample pending at this instant without changing the monotonic generation.
        // Lifecycle owners must still stop or synchronize the producer if publications after the
        // discard must also be excluded. Returns true iff a pending sample was discarded.
        bool     clearPending();
        bool     pending() const;
        uint64_t generation() const;

      private:
        mutable std::mutex        m_mutex;
        SXRViewpointEncodedSample m_latest;
        uint64_t                  m_generation = 0;
        uint64_t                  m_superseded = 0;
        bool                      m_pending    = false;
    };
}
