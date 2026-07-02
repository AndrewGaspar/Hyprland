#pragma once
#ifdef HAVE_OPENXR

// CXRInput — the OpenXR action system (docs/openxr/04-input.md). Owns the "hyprland" action set
// (aim/grip poses, select/grab analog, scroll, menu, haptic), the suggested bindings for every
// supported interaction profile, and the per-hand aim/grip XrActionSpaces. It is created on the
// MAIN thread in COpenXRManager::start() (after the session + reference spaces exist, before the
// frame thread spawns) so that xrAttachSessionActionSets — legal only once per session — runs
// before the frame loop begins. sample() runs once per XR frame on the FRAME thread.
//
// This header pulls in only the base openxr.h (no platform macros / EGL headers needed), matching
// XRAnchor.hpp / XRMonitorLayer.hpp.

#include <openxr/openxr.h>

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <variant>

#include "XRQueue.hpp"
#include "XRMath.hpp"               // OpenXR::SXRPose
#include "XRAnchor.hpp"             // OpenXR::eXRHand
#include "../SharedDefs.hpp"        // MONITORID
#include "../helpers/math/Math.hpp" // Vector2D + wl_pointer_axis

class CXRSession;

// ---- frame -> main event structs (doc 04 §7.1) ----
enum class eXRInputEventType : uint8_t {
    MOTION_ABS = 0, // pointer moved on a quad
    BUTTON,         // select/menu edge
    AXIS,           // scroll
    FRAME,          // batch terminator
};

struct SXRInputEvent {
    eXRInputEventType type      = eXRInputEventType::FRAME;
    MONITORID         monitorID = -1; // MOTION_ABS / BUTTON target
    Vector2D          uv;             // MOTION_ABS: 0.0-1.0
    uint32_t          button    = 0;  // BUTTON: BTN_LEFT 0x110 / BTN_RIGHT 0x111
    bool              pressed   = false;
    wl_pointer_axis   axis      = WL_POINTER_AXIS_VERTICAL_SCROLL; // AXIS
    double            axisDelta = 0.0;                             // AXIS
    uint32_t          timeMs    = 0;
};

enum class eXRStateEventType : uint8_t {
    SESSION_STATE, // XrSessionState changed -> openxrsessionstate / openxractive
    GRAB,          // §6 -> xrmonitorgrab (WP8)
    TRACKING,      // device-lock tracking gained/lost (informational, logged)
    LAYER_REMOVED, // DEVIATION (doc 04 §7.1 lists 3): internal removal-barrier ack (str = name)
};

struct SXRStateEvent {
    eXRStateEventType type      = eXRStateEventType::SESSION_STATE;
    MONITORID         monitorID = -1;
    int32_t           a         = 0; // SESSION_STATE: eXRManagerState value; GRAB: 1/0
    std::string       str;           // optional payload (e.g. monitor name)
};

using XRQueueItem = std::variant<SXRInputEvent, SXRStateEvent>;

constexpr size_t XR_QUEUE_CAP = 1024; // doc 04 §7.2 (power of two)
using CXRQueue                = OpenXR::CXRSPSCRing<XRQueueItem, XR_QUEUE_CAP>;

// Per-hand sampled action state, produced each frame by sample() and read by the frame loop
// (anchor solve grip poses now; ray cast / hysteresis / grab in WP7/WP8).
struct SXRHandState {
    std::optional<OpenXR::SXRPose> aim;  // aim pose in the reference space (nullopt = invalid)
    std::optional<OpenXR::SXRPose> grip; // grip pose in the reference space (nullopt = invalid)
    float                          select = 0.f;
    float                          grab   = 0.f;
    Vector2D                       stick; // thumbstick
    bool                           menu   = false;
    bool                           active = false; // any action bound + active this frame (FOCUSED)
};

class CXRInput {
  public:
    CXRInput()  = default;
    ~CXRInput() = default;

    // Main thread. Build the action set + actions, suggest bindings for every supported profile
    // (a missing/unsupported profile is logged and skipped, never fatal), create the four action
    // spaces, and attach the set to the session. Returns false only on a genuinely fatal error
    // (action set / core action creation failed). hasHandInteraction gates the optional
    // ext/hand_interaction profile.
    bool init(CXRSession& session, bool hasHandInteraction);

    // Main thread, during teardown (frame thread already joined). Destroys action spaces + set.
    void destroy();

    // Frame thread. xrSyncActions + per-hand pose/analog sampling at predictedDisplayTime,
    // locating aim/grip in refSpace. Emits WP6 instrumentation input events across the queue.
    void sample(XrTime predictedDisplayTime, XrSpace refSpace);

    // Frame-thread reads of the latest sample:
    const SXRHandState& hand(OpenXR::eXRHand h) const {
        return m_hands[h];
    }
    std::optional<OpenXR::SXRPose> grip(OpenXR::eXRHand h) const {
        return m_hands[h].grip;
    }
    // Grip action space handles for the device-lock late-latch path (doc 03 §3.4). XR_NULL_HANDLE
    // when input is unavailable.
    XrSpace gripSpace(OpenXR::eXRHand h) const {
        return m_gripSpace[h];
    }

    // Frame thread (set on the main thread before the frame thread starts). Producer sink for
    // frame->main events (queue push + eventfd wake, owned by COpenXRManager).
    void setEmitter(std::function<void(XRQueueItem)> emit) {
        m_emit = std::move(emit);
    }

  private:
    bool                             suggestBindings();
    bool                             createActionSpaces();
    void                             logInteractionProfileOnce();

    std::function<void(XRQueueItem)> m_emit;

    XrInstance                       m_instance           = XR_NULL_HANDLE;
    XrSession                        m_session            = XR_NULL_HANDLE;
    bool                             m_hasHandInteraction = false;

    XrActionSet                      m_actionSet    = XR_NULL_HANDLE;
    XrAction                         m_aimAction    = XR_NULL_HANDLE;
    XrAction                         m_gripAction   = XR_NULL_HANDLE;
    XrAction                         m_selectAction = XR_NULL_HANDLE;
    XrAction                         m_grabAction   = XR_NULL_HANDLE;
    XrAction                         m_scrollAction = XR_NULL_HANDLE;
    XrAction                         m_menuAction   = XR_NULL_HANDLE;
    XrAction                         m_hapticAction = XR_NULL_HANDLE;

    std::array<XrPath, 2>            m_handPath{XR_NULL_PATH, XR_NULL_PATH};
    std::array<XrSpace, 2>           m_aimSpace{XR_NULL_HANDLE, XR_NULL_HANDLE};
    std::array<XrSpace, 2>           m_gripSpace{XR_NULL_HANDLE, XR_NULL_HANDLE};

    std::array<SXRHandState, 2>      m_hands;

    bool                             m_attached      = false;
    bool                             m_profileLogged = false; // interaction profile logged once for debuggability
    // WP6 instrumentation: a per-hand Schmitt trigger on select, proving action state crosses the
    // frame->main queue (the real pointer hysteresis + ray cast lands in WP7).
    std::array<bool, 2> m_selectPressed{false, false};
};

#endif // HAVE_OPENXR
