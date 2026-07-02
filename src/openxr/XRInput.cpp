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

    const bool     focused = r == XR_SUCCESS; // XR_SESSION_NOT_FOCUSED => actions inactive

    static auto    PSELON  = CConfigValue<Hyprlang::FLOAT>("openxr:pointer_trigger_threshold");
    static auto    PSELOFF = CConfigValue<Hyprlang::FLOAT>("openxr:pointer_trigger_threshold_release");
    const float    onT     = (float)*PSELON;
    const float    offT    = (float)*PSELOFF;

    const uint32_t timeMs = (uint32_t)Time::millis(Time::steadyNow());

    bool           emittedAny = false;

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

        // WP6 instrumentation: a Schmitt trigger on select. On an edge, emit a BUTTON event
        // across the frame->main queue so the plumbing can be proven on the main thread. The
        // real ray cast + owner arbitration + monitor targeting lands in WP7.
        bool& pressed = m_selectPressed[hand];
        bool  edge    = false;
        if (!pressed && h.select >= onT) {
            pressed = true;
            edge    = true;
        } else if (pressed && h.select <= offT) {
            pressed = false;
            edge    = true;
        }
        if (edge && m_emit) {
            SXRInputEvent ev;
            ev.type      = eXRInputEventType::BUTTON;
            ev.monitorID = -1; // WP7 fills the hovered monitor
            ev.button    = BTN_LEFT;
            ev.pressed   = pressed;
            ev.timeMs    = timeMs;
            m_emit(ev);
            emittedAny = true;
        }

        m_hands[hand] = h;
    }

    if (focused)
        logInteractionProfileOnce();

    // Terminate the batch with a FRAME event (doc 04 §5/§7) only when something was queued, so
    // an idle session adds zero wakeups.
    if (emittedAny && m_emit) {
        SXRInputEvent frame;
        frame.type   = eXRInputEventType::FRAME;
        frame.timeMs = timeMs;
        m_emit(frame);
    }
}

#endif // HAVE_OPENXR
