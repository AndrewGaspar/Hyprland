# 04 — Input: `CXRInput` + `CXRPointerDevice`

Part of the HypXRland design doc set (`docs/openxr/`). This document specifies controller/hand
input: the OpenXR action system, ray-pointer synthesis into Hyprland's normal input pipeline, the
grab state machine, and the frame-thread→main-thread event handoff.

Scope boundaries: the grab **pose math** (offset capture, re-anchor on release, push/pull/resize
formulas) is `03-anchoring.md` §4 — this doc owns *when* those calls happen. All IPC/event/config
**tables** (exact socket2 payload formats, dispatcher registration, config var registration) are
`05-ipc-config.md` — this doc references event names and config vars by name only.

## 0. Files, threads, ownership

```
src/openxr/XRInput.hpp/.cpp          CXRInput   — FRAME THREAD: action set, xrSyncActions,
                                                  ray cast, hysteresis, grab machine, haptics,
                                                  producer side of the event queue
src/openxr/XRPointerDevice.hpp/.cpp  CXRPointerDevice — MAIN THREAD: IPointer subclass,
                                                  consumer side, signal emission
```

Both files are wrapped in `#ifdef HAVE_OPENXR` (unlike `XRMath.hpp`/`XRAnchor.*`, see doc 03 §0).

Data flow:

```
frame thread                                          main thread
────────────                                          ───────────
xrSyncActions / xrLocateSpace / xrGetActionState*
  → ray-quad hit test (§3) → hysteresis (§4)
  → grab machine (§6, calls CXRAnchor §4 math)
  → SXRInputEvent / SXRStateEvent
      → SPSC ring buffer ──write(eventfd)──►  wl_event_loop callback drains ring
                                                → CXRPointerDevice signal emission (§8)
                                                → g_pEventManager->postEvent(...) for state
                                                  events (ALL IPC emission stays main-thread)
```

## 1. Action set and suggested bindings

One action set, created at session init (before `xrAttachSessionActionSets`, which may be called
**only once** per session — attach after all suggestions):

| field | value |
|---|---|
| `actionSetName` | `hyprland` |
| `localizedActionSetName` | `Hyprland` |
| `priority` | `0` |

Subaction paths for every hand-scoped action: `/user/hand/left`, `/user/hand/right` (created with
`xrStringToPath`; pass both in `XrActionCreateInfo::subactionPaths`).

| action name | `XrActionType` | purpose |
|---|---|---|
| `aim_pose` | `XR_ACTION_TYPE_POSE_INPUT` | pointer ray origin/direction (§3) |
| `grip_pose` | `XR_ACTION_TYPE_POSE_INPUT` | grab anchor space; device-lock space (doc 03 §3.4) |
| `select` | `XR_ACTION_TYPE_FLOAT_INPUT` | left click, with hysteresis (§4) |
| `grab` | `XR_ACTION_TYPE_FLOAT_INPUT` | grab gesture, with hysteresis (§4, §6) |
| `scroll` | `XR_ACTION_TYPE_VECTOR2F_INPUT` | thumbstick: scroll when free (§5), push-pull/resize when grabbing (§6) |
| `menu` | `XR_ACTION_TYPE_BOOLEAN_INPUT` | context menu — synthesized as `BTN_RIGHT` click on the hovered quad (v1 semantics) |
| `haptic` | `XR_ACTION_TYPE_VIBRATION_OUTPUT` | tick pulses (§6.3) |

Create one `XrActionSpace` per hand for `aim_pose` and `grip_pose` (`xrCreateActionSpace`,
`poseInActionSpace = identity`) — 4 spaces total. The two grip spaces are also handed to
`CXRMonitorLayer` as the targets of `XR_SPACE_GRIP_LEFT/RIGHT` (doc 03 §2.3).

Boolean inputs bound to FLOAT actions and float inputs bound to BOOLEAN actions are converted
automatically by the runtime (OpenXR §11 input remapping) — used below for
khr/simple_controller's `select/click`.

### 1.1 `/interaction_profiles/khr/simple_controller`

This profile has **no analog grab and no thumbstick** — see §1.5 for the long-press-select grab
fallback. Suggested bindings (each row suggested for both hands unless split):

| action | binding path(s) |
|---|---|
| `aim_pose` | `/user/hand/left/input/aim/pose`, `/user/hand/right/input/aim/pose` |
| `grip_pose` | `/user/hand/left/input/grip/pose`, `/user/hand/right/input/grip/pose` |
| `select` | `/user/hand/left/input/select/click`, `/user/hand/right/input/select/click` (bool→float: 0.0/1.0) |
| `menu` | `/user/hand/left/input/menu/click`, `/user/hand/right/input/menu/click` |
| `haptic` | `/user/hand/left/output/haptic`, `/user/hand/right/output/haptic` |
| `grab`, `scroll` | *not suggested* |

### 1.2 `/interaction_profiles/valve/index_controller`

This is the profile Monado's remote test driver exposes (doc 06) — the integration-test profile.

| action | binding path(s) |
|---|---|
| `aim_pose` | `/user/hand/left/input/aim/pose`, `/user/hand/right/input/aim/pose` |
| `grip_pose` | `/user/hand/left/input/grip/pose`, `/user/hand/right/input/grip/pose` |
| `select` | `/user/hand/left/input/trigger/value`, `/user/hand/right/input/trigger/value` |
| `grab` | `/user/hand/left/input/squeeze/value`, `/user/hand/right/input/squeeze/value` |
| `scroll` | `/user/hand/left/input/thumbstick`, `/user/hand/right/input/thumbstick` (vector2 parent path) |
| `menu` | `/user/hand/left/input/a/click`, `/user/hand/right/input/a/click` |
| `haptic` | `/user/hand/left/output/haptic`, `/user/hand/right/output/haptic` |

### 1.3 `/interaction_profiles/oculus/touch_controller`

Menu is asymmetric on Touch: the left controller has `menu/click`; the right controller's
`system/click` is reserved by the runtime, so use `b/click` there.

| action | binding path(s) |
|---|---|
| `aim_pose` | `/user/hand/left/input/aim/pose`, `/user/hand/right/input/aim/pose` |
| `grip_pose` | `/user/hand/left/input/grip/pose`, `/user/hand/right/input/grip/pose` |
| `select` | `/user/hand/left/input/trigger/value`, `/user/hand/right/input/trigger/value` |
| `grab` | `/user/hand/left/input/squeeze/value`, `/user/hand/right/input/squeeze/value` |
| `scroll` | `/user/hand/left/input/thumbstick`, `/user/hand/right/input/thumbstick` |
| `menu` | `/user/hand/left/input/menu/click`, `/user/hand/right/input/b/click` |
| `haptic` | `/user/hand/left/output/haptic`, `/user/hand/right/output/haptic` |

### 1.4 `/interaction_profiles/ext/hand_interaction_ext` (optional)

Suggest **only when `XR_EXT_hand_interaction` was enabled at instance creation** (doc 01 lists it
as optional) — suggesting an unknown profile path returns `XR_ERROR_PATH_UNSUPPORTED` and must not
abort init; skip and log. No thumbstick/menu/haptic exist on hands (scroll and menu simply
unavailable; §6.3 haptics are no-ops via `XR_ERROR_ACTION_TYPE_MISMATCH` avoidance — just don't
fire haptics when the current profile lacks a binding).

| action | binding path(s) |
|---|---|
| `aim_pose` | `/user/hand/left/input/aim/pose`, `/user/hand/right/input/aim/pose` |
| `grip_pose` | `/user/hand/left/input/grip/pose`, `/user/hand/right/input/grip/pose` |
| `select` | `/user/hand/left/input/pinch_ext/value`, `/user/hand/right/input/pinch_ext/value` |
| `grab` | `/user/hand/left/input/grasp_ext/value`, `/user/hand/right/input/grasp_ext/value` |
| `scroll`, `menu`, `haptic` | *not suggested* |

### 1.5 Grab fallback for profiles without an analog grab (simple_controller) — **NOT IMPLEMENTED v1**

> **Status (WP13 reconciliation): not implemented.** The design below was the intended v1
> behavior, but no code path for it exists — `grep -rn "LONGPRESS\|XR_GRAB_LONGPRESS" src/`
> returns nothing, and `khr/simple_controller` handling in `CXRInput` (§1.1's suggested bindings)
> has no `grab`/`select` long-press state machine of any kind. As shipped, **`khr/simple_controller`
> has no way to grab an XR monitor** — select-click still works for the normal pointer/click path
> (§4), only the grab emulation described here is missing. Left in this doc as a known v1 gap /
> future-work spec, not a description of current behavior.

Detect the active profile with `xrGetCurrentInteractionProfile(session, "/user/hand/left|right")`
(re-query on `XrEventDataInteractionProfileChanged`). When the hand's profile is
`khr/simple_controller` (no `grab` binding), grab is emulated by **long-pressing select**:

- On select press-edge while hovering a quad: emit `BTN_LEFT` press immediately (normal click
  path) and start a timer `XR_GRAB_LONGPRESS_MS = 400`.
- If select is still held ≥ 400 ms, the hover target is unchanged, and no pointer motion beyond a
  small slop occurred: emit `BTN_LEFT` **release**, then enter GRABBED (§6) as if `grab` crossed
  its threshold.
- Grab ends when select is released.

This intentionally sacrifices long-press-drag on simple_controller; real profiles are unaffected.

## 2. Frame-thread sampling loop

Called once per XR frame from the frame loop (doc 01), after `xrWaitFrame` gave
`predictedDisplayTime`, before layer solve/submit:

```
CXRInput::sample(XrTime predictedDisplayTime):
    XrActiveActionSet active = { m_actionSet, XR_NULL_PATH };
    XrActionsSyncInfo si     = { …, .countActiveActionSets = 1, .activeActionSets = &active };
    res = xrSyncActions(m_session, &si);
    // XR_SESSION_NOT_FOCUSED is a SUCCESS code: actions simply read inactive.
    // xrSyncActions only delivers input while session state == FOCUSED. NOTE for tests
    // (doc 06 / plan risk 3): verify Monado's remote driver reaches FOCUSED, else input
    // tests must gate on VISIBLE and be skipped.

    for hand in {LEFT, RIGHT}:
        XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
        xrLocateSpace(m_aimSpace[hand],  m_localFloorSpace, predictedDisplayTime, &loc);
        h.aim  = validOrNullopt(loc);   // requires POSITION_VALID | ORIENTATION_VALID
        xrLocateSpace(m_gripSpace[hand], m_localFloorSpace, predictedDisplayTime, &loc);
        h.grip = validOrNullopt(loc);

        h.select = getFloat(m_selectAction, m_handPath[hand]);   // xrGetActionStateFloat
        h.grab   = getFloat(m_grabAction,   m_handPath[hand]);
        h.stick  = getVec2 (m_scrollAction, m_handPath[hand]);   // xrGetActionStateVector2f
        h.menu   = getBool (m_menuAction,   m_handPath[hand]);
        // each getter returns {value, isActive}; inactive ⇒ treat as 0 / released

    updateGrabMachine(dt)          // §6 — may consume this hand's ray + stick
    for hand not grabbing:
        castRay(hand)              // §3 → hover / uv / owner arbitration
    emitHysteresisEdges()          // §4 → BUTTON events
    emitScroll(dt)                 // §5 → AXIS events
    if anythingQueued: push(FRAME event); write(m_eventFd, 1)
```

Grip poses (`h.grip`) are also what `CXRMonitorLayer::solve()` receives as
`SXRSolveInput::gripLeft/Right`, and the latest `{view, grips}` tuple is copied out for the
main-thread `SXRVerbContext` (doc 03 §5).

## 3. Ray → quad intersection

The pointer ray for a hand is the **aim pose**: origin = `aim.pos`, direction =
`qRotate(aim.rot, (0,0,−1))` (aim −Z per OpenXR convention).

For each visible layer with solved world pose `Q` (= `SXRSolveResult::worldPose`, doc 03 §2.3),
width `w`, height `h` (meters): transform the ray into quad-local frame and intersect the z = 0
plane (the quad lies in its local x–y plane, centered at origin):

```
o = qRotate(qInverse(Q.rot), rayOrigin − Q.pos)     // = poseInverse(Q) applied to origin
d = qRotate(qInverse(Q.rot), rayDir)

if |d.z| < 1e-6: miss                               // ray parallel to quad plane
t = −o.z / d.z
hit iff t > 0  AND  |p.x| ≤ w/2  AND  |p.y| ≤ h/2,  where p = o + t·d
```

**Nearest `t` across all visible layers wins** (occlusion between overlapping quads).

UV and pixel mapping (quad +X right, +Y up; surface v grows downward):

```
u = p.x / w + 0.5          // 0 at left edge, 1 at right
v = 0.5 − p.y / h          // 0 at top edge, 1 at bottom
pixel = (u * pxW, v * pxH)
```

`(u, v)` is exactly the 0.0–1.0 range `IPointer::SMotionAbsoluteEvent::absolute` expects
(`src/devices/IPointer.hpp:26-30`) — no pixel conversion needed on our side; Hyprland maps it onto
the bound output.

**Two-hand policy: one synthetic pointer.** Both hands cast every frame, but only the *owner* hand
drives the pointer. Ownership transfers to the hand that last produced (a) a click edge
(select press) or (b) a hover change (its ray's hit monitor differs from its previous frame's hit,
including none→some). The non-owner hand's hover is still tracked (for grab targeting §6 and
last-hovered bookkeeping §9) but emits no motion/button events.

Motion events are emitted only when the owner's hit `(monitorID, uv)` changed since the last
emission (uv epsilon `1e-4`) — coalescing at the source.

## 4. Hysteresis (Schmitt trigger) for analog buttons

Config vars (registration in doc 05):

| var | default | meaning |
|---|---|---|
| `openxr:pointer_trigger_threshold` | 0.7 | select: press edge at value ≥ this |
| `openxr:pointer_trigger_threshold_release` | 0.4 | select: release edge at value ≤ this |
| `openxr:grab_threshold` | 0.7 | grab: engage at value ≥ this |
| `openxr:grab_threshold_release` | 0.4 | grab: disengage at value ≤ this |

```
struct SSchmitt {
    bool state = false;
    bool update(float v, float on, float off) {   // returns true on edge
        if (!state && v >= on)  { state = true;  return true; }
        if ( state && v <= off) { state = false; return true; }
        return false;
    }
};
```

Per hand: `m_selectTrig`, `m_grabTrig`. A select press edge on the owner hand while hovering emits
`BTN_LEFT` (0x110) press on the hovered monitor; release edge emits release **even if the ray has
left the quad** (button release must never be dropped — Hyprland's normal drag semantics take care
of the rest). Menu bool press/release edges map identically to `BTN_RIGHT` (0x111). Select edges
also transfer pointer ownership (§3).

## 5. Scroll

Free-hand (not grabbing) thumbstick → axis events, batched per frame:

```
if |stick.y| > XR_STICK_DEADZONE (0.1):
    delta = −stick.y * 15.0 * *PSCROLLSPEED        // openxr:scroll_speed, default 1.0
    push AXIS { axis = WL_POINTER_AXIS_VERTICAL_SCROLL, delta }
if |stick.x| > XR_STICK_DEADZONE:
    delta =  stick.x * 15.0 * *PSCROLLSPEED
    push AXIS { axis = WL_POINTER_AXIS_HORIZONTAL_SCROLL, delta }
```

- **15.0 = one standard wheel notch** in Wayland axis units; a fully deflected stick therefore
  scrolls one notch per frame batch.
- Sign: OpenXR thumbstick +y is up; wl positive vertical delta means scroll down — hence the
  negation (stick up ⇒ content scrolls up, matching wheel-up).
- Emitted with `source = WL_POINTER_AXIS_SOURCE_CONTINUOUS`, `deltaDiscrete = 0` (continuous
  device, like touchpads — avoids discrete-step rounding).
- Only the owner hand scrolls, only while hovering; each batch is terminated by the frame event
  (§7), which flushes `onPointerFrame` → seat frame.

## 6. Grab state machine

Per-hand inputs, but at most **one grab per monitor** and one grab per hand; state lives in
`CXRInput`:

```
                     ray hits quad Q                         grabTrig ≥ grab_threshold
        ┌──────────────────────────────────┐          ┌────────(±5° cone forgiveness)────────┐
        │                                  ▼          │                                      ▼
      IDLE                            HOVER(Q, hand) ─┘                          GRABBED(Q, hand, offset)
        ▲                                  │  ▲                                              │
        │      ray leaves all quads        │  │      still hovering Q?                       │
        └──────────────────────────────────┘  └──── yes ─── grabTrig ≤ grab_threshold_release┘
        ▲                                                        no │
        └────────────────────────────────────────────────────────── ┘
```

| transition | actions |
|---|---|
| IDLE → HOVER | update last-hovered XR monitor (§9) |
| HOVER → GRABBED (grab value crosses `openxr:grab_threshold`, rising) | `anchor->beginGrab(hand, gripWorld)` (doc 03 §4.1); suppress this hand's pointer (no motion/button/scroll from it; if it owned the pointer, ownership becomes free-for-take by the other hand); haptic tick (§6.3); queue `SXRStateEvent` → socket2 `xrmonitorgrab` with payload `…,1` (exact format: doc 05) |
| while GRABBED (each frame) | quad pose = `grip ∘ offset` via the doc 03 §4.2 solve override (runtime late-latched); `stick.y` → `anchor->grabPushPull(stick.y * XR_GRAB_PUSHPULL_SPEED * dt)` — push/pull along the grip→quad ray, clamped 0.3–5 m; `stick.x` → `anchor->grabResize(stick.x * XR_GRAB_RESIZE_SPEED * dt)` — width clamped 0.2–4 m (doc 03 §4.3) |
| GRABBED → HOVER/IDLE (grab value ≤ `openxr:grab_threshold_release`) | `anchor->endGrab(...)` — re-anchors into the persistent mode (doc 03 §4.4); haptic tick; queue `xrmonitorgrab …,0`; un-suppress the hand's pointer; HOVER if its ray still hits a quad else IDLE |

**5° cone forgiveness on entry:** squeezing while pointing *near* a quad still grabs it. On the
grab rising edge, if the hand hovers nothing, redo the §3 intersection with expanded half-extents:

```
slack  = tan(XR_GRAB_CONE_DEG = 5°) * t          // t from the plane intersection
hit iff t > 0 AND |p.x| ≤ w/2 + slack AND |p.y| ≤ h/2 + slack
```

nearest-t across layers as usual; if still nothing, the squeeze is ignored (no state change).

Interactions with the rest of the system:

- The **other** hand keeps full pointer function while one hand grabs (it can hover/click other
  monitors, including the grabbed one).
- A second grab on an already-GRABBED monitor is ignored (first grabber wins); each hand may grab
  a different monitor simultaneously.
- Monitor destroyed while grabbed → force-release without `endGrab` re-anchor (layer is gone),
  emit `xrmonitorgrab …,0`.
- Grip tracking lost while grabbed → hold (doc 03 §4.2 falls back to last world); release still
  processed on the grab-release edge.

### 6.3 Haptics

Short tick via `xrApplyHapticFeedback` on: grab enter, grab release, select click press (not
release). Parameters:

```
XrHapticVibration vib { XR_TYPE_HAPTIC_VIBRATION };
vib.duration  = XR_HAPTIC_TICK_NS = 10'000'000;   // 10 ms
vib.frequency = XR_FREQUENCY_UNSPECIFIED;
vib.amplitude = 0.5f;
XrHapticActionInfo hai { …, .action = m_hapticAction, .subactionPath = m_handPath[hand] };
xrApplyHapticFeedback(m_session, &hai, (XrHapticBaseHeader*)&vib);
```

Fire-and-forget from the frame thread; skip when the hand's current profile has no haptic binding
(§1.4).

## 7. Frame→main handoff: event queue

### 7.1 Event structs

```cpp
// XRInput.hpp
enum class eXRInputEventType : uint8_t {
    MOTION_ABS = 0, // pointer moved on a quad
    BUTTON,         // select/menu edge
    AXIS,           // scroll
    FRAME,          // batch terminator
};

struct SXRInputEvent {
    eXRInputEventType type      = eXRInputEventType::FRAME;
    MONITORID         monitorID = -1;  // src/SharedDefs.hpp:59 (int64_t); MOTION_ABS/BUTTON
    Vector2D          uv;              // MOTION_ABS: 0.0–1.0 (§3)
    uint32_t          button    = 0;   // BUTTON: BTN_LEFT 0x110 / BTN_RIGHT 0x111
    bool              pressed   = false;
    wl_pointer_axis   axis      = WL_POINTER_AXIS_VERTICAL_SCROLL; // AXIS
    double            axisDelta = 0.0;                             // AXIS (§5 units)
    uint32_t          timeMs    = 0;   // Time::millis(Time::steadyNow()) at sample time
};

enum class eXRStateEventType : uint8_t {
    SESSION_STATE,   // XrSessionState changed → openxrsessionstate / openxractive (doc 05)
    GRAB,            // §6 → xrmonitorgrab
    TRACKING,        // device-lock tracking gained/lost (informational, logged)
    LAYER_REMOVED,   // (as built, WP13 reconciliation — see below) removal-barrier ack
};

struct SXRStateEvent {
    eXRStateEventType type;
    MONITORID         monitorID = -1;
    int32_t           a         = 0;   // GRAB: 1/0; SESSION_STATE: XrSessionState value
    std::string       str;             // optional payload (e.g. monitor name if id-lookup raced);
                                        // LAYER_REMOVED: the removed layer's monitor name
};

using XRQueueItem = std::variant<SXRInputEvent, SXRStateEvent>;
```

**`LAYER_REMOVED` (as built, WP13 reconciliation):** doc 02's removal-barrier section ("Destroy
paths") already describes the frame thread pushing a "`layer-removed(name)` ack onto the frame→
main queue" once it has dropped a pending-removal layer from its snapshot and torn down that
layer's frame-side resources — this is the fourth `eXRStateEventType` value, `LAYER_REMOVED`
(`str` = the layer's monitor name). It is internal-only (not a socket2 event): the main-thread
drain uses it purely to know when it is safe to erase the layer from `m_layers` and call
`m_output->destroy()` (doc 02 step 3 of path A).

Session/state events use the **same queue** as input so that *all* IPC emission
(`g_pEventManager->postEvent`, `src/managers/EventManager.hpp:17`) happens on the main thread —
`CEventManager` is not thread-safe and every existing caller is main-thread.

### 7.2 SPSC ring + eventfd

- Single-producer (frame thread) / single-consumer (main thread) ring of `XRQueueItem`,
  power-of-two capacity `XR_QUEUE_CAP = 1024`, `std::atomic<uint32_t> m_head/m_tail` with
  release/acquire ordering (producer writes item then releases head; consumer acquires head,
  reads, releases tail).
- Full-queue policy: drop `MOTION_ABS`/`AXIS` (they are re-generated next frame); `BUTTON`,
  `FRAME`, and all `SXRStateEvent`s must not be dropped — log an error once if it ever happens
  (1024 deep with per-dispatch drain cannot realistically fill).
- Wakeup: `m_eventFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)`. Producer `write()`s `uint64_t 1`
  after each batch. Main-thread registration (pattern:
  `src/helpers/MainLoopExecutor.cpp:17`):

```cpp
m_eventSource = wl_event_loop_add_fd(g_pEventLoopManager->m_wayland.loop, m_eventFd.get(),
                                     WL_EVENT_READABLE, ::onXRInputQueue, this);
```

- Callback: `read()` 8 bytes to clear the counter, then drain the ring to empty, dispatching each
  item (§8 for input, `CXRIpc`/`COpenXRManager` for state events). Teardown:
  `wl_event_source_remove(m_eventSource)` on the main thread **before** joining the frame thread
  (doc 01 teardown ordering), then close the fd.

## 8. `CXRPointerDevice` — the synthetic pointer

Modeled directly on `CTestMouse` (`hyprtester/plugin/src/main.cpp:148-173`), the proven pattern
for synthetic `IPointer` devices.

```cpp
// src/openxr/XRPointerDevice.hpp
#pragma once
#include "../devices/IPointer.hpp"

class CXRPointerDevice : public IPointer {
  public:
    static SP<CXRPointerDevice> create() {
        auto p          = SP<CXRPointerDevice>(new CXRPointerDevice());
        p->m_self       = p;
        p->m_deviceName = "xr-pointer";
        p->m_hlName     = "xr-pointer"; // overwritten by setupMouse via getNameForNewDevice
        return p;
    }

    virtual bool isVirtual() {
        return false; // mirrors CTestMouse registration; keeps per-device config
                      // (`device[xr-pointer] { ... }`) behaving like a physical mouse
    }

    virtual SP<Aquamarine::IPointer> aq() {
        return nullptr; // no backend device; setupMouse's libinput branch is skipped (guarded)
    }

    void destroy() {
        m_events.destroy.emit();
    }

  private:
    CXRPointerDevice() = default;
};
```

**Registration** (main thread, in `COpenXRManager` session start, gated on `openxr:pointer` = 1):

```cpp
m_pointerDevice = CXRPointerDevice::create();
g_pInputManager->newMouse(m_pointerDevice);   // InputManager.cpp:1266 → setupMouse (:1282)
```

`setupMouse` attaches it to the pointer manager (`Pointer::mgr()->attachPointer`,
`PointerManager.cpp:930`) and installs a destroy listener → `destroyPointer`
(`InputManager.cpp:1305`, `InputManager.hpp:111`).

**Removal** (manager stop / `openxr:pointer` hot-toggled off):

```cpp
m_pointerDevice->destroy();   // fires m_events.destroy → InputManager + PointerManager detach
m_pointerDevice.reset();
```

**Event replay** — the main-thread queue drain maps `SXRInputEvent` to `m_pointerEvents` signals
(`src/devices/IPointer.hpp:92-109`) exactly as follows:

| queue item | emission |
|---|---|
| `MOTION_ABS` | resolve monitor by `monitorID` (skip event if the monitor died in flight); **`m_pointerDevice->m_boundOutput = monitor->m_name;`** then `m_pointerEvents.motionAbsolute.emit(SMotionAbsoluteEvent{ .timeMs = e.timeMs, .absolute = e.uv, .device = m_pointerDevice })` |
| `BUTTON` | `m_pointerEvents.button.emit(SButtonEvent{ .timeMs = e.timeMs, .button = e.button, .state = e.pressed ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED, .mouse = true })` |
| `AXIS` | `m_pointerEvents.axis.emit(SAxisEvent{ .timeMs = e.timeMs, .source = WL_POINTER_AXIS_SOURCE_CONTINUOUS, .axis = e.axis, .delta = e.axisDelta, .deltaDiscrete = 0, .mouse = true })` |
| `FRAME` | `m_pointerEvents.frame.emit()` |

`m_boundOutput` (`IPointer.hpp:112`) is the load-bearing detail: `motionAbsolute` routes through
`PointerManager::attachPointer`'s listener → `g_pInputManager->onMouseWarp`
(`PointerManager.cpp:951`), which maps the 0–1 `absolute` coordinate onto the named output's box —
so setting it to the **hit XR monitor's name** before each emission lands the cursor on the right
virtual monitor with zero extra routing code.

## 9. Selected-monitor concept

Some verbs need a target without a name (`xrmonitor destroy active`, and
`move/rotate/scale/distance/center/anchor` when the target is omitted/`active`). Resolution order,
owned by `COpenXRManager::selectedMonitor()`:

1. **Explicit selection** — last `xrmonitor select <name|next|prev>` (doc 05), if that monitor is
   still alive. `next`/`prev` cycle through live XR monitors in creation order. Cleared when the
   selected monitor is destroyed.
2. **Last ray-hovered XR monitor** — updated by §3/§6 hover from either hand (a
   main-thread-mirrored `MONITORID`, refreshed during queue drain), if alive.
3. **Focused monitor, if it is an XR monitor** — `Desktop::focusState()`-derived current monitor;
   applies when the user warped focus there with a normal keyboard bind.

If all three fail, the verb returns a dispatcher error ("no XR monitor selected").

## 10. Keyboard leg

**No synthetic keyboard.** The user's real keyboard already types into whatever XR monitor has
focus through the completely ordinary focus path (the ray click focuses the window under the
cursor like any mouse click). Repositioning from the keyboard = normal Hyprland binds calling the
`xrmonitor` dispatcher (registration and full verb table: doc 05; verb math: doc 03 §5).

Example binds (args are space-separated inside the single dispatcher-arg field, per the doc 05
grammar):

```ini
bind = SUPER, F9,          xrmonitor, create XR-1 2560x1440@90
bind = SUPER, F10,         xrmonitor, destroy active
bind = SUPER, bracketright, xrmonitor, select next
bind = SUPER, bracketleft,  xrmonitor, select prev
bind = SUPER ALT, left,    xrmonitor, move -0.1 0 0
bind = SUPER ALT, right,   xrmonitor, move 0.1 0 0
bind = SUPER ALT, up,      xrmonitor, move 0 0.1 0
bind = SUPER ALT, down,    xrmonitor, move 0 -0.1 0
bind = SUPER ALT, prior,   xrmonitor, distance -0.1
bind = SUPER ALT, next,    xrmonitor, distance 0.1
bind = SUPER ALT, equal,   xrmonitor, scale +0.1
bind = SUPER ALT, minus,   xrmonitor, scale -0.1
bind = SUPER ALT, home,    xrmonitor, center
bind = SUPER ALT, h,       xrmonitor, anchor active head
bind = SUPER ALT, l,       xrmonitor, anchor active local
```

## 11. Idle

**No extra code is needed for idle activity.** Every listener installed by
`CPointerManager::attachPointer` (`src/pointer/PointerManager.cpp:942-968`) — motion,
motionAbsolute, button, axis — calls `PROTO::idle->onActivity()` after dispatching. Because
`CXRPointerDevice` is attached through that exact path, XR pointer input resets hypridle/idle
timers for free, identically to a physical mouse.

Idle **inhibition** while the session is FOCUSED (`openxr:inhibit_idle`) is a separate mechanism
and is owned by `05-ipc-config.md` (the `recheckIdleInhibitorStatus()` hook).

## 12. Constants introduced by this doc

```cpp
constexpr int   XR_GRAB_LONGPRESS_MS   = 400;          // §1.5 simple_controller fallback (NOT IMPLEMENTED v1, see §1.5)
constexpr float XR_STICK_DEADZONE      = 0.1f;         // §5
constexpr float XR_SCROLL_NOTCH        = 15.0f;        // §5, wl units per full deflection/frame
constexpr float XR_GRAB_CONE_DEG       = 5.0f;         // §6 entry forgiveness
constexpr float XR_GRAB_PUSHPULL_SPEED = 2.0f;         // m/s at full stick.y while grabbed
constexpr float XR_GRAB_RESIZE_SPEED   = 1.0f;         // m/s of width at full stick.x
constexpr long  XR_HAPTIC_TICK_NS      = 10'000'000;   // §6.3, 10 ms
constexpr size_t XR_QUEUE_CAP          = 1024;         // §7.2, power of two
```

Distance/width clamps (0.3–5 m, 0.2–4 m) live in doc 03 §8 (`XR_DISTANCE_*`, `XR_WIDTH_*`).

## Context files to read before implementing

- `/home/ajg/code/Hyprland/docs/openxr/00-overview.md` — thread model, lifecycle states
- `/home/ajg/code/Hyprland/docs/openxr/01-session-graphics.md` — frame loop (where `sample()` is called), session event pump, teardown ordering
- `/home/ajg/code/Hyprland/docs/openxr/02-virtual-monitors.md` — `CXRMonitorLayer`, layer snapshot, monitor IDs
- `/home/ajg/code/Hyprland/docs/openxr/03-anchoring.md` — grab pose math (§4), solve API, `SXRVerbContext`
- `/home/ajg/code/Hyprland/docs/openxr/05-ipc-config.md` — config var + dispatcher + hyprctl + socket2 event tables, idle-inhibit hook
- `/home/ajg/code/Hyprland/hyprtester/plugin/src/main.cpp` — `CTestMouse`/`CTestKeyboard` (lines 97–173): the reference synthetic-device pattern; registration at lines 753–763
- `/home/ajg/code/Hyprland/src/devices/IPointer.hpp` — event structs, `m_pointerEvents` signals, `m_boundOutput`
- `/home/ajg/code/Hyprland/src/devices/IHID.hpp` — `m_deviceName`/`m_hlName`, destroy signal
- `/home/ajg/code/Hyprland/src/managers/input/InputManager.cpp` — `newMouse` (:1266), `setupMouse` (:1282), destroy listener (:1305)
- `/home/ajg/code/Hyprland/src/pointer/PointerManager.cpp` — `attachPointer` (:930–1010): signal routing + `PROTO::idle->onActivity()`
- `/home/ajg/code/Hyprland/src/managers/EventManager.hpp` — `SHyprIPCEvent` / `postEvent` (main-thread only)
- `/home/ajg/code/Hyprland/src/helpers/MainLoopExecutor.cpp` — the fd-on-wayland-event-loop pattern (:17)
- `/home/ajg/code/Hyprland/src/managers/eventLoop/EventLoopManager.hpp` — `m_wayland.loop` (:92)
- `/home/ajg/code/Hyprland/src/SharedDefs.hpp` — `MONITORID` (:59)
- `git show openxr:src/openxr/COpenXRManager.cpp` — WIP branch frame-thread structure
