# 01 — Session & Graphics Core

The OpenXR session and its graphics backend: `CXRSession`
(`src/openxr/XRSession.{hpp,cpp}`), `CXRGraphics` (`src/openxr/XRGraphics.{hpp,cpp}`),
and the frame-thread loop that `COpenXRManager` runs. Read doc 00 first for the thread
model and lifecycle states. Everything here is behind `#ifdef HAVE_OPENXR`.

The session binds OpenXR to a GLES/EGL context living on a GBM device, submits one
`XrCompositionLayerQuad` per virtual monitor, and paces those monitors from a dedicated frame
thread. It targets runtimes that expose an EGL graphics binding — Monado (including its null
compositor) and WiVRn.

## Header / include-order contract

Platform macros must be defined **before** the EGL/GLES headers, and those before the OpenXR
headers, in every translation unit that touches the graphics binding:

```cpp
#define XR_USE_PLATFORM_EGL
#define XR_USE_GRAPHICS_API_OPENGL_ES

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>   // XrGraphicsBindingEGLMNDX, XrSwapchainImageOpenGLESKHR
```

Headers that must compile in TUs without GL/EGL (e.g. `XRMonitorLayer.hpp`) forward-declare the
handle types instead:

```cpp
using XR_GLuint      = unsigned int; // = GLuint
using XR_EGLImageKHR = void*;        // = EGLImageKHR
```

## Extensions

Enumerated with `xrEnumerateInstanceExtensionProperties` and checked by name:

| Extension | Requirement | Purpose |
|---|---|---|
| `XR_MNDX_egl_enable` | **required** | EGL graphics binding (`XrGraphicsBindingEGLMNDX`) |
| `XR_KHR_opengl_es_enable` | **required** | GLES swapchain images, `xrGetOpenGLESGraphicsRequirementsKHR` |
| `XR_EXT_local_floor` | optional | `LOCAL_FLOOR` reference space; else `LOCAL` + `openxr:floor_offset` |
| `XR_EXTX_overlay` | optional | overlay sessions (compose over another XR app) |
| `XR_EXT_user_presence` | optional | real donned/doffed signal for the monitor plug gate (doc 02) |
| `XR_KHR_vulkan_enable2` | optional | probe the runtime's GPU for the cross-GPU safety check |
| `XR_EXT_hand_interaction` / `XR_EXT_hand_tracking` | optional | hand-tracking input (doc 04) |

If either required extension is missing, `start()` fails to `XR_STATE_UNAVAILABLE`. Every
optional extension that is present is enabled, and its availability is recorded on `CXRSession`
for the other components to read.

## Session bring-up

All creation runs on the **main thread** inside `COpenXRManager::start()` (state
`XR_STATE_STARTING`), before the frame thread exists:

1. **Kernel-taint tripwire.** `/proc/sys/kernel/tainted` is checked *first*, before the runtime
   handshake and before any GPU enumeration — a kernel that has already oopsed means the GPU
   driver stack may be corrupt, and enumeration is the point of no return (below).
2. **Overlay probe.** `openxr:overlay` / `openxr:overlay_z` are read once, before
   `createInstance()` (see "Overlay sessions" below).
3. `createInstance()` — extension checks; `XrApplicationInfo` names the app `Hyprland`,
   `apiVersion = XR_API_VERSION_1_0`. A missing runtime (loader returns
   `XR_ERROR_RUNTIME_UNAVAILABLE`/`_FAILURE`/`_INSTANCE_LOST` or file-not-found) →
   `XR_STATE_UNAVAILABLE`.
4. `getSystem()` — `xrGetSystem` with a head-mounted form factor
   (`XR_ERROR_FORM_FACTOR_UNAVAILABLE` → UNAVAILABLE, the dormant "waiting for the headset"
   case), then `xrGetSystemProperties` records `maxLayerCount` (spec floor 16), and
   `xrEnumerateEnvironmentBlendModes` records the runtime's blend modes preferred-first.
5. **Blend-mode selection** from `openxr:blend_mode` against the enumerated list (below).
6. `gfx.initEGL(openxr:gpu)` — GPU selection (below).
7. **Cross-GPU safety check** — refuse to start on a GPU mismatch (below).
8. `createSession(gfx)` — `xrGetOpenGLESGraphicsRequirementsKHR` first (mandatory before
   session creation), then `xrCreateSession` with the EGL binding:

   ```cpp
   XrGraphicsBindingEGLMNDX binding = {XR_TYPE_GRAPHICS_BINDING_EGL_MNDX};
   binding.getProcAddress = eglGetProcAddress;
   binding.display        = gfx.m_eglDisplay;
   binding.config         = gfx.m_config;
   binding.context        = gfx.m_xrContext;
   ```

9. `createSpaces()` — the reference space and the view space (below).
10. `initBlitGL()` — compile the blit program, still on the main thread while the context is
   free.
11. Choose the swapchain format once (below).
12. **Action system** (doc 04): build the action set, suggest bindings for every interaction
    profile, create the aim/grip action spaces, and `xrAttachSessionActionSets`. This is
    eager — there is no lazy/first-use deferral.
13. Bind any existing `CXRMonitorLayer`s (monitors created while disabled — doc 02), spawn the
    frame thread (`m_running = true`), state → `XR_STATE_RUNNING_IDLE`.

**No swapchains are created here.** Per-layer swapchains are created by the frame thread once
the session reaches READY (doc 02).

## GPU selection

The XR EGL context lives on a GBM device on a specific DRM render node:

- **Default:** match Hyprland's primary GPU. The render node is resolved from the compositor's
  DRM node and matched against the EGL devices (`eglQueryDevicesEXT` /
  `eglQueryDeviceStringEXT`).
- **Override:** `openxr:gpu` = an explicit render-node path (e.g. `/dev/dri/renderD129`). If
  set it wins outright; if it matches no EGL device, `start()` fails loudly to UNAVAILABLE
  rather than silently falling back.

Two implementation choices, kept because they are load-bearing:

- **The context is created on a GBM platform display**, not a device-platform display:
  `gbm_create_device(fd)` then `eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_KHR, ...)`. The
  device platform is headless/compute-only and leaves gallium's pipe context partially
  uninitialised, which crashes in `driUnbindContext`. Ownership is tracked in `m_gbmOwned` /
  `m_gbmFd` and destroyed last in teardown.
- **A cross-GPU dmabuf import can hard-crash the graphics driver** (`radeonsi
  driUnbindContext`) — an uncatchable SEGV that would take the whole desktop session down.
  That single fact shapes the GPU handling: match GPUs when possible, refuse when a mismatch
  is detected, and allocate linear buffers when a cross-GPU split is unavoidable (doc 02,
  `openxr:force_linear`).

### Cross-GPU safety check (fail closed)

On a multi-GPU / hybrid (Optimus) machine the runtime may composite on a different GPU than
the one the XR EGL context landed on — and the runtime imports the compositor's buffers at
`xrCreateSwapchain`, on the frame thread, where a mismatch crashes. WiVRn and Monado accept a
mismatched EGL binding at `xrCreateSession` without complaint, so session bring-up is the last
point at which the compositor can refuse while the desktop is still intact.

`start()` resolves the runtime's DRM render node and compares it to the XR context's node:

- **Mismatch** → `start()` aborts to UNAVAILABLE with an error telling the user to point
  `openxr:gpu` at the runtime's GPU. The desktop session is untouched.
- **Match** → proceed.
- **Undeterminable** (runtime advertises neither probe extension, or the probe times out) →
  proceed with a warning, since a setup that would actually work should not be blocked.

The question is "which GPU does the runtime **composite** on", and it is asked of
`XR_MND_query_egl_device`: `xrGetSystemEGLDeviceMND` names the `EGLDeviceEXT` an EGL client is
meant to build its context on, which is the compositor's device by construction — an EGL binding
renders through the runtime's GL client compositor, which imports the compositor's swapchain
images by an opaque fd, valid only on the device that exported them. It is answered in-process
(the runtime enumerates our EGL devices through the `getProcAddress` we hand it and matches by
UUID) with no IPC and no Vulkan, and its `EGLDeviceEXT` maps to a DRM node with
`eglQueryDeviceStringEXT` — the same render-node-then-primary order `selectDisplay()` used to
choose the context's device.

In-process is not the same as safe on the main thread, though: that `getProcAddress` callback is a
full glvnd EGL device enumeration, so the query reaches into every installed vendor driver exactly
like our own enumeration does (see the next subsection). It therefore runs on the same bounded
throwaway thread as the Vulkan fallback below.

`XR_KHR_vulkan_enable2` remains as the **fallback** for a runtime without the EGL query.
`xrGetVulkanGraphicsDevice2KHR` answers a different question — which GPU a *Vulkan application*
should render on — which coincides with the compositor's on every runtime that does not
deliberately split the two, and does not on a WiVRn configured for cross-GPU rendering (game on
the dGPU, composite and encode on the iGPU). It runs on a throwaway thread with a bounded (3s)
wait: `vkCreateInstance` can deadlock against the runtime's own in-process Vulkan use, and the
check must never freeze the compositor — on timeout the thread is abandoned (it bails before
touching any XR handle) and bring-up continues unverified.

The resolved runtime GPU, and which of the two queries answered, are surfaced as `runtimeGpu` in
`hyprctl openxr status`.

### Sick-driver refusal (kernel taint tripwire)

The cross-GPU check above assumes the drivers it is asking are *working*. This one covers the case
where they are not.

**What a pin cannot do.** It is tempting to think `openxr:gpu` keeps XR away from a GPU it is not
pinned to. It does not, and cannot. Measured on a dual-vendor box (NVIDIA RTX 5070 + AMD 890M,
glvnd with `10_nvidia.json` and `50_mesa.json`), instrumenting `/proc/self/maps` and
`/proc/self/fd` around each call:

```
after eglGetProcAddress("eglQueryDevicesEXT")   libEGL_nvidia LOADED, libnvidia-eglcore LOADED, mesa LOADED
after eglQueryDevicesEXT(0, nullptr, &n)        /dev/nvidiactl, /dev/nvidia0 OPEN
after the per-device eglQueryDeviceStringEXT    /dev/dri/renderD128 OPEN
```

Every vendor library is loaded by the **first `eglGetProcAddress`**, and the NVIDIA **kernel
driver** is contacted by the **count-only** enumeration call — both before a single `EGLDeviceEXT`
handle exists for a pin to filter on. libglvnd has to work this way: `eglQueryDevicesEXT` is not
tied to a display, so `libEGL` must ask every vendor and merge the answers, and `eglGetProcAddress`
must load every vendor to discover who implements the extension at all. There is no EGL API for
"only this vendor". The only lever is the `__EGL_VENDOR_LIBRARY_FILENAMES` environment variable,
which is deliberately **not** used: it is session-scoped and would be inherited by every client app
the compositor spawns, a far larger blast radius than the problem.

So bring-up cannot avoid entering a sick GPU driver. It can only decline to start.

**The tripwire.** Before the runtime handshake and before any GPU enumeration, `start()` reads
`/proc/sys/kernel/tainted` and checks bit 7, `TAINT_DIE` — "kernel has oopsed before". It is set
for the whole boot and never cleared, which is exactly right: once any kernel oops has happened, no
amount of waiting makes the driver stack trustworthy again.

- **`TAINT_DIE` set** → bring-up is refused outright, before touching EGL. The state goes
  UNAVAILABLE and the reason appears as a `blocked:` line in `hyprctl openxr status`.
- **Anything else** → proceed. The check is deliberately narrow: the everyday taint bits
  (proprietary / out-of-tree / unsigned modules — `12288` on a stock NVIDIA box, every boot) say
  nothing about driver health, and blocking on them would fire permanently on exactly the machines
  XR runs on.
- **Unreadable or unparsable** → proceed. The tripwire **fails open**; a missing `/proc` entry must
  never cost a working setup its session.

It is re-evaluated on **every** attempt, re-probe retries included, so ignoring it does not make it
go quiet — the `blocked:` line stays up and the state keeps returning to UNAVAILABLE. The error is
logged loudly once and at DEBUG thereafter (a retry every few seconds would otherwise bury the
log); a `hyprctl openxr disable && hyprctl openxr enable` cycle re-arms the loud version.

`openxr:ignore_kernel_taint = 1` overrides the refusal for development. It still warns each start,
because a hatch set months ago should not be silent.

**Why this is here at all.** Hard reboot #6: an NVIDIA driver use-after-free cascaded through the
kernel, which printed `Fixing recursive fault but reboot is needed!` 29 minutes before the machine
died. The compositor was a bystander — but it had a bring-up path that would have walked into the
already-corrupt driver seconds later, with a pin that could not have helped. This check refuses that
session outright.

**And when a driver is sick but the kernel has not oopsed yet**, the second guard applies: every
call that enters a GPU driver or the runtime during bring-up — the EGL device enumeration in
`selectDisplay()`, `xrGetSystemEGLDeviceMND`, and the Vulkan probe — runs on a bounded throwaway
thread (`OpenXR::runBoundedProbe`, 3s). A driver that never returns costs XR, not the desktop: the
thread is abandoned, the enumeration is treated as a failure, and bring-up refuses cleanly. The
accepted cost is that an abandoned thread leaks for the life of the process — there is no safe way
to cancel a thread stuck inside a driver, and one leaked thread beats a frozen desktop (2026-07-15,
when a stalled leased DP link froze the whole desktop until a power cycle).

## EGL context ownership — the critical invariant

The XR frame thread **exclusively owns the EGL context** while it runs; the main thread may
use it only before the frame thread starts and after it is joined. Ownership alone is not
enough — the context must also be **unbound whenever GL commands are not actively being
issued**, because the runtime's in-process compositor thread binds the context itself during
`xrCreateSwapchain`. If our context is already current when the runtime calls
`eglMakeCurrent`, Mesa takes a rebind path that calls `driUnbindContext` on the surfaceless
drawable and crashes in AMD gallium.

Concretely: every GL burst is wrapped in `CXRGraphics::CScopedGLContext` (which does
`eglMakeCurrent(..., m_xrContext)` on entry and `EGL_NO_CONTEXT` on exit), and **no XR call
that may touch the runtime's GL interop** — `xrCreateSwapchain`, `xrDestroySwapchain`,
`xrEnumerateSwapchainImages`, `xrAcquire/Wait/ReleaseSwapchainImage`, `xrEndFrame` — is made
while the context is current. The frame loop acquires/waits, then binds → blits → unbinds,
then releases.

## Reference spaces

Created with an identity pose:

- **`m_refSpace`:** `LOCAL_FLOOR` when `XR_EXT_local_floor` is available (`m_usingLocalFloor =
  true`); otherwise `LOCAL`, in which case floor-relative Y placement adds `openxr:floor_offset`
  (default 1.5 m, assumed eye height) inside the anchor math (doc 03).
- **`m_viewSpace`:** `VIEW` — located every frame to drive head/body anchor modes and input
  rays.

`XrEventDataReferenceSpaceChangePending` (a runtime recenter) is forwarded to the anchor
engine (doc 03).

## Swapchain format

Chosen once per session from `xrEnumerateSwapchainFormats`, preferring `GL_SRGB8_ALPHA8`, then
`GL_RGBA8`, then `GL_RGBA4`, else the first enumerated — a format is never passed unless it was
enumerated, because some runtimes crash rather than return an error on an unsupported format.
Per-layer swapchains (doc 02) use it with color-attachment + sampled usage, single
sample/face/array/mip, sized to the **monitor's pixel mode** (a quad has no per-eye
resolution).

## Environment blend mode

The mode submitted to `xrEndFrame` decides what a quad is composited *over*:

| `XrEnvironmentBlendMode` | `openxr:blend_mode` | Effect |
|---|---|---|
| `OPAQUE` | `opaque` | quads over black — the classic "floating in a void" look |
| `ALPHA_BLEND` | `alpha` | quads over the runtime's **passthrough** underlay (e.g. WiVRn on Quest 3 — monitors in your real room) |
| `ADDITIVE` | `additive` | additive / optical-see-through displays |

`OpenXR::pickBlendMode(supported, config)` (pure, unit-tested in `tests/xr/`) maps
`openxr:blend_mode` onto the enumerated list: `auto` (or an unrecognized value) takes the
runtime's first-listed (preferred) mode; an explicit mode is honored if supported, else it
falls back to the preferred mode with a warning. The value is read **once at session start** —
changing `openxr:blend_mode` takes effect on the next start (`hyprctl openxr disable && enable`,
or a reload that toggles `openxr:enabled`). The active mode is surfaced as `blendMode` in
`hyprctl openxr status`.

## Overlay sessions (`XR_EXTX_overlay`)

By default HypXRland is an **exclusive** XR client: it owns the frame and its quads composite
over the blend-mode background. With `openxr:overlay = 1` it instead runs as an **overlay**
session, so its quads composite on top of *another* XR client's scene — a VR game, or the
`hypxrpaper` ambient-background app.

- `createInstance()` enables `XR_EXTX_overlay` only when it was requested **and** the runtime
  advertises it. Requested-but-unsupported logs a warning and creates a normal session —
  overlay never fails startup.
- `createSession()` chains `XrSessionCreateInfoOverlayEXTX{ sessionLayersPlacement =
  openxr:overlay_z }` into the session-create `next` chain.
- `openxr:overlay` / `openxr:overlay_z` are read once at session start (same semantics as
  `blend_mode`).

On Monado (and WiVRn, which inherits its compositor) an overlay session is held **visible +
focused** the whole time the service runs — even with no primary app — so ray input, grab, and
idle-inhibit behave exactly as in exclusive mode. Delivered client frames are composited
bottom-to-top by `z_order` with the primary pinned beneath, so any `overlay_z` puts our quads
above it; the environment blend comes from the primary client, so our own blend mode is
effectively ignored while we are an overlay. The runtime delivers controller input to both
clients at once — input is **not** arbitrated between the game and the desktop. The actual
session type is surfaced as `overlay` in `hyprctl openxr status` (a downgraded request reads
`false`). SteamVR-Linux does not support the extension. See doc 05 for the user-facing setup and
`hypxrpaper` recipe.

## Session-state handling (`CXRSession::pollEvents`, frame thread)

Pumped each frame with the standard `xrPollEvent` loop.
`XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED`:

| `XrSessionState` | Frame-thread action | Manager state |
|---|---|---|
| `IDLE` | nothing | `RUNNING_IDLE` |
| `READY` | `xrBeginSession` (primary stereo view config — required even though we submit only quads); `m_sessionBegan = true` | `RUNNING_IDLE` |
| `SYNCHRONIZED` | keep pumping `xrWaitFrame`/`xrBeginFrame`/`xrEndFrame`; not visible — no pacing | `RUNNING_IDLE` |
| `VISIBLE` | pacing on (doc 02) | `RUNNING_VISIBLE` |
| `FOCUSED` | pacing + `xrSyncActions` (doc 04) | `RUNNING_FOCUSED` |
| `STOPPING` | `xrEndSession`; stop pacing; **stay alive** and keep polling — the runtime may return to READY | `RUNNING_IDLE` |
| `EXITING` | frame loop exits; the main thread tears down and lands in `DISABLED` (a deliberate quit; `openxr:enabled` untouched) | → `DISABLED` |
| `LOSS_PENDING` | as EXITING but the main thread lands in `UNAVAILABLE` (auto-reconnects via the reprobe timer) | → `UNAVAILABLE` |

`XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING` is treated as `LOSS_PENDING`. So is a **dead
runtime**: `xrPollEvent` itself returning `XR_ERROR_INSTANCE_LOST`/`_SESSION_LOST` (the runtime
process simply vanished, delivering no event) is handled identically.
`XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING` is forwarded to the anchors (doc 03). Every
reported transition makes the main thread post `openxrsessionstate` and re-run the idle check.

The pump runs continuously from `xrBeginSession` onward (gated only on `m_sessionBegan`). It
does **not** wait for the session to already be VISIBLE — a runtime only advances READY →
SYNCHRONIZED → VISIBLE once the app starts submitting frames, so `xrEndFrame` (with zero or
blank layers, which is valid) is what carries the session forward. Pacing
(`scheduleFrame()`, doc 02) is the only thing gated on VISIBLE/FOCUSED.

## Frame-thread loop

`COpenXRManager::frameThread()` — orchestration; blit, pacing, and mode-change details are in
doc 02, anchors in doc 03, input in doc 04:

```
while m_running:
    session.pollEvents()
    if session.m_exitRequested: break                      # EXITING / LOSS_PENDING
    if !session.m_sessionBegan: sleep 50ms; continue       # idle throttle

    layers = snapshot()          # lock, copy the layer refs + each layer's quad params, unlock;
                                 # also destroy layers pending removal, then ack to main (doc 02)

    if session.m_xrState in {VISIBLE, FOCUSED}:            # pacing (doc 02)
        for l in layers: l.monitor->m_output->scheduleFrame()

    xrWaitFrame -> frameState (predictedDisplayTime)
    xrBeginFrame
    headPose = xrLocateSpace(viewSpace, refSpace, predictedDisplayTime)

    for l in layers:
        recreate/create the swapchain if dirty or absent (doc 02)
        if l presented a new buffer: acquire/wait swapchain image;
            { CScopedGLContext ctx: blit dmabuf -> swapchain image }   # bind ONLY here
            release swapchain image
        else if the quad already has content: skip (it re-presents its last image)

    input.frame(predictedDisplayTime)                      # xrSyncActions, rays, grab (doc 04)

    quads = []
    for l in layers sorted by (z_order, sequence), while quads.size < maxLayerCount:
        if !l.quadActive or !l.hasContent: continue        # layer-cap policy (doc 02)
        (space, pose) = l.m_anchor->solve(headPose, dt)    # doc 03; device layers return the grip action space
        quads.push(XrCompositionLayerQuad{ space, pose, subImage, size, eyeVisibility=BOTH })

    xrEndFrame(predictedDisplayTime, session.m_blendMode, quads)
```

A quad layer re-presents its most recently released swapchain image every runtime frame, so the
loop only acquires a swapchain image for monitors that actually presented a new desktop buffer;
otherwise it skips the blit. `xrEndFrame` with zero layers is valid and is what carries the
session through SYNCHRONIZED before any monitor has content.

## Blit pipeline (`CXRGraphics::blitBuffer`)

A monitor's presented buffer is copied into the acquired swapchain image, on the frame thread
inside a `CScopedGLContext`. Three paths, tried in order:

1. **DMA-BUF (primary).** Build an `EGL_LINUX_DMA_BUF_EXT` image from the buffer's dmabuf
   attributes (fourcc, per-plane fd/offset/pitch), import it as a `GL_TEXTURE_EXTERNAL_OES`
   texture, and draw a fullscreen triangle (no VBO; the fragment shader samples a
   `samplerExternalOES`) into a transient FBO with the swapchain image as the color
   attachment. The previous EGL image is destroyed and the new one stored per-layer.
2. **CPU data-pointer fallback.** For buffers that expose a CPU data pointer,
   `glTexSubImage2D` into a per-layer staging texture sized to the actual monitor mode, then
   blit to the swapchain image. (`DRM_FORMAT_XRGB8888` is BGRX in memory; `GL_BGRA_EXT` swaps
   it to RGBA.)
3. **Clear fallback.** A transient FBO + `glClear` to opaque black.

**Alpha is forced opaque in every path.** Hyprland monitor buffers are typically XRGB — the
alpha channel is undefined. Under passthrough (`ALPHA_BLEND`) the runtime composites each quad
over the passthrough underlay using its alpha, so undefined alpha would punch see-through holes
in the monitors. To keep monitors fully opaque regardless of blend mode: the DMA-BUF shader
writes `a = 1.0`; the CPU path forces destination alpha to 1.0 with a masked clear after the
blit; the clear fallback already clears with alpha 1.0.

## Teardown ordering (`COpenXRManager::stop()`, main thread)

State → `XR_STATE_STOPPING`, then strictly in order:

1. `m_running = false`; **join the frame thread** — before touching any EGL or XR object. The
   main thread then owns the context again.
2. GL cleanup **with the context current**: per-layer EGL images and staging textures, then
   the shared blit program/VAO/external texture; unbind.
3. Per-layer `xrDestroySwapchain` (context **not** current — same interop rule as creation).
4. `xrDestroySpace` ×2, `xrDestroySession`, `xrDestroyInstance` (errors ignored if the instance
   was lost).
5. `eglDestroyContext`, `eglTerminate`, then (if we opened it) `gbm_device_destroy` + `close` —
   last, since the display depends on them.
6. Monitor disposition per `openxr:destroy_monitors_on_stop` and `openxr:monitors_follow_session`
   (doc 02): keep the outputs (unplugged) or destroy them.
7. State → `DISABLED` (or `UNAVAILABLE` on instance loss); post `openxrsessionstate`.

`stop()` is idempotent and callable from a config toggle, `hyprctl openxr disable`, an
EXITING/LOSS_PENDING notification, or compositor shutdown. On full shutdown the manager is
reset early in `CCompositor::cleanup()` (doc 00) so the synthetic pointer's teardown
(`removePointerDevice()`, doc 04) reaches a live input manager and seat.

## Logging

`Log::logger->log(Log::DEBUG | Log::WARN | Log::ERR, ...)` — there is no `Log::LOG` level.
