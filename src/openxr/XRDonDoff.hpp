#pragma once

// XRDonDoff — the pure decisions of don/doff intent (docs/openxr/research/30-donoff-intent.md):
// per-frame head features (§5.1), motion onset (§6.1), seat selection + intrinsic plausibility
// (§6.3), and the FSM transition function (§4.3).
//
// Compiled UNCONDITIONALLY, no OpenXR headers, no logging, no allocation: every function here is a
// pure function of its arguments and is gtested over synthetic traces (tests/xr/donoff_*.cpp).

#include "XRMath.hpp"
#include "XRPoseHistory.hpp"

#include <array>
#include <cstdint>

namespace OpenXR {
    // ---- §5.1 per-frame head features ----

    constexpr float XR_DEG                 = 3.14159265F / 180.F;
    constexpr float XR_FEAT_ANG_TAU_S      = 0.080F;
    constexpr float XR_FEAT_LIN_TAU_S      = 0.120F;
    constexpr float XR_FEAT_LEVEL_PITCH    = 35.F * XR_DEG;
    constexpr float XR_FEAT_STILL_ANG      = 20.F * XR_DEG; // rad/s
    constexpr float XR_FEAT_STILL_LIN      = 0.10F;         // m/s
    constexpr float XR_FEAT_DEAD_TAU_S     = 2.0F;
    constexpr float XR_FEAT_DEAD_STILL_ANG = 0.2F * XR_DEG; // rad/s RMS
    constexpr float XR_FEAT_DEAD_STILL_LIN = 0.001F;        // m/s RMS

    struct SXRHeadFeatures {
        float angSpeed  = 0.F; // rad/s, EMA τ = 80 ms
        float linSpeed  = 0.F; // m/s, EMA τ = 120 ms (meaningful only when position-tracked)
        float pitch     = 0.F; // rad
        float upY       = 1.F; // cos(tilt from upright)
        float yaw       = 0.F; // rad, qYawOf — valid only when `upright`
        bool  upright   = false;
        bool  level     = false;
        bool  still     = false;
        bool  deadStill = false;
        // running state for the next step
        float angSqEma  = 0.F;
        float linSqEma  = 0.F;
        float observedS = 0.F; // seconds of consecutive valid frames (dead-still needs 2 s)
        bool  primed    = false;
    };

    // One step. `prevValid` false (first frame, or after an invalid view) restarts the EMAs.
    SXRHeadFeatures xrHeadFeatures(const SXRPose& prev, const SXRPose& cur, float dt, const SXRHeadFeatures& prevF, bool posTracked, bool prevValid);

    // ---- §6.1 motion onset ----

    struct SXROnsetParams {
        int64_t quietMinNs = 750'000'000LL;
        int64_t maxBackNs  = 8'000'000'000LL;
    };

    struct SXROnset {
        bool    found        = false;
        int64_t tOnsetNs     = 0; // end of the latest quiet window before the edge
        int64_t quietStartNs = 0; // start of that contiguous quiet run (bounded by maxBack)
    };

    // QUIET: worn, trusted, upright, level and still. (WORN already folds in dead-stillness on the
    // runtimes where it is a wear signal, §4.1.)
    bool     xrSampleQuiet(uint32_t flags);
    SXROnset xrFindMotionOnset(const CXRPoseHistory& h, int64_t tEdgeNs, const SXROnsetParams& p);

    // ---- §6.3 seat selection ----

    enum eXRSeatSource : uint8_t {
        XR_SEAT_NONE = 0,
        XR_SEAT_DWELL,         // the open / most recent episode, cut at the onset
        XR_SEAT_QUIET,         // the quiet window ending at the onset
        XR_SEAT_EPISODE_OLDER, // an older closed episode
        XR_SEAT_SUMMARY,       // a run of T2/T3 buckets the detector missed
        XR_SEAT_RESEAT,        // a deliberate re-seat (§7) — committed directly, never selected
        XR_SEAT_CREATED,       // seeded by `create`
        XR_SEAT_DECLARED,      // no memory: the declared rig
        XR_SEAT_LEGACY,        // written by the legacy staging + drift gate
    };

    enum eXRSeatRefusal : uint8_t {
        XR_SEATREF_OK = 0,
        XR_SEATREF_SHORT,       // shorter than dwellMin
        XR_SEATREF_P1_EVIDENCE, // no upright evidence / non-finite / spread gaze and no group frame
        XR_SEATREF_P2_FRAME,    // measured in a frame that cannot be bridged to the current one
        XR_SEATREF_P3_FACING,   // the arrangement is behind the seat (D3)
        XR_SEATREF_P4_REACH,    // the primary is out of reach, or something is absurdly far
        XR_SEATREF_P5_GAZE,     // gaze ran and the user hardly looked at a monitor from there
        XR_SEATREF_RANK,        // a weaker witness disagrees with a strong memory
        XR_SEATREF_NO_MONITORS, // nothing eligible to remember
    };

    struct SXRSeatCandidate {
        eXRSeatSource src = XR_SEAT_NONE;
        SXRPose       seat; // yaw-only, in gen `gen`; pos.y is the mean head height (unused by the frame)
        uint16_t      gen      = 0;
        int64_t       tStartNs = 0, tEndNs = 0;
        uint64_t      durUs            = 0;
        float         yawConcentration = 0.F;
        float         gazeFrac         = 0.F;
        uint64_t      gazeEvalUs       = 0;
        uint32_t      primaryGaze      = 0; // most-gazed stable id, 0 = none
        float         spreadM          = 0.F;
        bool          haveYaw          = false;
        bool          yawFromGroup     = false;
        uint8_t       dof              = 3;
    };

    constexpr size_t XR_ARR_MAX = 16;

    struct SXRArrangementMonitor {
        uint32_t id = 0;
        SXRPose  world;            // live anchor pose (LOCAL_FLOOR, current gen)
        bool     eligible = false; // LOCAL, not removed, frame established, adaptive docked
        bool     memValid = false;
        SXRPose  memOffset; // current memory (head-relative)
        uint8_t  memSrc = XR_SEAT_NONE;
    };

    struct SXRArrangementView {
        std::array<SXRArrangementMonitor, XR_ARR_MAX> m{};
        size_t                                        n              = 0;
        uint16_t                                      gen            = 0;
        bool                                          groupSeatValid = false;
        SXRPose                                       groupSeat; // xrGroupSeatFrame of the eligible set
        bool                                          authoredSinceCommit = false;
    };

    struct SXRSeatParams {
        SXROnsetParams onset;
        int64_t        dwellMinNs       = 8'000'000'000LL;
        int64_t        lookbackNs       = 30LL * 60 * 1'000'000'000LL;
        float          minConcentration = 0.4F;
        float          facingMaxRad     = 100.F * XR_DEG;
        float          reachPrimaryM    = 5.F;
        float          reachAllM        = 20.F;
        float          gazeMinFrac      = 0.15F;
        int64_t        gazeMinEvalNs    = 1'000'000'000LL; // P5 applies only with this much gaze evidence
        float          rankDisagreeRad  = 90.F * XR_DEG;
        float          summaryFrac      = 0.6F;
        float          seatRadius       = 0.35F;
    };

    struct SXRSeatRefusalRec {
        uint8_t src    = XR_SEAT_NONE;
        uint8_t reason = XR_SEATREF_OK;
        float   detail = 0.F; // degrees (P3, rank) or metres (P4) or fraction (P5)
        int64_t tEndNs = 0;
    };

    struct SXRSeatVerdict {
        bool                             ok = false;
        SXRSeatCandidate                 chosen;
        SXRPose                          seatNow; // chosen.seat re-expressed in the arrangement's gen
        uint32_t                         primary        = 0;
        float                            primaryBearing = 0.F; // rad, signed, right positive
        SXROnset                         onset;
        std::array<SXRSeatRefusalRec, 8> refused{};
        uint8_t                          nRefused   = 0;
        uint8_t                          candidates = 0;
    };

    // signed bearing (rad, right positive) of a world point from a yaw-only seat's forward
    float xrBearingInSeat(const SXRPose& seat, const Vec3& world);
    // the seat expressed in gen `toGen`, through KNOWN generation records; false when unbridgeable
    bool xrBridgeSeat(const CXRPoseHistory& h, uint16_t fromGen, uint16_t toGen, SXRPose& seat);
    // build a candidate from (cut) episode sums
    SXRSeatCandidate xrSeatFromAccum(const SXREpisodeAccum& a, eXRSeatSource src);
    // P1..P5 + the ranking rule. May replace the candidate's yaw with the group frame's (R̄ < 0.4).
    eXRSeatRefusal xrSeatPlausible(SXRSeatCandidate& c, const CXRPoseHistory& h, const SXRArrangementView& arr, const SXRSeatParams& p, float& detail, uint32_t& primary,
                                   float& primaryBearing, SXRPose& seatNow);
    SXRSeatVerdict xrSelectSeat(const CXRPoseHistory& h, int64_t tEdgeNs, const SXRArrangementView& arr, const SXRSeatParams& p);

    const char*    xrSeatSourceName(uint8_t src);
    const char*    xrSeatRefusalName(uint8_t r);

    // ---- §6.4 provenance ----

    struct SXRRestoreProvenance {
        uint8_t src           = XR_SEAT_NONE;
        int64_t committedAtNs = 0;
        int64_t seatStartNs = 0, seatEndNs = 0;
        float   yawConcentration = 0.F, gazeFrac = 0.F;
        uint8_t lastRefusal = XR_SEATREF_OK;
    };

    // ---- §4.3 the FSM ----

    enum eXRDonState : uint8_t {
        XR_DON_NO_SESSION = 0,
        XR_DON_UNWORN,
        XR_DON_DONNING,
        XR_DON_WORN,
        XR_DON_DOFFING,
    };

    enum eXRDonInput : uint8_t {
        XR_DIN_SESSION_UP = 0,
        XR_DIN_SESSION_DOWN,
        XR_DIN_WORN_UP,
        XR_DIN_WORN_DOWN,
        XR_DIN_HYDRATED,
        XR_DIN_BACKSTOP,
        XR_DIN_DISCONTINUITY,
        XR_DIN_RESEAT_DONE,
        XR_DIN_AUTHORED,
        XR_DIN_COMMIT_DONE, // the DOFFING commit has run
    };

    enum eXRDonAction : uint8_t {
        XR_DA_COMMIT_FROM_HISTORY = 1u << 0,
        XR_DA_COMMIT_RESEAT       = 1u << 1,
        XR_DA_PUBLISH_DONNING     = 1u << 2,
        XR_DA_SHOW_IN_PLACE       = 1u << 3,
        XR_DA_MARK_AUTHORED       = 1u << 4,
        XR_DA_LOG_WARN            = 1u << 5,
    };

    struct SXRDonFacts {
        bool decisionCertainResume = true;  // §5.4: the pending don decision is certainly RESUME
        bool lateDiscontinuity     = false; // ≤ 2 s after HYDRATED
        bool committedSinceWorn    = false; // a doff commit already ran since the last WORN
        bool recenterOnPlug        = true;
        bool donGate               = false; // Phase 2: hide + settle on don (off in Phases 0/1)
    };

    struct SXRDonStep {
        eXRDonState next    = XR_DON_NO_SESSION;
        uint8_t     actions = 0;
    };

    SXRDonStep  xrDonTransition(eXRDonState s, eXRDonInput in, const SXRDonFacts& f);
    const char* xrDonStateName(uint8_t s);
}
