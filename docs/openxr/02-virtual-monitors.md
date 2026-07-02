# HypXRland — Virtual Monitors & Quad Layers

Doc 02 of the `docs/openxr/` set. Covers `CXRMonitorLayer`
(`src/openxr/XRMonitorLayer.{hpp,cpp}`) and the monitor-facing API of
`COpenXRManager`: the create/destroy funnel, presented-buffer handoff, frame
pacing, mode changes, the layer-count cap, and the (zero-code) mirroring recipe.
Read doc 00 (thread model, handoff table) and doc 01 (frame loop, swapchain rules,
EGL invariants) first. All code `#ifdef HAVE_OPENXR`.

Core idea: an "XR monitor" is an ordinary **Aquamarine headless output** — a real
`CMonitor` that Hyprland treats like any other (workspaces, rules, screenshare,
mirroring all work untouched) — plus a `CXRMonitorLayer` that ferries its presented
buffers into an `XrSwapchain` and submits one `XrCompositionLayerQuad` per frame.
Monitors created while no XR session runs are **plain headless outputs**; their
quads bind lazily when a session starts.

## How headless outputs work today (verified on main)

- Creation: find the headless backend implementation and call
  `impl->createOutput(name)` — exactly what `hyprctl output create headless NAME`
  does (`src/debug/HyprCtl.cpp:1743-1797`, `dispatchOutput`). That fires
  `m_aqBackend->events.newOutput` (`src/Compositor.cpp:443`) →
  `State::monitorState()->add(output)` (`src/state/MonitorState.cpp:108-148`) which
  constructs the `CMonitor`, emits `Event::bus()->m_events.monitor.newMon`, and runs
  `CMonitor::onConnect(false)` — all synchronously on the main thread.
- Non-DRM outputs get `m_createdByUser = true` (`src/output/Monitor.cpp:257-258`),
  which permits `hyprctl output destroy` and enables resize-by-state-event.
- Normal `monitor=` rules apply to headless outputs **by name**
  (`Config::monitorRuleMgr()->get()` in `onConnect`, `Monitor.cpp:261`).
- Resizing: the aq output's state event with a size sets `m_forceSize` and re-runs
  `applyMonitorRule` (`Monitor.cpp:214-242`); rule changes emit
  `m_events.modeChanged`.
- Destruction: `PMONITOR->m_output->destroy()` → the monitor's destroy listener
  (`Monitor.cpp:201-212`) runs `onDisconnect(true)` and
  `State::monitorState()->remove()` → `Event::bus()->m_events.monitor.destroyMon`.
- Every presented frame emits `mon->m_events.presented` on the main thread
  (`Monitor.cpp:196-199`); the committed buffer is
  `mon->m_output->state->state().buffer` (WIP-verified).

## `CXRMonitorLayer`

One instance per XR monitor, owned by `COpenXRManager::m_layers`
(`std::vector<SP<CXRMonitorLayer>>`, guarded by `m_layersMu` — doc 00 handoff
table). Thread ownership is annotated per field and is load-bearing:

```cpp
// src/openxr/XRMonitorLayer.hpp
class CXRMonitorLayer {
  public:
    // ---- main thread ----
    std::string             m_monitorName;             // key; survives monitor teardown
    PHLMONITORREF           m_monitor;                 // weak ref to the headless output's CMonitor
    CHyprSignalListener     m_presentedListener;       // mon->m_events.presented
    CHyprSignalListener     m_modeChangedListener;     // mon->m_events.modeChanged
    CHyprSignalListener     m_destroyListener;         // mon->m_events.destroy (external destroy)
    bool                    m_createdByXR = true;      // false for xrmonitor-adopted pre-existing outputs

    // ---- main → frame handoff (see doc 00 table) ----
    std::mutex              m_bufMu;
    SP<Aquamarine::IBuffer> m_latestBuffer;            // written under m_bufMu on presented
    Vector2D                m_pendingSize;             // written under m_bufMu on mode change
    std::atomic<bool>       m_haveNewFrame{false};     // release-store after buffer write
    std::atomic<bool>       m_swapchainDirty{false};   // set on mode change / (re)bind
    std::atomic<bool>       m_pendingRemoval{false};   // removal barrier flag

    // ---- quad params: main writes under COpenXRManager::m_layersMu,
    //      frame thread copies into its per-frame snapshot ----
    SP<CXRAnchor>           m_anchor;                  // anchor mode + pose; interface in doc 03
    float                   m_sizeMeters = 1.6f;       // quad width (m); height = width * pxH/pxW
    int                     m_zOrder     = 0;          // explicit composition tier override (see below)
    uint64_t                m_seq        = 0;          // creation sequence, monotonic (cap policy)

    // ---- frame thread only (touched only between xrBeginFrame/xrEndFrame or teardown) ----
    XrSwapchain             m_swapchain = XR_NULL_HANDLE;
    std::vector<uint32_t>   m_swapchainImages;         // GLuints from XrSwapchainImageOpenGLESKHR
    Vector2D                m_swapchainSize;           // size the swapchain was created at
    XR_GLuint               m_cpuTex     = 0;          // CPU-fallback staging tex, sized to mode
    XR_EGLImageKHR          m_lastEGLImg = nullptr;    // last dmabuf EGLImage (destroyed on next blit)
    bool                    m_hasContent = false;      // at least one successful blit since (re)create
    bool                    m_quadActive = true;       // false while suspended by the layer cap
};
```

The presented-buffer handoff is the **WIP's proven pattern**
(`onMonitorPresented` / the frame-loop grab in
`git show openxr:src/openxr/COpenXRManager.cpp`), moved from manager-global to
per-layer:

```cpp
// main thread — connected in bindToMonitor():
m_presentedListener = mon->m_events.presented.listen([this]() {
    const auto mon = m_monitor.lock();
    if (!mon) return;
    auto buf = mon->m_output->state->state().buffer;
    if (!buf) return;
    std::lock_guard lk(m_bufMu);
    m_latestBuffer = buf;                                  // SP keeps the buffer alive across threads
    m_haveNewFrame.store(true, std::memory_order_release);
});

// frame thread — once per frame (doc 01 loop):
SP<Aquamarine::IBuffer> buf;
if (m_haveNewFrame.load(std::memory_order_acquire)) {
    std::lock_guard lk(m_bufMu);
    buf = std::move(m_latestBuffer);
    m_haveNewFrame.store(false, std::memory_order_relaxed);
}
```

## Create funnel

Everything that makes an XR monitor — `xrmonitor=` config keyword,
`xrmonitor create` dispatcher, `hyprctl openxr create` (surfaces + syntax in
doc 05) — funnels into one main-thread method:

```cpp
struct SXRMonitorParams {
    std::string                m_name;                 // e.g. "XR-1"; must be unique
    std::optional<Vector2D>    m_resolution;           // WxH; absent => headless default (1920x1080)
    std::optional<float>       m_refreshRate;          // @Hz part
    SXRAnchorState             m_anchor;               // parsed initial anchor state — struct defined in doc 03
                                                       // (its widthMeters is seeded from m_sizeMeters below)
    std::optional<float>       m_sizeMeters;           // absent => *openxr:default_size (1.6)
};

// returns the layer, or an error string for the IPC caller
std::expected<SP<CXRMonitorLayer>, std::string> COpenXRManager::createXRMonitor(SXRMonitorParams params);
void                                            COpenXRManager::destroyXRMonitor(const std::string& name);
```

`createXRMonitor` flow (main thread, works in **every** manager state including
DISABLED — that is what makes lazy binding possible):

1. Validate: name not already used by any monitor
   (`State::monitorState()->allMonitors()` by `m_name` — same checks as
   `dispatchOutput`, HyprCtl.cpp:1753-1762) and no existing layer with that name.
2. Construct the `CXRMonitorLayer` with `m_seq = ++m_seqCounter`, quad params from
   `params` (defaults from `openxr:default_size` / `openxr:default_distance` via the
   anchor spec, doc 03); push into `m_layers` under `m_layersMu` (still unbound).
3. Find the headless implementation and create the output — same recipe as
   `dispatchOutput` and the WIP's `createVirtualMonitor()`:

   ```cpp
   for (auto const& impl : g_pCompositor->m_aqBackend->getImplementations()) {
       if (impl->type() == Aquamarine::AQ_BACKEND_HEADLESS) { impl->createOutput(params.m_name); break; }
   }
   ```

   The `newOutput` → `monitorState()->add()` → `CMonitor` ctor + `onConnect` chain
   runs synchronously, so immediately afterwards
   `State::monitorState()->query().name(params.m_name).run()` yields the monitor.
   No headless implementation (should not happen — headless is
   `AQ_BACKEND_REQUEST_MANDATORY`, Compositor.cpp:320-321) → remove the layer,
   return an error.
4. `bindToMonitor(mon)`: set `m_monitor`, connect the three listeners
   (`presented`, `modeChanged`, `destroy`).
5. Apply the requested resolution, if any: copy the monitor's matched rule, set
   `m_resolution` (and refresh), `mon->applyMonitorRule(std::move(rule))`. An
   explicit user `monitor=` rule matching this name **wins** — skip this step when
   `Config::monitorRuleMgr()->get(mon)` matched a non-default rule. (`xrmonitor=`
   owns existence + XR placement only; display properties stay with `monitor=`.)
6. Emit `xrmonitoradded` socket2 event (doc 05).
7. If a session is running: `m_swapchainDirty = true` — the frame thread creates the
   swapchain on its next pass (it already snapshots `m_layers` per frame, so no
   extra wakeup is needed).

**Lazy quad binding**: on `start()` (doc 01, step 8), the manager walks `m_layers`;
records whose monitor still exists get bound (if not already) and marked dirty;
records whose named monitor disappeared while disabled are dropped. Declared
`xrmonitor=` entries are reconciled against live layers on config reload —
declared-but-missing get created, layers created at runtime (dispatcher/hyprctl)
are **left alone**; removal of a declared line does not destroy a live monitor
(reconcile semantics + parsing live in doc 05).

## Frame pacing

Headless outputs advertise a meaningless refresh rate, and nothing else schedules
frames on an idle output. The WIP's mechanism (frame-loop comment: *"Ask Hyprland to
render OPENXR-1 — this drives the ~90Hz render rate on the virtual monitor
regardless of what mode it advertises"*) is kept, per layer:

- The frame thread calls `mon->m_output->scheduleFrame()` for **each bound,
  non-pending-removal layer**, once per XR frame-loop iteration, **only while the
  session is VISIBLE or FOCUSED** (doc 01 loop). This is the single sanctioned
  cross-thread monitor call (doc 00 handoff table) — never call it from the frame
  thread outside those states, and never render/damage from the frame thread.
- Result: the XR runtime's `xrWaitFrame` cadence (e.g. 90 Hz) drives compositor
  renders of each visible XR monitor; when the session drops to idle/synchronized,
  pacing stops and the outputs go back to damage-driven rendering.

## Mode changes (swapchain recreate protocol)

Trigger paths: user edits `monitor=XR-1,2560x1440@90,...` and reloads;
`m_forceSize` via the output state event; any `applyMonitorRule` that changes the
pixel size (all emit `m_events.modeChanged`).

- **Main thread** (`m_modeChangedListener`): read `mon->m_pixelSize`; if it differs
  from the last size sent, write it to `m_pendingSize` under `m_bufMu` and
  `m_swapchainDirty.store(true)`. Never touches the swapchain.
- **Frame thread** (top of per-layer work, between frames — after the previous
  `xrEndFrame`, before any acquire on this swapchain):

```
if l.m_swapchainDirty.exchange(false):
    newSize = (lock m_bufMu) l.m_pendingSize
    if l.m_swapchain: xrDestroySwapchain(l.m_swapchain)     # context NOT current (doc 01 invariant)
    create swapchain: format = session.m_swapchainFormat, width/height = newSize   (doc 01 rules)
    enumerate XrSwapchainImageOpenGLESKHR images -> l.m_swapchainImages
    l.m_swapchainSize = newSize; l.m_hasContent = false
    { CScopedGLContext: realloc l.m_cpuTex to newSize, destroy l.m_lastEGLImg }     # CPU-fallback staging
```

  Destroying between frames is safe: no image of this swapchain is acquired, and
  a swapchain absent from the current frame's `xrEndFrame` array is not referenced
  by the runtime's next composite. The quad disappears for the (usually one) frame
  until the next blit lands — acceptable; `m_hasContent = false` keeps it out of
  the layer array until then. The quad's aspect follows automatically
  (`height = m_sizeMeters * h / w`).

## Destroy paths (removal barrier)

The frame thread may be mid-frame with the layer's swapchain acquired and the
buffer SP in hand, so `m_output->destroy()` must not run until the frame thread has
let go. **Barrier: the layer is removed from the frame thread's snapshot set and
any in-flight frame has completed before the output is destroyed on the main
thread.**

**A. XR-initiated (`destroyXRMonitor`, dispatcher/hyprctl/`destroy_monitors_on_stop`):**

1. Main: find layer, `m_pendingRemoval.store(true)`. Disconnect
   `m_presentedListener` / `m_modeChangedListener` (no new buffers queued).
2. Frame thread, at the snapshot point of its next iteration: layers with
   `m_pendingRemoval` are excluded from the snapshot (so never blitted or
   submitted again); after `xrEndFrame` of the current iteration it destroys the
   layer's frame-side resources (`xrDestroySwapchain` context-not-current; EGLImage
   + `m_cpuTex` inside a `CScopedGLContext`) and pushes a
   `layer-removed(name)` ack onto the frame→main queue (channel [C], eventfd).
3. Main, on ack: erase the layer from `m_layers` under `m_layersMu`, then
   `mon->m_output->destroy()` → normal `CMonitor` teardown → emit
   `xrmonitorremoved` (doc 05).
4. **No session running** (no frame thread): skip the barrier — destroy frame-side
   resources directly if any linger (there should be none while DISABLED), erase,
   `m_output->destroy()`.

**B. External destroy (monitor dies first — `hyprctl output destroy XR-1`, backend
teardown):** `m_destroyListener` fires on the main thread → same as (A) but step 3
skips `m_output->destroy()` (already gone) and `m_monitor` is already expired; the
layer must tolerate a dead monitor ref between flag and ack (every frame-thread
access already goes through `.lock()` guards).

**C. Session stop (doc 01 teardown, after the join):** frame thread is gone, so no
barrier needed. If `openxr:destroy_monitors_on_stop` (default 1): destroy every
layer with `m_createdByXR` via the no-session path of (A). Else: keep the outputs
running as plain headless monitors and keep the (now unbound) layer records for the
next `start()`.

## Layer-count limit

`CXRSession::m_maxLayerCount` = `XrSystemGraphicsProperties::maxLayerCount` from
`xrGetSystemProperties` (doc 01; the spec guarantees ≥ 16). Total layers submitted
in `xrEndFrame` must not exceed it — ours are all quads (plus nothing else; we
submit no projection layer).

Policy (recency wins): creating a monitor **never fails** for cap reasons — the
newest monitor always functions as an output and gets a quad; when the active-quad
count would exceed `m_maxLayerCount`, the **oldest** quads (lowest `m_seq`) are
dropped: `m_quadActive = false`, the layer stops being submitted, but its monitor
keeps rendering as a normal (paced) headless output — reachable via mirroring or
re-activation. On each change: `Log::WARN` naming the suspended monitor + a socket2
notification (`xrmonitorquad>>NAME,0|1` — final event table in doc 05). When
capacity frees (a monitor is destroyed), suspended layers re-activate newest-first,
also with the event.

Frame-thread enforcement is the sort/truncate step in the doc 01 loop pseudocode;
the manager recomputes `m_quadActive` flags on the main thread whenever the layer
set changes (under `m_layersMu`), so IPC state (`hyprctl openxr status`) matches
what is rendered.

AS-BUILT AMENDMENT — composition order is depth-sorted per frame: OpenXR composites
the `xrEndFrame` layer array in submission order (later entries on top) with no
regard for 3D position, so the frame thread orders quads by their freshly solved
distance from the viewer, farthest first — nearer quads therefore occlude farther
ones, as expected in a 3D scene. `m_zOrder` remains an explicit override tier
(compared first); creation `m_seq` breaks remaining ties.

## Mirroring an XR monitor onto a physical one — pure recipe, zero new code

Requirement: see an XR monitor on a physical display. This already works with the
existing mirror machinery:

```ini
# hyprland.conf — mirror XR-1 onto DP-1
monitor = DP-1, preferred, auto, 1, mirror, XR-1
```

(or at runtime: `hyprctl keyword monitor "DP-1, preferred, auto, 1, mirror, XR-1"`.)

Why it works for headless sources — the mechanism, verified in source:

- The rule's `mirror` arg lands in `CMonitorRule::m_mirrorOf`; `applyMonitorRule`
  calls `setMirror("XR-1")` at its end (`src/output/Monitor.cpp:710`, also
  `onConnect` at :341-342 for boot-time rules).
- `CMonitor::setMirror` (`Monitor.cpp:1330-1396`) resolves the source by name,
  refuses mirror-of-mirror and self-mirror, moves DP-1's workspaces to a backup
  monitor, sets `DP-1.m_mirrorOf = XR-1` and appends DP-1 to `XR-1.m_mirrors`,
  software-locks the cursor on the source, and emits
  `Event::bus()->m_events.monitor.layoutChanged` (which drops DP-1 from the
  workspace-bearing monitor list — `src/state/MonitorState.cpp:32-46`).
- On the **source** side: `XR-1.m_mirrors` being non-empty makes
  `CMonitor::needsACopyFB()` return true (`Monitor.cpp:2705-2707`), so the renderer
  keeps a **mirror FB** alive and saves each composited frame of XR-1 into it (the
  `needsACopyFB()` checks in `src/render/Renderer.cpp` ~1745/2189 — the same
  copy-FB path screenshare uses via `Screenshare::mgr()->outputNeedsCopyFB`).
- On the **mirror** side: DP-1's render path sees `isMirror()` and calls
  `IHyprRenderer::renderMirrored()` (`Renderer.cpp:1950-1978`), which draws
  `XR-1->resources()->getMirrorTexture()` (`src/output/MonitorResources.hpp`)
  scaled/letterboxed/transform-corrected onto DP-1.
- None of this cares about the source's backend: the mirror FB is a GL texture copy
  made inside the compositor at composite time — a headless source is
  indistinguishable from a DRM one. It composes cleanly with the XR quad blit,
  which independently consumes the *presented buffer* (`presented` signal); the two
  consumers never touch the same object.
- Pacing: XR-1 renders when the frame thread paces it (session visible) or when
  damaged; each new XR-1 frame damages its mirrors, so DP-1 updates at
  min(XR cadence, DP-1 refresh). If the session is idle (no pacing), the mirror
  simply shows the last rendered frame — correct and cheap.

Implementation note: there is deliberately **no code** in `src/openxr/` for
mirroring. Doc 05's consumer recipes reference this section; hyprtester covers it
with a `mirror` integration test (doc 06).

## Interaction with the rest of the surface (pointers into sibling docs)

- `xrmonitor` dispatcher verbs (`create/destroy/select/anchor/move/rotate/scale/
  distance/center`) and `hyprctl openxr` subcommands call `createXRMonitor` /
  `destroyXRMonitor` / quad-param setters on the main thread — tables in doc 05.
- Anchor solving and the meaning of `SXRAnchorState` — doc 03. From this doc's
  perspective an anchor is opaque: main thread swaps/updates `m_anchor` under
  `m_layersMu`; the frame thread calls `solve()` on its snapshot.
- Ray input targets layers by intersecting quads and routes `motionAbsolute` bound
  to the hit monitor's output — doc 04.
- Events emitted here (`xrmonitoradded`, `xrmonitorremoved`, `xrmonitorquad`) and
  `hyprctl openxr status` fields — doc 05.

## Context files to read before implementing

- `/home/ajg/code/Hyprland/docs/openxr/00-overview.md` — thread model + handoff table (normative for every field above)
- `/home/ajg/code/Hyprland/docs/openxr/01-session-graphics.md` — frame loop, swapchain creation rules, EGL context invariant, teardown ordering
- `git show openxr:src/openxr/COpenXRManager.cpp` — WIP: `createVirtualMonitor`, `setupMonitor`, `onMonitorPresented`, frame-loop buffer grab + pacing
- `/home/ajg/code/Hyprland/src/debug/HyprCtl.cpp` (lines 1743–1797) — `dispatchOutput`: the existing create/destroy path incl. name checks and `m_createdByUser` guard
- `/home/ajg/code/Hyprland/src/state/MonitorState.cpp` — `add`/`remove` flow, `Event::bus()` monitor events
- `/home/ajg/code/Hyprland/src/output/Monitor.cpp` — ctor listeners (destroy ~201, state/`m_forceSize` ~214), `onConnect` ~106, `m_createdByUser` ~258, `applyMonitorRule` ~715, `setMirror` ~1330, `needsACopyFB` ~2705
- `/home/ajg/code/Hyprland/src/output/Monitor.hpp` — `CMonitor` fields + `m_events` signals
- `/home/ajg/code/Hyprland/src/output/MonitorResources.hpp` — `getMirrorTexture` / `markMirrorFBStale` (mirroring mechanism)
- `/home/ajg/code/Hyprland/src/render/Renderer.cpp` — `renderMirrored` (~1950), `needsACopyFB` call sites (~1745, ~2189)
- `/home/ajg/code/Hyprland/src/render/Renderer.hpp` — `beginRender`/`beginRenderToBuffer`/`endRender` signatures (context for how render-to-buffer consumers coexist)
- `/home/ajg/code/Hyprland/src/managers/screenshare/ScreenshareFrame.cpp` — `copyDmabuf()`: the reference `beginRender(..., RENDER_MODE_TO_BUFFER, buffer, ...)` recipe if a compositor-side copy path is ever needed instead of the presented-buffer handoff
- `/home/ajg/code/Hyprland/src/event/EventBus.hpp` — monitor added/removed/layoutChanged signals
- `/home/ajg/code/Hyprland/src/config/shared/monitor/MonitorRuleManager.hpp` — rule matching used in create step 5
