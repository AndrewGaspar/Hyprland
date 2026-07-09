# 04 — Grabbable Borders & Hand-Tracking Grab (research + design)

Status: research / design proposal (uncommitted). No implementation here. Companion to the
committed input design doc `docs/openxr/04-input.md` (§6 grab machine) and `03-anchoring.md` (§4
grab pose math). Where this doc proposes changing shipped behavior it says so explicitly.

Author target: the user, to triage into WPs for Opus implementation subagents.

---

## TL;DR

- **Root cause of the fist lurch.** Today grab is a whole-quad affordance: point anywhere at a
  quad, cross the `grab` (squeeze/grasp) Schmitt threshold, and `CXRAnchor::beginGrab` snapshots
  the quad relative to the **grip (wrist) pose**. On release, `endGrab` re-anchors from the quad's
  world pose **sampled at the exact release frame** (`grip ∘ m_grabOffset`). With controllers this
  is fine. With WiVRn hand-tracking the `grab` action is bound to `grasp_ext` (fist curl): *opening
  the fist to release* both (a) is the release edge and (b) physically swings the wrist — so the
  reanchor latches the swung pose and the window lurches. Two compounding faults: the gesture moves
  the anchor, and the reanchor reads the worst possible instant.
- **Two fixes, layered.** (1) *Grabbable borders*: make the quad **body** click-only and confine
  grab to a **border band / grab bar** — this de-conflates click intent from move intent and gives
  hands a place to pinch. (2) *Release stabilization*: a small **per-hand ring buffer of grab
  poses** so `endGrab` re-anchors from a pose ~100 ms **before** the release edge, plus velocity-
  outlier rejection. Fix (2) helps controllers too.
- **Hands should pinch, not fist.** Both installed runtimes (WiVRn server 26.6.1, Monado v25.1.0)
  expose `XR_EXT_hand_interaction` with `pinch_ext/value` **and a stable `pinch_ext/pose`** (thumb-
  index contact point) and `XR_EXT_hand_tracking`. Meta/Ultraleap/visionOS all converge on *pinch a
  dedicated bar to move a panel*. Bind grab to **pinch on the border/bar**, and anchor the hand-grab
  to `pinch_ext/pose` (far steadier through open/close than the wrist grip).
- **Everything stays frame-thread-pure.** The ring buffer, filter, and classifier are POD math with
  zero hyprutils SP/WP refcount ops — compliant with the load-bearing rule in
  `XRMonitorLayer.hpp`. The border is drawn by extending the swapchain blit (`XRGraphics.cpp`).
- **6 work packages** (WP-G1…G6). G1–G4 are fully testable with the Monado remote driver (fake
  controllers can script a squeeze and a release jerk). G5–G6 (hand pinch path, one-euro carry
  filter) **require live Quest 3 + WiVRn** because the remote driver only advertises
  `valve/index_controller`, never a hand-interaction profile.

---

## 1. Current-behavior analysis (read from the source)

Files: `src/openxr/XRInput.{hpp,cpp}`, `src/openxr/XRAnchor.{hpp,cpp}`, `src/openxr/XRMath.hpp`,
`src/openxr/OpenXRManager.cpp` (frame loop), `src/openxr/XRGraphics.cpp`,
`src/config/values/ConfigValues.cpp` (openxr section, lines 709-736).

### 1.1 The grab gesture → begin/end (thresholds, hysteresis)

`CXRInput::processPointer` (`XRInput.cpp:348-515`) runs on the **frame thread**, once per frame,
under `COpenXRManager::m_layersMu`. Grab is a per-hand Schmitt trigger:

```
m_grabTrig[hand].update(m_hands[hand].grab, grabOnT, grabOffT)   // SXRSchmitt, XRInput.hpp:100
grabOnT  = openxr:grab_threshold          (default 0.7)
grabOffT = openxr:grab_threshold_release  (default 0.4)   // hysteresis band 0.4–0.7
```

- **Rising edge** (`XRInput.cpp:431-473`): pick a target — the hand's currently hovered quad
  (`m_hoverMon[hand]`), or, if hovering nothing, re-run the ray test with a **5° entry cone**
  (`XR_GRAB_CONE_DEG`, `slack = tan(5°)·t`). If a target is found, it is **any hovered quad
  regardless of where on the quad the ray lands** — there is no body/edge distinction today. Then
  `target->anchor->beginGrab(hand, *worldGrip)`, set `m_grabbing[hand]=true`, haptic tick, emit a
  `GRAB` state event.
- **While grabbed** (`XRInput.cpp:495-514`): the grabbing hand **casts no ray** (`XRInput.cpp:399`
  gates the ray on `!m_grabbing[hand]`) and its thumbstick feeds `grabPushPull`(stick.y) /
  `grabResize`(stick.x) instead of scroll.
- **Falling edge** (`XRInput.cpp:474-490`): `target->anchor->endGrab(solveIn, tune)`; haptic tick;
  emit `GRAB …,0`; clear state. If the monitor vanished mid-grab, force-release with no reanchor.

There is **no long-press fallback** for profiles without an analog grab — `04-input.md` §1.5 is
marked NOT IMPLEMENTED. `khr/simple_controller` cannot grab at all today.

### 1.2 What pose actually moves the quad (and the lurch mechanism)

Grab pose math is `CXRAnchor` (`XRAnchor.cpp:243-315`), pure math, compiled unconditionally:

- `beginGrab(hand, gripWorld)` (`:243`): `m_grabOffset = poseInverse(gripWorld) ∘ m_lastWorld`,
  `m_grabbed=true`, `m_grabHand=hand`. The offset is captured **in the grabbing hand's grip
  space**.
- During carry, `solve()` (`:91-114`) takes the **grab override**: it returns
  `space = XR_SPACE_GRIP_LEFT/RIGHT` and `pose = m_grabOffset`. The frame loop
  (`OpenXRManager.cpp:876-897`) submits the quad against the grip **XrActionSpace** with that
  offset — so the **runtime late-latches the controller/hand pose at display time** (zero added
  latency, no filtering, no smoothing on our side). For head/body modes the offset's *rotation* is
  refreshed each frame to keep facing the viewer; position stays a rigid grip-space offset.
- `endGrab(in, tune)` (`:261`): `W = grip ∘ m_grabOffset` **using this frame's grip pose**, then
  `reanchorFromWorld(W, …)` bakes `W` into the persistent mode (LOCAL: `anchorPose = W`; HEAD/BODY:
  re-express against view/body; DEVICE: against grip).

**So the pose that moves the quad while grabbed is the grip (wrist) pose, and the pose that
freezes it on release is `grip ∘ offset` at the release frame.** With hand-tracking `grip` is the
wrist and `grab` is `grasp_ext` (fist). Opening the fist to release is exactly the moment the wrist
pose is dirtiest, and that dirty pose is what gets baked. That is the lurch. It also explains why
controllers are "acceptable": the trigger/grip release barely moves a hand holding a controller.

### 1.3 Hit test + hover today

`OpenXR::rayQuadIntersect` (`XRMath.hpp:246-269`) is the only classifier: ray vs quad plane,
bounds test on `|p.x| ≤ w/2+slack`, `|p.y| ≤ h/2+slack`, returns `{hit, t, u, v}` with
`u = p.x/w + 0.5`, `v = 0.5 − p.y/h` (v grows downward, 0 = top). Nearest-t wins (occlusion). It has
**no notion of border vs body** — a hit anywhere is a body hit.

Hover is tracked per hand (`m_hoverMon`, `m_hoverUV`) and mirrored to `CXRMonitorLayer::m_hovered`
(a **main-thread status-only bool**, `OpenXRManager.cpp:471-516`, surfaced in the status JSON,
`:1638`). **No border is ever drawn.** The blit shader (`XRGraphics.cpp:246-274`) samples the
DMA-BUF content and forces `fragColor.a = 1.0`; there is no border/frame geometry, no per-state
colorization, no uniforms beyond the sampler.

### 1.4 Config style (for naming new vars)

`src/config/values/ConfigValues.cpp` openxr section (709-736): all vars are
`MS<Type>("openxr:snake_case", "description", default, {.min=…, .max=…})`, front-end-agnostic
(hyprlang + Lua both consume the list). Existing grab-adjacent vars: `openxr:grab_threshold`,
`openxr:grab_threshold_release`, `openxr:pointer_trigger_threshold[_release]`, `openxr:scroll_speed`,
`openxr:default_size`, `openxr:default_distance`. New vars below follow the same shape.

---

## 2. How hands arrive via WiVRn / Monado (probed on this box)

`strings` on the read-only runtime libraries (allowed):

- `/usr/lib/wivrn/libopenxr_wivrn.so` (WiVRn server **26.6.1**) advertises: `XR_EXT_hand_interaction`,
  `XR_EXT_hand_tracking`, `XR_EXT_hand_tracking_data_source`, `XR_EXT_palm_pose`, `XR_EXT_local_floor`,
  `XR_EXTX_overlay`, `XR_KHR_composition_layer_equirect2/cylinder`, plus the FB/Meta touch profiles.
- `/usr/lib/libopenxr_monado.so` (Monado **v25.1.0**) advertises `XR_EXT_hand_interaction`,
  `XR_EXT_hand_tracking`, `XR_EXT_hand_tracking_data_source` and the
  `ext/hand_interaction_ext` + `microsoft/hand_interaction` profiles.

Both expose the full `ext/hand_interaction_ext` component set:

```
/user/hand/{left,right}/input/pinch_ext/value      float   0..1 pinch strength
/user/hand/{left,right}/input/pinch_ext/pose       pose    thumb–index contact point   ★
/user/hand/{left,right}/input/pinch_ext/ready_ext  bool
/user/hand/{left,right}/input/grasp_ext/value      float   0..1 fist curl                (today's `grab`)
/user/hand/{left,right}/input/grasp_ext/pose       —       (grasp has no pose)
/user/hand/{left,right}/input/aim_activate_ext/value  float
/user/hand/{left,right}/input/poke_ext/pose        pose    fingertip
/user/hand/{left,right}/input/aim/pose , grip/pose , grip_surface/pose
```

**What we bind today** (`XRInput.cpp:189-197`, only when `m_hasHandInteraction`):
`select → pinch_ext/value`, `grab → grasp_ext/value`, `aim_pose → aim/pose`, `grip_pose → grip/pose`.
So with hands, **grab is already the fist** — precisely the unworkable gesture. We do **not** bind
`pinch_ext/pose`, so we have no stable pinch anchor available.

`m_hasHandInteraction` is set on `CXRSession` (`XRSession.hpp:79`) from the enabled instance
extension; `m_hasHandTracking` too. The remote test driver (Monado null-compositor + remote driver
on TCP 4242) advertises **`valve/index_controller` only** — it never produces a hand-interaction
profile. Hand paths only appear from a real headset (WiVRn + Quest 3). This is the hard testing
boundary.

### 2.1 Why pinch beats fist for precise release (vendor guidance)

- **Meta Hand Tracking Design Guidelines**: pinch (index-thumb) is "the basic interaction primitive
  for UI interactions", equivalent to a controller click/select; `PinchStrength` is a continuous
  0→1 where 1 = fingers touching. Grab is "form a *loose* fist… the system looks for finger flexion,
  **don't clench too tightly**" — Meta itself flags the fist as coarse and pose-perturbing, and
  reserves it for grabbing 3D objects, not moving precise UI. ([Meta Interactions Setup], [Meta
  Hand Tracking Overview].)
- **`pinch_ext/pose` is a defined, stabilized point.** Because it is the thumb-index contact, its
  translation barely moves as the pinch opens (the fingers separate roughly symmetrically about it),
  whereas the wrist (`grip/pose`) rotates through a large arc during a fist open/close. Anchoring to
  the pinch pose removes most of the release swing before any filtering.
- **visionOS** places a **window bar below each window**; you look-and-pinch the bar and drag to
  move, pinch the content to interact. **Meta Quest system UI** panels have a **grab handle bar
  under the panel**. **SteamVR** dashboard/overlays grab from their frame/edge. The universal
  pattern: *a dedicated handle + pinch + release stabilization*, never "fist the content".
- **Ultraleap design guidelines** recommend pinch for precise manipulation and warn that whole-hand
  grabs are for coarse, large-object interaction; they also recommend hysteresis + filtering on the
  pinch signal.

---

## 3. Prior art — the common pattern

| System | Move a floating panel by… | Handle geometry | Release stabilization |
|---|---|---|---|
| Apple visionOS | look + **pinch the window bar**, drag | thin bar **below** the window | system smoothing; bar is off the content so no click conflict |
| Meta Quest system UI | **pinch/grab the handle bar** under a panel | bar **below** panel | Hands 2.x pinch stabilization + pinch pose |
| Meta Interaction SDK (grabbable) | pinch or palm grab on a **grab surface** | explicit grab collider, distinct from UI | pose from pinch point; velocity on throw only |
| SteamVR overlays | point at overlay **edge/frame**, grip | frame border | overlay transform smoothing |
| Ultraleap | **pinch** for precise, palm grab for coarse | dedicated interactable | 1€-style filtering on joints |

**Extracted invariant:** the *content* is for interaction (click/scroll), a *dedicated affordance*
(bar or border) is for moving, the *move gesture is pinch* (for hands), and the *release is
stabilized* (stable anchor point + smoothing/latching). Our design adopts all four.

---

## 4. Release-perturbation mitigation techniques (with citations)

These fix the fist lurch **and** tighten controller feel. Ordered by leverage.

1. **Stable anchor point (biggest single win for hands).** Anchor the hand-grab to `pinch_ext/pose`
   instead of `grip/pose`. The contact point is nearly stationary through the pinch open, so the
   release pose is already ~correct before any temporal trick. (Meta/Ultraleap guidance, §2.1.)

2. **Release pose latching (fixes fist *and* controllers).** Keep a short **ring buffer** of the
   grab anchor's world pose per frame while grabbed. On the release edge, re-anchor from the pose
   at `T − openxr:grab_release_latency_ms` (default ~100 ms) rather than the release frame. The
   perturbation lives entirely in the last ~1–3 frames of the gesture, so rewinding past it removes
   the lurch. This is the "rewind to before the release edge" the task asks for. (General VR input
   practice; the same idea underlies throw-velocity windows in Meta Interaction SDK.)

3. **Velocity-outlier rejection at release.** Compute the anchor's linear/angular speed over the last
   few ring samples; if the release-frame speed exceeds `openxr:grab_release_velocity_reject`
   (m/s + rad/s), walk the ring back to the most recent **calm** sample (below threshold) instead of
   a fixed time offset. Catches the fist-open jerk directly even when it is faster than the latency
   window. (Outlier rejection is standard; conceptually the inverse of gesture "throw" detection.)

4. **One-euro low-pass during carry (optional polish, mainly hands).** Filter the carried world pose
   with the **1€ filter**: a first-order low-pass whose cutoff rises with speed — low cutoff kills
   jitter when the panel is nearly still, high cutoff avoids lag when you move it fast. Two intuitive
   params (`min_cutoff`, `beta`). **Casiez, Roussel & Vogel, "1€ Filter: A Simple Speed-based
   Low-pass Filter for Noisy Input in Interactive Systems," CHI '12, pp. 2527–2530.**
   Reference implementation: github.com/casiez/OneEuroFilter. Caveat: filtering means we can no
   longer use the runtime's zero-latency grip-space late-latch for hands; see §5.4 trade-off.

5. **Hysteresis on the grasp value (already present, tune for hands).** The 0.4/0.7 Schmitt band
   already debounces; hands may want a wider release band (e.g. release at 0.25) so the fist has to
   open decisively — but latching (2) is the real fix, so keep the shared default and expose the
   band we already have.

6. **Deadman / minimum-grab-time.** Ignore grabs shorter than a few frames (spurious pinch/grasp
   spikes). Cheap, avoids phantom micro-moves. Optional.

---

## 5. Proposed design for HypXRland

### 5.1 Geometry: border band + optional grab bar

Define a per-quad **grab geometry** in UV space (resolution-independent, scales automatically when
the quad is resized because it is a fraction of the quad, not pixels):

```
openxr:grab_affordance = border | bar | both | none      (default: border)
openxr:grab_border_width  = 0.06     // fraction of min(w,h) that is the grab band (0..0.5)
openxr:grab_bar_edge      = bottom | top                 (default: bottom, matches visionOS/Quest)
openxr:grab_bar_thickness = 0.08     // fraction of quad height for the bar (0..0.5)
```

- **Border band**: the outer frame of the quad. In UV, a fragment/ray hit is *border* when
  `u < bU || u > 1−bU || v < bV || v > 1−bV`, where `bU = border_width·min(w,h)/w`,
  `bV = border_width·min(w,h)/h` (so the band is the same **metric** thickness on all four edges
  despite the aspect ratio).
- **Grab bar**: a strip of height `bar_thickness` along the chosen edge; e.g. bottom bar is
  `v > 1 − bar_thickness`. The bar is the visionOS/Quest metaphor and is the recommended default for
  hands (a big, obvious pinch target). `both` = border + bar.
- **Body** = everything else.

Pure classifier, added to `XRMath.hpp` (unconditional, gtest-covered), consuming the existing
`rayQuadIntersect` UV output:

```cpp
namespace OpenXR {
  enum eXRQuadRegion : uint8_t { XR_REGION_BODY = 0, XR_REGION_BORDER, XR_REGION_BAR };
  struct SXRGrabGeometry {
      bool  border = true, bar = false, barTop = false;
      float borderWidthFrac = 0.06f;   // of min(w,h)
      float barThicknessFrac = 0.08f;  // of h
  };
  // u,v in [0,1] from rayQuadIntersect; w,h in meters (for the metric-even border).
  eXRQuadRegion classifyQuadHit(float u, float v, float w, float h, const SXRGrabGeometry& g);
}
```

`rayQuadIntersect` itself is **unchanged** (still returns `u,v`); classification is a separate pure
call so the ray/UV math and its gtests stay stable. The frame loop passes the per-quad
`SXRGrabGeometry` (read once from config) into `SXRPointerTarget` (new field) so `processPointer`
can classify each hit.

### 5.2 Interaction rules

| Region hit | select / pinch-value (click) | grab gesture (squeeze / grasp / pinch) |
|---|---|---|
| **Body** | BTN_LEFT click + scroll (unchanged) | **never grabs** (unless `openxr:grab_anywhere` for the squeeze/grasp path — see below) |
| **Border** | click still works (it is still the surface) | **grabs** |
| **Bar** | bar is chrome, not content: **no click**, grab only | **grabs** |

- **Body is move-immune.** This is the de-confliction the user asked for: pointing at content and
  pressing the trigger is unambiguously a click; you can only *move* from the border/bar.
- **`openxr:grab_anywhere` (bool, default true)** governs only the **squeeze/grasp** gesture
  (controller grip; hand fist). Default true **preserves today's controller UX** (grip anywhere
  grabs). Controller users who want deliberate moves set it false. For hands, the fist path is
  additionally gated by `openxr:hand_grab` (below), so a fist on the body never moves a window
  regardless of `grab_anywhere`.
- **Pinch-grab always requires border/bar**, never body — because pinch on the body is a *click*.
  This is the visionOS rule and needs no extra config: region alone disambiguates.

### 5.3 Hand-specific: pinch grab + active-device detection

- **Bind the pinch pose.** Add a `pinch_pose` action (`XR_ACTION_TYPE_POSE_INPUT`) + per-hand
  action space, suggested for `ext/hand_interaction_ext` on `…/input/pinch_ext/pose`. This is the
  grab anchor for hands.
- **`openxr:hand_grab = pinch | grasp | both` (default pinch).** Chooses which hand gesture
  initiates a grab. Default `pinch` disables the unworkable fist entirely for hands; `grasp`
  restores the old behavior; `both` allows either. When the grab gesture is pinch, the grab anchor
  space is `pinch_ext/pose`; when grasp, it stays `grip/pose` (grasp has no pose).
- **Active-device detection.** Cache per-hand current interaction profile
  (`xrGetCurrentInteractionProfile`, refreshed on `XrEventDataInteractionProfileChanged`) as
  `m_handProfile[2]`. "Hands are active for hand H" ⟺ that profile is `ext/hand_interaction_ext`.
  When hands are active, `processPointer` uses the pinch value + pinch pose and forces the
  border/bar gating (ignores `grab_anywhere`). When a controller profile is active, behavior falls
  back to squeeze on grip pose with `grab_anywhere` honored. This makes a mixed session (controller
  in one hand, hand-tracked other) behave correctly per hand.

### 5.4 Release stabilization data structures (frame thread only, POD)

All of this lives in `CXRInput` (frame thread) and `CXRAnchor` (pure math). **Zero hyprutils
refcount ops** — every type is POD (`SXRPose` = `Vec3`+`Quat`), satisfying the
`XRMonitorLayer.hpp` rule by construction.

```cpp
// XRInput.hpp — frame-thread-only, per hand
struct SXRGrabSample { OpenXR::SXRPose world; uint32_t timeMs = 0; float speed = 0.f; };
struct SXRGrabRing {
    static constexpr size_t CAP = 128;          // ~1.4 s at 90 Hz; power of two
    std::array<SXRGrabSample, CAP> buf{};
    uint32_t count = 0, head = 0;
    void    reset();
    void    push(const OpenXR::SXRPose& w, uint32_t timeMs);      // computes speed vs previous
    // pose at (nowMs - latencyMs), linearly/nlerp-interpolated; falls back to oldest sample
    OpenXR::SXRPose sampleBack(uint32_t nowMs, uint32_t latencyMs) const;
    // most recent sample whose speed < thresh, searching back from newest (velocity rejection)
    OpenXR::SXRPose lastCalm(float linThresh, uint32_t nowMs, uint32_t maxBackMs) const;
};
std::array<SXRGrabRing, 2> m_grabRing;          // per hand
```

- While grabbed, each frame `processPointer` pushes the **current world grab pose** (the anchor's
  `lastWorld()` for that frame, which the solve already computed) into `m_grabRing[hand]`.
- On the release edge, compute the release world pose:
  `W = velocity_reject ? ring.lastCalm(...) : ring.sampleBack(now, latency_ms)`, then call a new
  overload `anchor->endGrab(W, in, tune)` that re-anchors from the **given** world pose instead of
  recomputing `grip ∘ offset`. Add to `CXRAnchor`:

  ```cpp
  void endGrab(const SXRPose& releaseWorld, const SXRSolveInput& in, const SXRAnchorTuning& tune);
  // (keeps the existing endGrab(in,tune) as endGrab(m_lastWorld, in, tune) for callers that
  //  want the current behavior / no ring available)
  ```

  This is a small, pure change: `reanchorFromWorld(releaseWorld, ctx, tune)` already exists.

- **One-euro carry filter (WP-G6, optional).** For hands only, replace the grip-space late-latch
  carry with a filtered LOCAL_FLOOR pose: run each axis of the pinch-anchored world pose through a
  1€ filter, submit the filtered pose in the reference space (not the grip action space). Trade-off:
  loses the runtime's zero-latency late-latch (adds one frame of latency) but removes the jitter the
  wrist/pinch signal carries — a good trade for shaky hands, a bad one for steady controllers, hence
  **default off** and controllers never use it. Filter state (`SXROneEuro{ float xHat, dxHat; bool
  init; }` per axis) is POD, frame-thread-only.

  ```
  openxr:grab_release_latency_ms   = 100    (0..500)   // rewind window on release
  openxr:grab_release_velocity_reject = 0.6  (0..5 m/s; 0 disables)  // calm-sample search
  openxr:grab_filter               = false             // 1€ carry filter (hands)
  openxr:grab_filter_min_cutoff    = 1.0    (0.01..10) // 1€ min cutoff (Hz)
  openxr:grab_filter_beta          = 0.007  (0..1)     // 1€ speed coefficient
  ```

### 5.5 Hover feedback + border rendering

Today quads have no drawn border. Add a **border pass** to the swapchain blit and drive its color
from grab state.

- **Per-layer visual state (frame-thread only, or atomics).** After `processPointer`, set on each
  `CXRMonitorLayer`:
  ```cpp
  std::atomic<uint8_t> m_hoverRegion{0};  // 0 none / 1 body / 2 border / 3 bar (per whichever hand hovers)
  std::atomic<bool>    m_grabbedNow{false};
  ```
  Both are written on the frame thread and read on the frame thread in `blitBuffer` — no cross-
  thread refcount, no main-thread coupling. (The existing main-thread `m_hovered` status bool
  stays for IPC.)
- **Render.** In `CXRGraphics`, after the content blit into `dstTex`, run a cheap border pass:
  4 `glScissor` + `glClear` rectangles for the border band and one for the bar, colored by state:
  - idle (not hovered): draw **nothing**, or a hairline neutral frame (config `openxr:grab_border_idle`).
  - hovering border/bar: `openxr:grab_hover_color` (invite — "you can grab here").
  - grabbed: `openxr:grab_active_color` on the whole frame + bar (confirm).

  Scissor-clear avoids touching the sampler shader and keeps GL state minimal (matches the file's
  existing FBO-clear idioms, e.g. `clearTex`). The band thickness in **pixels** is
  `border_width·min(w,h)` mapped through the quad's meters→pixels (`m_swapchainSize`); the bar is
  `bar_thickness·pxH`. Alpha stays 1.0 (opaque under passthrough, same rule as the content path).

  Colors as config (Hyprland supports color types):
  ```
  openxr:grab_border_idle  = 0x00000000   // transparent = no idle frame
  openxr:grab_hover_color  = 0x66_aa_cc_ff // accent, ~40% — hover invite
  openxr:grab_active_color = 0xff_66_aa_ff // bright — grabbed
  ```

  (Rendering the border into the swapchain, rather than as a separate XR layer, keeps the border
  aligned with the content 1:1 through resize/anchor with no extra composition-layer bookkeeping.)

### 5.6 Config surface (all new vars, `openxr:` prefix, ConfigValues.cpp style)

> NOTE (2026-07-09): defaults + `grab_release_velocity_reject` semantics in this table are the
> original research values — superseded by the shipped live-tuned set in §11.

| var | type | default | meaning |
|---|---|---|---|
| `openxr:grab_affordance` | string | `border` | `border`\|`bar`\|`both`\|`none` — affordance drawn + hit for grab |
| `openxr:grab_border_width` | float | `0.06` | grab band thickness, fraction of `min(w,h)` (0..0.5) |
| `openxr:grab_bar_edge` | string | `bottom` | `bottom`\|`top` — which edge the grab bar sits on |
| `openxr:grab_bar_thickness` | float | `0.08` | grab bar height, fraction of quad height (0..0.5) |
| `openxr:grab_anywhere` | bool | `true` | squeeze/grasp (controller grip / hand fist) grabs from the body too; hands' fist still gated by `hand_grab` |
| `openxr:hand_grab` | string | `pinch` | `pinch`\|`grasp`\|`both` — hand gesture that starts a grab (pinch anchors to `pinch_ext/pose`) |
| `openxr:grab_release_latency_ms` | int | `100` | rewind window used for the release reanchor (0..500) |
| `openxr:grab_release_velocity_reject` | float | `0.6` | m/s; above this at release, reanchor from last calm sample; 0 disables |
| `openxr:grab_filter` | bool | `false` | 1€ low-pass the carried pose (hands; adds ~1 frame latency) |
| `openxr:grab_filter_min_cutoff` | float | `1.0` | 1€ min cutoff Hz (0.01..10) |
| `openxr:grab_filter_beta` | float | `0.007` | 1€ speed coefficient (0..1) |
| `openxr:grab_border_idle` | color | `0x00000000` | idle frame color (transparent = none) |
| `openxr:grab_hover_color` | color | accent | border/bar color while hovered (grab invite) |
| `openxr:grab_active_color` | color | bright | frame+bar color while grabbed |

Hot-reload note (from MEMORY): the legacy `hyprctl keyword` path never fires
`config.props_refreshed`; any of these that must hot-toggle need the same special-case as
`openxr:enabled`/`inhibit_idle` in `ConfigManager.cpp parseKeyword`. Most of these are read
per-frame from `CConfigValue` cached pointers (geometry, thresholds, colors) so they pick up Lua
reloads for free; only ones read once at session start would need the special-case — keep them all
per-frame-read to avoid it.

---

## 6. Work-package breakdown

Each WP is sized for one implementation subagent. **Every subagent prompt must carry the
process-cleanup safety rule** (never `pkill/killall Hyprland`; kill only by tracked PID or full
path `pkill -9 -f 'build-debug/Hyprland'`) and the **frame-thread zero-refcount rule**.

Dependency graph: `G1 → {G2, G3}`, `G4` (parallel, depends only on current code), `{G1,G3,G4} → G5 → G6`.

### WP-G1 — Hit-region classifier + geometry plumbing
- **Scope.** Add `eXRQuadRegion` + `SXRGrabGeometry` + `classifyQuadHit` to `XRMath.hpp` (pure).
  Add the 4 geometry config vars. Read geometry in the frame loop, attach `SXRGrabGeometry` +
  computed region to `SXRPointerTarget`; `processPointer` classifies each hover hit and stores the
  region (extend `m_hoverRegion` state). No behavior change to grab yet — just classification +
  hover-region bookkeeping.
- **Acceptance.** New gtests in `tests/xr/` for `classifyQuadHit` (corners→border, center→body,
  bottom strip→bar, aspect-ratio metric-evenness). Remote-driver preview: fake controller ray at
  center vs edge logs BODY vs BORDER/BAR. 313/313 existing gtests still pass. **No live Quest.**

### WP-G2 — Border/bar rendering in the swapchain
- **Scope.** Extend `CXRGraphics` with a border pass (scissor-clear rectangles) after the content
  blit; add `m_hoverRegion`/`m_grabbedNow` layer state (frame-thread) and the 3 color vars; drive
  color by state. Depends G1 (needs region state).
- **Acceptance.** Shader/GL compiles; `scripts/preview-xr.sh` shows a visible border, hover accent
  on the edge, bright frame while grabbing (visual screenshot). No gtest for pixels; a smoke assert
  that the border pass runs. **No live Quest** (desktop preview is enough for the visual).

### WP-G3 — Grab-region gating + `grab_anywhere`
- **Scope.** Gate `beginGrab` on region: body never grabs unless `grab_anywhere` (squeeze/grasp
  path only). Border/bar always grab. Bar suppresses click. Add `openxr:grab_anywhere`. Pure
  decision helper `bool grabAllowed(region, gesture, grabAnywhere, handActive)` with gtests.
  Depends G1.
- **Acceptance.** gtest of `grabAllowed` truth table. Remote-driver test (extend the `--xr` suite):
  squeeze while aiming quad **center** with `grab_anywhere=false` → **no** GRAB event; aiming
  **border** → GRAB event; with `grab_anywhere=true` center grabs. **No live Quest.**

### WP-G4 — Release latching ring + velocity rejection
- **Scope.** `SXRGrabRing` (POD, frame-thread) per hand in `CXRInput`; push each grabbed frame;
  `CXRAnchor::endGrab(worldPose, in, tune)` overload; wire release edge to reanchor from the latched
  / calm pose. Add `openxr:grab_release_latency_ms`, `openxr:grab_release_velocity_reject`. Parallel
  with G1–G3 (touches endGrab + the release edge, not the region code).
- **Acceptance.** gtests: ring interpolation (`sampleBack`), `lastCalm` picks the pre-jerk sample,
  `endGrab(world,…)` equals `reanchorFromWorld` for each mode. Remote-driver test: script a grip
  pose that is steady then jerks 20 cm on the release frame → grabbed monitor's final `lastWorld`
  matches the **pre-jerk** pose within tolerance, not the jerk. Controllers exercise this fully.
  **No live Quest required** (remote poses script the jerk); real fist-open validation is a
  follow-up on Quest.

### WP-G5 — Hand pinch grab path (LIVE QUEST REQUIRED)
- **Scope.** Bind `pinch_pose` action + space on `ext/hand_interaction_ext`; per-hand
  `xrGetCurrentInteractionProfile` caching + `InteractionProfileChanged` handling; `openxr:hand_grab`
  (pinch anchors to pinch pose; grasp keeps grip pose); force border/bar gating when hands active;
  pinch-value Schmitt for the hand grab gesture. Depends G1, G3, G4.
- **Acceptance.** Builds; gtests for the profile→gesture selection helper (pure). **Live Quest 3 +
  WiVRn (`preview-xr.sh --wivrn`)**: hand-track, pinch the grab bar, move a monitor, **open the
  pinch to release with no lurch** (the whole point). The Monado remote driver cannot produce a
  hand-interaction profile, so this WP's core behavior is **only verifiable on the headset** — mark
  it clearly and serialize the live run (only one monado/WiVRn service per box).

### WP-G6 — One-euro carry filter (optional; LIVE QUEST to tune)
- **Scope.** `SXROneEuro` POD per axis; when `openxr:grab_filter` and hands active, submit a filtered
  LOCAL_FLOOR pose instead of the grip-space late-latch; add `min_cutoff`/`beta` vars. Depends G5.
- **Acceptance.** gtest of the 1€ step against the reference implementation's outputs for a known
  input series. **Live Quest** to confirm reduced jitter without objectionable lag and to tune the
  two params. Default off, so shipping without it is safe.

---

## 7. Open questions for the user

1. **Bar vs full border vs both — default?** This doc defaults `openxr:grab_affordance=border`
   (whole frame grabbable) as the least surprising, but the visionOS/Quest convention that hands
   users know best is a **bottom bar**. Do you want `bar` (or `both`) as the default, at least when
   hands are the active device?
2. **Should `grab_anywhere` default true (preserve today's controller grip-anywhere) or false
   (deliberate borders for everyone)?** True keeps muscle memory for your Quest-controller sessions;
   false makes the border the one true way and is cleaner to teach.
3. **Bar edge — bottom (visionOS/Quest) or top (window-titlebar metaphor)?** Defaulting bottom.
4. **Late-latch vs filtered carry for hands.** Do you prefer zero-latency-but-jittery carry
   (keep the grip/pinch-space late-latch) or one-frame-latency-but-smooth (1€ filter, WP-G6)? The
   pinch-pose anchor (WP-G5) alone may make the carry good enough that G6 is unnecessary.
5. **Click on the border.** Should the border still pass clicks through to the desktop content under
   it (border overlaps real pixels), or reserve the border purely as chrome (no click, like the
   bar)? Reserving it is simpler to reason about but shrinks the usable content rect.
6. **Bar occlusion of content.** A bar drawn *into* the swapchain covers a strip of desktop pixels.
   Acceptable, or should the bar extend the quad **beyond** the content rect (taller quad, content
   unchanged) — more faithful to visionOS but changes the quad-size math?

---

## Sources

- Casiez, Roussel, Vogel. *1€ Filter: A Simple Speed-based Low-pass Filter for Noisy Input in
  Interactive Systems.* CHI '12, pp. 2527–2530.
  https://gery.casiez.net/publications/CHI2012-casiez.pdf · impl github.com/casiez/OneEuroFilter
- Meta Horizon OS — *Interactions Setup* (pinch = UI primitive; grab = loose fist, "don't clench too
  tightly"). https://developers.meta.com/horizon/documentation/unity/unity-handtracking-interactions/
- Meta Horizon OS — *Hand Tracking Overview* (PinchStrength 0→1).
  https://developers.meta.com/horizon/documentation/unity/unity-handtracking-overview/
- Runtime capabilities probed locally: `/usr/lib/wivrn/libopenxr_wivrn.so` (WiVRn 26.6.1),
  `/usr/lib/libopenxr_monado.so` (Monado v25.1.0) — `XR_EXT_hand_interaction` with
  `pinch_ext/value`, `pinch_ext/pose`, `grasp_ext/value`; `XR_EXT_hand_tracking`.
- Apple visionOS window bar / Meta Quest system-UI grab bar / SteamVR overlay grab — platform HIG
  conventions (dedicated handle + pinch + release stabilization).

---

## 8. USER DECISIONS (2026-07-07) — supersede §7 defaults

1. **Affordance**: bottom **bar only** for moving (auto-hide: alpha-fade in on hover/proximity,
   out when idle) + **grabbable corners for resizing**. No full-border grab zone.
2. **Chrome geometry**: expand the quad with a **transparent alpha margin** around the content —
   swapchain = content + margin; desktop blits into the inner rect (alpha=1), chrome renders in
   the margin (alpha 0 when hidden). `size:` keeps meaning CONTENT meters; quad meters = content
   + margins. Ray-hit UV remaps to the inner rect for clicks; margin hits classify as chrome
   (bar/corner). No desktop pixels are ever covered.
   Known costs accepted: inset math in swapchain sizing + both blit paths + CPU staging tex;
   UV remap in the pointer path; premultiplied alpha to avoid edge halo; hover-state-driven
   chrome redraws.
3. **Controllers keep grab-anywhere** (`openxr:grab_anywhere` default true for controllers;
   hands always use bar/corners).
4. **Smoothing**: release late-latch AND the 1€ carry filter are BOTH in scope (WP-G6 promoted
   from optional).
5. New scope vs §6: corner-resize handles (hands pinch a corner to resize; controllers keep
   stick-resize too) and bar auto-hide need to be folded into the WP slicing.

## 9. WP-G3 as implemented (2026-07-08)

- **Gating helper** (`XRMath.hpp`, pure/gtest): `grabActionForRegion(region, grabAnywhere,
  handActive) -> {NONE, MOVE, RESIZE_TL/TR/BL/BR}`. BAR→MOVE always; CORNER_*→RESIZE (that corner)
  always; BODY→MOVE iff `grab_anywhere && !handActive`; MARGIN/NONE→NONE. `handActive` is the
  WP-G5 slot (hands forced to chrome) — passed `false` today; G5 flips it with no rework. Replaces
  WP-G1's BODY hard-gate in both `CXRInput::processPointer` grab paths (hover + cone fallback).
- **Bar move-grab** reuses the existing move machine verbatim (`beginGrab`/`endGrab` + WP-G4 ring
  of carried QUAD poses); only the trigger region changed.
- **Corner resize** (`CXRAnchor::beginResize/grabResizeCorner/endResize`, pure/gtest): a resize
  does NOT device-lock the quad (`m_grabbed` stays false → `solve()` keeps running the persistent
  mode); instead it scales the CONTENT width in meters with the OPPOSITE corner pinned. `beginResize`
  snapshots the pinned corner + pin→corner diagonal (world) + start width; each frame the width comes
  from the grabbing grip's projection onto that fixed diagonal (aspect fixed by the pixel mode), the
  new content center is the diagonal midpoint, and the resized content pose is re-expressed via the
  existing `reanchorFromWorld`. Clamps to the SAME `XR_WIDTH_MIN/MAX` (0.2–4.0 m) the stick-resize
  uses — no new min/max vars; both paths mutate `m_state.widthMeters` under `m_layersMu`.
- **Anchor modes**: because resize routes through `reanchorFromWorld`, LOCAL pins the opposite
  corner exactly in world; head/body/device re-seed the offset (and spring) at the resized pose each
  frame, so the leash FOLLOWS the size change instead of fighting it and the mode is preserved (a
  head-leashed quad stays head-leashed). Orientation is held at the grab-start value for a stable
  diagonal.
- **Release latch reuse**: the WP-G4 ring is repurposed per grab kind — it holds carried QUAD poses
  for a MOVE (pose latch, as before) and GRIP poses for a RESIZE. `endResize` runs one final
  `grabResizeCorner` from `pickReleasePose(...)` (the latched / velocity-rejected GRIP sample), so a
  release jerk perturbs neither the final size nor the pinned-corner position — size gets the same
  lurch rejection the move path gets for free.
- **Config**: one new var `openxr:grab_anywhere` (bool, default true), read per-frame (hot-toggles).
- **Status**: `hyprctl openxr status` adds `grabKind` (`none`|`move`|`resize`); JSON keeps `grabbed`
  boolean for back-compat and adds a `"grabKind"` string.

## 10. WP-G6 as implemented (2026-07-08) — optional 1€ carry filter

- **Filter math** (`XRMath.hpp`, pure/unconditional/gtest): `SXROneEuro` (POD per-axis state:
  `init`, `xPrevRaw`, `xHat`, `dxHat`) + `oneEuroStep(state, value, dt, minCutoff, beta, dCutoff)`
  — a faithful transcription of the Casiez CHI'12 reference (github.com/casiez/OneEuroFilter,
  `1eurofilter.cc`): `dvalue = (value − lastRaw)/dt`, derivative low-passed at `dCutoff` (=1 Hz),
  speed-adaptive `cutoff = minCutoff + beta·|edvalue|`, value low-passed at that cutoff. First
  sample passes through; a non-positive `dt` holds the last output (guards the reference's `1/dt`).
  `SXROneEuroPose` bundles 3 position + 4 quaternion filters; `oneEuroStepPose` filters position
  per axis and the quaternion **component-wise + renormalize**, first flipping the incoming quat
  into the last filtered quat's hemisphere (q ≡ −q as a rotation, but a component low-pass across
  the antipode collapses toward 0). This is the reference repo's own guidance for rotations.
- **Reference-value derivation** (`tests/xr/one_euro.cpp`): the load-bearing gtest ports the
  reference `LowPassFilter`/`OneEuroFilter` C++ **verbatim in double precision** and drives it and
  `oneEuroStep` with the *same* 400-sample timestamped noisy ramp at the canonical params
  (minCutoff 1.0, beta 0.007, dCutoff 1.0, 90 Hz); every output must match within 2e-4 (float vs
  double drift). Two hand-computed anchors (beta 0 ⇒ constant cutoff, dt 0.1 s ⇒ α=0.3858695:
  feeding 0 then 1.0 gives 0.3858695 then 0.6228438) pin the algebra independently of the port.
  Plus: first-sample passthrough, monotone step-response convergence + no overshoot, ≥4× jitter
  variance reduction, dt robustness (hold on dt≤0, finite under varied dt), reset re-arms
  passthrough, pose axis-independence + unit-quaternion + hemisphere handling.
- **Carry-path change** (`CXRAnchor::solve` grab override): when `openxr:grab_filter` is set AND
  the grab is a hand grab (`beginGrab(..., handActive=true)`), the carried world pose
  `device ∘ offset` is run through `oneEuroStepPose` and the result is submitted **in LOCAL_FLOOR**
  (`res.space = XR_SPACE_LOCAL_FLOOR`) instead of the grip/pinch **device-space late-latch**.
  Rationale (§5.4): a filtered pose is no longer a rigid device-space offset, so the runtime's
  zero-latency late-latch cannot express it — we trade ~1 frame of latency for the removal of
  hand-tracking jitter. **Controllers and filter-off are bit-for-bit unchanged**: the gate is
  `in.grabFilter && m_grabHandActive && dev`, and the else-branch is the original
  `res.pose = m_grabOffset` + device-space selector. Only MOVE grabs filter (resize routes through
  `reanchorFromWorld`, untouched). Filter state resets at every `beginGrab`.
- **Latch interaction (WP-G4)**: `m_lastWorld` becomes the *filtered* pose, and the WP-G4 release
  ring pushes `anchor->lastWorld()` each frame, so in filtered mode the ring records the **filtered**
  carry poses. On release `pickReleasePose` → `endGrab(releaseWorld, …)` therefore re-anchors to a
  pose on the *smoothed* trajectory the user actually saw — the latch and the filter compose without
  a pop (the single-frame-grab fallback `endGrab(in,tune)` still uses the raw release pose, which is
  harmless with no history to smooth).
- **Config** (all read per-frame from the frame loop, hot-toggle): `openxr:grab_filter` (bool,
  default **false** for safety — flip after live tuning), `openxr:grab_filter_min_cutoff` (float Hz,
  default 1.0, 0.01–10), `openxr:grab_filter_beta` (float, default 0.007, 0–1).
- **Status**: `hyprctl openxr status` appends `, filtered` to a hand's input label
  (`hands (pinch, filtered)`) and a `"filtered"` bool per hand in JSON, true when `grab_filter` is on
  AND that hand is on the hand-interaction profile.

### Tuning guide (for the live Quest session — G6 is default-off until tuned)

Turn it on with `openxr:grab_filter = true` (hot-reloadable), then grab a monitor **by the bar with
a pinch** and adjust the two knobs while watching a held-still panel and a fast drag:

- **`min_cutoff` (Hz, default 1.0)** — the floor cutoff, i.e. how much smoothing applies when your
  hand is nearly still. **Lower it** (toward 0.1–0.5) if a panel you are holding *still* still
  jitters/shimmers. **Raise it** (toward 2–4) if slow, deliberate moves feel mushy/laggy. Too low ⇒
  visible lag and a "floaty" panel that trails your hand; too high ⇒ jitter returns at rest.
- **`beta` (default 0.007)** — how fast the cutoff opens up as you move faster (lag-vs-jitter during
  motion). **Raise it** (0.05–0.5) if *fast* drags feel laggy or rubber-banded; **lower it**
  (toward 0.001) if fast moves overshoot or the panel jitters while moving. Too low ⇒ fast moves
  lag; too high ⇒ you lose smoothing exactly when the hand is shakiest.
- **Method (from the 1€ paper):** set `beta = 0` first and lower `min_cutoff` until a *held-still*
  panel stops jittering with acceptable lag; then raise `beta` until *fast* moves have acceptable
  lag. Suggested trials: `(min_cutoff, beta)` = `(1.0, 0.007)` → `(0.5, 0.05)` → `(0.3, 0.2)`.
- If the pinch-pose anchor (WP-G5) alone already feels good, you may not need the filter at all —
  leaving `grab_filter = false` keeps the zero-latency device-space carry.

## 11. Live-tuned defaults shipped (2026-07-09, Quest 3)

After a live headset session the defaults changed to what validated on-device (replaces the
default columns above): `chrome_margin 0.04→0.10`, `chrome_bar_height 0.05→0.08`,
`chrome_bar_width_frac 0.6→0.8`, `chrome_corner_size 0.06→0.09` (bigger, easier grab targets);
`hand_grab pinch→both` paired with `hand_grab_anywhere grasp` (fist grabs anywhere, pinch stays
chrome-only + keeps its click); `grab_filter false→true` now default-on, extended to controllers
via the new `grab_filter_scope=hands|all` (default `all` — controllers reported carry jitter; the
filtered branch's LOCAL_FLOOR/late-latch-drop trade in §10 now applies to controllers too),
`grab_filter_beta 0.007→0.025`. **`grab_release_velocity_reject` was re-purposed from an ABSOLUTE
m/s threshold to a RELATIVE RATIO K (default 3.0, 0=off)** — the absolute threshold rewound
deliberate fast moves; now rejection triggers only when the peak speed in the ~80 ms release window
exceeds K× the typical carry speed — a lower-trimmed mean (mean of the faster half) of the preceding
samples, so a flick started from rest is judged by its flick pace, not dragged to ~0 by the
stationary just-grabbed samples like a median would be (`SXRGrabRing::releasePeakSpeed` /
`carryTypicalSpeed`, gtest-covered), so a uniformly fast flick (release ≈ carry pace) is kept while a
calm-carry-then-jerk fist-open is rewound past the jerk.
