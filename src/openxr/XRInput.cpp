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

    // WP-G5: the hand pinch value + stable pinch pose (only bound to ext/hand_interaction_ext).
    // Created only when the extension is enabled — no controller profile references them.
    if (m_hasHandInteraction) {
        if (!makeAction(m_pinchValueAction, "pinch_value", "Pinch value", XR_ACTION_TYPE_FLOAT_INPUT) ||
            !makeAction(m_pinchPoseAction, "pinch_pose", "Pinch pose", XR_ACTION_TYPE_POSE_INPUT))
            return false;
    }

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

    // ext/hand_interaction_ext — only when XR_EXT_hand_interaction was enabled (doc 04 §1.4, WP-G5).
    // pinch_ext/value is bound to BOTH select (a body pinch = a click, §5.2) and the dedicated
    // pinch_value action (the hand grab gesture, region-gated to the bar/corners); grasp_ext/value
    // stays on grab (the fist, used when openxr:hand_grab is grasp/both). pinch_ext/pose is the
    // stable MOVE-grab anchor. aim drives the ray; grip is kept for grasp anchoring + the ref-space
    // solve. Binding one source (pinch value) to two actions is legal and intentional.
    if (m_hasHandInteraction) {
        std::vector<XrActionSuggestedBinding> b = {
            bind(m_aimAction, "/user/hand/left/input/aim/pose"),               bind(m_aimAction, "/user/hand/right/input/aim/pose"),
            bind(m_gripAction, "/user/hand/left/input/grip/pose"),             bind(m_gripAction, "/user/hand/right/input/grip/pose"),
            bind(m_selectAction, "/user/hand/left/input/pinch_ext/value"),     bind(m_selectAction, "/user/hand/right/input/pinch_ext/value"),
            bind(m_pinchValueAction, "/user/hand/left/input/pinch_ext/value"), bind(m_pinchValueAction, "/user/hand/right/input/pinch_ext/value"),
            bind(m_pinchPoseAction, "/user/hand/left/input/pinch_ext/pose"),   bind(m_pinchPoseAction, "/user/hand/right/input/pinch_ext/pose"),
            bind(m_grabAction, "/user/hand/left/input/grasp_ext/value"),       bind(m_grabAction, "/user/hand/right/input/grasp_ext/value"),
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

    if (!(makeSpace(m_aimAction, XR_HAND_LEFT, m_aimSpace[XR_HAND_LEFT]) && makeSpace(m_aimAction, XR_HAND_RIGHT, m_aimSpace[XR_HAND_RIGHT]) &&
          makeSpace(m_gripAction, XR_HAND_LEFT, m_gripSpace[XR_HAND_LEFT]) && makeSpace(m_gripAction, XR_HAND_RIGHT, m_gripSpace[XR_HAND_RIGHT])))
        return false;

    // WP-G5: pinch pose spaces (hands only). A creation failure here is non-fatal — hands simply
    // fall back to the grip pose for anchoring (pinchSpace stays XR_NULL_HANDLE); the aim/grip
    // spaces above are what the core pointer/anchor paths need.
    if (m_hasHandInteraction) {
        if (!makeSpace(m_pinchPoseAction, XR_HAND_LEFT, m_pinchSpace[XR_HAND_LEFT]) || !makeSpace(m_pinchPoseAction, XR_HAND_RIGHT, m_pinchSpace[XR_HAND_RIGHT]))
            Log::logger->log(Log::WARN, "[OPENXR] pinch pose action spaces unavailable; hand grabs will anchor to the grip pose");
    }

    return true;
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
    for (auto& s : m_pinchSpace)
        if (s != XR_NULL_HANDLE) {
            xrDestroySpace(s);
            s = XR_NULL_HANDLE;
        }
    if (m_actionSet != XR_NULL_HANDLE) {
        xrDestroyActionSet(m_actionSet); // also destroys child actions
        m_actionSet = XR_NULL_HANDLE;
    }
    m_aimAction = m_gripAction = m_selectAction = m_grabAction = m_scrollAction = m_menuAction = m_hapticAction = XR_NULL_HANDLE;
    m_pinchValueAction = m_pinchPoseAction = XR_NULL_HANDLE;
    m_attached                             = false;
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

void CXRInput::refreshHandProfiles() {
    for (auto hand : {XR_HAND_LEFT, XR_HAND_RIGHT}) {
        OpenXR::eXRInputKind kind = OpenXR::XR_INPUT_CONTROLLER;
        XrInteractionProfileState st = {XR_TYPE_INTERACTION_PROFILE_STATE};
        if (XR_SUCCEEDED(xrGetCurrentInteractionProfile(m_session, m_handPath[hand], &st)) && st.interactionProfile != XR_NULL_PATH)
            kind = OpenXR::xrInputKindForProfile(fromPath(m_instance, st.interactionProfile));
        m_handActive[hand] = kind == OpenXR::XR_INPUT_HANDS;
        m_handInputKindAtomic[hand].store((uint8_t)kind, std::memory_order_release);
    }
}

void CXRInput::sample(XrTime predictedDisplayTime, XrSpace refSpace) {
    if (!m_attached)
        return;

    // WP-G5: refresh the per-hand active-device cache when the runtime signalled an interaction
    // profile change (or on the first sample). Cheap + rare — never per-frame in steady state.
    if (m_profileDirty.exchange(false, std::memory_order_acq_rel))
        refreshHandProfiles();

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
        h.pinch = locate(m_pinchSpace[hand]); // WP-G5: XR_NULL_HANDLE space -> nullopt

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

        // WP-G5: pinch strength (hands only; the action is unbound for controller profiles).
        if (m_pinchValueAction != XR_NULL_HANDLE) {
            gi.action             = m_pinchValueAction;
            XrActionStateFloat pf = {XR_TYPE_ACTION_STATE_FLOAT};
            if (XR_SUCCEEDED(xrGetActionStateFloat(m_session, &gi, &pf)) && pf.isActive)
                h.pinchValue = pf.currentState;
        }

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

OpenXR::eXRQuadRegion CXRInput::chromeHoverRegion(MONITORID id) const {
    // Precedence so a resize/move affordance highlights over a plain body/margin hover.
    auto rank = [](OpenXR::eXRQuadRegion r) -> int {
        if (OpenXR::xrRegionIsCorner(r) || r == OpenXR::XR_REGION_BAR)
            return 2;
        if (r == OpenXR::XR_REGION_BODY || r == OpenXR::XR_REGION_MARGIN)
            return 1;
        return 0;
    };
    OpenXR::eXRQuadRegion best = OpenXR::XR_REGION_NONE;
    for (int h = 0; h < 2; ++h) {
        if (m_hoverChromeMon[h] == id && rank(m_hoverRegion[h]) > rank(best))
            best = m_hoverRegion[h];
    }
    return best;
}

bool CXRInput::isMonitorGrabbed(MONITORID id) const {
    return m_grabbedMon[OpenXR::XR_HAND_LEFT] == id || m_grabbedMon[OpenXR::XR_HAND_RIGHT] == id;
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
    static auto PGRABANY    = CConfigValue<Hyprlang::INT>("openxr:grab_anywhere");
    static auto PHANDGRAB    = CConfigValue<std::string>("openxr:hand_grab");
    const bool  grabAnywhere = *PGRABANY != 0;
    const OpenXR::eXRHandGrab handGrabMode = OpenXR::xrParseHandGrab(*PHANDGRAB); // hot-toggles (read per-frame)
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
    //    casts nothing (WP8). Hits are classified against each quad's chrome layout (WP-G1): a
    //    BODY hit drives the pointer (uv REMAPPED to content uv so clicks land on the same desktop
    //    pixel as before the chrome margins existed); bar/corner/margin hits are hover-only (no
    //    pointer events — chrome visuals come in WP-G2). BODY-hover changes (incl. none<->some)
    //    transfer pointer ownership (§3), preserving today's click/scroll semantics exactly.
    for (auto hand : {XR_HAND_LEFT, XR_HAND_RIGHT}) {
        MONITORID            newBodyMon = -1; // BODY hover (drives pointer); remapped content uv
        Vector2D             newBodyUV;
        MONITORID            newChromeMon = -1;                     // whichever quad the ray hit (any region)
        OpenXR::eXRQuadRegion newRegion   = OpenXR::XR_REGION_NONE; // its region

        if (!m_grabbing[hand] && m_hands[hand].aim) {
            const OpenXR::SXRPose& aim    = *m_hands[hand].aim;
            const OpenXR::Vec3     origin = aim.pos;
            const OpenXR::Vec3     dir    = OpenXR::qRotate(aim.rot, OpenXR::Vec3{0.f, 0.f, -1.f});

            float                        bestT   = std::numeric_limits<float>::max();
            const SXRPointerTarget*      bestTgt = nullptr;
            OpenXR::SXRQuadHit           bestHit;
            for (const auto& t : targets) {
                const OpenXR::SXRQuadHit hit = OpenXR::rayQuadIntersect(t.worldPose, origin, dir, t.w, t.h);
                if (hit.hit && hit.t < bestT) {
                    bestT   = hit.t;
                    bestTgt = &t;
                    bestHit = hit;
                }
            }
            if (bestTgt) {
                newChromeMon = bestTgt->id;
                newRegion    = OpenXR::classifyQuadHit(bestHit.u, bestHit.v, bestTgt->chrome);
                if (newRegion == OpenXR::XR_REGION_BODY) {
                    float cu = 0.f, cv = 0.f;
                    if (OpenXR::remapToContentUV(bestHit.u, bestHit.v, bestTgt->chrome, cu, cv)) {
                        newBodyMon = bestTgt->id;
                        newBodyUV  = Vector2D{cu, cv};
                    }
                }
            }
        }

        // Body hover (pointer). Ownership transfers on a body-hover change, exactly as before.
        if (newBodyMon != m_hoverMon[hand]) {
            m_owner = hand; // hover change owns the pointer (§3)
            Log::logger->log(Log::DEBUG, "[OPENXR] hand {} body-hover {} -> {} (content uv {:.3f},{:.3f})", hand == XR_HAND_LEFT ? "L" : "R", (long long)m_hoverMon[hand],
                             (long long)newBodyMon, newBodyUV.x, newBodyUV.y);
        }
        m_hoverMon[hand] = newBodyMon;
        m_hoverUV[hand]  = newBodyUV;

        // Chrome region transition (hover-only; aids WP-G2 chrome visuals / WP-G3 grab gating).
        if (newChromeMon != m_hoverChromeMon[hand] || newRegion != m_hoverRegion[hand]) {
            Log::logger->log(Log::DEBUG, "[OPENXR] hand {} region {}@{} -> {}@{}", hand == XR_HAND_LEFT ? "L" : "R", OpenXR::xrRegionName(m_hoverRegion[hand]),
                             (long long)m_hoverChromeMon[hand], OpenXR::xrRegionName(newRegion), (long long)newChromeMon);
        }
        m_hoverChromeMon[hand] = newChromeMon;
        m_hoverRegion[hand]    = newRegion;
    }

    // 2. Grab state machine (doc 04 §6 / doc 03 §4). Uses this frame's just-updated hover (step
    //    1) to decide grab entry, and re-resolves the grabbed target by id every frame (a target
    //    vanishing mid-grab means its monitor was destroyed -> force release, no re-anchor).
    for (auto hand : {XR_HAND_LEFT, XR_HAND_RIGHT}) {
        const std::optional<OpenXR::SXRPose>& worldGrip = hand == XR_HAND_LEFT ? solveIn.gripLeft : solveIn.gripRight;
        // WP-G5: hands vs controller device selection. When hands are active the grab gesture value
        // comes from pinch/grasp per openxr:hand_grab (a controller keeps the plain squeeze), and a
        // pinch-driven grab anchors to the stable pinch pose (floor-shifted, from the frame loop) —
        // falling back to grip when no pinch pose is available. `grabDevPose` follows the current
        // grab's already-decided device (m_grabDevicePinch), used for per-frame carry/resize/latch.
        const bool                            handIsHands = handActive(hand);
        const float                           pinchVal    = m_hands[hand].pinchValue;
        const float                           graspVal    = m_hands[hand].grab;
        const float                           grabVal     = handIsHands ? OpenXR::xrHandGrabValue(handGrabMode, pinchVal, graspVal) : graspVal;
        const std::optional<OpenXR::SXRPose>& pinchPose   = hand == XR_HAND_LEFT ? solveIn.pinchLeft : solveIn.pinchRight;
        const bool                            wantPinch   = handIsHands && OpenXR::xrHandGrabUsesPinch(handGrabMode, pinchVal, graspVal) && pinchPose.has_value();
        const std::optional<OpenXR::SXRPose>& newDevPose  = wantPinch ? pinchPose : worldGrip;
        const std::optional<OpenXR::SXRPose>& grabDevPose = m_grabDevicePinch[hand] ? pinchPose : worldGrip;

        if (m_grabTrig[hand].update(grabVal, grabOnT, grabOffT)) {
            if (m_grabTrig[hand].state) {
                // Rising edge: the grab gesture is gated by the chrome region it landed on (WP-G3).
                // The BAR moves; a CORNER resizes (from that corner); the BODY moves only with
                // openxr:grab_anywhere (the controller-grip convenience); the transparent MARGIN
                // never grabs. grabActionForRegion() is the single decision point — WP-G5 passes
                // handIsHands so a hand is forced to the bar/corners (never body). Prefer the region
                // the hover step already classified this frame; otherwise redo the intersection with
                // the 5-degree entry cone (doc 04 §6) and take the nearest grab-yielding hit.
                const SXRPointerTarget* target = nullptr;
                OpenXR::eXRGrabAction   action = OpenXR::XR_GRAB_ACTION_NONE;
                OpenXR::eXRQuadRegion   region = OpenXR::XR_REGION_NONE;

                if (m_hoverChromeMon[hand] >= 0) {
                    const OpenXR::eXRGrabAction a = OpenXR::grabActionForRegion(m_hoverRegion[hand], grabAnywhere, handIsHands);
                    if (a != OpenXR::XR_GRAB_ACTION_NONE) {
                        for (const auto& t : targets)
                            if (t.id == m_hoverChromeMon[hand]) {
                                target = &t;
                                action = a;
                                region = m_hoverRegion[hand];
                                break;
                            }
                    }
                }
                if (!target && m_hands[hand].aim) {
                    const OpenXR::SXRPose&  aim    = *m_hands[hand].aim;
                    const OpenXR::Vec3      origin = aim.pos;
                    const OpenXR::Vec3      dir    = OpenXR::qRotate(aim.rot, OpenXR::Vec3{0.f, 0.f, -1.f});
                    float                   bestT  = std::numeric_limits<float>::max();
                    for (const auto& t : targets) {
                        const auto pt = planeT(t.worldPose, origin, dir);
                        if (!pt)
                            continue;
                        const float              slack = std::tan(XR_GRAB_CONE_DEG * (float)M_PI / 180.F) * (*pt);
                        const OpenXR::SXRQuadHit hit   = OpenXR::rayQuadIntersect(t.worldPose, origin, dir, t.w, t.h, slack);
                        if (!hit.hit || hit.t >= bestT)
                            continue;
                        const OpenXR::eXRQuadRegion  reg = OpenXR::classifyQuadHit(hit.u, hit.v, t.chrome);
                        const OpenXR::eXRGrabAction  a   = OpenXR::grabActionForRegion(reg, grabAnywhere, handIsHands);
                        if (a == OpenXR::XR_GRAB_ACTION_NONE)
                            continue;
                        bestT  = hit.t;
                        target = &t;
                        action = a;
                        region = reg;
                    }
                }

                if (target && target->anchor && !target->anchor->grabbed() && newDevPose && action != OpenXR::XR_GRAB_ACTION_NONE) {
                    // WP-G5: anchor a hand grab to the pinch pose (wantPinch), a controller/grasp
                    // grab to the grip pose. m_grabDevicePinch remembers the choice so every carry/
                    // resize/latch frame feeds the SAME device pose.
                    m_grabDevicePinch[hand] = wantPinch;
                    if (OpenXR::xrGrabActionIsResize(action)) {
                        // Content aspect (h/w) from the target's full-quad meters * chrome content
                        // fractions — held fixed for the resize (matches the solve's pixel aspect).
                        const float contentW = target->w * target->chrome.contentFracW();
                        const float contentH = target->h * target->chrome.contentFracH();
                        const float aspect   = contentW > 0.f ? contentH / contentW : 1.f;
                        target->anchor->beginResize(hand, region, *newDevPose, aspect);
                        m_grabKind[hand] = OpenXR::XR_GRABKIND_RESIZE;
                    } else {
                        // WP-G6: pass handIsHands so a hand MOVE grab is eligible for the 1€ carry
                        // filter (openxr:grab_filter); controllers pass false and are never filtered.
                        target->anchor->beginGrab(hand, *newDevPose, wantPinch, handIsHands);
                        m_grabKind[hand] = OpenXR::XR_GRABKIND_MOVE;
                    }
                    m_grabbing[hand]       = true;
                    m_grabbedMon[hand]     = target->id;
                    m_grabbedMonName[hand] = target->name;
                    m_grabRing[hand].reset(); // start a fresh carry history (WP-G4)
                    if (m_owner == (int)hand)
                        m_owner = -1; // pointer ownership free-for-take by the other hand (§6)
                    hapticTick(hand);
                    emitGrabState(target->id, target->name, true);
                }
                // else: the region isn't grabbable (margin / body without grab_anywhere), no target
                // even with cone forgiveness, or it's already grabbed by the other hand -> the
                // squeeze is ignored, no state change (doc 04 §6 / research §5.2).
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
                    if (m_grabKind[hand] == OpenXR::XR_GRABKIND_RESIZE) {
                        // The ring holds the resize DEVICE poses (grip for controllers, pinch for a
                        // hand resize — WP-G5); the latched/velocity-rejected sample gives the FINAL
                        // size + pinned-corner position, rejecting the release jerk.
                        if (m_grabRing[hand].size() > 0) {
                            const OpenXR::SXRPose latched = OpenXR::pickReleasePose(m_grabRing[hand], timeMs, relLatencyMs, relVelReject);
                            target->anchor->endResize(latched, solveIn, tune);
                        } else if (grabDevPose)
                            target->anchor->endResize(*grabDevPose, solveIn, tune);
                        // else: no device pose and no history -> leave the last resized size in place.
                    } else if (m_grabRing[hand].size() > 0) {
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
                m_grabKind[hand]   = OpenXR::XR_GRABKIND_NONE;
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
                m_grabKind[hand]   = OpenXR::XR_GRABKIND_NONE;
            } else if (m_grabKind[hand] == OpenXR::XR_GRABKIND_RESIZE) {
                // Corner resize: drive the size from THIS frame's device world pose (grip, or the
                // pinch pose for a hand resize — WP-G5) and record it for the release latch. No stick
                // verbs — the hand's motion is the resize. (Runs under m_layersMu, same discipline as
                // the stick-resize path — both mutate widthMeters.)
                if (grabDevPose) {
                    target->anchor->grabResizeCorner(*grabDevPose, solveIn, tune);
                    m_grabRing[hand].push(*grabDevPose, timeMs);
                }
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
