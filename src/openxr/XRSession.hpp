#pragma once
#ifdef HAVE_OPENXR

#include <openxr/openxr.h> // XrInstance, XrSystemId, XrSession, XrSpace, XrSessionState, ...
#include <cstdint>
#include <string>
#include <vector>

#include "XRMath.hpp"          // OpenXR::SXRPose (pure math) — conversion helpers below
#include "XRMonitorConfig.hpp" // OpenXR::eXRBlendMode (unconditional) — blend-mode conversions below

class CXRGraphics;

// XrEnvironmentBlendMode <-> the unconditional OpenXR::eXRBlendMode mirror (doc 01). Kept here
// (guarded, alongside the XrPosef conversions) so XRMonitorConfig can stay OpenXR-header-free.
inline OpenXR::eXRBlendMode xrBlendModeFromXr(XrEnvironmentBlendMode m) {
    switch (m) {
        case XR_ENVIRONMENT_BLEND_MODE_OPAQUE: return OpenXR::XR_BLEND_OPAQUE;
        case XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND: return OpenXR::XR_BLEND_ALPHA;
        case XR_ENVIRONMENT_BLEND_MODE_ADDITIVE: return OpenXR::XR_BLEND_ADDITIVE;
        default: return OpenXR::XR_BLEND_OPAQUE;
    }
}
inline XrEnvironmentBlendMode xrBlendModeToXr(OpenXR::eXRBlendMode m) {
    switch (m) {
        case OpenXR::XR_BLEND_OPAQUE: return XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        case OpenXR::XR_BLEND_ALPHA: return XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND;
        case OpenXR::XR_BLEND_ADDITIVE: return XR_ENVIRONMENT_BLEND_MODE_ADDITIVE;
    }
    return XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
}

// Memberwise XrPosef <-> SXRPose conversions live in the session-side code (doc 03 §0), not in
// XRMath.hpp (which must stay OpenXR-header-free).
inline OpenXR::SXRPose xrToPose(const XrPosef& p) {
    return {{p.position.x, p.position.y, p.position.z}, {p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w}};
}
inline XrPosef xrFromPose(const OpenXR::SXRPose& p) {
    XrPosef o;
    o.position    = {p.pos.x, p.pos.y, p.pos.z};
    o.orientation = {p.rot.x, p.rot.y, p.rot.z, p.rot.w};
    return o;
}

// CXRSession owns the OpenXR instance/system/session, the reference + view spaces, and
// the XR event pump that drives the session state machine. See doc 01.
//
// Threading: ALL creation (createInstance/getSystem/createSession/createSpaces) runs on
// the MAIN thread inside COpenXRManager::start(), before the frame thread exists.
// pollEvents() runs on the FRAME thread. destroy() runs on the main thread after the
// frame thread has been joined.
class CXRSession {
  public:
    CXRSession() = default;
    ~CXRSession();

    bool               createInstance();                // false => UNAVAILABLE (no runtime / missing required ext)
    bool               getSystem();                     // xrGetSystem + xrGetSystemProperties + enumerate blend modes
    bool               createSession(CXRGraphics& gfx); // XrGraphicsBindingEGLMNDX
    bool               createSpaces(CXRGraphics& gfx);  // reference space (LOCAL_FLOOR/LOCAL) + VIEW space
    bool               chooseSwapchainFormat();         // enumerate + pick once; stored on m_swapchainFormat
    // Teardown ordering (doc 01): spaces, session, instance. When runtimeLost is true (the runtime IPC
    // is already dead — a killed/restarted monado-service), the per-child xr destroy calls (spaces,
    // session) are SKIPPED: each would be a doomed IPC round-trip that only spams "Broken pipe" and,
    // on a wedged runtime, could block. The child handles are nulled locally and xrDestroyInstance is
    // still called (it frees loader-side state + the socket fd and implicitly reaps the child handles).
    void               destroy(bool runtimeLost = false);

    void               pollEvents(); // FRAME thread: XR event pump + session state machine

    // Latch "the runtime IPC is dead" from ANY xr call that returns a loss code (pollEvents, the frame
    // loop's wait/begin, a bounded swapchain wait). Idempotent + once-logged. Sets m_exitRequested so
    // the frame loop breaks at the top of its next iteration and the main thread tears down to
    // UNAVAILABLE (+ reprobe). Frame-thread only (matches the existing m_exitRequested/m_instanceLost
    // writers); the main thread reads the flags after the frame thread is joined.
    void               markRuntimeLost(const char* why, int xrResult);

    const std::string& runtimeName() const {
        return m_runtimeName;
    }
    const std::string& systemName() const {
        return m_systemName;
    }

    XrInstance m_instance        = XR_NULL_HANDLE;
    XrSystemId m_systemId        = XR_NULL_SYSTEM_ID;
    XrSession  m_session         = XR_NULL_HANDLE;
    XrSpace    m_refSpace        = XR_NULL_HANDLE; // LOCAL_FLOOR or LOCAL
    XrSpace    m_viewSpace       = XR_NULL_HANDLE; // VIEW
    bool       m_usingLocalFloor = false;
    bool       m_hasLocalFloor = false, m_hasHandInteraction = false, m_hasHandTracking = false;
    // XR_KHR_vulkan_enable2 advertised AND enabled (probe-only, for the wrong-GPU guard in start()).
    bool       m_hasVulkanEnable2 = false;

    // XR_EXT_user_presence (report-19): the real donned/doffed signal. m_hasUserPresence = extension
    // advertised AND enabled; m_supportsUserPresence = the system property (a runtime may enable the
    // ext but the device not support presence — e.g. Monado's null/remote drivers). Read on the main
    // thread at start(); the manager gates `visible`-mode plugging on presence when supported.
    bool       m_hasUserPresence      = false;
    bool       m_supportsUserPresence = false;

    // report-20 issue B1: set by getSystem() when xrGetSystem fails with XR_ERROR_FORM_FACTOR_UNAVAILABLE
    // — the spec's "runtime up, headset not connected/donned, poll me later" result. Lets the manager's
    // dormant re-probe distinguish "waiting for the runtime" from "waiting for the headset". Reset to
    // false on a successful getSystem().
    bool       m_formFactorUnavailable = false;

    // Overlay session (XR_EXTX_overlay, doc 01). COpenXRManager::start() sets m_overlayRequested /
    // m_overlayZ from openxr:overlay / openxr:overlay_z BEFORE createInstance(). m_hasOverlay records
    // whether the runtime advertises the extension; m_isOverlay is the ACTUAL state — true only when
    // overlay was requested AND supported, i.e. an XrSessionCreateInfoOverlayEXTX was chained into
    // xrCreateSession. Requested-but-unsupported downgrades to a normal session (one-time WARN).
    bool       m_overlayRequested = false;
    uint32_t   m_overlayZ         = 1; // sessionLayersPlacement; higher composites later / on top
    bool       m_hasOverlay       = false;
    bool       m_isOverlay        = false;

    // Frame-thread-only after start().
    XrSessionState m_xrState       = XR_SESSION_STATE_UNKNOWN;
    bool           m_sessionBegan  = false;
    bool           m_exitRequested = false; // set on EXITING / LOSS_PENDING / instance loss
    bool           m_instanceLost  = false; // set on LOSS_PENDING / XrEventDataInstanceLossPending / any loss code
    bool           m_lostLogged    = false; // markRuntimeLost() logs the loss WARN exactly once per session
    // WP-G5: set by pollEvents on XrEventDataInteractionProfileChanged; the frame loop forwards it
    // to CXRInput (re-read xrGetCurrentInteractionProfile) then clears it. Frame-thread only.
    bool           m_interactionProfileChanged = false;
    // report-19: set by pollEvents on XrEventDataUserPresenceChangedEXT; the frame loop forwards the
    // new presence to main (SXRStateEvent USER_PRESENCE) then clears the flag. Frame-thread only.
    bool           m_userPresenceChanged = false;
    bool           m_userPresent         = false; // last isUserPresent seen (frame thread)

    uint32_t       m_maxLayerCount   = 16; // XrSystemGraphicsProperties::maxLayerCount (spec floor 16)
    int64_t        m_swapchainFormat = 0;  // chosen once after session creation

    // Environment blend modes (doc 01). m_blendModes is the runtime's advertised list in
    // preference order (xrEnumerateEnvironmentBlendModes, preferred-first), enumerated once in
    // getSystem(); it feeds OpenXR::pickBlendMode. m_blendMode is the selected mode submitted in
    // every xrEndFrame — chosen from openxr:blend_mode by COpenXRManager::start() at session start.
    std::vector<OpenXR::eXRBlendMode> m_blendModes = {OpenXR::XR_BLEND_OPAQUE};
    XrEnvironmentBlendMode            m_blendMode  = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;

    // Recenter (XrEventDataReferenceSpaceChangePending, doc 03 §6). Set on the frame thread by
    // pollEvents when the runtime recenters our reference space; consumed + cleared by the frame
    // loop, which applies it to every anchor. m_recenterPose is the new origin in the old space.
    bool            m_recenterPending   = false;
    bool            m_recenterPoseValid = false;
    OpenXR::SXRPose m_recenterPose;

  private:
    std::string m_runtimeName;
    std::string m_systemName;
};

#endif // HAVE_OPENXR
