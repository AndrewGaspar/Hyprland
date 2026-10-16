#pragma once
#ifdef HAVE_OPENXR

// XRPlaneDetection — research/32 C1: the XR_EXT_plane_detection consumer, one IXRPlaneSource.
//
// MAIN THREAD ONLY. The begin -> state -> get cycle is spread over poll() calls (one step each, at
// the surfaces poll cadence, ~1-2 Hz), so no call waits on a detection; each step is a single short
// runtime IPC. Every completed detection is RE-LOCATED on every poll (xrGetPlaneDetectionsEXT
// locates against the base space at the time it is given), which is what keeps the planes in the
// current reference space across a recenter without this class knowing about recenters at all.
//
// Monado gates xrCreatePlaneDetectorEXT on the head xdev's `supported.planes` and answers
// XR_ERROR_FEATURE_UNSUPPORTED when the headset has no plane source — that is an expected,
// permanent answer for the session and is reported as such, never retried.

#include <openxr/openxr.h>

#include <cstdint>
#include <string>
#include <vector>

#include "XRPlaneSource.hpp"

class CXRPlaneDetectionSource : public OpenXR::IXRPlaneSource {
  public:
    // `enabled`: the extension was requested AND advertised AND enabled on the instance;
    // `features` is XrSystemPlaneDetectionPropertiesEXT::supportedFeatures. The handles are the
    // session's (non-owning); the manager destroys this source before the session.
    CXRPlaneDetectionSource(XrInstance instance, XrSession session, XrSpace refSpace, bool enabled, bool advertised, uint64_t features);
    ~CXRPlaneDetectionSource() override;

    // Teardown (main thread, frame thread joined). runtimeLost skips the doomed destroy IPC; the
    // instance destroy reaps the handle.
    void                 shutdown(bool runtimeLost) override;

    const char*          name() const override;
    OpenXR::eXRPlanePoll poll(const OpenXR::SXRPlanePollContext& ctx, std::vector<OpenXR::SXRPlane>& out) override;
    std::string          status() const override;

  private:
    enum eDetState : uint8_t {
        DET_OFF = 0,     // not enabled / not advertised / not compiled: never produces planes
        DET_UNSUPPORTED, // the runtime said this headset has no plane detection (FEATURE_UNSUPPORTED)
        DET_FAILED,      // a hard failure; given up for this session
        DET_IDLE,        // detector exists, no detection running
        DET_PENDING,     // a detection was begun; polling its state
        DET_READY,       // a detection is DONE; re-located on every poll until the next one begins
    };

    bool        ensureDetector();
    bool        begin(const OpenXR::SXRPlanePollContext& ctx);
    bool        fetch(const OpenXR::SXRPlanePollContext& ctx, std::vector<OpenXR::SXRPlane>& out);
    void        fail(eDetState state, const std::string& why);

    XrInstance  m_instance   = XR_NULL_HANDLE;
    XrSession   m_session    = XR_NULL_HANDLE;
    XrSpace     m_refSpace   = XR_NULL_HANDLE;
    bool        m_advertised = false;
    uint64_t    m_features   = 0;
    eDetState   m_state      = DET_OFF;
    std::string m_why; // the last state change's reason, for status()
    uint32_t    m_generation = 0;
    uint32_t    m_readyPolls = 0; // polls since the current detection completed
    uint32_t    m_errors     = 0; // consecutive ERROR states / failed calls
    size_t      m_lastCount  = 0;
    bool        m_contour    = false;
    bool        m_swapLogged = false;

#ifdef XR_EXT_plane_detection
    XrPlaneDetectorEXT                      m_detector   = XR_NULL_HANDLE;
    PFN_xrCreatePlaneDetectorEXT            m_pfnCreate  = nullptr;
    PFN_xrDestroyPlaneDetectorEXT           m_pfnDestroy = nullptr;
    PFN_xrBeginPlaneDetectionEXT            m_pfnBegin   = nullptr;
    PFN_xrGetPlaneDetectionStateEXT         m_pfnState   = nullptr;
    PFN_xrGetPlaneDetectionsEXT             m_pfnGet     = nullptr;
    PFN_xrGetPlanePolygonBufferEXT          m_pfnPolygon = nullptr;
    std::vector<XrPlaneDetectorLocationEXT> m_locations;
    std::vector<XrVector2f>                 m_vertices;
#endif
};

#endif // HAVE_OPENXR
