# 10 — View Bounding for Head/Body-Anchored Monitors (research, no implementation)

Status: research only. No code changed, no live runs, no commits. Author: research pass 2026-07-08.

The ask (user's words): *"Bounding positioning for head/body-anchored monitors so they can't
drift out of view."* Failure mode: deadzones + springs + user-set offsets let a head/body-leashed
monitor end up — or lag — outside the visible field of view, leaving a "lost" follower monitor
that follows you forever from somewhere you can't see.

Cross-refs: `docs/openxr/03-anchoring.md` (leash spec), `research/04-grabbable-borders.md`
(release latch we must compose with), `research/07-premium-chrome.md` (chrome system, for the
edge-indicator option), `research/03-monitor-grids.md` (grid anchors would inherit whatever we do
here). Source of truth: `src/openxr/XRAnchor.{hpp,cpp}`, `src/openxr/OpenXRManager.cpp` frame
loop, `src/config/values/ConfigValues.cpp:718-721`.

---

## TL;DR

1. **Five distinct ways a head/body monitor leaves the FOV today** (§1): unbounded configured/
   grab-set offsets (the big one — nothing anywhere clamps offset *direction*, only distance and
   width), deadzone hysteresis parking the quad up to `leash_deadzone_angle` off-target forever,
   spring lag ≈ `Ω·leash_response` during sustained rotation (transient), body mode ignoring
   pitch *by design* (look up ⇒ gone), and body height staying fixed when you sit/stand
   (`body_leash_follow_height=0` default).
2. **The canonical prior art is MRTK's Follow/RadialView solvers** (§2): clamp the *goal* onto a
   max-view-degrees cone around the view forward, then let the normal smoothing chase the clamped
   goal. Real defaults: RadialView `maxViewDegrees = 30°`; Follow `maxViewHorizontalDegrees = 30°`,
   `maxViewVerticalDegrees = 20°`, `reorientWhenOutsideParameters = true`. Comfort literature:
   ±30° is the "no head movement" comfortable cone (Meta), optimal vertical zone 0–35° below
   horizon and neck rotation ≤ 45° off-center (Microsoft).
3. **Recommendation (§3): MRTK-style cone clamp on the solve TARGET (`T.pos`), pre-deadzone,
   never touching `m_state.anchorPose`.** Head mode: full yaw+pitch cone. Body mode: yaw clamp
   always + a *soft pitch recall* (body ignoring pitch is a feature; only rescue it after the quad
   has been out of the vertical band for `recall_ms`). Grabs need zero special-casing — the grab
   override returns before the HEAD/BODY branches, and release re-anchoring naturally feeds the
   next frame's clamp. Bounding is a pure runtime constraint: offsets serialize exactly as
   configured, zero persistence changes.
4. **FOV source**: the frame loop has *no* `xrLocateViews` today (verified — only
   `xrLocateSpace(viewSpace…)`, `OpenXRManager.cpp:843`). Add one `xrLocateViews` per frame next
   to it; `XrView::fov` gives per-eye asymmetric half-angles; effective bound =
   `min(config_max_angle, fov_half_angle − margin)`. Config values alone (no FOV) are a fine v1.
5. **Two small work packages** (§5): WP-B1 pure-math clamp + tuning plumbing + gtests (S);
   WP-B2 FOV capture, soft recall timing, body pitch band, IPC status surfacing (S/M). Optional
   WP-B3 edge indicator rides research/07's chrome plan.

---

## 1. Drift-mechanism analysis — how a leashed monitor gets lost today

The head/body solve is `CXRAnchor::solve()` (`src/openxr/XRAnchor.cpp:159-334`), tuned by
`SXRAnchorTuning` (`src/openxr/XRAnchor.hpp:144-150`) read per-frame in
`COpenXRManager::readAnchorTuning()` (`src/openxr/OpenXRManager.cpp:1586-1600`) from the
`openxr:leash_*` vars (`src/config/values/ConfigValues.cpp:718-721`; defaults: response 0.35 s,
deadzone 15°, 0.25 m).

### 1.1 Unbounded offset eccentricity (persistent — the core bug class)

The head-mode target is simply `T = poseCompose(in.view, O)` with `O = m_state.anchorPose`, the
raw view-space offset (`XRAnchor.cpp:220-221`). **Nothing constrains the *direction* of `O`**:

- **Config**: the `xrmonitor` keyword accepts any `offset:x,y,z` (`src/openxr/XRMonitorConfig.cpp:164-167`);
  the only validation is "is it a vec3". `offset:0,0,2` (behind the head, +Z) is accepted and
  produces a monitor that is *never* visible yet follows every head motion.
- **Verbs**: `applyMove` does an unbounded `anchorPose.pos += D_view` (`XRAnchor.cpp:504-506`);
  repeated `xrmonitor move` calls walk the offset to arbitrary eccentricity. `applyDistance`
  clamps the *length* to 0.3–5.0 m but never the direction (`XRAnchor.cpp:552-584`).
  `applyRotate` on HEAD orbits the offset around the head by any angle (`XRAnchor.cpp:533-536`)
  — `xrmonitor rotate 120` deliberately parks it far outside the FOV.
- **Grabs**: on release, `reanchorFromWorld` re-expresses wherever the quad was carried into a new
  view-space offset: `anchorPose = poseCompose(poseInverse(ctx.view), W)` (`XRAnchor.cpp:457-465`,
  body variant `:467-478`). Grab a head-leashed monitor, park it 90° to your left, release: the
  offset is now "90° left of wherever I look", permanently peripheral. This is the most likely
  accidental path — the user *thought* they were parking it in the world.

Eccentricity of a head offset: `θ = angle between (0,0,−1) and O.pos` in view space. With
`offset:1.2,0,-0.8`, θ ≈ 56° — outside every headset's comfortable cone (§2) even before deadzone.

### 1.2 Deadzone hysteresis parks the quad off-target after a turn (persistent, bounded)

The latch machine (`XRAnchor.cpp:239-249`, spec doc 03 §3.2) only starts chasing when
`angDev > deadzoneAngleRad` **or** `posDev > deadzoneDistance`, and while LATCHED the position is
frozen. So after any head turn that ends with the quad displaced *just under* the thresholds, the
quad rests up to `leash_deadzone_angle` (default **15°**) away from its configured spot —
*forever* (it re-latches at `XR_LEASH_SETTLE_POS = 0.01 m` of the target, `XRAnchor.hpp:53`,
`XRAnchor.cpp:244-247`, but only re-chases when the threshold is crossed again). Combined with a
legitimately-eccentric offset (say 30°), the resting eccentricity is up to offset + deadzone ≈ 45°
— at or past the FOV edge of most HMDs (Quest 3 ≈ 55° half-angle horizontal, less vertically).
This is exactly why any bound must account for the deadzone: **resting eccentricity ≤ clamp cone +
deadzone angle** if the clamp is applied to the target (§3.1).

### 1.3 Spring lag during sustained rotation (transient, unbounded during the motion)

Once CHASING, the critically-damped spring (`springStep`, `XRAnchor.cpp:85-92`) tracks a target
that *keeps moving* while the head rotates. For a ramp (target moving at speed `v`) a critically
damped follower with `ω = 2/response` has steady-state lag `2v/ω = v·response`. In angular terms
at offset radius `r`: **lag ≈ Ω · leash_response**. A brisk 180°-in-one-second turn (Ω ≈ 180°/s)
with the default 0.35 s response lags ~60–65° — the monitor is fully out of view *during* the
turn and takes ~`2.4 · response ≈ 0.85 s` to re-converge after it stops (doc 03 §3.2 envelope
math). This is transient and self-healing; a target-side clamp does **not** remove it (only an
output-side clamp would, at the cost of yanking — §3.1). Prior art (MRTK) accepts this transient.

### 1.4 Body mode ignores pitch by design (persistent whenever you look up/down)

`computeBodyFrame` flattens the frame to yaw-only: `bf.rot = qFromYaw(m_lastYaw)`
(`XRAnchor.cpp:139-157`, spec doc 03 §3.3), and the body-mode deadzone is deliberately
*distance-dominant* — "head pitch … must not wake the quad" (doc 03 §3.3; `angDev` at
`XRAnchor.cpp:279` is a yaw-only difference). Consequences:

- Look up at the ceiling, recline in a chair, lie down: the quad stays on its yaw ring at
  `bodyHeight` — completely out of view, and no amount of *pitch* will ever bring it back.
- The yaw-hold hysteresis (`XR_BODY_YAW_HOLD = 0.15` / `RESUME = 0.25`, `XRAnchor.hpp:54-55`,
  `XRAnchor.cpp:143-150`) freezes `m_lastYaw` while gazing near-vertical; turn 180° while looking
  down (picking something up) and until `horiz > 0.25` the quad is *behind* you. Transient
  (recovers on level-off) but disorienting.
- Height: with `body_leash_follow_height = 0` (default, `ConfigValues.cpp:721`), `bodyHeight` is
  captured once (`XRAnchor.cpp:262-265`) — sit on the floor, or stand after configuring seated,
  and the quad sits above/below the vertical FOV at typical 1–1.5 m distances (a 40 cm height
  mismatch at 1.5 m is ~15° of elevation error, on top of everything else).

### 1.5 View-tracking loss holds the last world pose (benign, listed for completeness)

With no valid view pose the frame loop holds the quad at `lastWorld()`
(`OpenXRManager.cpp:917-925`); on recovery the spring is still seeded from the last world pos
(`XRAnchor.cpp:223-229`), so the quad glides back. Not a drift source, but the bounding code must
tolerate `viewValid == false` frames (do nothing — no view means no cone).

**Summary table**

| # | Mechanism | Persistent? | Today's bound | Code |
|---|---|---|---|---|
| 1 | Offset direction unbounded (config / verbs / grab release) | yes | none | `XRAnchor.cpp:221,457-478,504-506,533-536`; `XRMonitorConfig.cpp:164-167` |
| 2 | Deadzone hysteresis rest offset | yes | `leash_deadzone_angle` (15°) *on top of* #1 | `XRAnchor.cpp:239-249` |
| 3 | Spring lag under sustained rotation | transient | ≈ Ω·response, unbounded mid-turn | `XRAnchor.cpp:85-92,242-249` |
| 4 | Body pitch-blindness + height mismatch + yaw hold | yes (until level gaze) | none vertically | `XRAnchor.cpp:139-157,262-265,279` |
| 5 | Tracking loss park | until recovery | n/a | `OpenXRManager.cpp:917-925` |

---

## 2. Prior art

### 2.1 MRTK (HoloLens) — the canonical view-bounded follower

MRTK's *solvers* are the reference implementation of "tag-along" UI; both relevant solvers clamp
the **goal** each update and hand it to the shared Solver base, which smooths toward the goal
(`moveLerpTime`, default 0.1 s) — i.e. **clamp the target, keep the spring smooth**, precisely the
composition our leash needs.

- **RadialView** ("keeps a portion of the object within the view frustum"): actual serialized
  defaults from `RadialView.cs` (MRTK 2.x, `Assets/MRTK/SDK/Features/Utilities/Solvers/`):
  `minViewDegrees = 0`, **`maxViewDegrees = 30`**, `aspectV = 1` (vertical = horizontal ×
  aspectV), `minDistance = 1 m`, `maxDistance = 2 m`, `referenceDirection = FacingWorldUp` (yaw
  from head, roll ignored — the same "no roll" convention as our `lookAtNoRoll`). Algorithm:
  compute the element's angle off the view forward, clamp between min/max view degrees,
  reposition on the clamped direction at the (distance-clamped) radius.
- **Follow** (`Follow.cs`, graduated from experimental in MRTK 2.4): **`maxViewHorizontalDegrees
  = 30`**, **`maxViewVerticalDegrees = 20`**, `minDistance = 0.3`, `defaultDistance = 0.7`,
  `maxDistance = 0.9`, **`reorientWhenOutsideParameters = true`** (only re-orient once outside the
  bounds — a deadzone, like ours), `orientToControllerDeadzoneDegrees = 60`, `angularClampMode =
  ViewDegrees` (alternatives: renderer/collider bounds — clamp until the *edge* of the object is
  at the FOV edge, not its center; relevant for our 1.6 m-wide quads, see §3.1 "center vs edge"),
  `tetherAngleSteps = 6` (optional angle quantization so the object "tethers" at discrete
  positions). AngularClamp corrects the *direction* vector onto the cone; it does not hard-set
  the rendered pose.
- MRTK3 keeps both solvers with the same semantics (Solvers doc, MRTK3 spatial-manipulation
  package).

Sources: [MRTK Solver overview (MRTK2)](https://learn.microsoft.com/en-us/windows/mixed-reality/mrtk-unity/mrtk2/features/ux-building-blocks/solvers/solver?view=mrtkunity-2022-05),
[MRTK3 Solvers](https://learn.microsoft.com/en-us/windows/mixed-reality/mrtk-unity/mrtk3-spatialmanipulation/packages/spatialmanipulation/solvers/solver),
[RadialView API](https://learn.microsoft.com/en-us/dotnet/api/microsoft.mixedreality.toolkit.utilities.solvers.radialview?view=mixed-reality-toolkit-unity-2020-dotnet-2.8.0),
[Follow API](https://learn.microsoft.com/en-us/dotnet/api/microsoft.mixedreality.toolkit.utilities.solvers.follow?view=mixed-reality-toolkit-unity-2020-dotnet-2.8.0),
[Follow solver PR #6981](https://github.com/microsoft/MixedRealityToolkit-Unity/pull/6981)
(defaults read from the MRTK GitHub sources, `RadialView.cs` / `Follow.cs`, main branch).

### 2.2 Comfort-angle numbers (what "in view" should mean)

- **Microsoft Mixed Reality comfort guidance**: avoid gaze angles > 10° *above* the horizon and
  > 60° below; the **optimal vertical zone is 0–35° below horizon**; avoid neck rotations > 45°
  off-center; place content 1.25–5 m away, converging near 2 m.
  ([Comfort — Mixed Reality](https://learn.microsoft.com/en-us/windows/mixed-reality/design/comfort))
- **Meta Horizon OS design guidance**: roughly a **60° comfortable cone without head movement
  (±30° from center)**; content at the peripheral FOV forces head/neck motion and "can increase
  visual and physical discomfort"; menus/GUIs comfortable at ~1 m, eye level or slightly below;
  prefer world-anchoring over head-following.
  ([Comfort](https://developers.meta.com/horizon/design/comfort/),
  [Key considerations](https://developers.meta.com/horizon/design/mr-design-guideline/),
  [Layouts](https://developers.meta.com/horizon/design/styles_layouts/))
- **visionOS / Apple HIG**: don't rigidly head-anchor content at all — "anchoring content so that
  it remains statically in front of someone can make them feel stuck, confined, and
  uncomfortable"; prefer space-anchored content, and when something must follow, follow *lazily*
  (the WWDC23 "Design considerations for vision and motion" session coined the "lazy follow"
  pattern — delayed, smoothed re-positioning rather than rigid head-lock; our deadzone+spring
  leash is already exactly a lazy follow).
  ([Spatial layout HIG](https://developer.apple.com/design/human-interface-guidelines/spatial-layout),
  [Ornaments HIG](https://developer.apple.com/design/human-interface-guidelines/ornaments) —
  visionOS 2 volume ornaments *relocate to the side the user is on*, i.e. even Apple's follow
  affordances are recall-based, not rigid)
- **Cockpit HUD practice**: aviation HUD total FOVs cluster around **24–30°** — the working
  assumption that symbology must live within ~±12–15° of the boresight to be read without head
  movement ([SKYbrary HUD](https://skybrary.aero/articles/head-display-hud),
  [Radiant Vision Systems on HUD quality](https://www.radiantvisionsystems.com/blog/quality-considerations-aviation-head-displays-huds)).
  Vision-science background for the same numbers: reading is a foveal task; acuity at 10°
  eccentricity is roughly a fifth of foveal, so text farther than ~10–15° from fixation demands an
  eye/head movement — eccentricity beyond the comfortable eye-rotation range (~±20°) demands a
  *neck* movement. This is why every source above lands in the 20–35° half-angle band.
- **Distinction worth keeping** (Meta + Apple agree): *fixed HUD* (rigid head-lock, zero lag) is
  for tiny reticles only; *lazy follow* (our leash) is correct for windows; view *bounding* is a
  constraint on lazy follow, not a switch to fixed HUD.

Takeaway defaults: a **~30–40° half-angle horizontal, ~20–30° vertical** clamp cone is defensible
from four independent sources; vertical should be biased *downward* (optimal zone below horizon).
Our quads are large (default 1.6 m at 1.5 m ⇒ the quad itself subtends ~56° × ~33°), so clamping
the *center* at 30° still leaves half the monitor within 2° of center-FOV; a wider default than
MRTK's 30° is reasonable for us (§4).

---

## 3. Design options

All three options share plumbing: new tuning fields in `SXRAnchorTuning` (pure math preserved —
`XRAnchor.{hpp,cpp}` stay OpenXR-free and gtest-able, doc 03 §0 hard constraint), read in
`readAnchorTuning()` (`OpenXRManager.cpp:1586`), applied inside `solve()`'s HEAD/BODY branches.

**FOV source.** The frame loop today locates only the VIEW *pose* —
`xrLocateSpace(m_session->m_viewSpace, m_session->m_refSpace, fs.predictedDisplayTime, …)`
(`OpenXRManager.cpp:842-849`); **there is no `xrLocateViews` call anywhere in `src/openxr/`**
(grep-verified), because quad layers never needed per-eye projections. To bound against the *real*
usable cone, add one `xrLocateViews(XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, refSpace,
predictedDisplayTime)` per frame right next to that locate; each `XrView::fov` is an
`XrFovf{angleLeft, angleRight, angleUp, angleDown}` of signed half-angles (asymmetric per eye).
Binocular usable half-angles: `halfH = max(view[1].fov.angleRight, view[0].fov.angleRight)` /
`|min(view[0].fov.angleLeft, …)|` (outer edges), `halfV` likewise from up/down. Feed
`min(config_angle, fovHalf − fov_margin)` into the tuning struct. FOV is near-constant per
session, so caching one located value and refreshing opportunistically is fine; per-frame costs
nothing extra since we're already inside the frame. v1 can ship config-only (no FOV query) —
the comfort literature numbers are far inside any HMD's FOV anyway.

### 3.1 Option A — cone clamp on the solve TARGET (recommended for head; yaw part of body)

MRTK's approach mapped onto our leash. In the HEAD branch, immediately after computing
`T = poseCompose(in.view, O)` (`XRAnchor.cpp:221`) and **before** the deadzone deviations
(`:231-237`):

```
d      = T.pos − in.view.pos                        // head → target ray, world
d_view = qRotate(qInverse(in.view.rot), d)          // back into view space
yaw    = atan2(−d_view.x, −d_view.z)  (sign: left+) // horizontal eccentricity
pitch  = asin(clamp(d_view.y / |d|, −1, 1))         // vertical eccentricity
if |yaw| > maxYaw or pitch outside [−maxPitchDown, +maxPitchUp]:
    clamp yaw/pitch into the box, rebuild d_view at radius |d|, T.pos = view.pos + qRotate(view.rot, d_view)
```

(An elliptical-cone clamp — scale yaw/pitch into a unit circle, renormalize — is equally easy and
avoids the "corner" of the box; MRTK uses the per-axis box; either is fine, pick the box for
explainability.) The BODY branch applies only the yaw half to `T` computed at `XRAnchor.cpp:268`
— measured against the *view* yaw, not the body yaw, since the point is visibility.

Key properties:

- **Clamp the TARGET, not the output.** The deadzone then compares the current smoothed pos
  against the *clamped* target, and the spring glides to it — no per-frame yanking, and the grab
  release / verb paths need no changes at all (they set `m_chasing = true` or fresh offsets, and
  the next solve clamps the new target). This is exactly MRTK's structure (solver computes a
  clamped goal; base class smooths). Clamping the OUTPUT (`m_springPos`) instead would also bound
  the §1.3 mid-turn lag, but turns sustained head rotation into a rigid drag at the cone edge —
  the "stuck to your face" feel Apple explicitly warns about; reject it.
- **Resting eccentricity ≤ cone + deadzone** (§1.2). Document it; or optionally tighten the
  deadzone check while the *target* is on the cone edge (skip the latch when the clamp engaged —
  one boolean). Simpler: accept it and size the default cone with the deadzone in mind.
- **`m_state.anchorPose` is never written.** The user's configured/grabbed offset survives intact
  (glance-away offsets keep their meaning; serialization §4 unchanged). Bounding is purely a
  display-time constraint — turn it off and the monitor returns to the configured spot.
- **Center vs edge**: clamping the quad *center* at 35° with a 1.6 m quad at 1.5 m leaves the near
  edge ~7° from center — fine. If we ever want MRTK's "bounds" clamp mode (edge-at-FOV-edge), the
  half-extent angle is `atan((w/2)/dist)` — cheap to add later as `angular_clamp_mode`.
- **Where**: ~15 lines of pure math in `XRAnchor.cpp` + 4 tuning fields; a `boundEngaged` bool in
  `SXRSolveResult` for IPC/status ("bounded": true in `hyprctl openxr status` JSON, `XRIpc.cpp`).
- **Grab interaction — nothing to do.** The grab override returns at `XRAnchor.cpp:170-208`,
  before the HEAD/BODY branches, so a grabbed quad is *never* clamped (users can deliberately hold
  it anywhere). On release, `endGrab` → `reanchorFromWorld` (`XRAnchor.cpp:430-449,451-478`)
  re-seeds spring + offset from the (WP-G4 latched) release pose; the *next* frame's solve clamps
  the target and the quad glides into the cone from wherever it was released. The release-latch
  ring (`SXRGrabRing`, `XRAnchor.hpp:76-119`) composes untouched — latch first, clamp after, no
  ordering hazard. Same story for `beginResize`/`grabResizeCorner` (resize re-anchors per frame
  while `m_grabbed` stays false — the clamp would fight the pinned corner, so gate the clamp on
  `grabbed()` which already covers `m_resizing`, `XRAnchor.hpp:261-263`).

### 3.2 Option B — soft recall (timeout, then chase) — recommended for body pitch

No hard constraint. Track out-of-cone time; only when the quad's *displayed* pose (use
`m_lastWorld` vs the current view) has been outside a threshold cone for longer than `recall_ms`,
re-target: set `m_chasing = true` with the clamped target from §3.1, letting the existing spring
recall it. Needs one accumulator (`float m_outOfConeSec`) in the solver state — still pure math
(dt is already an input).

- Gentler: a quick glance away (or a deliberate "keep it at my 9 o'clock while I read something")
  doesn't move anything; only a *sustained* loss recalls.
- This is the right shape for **body-mode pitch**, where "ignores pitch" is a designed feature
  (doc 03 §3.3): a hard per-frame pitch clamp would reintroduce pitch-following and destroy the
  desk-monitor metaphor. Instead: if the view pitch keeps the quad outside `±maxPitch` for
  `recall_ms`, temporarily lift/lower the *target* elevation (clamped, not re-written into the
  offset) until the gaze levels again — the quad "peeks" into view when you've been lying back for
  a second, and settles back to desk height when you sit up (offset unchanged, so it returns).
- Weakness as the *only* mechanism for head mode: during the `recall_ms` window the monitor is
  lost, and a user who keeps moving can keep resetting the timer. Head mode wants A; body pitch
  wants B; both share the same clamp math.

### 3.3 Option C — edge indicator instead of movement

Keep the pose; show an affordance at the FOV edge pointing at the lost monitor (the standard
game-HUD "off-screen marker"). Two implementation routes:

- **Compositor-drawn indicator quad**: a small head-locked (VIEW-space) quad per lost monitor at
  the clamped cone direction — a new tiny swapchain + `XrCompositionLayerQuad` submitted in the
  existing frame loop (`OpenXRManager.cpp:929-1018` already sorts/submits N quads). Cheap but new
  machinery (a non-monitor layer type).
- **Chrome-system route** (research/07): the planned theme-engine chrome could draw an arrow/glow
  in an *existing* monitor's margin pointing toward the lost one, or the future `hypxrchrome`
  companion (07 §TL;DR-5) could own indicator ornaments entirely — latency-tolerant, so IPC is
  fine there (a `xrmonitorlost` socket2 event + pose polling).

C is the correct answer for users who *want* far offsets (deliberate 90°-left status monitor):
they opted out of bounding, but still get findability. It is not a substitute for A/B as the
default — a lost *follower* should come back by itself. Ship C later, behind
`bound_mode = indicate`, ideally after research/07's chrome work lands.

### Set-time validation (complementary, all options)

At `xrmonitor` parse time (`XRMonitorConfig.cpp:164-167` / the keyword handler), compute the
offset eccentricity for head/body specs and log a `Log::WARN` when it exceeds the configured cone
("offset is 56° off-center; monitor will rest at the view-bound edge / be recalled") — cheap,
catches typos like a wrong sign on z, and costs nothing at runtime. Do **not** reject or mutate:
the runtime constraint (A/B) already guarantees visibility, and hard validation would break
saved layouts that predate the feature.

### Recommendation

**A + B composed, on by default; C later.** Head mode: target cone clamp (A) with defaults from
§4. Body mode: yaw clamp (A) + pitch soft-recall (B). Keep `bound_mode = none` as the opt-out for
deliberate peripheral placements, and pair it with the parse-time warning so it's a choice, not an
accident. This is MRTK-proven, ~30 lines of pure gtest-able math, zero new threads/locks, zero
persistence impact, and zero interaction with the grab machine.

---

## 4. Config sketch

Registered in `Values::getConfigValues()` (`src/config/values/ConfigValues.cpp`, one `MS<>` entry
each, next to the existing `openxr:leash_*` block at `:718-721`); all read per-frame via
`readAnchorTuning()` → hot-toggle for free (same pattern as the grab filter vars,
`OpenXRManager.cpp:834-839`).

```
openxr:leash_bound_mode        = clamp     # none | clamp | recall | indicate   (head mode; body yaw always uses clamp when != none)
openxr:leash_max_yaw           = 40.0      # deg half-angle, horizontal eccentricity of the quad CENTER
openxr:leash_max_pitch_up      = 15.0      # deg above view forward   (Microsoft: avoid >10° above horizon)
openxr:leash_max_pitch_down    = 35.0      # deg below view forward   (optimal zone 0–35° below)
openxr:leash_recall_ms         = 800       # recall mode + body pitch band: out-of-cone dwell before recall
openxr:body_leash_pitch_band   = true      # body mode: enable the soft pitch recall (B); yaw clamp is governed by bound_mode
openxr:leash_bound_fov_margin  = 5.0       # deg; when xrLocateViews FOV is available, effective bound = min(config, fovHalf − margin); 0 disables FOV shrink
```

Default rationale: MRTK ships 30°/20° for palm-sized slates; our quads subtend ~30–55° themselves,
users deliberately offset multi-monitor layouts, and the deadzone adds up to 15° of rest slack —
so 40° yaw / 15° up / 35° down keeps the quad's near half inside the Meta ±30° comfort cone while
not fighting reasonable two-monitor layouts. `SXRAnchorTuning` grows `boundMode, maxYawRad,
maxPitchUpRad, maxPitchDownRad, recallSec, bodyPitchBand, fovHalfHRad, fovHalfVRad` (0 = unknown).

**Serialization: none — take the position that bounding is a runtime constraint.** The clamp
never writes `m_state.anchorPose`, so `hyprctl openxr layout` (`serializeXRMonitorLine`,
`XRMonitorConfig.cpp:295-299`) emits exactly the user's offsets, round-trips are unchanged, and
disabling the feature restores today's behavior bit-for-bit. The only observable additions are
IPC: a `"bounded": true|false` per-monitor field in the status JSON and (if C ships) a
`xrmonitorlost`/`xrmonitorfound` socket2 event pair. No `xrmonitor` grammar change; optionally a
future per-monitor `bound:none` token if per-monitor opt-out proves needed (grids from
research/03 would want the *parent* transform bounded, not each cell — a reason to keep the knob
global for now).

---

## 5. Work packages

- **WP-B1 — cone-clamp math + plumbing (S).** Pure-math clamp (view-space yaw/pitch box, §3.1)
  in `CXRAnchor::solve()` HEAD branch + BODY yaw; `SXRAnchorTuning` fields; config vars;
  `readAnchorTuning()`; `boundEngaged` in `SXRSolveResult` + status JSON; parse-time eccentricity
  warning. gtests (extend `tests/xr/anchor_math.cpp`): offset-behind-head clamps to cone edge and
  faces viewer; clamped target respects deadzone/no-jitter at the edge (LATCHED at cone edge stays
  frozen); grab → carry outside cone → release glides back in (composes with `pickReleasePose`);
  resize not clamped while resizing; `bound_mode=none` is bit-identical to today. Headless-safe:
  no runtime needed (this is the whole point of the pure solver). Deps: none.
- **WP-B2 — FOV capture + soft recall + body pitch band (S/M).** `xrLocateViews` beside
  `OpenXRManager.cpp:843`, binocular half-angle reduction, `fov_margin` min-combine;
  out-of-cone dwell accumulator + recall mode; body pitch band (§3.2) incl. interaction with the
  yaw-hold hysteresis (recall must not fight `m_yawHolding` — clamp measures against *view* yaw,
  which is fine while yaw holds); gtests for dwell timing (dt-driven, no clocks). Live validation
  on Quest 3 via `preview-xr.sh --wivrn` (FOV values + feel of the recall). Deps: WP-B1.
- **WP-B3 (optional, later) — edge indicator (M).** `bound_mode = indicate`: lost-monitor events
  + indicator affordance; implementation choice (compositor indicator quad vs chrome/companion)
  should follow research/07's WP-C decisions rather than lead them. Deps: WP-B1, research/07
  triage.

---

## 6. Open questions

1. **Default `bound_mode`: `clamp` or `none`?** Recommendation above says `clamp` (the feature is
   worthless off-by-default for the accidental-loss case, and deliberate placements get the
   opt-out + warning), but it *changes existing layouts'* resting positions for offsets beyond
   the cone — user call.
2. **Body pitch band on by default?** It rescues the lying-back case but softens the "desk
   monitor" metaphor; if off by default, at minimum ship it as the documented fix for §1.4.
   Related: should `body_leash_follow_height` interplay (a height mismatch masquerades as pitch
   eccentricity) trigger the same recall, or should recall adjust *height* rather than elevation
   angle for body mode? (Height-recall preserves the metaphor better.)
3. **Deadzone at the cone edge**: accept resting eccentricity of cone+deadzone (simple), or skip
   the latch while the clamp is engaged (tighter, slightly more motion)? Prototype both in WP-B1
   gtests and pick by feel on hardware.
4. **Clamp shape**: per-axis box (MRTK, explainable) vs elliptical cone (no corner artifacts) —
   trivial either way, decide in WP-B1 review.
5. **Mid-turn lag** (§1.3) is *not* fixed by target clamping — is a max-chase-speed or
   response-shortening-when-far term wanted later, or is transient loss during a fast turn
   acceptable (MRTK accepts it)?
6. **Grids** (research/03): when grid anchors land, bounding should apply to the grid parent
   transform once, not per cell — keep the clamp in the anchor that owns the leash.
