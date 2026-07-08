#include "XRAnchor.hpp"

// Pure math — compiled unconditionally, no OpenXR headers (see the header). See
// docs/openxr/03-anchoring.md §3–§6 for every formula below.

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

SXRPose OpenXR::pickReleasePose(const SXRGrabRing& ring, uint32_t nowMs, uint32_t latencyMs, float velReject) {
    if (ring.size() == 0)
        return SXRPose{};
    if (velReject > 0.F && ring.size() >= 2 && ring.at(0).linSpeed > velReject)
        return ring.lastCalm(velReject, nowMs, XR_GRAB_MAX_REWIND_MS);
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
    res.heightMeters = m_state.widthMeters * (float)in.pxH / (float)(in.pxW ? in.pxW : 1);

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
        if (in.grabFilter && m_grabHandActive && dev) {
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

    switch (m_state.mode) {
        case XR_ANCHOR_LOCAL: {
            res.space     = XR_SPACE_LOCAL_FLOOR;
            res.pose      = m_state.anchorPose;
            res.worldPose = m_state.anchorPose;
            break;
        }

        case XR_ANCHOR_HEAD: {
            const SXRPose& O = m_state.anchorPose;
            const SXRPose  T = poseCompose(in.view, O);

            if (!m_springInit) {
                m_springPos   = m_hasLastWorld ? m_lastWorld.pos : T.pos;
                m_springVel   = Vec3{};
                m_smoothedRot = lookAtNoRoll(m_springPos, in.view.pos, m_state.anchorPose.rot);
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

            res.space     = XR_SPACE_LOCAL_FLOOR;
            res.pose      = SXRPose{m_springPos, m_smoothedRot};
            res.worldPose = res.pose;
            break;
        }

        case XR_ANCHOR_BODY: {
            if (!m_bodyHeightCaptured) {
                m_state.bodyHeight   = in.view.pos.y;
                m_bodyHeightCaptured = true;
            }
            const SXRPose  bodyFrame = computeBodyFrame(in.view, tune, true);
            const SXRPose& O         = m_state.anchorPose;
            const SXRPose  T         = poseCompose(bodyFrame, O);

            if (!m_springInit) {
                m_springPos   = m_hasLastWorld ? m_lastWorld.pos : T.pos;
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

            res.space     = XR_SPACE_LOCAL_FLOOR;
            res.pose      = SXRPose{m_springPos, m_smoothedRot};
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

void CXRAnchor::reanchorFromWorld(const SXRPose& W, const SXRVerbContext& ctx, const SXRAnchorTuning& tune) {
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
}
