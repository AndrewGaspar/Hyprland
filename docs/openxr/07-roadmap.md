# 07 — Roadmap: Work Packages, Dependencies, Acceptance Criteria

This document turns the design set (`docs/openxr/00-overview.md` … `06-testing.md`) into an
ordered sequence of work packages (WP1–WP13). Each WP is sized for one implementation
subagent producing one reviewable commit/PR, and lists exactly the context it needs — an
implementer should read *only* its "Context to load" list plus this entry, and have enough
to work cold.

The doc-authoring WP0 (this design set) is complete by the time you read this.

---

## Dependency graph

```
            ┌──────────────────────────────────────────────────────────┐
            │                                                          │
 WP1 ──► WP2 ──► WP3 ──► WP4 ──► WP5 ──────────────► WP8 ──────┐       │
            │              │  └────────► WP7 ◄────┐    │       │       │
            ├─► WP6 ───────┼─────────────┘        │    │       ▼       │
            ├─► WP9 ───────┼──────────────────────┼────┼──► WP12       │
            └─► WP10 ──────┴──► WP11 ◄── (WP4,5)  │    │       │       │
                  │                       │       │    │       │       │
                  └───────────────────────┴───────┴────┴───────┴──► WP13
```

- **Critical path: WP1 → WP2 → WP3 → WP4 → WP5 → WP8** (the interactive-XR feature spine).
- **WP6, WP9, WP10 unblock immediately after WP2** and should run in parallel with WP3–5.
- **WP10 is deliberately early** (needs only WP2): it de-risks the project's #1 unknown —
  Monado null-compositor + EGL client interop (with lavapipe) — before the whole monitor/
  input stack is built on top. If WP10 finds the interop broken, the fallback is real-GPU
  local testing only, and that decision should be made early.

---

## Work packages

### WP1 — Build gating, manager skeleton, config surface, status IPC

**Goal.** The extension exists as a compilable, toggleable skeleton: `HAVE_OPENXR` build
gating, `COpenXRManager` with the lifecycle enum and DISABLED/UNAVAILABLE handling (no real
session yet), every `openxr:*` config var registered, and the status half of the IPC surface.

**Deliverables.**
- Created: `src/openxr/OpenXRManager.{hpp,cpp}`, `src/openxr/XRIpc.{hpp,cpp}`,
  `src/openxr/XRMath.hpp` (stub ok).
- Modified: `CMakeLists.txt` (pkg_check_modules `openxr` → `HAVE_OPENXR`, conditional
  sources), `src/config/values/ConfigValues.cpp` (the full `openxr:` block,
  `05-ipc-config.md` §1.2), manager init in the compositor bring-up (STAGE_LATE, after
  XWayland — `00-overview.md`), `flake.nix`/CI deps if needed.
- IPC: `hyprctl openxr status|enable|disable` (with `-j`), `openxrsessionstate` +
  `openxractive` events, config.reloaded/props_refreshed listeners driving `start()`/`stop()`.
  With no session code yet, `start()` goes DISABLED→UNAVAILABLE (probe: `XR_RUNTIME_JSON` /
  loader absent ⇒ unavailable; that's all of it for now).

**Depends on:** — (first implementation WP).

**Acceptance.**
- `cmake --build build-debug --target Hyprland` green **both** with the `openxr` package
  installed and with it absent (simulate: point `PKG_CONFIG_PATH` away).
- Without `HAVE_OPENXR`: `hyprctl openxr` returns hyprctl's unknown-request error; the
  `xrmonitor` dispatcher is absent or returns the built-without error; zero behavior change.
- With `HAVE_OPENXR` but `XR_RUNTIME_JSON` unset: `hyprctl -j openxr` returns
  `"state": "unavailable"` with empty `runtimeName`; `hyprctl openxr enable` returns a clean
  error string, no crash.
- `openxr:enabled=1` in config + reload toggles state DISABLED↔UNAVAILABLE and posts
  `openxrsessionstate` each way (verify with the socat one-liner, `05-ipc-config.md` §7.2).
- `hyprctl descriptions` lists all `openxr:*` vars with their descriptions.

**Context to load.** `00-overview.md`, `05-ipc-config.md` §1+§4+§5;
`src/config/values/ConfigValues.cpp`, `src/config/values/ConfigValues.hpp`,
`src/debug/HyprCtl.cpp` (registerCommand cluster ~1987, `dispatchOutput` 1743),
`src/managers/EventManager.hpp`, `CMakeLists.txt`, `src/Compositor.cpp` (init stages).

---

### WP2 — Session/graphics core (WIP port)

**Goal.** A real OpenXR session: instance/system/session creation over EGL/GBM
(`XR_MNDX_egl_enable` + `XR_KHR_opengl_es_enable`), reference spaces, the dedicated frame
thread, and — the part the WIP lacked — the *complete* session state machine
(STOPPING/EXITING/LOSS_PENDING) and clean teardown ordering. Port the good parts of
`git show openxr:src/openxr/COpenXRManager.cpp`; redesign per `01-session-graphics.md`.

**Deliverables.**
- Created: `src/openxr/XRSession.{hpp,cpp}`, `src/openxr/XRGraphics.{hpp,cpp}`.
- Modified: `OpenXRManager.{hpp,cpp}` (STARTING→RUNNING{idle/visible/focused}→STOPPING wired
  to the frame thread's event pump via the frame→main queue *stub* — full queue infra is
  WP6; a minimal eventfd handoff for session-state events is in scope here),
  `XRIpc` status now reports runtimeName/systemName.
- GPU selection: prefer Hyprland's primary GPU; `openxr:gpu` override; CPU-blit fallback
  retained (sized correctly, not the WIP's hard-coded 1920×1080 — actual sizing lands with
  WP3's blits).

**Depends on:** WP1.

**Acceptance.**
- Against a running Monado (real or null-compositor): `hyprctl openxr enable` reaches
  `idle`/`visible`/`focused` (as the runtime allows) and `hyprctl -j openxr` shows Monado's
  runtimeName; `disable` tears down with zero GL/XR validation errors and returns to
  `disabled`; enable→disable→enable ×5 in a loop leaks nothing obvious and never crashes.
- Killing `monado-service` mid-session (LOSS_PENDING/EXITING path) degrades to
  `unavailable` without crashing.
- `openxr:gpu = /dev/dri/renderDXXX` is honored (log line proves the node used).
- Builds green with and without the openxr package.

**Context to load.** `01-session-graphics.md`, `00-overview.md` (thread model);
`git show openxr:src/openxr/COpenXRManager.cpp` (the WIP to port); `src/openxr/OpenXRManager.cpp`
(WP1 skeleton), `src/render/OpenGL.hpp` (EGL context ownership), `src/helpers/Monitor` GPU
info as referenced by `01-session-graphics.md`.

---

### WP3 — One quad layer end-to-end

**Goal.** A single virtual monitor visible in the headset: headless output creation,
presented-buffer handoff, per-layer swapchain, dmabuf→EGLImage blit, `XrCompositionLayerQuad`
submission, and frame pacing (`scheduleFrame()` only while VISIBLE+).

**Deliverables.**
- Created: `src/openxr/XRMonitorLayer.{hpp,cpp}`, `src/openxr/XRMonitorConfig.{hpp,cpp}`
  (the `SXRMonitorParams` struct + parser — parser body may be minimal here; full grammar
  is WP4's), `COpenXRManager::createXRMonitor(SXRMonitorParams)` /
  `destroyXRMonitor(...)` funnel.
- Modified: `XRGraphics` (blit pipeline, samplerExternalOES; CPU fallback sized from the
  actual mode), `XRSession` (layer submission), a temporary debug path to create one monitor
  (e.g. hardcoded behind an env var, or wire the minimal `hyprctl openxr create` early).

**Depends on:** WP2.

**Acceptance.**
- With a session up, creating a monitor shows a live, updating quad in the headset (or is
  accepted by Monado's null compositor without error — verifiable in WP10's session_up).
- The backing output appears in `hyprctl -j monitors`, accepts a workspace, renders clients.
- No frame callbacks / `scheduleFrame()` churn while the session is idle (verify: CPU near
  0% with headset off).
- Destroying the monitor mid-presentation is clean (no use-after-free under ASan run).

**Context to load.** `02-virtual-monitors.md`, `01-session-graphics.md` (blit pipeline);
`src/debug/HyprCtl.cpp:1743-1797` (`output create` — the headless createOutput call),
`src/output/Monitor.cpp` (presented-buffer/state events per `02-virtual-monitors.md`),
`src/openxr/XRGraphics.cpp`, `src/openxr/XRSession.cpp` (WP2).

---

### WP4 — Monitor lifecycle: keyword, dispatcher, hyprctl, events, limits

**Goal.** The full monitor-management surface: `xrmonitor` config keyword with full grammar
+ reload reconciliation, `xrmonitor` dispatcher and `hyprctl openxr` create/destroy/select,
`xrmonitoradded`/`xrmonitorremoved` events, mode-change swapchain recreation, the 16-layer
limit behavior, and lazy quad binding for monitors created without a session.

**Deliverables.**
- Modified: `src/openxr/XRMonitorConfig.{hpp,cpp}` (full parser, `05-ipc-config.md` §2.2),
  `src/config/legacy/ConfigManager.cpp` (+ its header): `handleXRMonitor` trampoline +
  `registerHandler` + declared-list reset in `resetHLConfig()`;
  `src/config/legacy/DispatcherTranslator.cpp`: `m_dispMap["xrmonitor"]`;
  `src/openxr/XRIpc.cpp`: create/destroy/select subcommands + events;
  `src/openxr/OpenXRManager.cpp`: reconcile (`05-ipc-config.md` §2.5), selection state (§3.2),
  layer-count limit; `XRMonitorLayer` mode-change recreate protocol.
- Created: `tests/xr/parser.cpp` (can also land in WP5 with the other unit tests; landing it
  here is preferred since the parser is done here).

**Depends on:** WP3.

**Acceptance.**
- The four grammar examples (`05-ipc-config.md` §2.4) parse and materialize; parser unit
  tests pass in `hyprland_gtests`.
- Reload reconcile: adding/removing/altering `xrmonitor` lines creates/destroys/updates
  exactly the diff; runtime-created monitors survive reloads untouched; unchanged declared
  monitors keep their monitor ID (no flicker).
- `hyprctl dispatch xrmonitor create/destroy/select ...` and the `hyprctl openxr`
  equivalents behave identically (same funnel); events fire with the documented payloads.
- 17th monitor create fails with a clean error (spec-guaranteed 16 quad layers).
- Monitors created while `disabled`/`unavailable` exist as plain headless outputs and bind
  quads when a session later starts.

**Context to load.** `05-ipc-config.md` §2+§3+§4, `02-virtual-monitors.md`;
`src/config/legacy/ConfigManager.cpp` (~275 trampolines, ~606 registerHandler cluster,
`resetHLConfig`), `src/config/legacy/DispatcherTranslator.cpp` (~790),
`src/openxr/XRMonitorConfig.hpp`, `src/openxr/XRIpc.cpp`, `tests/config/MonitorParser.cpp`
(unit-test style reference).

---

### WP5 — Anchoring engine + verbs + unit tests

**Goal.** All four anchor modes (`local`, `head`, `body`, `device:left|right`) with the
deadzone + critically-damped-spring leash math, recenter handling
(`XrEventDataReferenceSpaceChangePending`), and the pose-mutation verbs
(`anchor/move/rotate/scale/distance/center`) on both dispatcher and hyprctl, plus the
`xrmonitoranchor` event and `hyprctl openxr layout`.

**Deliverables.**
- Created: `src/openxr/XRAnchor.{hpp,cpp}` (pure math, unconditional compile),
  `tests/xr/anchor_math.cpp`.
- Modified: `XRMonitorLayer` (per-frame anchor solve on the frame thread; device mode sets
  quad `space` to the grip action space), `XRIpc`/`DispatcherTranslator` (the verbs),
  `OpenXRManager` (verb funnel methods), `XRSession` (recenter event).

**Depends on:** WP4.

**Acceptance.**
- `tests/xr/anchor_math.cpp` green in CI (spring convergence/no-overshoot, yaw extraction,
  deadzones, dt-independence).
- Manual: `anchor active head` follows the view with the configured deadzone/response;
  `anchor active local` freezes it; `device:left` tracks the controller with zero visible
  latency (runtime late-latch, quad space = action space).
- All verbs clamp per `05-ipc-config.md` §3.1 (pitch ±85°, size 0.2–4 m, distance 0.3–5 m)
  and fire `xrmonitoranchor` on mode changes.
- `hyprctl openxr layout` emits lines that, pasted into a config, reproduce the current
  arrangement (round-trip through the WP4 parser).

**Context to load.** `03-anchoring.md` (entire), `05-ipc-config.md` §3;
`src/openxr/XRMath.hpp`, `src/openxr/XRMonitorLayer.cpp`, `src/openxr/XRIpc.cpp`,
`tests/xr/parser.cpp` (test style).

---

### WP6 — Action system + frame→main queue infrastructure

**Goal.** The OpenXR action set (aim/grip poses, select, grab, scroll, menu, haptic) with
suggested bindings for all four profiles, `xrSyncActions` in the frame loop, and the
*general* frame→main SPSC event queue (eventfd on the wayland loop) that WP2's minimal
session-state handoff graduates into. No pointer injection yet (WP7).

**Deliverables.**
- Created: `src/openxr/XRInput.{hpp,cpp}` (actions, sync, per-frame action state snapshot).
- Modified: `OpenXRManager` (queue ownership + main-thread drain dispatching to IPC/idle/
  focus consumers), `XRSession` (attach action sets).

**Depends on:** WP2 (parallel with WP3–5).

**Acceptance.**
- With Monado remote driving a trigger pull, a debug log line on the *main thread* proves
  action state crossed the queue (temporary instrumentation acceptable).
- Bindings suggested for `khr/simple_controller`, `valve/index_controller`,
  `oculus/touch_controller`, `ext/hand_interaction` — no `xrSuggestInteractionProfileBindings`
  errors on any.
- Queue is lossless under burst (unit-style stress: enqueue 10k events from a thread, drain
  on the loop, count matches) and adds no wakeups when idle.

**Context to load.** `04-input.md` (action tables + queue design), `00-overview.md` (thread
model/handoff table); `src/openxr/XRSession.cpp`, `src/managers/EventLoopManager` (adding
fds/timers to the wl event loop), `src/openxr/OpenXRManager.cpp`.

---

### WP7 — Ray pointer

**Goal.** `CXRPointerDevice` (an `IPointer` subclass registered via
`g_pInputManager->newMouse`) driven by ray–quad intersection: aim pose → nearest-t hit
across layers → UV → `motionAbsolute` bound to the hit output; select-trigger hysteresis
(0.7/0.4) → button; thumbstick → axis; last-active hand owns the single pointer.

**Deliverables.**
- Created: `src/openxr/XRPointerDevice.{hpp,cpp}`, `tests/xr/ray_intersect.cpp`.
- Modified: `XRInput` (ray cast per frame, hover tracking → selection candidate §3.2 of
  `05-ipc-config.md`), `OpenXRManager` drain (inject on main thread), `XRIpc`
  (`hovered` in status JSON).

**Depends on:** WP4, WP6.

**Acceptance.**
- `tests/xr/ray_intersect.cpp` green in CI.
- Pointing at an XR monitor moves the Hyprland cursor onto the correct pixel of that output;
  trigger click focuses/activates the window under the ray; hysteresis: analog jitter around
  0.55 never double-clicks.
- Idle timers reset on XR input with **zero** explicit `onActivity()` calls in `src/openxr/`
  (grep proves it) — the normal device path provides it (`05-ipc-config.md` §6.4).
- `openxr:pointer = 0` removes the device live.

**Context to load.** `04-input.md` (intersection math, device design), `05-ipc-config.md`
§3.2+§6.4; `hyprtester/plugin/src/main.cpp` (`CTestMouse` — the IPointer-subclass reference),
`src/managers/input/InputManager.hpp` (`newMouse`), `src/openxr/XRInput.cpp`.

---

### WP8 — Grab interaction

**Goal.** The squeeze-driven grab state machine: grab = temporary device-lock with offset
`inv(gripPose)∘quadPose`; while grabbed, thumbstick.y push/pull (0.3–5 m) and thumbstick.x
resize (0.2–4 m); on release, convert back to the persistent anchor mode (re-anchor);
haptic pulses on grab/release; `xrmonitorgrab` events and `grabbed` in status JSON.

**Deliverables.**
- Modified: `XRInput` (grab machine on the frame thread), `XRAnchor` (grab-offset compose /
  release re-anchor — extend the WP5 unit tests), `XRMonitorLayer`, `OpenXRManager` drain +
  `XRIpc` (event + JSON field).

**Depends on:** WP5, WP7.

**Acceptance.**
- Grab-move a monitor 0.5 m: it tracks the controller rigidly (no lag), release re-anchors
  into its previous mode at the new pose; `xrmonitoranchor` fires if the mode conversion
  changed the stored pose representation; `xrmonitorgrab>>name,1/0` bracket the interaction.
- Push/pull and resize clamps hold; haptics fire on grab/release; grabbing with the second
  hand steals cleanly (last-active-hand rule).
- Extended anchor unit tests (offset round-trip) green.

**Context to load.** `04-input.md` (grab machine), `03-anchoring.md` (re-anchor on release);
`src/openxr/XRInput.cpp`, `src/openxr/XRAnchor.cpp`, `tests/xr/anchor_math.cpp`.

---

### WP9 — Idle-inhibit hook (tiny)

**Goal.** `shouldInhibitIdle()` + the guarded check inside
`CInputManager::recheckIdleInhibitorStatus()` before its final `setInhibit(false)`, and
recheck calls on every session-state transition. The only XR touch outside `src/openxr/`.

**Deliverables.** Modified: `src/managers/input/IdleInhibitor.cpp` (the `#ifdef HAVE_OPENXR`
block, exact snippet in `05-ipc-config.md` §6.2), `src/openxr/OpenXRManager.cpp`
(`shouldInhibitIdle()`, recheck-on-transition, recheck in `onConfigReload`).

**Depends on:** WP2.

**Acceptance.**
- State focused + `inhibit_idle=1` ⇒ an obey-inhibitors ext-idle-notify client never idles;
  flipping either condition un-inhibits within one recheck.
- Builds without `HAVE_OPENXR` (the hook compiles away); no behavior change for non-XR
  inhibitor logic (existing inhibitor tests still pass).

**Context to load.** `05-ipc-config.md` §6; `src/managers/input/IdleInhibitor.cpp`,
`src/protocols/IdleNotify.hpp`, `src/openxr/OpenXRManager.cpp`.

---

### WP10 — Test infrastructure + first two integration tests (schedule early)

**Goal.** The whole `06-testing.md` §2–§5 apparatus: `WITH_XR_TESTS` gating, `--xr` mode in
hyprtester, `MonadoOrchestrator`, vendored `monado_remote_wire.hpp` (pinned
`c2ddab59dc41366fe520dc4e8abcfea257ecf0b8`), `RemoteClient`, `xr_helpers`, `xr-test.conf`
(minus the `xrmonitor` lines until WP4 — start with `openxr:enabled=1` only, add the
declared fixtures in WP11), and tests `xr_session_up` + `xr_runtime_absent`.
**This WP exists early to de-risk Monado-null-compositor + EGL interop** and to answer the
FOCUSED-vs-VISIBLE question (`06-testing.md` §6 caveat) before WP7/8/12 assume the answer.

**Depends on:** WP2 (needs a session to bring up; WP4's keyword not required).

**Acceptance.**
- `cmake -B build-debug -DWITH_TESTS=ON -DWITH_XR_TESTS=ON` builds; without the option,
  hyprtester binary is byte-identical in behavior (no xr symbols run).
- `hyprtester --xr` with local Monado: `xr_session_up` passes (documents whether FOCUSED is
  reached — write the answer into the WP's PR description AND update `06-testing.md` §6 if
  it's VISIBLE-only).
- Without `monado-service`: every test SKIPs green except `xr_runtime_absent`, which passes.
- Wire-header `static_assert` sizes verified against the pinned Monado checkout; ABI
  mismatch path produces SKIP not FAIL (test by corrupting the magic locally).
- Artifacts land in `hyprtester/artifacts/<run-id>/` on a forced failure.

**Context to load.** `06-testing.md` (entire — it is this WP's spec);
`hyprtester/src/main.cpp`, `hyprtester/src/shared.hpp`, `hyprtester/src/hyprctlCompat.hpp`,
`hyprtester/CMakeLists.txt`, root `CMakeLists.txt` (~675–700),
`/home/ajg/code/monado/src/xrt/drivers/remote/r_interface.h` @ pinned commit.

---

### WP11 — Integration suite: monitors, anchors, mirror, teardown

**Goal.** Tests `xr_monitor_create_destroy`, `xr_config_declared` (+ add the two `xrmonitor`
fixture lines to `xr-test.conf`), `xr_anchor_transitions` (incl. leash convergence after the
scripted 90° head yaw via `RemoteClient::animate`), `xr_mirror`, `xr_disable_teardown`.

**Depends on:** WP4, WP5, WP10.

**Acceptance.** All five pass locally against Monado per the step/assertion table in
`06-testing.md` §6; each uses `XR-t<pid>-<n>` naming; each dumps artifacts on failure; suite
still SKIPs green without Monado. Update to a passing state, not "known-flaky": if a test
flakes ≥1-in-10 runs, fix the wait conditions before merging.

**Context to load.** `06-testing.md` §5+§6, `05-ipc-config.md` (JSON schema + reconcile
semantics under test), `03-anchoring.md` (expected convergence math);
`hyprtester/src/xr/RemoteClient.hpp`, `hyprtester/src/xr/xr_helpers.hpp`,
`hyprtester/src/tests/xr/tests.hpp`.

---

### WP12 — Integration suite: input + idle

**Goal.** Tests `xr_ray_click_routing`, `xr_grab_move`, `xr_scroll`, `xr_idle_inhibit`
(including the new `idle-notify` test client in `hyprtester/CMakeLists.txt`). Apply the
FOCUSED/VISIBLE gating decision recorded by WP10.

**Depends on:** WP7, WP8, WP9, WP10.

**Acceptance.** All four pass locally per `06-testing.md` §6 (rows 5–8); `idle-notify`
client builds under the existing `clientNew` machinery; input tests carry the
`waitForXrState` gate agreed in WP10; suite SKIPs green without Monado.

**Context to load.** `06-testing.md` §4+§6, `04-input.md` (the math the aim-pose computation
inverts), `05-ipc-config.md` §6; `hyprtester/src/xr/RemoteClient.hpp`,
`hyprtester/CMakeLists.txt` (`clientNew`), `hyprtester/clients/pointer-scroll.cpp`
(client reference).

---

### WP13 — Polish

**Goal.** Ship-readiness residue: example config block for the wiki/`assets`, man-page /
wiki notes (`hyprctl openxr`, `xrmonitor` keyword + dispatcher, the enable-vs-config
subtlety from `05-ipc-config.md` §4.2), Lua-config exposure decision for `xrmonitor`
(implement or explicitly document classic-config-only), layout persistence stretch
(auto-write a sourceable `xrmonitor` snippet on clean stop — only if cheap), and a final
docs/openxr consistency pass against the implemented reality.

**Depends on:** all previous.

**Acceptance.** `docs/hyprctl.1.rst` (or the wiki-notes file the maintainers prefer)
mentions the `openxr` command; an example config block exists and parses; every deviation
between docs/openxr/*.md and shipped behavior is either fixed in code or amended in the doc
in the same commit; full unit + integration suites green.

**Context to load.** All of `docs/openxr/`; `docs/hyprctl.1.rst`, `example/` config assets.

---

## Conventions for implementers (all WPs)

- **Branch**: all work lands on the `hypxrland` branch (created from `main`). One WP per
  commit (squash your WIP), commit message `openxr: WP<N> — <title>`.
- **Build check** before every commit: `cmake --build build-debug --target Hyprland` — and
  for WPs touching tests, also the `hyprland_gtests` / `hyprtester` targets. WPs 1–4 must
  additionally verify the **no-openxr-package** build (acceptance above).
- **Code style**: match surrounding Hyprland idiom exactly — `m_` members, `g_p` globals,
  `SP<>`/`WP<>`/`UP<>` smart pointers (`makeShared`/`makeUnique`), `Debug::log`
  (`Log::logger->log(Log::DEBUG|WARN|ERR, ...)` on current main — note there is no
  `Log::LOG` level), `sc<>`/`rc<>`/`dc<>` casts, clang-format with the repo's config.
- **Gating**: **all** XR code behind `#ifdef HAVE_OPENXR`, with three deliberate
  exceptions that compile unconditionally so unit tests always run: `XRMath.hpp`,
  `XRAnchor.{hpp,cpp}`, `XRMonitorConfig.{hpp,cpp}` (no OpenXR headers allowed in these).
  Outside `src/openxr/`, the total footprint is: build wiring, config value entries, the
  keyword/dispatcher registration lines, manager init, and the one guarded idle-hook block —
  nothing else.
- **Dependencies**: no new hard dependencies beyond the `openxr` (loader) package, and it
  stays optional. The test-only vendored wire header (`monado_remote_wire.hpp`) is the sole
  sanctioned vendoring; it never ships in the compositor.
- **Threading discipline**: main thread owns monitors/config/IPC/injection; frame thread owns
  GL/XR handles; the only crossings are the documented snapshot (main→frame) and event queue
  (frame→main). Any new crossing needs a doc update first.
- **When docs and reality conflict**: the docs were written against `main` @ the WP0 date;
  if the tree has drifted (e.g. config internals moved again), follow the tree's current
  idiom and note the deviation in your PR description so WP13 reconciles the docs.

---

## Context files to read before implementing

(For this doc itself — i.e. before *scheduling* WPs.)

- `docs/openxr/00-overview.md` — component map and lifecycle every WP references
- `docs/openxr/01-session-graphics.md` … `06-testing.md` — per-WP specs as listed above
- The per-WP "Context to load" lists — each is intentionally under ~10 files; do not load
  the whole tree
- `git show openxr:src/openxr/COpenXRManager.cpp` — the WIP branch source being ported in WP2/WP3
