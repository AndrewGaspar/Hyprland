# 15 — Direct (near-field) Pinch Manipulation & Touch (research, no implementation)

Trigger: live Quest 3 + WiVRn + passthrough feedback (2026-07-08): *"I'd like to be able to use
direct pinch manipulation when the target is actually physically reachable by me, but I was
disappointed that didn't seem to work. Though that lack of hand rendering in session probably
would make it harder."* — i.e. walk up to a floating monitor, pinch the bar with the fingers AT
the bar (not aiming a ray), touch/poke content directly.

Scope: what data we need vs have (verified against WiVRn 26.6.1 on this box), near/far
arbitration design, the hand-visibility problem, comfort prior art, WP sketch. Companion to
research/04 (grabbable borders — the grab machine this composes with), 07 (premium chrome),
09 (transparency / colorScaleBias).

---

## TL;DR

1. **Direct pinch-grab of the bar needs ZERO new OpenXR data.** The pinch pose
   (`pinch_ext/pose`) is already bound and sampled every frame (WP-G5,
   `src/openxr/XRInput.cpp:208`); it IS a fingertip-ish anchor (thumb–index contact point). A
   per-hand near/far mode keyed on *pinch-pose distance to the quad rectangle* — with
   hysteresis — plus "classify the region under the *projected contact point* instead of the
   ray hit" reuses the whole WP-G3/G4/G5/G6 grab machine unchanged. This is v1 (WP-D1+D2).
2. **Why it felt broken today (mechanical diagnosis):** grabs are gated on the *aim-ray hit*
   (`XRInput.cpp:573`, cone fallback ±5° `XRInput.hpp:80`). With your fingers ON the bar, the
   aim ray originates at your hand and exits the quad at a grazing, far-away UV (or misses) —
   the bar under your fingertips is never the classified region, so the pinch does nothing (or
   grabs a wrong region). Plus the hand-visibility problem (point 5) removes all feedback.
3. **WiVRn hand-tracking verified REAL, not simulated** (read-only probes, §1): the client lib
   advertises `XR_EXT_hand_tracking` + `XR_EXT_hand_tracking_data_source`; the wire protocol
   streams all 26 Quest joints per hand (pose + linear/angular velocity + per-joint radius),
   demand-driven and time-interpolated server-side. **`poke_ext/pose` (index fingertip) is also
   natively streamed** — touch/poke does NOT require consuming joints either. Full joints are
   only needed for multi-fingertip cursors and (future) hand-shaped punch-out.
4. **Near/far arbitration = the MRTK / Meta ISDK pattern**: per-hand mode, enter NEAR when the
   pinch pose is within `D_enter` of any quad, exit at `D_exit > D_enter` (hysteresis + short
   dwell); a NEAR hand casts **no ray at all** (Quest hides beams in direct-touch range); two
   hands run modes independently. Direct grab = same Schmitt trigger, same
   `grabActionForRegion` gating (bar/corner only for hands), same `beginGrab`/`beginResize`,
   same WP-G4 release latch + WP-G6 1€ filter — they are gesture- and acquisition-agnostic.
5. **The hand-visibility problem is real and structural**: quads are composition layers,
   composited OVER the passthrough image with no depth — a real hand reaching "onto" a monitor
   disappears behind it exactly at the moment of contact. Meta solves this system-side on
   Quest (hands re-composited above layers); **that mechanism does not exist over WiVRn**
   (`XR_FB_hand_tracking_mesh` absent from the client lib — strings-verified; WiVRn's server
   compositor has no hand knowledge). Pragmatic v1: **in-swapchain contact cursor** (ring that
   shrinks with distance, dot on contact — the Meta/HoloLens fingertip-cursor pattern, drawn in
   the existing `drawChrome` pass) plus **optional proximity fade** of the quad via
   `XrCompositionLayerColorScaleBias` (research/09 §2.1 machinery) so your real hand shows
   through the panel while reaching. Per-pixel hand punch-out from joints is a later experiment
   (WP-D7), not v1.
6. **Direct targets must be bigger than ray targets** (fingertip occlusion, no tactile stop —
   §4): when a hand is NEAR, swell the chrome (bar height / corner size ×~1.5) — geometry is
   already recomputed per frame (`makeChromeGeometry`, `XRMath.hpp:574`), so this is cheap and
   ties into research/07's hover-scale motion language.
7. **Poke-to-click is v2** (WP-D5): bind `poke_ext/pose`, run a press-plane machine (press on
   surface crossing inside the content region, ~2 cm press-through tolerance, release on
   recede — the MRTK poke pattern) and emit the existing MOTION_ABS/BUTTON events at the
   contact UV. Recommend shipping grab-only first: poke needs live-Quest tuning and adds
   accidental-touch risk while walking.
8. **Testing asymmetry** (verified in vendored monado): the remote test driver *synthesizes 26
   joints from 5 wire curl values* (`u_hand_sim_simulate_for_valve_index_knuckles`,
   `subprojects/monado/src/xrt/drivers/remote/r_device.c:141-180`; the harness already drives
   `hand_curl` — `hyprtester/src/xr/RemoteClient.cpp:130-132`) — so *joints-based* logic is
   headless-testable. But **no monado driver provides `pinch_ext/pose` / `poke_ext/pose`**
   (`XRT_INPUT_HAND_PINCH_POSE`/`POKE_POSE` appear only in `xrt_defines.h` + `bindings.json`),
   so pose-based direct interaction is live-Quest-only end-to-end. All mode-machine /
   projection / press-machine math must therefore live in `XRMath.hpp` as pure functions with
   gtests (the WP-G pattern).

---

## 1. Capability verification (what we have vs what direct interaction wants)

All probes read-only, performed 2026-07-09 on this box (WiVRn server **26.6.1**,
`/usr/bin/wivrn-server`; full distro debug sources on disk at
`/usr/src/debug/wivrn-server/WiVRn-26.6.1/`).

### 1.1 What we sample today (HypXRland, branch `hypxrland`)

Per hand, per frame (`SXRHandState`, `src/openxr/XRInput.hpp:119-129`):

| Signal | Source binding (hands = `ext/hand_interaction_ext`) | Bound? |
|---|---|---|
| `aim` pose | `/input/aim/pose` | yes (`XRInput.cpp:204`) |
| `grip` pose | `/input/grip/pose` | yes (`:205`) |
| `pinch` pose | `/input/pinch_ext/pose` — thumb–index contact point | yes (WP-G5, `:208`) |
| `select` / `pinchValue` | `/input/pinch_ext/value` | yes (`:206-207`) |
| `grab` (fist) | `/input/grasp_ext/value` | yes (`:209`) |
| **poke pose** | `/input/poke_ext/pose` — index fingertip | **NO — not bound** |
| **26 joints** | `XR_EXT_hand_tracking` (`xrCreateHandTrackerEXT` + `xrLocateHandJointsEXT`) | **NO — never created** |

`XR_EXT_hand_tracking` is *already enabled at instance level* when advertised
(`src/openxr/XRSession.cpp:76-82` pushes it into the extension list and records
`m_hasHandTracking`) — nothing consumes it yet.

### 1.2 WiVRn: hand tracking is advertised AND real (re-verified, deeper than research/04 §2)

`strings -a /usr/lib/wivrn/libopenxr_wivrn.so` (client lib the app loads):

- **Present**: `XR_EXT_hand_tracking`, `XR_EXT_hand_tracking_data_source`,
  `XR_EXT_hand_interaction`, `XR_EXT_palm_pose` (plus the previously known
  `XR_EXTX_overlay`, `XR_KHR_composition_layer_color_scale_bias`, …).
- **Absent** (relevant negatives): `XR_FB_hand_tracking_mesh`, `XR_FB_hand_tracking_aim`,
  `XR_EXT_hand_joints_motion_range`, any hand-occlusion/presence extension.

**Are the joints real Quest joints or server-side fakes? REAL.** From the on-disk WiVRn
sources:

- Wire packet `wivrn::from_headset::hand_tracking`
  (`common/wivrn_packets.h:373-405`): per hand, `std::optional<std::array<pose, XR_HAND_JOINT_COUNT_EXT>>`
  — **26 joints**, each with position, packed quaternion, **linear + angular velocity**,
  per-joint **radius** (uint16, 0.1 mm units, to dodge packet fragmentation) and
  validity/tracked flags. `static_assert(XRT_HAND_JOINT_COUNT == XR_HAND_JOINT_COUNT_EXT)`
  (`server/driver/hand_joints_list.cpp:25`). The headset client fills this from the Quest's own
  runtime and streams it; the server never synthesizes.
- Server side keeps a timestamped history and **interpolates joint sets to the app's requested
  time** (`hand_joints_list.cpp` `interpolate`/`extrapolate`), re-expressing joints relative to
  the wrist via monado relation chains.
- Serving path: `wivrn_controller::get_hand_tracking` answers
  `XRT_INPUT_HT_UNOBSTRUCTED_LEFT/RIGHT` and — key — **demand-drives the stream**:
  `cnx->add_tracking_request(device_id::LEFT_HAND/RIGHT_HAND, …)`
  (`server/driver/wivrn_controller.cpp:821-832`) feeds `tracking_control`, which tells the
  headset (1 s cadence, `server/driver/tracking_control.{h,cpp}`) which devices to sample and
  with what prediction window. So creating an `XrHandTrackerEXT` and locating it each frame is
  exactly what turns the joint stream on; not locating it costs nothing.
- `wivrn_session.cpp:121-125` registers the controllers as the `hand_tracking.unobstructed`
  role devices; `XR_EXT_hand_tracking_data_source` is wired (`ext_hand_tracking_data_source_enabled`
  string in the server binary), with the unobstructed (camera-tracked) source served by the
  same controller xdev.

**`poke_ext/pose` is natively streamed too** — it is its own tracked device pose on the wire
(`device_id::LEFT_POKE/RIGHT_POKE`, `wivrn_controller.cpp:618`, input
`XRT_INPUT_HAND_POKE_POSE` `:654,714`, located like grip/aim `:810-811`). Same for
`pinch_ext/pose` (`:617`). I.e. **fingertip data without ever touching joints**: the OpenXR
1.1 spec places the poke pose "at the surface of the extended index fingertip", +Z pointing
from fingertip toward the knuckle (the fingertip sphere's center sits one radius along +Z) —
purpose-built for "using a fingertip to touch and push a small object … typing on a virtual
keyboard" (`XR_EXT_hand_interaction`, registry.khronos.org). Exactly what a touch/poke
interactor wants.

### 1.3 Vendored Monado (test rig, `subprojects/monado` @ c2ddab59)

- `ext/hand_interaction_ext` profile in `src/xrt/auxiliary/bindings/bindings.json:209` has
  pinch/grasp/aim-activate *values*; the *poses* live in the virtual profile
  `/virtual_profiles/ext/hand_interaction_poses` (`bindings.json:53-80`) mapping
  `pinch_ext/pose → XRT_INPUT_HAND_PINCH_POSE`, `poke_ext/pose → XRT_INPUT_HAND_POKE_POSE`.
- **No driver in the tree produces those two inputs** (repo-wide grep: they appear only in
  `xrt_defines.h` and `bindings.json`). The remote test driver is a Valve Index controller
  (`r_device.c:198-207`) — as research/04 §2 found, it never surfaces a hand-interaction
  profile, so pinch/poke POSES are dead in the headless rig.
- **But the remote driver DOES hand tracking**: `supported.hand_tracking = true`, and
  `r_device_get_hand_tracking` synthesizes a full joint set from the five per-finger curl
  floats in the wire struct (`r_interface.h:80` `float hand_curl[5]`;
  `u_hand_sim_simulate_for_valve_index_knuckles`, `r_device.c:141-180`). Our pinned wire header
  already carries them and the harness already sets them
  (`hyprtester/src/xr/monado_remote_wire.hpp:70,79`, `hyprtester/src/xr/RemoteClient.cpp:130-132`).
  ⇒ **scripted, deterministic 26-joint hands over TCP 4242, headless** — if/when we consume
  `XR_EXT_hand_tracking` (WP-D6), including driving an index-tip through a quad.

### 1.4 Gap table

| Direct-interaction need | Canonical source | What we have | Verdict |
|---|---|---|---|
| A hand-anchored point near the fingers | pinch pose | **already sampled** | v1 uses it as-is |
| Fingertip for poke/touch | `poke_ext/pose` or joint `INDEX_TIP` | WiVRn streams poke pose; one new action+space | v2, cheap |
| Full skeleton (cursor per finger, occlusion, custom gestures) | `XR_EXT_hand_tracking` | advertised + real over WiVRn; unused | later (WP-D6) |
| Pinch strength for the grab edge | `pinch_ext/value` | already drives the Schmitt trigger | reuse |
| Hand visibility over the quad | system hand-presence compositing | **does not exist over WiVRn** | must build feedback ourselves (§3) |

**Honest assessment of the pinch-pose-only fallback**: for *bar-grab and corner-resize* it is
genuinely good — the pinch pose is the stabilized thumb–index contact point (research/04 §2.1),
which is what your fingers physically place on the bar; a ~1–3 cm offset from the visual
fingertips is absorbed by fat chrome targets + the forgiveness snap (§2.4). For *poke/touch*
it is wrong: the pinch pose sits between thumb and index, several cm behind the extended index
tip, and during a relaxed open hand it wanders — a poke press plane keyed on it would feel
mushy and offset. Poke should wait for `poke_ext/pose` (still not joints).

---

## 2. Interaction design — near/far arbitration

The classic XR input problem. Pattern sources: MRTK's per-hand near/far interactor switching,
Meta ISDK's direct-touch + distance-grab coexistence, visionOS direct touch vs eyes+pinch
(§4 for parameter values). Design below is fitted to our existing frame-thread pipeline
(`CXRInput::processPointer`, `src/openxr/XRInput.cpp:388+`, called under `m_layersMu` from
`OpenXRManager.cpp:1063` with the frame's solved `SXRPointerTarget` vector).

### 2.1 Distance metric

Per hand, per frame, over the same `pointerTargets` the ray already uses (each carries the
solved world pose + full-quad w/h + chrome geometry, `XRInput.hpp:91-99`):

1. Transform the **pinch pose** (fall back: grip pose for controllers, §2.6) into quad-local
   space (`poseInverse`/`qRotate`, `XRMath.hpp` has all of it).
2. `(u,v)` = projection onto the quad plane in full-quad UV; `dPlane` = signed local z
   (positive = viewer side).
3. **Distance-to-rectangle**, not distance-to-plane: clamp `(u,v)` into the quad (+ a lateral
   slack, e.g. 5 cm) and take the Euclidean distance from the hand point to that clamped
   surface point. This kills the classic bug where a hand *beside* a huge quad's infinite
   plane triggers NEAR.
4. `nearTarget` = argmin over targets; ties broken by |dPlane| (front-most surface).

New pure helper `pointQuadDistance(...) -> {dist, dPlane, u, v}` next to `rayQuadIntersect`
(`XRMath.hpp:350`) — gtest it exhaustively (WP-D1).

### 2.2 Per-hand mode machine (hysteresis, the anti-flap core)

```
        dist ≤ D_enter for N_dwell frames            dist ≥ D_exit  (or target vanished)
FAR ────────────────────────────────────▶ NEAR ────────────────────────────────────▶ FAR
      (defaults: D_enter 0.10 m, N ≈ 3 frames)         (default: D_exit 0.25 m)
```

- `D_enter < D_exit` (Schmitt in space, same idea as our trigger thresholds
  `openxr:grab_threshold`/`_release`). Prior-art anchors (§4.1): MRTK3 enters near mode at a
  0.1 m proximity sphere; MRTK2 at 0.25 m with an explicit ×1.4 exit (0.35 m); Meta ISDK poke
  hover uses 0.15 m enter / 0.20 m exit. Our 0.10/0.25 pair is deliberately stickier than
  ×1.4 because our quads are large targets and the reaching hand is invisible (§3) — favor
  staying NEAR. A short dwell (2–3 frames ≈ 25–40 ms at 72–90 Hz) rejects tracking blips; the
  *exit* side gets a longer dwell (~150 ms) so a momentary tracking loss mid-reach doesn't
  dump you to FAR.
- **Mode is frozen while that hand grabs or presses** — a grab that drifts past `D_exit`
  stays a direct grab until release (matches MRTK: the active interactor keeps selection).
- Hand loses tracking (`pinch == nullopt`): keep NEAR through the exit-dwell, then FAR.
- Per-hand and fully independent: left NEAR moving a close monitor while right FAR
  ray-scrolls a far one is a supported, arbitration-free state (the arrays in `CXRInput` are
  already all per-hand).
- State is frame-thread POD (obeys the `XRMonitorLayer.hpp` refcount rule); mirror one atomic
  per hand for `hyprctl openxr status` (`"inputMode": "far"|"near"`), like
  `m_handInputKindAtomic`.

### 2.3 NEAR-mode behavior (v1 = chrome grab only)

While a hand is NEAR:

- **Its ray is fully suppressed** — skip that hand in the ray-cast/hover/owner/scroll steps of
  `processPointer` (Quest/visionOS both hide the beam in direct range; a beam originating
  *inside* the panel produces garbage hits anyway — that's today's bug, TL;DR-2). The other
  hand's ray continues to work normally, including on the same quad.
- **Hover** comes from the projection: when `|dPlane| ≤ contact_band` (~6 cm) and inside the
  quad+slack, classify `(u,v)` with the existing `classifyQuadHit` (`XRMath.hpp:610`) and feed
  the existing chrome-hover plumbing (`m_hoverChromeMon`/`m_hoverRegion` — the WP-G2 chrome
  fade-in + highlight then just work, and the frame loop's atomics to the layer
  (`OpenXRManager.cpp:1067-1074`) light up unchanged).
- **No pointer events** in v1: a NEAR hand never becomes `m_owner`, never emits
  MOTION_ABS/BUTTON — so no fights with the single-seat cursor and zero risk of typing
  garbage into a window while grabbing. (Poke, v2, changes this deliberately.)
- **Grab edge**: the SAME per-hand Schmitt on `pinchValue`
  (`openxr:grab_threshold`/`_release`, `XRInput.cpp:559`), but the region decision uses the
  *projected* region instead of the ray region, then flows into the untouched machinery:
  `grabActionForRegion(region, grabAnywhere, handActive=true)` (`XRMath.hpp:430` — hands stay
  chrome-gated: bar → MOVE, corners → RESIZE, body → nothing),
  `anchor->beginGrab(hand, pinchPose, usePinch=true, handActive=true)` (`XRAnchor.hpp:209`) or
  `beginResize(hand, corner, pinchPose, aspect)` (`:233`). Per-frame carry, the WP-G4 release
  latch ring, velocity rejection, and the WP-G6 1€ filter all operate on the device-pose
  stream and are explicitly gesture-agnostic (`XRInput.hpp:294-300`) — **they compose with a
  direct grab with no changes**. Two real gaps found:
  1. The ±5° **cone fallback** acquisition (`XR_GRAB_CONE_DEG`, second chance at
     `XRInput.cpp:598`) is a far-field forgiveness and must be skipped in NEAR — replaced by
     the snap below.
  2. `beginResize` today receives the *grip* pose for hands (G3 predates G5's pinch anchor
     path for resize); direct corner-resize should pass the pinch pose for begin AND the
     per-frame `grabResizeCorner` + latched release — one consistent device space, same rule
     the MOVE path already follows (`m_grabDevicePinch`, `XRInput.hpp:289-292`).
- **Forgiveness snap (fat-finger)**: if the pinch closes while in the contact band but the
  projection classifies MARGIN/NONE/BODY, snap to the nearest bar/corner within
  `openxr:direct_snap_range` (~3–4 cm in quad meters); if none, the pinch is a no-op. Direct
  touch has no tactile stop and the fingers occlude the target — everyone's guidance says be
  generous (§4).
- **Feedback**: contact cursor + chrome swell (§3.2, §4) — mandatory, not optional; without
  visible hands this is the only confirmation channel. Hands have no haptics; a cursor
  color-state change on grab-begin plays that role (a themed sound later via research/07's
  companion).

### 2.4 Poke / direct touch on content (v2, WP-D5)

Bind `poke_ext/pose` (+ action space) — verified natively streamed by WiVRn (§1.2). Press
machine per hand, pure function + gtests, driven by the poke point's signed distance `dPoke`
to the *content* rect:

```
IDLE ──(in front, |lateral| in content, dPoke < hover_band)──▶ ARMED   (show cursor, cursor shrinks as dPoke→0)
ARMED ──(SWEPT segment prev→cur poke point crosses the surface, front side)──▶ PRESSED
        emit MOTION_ABS(crossing uv) + BUTTON(BTN_LEFT, down)
PRESSED: MOTION_ABS follows projected uv (drag works); tolerate penetration to −press_through (~2 cm);
         cancel (release at last uv) if penetration > max_pen (~10 cm — arm went through the panel)
PRESSED ──(dPoke > release_plane, ~+8 mm)──▶ ARMED   emit BUTTON up   (asymmetric press/release = debounce)
```

The press test MUST be a swept-trajectory crossing between consecutive frames, never an
instantaneous depth sample — at 72 Hz a fast poke moves several cm per frame and tunnels
straight through a plane test (both MRTK3 `PokeInteractor` and Meta ISDK use swept tests for
exactly this reason, §4.2). Release threshold sits between ISDK's 2 mm pull-off and MRTK2's
10 mm debounce; `enforce front push` (only front-side crossings press) and lateral roll-off
cancel (slide off the content rect ⇒ release) come straight from the MRTK/ISDK button specs.

- Emits ride the existing frame→main queue (`SXRInputEvent` MOTION_ABS/BUTTON,
  `XRInput.hpp:41-50`) and the existing click routing — nothing downstream changes.
- A pressing hand takes `m_owner` and the `m_leftHolder` slot exactly like a ray click; the
  single-pointer arbitration already handles two-hand conflicts.
- Scroll-by-drag comes for free (apps see a press-drag); kinetic touch-scroll is NOT in scope.
- Poke targets **content**, pinch targets **chrome** — no ambiguity inside one hand: poke
  fires only on surface *crossing* of the content region; pinch fires only on the
  pinch-value edge over bar/corner. (visionOS/ISDK run both concurrently the same way.)
- Ship behind `openxr:direct_poke = off|on` (default **off** in v1 — accidental presses while
  walking through a monitor are a real hazard; see open question Q3, and note a NEAR hand
  with poke off can never press anything, which is the safe default while the visibility
  problem is only partially solved).

### 2.5 Interaction with anchor modes

Nothing special: direct grabs use the same `beginGrab`/`endGrab`/`reanchorFromWorld` paths,
so LOCAL/head/body/device monitors behave exactly as they do for ray grabs (incl. research/13
adaptive handoffs later). One nicety: head-leashed quads within reach are *moving targets*
(they follow the head); the NEAR distance test uses the same frame's solved pose the ray
uses, so this is consistent frame-by-frame — no extra work.

### 2.6 Controllers too?

The identical machine works for controllers with `grip` pose distance + squeeze: walk up and
squeeze-grab the bar directly. Cheap to include (the mode machine is pose-agnostic), and Meta
supports controller direct touch. Recommend: include in WP-D2 with the pose picked per input
kind (pinch for hands, grip for controllers), gated by one shared `openxr:direct_interaction`
switch. Controllers keep `grab_anywhere` body-grabs in NEAR (they have a real trigger and no
occlusion problem, and user decision research/04 §8.3 keeps controllers permissive).

---

## 3. The hand-visibility problem (user-flagged) — solutions compared

**The structural fact**: our monitors are `XrCompositionLayerQuad`s. In `blend_mode=alpha`
the runtime lays them over the passthrough video with **no depth test** — the quad always
wins. A hand reaching to the bar is *behind* the quad in composition order even though it is
*in front of it* in depth ⇒ your real hand visually vanishes exactly when it matters. On
Quest-native, Meta re-composites tracked hands ON TOP of layers system-side ("hands over UI" /
Direct Touch affordances); nothing equivalent exists in the WiVRn→server-composite→encode
pipeline (§1.2 — the server compositor is layer-only, hand-blind; research/09 §TL;DR-1
describes that pipeline).

| Option | Mechanism | Availability | Cost | Verdict |
|---|---|---|---|---|
| (a) System hand punch-through (`XR_FB_hand_tracking_mesh` / hand-presence layers) | runtime composites hand mesh/video above layers | **absent**: not in WiVRn client lib (strings), not in vendored Monado (no oxr implementation). Stronger: **no OpenXR extension anywhere provides compositor-level hands-over-layers** (verified negative across the 1.1+EXT spec by the prior-art pass, §4.4) — even on Quest, Meta's sanctioned app-side pattern is *touch limiting* (clamp the rendered hand at the panel), which is meaningless for real passthrough hands; `XR_META_environment_depth` hand-removal is a depth-occlusion helper, also not on WiVRn | — | **REJECT** (nothing to adopt; revisit only if WiVRn grows a hand-presence feature) |
| (b) **In-swapchain contact cursor / hand ghost** | draw a hover cursor at the projected hand point in the quad's own margin+content pixels (extend the WP-G2 `drawChrome` pass, `XRGraphics.hpp:62-77`) | works today, pinch pose only | trivial fragment work; chrome-only redraws already exist (`OpenXRManager.cpp:808-811`); NEAR forces per-frame chrome redraw of ONE quad | **v1 RECOMMENDED** |
| (c) Per-pixel alpha punch-out of the quad where the hand is | project 26 joints → capsule silhouette SDF → write premultiplied `rgba=0` holes during blit | needs WP-D6 joints; silhouette is blobby; joint latency vs passthrough-video latency mismatch ⇒ swimming holes; in `opaque`/hypxrpaper env a hole shows background, not your hand | moderate GPU + real polish risk | **DEFER** — flagged experiment (WP-D7), only meaningful in passthrough |
| (d) **Proximity fade** — quad drops opacity as a hand reaches | per-layer `XrCompositionLayerColorScaleBias`, `colorScale=(f,f,f,f)` premultiplied rule (research/09 §2.1, verified WiVRn-supported) | works today; needs the small CSB-chaining plumbing (research/09 WP-T1) | free (runtime shader) | **v1 companion** — default ON, mild |

**Recommended v1 = (b) + (d):**

- **(b) Contact cursor**: ring centered at the projected `(u,v)`, radius shrinking with
  `|dPlane|` (the Meta fingertip-cursor / HoloLens pattern §4), filling to a dot at contact;
  uses the chrome color vars (`openxr:chrome_col_hover`/`_grab`,
  `src/config/values/ConfigValues.cpp:777-778`). Draw it for any NEAR hand inside the hover
  band — over chrome AND content (the cursor must follow the hand onto the bar *and* precede
  a future poke on content; `drawChrome` composites after the content blit, so drawing over
  content pixels is the same pass). This directly answers "lack of hand rendering": you get a
  touchscreen-style hover cursor where your fingers are, in both passthrough and opaque
  environments.
- **(d) Proximity fade**: while a hand is within the contact band of a quad, fade THAT quad
  toward `openxr:direct_fade_opacity` (default ~0.7) over ~120 ms; restore on exit/grab.
  In passthrough your real hand becomes visible *through* the panel exactly during the reach —
  cheap and elegant, and it composes with all of research/09's transparency work (share the
  envelope code). In `opaque` it still reads as "the panel acknowledges your hand". Keep it
  OFF for a quad that is being poke-pressed (fading the thing you're reading while touching
  it is counterproductive — fade hardest during *approach*, ease back to full at contact:
  scale `f` by `smoothstep(contact_band → 0, |dPlane|)` so contact ≈ solid again).

With joints (WP-D6) the cursor upgrades to five fingertip dots / a soft hand-shadow blob
(the "shadow on the panel" affordance Meta uses), same draw pass, no architectural change.

---

## 4. Comfort / ergonomics prior art (parameter values)

Gathered 2026-07-09 by a dedicated web pass; every number below is from shipped source code or
official docs (URL list in §7). Where the big SDKs publish no number, that is stated —
several folklore values were explicitly debunked.

### 4.1 Near/far arbitration — how everyone does it, with real numbers

| System | Mechanism | Enter | Exit / hysteresis |
|---|---|---|---|
| MRTK3 | `InteractionModeManager`: prioritized per-hand modes; "Near" mode's interactor-type list simply omits the ray → ray disabled | 0.1 m trigger sphere on the hand (`NearInteractionModeDetector`) | +1 physics frame only, BUT mode is **latched while any near interactor has a selection** |
| MRTK2 | `DefaultPointerMediator`: far pointers disabled while a near pointer is "near an object"; HL2-matched cone 66°, sphere pulled 8 cm behind palm | 0.25 m (cast 0.05 + margin 0.20) | **0.35 m — explicit ×1.4** (`nearObjectSmoothingFactor 0.4`) |
| Meta ISDK | per-hand `InteractorGroup`, priority order + candidate scoring; poke winning hover starves the ray interactor → beam never draws; **selecting interactor is locked until selection ends** | poke hover 0.15 m (`_enterHoverNormal`, v72) | 0.20 m (`_exitHoverNormal`) — note Meta *raised* this pair 5× from the old 0.03/0.05 |
| Meta Quest system ray | beam fades out over the **last 5 cm** of hand-to-panel approach (official raycasting spec) | — | — |
| Quest Direct Touch (v50) | system panels come within arm's reach; index fingertip only; ray+pinch stay concurrently available | **no published switch distance** (any specific number is folklore) | — |
| visionOS | *no distance threshold at all*: direct = actual geometric intersection of the hand with content, "within arm's reach"; per-event kinds `touch`/`directPinch`/`indirectPinch` | — | — |

Convergent recipe (their words, our design): per-hand independent modes (all three SDKs);
near-mode entry 10–25 cm with exit hysteresis of ×1.4 / +5 cm; **always latch the mode while a
grab/press is in progress**; kill the ray entirely for the near hand. §2.2 follows all four
rules.

### 4.2 Press / poke mechanics

| Parameter | MRTK2 | MRTK3 | Meta ISDK (v72) | Ours (§2.4) |
|---|---|---|---|---|
| Fingertip proxy | sphere, prefab 0.15 cast | **5 mm sphere** (`DefaultPokeRadius`), swept along inter-frame trajectory | **5 mm sphere** at index tip, **swept crossing test** | poke pose point, swept segment |
| Press threshold | 2 cm push (`pressDistance`); shipped HL2 button ≈ 8.5 mm travel | select at 0.9 of push depth, deselect at 0.1 (huge normalized hysteresis) | surface crossing | surface crossing (front side only) |
| Release back-off | **1 cm** (`releaseDistanceDelta`) + 1 cm touch debounce pull-off | 0.9/0.1 progress | **2 mm** rise (`_touchReleaseThreshold`); optional RecoilAssist ±2 cm | ~8 mm (between the two) |
| Press-through / cancel | `maxPushDistance` 0.2 m | `endPushPlane` 0.2 m | cancel at **30 cm** through (`_cancelSelectNormal`), 3 cm lateral slide-off | cancel at 10 cm through |
| Anti-drag confusion | — | `rejectXYRollOff`, `enforceFrontPush` | 1 cm drag thresholds normal+tangent | front-push only, roll-off release |
| Missing haptics | proximity light + sound | same | system "tap" sound; **touch limiting** (visual hand clamped at panel = pseudo-haptic) | cursor state change (+ themed sound later, research/07 companion) |

### 4.3 Target sizing — why direct targets must be bigger

- **Meta**: poke targets **minimum 22×22 mm with ≥12 mm spacing**, index finger only ("other
  fingers less accurate"); avoid interaction closer than 10 cm to the HMD; combine poke with
  ray+pinch rather than relying on poke alone.
- **Microsoft**: direct-touch minimum **1.6×1.6 cm @ 45 cm** (≥2° visual angle), recommended
  button **3.2×3.2 cm @ 45 cm** — the shipped HoloLens 2 button collider is exactly
  32×32×16 mm; ray/gaze targets are allowed *smaller in angle* (3.5 cm @ 2 m = 1°). I.e.
  Microsoft's direct targets are ~2× the angular size of its ray targets — fingertip
  occlusion + no tactile stop is the stated rationale.
- **visionOS**: 60 pt minimum everywhere, and points are angular: **60 pt ≈ 2.5° ≈ 4.4 cm at
  1 m**; centers ≥60 pt apart.
- **Ultraleap**: buttons ≥2 cm, light up on approach, depress like mechanical buttons;
  TouchFree kiosks 20–30 mm + ≥5 mm spacing.

**Implication for our chrome**: current defaults — bar height 0.05 m
(`ConfigValues.cpp:763`), corners 0.06 m (`:767`) — already clear Meta's 22 mm floor and
Microsoft's 32 mm recommendation, so direct bar-grabs are ergonomically viable with today's
geometry. The swell (×1.5 in NEAR, §2.3) is still worth it as the approach affordance and to
compensate fingertip occlusion of the exact edge, rather than shipping permanently fatter
chrome that wastes margin in FAR mode. Swell also doubles as the approach affordance
(Ultraleap's "light up on approach", research/07's hover-scale).

### 4.4 Contact affordances (the cursor patterns to copy)

- **HoloLens 2 donut cursor** (Microsoft direct-manipulation doc): ring stays parallel to the
  surface, **gradually shrinks with distance, collapses to a dot at contact** + touch event;
  paired with a **ProximityLight** — a glow blob projected on the surface under the fingertip
  that brightens/shrinks on approach. MRTK2 `FingerCursor` + MRTK3 ring (`RingMagnetism`) are
  the shipped implementations; engagement range = the poke `touchableDistance` (0.2 m).
- **visionOS**: buttons show a hover state with "a highlight that gets brighter as you
  approach the button surface" (WWDC23); contact is "accompanied by matching spatial sound".
- **Meta Quest shell**: shrinking fingertip ring + under-finger glow observable in Direct
  Touch, but no official spec exists (flagged unverified); documented guidance is only "Z
  distance needs to be clearly communicated".
- Depth-cue literature for the shadow variant: Shadow Reaching (UIST '07), ShadowTouch
  (UIST '23) — a cast shadow communicates finger-surface distance pre-contact.

§3-(b)'s ring-shrinks-to-dot cursor is a direct copy of the HL2 pattern; the proximity fade
(§3-(d)) plays the ProximityLight/brightening role with the machinery we already have.

### 4.5 OpenXR spec facts locked down (registry-verified)

- `XR_EXT_hand_tracking`: 26 joints (`XR_HAND_JOINT_COUNT_EXT=26`,
  `XR_HAND_JOINT_INDEX_TIP_EXT=10`), per-joint radius in meters, velocities via chained
  `XrHandJointVelocitiesEXT` — matches the WiVRn wire content 1:1 (§1.2).
- `XR_EXT_hand_interaction` mandates all four poses (aim/grip/pinch/poke) on every hand
  profile once enabled; poke pose "at the surface of the extended index fingertip", +Z toward
  the knuckle; pinch value "should be linear to the distance between the finger and thumb
  tips", 0 in a relaxed open hand; `ready_ext` gates values to 0 when the hand isn't ready.
- `XR_EXT_hand_tracking_data_source`: request UNOBSTRUCTED vs CONTROLLER sources at tracker
  creation; WiVRn wires the unobstructed source (§1.2).
- **Negative result**: no OpenXR extension exists for compositor-level hands-over-layers
  (§3-(a)); `XR_FB_hand_tracking_mesh`/`_capsules`/`_aim` are app-side data providers, and
  none are on WiVRn anyway.

---

## 5. Work-package sketch (WP-D1 … D7)

All frame-thread state POD (XRMonitorLayer.hpp refcount rule); all decision math pure in
`XRMath.hpp` + gtests (WP-G precedent). Config in `ConfigValues.cpp`, read per-frame via
`CConfigValue` (hot-tunable during a live session — essential here, thresholds are
feel-driven).

- **WP-D1 — Geometry + mode machine (pure, headless)**: `pointQuadDistance` (point→rect
  distance + projection uv + signed plane distance), per-hand NEAR/FAR machine with
  enter/exit hysteresis + dwell + grab-freeze, nearest-target selection, forgiveness-snap
  helper (nearest bar/corner within range from a uv). Pure structs/functions + exhaustive
  gtests. No behavior change yet. *Headless.*
- **WP-D2 — Direct chrome grab (the core feature)**: wire the mode machine into
  `processPointer`; NEAR suppresses that hand's ray/hover/scroll/cone-fallback; projected-uv
  hover feeds existing chrome plumbing; pinch edge → snap → `grabActionForRegion` →
  `beginGrab`/`beginResize` with the pinch pose (fix the resize-begin grip/pinch consistency
  gap, §2.3-2); controllers via grip+squeeze; status JSON `inputMode`; config:
  `openxr:direct_interaction` (bool, default on), `openxr:direct_enter_range` (0.10),
  `openxr:direct_exit_range` (0.25), `openxr:direct_contact_band` (0.06),
  `openxr:direct_snap_range` (0.04). Gating/selection functions pure+gtested; end-to-end is
  **LIVE QUEST REQUIRED** (no pinch pose in the test rig, §1.3). Deps: D1.
- **WP-D3 — Contact cursor + chrome swell**: cursor ring/dot in the `drawChrome` pass at the
  projected point (over margin and content), radius ∝ distance, grab/press color states;
  per-frame chrome redraw for quads with a NEAR hand; chrome geometry swell
  (`openxr:direct_chrome_scale`, ~1.5× bar height + corner size when NEAR, eased — reuses
  `makeChromeGeometry` inputs; keep hit-classification and drawing consistent by scaling the
  *inputs* once per frame). **LIVE QUEST for feel**; draw math unit-testable. Deps: D2 (D1
  for projection only — can start after D1 with fake mode state).
- **WP-D4 — Proximity fade**: minimal per-layer `XrCompositionLayerColorScaleBias` chaining
  (or rebase onto research/09 WP-T1 if that lands first — coordinate, do not duplicate),
  approach-eased fade per §3-(d), `openxr:direct_fade` (bool, on) +
  `openxr:direct_fade_opacity` (0.7). Submit-side testable headless (inspect chained struct);
  feel live. Deps: D2 (mode signal); soft-dep research/09 WP-T1.
- **WP-D5 — Poke-to-click (v2)**: bind `poke_ext/pose` + action space (profile already
  offers it on WiVRn and in monado's bindings.json — suggestion cannot fail); press machine
  §2.4 (pure + gtests) emitting MOTION_ABS/BUTTON at contact uv; pointer-owner integration;
  `openxr:direct_poke` (default off), `openxr:poke_press_through` (0.02),
  `openxr:poke_release_mm` (8), max-penetration cancel (0.10). **LIVE QUEST REQUIRED**
  end-to-end. Deps: D2, D3 (cursor over content is the poke affordance).
- **WP-D6 — Consume `XR_EXT_hand_tracking` (enabler, optional for v1)**:
  `xrCreateHandTrackerEXT` per hand at session start (gated on `m_hasHandTracking` +
  `XrSystemHandTrackingPropertiesEXT`), `xrLocateHandJointsEXT` at
  `predictedDisplayTime`/refSpace on the frame thread next to `sample()`; expose
  index-tip/thumb-tip + radii in `SXRHandState`; upgrade the cursor to fingertip dots;
  fallback poke point when `hand_interaction` is absent but joints exist. **Headless-testable**
  (remote-driver curl synthesis, §1.3 — extend hyprtester with a joints-based direct-grab
  test the pose path can't have). Deps: none (parallel to D2+); D7 requires it.
- **WP-D7 — Hand punch-out experiment (flagged, passthrough-only)**: joints→capsule SDF
  silhouette rendered as premultiplied `rgba=0` into the quad where the hand overlaps.
  Prototype behind `openxr:direct_hand_cutout` (default off); evaluate swimming/latency
  honestly, expect rejection. Deps: D6. **LIVE QUEST REQUIRED.**

Suggested order: D1 → D2 → D3 (v1 ships here) → D4 → D5; D6 parallel after D1; D7 last, maybe
never.

---

## 6. Open questions for the user

1. **Thresholds**: happy with enter 10 cm / exit 25 cm / contact band 6 cm as starting
   defaults (all hot-tunable live)? These are the numbers to tune in-headset during D2
   review.
2. **Ray suppression**: OK that a NEAR hand casts no ray at all (can't ray-point at a far
   monitor while standing at a near one with that hand)? Recommended yes — matches
   Quest/visionOS and kills the garbage-ray bug class; the *other* hand still has a ray.
3. **Poke in v1 or grab-only v1?** Recommendation: grab-only v1 (D2+D3), poke behind
   `openxr:direct_poke=off` in D5 — accidental content clicks while reaching/walking are the
   main risk the big SDKs spend their tolerance budgets on.
4. **Cursor style**: shrinking ring→dot (recommended), plain dot, or soft shadow blob?
   And should it also appear for FAR ray hover (a unified cursor language) or stay
   NEAR-only (recommended NEAR-only; the desktop's own cursor already serves FAR)?
5. **Proximity fade default**: on at 0.7 (recommended), off by default, or passthrough-only
   (skip when `blend_mode=opaque`/hypxrpaper env is dark)?
6. **Chrome swell**: swell chrome ×1.5 when a hand is near (recommended), or keep geometry
   static and rely on the snap forgiveness only?
7. **Controllers**: include controller direct-grab (grip proximity + squeeze) in D2
   (recommended), or hands-only?

---

## 7. Sources

**Local code (branch `hypxrland`)**
- `src/openxr/XRInput.{hpp,cpp}` — hand state, bindings (`XRInput.cpp:203-211`), ray/hover/grab
  pipeline (`:388-660`), G4 latch + G5 pinch anchoring + G6 filter plumbing.
- `src/openxr/XRMath.hpp` — `rayQuadIntersect:350`, `classifyQuadHit:610`,
  `grabActionForRegion:430`, `makeChromeGeometry:574`, 1€ filter `:233-333`.
- `src/openxr/XRAnchor.hpp:209-235` — `beginGrab/endGrab/beginResize/grabResizeCorner/endResize`.
- `src/openxr/XRSession.cpp:70-90` — `XR_EXT_hand_tracking` already enabled when advertised.
- `src/openxr/OpenXRManager.cpp:734-811` (chrome redraw), `:953-1074` (targets,
  `processPointer`, hover/grab atomics to layers).
- `src/openxr/XRGraphics.hpp:58-77` — `drawChrome` (WP-G2 pass the cursor extends).
- `src/config/values/ConfigValues.cpp:712-786` — existing `openxr:*` vars.
- `hyprtester/src/xr/{RemoteClient.cpp:130-132, monado_remote_wire.hpp:70-79}` — wire curls.
- `docs/openxr/research/04-grabbable-borders.md` (§2, §2.1, §5, §8-§10),
  `07-premium-chrome.md` (§4.1), `09-monitor-transparency.md` (§2.1, TL;DR).

**Read-only binary/source probes (this box, 2026-07-09)**
- `strings -a /usr/lib/wivrn/libopenxr_wivrn.so` — extension inventory (presence AND absence).
- `/usr/src/debug/wivrn-server/WiVRn-26.6.1/` (distro debug sources, WiVRn 26.6.1):
  `common/wivrn_packets.h:373-405` (26-joint wire packet),
  `server/driver/hand_joints_list.cpp` (interpolation),
  `server/driver/wivrn_controller.cpp:610-660,806-862` (pinch/poke pose devices,
  `get_hand_tracking`, demand-driven `add_tracking_request`),
  `server/driver/wivrn_session.cpp:121-166,645-651,1290-1330` (roles, packet dispatch,
  feature inc/dec), `server/driver/tracking_control.{h,cpp}` (stream gating).
- `subprojects/monado` @ c2ddab59: `src/xrt/auxiliary/bindings/bindings.json:53-80,209-260`
  (hand-interaction profile + pose virtual profile),
  `src/xrt/drivers/remote/r_device.c:141-207` (curl→joints synthesis, Index profile),
  `src/xrt/drivers/remote/r_interface.h:74-90` (wire struct).

**Web (fetched 2026-07-09; all §4 numbers trace to one of these)**

*Microsoft / MRTK*
- MRTK3 interaction modes: https://learn.microsoft.com/en-us/windows/mixed-reality/mrtk-unity/mrtk3-input/packages/input/interaction-mode-manager
- MRTK3 source (mode manager, detectors, PokeInteractor, PressableButton, StatefulInteractable, hand prefabs): https://github.com/MixedRealityToolkit/MixedRealityToolkit-Unity — `org.mixedrealitytoolkit.input/InteractionModes/{InteractionModeManager,ProximityDetector,NearInteractionModeDetector}.cs`, `Interactors/Poke/PokeInteractor.cs`, `Assets/Prefabs/MRTK {Interaction Manager,LeftHand Controller}.prefab`; `org.mixedrealitytoolkit.uxcore/Button/PressableButton.cs`, `org.mixedrealitytoolkit.core/Interactables/StatefulInteractable.cs`
- MRTK2 source: https://github.com/microsoft/MixedRealityToolkit-Unity — `SpherePointer.cs`, `PokePointer.cs`, `DefaultPointerMediator.cs`, `BaseNearInteractionTouchable.cs`, `PressableButton.cs`, `Prefabs/Pointers/{GrabPointer,ConicalGrabPointer}.prefab`, `PressableButtonHoloLens2_NoLabel.prefab`
- MRTK2 pointers doc: https://learn.microsoft.com/en-us/windows/mixed-reality/mrtk-unity/mrtk2/features/input/pointers
- Button sizing: https://learn.microsoft.com/en-us/windows/mixed-reality/design/button
- Direct manipulation + donut cursor + ProximityLight: https://learn.microsoft.com/en-us/windows/mixed-reality/design/direct-manipulation ; fingertip visualization: https://learn.microsoft.com/en-us/windows/mixed-reality/mrtk-unity/mrtk2/features/ux-building-blocks/fingertip-visualization and .../mrtk3-input/packages/input/fingertip-visualization

*Meta*
- ISDK poke: https://developers.meta.com/horizon/documentation/unity/unity-isdk-poke-interaction/ ; interactor groups: https://developers.meta.com/horizon/documentation/unity/unity-isdk-interactor-group/ ; distance grab: https://developers.meta.com/horizon/documentation/unity/unity-isdk-distance-grab-interaction/
- ISDK serialized defaults (v72 package mirrors): `com.meta.xr.sdk.interaction@72.0.0` via https://github.com/musimathicslab/MarcoSmiles_AR2.0 ; older Oculus Integration values via https://github.com/NovaUI-Unity/AppleXRConcept ; https://github.com/Shopify/handy
- System ray spec (5 cm beam fade): https://developers.meta.com/horizon/design/raycasting_specs/
- Hands design guidance (22 mm / 12 mm, index-only): https://developers.meta.com/horizon/design/hands-interaction-types/ ; best practices: https://developers.meta.com/horizon/design/hands-best-practices/
- Direct Touch v50: https://www.meta.com/blog/meta-quest-v50-direct-touch-in-game-multitasking/ ; https://about.fb.com/news/2023/02/meta-quest-direct-touch-use-your-fingers-in-vr/ ; hands-on: https://skarredghost.com/2023/05/03/quest-direct-touch-review/
- Passthrough occlusion / Depth API: https://developers.meta.com/horizon/documentation/unity/unity-customize-passthrough-passthrough-occlusions/

*Apple*
- WWDC23 "Design for spatial input": https://developer.apple.com/videos/play/wwdc2023/10073/ ; "Design considerations for vision and motion": https://developer.apple.com/videos/play/wwdc2023/10078/
- HIG: gestures https://developer.apple.com/design/human-interface-guidelines/gestures ; eyes / spatial layout https://developer.apple.com/design/human-interface-guidelines/spatial-layout
- SpatialEventCollection.Event.Kind (`directPinch`): https://developer.apple.com/documentation/swiftui/spatialeventcollection/event/kind-swift.enum
- WWDC25 "Design hover interactions" (60 pt ≈ 2.5° ≈ 4.4 cm @ 1 m): https://developer.apple.com/videos/play/wwdc2025/303/

*Khronos / other*
- OpenXR 1.1 spec: `XR_EXT_hand_tracking`, `XR_EXT_hand_interaction` (poke pose wording), `XR_EXT_hand_tracking_data_source`, `XR_FB_hand_tracking_mesh`, `XR_META_environment_depth`: https://registry.khronos.org/OpenXR/specs/1.1/html/xrspec.html
- WiVRn hand-interaction exposure: https://github.com/WiVRn/WiVRn/issues/238
- Ultraleap UI components: https://docs.ultraleap.com/xr-guidelines/Components/ui-components.html ; TouchFree: https://docs.ultraleap.com/TouchFree/touchless-interfaces/interactions.html
- Shadow Reaching (UIST '07): https://www.cs.ubc.ca/labs/imager/tr/2007/Shoemaker2007/shoemaker_shadow_reaching.pdf ; ShadowTouch (UIST '23): https://dl.acm.org/doi/10.1145/3586183.3606785
- Mode-switch literature: Gaze+Pinch (SUI '17) https://dl.acm.org/doi/10.1145/3131277.3132180 ; HOMER (I3D '97) https://dl.acm.org/doi/10.1145/253284.253301 ; Go-Go (UIST '96) https://dl.acm.org/doi/10.1145/237091.237102
