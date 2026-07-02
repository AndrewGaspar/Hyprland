#include "XRAnchor.hpp"

// Pure math — compiled unconditionally, no OpenXR headers (see the header). See
// docs/openxr/03-anchoring.md §3–§6 for every formula below.

#include <cmath>

using namespace OpenXR;

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

    // Grab override (§4.2): behave as device-locked to the grabbing hand.
    if (m_grabbed) {
        res.space                          = m_grabHand == XR_HAND_LEFT ? XR_SPACE_GRIP_LEFT : XR_SPACE_GRIP_RIGHT;
        res.pose                           = m_grabOffset;
        const std::optional<SXRPose>& grip = m_grabHand == XR_HAND_LEFT ? in.gripLeft : in.gripRight;
        res.worldPose                      = grip ? poseCompose(*grip, m_grabOffset) : m_lastWorld;
        m_lastWorld                        = res.worldPose;
        m_hasLastWorld                     = true;
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

void CXRAnchor::beginGrab(eXRHand hand, const SXRPose& gripWorld) {
    // Capture the quad's current displayed world pose relative to the grabbing hand (§4.1).
    m_grabOffset = poseCompose(poseInverse(gripWorld), m_lastWorld);
    m_grabbed    = true;
    m_grabHand   = hand;
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

void CXRAnchor::endGrab(const SXRSolveInput& in, const SXRAnchorTuning& tune) {
    const std::optional<SXRPose>& grip = m_grabHand == XR_HAND_LEFT ? in.gripLeft : in.gripRight;
    const SXRPose                 W    = grip ? poseCompose(*grip, m_grabOffset) : m_lastWorld;
    m_grabbed                          = false;

    SXRVerbContext ctx;
    ctx.view      = in.view;
    ctx.viewValid = true;
    ctx.gripLeft  = in.gripLeft;
    ctx.gripRight = in.gripRight;
    reanchorFromWorld(W, ctx, tune);
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
