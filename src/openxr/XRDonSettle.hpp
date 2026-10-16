#pragma once

// XRDonSettle — research/30 Phase 2 (§5.2 / §5.3): on don, wait for the head to settle before
// anything is shown or moved. A small, PURE state machine fed one sample per frame by the frame
// thread: no OpenXR headers, no logging, no allocation (a fixed ring), no clock of its own. It is
// gtested over synthetic traces in tests/xr/donoff_settle.cpp.
//
// Two modes, picked on the main thread at the DONNING edge:
//
//  * XR_SETTLE_POSE — the hydration will PLACE monitors around the head (a RESTORE re-seat), so the
//    head has to be still, level, upright and tracked for the hold window, and the hydration pose is
//    that window's mean (position mean, circular yaw mean), never the last sample. At the timeout the
//    fallback ladder of §5.2 picks a pose (rung 1 / 2) or none (rung 3).
//  * XR_SETTLE_TRACKING — nothing will move (a RESUME); the room is only hidden because the runtime
//    is known to deliver 6DoF and is not doing so yet (XREAL SLAM re-acquiring after a doff: rotation
//    is good, position is pinned, so translating the head swims the room). Shown as soon as position
//    tracking has held for XR_SETTLE_TRACK_HOLD_NS, or at the timeout.

#include "XRMath.hpp"

#include <array>
#include <cstdint>

namespace OpenXR {
    constexpr float   XR_SETTLE_DEG               = 3.14159265F / 180.F;
    constexpr int64_t XR_SETTLE_MIN_DELAY_NS      = 150'000'000LL;        // after the edge
    constexpr int64_t XR_SETTLE_HOLD_NS           = 500'000'000LL;        // all criteria, continuously
    constexpr int64_t XR_SETTLE_TRACK_HOLD_NS     = 250'000'000LL;        // TRACKING mode: position tracked, continuously
    constexpr float   XR_SETTLE_ANG_MAX           = 30.F * XR_SETTLE_DEG; // rad/s, every sample of the hold
    constexpr float   XR_SETTLE_ANG_MEAN          = 12.F * XR_SETTLE_DEG; // rad/s, mean over the hold
    constexpr float   XR_SETTLE_PITCH             = 35.F * XR_SETTLE_DEG;
    constexpr float   XR_SETTLE_ROLL              = 20.F * XR_SETTLE_DEG;
    constexpr float   XR_SETTLE_LIN_MAX           = 0.15F;                // m/s, when position-tracked
    constexpr int64_t XR_SETTLE_BEST_SUB_NS       = 300'000'000LL;        // rung 1 sub-window
    constexpr float   XR_SETTLE_RECLINED_PITCH    = 60.F * XR_SETTLE_DEG; // rung 2
    constexpr int64_t XR_SETTLE_DEFAULT_TIMEOUT   = 3'000'000'000LL;
    constexpr int64_t XR_SETTLE_BACKSTOP_EXTRA_MS = 1000; // main-thread backstop = timeout + this

    enum eXRSettleMode : uint8_t {
        XR_SETTLE_POSE = 0,
        XR_SETTLE_TRACKING,
    };

    enum eXRSettleState : uint8_t {
        XR_SETTLE_WAITING = 0,
        XR_SETTLE_SETTLED,
        XR_SETTLE_TIMEOUT,
    };

    // §5.2 timeout ladder; 0 = settled normally
    enum eXRSettleRung : uint8_t {
        XR_RUNG_SETTLED = 0,
        XR_RUNG_BEST_WINDOW,  // the calmest upright+level sub-window seen since the edge
        XR_RUNG_LAST_UPRIGHT, // the most recent upright sample with |pitch| <= 60° (reclined user)
        XR_RUNG_IN_PLACE,     // nothing usable: show where they are, move nothing on our own account
    };

    // why the hold never completed (logged with a timeout)
    enum eXRSettleBlock : uint8_t {
        XR_SBLK_NONE = 0,
        XR_SBLK_NO_VIEW,     // never a valid, trusted head
        XR_SBLK_NO_POSITION, // 6DoF expected, position never tracked long enough
        XR_SBLK_NOT_UPRIGHT,
        XR_SBLK_NOT_LEVEL, // pitch or roll out of bounds
        XR_SBLK_MOVING,    // angular or linear speed
    };

    struct SXRSettleParams {
        eXRSettleMode mode      = XR_SETTLE_POSE;
        int64_t       timeoutNs = XR_SETTLE_DEFAULT_TIMEOUT;
        bool          needPos   = false; // the runtime is expected to deliver POSITION_TRACKED
    };

    struct SXRSettleInput {
        int64_t tNs = 0; // steady clock
        SXRPose head;
        bool    valid      = false;
        bool    trusted    = false; // TRACKED under the conditional rule (headTrustworthy)
        bool    posTracked = false; // XR_SPACE_LOCATION_POSITION_TRACKED_BIT
        float   angSpeed   = 0.F;   // rad/s (the history's EMA'd features)
        float   linSpeed   = 0.F;   // m/s
    };

    struct SXRSettleResult {
        eXRSettleState state    = XR_SETTLE_WAITING;
        eXRSettleRung  rung     = XR_RUNG_SETTLED;
        eXRSettleBlock block    = XR_SBLK_NONE; // the commonest reason the hold kept breaking
        bool           havePose = false;        // POSE mode: `pose` is the hydration pose (yaw-only rotation)
        SXRPose        pose;
        int64_t        settleNs = 0;                                         // edge -> decision
        float          angMax = 0.F, angMean = 0.F, pitch = 0.F, roll = 0.F; // over the window used (rad, rad/s)
    };

    // head roll about its forward axis, rad (right ear down positive)
    float xrHeadRoll(const Quat& q);
    // §5.3: the don fade-in, smoothstep over fadeNs; 1 when fadeNs <= 0
    float xrDonFadeAlpha(int64_t sinceNs, int64_t fadeNs);

    class CXRSettleDetector {
      public:
        void            reset(int64_t tEdgeNs, const SXRSettleParams& p);
        SXRSettleResult step(const SXRSettleInput& in);
        bool            active() const;

      private:
        struct SSample {
            int64_t tNs = 0;
            Vec3    pos;
            float   yaw    = 0.F;
            float   ang    = 0.F;
            float   pitch  = 0.F;
            float   roll   = 0.F;
            bool    ok     = false; // satisfies every per-sample criterion of the mode
            bool    usable = false; // valid, trusted, upright and level (rung 1)
        };

        struct SWindow {
            bool  valid = false;
            Vec3  pos;
            float yaw     = 0.F;
            float angMean = 0.F, angMax = 0.F, pitch = 0.F, roll = 0.F;
        };

        // trailing window of samples ending at the newest, all satisfying `pred`, spanning >= spanNs
        SWindow                   trailingWindow(int64_t spanNs, bool usableOnly) const;
        void                      noteBlock(eXRSettleBlock b);

        static constexpr size_t   RING = 256; // the longest window is 500 ms; 256 samples covers 500 Hz

        std::array<SSample, RING> m_ring{};
        size_t                    m_head   = 0; // next write
        size_t                    m_n      = 0;
        bool                      m_active = false;
        SXRSettleParams           m_params;
        int64_t                   m_tEdgeNs = 0;
        // rung 1: the calmest usable sub-window since the edge
        SWindow m_best;
        // rung 2: the most recent upright sample with |pitch| <= 60°
        bool                    m_haveReclined = false;
        SSample                 m_reclined;
        std::array<uint32_t, 6> m_blockCounts{};
    };
}
