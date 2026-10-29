#ifdef HAVE_OPENXR

#include "XRPlaneDetection.hpp"
#include "XRSession.hpp"
#include "../debug/log/Logger.hpp"

#include <format>

#include <hyprutils/memory/Casts.hpp>

using Hyprutils::Memory::rc;
using Hyprutils::Memory::sc;

// A completed detection is re-located on every poll; a NEW detection (which can find new planes and
// drop stale ones) is begun after this many re-locations. At the ~750 ms default cadence that is a
// fresh detection every ~6 s, which is plenty for furniture.
static constexpr uint32_t DET_REDETECT_POLLS = 8;
// A runtime that keeps answering ERROR is not going to start answering DONE.
static constexpr uint32_t DET_MAX_ERRORS = 5;
// Below this a detected patch is noise for the purpose of putting a window on it (m^2).
static constexpr float DET_MIN_AREA = 0.04F;
// The detection volume: centred on the head, generous enough for a room.
static constexpr float DET_BOX_EXTENT = 10.F;

CXRPlaneDetectionSource::CXRPlaneDetectionSource(XrInstance instance, XrSession session, XrSpace refSpace, bool enabled, bool advertised, uint64_t features) :
    m_instance(instance), m_session(session), m_refSpace(refSpace), m_advertised(advertised), m_features(features) {
#ifdef XR_EXT_plane_detection
    if (!enabled) {
        m_why =
            advertised ? "advertised but not enabled (openxr:surfaces_detect was off at session start)" : "the runtime does not advertise XR_EXT_plane_detection";
        return;
    }
    if (!(features & XR_PLANE_DETECTION_CAPABILITY_PLANE_DETECTION_BIT_EXT)) {
        m_state = DET_UNSUPPORTED;
        m_why   = std::format("the system reports no plane detection capability (supportedFeatures 0x{:x})", features);
        return;
    }
    auto load = [&](const char* fn, auto& out) {
        if (XR_FAILED(xrGetInstanceProcAddr(m_instance, fn, rc<PFN_xrVoidFunction*>(&out))) || !out)
            out = nullptr;
        return out != nullptr;
    };
    const bool ok = load("xrCreatePlaneDetectorEXT", m_pfnCreate) && load("xrDestroyPlaneDetectorEXT", m_pfnDestroy) && load("xrBeginPlaneDetectionEXT", m_pfnBegin) &&
        load("xrGetPlaneDetectionStateEXT", m_pfnState) && load("xrGetPlaneDetectionsEXT", m_pfnGet) && load("xrGetPlanePolygonBufferEXT", m_pfnPolygon);
    if (!ok) {
        m_state = DET_FAILED;
        m_why   = "the runtime enabled XR_EXT_plane_detection but did not resolve its functions";
        return;
    }
    m_state = DET_IDLE;
    m_why   = "ready";
#else
    m_why = "this build's OpenXR headers predate XR_EXT_plane_detection";
#endif
}

CXRPlaneDetectionSource::~CXRPlaneDetectionSource() {
    shutdown(false);
}

void CXRPlaneDetectionSource::shutdown(bool runtimeLost) {
#ifdef XR_EXT_plane_detection
    if (m_detector != XR_NULL_HANDLE && !runtimeLost && m_pfnDestroy)
        m_pfnDestroy(m_detector);
    m_detector = XR_NULL_HANDLE;
#endif
    if (m_state >= DET_IDLE)
        m_state = DET_OFF;
}

const char* CXRPlaneDetectionSource::name() const {
    return "plane_detection";
}

void CXRPlaneDetectionSource::fail(eDetState state, const std::string& why) {
    m_state = state;
    m_why   = why;
    LOG(state == DET_UNSUPPORTED ? Log::DEBUG : Log::WARN, "[OPENXR] surfaces: XR_EXT_plane_detection {} — {}", state == DET_UNSUPPORTED ? "unsupported" : "disabled", why);
}

std::string CXRPlaneDetectionSource::status() const {
    switch (m_state) {
        case DET_OFF: return std::format("off ({})", m_why);
        case DET_UNSUPPORTED: return std::format("unsupported ({})", m_why);
        case DET_FAILED: return std::format("failed ({})", m_why);
        case DET_IDLE: return "idle";
        case DET_PENDING: return "detecting";
        case DET_READY: return std::format("tracking {} plane(s), generation {}{}", m_lastCount, m_generation, m_contour ? ", contours" : "");
    }
    return "?";
}

#ifdef XR_EXT_plane_detection

static OpenXR::eXRPlaneOrient orientFromXr(XrPlaneDetectorOrientationEXT o) {
    switch (o) {
        case XR_PLANE_DETECTOR_ORIENTATION_HORIZONTAL_UPWARD_EXT: return OpenXR::XR_PLANE_UP;
        case XR_PLANE_DETECTOR_ORIENTATION_HORIZONTAL_DOWNWARD_EXT: return OpenXR::XR_PLANE_DOWN;
        case XR_PLANE_DETECTOR_ORIENTATION_VERTICAL_EXT: return OpenXR::XR_PLANE_VERTICAL;
        default: return OpenXR::XR_PLANE_ARBITRARY;
    }
}

static OpenXR::eXRPlaneLabel labelFromXr(XrPlaneDetectorSemanticTypeEXT s) {
    switch (s) {
        case XR_PLANE_DETECTOR_SEMANTIC_TYPE_CEILING_EXT: return OpenXR::XR_PLANE_LABEL_CEILING;
        case XR_PLANE_DETECTOR_SEMANTIC_TYPE_FLOOR_EXT: return OpenXR::XR_PLANE_LABEL_FLOOR;
        case XR_PLANE_DETECTOR_SEMANTIC_TYPE_WALL_EXT: return OpenXR::XR_PLANE_LABEL_WALL;
        case XR_PLANE_DETECTOR_SEMANTIC_TYPE_PLATFORM_EXT: return OpenXR::XR_PLANE_LABEL_PLATFORM;
        default: return OpenXR::XR_PLANE_LABEL_UNKNOWN;
    }
}

bool CXRPlaneDetectionSource::ensureDetector() {
    if (m_detector != XR_NULL_HANDLE)
        return true;
    // Contours when the runtime can produce them; the extents rectangle otherwise.
    XrPlaneDetectorCreateInfoEXT info{XR_TYPE_PLANE_DETECTOR_CREATE_INFO_EXT};
    m_contour  = true;
    info.flags = XR_PLANE_DETECTOR_ENABLE_CONTOUR_BIT_EXT;
    XrResult r = m_pfnCreate(m_session, &info, &m_detector);
    if (r == XR_ERROR_VALIDATION_FAILURE) {
        m_contour  = false;
        info.flags = 0;
        r          = m_pfnCreate(m_session, &info, &m_detector);
    }
    if (r == XR_ERROR_FEATURE_UNSUPPORTED) {
        m_detector = XR_NULL_HANDLE;
        fail(DET_UNSUPPORTED, "xrCreatePlaneDetectorEXT: XR_ERROR_FEATURE_UNSUPPORTED — this headset has no plane source");
        return false;
    }
    if (XR_FAILED(r)) {
        m_detector = XR_NULL_HANDLE;
        fail(DET_FAILED, std::format("xrCreatePlaneDetectorEXT failed ({})", sc<int>(r)));
        return false;
    }
    LOG(Log::DEBUG, "[OPENXR] surfaces: plane detector created ({})", m_contour ? "with contours" : "extents only");
    return true;
}

bool CXRPlaneDetectionSource::begin(const OpenXR::SXRPlanePollContext& ctx) {
    XrPlaneDetectorBeginInfoEXT info{XR_TYPE_PLANE_DETECTOR_BEGIN_INFO_EXT};
    info.baseSpace         = m_refSpace;
    info.time              = ctx.xrTime;
    info.orientationCount  = 0; // every orientation: walls are cheap to ignore, and labels come along
    info.semanticTypeCount = 0;
    info.maxPlanes         = sc<uint32_t>(OpenXR::XR_PLANE_MAX);
    info.minArea           = DET_MIN_AREA;
    // The box is named in the base space, so undo the LOCAL fallback's floor shift on the head.
    OpenXR::SXRPose box;
    if (ctx.headValid)
        box.pos = OpenXR::Vec3{ctx.head.x, ctx.head.y - ctx.floorShift, ctx.head.z};
    info.boundingBoxPose   = xrFromPose(box);
    info.boundingBoxExtent = {DET_BOX_EXTENT, DET_BOX_EXTENT, DET_BOX_EXTENT};

    const XrResult r = m_pfnBegin(m_detector, &info);
    if (r == XR_ERROR_FEATURE_UNSUPPORTED) {
        fail(DET_UNSUPPORTED, "xrBeginPlaneDetectionEXT: XR_ERROR_FEATURE_UNSUPPORTED");
        return false;
    }
    if (XR_FAILED(r)) {
        // SPACE_NOT_LOCATABLE (no head pose yet) and friends: try again next poll, give up eventually
        if (++m_errors >= DET_MAX_ERRORS)
            fail(DET_FAILED, std::format("xrBeginPlaneDetectionEXT keeps failing ({})", sc<int>(r)));
        return false;
    }
    m_state = DET_PENDING;
    return true;
}

bool CXRPlaneDetectionSource::fetch(const OpenXR::SXRPlanePollContext& ctx, std::vector<OpenXR::SXRPlane>& out) {
    XrPlaneDetectorGetInfoEXT info{XR_TYPE_PLANE_DETECTOR_GET_INFO_EXT};
    info.baseSpace = m_refSpace;
    info.time      = ctx.xrTime;

    XrPlaneDetectorLocationsEXT locs{XR_TYPE_PLANE_DETECTOR_LOCATIONS_EXT};
    XrResult                    r = m_pfnGet(m_detector, &info, &locs);
    if (XR_FAILED(r))
        return false;
    m_locations.assign(locs.planeLocationCountOutput, XrPlaneDetectorLocationEXT{XR_TYPE_PLANE_DETECTOR_LOCATION_EXT});
    locs.planeLocationCapacityInput = sc<uint32_t>(m_locations.size());
    locs.planeLocations             = m_locations.data();
    r                               = m_pfnGet(m_detector, &info, &locs);
    if (XR_FAILED(r))
        return false;
    m_locations.resize(std::min<size_t>(locs.planeLocationCountOutput, m_locations.size()));

    out.clear();
    for (const auto& l : m_locations) {
        if (out.size() >= OpenXR::XR_PLANE_MAX)
            break;
        constexpr XrSpaceLocationFlags VALID = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        if ((l.locationFlags & VALID) != VALID)
            continue;

        // The outer contour (polygon buffer 0), when there is one.
        m_vertices.clear();
        if (l.polygonBufferCount > 0) {
            XrPlaneDetectorPolygonBufferEXT buf{XR_TYPE_PLANE_DETECTOR_POLYGON_BUFFER_EXT};
            if (XR_SUCCEEDED(m_pfnPolygon(m_detector, l.planeId, 0, &buf)) && buf.vertexCountOutput > 0) {
                m_vertices.resize(buf.vertexCountOutput);
                buf.vertexCapacityInput = sc<uint32_t>(m_vertices.size());
                buf.vertices            = m_vertices.data();
                if (XR_FAILED(m_pfnPolygon(m_detector, l.planeId, 0, &buf)))
                    m_vertices.clear();
                else
                    m_vertices.resize(std::min<size_t>(buf.vertexCountOutput, m_vertices.size()));
            }
        }
        std::vector<OpenXR::SXRVec2> poly;
        poly.reserve(m_vertices.size());
        for (const auto& v : m_vertices)
            poly.push_back(OpenXR::SXRVec2{v.x, v.y});
        // A contour too detailed for the fixed record is not a contour we can test against honestly:
        // fall back to the extents rectangle rather than truncating it into a different shape.
        if (poly.size() > OpenXR::XR_PLANE_MAX_VERTS)
            poly.clear();

        bool            swapped = false;
        OpenXR::SXRPose pose    = xrToPose(l.pose);
        pose.pos.y += ctx.floorShift;
        if (!OpenXR::xrPoseFinite(pose))
            continue;
        OpenXR::SXRPlane p = OpenXR::xrCanonicalPlane(pose, orientFromXr(l.orientation), poly.data(), poly.size(), l.extents.width, l.extents.height, swapped);
        if (swapped && !m_swapLogged) {
            m_swapLogged = true;
            LOG(Log::DEBUG, "[OPENXR] surfaces: the runtime's plane poses use a +Y normal; re-expressing them as +Z (logged once)");
        }
        p.id         = l.planeId;
        p.label      = labelFromXr(l.semanticType);
        p.source     = OpenXR::XR_PLANE_SRC_DETECTION;
        p.generation = m_generation;
        out.push_back(p);
    }
    m_lastCount = out.size();
    return true;
}

OpenXR::eXRPlanePoll CXRPlaneDetectionSource::poll(const OpenXR::SXRPlanePollContext& ctx, std::vector<OpenXR::SXRPlane>& out) {
    if (m_state == DET_OFF || m_state == DET_UNSUPPORTED || m_state == DET_FAILED)
        return OpenXR::XR_PLANE_POLL_UNAVAILABLE;
    if (ctx.xrTime <= 0)
        return OpenXR::XR_PLANE_POLL_UNCHANGED; // no frame yet: nothing to locate against
    if (!ensureDetector())
        return OpenXR::XR_PLANE_POLL_UNAVAILABLE;

    switch (m_state) {
        case DET_IDLE: begin(ctx); return OpenXR::XR_PLANE_POLL_UNCHANGED;
        case DET_PENDING: {
            XrPlaneDetectionStateEXT st = XR_PLANE_DETECTION_STATE_NONE_EXT;
            if (XR_FAILED(m_pfnState(m_detector, &st))) {
                if (++m_errors >= DET_MAX_ERRORS)
                    fail(DET_FAILED, "xrGetPlaneDetectionStateEXT keeps failing");
                return OpenXR::XR_PLANE_POLL_UNCHANGED;
            }
            if (st == XR_PLANE_DETECTION_STATE_PENDING_EXT)
                return OpenXR::XR_PLANE_POLL_UNCHANGED;
            if (st == XR_PLANE_DETECTION_STATE_FATAL_EXT) {
                fail(DET_FAILED, "the runtime reported XR_PLANE_DETECTION_STATE_FATAL_EXT");
                return OpenXR::XR_PLANE_POLL_UNAVAILABLE;
            }
            if (st != XR_PLANE_DETECTION_STATE_DONE_EXT) { // NONE / ERROR: start over
                if (++m_errors >= DET_MAX_ERRORS)
                    fail(DET_FAILED, std::format("detections keep ending in state {}", sc<int>(st)));
                else
                    m_state = DET_IDLE;
                return OpenXR::XR_PLANE_POLL_UNCHANGED;
            }
            m_state      = DET_READY;
            m_readyPolls = 0;
            m_errors     = 0;
            ++m_generation;
            [[fallthrough]];
        }
        case DET_READY: {
            if (!fetch(ctx, out)) {
                m_state = DET_IDLE; // the detection went away under us: run a new one
                return OpenXR::XR_PLANE_POLL_UNCHANGED;
            }
            if (++m_readyPolls >= DET_REDETECT_POLLS)
                m_state = DET_IDLE; // the next poll begins a fresh detection; these planes stay published
            return OpenXR::XR_PLANE_POLL_UPDATED;
        }
        default: break;
    }
    return OpenXR::XR_PLANE_POLL_UNCHANGED;
}

#else // !XR_EXT_plane_detection

OpenXR::eXRPlanePoll CXRPlaneDetectionSource::poll(const OpenXR::SXRPlanePollContext&, std::vector<OpenXR::SXRPlane>&) {
    return OpenXR::XR_PLANE_POLL_UNAVAILABLE;
}

bool CXRPlaneDetectionSource::ensureDetector() {
    return false;
}
bool CXRPlaneDetectionSource::begin(const OpenXR::SXRPlanePollContext&) {
    return false;
}
bool CXRPlaneDetectionSource::fetch(const OpenXR::SXRPlanePollContext&, std::vector<OpenXR::SXRPlane>&) {
    return false;
}

#endif // XR_EXT_plane_detection

#endif // HAVE_OPENXR
