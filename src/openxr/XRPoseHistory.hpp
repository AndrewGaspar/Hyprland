#pragma once

// XRPoseHistory — the multi-resolution, in-memory head + monitor pose history behind don/doff intent
// (docs/openxr/research/30-donoff-intent.md §8).
//
// Compiled UNCONDITIONALLY (no OpenXR headers) so hyprland_gtests links it like XRMath.hpp's pose
// ring. Everything here is POD: fixed-capacity arrays, no allocation, no strings, no refcounts. The
// single writer is the XR frame thread (one push per frame under COpenXRManager::m_historyMu); the
// main thread copies the whole object out under the same mutex and decides on the copy.
//
// Tiers (§8.1): T0 is every frame; T1..T4 are SUMMARIES (250 ms, 2 s, 15 s, 2 min) whose fields are
// all associative (sums, min/max, or/and), so a T4 bucket knows exactly what brute force over its raw
// frames would — never a subsample. E holds dwell episodes, G the origin generations, J the journal.

#include "XRMath.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace OpenXR {
    constexpr size_t  XR_HIST_MAX_MONITORS               = 8; // per record (§8.4)
    constexpr size_t  XR_HIST_T0_CAP                     = 2048;
    constexpr size_t  XR_HIST_TIER_CAP                   = 1024;
    constexpr size_t  XR_HIST_EPISODE_CAP                = 256;
    constexpr size_t  XR_HIST_GEN_CAP                    = 64;
    constexpr size_t  XR_HIST_JOURNAL_CAP                = 256;
    constexpr size_t  XR_HIST_TIERS                      = 4;
    constexpr int64_t XR_HIST_TIER_RES_NS[XR_HIST_TIERS] = {
        250'000'000LL,
        2'000'000'000LL,
        15'000'000'000LL,
        120'000'000'000LL,
    };
    // A frame gap longer than this is not "time spent" in any state (a stalled loop, a session pause):
    // it is clamped so a hitch can neither vote nor inflate a dwell (§6.2).
    constexpr int64_t XR_HIST_MAX_DT_NS = 50'000'000LL;

    enum eXRHistFlag : uint32_t {
        XR_HF_VALID           = 1u << 0,
        XR_HF_TRACKED         = 1u << 1,
        XR_HF_POS_TRACKED     = 1u << 2,
        XR_HF_VISIBLE         = 1u << 3,
        XR_HF_PRESENT         = 1u << 4,
        XR_HF_PRESENCE_KNOWN  = 1u << 5,
        XR_HF_WORN            = 1u << 6,
        XR_HF_UPRIGHT         = 1u << 7,
        XR_HF_LEVEL           = 1u << 8,
        XR_HF_STILL           = 1u << 9,
        XR_HF_DEAD_STILL      = 1u << 10,
        XR_HF_IN_DWELL        = 1u << 11,
        XR_HF_GRAB            = 1u << 12,
        XR_HF_ROAMING         = 1u << 13,
        XR_HF_GAZE_ON_MONITOR = 1u << 14,
        XR_HF_DONNING         = 1u << 15,
        XR_HF_TRUSTED         = 1u << 16, // TRACKED, or VALID on a runtime that never reports TRACKED
        XR_HF_GAZE_EVAL       = 1u << 17, // the gaze pass ran this frame (P5 applies only then)
        XR_HF_EP_ACCUM        = 1u << 18, // this sample was summed into the running episode (set by push)
    };

    enum eXRHistMonitorFlag : uint8_t {
        XR_HMF_STALE    = 1u << 0, // anchorPose still expressed in a dead frame (research/28 §F)
        XR_HMF_GRABBED  = 1u << 1,
        XR_HMF_ROAMING  = 1u << 2, // adaptive, not docked
        XR_HMF_ELIGIBLE = 1u << 3, // anchor:local and not pending removal (xrReseatEligible)
        XR_HMF_GAZED    = 1u << 4, // the raw gaze hit this frame
    };

    // One monitor in one record. `id` is the layer's stable history id (never 0, never reused, not a
    // MONITORID — that is -1 while unplugged and recycled by the compositor). For anchor:local `pos/rot`
    // is the LOCAL_FLOOR world pose in the record's gen; for the other modes it is the anchor offset.
    struct SXRHistMonitorPose {
        uint32_t id    = 0;
        uint8_t  mode  = 0; // eXRAnchorMode
        uint8_t  flags = 0; // eXRHistMonitorFlag
        uint16_t pad   = 0;
        Vec3     pos;
        Quat     rot;
    };

    struct SXRHistSample {
        int64_t            tNs = 0; // steady clock (CLOCK_MONOTONIC) ns — the clock main-thread edges use
        Vec3               headPos;
        Quat               headRot;
        float              angSpeed        = 0.F; // rad/s EMA (§5.1)
        float              linSpeed        = 0.F; // m/s EMA
        uint32_t           arrangeEpoch    = 0;
        uint32_t           gazeMonitorId   = 0; // stable id of the raw gaze hit, 0 = none
        uint16_t           gen             = 0;
        uint8_t            nMonitors       = 0;
        uint8_t            monitorsDropped = 0;
        uint32_t           flags           = 0; // eXRHistFlag
        SXRHistMonitorPose monitors[XR_HIST_MAX_MONITORS];
    };

    struct SXRHistMonitorSummary {
        uint32_t id = 0, n = 0;
        Vec3     posSum, posMin, posMax;
        Quat     rotSum{0.F, 0.F, 0.F, 0.F}; // Σ of hemisphere-canonical (w >= 0) quats
        uint8_t  mode = 0, flagsOr = 0, flagsAnd = 0xFF, pad = 0;
    };

    struct SXRHistSummary {
        int64_t               t0Ns = 0, t1Ns = 0;
        uint32_t              n      = 0;
        uint32_t              wornUs = 0, trustedUs = 0, levelUs = 0, stillUs = 0, dwellUs = 0, gazeOnMonitorUs = 0, gazeEvalUs = 0;
        Vec3                  posSum; // over LEVEL samples
        uint32_t              nLevel = 0;
        Vec3                  posMin, posMax;                           // over trusted samples
        float                 yawWCos = 0.F, yawWSin = 0.F, yawW = 0.F; // stillness-weighted (§6.2)
        float                 pitchSum    = 0.F;                        // over LEVEL
        float                 angSpeedMax = 0.F, linSpeedMax = 0.F;
        uint32_t              arrangeEpochLast = 0;
        uint32_t              flagsOr = 0, flagsAnd = 0xFFFFFFFFu;
        uint16_t              gen             = 0;
        uint8_t               closedEarly     = 0; // 1 = gen change, 2 = session end
        uint8_t               nMonitors       = 0;
        uint8_t               monitorsDropped = 0;
        SXRHistMonitorSummary monitors[XR_HIST_MAX_MONITORS];
    };

    // ---- §6.2 tuning: the episode detector ----
    struct SXREpisodeParams {
        float   seatRadius = 0.35F;           // m, horizontal
        int64_t dwellMinNs = 8'000'000'000LL; // worn+trusted time before an episode opens
        float   minLevel   = 0.70F;           // level fraction an episode needs to open
        int64_t walkOutNs  = 1'500'000'000LL; // outside seatRadius this long = a walk
    };

    // The running (open or candidate) episode, with the raw sums the published SXRDwellEpisode is
    // finalized from. Everything is a sum so a tail can be SUBTRACTED exactly (xrSelectSeat cuts the
    // take-off motion out at the motion onset, §6.3).
    struct SXREpisodeAccum {
        int64_t  tStartNs = 0, tLastNs = 0;
        uint64_t durUs = 0, levelUs = 0, gazeEvalUs = 0, gazeOnUs = 0;
        double   posSumX = 0.0, posSumY = 0.0, posSumZ = 0.0;
        uint64_t nPos    = 0;
        double   yawWCos = 0.0, yawWSin = 0.0, yawW = 0.0;
        Vec3     posMin, posMax;
        float    angSpeedMax = 0.F;
        uint32_t gazeIds[8]  = {0, 0, 0, 0, 0, 0, 0, 0};
        uint64_t gazeUs[8]   = {0, 0, 0, 0, 0, 0, 0, 0};
        uint32_t epochStart = 0, epochEnd = 0;
        uint16_t gen        = 0;
        bool     posTracked = false;
        bool     any        = false;
    };

    enum eXREpisodeEnd : uint8_t {
        XR_EP_OPEN = 0,
        XR_EP_WALK,
        XR_EP_DOFF,
        XR_EP_GEN,
        XR_EP_SESSION,
    };

    struct SXRDwellEpisode {
        int64_t            tStartNs = 0, tEndNs = 0;
        Vec3               seatPos;
        float              seatYaw = 0.F, yawConcentration = 0.F;
        Vec3               posMin, posMax;
        float              levelFrac = 0.F, gazeFrac = 0.F, angSpeedMax = 0.F;
        uint32_t           durUs = 0, gazeEvalUs = 0;
        uint32_t           gazeIds[4]        = {0, 0, 0, 0};
        uint32_t           gazeUs[4]         = {0, 0, 0, 0};
        uint32_t           arrangeEpochStart = 0, arrangeEpochEnd = 0;
        uint16_t           gen       = 0;
        uint8_t            dof       = 3;
        uint8_t            endReason = XR_EP_OPEN;
        uint8_t            nMonitors = 0;
        SXRHistMonitorPose arrangement[XR_HIST_MAX_MONITORS];
        // the raw sums the fields above were finalized from, kept so a tail (the take-off motion) can
        // be cut out exactly after the episode has already been closed by the frame thread (§6.3)
        SXREpisodeAccum acc;
    };

    enum eXRGenBridge : uint8_t {
        XR_BRIDGE_SAME = 0,
        XR_BRIDGE_KNOWN,
        XR_BRIDGE_STAGE,
        XR_BRIDGE_UNKNOWN,
    };

    enum eXRGenCause : uint8_t {
        XR_GENC_SESSION_START = 0,
        XR_GENC_RECENTER_KNOWN,
        XR_GENC_DISCONTINUITY,
    };

    // gen `gen` began at tNs; `M` re-expresses gen-1 poses into gen poses as P_new = inv(M) ∘ P_old
    // (CXRAnchor::onReferenceSpaceChanged's convention) when bridge == KNOWN.
    struct SXRGenRecord {
        int64_t  tNs = 0;
        SXRPose  M;
        uint16_t gen    = 0;
        uint8_t  bridge = XR_BRIDGE_UNKNOWN;
        uint8_t  cause  = XR_GENC_SESSION_START;
    };

    enum eXRJournalKind : uint8_t {
        XR_J_PRESENCE = 0,  // a = present
        XR_J_VISIBILITY,    // a = visible
        XR_J_SESSION,       // a = up
        XR_J_WORN,          // a = worn, b = wear-signal source
        XR_J_DOFF_COMMIT,   // a = seat source, b = committed count, x = onset-before-edge s
        XR_J_RESEAT_COMMIT, // b = committed count
        XR_J_AUTHORED,      // a = monitor id (0 = group), b = arrange epoch
        XR_J_MONITOR_CREATED,
        XR_J_MONITOR_DESTROYED,
        XR_J_MONITOR_RENAMED, // reserved (no rename verb exists today)
        XR_J_FORGET,          // a = monitor id (0 = all), b = count
        XR_J_GEN,             // a = new gen, b = bridge
        XR_J_SHADOW,          // a = shadow seat source, b = agreeing monitors, x = worst disagreement (deg)
    };

    struct SXRJournalEntry {
        int64_t  tNs    = 0;
        uint32_t a      = 0;
        uint32_t b      = 0;
        float    x      = 0.F;
        float    y      = 0.F;
        uint8_t  kind   = 0;
        uint8_t  pad[7] = {0, 0, 0, 0, 0, 0, 0};
    };

    // Fixed-capacity overwrite ring. `count` is the monotonic total pushed; at(0) is the oldest live.
    template <typename T, size_t N>
    struct SXRHistRing {
        static_assert(N > 0 && (N & (N - 1)) == 0, "SXRHistRing capacity must be a power of two");
        std::array<T, N> buf{};
        uint64_t         count = 0;

        void             push(const T& v) {
            buf[count & (N - 1)] = v;
            ++count;
        }
        size_t size() const {
            return count < N ? static_cast<size_t>(count) : N;
        }
        bool empty() const {
            return count == 0;
        }
        const T& at(size_t i) const {
            return buf[(count - size() + i) & (N - 1)];
        }
        // k = 0 is the newest
        const T& back(size_t k = 0) const {
            return buf[(count - 1 - k) & (N - 1)];
        }
    };

    // ---- pure helpers (XRPoseHistory.cpp) ----

    // Head +Y in world, y component: cos of the tilt away from upright (pitch and roll combined).
    float xrHeadUpY(const Quat& q);
    // §5.1 / R2: the ONLY gate a durable yaw read may use. A head pitched past vertical (glasses lifted
    // up and back, or parked on the forehead) has upY < 0 and qYawOf would return yaw + 180°.
    bool xrHeadUpright(const Quat& q);
    // forward-vector pitch, rad (positive = looking up)
    float xrHeadPitch(const Quat& q);
    // §6.2: a sample's yaw vote. dt is clamped to XR_HIST_MAX_DT_NS; weight = dt_s / (1 + ω/ω₀).
    constexpr float XR_STILL_OMEGA0 = 6.F * 3.14159265F / 180.F; // rad/s
    float           xrStillnessWeight(int64_t dtNs, float angSpeed);
    // time-weighted contribution of a sample covering (prev, cur], clamped
    int64_t xrHistDtNs(int64_t prevNs, int64_t curNs);

    // the summary of one raw sample covering dtNs
    SXRHistSummary xrHistSummaryOf(const SXRHistSample& s, int64_t dtNs);
    // associative merge (b's span follows a's); per-monitor entries merge by id (§8.3)
    void xrHistMerge(SXRHistSummary& a, const SXRHistSummary& b);
    void xrHistMonitorMerge(SXRHistMonitorSummary* dst, uint8_t& nDst, uint8_t& dropped, const SXRHistMonitorSummary* src, uint8_t nSrc);
    // mean yaw + concentration of a summary's weighted vector; false when there is no vote
    bool xrHistMeanYaw(const SXRHistSummary& s, float& yaw, float& concentration);

    // episode accumulation (also used to build QUIET / tail windows over T0)
    void xrEpisodeAccumulate(SXREpisodeAccum& a, const SXRHistSample& s, int64_t dtNs);
    void xrEpisodeSubtract(SXREpisodeAccum& a, const SXREpisodeAccum& tail);
    // subtract everything the sums hold after cutNs (samples marked XR_HF_EP_ACCUM in T0), and end
    // the episode at cutNs. Exact for every summed field; min/max stay an over-estimate.
    void xrEpisodeCutTail(SXREpisodeAccum& acc, const SXRHistRing<SXRHistSample, XR_HIST_T0_CAP>& t0, int64_t cutNs);
    // seat pose + stats; `concentration` is the weighted resultant length
    SXRDwellEpisode xrEpisodeFinalize(const SXREpisodeAccum& a, uint8_t endReason);

    class CXRPoseHistory {
      public:
        // FRAME THREAD (under the owner's mutex). Pushes one frame: computes the IN_DWELL flag, folds
        // the sample into the tiers and steps the episode detector. A gen change closes the open
        // buckets and the episode first (no summary ever averages two frames).
        void push(SXRHistSample s);
        // session end: close every open bucket (closedEarly = 2) and the episode
        void endSession(int64_t tNs);
        // close the running episode now (doff edge seen by the main thread); idempotent
        void closeEpisode(int64_t tEndNs, uint8_t reason);

        void recordGen(const SXRGenRecord& g);
        void journal(const SXRJournalEntry& e);

        // readers
        const SXRHistRing<SXRHistSample, XR_HIST_T0_CAP>&        t0() const;
        const SXRHistRing<SXRHistSummary, XR_HIST_TIER_CAP>&     tier(size_t k) const; // k = 0..3 (T1..T4)
        const SXRHistSummary&                                    openBucket(size_t k) const;
        bool                                                     openBucketValid(size_t k) const;
        const SXRHistRing<SXRDwellEpisode, XR_HIST_EPISODE_CAP>& episodes() const;
        const SXRHistRing<SXRGenRecord, XR_HIST_GEN_CAP>&        gens() const;
        const SXRHistRing<SXRJournalEntry, XR_HIST_JOURNAL_CAP>& journalRing() const;
        // the running episode (candidate or open). `open` is true once it qualified as a dwell.
        const SXREpisodeAccum& runningEpisode() const;
        bool                   runningEpisodeOpen() const;
        SXREpisodeParams       m_params;

      private:
        void                                                                     foldIntoTier(size_t k, const SXRHistSummary& s);
        void                                                                     closeTier(size_t k, uint8_t reason);
        void                                                                     closeAllTiers(uint8_t reason);
        void                                                                     stepEpisode(SXRHistSample& s, int64_t dtNs);
        void                                                                     finishEpisode(int64_t tEndNs, uint8_t reason);
        void                                                                     resetCandidate(const SXRHistSample& s);

        SXRHistRing<SXRHistSample, XR_HIST_T0_CAP>                               m_t0;
        std::array<SXRHistRing<SXRHistSummary, XR_HIST_TIER_CAP>, XR_HIST_TIERS> m_tiers;
        std::array<SXRHistSummary, XR_HIST_TIERS>                                m_open{};
        std::array<bool, XR_HIST_TIERS>                                          m_openValid{};
        SXRHistRing<SXRDwellEpisode, XR_HIST_EPISODE_CAP>                        m_episodes;
        SXRHistRing<SXRGenRecord, XR_HIST_GEN_CAP>                               m_gens;
        SXRHistRing<SXRJournalEntry, XR_HIST_JOURNAL_CAP>                        m_journal;

        SXREpisodeAccum                                                          m_ep;
        bool                                                                     m_epOpen = false;
        Vec3                                                                     m_epAnchor;
        int64_t                                                                  m_epOutsideNs = 0; // 0 = inside
        int64_t                                                                  m_lastNs      = 0;
        uint16_t                                                                 m_lastGen     = 0;
        bool                                                                     m_haveLast    = false;
    };

    static_assert(sizeof(SXRHistMonitorPose) == 36);
    static_assert(sizeof(SXRHistSample) == 64 + XR_HIST_MAX_MONITORS * sizeof(SXRHistMonitorPose));
    static_assert(sizeof(SXRHistMonitorSummary) == 64);
    static_assert(sizeof(SXRJournalEntry) == 32);
    static_assert(std::is_trivially_copyable_v<CXRPoseHistory>, "the history is copied out whole under the lock");
    static_assert(sizeof(CXRPoseHistory) < 3'700'000, "research/30 §8.1 budget (~3.5 MiB)");
}
