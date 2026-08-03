# HypXRland — Virtual Monitors & Quad Layers

This page covers `CXRMonitorLayer` (`src/openxr/XRMonitorLayer.{hpp,cpp}`) and the
monitor-facing surface of `COpenXRManager`: the create/destroy funnel, the
presented-buffer handoff, frame pacing, mode changes, cross-GPU linear buffers, the
plugged-state follow lifecycle, the runtime layer-count cap, and mirroring an XR
monitor onto a physical one. The thread model and handoff table live in the overview
page; the frame loop, swapchain rules, and EGL invariants live in the
session-graphics page. All code is `#ifdef HAVE_OPENXR`.

An **XR monitor** is an ordinary Aquamarine **headless output** — a real `CMonitor`
that Hyprland treats like any other (workspaces, monitor rules, screenshare, and
mirroring all work untouched) — paired with a `CXRMonitorLayer` that ferries its
presented buffers into an `XrSwapchain` and submits one `XrCompositionLayerQuad` per
frame. A monitor created while no XR session is running is a plain headless output;
its quad binds lazily when a session starts.

## `CXRMonitorLayer`

One instance per XR monitor, owned by `COpenXRManager::m_layers`
(guarded by `m_layersMu`; see the overview handoff table). The layer holds three
groups of state, each with a load-bearing thread owner (annotated per field in the
header):

- **Main thread:** the monitor key (`m_monitorName`, survives monitor teardown), a
  weak `PHLMONITORREF`, the three signal listeners (`presented`, `modeChanged`,
  `destroy`), the live anchoring engine (`m_anchor`; interface in the anchoring
  page), and the reconcile bookkeeping (`m_declaredByConfig`, `m_declaredAnchor`,
  `m_reqResolution`, `m_reqRefresh`, `m_userProvidedMode`).
- **Main → frame handoff:** `m_latestBuffer` / `m_retiredBuffers` / `m_pendingSize`
  under `m_bufMu`, plus the `m_haveNewFrame` / `m_swapchainDirty` /
  `m_pendingRemoval` atomics and the cached `m_monitorId`.
- **Frame thread only:** the `XrSwapchain`, its enumerated GL image handles, the
  swapchain/content/chrome sizes, the CPU-fallback staging texture, and the chrome
  fade state.

### The refcount thread-safety rule

hyprutils `CSharedPointer`/`CWeakPointer` refcounts are **plain unsigned ints, not
atomic**. An increment or decrement from the frame thread races the main thread's
copies of the same object and silently corrupts the count (observed as a `CMonitor`
freed while still owned by the monitor-state vectors). The layer is built around this
constraint:

- The frame thread never copies, destroys, or `lock()`s a hyprutils SP/WP whose impl
  the main thread also touches. The monitor facts it needs are cached as plain values
  written by the main thread: `m_monitorId` (atomic) and `m_pendingSize` (under
  `m_bufMu`).
- Presented buffer SPs are **handed back to the main thread** for their final release
  (`retireBuffer` → `releaseBuffers`); a `std::move` never touches the refcount.
- The layer itself crosses threads as a `std::shared_ptr` (`PXRLAYER` — a deliberate
  deviation from the codebase-standard hyprutils SP, because only `shared_ptr`'s
  control block is atomic), and the manager guarantees `~CXRMonitorLayer` runs on the
  main thread so its listener/WP teardown is safe.

## Create funnel

Everything that makes an XR monitor — the `xrmonitor=` config keyword, the
`xrmonitor create` dispatcher, and `hyprctl openxr create` (surfaces and syntax in
the configuration page) — parses into an `SXRMonitorParams` (name, optional pixel
mode, optional size, parsed anchor state) and funnels into one main-thread method:

```cpp
std::expected<PXRLAYER, std::string> COpenXRManager::createXRMonitor(const SXRMonitorParams& params);
void                                 COpenXRManager::destroyXRMonitor(const std::string& name);
```

`createXRMonitor` runs on the main thread and works in **every** manager state,
including when no session exists — that is what makes lazy binding possible:

1. **Validate uniqueness:** the name must not already belong to a monitor
   (`State::monitorState()->allMonitors()`) or an existing layer.
2. **Construct the layer** with a monotonic `m_seq` (used by the cap policy) and the
   quad width from `params` or `openxr:default_size` (default 1.6 m). Seed the
   anchoring engine; when the caller gave no explicit anchor (the create verb may
   omit one), place the monitor along the current gaze at `openxr:default_distance`
   (default 1.5 m), falling back to a fixed forward pose when there is no tracking
   yet. Push the layer into `m_layers` (still unbound).
3. **Create the headless output** by finding the `AQ_BACKEND_HEADLESS`
   implementation and calling `impl->createOutput(name)` — the same recipe as
   `hyprctl output create headless`. The `newOutput → monitorState()->add() →
   CMonitor` ctor + `onConnect` chain runs synchronously, so the monitor is
   queryable immediately by name. XR-created outputs are tagged
   `mon->m_xrManagedPlug = true` so the monitor-rule manager never re-enables them
   while the plug lifecycle holds them unplugged.
4. **Bind** (`bindToMonitor`): cache the monitor weak ref and `m_monitorId`, seed
   `m_pendingSize`, mark `m_swapchainDirty`, and connect the `presented` /
   `modeChanged` / `destroy` listeners. The `destroy` callback runs the external-
   destroy removal barrier (path B below).
5. **Apply the requested pixel mode**, if any. An explicit user `monitor=NAME,...`
   rule that already set a resolution **wins** (captured once as
   `m_userProvidedMode`). Otherwise the manager registers a **persistent named
   monitor rule** carrying the requested mode (`registerDeclaredMonitorRule`) so the
   declared resolution survives plug/unplug/reload — without it, every plug edge's
   `onConnect` would re-derive the mode and fall back to the headless default
   (1920x1080@60).
6. **Plugged-state gate:** with `openxr:monitors_follow_session` active (default
   `visible`), a monitor created while the session is not usable starts life
   **unplugged** — created with a stable id and mode, then immediately `onDisconnect`ed
   through the ordinary hotplug path, so a sessionless or doffed desktop never places
   workspaces on a display that isn't really there. It plugs in on the next
   session/visibility edge.
7. Emit the `xrmonitoradded` event.
8. **If a session is running:** decide cross-GPU linear buffers now
   (`applyCrossGpuLinear`) and mark `m_swapchainDirty` — the frame thread creates the
   swapchain on its next pass, since it re-snapshots `m_layers` every frame and needs
   no extra wakeup.
9. Recompute the layer-cap active set (`recomputeQuadActive`).

**Lazy binding:** on session start the manager walks `m_layers`
(`bindExistingLayers`): records whose named monitor still exists bind and are marked
dirty; records whose monitor disappeared while stopped are dropped.

## Presented-buffer handoff

The desktop's committed buffer reaches the frame thread through the layer, once per
presented frame:

```cpp
// main thread — connected in bindToMonitor():
m_presentedListener = mon->m_events.presented.listen([this]() {
    const auto pmon = m_monitor.lock();
    if (!pmon || !pmon->m_output || !pmon->m_output->state) return;
    auto buf = pmon->m_output->state->state().buffer;
    if (!buf) return;
    std::lock_guard lk(m_bufMu);
    retired.swap(m_retiredBuffers);   // release frame-consumed buffers here, on main
    m_latestBuffer = buf;             // SP keeps the buffer alive across threads
    m_haveNewFrame.store(true, std::memory_order_release);
});

// frame thread — once per frame (takeLatestBuffer):
if (m_haveNewFrame.load(std::memory_order_acquire)) {
    std::lock_guard lk(m_bufMu);
    buf = std::move(m_latestBuffer);  // MOVED out: no refcount op
    m_haveNewFrame.store(false, std::memory_order_relaxed);
}
```

The frame thread blits `buf` into the layer's swapchain (dmabuf EGLImage import, with
a CPU-staging fallback; blit details in the session-graphics page), then hands the SP
back via `retireBuffer`, which stashes it in `m_retiredBuffers`. The main thread
releases those refs in `releaseBuffers` — called from the `presented` listener each
frame and from every removal/teardown path — so a buffer SP's final decrement always
happens on the main thread. Which blit path last produced content
(none/dmabuf/cpu/black) is published per layer for `hyprctl openxr status`.

## Frame pacing

Headless outputs advertise a meaningless refresh rate and nothing else schedules
frames on an idle output, so the XR session must drive them. While the session is
**VISIBLE or FOCUSED**, the frame thread — once per `xrWaitFrame` iteration —
enqueues a `SCHEDULE_FRAMES` event onto the frame→main queue. The **main thread**
drains it and calls `mon->scheduleFrame()` for every bound, non-pending-removal
layer.

The scheduling call is deliberately main-thread-only: `CMonitor::scheduleFrame()`
lands in aquamarine's idle-callback vector, which the main thread concurrently drains
in `CBackend::dispatchIdle` with no lock — calling it from the frame thread would
corrupt the heap. The result is that the runtime's frame cadence (e.g. 90 Hz) drives
compositor renders of each visible XR monitor. When the session drops to
idle/synchronized, pacing stops and the outputs return to damage-driven rendering.

## Mode changes (swapchain recreate protocol)

A mode change is triggered by any `applyMonitorRule` that changes the pixel size
(user edits `monitor=XR-1,2560x1440@90` and reloads, an output state event that sets
`m_forceSize`, etc.) — all of which emit `mon->m_events.modeChanged`.

- **Main thread** (`m_modeChangedListener`): read `mon->m_pixelSize`; if it differs
  from `m_pendingSize`, write the new size under `m_bufMu` and set `m_swapchainDirty`.
  It never touches the swapchain.
- **Frame thread** (top of per-layer work, between frames): when
  `m_swapchainDirty.exchange(false)` is set, `createLayerSwapchain` destroys the old
  swapchain (context **not** current — the interop rule), creates a new one at the new
  size in the session's swapchain format, enumerates its GL images, and reallocates
  the CPU staging texture and chrome snapshot inside a scoped GL context.
  `m_hasContent` resets to false, keeping the quad out of the submitted layer array
  until the next blit lands (usually one frame).

Destroying the swapchain between frames is safe: no image of it is acquired at that
point, and a swapchain absent from the current frame's `xrEndFrame` array is not
referenced by the runtime's next composite. The quad's aspect follows the pixel mode
automatically (`height = width * pxH/pxW`, computed from the content rect).

### Chrome margins

The swapchain is allocated as **content plus a transparent alpha margin**, not
content-only. `createLayerSwapchain` takes the monitor's pixel mode as the inner
content rect and expands it by the configured chrome margins; the desktop blits into
the inner content rect (`m_contentSize` / `m_contentOffsetPx`) and the margin holds
the grab affordances (move-bar and corner handles). The `size:` meters always mean
**content** width, and the submit path grows the quad to full-quad meters and shifts
the pose so the content stays exactly where the anchor placed it. With
`openxr:chrome_enabled = 0` all margins collapse to zero and the swapchain is
content-only. The chrome interaction model (hit regions, fade, grab/resize) is
covered in the input page.

## Cross-GPU linear buffers

Desktop buffers are **native-tiled by default**. They are allocated
`DRM_FORMAT_MOD_LINEAR` only when `openxr:force_linear` engages — `on` forces it,
`off` disables it, and `auto` (the default) forces linear **only when a cross-GPU
split is detected** between the XR runtime's EGL render node and the output's
buffer-allocator DRM node.

`applyCrossGpuLinear` (main thread) resolves both DRM nodes, calls
`OpenXR::shouldForceLinear` (which forces linear in `auto` only when both nodes are
positively known and differ), and flips `mon->m_forceLinearSwapchain`. On a change it
forces aquamarine's full-reconfigure path so the buffers are actually re-allocated
with the new modifier, then logs the modifier the buffers actually carry. Linear
tiling costs some compositing throughput; it is the accepted cost of letting a
second GPU import the desktop's buffers. The decision needs the XR EGL node, which
only exists once a session is up, so monitors created while stopped get it at
`bindExistingLayers`. The per-monitor linear state is reported in `hyprctl openxr
status`.

## Destroy paths (removal barrier)

The frame thread may be mid-frame with a layer's swapchain acquired and a buffer SP in
hand, so `m_output->destroy()` must not run until the frame thread has let go. The
barrier removes the layer from the frame thread's snapshot set and waits for an ack
before the output is destroyed on the main thread.

**A. XR-initiated** (`destroyXRMonitor` from the dispatcher/hyprctl, or
`destroy_monitors_on_stop`):

1. Main: find the layer, `stopMainListeners()` (no new buffers/mode changes queued),
   set `m_pendingRemoval`.
2. Frame thread: layers with `m_pendingRemoval` are excluded from the per-frame
   snapshot (never blitted or submitted again); after the current `xrEndFrame` it
   destroys the layer's frame-side resources (GL objects with the context current,
   then the swapchain with the context not current), drops its refs, and enqueues a
   `LAYER_REMOVED` ack onto the frame→main queue.
3. Main, on the ack (`finalizeLayerRemoval`): release any queued/retired buffers,
   erase the layer from `m_layers`, then destroy the output. Recompute the layer cap
   and emit `xrmonitorremoved`.
4. **No session running** (no frame thread): skip the barrier — release buffers,
   erase, destroy the output directly.

**B. External destroy** (the monitor dies first — `hyprctl output destroy`, backend
teardown): `m_destroyListener` fires on the main thread and runs the same handoff. If
a session is running it goes through the barrier; otherwise it finalizes directly. The
output is already gone, so step 3 skips `m_output->destroy()`, and every frame-thread
access already tolerates a dead monitor ref through its `.lock()` guards.

The output is torn down through `destroyOutputDeferred`, which works around an
aquamarine headless-output lifetime issue: `CHeadlessOutput::scheduleFrame()` queues
the output's own frame callback (raw `this`, no liveness guard) into the backend's
idle list, and neither `destroy()` nor the destructor removes it — freeing the output
with a callback still queued would emit on freed memory on the next `dispatchIdle`.
Because XR pacing schedules a frame on the output almost every runtime frame, this is
a hot path during create/destroy churn. `destroyOutputDeferred` calls `destroy()`
(which clears the frame listener, so the stale callback becomes an inert emit) and
then keeps a reference to the output alive inside a **sentinel idle event** queued
after any pending callback, dropping the last reference only once the idle queue has
drained past it.

**C. Session stop:** the frame thread is already joined, so no barrier is needed.
With `openxr:destroy_monitors_on_stop` (default false) the outputs and layer records
are kept for the next session start (swapchain unbound, lazy binding re-runs); with it
set, every `m_createdByXR` layer is destroyed. When monitors are kept and
`openxr:monitors_follow_session != off`, session stop **unplugs** them immediately (no
grace) so a sessionless desktop has no phantom monitors.

## Plugged-state follow lifecycle

`openxr:monitors_follow_session` controls when XR monitors behave like **unplugged
external monitors** — held disabled, with their workspaces evacuated to the remaining
monitors exactly like a physical unplug and returned by name on replug:

- `off` — never unplug (always present).
- `session` — plugged while any OpenXR session exists.
- `visible` (default) — plugged only while the session is actually being worn.

Under `visible`, the plug gate (`monitorsShouldBePluggedNow` →
`OpenXR::wantXRMonitorsPlugged`) requires a live session that is **VISIBLE/FOCUSED**
and, when the runtime exposes `XR_EXT_user_presence` (e.g. WiVRn), also **user
presence** — both signals must currently agree. This is because a service-mode runtime
keeps a session alive with the headset sitting doffed on a shelf, and WiVRn's presence
signal can stick `present` while doffed; requiring visibility too lets a doff unplug
even when presence is stuck. `session` mode would leave those shelved monitors always
plugged, which is why `visible` is the default.

The lifecycle funnels through `updateMonitorsPlugged`, driven from the session-state
and user-presence edges on the frame→main queue:

- **Donning** (edge to VISIBLE/FOCUSED, or presence becoming present) plugs
  immediately and cancels any pending unplug.
- **Doffing** (drop to IDLE/SYNCHRONIZED, or presence becoming absent) arms a one-shot
  `CEventLoopTimer` for `openxr:monitor_unplug_grace_ms` (default 20000) rather than
  unplugging at once; donning within the grace cancels it, so a quick glance away
  never rearranges workspaces.
- **First plug of a session:** in `visible` mode the first plug waits until visibility
  has been continuously sustained past `openxr:monitor_plug_settle_ms` (default 1500),
  guarding against runtimes that sprint to VISIBLE/FOCUSED at session creation even
  while doffed. Later plugs use the grace instead and never defer.

`setMonitorsPlugged` is the pure applicator: it drives each session-following,
XR-created monitor to the target state with the same `mon->onConnect(true)` /
`mon->onDisconnect()` the rule manager uses for a `monitor=...,disable` flip. It never
touches adopted pre-existing monitors and never destroys the output.

### Recenter on plug

With `openxr:recenter_on_plug` (default true), the **first don of a session** re-seats
`anchor:local` monitors relative to the current head pose instead of the runtime's
(often arbitrary) LOCAL_FLOOR origin. The main thread arms a flag before plugging; the
frame thread, which owns the head pose, consumes it on its next valid-view frame and
calls `recenterLocalToHead` on every layer, passing the same head pose to all of them
so a multi-monitor layout is transformed **rigidly** (relative arrangement preserved).
A brief doff-and-don within the same session does **not** re-seat — the head-relative
pose from the first don is kept.

## Declared vs runtime monitors

Monitors declared with the `xrmonitor=` config keyword are tagged `m_declaredByConfig`
and reconciled against live layers on every config reload
(`reconcileDeclaredMonitors`): declared-but-missing entries are created;
declared-and-live entries are diffed for mode/anchor/size changes; and a declared line
that disappears destroys its live monitor. Monitors created at runtime (dispatcher or
hyprctl) are **never touched** by reconciliation — removing a declared line does not
destroy a runtime monitor, and a name collision leaves the runtime monitor alone.
Declared monitors persist their declared mode (via the persistent monitor rule) and
re-materialize on the next session start. The keyword grammar and reconcile semantics
are in the configuration page.

## Layer-count limit

`CXRSession::m_maxLayerCount` is `XrSystemGraphicsProperties::maxLayerCount` from
`xrGetSystemProperties` (the spec guarantees at least 16). Every layer we submit in
`xrEndFrame` is a quad, and their total must not exceed that cap.

The policy is **recency-wins** and creating a monitor never fails for cap reasons: the
newest monitor always functions as an output and gets a quad. When the active-quad
count would exceed the cap, the **oldest** quads (lowest `m_seq`) are suspended —
`m_quadActive = false`, so the quad stops being submitted, but the monitor keeps
rendering as a normal paced headless output (still reachable via mirroring, and
re-activated newest-first when capacity frees). Each transition logs a `Log::WARN`
naming the monitor and emits an `xrmonitorquad>>NAME,0|1` event. `recomputeQuadActive`
computes the active set on the main thread whenever the layer set changes, so
`hyprctl openxr status` matches what is rendered, and the frame thread applies the same
cap as a final truncate.

**Composition order** is depth-sorted per frame. OpenXR composites the `xrEndFrame`
layer array in submission order (later entries draw on top) with no regard for 3D
position, so the frame thread orders quads by their freshly solved distance from the
viewer — farthest first — so nearer quads occlude farther ones. `m_zOrder` is an
explicit override tier compared first; creation `m_seq` breaks remaining ties.

## The 2D layout plane (2D-plane sync)

An XR monitor is still an ordinary `CMonitor` in Hyprland's flat 2D layout plane, and that plane —
not the 3D arrangement — is what governs the mouse. `CPointerManager::closestValid` clamps the
pointer to the **union of monitor boxes**, and `CMonitorQueryCore::directionLookup` only finds a
neighbour when the facing edges are within `STICKS` (2 px) **and** the perpendicular ranges overlap.
So the 2D box positions decide where the cursor crosses and where `movefocus` goes, and they
originally had nothing to do with where the quad floats: a new XR monitor takes the default monitor
rule (offset sentinel `{-INT32_MAX, -INT32_MAX}` = "auto") and
`CMonitorPositionController::arrange` appends it flush-right in **creation order** at `y = 0`.

`COpenXRManager::syncLayout2D()` closes that gap. It projects each quad's world (or follow-frame)
centre to an (azimuth, elevation) pair about a latched reference eye, scales both by
`openxr:layout2d:px_per_degree`, and compacts the result into gap-free, overlap-free rows and
columns — then writes each monitor's `m_activeMonitorRule.m_offset` and calls
`CMonitorLayoutController::scheduleRecheck()`, so the ordinary `arrange()` pipeline does the
placement, the xdg-output update and the `monitor.layoutChanged` emission. `CMonitor::moveTo` then
shifts floating windows by the delta, re-arranges the monitor's layer surfaces, relayouts tiled
windows and re-clamps the cursor.

Threading and cadence follow the rules the rest of this page sets out: the whole pass is **main
thread**, taking its pose snapshot under `m_layersMu` exactly as `layoutDump()` does (the frame
thread already publishes `CXRAnchor::lastWorld()`, so no new cross-thread plumbing and no refcount
work off-main), and it is **event-driven and debounced** — monitor add/remove, plug/unplug, a grab
RELEASE, an adaptive dock/undock, a pose verb, a config reload, or an explicit `sync-layout`. It
never runs per frame, and it refuses to run at all while any quad is being carried (it re-arms
instead, so the release still gets its relayout).

The projection itself is pure and lives in `src/openxr/XRLayout2D.{hpp,cpp}` (compiled
unconditionally, gtested in `tests/xr/layout2d.cpp`). The user-facing surface — `openxr:layout2d:*`,
`hyprctl openxr sync-layout`, the per-monitor pin opt-out, the reference-frame semantics — is
documented in [`05-configuration.md` §2](05-configuration.md#2d-plane-sync-layout2d); the design
rationale is [`research/archive/12-spatial-2d-layout.md`](research/archive/12-spatial-2d-layout.md).

## Mirroring an XR monitor onto a physical one

Seeing an XR monitor on a physical display needs **no XR code** — the ordinary
`monitor=` mirror machinery handles it, because the source's backend is irrelevant to
mirroring:

```ini
# hyprland.conf — mirror XR-1 onto DP-1
monitor = DP-1, preferred, auto, 1, mirror, XR-1
```

(or at runtime: `hyprctl keyword monitor "DP-1, preferred, auto, 1, mirror, XR-1"`.)

- `applyMonitorRule` calls `CMonitor::setMirror("XR-1")` (`src/output/Monitor.cpp`),
  which resolves the source by name, moves DP-1's workspaces to a backup monitor, sets
  `DP-1.m_mirrorOf = XR-1`, appends DP-1 to `XR-1.m_mirrors`, and emits the layout-
  changed event.
- On the **source** side, `XR-1.m_mirrors` being non-empty makes
  `CMonitor::needsACopyFB()` return true, so the renderer keeps a mirror framebuffer
  and saves each composited frame of XR-1 into it — the same copy-FB path screenshare
  uses.
- On the **mirror** side, DP-1's render path sees `isMirror()` and calls
  `IHyprRenderer::renderMirrored()`, which draws the source's mirror texture scaled and
  letterboxed onto DP-1.
- The mirror FB is a GL texture copy made inside the compositor at composite time, so a
  headless source is indistinguishable from a DRM one. It composes cleanly with the XR
  quad blit, which independently consumes the *presented buffer* — the two consumers
  never touch the same object.
- Pacing: XR-1 renders when the frame thread paces it (session visible) or when
  damaged, and each new XR-1 frame damages its mirrors, so DP-1 updates at min(XR
  cadence, DP-1 refresh). An idle session simply shows the last rendered frame.

## Status observability

`hyprctl openxr status` reports, per monitor: `plugged` (the output's enabled state),
`linear` (cross-GPU linear buffers), `contentPath` (which blit path last produced the
layer's content), `grabbed` / `grabKind` (move vs resize), `hovered` (last ray hover),
and the adaptive-anchoring phase. Field formats are in the configuration page.

## Related pages

- Overview — thread model and handoff table (normative for the field ownership above).
- Session & graphics — frame loop, swapchain creation, EGL context invariant, blit,
  blend mode, teardown ordering.
- Anchoring — `SXRAnchorState`, `CXRAnchor::solve()`, and how the frame thread poses
  each quad; from this page an anchor is opaque (main thread mutates `m_anchor` under
  `m_layersMu`, frame thread solves its snapshot).
- Input — the ray pointer that targets quads and routes `motionAbsolute` to the hit
  monitor, plus the chrome grab/resize model.
- Configuration — the `xrmonitor=` keyword, the `xrmonitor` dispatcher and `hyprctl
  openxr` subcommands, the event surface (`xrmonitoradded`, `xrmonitorremoved`,
  `xrmonitorquad`, ...), and the `hyprctl openxr status` field reference.
- Testing — the hyprtester coverage, including the mirroring integration test.
