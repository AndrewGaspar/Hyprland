#include "XRInput.hpp"
#ifdef HAVE_OPENXR

#include <openxr/openxr.h>

#include <cstring>
#include <vector>

#include <linux/input-event-codes.h> // BTN_LEFT

#include "XRSession.hpp"
#include "../debug/log/Logger.hpp"
#include "../config/ConfigValue.hpp"
#include "../helpers/time/Time.hpp"

using namespace OpenXR;

// ---------------------------------------------------------------------------------------------
// Small helpers.
// ---------------------------------------------------------------------------------------------

namespace {
    XrPath toPath(XrInstance instance, const char* str) {
        XrPath p = XR_NULL_PATH;
        if (XR_FAILED(xrStringToPath(instance, str, &p)))
            return XR_NULL_PATH;
        return p;
    }

    std::string fromPath(XrInstance instance, XrPath path) {
        if (path == XR_NULL_PATH)
            return "(none)";
        uint32_t len = 0;
        if (XR_FAILED(xrPathToString(instance, path, 0, &len, nullptr)) || len == 0)
            return "(unknown)";
        std::string out(len, '\0');
        if (XR_FAILED(xrPathToString(instance, path, len, &len, out.data())))
            return "(unknown)";
        if (!out.empty() && out.back() == '\0')
            out.pop_back();
        return out;
    }

    // Suggest one interaction profile's bindings. A profile the runtime does not know
    // (XR_ERROR_PATH_UNSUPPORTED) is logged and skipped — never fatal (doc 04 §1.4).
    bool suggestProfile(XrInstance instance, const char* profilePath, const std::vector<XrActionSuggestedBinding>& bindings) {
        XrPath profile = toPath(instance, profilePath);
        if (profile == XR_NULL_PATH) {
            Log::logger->log(Log::WARN, "[OPENXR] could not intern interaction profile path '{}'", profilePath);
            return false;
        }

        XrInteractionProfileSuggestedBinding sb = {XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        sb.interactionProfile                   = profile;
        sb.countSuggestedBindings               = (uint32_t)bindings.size();
        sb.suggestedBindings                    = bindings.data();

        XrResult r = xrSuggestInteractionProfileBindings(instance, &sb);
        if (XR_SUCCEEDED(r)) {
            Log::logger->log(Log::DEBUG, "[OPENXR] suggested bindings for {}", profilePath);
            return true;
        }
        // Unknown/unsupported profile: informational only.
        if (r == XR_ERROR_PATH_UNSUPPORTED)
            Log::logger->log(Log::DEBUG, "[OPENXR] interaction profile {} unsupported by the runtime; skipped", profilePath);
        else
            Log::logger->log(Log::WARN, "[OPENXR] xrSuggestInteractionProfileBindings({}) failed: {}", profilePath, (int)r);
        return false;
    }
}

// ---------------------------------------------------------------------------------------------
// init: action set, actions, bindings, action spaces, attach.
// ---------------------------------------------------------------------------------------------

bool CXRInput::init(CXRSession& session, bool hasHandInteraction) {
    m_instance           = session.m_instance;
    m_session            = session.m_session;
    m_hasHandInteraction = hasHandInteraction;

    if (m_instance == XR_NULL_HANDLE || m_session == XR_NULL_HANDLE) {
        Log::logger->log(Log::ERR, "[OPENXR] CXRInput::init called without a live instance/session");
        return false;
    }

    m_handPath[XR_HAND_LEFT]  = toPath(m_instance, "/user/hand/left");
    m_handPath[XR_HAND_RIGHT] = toPath(m_instance, "/user/hand/right");

    // 1. Action set.
    XrActionSetCreateInfo asInfo = {XR_TYPE_ACTION_SET_CREATE_INFO};
    strncpy(asInfo.actionSetName, "hyprland", XR_MAX_ACTION_SET_NAME_SIZE - 1);
    strncpy(asInfo.localizedActionSetName, "Hyprland", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
    asInfo.priority = 0;
    if (XR_FAILED(xrCreateActionSet(m_instance, &asInfo, &m_actionSet))) {
        Log::logger->log(Log::ERR, "[OPENXR] xrCreateActionSet failed");
        return false;
    }

    // 2. Actions (all hand-scoped: both subaction paths).
    auto makeAction = [&](XrAction& out, const char* name, const char* localized, XrActionType type) -> bool {
        XrActionCreateInfo ai = {XR_TYPE_ACTION_CREATE_INFO};
        strncpy(ai.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
        strncpy(ai.localizedActionName, localized, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
        ai.actionType          = type;
        ai.countSubactionPaths = 2;
        ai.subactionPaths      = m_handPath.data();
        if (XR_FAILED(xrCreateAction(m_actionSet, &ai, &out))) {
            Log::logger->log(Log::ERR, "[OPENXR] xrCreateAction('{}') failed", name);
            return false;
        }
        return true;
    };

    if (!makeAction(m_aimAction, "aim_pose", "Aim pose", XR_ACTION_TYPE_POSE_INPUT) || !makeAction(m_gripAction, "grip_pose", "Grip pose", XR_ACTION_TYPE_POSE_INPUT) ||
        !makeAction(m_selectAction, "select", "Select", XR_ACTION_TYPE_FLOAT_INPUT) || !makeAction(m_grabAction, "grab", "Grab", XR_ACTION_TYPE_FLOAT_INPUT) ||
        !makeAction(m_scrollAction, "scroll", "Scroll", XR_ACTION_TYPE_VECTOR2F_INPUT) || !makeAction(m_menuAction, "menu", "Menu", XR_ACTION_TYPE_BOOLEAN_INPUT) ||
        !makeAction(m_hapticAction, "haptic", "Haptic", XR_ACTION_TYPE_VIBRATION_OUTPUT))
        return false;

    // 3. Suggested bindings for every supported profile.
    if (!suggestBindings())
        Log::logger->log(Log::WARN, "[OPENXR] no interaction profile bindings were accepted by the runtime");

    // 4. Per-hand aim + grip action spaces (poseInActionSpace = identity).
    if (!createActionSpaces())
        return false;

    // 5. Attach — legal only once per session.
    XrSessionActionSetsAttachInfo attach = {XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach.countActionSets               = 1;
    attach.actionSets                    = &m_actionSet;
    XrResult r                           = xrAttachSessionActionSets(m_session, &attach);
    if (XR_FAILED(r)) {
        Log::logger->log(Log::ERR, "[OPENXR] xrAttachSessionActionSets failed: {}", (int)r);
        return false;
    }
    m_attached = true;
    Log::logger->log(Log::DEBUG, "[OPENXR] action set 'hyprland' attached to session");
    return true;
}

bool CXRInput::suggestBindings() {
    // Helper to intern a binding path and pair it with an action.
    auto bind = [&](XrAction action, const char* path) -> XrActionSuggestedBinding { return XrActionSuggestedBinding{action, toPath(m_instance, path)}; };

    bool any = false;

    // khr/simple_controller — no analog grab / no thumbstick (doc 04 §1.1).
    {
        std::vector<XrActionSuggestedBinding> b = {
            bind(m_aimAction, "/user/hand/left/input/aim/pose"),        bind(m_aimAction, "/user/hand/right/input/aim/pose"),
            bind(m_gripAction, "/user/hand/left/input/grip/pose"),      bind(m_gripAction, "/user/hand/right/input/grip/pose"),
            bind(m_selectAction, "/user/hand/left/input/select/click"), bind(m_selectAction, "/user/hand/right/input/select/click"),
            bind(m_menuAction, "/user/hand/left/input/menu/click"),     bind(m_menuAction, "/user/hand/right/input/menu/click"),
            bind(m_hapticAction, "/user/hand/left/output/haptic"),      bind(m_hapticAction, "/user/hand/right/output/haptic"),
        };
        any |= suggestProfile(m_instance, "/interaction_profiles/khr/simple_controller", b);
    }

    // valve/index_controller — the Monado remote-driver profile (doc 04 §1.2).
    {
        std::vector<XrActionSuggestedBinding> b = {
            bind(m_aimAction, "/user/hand/left/input/aim/pose"),         bind(m_aimAction, "/user/hand/right/input/aim/pose"),
            bind(m_gripAction, "/user/hand/left/input/grip/pose"),       bind(m_gripAction, "/user/hand/right/input/grip/pose"),
            bind(m_selectAction, "/user/hand/left/input/trigger/value"), bind(m_selectAction, "/user/hand/right/input/trigger/value"),
            bind(m_grabAction, "/user/hand/left/input/squeeze/value"),   bind(m_grabAction, "/user/hand/right/input/squeeze/value"),
            bind(m_scrollAction, "/user/hand/left/input/thumbstick"),    bind(m_scrollAction, "/user/hand/right/input/thumbstick"),
            bind(m_menuAction, "/user/hand/left/input/a/click"),         bind(m_menuAction, "/user/hand/right/input/a/click"),
            bind(m_hapticAction, "/user/hand/left/output/haptic"),       bind(m_hapticAction, "/user/hand/right/output/haptic"),
        };
        any |= suggestProfile(m_instance, "/interaction_profiles/valve/index_controller", b);
    }

    // oculus/touch_controller — asymmetric menu (left menu/click, right b/click) (doc 04 §1.3).
    {
        std::vector<XrActionSuggestedBinding> b = {
            bind(m_aimAction, "/user/hand/left/input/aim/pose"),         bind(m_aimAction, "/user/hand/right/input/aim/pose"),
            bind(m_gripAction, "/user/hand/left/input/grip/pose"),       bind(m_gripAction, "/user/hand/right/input/grip/pose"),
            bind(m_selectAction, "/user/hand/left/input/trigger/value"), bind(m_selectAction, "/user/hand/right/input/trigger/value"),
            bind(m_grabAction, "/user/hand/left/input/squeeze/value"),   bind(m_grabAction, "/user/hand/right/input/squeeze/value"),
            bind(m_scrollAction, "/user/hand/left/input/thumbstick"),    bind(m_scrollAction, "/user/hand/right/input/thumbstick"),
            bind(m_menuAction, "/user/hand/left/input/menu/click"),      bind(m_menuAction, "/user/hand/right/input/b/click"),
            bind(m_hapticAction, "/user/hand/left/output/haptic"),       bind(m_hapticAction, "/user/hand/right/output/haptic"),
        };
        any |= suggestProfile(m_instance, "/interaction_profiles/oculus/touch_controller", b);
    }

    // ext/hand_interaction_ext — only when XR_EXT_hand_interaction was enabled (doc 04 §1.4).
    if (m_hasHandInteraction) {
        std::vector<XrActionSuggestedBinding> b = {
            bind(m_aimAction, "/user/hand/left/input/aim/pose"),           bind(m_aimAction, "/user/hand/right/input/aim/pose"),
            bind(m_gripAction, "/user/hand/left/input/grip/pose"),         bind(m_gripAction, "/user/hand/right/input/grip/pose"),
            bind(m_selectAction, "/user/hand/left/input/pinch_ext/value"), bind(m_selectAction, "/user/hand/right/input/pinch_ext/value"),
            bind(m_grabAction, "/user/hand/left/input/grasp_ext/value"),   bind(m_grabAction, "/user/hand/right/input/grasp_ext/value"),
        };
        any |= suggestProfile(m_instance, "/interaction_profiles/ext/hand_interaction_ext", b);
    }

    return any;
}

bool CXRInput::createActionSpaces() {
    auto makeSpace = [&](XrAction action, OpenXR::eXRHand hand, XrSpace& out) -> bool {
        XrActionSpaceCreateInfo si = {XR_TYPE_ACTION_SPACE_CREATE_INFO};
        si.action                  = action;
        si.subactionPath           = m_handPath[hand];
        si.poseInActionSpace       = {{0, 0, 0, 1}, {0, 0, 0}}; // identity
        XrResult r                 = xrCreateActionSpace(m_session, &si, &out);
        if (XR_FAILED(r)) {
            Log::logger->log(Log::ERR, "[OPENXR] xrCreateActionSpace failed: {}", (int)r);
            out = XR_NULL_HANDLE;
            return false;
        }
        return true;
    };

    return makeSpace(m_aimAction, XR_HAND_LEFT, m_aimSpace[XR_HAND_LEFT]) && makeSpace(m_aimAction, XR_HAND_RIGHT, m_aimSpace[XR_HAND_RIGHT]) &&
        makeSpace(m_gripAction, XR_HAND_LEFT, m_gripSpace[XR_HAND_LEFT]) && makeSpace(m_gripAction, XR_HAND_RIGHT, m_gripSpace[XR_HAND_RIGHT]);
}

void CXRInput::destroy() {
    for (auto& s : m_aimSpace)
        if (s != XR_NULL_HANDLE) {
            xrDestroySpace(s);
            s = XR_NULL_HANDLE;
        }
    for (auto& s : m_gripSpace)
        if (s != XR_NULL_HANDLE) {
            xrDestroySpace(s);
            s = XR_NULL_HANDLE;
        }
    if (m_actionSet != XR_NULL_HANDLE) {
        xrDestroyActionSet(m_actionSet); // also destroys child actions
        m_actionSet = XR_NULL_HANDLE;
    }
    m_aimAction = m_gripAction = m_selectAction = m_grabAction = m_scrollAction = m_menuAction = m_hapticAction = XR_NULL_HANDLE;
    m_attached                                                                                                  = false;
}

// ---------------------------------------------------------------------------------------------
// sample: xrSyncActions + per-hand pose/analog read (frame thread, doc 04 §2).
// ---------------------------------------------------------------------------------------------

void CXRInput::logInteractionProfileOnce() {
    if (m_profileLogged)
        return;
    m_profileLogged = true;
    for (auto hand : {XR_HAND_LEFT, XR_HAND_RIGHT}) {
        XrInteractionProfileState st = {XR_TYPE_INTERACTION_PROFILE_STATE};
        if (XR_SUCCEEDED(xrGetCurrentInteractionProfile(m_session, m_handPath[hand], &st)))
            Log::logger->log(Log::DEBUG, "[OPENXR] current interaction profile ({}): {}", hand == XR_HAND_LEFT ? "left" : "right", fromPath(m_instance, st.interactionProfile));
    }
}

void CXRInput::sample(XrTime predictedDisplayTime, XrSpace refSpace) {
    if (!m_attached)
        return;

    XrActiveActionSet active = {m_actionSet, XR_NULL_PATH};
    XrActionsSyncInfo si     = {XR_TYPE_ACTIONS_SYNC_INFO};
    si.countActiveActionSets = 1;
    si.activeActionSets      = &active;

    XrResult r = xrSyncActions(m_session, &si);
    // XR_SESSION_NOT_FOCUSED is a SUCCESS code: actions simply read inactive. Only genuine
    // failures are worth a (throttled) log — but do not spam the frame loop.
    if (XR_FAILED(r)) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            Log::logger->log(Log::WARN, "[OPENXR] xrSyncActions failed: {} (logged once)", (int)r);
        }
        for (auto& h : m_hands)
            h = SXRHandState{};
        return;
    }

    const bool focused = r == XR_SUCCESS; // XR_SESSION_NOT_FOCUSED => actions inactive

    for (auto hand : {XR_HAND_LEFT, XR_HAND_RIGHT}) {
        SXRHandState h;

        // Poses: xrLocateSpace of the aim/grip action spaces in refSpace at the predicted time.
        auto locate = [&](XrSpace space) -> std::optional<OpenXR::SXRPose> {
            if (space == XR_NULL_HANDLE)
                return std::nullopt;
            XrSpaceLocation loc = {XR_TYPE_SPACE_LOCATION};
            if (XR_FAILED(xrLocateSpace(space, refSpace, predictedDisplayTime, &loc)))
                return std::nullopt;
            if (!(loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) || !(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
                return std::nullopt;
            return xrToPose(loc.pose);
        };
        h.aim  = locate(m_aimSpace[hand]);
        h.grip = locate(m_gripSpace[hand]);

        // Analog / boolean state (each getter yields {value/currentState, isActive}).
        XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO};
        gi.subactionPath        = m_handPath[hand];

        gi.action             = m_selectAction;
        XrActionStateFloat sf = {XR_TYPE_ACTION_STATE_FLOAT};
        if (XR_SUCCEEDED(xrGetActionStateFloat(m_session, &gi, &sf)) && sf.isActive) {
            h.select = sf.currentState;
            h.active = true;
        }

        gi.action             = m_grabAction;
        XrActionStateFloat gf = {XR_TYPE_ACTION_STATE_FLOAT};
        if (XR_SUCCEEDED(xrGetActionStateFloat(m_session, &gi, &gf)) && gf.isActive)
            h.grab = gf.currentState;

        gi.action                = m_scrollAction;
        XrActionStateVector2f vf = {XR_TYPE_ACTION_STATE_VECTOR2F};
        if (XR_SUCCEEDED(xrGetActionStateVector2f(m_session, &gi, &vf)) && vf.isActive)
            h.stick = Vector2D{vf.currentState.x, vf.currentState.y};

        gi.action               = m_menuAction;
        XrActionStateBoolean bf = {XR_TYPE_ACTION_STATE_BOOLEAN};
        if (XR_SUCCEEDED(xrGetActionStateBoolean(m_session, &gi, &bf)) && bf.isActive)
            h.menu = bf.currentState;

        m_hands[hand] = h;
    }

    if (focused)
        logInteractionProfileOnce();
}

// ---------------------------------------------------------------------------------------------
// processPointer: ray cast, hover/owner arbitration, hysteresis, scroll (frame thread, §3-§5).
// ---------------------------------------------------------------------------------------------

void CXRInput::hapticTick(OpenXR::eXRHand hand) {
    if (m_hapticAction == XR_NULL_HANDLE)
        return;
    XrHapticVibration vib  = {XR_TYPE_HAPTIC_VIBRATION};
    vib.duration           = 10'000'000; // XR_HAPTIC_TICK_NS = 10 ms (doc 04 §6.3)
    vib.frequency          = XR_FREQUENCY_UNSPECIFIED;
    vib.amplitude          = 0.5f;
    XrHapticActionInfo hai = {XR_TYPE_HAPTIC_ACTION_INFO};
    hai.action             = m_hapticAction;
    hai.subactionPath      = m_handPath[hand];
    // Fire-and-forget: a profile without a haptic binding just returns an error we ignore.
    xrApplyHapticFeedback(m_session, &hai, reinterpret_cast<const XrHapticBaseHeader*>(&vib));
}

void CXRInput::processPointer(const std::vector<SXRPointerTarget>& targets, uint32_t timeMs, const OpenXR::SXRSolveInput& solveIn, const OpenXR::SXRAnchorTuning& tune) {
    if (!m_emit)
        return;

    static auto PSELON      = CConfigValue<Hyprlang::FLOAT>("openxr:pointer_trigger_threshold");
    static auto PSELOFF     = CConfigValue<Hyprlang::FLOAT>("openxr:pointer_trigger_threshold_release");
    static auto PGRABON     = CConfigValue<Hyprlang::FLOAT>("openxr:grab_threshold");
    static auto PGRABOFF    = CConfigValue<Hyprlang::FLOAT>("openxr:grab_threshold_release");
    static auto PSCROLLSPD  = CConfigValue<Hyprlang::FLOAT>("openxr:scroll_speed");
    static auto PRELLATENCY = CConfigValue<Hyprlang::INT>("openxr:grab_release_latency_ms");
    static auto PRELVELREJ  = CConfigValue<Hyprlang::FLOAT>("openxr:grab_release_velocity_reject");
    const float onT         = (float)*PSELON;
    const float offT        = (float)*PSELOFF;
    const float grabOnT     = (float)*PGRABON;
    const float grabOffT    = (float)*PGRABOFF;
    const float scrollSpeed = (float)*PSCROLLSPD;
    const int64_t  relLatencyRaw = (int64_t)*PRELLATENCY;
    const uint32_t relLatencyMs  = relLatencyRaw > 0 ? (uint32_t)relLatencyRaw : 0;
    const float    relVelReject  = (float)*PRELVELREJ;

    bool        emittedAny = false;

    auto        emit = [&](SXRInputEvent ev) {
        ev.timeMs = timeMs;
        m_emit(ev);
        emittedAny = true;
    };

    auto emitGrabState = [&](MONITORID id, const std::string& name, bool begin) {
        SXRStateEvent ev;
        ev.type      = eXRStateEventType::GRAB;
        ev.monitorID = id;
        ev.a         = begin ? 1 : 0;
        ev.str       = name;
        m_emit(ev);
    };

    // Ray-plane parameter t only (no bounds test) — used by the §6 cone-forgiveness pass to size
    // the slack (tan(cone) * t) before re-testing bounds with rayQuadIntersect.
    auto planeT = [](const OpenXR::SXRPose& Q, const OpenXR::Vec3& o, const OpenXR::Vec3& d) -> std::optional<float> {
        const OpenXR::Quat qi = OpenXR::qInverse(Q.rot);
        const OpenXR::Vec3 lo = OpenXR::qRotate(qi, o - Q.pos);
        const OpenXR::Vec3 ld = OpenXR::qRotate(qi, d);
        if (std::fabs(ld.z) < 1e-6F)
            return std::nullopt;
        const float t = -lo.z / ld.z;
        if (t <= 0.F)
            return std::nullopt;
        return t;
    };

    // 1. Ray cast per hand: nearest-t hit across all targets wins (occlusion). A grabbing hand
    //    casts nothing (WP8). Hover changes (incl. none<->some) transfer pointer ownership (§3).
    for (auto hand : {XR_HAND_LEFT, XR_HAND_RIGHT}) {
        MONITORID newMon = -1;
        Vector2D  newUV;
        if (!m_grabbing[hand] && m_hands[hand].aim) {
            const OpenXR::SXRPose& aim    = *m_hands[hand].aim;
            const OpenXR::Vec3     origin = aim.pos;
            const OpenXR::Vec3     dir    = OpenXR::qRotate(aim.rot, OpenXR::Vec3{0.f, 0.f, -1.f});

            float                  bestT = std::numeric_limits<float>::max();
            for (const auto& t : targets) {
                const OpenXR::SXRQuadHit hit = OpenXR::rayQuadIntersect(t.worldPose, origin, dir, t.w, t.h);
                if (hit.hit && hit.t < bestT) {
                    bestT  = hit.t;
                    newMon = t.id;
                    newUV  = Vector2D{hit.u, hit.v};
                }
            }
        }

        if (newMon != m_hoverMon[hand]) {
            m_owner = hand; // hover change owns the pointer (§3)
            Log::logger->log(Log::DEBUG, "[OPENXR] hand {} hover {} -> {} (uv {:.3f},{:.3f})", hand == XR_HAND_LEFT ? "L" : "R", (long long)m_hoverMon[hand], (long long)newMon,
                             newUV.x, newUV.y);
        }
        m_hoverMon[hand] = newMon;
        m_hoverUV[hand]  = newUV;
    }

    // 2. Grab state machine (doc 04 §6 / doc 03 §4). Uses this frame's just-updated hover (step
    //    1) to decide grab entry, and re-resolves the grabbed target by id every frame (a target
    //    vanishing mid-grab means its monitor was destroyed -> force release, no re-anchor).
    for (auto hand : {XR_HAND_LEFT, XR_HAND_RIGHT}) {
        const std::optional<OpenXR::SXRPose>& worldGrip = hand == XR_HAND_LEFT ? solveIn.gripLeft : solveIn.gripRight;

        if (m_grabTrig[hand].update(m_hands[hand].grab, grabOnT, grabOffT)) {
            if (m_grabTrig[hand].state) {
                // Rising edge: try to begin a grab on the hovered quad, or (if hovering nothing)
                // redo the intersection with the 5-degree entry cone (doc 04 §6).
                const SXRPointerTarget* target = nullptr;
                if (m_hoverMon[hand] >= 0) {
                    for (const auto& t : targets)
                        if (t.id == m_hoverMon[hand]) {
                            target = &t;
                            break;
                        }
                }
                if (!target && m_hands[hand].aim) {
                    const OpenXR::SXRPose&  aim    = *m_hands[hand].aim;
                    const OpenXR::Vec3      origin = aim.pos;
                    const OpenXR::Vec3      dir    = OpenXR::qRotate(aim.rot, OpenXR::Vec3{0.f, 0.f, -1.f});
                    float                   bestT  = std::numeric_limits<float>::max();
                    const SXRPointerTarget* best   = nullptr;
                    for (const auto& t : targets) {
                        const auto pt = planeT(t.worldPose, origin, dir);
                        if (!pt)
                            continue;
                        const float              slack = std::tan(XR_GRAB_CONE_DEG * (float)M_PI / 180.F) * (*pt);
                        const OpenXR::SXRQuadHit hit   = OpenXR::rayQuadIntersect(t.worldPose, origin, dir, t.w, t.h, slack);
                        if (hit.hit && hit.t < bestT) {
                            bestT = hit.t;
                            best  = &t;
                        }
                    }
                    target = best;
                }

                if (target && target->anchor && !target->anchor->grabbed() && worldGrip) {
                    target->anchor->beginGrab(hand, *worldGrip);
                    m_grabbing[hand]       = true;
                    m_grabbedMon[hand]     = target->id;
                    m_grabbedMonName[hand] = target->name;
                    m_grabRing[hand].reset(); // start a fresh carry history (WP-G4)
                    if (m_owner == (int)hand)
                        m_owner = -1; // pointer ownership free-for-take by the other hand (§6)
                    hapticTick(hand);
                    emitGrabState(target->id, target->name, true);
                }
                // else: no target even with cone forgiveness, or it's already grabbed by the
                // other hand -> the squeeze is ignored, no state change (doc 04 §6).
            } else if (m_grabbing[hand]) {
                // Falling edge: end the grab and re-anchor into the persistent mode. WP-G4: instead
                // of re-anchoring from THIS frame's grip pose (which the release gesture just
                // perturbed — the lurch), re-anchor from the latched / velocity-rejected pose out of
                // the carry ring. Falls back to the release-frame endGrab if the ring is empty
                // (e.g. a grab that lasted a single frame).
                const SXRPointerTarget* target = nullptr;
                for (const auto& t : targets)
                    if (t.id == m_grabbedMon[hand]) {
                        target = &t;
                        break;
                    }
                if (target && target->anchor) {
                    if (m_grabRing[hand].size() > 0) {
                        const OpenXR::SXRPose releaseWorld = OpenXR::pickReleasePose(m_grabRing[hand], timeMs, relLatencyMs, relVelReject);
                        target->anchor->endGrab(releaseWorld, solveIn, tune);
                    } else
                        target->anchor->endGrab(solveIn, tune);
                }
                // else: the layer is gone (destroyed mid-grab) -> force release, no re-anchor.
                hapticTick(hand);
                emitGrabState(m_grabbedMon[hand], m_grabbedMonName[hand], false);
                m_grabbing[hand]   = false;
                m_grabbedMon[hand] = -1;
                m_grabbedMonName[hand].clear();
                m_grabRing[hand].reset();
            }
        }

        // While grabbed (every frame, incl. the frame a grab just began): thumbstick push/pull +
        // resize, and a liveness check (monitor destroyed mid-grab -> force release, doc 04 §6).
        if (m_grabbing[hand]) {
            const SXRPointerTarget* target = nullptr;
            for (const auto& t : targets)
                if (t.id == m_grabbedMon[hand]) {
                    target = &t;
                    break;
                }
            if (!target || !target->anchor) {
                emitGrabState(m_grabbedMon[hand], m_grabbedMonName[hand], false);
                m_grabbing[hand]   = false;
                m_grabbedMon[hand] = -1;
                m_grabbedMonName[hand].clear();
            } else {
                const Vector2D stick = m_hands[hand].stick;
                if (std::fabs(stick.y) > XR_STICK_DEADZONE)
                    target->anchor->grabPushPull(stick.y * XR_GRAB_PUSHPULL_SPEED * solveIn.dt);
                if (std::fabs(stick.x) > XR_STICK_DEADZONE)
                    target->anchor->grabResize(stick.x * XR_GRAB_RESIZE_SPEED * solveIn.dt);
                // Record the carried world pose for the release latch (WP-G4). The solve already
                // ran this frame, so lastWorld() is this frame's grip ∘ offset composed pose.
                if (target->anchor->hasLastWorld())
                    m_grabRing[hand].push(target->anchor->lastWorld(), timeMs);
            }
        }
    }

    // MOTION_ABS for the owner, coalesced on (monitor, uv). monitorID == -1 signals the ray left
    // all quads (a hover clear on the main thread). Emitted before buttons so a click lands on
    // the freshly-warped pixel.
    auto emitOwnerMotion = [&]() {
        if (m_owner < 0)
            return;
        const MONITORID om  = m_hoverMon[m_owner];
        const Vector2D  ouv = m_hoverUV[m_owner];
        if (om >= 0) {
            if (om != m_emittedMon || (ouv - m_emittedUV).size() > XR_UV_EPSILON) {
                SXRInputEvent ev;
                ev.type      = eXRInputEventType::MOTION_ABS;
                ev.monitorID = om;
                ev.uv        = ouv;
                emit(ev);
                m_emittedMon = om;
                m_emittedUV  = ouv;
            }
        } else if (m_emittedMon >= 0) {
            SXRInputEvent ev;
            ev.type      = eXRInputEventType::MOTION_ABS;
            ev.monitorID = -1; // hover clear
            emit(ev);
            m_emittedMon = -1;
        }
    };

    emitOwnerMotion();

    // 3. Select + menu edges per hand. A press transfers ownership and (single pointer) is only
    //    honored when no other hand already holds that button; a release always closes its own
    //    press so the button can never stick.
    for (auto hand : {XR_HAND_LEFT, XR_HAND_RIGHT}) {
        if (m_grabbing[hand])
            continue;

        // select -> BTN_LEFT
        if (m_selectTrig[hand].update(m_hands[hand].select, onT, offT)) {
            if (m_selectTrig[hand].state) {
                m_owner = hand; // press owns the pointer (§4)
                emitOwnerMotion();
                if (m_hoverMon[hand] >= 0 && m_leftHolder < 0) {
                    m_leftHolder = hand;
                    SXRInputEvent ev;
                    ev.type      = eXRInputEventType::BUTTON;
                    ev.monitorID = m_hoverMon[hand];
                    ev.button    = BTN_LEFT;
                    ev.pressed   = true;
                    emit(ev);
                    hapticTick(hand); // press only (doc 04 §6.3)
                }
            } else if (m_leftHolder == hand) {
                m_leftHolder = -1;
                SXRInputEvent ev;
                ev.type      = eXRInputEventType::BUTTON;
                ev.monitorID = m_hoverMon[hand];
                ev.button    = BTN_LEFT;
                ev.pressed   = false; // release fires even if the ray has left the quad
                emit(ev);
            }
        }

        // menu -> BTN_RIGHT (bool via a 0.5 threshold Schmitt)
        if (m_menuTrig[hand].update(m_hands[hand].menu ? 1.f : 0.f, 0.5f, 0.5f)) {
            if (m_menuTrig[hand].state) {
                m_owner = hand;
                emitOwnerMotion();
                if (m_hoverMon[hand] >= 0 && m_rightHolder < 0) {
                    m_rightHolder = hand;
                    SXRInputEvent ev;
                    ev.type      = eXRInputEventType::BUTTON;
                    ev.monitorID = m_hoverMon[hand];
                    ev.button    = BTN_RIGHT;
                    ev.pressed   = true;
                    emit(ev);
                }
            } else if (m_rightHolder == hand) {
                m_rightHolder = -1;
                SXRInputEvent ev;
                ev.type      = eXRInputEventType::BUTTON;
                ev.monitorID = m_hoverMon[hand];
                ev.button    = BTN_RIGHT;
                ev.pressed   = false;
                emit(ev);
            }
        }
    }

    // 4. Scroll: only the owner hand, only while hovering (not grabbing). Thumbstick -> axis,
    //    one wheel notch per full deflection per frame batch (doc 04 §5).
    if (m_owner >= 0 && !m_grabbing[m_owner] && m_hoverMon[m_owner] >= 0) {
        const Vector2D stick = m_hands[m_owner].stick;
        if (std::fabs(stick.y) > XR_STICK_DEADZONE) {
            SXRInputEvent ev;
            ev.type      = eXRInputEventType::AXIS;
            ev.axis      = WL_POINTER_AXIS_VERTICAL_SCROLL;
            ev.axisDelta = -stick.y * XR_SCROLL_NOTCH * scrollSpeed; // stick up => wheel up
            emit(ev);
        }
        if (std::fabs(stick.x) > XR_STICK_DEADZONE) {
            SXRInputEvent ev;
            ev.type      = eXRInputEventType::AXIS;
            ev.axis      = WL_POINTER_AXIS_HORIZONTAL_SCROLL;
            ev.axisDelta = stick.x * XR_SCROLL_NOTCH * scrollSpeed;
            emit(ev);
        }
    }

    // 5. Terminate the batch with a FRAME event (§5/§7) only when something was queued, so an
    //    idle session adds zero wakeups.
    if (emittedAny) {
        SXRInputEvent frame;
        frame.type = eXRInputEventType::FRAME;
        emit(frame);
    }
}

#endif // HAVE_OPENXR
