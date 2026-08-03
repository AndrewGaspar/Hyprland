# HypXRland — OpenXR Extension for Hyprland: Overview

HypXRland is Hyprland's built-in OpenXR client. When a headset (or an OpenXR runtime such
as Monado or WiVRn) is available, Hyprland can render any number of **virtual monitors** as
flat panels floating in 3D space around you, drive them with a controller/hand ray pointer,
and place them relative to the room, your head, your body, or a controller. Everything is
controlled through ordinary Hyprland config, keybinds, `hyprctl`, and socket2 events.

This page is the entry point: what the system does today, and a map to the rest of the docs.
Everything is behind the `HAVE_OPENXR` build flag; on a build without the `openxr` package the
whole feature compiles away and the config/keyword/dispatcher surfaces parse as clean no-ops.

## What it does today

- **Virtual monitors in XR.** Arbitrary on-demand headless outputs, each rendered as one
  `XrCompositionLayerQuad` — a flat panel showing that monitor's desktop. They are real
  Hyprland outputs: workspaces, windows, scale, VRR, and mirroring all work by name, exactly
  as for a physical display. Declare them with the `xrmonitor` keyword or spin them up at
  runtime.
- **Anchoring.** Each monitor is placed in one of four modes: `local` (bolted to the room),
  `head` (a HUD that follows your gaze with a damped spring), `body` (turns with you but not
  with head pitch), or `device:left|right` (rigidly locked to a controller). **Adaptive
  anchoring** lets a room-docked monitor pick itself up and follow you when you walk away from
  your desk, then re-dock when you return.
- **Input.** A controller or hand-tracking ray drives a synthetic pointer through Hyprland's
  normal input path, so clicks, focus, and scroll behave like a real mouse. Monitors can be
  grabbed and moved/resized with a controller grip or a hand pinch/grasp, using transparent
  on-panel **chrome** (a move-bar and corner handles).
- **Passthrough and environments.** Environment blend modes (`opaque`, `alpha` for
  passthrough, `additive`) composite your monitors over a black void or your real room.
  **Overlay** mode composites them on top of another running OpenXR app — a VR game, or the
  companion `hypxrpaper` ambient-background app.
- **Desktop integration.** Session state is exposed on socket2 and via `hyprctl openxr status`
  so bars and scripts can react; idle/lock is inhibited while you are in the headset; XR
  monitors act like external displays that plug and unplug as you don and doff the headset.
- **Multi-GPU / hybrid graphics.** The XR EGL context can be pinned to the runtime's GPU
  (`openxr:gpu`), buffers are allocated linear for cross-GPU import when needed, and the
  compositor refuses to start (rather than crash the driver) if it detects a GPU mismatch.

Runtimes exercised: **Monado** (including its null compositor for headless testing) and
**WiVRn** (e.g. Meta Quest 3, with hand tracking and passthrough). Overlay mode needs
`XR_EXTX_overlay`, which Monado and WiVRn provide and SteamVR-Linux does not.

## The doc set

| Doc | Scope |
|---|---|
| `00-overview.md` | (this doc) what the system does, architecture, threads, lifecycle |
| `01-session-graphics.md` | OpenXR session, EGL/GPU selection, the frame-thread loop, blit pipeline, blend modes, overlay sessions, teardown |
| `02-virtual-monitors.md` | the per-monitor layer, buffer handoff, pacing, mode changes, cross-GPU linear buffers, mirroring, the plug/unplug follow lifecycle |
| `03-anchoring.md` | the four anchor modes, pose/leash math, adaptive anchoring, recenter, layout persistence |
| `04-input.md` | action sets, the ray pointer, the grab state machine, grabbable chrome, hand pinch/grasp |
| `05-configuration.md` | user reference: every `openxr:*` var, the `xrmonitor` + `xrrule` keywords, the dispatcher, `hyprctl openxr`, socket2 events, idle integration, consumer recipes, overlay/hypxrpaper |
| `06-testing.md` | pure-math gtests, the Monado-backed integration suite, the containerized runner |
| `07-xreal.md` | the XREAL Air 2 Ultra 3DoF display rig (WP-XR1): udev, the xreal Monado build flavor, the `xreal-ctl` HID helper, the flat↔XR toggle, the 3DoF profile, and the live checklist |

A ready-to-copy config with every variable, several `xrmonitor` declarations, and example
binds lives at `example/openxr.conf`. For the XREAL Air 2 Ultra as a 3DoF display, see
`docs/openxr/07-xreal.md` and the profile at `example/xreal.conf`.

## Architecture

All code lives in `src/openxr/`, behind `#ifdef HAVE_OPENXR`:

```
src/openxr/
  OpenXRManager.{hpp,cpp}   COpenXRManager  — orchestrator, main-thread API, lifecycle state machine
  XRSession.{hpp,cpp}       CXRSession      — instance/system/session/spaces, XR event pump
  XRGraphics.{hpp,cpp}      CXRGraphics     — GBM/EGL display + context, GL blit resources, swapchains
  XRMonitorLayer.{hpp,cpp}  CXRMonitorLayer — one per virtual monitor: output ref, buffer handoff, swapchain, quad
  XRMonitorConfig.{hpp,cpp} SXRMonitorParams + parsers (pure, compiled unconditionally)
  XRAnchor.{hpp,cpp}        CXRAnchor       — anchor solve; pure math, no XR handles, gtest-able
  XRInput.{hpp,cpp}         CXRInput        — actions, xrSyncActions, ray cast, grab machine (frame thread)
  XRPointerDevice.{hpp,cpp} CXRPointerDevice— IPointer subclass, synthetic pointer (main thread)
  XRIpc.{hpp,cpp}           CXRIpc          — hyprctl "openxr" command surface
  XRMath.hpp                Vec3/Quat/pose + ray/quad/chrome helpers
```

`COpenXRManager` (global `g_pOpenXRManager`) is the only entry point the rest of Hyprland
touches. It is constructed at `STAGE_LATE` during compositor bring-up (after XWayland). The
`xrmonitor` keyword, the `xrmonitor` dispatcher, and the idle-inhibit hook are the only touch
points outside `src/openxr/`; the config vars, keyword, and dispatcher are registered
unconditionally so user configs parse identically on non-XR builds.

### Two threads

The extension runs on two threads with narrow, explicit handoffs:

```
==================== MAIN THREAD =====================    ================= XR FRAME THREAD =================
                                                          (spawned by COpenXRManager::start(); owns the XR
 ConfigManager ── openxr:* vars, xrmonitor keyword ──┐     EGL context exclusively while running)
 HyprCtl ──────── "openxr" command ──────────────────┤
 DispatcherTranslator ── "xrmonitor" dispatcher ─────┤       while (m_running):
                                                     v         CXRSession::pollEvents()      (state machine)
 ┌───────────────────────────────────────────┐                snapshot the layer set  ◄──────────────[A]
 │ COpenXRManager                            │  [A] layers    xrWaitFrame / xrBeginFrame
 │   start()/stop(), lifecycle state         │──────────►     per layer: latest buffer ◄──────────────[B]
 │   m_layers: vector<CXRMonitorLayer>       │                  dmabuf → EGLImage → blit → swapchain
 │   create/destroy XR monitors              │                  CXRAnchor::solve() → quad pose
 └──────────────┬────────────────────────────┘                CXRInput: xrSyncActions, ray cast, grab
                │ createOutput / destroy                       xrEndFrame(quad layer array)
                v                                              scheduleFrame() pacing (VISIBLE/FOCUSED)
 Aquamarine headless backend ─► CMonitor
                │ presented (per frame)
                v
 CXRMonitorLayer::onPresented() ── [B] per-layer buffer handoff ──► (read on frame thread)

 pointer injection, socket2      [C] SPSC queue + eventfd on the wayland event loop
 emission, idle recheck  ◄─────────────── session-state transitions, input events, acks ◄─── frame → main
```

**Main thread** owns monitor lifecycle, config, IPC, and input injection. The **XR frame
thread** exclusively owns the XR EGL context, the frame loop, blits, anchor solving, and
action sampling. Two disciplines keep the threads from corrupting shared state: the frame
thread performs **no hyprutils refcount operations** (layers cross threads as
`std::shared_ptr` — `PXRLAYER` — precisely because its control block is atomic and
hyprutils' is not), and it reads **no string config values** (parsed enums are published via
atomics). Details are in docs 01, 02, and 04.

### Handoff table

| Data | Field(s) | Guard | Direction | Protocol |
|---|---|---|---|---|
| Layer set + quad params | `COpenXRManager::m_layers` (`std::vector<PXRLAYER>`), per-layer anchor spec / size / z-order | `m_layersMu` (`std::mutex`) | main → frame | Main mutates on create/destroy/dispatcher/reload. The frame thread takes the lock once per frame, copies the layer pointers and each layer's quad params into a snapshot, releases. Never holds the lock during GL/XR calls. |
| Presented buffer | `CXRMonitorLayer::m_latestBuffer`, `m_haveNewFrame` (`std::atomic<bool>`) | per-layer `m_bufMu` + acquire/release on the atomic | main → frame | Main writes the buffer under the mutex then release-stores the flag; the frame thread acquire-loads, takes the mutex, moves the buffer out, clears the flag. |
| Retired buffers | `CXRMonitorLayer::m_retiredBuffers` | `m_bufMu` | frame → main | Consumed buffers are handed back for main-thread release (`releaseBuffers`), so buffer refcounts are only ever touched on the main thread. |
| Mode change | `m_swapchainDirty` (atomic) + `m_pendingSize` (under `m_bufMu`) | atomic + `m_bufMu` | main → frame | Main sets on bind / `modeChanged`; the frame thread recreates the swapchain between frames (doc 02). |
| Removal barrier | `m_pendingRemoval` (atomic); ack via channel [C] | atomic | main → frame → main | Main sets the flag; the frame thread drops the layer from its snapshot, destroys its swapchain/GL resources after the in-flight frame — dropping its refs **before** the ack — and only then does main destroy the output (doc 02). |
| Run flag | `COpenXRManager::m_running` (atomic) | atomic | main → frame | `stop()` clears it, then joins. |
| Session state, input events, acks | SPSC queue drained by an eventfd on the wayland event loop | lock-free SPSC | frame → main | The frame thread pushes and signals the eventfd; the main-thread callback drains and dispatches (socket2 emission, pointer injection, teardown acks). |
| Frame pacing | `mon->m_output->scheduleFrame()` | none (aquamarine-internal) | frame → compositor | Called from the frame thread only while the session is VISIBLE or FOCUSED (doc 02). |

### Lifecycle

`COpenXRManager` owns one top-level state:

```
DISABLED        openxr:enabled = 0, or stopped; no XR objects exist
UNAVAILABLE     start attempted but no runtime / no headset, or instance loss;
                dormant, with a backoff reprobe timer (openxr:reprobe) that retries automatically
STARTING        start() in progress on the main thread
RUNNING_IDLE    session exists; XrSessionState IDLE/READY/SYNCHRONIZED/STOPPING
RUNNING_VISIBLE XrSessionState VISIBLE — quads composited, pacing active
RUNNING_FOCUSED XrSessionState FOCUSED — input active, idle-inhibit active
STOPPING        stop() in progress: joining the frame thread, tearing down
```

- `openxr:enabled` (default `false`) is honored. Three entry points funnel into `start()` /
  `stop()` on the main thread: the startup check, a config-reload listener, and
  `hyprctl openxr enable|disable`.
- **UNAVAILABLE is dormant, not terminal.** While `openxr:enabled` and `openxr:reprobe` are
  set, a timer re-attempts `start()` so the session comes up automatically once the runtime
  starts or the headset is donned. "Waiting for the headset" — xrGetSystem
  `FORM_FACTOR_UNAVAILABLE`, **or a reachable runtime answering in a degraded mode** (WiVRn's
  service listens on its socket from startup and only advertises the real extension set once
  the headset connects) — polls at the fixed base interval; only a truly absent, quiet runtime
  grows the backoff (up to 30s). Session/instance loss lands here and thus auto-reconnects.
  An **event-driven** leg (`openxr:reprobe_watch`, default on) also inotify-watches
  `$XDG_RUNTIME_DIR`: the IPC socket appearing (`monado_comp_ipc` / `wivrn/comp_ipc`) **or the
  pid file being created/rewritten** (`monado.pid` / `wivrn.pid` — WiVRn's forked compositor
  server touches it exactly at headset-connect, the only don-time filesystem signal it emits)
  probes within ~150ms, resets the backoff to base, and keeps it capped at base for 60s of
  watched-dir activity; the timer stays as the fallback.
- `RUNNING` sub-states mirror `XrSessionState`, driven by the frame thread's event pump; each
  transition posts the `openxrsessionstate` socket2 event and re-runs the idle-inhibit check.
- Runtime-initiated `STOPPING` is not manager STOPPING — the frame thread ends the session
  and waits for READY again. A deliberate quit from the runtime UI (`EXITING`) lands in
  DISABLED; instance loss lands in UNAVAILABLE.
- On full compositor shutdown, `g_pOpenXRManager` is reset early in `CCompositor::cleanup()`
  (before the input manager, seat, renderer, and XWayland) so the synthetic pointer can be
  detached while those subsystems are still alive.

The complete state table and its `XrSessionState` mapping is in doc 01; the config/IPC
surface that observes and drives it is in doc 05.
