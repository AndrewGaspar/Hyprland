# 03 — Anchoring: `CXRAnchor` + `XRMath.hpp`

Part of the OpenXR document set (`docs/openxr/`). This page describes the anchoring engine: the
pure-math pose solver that decides, every XR frame, where each virtual monitor's
`XrCompositionLayerQuad` sits and which `XrSpace` it references.

The engine covers four placement modes (world-fixed, head-leashed, body-leashed, controller-locked),
an *adaptive* decorator that lets a desk-fixed monitor follow the user and re-dock, the pose math
behind grabbing and releasing a monitor with a controller or hand, the keyboard/IPC repositioning
verbs, recentering, and the pose→text serialization used to persist a layout.

Scope boundary: the grab **state machine** — when a grab begins and ends, which gesture triggers it,
thumbstick push/pull, haptics, chrome rendering — belongs to the input document, which drives the
math here through a handful of calls. This page owns the grab **pose math**. The `xrmonitor`
keyword grammar, the dispatcher/hyprctl registration, and the config-variable definitions belong to
the configuration/IPC document; this page owns the math those verbs perform and the serialization
rules.

## 1. Files and the pure-math constraint

```
src/openxr/XRMath.hpp        Vec3 / Quat / SXRPose + helpers (header-only)
src/openxr/XRAnchor.hpp      CXRAnchor, SXRAnchorState, solve API
src/openxr/XRAnchor.cpp      implementation
```

`XRMath.hpp` and `XRAnchor.{hpp,cpp}` include no OpenXR header and are not guarded by `HAVE_OPENXR`
— unlike every other file in `src/openxr/`, which wraps its contents in `#ifdef HAVE_OPENXR`. All
solve code is pure math: poses in, and a pose plus a space *selection* (an enum, `eXRSpaceSelector`,
not an `XrSpace` handle) out. Two properties follow from this:

- **Testability.** The gtest target (`hyprland_gtests`) globs `tests/*.cpp` and links against
  `hyprland_lib`, so the anchor-math tests compile and run on machines without the `openxr` package.
  These are the only `src/openxr/` files that build unconditionally, together with the config parser
  `XRMonitorConfig.{hpp,cpp}` (also pure).
- **Determinism.** `solve()` takes `dt` and all poses as arguments — no clocks, no globals, no config
  lookups. Tuning values arrive in a struct (`SXRAnchorTuning`). The caller (`COpenXRManager`) reads
  config and passes it in.

Conversion helpers between `XrPosef` and `SXRPose` (trivially memberwise) live in the session-side
code, not in `XRMath.hpp`.

**Namespace.** These types live in `namespace OpenXR`, matching the rest of `src/openxr/`, so
`Vec3`, `Quat`, `SXRPose`, `CXRAnchor` and friends are `OpenXR::Vec3` etc. This is a plain named C++
namespace, unrelated to the OpenXR SDK headers, so it does not breach the "no OpenXR headers" rule.

**Threading.** A `CXRAnchor` is owned by its `CXRMonitorLayer` (see the virtual-monitors document).
`solve()` runs on the XR frame thread. The repositioning verbs and mode transitions run on the main
thread through `COpenXRManager`, which holds the same mutex that guards the per-frame layer snapshot.
`CXRAnchor` itself contains no locking. Everything the frame thread touches here is plain POD (poses,
floats) with zero reference-counted pointers, satisfying the frame-thread rule documented in
`XRMonitorLayer.hpp`.

## 2. Coordinate conventions and `XRMath.hpp`

### 2.1 Conventions

- OpenXR convention throughout: right-handed, **+Y up**, **−Z forward**, **+X right**, meters.
- All "world" poses are expressed in the `LOCAL_FLOOR` reference space (y = 0 at the floor). The
  session/graphics document covers how that space is obtained (native `XR_EXT_local_floor`, or the
  `openxr:floor_offset` fallback).
- A quad lies in the x–y plane of its pose, centered on the pose origin, visible from its **+Z**
  side. "The quad faces the user" means its local +Z points at the user's head.
- Quaternions are unit quaternions `{x, y, z, w}`, identity `{0,0,0,1}`. `qMul(a, b)` is the Hamilton
  product with `b` applied first: `qRotate(qMul(a,b), v) == qRotate(a, qRotate(b, v))`.
- Angles are radians internally. Degrees appear only at the user-facing edges (config, dispatcher
  args, serialization) and are converted immediately.

Hyprland's own math has only `Vector2D`, hence the 3D types here. They use `float` precision to match
`XrVector3f`/`XrQuaternionf`.

### 2.2 Types and helpers

`Vec3` carries the usual arithmetic operators plus `dot`, `cross`, `length`, `normalized`
(`normalized()` returns zero for a zero-length vector rather than dividing by zero). `Quat` is a bare
`{x,y,z,w}`. `SXRPose` is `{Vec3 pos; Quat rot}`.

The quaternion helpers are:

```
qMul(a,b)              Hamilton product, b applied first
qConjugate(q)          {-x,-y,-z,w}; == inverse for unit quats
qInverse(q)            alias of qConjugate (unit quats only)
qRotate(q,v)           v + 2w(u×v) + 2u×(u×v), u = (x,y,z)
qNormalize(q)
qSlerp(a,b,t)          shortest-arc (negate b if dot<0), nlerp fallback near-parallel
qFromAxisAngle(axis,rad)
qFromYaw(rad)          about +Y: {0, sin(rad/2), 0, cos(rad/2)}
qFromPitch(rad)        about +X: {sin(rad/2), 0, 0, cos(rad/2)}
qYawOf(q,fallback)     yaw about +Y such that yaw 0 faces −Z (see 2.4)
qAngleBetween(a,b)     2·acos(|dot|), clamped
lookAtNoRoll(from,to,fallback)   (see 2.3)
```

Pose helpers: `poseCompose(a, b)` composes `a ∘ b` (B's space nested inside A's), and
`poseInverse(p)` inverts. The defining relations, both required for the grab and recenter round-trips
below:

```
poseCompose(a,b) = { pos = a.pos + qRotate(a.rot, b.pos),  rot = qMul(a.rot, b.rot) }
poseInverse(p)   = { pos = −qRotate(qConjugate(p.rot), p.pos),  rot = qConjugate(p.rot) }
```

so `poseCompose(poseInverse(P), P)` is the identity.

### 2.3 `lookAtNoRoll`

Produces an orientation whose **+Z axis points from `from` toward `to`** (so the quad faces that
direction) and whose +X axis is horizontal (roll removed). It builds an orthonormal basis with
`z = normalize(to − from)`, `x = normalize(up × z)` (horizontal by construction), `y = z × x`, forms
the rotation matrix with those columns, and converts to a quaternion by Shepperd's method. When `z`
is near-vertical (`|up × z|` tiny) roll is undefined, and it returns the caller's `fallback` (the
previous orientation).

### 2.4 Yaw extraction

`qYawOf` defines yaw as rotation about world +Y with yaw 0 facing −Z. It projects the rotated forward
vector `f = qRotate(q, (0,0,−1))` onto the XZ plane and returns `atan2(−f.x, −f.z)`; positive yaw
turns left. When `f` is near ±Y (`sqrt(f.x²+f.z²) < 1e-4`) yaw is ill-conditioned and it returns the
supplied `fallback`. The *hysteresis* for sustained near-vertical gaze lives in `CXRAnchor` (§4.3),
not in the helper.

`XRMath.hpp` also carries pure helpers that belong to other subsystems but share the
no-OpenXR-headers property: the ray/quad intersection and chrome-region classification used by the
pointer path (input document), the 1€ filter used by the carry smoother (§5.4), and the geofence /
easing helpers used by adaptive anchoring (§6).

## 3. Anchor state and the solve API

### 3.1 Modes and persistent state

```cpp
enum eXRAnchorMode : uint8_t {
    XR_ANCHOR_LOCAL = 0, // fixed in LOCAL_FLOOR
    XR_ANCHOR_HEAD,      // head leash (view-space offset, spring + deadzone)
    XR_ANCHOR_BODY,      // body leash (yaw-only body frame)
    XR_ANCHOR_DEVICE,    // locked to a controller grip space
};
```

`eXRSpaceSelector` names which `XrSpace` a quad references: `XR_SPACE_LOCAL_FLOOR`, the left/right
controller grip spaces (`XR_SPACE_GRIP_*`), and the left/right hand pinch-pose spaces
(`XR_SPACE_PINCH_*`). `CXRMonitorLayer` maps the selector to a real handle. The pinch spaces are used
only by a pinch-anchored hand grab (§5); every non-grab path stays in `LOCAL_FLOOR` or a grip space.

`SXRAnchorState` is the persistent, serializable identity of a monitor's placement:

```cpp
struct SXRAnchorState {
    eXRAnchorMode mode;      // default LOCAL
    eXRHand       device;    // meaningful iff mode == DEVICE
    SXRPose       anchorPose; // meaning depends on mode (below)
    float         bodyHeight;  // BODY only: y of the body-frame origin (meters)
    float         widthMeters; // quad width in meters; seeded from the configured default size
    SXRAdaptiveConfig adaptive; // adaptive decorator (§6); only meaningful on LOCAL
};
```

`anchorPose` means: the quad pose in `LOCAL_FLOOR` for `LOCAL`; a view-space offset for `HEAD` (its
display orientation is recomputed each frame, §4.2); a yaw-only body-frame offset for `BODY`; the
grip-space offset for `DEVICE`. Quad **height is always derived**: `height = width · pxH / pxW`.

### 3.2 Tuning and solve I/O

`SXRAnchorTuning` is filled from config each frame and passed into `solve()`. The leash parameters
and their defaults:

| field | config key | default |
|---|---|---|
| `leashResponse` | `openxr:leash_response` | 0.35 s |
| `deadzoneAngleRad` | `openxr:leash_deadzone_angle` | 15° |
| `deadzoneDistance` | `openxr:leash_deadzone_distance` | 0.25 m |
| `bodyFollowHeight` | `openxr:body_leash_follow_height` | off |
| `defaultDistance` | `openxr:default_distance` | 1.5 m |

The struct also carries the adaptive thresholds (§6). All of these are re-read every frame, so they
tune live. Numeric config values are read directly on the frame thread (a reload can tear a number
but never crashes); the two *string* adaptive options are parsed to enums on the main thread and
published as atomics, because a string config value dangles across a reload.

`solve()` takes an `SXRSolveInput` — the `VIEW` pose in `LOCAL_FLOOR` at predicted display time, `dt`
(clamped to `[0, 0.1]` s), the optional left/right grip and pinch poses (present only when both the
position and orientation validity bits are set), the current pixel mode for aspect, and the carry
filter parameters — and returns an `SXRSolveResult`: the space selector, the quad pose expressed in
that space, the same pose composed into `LOCAL_FLOOR` (`worldPose`, used for hit tests and IPC), and
the width/height in meters. `CXRMonitorLayer` maps the selector to an `XrSpace` and converts `pose`
to `XrPosef`.

## 4. Per-mode solve

`solve()` dispatches in this order:

```
if grabbed:            grab override (§5)
else if adaptive:      adaptive decorator (§6)  — persistent mode is always LOCAL here
else switch mode:      LOCAL §4.1 | HEAD/BODY §4.2/4.3 | DEVICE §4.4
finally:               cache m_lastWorld = worldPose
```

`m_lastWorld` is the last composed world pose; it feeds grab capture, the tracking-loss fallback, hit
testing, and IPC dumps.

### 4.1 `LOCAL`

Zero per-frame work: the space is `LOCAL_FLOOR` and both `pose` and `worldPose` are `anchorPose`
verbatim.

### 4.2 `HEAD` — head leash

The stored offset `O` lives in view space; the raw position target is `T = poseCompose(view, O)`.
Position is filtered through a **deadzone latch** plus a **critically-damped spring**, so small head
motion does not drag the quad, but a real move is followed smoothly.

- *Deadzone.* Against the current smoothed position, two deviations are measured: the angular
  deviation of the head→quad direction and the positional deviation from the target. While *latched*
  the position is frozen. Once either deviation exceeds `deadzoneAngleRad` / `deadzoneDistance` the
  latch releases and the spring *chases*; when it settles within `XR_LEASH_SETTLE_POS` (0.01 m) it
  re-latches. This prevents perpetual micro-chasing.
- *Spring.* A critically-damped harmonic oscillator toward the (piecewise-constant) target, stepped
  by an exact closed-form integrator that is unconditionally stable for any `dt` (not forward Euler),
  applied per component:

  ```
  ω = 2 / leashResponse;   Δ = x − T;   k = v + ω·Δ;   E = e^(−ω·dt)
  x' = T + (Δ + k·dt)·E;   v' = (v − k·ω·dt)·E
  ```

  From rest this envelope is `(1 + ωt)e^{−ωt}` — a snappy, overshoot-free follow (≈41% residual after
  `leashResponse`, <5% after ≈2.4·`leashResponse`).

Orientation is **not** taken from `O.rot`. Every frame the quad is re-aimed at the head with
`lookAtNoRoll(pos, view.pos, previous)` so it can never go edge-on, and the result is low-passed by a
slerp whose factor `1 − e^(−dt/leashResponse)` is the exact discretization of a first-order filter.
The result space is `LOCAL_FLOOR`.

When the mode is entered the spring is seeded at the current world pose (or at `poseCompose(view, O)`
if there is no current pose yet) with zero velocity and the latch closed.

### 4.3 `BODY` — body leash

The body frame is a yaw-only frame at the head's XZ position. Its height is `bodyHeight` — captured
as the view y when the mode is entered — unless `bodyFollowHeight` is set, in which case it tracks the
live head height. `bodyHeight` is what keeps quads at desk height when the user sits or stands.

The frame's yaw comes from the head's horizontal facing, guarded by hysteresis so a near-vertical
gaze (looking straight up/down, where yaw is ill-conditioned) holds the last valid yaw: it stops
updating below `XR_BODY_YAW_HOLD` (0.15) of horizontal projection and resumes above
`XR_BODY_YAW_RESUME` (0.25).

The target `T = poseCompose(bodyFrame, O)` drives the same latch-and-spring machine as `HEAD`, but
with the deviations made distance-dominant: positional deviation is full 3D distance, angular
deviation is the yaw difference only (head pitch and small yaw wobble must not wake the quad).
Orientation is flattened to yaw-only (pitch and roll forced to 0) and slerp-smoothed. Result space is
`LOCAL_FLOOR`.

### 4.4 `DEVICE` — controller lock

The point of this mode is that the compositor **late-latches the pose**: the quad's `XrSpace` *is* the
controller grip space, so the runtime re-evaluates the controller pose at display time — zero added
latency and no per-frame solve on our side. When the grip is tracked, the space is the grip selector
and `pose` is the stored grip-space offset verbatim; `worldPose` is composed only for hit tests / IPC.

On tracking loss the quad is parked in the world at its last composed pose (`LOCAL_FLOOR`) until the
grip is valid again; if tracking was *never* valid it is placed at `defaultDistance` in front of the
current view. There is no smoothing or deadzone.

## 5. Grab interplay — pose math

The grab **state machine** (thresholds, which hand, gesture selection, thumbstick handling, haptics,
the `xrmonitorgrab` event) belongs to the input document. It drives `CXRAnchor` through the calls
below; all composition happens in `LOCAL_FLOOR`.

### 5.1 Begin, carry, release

`beginGrab(hand, deviceWorld, usePinch, handActive)` captures the quad's current *displayed* world
pose relative to the grabbing device: `m_grabOffset = poseCompose(poseInverse(deviceWorld),
m_lastWorld)`. Because `m_lastWorld` (not the raw anchor target) is used, grabbing a mid-flight
leashed quad picks it up exactly where it is rendered — no snap. `deviceWorld` is the wrist grip pose
for a controller or fist grasp, or the hand **pinch** pose when `usePinch` is set; the latter anchors
to the steadier thumb-index contact point. `handActive` marks a tracked-hand grab, which arms the
optional carry filter (§5.4).

While grabbed, `solve()` short-circuits: the layer behaves as device-locked to the grabbing device,
returning the grip-or-pinch space selector and `m_grabOffset` as the pose, so the runtime tracks the
device 1:1 with no added latency. For the two **user-facing modes** (`HEAD`/`BODY`), the offset's
*rotation* is refreshed each frame from the current poses (head: re-aimed at the viewer; body:
yaw-only facing) so the quad keeps facing the user while carried instead of staying rigid to the
wrist. Position stays a fixed device-space offset, so the zero-latency positional late-latch is
unchanged. `LOCAL`/`DEVICE` grabs stay fully rigid — carrying and tilting the panel like an object is
the intended metaphor.

`grabPushPull(delta)` and `grabResize(delta)` mutate the carried offset while grabbed. Push/pull
scales the grip→quad distance along its ray, clamped to `[XR_DISTANCE_MIN, XR_DISTANCE_MAX]` =
0.3–5.0 m. Resize scales `widthMeters`, clamped to `[XR_WIDTH_MIN, XR_WIDTH_MAX]` = 0.2–4.0 m.

Release re-anchors the carried world pose back into the persistent mode via `reanchorFromWorld`,
which is shared with the mode-transition path (§7). The conversions are the exact inverses of the
per-mode composes in §4:

| mode | conversion |
|---|---|
| `LOCAL` | `anchorPose = W` |
| `HEAD` | `anchorPose = poseCompose(poseInverse(view), W)`; spring reseeded at `W`, orientation re-aimed |
| `BODY` | recompute the body frame; `anchorPose = poseCompose(poseInverse(bodyFrame), W)`, then flatten rotation to yaw-only; spring reseeded |
| `DEVICE` | `anchorPose = poseCompose(poseInverse(grip), W)` against the anchor hand; if that grip is untracked at release, park via the loss path and recompute the offset on the first valid grip frame |

`beginGrab` immediately followed by `endGrab` with unchanged poses is a bit-exact round-trip for
every mode, because `W = grip ∘ (grip⁻¹ ∘ m_lastWorld) = m_lastWorld`.

### 5.2 Corner resize grab

A grab on a corner handle resizes rather than moves. It does **not** device-lock the quad
(`solve()` keeps running the persistent mode); instead it scales the *content* width while pinning
the diagonally-opposite corner (a visionOS-like resize). At begin it snapshots the pinned corner and
the content diagonal in world from the displayed pose; each frame it projects the grabbing device's
motion onto that fixed diagonal to derive the new width (aspect is fixed by the pixel mode), pins the
opposite corner exactly, and re-expresses the resized content pose through the same
`reanchorFromWorld`, so `HEAD`/`BODY`/`DEVICE` modes keep their offset semantics and the leash follows
the size change instead of fighting it. Orientation is held at the grab-start value for a stable
diagonal.

### 5.3 Release-latch ring — rejecting the release lurch

A squeeze/grasp/pinch release often swings the device pose exactly on the release frame — most
sharply a fist-open on hand tracking, but also a controller flick. Re-anchoring from that frame would
bake the swing into the persistent anchor and the window would lurch. `CXRInput` keeps a short
per-hand ring of the *carried* world pose (`SXRGrabRing`, POD, frame-thread-owned), and
`pickReleasePose` selects a clean release pose from it:

- **Latency rewind.** By default it returns the carried pose sampled `grab_release_latency_ms`
  (default 100 ms) earlier, interpolated between bracketing samples.
- **Velocity-outlier rejection.** `openxr:grab_release_velocity_reject` is a **ratio** K (default
  3.0; not a speed). Rejection triggers only when the peak speed inside the release window
  (`XR_GRAB_RELEASE_WINDOW_MS` = 80 ms) is an outlier relative to the preceding carry — greater than
  K× the *typical* carry speed and above an absolute floor (`XR_GRAB_CARRY_SPEED_FLOOR` = 0.05 m/s).
  When it fires, the release walks back to the last carry-paced ("calm") sample instead of the
  latency point. "Typical" is a lower-trimmed mean (the mean of the faster half of the carry samples),
  not a median: a flick that starts from rest keeps stationary just-grabbed samples in the ring,
  which would drag a median toward zero and misclassify the flick. A uniformly fast carry has
  peak ≈ carry (ratio ≈ 1) and is therefore *not* rewound — a deliberate fast flick is preserved,
  while a calm carry with a jerk at the very end is rewound past the jerk. The rewind is capped at
  `XR_GRAB_MAX_REWIND_MS` = 500 ms.

The corner-resize path uses the same latched sample for its final size, so a release jerk cannot
perturb the size either. This ring math is pure and gtest-covered.

### 5.4 1€ carry filter

An optional 1€ low-pass (Casiez et al., CHI '12) smooths the carried pose. Its two parameters are the
minimum cutoff (`openxr:grab_filter_min_cutoff`) and beta (`openxr:grab_filter_beta`), and
`openxr:grab_filter_scope` selects whether it applies to hand grabs only or to controllers too.
Filtering means the carried pose is no longer a rigid device-space offset, so the filtered path drops
the runtime's zero-latency late-latch and submits the smoothed world pose in `LOCAL_FLOOR` instead —
a deliberate trade of roughly one frame of latency for reduced jitter. The filter state resets at each
`beginGrab`, and `m_lastWorld` becomes the filtered pose so the release ring records what the user
actually saw. The 1€ implementation is a faithful, allocation-free transcription of the reference
algorithm and is gtest-covered against a direct port.

## 6. Adaptive anchoring

Adaptive anchoring is a **decorator on an `anchor:local` desk pose**, not a fifth mode. The persisted
mode stays `LOCAL` (the desk pose is the persistent identity); a small config plus a runtime phase
machine on `CXRAnchor` lets the monitor pick itself up and follow the user when they walk away from
the desk, then re-dock when they return, with an eased pose blend between the two. When enabled,
`solve()` runs the decorator (`adaptiveStep`) before the mode switch and always submits a
`LOCAL_FLOOR` world pose.

### 6.1 Geofence and phases

The "desk seat" is the head position captured while docked. Each frame the head's distance from the
seat is measured — **horizontal (XZ) by default**, so standing up does not count as walking away;
full 3D when `openxr:adaptive_use_height` is set. The phase machine (`docked → undocking → roaming →
redocking → docked`) is driven by a hysteretic geofence with dwell timers:

- Undock when the head is beyond `openxr:adaptive_leave_radius` (R_out, default 1.5 m) continuously
  for `openxr:adaptive_leave_dwell_ms` (default 400 ms).
- Re-dock when it returns within `openxr:adaptive_return_radius` (R_in, default 1.0 m, required to be
  < R_out for a hysteresis dead band) continuously for `openxr:adaptive_return_dwell_ms` (default
  800 ms).

A single spurious sample resets a dwell timer, so it cannot flap. Auto-redock is additionally gated on
the head having actually crossed R_out since the last dock, so a manual or forced undock while still
seated stays roaming instead of snapping straight back. The undock↔redock transitions can reverse
mid-flight if the user turns around, keeping their eased progress.

### 6.2 Transition, roam mode, and offset

The dock↔roam handoff is an eased pose blend over `openxr:adaptive_transition_ms` (default 700 ms),
with easing `openxr:adaptive_transition_ease` (`smoothstep` default, or `linear` / `ease_out`). While
roaming, the monitor head- or body-leashes through the same `solveLeash` used by the normal leash
modes (a monitor is never docked and roaming at once, so they share one spring); the roam mode is
`openxr:adaptive_roam_mode` (`body` default, or `head`). The roam offset is the configured
`roam_offset` if set, else — when `openxr:adaptive_carry_offset` is on — the offset captured from the
desk pose at the moment of undock, else a comfortable default straight ahead at `defaultDistance`.

A grab suspends the geofence (the grab override returns before the decorator). A mid-flight transition
is settled to its nearer endpoint at grab-begin so the release resolves into a stable docked/roaming
state; a release while roaming re-anchors into the roam frame (updating the runtime roam offset), not
the desk pose. `dock here` redefines the desk pose to the current displayed pose.

### 6.3 Per-monitor config, verbs, events

The `xrmonitor` line may carry adaptive tokens that override the globals, valid on `anchor:local`
only: `adaptive:on|off`, `roam:head|body`, `roam_offset:x,y,z`, `roam_yaw:deg`, `leave:R`,
`return:R`, `carry:on|off`. (The parser rejects a `return` ≥ `leave`.) Dispatcher/hyprctl verbs
control it live: `adaptive on|off|toggle`, `undock`, `dock` (optionally `dock here`), and
`roam head|body`. All require the target monitor to be `anchor:local`. The frame thread emits an
`xrmonitorundocked` / `xrmonitordocked` event on the terminal roaming/docked edge. `hyprctl openxr
status` reports the adaptive phase, roam mode, geofence distance, and transition parameter; while
roaming or transitioning it reports the live follow pose rather than the desk pose.

## 7. Repositioning verbs and mode transitions

The dispatcher exposes `xrmonitor move|rotate|scale|distance|center` and `xrmonitor anchor <target>
<mode>`. Registration, target-monitor resolution, and argument parsing belong to the configuration/IPC
and input documents; this page owns the math. Each verb captures a copy of the most recent
frame-thread solve context (`SXRVerbContext`: last view pose plus validity and grip poses) so it never
blocks the frame thread, then mutates the anchor on the main thread under the layer mutex. Angles
arrive in degrees and are converted at the boundary. A verb returns an error (no mutation) when the
data it needs is missing: `move`/`distance`/`center` need a valid view; `DEVICE`-mode conversions need
the anchor hand's grip; `scale` and `LOCAL`/`HEAD`/`BODY` `rotate` never need tracking.

- **`move dx dy dz`** — view-relative translation in meters (`dz > 0` is away from the face). The
  world delta `qRotate(view.rot, (dx, dy, −dz))` is applied per mode: added to the world pose for
  `LOCAL`, to the view-space offset for `HEAD`, to the body-frame offset for `BODY`, to the grip-space
  offset for `DEVICE`. Leashed modes set the chase latch so the quad glides to the new target.

- **`rotate dyaw [dpitch]`** — degrees, with a deliberate per-mode asymmetry. `LOCAL`/`DEVICE` rotate
  *in place* (yaw pre-multiplied in the parent frame, pitch post-multiplied about the quad's own X).
  `HEAD` *orbits* the offset around the head (its display orientation is lookAt-driven, so rotating in
  place would be meaningless). `BODY` orbits around the body axis and turns the stored yaw-only facing
  with it. Pitch is clamped to ±85° at the dispatcher.

- **`scale <f | ±d>`** — a bare number is a multiplicative factor, an explicitly signed value is an
  additive delta in meters; width is clamped to 0.2–4.0 m and height follows from the aspect.

- **`distance <±m>`** — slides the quad along the view→quad ray, clamped to 0.3–5.0 m, orientation
  unchanged (requires a current pose).

- **`center`** — re-places the quad at `defaultDistance` straight ahead of the view, facing the head.
  For `HEAD` it does not warp the spring but sets the chase latch so it glides.

**`xrmonitor anchor <target> <mode>`** changes the anchor mode seamlessly — the quad must not move.
With an explicit `offset:x,y,z` argument it re-seeds the state directly at that offset; otherwise it
takes the current displayed world pose (or a `center` placement if none) and runs the same
`reanchorFromWorld` conversion table as grab release (§5.1) for the new mode, resetting the solver
runtime state. `head`/`body` conversions need a valid view; `device:left|right` needs that grip (if
untracked, the quad is parked until tracking returns). A mode change emits an `xrmonitoranchor` event.

## 8. Recentering

### 8.1 Reference-space change

When the runtime issues `XrEventDataReferenceSpaceChangePending` for the floor space, the session
event pump forwards the new-origin pose `M` (the new origin expressed in the previous space), and the
frame thread calls `onReferenceSpaceChanged(M)` on every anchor between frames. A pose in old
coordinates becomes `poseCompose(poseInverse(M), P_old)` in new coordinates. Per mode:

- **`LOCAL`** re-derives `anchorPose` so the quad stays physically where it is across the recenter.
- **`HEAD` / `DEVICE`** offsets are relative to view/grip spaces, which move with the user, so they
  are unaffected.
- **`BODY`** leaves the offset alone but shifts the stored `bodyHeight` by the new origin's y.

In every case the cached solver state (`m_lastWorld`, spring position/velocity, smoothed rotation)
and the adaptive seat and any frozen transition pose are re-expressed too, so there is no one-frame
pop.

#### When the runtime withholds the delta

`poseValid` is allowed to be false, and **monado always sets it false** — `u_space_overseer.c` pushes
`pose_valid = false` with an identity pose even though `recenter_local_spaces` has just computed the
exact delta (research/22 §4.3). So on WiVRn this is not an edge case: it is *every* recenter, and
every re-don, since the Quest re-derives `LOCAL_FLOOR` when you put it back on.

Leaving the stored poses untouched there — the old, conservative behavior — is what produced the
"monitors thrown in a random direction across the room" report. One live session logged the latched
head frame moving **8.25 m and ~155°** across a single mid-session recenter with nothing applied to
any anchor.

The head is the one physical object observable on both sides of the swap, and it does not move while
the origin does, so the frame loop reconstructs what the runtime refused to send
(`solveReferenceSpaceChangeFromHead`, `XRMath.hpp`):

```
headOld = M ∘ headNew   ⇒   M = headOld ∘ inv(headNew)
```

`headOld` is the last head pose located *before* `pollEvents` saw the event; `headNew` is the locate
on the very next frame. Only the **yaw** of each is used — both frames are gravity-aligned, so the
true delta is 4-DoF, and the head's real pitch/roll change between the two samples would otherwise
tilt the whole monitor group. The reconstruction runs before the solve, so the corrected placement
is what the frame actually renders; a delta that comes out as the identity is skipped rather than
warping every spring for nothing.

`xrRecenterFix` picks between three answers:

| condition | action |
|---|---|
| `poseValid` | apply `poseInPreviousSpace` verbatim |
| no pose, head sample ≤ `XR_RECENTER_HEAD_MAX_AGE_NS` (0.5 s) old | reconstruct `M` from the head pair — monitors stay where they are **in the room** |
| no pose, no head sample that recent | re-seat the group to the head (§8.2), gated on `openxr:recenter_on_plug` |

The third row is the doff case: tracking did not straddle the change because the headset was off, so
nothing observed the old frame and the wearer may not even be standing where they were. Re-seating is
the same rigid, arrangement-preserving operation the first plug of a session performs, and it asks
the same permission. With `recenter_on_plug = false` the anchors are left alone and a WARN says so —
that is a deliberate "don't move my monitors" choice, and its cost is that the coordinates are now
expressed in a frame that no longer exists.

### 8.2 Recenter on plug

Separately, `openxr:recenter_on_plug` re-seats `anchor:local` monitors relative to the head on the
**first don of a session**. Under a boundaryless/standby runtime the `LOCAL_FLOOR` origin is arbitrary,
so a monitor declared at e.g. `pos:0,1.5,-1.5` would land wherever that origin happens to be, often
far from the user. On the first plug the main thread arms the frame thread, which on its next
valid-view frame calls `recenterLocalToHead(view, seat)` on each local monitor. This reinterprets the
given offset as head-relative: it plants the whole rig (position and facing) in a yaw-only frame at
the current head's floor XZ (`xrHeadFrame`), preserving the configured height and distance. Passing
the same view to every monitor transforms the group rigidly, so the relative arrangement is preserved.
It warps (no glide), and for an adaptive monitor it re-docks the desk seat at the current head. A
re-plug after a brief doff does not re-arm — the first-don placement is kept.

Which offset gets planted is §8.3.

### 8.3 Cross-session restore — what "seat" holds

§8.1 fixed recentering *within* a session. Across a **session restart** (a `wivrn-server` restart, the
compositor recycling its XR session) there is no delta to reconstruct at all: the new session's
`LOCAL_FLOOR` is simply a different frame, and every stored `LOCAL` coordinate is a number about a
space that no longer exists. §8.2's re-seat is the answer to that — but only if the offset it plants
means "relative to the wearer".

For a config-declared monitor it does by construction: `pos:0,1.5,-1.5` on an `xrmonitor` line is a
sentence about the user, not about the runtime's origin. That is why declared monitors always came
back correctly.

A monitor created at runtime (`hyprctl openxr create XR-3`) has no such declaration. What
`createXRMonitor` stores as its "declared" anchor is the pose `applyCenter` derived from wherever the
head was standing at the time — a `LOCAL_FLOOR` world pose. Re-seating from *that* composes a dead
frame's coordinates into the head frame, and the monitor lands as far away as the two origins happen
to differ. Reported live 2026-08-16 as monitors "spun way off, outside my house"; the log arithmetic:
session one latched at eye `[4.23, 1.04, 5.75]`, XR-3/XR-4 were created in that frame, a mid-session
recenter then moved the origin (`reconstructed a 7.13m / 14.7 deg frame change from the head`) which
re-expressed the live anchors but not the frozen declared copies, and session two re-seated the ad-hoc
monitors from those 7-metre stale offsets.

The durable form of a placement is the same pose named against the **wearer** instead of the origin:

```
offset = inv(xrHeadFrame(head)) ∘ anchorPose      // xrPoseInHeadFrame, XRMath.hpp
```

That is reference-space independent — when the origin moves, the head and the monitor move with it,
and the offset does not change (gtest: `CaptureIsReferenceSpaceIndependent`). It is the exact inverse
of the composition §8.2 performs, so the two round-trip.

**Capture.** The frame loop re-derives each `anchor:local` monitor's offset into
`CXRMonitorLayer::m_restoreOffset` every frame, immediately after the re-seat consumption (so a first
plug reads the freshly seated pose rather than clobbering a good offset with the coordinates it is in
the middle of replacing). It is gated on `m_restoreCapture` — plugged **and** wearing — because frames
keep arriving after a doff from a headset lying on a desk, and remembering *that* arrangement would be
worse than the bug. `publishRestoreCapture()` folds the plug state and the presence edge; on a runtime
without `XR_EXT_user_presence` the plug state is the whole gate. Capture is also skipped while an
adaptive monitor is anything but `DOCKED`: its `anchorPose` is the saved desk pose while the user has
walked away from it, so measuring that against their current head would remember the walk.

**Restore.** `xrReseatSource(mode, restoreValid)` picks:

| condition | offset planted |
|---|---|
| `LOCAL` with a captured offset | the placement the user left, replanted rigidly around the current head |
| `LOCAL`, never placed under tracking | the declared/creation-time rig (pre-existing behavior) |
| `HEAD` / `BODY` / `DEVICE` | n/a — the re-seat is a no-op, these ride the user already |

The offsets live on the layer, which outlives the XR session in-process, so the constellation survives
a session restart. **Limitation:** they do not survive a compositor restart. `hyprctl openxr layout`
remains the way to make a layout permanent.

**A grab-moved declared monitor keeps its moved pose** across a session restart, rather than snapping
back to its `xrmonitor` line. This page had been silent on the question; the choice matches the
within-session ladder (§8.1 holds a moved monitor where the user put it) and the reload behavior
(a config reload does not clobber live geometry). Editing the `pos:` in the config still wins — reload
reconciliation compares against `m_declaredAnchor` and re-applies a changed declaration.

**Degenerate cases** all fall through to the declared rig, which is what shipped before: a session that
was never donned, a session with no head sample by the time it ended, and a monitor created while the
headset was off (its untracked default is `(0, 1.4, -default_distance)`, read as head-relative, so it
lands in front of whoever plugs in). `openxr create` with a caller-supplied `pos:` is deliberately left
uncaptured at creation for the same reason — an explicit `pos:` is a declared rig, and measuring where
it currently sits would make a monitor the user cannot see permanently unreachable.

`hyprctl openxr status` reports this per monitor (`restore [x, y, z] (head-relative)`, or `restore
none`), so "will my room come back" is answerable before restarting anything.

## 9. Layout persistence — `hyprctl openxr layout`

`hyprctl openxr layout` walks the live XR monitors and prints paste-ready `xrmonitor = …` lines the
user can drop into their config. The keyword grammar is owned by the configuration/IPC document; this
page owns the pose→text rules (`serializeXRMonitorLine`):

- `pos:` / `offset:` is `anchorPose.pos` in meters at 3 decimals, meaning per mode as stored (§3.1):
  world for `local`, view offset for `head`, body offset for `body`, grip offset for `device`. For an
  `anchor:local` monitor the line carries the anchor's **live solved world pose** (`lastWorld()`), so
  a dump mid-grab reproduces what the user currently sees; an *adaptive* monitor always serializes its
  saved desk pose, never the live roam pose, so a save-while-roaming round-trips to the persistent
  identity. `head`/`body`/`device` lines carry the persistent offset, which is the config
  representation for those modes.
- Rotation is serialized as **yaw/pitch degrees only**, derived from the forward vector, at 1 decimal.
  `head` prints no rotation (its display orientation is lookAt-driven); `body` prints `yaw:` only
  (pitch/roll forced to 0); `local`/`device` print `yaw:` and `pitch:` (pitch omitted when
  `|pitch| < 0.05°`). Deserialization reconstructs `rot = qMul(qFromYaw(yaw), qFromPitch(pitch))`,
  recovering yaw and pitch exactly.
- **Roll is not representable** in the serialization. A grabbed `local`/`device` quad can pick up roll,
  and serializing drops it (head/body quads never carry roll by construction).
- `size:` is `widthMeters` at 2 decimals; height is never serialized (it is derived from the aspect).
- `bodyHeight` is not serialized; on config load it is captured from the first tracked view pose, so a
  saved body layout re-anchors at the height of first wear.
- Adaptive tokens are appended when the decorator is enabled: `adaptive:on`, then `roam:` only if the
  monitor overrode the global mode, `roam_offset:`/`roam_yaw:` if a roam offset was set, and
  `leave:`/`return:`/`carry:` for any per-monitor overrides.

## 10. Constants and tuning

The single constant block in `XRAnchor.hpp`:

```cpp
XR_LEASH_SETTLE_POS   = 0.01   // m, re-latch threshold (§4.2)
XR_BODY_YAW_HOLD      = 0.15   // horiz-projection below which body yaw holds (§4.3)
XR_BODY_YAW_RESUME    = 0.25   // …and above which it resumes (hysteresis)
XR_WIDTH_MIN/MAX      = 0.2 / 4.0   // m (scale / resize)
XR_DISTANCE_MIN/MAX   = 0.3 / 5.0   // m (push-pull / distance)
XR_SOLVE_DT_MAX       = 0.1    // s, dt clamp
XR_GRAB_MAX_REWIND_MS      = 500  // release-rewind cap (§5.3)
XR_GRAB_RELEASE_WINDOW_MS  = 80   // "release" window for the outlier test
XR_GRAB_CARRY_SPEED_FLOOR  = 0.05 // m/s, ratio floor + absolute gate
XR_GRAB_CALM_MARGIN        = 1.5  // ×carry pace counted as pre-jerk "calm"
```

The config-derived tuning (`SXRAnchorTuning`, §3.2) and the grab-release / carry-filter parameters
(§5.3, §5.4) are defined in the configuration/IPC document. The pure-math helpers in this engine are
exercised headlessly by the gtest suite described in the testing document.
