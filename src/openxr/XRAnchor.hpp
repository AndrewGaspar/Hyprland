#pragma once

// XRAnchor — the pure-math anchoring engine (docs/openxr/03-anchoring.md).
//
// Compiled UNCONDITIONALLY (no HAVE_OPENXR guard, no OpenXR headers) so hyprland_gtests can
// always exercise it (tests/xr/anchor_math.cpp). All solve code is pure math: poses in, a pose
// + a space *selection* (enum, not an XrSpace handle) out. Tuning arrives in a struct; there are
// no clocks, globals, or config lookups here — the caller (COpenXRManager) reads config and
// passes it in. CXRAnchor contains no locking; threading is the caller's responsibility.

#include <array>
#include <cstdint>
#include <optional>

#include "XRMath.hpp"

namespace OpenXR {
    // ---- enums (doc 03 §2.1) ----
    enum eXRAnchorMode : uint8_t {
        XR_ANCHOR_LOCAL = 0, // fixed in LOCAL_FLOOR
        XR_ANCHOR_HEAD,      // head leash (view-space offset, spring + deadzone)
        XR_ANCHOR_BODY,      // body leash (yaw-only body frame)
        XR_ANCHOR_DEVICE,    // locked to a controller grip space
    };

    enum eXRHand : uint8_t {
        XR_HAND_LEFT = 0,
        XR_HAND_RIGHT,
    };

    // Which XrSpace the quad layer must reference; mapped to real handles by CXRMonitorLayer.
    enum eXRSpaceSelector : uint8_t {
        XR_SPACE_LOCAL_FLOOR = 0,
        XR_SPACE_GRIP_LEFT,
        XR_SPACE_GRIP_RIGHT,
    };

    // ---- constants (doc 03 §8) ----
    constexpr float XR_LEASH_SETTLE_POS = 0.01F; // m, re-latch threshold (§3.2)
    constexpr float XR_BODY_YAW_HOLD    = 0.15F; // horiz-projection len below which yaw holds (§3.3)
    constexpr float XR_BODY_YAW_RESUME  = 0.25F; // ...and above which it resumes (hysteresis)
    constexpr float XR_WIDTH_MIN        = 0.2F;  // m (§4.3, §5.3)
    constexpr float XR_WIDTH_MAX        = 4.0F;
    constexpr float XR_DISTANCE_MIN     = 0.3F; // m (§4.3, §5.4)
    constexpr float XR_DISTANCE_MAX     = 5.0F;
    constexpr float XR_SOLVE_DT_MAX     = 0.1F; // s, dt clamp (§2.3)

    // ---- release-latching ring (grabbable-borders WP-G4, research 04-grabbable-borders.md §5.4) ----
    //
    // The grab-release lurch: on a squeeze/grasp/pinch release, the input device (especially a
    // fist-open on hand tracking, but also a controller flick) swings the grip pose exactly on the
    // release frame; re-anchoring from that frame's pose bakes the swing into the persistent anchor
    // and the window lurches. The fix is to keep a short per-hand history of the CARRIED world pose
    // and, on release, re-anchor from a pose a little earlier than the release edge — before the
    // perturbation — and/or from the last "calm" (below-velocity-threshold) sample.
    //
    // Everything here is POD math (SXRPose = Vec3 + Quat) with ZERO hyprutils SP/WP refcount ops,
    // satisfying the frame-thread rule in XRMonitorLayer.hpp by construction. The per-hand ring
    // instances live in CXRInput (frame thread); the struct lives here (unconditional) so the pure
    // interpolation / velocity-rejection math is gtest-covered like the rest of the anchor engine.

    constexpr uint32_t XR_GRAB_MAX_REWIND_MS = 500; // cap on how far velocity-rejection may rewind

    struct SXRGrabSample {
        SXRPose  world;           // carried quad world pose (LOCAL_FLOOR) this frame
        uint32_t timeMs   = 0;    // monotonic sample stamp
        float    linSpeed = 0.F;  // m/s of world.pos vs the previous sample (0 for the first)
    };

    // Fixed-capacity circular buffer of recent carry poses. index 0 in the "back" accessors is the
    // NEWEST sample. Not thread-safe (single-thread, frame-owned by design).
    struct SXRGrabRing {
        static constexpr uint32_t CAP = 128; // ~1.4 s at 90 Hz; power of two

        std::array<SXRGrabSample, CAP> buf{};
        uint32_t                       count = 0; // logical size, saturates at CAP
        uint32_t                       head  = 0; // index of the next write

        void     reset() {
            count = 0;
            head  = 0;
        }
        uint32_t size() const {
            return count < CAP ? count : CAP;
        }
        // back == 0 -> newest; caller guarantees back < size().
        const SXRGrabSample& at(uint32_t back) const {
            return buf[(head + CAP - 1 - back) % CAP];
        }
        // Append a carried pose; computes linSpeed against the previous newest sample.
        void push(const SXRPose& world, uint32_t timeMs);
        // World pose at (nowMs - latencyMs), linearly (pos) / slerp (rot) interpolated between the
        // bracketing samples; clamps to newest/oldest; identity if empty.
        SXRPose sampleBack(uint32_t nowMs, uint32_t latencyMs) const;
        // Most recent sample whose linSpeed < linThresh, searching back from newest but never past
        // (nowMs - maxBackMs). If the whole in-window span is above threshold, returns the
        // furthest-back in-window sample (maximally rewound). Identity if empty.
        SXRPose lastCalm(float linThresh, uint32_t nowMs, uint32_t maxBackMs) const;
    };

    // Pure release-pose selector (gtest truth table). If velReject > 0 and the newest sample is
    // moving faster than velReject, walk back to the last calm sample (velocity-outlier rejection);
    // otherwise rewind by latencyMs. Identity if the ring is empty (caller should fall back to the
    // release-frame endGrab in that case).
    SXRPose pickReleasePose(const SXRGrabRing& ring, uint32_t nowMs, uint32_t latencyMs, float velReject);

    // ---- per-layer persistent state (doc 03 §2.1) ----
    struct SXRAnchorState {
        eXRAnchorMode mode   = XR_ANCHOR_LOCAL;
        eXRHand       device = XR_HAND_LEFT; // meaningful iff mode == XR_ANCHOR_DEVICE

        // Meaning depends on mode (§2.1):
        //   LOCAL : quad pose in LOCAL_FLOOR (world)
        //   HEAD  : offset in VIEW space (display orientation is lookAt-driven, §3.2)
        //   BODY  : offset in the yaw-only body frame (§3.3)
        //   DEVICE: offset in the grip space of `device`
        SXRPose anchorPose;

        float   bodyHeight  = 0.F;  // BODY only: stored y of the body frame origin (meters)
        float   widthMeters = 1.6F; // quad width; height derived = widthMeters * pxH / pxW

        bool    operator==(const SXRAnchorState& o) const {
            return mode == o.mode && device == o.device && anchorPose.pos.x == o.anchorPose.pos.x && anchorPose.pos.y == o.anchorPose.pos.y &&
                anchorPose.pos.z == o.anchorPose.pos.z && anchorPose.rot.x == o.anchorPose.rot.x && anchorPose.rot.y == o.anchorPose.rot.y &&
                anchorPose.rot.z == o.anchorPose.rot.z && anchorPose.rot.w == o.anchorPose.rot.w;
        }
    };

    // ---- solve API (doc 03 §2.3) ----
    struct SXRAnchorTuning {
        float leashResponse    = 0.35F;   // openxr:leash_response          (s)
        float deadzoneAngleRad = 0.2618F; // openxr:leash_deadzone_angle  (rad; cfg in deg, ~15)
        float deadzoneDistance = 0.25F;   // openxr:leash_deadzone_distance (m)
        bool  bodyFollowHeight = false;   // openxr:body_leash_follow_height
        float defaultDistance  = 1.5F;    // openxr:default_distance        (m)
    };

    struct SXRSolveInput {
        SXRPose                view;     // VIEW pose in LOCAL_FLOOR at predictedDisplayTime
        float                  dt = 0.F; // seconds since last solve (clamped to [0, 0.1])
        std::optional<SXRPose> gripLeft; // grip poses in LOCAL_FLOOR; nullopt = tracking invalid
        std::optional<SXRPose> gripRight;
        uint32_t               pxW = 1, pxH = 1; // current monitor mode, for aspect
    };

    struct SXRSolveResult {
        eXRSpaceSelector space = XR_SPACE_LOCAL_FLOOR; // which XrSpace the quad layer references
        SXRPose          pose;                         // quad pose expressed IN that space
        SXRPose          worldPose;                    // same pose in LOCAL_FLOOR (hit tests, IPC)
        float            widthMeters  = 1.6F;
        float            heightMeters = 0.9F;
    };

    // Verb context captured by the manager from the most recent frame-thread solve inputs (§5).
    struct SXRVerbContext {
        SXRPose                view;
        bool                   viewValid = false;
        std::optional<SXRPose> gripLeft, gripRight;
    };

    class CXRAnchor {
      public:
        CXRAnchor() = default;

        // (Re)seed the anchor from a persistent state (config load, create). Marks the solver
        // uninitialized so the first solve() seeds the spring from the state/view.
        void initFromState(const SXRAnchorState& state);

        // Per-frame solve on the frame thread (§3).
        SXRSolveResult solve(const SXRSolveInput& in, const SXRAnchorTuning& tune);

        // ---- grab pose math (§4) — the grab state MACHINE is WP8 ----
        void beginGrab(eXRHand hand, const SXRPose& gripWorld);
        void grabPushPull(float deltaMeters);
        void grabResize(float deltaMeters);
        // Re-anchor from the quad's world pose at the release frame (grip ∘ offset). Kept for
        // callers with no release-latch ring available.
        void endGrab(const SXRSolveInput& in, const SXRAnchorTuning& tune);
        // WP-G4: re-anchor from an EXPLICIT world pose (the latched / velocity-rejected release
        // pose from SXRGrabRing) instead of the release-frame grip pose. `in`/`tune` still supply
        // the view + grip context needed to re-express the world pose into the persistent mode.
        void endGrab(const SXRPose& releaseWorld, const SXRSolveInput& in, const SXRAnchorTuning& tune);

        // ---- verbs (§5) — main thread, under the layer mutex ----
        // d = (dx, dy, dz) as given by the user; the solver forms view -Z for dz.
        bool applyMove(const Vec3& d, const SXRVerbContext& ctx);
        bool applyRotate(float dyawRad, float dpitchRad, const SXRVerbContext& ctx);
        bool applyScale(bool isDelta, float f);
        bool applyDistance(float dMeters, const SXRVerbContext& ctx);
        bool applyCenter(const SXRVerbContext& ctx, float defaultDistance);

        // ---- mode transitions (§5.6) + recentering (§6) ----
        bool setMode(eXRAnchorMode newMode, eXRHand hand, const SXRVerbContext& ctx, const SXRAnchorTuning& tune);
        void onReferenceSpaceChanged(const SXRPose& poseInPreviousSpace);

        // ---- accessors (main thread reads for IPC/status) ----
        const SXRAnchorState& state() const {
            return m_state;
        }
        SXRPose lastWorld() const {
            return m_lastWorld;
        }
        bool hasLastWorld() const {
            return m_hasLastWorld;
        }
        bool grabbed() const {
            return m_grabbed;
        }

        SXRAnchorState m_state;

      private:
        // Re-express a world pose W into the persistent mode's representation and reset the
        // solver runtime state (shared by endGrab §4.4 and setMode §5.6).
        void reanchorFromWorld(const SXRPose& W, const SXRVerbContext& ctx, const SXRAnchorTuning& tune);
        // Body frame from the view (§3.3). updateFilter runs the yaw hysteresis.
        SXRPose computeBodyFrame(const SXRPose& view, const SXRAnchorTuning& tune, bool updateFilter);
        // §5.5 center placement (view forward at defaultDistance, facing the head).
        SXRPose centerPlacement(const SXRVerbContext& ctx, float defaultDistance) const;

        // leash spring state
        Vec3 m_springPos;          // current smoothed quad position (world)
        Vec3 m_springVel;          // its velocity
        Quat m_smoothedRot;        // current smoothed orientation (world)
        bool m_chasing    = false; // deadzone latch: false = LATCHED, true = CHASING
        bool m_springInit = false; // spring seeded for the current mode
        // body-frame yaw filter
        float m_lastYaw            = 0.F;
        bool  m_yawHolding         = false;
        bool  m_bodyHeightCaptured = false;
        // grab
        bool    m_grabbed  = false;
        eXRHand m_grabHand = XR_HAND_LEFT;
        SXRPose m_grabOffset;                // in grabbing hand's grip space
        bool    m_deviceOffsetDirty = false; // DEVICE: recompute offset on first valid grip
        // last composed world pose (LOCAL_FLOOR)
        SXRPose m_lastWorld;
        bool    m_hasLastWorld = false;
    };
}
