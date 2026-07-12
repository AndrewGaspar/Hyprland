#include "XRSession.hpp"
#ifdef HAVE_OPENXR

// Platform macros must precede the EGL/GLES headers, and those must precede the OpenXR
// platform header (which declares XrGraphicsBindingEGLMNDX). See doc 01's include contract.
#define XR_USE_PLATFORM_EGL
#define XR_USE_GRAPHICS_API_OPENGL_ES

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

// XR_EXTX_overlay (provisional extension #34) — chained into xrCreateSession for overlay mode
// (doc 01). Present in the openxr package we build against; define locally as a fallback for
// older headers. The struct is stable-shaped — copied verbatim from the OpenXR registry.
#ifndef XR_EXTX_overlay
#define XR_EXTX_overlay 1
#define XR_EXTX_OVERLAY_EXTENSION_NAME "XR_EXTX_overlay"
#define XR_TYPE_SESSION_CREATE_INFO_OVERLAY_EXTX ((XrStructureType)1000033000)
typedef XrFlags64 XrOverlaySessionCreateFlagsEXTX;
typedef struct XrSessionCreateInfoOverlayEXTX {
    XrStructureType                 type;
    const void* XR_MAY_ALIAS        next;
    XrOverlaySessionCreateFlagsEXTX createFlags;
    uint32_t                        sessionLayersPlacement;
} XrSessionCreateInfoOverlayEXTX;
#endif

#include <cstring>
#include <vector>

#include "XRGraphics.hpp"
#include "../debug/log/Logger.hpp"

// Small XR_CHK helper: logs via Log::logger and returns false on failure (doc 01 asked to
// drop the WIP's fprintf duplication + XR_LOG macro).
#define XR_CHK(expr)                                                                                                                                                               \
    do {                                                                                                                                                                           \
        XrResult _r = (expr);                                                                                                                                                      \
        if (XR_FAILED(_r)) {                                                                                                                                                       \
            Log::logger->log(Log::ERR, "[OPENXR] " #expr " failed: {}", (int)_r);                                                                                                  \
            return false;                                                                                                                                                          \
        }                                                                                                                                                                          \
    } while (0)

CXRSession::~CXRSession() {
    destroy();
}

bool CXRSession::createInstance() {
    uint32_t extCount = 0;
    xrEnumerateInstanceExtensionProperties(nullptr, 0, &extCount, nullptr);
    std::vector<XrExtensionProperties> extProps(extCount, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, extCount, &extCount, extProps.data());

    auto hasExt = [&](const char* name) {
        for (auto& e : extProps)
            if (strcmp(e.extensionName, name) == 0)
                return true;
        return false;
    };

    if (!hasExt("XR_MNDX_egl_enable") || !hasExt("XR_KHR_opengl_es_enable")) {
        Log::logger->log(Log::ERR, "[OPENXR] required extensions XR_MNDX_egl_enable / XR_KHR_opengl_es_enable not available");
        return false;
    }

    std::vector<const char*> exts = {"XR_MNDX_egl_enable", "XR_KHR_opengl_es_enable"};

    // Enable every optional extension that is present, recording availability for the rest
    // of the integration to read.
    m_hasLocalFloor      = hasExt("XR_EXT_local_floor");
    m_hasHandInteraction = hasExt("XR_EXT_hand_interaction");
    m_hasHandTracking    = hasExt("XR_EXT_hand_tracking");
    if (m_hasLocalFloor)
        exts.push_back("XR_EXT_local_floor");
    if (m_hasHandInteraction)
        exts.push_back("XR_EXT_hand_interaction");
    if (m_hasHandTracking)
        exts.push_back("XR_EXT_hand_tracking");
    m_usingLocalFloor = m_hasLocalFloor;

    // XR_KHR_vulkan_enable2 (probe-only): enabled purely so start() can learn which GPU the
    // runtime composites on (OpenXR::probeRuntimeRenderNode) and fail closed on a wrong openxr:gpu
    // BEFORE the frame thread hands the runtime a cross-GPU EGL context — which hard-crashes the
    // graphics driver (radeonsi driUnbindContext, coredumps 8986/39318). The session itself still
    // uses the EGL/GLES binding; the two graphics enable-extensions coexist on one instance.
    m_hasVulkanEnable2 = hasExt("XR_KHR_vulkan_enable2");
    if (m_hasVulkanEnable2)
        exts.push_back("XR_KHR_vulkan_enable2");

    // XR_EXT_user_presence (report-19): enable it when advertised so the plug gate can key on the
    // real donned/doffed signal instead of session visibility (WiVRn advertises it; system support is
    // confirmed separately in getSystem). Availability recorded either way for the visibility fallback.
    m_hasUserPresence = hasExt(XR_EXT_USER_PRESENCE_EXTENSION_NAME);
    if (m_hasUserPresence)
        exts.push_back(XR_EXT_USER_PRESENCE_EXTENSION_NAME);

    // Overlay session (doc 01): enable XR_EXTX_overlay only when requested (openxr:overlay) AND
    // advertised by the runtime. Requested-but-unsupported downgrades to a normal session with a
    // one-time WARN — never fail startup for this. m_isOverlay is the actual decision, consumed by
    // createSession() to chain XrSessionCreateInfoOverlayEXTX.
    m_hasOverlay = hasExt(XR_EXTX_OVERLAY_EXTENSION_NAME);
    m_isOverlay  = m_overlayRequested && m_hasOverlay;
    if (m_isOverlay)
        exts.push_back(XR_EXTX_OVERLAY_EXTENSION_NAME);
    else if (m_overlayRequested && !m_hasOverlay)
        Log::logger->log(Log::WARN, "[OPENXR] openxr:overlay requested but this runtime does not advertise XR_EXTX_overlay; creating a normal (exclusive) session");

    XrApplicationInfo appInfo = {};
    strncpy(appInfo.applicationName, "Hyprland", XR_MAX_APPLICATION_NAME_SIZE - 1);
    appInfo.applicationVersion = 1;
    strncpy(appInfo.engineName, "Hyprland", XR_MAX_ENGINE_NAME_SIZE - 1);
    appInfo.apiVersion = XR_API_VERSION_1_0;

    XrInstanceCreateInfo info  = {XR_TYPE_INSTANCE_CREATE_INFO};
    info.applicationInfo       = appInfo;
    info.enabledExtensionCount = (uint32_t)exts.size();
    info.enabledExtensionNames = exts.data();

    XR_CHK(xrCreateInstance(&info, &m_instance));

    XrInstanceProperties props = {XR_TYPE_INSTANCE_PROPERTIES};
    if (XR_SUCCEEDED(xrGetInstanceProperties(m_instance, &props)))
        m_runtimeName = props.runtimeName;

    Log::logger->log(Log::DEBUG, "[OPENXR] instance created (runtime: {}, local_floor: {}, hand_interaction: {}, hand_tracking: {}, overlay: {}, user_presence: {})",
                     m_runtimeName.empty() ? "?" : m_runtimeName, m_hasLocalFloor, m_hasHandInteraction, m_hasHandTracking, m_isOverlay, m_hasUserPresence);
    return true;
}

bool CXRSession::getSystem() {
    XrSystemGetInfo sysInfo = {XR_TYPE_SYSTEM_GET_INFO};
    sysInfo.formFactor      = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XR_CHK(xrGetSystem(m_instance, &sysInfo, &m_systemId));

    XrSystemProperties sysProps = {XR_TYPE_SYSTEM_PROPERTIES};
    // Chain XrSystemUserPresencePropertiesEXT to learn whether the DEVICE supports user presence
    // (report-19): the ext may be enabled while the current device does not report presence (Monado
    // null/remote), in which case we must fall back to the visibility signal rather than gate on a
    // presence event that never comes.
    XrSystemUserPresencePropertiesEXT presenceProps = {XR_TYPE_SYSTEM_USER_PRESENCE_PROPERTIES_EXT};
    if (m_hasUserPresence) {
        presenceProps.next = sysProps.next;
        sysProps.next      = &presenceProps;
    }
    if (XR_SUCCEEDED(xrGetSystemProperties(m_instance, m_systemId, &sysProps))) {
        m_systemName    = sysProps.systemName;
        m_maxLayerCount = sysProps.graphicsProperties.maxLayerCount;
        if (m_maxLayerCount < 16) // spec guarantees at least 16
            m_maxLayerCount = 16;
        if (m_hasUserPresence)
            m_supportsUserPresence = presenceProps.supportsUserPresence;
    }

    Log::logger->log(Log::DEBUG, "[OPENXR] system id {} ({}), maxLayerCount {}, user_presence {}", (unsigned long long)m_systemId, m_systemName.empty() ? "?" : m_systemName,
                     m_maxLayerCount, !m_hasUserPresence ? "ext-absent" : (m_supportsUserPresence ? "supported" : "unsupported"));

    // Enumerate the supported environment blend modes for the primary-stereo view configuration
    // (doc 01). This needs only instance + system (not a session), so it runs here. The list is
    // returned in runtime-preference order; OpenXR::pickBlendMode consumes it at session start.
    uint32_t blendCount = 0;
    XrResult br         = xrEnumerateEnvironmentBlendModes(m_instance, m_systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &blendCount, nullptr);
    if (XR_SUCCEEDED(br) && blendCount > 0) {
        std::vector<XrEnvironmentBlendMode> modes(blendCount);
        if (XR_SUCCEEDED(xrEnumerateEnvironmentBlendModes(m_instance, m_systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, blendCount, &blendCount, modes.data()))) {
            m_blendModes.clear();
            std::string list;
            for (auto m : modes) {
                m_blendModes.push_back(xrBlendModeFromXr(m));
                list += (list.empty() ? "" : ", ") + OpenXR::blendModeToString(xrBlendModeFromXr(m));
            }
            if (m_blendModes.empty()) // all values were unknown enums — keep a safe default
                m_blendModes = {OpenXR::XR_BLEND_OPAQUE};
            Log::logger->log(Log::DEBUG, "[OPENXR] environment blend modes (preferred first): {}", list);
        }
    } else
        Log::logger->log(Log::WARN, "[OPENXR] xrEnumerateEnvironmentBlendModes failed/empty ({}); assuming OPAQUE only", (int)br);

    return true;
}

bool CXRSession::createSession(CXRGraphics& gfx) {
    // xrGetOpenGLESGraphicsRequirementsKHR is mandatory before session creation.
    PFN_xrGetOpenGLESGraphicsRequirementsKHR pfnGetReqs = nullptr;
    XR_CHK(xrGetInstanceProcAddr(m_instance, "xrGetOpenGLESGraphicsRequirementsKHR", reinterpret_cast<PFN_xrVoidFunction*>(&pfnGetReqs)));
    XrGraphicsRequirementsOpenGLESKHR reqs = {XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
    XR_CHK(pfnGetReqs(m_instance, m_systemId, &reqs));

    XrGraphicsBindingEGLMNDX binding = {XR_TYPE_GRAPHICS_BINDING_EGL_MNDX};
    binding.getProcAddress           = eglGetProcAddress;
    binding.display                  = gfx.m_eglDisplay;
    binding.config                   = gfx.m_config;
    binding.context                  = gfx.m_xrContext;

    XrSessionCreateInfo sessionInfo = {XR_TYPE_SESSION_CREATE_INFO};
    sessionInfo.systemId            = m_systemId;
    sessionInfo.next                = &binding;

    // Overlay session (doc 01): chain XrSessionCreateInfoOverlayEXTX between sessionInfo and the
    // EGL binding. On Monado the placement maps straight into z_order (primary pinned to INT64_MIN),
    // so any value composites our quads above the primary client. m_isOverlay was decided in
    // createInstance (requested AND the extension is enabled).
    XrSessionCreateInfoOverlayEXTX overlayInfo = {XR_TYPE_SESSION_CREATE_INFO_OVERLAY_EXTX};
    if (m_isOverlay) {
        overlayInfo.createFlags            = 0;
        overlayInfo.sessionLayersPlacement = m_overlayZ;
        overlayInfo.next                   = &binding;
        sessionInfo.next                   = &overlayInfo;
        Log::logger->log(Log::DEBUG, "[OPENXR] creating overlay session (sessionLayersPlacement = {})", m_overlayZ);
    }

    // The WIP binds m_xrContext current around xrCreateSession and unbinds after — keep it.
    eglMakeCurrent(gfx.m_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, gfx.m_xrContext);
    XrResult r = xrCreateSession(m_instance, &sessionInfo, &m_session);
    eglMakeCurrent(gfx.m_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

    if (XR_FAILED(r)) {
        Log::logger->log(Log::ERR, "[OPENXR] xrCreateSession failed: {}", (int)r);
        return false;
    }

    Log::logger->log(Log::DEBUG, "[OPENXR] session created ({})", m_isOverlay ? "overlay" : "exclusive");
    return true;
}

bool CXRSession::createSpaces(CXRGraphics& gfx) {
    // Reference space: LOCAL_FLOOR if the runtime supports it, else LOCAL (the floor offset
    // for the LOCAL fallback is applied later inside the anchor math — doc 03 — not here).
    XrReferenceSpaceCreateInfo info = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    info.poseInReferenceSpace       = {{0, 0, 0, 1}, {0, 0, 0}};

    // The WIP kept the context current around reference-space creation; preserve that.
    eglMakeCurrent(gfx.m_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, gfx.m_xrContext);

    info.referenceSpaceType = m_usingLocalFloor ? XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR_EXT : XR_REFERENCE_SPACE_TYPE_LOCAL;
    XrResult r1             = xrCreateReferenceSpace(m_session, &info, &m_refSpace);

    info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    XrResult r2             = xrCreateReferenceSpace(m_session, &info, &m_viewSpace);

    eglMakeCurrent(gfx.m_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

    if (XR_FAILED(r1)) {
        Log::logger->log(Log::ERR, "[OPENXR] xrCreateReferenceSpace (reference) failed: {}", (int)r1);
        return false;
    }
    if (XR_FAILED(r2)) {
        Log::logger->log(Log::ERR, "[OPENXR] xrCreateReferenceSpace (view) failed: {}", (int)r2);
        return false;
    }

    Log::logger->log(Log::DEBUG, "[OPENXR] reference spaces created ({} + VIEW)", m_usingLocalFloor ? "LOCAL_FLOOR" : "LOCAL");
    return true;
}

bool CXRSession::chooseSwapchainFormat() {
    // Enumerate supported swapchain formats. Monado may crash (instead of returning an
    // error) if given an unsupported format, so always pick from this enumerated list.
    uint32_t fmtCount = 0;
    xrEnumerateSwapchainFormats(m_session, 0, &fmtCount, nullptr);
    std::vector<int64_t> formats(fmtCount);
    if (fmtCount)
        xrEnumerateSwapchainFormats(m_session, fmtCount, &fmtCount, formats.data());

    constexpr int64_t kSRGBA = 0x8C43; // GL_SRGB8_ALPHA8
    constexpr int64_t kRGBA8 = 0x8058; // GL_RGBA8
    constexpr int64_t kRGBA4 = 0x8056; // GL_RGBA4

    if (formats.empty()) {
        m_swapchainFormat = kRGBA8;
        Log::logger->log(Log::WARN, "[OPENXR] runtime enumerated no swapchain formats; defaulting to GL_RGBA8");
        return true;
    }

    // Prefer SRGB8_ALPHA8, then RGBA8, then RGBA4, else the first enumerated.
    int64_t chosen = formats[0];
    bool    found  = false;
    for (auto f : formats)
        if (f == kSRGBA) {
            chosen = f;
            found  = true;
            break;
        }
    if (!found)
        for (auto f : formats)
            if (f == kRGBA8) {
                chosen = f;
                found  = true;
                break;
            }
    if (!found)
        for (auto f : formats)
            if (f == kRGBA4) {
                chosen = f;
                break;
            }

    m_swapchainFormat = chosen;
    Log::logger->log(Log::DEBUG, "[OPENXR] chosen swapchain format 0x{:x}", (unsigned long long)chosen);
    return true;
}

void CXRSession::pollEvents() {
    XrEventDataBuffer event = {XR_TYPE_EVENT_DATA_BUFFER};
    XrResult          r     = XR_SUCCESS;
    while (m_instance != XR_NULL_HANDLE && (r = xrPollEvent(m_instance, &event)) == XR_SUCCESS) {
        switch (event.type) {
            case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
                auto* ev  = reinterpret_cast<XrEventDataSessionStateChanged*>(&event);
                m_xrState = ev->state;
                Log::logger->log(Log::DEBUG, "[OPENXR] session state -> {}", (int)m_xrState);

                switch (m_xrState) {
                    case XR_SESSION_STATE_READY: {
                        // A view configuration is required even though we submit only quad
                        // layers (doc 01).
                        XrSessionBeginInfo beginInfo           = {XR_TYPE_SESSION_BEGIN_INFO};
                        beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                        XrResult r                             = xrBeginSession(m_session, &beginInfo);
                        if (XR_SUCCEEDED(r)) {
                            m_sessionBegan = true;
                            Log::logger->log(Log::DEBUG, "[OPENXR] session begun");
                        } else
                            Log::logger->log(Log::ERR, "[OPENXR] xrBeginSession failed: {}", (int)r);
                        break;
                    }
                    case XR_SESSION_STATE_STOPPING: {
                        // Runtime-initiated stop: end the session and stay alive; the runtime
                        // may return to READY. This is NOT manager-level STOPPING (doc 00).
                        xrEndSession(m_session);
                        m_sessionBegan = false;
                        break;
                    }
                    case XR_SESSION_STATE_EXITING: {
                        // User deliberately exited XR — full teardown, land in DISABLED.
                        m_exitRequested = true;
                        break;
                    }
                    case XR_SESSION_STATE_LOSS_PENDING: {
                        // Session/instance loss — full teardown, land in UNAVAILABLE.
                        m_exitRequested = true;
                        m_instanceLost  = true;
                        break;
                    }
                    default: break;
                }
                break;
            }
            case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING: {
                Log::logger->log(Log::WARN, "[OPENXR] instance loss pending -> teardown to unavailable");
                m_exitRequested = true;
                m_instanceLost  = true;
                break;
            }
            case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED: {
                // WP-G5: a hand/controller (dis)connected or the runtime rebound a profile. Flag it
                // so the frame loop tells CXRInput to re-read each hand's current interaction profile
                // (active-device detection). The event carries only the session; per-hand queries
                // happen in CXRInput::refreshHandProfiles.
                m_interactionProfileChanged = true;
                Log::logger->log(Log::DEBUG, "[OPENXR] interaction profile changed");
                break;
            }
            case XR_TYPE_EVENT_DATA_USER_PRESENCE_CHANGED_EXT: {
                // report-19: the headset was donned or doffed (or the runtime's initial presence
                // report at session begin). Record it + flag for the frame loop to forward to main,
                // where it drives the `visible`-mode monitor plug/unplug edges.
                auto* ev              = reinterpret_cast<XrEventDataUserPresenceChangedEXT*>(&event);
                m_userPresent         = ev->isUserPresent;
                m_userPresenceChanged = true;
                Log::logger->log(Log::DEBUG, "[OPENXR] user presence changed -> {}", (bool)m_userPresent ? "present" : "absent");
                break;
            }
            case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING: {
                // Runtime recenter (doc 03 §6). Forward to the anchor engine via the frame loop.
                auto*      ev      = reinterpret_cast<XrEventDataReferenceSpaceChangePending*>(&event);
                const auto ourType = m_usingLocalFloor ? XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR_EXT : XR_REFERENCE_SPACE_TYPE_LOCAL;
                if (ev->referenceSpaceType == ourType) {
                    m_recenterPending   = true;
                    m_recenterPoseValid = ev->poseValid;
                    m_recenterPose      = xrToPose(ev->poseInPreviousSpace);
                    Log::logger->log(Log::DEBUG, "[OPENXR] reference space change pending (recenter, poseValid={})", (bool)ev->poseValid);
                }
                break;
            }
            default: break;
        }
        event = {XR_TYPE_EVENT_DATA_BUFFER};
    }

    // A dead runtime (e.g. monado-service killed) does NOT deliver a LOSS_PENDING event —
    // instead xrPollEvent (and every other XR call) starts returning XR_ERROR_INSTANCE_LOST
    // /XR_ERROR_SESSION_LOST directly. XR_EVENT_UNAVAILABLE is the normal "no more events"
    // sentinel and must be ignored. Treat the lost codes as instance loss -> teardown ->
    // UNAVAILABLE.
    if (r == XR_ERROR_INSTANCE_LOST || r == XR_ERROR_SESSION_LOST) {
        Log::logger->log(Log::WARN, "[OPENXR] runtime lost (xrPollEvent -> {}) -> teardown to unavailable", (int)r);
        m_exitRequested = true;
        m_instanceLost  = true;
    }
}

void CXRSession::destroy() {
    // Called after the frame thread is joined; the EGL context is NOT current here (the
    // manager's CXRGraphics::destroyGL already unbound it), matching the interop rule.
    // If the instance was lost, these calls may return errors — ignore, but null handles.
    if (m_viewSpace != XR_NULL_HANDLE) {
        xrDestroySpace(m_viewSpace);
        m_viewSpace = XR_NULL_HANDLE;
    }
    if (m_refSpace != XR_NULL_HANDLE) {
        xrDestroySpace(m_refSpace);
        m_refSpace = XR_NULL_HANDLE;
    }
    if (m_session != XR_NULL_HANDLE) {
        xrDestroySession(m_session);
        m_session = XR_NULL_HANDLE;
    }
    if (m_instance != XR_NULL_HANDLE) {
        xrDestroyInstance(m_instance);
        m_instance = XR_NULL_HANDLE;
    }
}

#endif // HAVE_OPENXR
