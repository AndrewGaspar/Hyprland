#pragma once
#ifdef HAVE_OPENXR

#include <openxr/openxr.h> // XrInstance, XrSystemId, XrSession, XrSpace, XrSessionState, ...
#include <cstdint>
#include <string>

#include "XRMath.hpp" // OpenXR::SXRPose (pure math) — conversion helpers below

class CXRGraphics;

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
    bool               getSystem();                     // xrGetSystem + xrGetSystemProperties
    bool               createSession(CXRGraphics& gfx); // XrGraphicsBindingEGLMNDX
    bool               createSpaces(CXRGraphics& gfx);  // reference space (LOCAL_FLOOR/LOCAL) + VIEW space
    bool               chooseSwapchainFormat();         // enumerate + pick once; stored on m_swapchainFormat
    void               destroy();                       // spaces, session, instance (teardown ordering — doc 01)

    void               pollEvents(); // FRAME thread: XR event pump + session state machine

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

    // Frame-thread-only after start().
    XrSessionState m_xrState       = XR_SESSION_STATE_UNKNOWN;
    bool           m_sessionBegan  = false;
    bool           m_exitRequested = false; // set on EXITING / LOSS_PENDING / instance loss
    bool           m_instanceLost  = false; // set on LOSS_PENDING / XrEventDataInstanceLossPending

    uint32_t       m_maxLayerCount   = 16; // XrSystemGraphicsProperties::maxLayerCount (spec floor 16)
    int64_t        m_swapchainFormat = 0;  // chosen once after session creation

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
