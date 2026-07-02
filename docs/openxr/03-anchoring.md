# 03 — Anchoring: `CXRAnchor` + `XRMath.hpp`

Part of the HypXRland design doc set (`docs/openxr/`). This document specifies the anchoring
engine: the pure-math pose solver that decides, every XR frame, where each virtual monitor's
`XrCompositionLayerQuad` goes and which `XrSpace` it references.

Scope: pose math and anchor state. The grab **state machine** (when a grab begins/ends, thumbstick
push/pull, haptics) is owned by `04-input.md`; this doc owns the grab **pose math**. The `xrmonitor`
config-keyword grammar, the dispatcher argument parsing, and all IPC tables are owned by
`05-ipc-config.md`; this doc owns the math those verbs perform and the pose→text serialization
rules.

## 0. Files, hard design constraint, testability

```
src/openxr/XRMath.hpp        Vec3 / Quat / SXRPose + helpers (header-only)
src/openxr/XRAnchor.hpp      CXRAnchor, SXRAnchorState, solve API
src/openxr/XRAnchor.cpp      implementation
tests/xr/anchor_math.cpp     gtest unit tests (section 9)
```

**Hard constraint: `XRMath.hpp` and `XRAnchor.{hpp,cpp}` must not include any OpenXR header and
must not be guarded by `HAVE_OPENXR`.** All solve code is pure math: poses in, pose + space
*selection* (an enum, not an `XrSpace` handle) out. Rationale:

1. Unit tests: `CMakeLists.txt:688` does `file(GLOB_RECURSE TESTFILES "tests/*.cpp")` into the
   always-built `hyprland_gtests` target (linked against `hyprland_lib`). `tests/xr/anchor_math.cpp`
   is therefore picked up automatically and must compile and run on CI machines **without** the
   `openxr` package installed. Every other file in `src/openxr/` wraps its contents in
   `#ifdef HAVE_OPENXR`; these two do not.
2. Determinism: the solver takes `dt` and all poses as arguments — no clocks, no globals, no config
   lookups (tuning values arrive in a struct, section 2.3).

Conversion helpers `XrPosef ↔ SXRPose` (trivially memberwise: `XrVector3f{x,y,z}`,
`XrQuaternionf{x,y,z,w}`) live in the session-side code (`src/openxr/XRSession.hpp`), **not** in
`XRMath.hpp`.

Threading: `CXRAnchor` instances are owned by their `CXRMonitorLayer` (see `02-virtual-monitors.md`).
`solve()` runs on the XR frame thread. Verb/config mutations (sections 5–7) run on the main thread
and go through `COpenXRManager`, which takes the same mutex that guards the per-frame layer
snapshot (see `00-overview.md`, thread model). `CXRAnchor` itself contains no locking.

## 1. Coordinate conventions and `XRMath.hpp`

### 1.1 Conventions

- OpenXR convention throughout: **right-handed**, **+Y up**, **−Z forward**, **+X right**, meters.
- All "world" poses are expressed in the `LOCAL_FLOOR` reference space (y = 0 at the floor). See
  `01-session-graphics.md` for how that space is created (native `XR_EXT_local_floor` or the
  `openxr:floor_offset` fallback).
- Quads: an `XrCompositionLayerQuad` lies in the x–y plane of its pose, centered on the pose
  origin, visible from the **+Z** side. Hence "the quad faces the user" ⇔ the quad's local +Z axis
  points at the user's head.
- Quaternions are unit quaternions `{x, y, z, w}`, identity `{0,0,0,1}`. `rotate(a*b, v) ==
  rotate(a, rotate(b, v))` (i.e. `b` applied first).
- Angles are radians internally. Degrees appear only at the user-facing edges (config, dispatcher
  args, serialization) and are converted immediately.

Hyprland only has `Vector2D` (`src/helpers/math/Math.hpp`), hence the new 3D types. `float`
precision (matches `XrVector3f`/`XrQuaternionf`).

**Implementation note (as built, WP13 reconciliation):** unlike `Vector2D`, these types do not
live at global scope — they live inside `namespace OpenXR { ... }`, matching the rest of
`src/openxr/` (`XRMonitorConfig.hpp` etc. are likewise in that namespace). This is a plain named
C++ namespace, not related to the OpenXR SDK headers, so it does not violate the "no OpenXR
headers" constraint above. All type names below (`Vec3`, `Quat`, `SXRPose`, ...) should be read as
`OpenXR::Vec3`, `OpenXR::Quat`, `OpenXR::SXRPose`, etc.

### 1.2 `XRMath.hpp` — required contents

```cpp
#pragma once
#include <cmath>

struct Vec3 {
    float x = 0.f, y = 0.f, z = 0.f;
    // member/free operators: + - unary- *scalar /scalar += -= *=
    float        dot(const Vec3& o) const;   // x*o.x + y*o.y + z*o.z
    Vec3         cross(const Vec3& o) const; // {y*o.z - z*o.y, z*o.x - x*o.z, x*o.y - y*o.x}
    float        length() const;             // sqrt(dot(*this))
    Vec3         normalized() const;         // *this / length(); caller guards length()==0
};

struct Quat {
    float x = 0.f, y = 0.f, z = 0.f, w = 1.f; // identity by default
};

struct SXRPose {
    Vec3 pos;
    Quat rot;
};

// ---- quaternion helpers (free functions) ----
Quat  qMul(const Quat& a, const Quat& b);          // Hamilton product, b applied first
Quat  qConjugate(const Quat& q);                   // {-x,-y,-z,w}; == inverse for unit quats
Quat  qInverse(const Quat& q);                     // alias of qConjugate (unit quats only)
Vec3  qRotate(const Quat& q, const Vec3& v);       // q v q*
Quat  qNormalize(const Quat& q);
Quat  qSlerp(const Quat& a, const Quat& b, float t); // shortest-arc (negate b if dot(a,b) < 0)
Quat  qFromAxisAngle(const Vec3& axisNormalized, float rad);
Quat  qFromYaw(float rad);                         // about +Y: {0, sin(rad/2), 0, cos(rad/2)}
Quat  qFromPitch(float rad);                       // about +X: {sin(rad/2), 0, 0, cos(rad/2)}
float qYawOf(const Quat& q, float fallback);       // see 1.5
float qAngleBetween(const Quat& a, const Quat& b); // 2*acos(|dot(a,b)|), clamped
Quat  lookAtNoRoll(const Vec3& from, const Vec3& to, const Quat& fallback); // see 1.4

// ---- pose helpers ----
SXRPose poseCompose(const SXRPose& a, const SXRPose& b); // a ∘ b, see 1.3
SXRPose poseInverse(const SXRPose& p);
```

Formulas the implementer must use:

```
qMul(a, b):
  w = a.w*b.w − a.x*b.x − a.y*b.y − a.z*b.z
  x = a.w*b.x + a.x*b.w + a.y*b.z − a.z*b.y
  y = a.w*b.y − a.x*b.z + a.y*b.w + a.z*b.x
  z = a.w*b.z + a.x*b.y − a.y*b.x + a.z*b.w

qRotate(q, v):                       // v' = v + 2w(u×v) + 2u×(u×v), u = (q.x,q.y,q.z)
  u = {q.x, q.y, q.z}
  t = 2 * u.cross(v)
  return v + q.w * t + u.cross(t)

qSlerp(a, b, t):
  d = ax*bx + ay*by + az*bz + aw*bw
  if d < 0: b = −b; d = −d                 // shortest arc
  if d > 0.9995: return qNormalize(lerp(a, b, t))   // nlerp fallback near-parallel
  th = acos(d)
  return (sin((1−t)th)/sin(th))·a + (sin(t·th)/sin(th))·b
```

### 1.3 Pose composition — derivation

A pose `P = {p, R}` maps a point `x` from its local space to its parent space:
`world(x) = qRotate(R, x) + p`.

Composition `A ∘ B` (B's space nested inside A's space):

```
(A∘B)(x) = A(B(x)) = R_A (R_B x + p_B) + p_A = (R_A R_B) x + (R_A p_B + p_A)

poseCompose(a, b) = { .pos = a.pos + qRotate(a.rot, b.pos),
                      .rot = qMul(a.rot, b.rot) }
```

Inverse: solve `y = R x + p` for `x`: `x = R⁻¹ y − R⁻¹ p`, so

```
poseInverse(p) = { .pos = −qRotate(qConjugate(p.rot), p.pos),
                   .rot = qConjugate(p.rot) }
```

Check: `poseCompose(poseInverse(P), P) == identity`. This is a required unit test (section 9).

### 1.4 `lookAtNoRoll` — derivation

Goal: an orientation whose **+Z axis points from `from` toward `to`** (quads are visible from +Z)
and whose +X axis is horizontal (roll removed).

```
z = (to − from).normalized()
up = (0, 1, 0)
if up.cross(z).length() < 1e-3:      // z nearly vertical → roll undefined
    return fallback                   // caller passes previous orientation
x = up.cross(z).normalized()          // horizontal by construction ⇒ zero roll
y = z.cross(x)                        // completes right-handed basis
```

Build the rotation matrix with **columns** `x, y, z` (they are the images of the basis vectors)
and convert to a quaternion (Shepperd's method):

```
m00=x.x m01=y.x m02=z.x
m10=x.y m11=y.y m12=z.y
m20=x.z m21=y.z m22=z.z
tr = m00 + m11 + m22
if tr > 0:
    s = 2*sqrt(tr + 1);        w = s/4;          qx=(m21−m12)/s; qy=(m02−m20)/s; qz=(m10−m01)/s
elif m00 > m11 and m00 > m22:
    s = 2*sqrt(1 + m00 − m11 − m22); qx = s/4;   w =(m21−m12)/s; qy=(m01+m10)/s; qz=(m02+m20)/s
elif m11 > m22:
    s = 2*sqrt(1 + m11 − m00 − m22); qy = s/4;   w =(m02−m20)/s; qx=(m01+m10)/s; qz=(m12+m21)/s
else:
    s = 2*sqrt(1 + m22 − m00 − m11); qz = s/4;   w =(m10−m01)/s; qx=(m02+m20)/s; qy=(m12+m21)/s
```

### 1.5 Yaw extraction — derivation

"Yaw" = rotation about world +Y such that yaw 0 faces −Z. Take the rotated forward vector and
project onto the XZ plane:

```
f = qRotate(q, (0, 0, −1))
yaw = atan2(−f.x, −f.z)
```

Sanity check with `R_y(θ) = [[cosθ,0,sinθ],[0,1,0],[−sinθ,0,cosθ]]`:
`R_y(θ)·(0,0,−1) = (−sinθ, 0, −cosθ)` ⇒ `atan2(sinθ, cosθ) = θ`. ✓
Positive yaw turns left (−Z toward −X), consistent with right-handed +Y rotation.

Degeneracy: when `f` is near ±Y (looking straight up/down), `f.x² + f.z²` → 0 and yaw is
ill-conditioned. `qYawOf(q, fallback)` returns `fallback` when `sqrt(f.x² + f.z²) < 1e-4`. The
*hysteresis* for this case lives in `CXRAnchor` (section 3.3), not in the math helper.

## 2. Anchor state

### 2.1 Enums and per-layer persistent state

```cpp
// XRAnchor.hpp
enum eXRAnchorMode : uint8_t {
    XR_ANCHOR_LOCAL = 0, // fixed in LOCAL_FLOOR
    XR_ANCHOR_HEAD,      // head leash (view-space offset, spring + deadzone)
    XR_ANCHOR_BODY,      // body leash (yaw-only body frame)
    XR_ANCHOR_DEVICE,    // locked to a controller grip space
};

enum eXRHand : uint8_t {
    XR_HAND_LEFT = 0,
    XR_HAND_RIGHT,
};

// which XrSpace the quad layer must reference; mapped to real handles by CXRMonitorLayer
enum eXRSpaceSelector : uint8_t {
    XR_SPACE_LOCAL_FLOOR = 0,
    XR_SPACE_GRIP_LEFT,   // CXRInput's left grip XrActionSpace
    XR_SPACE_GRIP_RIGHT,  // CXRInput's right grip XrActionSpace
};

struct SXRAnchorState {
    eXRAnchorMode mode   = XR_ANCHOR_LOCAL;
    eXRHand       device = XR_HAND_LEFT; // meaningful iff mode == XR_ANCHOR_DEVICE

    // Meaning depends on mode:
    //   LOCAL : quad pose in LOCAL_FLOOR (world)
    //   HEAD  : offset in VIEW space (pos = where the quad sits relative to the head;
    //           rot stored for round-tripping but display orientation is lookAt-driven, §3.2)
    //   BODY  : offset in the yaw-only body frame (§3.3)
    //   DEVICE: offset in the grip space of `device`
    SXRPose anchorPose;

    float bodyHeight  = 0.f;  // BODY only: stored y of the body frame origin (meters)
    float widthMeters = 1.6f; // quad width; seeded from openxr:default_size
    // quad height is ALWAYS derived: heightMeters = widthMeters * pxH / pxW
};
```

### 2.2 Solver runtime state (not persisted)

```cpp
class CXRAnchor {
  public:
    SXRSolveResult solve(const SXRSolveInput& in, const SXRAnchorTuning& tune); // §3
    // grab pose math (§4) — called by CXRInput's grab machine via COpenXRManager
    void beginGrab(eXRHand hand, const SXRPose& gripWorld);
    void grabPushPull(float deltaMeters);                    // §4.3
    void grabResize(float deltaMeters);                      // §4.3
    void endGrab(const SXRSolveInput& in, const SXRAnchorTuning& tune);
    // verbs (§5) — main thread, under the layer mutex
    bool applyMove(const Vec3& d, const SXRVerbContext& ctx);
    bool applyRotate(float dyawRad, float dpitchRad, const SXRVerbContext& ctx);
    bool applyScale(bool isDelta, float f);
    bool applyDistance(float dMeters, const SXRVerbContext& ctx);
    bool applyCenter(const SXRVerbContext& ctx, float defaultDistance);
    // mode transitions (§5.6) and recentering (§6)
    bool setMode(eXRAnchorMode newMode, eXRHand hand, const SXRVerbContext& ctx);
    void onReferenceSpaceChanged(const SXRPose& poseInPreviousSpace); // §6

    SXRAnchorState m_state;

  private:
    // leash spring state
    Vec3    m_springPos;         // current smoothed quad position (world)
    Vec3    m_springVel;         // its velocity
    Quat    m_smoothedRot;       // current smoothed orientation (world)
    bool    m_chasing = false;   // deadzone latch: false = LATCHED, true = CHASING
    // body-frame yaw filter
    float   m_lastYaw    = 0.f;
    bool    m_yawHolding = false;
    // grab
    bool    m_grabbed = false;
    eXRHand m_grabHand = XR_HAND_LEFT;
    SXRPose m_grabOffset;        // in grabbing hand's grip space
    // last composed world pose (LOCAL_FLOOR) — used for grab capture, tracking-loss
    // fallback, hit testing, IPC dumps
    SXRPose m_lastWorld;
    bool    m_hasLastWorld = false;
};
```

### 2.3 Solve inputs/outputs — the pure API

```cpp
struct SXRAnchorTuning {          // read from config by COpenXRManager, passed in
    float leashResponse;          // openxr:leash_response          (s,   default 0.35)
    float deadzoneAngleRad;       // openxr:leash_deadzone_angle    (cfg in deg, default 15)
    float deadzoneDistance;       // openxr:leash_deadzone_distance (m,   default 0.25)
    bool  bodyFollowHeight;       // openxr:body_leash_follow_height (default 0)
    float defaultDistance;        // openxr:default_distance        (m,   default 1.5)
};

struct SXRSolveInput {
    SXRPose                view;      // VIEW pose in LOCAL_FLOOR at predictedDisplayTime
    float                  dt;        // seconds since last solve (clamp to [0, 0.1])
    std::optional<SXRPose> gripLeft;  // grip poses in LOCAL_FLOOR; nullopt = tracking invalid
    std::optional<SXRPose> gripRight;
    uint32_t               pxW = 1, pxH = 1; // current monitor mode, for aspect
};

struct SXRSolveResult {
    eXRSpaceSelector space;       // which XrSpace the quad layer references
    SXRPose          pose;        // XrCompositionLayerQuad::pose, expressed IN that space
    SXRPose          worldPose;   // same pose composed into LOCAL_FLOOR (hit tests, IPC);
                                  // for grip spaces with lost tracking: last known world
    float            widthMeters;
    float            heightMeters; // widthMeters * pxH / pxW
};
```

`CXRMonitorLayer` calls `solve()` once per frame on the frame thread with the view pose from
`xrLocateSpace(viewSpace, localFloorSpace, predictedDisplayTime, …)` and grips from `CXRInput`
(nullopt unless both `XR_SPACE_LOCATION_POSITION_VALID_BIT` and
`XR_SPACE_LOCATION_ORIENTATION_VALID_BIT` are set). It maps `space` to the actual `XrSpace`
handle and converts `pose` to `XrPosef`.

## 3. Per-mode solve

Dispatch order inside `solve()`:

```
if m_grabbed:          return grab override (§4.2)
switch (m_state.mode): LOCAL §3.1 | HEAD §3.2 | BODY §3.3 | DEVICE §3.4
finally:               m_lastWorld = result.worldPose; m_hasLastWorld = true
```

### 3.1 `XR_ANCHOR_LOCAL`

Zero per-frame work:

```
space     = XR_SPACE_LOCAL_FLOOR
pose      = m_state.anchorPose      // verbatim
worldPose = m_state.anchorPose
```

### 3.2 `XR_ANCHOR_HEAD` — head leash

The stored offset `O = m_state.anchorPose` lives in **view space**. Raw target:

```
T = poseCompose(in.view, O)          // T.pos is the only part used for position
```

**Deadzone (position).** Two deviation measures, both against the *current* smoothed position
`x = m_springPos` with head at `h = in.view.pos`:

```
angDev = acos(clamp(dot(normalize(x − h), normalize(T.pos − h)), −1, 1))
posDev = |x − T.pos|
```

Latch state machine (`m_chasing`):

```
             angDev > deadzoneAngleRad  OR  posDev > deadzoneDistance
   LATCHED ──────────────────────────────────────────────────────────► CHASING
      ▲                                                                   │
      │        posDev < XR_LEASH_SETTLE_POS (0.01 m)                      │ spring integrates
      │        (re-latch when settled)                                    │ every frame
      └───────────────────────────────────────────────────────────────────┘
```

While LATCHED the position is frozen (`x_{n+1} = x_n`, `v = 0`) — small head motion doesn't drag
the quad around. Once CHASING, the spring runs until settled, then re-latches (prevents perpetual
micro-chasing).

**Critically-damped spring — derivation.** Damped harmonic oscillator toward (piecewise-constant)
target `T`:

```
ẍ = −ω²(x − T) − 2ζω ẋ,   critical damping ζ = 1
```

With `u(t) = x(t) − T`: `ü + 2ωu̇ + ω²u = 0`, characteristic root `−ω` (double), general solution
`u(t) = (C₁ + C₂ t) e^{−ωt}`. Initial conditions `u(0) = Δ = x_n − T`, `u̇(0) = v_n`:

```
C₁ = Δ
u̇(t) = (C₂ − ωC₁ − ωC₂ t) e^{−ωt}  ⇒  C₂ − ωΔ·… evaluate at 0: C₂ − ωC₁ = v_n ⇒ C₂ = v_n + ωΔ
```

Exact step over `dt` (unconditionally stable for any `dt`; apply per component of the Vec3):

```
ω      = 2 / leashResponse
Δ      = x_n − T.pos
k      = v_n + ω·Δ
E      = e^(−ω·dt)
x_{n+1} = T.pos + (Δ + k·dt) · E
v_{n+1} = (v_n − k·ω·dt) · E
```

`ω = 2/leashResponse` puts the residual at `(1 + 2)e^{−2} ≈ 40%…` more precisely from rest the
envelope is `(1 + ωt)e^{−ωt}`, i.e. ≈ 41% after `t = leashResponse` and < 5% after
`t ≈ 2.4·leashResponse` — a snappy but overshoot-free follow.

**No overshoot from rest (the property the unit test checks):** with `v_n = 0`,
`u(t) = Δ(1 + ωt)e^{−ωt}` — same sign as `Δ` for all `t`, and
`d/dt|u| = −|Δ|ω²t e^{−ωt} ≤ 0`: strictly monotone decay, never crosses the target.

**Orientation.** The stored `O.rot` is *not* used for display. Every frame (regardless of latch
state) the target orientation is recomputed so the quad always faces the head and can never go
edge-on:

```
R_target      = lookAtNoRoll(m_springPos, in.view.pos, m_smoothedRot)
α             = 1 − e^(−dt / leashResponse)      // exact step of ẋ = (target − x)/τ
m_smoothedRot = qSlerp(m_smoothedRot, R_target, α)
```

(The slerp factor is the exact discretization of the first-order low-pass
`Ṙ = (R_target − R)/τ`, `τ = leashResponse`: solution `R(dt) = target + (R₀ − target)e^{−dt/τ}`.)

Result:

```
space = XR_SPACE_LOCAL_FLOOR
pose = worldPose = { m_springPos (after step), m_smoothedRot }
```

Initialization: when the mode is entered (config load, `setMode`, grab release) the spring is
seeded `m_springPos = current world pos, m_springVel = 0, m_chasing = false`; if there is no
current world pose yet (fresh layer), seed from `poseCompose(view, O)` and snap.

### 3.3 `XR_ANCHOR_BODY` — body leash

**Yaw-only body frame.** Extract the head's horizontal facing:

```
fwd  = qRotate(in.view.rot, (0, 0, −1))
horiz = sqrt(fwd.x² + fwd.z²)         // == |sin(pitch complement)|; small when looking up/down
```

Hysteresis guard for near-vertical gaze (`|fwd.y| ≈ 1` ⇔ `horiz ≈ 0`), constants
`XR_BODY_YAW_HOLD = 0.15`, `XR_BODY_YAW_RESUME = 0.25`:

```
if m_yawHolding:  if horiz > XR_BODY_YAW_RESUME: m_yawHolding = false
else:             if horiz < XR_BODY_YAW_HOLD:   m_yawHolding = true
if !m_yawHolding: m_lastYaw = atan2(−fwd.x, −fwd.z)
yaw = m_lastYaw                        // held at last valid value while looking up/down
```

Body frame:

```
bodyFrame.pos = ( in.view.pos.x,
                  tune.bodyFollowHeight ? in.view.pos.y : m_state.bodyHeight,
                  in.view.pos.z )
bodyFrame.rot = qFromYaw(yaw)
```

`m_state.bodyHeight` is captured whenever the mode is entered (= view y at that moment) and by
recenter handling; it is what keeps quads at desk height when the user sits/stands with
`body_leash_follow_height = 0`.

**Target and spring.** `T = poseCompose(bodyFrame, O)` with `O = m_state.anchorPose`. Same latch
machine and identical spring math as §3.2, with the deviations made *distance-dominant* — head
pitch and small yaw wobble must not wake the quad:

```
posDev = |x − T.pos|                                  // full 3D distance
angDev = |wrapPi(qYawOf(m_smoothedRot, yaw) − qYawOf(T.rot, yaw))|   // yaw difference only
chase when: posDev > deadzoneDistance  OR  angDev > deadzoneAngleRad
```

(`wrapPi` wraps to (−π, π]. Because the body yaw is already hysteresis-filtered and pitch is
excluded, in practice the positional term dominates.)

**Orientation: pitch/roll forced to 0.** The composed rotation is flattened to yaw-only, then
smoothed (slerp between two yaw-only quats stays yaw-only):

```
targetYaw     = qYawOf(qMul(bodyFrame.rot, O.rot), yaw)
R_target      = qFromYaw(targetYaw)
m_smoothedRot = qSlerp(m_smoothedRot, R_target, 1 − e^(−dt/leashResponse))
```

Result: `space = XR_SPACE_LOCAL_FLOOR`, `pose = worldPose = {m_springPos, m_smoothedRot}`.

### 3.4 `XR_ANCHOR_DEVICE` — device lock

The whole point of this mode is that **the compositor runtime late-latches the pose**: the quad's
`XrSpace` *is* the grip action space, so the runtime re-evaluates the controller pose at display
time — zero added latency, zero per-frame solve on our side.

```
grip = (m_state.device == XR_HAND_LEFT) ? in.gripLeft : in.gripRight
if grip.has_value():
    space     = (device == LEFT) ? XR_SPACE_GRIP_LEFT : XR_SPACE_GRIP_RIGHT
    pose      = m_state.anchorPose                       // stored grip-space offset, verbatim
    worldPose = poseCompose(*grip, m_state.anchorPose)   // for hit tests / grab / IPC only
else:
    // tracking loss: park the quad in the world at its last composed pose
    space     = XR_SPACE_LOCAL_FLOOR
    pose      = worldPose = m_lastWorld                  // until grip becomes valid again
```

No smoothing, no deadzone. (If `m_hasLastWorld` is false — tracking was never valid — park at
`defaultDistance` in front of the current view, computed as in §5.5 `center`.)

## 4. Grab interplay — pose math

The grab *state machine* (thresholds, which hand, thumbstick handling, haptics, the
`xrmonitorgrab` event) is specified in `04-input.md` §6. It drives `CXRAnchor` through the four
calls below. All composition happens in LOCAL_FLOOR.

### 4.1 `beginGrab(hand, gripWorld)`

Capture the quad's current *displayed* world pose relative to the grabbing hand:

```
m_grabOffset = poseCompose(poseInverse(gripWorld), m_lastWorld)
m_grabbed    = true
m_grabHand   = hand
```

`m_lastWorld` is used (not the raw anchor target) so grabbing a mid-flight leashed quad picks it
up exactly where it is rendered — no snap.

### 4.2 Solve override while grabbed

While `m_grabbed`, `solve()` short-circuits: the layer temporarily behaves as device-locked to the
grabbing hand:

```
space     = (m_grabHand == LEFT) ? XR_SPACE_GRIP_LEFT : XR_SPACE_GRIP_RIGHT
pose      = m_grabOffset
worldPose = grip(m_grabHand).has_value() ? poseCompose(grip, m_grabOffset) : m_lastWorld
```

Runtime late-latching makes the quad track the controller 1:1 with no added latency.

### 4.3 `grabPushPull` / `grabResize` (driven by thumbstick, see 04 §6)

Both mutate the temporary `m_grabOffset` / width, formulated as ray scaling so the clamp semantics
are exact:

```
grabPushPull(delta):                       // delta = stick.y * rate * dt, meters
    d = m_grabOffset.pos.length()
    dir = (d < 1e-4) ? Vec3(0,0,−1) : m_grabOffset.pos / d
    d' = clamp(d + delta, 0.3, 5.0)        // grip→quad distance clamp, meters
    m_grabOffset.pos = dir * d'

grabResize(delta):                          // delta = stick.x * rate * dt, meters
    m_state.widthMeters = clamp(m_state.widthMeters + delta, 0.2, 4.0)
```

### 4.4 `endGrab(in, tune)` — re-anchor into the persistent mode

First compose the final world pose (if the grabbing grip is invalid at release, fall back to
`m_lastWorld`):

```
W = poseCompose(gripWorld(m_grabHand), m_grabOffset)
m_grabbed = false
```

Then convert `W` back into the representation of the **persistent** mode. These are the exact
formulas (also reused verbatim by `setMode`, §5.6):

| mode | conversion |
|---|---|
| `XR_ANCHOR_LOCAL` | `anchorPose = W` |
| `XR_ANCHOR_HEAD` | `anchorPose = poseCompose(poseInverse(in.view), W)` — new view-space offset. Reset solver: `m_springPos = W.pos`, `m_springVel = 0`, `m_smoothedRot = lookAtNoRoll(W.pos, in.view.pos, W.rot)`, `m_chasing = false` |
| `XR_ANCHOR_BODY` | recompute `bodyFrame` from `in.view` per §3.3 (current yaw-filter state); `anchorPose = poseCompose(poseInverse(bodyFrame), W)`; then flatten: `anchorPose.rot = qFromYaw(qYawOf(anchorPose.rot, 0))`. Reset spring as above with `m_smoothedRot = qFromYaw(targetYaw)`. `bodyHeight` is left unchanged (the offset's y absorbs the new height) |
| `XR_ANCHOR_DEVICE` | `anchorPose = poseCompose(poseInverse(gripWorld(m_state.device)), W)` — offset relative to the *anchor* hand. If `m_state.device == m_grabHand` this reduces algebraically to `m_grabOffset` exactly. If the anchor hand's grip is invalid at release, park via the §3.4 loss path (`m_lastWorld = W`) and recompute the offset on the first frame it is valid again |

**Round-trip identity** (unit-tested): `beginGrab` followed immediately by `endGrab` with
unchanged poses leaves the solved world pose bit-identical (up to float epsilon) for every mode:
`W = grip ∘ (grip⁻¹ ∘ m_lastWorld) = m_lastWorld`, and each conversion is the exact inverse of the
corresponding compose in §3.

## 5. Keyboard repositioning verbs

Dispatcher: `xrmonitor move|rotate|scale|distance|center …` (registration, target-monitor
resolution, and argument *parsing* are `05-ipc-config.md`; selected-monitor resolution is
`04-input.md` §9). `XRIpc` parses, resolves the target layer, then calls the `apply*` methods
below on the main thread under the layer mutex. Angles arrive in **degrees** from the user and are
converted to radians at the call boundary.

Context struct (captured by the manager from the most recent frame-thread solve inputs — a copy,
so verbs never block the frame thread):

```cpp
struct SXRVerbContext {
    SXRPose                view;      // last known view pose (LOCAL_FLOOR)
    bool                   viewValid; // false before the first tracked frame
    std::optional<SXRPose> gripLeft, gripRight;
};
```

Every verb returns `false` (dispatcher error, no mutation) when data it needs is missing:
`move/distance/center` need `viewValid`; DEVICE-mode conversions need the anchor hand's grip.
`scale` and LOCAL/HEAD/BODY `rotate` never need tracking.

### 5.1 `move dx dy dz` — VIEW-relative translation (meters)

Axes are the **view's** right/up/forward; `dz > 0` = away from the face = view −Z:

```
D_view  = (dx, dy, −dz)
D_world = qRotate(ctx.view.rot, D_view)
```

Per-mode mutation of the persistent state:

| mode | mutation |
|---|---|
| LOCAL | `anchorPose.pos += D_world` |
| HEAD | `anchorPose.pos += D_view` (offset already lives in view space); set `m_chasing = true` so the quad glides to the new target |
| BODY | `anchorPose.pos += qRotate(qInverse(bodyFrame.rot), D_world)` (bodyFrame from ctx.view per §3.3); `m_chasing = true` |
| DEVICE | `anchorPose.pos += qRotate(qInverse(grip.rot), D_world)` (needs anchor hand grip) |

### 5.2 `rotate dyaw [dpitch]` — degrees

Deliberate mode asymmetry (document in user-facing docs too):

- **LOCAL / DEVICE — rotate in place** (position unchanged). Yaw applied in the parent frame
  (pre-multiply), pitch about the quad's own X axis (post-multiply):

  ```
  anchorPose.rot = qMul(qFromYaw(dyaw), anchorPose.rot)
  anchorPose.rot = qMul(anchorPose.rot, qFromPitch(dpitch))
  ```

- **HEAD — orbit around the head.** Display orientation is lookAt-driven (§3.2), so rotating in
  place is meaningless; instead orbit the offset position (pitch about view X first, then yaw
  about view Y; positive dpitch raises the quad, positive dyaw moves it left — from
  `R_x(θ)(0,0,−1) = (0, sinθ, −cosθ)` and `R_y(θ)(0,0,−1) = (−sinθ, 0, −cosθ)`):

  ```
  anchorPose.pos = qRotate(qFromYaw(dyaw), qRotate(qFromPitch(dpitch), anchorPose.pos))
  m_chasing = true
  ```

- **BODY — orbit around the body axis**, keeping the facing consistent; pitch moves it up/down the
  orbit, yaw also turns the stored facing so the quad keeps pointing inward:

  ```
  anchorPose.pos = qRotate(qFromYaw(dyaw), qRotate(qFromPitch(dpitch), anchorPose.pos))
  anchorPose.rot = qMul(qFromYaw(dyaw), anchorPose.rot)   // stays yaw-only
  m_chasing = true
  ```

### 5.3 `scale <f | ±delta>` — mode-independent

Arg starting with `+`/`-` = additive delta in meters, otherwise multiplicative factor. Width clamp
**0.2–4.0 m** (same clamp as grab resize):

```
widthMeters = clamp(isDelta ? widthMeters + f : widthMeters * f, 0.2, 4.0)
```

(Height follows automatically: `height = width * pxH / pxW`.)

### 5.4 `distance <±m>` — slide along the view→quad ray, clamp **0.3–5.0 m**

```
d    = m_lastWorld.pos − ctx.view.pos          // requires m_hasLastWorld (else fail)
len  = d.length();  if len < 1e-4: fail
dir  = d / len
len' = clamp(len + dm, 0.3, 5.0)
P'   = ctx.view.pos + dir * len'               // new world position, orientation unchanged
```

| mode | mutation |
|---|---|
| LOCAL | `anchorPose.pos = P'` |
| HEAD | `anchorPose.pos *= len'/len` (the view-space offset *is* the head ray — exact equivalent); `m_chasing = true` |
| BODY | `anchorPose.pos = qRotate(qInverse(bodyFrame.rot), P' − bodyFrame.pos)`; `m_chasing = true` |
| DEVICE | `anchorPose.pos = qRotate(qInverse(grip.rot), P' − grip.pos)` (needs grip) |

### 5.5 `center` — re-place in front of the view at current size

```
fwd = qRotate(ctx.view.rot, (0, 0, −1))
W'  = { .pos = ctx.view.pos + fwd * tune.defaultDistance,   // openxr:default_distance
        .rot = lookAtNoRoll(W'.pos, ctx.view.pos, identity) }
```

| mode | mutation |
|---|---|
| LOCAL | `anchorPose = W'` |
| HEAD | `anchorPose = { (0, 0, −defaultDistance), identity }`; do **not** warp the spring — set `m_chasing = true` so it glides |
| BODY | recompute bodyFrame; `anchorPose = poseCompose(poseInverse(bodyFrame), W')`, rot flattened to `qFromYaw(qYawOf(…))`; `m_chasing = true` |
| DEVICE | `anchorPose = poseCompose(poseInverse(grip), W')` (needs grip) |

### 5.6 `xrmonitor anchor <target> <mode>` — mode transitions

`setMode()` must be seamless: the quad must not move when the mode changes. Implementation: take
`W = m_lastWorld` (or the §5.5 `center` placement if `!m_hasLastWorld`) and run **exactly the
§4.4 conversion table** for the *new* mode, then reset the solver runtime state as described
there. For `device:left|right`, `m_state.device` is set from the parsed mode; if that grip is
untracked, store `m_lastWorld = W` and let §3.4's loss path hold the quad until tracking returns.

## 6. Recentering — `XrEventDataReferenceSpaceChangePending`

The session event pump (`01-session-graphics.md`) forwards this event when
`referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR` (or `LOCAL` when using the
floor-offset fallback). Semantics per the OpenXR spec: `poseInPreviousSpace` is the pose of the
**new** space origin expressed in the **previous** space, valid iff `poseValid`.

A pose `P_old` in old coordinates re-expressed in new coordinates:

```
M      = poseInPreviousSpace           // new origin in old space
P_new  = poseCompose(poseInverse(M), P_old)
```

Per-anchor handling in `onReferenceSpaceChanged(M)`:

- **LOCAL**: re-derive so the quad stays physically where it is across runtime recenters and
  drift corrections: `anchorPose = inv(M) ∘ anchorPose`. Also re-express the cached solver state:
  `m_lastWorld = inv(M) ∘ m_lastWorld`, `m_springPos` likewise, `m_springVel` rotated by
  `qInverse(M.rot)`, `m_smoothedRot = qMul(qConjugate(M.rot), m_smoothedRot)`.
  If `poseValid == false`, leave the stored pose untouched (the quad jumps with the new origin —
  nothing better is possible).
- **HEAD / DEVICE**: offsets are relative to view/grip spaces, which move with the user —
  **unaffected**; only re-express the cached `m_lastWorld`/spring state as above so there is no
  one-frame pop.
- **BODY**: the offset is body-relative (unaffected), but `bodyHeight` is a stored LOCAL_FLOOR
  height: `bodyHeight = (inv(M) ∘ {(0, bodyHeight, 0), identity}).pos.y` — i.e. subtract the new
  origin's y. Re-express cached state as above.

The event is delivered on the frame thread between frames; apply it there (the anchor state mutex
is already documented in §0).

## 7. Persistence v1 — `hyprctl openxr layout`

v1 persistence is deliberately dumb: `hyprctl openxr layout` walks all live XR monitors and prints
paste-ready config lines the user drops into their `hyprland.conf`. The **grammar** of the
`xrmonitor` keyword is owned by `05-ipc-config.md` (shape:
`xrmonitor = NAME, WxH@Hz, anchor-spec, size:W`, anchor-spec one of
`anchor:local pos:x,y,z yaw:Y [pitch:P]` | `anchor:head offset:x,y,z` | `anchor:body offset:x,y,z yaw:Y`
| `anchor:device:left|right offset:x,y,z yaw:Y [pitch:P]` — defer to doc 05 for the authoritative
grammar). This section owns the **pose → anchor-spec serialization rules**:

- `pos:` / `offset:` = `anchorPose.pos`, printed as meters with 3 decimals (`{:.3f}`), meaning per
  mode exactly as stored (§2.1): world for `local`, view offset for `head`, body offset for
  `body`, grip offset for `device`.
- Rotation is serialized as **yaw/pitch degrees only**, derived from the forward vector:

  ```
  f     = qRotate(anchorPose.rot, (0, 0, −1))
  pitch = degrees(asin(clamp(f.y, −1, 1)))
  yaw   = degrees(atan2(−f.x, −f.z))        // if f.x² + f.z² < 1e-8: yaw = 0
  ```

  printed with 1 decimal; `pitch:` omitted when `|pitch| < 0.05°`; for `head` no rotation is
  printed at all (display orientation is lookAt-driven, §3.2); for `body` only `yaw:` (pitch/roll
  are forced to 0, §3.3).
- Deserialization (the keyword parser, doc 05) reconstructs
  `rot = qMul(qFromYaw(yaw), qFromPitch(pitch))`. Round trip: pitch-then-yaw applied to (0,0,−1)
  gives `(−cos p sin y, sin p, −cos p cos y)`, from which the formulas above recover `y`, `p`
  exactly.
- **Roll is intentionally not representable.** A grabbed LOCAL/DEVICE quad can carry roll at
  release; serializing drops it (head-leashed quads never have roll by construction). This is a
  documented v1 limitation, not a bug — note it in the layout dump as a trailing comment only if
  any layer had `|roll| > 1°`.
- `size:` = `widthMeters`, 2 decimals. Height is never serialized (derived from the mode's aspect).
- `bodyHeight` is **not** serialized in v1; on config load it is captured from the first tracked
  view pose (§3.3). Known v1 quirk: a saved body layout re-anchors at the height of first wear.

## 8. Constants (single header block in `XRAnchor.hpp`)

```cpp
constexpr float XR_LEASH_SETTLE_POS   = 0.01f;  // m,  re-latch threshold (§3.2)
constexpr float XR_BODY_YAW_HOLD      = 0.15f;  // horiz-projection len below which yaw holds (§3.3)
constexpr float XR_BODY_YAW_RESUME    = 0.25f;  // …and above which it resumes (hysteresis)
constexpr float XR_WIDTH_MIN          = 0.2f;   // m  (§4.3, §5.3)
constexpr float XR_WIDTH_MAX          = 4.0f;
constexpr float XR_DISTANCE_MIN       = 0.3f;   // m  (§4.3, §5.4)
constexpr float XR_DISTANCE_MAX       = 5.0f;
constexpr float XR_SOLVE_DT_MAX       = 0.1f;   // s, dt clamp (§2.3)
```

Config-derived tuning (`SXRAnchorTuning`) is read by `COpenXRManager` from `openxr:leash_response`
(0.35 s), `openxr:leash_deadzone_angle` (15°), `openxr:leash_deadzone_distance` (0.25 m),
`openxr:body_leash_follow_height` (0), `openxr:default_distance` (1.5 m) — registration and
descriptions in doc 05.

## 9. Unit test spec — `tests/xr/anchor_math.cpp`

gtest, picked up automatically by the `tests/*.cpp` glob into `hyprland_gtests`
(`CMakeLists.txt:688`); must build and pass with no OpenXR runtime or headers present. Include
only `src/openxr/XRMath.hpp` + `src/openxr/XRAnchor.hpp`. Use `EXPECT_NEAR` with 1e-5 tolerances
unless stated. Required cases:

1. **`MathComposeInverseIdentity`** — random-ish poses: `poseCompose(poseInverse(P), P)` ≈
   identity; `qRotate(qMul(a,b), v) == qRotate(a, qRotate(b, v))`.
2. **`MathLookAtNoRoll`** — for several from/to pairs: result's +Z points at `to`; result's +X has
   `y == 0` (roll removed); near-vertical from/to returns the fallback.
3. **`YawExtraction`** — `qYawOf(qFromYaw(θ), 0) == θ` for θ ∈ {0, ±45°, ±90°, 179°}; composed
   yaw∘pitch still extracts θ; serialization round trip of §7 recovers yaw and pitch.
4. **`SpringConvergesNoOvershoot`** — HEAD mode, quad latched at rest 1 m from a new target
   (force CHASING): step `solve()` at dt = 1/90 s; assert `sign(x − T)` never flips per component,
   `|x − T|` is monotonically non-increasing, and `|x − T| < 0.001` within `5 * leashResponse`
   seconds (critical-damping property, §3.2 derivation).
5. **`SpringStableLargeDt`** — same setup, one step with dt = 0.1: still no overshoot (exact
   integrator, not Euler).
6. **`DeadzoneHoldRelease`** — HEAD mode settled; move the view so `angDev` stays below
   `deadzoneAngleRad` and `posDev` below `deadzoneDistance`: position must not change at all.
   Exceed either bound: chasing starts; after convergence within `XR_LEASH_SETTLE_POS` it
   re-latches (small further deviation again produces zero motion).
7. **`BodyYawNearVerticalHysteresis`** — BODY mode: pitch the view toward straight-down past the
   hold threshold while yawing 90°: `m_lastYaw` (observable via the solved orientation) must stay
   at the pre-pitch value; return below the resume threshold: yaw updates again. Also: pure head
   pitch never changes the body target (deadzone independence).
8. **`GrabRoundTripIdentity`** — for each of the four modes (DEVICE with both same-hand and
   opposite-hand anchor): solve once to establish `m_lastWorld`, `beginGrab`, immediately
   `endGrab` with identical inputs, solve again — world pose unchanged within 1e-5, and for HEAD
   the spring does not kick (position deviation stays 0 next frame).
9. **`GrabOffsetFollows`** — beginGrab, translate+rotate the grip by a known pose delta, solve:
   worldPose == `newGrip ∘ inv(oldGrip) ∘ oldWorld`.
10. **`VerbMath`** — `applyMove` in each mode moves the solved world pose by exactly
    `qRotate(view.rot, (dx,dy,−dz))` (after spring convergence for leashed modes);
    `applyDistance` clamps at 0.3 and 5.0; `applyScale` clamps at 0.2 and 4.0 and handles both
    factor and ±delta; `applyCenter` places the quad at `defaultDistance` along view forward,
    facing the head; `applyRotate` on LOCAL is in-place (pos unchanged), on HEAD orbits (distance
    to head unchanged, position moved).
11. **`DeviceTrackingLoss`** — DEVICE mode, grip valid → nullopt: result switches to
    `XR_SPACE_LOCAL_FLOOR` with the last composed world pose; grip valid again: back to the grip
    space with the stored offset.
12. **`ReferenceSpaceChange`** — LOCAL anchor: apply `onReferenceSpaceChanged(M)`; solved world
    pose re-expressed such that `M ∘ newPose == oldPose`. HEAD/BODY/DEVICE stored offsets
    unchanged; BODY `bodyHeight` shifted by the origin's y.

## Context files to read before implementing

- `/home/ajg/code/Hyprland/docs/openxr/00-overview.md` — thread model, lifecycle, component diagram
- `/home/ajg/code/Hyprland/docs/openxr/01-session-graphics.md` — reference spaces, event pump, frame loop (where `solve()` is called from)
- `/home/ajg/code/Hyprland/docs/openxr/02-virtual-monitors.md` — `CXRMonitorLayer`, layer snapshot mutex, quad submission
- `/home/ajg/code/Hyprland/docs/openxr/04-input.md` — grab state machine that drives §4; `SXRVerbContext` capture; selected-monitor resolution
- `/home/ajg/code/Hyprland/docs/openxr/05-ipc-config.md` — `xrmonitor` keyword grammar, dispatcher/hyprctl registration, config var registration
- `/home/ajg/code/Hyprland/CMakeLists.txt` — lines ~680–700: `hyprland_gtests` target + `tests/*.cpp` glob
- `/home/ajg/code/Hyprland/tests/` — any existing `*.cpp` (e.g. under `tests/helpers/`) for gtest conventions in this repo
- `/home/ajg/code/Hyprland/src/helpers/math/Math.hpp` — `Vector2D` (what exists today; why Vec3/Quat are new)
- `git show openxr:src/openxr/COpenXRManager.cpp` — WIP branch, for how view poses were located per frame
