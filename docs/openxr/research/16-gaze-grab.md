# 16 — Gaze-Vector Monitor Selection + Keybind Grab / Push / Pull / Release

Status: **research / design only. Nothing here is implemented.** No live runs, no code changes, no
commits beyond this file. Author: research pass 2026-07-11 against branch `hypxrland`
(worktree base `76b03233`), source-read only.

The ask (user's words): *"Gaze vector monitor selection + key bind to grab the monitor I'm looking
at, then key bind support for pulling the monitor closer to / pushing it further, and finally a key
bind to release it. Ideally could also bind a key to 'hold' through the motion. For headsets
without eye tracking, this will just be the center FOV gaze."*

I.e. keyboard-driven monitor manipulation steered by where you are *looking*: look at a monitor →
press a bound key to grab it → it follows your gaze while held (or toggled) → keys pull it nearer /
push it farther along the gaze ray → a key releases/docks it. The user's device is a **Quest 3
(no eye tracking) via WiVRn**, so v1 gaze is the head-pose forward vector (center FOV); the design
slots real eye tracking in later.

Cross-refs (source of truth): `src/openxr/XRAnchor.{hpp,cpp}` (grab override + carry + reanchor
machinery this is built on), `src/openxr/XRInput.{hpp,cpp}` (ray cast / hover / grab state machine),
`src/openxr/XRMath.hpp` (`rayQuadIntersect`, `oneEuroStepPose`, region classifier),
`src/openxr/OpenXRManager.cpp` (frame-loop solve `:843-925`, pointer + hover publish `:1058-1078`,
verbs `:1921-2127`, `resolveSelected` `:1619`), `src/openxr/XRMonitorLayer.hpp` (THREAD-SAFETY RULE
+ the frame→main atomic write-back contract `:140-150`), `src/managers/KeybindManager.cpp` (bind
dispatch + `r`/`e` flags), `src/config/legacy/DispatcherTranslator.cpp:800` (`xrmonitor` dispatcher
funnel). Composition: `docs/openxr/research/13-adaptive-anchoring.md` (dock/roam decorator — gaze
grab must suspend its geofence like a hand grab does), `docs/openxr/research/14-ray-aim-assist.md`
(aim jitter / hover hysteresis / 1€ filter / target-highlight analysis — reused wholesale for the
gaze ray), `docs/openxr/research/07-premium-chrome.md` (chrome visual vocabulary for the
selection highlight).

---

## TL;DR

1. **This is ~90% reuse of machinery that already exists**, wired to a new *input source* (the head
   ray) and a new *trigger* (a keybind instead of a squeeze). The gaze ray reuses
   `rayQuadIntersect` (`XRMath.hpp:350`) exactly as the controller ray does; the carry reuses the
   `solve()` grab-override shape (`XRAnchor.cpp:217`); "pull nearer / push farther" is
   `grabPushPull`/`applyDistance` distance math (`XRAnchor.cpp:400`/`:603`) already shipped for the
   stick and the `xrmonitor distance` verb; release is `reanchorFromWorld` (`XRAnchor.cpp:502`), the
   same primitive `endGrab`/`setMode` use. The genuinely *new* code is small: a gaze-hover pass, a
   gaze-carry `solve()` branch, four dispatchers, and a main↔frame handoff for the keybind.

2. **v1 gaze = the VIEW-space head-forward vector, which the frame loop already locates every frame**
   (`OpenXRManager.cpp:843-852`, `viewPose` = `xrLocateSpace(m_viewSpace, m_refSpace, …)`). Center
   FOV = `qRotate(viewPose.rot, {0,0,-1})`. **Zero new OpenXR plumbing for v1** — no action, no
   xrSyncActions dependency, works headless (the Monado remote driver already scripts the head
   pose, so the whole feature is gtest-/hyprtester-testable exactly like research/13's geofence).

3. **Real eye tracking (`XR_EXT_eye_gaze_interaction`) is available in the WiVRn runtime binary but
   NOT on the Quest 3 device.** Strings-probe of the installed runtime
   (`/usr/lib/wivrn/libopenxr_wivrn.so`) shows the full extension is compiled in:
   `XR_EXT_eye_gaze_interaction`, `/user/eyes_ext/input/gaze_ext/pose`,
   `XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT`, `XR_TYPE_SYSTEM_EYE_TRACKING_PROPERTIES_FB`
   (§2.2). But eye-tracking is a *device* capability: Quest 3 / 3S have **no eye-tracking hardware**,
   so `XrSystemEyeGazeInteractionPropertiesEXT::supportsEyeGazeInteraction` returns `XR_FALSE` and
   the `/interaction_profiles/ext/eye_gaze_interaction` binding never goes active — **only Quest
   Pro** (and Vision-Pro-class HMDs) report it (§2.3). So the eye path is *design-for*, not
   *build-now*: an optional `SXRSolveInput::gaze` field + one action, gated on the system-property
   probe, transparently falling back to VIEW. The vendored Monado null/remote driver has no eye
   device either → the eye path is inherently live-only on a Quest Pro; VIEW is the testable path.

4. **Grab/push/pull/release compose onto the anchor engine as a fourth carry override, not a new
   anchor mode.** A gaze carry is "place the quad on the gaze ray at distance `d`, billboarded to
   face the viewer, updated every frame" — structurally the `HEAD`-mode grab override
   (`XRAnchor.cpp:231-237`) but driven by the *gaze* ray rather than a grip pose, and submitted in
   `LOCAL_FLOOR` (no device late-latch — there is no controller). Push/pull mutates a scalar
   `m_gazeDist` (the `grabPushPull` clamp, `XR_DISTANCE_MIN/MAX`). Release calls `reanchorFromWorld`
   into the monitor's persistent mode — identical to `endGrab`. The persistent `m_state.mode` is
   untouched throughout (§4).

5. **Selection stability is the one real design problem, and research/14 already solved most of it.**
   A gaze ray is jittery (head micro-tremor; far worse with real saccading eyes). Whole monitors are
   large angular targets (~35° wide, §3.2) so *which* monitor is easy — the danger is at monitor
   **boundaries** and the **instant of keypress** (a saccade at that frame grabs the wrong monitor).
   Fixes, all lifted from research/14: a **1€ filter on the gaze pose** before hit-testing (§14
   Stage B), **dwell** (require gaze to rest on a monitor ~150–250 ms before it becomes the grab
   candidate), and **hysteresis** (sticky selection with a small exit margin). At keypress, grab the
   *dwell-stable* candidate, never the instantaneous ray. Because gaze has **no cursor**, the
   selected-monitor **highlight is the only feedback** (visionOS precedent, research/14 §3) — reuse
   the chrome hover state as a whole-quad "gaze-selected" highlight (§3.3).

6. **Keybinds are ordinary Hyprland binds on the physical keyboard — no XR input path for the keys.**
   The user presses real keys; Hyprland routes them normally. Propose four dispatchers funnelled
   through the existing `xrmonitor` shim (`DispatcherTranslator.cpp:800`): `openxr:gazegrab`,
   `openxr:gazerelease`, `openxr:gazegrabtoggle`, `openxr:gazedist <±m|abs>`. **Hold-to-carry = one
   physical key bound press+release**: `bind = …, openxr:gazegrab` + `bindr = …, openxr:gazerelease`
   (Hyprland's `r` flag fires the dispatcher on key *release*, `KeybindManager.cpp:724`). Push/pull
   while walking = `binde` (repeat) on `openxr:gazedist +0.1` so holding the key keeps pushing at the
   keyboard repeat rate (`KeybindManager.cpp:823`). Toggle mode covers "grab, let go of the key,
   reposition, press again to release" (§5).

7. **Thread-safety: the dispatcher runs on the MAIN thread, the gaze solve on the FRAME thread.** The
   handoff mirrors the shipped patterns exactly (§6): the frame thread publishes the current
   gaze-hovered monitor id into a manager `std::atomic<int64_t> m_gazeHoveredId` (the `m_monitorId`
   pattern, `XRMonitorLayer.hpp:102`); the dispatcher reads it, resolves the layer under `m_layersMu`,
   and sets the gaze-grab flag + distance on that `CXRAnchor` **under `m_layersMu`** — the exact
   discipline the pose verbs already use (`cmdMove`/`cmdDistance` take `std::scoped_lock lock(m_layersMu)`,
   `OpenXRManager.cpp:2042`/`:2110`). The gaze-carry scalars live *inside* `CXRAnchor` as plain POD
   (no atomics needed — always touched under the lock), and `solve()` (frame thread, also under
   `m_layersMu`, `:897`) reads them. Zero hyprutils refcount ops on the frame thread → satisfies the
   `XRMonitorLayer.hpp` rule by construction.

**Recommendation:** ship **VIEW-space gaze grab** (WP-Z1..Z5) as the whole v1 — it fully answers the
request on the user's Quest 3, is entirely headless-testable, and reuses the grab/anchor/verb
machinery. Add the **eye-gaze source** (WP-Z6) as an opt-in that auto-falls-back to VIEW, but treat
it as unvalidatable until a Quest Pro is on hand. Fold in research/14's **gaze 1€ filter + dwell +
hysteresis** (WP-Z2) from the start — without them the boundary/keypress jitter will be the felt
pain, exactly as it was for the controller ray.

---

## 1. What exists to build on (verified in code)

- **The head pose is located every frame already.** `OpenXRManager.cpp:843-852` locates
  `m_viewSpace` in the reference space at `predictedDisplayTime` into `viewPose` (validity gated on
  the position+orientation bits), floor-shifted in the LOCAL fallback. This is the v1 gaze origin +
  direction with **no new XR calls**. It is fed into every anchor `solve()` as `in.view`
  (`:904`) and cached into `m_lastVerbCtx` for the main-thread verbs (`:898`).

- **`rayQuadIntersect(Q, o, d, w, h, slack)`** (`XRMath.hpp:350`) — pure ray/quad hit returning
  `{hit, t, u, v}`, `slack` expanding the half-extents (the ±5° grab cone uses it). The controller
  ray calls it per hand across `SXRPointerTarget[]` (`XRInput.cpp` ray cast). A gaze ray is the same
  call with `o = gazeOrigin`, `d = gazeDir` — nothing new.

- **The frame loop already builds `SXRPointerTarget[]`** — one per visible quad, full-quad world
  pose + chrome geometry (`OpenXRManager.cpp:1037-1045`, struct `XRInput.hpp:91`). A gaze-hover pass
  reuses this exact vector (it is built before `processPointer`), so gaze selection costs one more
  ray loop over an array that already exists.

- **The grab override in `solve()`** (`XRAnchor.cpp:217-260`) is the template for the gaze carry.
  For `HEAD`/`BODY` modes it re-billboards the carried quad to face the viewer every frame
  (`lookAtNoRoll(worldPos, in.view.pos, …)`, `:233`) while holding a device-space offset — a gaze
  carry is the *same* re-facing but with the position coming from `view.pos + gazeDir·d` and
  submitted in `LOCAL_FLOOR` (there is no controller to late-latch against).

- **`grabPushPull(deltaMeters)`** (`XRAnchor.cpp:400`) clamps a carried distance to
  `[XR_DISTANCE_MIN, XR_DISTANCE_MAX]` = `[0.3, 5.0] m` (`XRAnchor.hpp:58`). It currently mutates the
  grab offset length; the gaze equivalent mutates `m_gazeDist` with the identical clamp. The user's
  "pull nearer / push farther along the gaze ray" is *exactly* this.

- **`applyDistance(dMeters, ctx)`** (`XRAnchor.cpp:603`) already moves a quad along the **view→quad
  ray** by `±m` for every mode, and is surfaced as the `xrmonitor distance` verb / `hyprctl openxr
  distance` (`cmdDistance`, `OpenXRManager.cpp:2097`). For a monitor that is *not* gaze-grabbed, the
  push/pull dispatcher can fall straight through to this existing verb; while gaze-grabbed it mutates
  `m_gazeDist`. Either way the distance math is written.

- **`reanchorFromWorld(W, ctx, tune)`** (`XRAnchor.cpp:502`) re-expresses any `LOCAL_FLOOR` world
  pose into the persistent mode and reseeds the leash spring. `endGrab` (`:499`) and `setMode`
  (`:688`) both call it. Gaze **release** calls it with the last carried world pose — the quad stays
  put where the user let it go, then the persistent mode (`local`/`head`/`body`) takes over.

- **Selection already has a "last ray-hovered" resolution rule.** `resolveSelected()`
  (`OpenXRManager.cpp:1619`) prefers `m_selectedMonitor`, then `m_lastHoveredMonitor` (set in
  `setHoveredMonitor`, `:533`), then focus. The gaze target is a parallel "last gaze-hovered"
  signal; the dispatcher can pick it directly rather than routing through `resolveSelected` (gaze
  grab should target *what I'm looking at now*, not the sticky selection).

- **The verb funnel + two-transport pattern.** Every `xrmonitor` verb is one `COpenXRManager::cmd*`
  method reached by both the dispatcher (`DispatcherTranslator.cpp:821-840`) and `hyprctl openxr`
  (doc 05). New gaze verbs slot into the same `if (verb == …)` ladder and `m_dispMap["xrmonitor"]`
  registration (`:917`).

- **The frame→main atomic write-back contract.** `CXRMonitorLayer` publishes per-frame state to the
  main thread via plain atomics — `m_hoverRegion`/`m_grabbedNow` (`XRMonitorLayer.hpp:149-150`),
  `m_monitorId` (`:102`) — never a hyprutils refcount op (the load-bearing THREAD-SAFETY RULE,
  `:32-45`). The gaze-hover publish is one more atomic in exactly this mould.

- **1€ pose filter + dwell/hysteresis primitives already exist.** `oneEuroStepPose`
  (`XRMath.hpp:314`, gtest-covered in `tests/xr/one_euro.cpp`) and the `SXRSchmitt` hysteresis
  trigger (`XRInput.hpp:102`) are the exact tools research/14 §4 specified for aim stabilization —
  reused here for the gaze ray (§3.1).

Nothing here needs a new thread, a new XR layer, or a new OpenXR extension for v1.

---

## 2. Gaze sources in OpenXR

### 2.1 v1: the VIEW reference space forward vector (center FOV)

The universal fallback — works on **every** headset including the user's Quest 3, requires **no**
extension, and is already computed. `viewPose` (`OpenXRManager.cpp:848`) is the pose of `XR_VIEW`
(the midpoint reference between the two eye views) in the reference space. The center-FOV gaze is:

```
gazeOrigin = viewPose.pos
gazeDir    = qRotate(viewPose.rot, {0, 0, -1})   // OpenXR forward = -Z
```

Semantics worth stating: this is **head** direction, not eye direction — the user aims by turning
their head so the target sits in the centre of their view, which is exactly the "center FOV gaze"
the request names. It is valid whenever `viewValid` (the frame loop's existing gate), i.e. whenever
head tracking is up; no FOCUSED/action-sync dependency (unlike the eye path). This makes the whole
v1 feature **headless-testable**: the Monado remote driver scripts the head pose (research/13's
geofence test drives exactly this), so gaze selection, carry, push/pull and release are all
scriptable without a headset.

### 2.2 Eye tracking: `XR_EXT_eye_gaze_interaction` — extension-support probe

Following the strings-probe method of research/01/04/13, the **installed WiVRn runtime binary**
advertises the full extension. From `/usr/lib/wivrn/libopenxr_wivrn.so`:

```
XR_EXT_eye_gaze_interaction
/interaction_profiles/ext/eye_gaze_interaction
/user/eyes_ext                 /user/eyes_ext/input/gaze_ext/pose
/pose/gaze_ext
XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT
XR_TYPE_EYE_GAZE_SAMPLE_TIME_EXT
XR_TYPE_SYSTEM_EYE_TRACKING_PROPERTIES_FB   XR_TYPE_EYE_GAZES_FB / _INFO_FB
monado_ext_eye_gaze_interaction
```

and the full runtime extension enumeration lists `XR_EXT_eye_gaze_interaction` alongside
`XR_EXT_hand_interaction`, `XR_EXTX_overlay`, `XR_KHR_composition_layer_equirect2`, etc. So WiVRn
(Monado-derived) is **built with** eye-gaze support and would expose it *if the connected device
had eye-tracking hardware*.

The **vendored Monado** (`subprojects/monado`, pinned `c2ddab59`; not checked out in this worktree)
is the same code family — the WiVRn binary's `monado_ext_eye_gaze_interaction` / `XRT_DEVICE_
EYE_GAZE_INTERACTION` strings are Monado's driver hooks. But the **null compositor + remote driver**
we use for headless tests advertises no eye device (same reason research/13 noted "remote driver
can't advertise hand profiles") → `supportsEyeGazeInteraction == XR_FALSE` under the test harness.

### 2.3 What the eye path actually requires — device support, not runtime support

Eye tracking is a **device** capability. The application queries it with `xrGetSystemProperties`
chaining `XrSystemEyeGazeInteractionPropertiesEXT` and reads `supportsEyeGazeInteraction`. On the
user's hardware:

| Device | Eye-tracking HW | `supportsEyeGazeInteraction` | Notes |
|---|---|---|---|
| **Quest 3 / 3S** (the user's) | **No** | `XR_FALSE` | Meta's CTO confirmed pancake-lens eye tracking was cut for cost/compute; no sensors present. **v1 uses VIEW.** |
| **Quest Pro** | Yes | `XR_TRUE` | The only Meta device that reports it; needs a per-user eye-calibration + a runtime consent prompt. |
| Vision Pro / Varjo / PSVR2-class | Yes | `XR_TRUE` | Vision-Pro-style gaze-first UIs are the archetype the eye path targets. |
| Monado null / remote driver (our tests) | No | `XR_FALSE` | Eye path inherently live-only; VIEW is the headless path. |

Pose semantics for the eye path (spec + Unity/Khronos references below): the gaze is a **pose
action** bound to `/user/eyes_ext/input/gaze_ext/pose` — **only a pose, no buttons/analog**. You
create one `XrAction` (`XR_ACTION_TYPE_POSE_INPUT`), suggest the eye-gaze interaction profile, make
an action space with `xrCreateActionSpace`, and each frame `xrLocateSpace` it against the reference
space (after `xrSyncActions`), reading the gaze ray from the located pose's orientation
(`-Z` forward) and origin. The pose is **only valid while the session is FOCUSED** (like every
action pose) and while the tracking bits are set — so the eye ray must gracefully treat "not
located this frame" as "fall back to VIEW," not "no target." The extension deliberately exposes
**only the interaction ray** (privacy-preserving) — raw per-eye vergence is a different extension
(`XR_FB_eye_tracking_social`), which we do **not** need. Calibration/accuracy caveats: runtime eye
calibration is per-user and can drift; practical accuracy is ~1–2° — good enough for *which monitor*
(monitors subtend tens of degrees) but another reason the **grab candidate must be dwell-stabilised**
rather than instantaneous (§3).

### 2.4 Design conclusion for the gaze source

Add an optional `SXRSolveInput::gaze` (a `std::optional<SXRPose>`, floor-shifted like the grips) and
a config `openxr:gaze_source = view | eye` (default `view`, auto-fallback to VIEW when
`eye` is requested but `supportsEyeGazeInteraction == XR_FALSE` or the pose is unlocated this frame).
The gaze-hover pass and the carry read `in.gaze.value_or(view-forward)` — one code path, the eye
field is just a better ray when present. **v1 ships `view` only**; the `gaze` field + the one eye
action are WP-Z6, unvalidatable here.

Sources:
[Khronos forum — XR_EXT_eye_gaze_interaction questions](https://community.khronos.org/t/xr-ext-eye-gaze-interaction-questions/111300),
[Unity OpenXR — Eye Gaze Interaction (pose-only, /user/eyes_ext/input/gaze_ext/pose)](https://docs.unity3d.com/Packages/com.unity.xr.openxr@1.15/manual/features/eyegazeinteraction.html),
[Microsoft OpenXR-MixedReality — EyeGazeInteraction sample](https://github.com/microsoft/OpenXR-MixedReality/blob/main/samples/EyeGazeInteractionUwp/Scene_EyeGazeInteraction.cpp),
[Meta — Eye Tracking in Movement SDK for OpenXR (Quest Pro only)](https://developers.meta.com/horizon/documentation/native/android/move-eye-tracking/),
[Meta Quest Pro eye tracking help](https://www.meta.com/help/quest/8107387169303764/),
[Why there is no eye tracking in Quest 3 — Meta CTO](https://mixed-news.com/en/quest-3-eye-tracking-hurdles/),
[WiVRn issue #408 — controller/hand + eye-gaze on Quest S3](https://github.com/WiVRn/WiVRn/issues/408).

---

## 3. Gaze → monitor selection

### 3.1 Reuse the ray/quad machinery for a HEAD ray

The gaze-hover pass is `processPointer`'s ray cast (`XRInput.cpp` ray loop) generalised to one more
"virtual pointer" whose pose is the gaze ray instead of a hand aim pose:

```
for each SXRPointerTarget t (already built, OpenXRManager.cpp:1037):
    hit = rayQuadIntersect(t.worldPose, gazeOrigin, gazeDir, t.w, t.h, gazeSlack)
    keep the nearest-t hit  ->  gazeHitMonitorId
publish gazeHitMonitorId (atomic, §6)
```

This does **not** need to live in `CXRInput` (which is controller/hand-centric and emits pointer
events). Cleanest placement: a small dedicated pass in the frame loop right after the pointer
targets are built and before/after `processPointer` (`OpenXRManager.cpp:1058`), so it reuses
`pointerTargets` and publishes to a manager-level atomic. Gaze must **not** drive the desktop
pointer (motion/click) — it only *selects*; clicking stays with the controller/hand ray. (If the
user later wants gaze-to-move-cursor, that is a separate opt-in, out of scope here.)

**Stabilisation (the hard part — research/14 already did this analysis).** A raw head/eye ray
jitters; feed it through the same three tools research/14 §4 specified for the aim ray:

- **1€ filter the gaze pose before hit-testing** (`oneEuroStepPose`, `XRMath.hpp:314`; a per-frame
  `SXROneEuroPose m_gazeFilter` on the frame thread). research/14 Stage B defaults (`min_cutoff` ~1.5,
  `beta` ~0.01) are the starting point; eyes may want more smoothing than head.
- **Dwell** — require the same monitor to be the nearest hit for `openxr:gaze_dwell_ms` (default
  ~150–250 ms) before it becomes the *grab candidate*. A pure `dt` accumulator (research/13's dwell
  shape) resets whenever the hit monitor changes. This is what stops a saccade / head-flick past a
  monitor from making it grabbable, and what makes "grab the thing I'm *looking at*" robust at the
  keypress instant: the dispatcher grabs the **dwell-stable candidate**, not the instantaneous ray.
- **Hysteresis** — once a monitor is the selected candidate, require the gaze to leave it by an
  angular margin (or for a competing monitor to win the nearest-hit by a margin) before switching, so
  adjacent-monitor boundaries don't flicker the selection. Mirror the `SXRSchmitt` pattern
  (`XRInput.hpp:102`) as a per-selection latch.

**Angular reality check (research/14 §2 math).** Whole monitors are *huge* targets: at the default
1.6 m content width and 1.5 m distance a monitor subtends ~`2·atan(0.8/1.5)` ≈ **56°** wide (and
~35° at 2.5 m) — so *which monitor* is never Fitts-hard. The jitter budget is spent entirely at
**boundaries between adjacent monitors** and at the **keypress frame**; that is precisely what dwell
+ hysteresis + the 1€ filter target. No cone-widening is needed for selection (unlike the tiny
chrome handles of research/14) — if anything a gaze grab of a *specific chrome region* is
undesirable; gaze selects the whole monitor, and the keybind grabs the whole monitor (MOVE-style),
leaving corner-resize to the hand ray.

### 3.2 What "the monitor I'm looking at" resolves to at keypress

Selection order for `openxr:gazegrab`, resolved on the main thread when the key is pressed:

1. The current **dwell-stable gaze candidate** (`m_gazeHoveredId`, §6) — *what I'm looking at now*.
2. If gaze isn't resting on any monitor (staring into passthrough), **fail cleanly** with a message
   (`"gazegrab: not looking at a monitor"`) — do **not** fall back to the sticky `m_selectedMonitor`
   (that would grab a monitor behind you). This is a deliberate divergence from `resolveSelected`'s
   fallback chain (`OpenXRManager.cpp:1619`): gaze grab is inherently "the looked-at one."

### 3.3 Visual feedback — the highlight IS the feedback (gaze has no cursor)

research/14's central finding applies doubly here: **you cannot aim a ray you cannot see**, and for
gaze there is *no* controller to hang a beam/cursor off. visionOS's answer — and ours — is a strong
**target highlight on the looked-at element** (research/14 §3, Apple hover-effect). Reuse the shipped
chrome visual-state contract:

- Publish a per-layer **gaze-selected** flag alongside `m_hoverRegion`/`m_grabbedNow`
  (`XRMonitorLayer.hpp:149-150`), written frame-thread after the gaze pass, read by the chrome draw.
- Render it as a **whole-quad affordance** — an outline / edge glow / full-bar tint in a distinct
  `openxr:gaze_col_*` (mirroring `chrome_col_hover`/`_grab`, `ConfigValues.cpp:790-792`), so the user
  sees *which* monitor will be grabbed *before* pressing the key. This is the single most important
  usability piece — without it the user is guessing (research/14 §1). It can ride the existing chrome
  fade envelope (`chromeFadeAdvance`, `m_chromeAlpha`, `XRMonitorLayer.hpp:156-161`) so it fades in
  on dwell and out on look-away, zero new animation machinery.
- Optional escalation while carried: reuse `m_grabbedNow`'s grab color so a gaze-carried monitor
  reads the same "I'm holding this" state as a hand grab.

Distinct-from-chrome note: the gaze highlight is a *whole-monitor* state, whereas
`m_hoverRegion` is a *sub-quad region* (bar/corner). They coexist — a monitor can be gaze-selected
(whole-quad glow) while a hand ray hovers its corner (region highlight). Keep them as separate
published fields.

---

## 4. Grab / push / pull / release mechanics (composition with the grab engine)

### 4.1 The gaze carry is a fourth `solve()` override, not a new anchor mode

`CXRAnchor` gains a small runtime state (POD, no persistence — the persistent `m_state.mode` is the
desk/head/body identity the monitor returns to on release):

```cpp
// runtime, touched only under COpenXRManager::m_layersMu (verbs write, solve reads)
bool    m_gazeGrabbed = false;
float   m_gazeDist    = 1.5F;   // metres along the gaze ray (clamped XR_DISTANCE_MIN..MAX)
bool    m_gazeFollow  = true;   // true = quad tracks the gaze ray; false = frozen after grab (§4.4)
```

`solve()` gains a pre-step **before** the existing grab override (`XRAnchor.cpp:217`), so a gaze grab
and a hand grab are mutually exclusive and the hand grab wins if both somehow fire (the hand is the
more explicit gesture):

```
if (m_gazeGrabbed && in has a gaze/view ray):
    gazeDir = normalize(gaze forward)                 // in.gaze.value_or(view-forward)
    P       = in.view.pos + gazeDir * m_gazeDist      // (or a frozen ray if !m_gazeFollow)
    R       = lookAtNoRoll(P, in.view.pos, m_lastWorld.rot)   // billboard to the viewer
    res.space = XR_SPACE_LOCAL_FLOOR
    res.pose  = res.worldPose = {P, R}
    m_lastWorld = {P, R};  m_hasLastWorld = true
    return res
```

This is line-for-line the `HEAD`-mode grab re-facing (`XRAnchor.cpp:231-237`) with the position
sourced from the gaze ray instead of a grip offset, and **submitted in `LOCAL_FLOOR`** — there is no
controller action space to late-latch, so (like the WP-G6 filtered branch, `XRAnchor.cpp:250-253`)
we submit the world pose directly. Everything is pure math over `in.view`/`in.gaze` + `dt`, so it is
gtest-able like the rest of the engine and touches no refcounts.

### 4.2 begin / push-pull / release verbs on `CXRAnchor`

Mirror the grab API (`XRAnchor.hpp:238-256`):

- **`beginGazeGrab()`** — snapshot `m_gazeDist = distance(view.pos, m_lastWorld.pos)` (grab it at
  the distance it currently sits, so it doesn't jump on grab), set `m_gazeGrabbed = true`. Needs
  `hasLastWorld()` (the quad has been solved at least once) — the manager gates on it. *Note:* the
  distance snapshot needs a view pose; the manager passes `currentVerbContext()` (last frame's view,
  `OpenXRManager.cpp:1606`), or defers the snapshot to the first carried frame in `solve()`.
- **`gazePushPull(deltaMeters)`** — `m_gazeDist = clamp(m_gazeDist + delta, XR_DISTANCE_MIN,
  XR_DISTANCE_MAX)` — the `grabPushPull` clamp verbatim (`XRAnchor.cpp:400-405`).
- **`gazeSetDist(absMeters)`** — same clamp, absolute (for `openxr:gazedist 2.0`).
- **`endGazeGrab(ctx, tune)`** — `m_gazeGrabbed = false; reanchorFromWorld(m_lastWorld, ctx, tune)`
  — the quad stays exactly where the user let it go, re-expressed into its persistent mode, spring
  reseeded (identical to `endGrab`, `XRAnchor.cpp:499`). If the persistent mode is `head`/`body`, it
  resumes leashing from there; if `local`, it stays world-fixed at the released pose.

There is **no release lurch problem** here (unlike the hand-release latch, research/04/WP-G4): a
keyboard release does not perturb the head/gaze pose, so the release-frame pose is clean. The
`SXRGrabRing` latch is unnecessary for gaze release — a welcome simplification.

### 4.3 Push/pull: reuse the shipped `distance` verb when NOT gaze-grabbed

`openxr:gazedist <±m>` should do the right thing in both states:

- **Gaze-grabbed** → `gazePushPull` on the live carry (frame thread reads `m_gazeDist`).
- **Not gaze-grabbed** (e.g. the user selected via gaze, toggled grab off, and now nudges) → fall
  through to the existing `applyDistance` (`XRAnchor.cpp:603`) / `cmdDistance` logic on the
  gaze-*selected* monitor, moving it along the view→quad ray. This means the "push/pull" keys are
  useful even outside a carry, and the distance math already exists and is gtested.

### 4.4 Follow-while-held vs freeze-on-grab (a taste decision — table)

The request says the monitor "follows your gaze while held." Two readable behaviours:

| Behaviour | What the monitor does while held | Feel | Recommendation |
|---|---|---|---|
| **Follow** (`m_gazeFollow=true`) | Rides the gaze ray — look elsewhere and it swings to stay centred; keys change its distance | Like carrying a lantern with your eyes; can feel "clingy"/nauseating if you look around a lot (it's a soft head-lock) | **Default.** Matches "follows your gaze while held"; comfortable because it is *transient* (only while the key is down/toggled). |
| **Freeze** (`m_gazeFollow=false`) | Detaches from the gaze at grab; stays put in the world; keys pull/push along the *grab-time* ray | Like grabbing an object then moving it only with the keys | Offer as `openxr:gaze_follow=false`. Better for "I picked which monitor by looking, now let me reposition it deliberately without it chasing my eyes." |

Both are one flag in the `solve()` branch (§4.1): `follow` recomputes `gazeDir` each frame; `freeze`
snapshots `gazeDir` at `beginGazeGrab`. Ship `follow` as default, expose the flag.

### 4.5 Composition with adaptive anchoring (research/13)

research/13's dock/roam decorator runs its geofence in `solve()`'s adaptive pre-step and **already
yields to the grab override** ("a grab always wins over adaptive… the grab override returns first",
research/13 §4.1/§5.1). A gaze carry must slot into the **same precedence**: gate `adaptiveStep` on
`!m_gazeGrabbed` too, so a gaze-carried monitor is never geofenced/transitioned mid-carry. On gaze
**release**, `reanchorFromWorld` redefines the monitor's world pose; research/13's §5.1 grab-release
policy applies unchanged (release while `DOCKED` redefines the dock pose; release while `ROAMING`
updates the roam offset). No new interaction beyond "gaze grab is another grab as far as adaptive is
concerned" — worth a one-line note in research/13's §5.1 when it lands.

### 4.6 Interaction with an in-flight hand grab

If a hand is already grabbing a monitor and the user presses `gazegrab`, the gaze grab should target
a *different* monitor (the gazed-at one) — two monitors carried at once (one by hand, one by gaze) is
fine and independent (per-anchor state). If the gaze candidate *is* the hand-grabbed monitor, ignore
the gaze grab (the hand owns it): the `beginGazeGrab` path checks `!anchor.grabbed()`
(`XRAnchor.hpp:299`) and returns a clean "already grabbed by hand" error.

---

## 5. Keybind plumbing

### 5.1 Keys are ordinary physical-keyboard binds — no XR input path

The user binds these on their real keyboard; Hyprland receives the key events through the normal
libinput → `KeybindManager` path. There is **no** XR-side key capture (that would be research/05's
`hypxrkeys` overlay, unrelated). The dispatchers just need to reach `COpenXRManager`, which they do
through the existing `xrmonitor` funnel.

### 5.2 Dispatcher names + funnel (extends `DispatcherTranslator.cpp:800`)

Add to the `xrmonitorDispatch` verb ladder and/or as top-level `openxr:*` dispatchers. Two viable
surfaces (pick one; recommendation: **top-level `openxr:*`** for ergonomics, since these are
gaze-global, not per-selected-monitor like `xrmonitor move`):

| Dispatcher | Arg | Action | `cmd*` method |
|---|---|---|---|
| `openxr:gazegrab` | — | Grab the dwell-stable gazed-at monitor; enter carry (latched). | `cmdGazeGrab()` |
| `openxr:gazerelease` | — | Release the gaze-carried monitor; reanchor into its mode. | `cmdGazeRelease()` |
| `openxr:gazegrabtoggle` | — | Toggle: grab if none held / release if held. | `cmdGazeToggle()` |
| `openxr:gazedist` | `<±m>` or `abs:<m>` | Push/pull the carried (or gaze-selected) monitor. | `cmdGazeDist(arg)` |

Each `cmd*` runs on the main thread, takes `std::scoped_lock lock(m_layersMu)`, resolves the gaze
target from `m_gazeHoveredId`, and mutates the anchor's gaze state — the exact shape of `cmdMove`
(`OpenXRManager.cpp:2027-2046`). Registered exactly like `m_dispMap["xrmonitor"]`
(`DispatcherTranslator.cpp:917`), returning a clean error without `HAVE_OPENXR`.

### 5.3 Hold-to-carry: press+release on one physical key (Hyprland bind flags)

Hyprland's bind flags (`ConfigManager.cpp:1588-1614`) give three relevant behaviours:

- **`r` (release)** — the dispatcher fires on key **release** instead of press
  (`KeybindManager.cpp:724`, `:732-749`). Binding the *same* key with both a plain `bind` and a
  `bindr` gives a **press→release pair** — the canonical push-to-talk / hold pattern.
- **`e` (repeat)** — the dispatcher re-fires on keyboard auto-repeat while held
  (`KeybindManager.cpp:823-828`), at the keyboard's repeat delay/rate. Mutually exclusive with `r`
  (`ConfigManager.cpp:1616`).
- The dispatcher can also read press-vs-release within one bind via
  `Config::Actions::state()->m_passPressed` (set to 1/0 around the call, `KeybindManager.cpp:800`) —
  how the `mouse` dispatcher gets its "1"/"0" prefix. Usable, but the `bind`+`bindr` pair is clearer
  and needs no special-casing.

**Recommended user config:**

```ini
# --- hold-to-carry: hold the key, monitor follows gaze; let go to drop it ---
bind  = SUPER, G, openxr:gazegrab      # press  -> grab the looked-at monitor
bindr = SUPER, G, openxr:gazerelease   # release -> drop it where it is

# --- pull nearer / push farther WHILE holding G (or any time, on the gaze-selected one) ---
binde = SUPER, bracketleft,  openxr:gazedist, -0.1   # repeat while held -> keep pulling nearer
binde = SUPER, bracketright, openxr:gazedist, +0.1   # repeat while held -> keep pushing farther

# --- alternative: toggle mode (press once to grab, again to release) ---
bind  = SUPER, T, openxr:gazegrabtoggle
```

Notes on the two "hold" senses the user raised:

- **"Hold through the motion"** (hold the key while walking, carrying the monitor) → the
  `bind`+`bindr` pair: the carry persists for the entire key-down duration regardless of key repeat
  (repeat is irrelevant — the grab is *state*, set on press, cleared on release). The user can walk
  around the house with the key held and the monitor stays gaze-carried; **key repeat does not
  re-trigger the grab** because `gazegrab` is idempotent (already-grabbed → no-op).
- **Push/pull while walking** → `binde` (repeat) on `gazedist ±0.1`: holding the bracket key keeps
  nudging distance at the repeat rate — smooth, tunable via the keyboard's own repeat settings, no XR
  timer needed. (An alternative continuous-velocity model — accumulate while the key is down, apply
  `v·dt` on the frame thread — is smoother but needs a held-key state + frame-thread integrator;
  `binde` is the simpler v1 and reuses the platform's repeat.)

### 5.4 Decision: hold-pair vs toggle vs single-bind-both-edges

| Model | Config | Pros | Cons | Recommendation |
|---|---|---|---|---|
| **Press/release pair** (`bind`+`bindr`) | 2 lines, 1 key | Dead-simple hold semantics; matches "hold through the motion"; no dispatcher state-reading | Two dispatchers | **Primary.** Ship as the documented default. |
| **Toggle** (`gazegrabtoggle`) | 1 line | Hands-free carry (grab, release key, reposition, press to drop); good for long carries | Must remember it's "armed"; needs the selected highlight to double as an "armed" indicator | **Ship alongside** — covers the "let go of the key and keep carrying" case. |
| **Single bind, both edges** (`m_passPressed`) | 1 line, 1 dispatcher | One keyword | Dispatcher must branch on press/release; less discoverable | Skip — no advantage over the pair. |

Ship **both** the pair and the toggle; they are two `cmd*` methods over the same state.

---

## 6. Thread-safety (main dispatcher ↔ frame-thread solve)

This is the one place to get exactly right; it mirrors the shipped patterns so there is a template
for every piece.

**The two threads and the shared state:**

- **Frame thread** runs the gaze-hover pass and `solve()` (`OpenXRManager.cpp:897`, under
  `m_layersMu`). It *reads* the gaze-grab state and *writes* the gaze-hover selection.
- **Main thread** runs the keybind dispatchers. It *writes* the gaze-grab state and *reads* the
  gaze-hover selection.

**Handoff design (two directions):**

1. **frame → main: which monitor is gazed-at.** Publish the dwell-stable gaze candidate id into a
   manager `std::atomic<int64_t> m_gazeHoveredId{-1}` — the **exact** `m_monitorId` pattern
   (`XRMonitorLayer.hpp:102`, `OpenXRManager.cpp:1033`). Written release-store on the frame thread
   after the gaze pass; read acquire-load in the dispatcher. Never a name string across the boundary
   (strings aren't atomic); the dispatcher maps id→layer under `m_layersMu` via `layerByMonitorID`
   (`OpenXRManager.cpp:495`). The per-layer **gaze-selected highlight** is a `std::atomic<bool>`
   `m_gazeSelected` on `CXRMonitorLayer` alongside `m_hoverRegion`/`m_grabbedNow`
   (`XRMonitorLayer.hpp:149-150`), same contract.

2. **main → frame: grab / distance / release.** The dispatcher takes `std::scoped_lock lock(m_layersMu)`
   (like `cmdMove`, `:2042`) and sets `m_gazeGrabbed`/`m_gazeDist` **inside** the target `CXRAnchor`.
   Because `solve()` also runs under `m_layersMu` (`:897`), these are **plain POD fields, no atomics
   needed** — the lock serialises the write against the read, exactly as `applyMove` mutates
   `anchorPose` today. The gaze *ray* the carry needs (`in.view`/`in.gaze`) is frame-thread-only and
   never crosses to main.

**Refcount rule (`XRMonitorLayer.hpp:32-45`):** the gaze pass and carry are pure math over POD poses
+ `dt`; they perform **zero** hyprutils SP/WP refcount ops (no `.lock()`, no copies of `m_monitor`),
reading the monitor id only via the `m_monitorId` atomic — satisfying the load-bearing rule by
construction, identically to the existing pointer/grab path.

**No new thread, no new queue.** Unlike the pointer *events* (which cross the SPSC ring to become
synthetic input, `XRInput.hpp:41`), gaze grab needs no event stream — the selection is a single
atomic and the commands are direct locked mutations. This is strictly simpler than the pointer path.

---

## 7. Config surface sketch

Declare in `ConfigValues.cpp` next to the `openxr:grab_*` / `chrome_*` family (`:717-792`); per-frame
reads via `readAnchorTuning()`-style access make the tuning hot (matching the leash/grab-filter vars).

```
# ---- gaze source ----
openxr:gaze_source        = view       # view | eye   (eye auto-falls-back to view if unsupported/unlocated)

# ---- selection stability (research/14 Stage B, reused) ----
openxr:gaze_filter        = true       # 1€ filter the gaze pose before hit-testing
openxr:gaze_filter_min_cutoff = 1.5  (0.01..10)
openxr:gaze_filter_beta       = 0.01 (0..1)
openxr:gaze_dwell_ms      = 200  (0..2000)   # rest this long on a monitor before it's grab-eligible
openxr:gaze_hysteresis_deg = 3.0 (0..15)     # sticky-selection exit margin between adjacent monitors

# ---- carry behaviour ----
openxr:gaze_follow        = true       # true = quad rides the gaze while held; false = freeze on grab (§4.4)
openxr:gaze_dist_step     = 0.1  (0.01..1)   # default push/pull step for `gazedist` w/o an arg (m)
# (distance clamp reuses XR_DISTANCE_MIN/MAX = 0.3..5.0 m, XRAnchor.hpp:58)

# ---- selection highlight (reuses the chrome fade envelope) ----
openxr:gaze_highlight     = true
openxr:gaze_col_select    = 0xcc66ccff  # whole-quad glow on the gazed-at monitor (dwell-stable)
openxr:gaze_col_carry     = 0xffff66aa  # while gaze-carried
```

Hot-reload: the numeric tuning is per-frame-read (free). A `gaze_source` change (view↔eye) touches
action attachment and, like `openxr:enabled`/`overlay`, may want the `parseKeyword` special-case or a
session restart (doc 05 / MEMORY hot-toggle caveat) — note it; VIEW-only v1 avoids the issue entirely.

---

## 8. Work packages (one-subagent-sized)

- **WP-Z1 — VIEW-space gaze-hover pass + selection publish · S · no deps · headless.** Add the
  gaze ray cast over `pointerTargets` in the frame loop (`OpenXRManager.cpp:1058` neighbourhood),
  the manager `std::atomic<int64_t> m_gazeHoveredId`, and the per-layer `m_gazeSelected` atomic.
  gtests (`tests/xr/`): scripted view pose + quad set → asserts nearest-hit monitor id; ray into
  passthrough → `-1`. Pure `rayQuadIntersect` reuse.

- **WP-Z2 — gaze stabilisation: 1€ filter + dwell + hysteresis · S/M · dep Z1 · headless.**
  `SXROneEuroPose m_gazeFilter` on the frame thread; dwell accumulator + selection Schmitt; config
  vars. gtests: jittered scripted gaze → asserts reduced selection flicker at a boundary; saccade
  past a monitor within `< dwell_ms` → never selected; hysteresis truth table. Reuses
  `oneEuroStepPose` (`one_euro.cpp`) + the `SXRSchmitt` shape. **This is the felt-quality WP** —
  research/14's core lesson.

- **WP-Z3 — `CXRAnchor` gaze-carry state + `solve()` override + begin/pushpull/end · M · dep none
  (pure engine) · headless.** `m_gazeGrabbed`/`m_gazeDist`/`m_gazeFollow`; the `solve()` pre-step
  (§4.1) placed before the grab override; `beginGazeGrab`/`gazePushPull`/`gazeSetDist`/`endGazeGrab`;
  gate any future `adaptiveStep` on `!m_gazeGrabbed`. gtests (`anchor_math.cpp` style): grab at
  distance d → quad sits at `view+dir·d` facing viewer; push/pull clamps to `[0.3,5.0]`; follow vs
  freeze; release reanchors into local/head/body with the quad not moving. Fully deterministic.

- **WP-Z4 — dispatchers + `cmd*` funnel + main↔frame handoff · M · dep Z1,Z3.** `cmdGazeGrab/
  Release/Toggle/Dist` under `m_layersMu`, resolving `m_gazeHoveredId`; top-level `openxr:gaze*`
  dispatchers (or `xrmonitor` verbs) registered like `DispatcherTranslator.cpp:917`; press+release
  pair + toggle semantics; `hyprctl openxr` transport for parity; status JSON gains
  `gaze: { source, hoveredMonitor, carrying, dist }`. Partly headless (verb→state assertions via the
  harness); the *key-flag feel* (bindr/binde) is a live keyboard check.

- **WP-Z5 — selection + carry highlight (chrome-fade reuse) · S · dep Z1,Z3, research/07.** Draw the
  `m_gazeSelected`/carry state as a whole-quad glow via the existing fade envelope
  (`m_chromeAlpha`/`chromeFadeAdvance`); `openxr:gaze_col_*`. **Live Quest** to judge (visual), with
  a headless seam on the state-selection logic.

- **WP-Z6 — eye-gaze source (`XR_EXT_eye_gaze_interaction`) · M · dep Z1-Z4 · LIVE Quest Pro
  only.** Probe `supportsEyeGazeInteraction`; one `XR_ACTION_TYPE_POSE_INPUT` bound to
  `/user/eyes_ext/input/gaze_ext/pose`; eye action space located each frame; fill
  `SXRSolveInput::gaze` when valid+FOCUSED, else leave empty (→ VIEW fallback in Z1/Z3);
  `openxr:gaze_source=eye`. **Unvalidatable on this hardware** (Quest 3 has no eye HW; remote driver
  advertises none) — mark blocked on a Quest-Pro-class device; the VIEW path (Z1-Z5) is the entire
  shippable v1.

**Headless vs live.** Z1-Z4 are fully headless (pure gtests + the Monado remote-driver scripted head
pose, exactly research/13's approach — the whole gaze *logic* is pure functions over poses). Z5's
*rendering* and the *bind-flag feel* need a live Quest 3 (`preview-xr.sh --wivrn`); the tuning vars
are hot so iteration is fast. Z6 needs a Quest Pro / eye-tracked HMD to exercise at all.

---

## 9. Open questions for the user (taste + scope)

1. **Follow vs freeze default while held?** Recommendation **follow** (the request's wording), with
   `openxr:gaze_follow=false` for "select-by-look then reposition deliberately." Which feels right
   for wandering the house — a monitor that swings to stay centred, or one that stays put and only
   changes distance? (§4.4)
2. **Dwell time.** ~200 ms before a monitor becomes grab-eligible stops saccade/head-flick
   mis-grabs but adds a beat of latency to "look → grab." Snappier (100 ms) or steadier (300 ms)?
   Hot-tunable. (§3.1)
3. **Keybind ergonomics — pair, toggle, or both?** Recommendation ship **both**: `bind`+`bindr`
   hold-pair as the documented default, plus `gazegrabtoggle` for hands-free long carries. Any
   preference, or a specific key layout you want documented? (§5.3/§5.4)
4. **Push/pull input model — `binde` repeat (v1) or continuous velocity while held?** `binde` reuses
   the keyboard repeat and is simple; a frame-thread velocity integrator is smoother but more code.
   Start with `binde`? (§5.3)
5. **Should gaze ever drive the desktop *cursor*/clicks, or only select+grab?** Recommendation
   **select+grab only** — clicking stays on the controller/hand ray (gaze-to-click needs dwell-click
   or a click key and is a separate feature). Agree? (§3.1)
6. **Highlight style.** Whole-quad outline vs edge-glow vs full-bar tint for the gazed-at monitor;
   distinct color from the hand-ray chrome hover so the two don't read the same. Preference? (§3.3)
7. **Eye tracking priority.** Given the Quest 3 has no eye HW, WP-Z6 is unvalidatable here — is it
   worth building speculatively (design's ready), or defer until a Quest-Pro-class device is around?
   (§2.3)
8. **Fail-closed selection.** `gazegrab` while looking at passthrough (no monitor) should **error
   cleanly**, not fall back to the last-selected monitor behind you — confirm that's the wanted
   behaviour. (§3.2)

---

## 10. Files referenced

- `src/openxr/XRAnchor.{hpp,cpp}` — grab override / carry template (`XRAnchor.cpp:217-260`),
  `beginGrab` (`:389`), `grabPushPull` + `XR_DISTANCE_MIN/MAX` clamp (`:400`, `XRAnchor.hpp:58`),
  `applyDistance` (`:603`), `reanchorFromWorld` (`:502`), `endGrab` (`:499`), `setMode` (`:668`),
  `centerPlacement`/`lookAtNoRoll` billboard (`:174`), anchor modes enum (`XRAnchor.hpp:19`),
  grab state fields (`XRAnchor.hpp:328-333`).
- `src/openxr/XRMath.hpp` — `rayQuadIntersect` (`:350`), `oneEuroStepPose`/`SXROneEuroPose`
  (`:314`/`:299`), `SXRPose`/`qRotate` (`:88`/`:114`), region classifier (`:391`).
- `src/openxr/XRInput.{hpp,cpp}` — `SXRPointerTarget` (`XRInput.hpp:91`), `processPointer`
  (`XRInput.hpp:159`), `SXRSchmitt` hysteresis (`XRInput.hpp:102`), hover fields (`:269-278`).
- `src/openxr/OpenXRManager.cpp` — view pose locate (`:843-852`), solve loop under `m_layersMu` +
  verb ctx (`:897-919`, `:898`), pointer targets build (`:1037-1045`), `processPointer` + hover
  publish (`:1058-1078`), `setHoveredMonitor`/`m_lastHoveredMonitor` (`:523-533`), `resolveSelected`
  (`:1619`), verbs `cmdMove`/`cmdDistance`/`cmdCenter` (`:2027`/`:2097`/`:2116`), `currentVerbContext`
  (`:1606`).
- `src/openxr/XRMonitorLayer.hpp` — THREAD-SAFETY RULE (`:32-45`), `m_monitorId` atomic (`:102`),
  frame→main visual-state atomics + fade envelope (`:140-161`).
- `src/config/legacy/DispatcherTranslator.cpp` — `xrmonitor` funnel (`:800-844`), dispatcher
  registration (`:917`).
- `src/managers/KeybindManager.cpp` — release-flag dispatch (`:724`, `:732-749`), repeat-flag
  (`:823-828`), `m_passPressed` press/release signalling (`:800`).
- `src/config/legacy/ConfigManager.cpp` — bind-flag parse `r`/`e`/… (`:1588-1614`), `e` vs `r`/`o`
  mutual exclusion (`:1616`).
- `src/config/values/ConfigValues.cpp` — `openxr:grab_*` / `chrome_*` config family + defaults
  (`:717-792`) — where the `openxr:gaze_*` vars declare.
- Related research: `docs/openxr/research/14-ray-aim-assist.md` (gaze/aim jitter, 1€ filter, dwell,
  target-highlight — reused wholesale), `13-adaptive-anchoring.md` (grab suspends the geofence — gaze
  grab composes identically), `07-premium-chrome.md` (chrome visual vocabulary for the highlight),
  `04-grabbable-borders.md` (grab machine + release latch — the release-lurch problem does NOT apply
  to keyboard release).
- Probe evidence: `/usr/lib/wivrn/libopenxr_wivrn.so` (installed WiVRn runtime) advertises
  `XR_EXT_eye_gaze_interaction` + `/user/eyes_ext/input/gaze_ext/pose` +
  `XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT`; vendored `subprojects/monado` @ `c2ddab59`
  (same family; null/remote driver advertises no eye device).
```
