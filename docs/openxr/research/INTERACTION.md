# INTERACTION — aiming, direct manipulation, and gaze-driven grabs

**Live planning surface.** Consolidates the still-pending interaction research into
one backlog. Deep detail lives in the archived source reports; this doc is the place
to triage and sequence.

Sources coalesced here:
- [`archive/14-ray-aim-assist.md`](archive/14-ray-aim-assist.md) — ray cursor / target assist for controller+ray aiming.
- [`archive/15-direct-manipulation.md`](archive/15-direct-manipulation.md) — near-field pinch + poke when a monitor is physically reachable.
- [`archive/16-gaze-grab.md`](archive/16-gaze-grab.md) — gaze-vector monitor selection + keybind grab/push/pull/release.

Already shipped and archived, but the shared machine every item below builds on:
[`archive/04-grabbable-borders.md`](archive/04-grabbable-borders.md) — grabbable chrome
(bar/corners), the `SXRGrabRing` release latch, per-hand pinch/grasp grabs, and the 1€
carry filter (WP-G1…G6, all landed; live-tuned defaults `ba584867`). No remainder — 04
is the foundation, not a pending item.

---

## Capability vision

The user aims monitors in three registers depending on distance and intent:

1. **Far / ray** — point a controller (or hand ray) at a floating monitor and grab its
   chrome. Today the ray is *invisible* and the targets are angularly tiny, so this is the
   dominant source of "hard to grab" pain.
2. **Near / direct** — walk up to a reachable monitor and pinch its bar with fingers *at*
   the bar (no ray), or poke content directly. Feels natural, needs no new OpenXR data.
3. **Gaze / keyboard** — look at a monitor and press a bound key to grab it; keys pull it
   nearer / push it farther along the gaze ray; a key releases or docks it. Works on a
   Quest 3 with no eye tracking via the head-forward (center-FOV) vector.

All three reuse the shipped grab engine (`CXRAnchor` begin/carry/release, `SXRGrabRing`
latch, 1€ filter, region classifier) — they are *new acquisition front-ends*, not new
anchor modes. The unifying lesson from all three reports: **stabilization (1€ filter +
dwell + hysteresis) is the felt-quality work**, not the raw hit-testing.

---

## Consolidated design decisions

### Aiming (from 14)
- **The single highest-ROI fix is a visible endpoint cursor.** There is no ray/cursor/
  reticle drawn anywhere today (`XRGraphics.cpp` `drawChrome` is the only per-frame GL
  besides the blit). "You cannot aim a ray you cannot see." Draw an endpoint cursor at the
  ray↔quad hit in the existing `drawChrome` pass before anything fancier.
- **Layer forgiveness in stages, cheapest first:** endpoint cursor + hover hysteresis
  (sticky region Schmitt) → aim-pose 1€ stabilization → snap/magnetism cone (chrome-only,
  never magnetize BODY so content precision is preserved) → optional beam last.
- **Align the hover highlight with the grab cone.** Today hover only lights up once the ray
  is *exactly* inside the rect, but the grab path already forgives ±5°; surface that slack
  in the highlight so the user sees the target is acquired before committing.
- The live stopgap size bump (`chrome_margin` 0.04→0.10, `chrome_bar_height` 0.05→0.08,
  `chrome_corner_size` 0.06→0.09) shipped; fold it into documented defaults and note the
  corner↔margin clamp coupling.

### Direct / near-field (from 15)
- **Direct pinch-grab needs ZERO new OpenXR data.** The pinch pose (`pinch_ext/pose`) is
  already sampled every frame (from WP-G5). A per-hand NEAR/FAR mode keyed on *pinch-pose
  distance to the quad rectangle* (with hysteresis + dwell + grab-freeze) plus "classify the
  region under the projected contact point instead of the ray hit" reuses the entire grab
  machine unchanged. This is v1.
- **Why it felt broken:** grabs are gated on the aim-ray hit; with fingers ON the bar the ray
  overshoots to infinity and misses. A NEAR hand must **suppress its own ray** and grab off
  the contact point instead.
- Distances (all hot-tunable): NEAR enter 0.10 m, exit 0.25 m, contact band 0.06 m, snap 0.04 m.
- **Hand visibility problem** — compared four options; ship: **(b) in-swapchain contact
  cursor** (draw a hover dot at the projected hand point in the quad's own pixels) as v1 +
  **(d) proximity fade** (per-layer `colorScaleBias`, default on, mild) as a companion.
  Reject (c) per-pixel hand punch-out (swimming/latency, passthrough-only) → flagged
  experiment only.
- **Poke-to-click is v2**, off by default — accidental content clicks while reaching/walking
  are the main risk the big SDKs spend their tolerance budget on.

### Gaze (from 16)
- **v1 gaze source is the VIEW reference-space forward vector (center FOV).** Eye tracking
  (`XR_EXT_eye_gaze_interaction`) is an opt-in that auto-falls-back to VIEW; it is
  unvalidatable on Quest 3 (no eye HW) — treat as blocked-on-hardware, not the shippable core.
- **Reuse the ray/quad machinery for a HEAD ray.** Selection = nearest quad hit by the gaze
  ray; the chrome highlight IS the feedback (gaze has no cursor).
- **The gaze carry is a fourth `solve()` override**, placed before the grab override — not a
  new anchor mode. begin/pushPull/setDist/end verbs on `CXRAnchor`; distance clamps to the
  existing 0.3–5.0 m. **No release-lurch problem** here (keyboard release, no wrist swing).
- Keys are ordinary physical-keyboard binds — no XR input path. Support hold-to-carry
  (press+release on one key via Hyprland bind flags) *and* a toggle mode.
- **1€ filter + dwell + selection hysteresis from the start** — without them the
  boundary/keypress jitter is the felt failure. This is the same lesson as report 14 Stage B.

### Shared threading contract
All acquisition state that crosses to the frame thread is plain-value POD; all decision math
is pure in `XRMath.hpp` with gtests (the WP-G precedent). Frame thread does zero hyprutils
refcount ops (MEMORY 2026-07-07). Config reads on the frame thread are numeric `CConfigValue`
only. Verbs run main-thread under `m_layersMu`; the frame→main atomic write-back publishes
selection/hover state.

---

## Merged work-package backlog

Three tracks that share the grab engine and the `XRMath` pure-function + gtest discipline.
Most logic is headless-testable via the Monado remote-driver scripted-pose approach; items
marked **LIVE** need a Quest 3 (`preview-xr.sh --wivrn`) for feel/latency. Tracks are
independent. **Track A (ray aim & cursor) has shipped** — the endpoint cursor, sticky hover, hover
haptic, aim 1€ filter, and chrome-only magnetism all landed (see the Track A table below); Tracks D
(near-field) and Z (gaze) remain.

### Track A — Ray aim & cursor (report 14) — **A1–A4, B, C, E SHIPPED**
Endpoint cursor + sticky hover + hover haptic + aim 1€ filter + chrome-only magnetism landed
together. Pure logic (`classifyQuadRegionForgiving`, `stepHoverStick`, `aimFilterMinCutoff`, cursor
pack/color/tint/size in `XRMath.hpp`) is gtested in `tests/xr/ray_assist.cpp` (+12 gtests); the GL
`drawCursor` pass rides the existing `drawChrome`/snapshot infra; live feel remains to be tuned on a
headset. Config surface: `openxr:cursor*`, `openxr:magnet*`, `openxr:hover_hysteresis`/
`hover_dropout_frames`, `openxr:aim_filter*`/`aim_pinch_damping`, `openxr:haptics`/`haptic_hover`/
`haptic_amplitude` (see `04-input.md` §6.1 / `05-configuration.md`). `hyprctl openxr status` now
reports the sticky-stabilized per-quad `region` (scriptable — `hyprtester` `xr_hover_region_stability`).

| WP | What | Status |
|----|------|--------|
| A1 | Endpoint cursor draw (`drawCursor` in `XRGraphics`); export per-hand hit-uv from `processPointer` | **SHIPPED** (live feel TODO) |
| A2 | Hover hysteresis / sticky region Schmitt | **SHIPPED** |
| A3 | Hover haptic tick on grabbable-enter + `openxr:haptics` gate | **SHIPPED** (actuator LIVE) |
| A4 | Align hover test with the grab cone (surface slack in the highlight) | **SHIPPED** (folded into C) |
| B  | 1€ aim-pose filter + pinch-onset damping | **SHIPPED** |
| C  | Chrome-only magnetism cone (promote grab cone to per-frame hover snap; never BODY) | **SHIPPED** |
| D  | Optional straight beam (2 quad layers) or defer to a `hypxrchrome` companion | **DEFERRED** — cursor first (report 14 recommendation); revisit only if the laser look is missed |
| E  | Fold the live size bump into defaults; document corner↔margin clamp coupling | **SHIPPED** (was already the defaults; documented in `04-input.md` §6) |

### Track D — Direct / near-field pinch & poke (report 15)
| WP | What | Headless? |
|----|------|-----------|
| D1 | Geometry + NEAR/FAR mode machine (`pointQuadDistance`, hysteresis, dwell, grab-freeze, forgiveness-snap) | Yes — exhaustive gtests |
| D2 | Direct chrome grab: wire mode machine into `processPointer`; NEAR suppresses that hand's ray; pinch→snap→`grabActionForRegion`→begin; controllers via grip+squeeze | logic pure/gtest; e2e **LIVE** |
| D3 | Contact cursor + chrome swell (~1.5×) in the `drawChrome` pass at the projected point | draw math testable; feel **LIVE** |
| D4 | Proximity fade via per-layer `colorScaleBias` (coordinate with VISUALS WP-T1 — do once) | struct testable; feel LIVE |
| D5 | Poke-to-click v2: `poke_ext/pose`, press-plane machine, default off | logic gtest; e2e **LIVE** |
| D6 | Consume `XR_EXT_hand_tracking` (26 joints) — upgrades cursor to fingertip dots; enabler | Headless (remote-driver joint synth) |
| D7 | Hand punch-out experiment (joints→capsule SDF), passthrough-only, expect rejection | **LIVE**; maybe never |

Order: D1 → D2 → D3 (v1 ships here) → D4 → D5; D6 parallel after D1; D7 last. D6 has no deps.

### Track Z — Gaze grab (report 16) — **Z1–Z5 SHIPPED** (2026-07-12)
| WP | What | Headless? | Status |
|----|------|-----------|--------|
| Z1 | VIEW-space gaze-hover pass + selection publish (`m_gazeHoveredId`, per-layer `m_gazeSelected`) | Yes — scripted pose gtests | **shipped** — `gazeSelectPass` |
| Z2 | Gaze stabilization: 1€ filter + dwell + selection hysteresis. **The felt-quality WP.** | Yes | **shipped** — `SXRGazeSelect`/`stepGazeSelect` + `gaze_filter`/`gaze_dwell_ms`/`gaze_hysteresis_deg` |
| Z3 | `CXRAnchor` gaze-carry state + `solve()` override + begin/pushPull/setDist/end | Yes — deterministic | **shipped** — solve gaze branch after the hand-grab override |
| Z4 | Dispatchers + `cmd*` funnel + main↔frame handoff; toggle + gazepush/gazerelease; status JSON `gaze:{}` | partly; key-flag feel LIVE | **shipped** — `gazegrab`(toggle)/`gazerelease`/`gazepush` |
| Z5 | Selection + carry highlight via the chrome-fade envelope + a gaze cursor dot; `openxr:gaze_cursor_col` | state logic seam; visual LIVE | **shipped** — candidate lights the move-bar; carry glows grab-color + gaze cursor |
| Z6 | Eye-gaze source (`XR_EXT_eye_gaze_interaction`), auto-fallback to VIEW | blocked on Quest-Pro-class HW | **deferred** — `SXRSolveInput::gaze` field + `gaze_source=view\|eye` scaffold in place |

Shipped decisions (locked with the user): gaze grab is a **TOGGLE** (tap grab / tap release); while
grabbed the monitor **follows the head ray** at grab-time distance (`gaze_follow`, freeze available);
push/pull is **stepped repeats** via `binde` + `gazepush`. Feedback = chrome highlight (no permanent
reticle) + a gaze cursor on the carried monitor. Conditional hand-input gating (Part A, `hand_input =
on|off|auto`) shipped alongside — the user's live pain was false hand gestures while typing.

**Live-check backlog (Z4 key-flag feel + Z5 visuals, need a Quest 3):** confirm the toggle/`binde`
feel on a real keyboard; judge the dwell time (200ms) + hysteresis (3°) + gaze-cursor size/color;
confirm the candidate highlight (move-bar hover) reads clearly at typical monitor distances.

### Cross-track dependencies & de-dup
- **1€ stabilization** appears in A-B, D (carry), and Z2 — all reuse the shipped
  `oneEuroStepPose` / `SXROneEuroPose`. Don't re-implement.
- **Proximity fade** (D4) and the VISUALS transparency plumbing (`colorScaleBias`, VISUALS
  WP-T1 / report 07 WP-C1) are the *same* struct-chaining work — do it once, whichever lands
  first, and have the other rebase onto it.
- A NEAR hand (D2) and a gaze grab (Z) and a far ray (A) can be simultaneously live on
  different hands / the head; the arbitration is per-hand mode + gaze being a separate carry
  override. Keep the "one carry per anchor" invariant.

---

## Open questions carried forward (for user triage)
- Ship grab-only direct v1 (D2+D3) with poke behind a default-off flag? (recommended)
- Is a NEAR hand casting *no* ray acceptable (matches Quest/visionOS)? (recommended yes)
- Gaze carry: follow-while-held vs freeze-on-grab as the default? Hold-pair vs toggle vs
  single-bind-both-edges as the default bind shape?
- Do we want a straight beam at all, or is the endpoint cursor + highlight enough? (report 14
  recommends cursor-first, beam deferred / offloaded to a companion.)
