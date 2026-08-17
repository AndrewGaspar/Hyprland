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
#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "XRQueue.hpp"
#include "XRMath.hpp"               // OpenXR::SXRPose
#include "XRStereoPair.hpp" // OpenXR::Stereo::SPairDecl (WP X1/X3 pane un-map, pure)
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
    SESSION_STATE,   // XrSessionState changed -> openxrsessionstate / openxractive
    USER_PRESENCE,   // report-19: XR_EXT_user_presence donned/doffed (a = 1 present / 0 absent) ->
                     // drives the `visible`-mode monitor plug gate on the main thread
    GRAB,            // §6 -> xrmonitorgrab (WP8)
    ADAPTIVE,        // research/13 -> xrmonitordocked/xrmonitorundocked (a = 1 undocked / 0 docked, str = name)
    TRACKING,        // device-lock tracking gained/lost (informational, logged)
    LAYER_REMOVED,   // DEVIATION (doc 04 §7.1 lists 3): internal removal-barrier ack (str = name)
    SCHEDULE_FRAMES, // pacing: main thread must scheduleFrame() the visible XR monitors —
                     // aquamarine's idle-callback list is not thread-safe, so the frame thread
                     // may NOT call CMonitor::scheduleFrame() directly (heap corruption)
    VIEWPOINT_STATE, // subscribed layer stable-valid edge (a=1) or tracking/geometry loss (a=0)
    RECENTERED,      // doc 03 §8.4: the runtime moved LOCAL_FLOOR (the headset's recenter button, a
                     // re-don, a guardian re-derive) and openxr:recenter = follow. The frame thread
                     // may NOT run the re-seat policy itself — deciding it reads a STRING config and
                     // arming it is main-thread bookkeeping — so it hands the edge over and the main
                     // thread runs the same requestReseatToHead() the `reseat` verb does
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

// ---- constants introduced by doc 04 §12 (the ones WP7 uses) ----
constexpr float XR_STICK_DEADZONE = 0.1F;  // §5: thumbstick deadzone
constexpr float XR_SCROLL_NOTCH   = 15.0F; // §5: wl axis units per full stick deflection / frame
constexpr float XR_UV_EPSILON     = 1e-4F; // §3: motion coalescing threshold

// ---- constants introduced by doc 04 §12 (the ones WP8 uses) ----
constexpr float XR_GRAB_CONE_DEG       = 5.0F; // §6 entry forgiveness
constexpr float XR_GRAB_PUSHPULL_SPEED = 2.0F; // m/s at full stick.y while grabbed
constexpr float XR_GRAB_RESIZE_SPEED   = 1.0F; // m/s of width at full stick.x

// A visible quad the ray pointer can hit, supplied by the frame loop each frame after the anchor
// solve (doc 04 §3). worldPose is the SOLVED quad pose expressed in the SAME reference frame as
// the sampled aim poses (device/grip-locked quads pass their world-composed pose). `anchor` is
// the live CXRAnchor for the grab machine (§6, WP8) to call beginGrab/grabPushPull/grabResize/
// endGrab on; it is a raw, frame-thread-only pointer valid strictly for the duration of the
// frame that produced this target vector (backed by the frame loop's own PXRLAYER
// snapshot) — never cached across frames.
struct SXRPointerTarget {
    MONITORID                 id = -1;
    OpenXR::SXRPose           worldPose;        // FULL quad (content + chrome margins) center pose
    float                     w = 0.F;          // FULL quad width in meters
    float                     h = 0.F;          // FULL quad height in meters
    std::string               name;             // monitor name (grab event payload / re-lookup by name)
    OpenXR::CXRAnchor*        anchor = nullptr;
    OpenXR::SXRChromeGeometry chrome;           // WP-G1: normalized chrome layout for hit classification + content-uv remap

    // WP X1/X3 (research/24 §5.6): the stereo declaration this monitor is being SUBMITTED with.
    // A paired monitor's quad shows one PANE, so a ray hit on it is a pane uv — and the pointer
    // wants a coordinate in the whole logical desktop. Without the un-map the cursor is off by half
    // a screen, which is §5.6's exact warning. The whole declaration travels rather than just the
    // layout because the un-map belongs to the PRODUCER: the CONTENT producer squeezes the uv back
    // into its half of a packed image, the DEPTH producer's panes are each the whole desktop and its
    // un-map is the identity. A default-constructed declaration makes it an identity either way.
    OpenXR::Stereo::SPairDecl stereo;
};

// Schmitt trigger for analog buttons (doc 04 §4). update() returns true on an edge.
struct SXRSchmitt {
    bool state = false;
    bool update(float v, float on, float off) {
        if (!state && v >= on) {
            state = true;
            return true;
        }
        if (state && v <= off) {
            state = false;
            return true;
        }
        return false;
    }
};

// Per-hand sampled action state, produced each frame by sample() and read by the frame loop
// (anchor solve grip poses now; ray cast / hysteresis / grab in WP7/WP8).
struct SXRHandState {
    std::optional<OpenXR::SXRPose> aim;   // aim pose in the reference space (nullopt = invalid)
    std::optional<OpenXR::SXRPose> grip;  // grip pose in the reference space (nullopt = invalid)
    std::optional<OpenXR::SXRPose> pinch; // WP-G5: pinch pose (ext/hand_interaction_ext), reference space
    float                          select = 0.f;
    float                          grab   = 0.f;      // grasp_ext (fist) / squeeze value
    float                          pinchValue = 0.f;  // WP-G5: pinch_ext strength (hands only)
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
    // runtimeLost: the runtime IPC is dead — skip the xr destroy calls (doomed IPC round-trips that
    // only spam "Broken pipe"; xrDestroyInstance in CXRSession::destroy reaps these child handles).
    void destroy(bool runtimeLost = false);

    // Frame thread. xrSyncActions + per-hand pose/analog sampling at predictedDisplayTime,
    // locating aim/grip in refSpace. Snapshots per-hand state into m_hands; the ray cast +
    // event emission happens in processPointer() once the frame loop has solved the quad poses.
    void sample(XrTime predictedDisplayTime, XrSpace refSpace);

    // Frame thread. Ray-cast every non-grabbing hand's aim pose against the solved quad targets,
    // resolve hover + the single-pointer owner (doc 04 §3), run the select/menu hysteresis (§4),
    // the grab state machine (§6, WP8 — begin/push-pull/resize/end via each target's CXRAnchor),
    // and scroll (§5), and emit MOTION_ABS / BUTTON / AXIS / FRAME / GRAB across the frame->main
    // queue. Called once per frame after the anchor solve, UNDER COpenXRManager::m_layersMu (the
    // grab machine mutates layer anchor state, same discipline as the solve loop). `timeMs` is
    // the sample-time stamp; `solveIn` carries this frame's view/grip poses (already shifted into
    // the same space the anchor solve used) and `tune` the anchor tuning, both needed by endGrab.
    // `handsEnabled` (research/16 Part A): when false, any hand whose active device is HAND tracking
    // (handActive(hand)) is fully inert — no ray/hover/cursor, no clicks, no grabs; an in-progress
    // hand grab is ended via the normal release latch. CONTROLLERS are never gated (handActive is
    // false for them), so a controller session behaves identically regardless. Computed by the
    // manager's conditional-hand-input gate (keyboard recency / roam / manual override).
    // handGrabMode / handGrabAnyMode (openxr:hand_grab / hand_grab_anywhere) are passed in already
    // parsed: the frame thread must never deref those CConfigValue<std::string>s (a concurrent reload
    // frees the backing store under it -> heap corruption, task #25). The manager reads them from
    // atomics (publishGrabStringTuning) and hands them down here.
    void processPointer(const std::vector<SXRPointerTarget>& targets, uint32_t timeMs, const OpenXR::SXRSolveInput& solveIn, const OpenXR::SXRAnchorTuning& tune,
                        OpenXR::eXRHandGrab handGrabMode, OpenXR::eXRHandGrabAnywhere handGrabAnyMode, bool handsEnabled = true);

    // Frame-thread reads of the latest sample:
    const SXRHandState& hand(OpenXR::eXRHand h) const {
        return m_hands[h];
    }
    std::optional<OpenXR::SXRPose> grip(OpenXR::eXRHand h) const {
        return m_hands[h].grip;
    }
    // WP-G5: pinch pose (ext/hand_interaction_ext), reference space. nullopt unless hands are the
    // active device and the pinch pose is valid this frame.
    std::optional<OpenXR::SXRPose> pinch(OpenXR::eXRHand h) const {
        return m_hands[h].pinch;
    }
    // Grip action space handles for the device-lock late-latch path (doc 03 §3.4). XR_NULL_HANDLE
    // when input is unavailable.
    XrSpace gripSpace(OpenXR::eXRHand h) const {
        return m_gripSpace[h];
    }
    // WP-G5: pinch pose action space for a pinch-anchored hand MOVE grab's late-latch. XR_NULL_HANDLE
    // when XR_EXT_hand_interaction is absent or the space failed to create.
    XrSpace pinchSpace(OpenXR::eXRHand h) const {
        return m_pinchSpace[h];
    }

    // WP-G5 active-device detection: the current interaction-profile KIND for a hand, cached from
    // xrGetCurrentInteractionProfile (refreshed on XrEventDataInteractionProfileChanged). Frame
    // thread writes it in sample(); handInputKind() also reads a main-thread-safe atomic mirror for
    // `hyprctl openxr status`. handActive(h) == true iff that hand is on ext/hand_interaction_ext.
    bool                handActive(OpenXR::eXRHand h) const {
        return m_handActive[h];
    }
    OpenXR::eXRInputKind handInputKind(OpenXR::eXRHand h) const {
        return (OpenXR::eXRInputKind)m_handInputKindAtomic[h].load(std::memory_order_acquire);
    }

    // Main thread (frame loop forwards the session's XrEventDataInteractionProfileChanged). Marks the
    // per-hand profile cache dirty so the next sample() re-reads xrGetCurrentInteractionProfile.
    void notifyInteractionProfileChanged() {
        m_profileDirty.store(true, std::memory_order_release);
    }

    // Frame-thread READ-ONLY exports for the WP-G2 chrome draw pass. These only surface state that
    // processPointer already resolved (m_hoverRegion/m_hoverChromeMon/m_grabbedMon) — they do NOT
    // touch the pointer/grab machine, so they respect the WP-G3 ownership boundary. Read on the
    // frame thread right after processPointer.
    //   chromeHoverRegion(id): the region a hand's ray currently classifies on monitor `id`
    //     (XR_REGION_NONE if neither hand hovers it). If both hands hover it, the higher-precedence
    //     region wins (corner/bar over body/margin) so a resize/move affordance highlights.
    //   isMonitorGrabbed(id):  whether either hand currently grabs monitor `id`.
    OpenXR::eXRQuadRegion chromeHoverRegion(MONITORID id) const;
    bool                  isMonitorGrabbed(MONITORID id) const;

    // Frame-thread READ-ONLY endpoint-cursor exports (report 14 Stage A1). processPointer resolves,
    // per hand, which quad the ray hit (cursorMon, -1 = none), the RAW full-quad hit uv (cursorUV),
    // and the cursor STATE (idle/grabbable/press/grab). The frame loop packs these onto the hovered
    // layer's per-hand atomic (XRMonitorLayer m_cursorPacked) for next frame's drawCursor pass —
    // the same one-frame-latency contract as chromeHoverRegion. Read right after processPointer.
    MONITORID            cursorMon(OpenXR::eXRHand h) const {
        return m_cursorMon[h];
    }
    Vector2D             cursorUV(OpenXR::eXRHand h) const {
        return m_cursorUV[h];
    }
    OpenXR::eXRCursorState cursorState(OpenXR::eXRHand h) const {
        return m_cursorState[h];
    }

    // Frame thread (set on the main thread before the frame thread starts). Producer sink for
    // frame->main events (queue push + eventfd wake, owned by COpenXRManager).
    void setEmitter(std::function<void(XRQueueItem)> emit) {
        m_emit = std::move(emit);
    }

  private:
    bool suggestBindings();
    bool createActionSpaces();
    void logInteractionProfileOnce();
    // WP-G5: re-read each hand's current interaction profile (xrGetCurrentInteractionProfile) and
    // update m_handActive + the status atomic. Called from sample() when m_profileDirty.
    void refreshHandProfiles();
    // Fire-and-forget haptic tick on a hand (doc 04 §6.3). No-op / ignored if the current
    // profile has no haptic binding (hands have no actuator — graceful). `amplitude` is 0-1
    // (openxr:haptic_amplitude); the master openxr:haptics gate is checked at the call site.
    void                             hapticTick(OpenXR::eXRHand hand, float amplitude);

    std::function<void(XRQueueItem)> m_emit;

    XrInstance                       m_instance           = XR_NULL_HANDLE;
    XrSession                        m_session            = XR_NULL_HANDLE;
    bool                             m_hasHandInteraction = false;

    XrActionSet                      m_actionSet       = XR_NULL_HANDLE;
    XrAction                         m_aimAction       = XR_NULL_HANDLE;
    XrAction                         m_gripAction      = XR_NULL_HANDLE;
    XrAction                         m_selectAction    = XR_NULL_HANDLE;
    XrAction                         m_grabAction      = XR_NULL_HANDLE;
    XrAction                         m_scrollAction    = XR_NULL_HANDLE;
    XrAction                         m_menuAction      = XR_NULL_HANDLE;
    XrAction                         m_hapticAction    = XR_NULL_HANDLE;
    // WP-G5: hand pinch (ext/hand_interaction_ext). pinch_ext/value drives the hand grab gesture;
    // pinch_ext/pose is the stable MOVE-grab anchor. Both created only when hasHandInteraction.
    XrAction                         m_pinchValueAction = XR_NULL_HANDLE;
    XrAction                         m_pinchPoseAction  = XR_NULL_HANDLE;

    std::array<XrPath, 2>            m_handPath{XR_NULL_PATH, XR_NULL_PATH};
    std::array<XrSpace, 2>           m_aimSpace{XR_NULL_HANDLE, XR_NULL_HANDLE};
    std::array<XrSpace, 2>           m_gripSpace{XR_NULL_HANDLE, XR_NULL_HANDLE};
    std::array<XrSpace, 2>           m_pinchSpace{XR_NULL_HANDLE, XR_NULL_HANDLE};

    std::array<SXRHandState, 2>      m_hands;

    // WP-G5 active-device cache (frame thread). m_handActive[h] is true iff hand h's current
    // interaction profile is ext/hand_interaction_ext; refreshed in sample() when m_profileDirty
    // (set at attach + on XrEventDataInteractionProfileChanged via notifyInteractionProfileChanged).
    // m_handInputKindAtomic mirrors the kind for the main-thread status read.
    std::array<bool, 2>                    m_handActive{false, false};
    std::array<std::atomic<uint8_t>, 2>    m_handInputKindAtomic{}; // OpenXR::eXRInputKind
    std::atomic<bool>                      m_profileDirty{true};

    bool                             m_attached      = false;
    bool                             m_profileLogged = false; // interaction profile logged once for debuggability

    // ---- ray pointer state (frame thread, doc 04 §3-§5) ----
    std::array<SXRSchmitt, 2> m_selectTrig;       // per-hand select hysteresis
    std::array<SXRSchmitt, 2> m_menuTrig;         // per-hand menu press/release (bool via 0.5/0.5)
    std::array<MONITORID, 2>  m_hoverMon{-1, -1}; // current BODY-hovered monitor per hand (-1 = none); drives motion/click/scroll
    std::array<Vector2D, 2>   m_hoverUV;          // current BODY-hover uv per hand, REMAPPED to content uv (WP-G1)
    // WP-G1 chrome bookkeeping: the region + quad the ray last classified for each hand, over the
    // FULL quad (incl. transparent margins). Distinct from m_hoverMon (body only) — non-body hits
    // are hover-only (no pointer events; WP-G2 will drive chrome visuals from this). Frame thread.
    std::array<MONITORID, 2>          m_hoverChromeMon{-1, -1};
    std::array<OpenXR::eXRQuadRegion, 2> m_hoverRegion{OpenXR::XR_REGION_NONE, OpenXR::XR_REGION_NONE};
    // While a hand grabs, it casts no ray, drives no pointer, and its stick feeds the grab
    // machine instead of scroll (doc 04 §2/§6).
    std::array<bool, 2> m_grabbing{false, false};

    // ---- ray aim / cursor / hover assist (report 14, frame thread) ----
    // Aim-pose 1€ filter per hand (openxr:aim_filter, Stage B); reset when the aim goes invalid so a
    // re-acquire doesn't jump. Sticky-hover Schmitt per hand (Stage A2); the region it publishes is
    // what m_hoverRegion carries (so chrome highlight + grab eligibility are both stabilized).
    std::array<OpenXR::SXROneEuroPose, 2> m_aimFilter;
    std::array<OpenXR::SXRHoverStick, 2>  m_hoverStick;
    std::array<bool, 2>                   m_prevHoverGrabbable{false, false}; // hover-enter haptic edge
    // Endpoint-cursor export (read by the frame loop, published onto the hovered layer's atomics).
    std::array<MONITORID, 2>              m_cursorMon{-1, -1};
    std::array<Vector2D, 2>               m_cursorUV;
    std::array<OpenXR::eXRCursorState, 2> m_cursorState{OpenXR::XR_CURSOR_HIDDEN, OpenXR::XR_CURSOR_HIDDEN};

    // ---- grab state machine (frame thread, doc 04 §6, WP8) ----
    std::array<SXRSchmitt, 2>  m_grabTrig;           // per-hand grab (squeeze) hysteresis
    std::array<MONITORID, 2>   m_grabbedMon{-1, -1}; // monitor id each hand currently grabs
    std::array<std::string, 2> m_grabbedMonName;     // its name, captured at grab-begin (event payload)
    // WP-G3: what the grab is doing per hand — a bar/body MOVE (rigid grip carry + release latch) or
    // a CORNER resize (scale about the pinned opposite corner). Decides the per-frame drive (stick
    // push-pull/resize for MOVE vs grabResizeCorner for RESIZE) and the release path (endGrab vs
    // endResize). The ring holds carried QUAD poses for MOVE, GRIP poses for RESIZE (both latched).
    std::array<OpenXR::eXRGrabKind, 2> m_grabKind{OpenXR::XR_GRABKIND_NONE, OpenXR::XR_GRABKIND_NONE};
    // WP-G5: whether each hand's active grab is anchored to the pinch pose (hands) vs the grip pose
    // (controllers/grasp). Drives which device pose feeds beginGrab/per-frame carry/resize + the
    // release latch, so begin and every subsequent frame use a consistent device space.
    std::array<bool, 2> m_grabDevicePinch{false, false};

    // ---- release-latching (WP-G4, research 04-grabbable-borders.md §5.4) ----
    // Per-hand ring of the carried quad's world pose, pushed every grabbed frame. On the release
    // edge the reanchor uses a pose from ~openxr:grab_release_latency_ms before the edge (and/or
    // the last calm sample) instead of the perturbed release-frame pose. POD, frame-thread-only —
    // zero hyprutils refcount ops (XRMonitorLayer.hpp rule). Gesture-agnostic: it hooks the grab
    // machine's release edge whatever gesture (squeeze / grasp / future pinch) drove it.
    std::array<OpenXR::SXRGrabRing, 2> m_grabRing;

    int                        m_owner      = -1;  // hand that owns the single pointer (-1 = none)
    MONITORID                  m_emittedMon = -1;  // last MOTION_ABS monitor emitted (coalescing, -1 = hover clear)
    Vector2D                   m_emittedUV;        // last MOTION_ABS uv emitted
    int                        m_leftHolder  = -1; // hand currently holding BTN_LEFT  (single pointer, -1 = none)
    int                        m_rightHolder = -1; // hand currently holding BTN_RIGHT (single pointer, -1 = none)
};

#endif // HAVE_OPENXR
