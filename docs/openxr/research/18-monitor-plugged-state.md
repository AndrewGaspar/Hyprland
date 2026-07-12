# 18 — XR monitor plugged-state tracking: don't instantiate xrmonitors until a session is live

Status: **research / design only. Nothing here is implemented.** No live runs, no code changes to
product source, no session start/stop. `hyprctl` was read-only. Author: research pass 2026-07-11.
Base commit: `452d6b02`.

The ask (user's words): *"I don't really want the xrmonitors to be instantiated if an OpenXR session
is not actually active. Go learn how to automatically cycle the monitors' 'plugged' state off when
the session is not connected. I assume this is something Hyprland must already handle with
hotplugging external monitors."*

Fishfood context: the user runs HypXRland as their **daily desktop** session (`~/code/hypxrland`).
At login there is usually **no headset connected**, yet every `xrmonitor=` config line materializes a
headless output at startup regardless of XR session state. The result is *phantom monitors*:
workspaces land on them, windows open on invisible displays, and focus/cursor can wander onto a
display that does not physically exist. Desired: an `xrmonitor` behaves like an **unplugged external
monitor** until an OpenXR session is actually running, and "unplugs" again when the session drops —
with all the graceful behavior real hotplug gets (workspace evacuation to remaining monitors on
unplug, workspace/window return on replug, sane focus handling).

Cross-refs (source of truth):
- Session/runtime lifecycle itself is **owned by research report 17** (sibling agent). This report
  *consumes* whatever session start/stop / connect/disconnect edges report 17 defines and does **not**
  design the session state machine. Where I say "session becomes active / inactive," the concrete
  edge is report 17's.
- XR monitor machinery: `src/openxr/OpenXRManager.cpp` (`createXRMonitor`, `destroyXRMonitor`,
  `finalizeLayerRemoval`, `destroyOutputDeferred`, `reconcileDeclaredMonitors`, `bindExistingLayers`,
  `teardownLayers`, `start`/`stop`, `setState`), `src/openxr/XRMonitorLayer.{hpp,cpp}` (the persistent
  declaration record).
- Hyprland hotplug: `src/output/Monitor.cpp` (`onConnect`/`onDisconnect`), `src/state/MonitorState.cpp`,
  `src/state/WorkspacePlacementController.cpp`, `src/config/shared/monitor/MonitorRuleManager.cpp`.
- The aquamarine headless-output framecb UAF and its `destroyOutputDeferred` workaround
  (`OpenXRManager.cpp:1418-1450`) — the single biggest reason to *avoid* output destroy on the hot path.

---

## TL;DR

1. **Recommendation: option (b) — create-but-disabled, driven by the session start/stop edge.** Keep
   the headless `CMonitor` output object alive for the whole HypXRland process, but hold it
   **disabled** (`m_enabled == false`) whenever no session is active, and **enable** it when a session
   comes up. This is *exactly* the mechanism Hyprland already uses for `monitor=…,disable`, and — the
   user's own hunch — it is the code path Hyprland uses to hotplug: a runtime disable/enable routes
   through `CMonitor::onDisconnect()` / `onConnect(true)` (`MonitorRuleManager.cpp:171-172`), which run
   the **identical workspace evacuation and return** logic as a physical unplug/replug, minus the
   object teardown (§1, §4).

2. **Why not full destroy (option a).** Destroying the headless output on every session drop re-enters
   the aquamarine framecb UAF path (`destroyOutputDeferred`, `OpenXRManager.cpp:1418`) on the *hottest*
   churn, loses the persistent `CXRMonitorLayer` anchor/adaptive state unless carefully retained, and
   erases the `CMonitor` id so per-name workspace-return maps are the only survivor. It buys a
   marginally cleaner `hyprctl monitors all` and costs reliability. Keep it available behind a flag,
   not the default.

3. **Why not DPMS-off (option c) — reject.** DPMS (`m_dpmsStatus`) powers the scanout off but leaves
   the monitor **enabled and in the placement set**: workspaces are *not* evacuated, windows still open
   on it, and it still advertises to screencast/portal. It fails the core requirement (§5). Reject.

4. **The disable path already gives full hotplug semantics *both directions* for free.** `onDisconnect`
   remembers the active workspace (`rememberWorkspaceForMonitor`, `Monitor.cpp:415-418`), stamps each
   evacuated workspace with `m_lastMonitor = name` (`:472-475`), and moves them to a `BACKUPMON`
   (`:477-484`). `onConnect` pulls them back by name via the `RETURNING` check
   (`ws->m_lastMonitor == m_name`, `:328-337`) *and* the remembered-workspace map (`:366-373`) *and*
   config workspace-binding rules (`:114-119`). All three survive because option (b) keeps the
   `CMonitor` and its **stable name** across the disable cycle (§4).

5. **State survives trivially.** Under (b) both the `CMonitor` output *and* the `CXRMonitorLayer`
   declaration record persist across the inactive→active cycle, so `m_declaredAnchor`, the live
   `m_anchor` leash spring, adaptive-anchoring phase, chrome state, and the monitor id are all intact —
   no re-seeding, no re-bind, no swapchain churn beyond the one already forced at session start (§6).

6. **Gate on session *existence* (RUNNING\_\*), not VISIBLE/FOCUSED.** Enabling on session-begin and
   disabling on session-end (the `start()`/`stop()` boundary) avoids flapping on the rapid
   IDLE↔VISIBLE churn a proximity sensor produces mid-session. A VISIBLE/FOCUSED gate would
   evacuate-and-return your whole desktop every time you briefly lift the headset (§6.2). This aligns
   with report 17's session edges.

7. **Five work packages** (§8): WP-M1 declaration-vs-enabled split + `monitors_follow_session` config
   (M, headless); WP-M2 enable/disable on the session edge, reusing `start()`/`stop()` (S); WP-M3
   reconcile-while-inactive (declared-but-no-session → created disabled) (S); WP-M4 hyprtester
   evacuation/return round-trip via harness session start/stop (M, headless); WP-M5 live-headset
   WiVRn connect/disconnect + focus + adaptive-survival validation (S, needs Quest).

---

## 1. How Hyprland hotplugs a physical monitor (verified in code)

The tree is refactored: the classic `Compositor.cpp`/`helpers/Monitor.cpp` logic now lives in
`src/output/Monitor.cpp` + `src/state/` controllers.

**Plug in (newOutput → CMonitor):**
- aquamarine `newOutput` listener: `src/Compositor.cpp:458-464` → `State::monitorState()->add(output)`.
- `CMonitorStateTracker::add` (`src/state/MonitorState.cpp:109-145`) assigns/reuses an id by name
  (`getNextAvailableMonitorID`, `:57-74`), pushes into `m_realMonitors`, then calls
  `mon->onConnect(false)` (`:120`).
- `CMonitor::onConnect` (`src/output/Monitor.cpp:107-389`) registers the output `destroy` and `state`
  listeners (`:202-243`), applies the matching monitor rule (`:262-311`), runs `setupDefaultWS`
  (`:320`) and the three workspace-return mechanisms (§4), and emits `monitoradded` (`:386-388`).

**Unplug (output destroy → CMonitor removal):** the `destroy` listener is armed inside `onConnect`:

```cpp
// src/output/Monitor.cpp:202-213
m_listeners.destroy = m_output->events.destroy.listen([this] {
    ...
    onDisconnect(true);                     // :207  (destroy = true — full teardown)
    m_output              = nullptr;        // :209
    State::monitorState()->remove(m_self.lock());  // :212
});
```

**`CMonitor::onDisconnect(bool destroy)`** (`src/output/Monitor.cpp:391-520`) is the evacuation
workhorse — and, importantly, it is **the same function** for both a physical unplug (`destroy=true`)
and a runtime disable (`destroy=false`, §4). It:
- early-outs if `!m_enabled` (`:405`) — a monitor already disabled does nothing;
- remembers the active workspace by monitor name: `rememberWorkspaceForMonitor(m_name, …)` (`:415-418`);
- picks `BACKUPMON` = first *other* monitor in `monitors()` (`:421-427`);
- collects workspaces on this monitor (or orphaned) into `wspToMove` (`:464-468`);
- stamps ownership for return: `if (w->m_lastMonitor.empty()) w->m_lastMonitor = m_name;` (`:472-475`);
- **evacuates**: `moveWorkspaceToMonitor(w, BACKUPMON)` per workspace (`:477-484`); if no backup exists,
  resets focus/surface/window (`:486-488`);
- clears `m_activeWorkspace`, sets `m_enabled = false` (`:461`), commits the output disabled (`:495-502`),
  and moves focus to `BACKUPMON` (`:504-505`).

`moveWorkspaceToMonitor` (`src/state/WorkspacePlacementController.cpp:243-385`) re-parents the
workspace and every window's `m_monitor`, translates floating/fullscreen geometry between origins,
and preserves active/focus if it was active.

**Last-monitor-gone safety net:** `src/state/FallbackState.cpp:27-40` spins up a headless `FALLBACK`
output (`m_isUnsafeFallback`, `:52`) when the last real monitor disappears — relevant only to a
pure-XR/headless session with no physical displays (§7.4).

---

## 2. How our XR monitors are created today

Single funnel: `COpenXRManager::createXRMonitor` (main thread), documented to work in **every** manager
state including `XR_STATE_DISABLED` (`OpenXRManager.hpp:82-85`). It:

1. builds the persistent `CXRMonitorLayer` (the *declaration* — holds name, anchor, id, config flags)
   and pushes it into `m_layers` (`OpenXRManager.cpp:1241-1267`);
2. creates the headless output on the aquamarine headless backend —
   `impl->createOutput(params.m_name)` (`:1269-1277`) — which synchronously runs
   `MonitorState::add → CMonitor ctor → onConnect`, so the monitor is queryable immediately (`:1284-1291`);
3. `layer->bindToMonitor(mon, onGone)` caches the monitor + wires listeners (`:1295-1313`).

**Startup is unconditional.** `COpenXRManager::init()` calls
`reconcileDeclaredMonitors()` **before** `start()` and regardless of `openxr:enabled`:

```cpp
// src/openxr/OpenXRManager.cpp:75-83
// Materialize any monitors declared in the config as plain headless outputs (doc 02 lazy
// binding). Their quads bind when a session later starts. ...
reconcileDeclaredMonitors();
static auto PENABLED = CConfigValue<Hyprlang::INT>("openxr:enabled");
if (*PENABLED) start();
```

`reconcileDeclaredMonitors` (`:1711-1784`) diffs the declared set D (from `xrmonitor=` lines) against
live config-created layers L (`m_declaredByConfig`): D\L → `createXRMonitor`, D∩L → in-place mode/anchor
diff, L\D → `destroyXRMonitor`. **This is the WP4 reconcile.** It runs at `init()` (`:78`), at the tail
of `start()` (`:293`), and on `onConfigReload()` when there is no enable/disable edge (`:1571`).

**Confirmed: monitor existence is NOT gated on session state today.** The headless `CMonitor` for every
`xrmonitor=` line exists from `init()` onward whether or not a session ever starts. Only the XR *quad*
(swapchain bind + compositing + pacing) is session-gated: `createXRMonitor` skips `m_swapchainDirty`
unless `m_running` (`:1336-1337`), and pacing (`SCHEDULE_FRAMES`, `:685-693`) only fires while VISIBLE.
The underlying output is never hidden or DPMS'd. **This is the phantom-monitor bug.**

### 2.1 The declaration/output split already exists in embryo

`CXRMonitorLayer` is the **durable declaration**, deliberately separable from the ephemeral `CMonitor`:
- `std::string m_monitorName` — "the durable key; survives monitor teardown" (`XRMonitorLayer.hpp:76`);
- `PHLMONITORREF m_monitor` — *weak* ref to the headless output's `CMonitor` (`:77`);
- `std::atomic<int64_t> m_monitorId{-1}` — cached at bind (`:102`);
- `bool m_declaredByConfig` — true iff from an `xrmonitor` keyword (`:86`), the reconcile key;
- `bool m_createdByXR = true` — false for adopted pre-existing outputs (`:81`), the stop-disposition key;
- `OpenXR::SXRAnchorState m_declaredAnchor` + live `OpenXR::CXRAnchor m_anchor` (`:90-91`) — the
  anchoring engine state that must survive an unplug/replug.

The manager **already** knows how to run with a layer whose monitor was externally destroyed:
`bindExistingLayers()` (`OpenXRManager.cpp:1452-1489`, run in `start()`) re-binds any layer whose named
monitor still exists and drops the rest; `teardownLayers()` (`:1491-1526`, run in `stop()`) either
destroys the output (if `openxr:destroy_monitors_on_stop` **and** `m_createdByXR`) or **keeps it as a
plain headless monitor and unbinds the swapchain for lazy re-bind** (`:1511-1519`). That "keep" branch
is the seed of option (b) — it just needs to *disable* the kept output instead of leaving it live.

---

## 3. What "session active" means, and where the hooks go

Two state layers (from the XR-side survey):
- **Manager state** `eXRManagerState` (`OpenXRManager.hpp:28-37`): DISABLED / UNAVAILABLE / STARTING /
  RUNNING\_IDLE / RUNNING\_VISIBLE / RUNNING\_FOCUSED / STOPPING, stored in `m_state` (`:250`), with
  derived `m_active` (VISIBLE‖FOCUSED) and `std::atomic<bool> m_running` (frame thread alive, `:267`).
- **Raw XR state** in `CXRSession` (frame thread): `m_xrState`, transitions handled in
  `pollEvents()` (`XRSession.cpp:279-367`) — READY→`xrBeginSession`, STOPPING→`xrEndSession`,
  EXITING/LOSS\_PENDING→teardown. The frame thread maps XR→manager state (`mapSessionState`, `:379-386`)
  and reports it over the queue; the main thread lands it in `setState()` (`:146-166`), which posts
  `openxrsessionstate` and derived `openxractive` events.

**Natural, main-thread hook points (already present):**
- **Session begin:** `COpenXRManager::start()` — right after `setState(XR_STATE_RUNNING_IDLE)` it already
  calls `bindExistingLayers()` (`:272`), then `reconcileDeclaredMonitors(); recomputeQuadActive();`
  (`:290-294`). *Enable* the declared outputs here.
- **Session end:** `COpenXRManager::stop()` → `teardownLayers()` (`:368`, `:1491`). *Disable* (rather than
  destroy) the declared outputs here.
- The precise begin/end edge — and what counts as a WiVRn "disconnect" — is **report 17's** call. Option
  (b) attaches to whatever main-thread edge 17 exposes (today: `start()`/`stop()`).

---

## 4. The disable path *is* Hyprland's software hotplug

The user's intuition is exactly right. A runtime enable↔disable transition is dispatched by the rule
manager straight into the hotplug functions:

```cpp
// src/config/shared/monitor/MonitorRuleManager.cpp:167-173
for (const auto& m : monsForRefresh) {
    if (!m->m_output) continue;
    if (m->m_enabled == m->m_activeMonitorRule.m_disabled)
        m->m_activeMonitorRule.m_disabled ? m->onDisconnect() : m->onConnect(true);
}
```

- **Disable (enabled→disabled):** `onDisconnect()` with the default `destroy=false`
  (`Monitor.hpp:261`) — the **full evacuation** of §1 runs, but `State::monitorState()->remove()` is
  *not* called and `m_output` stays alive, so the `CMonitor` object (and its stable id/name) persists.
- **Enable (disabled→enabled):** `onConnect(true)` — the **full return** of §4.1 runs.

### 4.1 Return-on-replug, three redundant mechanisms (all keyed on the stable name)

`onConnect` restores workspaces via, in order:
1. **`m_lastMonitor` ownership tag** — `RETURNING = (ws->m_lastMonitor == m_name)` (`Monitor.cpp:328`),
   moves the workspace back and clears the tag (`:332-337`). Set during disconnect (`:472-475`); survives
   cascaded disconnects.
2. **Remembered active workspace** — `rememberedWorkspaceForMonitor(m_name)` (`:366-373`,
   `WorkspaceState.cpp:110-114`), a name→id map (`m_seenMonitorWorkspaceMap`) restoring which workspace
   was active on this monitor and switching to it.
3. **Config workspace binding** — `ensurePersistentWorkspacesPresent` / `ensureWorkspacesOnAssignedMonitors`
   deferred at `:114-119`; `getBoundMonitorForWS` matches by name *or* `desc:`
   (`WorkspaceRuleManager.cpp:69-75`). This is `workspace = N, monitor:XR-1` / `wsbind`.

**All three key on the monitor name.** Option (b) keeps the name (and the whole `CMonitor`) constant
across the cycle, so all three work with zero new plumbing. Option (a) preserves only #2 and #3 (both
name-keyed and CMonitor-independent); #1 (`m_lastMonitor`) is set on the workspace and also survives, but
the destroy/recreate churn is where the aquamarine UAF lives (§2, §5).

---

## 5. The three "off" states, compared

| state | set where | workspaces evacuated? | in `monitors()` placement set? | in `hyprctl monitors` / `…all` | screencast/portal advertises? | on replug/return |
|---|---|---|---|---|---|---|
| **destroyed output** | `m_output=nullptr` + `remove()` (`Monitor.cpp:209,212`) | yes (`onDisconnect(true)`) | no (gone) | absent / absent | no | recreate → `onConnect` restores by name |
| **`m_enabled=false` (disable)** | `onDisconnect()` / disabled rule (`:461`, `729`) | **yes** (`onDisconnect`) | **no** — excluded at `MonitorState.cpp:36` | absent(default) / present(`all`) | no (`m_enabled` gated) | `onConnect(true)` restores by name |
| **`m_dpmsStatus=false` (DPMS off)** | `setDPMS` (`:2297-2361`) | **no** | **yes** | present / present | **yes** (still enumerated) | n/a (never evacuated) |

`monitors()` (`MonitorState.cpp:53`) is the placement-relevant list, rebuilt to exclude `!m_enabled`
(`:36`); `allMonitors()` (`:49`) is the raw list behind `hyprctl monitors all`. Consumers that gate on
`m_enabled`: the placement list (`:36`), `scheduleFrame` (`Monitor.cpp:1127`), the `add()` post-processing
(`MonitorState.cpp:122,127`), pointer render (`PointerManager.cpp:273,292`), `mostHzMonitor` selection.

**Takeaways:** (a) and (b) both remove the monitor from placement and from default `hyprctl monitors`;
(c) does neither. Only (a) also removes it from `hyprctl monitors all`. (c) additionally keeps
advertising to screencast/xdg-desktop-portal — actively confusing — which alone disqualifies it.

---

## 6. Design options, evaluated

### 6.1 Option (a) — defer creation / destroy on session end

Create the declared headless outputs only when a session becomes active; destroy them on session end
(keep the `CXRMonitorLayer` declaration so the anchor state and reconcile survive).

- **Migration both ways:** full physical-hotplug semantics via `onDisconnect(true)` + `remove()` on end
  and `createOutput → onConnect` on start. Return by remembered-workspace map + config bind (name-keyed)
  works; `m_lastMonitor` on the workspace also survives.
- **`hyprctl monitors` cleanliness:** *best* — the monitor is genuinely absent from `monitors` **and**
  `monitors all` when inactive.
- **WP4 reconcile:** invasive — `reconcileDeclaredMonitors`/`init()` must stop creating outputs when no
  session (create the layer record but defer `createOutput`); `bindExistingLayers` already handles the
  "layer exists, output must be (re)made" side.
- **Adaptive/anchor survival:** the live `CXRAnchor` leash spring + adaptive phase live on the
  `CXRMonitorLayer`; safe **iff** the layer is deliberately retained across the destroy (today
  `destroy_monitors_on_stop` erases the layer at `:1524` — must not, for declared layers).
- **Portal/screenshare:** clean (output absent).
- **aquamarine framecb UAF:** **re-entered on every session end** via `destroyOutputDeferred`
  (`:1418-1450`) — the workaround exists and is battle-tested, but this is the single most fragile path in
  the subsystem and option (a) exercises it on the *most frequent* transition.

### 6.2 Option (b) — create-but-disabled *(recommended)*

Keep the headless output for process lifetime; hold it disabled when no session; enable on session start,
disable on session end — mirroring `MonitorRuleManager.cpp:171-172`.

- **Migration both ways:** identical evacuation/return to (a) because it uses the *same*
  `onDisconnect`/`onConnect` functions — and *all three* return mechanisms (§4.1) work because the
  `CMonitor` name/id are constant.
- **`hyprctl monitors` cleanliness:** absent from default `monitors`; lingers in `monitors all`
  (acceptable — a disabled monitor, exactly like `monitor=…,disable`).
- **WP4 reconcile:** minimal change — `init()` still creates the outputs (stable ids), just disabled; new
  `xrmonitor=` declared with no session → created disabled; layer + monitor both persist so reconcile
  diffs are unchanged.
- **Adaptive/anchor survival:** *best* — nothing is torn down; `m_anchor` spring, adaptive phase, chrome
  atomics, swapchain-dirty flag all persist. Session start forces one swapchain rebuild anyway.
- **Portal/screenshare:** clean (`m_enabled=false` → not advertised).
- **aquamarine framecb UAF:** **never triggered** — no output is destroyed on the session edge, so
  `destroyOutputDeferred` is out of the hot path entirely. Biggest reliability win.
- **Flapping guard:** enable/disable on the `start()`/`stop()` boundary (session existence), *not* on
  VISIBLE/FOCUSED. Mid-session IDLE↔VISIBLE churn (proximity sensor, dashboard-in-front) keeps the
  monitors enabled; only a genuine session end unplugs them. A VISIBLE gate would evacuate-and-return the
  whole desktop every time the headset is briefly lifted — do not do that.

**Enable/disable primitive.** Do **not** rely on `applyMonitorRule` with a disabled rule alone — that
branch (`Monitor.cpp:729-742`) only flips the output enabled bit and does **not** evacuate; the evacuation
lives in `onDisconnect`. Mirror the rule manager: drive the transition through
`onDisconnect()` / `onConnect(true)` (keeping `m_activeMonitorRule.m_disabled` consistent so a later real
reload agrees). This is a small, well-scoped primitive on the XR side that calls the same two functions
the rule manager already calls.

### 6.3 Option (c) — DPMS-off *(reject)*

`setDPMS(false)` powers scanout off but leaves the monitor enabled and in the placement set: **no
evacuation, windows still open on it, still advertised to screencast/portal** (§5). Fails the ask.

### 6.4 Decision

| criterion | (a) destroy | (b) disabled | (c) dpms |
|---|---|---|---|
| evacuate on inactive | ✅ | ✅ | ❌ |
| return on active | ✅ (name-keyed) | ✅ (name + id) | n/a |
| `hyprctl monitors` (default) clean | ✅ | ✅ | ❌ |
| `hyprctl monitors all` clean | ✅ | ⚠️ lingers | ❌ |
| anchor/adaptive state survives | ⚠️ if layer retained | ✅ trivially | ✅ |
| WP4 reconcile churn | ⚠️ invasive | ✅ minimal | ✅ |
| portal/screencast clean | ✅ | ✅ | ❌ |
| avoids aquamarine framecb UAF | ❌ (hot path) | ✅ | ✅ |

**Recommend (b).** Ship it as the default behind `openxr:monitors_follow_session` (default `true`);
keep the existing `openxr:destroy_monitors_on_stop` for users who explicitly want option-(a) full
removal (its meaning becomes "on session stop, destroy rather than disable"). Users who want today's
always-present behavior set `monitors_follow_session = false`.

---

## 7. Details, edges, and interactions

**7.1 `openxr:destroy_monitors_on_stop` today (`ConfigValues.cpp:814-815`, default `true`).** It only
acts at `stop()` and only on `m_createdByXR` layers, and it also *erases the layer*
(`OpenXRManager.cpp:1524`). Under (b) the default follow-session path must **not** run that erase for
declared monitors; the two options coexist as: `monitors_follow_session` (disable-on-inactive, keep) vs
`destroy_monitors_on_stop` (destroy-on-stop, option-a semantics). Precedence and interaction need a short
config note (§8 WP-M1).

**7.2 The `init()` ordering already favors (b).** `init()` creates outputs before `start()` (`:75-83`);
(b) just marks freshly-created declared outputs disabled unless `m_running`, then enables them in the
existing `start()` tail (`:290-294`) and disables them in the existing `stop()`/`teardownLayers()`
(`:368`).

**7.3 Reconcile while inactive.** A `hyprctl keyword xrmonitor …` or reload that adds a declaration with
no session must create the output **disabled** (via `createXRMonitor` + immediate disable), so it also
does not phantom. `reconcileDeclaredMonitors` already handles create/diff/destroy; the only addition is
"created output starts disabled when `!m_running`."

**7.4 Pure-XR / headless-only sessions.** If XR monitors are the *only* monitors (hermetic container
tests, or a headset-only boot), disabling them all with no physical `BACKUPMON` triggers the
`FALLBACK` output (`FallbackState.cpp:27-40`). That is the existing, correct behavior for "all monitors
gone"; the fishfood desktop always has physical monitors so `BACKUPMON` exists. Note this in the test
harness (a hermetic test that disables the sole XR monitor should expect the fallback, not a crash).

**7.5 Cursor/focus.** `onDisconnect` warps the cursor to `BACKUPMON` center (`Monitor.cpp:479`) and moves
focus to it (`:504-505`); `onConnect` re-grabs focus if focus is empty/stale (`:346-347,356-378`). Cursor
refocus then follows the pointer via `InputManager.cpp:272,917-918`. All of this comes for free under (b).

**7.6 XR quad binding.** On enable at session start, the layer is already bound (option b never unbinds),
so the frame thread just needs `m_swapchainDirty` set — the same one-line nudge `createXRMonitor` does at
`:1336-1337`. No `bindExistingLayers` re-bind is needed for kept-enabled layers, though calling it is
harmless (it skips already-bound layers, `:1459-1460`).

---

## 8. Work packages (one-subagent-sized)

- **WP-M1 — declaration-vs-enabled split + config · M · no deps · headless.** Add
  `openxr:monitors_follow_session` (Bool, default `true`) to `ConfigValues.cpp`. Introduce a small XR-side
  primitive `setDeclaredMonitorsEnabled(bool)` that, per declared layer with a live `m_monitor`, drives
  `onConnect(true)` / `onDisconnect()` exactly as `MonitorRuleManager.cpp:171-172` does (keeping
  `m_activeMonitorRule.m_disabled` consistent). Make `createXRMonitor`/`reconcileDeclaredMonitors` create
  declared outputs **disabled when `!m_running`** and `monitors_follow_session` is on. Document the
  interaction with `destroy_monitors_on_stop` (WP-M1 note). Acceptance: with `enabled=0`, a declared
  `xrmonitor` is absent from `hyprctl monitors` (present in `monitors all`), and no workspace is placed on
  it.

- **WP-M2 — enable/disable on the session edge · S · dep M1, report 17.** Call
  `setDeclaredMonitorsEnabled(true)` at the session-begin edge (in `start()`, after
  `setState(RUNNING_IDLE)` / the existing `reconcileDeclaredMonitors()` tail, `:290-294`) and
  `setDeclaredMonitorsEnabled(false)` at the session-end edge (in `stop()` / `teardownLayers()`, `:368`),
  gating on report 17's actual begin/end edge. Gate on session *existence*, not VISIBLE/FOCUSED (§6.2).
  Acceptance: start a session → declared monitors appear enabled; stop → they disable; no evacuation
  flap across mid-session IDLE↔VISIBLE transitions.

- **WP-M3 — reconcile-while-inactive · S · dep M1.** Ensure a `hyprctl keyword xrmonitor …` /
  config-reload that adds a declaration with no session creates the output **disabled**, and that
  removing one while inactive tears down cleanly. Acceptance: add/remove xrmonitor lines with `enabled=0`
  → no phantom, no leak, correct `hyprctl monitors all` set.

- **WP-M4 — hyprtester evacuation/return round-trip · M · dep M2 · headless.** Using the harness's
  ability to start/stop a Monado session (`hyprtester/src/tests/xr/session.cpp`, `monitors.cpp`): assert
  (1) declared `xrmonitor` absent from `j/monitors` with no session, present after start; (2) open a
  window / switch a workspace onto the XR monitor while active, stop the session, assert the workspace +
  window migrated to a physical monitor (evacuation), restart, assert they returned to the XR monitor by
  name (`m_lastMonitor` + remembered-workspace). This is the payoff of reusing the native disable path —
  it is fully scriptable headless. Acceptance: the round-trip passes in the hermetic container suite.

- **WP-M5 — live-headset WiVRn validation · S · dep M2 · needs Quest.** With `preview-xr.sh --wivrn`:
  connect the headset (monitors plug in, focus/cursor behave), disconnect (monitors unplug, desktop
  reflows onto physical displays, no window loss), reconnect (workspaces/windows return, adaptive-anchor
  pose survives). Confirm no aquamarine crash across repeated connect/disconnect cycles (the reliability
  claim of option b). Acceptance: 5+ connect/disconnect cycles with intact return and no crash.

**Headless vs live.** M1/M3/M4 are fully headless (config + harness session start/stop). M2 is headless
to wire and test but its *edge selection* depends on report 17. M5 needs the Quest to exercise real WiVRn
connect/disconnect (the harness starts/stops sessions but does not model a mid-run headset yank the way a
physical disconnect does).

---

## 9. Open questions for the user

1. **Default gate = session existence (start↔stop), not VISIBLE/FOCUSED — confirm.** This keeps XR
   monitors present while you briefly lift the headset mid-session and only unplugs them when the session
   truly ends. A VISIBLE gate would be twitchy. Agree with existence as the boundary? (Ties to report 17's
   definition of "session end / WiVRn disconnect.")
2. **`monitors_follow_session` default `true` — confirm.** Opting out (`false`) restores today's
   always-present behavior for anyone who wants the outputs up before the headset.
3. **Keep `destroy_monitors_on_stop` as the option-(a) full-removal escape hatch, or retire it?** Under
   (b) it becomes "destroy rather than disable on stop." Do you want the truly-absent-from-`monitors all`
   behavior available, or is disable-only enough?
4. **`hyprctl monitors all` lingering entry — acceptable?** Under (b) a disabled XR monitor still shows in
   `monitors all` (exactly like `monitor=…,disable`). Option (a) removes it there too, at the cost of the
   aquamarine-UAF hot path. Is the lingering `all` entry fine?
5. **Persist workspace assignments across sessions?** Return-on-replug uses in-memory tags
   (`m_lastMonitor`, `m_seenMonitorWorkspaceMap`) that reset on a HypXRland restart. If you want a
   window's XR-monitor home to survive a full logout, that is `workspace = N, monitor:XR-1` / `wsbind`
   config (name-keyed, already works) — want the report to spec generating those, or leave layout to
   runtime memory?
