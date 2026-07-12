# HypXRland — OpenXR Extension for Hyprland: Overview

Doc 00 of the `docs/openxr/` design-doc set. This doc is the map: goals, component
layout, the thread model, the lifecycle state machine, build gating, and the
relationship to the existing WIP prototype (git branch `openxr`, commit `d5c54bb9`).
Every other doc in the set assumes the reader has read this one.

The doc set:

| Doc | Scope |
|---|---|
| `00-overview.md` | (this doc) architecture, threads, lifecycle, build gating, WIP port map |
| `01-session-graphics.md` | OpenXR instance/session, EGL/GBM graphics, frame thread loop, blit pipeline, teardown |
| `02-virtual-monitors.md` | `CXRMonitorLayer`, virtual-monitor create/destroy, buffer handoff, pacing, mode changes, mirroring |
| `03-anchoring.md` | `CXRAnchor` interface, pose math, leashing, grab re-anchor, recenter, layout persistence |
| `04-input.md` | action sets, ray casting, `CXRPointerDevice`, grab state machine, frame→main event queue |
| `05-ipc-config.md` | full config/dispatcher/hyprctl/socket2 tables, idle-inhibit hook, consumer recipes |
| `06-testing.md` | Monado orchestration, hyprtester XR suite |
| `07-roadmap.md` | work packages, dependencies, acceptance criteria |
| `08-wiki-notes.md` | ready-to-paste external-wiki reference: config vars, keyword/dispatcher/hyprctl syntax, events, hypridle recipe |

## Goals

- Arbitrary on-demand **virtual monitors** (headless outputs) rendered as
  positionable **`XrCompositionLayerQuad`** layers in XR space — one quad per monitor.
- **XR-session state surfaced via IPC**: socket2 events + `hyprctl openxr status`, so
  bars and hypridle can react; idle-inhibit while an XR session is focused; mirroring
  virtual monitors onto physical ones with **zero new code** (existing `setMirror`).
- **Anchoring**: absolute placement in tracking space, head/body leashing, and
  device (controller) locking; grab-to-move with controllers/hands.
- **Input**: XR controller/hand rays drive a synthetic pointer through Hyprland's
  normal input path; repositioning verbs exposed as dispatchers/binds/config.
- Clean **enable/disable at runtime** — `openxr:enabled` config var honored, hot
  toggle, `hyprctl openxr enable|disable`.

## Non-goals

- **No stereo 3D compositor.** We do not render per-eye projection layers of a 3D
  scene; each monitor is a flat quad composited by the XR runtime. (The WIP's stereo
  projection billboard is explicitly replaced.)
- **No compositor-drawn indicator overlay.** "XR is active" indication is IPC
  events + hyprctl state only; bars draw their own indicators.
- **No per-eye parallax** or any per-eye differentiation of quad content
  (`eyeVisibility = XR_EYE_VISIBILITY_BOTH` always).

## Component diagram

Fresh module `src/openxr/`, all code behind `#ifdef HAVE_OPENXR`:

```
src/openxr/
  OpenXRManager.{hpp,cpp}   COpenXRManager  — orchestrator, main-thread API, lifecycle state machine
  XRSession.{hpp,cpp}       CXRSession      — instance/system/session/spaces, XR event pump
  XRGraphics.{hpp,cpp}      CXRGraphics     — GBM/EGL display+context, GL blit resources, swapchain plumbing (WIP port)
  XRMonitorLayer.{hpp,cpp}  CXRMonitorLayer — one per virtual monitor: output ref, buffer handoff, swapchain, quad
  XRAnchor.{hpp,cpp}        CXRAnchor       — anchor solve; pure math, no XR handles, gtest-able
  XRInput.{hpp,cpp}         CXRInput        — actions, xrSyncActions, ray cast, grab machine (frame thread)
  XRPointerDevice.{hpp,cpp} CXRPointerDevice— IPointer subclass, synthetic pointer (main thread)
  XRIpc.{hpp,cpp}           CXRIpc          — hyprctl "openxr" command, "xrmonitor" dispatcher, socket2 events
  XRMath.hpp                Vec3/Quat/pose helpers
```

Runtime picture — two threads, three handoff channels:

```
==================== MAIN THREAD =====================    ================= XR FRAME THREAD =================
                                                           (spawned by COpenXRManager::start(), owns the EGL
 ConfigManager ── openxr:* vars, xrmonitor keyword ──┐      context m_xrContext exclusively while running)
 HyprCtl ──────── "openxr" command ──────────────────┤
 DispatcherTranslator ── "xrmonitor" dispatcher ─────┤        while (m_running):
                                                     v          CXRSession::pollEvents()      (state machine)
 ┌───────────────────────────────────────────┐                  pace: mon->m_output->scheduleFrame()
 │ COpenXRManager                            │                  xrWaitFrame / xrBeginFrame
 │   start()/stop(), lifecycle state         │  [A] m_layersMu  snapshot m_layers ◄──────────────────[A]
 │   m_layers: vector<SP<CXRMonitorLayer>> ──┼──────────────►   per layer:
 │   createXRMonitor()/destroyXRMonitor()    │                    read latest buffer ◄────────────────[B]
 └──────────────┬────────────────────────────┘                    CXRGraphics: dmabuf→EGLImage→
                │ impl->createOutput(name) /                        samplerExternalOES blit → swapchain
                │ mon->m_output->destroy()                        CXRAnchor::solve() → quad pose
                v                                                 CXRInput: xrSyncActions, ray cast, grab
 Aquamarine headless backend ─► CMonitor                        xrEndFrame(quad layer array)
                │ m_events.presented (per frame)
                v
 CXRMonitorLayer::onPresented() ── [B] per-layer m_bufMu + atomic m_haveNewFrame ──► (read on frame thread)

 CXRPointerDevice injection,                  [C] SPSC queue + eventfd on the
 socket2 emission, focus/idle  ◄───────────────── wayland event loop ◄──────────── frame→main events
                                                  (input events, session-state
                                                   transitions, teardown acks)
```

`CXRAnchor` and `CXRInput` run on the frame thread; `CXRPointerDevice` and `CXRIpc`
run on the main thread; `CXRMonitorLayer` straddles both with explicitly-guarded
fields (see the handoff table). `COpenXRManager` is the only entry point the rest of
Hyprland touches (global `g_pOpenXRManager`, an `inline UP<COpenXRManager>` as in
the WIP).

## Thread model and handoff table

Rule of thumb: **main thread** owns monitor lifecycle, config, IPC, and input
injection. The **XR frame thread** exclusively owns the EGL context (`m_xrContext`),
the frame loop, blits, anchor solving, and action sampling. Nothing else may call
into OpenXR or the XR EGL context while the frame thread runs (see doc 01 for the
EGL binding invariant — the context must also be *unbound* between GL bursts so
Monado's compositor thread can bind it).

| Data | Field(s) | Guard | Direction | Protocol |
|---|---|---|---|---|
| Layer set + quad params | `COpenXRManager::m_layers` (`std::vector<SP<CXRMonitorLayer>>`), per-layer anchor spec / size / z-order | `COpenXRManager::m_layersMu` (`std::mutex`) | main → frame | Main mutates on create/destroy/dispatcher/config-reload. Frame thread takes the lock **once per frame**, copies the SP vector and each layer's quad params into a snapshot, releases. Never holds the lock during GL/XR calls. |
| Presented buffer | `CXRMonitorLayer::m_latestBuffer` (`SP<Aquamarine::IBuffer>`), `m_haveNewFrame` (`std::atomic<bool>`) | per-layer `std::mutex m_bufMu` + acquire/release on the atomic | main → frame | WIP-proven pattern (`COpenXRManager::onMonitorPresented` in the WIP): main writes buffer under mutex then `store(true, release)`; frame checks `load(acquire)`, takes mutex, moves out the SP, `store(false)`. |
| Mode-change | `CXRMonitorLayer::m_swapchainDirty` (`std::atomic<bool>`) + `m_pendingSize` (under `m_bufMu`) | atomic + `m_bufMu` | main → frame | Main sets on `modeChanged`; frame thread recreates the swapchain between frames (doc 02). |
| Removal barrier | `CXRMonitorLayer::m_pendingRemoval` (`std::atomic<bool>`) | atomic; ack via channel [C] | main → frame → main | Main sets flag; frame thread drops the layer from its snapshot, destroys its swapchain/GL resources after the in-flight frame, acks; only then does main call `m_output->destroy()` (doc 02). |
| Run flag | `COpenXRManager::m_running` (`std::atomic<bool>`) | atomic | main → frame | `stop()` clears it, then joins. |
| Session-state transitions, input events, acks | SPSC queue drained by an `eventfd` registered on the wayland event loop | lock-free SPSC (single producer = frame thread, single consumer = main) | frame → main | Frame thread pushes + writes eventfd; main-thread callback drains and dispatches (IPC event emission, pointer injection, teardown acks). Queue infrastructure detailed in doc 04. |
| Frame pacing | `mon->m_output->scheduleFrame()` | none (aquamarine-internal) | frame → compositor | Called **from the frame thread only**, one deliberate exception to "main owns monitors" — proven in the WIP frame loop; only while session VISIBLE or FOCUSED. |

## Lifecycle state machine

`COpenXRManager` owns one top-level state (invented enum, keep exactly):

```cpp
enum eXRManagerState : uint8_t {
    XR_STATE_DISABLED = 0,   // openxr:enabled == 0, or stopped; no XR objects exist
    XR_STATE_UNAVAILABLE,    // start attempted: no runtime / xrCreateInstance or system lookup failed,
                             // or instance loss. DORMANT: a backoff reprobe timer retries start() while
                             // openxr:enabled + openxr:reprobe (report-17 WP-L3). User re-enable also works.
    XR_STATE_STARTING,       // start() in progress on the main thread
    XR_STATE_RUNNING_IDLE,   // session exists; XrSessionState IDLE/READY/SYNCHRONIZED/STOPPING
    XR_STATE_RUNNING_VISIBLE,// XrSessionState VISIBLE — quads composited, pacing active
    XR_STATE_RUNNING_FOCUSED,// XrSessionState FOCUSED — input active, idle-inhibit active
    XR_STATE_STOPPING,       // stop() in progress: joining frame thread, tearing down
};
```

```
                 openxr:enabled=1 at startup, config hot-toggle,
                 or `hyprctl openxr enable`
   DISABLED ───────────────────────────────► STARTING
      ▲                                          │
      │ stop() complete                          │ instance/system/session created,
      │                                          │ frame thread spawned
   STOPPING ◄──────────────┐                     ▼
      ▲                    │              RUNNING{idle}
      │ openxr:enabled=0,  │               ▲   │    ▲
      │ `hyprctl openxr    │  runtime      │   ▼    │  (XrSessionState changes,
      │  disable`, exit    │  STOPPING──►idle  VISIBLE  reported by frame thread
      │                    │                    │   ▲   via channel [C])
      └────────────────────┤                    ▼   │
                           │                  FOCUSED
   UNAVAILABLE ◄───────────┘
      ▲            EXITING → teardown → DISABLED
      │            LOSS_PENDING / XrEventDataInstanceLossPending → teardown → UNAVAILABLE
      └── STARTING failed (no runtime, xrCreateInstance/xrGetSystem failed)
```

Transition rules:

- `openxr:enabled` (default **0**) is **honored** — this is an explicit fix over the
  WIP, which registered nothing and initialized unconditionally. Three entry points,
  all funnel into `COpenXRManager::start()` / `COpenXRManager::stop()` on the main
  thread: (1) startup check in the manager ctor (constructed at `STAGE_LATE` in
  `CCompositor::initManagers`, after XWayland, same hook point as the WIP);
  (2) a dynamic config callback on `openxr:enabled` fired on config reload;
  (3) `hyprctl openxr enable|disable`.
- `UNAVAILABLE` is **dormant, not terminal** (report-17 WP-L3 / report-20 issue B1). While
  `openxr:enabled` and `openxr:reprobe` are set, a `CEventLoopTimer` re-attempts `start()` on a
  backoff: "waiting for the runtime" (no runtime/server) grows the delay from
  `openxr:reprobe_interval_ms` up to 30s; "waiting for the headset" (runtime up, `xrGetSystem` returns
  `XR_ERROR_FORM_FACTOR_UNAVAILABLE`) polls at the fixed interval. The timer is armed on entering
  UNAVAILABLE, disarmed (backoff preserved) on `STARTING`, and disarmed+reset on any running/disabled
  steady state. A subsequent explicit `start()` (hyprctl/config toggle) also retries from scratch —
  and `onConfigReload()` now starts from UNAVAILABLE too (WP-L7), so `hyprctl keyword openxr:enabled 1`
  is no longer a silent no-op. Session/instance loss lands here and thus auto-reconnects; `EXITING`
  (user quit XR from the runtime UI) lands in DISABLED and does not.
- `RUNNING` sub-states mirror `XrSessionState` and are driven by the frame thread's
  `pollEvents()`; the frame thread reports transitions over channel [C] so the main
  thread emits the `openxrsessionstate` socket2 event (payloads: `disabled`,
  `unavailable`, `starting`, `idle`, `visible`, `focused`, `stopping`) and pokes
  `recheckIdleInhibitorStatus()`. Emission mechanics belong to doc 05.
- Runtime-initiated `XR_SESSION_STATE_STOPPING` is **not** manager STOPPING: the
  frame thread calls `xrEndSession` and drops back to `RUNNING{idle}`, waiting for
  READY again. `EXITING` and `LOSS_PENDING` trigger full teardown coordinated with
  the main thread (doc 01, "Session state handling").
- Teardown ordering invariant (doc 01): **join the frame thread before destroying
  any EGL or XrInstance object**.
- Full compositor shutdown ordering (as built, WP13 reconciliation): `g_pOpenXRManager` is not
  left to the default global-destructor order. `CCompositor::cleanup()` resets it explicitly and
  very early — right after unloading plugins, well before `g_pInputManager`, `g_pSeatManager`,
  the renderer, or `g_pXWayland` are torn down. This avoids a use-after-free: the manager's
  destructor calls `stop()`, which (via `removePointerDevice()`, doc 04 §8) reaches into
  `CInputManager`/`CSeatManager` to detach the synthetic pointer — those must still be alive when
  that runs. See doc 01's "Teardown ordering" section for the full sequence and rationale.

## Build gating

Ported from the WIP `CMakeLists.txt` diff, verbatim in spirit:

```cmake
option(WITH_OPENXR "Build with OpenXR support" ON)
if(WITH_OPENXR)
  pkg_check_modules(openxr_dep IMPORTED_TARGET openxr)
  if(openxr_dep_FOUND)
    message(STATUS "OpenXR support enabled (${openxr_dep_VERSION})")
  else()
    message(STATUS "OpenXR not found, disabling OpenXR support")
  endif()
endif()
...
if(WITH_OPENXR AND openxr_dep_FOUND)
  target_compile_definitions(hyprland_lib PRIVATE HAVE_OPENXR=1)
  target_link_libraries(hyprland_lib PUBLIC PkgConfig::openxr_dep)
endif()
```

- Everything under `src/openxr/` is wrapped in `#ifdef HAVE_OPENXR` (headers compile
  to nothing without it, as the WIP header does).
- Touch points outside `src/openxr/`, each individually `#ifdef HAVE_OPENXR`-guarded:
  `src/Compositor.cpp` `initManagers(STAGE_LATE)` (manager construction),
  `src/Compositor.hpp` (header include), the idle-inhibit hook in
  `src/managers/input/IdleInhibitor.cpp` (doc 05), and the registration sites for
  config vars / hyprctl command / dispatcher (doc 05). Recommendation: register the
  `openxr:*` config vars and `xrmonitor` keyword **unconditionally** so user configs
  parse identically on non-XR builds; guard only behavior. Final call documented in
  doc 05's tables.
- `cmake --build build-debug --target Hyprland` must stay green both with and
  without the `openxr` package installed.

## WIP port-vs-redesign table

The WIP (`git show openxr:src/openxr/COpenXRManager.cpp`, 861 lines, single class)
proves the hard plumbing. Disposition of each piece:

| Piece | Disposition | Notes |
|---|---|---|
| Platform macro / include order (`XR_USE_PLATFORM_EGL`, `XR_USE_GRAPHICS_API_OPENGL_ES` before EGL/GLES, then openxr headers) | **Port verbatim** | doc 01 |
| Required-extension check + `XrGraphicsBindingEGLMNDX` session binding | **Port verbatim** | doc 01 |
| EGL config selection cascade (config-ID match → progressively permissive `eglChooseConfig` → manual scan; explicit `EGL_SURFACE_TYPE`) | **Port verbatim** | Mesa quirk workarounds, keep the comments |
| EGL context binding discipline (`eglMakeCurrent` only around GL bursts, `EGL_NO_CONTEXT` otherwise; never current across `xrCreateSwapchain`) | **Port verbatim** | Monado/Mesa `driUnbindContext` crash avoidance — doc 01 quotes the reasoning |
| Swapchain format enumeration + preference (SRGB8_ALPHA8 → RGBA8 → RGBA4; never pass an un-enumerated format — Monado may crash) | **Port verbatim** | doc 01 |
| dmabuf→`EGLImageKHR`→`samplerExternalOES` fullscreen-triangle blit (shaders, multi-plane attribs) | **Port verbatim** | doc 01 |
| Presented-buffer handoff (`m_frameMu` + `SP<IBuffer>` + atomic `m_haveNewFrame`) | **Port**, moved per-layer into `CXRMonitorLayer` | doc 02 |
| Frame pacing via `mon->m_output->scheduleFrame()` from the frame thread | **Port**, per-layer, gated on VISIBLE/FOCUSED | doc 02 |
| Teardown ordering (join thread → GL cleanup with context current → destroy context/display/GBM → destroy XR handles) | **Port** | doc 01 |
| GPU selection (sysfs walk picking PCI vendor `0x1002`/AMD; GBM platform instead of device platform) | **Adapt** | Default becomes "match Hyprland's primary GPU render node"; `openxr:gpu` explicit override; the AMD heuristic and GBM-platform choice are kept as documented rationale (cross-GPU dmabuf import crashes Monado) — doc 01 |
| CPU `glTexSubImage2D` fallback path | **Adapt** | WIP hard-codes a 1920×1080 staging texture (`initBlitGL`); fixed to size from the actual monitor mode, per layer — doc 01/02 |
| Virtual monitor creation (`impl->createOutput("OPENXR-1")`, hardcoded, at init) | **Adapt** | Becomes `COpenXRManager::createXRMonitor(SXRMonitorParams)` funnel, arbitrary count, lazy quad binding — doc 02 |
| Reference space (LOCAL only) | **Adapt** | LOCAL_FLOOR if `XR_EXT_local_floor`, else LOCAL + `openxr:floor_offset`; plus VIEW space — doc 01 |
| Stereo `XrCompositionLayerProjection` billboard (2 per-eye swapchains, `xrLocateViews`, same image both eyes) | **Replace** | Per-monitor `XrCompositionLayerQuad`, one swapchain per monitor sized to its pixel mode — docs 01/02 |
| Session-state handling (READY→begin, STOPPING→end only) | **Replace** | Full handling incl. SYNCHRONIZED/VISIBLE/FOCUSED sub-states, EXITING, LOSS_PENDING, `XrEventDataInstanceLossPending` — doc 01 |
| Unconditional init at STAGE_LATE; `openxr:enabled` registered but **never read** | **Replace** | Honored `openxr:enabled` + hot enable/disable + `hyprctl openxr enable|disable` → `start()`/`stop()` |
| Single monolithic `COpenXRManager` class | **Replace** | Module split per the component diagram |
| Lifecycle state machine + socket2 events, anchoring, XR input, IPC surface, layer cap, mode-change recreate, removal barrier, mirroring recipe, tests | **New** | docs 02–06 |

## Context files to read before implementing

- `/home/ajg/.claude/plans/i-want-you-to-enumerated-scone.md` — the approved plan (naming source of truth)
- WIP prototype: `git show openxr:src/openxr/COpenXRManager.cpp`, `git show openxr:src/openxr/COpenXRManager.hpp`, `git diff main...openxr -- CMakeLists.txt src/Compositor.cpp src/Compositor.hpp`
- `/home/ajg/code/Hyprland/CMakeLists.txt` — build system layout
- `/home/ajg/code/Hyprland/src/Compositor.cpp` — `initServer` backend creation (~315–352), `initAllSignals` newOutput (~443), `initManagers` `STAGE_LATE` (~706)
- `/home/ajg/code/Hyprland/src/Compositor.hpp` — where the WIP hooked its include
- `/home/ajg/code/Hyprland/src/output/Monitor.hpp` — `CMonitor` members and signals (`m_events.presented/modeChanged/destroy`)
- `/home/ajg/code/Hyprland/src/output/Monitor.cpp` — ctor listeners (~201/214), `onConnect` (~106), `setMirror` (~1330)
- `/home/ajg/code/Hyprland/src/state/MonitorState.cpp` — monitor add/remove flow, `Event::bus()` monitor events
- `/home/ajg/code/Hyprland/src/event/EventBus.hpp` — the event bus used for monitor lifecycle
- `/home/ajg/code/Hyprland/src/debug/HyprCtl.cpp` (lines 1743–1797) — existing `output create headless` path
- `/home/ajg/code/Hyprland/src/config/values/ConfigValues.cpp` — `getConfigValues()`, where `openxr:*` vars are declared (see doc 05 §1.1)
- `/home/ajg/code/Hyprland/src/managers/eventLoop/EventLoopManager.hpp` — wayland event loop integration (eventfd hosting)
