#include "XRAnchor.hpp"

// Pure math — compiled unconditionally, no OpenXR headers (see the header). See
// docs/openxr/03-anchoring.md §3–§6 for every formula below.

#include <algorithm>
#include <cmath>

using namespace OpenXR;

namespace {
    // Linear-pos / shortest-arc-rot interpolation between two carried poses (§5.4 ring).
    SXRPose lerpPose(const SXRPose& a, const SXRPose& b, float f) {
        return SXRPose{a.pos + (b.pos - a.pos) * f, qSlerp(a.rot, b.rot, f)};
    }
} // namespace

// ---- release-latching ring (WP-G4) — pure POD math, frame-thread-owned by CXRInput ----

void SXRGrabRing::push(const SXRPose& world, uint32_t timeMs) {
    float sp = 0.F;
    if (size() > 0) {
        const SXRGrabSample& prev = at(0);
        const float          dtS  = timeMs > prev.timeMs ? (float)(timeMs - prev.timeMs) / 1000.F : 0.F;
        if (dtS > 0.F)
            sp = (world.pos - prev.world.pos).length() / dtS;
    }
    buf[head] = SXRGrabSample{world, timeMs, sp};
    head      = (head + 1) % CAP;
    if (count < CAP)
        ++count;
}

SXRPose SXRGrabRing::sampleBack(uint32_t nowMs, uint32_t latencyMs) const {
    if (size() == 0)
        return SXRPose{};

    const uint32_t targetMs = nowMs > latencyMs ? nowMs - latencyMs : 0;
    const SXRGrabSample& newest = at(0);
    if (targetMs >= newest.timeMs)
        return newest.world;
    const SXRGrabSample& oldest = at(size() - 1);
    if (targetMs <= oldest.timeMs)
        return oldest.world;

    for (uint32_t b = 0; b + 1 < size(); ++b) {
        const SXRGrabSample& newer = at(b);
        const SXRGrabSample& older = at(b + 1);
        if (older.timeMs <= targetMs && targetMs <= newer.timeMs) {
            const uint32_t span = newer.timeMs - older.timeMs;
            const float    f    = span > 0 ? (float)(targetMs - older.timeMs) / (float)span : 0.F;
            return lerpPose(older.world, newer.world, f);
        }
    }
    return newest.world; // unreachable given the clamps above
}

SXRPose SXRGrabRing::lastCalm(float linThresh, uint32_t nowMs, uint32_t maxBackMs) const {
    if (size() == 0)
        return SXRPose{};

    const uint32_t floorMs  = nowMs > maxBackMs ? nowMs - maxBackMs : 0;
    SXRPose        fallback = at(0).world; // furthest-back in-window sample seen so far
    for (uint32_t b = 0; b < size(); ++b) {
        const SXRGrabSample& s = at(b);
        if (s.timeMs < floorMs)
            break; // do not rewind past the window
        fallback = s.world;
        if (s.linSpeed < linThresh)
            return s.world;
    }
    return fallback; // the whole in-window span was above threshold -> maximally rewound
}

float SXRGrabRing::releasePeakSpeed(uint32_t nowMs, uint32_t windowMs) const {
    const uint32_t floorMs = nowMs > windowMs ? nowMs - windowMs : 0;
    float          peak    = 0.F;
    for (uint32_t b = 0; b < size(); ++b) {
        const SXRGrabSample& s = at(b);
        if (s.timeMs <= floorMs)
            break; // older than the release window -> carry, not release
        if (s.linSpeed > peak)
            peak = s.linSpeed;
    }
    return peak;
}

float SXRGrabRing::carryTypicalSpeed(uint32_t nowMs, uint32_t windowMs, uint32_t maxBackMs) const {
    const uint32_t hiMs = nowMs > windowMs ? nowMs - windowMs : 0;   // newest carry edge (exclude release window)
    const uint32_t loMs = nowMs > maxBackMs ? nowMs - maxBackMs : 0; // oldest carry edge (rewind cap)
    std::array<float, CAP> speeds{};
    uint32_t               n = 0;
    for (uint32_t b = 0; b < size(); ++b) {
        const SXRGrabSample& s = at(b);
        if (s.timeMs > hiMs)
            continue; // still inside the release window
        if (s.timeMs < loMs)
            break; // past the rewind cap
        speeds[n++] = s.linSpeed;
    }
    if (n == 0)
        return 0.F;
    // Lower-trimmed mean: average of the faster ceil(n/2) samples. Robust both ways — stationary
    // just-grabbed samples (a rest-then-flick carry) can't drag it toward 0 like a median would,
    // and a single-frame tracking spike can't dominate it like a max would.
    std::sort(speeds.begin(), speeds.begin() + n); // ascending
    const uint32_t k   = (n + 1) / 2;              // faster half, ceil
    float          sum = 0.F;
    for (uint32_t i = n - k; i < n; ++i)
        sum += speeds[i];
    return sum / (float)k;
}

SXRPose OpenXR::pickReleasePose(const SXRGrabRing& ring, uint32_t nowMs, uint32_t latencyMs, float velRejectRatio) {
    if (ring.size() == 0)
        return SXRPose{};
    if (velRejectRatio > 0.F && ring.size() >= 2) {
        const float peak  = ring.releasePeakSpeed(nowMs, XR_GRAB_RELEASE_WINDOW_MS);
        const float carry = ring.carryTypicalSpeed(nowMs, XR_GRAB_RELEASE_WINDOW_MS, XR_GRAB_MAX_REWIND_MS);
        const float denom = std::max(carry, XR_GRAB_CARRY_SPEED_FLOOR);
        // Outlier iff the release is both absolutely non-trivial AND relatively much faster than the
        // typical carry. A uniformly fast carry keeps peak ≈ carry (ratio ≈ 1) -> not an outlier.
        if (peak > XR_GRAB_CARRY_SPEED_FLOOR && peak > velRejectRatio * denom)
            return ring.lastCalm(denom * XR_GRAB_CALM_MARGIN, nowMs, XR_GRAB_MAX_REWIND_MS);
    }
    return ring.sampleBack(nowMs, latencyMs);
}

namespace {
    // Critically-damped spring exact step (§3.2). Unconditionally stable for any dt; applied
    // per Vec3 component via the vector ops.
    void springStep(Vec3& x, Vec3& v, const Vec3& target, float response, float dt) {
        const float w = 2.F / response;
        const Vec3  D = x - target;
        const Vec3  k = v + D * w;
        const float E = std::exp(-w * dt);
        x             = target + (D + k * dt) * E;
        v             = (v - k * (w * dt)) * E;
    }

    // wrap to (-pi, pi]
    float wrapPi(float a) {
        constexpr float PI  = 3.14159265358979323846F;
        constexpr float TAU = 2.F * PI;
        a                   = std::fmod(a, TAU);
        if (a <= -PI)
            a += TAU;
        else if (a > PI)
            a -= TAU;
        return a;
    }

    Quat identityQuat() {
        return {0.F, 0.F, 0.F, 1.F};
    }
}

void CXRAnchor::initFromState(const SXRAnchorState& state) {
    m_state              = state;
    m_springInit         = false;
    m_chasing            = false;
    m_springVel          = Vec3{};
    m_bodyHeightCaptured = false;
    m_deviceOffsetDirty  = false;
    m_grabbed            = false;
    m_grabPinch          = false;
    m_grabHandActive     = false;
    m_carryFilter.reset();
    m_resizing           = false;
    // adaptive runtime reset (the config in m_state.adaptive is preserved; the phase machine is not).
    m_adPhase            = XRAD_DOCKED;
    m_dockSeatCaptured   = false;
    m_outDwell           = 0.F;
    m_inDwell            = 0.F;
    m_adT                = 0.F;
    m_seedLeashAtTarget  = false;
    m_adRoamRuntimeSet   = false;
    m_adLeftSinceDock    = false;
    // m_lastWorld / m_hasLastWorld intentionally preserved: switching representation of an
    // existing on-screen quad should not lose where it currently is.
}

SXRPose CXRAnchor::centerPlacement(const SXRVerbContext& ctx, float defaultDistance) const {
    if (!ctx.viewValid) {
        // No tracking yet: a sensible eye-height default in front of the origin.
        return SXRPose{Vec3{0.F, 1.4F, -defaultDistance}, identityQuat()};
    }
    const Vec3 fwd = qRotate(ctx.view.rot, Vec3{0.F, 0.F, -1.F});
    SXRPose    W;
    W.pos = ctx.view.pos + fwd * defaultDistance;
    W.rot = lookAtNoRoll(W.pos, ctx.view.pos, identityQuat());
    return W;
}

SXRPose CXRAnchor::computeBodyFrame(const SXRPose& view, const SXRAnchorTuning& tune, bool updateFilter) {
    const Vec3  fwd   = qRotate(view.rot, Vec3{0.F, 0.F, -1.F});
    const float horiz = std::sqrt(fwd.x * fwd.x + fwd.z * fwd.z);

    if (updateFilter) {
        if (m_yawHolding) {
            if (horiz > XR_BODY_YAW_RESUME)
                m_yawHolding = false;
        } else if (horiz < XR_BODY_YAW_HOLD)
            m_yawHolding = true;
        if (!m_yawHolding)
            m_lastYaw = std::atan2(-fwd.x, -fwd.z);
    }

    SXRPose bf;
    bf.pos = Vec3{view.pos.x, tune.bodyFollowHeight ? view.pos.y : m_state.bodyHeight, view.pos.z};
    bf.rot = qFromYaw(m_lastYaw);
    return bf;
}

SXRSolveResult CXRAnchor::solve(const SXRSolveInput& in, const SXRAnchorTuning& tune) {
    const float    dt = std::clamp(in.dt, 0.F, XR_SOLVE_DT_MAX);

    SXRSolveResult res;
    res.widthMeters  = m_state.widthMeters;
    res.heightMeters = quadHeightMeters(m_state.widthMeters, in.pxW, in.pxH);

    // Grab override (§4.2): behave as device-locked to the grabbing hand. The device is the wrist
    // grip pose for controllers/grasp, or (WP-G5) the hand pinch pose — the offset was captured
    // against whichever pose beginGrab received, so the same device pose + a matching space
    // selector (GRIP_* vs PINCH_*) must be used here for the runtime late-latch to compose right.
    if (m_grabbed) {
        const bool left = m_grabHand == XR_HAND_LEFT;
        if (m_grabPinch)
            res.space = left ? XR_SPACE_PINCH_LEFT : XR_SPACE_PINCH_RIGHT;
        else
            res.space = left ? XR_SPACE_GRIP_LEFT : XR_SPACE_GRIP_RIGHT;
        const std::optional<SXRPose>& dev = m_grabPinch ? (left ? in.pinchLeft : in.pinchRight) : (left ? in.gripLeft : in.gripRight);

        // §4.2 amendment: user-facing modes keep re-evaluating orientation continuously while
        // carried instead of staying rigid to the wrist until release. Position remains a
        // device-space offset (late-latched by the runtime, zero added latency); only the
        // offset's rotation is refreshed from this frame's poses — head faces the viewer,
        // body faces yaw-only. local/device grabs stay fully rigid: carrying like an object
        // (tilt it with the wrist) is the expected metaphor there.
        if (dev && (m_state.mode == XR_ANCHOR_HEAD || m_state.mode == XR_ANCHOR_BODY)) {
            const Vec3 worldPos = poseCompose(*dev, m_grabOffset).pos;
            Quat       face     = lookAtNoRoll(worldPos, in.view.pos, m_lastWorld.rot);
            if (m_state.mode == XR_ANCHOR_BODY)
                face = qFromYaw(qYawOf(face, m_lastYaw));
            m_grabOffset.rot = qMul(qInverse(dev->rot), face);
        }

        // WP-G6: optional 1€ low-pass on the carried pose (hands only, opt-in). Filtering means we
        // can no longer use the runtime's zero-latency device-space late-latch — the smoothed pose no
        // longer equals a rigid device-space offset — so submit the FILTERED world pose in LOCAL_FLOOR
        // instead (§5.4 trade-off: +~1 frame latency, −jitter). Controllers and filter-off keep the
        // exact device-space path below. m_lastWorld becomes the filtered pose, so the WP-G4 release
        // ring records what the user actually saw and the release re-anchors to match.
        SXRPose world = dev ? poseCompose(*dev, m_grabOffset) : m_lastWorld;
        // WP-G6 + live-tune 2026-07-09: filter hands always; filter controllers too when
        // grabFilterScopeAll (openxr:grab_filter_scope=all, the default). The filtered branch drops
        // the device-space late-latch (submits LOCAL_FLOOR), so enabling controllers here applies
        // that same ~1-frame-latency trade to them — deliberate, to kill controller carry jitter.
        if (in.grabFilter && (m_grabHandActive || in.grabFilterScopeAll) && dev) {
            world     = oneEuroStepPose(m_carryFilter, world, dt, in.grabFilterMinCutoff, in.grabFilterBeta);
            res.space = XR_SPACE_LOCAL_FLOOR;
            res.pose  = world;
        } else
            res.pose = m_grabOffset;
        res.worldPose  = world;
        m_lastWorld    = world;
        m_hasLastWorld = true;
        return res;
    }

    // Gaze carry override (research/16 §4.1): place the quad on the gaze ray at m_gazeDist, facing
    // the viewer, refreshed every frame. Placed AFTER the hand-grab override so a hand grab wins if
    // both somehow fire (the hand is the more explicit gesture); the manager also enforces mutual
    // exclusion at begin (a gaze grab is refused on a hand-grabbed monitor and vice-versa). The gaze
    // source is in.gaze when present (future eye path, §2.4) else the VIEW forward vector. Submitted
    // in LOCAL_FLOOR (no controller action space to late-latch — like the WP-G6 filtered branch).
    if (m_gazeGrabbed) {
        const SXRPose gazePose = in.gaze.value_or(in.view);
        Vec3          dir      = m_gazeFollow ? poseForward(gazePose.rot) : m_gazeFrozenDir;
        const float   dl       = dir.length();
        dir                    = dl > 1e-5F ? dir / dl : Vec3{0.F, 0.F, -1.F};
        const Vec3 origin      = m_gazeFollow ? gazePose.pos : m_gazeFrozenOrigin;
        const Vec3 P           = origin + dir * m_gazeDist;
        const Quat R           = lookAtNoRoll(P, in.view.pos, m_lastWorld.rot);

        res.space      = XR_SPACE_LOCAL_FLOOR;
        res.pose       = SXRPose{P, R};
        res.worldPose  = res.pose;
        m_lastWorld    = res.pose;
        m_hasLastWorld = true;
        return res;
    }

    // Adaptive anchoring decorator (research/13 §4.1): a LOCAL anchor that adaptively picks up and
    // follows the head, then re-docks. Runs only when enabled and not grabbed (the grab override
    // above returned first — a grab always wins over adaptive, §5.1). Short-circuits the mode switch;
    // it always submits a LOCAL_FLOOR world pose. A gaze carry (above) also short-circuits it, so a
    // gaze-carried adaptive monitor is never geofenced mid-carry (research/16 §4.5).
    if (m_state.adaptive.enabled) {
        const SXRPose W = adaptiveStep(in, tune);
        res.space       = XR_SPACE_LOCAL_FLOOR;
        res.pose        = W;
        res.worldPose   = W;
        m_lastWorld     = W;
        m_hasLastWorld  = true;
        return res;
    }

    switch (m_state.mode) {
        case XR_ANCHOR_LOCAL: {
            res.space     = XR_SPACE_LOCAL_FLOOR;
            res.pose      = m_state.anchorPose;
            res.worldPose = m_state.anchorPose;
            break;
        }

        case XR_ANCHOR_HEAD:
        case XR_ANCHOR_BODY: {
            res.space     = XR_SPACE_LOCAL_FLOOR;
            res.pose      = solveLeash(m_state.mode, m_state.anchorPose, in, tune, false);
            res.worldPose = res.pose;
            break;
        }

        case XR_ANCHOR_DEVICE: {
            const std::optional<SXRPose>& grip = m_state.device == XR_HAND_LEFT ? in.gripLeft : in.gripRight;
            if (grip) {
                if (m_deviceOffsetDirty && m_hasLastWorld) {
                    m_state.anchorPose  = poseCompose(poseInverse(*grip), m_lastWorld);
                    m_deviceOffsetDirty = false;
                }
                res.space     = m_state.device == XR_HAND_LEFT ? XR_SPACE_GRIP_LEFT : XR_SPACE_GRIP_RIGHT;
                res.pose      = m_state.anchorPose;
                res.worldPose = poseCompose(*grip, m_state.anchorPose);
            } else {
                // tracking loss: park in the world at the last composed pose (§3.4).
                res.space = XR_SPACE_LOCAL_FLOOR;
                if (m_hasLastWorld)
                    res.pose = m_lastWorld;
                else {
                    SXRVerbContext ctx;
                    ctx.view      = in.view;
                    ctx.viewValid = true;
                    res.pose      = centerPlacement(ctx, tune.defaultDistance);
                }
                res.worldPose = res.pose;
            }
            break;
        }
    }

    m_lastWorld    = res.worldPose;
    m_hasLastWorld = true;
    return res;
}

// ---- head/body leash solve (shared by the normal path + adaptive roam, research/13 §4.3) ----

SXRPose CXRAnchor::solveLeash(eXRAnchorMode mode, const SXRPose& O, const SXRSolveInput& in, const SXRAnchorTuning& tune, bool seedAtTarget) {
    const float dt = std::clamp(in.dt, 0.F, XR_SOLVE_DT_MAX);

    if (mode == XR_ANCHOR_HEAD) {
        const SXRPose T = poseCompose(in.view, O);

        if (!m_springInit) {
            m_springPos   = (seedAtTarget || !m_hasLastWorld) ? T.pos : m_lastWorld.pos;
            m_springVel   = Vec3{};
            m_smoothedRot = lookAtNoRoll(m_springPos, in.view.pos, O.rot);
            m_chasing     = false;
            m_springInit  = true;
        }

        const Vec3& h      = in.view.pos;
        const Vec3  dCur   = (m_springPos - h);
        const Vec3  dTgt   = (T.pos - h);
        float       angDev = 0.F;
        if (dCur.length() > 1e-4F && dTgt.length() > 1e-4F)
            angDev = std::acos(std::clamp(dCur.normalized().dot(dTgt.normalized()), -1.F, 1.F));
        const float posDev = (m_springPos - T.pos).length();

        if (!m_chasing && (angDev > tune.deadzoneAngleRad || posDev > tune.deadzoneDistance))
            m_chasing = true;

        if (m_chasing) {
            springStep(m_springPos, m_springVel, T.pos, tune.leashResponse, dt);
            if ((m_springPos - T.pos).length() < XR_LEASH_SETTLE_POS) {
                m_chasing   = false;
                m_springVel = Vec3{};
            }
        } else
            m_springVel = Vec3{};

        const Quat  R_target = lookAtNoRoll(m_springPos, in.view.pos, m_smoothedRot);
        const float alpha    = 1.F - std::exp(-dt / tune.leashResponse);
        m_smoothedRot        = qSlerp(m_smoothedRot, R_target, alpha);
        return SXRPose{m_springPos, m_smoothedRot};
    }

    // XR_ANCHOR_BODY
    if (!m_bodyHeightCaptured) {
        m_state.bodyHeight   = in.view.pos.y;
        m_bodyHeightCaptured = true;
    }
    const SXRPose bodyFrame = computeBodyFrame(in.view, tune, true);
    const SXRPose T         = poseCompose(bodyFrame, O);

    if (!m_springInit) {
        m_springPos   = (seedAtTarget || !m_hasLastWorld) ? T.pos : m_lastWorld.pos;
        m_springVel   = Vec3{};
        m_smoothedRot = qFromYaw(qYawOf(qMul(bodyFrame.rot, O.rot), m_lastYaw));
        m_chasing     = false;
        m_springInit  = true;
    }

    const float posDev = (m_springPos - T.pos).length();
    const float angDev = std::fabs(wrapPi(qYawOf(m_smoothedRot, m_lastYaw) - qYawOf(T.rot, m_lastYaw)));

    if (!m_chasing && (posDev > tune.deadzoneDistance || angDev > tune.deadzoneAngleRad))
        m_chasing = true;

    if (m_chasing) {
        springStep(m_springPos, m_springVel, T.pos, tune.leashResponse, dt);
        if ((m_springPos - T.pos).length() < XR_LEASH_SETTLE_POS) {
            m_chasing   = false;
            m_springVel = Vec3{};
        }
    } else
        m_springVel = Vec3{};

    const float targetYaw = qYawOf(qMul(bodyFrame.rot, O.rot), m_lastYaw);
    const Quat  R_target  = qFromYaw(targetYaw);
    const float alpha     = 1.F - std::exp(-dt / tune.leashResponse);
    m_smoothedRot         = qSlerp(m_smoothedRot, R_target, alpha);
    return SXRPose{m_springPos, m_smoothedRot};
}

// ---- adaptive anchoring (research/13 §4) ----

SXRPose CXRAnchor::adaptiveRoamOffset(const SXRAnchorTuning& tune) const {
    if (m_adRoamRuntimeSet)
        return m_adRoamOffset; // carry-current-offset captured at undock
    if (m_state.adaptive.hasRoamOffset)
        return m_state.adaptive.roamOffset;
    // Default comfortable follow offset: straight ahead at the default distance. For HEAD this is a
    // view-space offset (y=0 keeps it at eye level); for BODY the y is relative to the captured body
    // height (≈ head height). solveLeash builds the look-at / yaw orientation, so rot stays identity.
    return SXRPose{Vec3{0.F, 0.F, -tune.defaultDistance}, Quat{0.F, 0.F, 0.F, 1.F}};
}

void CXRAnchor::captureCarryOffset(const SXRSolveInput& in, const SXRAnchorTuning& tune) {
    // Express the current desk world pose as an offset in the roam frame so the follower keeps its
    // head-relative placement from the desk (research/13 §4.4). Ephemeral (runtime), cleared on redock.
    const SXRPose W = m_state.anchorPose;
    if (effectiveRoamMode(tune) == XR_ANCHOR_HEAD)
        m_adRoamOffset = poseCompose(poseInverse(in.view), W);
    else {
        const SXRPose bodyFrame = computeBodyFrame(in.view, tune, false);
        m_adRoamOffset          = poseCompose(poseInverse(bodyFrame), W);
        m_adRoamOffset.rot      = qFromYaw(qYawOf(m_adRoamOffset.rot, 0.F));
    }
    m_adRoamRuntimeSet = true;
}

void CXRAnchor::beginUndock(const SXRPose& fromPose) {
    m_adFrom            = fromPose;
    m_adT               = 0.F;
    m_adPhase           = XRAD_UNDOCKING;
    m_outDwell          = 0.F;
    m_inDwell           = 0.F;
    m_springInit         = false; // reseed the roam spring at its target -> no kick on hand-off
    m_seedLeashAtTarget  = true;
    m_bodyHeightCaptured = false; // recapture the comfortable roam height now (no-op for head roam)
}

void CXRAnchor::beginRedock(const SXRPose& fromPose) {
    m_adFrom   = fromPose;
    m_adT      = 0.F;
    m_adPhase  = XRAD_REDOCKING;
    m_outDwell = 0.F;
    m_inDwell  = 0.F;
}

SXRPose CXRAnchor::adaptiveStep(const SXRSolveInput& in, const SXRAnchorTuning& tune) {
    const float                dt  = std::clamp(in.dt, 0.F, XR_SOLVE_DT_MAX);
    const SXRAdaptiveConfig&   cfg = m_state.adaptive;

    // Effective geofence radii: per-monitor override (>=0) else the global tuning.
    const float         rOut      = cfg.leaveRadius >= 0.F ? cfg.leaveRadius : tune.adLeaveRadius;
    const float         rIn       = cfg.returnRadius >= 0.F ? cfg.returnRadius : tune.adReturnRadius;
    const eXRAnchorMode roamMode  = effectiveRoamMode(tune);
    const SXRPose       roamOff   = adaptiveRoamOffset(tune);
    const bool          effCarry  = cfg.carryOverride ? cfg.carryOffset : tune.adCarryOffset;
    const SXRPose       dockPose  = m_state.anchorPose; // the persistent desk pose (LOCAL identity)

    // Seat capture: the head position the instant the monitor is DOCKED with a valid view (§3.1).
    if (!m_dockSeatCaptured && m_adPhase == XRAD_DOCKED) {
        m_dockHeadPos      = in.view.pos;
        m_dockSeatCaptured = true;
    }
    // Geofence distance from the desk seat (XZ by default so standing up doesn't undock, §3.1).
    const float d    = m_dockSeatCaptured ? (tune.adUseHeight ? dist3(in.view.pos, m_dockHeadPos) : horizDistXZ(in.view.pos, m_dockHeadPos)) : 0.F;
    m_adSeatDist     = d;
    const bool  gate = m_dockSeatCaptured; // don't transition until a seat exists

    // Latch "the user has actually left the desk since docking" once the head crosses R_out. Auto-
    // redock (and the undock->redock reverse) is gated on this, so a manual/forced undock while still
    // sitting at the desk stays roaming instead of instantly snapping back (the geofence would
    // otherwise see d<R_in and re-dock). A real walk-out sets it naturally (d>R_out triggered undock).
    if (gate && d > rOut)
        m_adLeftSinceDock = true;
    const bool wantReturn = gate && m_adLeftSinceDock;

    SXRPose W;
    switch (m_adPhase) {
        case XRAD_DOCKED: {
            W = dockPose;
            if (gate && dwellStep(d > rOut, m_outDwell, dt, tune.adLeaveDwell)) {
                if (effCarry)
                    captureCarryOffset(in, tune);
                beginUndock(W);
            }
            break;
        }
        case XRAD_UNDOCKING: {
            m_adT               = envAdvance(m_adT, dt, tune.adTransition);
            const SXRPose to    = solveLeash(roamMode, roamOff, in, tune, m_seedLeashAtTarget);
            m_seedLeashAtTarget = false;
            W                   = lerpPose(m_adFrom, to, easeApply(tune.adEase, m_adT));
            // Reverse interrupt: returned inside R_in for T_in — head back to the desk, keep progress.
            if (wantReturn && dwellStep(d < rIn, m_inDwell, dt, tune.adReturnDwell)) {
                m_adFrom   = W;
                m_adT      = 1.F - m_adT;
                m_adPhase  = XRAD_REDOCKING;
                m_outDwell = 0.F;
            } else if (m_adT >= 1.F) {
                m_adPhase = XRAD_ROAMING;
                m_inDwell = 0.F;
            }
            break;
        }
        case XRAD_ROAMING: {
            W = solveLeash(roamMode, roamOff, in, tune, false);
            if (wantReturn && dwellStep(d < rIn, m_inDwell, dt, tune.adReturnDwell))
                beginRedock(W);
            break;
        }
        case XRAD_REDOCKING: {
            m_adT = envAdvance(m_adT, dt, tune.adTransition);
            W     = lerpPose(m_adFrom, dockPose, easeApply(tune.adEase, m_adT));
            // Reverse interrupt: left again past R_out for T_out — peel back off toward roam.
            if (gate && dwellStep(d > rOut, m_outDwell, dt, tune.adLeaveDwell)) {
                m_adFrom            = W;
                m_adT               = 1.F - m_adT;
                m_adPhase           = XRAD_UNDOCKING;
                m_inDwell           = 0.F;
                m_springInit        = false; // reseed the roam spring
                m_seedLeashAtTarget = true;
                if (roamMode == XR_ANCHOR_BODY)
                    m_bodyHeightCaptured = false;
            } else if (m_adT >= 1.F) {
                m_adPhase          = XRAD_DOCKED;
                m_state.anchorPose = dockPose; // reassert the exact desk pose
                m_outDwell         = 0.F;
                m_adRoamRuntimeSet = false; // drop any carry offset; recapture on the next undock
                m_adLeftSinceDock  = false; // re-arm the "left the desk" latch for the next cycle
            }
            break;
        }
    }
    return W;
}

void CXRAnchor::adaptiveSetEnabled(bool en) {
    m_state.adaptive.enabled = en;
    m_adPhase                = XRAD_DOCKED;
    m_adT                    = 0.F;
    m_outDwell               = 0.F;
    m_inDwell                = 0.F;
    m_dockSeatCaptured       = false; // recapture on the next valid-view solve
    m_adRoamRuntimeSet       = false;
    m_adLeftSinceDock        = false;
}

void CXRAnchor::adaptiveForceUndock() {
    if (!m_state.adaptive.enabled)
        return;
    if (m_adPhase == XRAD_ROAMING || m_adPhase == XRAD_UNDOCKING)
        return;
    beginUndock(m_hasLastWorld ? m_lastWorld : m_state.anchorPose);
}

void CXRAnchor::adaptiveForceDock() {
    if (!m_state.adaptive.enabled)
        return;
    if (m_adPhase == XRAD_DOCKED || m_adPhase == XRAD_REDOCKING)
        return;
    beginRedock(m_hasLastWorld ? m_lastWorld : m_state.anchorPose);
}

void CXRAnchor::adaptiveDockHere() {
    // Redefine the desk pose to the current displayed pose and recapture the seat next frame.
    if (m_hasLastWorld)
        m_state.anchorPose = m_lastWorld;
    m_adPhase          = XRAD_DOCKED;
    m_adT              = 0.F;
    m_outDwell         = 0.F;
    m_inDwell          = 0.F;
    m_dockSeatCaptured = false;
    m_adRoamRuntimeSet = false;
    m_adLeftSinceDock  = false;
}

void CXRAnchor::adaptiveSetRoamMode(eXRAnchorMode m) {
    if (m != XR_ANCHOR_HEAD && m != XR_ANCHOR_BODY)
        return;
    m_state.adaptive.roamMode    = m;
    m_state.adaptive.roamModeSet = true;
    // Reseed the leash for the new mode on the next roam frame (no kick).
    m_springInit         = false;
    m_bodyHeightCaptured = false;
    m_seedLeashAtTarget  = true;
}

// ---- grab pose math (§4) ----

void CXRAnchor::beginGrab(eXRHand hand, const SXRPose& deviceWorld, bool usePinch, bool handActive) {
    // Capture the quad's current displayed world pose relative to the grabbing hand's device
    // pose (§4.1). `deviceWorld` is the grip pose (controllers/grasp) or the pinch pose (WP-G5).
    m_grabOffset     = poseCompose(poseInverse(deviceWorld), m_lastWorld);
    m_grabbed        = true;
    m_grabHand       = hand;
    m_grabPinch      = usePinch;
    m_grabHandActive = handActive; // WP-G6: eligible for the 1€ carry filter
    m_carryFilter.reset();         // fresh smoothing each grab (no carry-over from a prior grab)

    // Adaptive: a grab suspends the geofence (the grab override returns before adaptiveStep). If a
    // transition is mid-flight, settle it to the nearer endpoint now so the release resolves cleanly
    // into a stable DOCKED/ROAMING representation (research/13 §5.1), and reset the dwell timers.
    if (m_state.adaptive.enabled) {
        if (m_adPhase == XRAD_UNDOCKING)
            m_adPhase = m_adT >= 0.5F ? XRAD_ROAMING : XRAD_DOCKED;
        else if (m_adPhase == XRAD_REDOCKING)
            m_adPhase = m_adT >= 0.5F ? XRAD_DOCKED : XRAD_ROAMING;
        m_adT      = 0.F;
        m_outDwell = 0.F;
        m_inDwell  = 0.F;
    }
}

void CXRAnchor::grabPushPull(float deltaMeters) {
    const float d    = m_grabOffset.pos.length();
    const Vec3  dir  = d < 1e-4F ? Vec3{0.F, 0.F, -1.F} : m_grabOffset.pos / d;
    const float dp   = std::clamp(d + deltaMeters, XR_DISTANCE_MIN, XR_DISTANCE_MAX);
    m_grabOffset.pos = dir * dp;
}

void CXRAnchor::grabResize(float deltaMeters) {
    m_state.widthMeters = std::clamp(m_state.widthMeters + deltaMeters, XR_WIDTH_MIN, XR_WIDTH_MAX);
}

// ---- corner resize grab (WP-G3) ----

void CXRAnchor::beginResize(eXRHand hand, eXRQuadRegion corner, const SXRPose& gripWorld, float aspectHW) {
    if (!m_hasLastWorld)
        return; // nothing displayed to resize from; caller gates on hasLastWorld()

    m_grabHand     = hand;
    m_resizeAspect = aspectHW > 0.F ? aspectHW : 1.F;

    const SXRPose& P  = m_lastWorld; // displayed CONTENT center pose (this frame's solve)
    const float    w0 = m_state.widthMeters;
    const float    h0 = w0 * m_resizeAspect;

    float sx = 1.F, sy = 1.F;
    xrCornerSigns(corner, sx, sy);
    const Vec3 right = qRotate(P.rot, Vec3{1.F, 0.F, 0.F});
    const Vec3 up    = qRotate(P.rot, Vec3{0.F, 1.F, 0.F});

    // Grabbed corner and its diagonally-opposite (pinned) corner, in world.
    const Vec3 grabbedCorner = P.pos + right * (sx * w0 * 0.5F) + up * (sy * h0 * 0.5F);
    const Vec3 pin           = P.pos - right * (sx * w0 * 0.5F) - up * (sy * h0 * 0.5F);
    const Vec3 diag          = grabbedCorner - pin;
    const float L0           = diag.length();

    m_resizeSx       = sx;
    m_resizeSy       = sy;
    m_resizePin      = pin;
    m_resizeDiagUnit = L0 > 1e-5F ? diag / L0 : right;
    m_resizeL0       = L0 > 1e-5F ? L0 : 1e-5F;
    m_resizeW0       = w0;
    m_resizeGrip0    = gripWorld;
    m_resizeRot      = P.rot;
    m_resizing       = true;
}

void CXRAnchor::grabResizeCorner(const SXRPose& gripWorld, const SXRSolveInput& in, const SXRAnchorTuning& tune) {
    if (!m_resizing)
        return;

    // Project the hand's motion since grab-start onto the fixed content diagonal: outward (away from
    // the pinned corner) grows, inward shrinks. Width scales with the diagonal; aspect is fixed.
    const float proj = (gripWorld.pos - m_resizeGrip0.pos).dot(m_resizeDiagUnit);
    float       wNew = m_resizeW0 * ((m_resizeL0 + proj) / m_resizeL0);
    wNew             = std::clamp(wNew, XR_WIDTH_MIN, XR_WIDTH_MAX);
    const float Lnew = m_resizeL0 * (wNew / m_resizeW0); // clamp-consistent diagonal

    // Opposite corner stays at m_resizePin; the content center is the diagonal midpoint.
    const Vec3 centerNew = m_resizePin + m_resizeDiagUnit * (Lnew * 0.5F);

    m_state.widthMeters = wNew;

    SXRVerbContext ctx;
    ctx.view      = in.view;
    ctx.viewValid = true;
    ctx.gripLeft  = in.gripLeft;
    ctx.gripRight = in.gripRight;
    // Re-express the resized content pose into the persistent mode. For LOCAL this pins the opposite
    // corner exactly in world; for head/body/device it re-seeds the anchor offset (and the spring)
    // at the resized pose so the leash follows the size change instead of fighting it (doc note in
    // XRAnchor.hpp). Orientation is held at the grab-start value for a stable diagonal.
    reanchorFromWorld(SXRPose{centerNew, m_resizeRot}, ctx, tune);
}

void CXRAnchor::endResize(const SXRPose& gripWorldLatched, const SXRSolveInput& in, const SXRAnchorTuning& tune) {
    // Final size/position from the LATCHED grip (WP-G4 ring) — rejects the release-frame jerk on the
    // size just as the move path rejects it on the pose. Then drop out of resize back to normal solve.
    grabResizeCorner(gripWorldLatched, in, tune);
    m_resizing = false;
}

void CXRAnchor::endGrab(const SXRSolveInput& in, const SXRAnchorTuning& tune) {
    // Release-frame pose (unlatched): device ∘ offset this frame == m_lastWorld from the solve.
    // The device pose is the pinch pose for a pinch-anchored grab (WP-G5), else the grip pose.
    const bool                    left = m_grabHand == XR_HAND_LEFT;
    const std::optional<SXRPose>& dev  = m_grabPinch ? (left ? in.pinchLeft : in.pinchRight) : (left ? in.gripLeft : in.gripRight);
    const SXRPose                 W    = dev ? poseCompose(*dev, m_grabOffset) : m_lastWorld;
    endGrab(W, in, tune);
}

void CXRAnchor::endGrab(const SXRPose& releaseWorld, const SXRSolveInput& in, const SXRAnchorTuning& tune) {
    m_grabbed   = false;
    m_grabPinch = false;

    SXRVerbContext ctx;
    ctx.view      = in.view;
    ctx.viewValid = true;
    ctx.gripLeft  = in.gripLeft;
    ctx.gripRight = in.gripRight;
    reanchorFromWorld(releaseWorld, ctx, tune);
}

bool CXRAnchor::abortGrab(const SXRSolveInput& in, const SXRAnchorTuning& tune) {
    if (!m_grabbed && !m_resizing)
        return false;

    // A RESIZE grab needs nothing but the flag cleared, and must NOT be re-anchored: it never
    // device-locked the quad (solve keeps running the persistent mode), and grabResizeCorner
    // already re-anchored at the dragged size on every frame of the drag. m_lastWorld is one solve
    // BEHIND that (the frame loop solves before it runs the grab machine), so re-anchoring from it
    // would rewind the last drag step and unpin the corner the resize was holding.
    const bool WASMOVE = m_grabbed;
    m_grabbed          = false;
    m_grabPinch        = false;
    m_resizing         = false;

    // A MOVE grab is a device lock, so the quad's persistent state is still the PRE-grab pose and
    // dropping the flag alone would teleport it back. m_lastWorld is this frame's composed
    // device ∘ offset pose — the one the user is looking at — so re-express that into the
    // persistent mode exactly as endGrab/endGazeGrab do. There is deliberately no release-latch
    // ring here: the hand never released, so there is no release jerk to reject.
    if (!WASMOVE || !m_hasLastWorld)
        return true;

    SXRVerbContext ctx;
    ctx.view      = in.view;
    ctx.viewValid = true;
    ctx.gripLeft  = in.gripLeft;
    ctx.gripRight = in.gripRight;
    reanchorFromWorld(m_lastWorld, ctx, tune);
    return true;
}

// ---- gaze carry (research/16 §4) ----

void CXRAnchor::beginGazeGrab(const SXRPose& view, bool follow) {
    // Snapshot the distance head->quad so the quad does not jump on grab (research/16 §4.2). Use
    // m_lastWorld (the displayed pose) rather than the raw gaze ray so it grabs exactly where the
    // monitor currently sits even if the ray is a hair off-centre.
    const Vec3  toQuad = m_lastWorld.pos - view.pos;
    const float d      = toQuad.length();
    m_gazeDist         = std::clamp(d, XR_DISTANCE_MIN, XR_DISTANCE_MAX);
    m_gazeFrozenOrigin = view.pos;
    m_gazeFrozenDir    = d > 1e-5F ? toQuad / d : poseForward(view.rot);
    m_gazeFollow       = follow;
    m_gazeGrabbed      = true;

    // Adaptive: a gaze carry suspends the geofence (the override returns before adaptiveStep). Settle
    // any mid-flight transition to the nearer endpoint + reset the dwell timers, exactly as a hand
    // grab does (research/13 §5.1), so release resolves into a stable DOCKED/ROAMING representation.
    if (m_state.adaptive.enabled) {
        if (m_adPhase == XRAD_UNDOCKING)
            m_adPhase = m_adT >= 0.5F ? XRAD_ROAMING : XRAD_DOCKED;
        else if (m_adPhase == XRAD_REDOCKING)
            m_adPhase = m_adT >= 0.5F ? XRAD_DOCKED : XRAD_ROAMING;
        m_adT      = 0.F;
        m_outDwell = 0.F;
        m_inDwell  = 0.F;
    }
}

void CXRAnchor::gazePushPull(float deltaMeters) {
    m_gazeDist = std::clamp(m_gazeDist + deltaMeters, XR_DISTANCE_MIN, XR_DISTANCE_MAX);
}

void CXRAnchor::gazeSetDist(float absMeters) {
    m_gazeDist = std::clamp(absMeters, XR_DISTANCE_MIN, XR_DISTANCE_MAX);
}

void CXRAnchor::endGazeGrab(const SXRSolveInput& in, const SXRAnchorTuning& tune) {
    m_gazeGrabbed = false;

    SXRVerbContext ctx;
    ctx.view      = in.view;
    ctx.viewValid = true;
    ctx.gripLeft  = in.gripLeft;
    ctx.gripRight = in.gripRight;
    // Quad stays exactly where the user let it go, re-expressed into its persistent mode + spring
    // reseeded (identical to endGrab; no release-latch ring — keyboard release, no lurch, §4.2).
    reanchorFromWorld(m_lastWorld, ctx, tune);
}

void CXRAnchor::reanchorRoam(const SXRPose& W, const SXRVerbContext& ctx, const SXRAnchorTuning& tune) {
    // Re-express a released world pose into the roam frame, updating the (runtime) roam offset and
    // re-seeding the leash spring so ROAM continues from where the user let go (research/13 §5.1).
    const eXRAnchorMode roamMode = effectiveRoamMode(tune);
    if (roamMode == XR_ANCHOR_HEAD) {
        if (ctx.viewValid)
            m_adRoamOffset = poseCompose(poseInverse(ctx.view), W);
        m_smoothedRot = lookAtNoRoll(W.pos, ctx.view.pos, W.rot);
    } else {
        const SXRPose bodyFrame = computeBodyFrame(ctx.view, tune, false);
        m_adRoamOffset          = poseCompose(poseInverse(bodyFrame), W);
        m_adRoamOffset.rot      = qFromYaw(qYawOf(m_adRoamOffset.rot, 0.F));
        m_smoothedRot           = qFromYaw(qYawOf(qMul(bodyFrame.rot, m_adRoamOffset.rot), m_lastYaw));
    }
    m_adRoamRuntimeSet = true;
    m_springPos        = W.pos;
    m_springVel        = Vec3{};
    m_chasing          = false;
    m_springInit       = true;
}

void CXRAnchor::reanchorFromWorld(const SXRPose& W, const SXRVerbContext& ctx, const SXRAnchorTuning& tune) {
    // Adaptive + roaming: the persistent mode is LOCAL (the desk pose), but a release while roaming
    // must update the ROAM offset, not the desk pose (§5.1). Everything else keeps LOCAL semantics.
    if (m_state.adaptive.enabled && (m_adPhase == XRAD_ROAMING || m_adPhase == XRAD_UNDOCKING)) {
        reanchorRoam(W, ctx, tune);
        return;
    }
    switch (m_state.mode) {
        case XR_ANCHOR_LOCAL: {
            m_state.anchorPose = W;
            break;
        }
        case XR_ANCHOR_HEAD: {
            if (ctx.viewValid)
                m_state.anchorPose = poseCompose(poseInverse(ctx.view), W);
            m_springPos   = W.pos;
            m_springVel   = Vec3{};
            m_smoothedRot = lookAtNoRoll(W.pos, ctx.view.pos, W.rot);
            m_chasing     = false;
            m_springInit  = true;
            break;
        }
        case XR_ANCHOR_BODY: {
            const SXRPose bodyFrame = computeBodyFrame(ctx.view, tune, false);
            m_state.anchorPose      = poseCompose(poseInverse(bodyFrame), W);
            m_state.anchorPose.rot  = qFromYaw(qYawOf(m_state.anchorPose.rot, 0.F));
            const float targetYaw   = qYawOf(qMul(bodyFrame.rot, m_state.anchorPose.rot), m_lastYaw);
            m_springPos             = W.pos;
            m_springVel             = Vec3{};
            m_smoothedRot           = qFromYaw(targetYaw);
            m_chasing               = false;
            m_springInit            = true;
            break;
        }
        case XR_ANCHOR_DEVICE: {
            const std::optional<SXRPose>& grip = m_state.device == XR_HAND_LEFT ? ctx.gripLeft : ctx.gripRight;
            if (grip) {
                m_state.anchorPose  = poseCompose(poseInverse(*grip), W);
                m_deviceOffsetDirty = false;
            } else {
                m_lastWorld         = W;
                m_hasLastWorld      = true;
                m_deviceOffsetDirty = true; // recompute the offset on the first valid grip frame
            }
            break;
        }
    }
}

// ---- verbs (§5) ----

bool CXRAnchor::applyMove(const Vec3& d, const SXRVerbContext& ctx) {
    if (!ctx.viewValid)
        return false;
    const Vec3 D_view  = Vec3{d.x, d.y, -d.z}; // dz > 0 = away = view -Z
    const Vec3 D_world = qRotate(ctx.view.rot, D_view);

    switch (m_state.mode) {
        case XR_ANCHOR_LOCAL: m_state.anchorPose.pos += D_world; break;
        case XR_ANCHOR_HEAD:
            m_state.anchorPose.pos += D_view;
            m_chasing = true;
            break;
        case XR_ANCHOR_BODY: {
            const SXRPose bodyFrame = computeBodyFrame(ctx.view, {}, false);
            m_state.anchorPose.pos += qRotate(qInverse(bodyFrame.rot), D_world);
            m_chasing = true;
            break;
        }
        case XR_ANCHOR_DEVICE: {
            const std::optional<SXRPose>& grip = m_state.device == XR_HAND_LEFT ? ctx.gripLeft : ctx.gripRight;
            if (!grip)
                return false;
            m_state.anchorPose.pos += qRotate(qInverse(grip->rot), D_world);
            break;
        }
    }
    return true;
}

bool CXRAnchor::applyRotate(float dyawRad, float dpitchRad, const SXRVerbContext& /*ctx*/) {
    switch (m_state.mode) {
        case XR_ANCHOR_LOCAL:
        case XR_ANCHOR_DEVICE:
            // rotate in place: yaw in the parent frame (pre-mul), pitch about own X (post-mul).
            m_state.anchorPose.rot = qMul(qFromYaw(dyawRad), m_state.anchorPose.rot);
            m_state.anchorPose.rot = qMul(m_state.anchorPose.rot, qFromPitch(dpitchRad));
            break;
        case XR_ANCHOR_HEAD:
            // orbit around the head (display orientation is lookAt-driven).
            m_state.anchorPose.pos = qRotate(qFromYaw(dyawRad), qRotate(qFromPitch(dpitchRad), m_state.anchorPose.pos));
            m_chasing              = true;
            break;
        case XR_ANCHOR_BODY:
            m_state.anchorPose.pos = qRotate(qFromYaw(dyawRad), qRotate(qFromPitch(dpitchRad), m_state.anchorPose.pos));
            m_state.anchorPose.rot = qMul(qFromYaw(dyawRad), m_state.anchorPose.rot);
            m_chasing              = true;
            break;
    }
    return true;
}

bool CXRAnchor::applyScale(bool isDelta, float f) {
    m_state.widthMeters = std::clamp(isDelta ? m_state.widthMeters + f : m_state.widthMeters * f, XR_WIDTH_MIN, XR_WIDTH_MAX);
    return true;
}

bool CXRAnchor::applyDistance(float dMeters, const SXRVerbContext& ctx) {
    if (!ctx.viewValid || !m_hasLastWorld)
        return false;
    const Vec3  d   = m_lastWorld.pos - ctx.view.pos;
    const float len = d.length();
    if (len < 1e-4F)
        return false;
    const Vec3  dir  = d / len;
    const float lenP = std::clamp(len + dMeters, XR_DISTANCE_MIN, XR_DISTANCE_MAX);
    const Vec3  Pp   = ctx.view.pos + dir * lenP;

    switch (m_state.mode) {
        case XR_ANCHOR_LOCAL: m_state.anchorPose.pos = Pp; break;
        case XR_ANCHOR_HEAD:
            m_state.anchorPose.pos *= lenP / len; // view-space offset IS the head ray
            m_chasing = true;
            break;
        case XR_ANCHOR_BODY: {
            const SXRPose bodyFrame = computeBodyFrame(ctx.view, {}, false);
            m_state.anchorPose.pos  = qRotate(qInverse(bodyFrame.rot), Pp - bodyFrame.pos);
            m_chasing               = true;
            break;
        }
        case XR_ANCHOR_DEVICE: {
            const std::optional<SXRPose>& grip = m_state.device == XR_HAND_LEFT ? ctx.gripLeft : ctx.gripRight;
            if (!grip)
                return false;
            m_state.anchorPose.pos = qRotate(qInverse(grip->rot), Pp - grip->pos);
            break;
        }
    }
    return true;
}

bool CXRAnchor::applyCenter(const SXRVerbContext& ctx, float defaultDistance) {
    if (!ctx.viewValid)
        return false;
    const SXRPose Wp = centerPlacement(ctx, defaultDistance);

    switch (m_state.mode) {
        case XR_ANCHOR_LOCAL: m_state.anchorPose = Wp; break;
        case XR_ANCHOR_HEAD:
            m_state.anchorPose = SXRPose{Vec3{0.F, 0.F, -defaultDistance}, identityQuat()};
            m_chasing          = true; // glide, don't warp
            break;
        case XR_ANCHOR_BODY: {
            const SXRPose bodyFrame = computeBodyFrame(ctx.view, {}, false);
            m_state.anchorPose      = poseCompose(poseInverse(bodyFrame), Wp);
            m_state.anchorPose.rot  = qFromYaw(qYawOf(m_state.anchorPose.rot, 0.F));
            m_chasing               = true;
            break;
        }
        case XR_ANCHOR_DEVICE: {
            const std::optional<SXRPose>& grip = m_state.device == XR_HAND_LEFT ? ctx.gripLeft : ctx.gripRight;
            if (!grip)
                return false;
            m_state.anchorPose = poseCompose(poseInverse(*grip), Wp);
            break;
        }
    }
    return true;
}

// ---- explicit local placement (hypxrvoice GAP 2) ----

void CXRAnchor::placeLocalAt(const SXRPose& W) {
    m_state.mode       = XR_ANCHOR_LOCAL;
    m_state.anchorPose = W;

    // A teleport ends any active grab/carry (their overrides live in separate state that would
    // otherwise fight the new pose) and resets the adaptive phase — W is the new docked desk pose.
    m_grabbed          = false;
    m_grabPinch        = false;
    m_grabHandActive   = false;
    m_gazeGrabbed      = false;
    m_resizing         = false;
    m_carryFilter.reset();
    m_adPhase          = XRAD_DOCKED;
    m_dockSeatCaptured = false;
    m_outDwell         = 0.F;
    m_inDwell          = 0.F;
    m_adT              = 0.F;

    // Warp: reseed the spring + last-world so the next solve lands exactly at W with no one-frame
    // chase from the old pose (same seeding as recenterLocalToHead's re-seat tail).
    m_springInit   = false;
    m_chasing      = false;
    m_springVel    = Vec3{};
    m_lastWorld    = W;
    m_hasLastWorld = true;
}

// ---- mode transition (§5.6) ----

bool CXRAnchor::setMode(eXRAnchorMode newMode, eXRHand hand, const SXRVerbContext& ctx, const SXRAnchorTuning& tune) {
    // Take the current displayed world pose (or a center placement if none) and re-express it
    // in the new mode's representation — the quad must not move.
    const SXRPose W = m_hasLastWorld ? m_lastWorld : centerPlacement(ctx, tune.defaultDistance);

    // head/body conversions need the view; device conversions need the anchor hand's grip.
    if ((newMode == XR_ANCHOR_HEAD || newMode == XR_ANCHOR_BODY) && !ctx.viewValid)
        return false;
    if (newMode == XR_ANCHOR_DEVICE) {
        const std::optional<SXRPose>& grip = hand == XR_HAND_LEFT ? ctx.gripLeft : ctx.gripRight;
        if (!grip)
            return false;
    }

    m_state.mode = newMode;
    if (newMode == XR_ANCHOR_DEVICE)
        m_state.device = hand;
    if (newMode == XR_ANCHOR_BODY)
        m_bodyHeightCaptured = false; // recapture on next solve

    reanchorFromWorld(W, ctx, tune);
    return true;
}

// ---- recentering (§6) ----

void CXRAnchor::onReferenceSpaceChanged(const SXRPose& M) {
    // A pose P_old in old coords re-expressed in new coords: P_new = inv(M) ∘ P_old.
    const SXRPose invM = poseInverse(M);

    switch (m_state.mode) {
        case XR_ANCHOR_LOCAL:
            // Keep the quad physically where it is across a recenter.
            m_state.anchorPose = poseCompose(invM, m_state.anchorPose);
            break;
        case XR_ANCHOR_HEAD:
        case XR_ANCHOR_DEVICE:
            // Offsets are relative to view/grip spaces (which move with the user): unaffected.
            break;
        case XR_ANCHOR_BODY:
            // bodyHeight is a stored LOCAL_FLOOR height: subtract the new origin's y.
            m_state.bodyHeight = poseCompose(invM, SXRPose{Vec3{0.F, m_state.bodyHeight, 0.F}, identityQuat()}).pos.y;
            break;
    }

    // Re-express cached solver state so there is no one-frame pop.
    if (m_hasLastWorld)
        m_lastWorld = poseCompose(invM, m_lastWorld);
    m_springPos   = poseCompose(invM, SXRPose{m_springPos, identityQuat()}).pos;
    m_springVel   = qRotate(qInverse(M.rot), m_springVel);
    m_smoothedRot = qMul(qConjugate(M.rot), m_smoothedRot);

    // Adaptive: the desk seat is a LOCAL_FLOOR position; a frozen transition `from` is a LOCAL_FLOOR
    // pose. Both must move with the recenter so the geofence + envelope stay physically consistent
    // (research/13 §5.6). The roam offset is view/body-frame relative -> unaffected (like HEAD/BODY).
    if (m_dockSeatCaptured)
        m_dockHeadPos = poseCompose(invM, SXRPose{m_dockHeadPos, identityQuat()}).pos;
    if (m_adPhase == XRAD_UNDOCKING || m_adPhase == XRAD_REDOCKING)
        m_adFrom = poseCompose(invM, m_adFrom);
}

void CXRAnchor::recenterLocalToHead(const SXRPose& view, const SXRAnchorState& declared) {
    // report-20 issue C. Only LOCAL monitors need re-seating; head/body/device offsets are already
    // expressed relative to the user's moving frames, so they land correctly on their own.
    if (m_state.mode != XR_ANCHOR_LOCAL)
        return;

    // Yaw-only head frame at the floor-projected head position. Building the frame at y=0 keeps the
    // declared offset's y as a floor-relative height (declared `pos:0,1.5,-1.5` -> still 1.5m above
    // the floor), while its XZ + facing are re-planted relative to where the user is now looking.
    const float   yaw = qYawOf(view.rot, 0.F);
    const SXRPose frame{Vec3{view.pos.x, 0.F, view.pos.z}, qFromYaw(yaw)};

    // Compose the ENTIRE declared rig (offset position AND orientation) into that frame. Because the
    // declared pose already encodes "in front of, and facing, a user at the origin looking down -Z",
    // the result is the monitor at the configured distance/height, in front of and facing the current
    // head. Passing the same `frame`-defining `view` for every monitor transforms the whole group
    // rigidly, so their relative arrangement is preserved.
    const SXRPose W    = poseCompose(frame, declared.anchorPose);
    m_state.anchorPose = W;

    // Warp there (this is a deliberate re-seat, not a glide): reseed the spring + last-world so the
    // next solve places the quad exactly at W with no one-frame chase from its stale far-away pose.
    m_springInit  = false;
    m_chasing     = false;
    m_springVel   = Vec3{};
    m_lastWorld   = W;
    m_hasLastWorld = true;

    // Adaptive: the re-seated pose becomes the new desk seat, docked, recaptured at the current head
    // on the next valid-view solve — the equivalent of `dock here` from where the user now stands.
    if (m_state.adaptive.enabled) {
        m_adPhase          = XRAD_DOCKED;
        m_adT              = 0.F;
        m_outDwell         = 0.F;
        m_inDwell          = 0.F;
        m_dockSeatCaptured = false;
        m_adRoamRuntimeSet = false;
        m_adLeftSinceDock  = false;
    }
}
