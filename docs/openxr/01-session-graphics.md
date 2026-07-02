# HypXRland — Session & Graphics Core

Doc 01 of the `docs/openxr/` set. Covers `CXRSession` (`src/openxr/XRSession.{hpp,cpp}`)
and `CXRGraphics` (`src/openxr/XRGraphics.{hpp,cpp}`), plus the frame-thread loop
that `COpenXRManager` runs. This is a **port-and-harden** of the WIP prototype
(`git show openxr:src/openxr/COpenXRManager.cpp`, branch `openxr`, commit
`d5c54bb9`) — the WIP proved this exact stack against Monado on real hardware, so
where this doc says "port", copy the WIP code and its comments; where it says
"changed", the delta is spelled out. Read doc 00 first for the thread model and
lifecycle states (`XR_STATE_*`).

Everything here is `#ifdef HAVE_OPENXR`.

## Header / include-order contract (port verbatim)

Platform macros must be defined **before** EGL/GLES headers, and those before the
OpenXR headers, in every TU that touches the graphics binding:

```cpp
#define XR_USE_PLATFORM_EGL
#define XR_USE_GRAPHICS_API_OPENGL_ES

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>   // XrGraphicsBindingEGLMNDX, XrSwapchainImageOpenGLESKHR
```

Headers that must compile in TUs without GL/EGL (e.g. `XRMonitorLayer.hpp`) use the
WIP's forward-declaration trick:

```cpp
using XR_GLuint      = unsigned int; // = GLuint
using XR_EGLImageKHR = void*;        // = EGLImageKHR
```

## Extensions

Enumerate with `xrEnumerateInstanceExtensionProperties` (two-call idiom) and check
by name (WIP `hasExt` lambda):

| Extension | Requirement | Purpose |
|---|---|---|
| `XR_MNDX_egl_enable` | **required** | EGL graphics binding (`XrGraphicsBindingEGLMNDX`) |
| `XR_KHR_opengl_es_enable` | **required** | GLES swapchain images (`XrSwapchainImageOpenGLESKHR`), `xrGetOpenGLESGraphicsRequirementsKHR` |
| `XR_EXT_local_floor` | optional | `LOCAL_FLOOR` reference space; else LOCAL + `openxr:floor_offset` |
| `XR_EXT_hand_interaction` | optional | hand-interaction bindings (doc 04) |
| `XR_EXT_hand_tracking` | optional | hand tracking (doc 04) |

If either required extension is missing → `start()` fails → `XR_STATE_UNAVAILABLE`.
Enable every optional extension that is present and record availability flags
(`m_hasLocalFloor`, `m_hasHandInteraction`, `m_hasHandTracking`) on `CXRSession` for
the other components to read.

## Class sketches

```cpp
// src/openxr/XRSession.hpp
class CXRSession {
  public:
    // all creation runs on the MAIN thread inside COpenXRManager::start(),
    // BEFORE the frame thread exists. pollEvents() runs on the frame thread.
    bool           createInstance();                    // false => UNAVAILABLE
    bool           getSystem();                         // xrGetSystem + xrGetSystemProperties
    bool           createSession(CXRGraphics& gfx);     // XrGraphicsBindingEGLMNDX
    bool           createSpaces();                      // ref space + view space
    void           destroy();                           // spaces, session, instance (see teardown ordering)

    void           pollEvents();                        // frame thread: XR event pump + state machine

    XrInstance     m_instance   = XR_NULL_HANDLE;
    XrSystemId     m_systemId   = XR_NULL_SYSTEM_ID;
    XrSession      m_session    = XR_NULL_HANDLE;
    XrSpace        m_refSpace   = XR_NULL_HANDLE;       // LOCAL_FLOOR or LOCAL
    XrSpace        m_viewSpace  = XR_NULL_HANDLE;       // VIEW
    bool           m_usingLocalFloor = false;
    bool           m_hasLocalFloor = false, m_hasHandInteraction = false, m_hasHandTracking = false;

    XrSessionState m_xrState      = XR_SESSION_STATE_UNKNOWN; // frame-thread-only after start
    bool           m_sessionBegan = false;                    // frame-thread-only after start
    bool           m_exitRequested = false;                   // set by pollEvents on EXITING/LOSS_PENDING
    bool           m_instanceLost  = false;                   // set on XrEventDataInstanceLossPending / LOSS_PENDING
    uint32_t       m_maxLayerCount = 16;  // XrSystemGraphicsProperties::maxLayerCount (spec floor 16)
    int64_t        m_swapchainFormat = 0; // chosen once after session creation
};

// src/openxr/XRGraphics.hpp
class CXRGraphics {
  public:
    bool        initEGL(const std::string& gpuOverride); // display + context + proc ptrs (main thread, in start())
    bool        initBlitGL();                             // program, VAO, external tex (main thread, in start())
    void        destroy();                                // GL then EGL (see teardown ordering)

    // frame thread, inside a CScopedGLContext:
    bool        blitBuffer(const SP<Aquamarine::IBuffer>& buf, CXRMonitorLayer& layer, XR_GLuint dstTex);
    void        clearTex(XR_GLuint dstTex, const Vector2D& size, float r, float g, float b);

    // RAII guard: ctor eglMakeCurrent(m_xrContext), dtor eglMakeCurrent(EGL_NO_CONTEXT).
    // The ONLY way GL work is done — see "EGL context ownership" below.
    struct CScopedGLContext { explicit CScopedGLContext(CXRGraphics&); ~CScopedGLContext(); };

    EGLDisplay         m_eglDisplay = EGL_NO_DISPLAY;
    EGLContext         m_xrContext  = EGL_NO_CONTEXT; // owned exclusively by frame thread after start
    EGLConfig          m_config     = nullptr;
    struct gbm_device* m_gbmOwned   = nullptr;        // set iff we opened our own device
    int                m_gbmFd      = -1;

    XR_GLuint          m_blitProg = 0, m_blitVAO = 0;
    XR_GLuint          m_extTex   = 0;                // GL_TEXTURE_EXTERNAL_OES, rebound per blit
    // eglCreateImageKHR / eglDestroyImageKHR / glEGLImageTargetTexture2DOES proc ptrs (WIP pattern)
};
```

Per-layer GL state (CPU-fallback staging texture `m_cpuTex`, last `EGLImageKHR`)
lives on `CXRMonitorLayer` (doc 02) because it is sized per monitor mode.

## Instance / system / session creation sequence

Runs on the **main thread** inside `COpenXRManager::start()` (state `XR_STATE_STARTING`),
exactly like the WIP's `init()` did all setup before `startFrameThread()`:

1. `createInstance()` — extension checks as above; `XrApplicationInfo` name/engine
   `"Hyprland"`, `apiVersion = XR_API_VERSION_1_0`. Failure (including "no runtime":
   the loader returns `XR_ERROR_INSTANCE_LOST`/`XR_ERROR_RUNTIME_FAILURE`/
   `XR_ERROR_RUNTIME_UNAVAILABLE` or file-not-found style errors) →
   `XR_STATE_UNAVAILABLE`, **no auto-retry polling** — the user retries via
   `hyprctl openxr enable` or a config reload.
2. `getSystem()` — `xrGetSystem` with `XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY`
   (`XR_ERROR_FORM_FACTOR_UNAVAILABLE` → UNAVAILABLE too), then
   `xrGetSystemProperties` and record
   `XrSystemProperties::graphicsProperties.maxLayerCount` into `m_maxLayerCount`
   (spec guarantees ≥ 16; doc 02's layer-cap policy consumes this).
3. `gfx.initEGL(*openxr:gpu)` — GPU selection below.
4. `createSession(gfx)` — `xrGetOpenGLESGraphicsRequirementsKHR` first (mandatory
   before session creation), then:

```cpp
XrGraphicsBindingEGLMNDX binding = {XR_TYPE_GRAPHICS_BINDING_EGL_MNDX};
binding.getProcAddress = eglGetProcAddress;
binding.display        = gfx.m_eglDisplay;
binding.config         = gfx.m_config;
binding.context        = gfx.m_xrContext;
// sessionInfo.next = &binding; xrCreateSession(...)
```

   The WIP binds `m_xrContext` current around `xrCreateSession` +
   `xrCreateReferenceSpace` and unbinds after — keep that.
5. `createSpaces()` — reference spaces below.
6. `gfx.initBlitGL()` — compile blit program (below), still on the main thread,
   before the frame thread exists (WIP comment: *"All of this happens before
   startFrameThread() so m_xrContext is still available here"*).
7. Enumerate + choose the swapchain format once (below); store on the session.
8. **Action system** (doc 04): build the `hyprland` action set, suggest bindings for all
   profiles, create the aim/grip action spaces, and `xrAttachSessionActionSets` —
   `CXRInput::init()`, called eagerly here (still on the main thread, still before the frame
   thread exists). Doc 04's own text already says the action set is "created at session init,
   before `xrAttachSessionActionSets`"; this step is where that happens — there is no lazy or
   first-use deferral anywhere in the code.
9. Bind existing `CXRMonitorLayer`s (monitors created while disabled — doc 02),
   spawn the frame thread (`m_running = true`), state → `XR_STATE_RUNNING_IDLE`.

Note: **no swapchains are created here.** Per-layer swapchains are created lazily by
the frame thread when the session reaches READY / when layers appear (doc 02),
mirroring the WIP which created swapchains on first READY.

## GPU selection

**Default (changed from WIP): match Hyprland's primary GPU render node.** Resolve
Hyprland's render node from `g_pCompositor->m_drmRenderNode.fd` (set from
`m_aqBackend->drmRenderNodeFD()` in `initServer`) via `drmGetDeviceNameFromFd2()`,
then enumerate EGL devices (`eglQueryDevicesEXT` / `eglQueryDeviceStringEXT` with
`EGL_DRM_RENDER_NODE_FILE_EXT`, falling back to `EGL_DRM_DEVICE_FILE_EXT` — WIP
code) and pick the device whose render-node path matches.

**Override:** `openxr:gpu` (string config var) = explicit DRM render-node path
(e.g. `/dev/dri/renderD129`); if set and non-empty it wins outright; if it matches
no EGL device, log `Log::ERR` and fail `start()` → UNAVAILABLE (misconfiguration
should be loud, not silently fall back).

**Historical rationale to preserve in comments** (this knowledge is why the code is
shaped the way it is — keep it even though the default heuristic changes):

- The WIP walked all EGL devices and picked the one whose DRM node's PCI vendor in
  sysfs (`/sys/class/drm/<node>/device/vendor`) was `0x1002` (AMD), skipping NVIDIA
  (`0x10de`). WIP comment: *"We need an EGL display on the SAME GPU that Monado uses
  (Mesa/AMD). Hyprland on this hybrid system uses the NVIDIA EGL device; passing a
  NVIDIA-backed context to Monado causes it to crash in driUnbindContext when
  importing AMD DMA-BUFs."* I.e. **cross-GPU dmabuf import crashes Monado** — that
  is the real constraint; "match Hyprland's GPU" is the better general default and
  `openxr:gpu` covers hybrid setups where Monado runs on the *other* GPU.
- **GBM platform, not device platform** (port verbatim): open the render node
  `O_RDWR | O_CLOEXEC`, `gbm_create_device(fd)`, then
  `eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_KHR, gbm, nullptr)`. WIP comment:
  *"EGL_PLATFORM_DEVICE_EXT is headless/compute-only and leaves gallium
  pipe_context state partially uninitialised, causing driUnbindContext to crash."*
  Track ownership in `m_gbmOwned`/`m_gbmFd` and destroy them last in teardown.
- Last-resort fallback if enumeration extensions are unavailable: reuse
  `g_pHyprOpenGL->m_eglDisplay` (WIP did this; *"may crash on cross-GPU DMA-BUF
  import, but worth trying"*). `eglInitialize` is refcounted, so initializing an
  already-initialized display is fine.

## EGL config + context (port verbatim)

`eglBindAPI(EGL_OPENGL_ES_API)`, then the WIP's progressive config cascade — keep
all rungs and comments:

1. GLES3 + `EGL_SURFACE_TYPE = EGL_WINDOW_BIT | EGL_PBUFFER_BIT` + RGBA8 (GBM path)
2. GLES3 + `EGL_SURFACE_TYPE = EGL_PBUFFER_BIT` + RGBA8 (device/NVIDIA path)
3. GLES3, `EGL_SURFACE_TYPE 0`, RGBA8
4. GLES3, `EGL_SURFACE_TYPE 0`, anything

WIP note to keep: *"default EGL_SURFACE_TYPE is EGL_WINDOW_BIT which excludes
pbuffer-only configs on EGL device platform — so we must specify it explicitly."*
(The WIP also has an alternative `getCompatibleConfig()` that re-finds Hyprland's
own config via `EGL_CONFIG_ID` and manually scans `eglGetConfigs` to work around
Mesa `eglChooseConfig` quirks — relevant only for the shared-display fallback.)

Context: `eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, {EGL_CONTEXT_CLIENT_VERSION, 3})`,
surfaceless (`EGL_NO_SURFACE` everywhere). Load `eglCreateImageKHR`,
`eglDestroyImageKHR`, `glEGLImageTargetTexture2DOES` via `eglGetProcAddress` once.

## EGL context ownership — the critical invariant (port verbatim)

The XR frame thread **exclusively owns `m_xrContext`** after the frame thread
starts; before it starts (during `start()`) and after it is joined (during `stop()`)
the main thread may use it. But ownership is not enough — the context must also be
**unbound whenever we are not actively issuing GL commands**, because Monado's
in-process compositor thread binds our context itself. Two WIP comments carry the
full reasoning; preserve both in code:

- Frame loop: *"Do NOT hold m_xrContext current continuously — Monado's compositor
  thread needs to bind it during xrCreateSwapchain. Only make it current around
  actual GL calls."*
- Before `xrCreateSwapchain`: *"Do NOT bind the context ourselves — Monado's
  context_begin will call eglMakeCurrent internally. If our context is already
  current when it does so, Mesa enters the 'rebind same context' path which still
  calls driUnbindContext on the surfaceless drawable, and that crashes in AMD
  gallium. Starting with no context current means Monado's eglMakeCurrent has
  nothing to unbind first."*

Concretely: every GL burst is wrapped in `CXRGraphics::CScopedGLContext`
(`eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, m_xrContext)` /
`... EGL_NO_CONTEXT` on scope exit), and **no XR call that may touch the runtime's
GL interop (`xrCreateSwapchain`, `xrDestroySwapchain`, `xrEnumerateSwapchainImages`,
`xrAcquire/Wait/ReleaseSwapchainImage`, `xrEndFrame`) is made while our context is
current.** The WIP acquires/waits, then binds → blits → unbinds, then releases —
keep exactly that shape.

## Reference spaces

Created in `createSpaces()` with identity `poseInReferenceSpace = {{0,0,0,1},{0,0,0}}`:

- `m_refSpace`: `XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR_EXT` if `XR_EXT_local_floor`
  was enabled (`m_usingLocalFloor = true`); else `XR_REFERENCE_SPACE_TYPE_LOCAL`.
  When falling back to LOCAL, all floor-relative Y placement adds
  `openxr:floor_offset` (float, default 1.5 m = assumed eye height above floor);
  the offset is applied inside `CXRAnchor` math (doc 03), not by shifting the space.
- `m_viewSpace`: `XR_REFERENCE_SPACE_TYPE_VIEW` — used every frame to locate the
  head pose for head/body anchor modes and input rays
  (`xrLocateSpace(m_viewSpace, m_refSpace, predictedDisplayTime, &loc)`).

`XrEventDataReferenceSpaceChangePending` (runtime recenter) is forwarded to the
anchor engine — handling in doc 03.

## Swapchain format selection (port verbatim)

Once per session: `xrEnumerateSwapchainFormats`, prefer `GL_SRGB8_ALPHA8` (0x8C43),
else `GL_RGBA8` (0x8058), else `GL_RGBA4` (0x8056), else first enumerated. WIP
comment to keep: *"Monado may crash (instead of returning an error) if given an
unsupported format, so always pick from this list."* Store on
`CXRSession::m_swapchainFormat`; per-layer swapchain creation (doc 02) uses it with
`usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT`,
`sampleCount/faceCount/arraySize/mipCount = 1`, and `width/height` = the **monitor's
pixel mode** (not eye resolution — quads have no eye resolution).

## Session state handling (`CXRSession::pollEvents`, frame thread)

The WIP only handled READY and STOPPING; this is the complete version. Pump with
the standard loop (`XrEventDataBuffer` reset each iteration, `while (xrPollEvent
== XR_SUCCESS)`).

`XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED`:

| `XrSessionState` | Frame-thread action | Manager state reported to main (channel [C]) |
|---|---|---|
| `IDLE` | nothing | `RUNNING_IDLE` |
| `READY` | `xrBeginSession` with `primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO` (a view configuration is required even though we submit only quad layers); `m_sessionBegan = true` | `RUNNING_IDLE` |
| `SYNCHRONIZED` | frame loop must keep calling `xrWaitFrame`/`xrBeginFrame`/`xrEndFrame`; not visible yet — no pacing | `RUNNING_IDLE` |
| `VISIBLE` | pacing on (doc 02) | `RUNNING_VISIBLE` |
| `FOCUSED` | pacing + `xrSyncActions` legal (doc 04) | `RUNNING_FOCUSED` |
| `STOPPING` | `xrEndSession`; `m_sessionBegan = false`; stop pacing; **stay alive** and keep polling — the runtime may return to READY | `RUNNING_IDLE` |
| `EXITING` | `m_exitRequested = true` → frame loop exits; notify main via channel [C]: main runs the `stop()` teardown (join is instant since the loop exited) and lands in `XR_STATE_DISABLED` (user exited XR deliberately; `openxr:enabled` untouched — re-enable is manual) | `STOPPING` → `DISABLED` |
| `LOSS_PENDING` | as EXITING but also `m_instanceLost = true`; main tears down and lands in `XR_STATE_UNAVAILABLE` | `STOPPING` → `UNAVAILABLE` |

`XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING`: treat as LOSS_PENDING → teardown →
`UNAVAILABLE` (no auto-retry; per spec the instance may be recreatable after
`lossTime`, but we deliberately leave retry to the user).

`XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING`: forward to anchors (doc 03).

**Dead-runtime case (as built, WP13 reconciliation):** `xrPollEvent` itself can return
`XR_ERROR_INSTANCE_LOST`/`XR_ERROR_SESSION_LOST` directly (no event delivered at all — the
runtime process is simply gone) rather than always delivering a well-formed
`XrEventDataInstanceLossPending`. `pollEvents()` treats these return codes exactly like
`LOSS_PENDING` (`m_instanceLost = true`, `m_exitRequested = true`) — add this as an implicit row
alongside the `XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING` handling above.

Every reported transition makes the main thread emit `openxrsessionstate` (doc 05)
and re-run the idle-inhibit check.

## Frame-thread loop (pseudocode)

`COpenXRManager::frameThread()` — the orchestration; blit details next section,
layer/pacing/mode-change specifics in doc 02, anchors doc 03, input doc 04:

```
while m_running:
    session.pollEvents()
    if session.m_exitRequested: break                      # EXITING / LOSS_PENDING

    if !session.m_sessionBegan:
        sleep 50ms; continue                               # WIP idle throttle

    layers = snapshot()          # lock m_layersMu; copy SP vector (bound, !m_pendingRemoval)
                                 # + copy each layer's quad params (anchor spec, size, z);
                                 # unlock. Also collect + destroy layers pending removal
                                 # (swapchain/GL teardown, then ack to main — doc 02).

    if session.m_xrState in {VISIBLE, FOCUSED}:            # pacing — doc 02
        for l in layers: l.monitor->m_output->scheduleFrame()

    xrWaitFrame  -> frameState (predictedDisplayTime); on failure: continue
    xrBeginFrame                                          ; on failure: continue

    headPose = xrLocateSpace(viewSpace, refSpace, predictedDisplayTime)

    for l in layers:
        if l.m_swapchainDirty: recreate swapchain at new size (doc 02); l.m_hasContent = false
        if l.m_swapchain == XR_NULL_HANDLE: create swapchain sized to monitor pixel mode (doc 02)

        buf = null
        if l.m_haveNewFrame.load(acquire):
            lock l.m_bufMu; buf = move(l.m_latestBuffer); l.m_haveNewFrame = false; unlock

        if buf == null and l.m_hasContent:
            continue                                       # skip blit — quad keeps showing the
                                                           # last released swapchain image
        xrAcquireSwapchainImage(l.m_swapchain) -> imgIdx
        xrWaitSwapchainImage(timeout = XR_INFINITE_DURATION)
        { CScopedGLContext ctx(gfx)                        # bind ONLY here
          if buf: gfx.blitBuffer(buf, l, l.m_swapchainImages[imgIdx]); l.m_hasContent = true
          else:   gfx.clearTex(l.m_swapchainImages[imgIdx], l.m_swapchainSize, 0, 0, 0)
        }                                                  # unbound again
        xrReleaseSwapchainImage(l.m_swapchain)

    input.frame(predictedDisplayTime)                      # xrSyncActions, rays, grab — doc 04

    quads = []
    for l in layers sorted by (m_zOrder, m_seq), while quads.size < session.m_maxLayerCount:
        if !l.m_quadActive or !l.m_hasContent: continue    # cap policy — doc 02
        (space, pose) = l.m_anchor->solve(headPose, dt)    # doc 03; device-locked layers
                                                           # return the grip XrActionSpace
        quads.push(XrCompositionLayerQuad{
            .layerFlags    = 0,
            .space         = space,
            .eyeVisibility = XR_EYE_VISIBILITY_BOTH,
            .subImage      = { l.m_swapchain, {{0,0}, {w, h}}, 0 },
            .pose          = pose,
            .size          = { l.m_sizeMeters, l.m_sizeMeters * h / w },
        })

    xrEndFrame(displayTime = frameState.predictedDisplayTime,
               environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
               layers = quads)                             # array of quad layer pointers
```

Notes:

- `xrEndFrame` with zero layers is valid (nothing composited yet) — required while
  SYNCHRONIZED and before any layer has content.
- The skip-blit optimization is safe because a quad layer re-presents the most
  recently released swapchain image every runtime frame; we only acquire when the
  monitor actually presented a new buffer.
- `scheduleFrame()` from the frame thread is the WIP-proven pacing mechanism (WIP
  comment: *"this drives the ~90Hz render rate on the virtual monitor regardless of
  what mode it advertises"*). Only while VISIBLE/FOCUSED.
- **DEVIATION from the original pseudocode (as built, WP13 reconciliation):** an earlier
  draft of this loop gated the pump itself on `session.m_xrState` already being in
  `{SYNCHRONIZED, VISIBLE, FOCUSED}`. In practice the runtime only advances
  READY → SYNCHRONIZED → VISIBLE once the application *starts submitting frames*
  (confirmed against Monado's null compositor) — gating the pump on those states is a
  deadlock: the session sits at READY forever because nothing ever calls
  `xrWaitFrame`/`xrBeginFrame`/`xrEndFrame` to let it progress. The implemented gate is
  simply `!session.m_sessionBegan` (set `true` in the `READY` row's `xrBeginSession` above):
  the pump runs continuously from `xrBeginSession` onward, and `xrEndFrame` with zero/blank
  layers (first note above) is what correctly carries the session through SYNCHRONIZED. The
  pacing gate (`scheduleFrame()` only while VISIBLE/FOCUSED) is unaffected and matches this
  doc as written.

## Blit pipeline (`CXRGraphics::blitBuffer`)

Ported from WIP `blitBufferToEye`, generalized to per-layer targets. Three paths,
tried in order:

**1. DMA-BUF path (primary).** `buf->dmabuf()` (`Aquamarine::SDMABUFAttrs`) →
build `EGLint` attrib list: `EGL_WIDTH/HEIGHT`, `EGL_LINUX_DRM_FOURCC_EXT` =
`dmab.format`, plane 0 fd/offset/pitch, plus plane 1/2 attribs when
`dmab.planes > 1` (WIP has the exact attrib tables) →
`eglCreateImageKHR(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attribs)`.
Destroy the layer's previous `m_lastEGLImg` first; store the new one on the layer
(per-layer, so removal teardown is self-contained). Then:
`glBindTexture(GL_TEXTURE_EXTERNAL_OES, m_extTex)` →
`glEGLImageTargetTexture2DOES` → transient FBO with `dstTex` as
`GL_COLOR_ATTACHMENT0` → `glViewport(0, 0, layerW, layerH)` → fullscreen-triangle
draw with the blit program. Shaders port verbatim (GLSL ES 3.00; vertex generates
the triangle from `gl_VertexID`, no VBO; fragment requires
`GL_OES_EGL_image_external_essl3` and samples a `samplerExternalOES`).

**2. CPU data-pointer fallback.** `buf->caps() & Aquamarine::BUFFER_CAPABILITY_DATAPTR`
→ `beginDataPtr(0)` → `glTexSubImage2D(GL_TEXTURE_2D, ..., GL_BGRA_EXT,
GL_UNSIGNED_BYTE, data)` into the layer's staging texture → `endDataPtr()` →
`glBlitFramebuffer` staging → `dstTex`. WIP comment to keep: *"DRM_FORMAT_XRGB8888
is BGRX in memory; GL_BGRA_EXT swaps to RGBA correctly."*
**Fixed vs WIP:** the WIP allocated the staging texture hard-coded at **1920×1080**
(`initBlitGL`), which corrupts any other mode. The staging texture is now per-layer
(`CXRMonitorLayer::m_cpuTex`), allocated lazily at the **actual monitor pixel mode**
and reallocated on mode change (doc 02).

**3. Clear fallback.** `clearTex(dstTex, size, r, g, b)` — transient FBO +
`glClear`. Black in production (WIP used cyan as a debug sentinel).

All three run on the frame thread inside a `CScopedGLContext`.

## Teardown ordering (`COpenXRManager::stop()`, main thread)

State → `XR_STATE_STOPPING`, then, strictly in order (the WIP destructor got this
right — generalize it):

1. `m_running = false`; **join the frame thread** — before touching any EGL or XR
   object. After the join the main thread owns `m_xrContext` again.
2. GL cleanup **with the context current** (WIP comment: *"context must be current —
   briefly make it so"*): per-layer `eglDestroyImageKHR(m_lastEGLImg)` and
   `glDeleteTextures(m_cpuTex)`, then shared `m_extTex`, `m_blitVAO`, `m_blitProg`;
   unbind (`EGL_NO_CONTEXT`).
3. Per-layer `xrDestroySwapchain` (context NOT current — same interop rule as
   creation), and mark layers unbound (`m_swapchain = XR_NULL_HANDLE`,
   `m_hasContent = false`).
4. `xrDestroySpace(m_viewSpace)`, `xrDestroySpace(m_refSpace)`,
   `xrDestroySession`, `xrDestroyInstance`. If `m_instanceLost`, calls may return
   errors — ignore, but still null the handles.
5. `eglDestroyContext`, `eglTerminate(m_eglDisplay)`; if `m_gbmOwned`:
   `gbm_device_destroy` + `close(m_gbmFd)` — last, the display depends on them.
6. Monitor disposition per `openxr:destroy_monitors_on_stop` (doc 02): destroy
   XR-created headless outputs, or leave them as plain outputs with unbound layer
   records for the next `start()`.
7. State → `XR_STATE_DISABLED` (or `XR_STATE_UNAVAILABLE` when stop was triggered
   by instance loss), emit `openxrsessionstate`.

`stop()` must be idempotent and callable from: config hot-toggle, `hyprctl openxr
disable`, EXITING/LOSS_PENDING notifications (channel [C] callback), and compositor
shutdown (manager destructor).

**Process-shutdown ordering, as built (WP13 reconciliation):** the sequence above is what runs
inside `COpenXRManager::stop()` itself — but on a *full compositor shutdown* (not a session
stop), the global `g_pOpenXRManager` (which owns the manager and would otherwise only be
destroyed at static/global teardown, i.e. after `main()`'s local state is torn down) is instead
reset **explicitly and very early** in `CCompositor::cleanup()`
(`src/Compositor.cpp`, right after `g_pPluginSystem->unloadAllPlugins()`), well **before**
`g_pInputManager`/`g_pSeatManager`/the renderer/`g_pXWayland` are reset. This is required, not
incidental: if the manager were left to the default global-destructor order, its destructor would
run `stop()` → `removePointerDevice()` → the pointer's destroy signal →
`CInputManager::destroyPointer` → `CSeatManager::setMouse`, by which point `cleanup()` has
already reset `g_pInputManager`/`g_pSeatManager` — a use-after-free. Doc 00's lifecycle section
cross-references this; see also doc 04 §8 for the pointer-device teardown path this ordering
protects.

## Logging

Use `Log::logger->log(Log::DEBUG | Log::WARN | Log::ERR, ...)` (there is no
`Log::LOG` level). Drop the WIP's `fprintf(stderr, ...)` duplication and its
`XR_LOG`/`XR_CHK` macros in favor of a small local `XR_CHK`-style helper that logs
via `Log::logger` and returns false.

## Context files to read before implementing

- `git show openxr:src/openxr/COpenXRManager.cpp` — the WIP source this doc ports (861 lines; read all of it)
- `git show openxr:src/openxr/COpenXRManager.hpp`
- `git diff main...openxr -- CMakeLists.txt src/Compositor.cpp src/Compositor.hpp` — build gating + hook point
- `/home/ajg/code/Hyprland/docs/openxr/00-overview.md` — thread model, lifecycle states, handoff table
- `/home/ajg/code/Hyprland/docs/openxr/02-virtual-monitors.md` — `CXRMonitorLayer`, swapchain-per-layer lifecycle consumed by the frame loop
- `/home/ajg/code/Hyprland/src/Compositor.cpp` — `initServer` (~315–352: backend creation, `m_drmRenderNode.fd` at ~367), `initManagers` `STAGE_LATE` (~706)
- `/home/ajg/code/Hyprland/src/render/OpenGL.hpp` — `g_pHyprOpenGL->m_eglDisplay` (shared-display fallback)
- `/home/ajg/code/Hyprland/src/output/Monitor.hpp` — `m_output`, `scheduleFrame`, `m_events.presented`
- `/home/ajg/code/Hyprland/src/debug/log/Logger.hpp` — logging API
- `/home/ajg/code/Hyprland/src/config/values/ConfigValues.cpp` — `getConfigValues()`, where `openxr:gpu`, `openxr:floor_offset` are declared (see doc 05 §1.1)
- `/home/ajg/code/Hyprland/src/managers/eventLoop/EventLoopManager.hpp` — event loop hosting the frame→main eventfd
