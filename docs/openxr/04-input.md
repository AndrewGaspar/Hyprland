# 04 — Input: `CXRInput` + `CXRPointerDevice`

Part of the OpenXR documentation set (`docs/openxr/`). This page describes controller and hand
input: the OpenXR action system, ray-pointer synthesis into Hyprland's normal input pipeline, the
grab state machine and grabbable chrome, and the frame-thread → main-thread event handoff.

Where responsibilities cross into a sibling page: the grab **pose math** (offset capture, re-anchor
on release, push/pull/resize formulas, distance/width clamps) lives with the anchoring page — this
page owns *when* those calls happen. The full config-var, dispatcher, `hyprctl`, and socket2 event
reference lives with the configuration page; this page names events and config vars but does not
re-tabulate their registration.

## Files, threads, ownership

```
src/openxr/XRInput.hpp/.cpp          CXRInput  — action set, xrSyncActions, ray cast,
                                                 hysteresis, grab machine, haptics, producer
                                                 side of the frame->main event queue
src/openxr/XRPointerDevice.hpp/.cpp  CXRPointerDevice — IPointer subclass, the synthetic pointer
src/openxr/XRMath.hpp                pure ray-quad intersection, chrome geometry + hit
                                                 classification, 1€ filter (unconditionally compiled)
```

`XRInput.*` and `XRPointerDevice.*` are wrapped in `#ifdef HAVE_OPENXR`. `XRMath.hpp` is compiled
unconditionally (no OpenXR headers), so its pure math — the ray-quad hit test, the chrome
classifier, the 1€ filter — is always buildable and unit-testable without a runtime present.

`CXRInput` is created on the **main thread** in `COpenXRManager::start()`, after the session and
reference spaces exist and before the frame thread spawns, so that `xrAttachSessionActionSets`
(legal only once per session) runs before the frame loop begins. Its `sample()` and
`processPointer()` run on the **frame thread**, once per XR frame. `CXRPointerDevice` lives entirely
on the **main thread**.

Data flow, frame thread to main thread:

```
frame thread                                          main thread
────────────                                          ───────────
CXRInput::sample()   xrSyncActions / xrLocateSpace
CXRInput::processPointer()
  → ray-quad hit test → chrome classify → hysteresis
  → grab machine (calls the layer's CXRAnchor)
  → SXRInputEvent / SXRStateEvent
      → SPSC ring ──write(eventfd)──►  wl_event_loop fd callback drains the ring
                                         → CXRPointerDevice signal emission (pointer events)
                                         → g_pEventManager->postEvent(...) for socket2 events
```

All IPC emission stays on the main thread — `CEventManager` is not thread-safe, so the frame thread
only ever hands work across the queue.

## 1. Action set and interaction profiles

`CXRInput::init()` builds one action set (`actionSetName` `hyprland`, localized `Hyprland`,
priority `0`), creates every action with both subaction paths `/user/hand/left` and
`/user/hand/right`, suggests bindings for each supported interaction profile, creates the per-hand
action spaces, and attaches the set.

| action | `XrActionType` | purpose |
|---|---|---|
| `aim_pose` | `POSE_INPUT` | pointer ray origin/direction |
| `grip_pose` | `POSE_INPUT` | grab anchor space; also the device-lock space handed to the layer solve |
| `select` | `FLOAT_INPUT` | left click, via Schmitt hysteresis |
| `grab` | `FLOAT_INPUT` | grab gesture (squeeze / fist), via hysteresis |
| `scroll` | `VECTOR2F_INPUT` | thumbstick: scroll when free, push-pull/resize while grabbing |
| `menu` | `BOOLEAN_INPUT` | synthesized as `BTN_RIGHT` on the hovered quad |
| `haptic` | `VIBRATION_OUTPUT` | tick pulses |

When `XR_EXT_hand_interaction` is enabled at instance creation, two more actions are created:
`pinch_value` (`FLOAT_INPUT`) and `pinch_pose` (`POSE_INPUT`). They are bound only on the hand
profile.

Action spaces (identity pose in action space) are created per hand for `aim_pose` and `grip_pose`
(four total), plus per hand for `pinch_pose` when hand interaction is present. The two grip spaces
are also the targets the layer solve late-latches against for grip-locked quads; a pinch space is
the late-latch target for a pinch-anchored hand move-grab. A pinch-space creation failure is
non-fatal — hand grabs fall back to the grip pose.

Suggesting a profile the runtime does not recognize returns `XR_ERROR_PATH_UNSUPPORTED`; this is
logged at debug and skipped, never fatal. Any other suggestion failure is a warning. Init only fails
on genuine errors (action set or core action creation).

### Bound profiles

**`khr/simple_controller`** — binds `aim_pose`, `grip_pose`, `select` (`select/click`, the runtime
converts the boolean to 0.0/1.0 for the float action), `menu` (`menu/click`), and `haptic`. This
profile has no analog grab and no thumbstick, so it drives the pointer and clicks but **cannot grab
or scroll an XR monitor** — there is no grab/thumbstick binding for it.

**`valve/index_controller`** — the profile Monado's remote test driver exposes. Binds `aim_pose`,
`grip_pose`, `select` (`trigger/value`), `grab` (`squeeze/value`), `scroll` (`thumbstick`), `menu`
(`a/click`), `haptic`.

**`oculus/touch_controller`** — same as Index, except menu is asymmetric: left is `menu/click`,
right is `b/click` (the right `system/click` is runtime-reserved).

**`ext/hand_interaction_ext`** — suggested only when `XR_EXT_hand_interaction` is enabled. Binds
`aim_pose`, `grip_pose`, and `grab` (`grasp_ext/value`, the fist curl). `pinch_ext/value` is bound
to **both** `select` (so a body pinch registers as a click) and the dedicated `pinch_value` action
(the pinch grab gesture); `pinch_ext/pose` binds `pinch_pose`, the stable move-grab anchor. Binding
one source to two actions is legal and intentional. Hands have no thumbstick, menu, or haptic
binding.

Boolean-to-float and float-to-boolean input remapping is handled by the runtime.

## 2. Frame-thread sampling loop

`CXRInput::sample(predictedDisplayTime, refSpace)` runs once per XR frame, after `xrWaitFrame`
yields `predictedDisplayTime`. When the runtime has signalled an interaction-profile change (or on
the first sample) it re-reads each hand's current profile to refresh the active-device cache. It
then calls `xrSyncActions`; `XR_SESSION_NOT_FOCUSED` is a success code (actions read inactive), so
only a genuine failure is worth a once-only warning, and it clears the hand state. Input is only
delivered while the session is FOCUSED.

For each hand it locates the aim, grip, and pinch action spaces in `refSpace` at the predicted time
(requiring both `POSITION_VALID` and `ORIENTATION_VALID`, else `nullopt`), and reads `select`,
`grab`, `pinch_value`, `scroll` (thumbstick), and `menu`; each getter yields `{value, isActive}`
and an inactive action reads as released/zero. The result is snapshotted into `m_hands[hand]`.

`sample()` only reads; the ray cast, hover arbitration, hysteresis, grab machine, and scroll all
happen in `processPointer()`, which the frame loop calls **after** it has solved each visible
layer's world pose and built the pointer-target list. `processPointer()` runs under the manager's
layer mutex, the same discipline as the solve loop, because the grab machine mutates layer anchor
state.

## 3. Ray → quad intersection and the single pointer

The pointer ray for a hand is its aim pose: origin = `aim.pos`, direction = `qRotate(aim.rot,
(0,0,−1))`. `OpenXR::rayQuadIntersect` transforms the ray into each quad's local frame and
intersects the `z = 0` plane (the quad lies in its local x–y plane, centered at origin, +X right /
+Y up):

```
o = qRotate(qInverse(Q.rot), rayOrigin − Q.pos)
d = qRotate(qInverse(Q.rot), rayDir)
if |d.z| < 1e-6: miss                               // ray parallel to the plane
t = −o.z / d.z                                       // miss if t ≤ 0
p = o + t·d
hit iff |p.x| ≤ w/2 + slack  AND  |p.y| ≤ h/2 + slack
u = p.x / w + 0.5    (0 left, 1 right)
v = 0.5 − p.y / h    (0 top, 1 bottom; surface v grows downward)
```

`slack` is 0 for the ordinary hover test; it is non-zero only for the grab entry cone (§7). The
`w`/`h` are the **full quad** meters (content plus chrome margins); the hit `(u, v)` is then
classified against the quad's chrome layout (§6), and a body hit is remapped to content `(u, v)`.
Nearest `t` across all visible targets wins, giving occlusion between overlapping quads.

Each layer contributes an `SXRPointerTarget` per frame: monitor id, the solved full-quad center
pose (in the same reference frame as the sampled aim poses), full-quad width/height, name, its live
`CXRAnchor` (a raw frame-thread-only pointer, never cached across frames), and its normalized chrome
geometry.

**One synthetic pointer, two hands.** Both hands cast every frame, but only the *owner* hand drives
the pointer. Ownership transfers to the hand that most recently changed its body-hover target
(including none↔some) or pressed select or menu. The non-owner hand's hover is still tracked (for
grab targeting and last-hovered bookkeeping) but emits no pointer events. Motion is emitted only
when the owner's `(monitorID, uv)` changed since the last emission (uv epsilon `1e-4`), coalescing
at the source; `monitorID = −1` signals the ray left all quads (a hover clear).

## 4. Hysteresis (Schmitt trigger) for analog buttons

`SXRSchmitt::update(v, on, off)` returns true on an edge: it latches on at `v ≥ on` and off at
`v ≤ off`. Per hand there is a select trigger and a grab trigger (and a menu trigger driven by the
boolean at a 0.5/0.5 threshold).

| config var | default | meaning |
|---|---|---|
| `openxr:pointer_trigger_threshold` | 0.7 | select press edge |
| `openxr:pointer_trigger_threshold_release` | 0.4 | select release edge |
| `openxr:grab_threshold` | 0.7 | grab engage edge |
| `openxr:grab_threshold_release` | 0.4 | grab disengage edge |

A select press on the owner hand while hovering a quad emits `BTN_LEFT` (0x110) press on the hovered
monitor and a haptic tick; the release edge always emits `BTN_LEFT` release — even if the ray has
since left the quad — so a drag can never leave the button stuck. Menu maps identically to
`BTN_RIGHT` (0x111), without a haptic. A press is only honored when no other hand already holds that
button (single pointer); a release always closes its own press. Select and menu presses also
transfer pointer ownership.

## 5. Scroll

The owner hand's thumbstick, while hovering and not grabbing, becomes axis events, batched per
frame. Past a deadzone of `0.1`, `delta = ∓stick.{y,x} · 15.0 · openxr:scroll_speed` (default
scroll speed `1.0`). `15.0` is one standard wheel notch in Wayland axis units, so a fully deflected
stick scrolls one notch per frame batch. The vertical sign is negated because OpenXR stick +y is up
while a positive Wayland vertical delta scrolls down, so pushing up scrolls content up. Axes are
emitted with `source = CONTINUOUS`, `deltaDiscrete = 0` (like a touchpad), and each batch is
terminated by the frame event that flushes the seat frame.

## 6. Grabbable chrome: geometry and hit classification

Each quad's submitted `XrCompositionLayerQuad` is grown with a **transparent alpha margin** around
the desktop content: the swapchain is content plus margins, the desktop blits into the inner content
rect (alpha 1), and the margin renders transparent (alpha 0). `size:` therefore still means
**content** meters; the quad grows to hold the chrome, which never covers a desktop pixel.

`SXRChromeGeometry` (built by `makeChromeGeometry`) describes the layout purely as normalized
fractions of the full quad (`u,v ∈ [0,1]`, v growing down). Storing fractions — not meters or pixels
— makes the blit inset, the submitted quad size, and the hit classifier all derive from one source,
so a body hit remaps to exactly the desktop pixel the content blit wrote, and stays consistent
across a live meters-resize with no swapchain churn.

| config var | default | meaning |
|---|---|---|
| `openxr:chrome_enabled` | true | master toggle (0 collapses margins to 0 → no chrome) |
| `openxr:chrome_margin` | 0.10 | transparent margin around content, meters |
| `openxr:chrome_bar_height` | 0.08 | move-bar height, meters (below content, in the bottom margin) |
| `openxr:chrome_bar_width_frac` | 0.8 | move-bar width as a fraction of content width |
| `openxr:chrome_corner_size` | 0.09 | corner resize handle size, meters (clamped into the margin) |
| `openxr:chrome_hide_delay_ms` | 1500 | idle grace before the chrome fades out |
| `openxr:chrome_fade_ms` | 150 | fade in/out duration |
| `openxr:chrome_col_idle` | 0x66aaaaaa | color at rest |
| `openxr:chrome_col_hover` | 0xcc66aaff | color of the element the ray points at |
| `openxr:chrome_col_grab` | 0xff66aaff | color while grabbed |

The left/right/top margins are `margin`; the bottom margin is `margin + barHeight`. The move-bar is
centered under the content, `barWidthFrac` of the content width. Corner handles are `cornerSize`
squares at the content's outer corners, clamped inside the margin so they never eat content.

`classifyQuadHit(u, v, g)` returns the region for a full-quad hit, and never returns `NONE` (it
assumes a real hit). Content interior wins first (so no click pixel is stolen), then corner handles,
then the move-bar, else the transparent margin:

```
XR_REGION_BODY        over content — the only region that moves/clicks the pointer
XR_REGION_BAR         bottom move-bar
XR_REGION_CORNER_{TL,TR,BL,BR}   corner resize handles
XR_REGION_MARGIN      transparent dead margin (hover-only, no events)
```

With `chrome_margin == 0 && chrome_bar_height == 0` (or `chrome_enabled = 0`) the geometry is a
full-quad content rect, `classifyQuadHit` always returns BODY, and the chrome draw pass is skipped —
so chrome-disabled behaves exactly as if the margins never existed. `remapToContentUV` converts a
body hit's full-quad `(u,v)` to content `(u,v)`; `contentPoseToQuadCenter` shifts the anchor's
content-center pose to the quad geometric center for submission (needed because the bottom margin,
holding the bar, makes the margins asymmetric).

Only body hits produce pointer motion/click/scroll; bar/corner/margin hits are hover-only. The
per-frame hover region and grabbed state are published to each layer via plain atomics
(`m_hoverRegion`, `m_grabbedNow`) so the next frame's chrome draw can fade and color the affordance;
`chromeFadeAdvance` ramps a layer's chrome alpha toward its visibility target using frame-time deltas
(the predicted-display-time delta, not a wall clock), holding at 1 while the quad is hovered or
grabbed and for `chrome_hide_delay_ms` after, then fading to 0.

## 7. Grab state machine

Grab is per hand, with at most one grab per monitor and one grab per hand. On the grab rising edge
the machine decides what the gesture landed on and whether it may grab there; while held it drives
the anchor each frame; on the falling edge it re-anchors into the persistent mode.

**Entry.** `grabActionForRegion(region, grabAnywhere, handActive, handBodyGrab)` is the single
decision point: the bar always MOVEs, each corner always RESIZEs from that corner, the transparent
margin never grabs, and the body grabs conditionally:

- **Controller** (`grabAnywhere` = `openxr:grab_anywhere`, default true): a grip on the content body
  moves it. With `grab_anywhere = false`, moving is confined to the bar and resizing to the corners.
- **Hand** (`handBodyGrab` from `openxr:hand_grab_anywhere`): hands grab the body only when the
  config permits *this grab's gesture*; `grab_anywhere` does not apply to hands.

If the hover step already classified a grabbable region on a target this frame, that is used;
otherwise the intersection is redone with a **5° entry cone** (`slack = tan(5°) · t`), nearest
grabbable hit wins, and if nothing grabbable is found the squeeze is ignored. A grab on a monitor
already grabbed by the other hand is ignored (first grabber wins); each hand may grab a different
monitor at once. On a successful grab: the anchor's `beginGrab`/`beginResize` is called with the
device pose, pointer ownership is released if this hand held it, a haptic ticks, and an
`xrmonitorgrab …,1` state event is queued.

**While held**, the grabbed target is re-resolved by id each frame (a vanished target means the
monitor was destroyed → force release, no re-anchor):

- A MOVE grab late-latches the quad to the device pose; the thumbstick pushes/pulls distance
  (`stick.y`, clamped by the anchor) and resizes width (`stick.x`), scaled by `dt` — controllers
  only, since hands have no stick.
- A corner RESIZE is driven directly by the device world pose (the hand/controller motion is the
  resize), scaling about the pinned opposite corner. No stick verbs.

**Release** re-anchors into the persistent mode. Rather than re-anchoring from the perturbed
release-frame pose (which the release gesture — a fist opening, a controller swing — jerks: the
"lurch"), the machine keeps a per-hand ring of carried poses and picks a release pose from it:

- `openxr:grab_release_latency_ms` (default 100): re-anchor from the pose sampled that many ms before
  the release edge, clearing the release perturbation.
- `openxr:grab_release_velocity_reject` (default 3.0, a **ratio**, not m/s): if the quad's peak speed
  in the ~80 ms release window exceeds this multiple of its typical carry speed (a trimmed mean of
  the faster half of the preceding ~500 ms), re-anchor instead from the last carry-paced sample.
  Being relative, a uniformly fast flick (release speed ≈ carry speed) is *not* rewound. 0 disables
  it.

If the ring is empty (e.g. a single-frame grab) the release falls back to the plain release-frame
re-anchor. Release always ticks the haptic and queues `xrmonitorgrab …,0`.

### 7.1 Carry smoothing (1€ filter)

`openxr:grab_filter` (default on) low-passes a move-grab carry with a 1€ filter (Casiez et al.,
CHI '12) to remove tracking jitter, at the cost of about one frame of latency.
`openxr:grab_filter_scope` (`all` default, or `hands`) selects which devices it applies to;
`openxr:grab_filter_min_cutoff` (1.0) and `openxr:grab_filter_beta` (0.025) tune it. Filtering a
device drops its zero-latency device-space late-latch — the smoothed pose is submitted in
LOCAL_FLOOR instead — which is the source of the ~1 frame cost. `SXROneEuro`/`oneEuroStepPose` in
`XRMath.hpp` are an allocation-free, POD transcription of the reference algorithm (position filtered
per axis; quaternion components low-passed with hemisphere alignment, then renormalized).

### 7.2 Hand tracking (`XR_EXT_hand_interaction`)

When a hand's active interaction profile is `ext/hand_interaction_ext` (e.g. Quest 3 over WiVRn),
that hand is treated as hands rather than a controller. `openxr:hand_grab` (default `both`) selects
the grab gesture:

- **pinch** (thumb-index): the analog value is `pinch_ext/value`; the grab anchors to the stable
  `pinch_ext/pose`, so opening the pinch to release does not lurch the window.
- **grasp** (fist curl): the value is `grasp_ext/value`; the grab anchors to the wrist grip pose.
- **both**: either gesture; the stronger contributor decides the anchor (pinch on a tie).

Hands grab from the move-bar and corners only — never the content body — unless
`openxr:hand_grab_anywhere` (default `grasp`) permits the triggering gesture to body-grab: `grasp`
lets a fist grab the body while a pinch stays chrome-only (and keeps its click), `pinch` lets a
pinch body-grab (which then both clicks and grabs), `both` allows either, `none` forbids body-grab
entirely. The decision is keyed on the gesture that actually crossed the grab threshold this grab,
not on the `hand_grab` mode. Controllers are unaffected — they use `grab_anywhere`.

`hyprctl openxr status` reports each hand's input kind: `controllers`, or `hands` with the gesture
and whether the carry is filtered.

### 7.3 Haptics

A short tick (`xrApplyHapticFeedback`, 10 ms, amplitude 0.5, unspecified frequency) fires on grab
enter, grab release, and select press (not release). It is fire-and-forget from the frame thread; a
profile with no haptic binding (hands) simply returns an error that is ignored.

## 8. Frame → main event queue

Two event families cross the queue:

```cpp
enum class eXRInputEventType : uint8_t { MOTION_ABS, BUTTON, AXIS, FRAME };

struct SXRInputEvent {
    eXRInputEventType type;
    MONITORID         monitorID;   // MOTION_ABS / BUTTON target (-1 = hover clear)
    Vector2D          uv;          // MOTION_ABS: 0.0-1.0 content uv
    uint32_t          button;      // BUTTON: BTN_LEFT 0x110 / BTN_RIGHT 0x111
    bool              pressed;
    wl_pointer_axis   axis;        // AXIS
    double            axisDelta;   // AXIS
    uint32_t          timeMs;
};

enum class eXRStateEventType : uint8_t {
    SESSION_STATE,   // XrSessionState changed -> openxrsessionstate / openxractive
    USER_PRESENCE,   // XR_EXT_user_presence donned/doffed -> monitor plug gate
    GRAB,            // -> xrmonitorgrab
    ADAPTIVE,        // -> xrmonitordocked / xrmonitorundocked
    TRACKING,        // device-lock tracking gained/lost (logged)
    LAYER_REMOVED,   // internal removal-barrier ack (str = removed monitor name)
    SCHEDULE_FRAMES, // main thread must scheduleFrame() the visible XR monitors
};

struct SXRStateEvent {
    eXRStateEventType type;
    MONITORID         monitorID;
    int32_t           a;    // GRAB: 1/0; SESSION_STATE: manager-state value
    std::string       str;  // optional payload (e.g. monitor name)
};

using XRQueueItem = std::variant<SXRInputEvent, SXRStateEvent>;
```

Input and state events share one queue so that all socket2 emission stays on the main thread. This
page owns the `GRAB` events (`xrmonitorgrab <name>,0|1`) and the input events; the other state
types are produced here as the transport but consumed on behalf of other subsystems — session state
and idle/active by the session/config machinery, `USER_PRESENCE` and `SCHEDULE_FRAMES` by the
monitor-plug and frame-pacing paths, `ADAPTIVE` (`xrmonitordocked`/`xrmonitorundocked`) by the
anchoring page's adaptive anchoring, and `LAYER_REMOVED` internally by the monitor removal barrier.
The frame thread cannot call `CMonitor::scheduleFrame()` directly (aquamarine's idle-callback list
is not thread-safe), which is why pacing goes through `SCHEDULE_FRAMES`.

The transport is `CXRSPSCRing<XRQueueItem, 1024>` (`XRQueue.hpp`): a lock-free single-producer /
single-consumer ring, power-of-two capacity, `release`/`acquire` ordering, one slot reserved to
disambiguate full from empty. The producer (`COpenXRManager::enqueue`) drops `MOTION_ABS`/`AXIS` on
a full ring (they regenerate next frame) but treats `BUTTON`, `FRAME`, and every state event as
non-droppable, logging once if the ring ever overflows (1024 deep with per-dispatch drain does not
realistically fill). After each queued batch the producer writes the eventfd
(`eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)`). The main thread registers that fd on the Wayland event
loop; the callback reads the counter and drains the ring to empty, dispatching input events to the
synthetic pointer and state events to their handlers. The frame loop only emits a terminating
`FRAME` event (and thus a wakeup) when something was actually queued, so an idle session adds zero
wakeups.

## 9. `CXRPointerDevice` — the synthetic pointer

`CXRPointerDevice` is an `IPointer` subclass modeled on hyprtester's `CTestMouse`. It reports
`isVirtual() == false` (so per-device config `device[xr-pointer]{…}` behaves like a physical mouse)
and `aq() == nullptr` (no backend device, so `setupMouse`'s libinput branch is skipped). It is
created on session start when `openxr:pointer` (default true) is set, registered via
`g_pInputManager->newMouse()` (→ `setupMouse` → `attachPointer` + a destroy listener), and removed
on session stop or when `openxr:pointer` is toggled off (`destroy()` fires the destroy signal that
detaches it).

The queue drain maps each `SXRInputEvent` to the device's pointer signals:

| queue item | emission |
|---|---|
| `MOTION_ABS` | resolve the monitor by id (skip if it died in flight), set `m_boundOutput = monitor name`, then emit `motionAbsolute` with the content uv |
| `BUTTON` | emit `button` (pressed/released, `mouse = true`) |
| `AXIS` | emit `axis` (`source = CONTINUOUS`, `deltaDiscrete = 0`, `mouse = true`) |
| `FRAME` | emit `frame` |

Setting `m_boundOutput` to the hit monitor's name before each motion is the load-bearing detail:
`motionAbsolute` routes through the pointer manager to `onMouseWarp`, which maps the 0–1 absolute
coordinate onto the named output's box — so the cursor lands on the right virtual monitor with no
extra routing. Hover bookkeeping (current/last-hovered monitor) is kept up to date on the drain even
when the pointer device is absent (`openxr:pointer = 0`), so status and selection still reflect the
ray.

## 10. Selected-monitor resolution

Some dispatcher verbs need a target without a name (e.g. `xrmonitor destroy active`, or a
move/anchor with the target omitted). `COpenXRManager::resolveSelected()` resolves in order:

1. **Explicit selection** — the last `xrmonitor select <name|next|prev>`, if that monitor is alive
   (`next`/`prev` cycle live XR monitors in creation order). Cleared when the selected monitor is
   destroyed.
2. **Last ray-hovered XR monitor** — set from the ray hover on the main-thread drain, if alive.
3. **Focused monitor, if it is an XR monitor** — for when focus was warped there by a normal bind.

If all three fail the verb returns "no XR monitor selected". The last-hovered target (rule 2) is
this page's contribution; the full verb table lives with the configuration page.

## 11. Keyboard leg

There is **no synthetic keyboard**. The real keyboard already types into whatever XR monitor holds
focus through the ordinary focus path — a ray click focuses the window under the cursor like any
mouse click. Repositioning from the keyboard is done with normal Hyprland binds calling the
`xrmonitor` dispatcher (verb table on the configuration page; verb math on the anchoring page), for
example:

```ini
bind = SUPER, F9,           xrmonitor, create XR-1 2560x1440@90
bind = SUPER, bracketright, xrmonitor, select next
bind = SUPER ALT, left,     xrmonitor, move -0.1 0 0
bind = SUPER ALT, prior,    xrmonitor, distance -0.1
bind = SUPER ALT, home,     xrmonitor, center
bind = SUPER ALT, h,        xrmonitor, anchor active head
```

## 12. Idle

No extra code drives idle activity. Every listener installed by `CPointerManager::attachPointer` —
motion, motionAbsolute, button, axis — calls `PROTO::idle->onActivity()` after dispatching, and
`CXRPointerDevice` is attached through that exact path, so XR pointer input resets idle timers for
free, identically to a physical mouse. Idle **inhibition** while the session is FOCUSED
(`openxr:inhibit_idle`) is a separate mechanism owned by the configuration page.

## 13. Constants

Named in `XRInput.hpp`:

```cpp
XR_STICK_DEADZONE      = 0.1     // thumbstick deadzone
XR_SCROLL_NOTCH        = 15.0    // wl axis units per full deflection / frame
XR_UV_EPSILON          = 1e-4    // motion coalescing threshold
XR_GRAB_CONE_DEG       = 5.0     // grab entry cone forgiveness
XR_GRAB_PUSHPULL_SPEED = 2.0     // m/s of distance at full stick.y while grabbed
XR_GRAB_RESIZE_SPEED   = 1.0     // m/s of width at full stick.x
XR_QUEUE_CAP           = 1024    // frame->main ring capacity (power of two)
```

The haptic tick duration (10 ms) is an inline literal in `hapticTick`. Distance and width clamps
live with the anchoring page.
