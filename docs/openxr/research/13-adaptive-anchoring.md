# 13 — Adaptive Anchoring: monitors that jump between world- and head-anchored by position

Status: **research / design only. Nothing here is implemented.** No live runs, no code changes, no
commits. Author: research pass 2026-07-09.

The ask (user's words): *"Configurable monitors which can jump between world-anchored and
head-anchored based on my current position. E.g. if I'm watching a video, I may want that anchored
at my desk, but when I stand up and start to walk away I want it to follow me around. It should
pleasantly animate moving between the two locations rather than just snapping on move."*

Daily-life context: the user now wanders their house with head-anchored monitors over passthrough
(boundary disabled). The desk-vs-roaming duality is a real, recurring workflow, not a demo.

Cross-refs (source of truth): `docs/openxr/03-anchoring.md` (leash + `reanchorFromWorld`/`setMode`
machinery this feature is built on), `src/openxr/XRAnchor.{hpp,cpp}`, `src/openxr/OpenXRManager.cpp`
frame loop (`:843-925` solve, `:1586` `readAnchorTuning`, `:1602` `currentVerbContext`), `src/openxr/XRMath.hpp`
(`chromeFadeAdvance` §8 — the pure envelope pattern to copy), `src/openxr/XRMonitorConfig.cpp:286`
(`serializeXRMonitorLine`). Composition points: `research/10-view-bounding.md` (cone clamp, applies in
ROAM only), `research/08-auto-layout.md` (slot reserve-vs-compact on undock), `research/12-spatial-2d-layout.md`
(2D layout recompute on dock/undock events), `research/04-grabbable-borders.md` (grab-release,
`pickReleasePose`).

---

## TL;DR

1. **Build it as an *orthogonal decorator*, not a fifth anchor mode.** The persisted mode stays
   `anchor:local` (the desk pose). A small per-monitor adaptive layer holds `{enabled, roamMode
   (head|body), roamOffset, dockPose}` and a runtime phase machine. This keeps serialization trivial
   (`anchor:local pos:… adaptive:on roam:body …`), reuses the head/body solve verbatim, and touches
   `CXRAnchor::solve()`'s dispatch by exactly one pre-step. A `XR_ANCHOR_ADAPTIVE` enum value would
   bloat every `switch` in the file and duplicate the leash — reject it (§4.1).

2. **Trigger = a horizontal geofence around the "desk seat" (head position captured at dock), with
   hysteresis + dwell in both directions** (§3). Leave radius `R_out` (default 1.5 m) held for
   `T_out` (400 ms) → undock; return inside `R_in` (1.0 m, `< R_out`) held for `T_in` (800 ms) →
   re-dock. Distance is **XZ-only by default** so *standing up doesn't undock* (the user's own
   distinction: stand = stay, walk away = follow). Position source is the view pose in LOCAL_FLOOR,
   already available every frame (`OpenXRManager.cpp:844`). Gaze is **not** a gate (you may be
   watching the video *as* you walk off); velocity is an optional accelerator, deferred.

3. **The "pleasant animation" is a pose-space blend over a dedicated envelope, not the leash
   spring** (§4.3). During a transition the quad is a lerp between a frozen `from` world pose and a
   *live* `to` world pose: undock blends desk-pose → live roam-solve target (which itself moves with
   you, so it "peels off the desk and catches up to your head"); re-dock blends the current roam
   pose → the static saved desk pose. Envelope parameter advances linearly by `dt` (copying
   `chromeFadeAdvance`) with a taste-tunable easing (`smoothstep` default) and configurable duration
   (`~700 ms`), independent of `leash_response`. This is exactly MRTK's *SolverHandler smoothing
   bridges a goal swap* mechanism (§2.1).

4. **Roam as `body` by default** (yaw-only, comfortable at a fixed height while walking — matches
   research/10's comfort findings and the user's "around the house"), with the research/10 bounding
   cone active only in ROAM. Roam to a **configured comfortable `roam_offset`**, not the carried
   desk offset (a desk-height monitor at knee level following you through the house is wrong) — the
   pose genuinely *changes* desk→eye-level, which is *why* the animated handoff exists. Offer
   carry-current-offset as an option.

5. **Everything the geofence/envelope needs is POD + `dt`** → it lives in `CXRAnchor` (frame thread,
   zero hyprutils refcount ops, gtest-able like `anchor_math.cpp`). Phase edges surface to IPC via a
   `std::atomic<uint8_t>` on the layer (the established `m_hoverRegion`/`m_grabbedNow` write-back
   pattern), which the main thread edge-detects into `xrmonitordocked`/`xrmonitorundocked` socket2
   events, status JSON, and the research/12 2D-relayout trigger.

6. **Six work packages** (§7): WP-A1 pure phase machine + geofence + envelope + gtests (S/M, no
   deps, headless); WP-A2 config/grammar/serialization (S); WP-A3 manager wiring + events + verbs +
   pinch override (M); WP-A4 grab-release policy + bounding gate (S/M, composes research/04+10);
   WP-A5 auto-layout slot-reservation + 2D-relayout hookup (M, deps research/08+12); WP-A6 hyprtester
   `--xr` scripted-head-walk geofence test (S). A1/A6 are headless-testable via the remote driver's
   scripted head pose; the *feel* (radii, dwell, easing) needs live Quest wandering.

---

## 1. What exists to build on (verified in code)

- **`reanchorFromWorld(W, ctx, tune)`** (`XRAnchor.cpp:451`) is the load-bearing primitive: it takes
  any LOCAL_FLOOR world pose and re-expresses it into `local`/`head`/`body`/`device`, *reseeding the
  leash spring* (`m_springPos = W.pos`, `m_chasing=false`, `m_springInit=true`). This is what
  undock/redock ultimately call to establish the persistent representation at each endpoint.
- **`setMode(newMode, hand, ctx, tune)`** (`:617`) is a *seamless but instantaneous* mode flip —
  takes `m_lastWorld` (or a center placement) and reanchors. It is the "no pleasant animation"
  version of exactly this feature; adaptive is `setMode` **plus** a phase machine **plus** a
  transition envelope. Reusable pieces, wrong ergonomics on its own.
- **Head/body leash solve** (`:219-297`): spring + deadzone + hysteresis, all pure, `dt`-driven.
  Adaptive roam *is* this solve with a different offset. The one refactor A1 needs: lift the
  HEAD/BODY case bodies into a `solveLeash(mode, O, in, tune) → worldPose` helper so both the normal
  path and the adaptive roam path call it (the spring state `m_springPos/m_springVel/m_smoothedRot`
  is shared — a monitor is never docked and roaming at once, so no conflict).
- **Head position every frame**: `viewPose` in LOCAL_FLOOR at `OpenXRManager.cpp:844`, fed to
  `solve()` as `in.view`. The geofence reads `in.view.pos` — no new plumbing, no `xrLocateViews`
  (unlike research/10's FOV need).
- **`chromeFadeAdvance`** (`XRMath.hpp:680`): the canonical pure envelope — linear ramp of a scalar
  toward a target at `dt/duration`, snap when duration ≤ 0, hold when `dt ≤ 0`. The transition
  parameter advance copies this shape exactly (easing applied at sample time).
- **`m_lastWorld`/`hasLastWorld()`** (`:253-258`): the current displayed world pose — the `from`
  endpoint captured when a transition begins, and (for a docked LOCAL monitor) identically equal to
  `dockPose`.
- **Frame→IPC write-back pattern**: `m_hoverRegion`/`m_grabbedNow` atomics on `CXRMonitorLayer`
  (`XRMonitorLayer.hpp:149-150`) are written frame-thread, read main-thread — the template for
  surfacing adaptive phase without a refcount hazard.
- **Verbs run main-thread under `m_layersMu`** and read a *copy* of last frame's poses via
  `currentVerbContext()` (`:1602`) — so `xrmonitor dock/undock/adaptive` never block the frame
  thread.
- **Config is per-frame hot**: `readAnchorTuning()` (`:1586`) re-reads `openxr:*` every frame →
  adaptive thresholds hot-tune for free (same as the grab-filter vars).

Nothing here is a shared *parent* transform or a group concept — each `CXRAnchor` is per-monitor, so
v1 adaptive is per-monitor (§6.4).

---

## 2. Prior art

### 2.1 MRTK — the blended-solver handoff mechanism (the one to copy)

MRTK is the reference for "tag-along UI that transitions smoothly between behaviors," and the
mechanism is precise and directly applicable:

- **Solvers write a shared goal, they don't touch the transform.** With `UpdatedLinkedTransform =
  true`, each `Solver.SolverUpdate()` writes its computed pose into intermediary
  `GoalPosition`/`GoalRotation`/`GoalScale` on the shared `SolverHandler` rather than to
  `gameObject.transform`. Solvers on one object **chain in inspector order** (`GetComponents<Solver>()`
  on Start; order overridable via `SolverHandler.Solvers`), each reading and modifying the previous
  goal.
- **The `SolverHandler` smooths the *object* toward the goal.** With `Smoothing = true`, the handler
  lerps the actual transform toward the current goal every frame; per-axis `MoveLerpTime` /
  `RotateLerpTime` / `ScaleLerpTime` set the rate (higher = slower). This is an exponential SmoothTo,
  not an animation clip.
- **⇒ The handoff is emergent: swap which solver drives the goal, and the shared SmoothTo *bridges
  the goal jump* into a glide.** Turning a `Follow` solver on/off (or reordering it) changes
  `GoalPosition` discontinuously, but the object slides to the new goal at `MoveLerpTime` instead of
  snapping. "Blended solver transition" in MRTK = *a goal swap absorbed by SolverHandler smoothing*.
  The `Momentum` solver optionally adds spring/overshoot on top.
- Two secondary "blend" knobs worth noting: `SurfaceMagnetism.OrientationMode = Blended` with
  `OrientationBlend` (0 = face tracked target, 1 = surface normal) is a *rotation-factor* blend, and
  `Follow.reorientWhenOutsideParameters` is a deadzone (only re-orient once outside the bounds) —
  both are shape details, not the handoff itself.

**Mapping onto HypXRland.** Our "solvers" are the docked-LOCAL solve and the roam head/body solve;
our "goal swap" is the phase change; our "SolverHandler SmoothTo" is the transition envelope (§4.3).
The one deliberate deviation: MRTK's SmoothTo is an *unbounded exponential* toward a live goal (same
family as our leash spring), whereas we want a *bounded, eased, fixed-duration* envelope so a
desk→roam handoff has a predictable ~700 ms arc with taste-controlled easing rather than the leash's
fixed critical-damping. (We already have the exponential option — it's `reanchorFromWorld` + the
spring — and §4.3 explains why the bounded envelope is the better default for a *large* pose change.)

Sources: [Solvers — MRTK3](https://learn.microsoft.com/en-us/windows/mixed-reality/mrtk-unity/mrtk3-spatialmanipulation/packages/spatialmanipulation/solvers/solver),
[SolverHandler API](https://learn.microsoft.com/en-us/dotnet/api/microsoft.mixedreality.toolkit.utilities.solvers.solverhandler?view=mixed-reality-toolkit-unity-2020-dotnet-2.8.0),
[Follow API](https://learn.microsoft.com/en-us/dotnet/api/microsoft.mixedreality.toolkit.utilities.solvers.follow?view=mixed-reality-toolkit-unity-2020-dotnet-2.8.0)
(Follow/RadialView defaults already tabulated in research/10 §2.1).

### 2.2 visionOS — recenter, not auto-follow

Apple deliberately *avoids* automatic head-following: windows are space-anchored, and the HIG warns
that "anchoring content so that it remains statically in front of someone can make them feel stuck,
confined, and uncomfortable" — prefer space-anchoring and, when following is needed, a *lazy*
(delayed, smoothed) follow. The relevant verb is **manual recenter**: pressing/holding the Digital
Crown re-centers all windows to the current head pose/height, and visionOS 2 volume ornaments
*relocate to the side the user is on* (a recall affordance, not rigid follow). Takeaway: adaptive is
*more aggressive* than Apple's stance (we auto-switch on position), so (a) make it **opt-in
per-monitor**, (b) keep a **manual recenter/dock/undock verb** as the always-available primitive the
auto-trigger merely drives, and (c) the ROAM state should be the *lazy* follow we already have (leash
+ deadzone), never a rigid head-lock. ([Spatial layout HIG](https://developer.apple.com/design/human-interface-guidelines/spatial-layout),
[Ornaments HIG](https://developer.apple.com/design/human-interface-guidelines/ornaments))

### 2.3 Meta Horizon OS — Travel Mode and "adjust content to posture/motion"

- **Travel Mode** (Quest v65+ seated on planes, v71+ trains) is a *tracking-stabilization* mode for
  when the whole reference frame (a vehicle) moves — it is **not** position-triggered re-anchoring,
  and it's worth being precise about that so we don't over-claim prior art. Its *relevance* is the
  adjacent idea that the system has a notion of "the user is in motion" and adapts; our geofence is a
  different, complementary motion signal (user translating within the room vs. the room translating
  under the user).
- Meta's MR design guidance explicitly says apps "designed to work in moving vehicles and different
  physical contexts" should **dynamically adjust content based on whether the user is standing,
  sitting, lying down, or in a moving vehicle** — i.e. posture/context-driven re-placement is an
  endorsed pattern, which is the closest first-party blessing of exactly this feature. The comfort
  numbers (±30° comfortable cone, prefer world-anchoring, ~1 m eye-level-or-below) are tabulated in
  research/10 §2.2 and drive the roam offset default here.
  ([Travel use case](https://developers.meta.com/horizon/discover/use-cases/entertainment-travel/),
  [Comfort](https://developers.meta.com/horizon/design/comfort/))

### 2.4 HoloLens tag-along thresholds; game diegetic↔HUD; smart-home follow-me

- **HoloLens / MRTK `Follow`** is literally distance-and-angle-thresholded re-parenting-lite: the
  element doesn't move until the head leaves user-defined bounds, then tags along — the same
  "deadzone then follow" our leash already is, and the conceptual ancestor of a geofence trigger.
  The HoloLens app bar / "tag-along" affordance popularized *threshold-triggered* follow.
- **Game UI diegetic→HUD transitions**: many titles render an element *in the world* (diegetic) when
  the player is near/looking, and promote it to a *screen-locked HUD* when they move away or it must
  stay visible — the exact docked↔roaming duality, and the reason a *smooth* promote/demote (not a
  snap) reads as polished. This is the mental model for "watching a video at my desk vs. carrying it
  through the house."
- **Smart-home "follow-me" audio/video handoff** (Sonos/Chromecast/AirPlay room-to-room, Alexa/Google
  follow-me) is the canonical *trigger-design* analogy: naive proximity handoff **flaps** at room
  boundaries, so every shipping system uses **presence dwell + hysteresis + a minimum hold time**
  before handing off. That is precisely §3's `R_out`/`R_in` gap + `T_out`/`T_in` dwell — designed to
  make "I paced past the doorway once" not yank the monitor off the desk.

---

## 3. Trigger design (the hard part: no flapping, no false positives)

### 3.1 Position source and metric

- **Source**: `in.view.pos` in LOCAL_FLOOR (the head), available every frame. No FOV/`xrLocateViews`
  needed (that was research/10's problem, not this one).
- **Anchor of the geofence**: the **"desk seat" = the head position captured the moment the monitor
  becomes DOCKED with a valid view** (`m_dockHeadPos`). Rationale: the user reasons about "how far am
  I from my desk," not "how far is my head from the floating quad." Using the seat (a) makes *all*
  monitors docked at one desk undock together for free (shared seat ⇒ shared trigger), and (b) is
  robust to the monitor being 1–2 m out in front. Recapture on manual `dock`/`center`/session-focus
  (this inherits the same "captured at first wear" quirk as `bodyHeight`, doc 03 §3.3 — note it).
- **Metric**: **horizontal (XZ) distance** `|(view.pos − m_dockHeadPos).xz|` by default. Standing up
  changes only `y`, so it must **not** count — the user's explicit distinction ("stand up" = stay,
  "walk away" = follow). A config `adaptive_use_height` includes `y` for users who *do* want
  lie-down/stand to trigger.

### 3.2 Hysteresis + dwell (the anti-flap core)

```
DOCKED:   d = horizDist(view.pos, dockHeadPos)
          if d > R_out:  outDwell += dt   else outDwell = 0
          if outDwell >= T_out:  -> begin UNDOCK
ROAMING:  d = horizDist(view.pos, dockHeadPos)     // seat is still the remembered desk seat
          if d < R_in:   inDwell  += dt   else inDwell  = 0
          if inDwell  >= T_in:   -> begin REDOCK
```

- **Two radii, `R_in < R_out`** (default 1.0 / 1.5 m) → a 0.5 m dead band; you must *commit* to
  leaving and *commit* to returning. Pacing in the band changes nothing.
- **Directional dwell**: `T_out` short-ish (400 ms — leaving should feel responsive; you're already
  walking) vs `T_in` longer (800 ms — returning should be deliberate so a walk-through-the-room
  doesn't re-dock onto a desk you're not sitting at). Both are pure `dt` accumulators (gtest-able,
  no clocks).
- Accumulators reset to 0 whenever the condition is momentarily false → a single spurious sample
  can't advance the timer.

### 3.3 Signals considered and where they land

| Signal | Verdict | Notes |
|---|---|---|
| **XZ geofence + dwell + hysteresis** (§3.2) | **v1 default** | Directly models "at my desk vs. walked away"; robust, cheap, headless-testable. |
| **Gaze / facing** | **Not a gate in v1** | User may watch the video *while* walking off ⇒ gaze-away must not suppress undock, and gaze-toward must not force it. Optional *modifier* later: require gaze-at-desk to re-dock (avoids re-docking while merely passing by). Deferred. |
| **Velocity (walk speed)** | **Deferred accelerator** | `|Δview.pos|/dt` sustained above a walk threshold could *shorten* `T_out` ("clearly walking away, undock sooner"). Noisy (head bob); design as an optional multiplier on `T_out`, not a standalone trigger. |
| **Room-scale boundary / scene** | **Unavailable — note it** | WiVRn advertises `XR_EXT_local_floor`, `XR_EXTX_overlay`, `equirect2/cylinder`, hand/interaction (research/04 §strings-probe) but **no `XR_FB_scene` / `XR_MSFT_scene_understanding` / spatial-entity / boundary** extension (grep of runtime strings + docs confirms none wired). So "undock when I cross a room boundary" is **not** implementable via WiVRn today; the geofence around the seat is the substitute. Record this as the reason we don't do room-aware triggers. |
| **Manual verb + pinch override** | **Always present** | `xrmonitor dock/undock` and a pinch gesture are the fallback and the thing the auto-trigger merely automates (visionOS lesson §2.2). Manual action also *recaptures the seat* and short-circuits any dwell. |

---

## 4. State machine + animation design

### 4.1 Representation: orthogonal decorator (not a new mode)

Add to the *persisted* state (a sub-struct of `SXRAnchorState`, or a parallel `SXRAdaptiveConfig`
referenced by the layer):

```cpp
struct SXRAdaptiveConfig {          // persisted (serialized in the xrmonitor line)
    bool          enabled   = false;
    eXRAnchorMode roamMode  = XR_ANCHOR_BODY;   // head or body only
    SXRPose       roamOffset;                    // comfortable follow offset in the roam frame
    bool          carryOffset = false;           // true = capture current offset at undock instead
    // dockPose is just m_state.anchorPose while mode == LOCAL (single source of truth).
};
```

Crucially **`m_state.mode` stays `XR_ANCHOR_LOCAL`** — the desk pose is the persistent identity.
Adaptive is a runtime overlay that, per phase, *chooses which solve to run*:

```cpp
enum eXRAdaptivePhase : uint8_t { XRAD_DOCKED, XRAD_UNDOCKING, XRAD_ROAMING, XRAD_REDOCKING };
```

Runtime (not persisted) added to `CXRAnchor`, all POD:

```cpp
eXRAdaptivePhase m_adPhase = XRAD_DOCKED;
Vec3   m_dockHeadPos;  bool m_dockSeatCaptured = false;
float  m_outDwell = 0, m_inDwell = 0;   // §3.2 accumulators
float  m_adT = 0;                        // transition envelope param [0,1]
SXRPose m_adFrom;                        // frozen `from` world pose for the current transition
```

`solve()` gains a single pre-step before the mode `switch`: if `adaptive.enabled`, call
`adaptiveStep(in, tune, cfg)` which (a) updates the phase machine (§3.2 + §4.2), (b) returns the
world pose to submit for this phase (§4.3), short-circuiting the normal `switch`. The grab override
(`:170`) still returns *first* — a grab always wins over adaptive (§5.1).

Why not a fifth `eXRAnchorMode`? It would (a) force a new case in every `switch`
(`solve`/`reanchorFromWorld`/`applyMove`/`applyRotate`/`applyDistance`/`applyCenter`/`onReferenceSpaceChanged`/`serializeXRMonitorLine`),
(b) need a place to stash *both* the desk pose and the roam offset inside one `anchorPose`, and (c)
muddy persistence. The decorator keeps `local` semantics pristine and reuses head/body wholesale.

### 4.2 Phase machine (with clean interrupts)

```
                geofence-leave (§3.2)  OR  undock verb/pinch
      DOCKED ─────────────────────────────────────────────► UNDOCKING
        ▲                                                        │ envelope t: 0→1
        │ envelope t: 0→1                                        ▼
     REDOCKING ◄───────────────────────────────────────────  ROAMING
        ▲         geofence-return (§3.2)  OR  dock verb/pinch     │
        └─────────────────────── (grab release near dock, opt) ──┘

Interrupts:
  • Reverse mid-transition (leave then quickly return, or vice-versa):
      UNDOCKING ⇄ REDOCKING. Re-freeze m_adFrom = current blended pose, retarget, keep m_adT
      but REMAP it (t' = 1 − t so progress already made isn't thrown away). No snap.
  • Grab at any phase: freeze the phase, suspend the geofence, run the grab override. On release,
      §5.1 picks the resumed phase/representation.
  • Tracking loss (viewValid == false): freeze accumulators + envelope, hold at m_lastWorld
      (mirrors OpenXRManager.cpp:917 and §3.4 device-loss); resume on recovery.
```

Terminal edges (`UNDOCKING→ROAMING`, `REDOCKING→DOCKED`) fire once `m_adT ≥ 1`; that is the
**definitive** edge that emits the IPC event and triggers 2D relayout (§5.3), *not* the begin edge —
so a reversed/aborted transition never emits a spurious "undocked."

### 4.3 The transition envelope (the "pleasant animation")

A pure blend between a **frozen `from`** and a **live `to`** world pose:

```
adaptiveStep():
  advance:  m_adT = envAdvance(m_adT, dt, transitionSec)     // copy chromeFadeAdvance shape
  e       = ease(m_adT)                                       // smoothstep default (§ config)
  switch phase:
    UNDOCKING:  to = solveLeash(roamMode, roamOffset, in, tune)   // LIVE: moves with the head
                W  = lerpPose(m_adFrom, to, e)                     // desk → catch-up-to-head
                if m_adT>=1: phase=ROAMING   (spring already settled at roamOffset ⇒ no kick)
    REDOCKING:  to = dockPose                                       // STATIC saved desk pose
                W  = lerpPose(m_adFrom, to, e)
                if m_adT>=1: phase=DOCKED; m_state.anchorPose=dockPose  // reassert exact desk pose
    DOCKED:     W = dockPose                                        // = m_state.anchorPose (LOCAL)
    ROAMING:    W = solveLeash(roamMode, roamOffset, in, tune)      // normal leash, bounding on
  res.space = XR_SPACE_LOCAL_FLOOR; res.pose = res.worldPose = W; m_lastWorld = W
```

Design points:

- **`from` is frozen, `to` may be live.** Undock's `to` is the *live* roam target — because if you
  undock *while walking*, the destination is head/body-anchored and moving; blending desk→live gives
  the "peel off the desk and glide to catch your head" motion the user asked for. Re-dock's `to` is
  the static desk pose (you've returned and are roughly stationary), so freezing `from` at the roam
  pose and easing to the desk pose is clean.
- **Envelope, not leash spring.** The pose change desk→eye-level is *large and deliberate*; a
  fixed-duration eased envelope reads as an intentional "the window is moving to follow you," whereas
  the critically-damped leash (tuned for tiny lazy-follow corrections) would feel either sluggish or
  abrupt and has no taste knob. We keep the *leash* for the steady-state ROAM follow; the *envelope*
  owns the handoff. (This is the deliberate deviation from MRTK's single-SmoothTo, §2.1.)
- **Reuse `lerpPose`** (already in `XRAnchor.cpp:11`, linear pos + `qSlerp` rot) — no new math.
- **`envAdvance`** = `chromeFadeAdvance` with the "grace/hide" terms dropped: ramp `t` toward 1 at
  `dt/transitionSec`, snap if `transitionSec ≤ 0`, hold if `dt ≤ 0`. Easing (`ease(t)`) is a separate
  pure fn (`linear` | `smoothstep` = `t*t*(3−2t)` | `ease_out`) applied at sample time, so the
  *parameter* stays linear and reversible (§4.2 remap is just `1−t`).
- **Seed continuity at hand-off into ROAM**: at `UNDOCKING→ROAMING`, the spring must already sit at
  the settled roam pose so it doesn't kick. Achieve by seeding `solveLeash`'s spring to
  `T = compose(view, roamOffset)` at *undock begin* (i.e. treat roam as already-latched at its
  target), so the *envelope* — not the spring — carries the visible motion; the spring only takes
  over the frame-to-frame lazy follow after `t=1`.
- **Frame-thread purity**: everything is POD + `dt`; zero hyprutils refcount ops → satisfies the
  `XRMonitorLayer.hpp` rule by construction, gtest-covered like the rest of the anchor engine.

### 4.4 Which roam mode + which offset

- **Roam mode: `body` default** (yaw-only, holds a comfortable height while you walk, no
  pitch-chasing when you look around the house — research/10 §1.4/§2 comfort). `head` available
  (`roam:head`) for users who want it pinned to gaze. `device` is nonsensical for adaptive (excluded).
- **Roam offset: configured comfortable `roam_offset` default, carry-current optional.** The desk
  pose is low and close (a desk); dragging *that* pose around the house at knee height is wrong — the
  handoff should *move it* to eye-level-ish ~1.2–1.4 m in front (Meta comfort), which is precisely
  why the animated transition exists. `adaptive_carry_offset = true` captures the current
  head-relative offset at undock for users who prefer positional continuity over comfort.

---

## 5. Interactions to analyze

### 5.1 Grabs (per phase)

The grab override returns before the mode dispatch (`:170`), so a grabbed adaptive monitor is never
geofenced/transitioned while held (correct — the user is manipulating it). The subtlety is the
**release representation**, driven by the phase at grab-begin:

- **Grabbed while DOCKED → release reanchors LOCAL, redefining `dockPose`** ("I moved my desk
  monitor"). Also recapture the seat? No — moving the *monitor* shouldn't move the *seat*; leave
  `m_dockHeadPos` unless a `dock` verb runs.
- **Grabbed while ROAMING → release reanchors into `roamMode`, updating `roamOffset`** (the released
  spot becomes the new follow offset). This needs `endGrab`/`reanchorFromWorld` to target the *roam*
  mode, not `m_state.mode` (LOCAL) — so the manager selects the reanchor mode from the adaptive phase
  before calling it (a small parameterization of `endGrab`, composes with `pickReleasePose` /
  research/04 latch untouched).
- **Grabbed while roaming, released *near the dock*** → **stay ROAMING by default** (the geofence
  owns docking; release just repositions the follower, and the dwell will redock if you settle).
  Predictable, no surprise snap. Offer `adaptive_grab_snaps_dock = true` for "release inside `R_in`
  ⇒ immediately begin REDOCK" (Quest magnetic-snap feel). Note both.
- **Grabbed mid-transition** → cancel the envelope, freeze into the grab; on release resolve to
  DOCKED or ROAMING per the release pose vs the geofence (whichever side of `R_in`/`R_out` the head
  is on), then let the machine settle. Simplest rule: post-release, re-evaluate §3.2 from the current
  head distance with dwell reset.

### 5.2 Bounding (research/10) — ROAM only

The cone clamp applies to the *roam target* and must be **gated on `phase == XRAD_ROAMING`**: DOCKED
is world-fixed (no cone), and during UNDOCKING/REDOCKING the *envelope* owns the pose (clamping the
live `to` mid-blend would fight the eased path). Since roam runs through `solveLeash`, gate the clamp
there on a `boundActive = (phase==ROAMING)` flag. This composes cleanly — research/10 already gates
its clamp on `!grabbed()`; add the phase gate alongside.

### 5.3 2D spatial layout (research/12) — recompute on definitive edges

Dock/undock changes a monitor's effective frame (world-fixed desk pose vs. follow-frame offset),
which is exactly a research/12 recompute trigger. Fire `syncLayout2D()` (debounced) on the
**terminal** `xrmonitordocked`/`xrmonitorundocked` edges only — never per-frame, never mid-transition
(the quad is visibly moving; the invisible 2D plane shouldn't churn). research/12's projection should
read the monitor in its *settled* representation: docked ⇒ world azimuth in the desk reference frame;
roaming ⇒ follow-frame angle (§12 §3 already handles both). The adaptive edge is a natural
"re-latch my desk reference frame" moment too (like `center`/recenter).

### 5.4 Auto-layout (research/08) — reserve the slot

An adaptive monitor that belongs to a world arc/cluster and undocks is "leaving its slot." Default
policy: **reserve (keep-gap)** so re-dock returns it to the same slot, rather than compacting
neighbors in and having them shuffle back out on re-dock (thrash). This wants an
`XR_SLOT_RESERVED_ADAPTIVE` transient state in research/08's slot model. Simpler v1 stance: **adaptive
monitors are excluded from auto-layout membership** (they own an explicit `dockPose` + `roam_offset`)
— revisit once research/08 lands. Note the dependency; don't block A1–A4 on it.

### 5.5 Multiple adaptive monitors

- **Per-monitor independence in v1** — each `CXRAnchor` runs its own machine. Monitors docked at the
  *same desk* share the seat position (each captured its own `m_dockHeadPos`, but they'll be ~equal),
  so they **undock/redock together for free** without a group concept. Different desks (different
  seats) undock independently — also correct.
- **A group/scene concept** (dock/undock a named *set* as one unit, a shared seat, a single
  transition envelope for the cluster) is **future work**, and is the natural place research/08's
  shared parent transform would slot in (undock the whole follow-cluster together). Note it; don't
  build it in v1.

### 5.6 Recenter / reference-space change

`onReferenceSpaceChanged` (`:643`) already re-expresses LOCAL anchors across a runtime recenter — so
`dockPose` (a LOCAL pose) is handled. Add: re-express `m_dockHeadPos` by `inv(M)` too (it's a
LOCAL_FLOOR position), and if mid-transition, re-express `m_adFrom`. Roam offsets are view/body-frame
(unaffected), matching the existing HEAD/BODY handling.

---

## 6. Config surface sketch

### 6.1 Global thresholds (declare in `ConfigValues.cpp`, next to `openxr:leash_*`; per-frame via `readAnchorTuning()` ⇒ hot-tune)

```
openxr:adaptive_leave_radius     = 1.5    # m, XZ dist from desk seat to begin undock  (R_out)
openxr:adaptive_return_radius    = 1.0    # m, XZ dist to begin redock (R_in < R_out; hysteresis)
openxr:adaptive_leave_dwell_ms   = 400    # must exceed R_out this long before undocking (T_out)
openxr:adaptive_return_dwell_ms  = 800    # must be within R_in this long before redocking (T_in)
openxr:adaptive_transition_ms    = 700    # dock<->roam blend duration (research 04/07 chrome ~150 is too fast for a whole-window move)
openxr:adaptive_transition_ease  = smoothstep   # linear | smoothstep | ease_out
openxr:adaptive_roam_mode        = body   # head | body   (default roam behavior)
openxr:adaptive_use_height       = false  # include Y in the geofence distance (true = lying down/standing can trigger)
openxr:adaptive_carry_offset     = false  # roam at the offset captured on undock instead of the configured roam offset
openxr:adaptive_grab_snaps_dock  = false  # release a roaming grab inside R_in => immediate redock
```

`SXRAnchorTuning` grows the corresponding fields (`adLeaveR, adReturnR, adLeaveDwellSec,
adReturnDwellSec, adTransitionSec, adEase, adRoamMode, adUseHeight, adCarryOffset, adGrabSnaps`).

### 6.2 Per-monitor `xrmonitor` grammar (orthogonal tokens on the existing anchor-spec)

The persistent anchor stays `anchor:local` (the desk pose); adaptive tokens decorate it:

```
xrmonitor = NAME, WxH@Hz, anchor:local pos:x,y,z yaw:Y, size:W,
            adaptive:on [roam:head|body] [roam_offset:x,y,z] [roam_yaw:Y]
            [leave:R_out] [return:R_in] [carry:on|off]

# example: a desk video screen that follows me around the house as a body-leashed panel
xrmonitor = XR-video, 1920x1080@60, anchor:local pos:0.3,1.05,-1.2 yaw:0, size:1.8,
            adaptive:on roam:body roam_offset:0,1.35,-1.2 roam_yaw:0
```

Per-monitor tokens override the global default for that monitor (research/10's per-monitor-vs-global
precedent). `roam_offset`/`roam_yaw` serialize the follow offset; absent ⇒ use
`openxr:default_distance` centered at a comfortable height.

### 6.3 Verbs / dispatchers (`xrmonitor …`, one impl two transports per doc 05)

| Verb | Args | Semantics |
|---|---|---|
| `xrmonitor adaptive` | `on\|off\|toggle` | Enable/disable the decorator on the selected monitor (recaptures seat on enable). |
| `xrmonitor undock` | — | Force UNDOCKING now (skips dwell); begins the envelope to roam. |
| `xrmonitor dock` | — | Force REDOCKING now; recaptures `dockPose = current desk pose`? No — dock returns to the *saved* `dockPose`; use `dock here` to redefine it to the current world pose. |
| `xrmonitor dock-here` | — | Set `dockPose = m_lastWorld` and recapture the seat (redefine the desk). |
| `xrmonitor roam` | `head\|body` | Change the roam mode live. |

A **pinch gesture** (hand tracking, already wired for grab, WP-G5) is the always-available manual
undock/dock toggle (research/04 pinch pose) — the "grab it off the desk / push it back" affordance.

### 6.4 IPC / events / status / serialization

- **socket2 events** (main thread, from the atomic edge-detect): `xrmonitorundocked <name>` on the
  `UNDOCKING→ROAMING` terminal edge, `xrmonitordocked <name>` on `REDOCKING→DOCKED`. Bars can show a
  "following" glyph. (Optionally `xrmonitorroaming`/mid-transition events — deferred; keep the event
  surface to the two definitive edges to avoid flap noise.)
- **`hyprctl openxr status -j`** gains per-monitor `adaptive: { enabled, phase:
  docked|undocking|roaming|redocking, roamMode, dockRadiusM, seatDistM, transitionT }`.
- **`hyprctl openxr layout` serialization must emit the SAVED dockPose**, not the live roam pose:
  `serializeXRMonitorLine` (`XRMonitorConfig.cpp:286`) already takes `anchor` + `pose` explicitly;
  for an adaptive monitor pass `dockPose` (and mode LOCAL) regardless of runtime phase, then append
  the `adaptive:on roam:…` tokens. This means a save-while-roaming round-trips to the desk pose — the
  correct persistent identity. The seat position and live phase are *not* serialized (ephemeral,
  recaptured on load — the `bodyHeight` quirk again).

Note the legacy `hyprctl keyword` hot-toggle caveat from MEMORY/doc 05: if any `openxr:adaptive_*`
must apply *without* a full reload under the hyprlang path, it needs the `parseKeyword` special-case
(like `openxr:enabled`/`inhibit_idle`); the per-frame `readAnchorTuning()` read makes most of them
live already, matching the leash/grab-filter vars.

---

## 7. Work packages (one-subagent-sized)

- **WP-A1 — pure phase machine + geofence + envelope + `solveLeash` refactor · S/M · no deps ·
  headless.** Lift HEAD/BODY solve bodies into `solveLeash(mode,O,in,tune)`; add `SXRAdaptiveConfig`,
  the `eXRAdaptivePhase` runtime state, §3.2 dwell/hysteresis, §4.3 envelope (`envAdvance` + `ease`),
  seat capture, reverse-interrupt remap. gtests in `tests/xr/`: scripted head-position sequence
  (walk out past `R_out`, hold `T_out` ⇒ UNDOCK; hover in the dead band ⇒ no change; return inside
  `R_in`, hold `T_in` ⇒ REDOCK); transition blend monotone + ends exactly on `dockPose`/roam target;
  reverse mid-transition ⇒ no snap and `t` remapped; `use_height` toggles standing-triggers;
  `enabled=false` bit-identical to today. Fully deterministic (dt-driven), no runtime.

- **WP-A2 — config vars + grammar + serialization · S · dep A1.** `openxr:adaptive_*` in
  `ConfigValues.cpp`; `SXRAnchorTuning` fields + `readAnchorTuning()`; `adaptive:/roam:/roam_offset:`
  tokens in `parseXRMonitorLine`; `serializeXRMonitorLine` emits `dockPose` + tokens (§6.4);
  parser/serializer gtests (round-trip: adaptive line → params → line).

- **WP-A3 — manager wiring + events + verbs + pinch · M · dep A1,A2.** `adaptiveStep` called in the
  frame-loop solve; `std::atomic<uint8_t> m_adPhase` on `CXRMonitorLayer` (frame write) →
  main-thread edge-detect → `xrmonitordocked`/`xrmonitorundocked` + status JSON; `xrmonitor
  adaptive/undock/dock/dock-here/roam` verbs (both transports); pinch-toggle hook (WP-G5 pose).
  Live-verifiable via `preview-xr.sh --wivrn`.

- **WP-A4 — grab-release policy + bounding gate · S/M · dep A1, research/04, research/10.**
  Parameterize `endGrab`/reanchor to target the phase's mode (§5.1); stay-roaming default +
  `grab_snaps_dock` option; gate the research/10 cone clamp on `phase==ROAMING` (§5.2);
  `onReferenceSpaceChanged` re-expresses `m_dockHeadPos`/`m_adFrom` (§5.6). gtests for each release
  path.

- **WP-A5 — 2D-relayout + auto-layout coexistence · M · dep A3, research/12 (and research/08 if
  landed).** Fire `syncLayout2D()` on the terminal dock/undock edges (§5.3); slot-reserve policy or
  explicit exclusion for auto-layout members (§5.4).

- **WP-A6 — hyprtester `--xr` geofence integration test · S · dep A3 · headless.** Drive the head
  pose out/in via the Monado remote driver (`r_head_data`, MEMORY wire ABI) across the radii, assert
  `xrmonitorundocked`/`xrmonitordocked` events and the monitor's world pose landing at
  roam-offset / dock-pose. This is the payoff of keeping the machine pure + `dt`-driven: the whole
  trigger is scriptable without a headset.

**Headless vs live.** A1/A2/A6 are fully headless (pure gtests + scripted remote-driver head walk).
A3/A4/A5 are wire-and-test headless but the **feel** — `R_out`/`R_in`/dwell defaults, the 700 ms
easing taste, body-vs-head roam comfort — needs **live Quest wandering the house** (`preview-xr.sh
--wivrn`); the config vars are hot-tunable so that iteration is fast.

---

## 8. Open questions for the user (taste + scope)

1. **Radii + dwell defaults.** `R_out=1.5 m / R_in=1.0 m / T_out=400 ms / T_in=800 ms` — do these
   match your desk geometry (how far do you get before you want it to follow)? The dead band and the
   asymmetric dwell are the anti-flap knobs; all four are hot-tunable.
2. **Roam mode default — `body` or `head`?** Recommendation `body` (yaw-only, fixed comfortable
   height while walking; research/10 comfort). `head` pins it to gaze. Which for the video-follow case?
3. **Roam offset — snap to a comfortable eye-level `roam_offset` (recommended) or carry the desk
   offset (`carry:on`)?** The desk pose is low/close; following you at that pose through the house is
   probably wrong, hence the animated move — but confirm.
4. **Transition duration + easing.** `700 ms smoothstep` is the starting taste; slower/faster? Any
   slight overshoot wanted (would need the `Momentum`-style spring instead of a pure ease)?
5. **XZ-only distance (standing up = stay docked) vs. include height.** Recommendation XZ-only per
   your "stand up then walk away" phrasing; confirm standing/leaning should never undock.
6. **Gaze modifier — leave it out (v1) or add "only re-dock when looking toward the desk"?** Your
   example implies undock should ignore gaze (watch while walking); is a gaze gate wanted for
   *re-dock* precision?
7. **Grab-release near dock — stay roaming (geofence owns docking, recommended) or magnetic-snap
   dock?**
8. **Group/scene undock (multiple monitors move as a set with a shared seat) — needed soon, or is
   per-monitor independence (which already undocks same-desk monitors together) enough for v1?**
9. **Auto-layout (research/08) interplay — reserve the slot on undock, or exclude adaptive monitors
   from layouts entirely in v1?** (Only matters once research/08 lands.)
```
