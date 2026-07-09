# 14 — Ray Aim, Cursor Visualization & Target Assist for XR Chrome

Research report. **No implementation, no live runs, no commits.** Written 2026-07-09 against
branch `hypxrland`, source-read only, in response to live Quest 3 + WiVRn feedback: *"really
hard to grab the bar/corners, especially with controller."* A stopgap size bump was applied
live (`chrome_margin` 0.04→0.10 m, `chrome_bar_height` 0.05→0.08, `chrome_corner_size`
0.06→0.09).

---

## TL;DR

1. **There is NO ray or cursor visualization anywhere in HypXRland today.** Confirmed by
   source read + grep across `src/openxr/`: the only per-frame GL draw besides the desktop
   blit is `CXRGraphics::drawChrome` (the bar + corner handles, `XRGraphics.cpp:562`). No
   beam, no laser, no endpoint dot, no reticle, no crosshair — not even a debug one. **The
   user is aiming a completely invisible ray.** The *only* feedback that the ray is on a
   target is the chrome hover-highlight (`chrome_col_hover`, `XRGraphics.cpp:611`), which only
   appears once the ray is already exactly inside the bar/corner rect. This is almost
   certainly the dominant cause of the pain and should be stated plainly to the user: **you
   cannot aim a ray you cannot see.** Every mainstream system (Quest, SteamVR, MRTK) draws a
   beam + endpoint cursor for controllers; visionOS omits the beam but still renders a strong
   gaze *hover highlight* on a large target — we have neither a beam nor a large target.

2. **The targets are genuinely small in angular terms.** The constraining dimension of the
   move-bar is its *height* (0.05 m default), and the corner handle is **clamped to the
   margin** (`cornerSize = clamp(cornerSize, 0, margin)`, `XRMath.hpp:579`), so the default
   0.06 corner is silently clamped to 0.04 m. At the default 1.5 m distance these subtend
   ~1.9° (bar) and ~1.5° (corner); at a comfortable 2.5 m they drop to **1.1° and 0.9°** —
   below the ~1.5–2° comfortable minimum for ray/gaze targets. The live bump helps
   (bar→3.1°/1.8°, corner→3.4°/2.1°) but corners are still marginal far away. Angular size
   dominates ray-selection difficulty *quadratically* (Kopper et al. distal-pointing model),
   so shrinking targets is exactly the wrong axis to be small on.

3. **The grab path already has ±5° cone forgiveness, but the *hover* path has none, and
   there is no aim-pose stabilization, no hover hysteresis, and no hover haptic.** The
   5° `XR_GRAB_CONE_DEG` slack (`XRInput.cpp:593`) only fires on the squeeze rising edge and
   only as a fallback when the exact-hit hover didn't already land a grabbable region — so the
   *visual* hover highlight is tighter than the grab tolerance, and the user gets no cursor and
   no highlight while hunting. The aim pose is fed **raw** into hit-testing (the 1€ filter
   exists but is wired only to the grab *carry*, `XRAnchor.cpp:200`, never to aim). Region
   transitions are instantaneous (no hysteresis on `m_hoverRegion`). Haptics fire on grab
   begin/end + click-press only (`XRInput.cpp:633/669/766`), never on hover-enter.

**Recommendation, in priority order:** (i) an **endpoint cursor** drawn in-swapchain at the
hit uv + **hover hysteresis** + **hover haptic tick** — small, cheap, high-impact, and likely
to resolve most of the pain on its own; (ii) a **1€ aim-pose filter** before hit-testing;
(iii) **snap/magnetism** toward the nearest chrome region within a cone (chrome-only, never for
content clicks); (iv) optional **beam rendering** (2 extra quad layers, or defer to the
`hypxrchrome` companion). Plus a near-free win independent of all the above: **align the hover
test's tolerance with the grab cone** so the highlight appears exactly when a squeeze would
grab.

---

## 1. What exists today (source-grounded)

### 1.1 The interaction pipeline
Per-frame, on the frame thread, after the anchor solve (`OpenXRManager.cpp:1054`):

1. `CXRInput::sample()` (`XRInput.cpp:298`) — `xrSyncActions` + `xrLocateSpace` of each hand's
   **aim** and **grip** action spaces in the reference space. Poses stored **raw** in
   `m_hands[hand].aim` / `.grip` / `.pinch`. No filtering.
2. Frame loop builds `SXRPointerTarget[]` — one per visible quad, full-quad (content + margin)
   world pose + `SXRChromeGeometry` (`OpenXRManager.cpp:1033-1041`).
3. `CXRInput::processPointer()` (`XRInput.cpp:426`):
   - **Ray cast** per non-grabbing hand: `dir = qRotate(aim.rot, {0,0,-1})`, nearest-`t`
     `rayQuadIntersect` across targets (`XRInput.cpp:493-520`). **Exact bounds, zero slack.**
   - `classifyQuadHit(u,v,chrome)` → BODY / BAR / CORNER_xx / MARGIN (`XRMath.hpp:610`).
   - BODY → remapped to content uv → drives the pointer (motion/click/scroll). BAR / CORNER /
     MARGIN → hover-only (no pointer events); recorded in `m_hoverRegion[hand]` /
     `m_hoverChromeMon[hand]`.
   - **Grab state machine** (`XRInput.cpp:543`): on the squeeze/pinch rising edge, prefer the
     region the exact-hit hover already classified; if that isn't grabbable, **fall back to a
     cone-forgiveness re-test** (below).
4. Frame loop publishes `chromeHoverRegion(id)` + `isMonitorGrabbed(id)` onto each layer for
   **next** frame's `drawChrome` (`OpenXRManager.cpp:1069-1074`).

### 1.2 The cone-forgiveness fallback grab path (studied per the ask)
`XRInput.cpp:584-606`. Only entered when `!target` — i.e. the exact-hit hover this frame did
**not** already classify a grabbable region on `m_hoverChromeMon[hand]`. It then re-rays every
target with slack:

```
slack = tan(XR_GRAB_CONE_DEG °) * planeT   // XR_GRAB_CONE_DEG = 5°, XRInput.hpp:80
hit   = rayQuadIntersect(..., slack)        // expands half-extents by `slack` on every side
```

`slack = tan(5°)·t` ≈ `0.0875·t` metres: **0.13 m at 1.5 m, 0.22 m at 2.5 m** of extra
half-extent on all four sides — a very generous ~5°-per-side expansion. Key properties:

- **Applies only at the squeeze instant**, not to hover/highlight.
- **Applies only as a fallback** — if the exact-hit hover already landed a grabbable region
  it's used directly (no slack needed there because it already hit).
- **Symmetric on all sides**, so a corner handle at grab-time effectively balloons by ±5° per
  edge — the grab is far more forgiving than the highlight that's supposed to guide it.
- **No visual counterpart.** The user sees a tight highlight (exact test) but the machine
  grabs on a loose test. The mismatch means: point near-but-not-on a corner → **no highlight**
  (looks like you'll miss) → squeeze anyway → it *does* grab (cone) — or, more often, the user
  doesn't squeeze because the absent highlight told them they'd miss. **Surfacing the cone
  visually (highlight/cursor on the slack test) is a near-free fix.**

### 1.3 Chrome geometry & the corner clamp (measured)
`makeChromeGeometry` (`XRMath.hpp:574`): margins L=R=T=`margin`, bottom = `margin + barHeight`;
bar centered under content at `barWidthFrac` of content width; corner handles are
`cornerSize`-metre squares **`clamp`ed to `margin`** (`XRMath.hpp:579`) so they never eat
content. Config defaults (`ConfigValues.cpp:759-778`):

| var | default | live-bumped |
|---|---|---|
| `openxr:chrome_margin` | 0.04 m | 0.10 m |
| `openxr:chrome_bar_height` | 0.05 m | 0.08 m |
| `openxr:chrome_bar_width_frac` | 0.6 | — |
| `openxr:chrome_corner_size` | 0.06 m (**clamped to margin ⇒ 0.04**) | 0.09 (clamp 0.10 ⇒ 0.09) |
| `openxr:default_size` (content W) | 1.6 m | — |
| `openxr:default_distance` | 1.5 m | — |

**Finding:** with the *default* config the corner handle is silently clamped from 0.06 to
**0.04 m** — the smallest, hardest target, and the one the user most struggled with. The live
bump raised the margin to 0.10 so the 0.09 corner survives the clamp. Any corner-size increase
must raise `chrome_margin` in lockstep or the clamp eats it.

### 1.4 What's drawn
`drawChrome` (`XRGraphics.cpp:562`) fills the bar + corner uv rects with premultiplied
`chrome_col_idle/hover/grab`, auto-hiding via `chromeFadeAdvance`. `hoverRegion` highlights the
pointed element; `grabbed` overrides to the grab color. **That is the entire visual
vocabulary of aiming.** No ray, no cursor, no per-hand indication of *which* hand is pointing
where, no endpoint. When the ray is in the transparent MARGIN or off-quad, there is zero
feedback of any kind.

---

## 2. Angular-size math on our chrome

Angular size of a target of linear extent `s` (in its constraining axis, perpendicular to the
line of sight) at distance `d`: **θ = 2·atan(s / 2d)**. The *constraining* axis is what governs
Fitts difficulty — for the bar that's its **height** (it's wide but thin); for a square corner
it's the side.

| Target (constraining extent) | 1.5 m | 2.0 m | 2.5 m |
|---|---|---|---|
| Bar height **0.05 m** (default) | **1.91°** | 1.43° | **1.15°** |
| Bar height **0.08 m** (bumped) | 3.06° | 2.29° | 1.83° |
| Corner **0.04 m** (default, post-clamp) | **1.53°** | 1.15° | **0.92°** |
| Corner **0.09 m** (bumped) | 3.44° | 2.58° | 2.06° |
| Bar *width* 0.96 m (0.6·1.6, non-constraining) | 35° | 27° | 22° |
| Grab cone slack (±, per side) | ~5° | ~5° | ~5° |

**Reference minimums (see §3 prior art):**
- Practitioner rule of thumb for **comfortable** ray/gaze targets: **~1.5–2°** in the
  constraining axis; ~1° is an aggressive floor where error rates climb steeply.
- Apple visionOS mandates **60 pt** minimum for gaze targets — roughly **~2–2.5°** at its
  nominal viewing distance — precisely *because* gaze/indirect aiming is imprecise.
- Kopper/Bowman distal-pointing model: movement time is dominated by **angular size**, and the
  difficulty growth is **quadratic** in the index, so a target that's half the angular size is
  disproportionately harder.

**Interpretation.** Default bar (1.9°→1.15°) and default clamped corners (1.5°→0.92°) sit *at
or below* the comfortable floor across the normal 1.5–2.5 m working range, and the corner falls
under 1° at 2.5 m — a genuinely hard ray target *even with a visible cursor*, and near-hopeless
without one. The live bump pushes the bar to a healthy 1.8–3.1° and corners to 2.1–3.4°, which
should be inside the comfortable band down to ~2.5 m. So the size bump was directionally
correct and worth keeping as new defaults — **but** it treats a symptom; the missing cursor and
missing hover feedback are the larger levers, and magnetism/hysteresis let us keep targets
small without the pain.

---

## 3. Prior art

| System | Beam? | Endpoint cursor | Stabilization | Target assist | Hover feedback |
|---|---|---|---|---|---|
| **Meta Quest** system UI (controllers) | **Yes** — curved/straight laser from controller, state-colored | Yes — dot/ring at hit, grows/animates on pinch-strength | Runtime-filtered **`aim`/PointerPose** (Meta: *"deriving a stable pointing direction… is non-trivial… filtering, gesture detection"*); extra damping as pinch strength rises | Sticky/segmented UI; large hit-slop on system buttons | Highlight glow + **haptic pulse** on hover-enter (controllers) |
| **Meta Quest** hand-tracking | Yes — thinner pinch ray from `pinch_ext`/aim | Yes — pinch cursor scales with `PinchStrength` | Same stabilized `aim` pose; pinch pose is a defined stabilized point | Same | Visual pinch cursor; no haptics (no hand actuator) |
| **SteamVR / OpenVR** (OVR Toolkit, XSOverlay) | Yes — laser per controller | Yes | Runtime pose | Overlay hit-slop; grab handles | Hover glow + edge highlight + **haptic + click sound** (XSOverlay's single most-cited touch) |
| **MRTK 2/3** far interaction | Yes — parabolic/straight hand ray | Yes — cursor sprite that morphs on state | Ray is smoothed; cursor has a stabilizer | **Bubble / snap** interactors; focus "sticks" to nearest interactable | Focus highlight; morphing cursor |
| **Apple visionOS** | **No beam** (eyes + pinch) | **No cursor** — the *target itself* highlights | Gaze fixation + system filtering | Large **60 pt** targets (§2); generous hover regions | Strong **hover-effect highlight** on the looked-at element |
| **HypXRland today** | **No** | **No** | **None on aim** (raw pose) | Grab-only ±5° cone (no visual) | Chrome highlight only, **exact** test, no haptic |

**Takeaways relevant to us:**

- **Every beam-based system pairs the beam with an endpoint cursor**, and the cursor is the
  part users actually track. Quest's cursor even encodes pinch-strength by scaling.
- **visionOS proves a beam is optional** *if* the target is large and highlights strongly on
  hover. We are the worst of both worlds: no beam *and* small targets *and* a highlight that
  only appears on exact entry. An endpoint cursor alone (visionOS-style reliance on
  target-highlight, plus a small dot) likely fixes most of the pain.
- **Meta explicitly documents that a stable pointing direction requires filtering** and hands
  the app a pre-stabilized `aim`/PointerPose — the runtime does *some* of this, but we then
  hit-test tight targets against the raw located pose, and controllers' small residual jitter +
  our sub-2° targets + no cursor compound.
- **Pinch/press perturbs the aim pose**, which is why Meta increases aim damping during pinch
  onset and why the pinch cursor is stabilized. Our grab uses the pinch *pose* for anchoring
  (good, WP-G5) but the *hit-test that decides which region gets grabbed* uses the raw aim
  pose sampled on the same frame the squeeze/pinch fires — the worst frame for aim stability.
- **Hover haptics** are the single cheapest tactile win (XSOverlay, Quest) and research 07
  (`07-premium-chrome.md` §3, C-list) already flagged it — we bind `m_hapticAction` and fire
  ticks, just never on hover-enter.

*(Angular-size / assist science: Kopper, Bowman et al. distal-pointing Fitts model — angular
size dominates, quadratic difficulty; Bubble Cursor (Grossman & Balakrishnan) and its VR
ray adaptation (Lu & Yu, "Bubble mechanism for ray-casting") — dynamic activation area snaps to
the nearest target so the user "does not have to accurately shoot through the target"; magnetic
cursor — warp to a nearby selectable, slow-on-target. 1€ filter: Casiez, Roussel & Vogel,
CHI'12, already vendored in `XRMath.hpp`.)*

Sources: [Meta Aim Hand / PointerPose (Unity XR Hands)](https://docs.unity3d.com/Packages/com.unity.xr.hands@1.3/manual/features/metahandtrackingaim.html),
[Meta hand-tracking aim pose forum](https://communityforums.atmeta.com/t5/OpenXR-Development/Hand-tracking-aim-pose/td-p/863890),
[Apple HIG — Eyes](https://developer.apple.com/design/human-interface-guidelines/eyes),
[Apple WWDC25 — Design hover interactions for visionOS](https://developer.apple.com/videos/play/wwdc2025/303/),
[MRTK Pointers](https://learn.microsoft.com/en-us/windows/mixed-reality/mrtk-unity/mrtk2/features/input/pointers?view=mrtkunity-2022-05),
[Lu & Yu — Bubble Mechanism for Ray-Casting in VR](https://pi.cs.tsinghua.edu.cn/lab/papers/Investigating%20Bubble%20Mechanism.pdf),
[Expanding Targets in VR: a Fitts' Law study](https://arxiv.org/pdf/2308.12515),
[XSOverlay settings/haptics](https://xsoverlay.vercel.app/settings).

---

## 4. Staged design

Ordered by impact-to-effort. Each stage is independently shippable and independently testable.

### Stage A — Endpoint cursor + hover hysteresis + hover haptic *(small, high impact)*

**A1. Endpoint cursor (in-swapchain).** The swapchain is ours, so the cheapest cursor is a
dot/ring drawn *into the hovered quad's swapchain image* at the hit uv, exactly like
`drawChrome` already draws into it. `processPointer` already computes the per-hand hit uv for
BODY (`m_hoverUV`) and classifies chrome hits; export the raw full-quad hit uv per hand
(`m_hoverChromeUV[hand]`, trivial addition) and have the frame loop pass it to a new
`CXRGraphics::drawCursor(layer, dst, uv, state, hand)`.
- **State-colored** (idle / hover-grabbable / press / grab), matching the chrome color set.
- **Size** in uv derived from a config angular size so it looks constant regardless of monitor
  meters; **grow on press/pinch-strength** (Quest-style) using `select`/`pinchValue`.
- **Per-hand** tint (L/R) so two rays are distinguishable.
- Draw for **every** hovered region incl. MARGIN/BODY — the point is the user always sees where
  the ray lands. Off-quad (ray missed all quads) has no swapchain to draw into → this is the
  one case that needs a beam or a "no target" affordance (Stage D, or accept it: the ray only
  matters near a quad).
- **Cost:** one more textured-quad draw per hovered layer per frame; no new XR layers; no new
  threads. Fits the existing frame-thread GL discipline.

**A2. Hover hysteresis.** Region transitions are currently instantaneous
(`m_hoverRegion[hand] = classify(...)` every frame). Add sticky targeting: once a hand's ray is
inside a *grabbable* region (bar/corner), require it to move a hysteresis margin (angular, e.g.
+0.5–1°, or a uv epsilon scaled by distance) *out* before dropping the region — and optionally
snap-hold across a 1–2 frame dropout. Mirrors the select Schmitt (`SXRSchmitt`) already used for
buttons; implement as a per-hand region Schmitt/latch. Stops the highlight (and the grab
eligibility) from flickering at the boundary, which is a large part of "hard to grab."

**A3. Hover haptic tick.** Fire `hapticTick(hand)` on a **grabbable-region-enter edge** (NONE/
body/margin → bar/corner). We already have the actuator bound and the tick helper; this is ~5
lines gated behind a new `openxr:haptics` bool. Controllers get a real tactile "you're on the
handle" cue — the highest-value controller-specific fix after the cursor. Hands get nothing
(no actuator) but lose nothing.

**A4. Free alignment fix.** Make the *hover* classification use the **same ±cone slack** as the
grab fallback (or expose the cone test's result to the highlight), so the highlight/cursor turns
"grabbable" exactly when a squeeze *would* grab. Removes the tight-highlight/loose-grab mismatch
(§1.2) at zero new machinery.

### Stage B — Aim-pose stabilization filter *(small)*

Run a **1€ filter on the aim pose before hit-testing**, independent of the existing grab-carry
filter. `SXROneEuroPose` / `oneEuroStepPose` already exist (`XRMath.hpp:314`) and are
gtest-verified. Add a per-hand `SXROneEuroPose m_aimFilter[2]` in `CXRInput`, filter
`m_hands[hand].aim` in `sample()` (or at the top of the ray cast) with `dt` from the solve,
behind `openxr:aim_filter` + `aim_filter_min_cutoff` / `aim_filter_beta`.
- **Applies to controllers AND hands** (unlike `grab_filter`, hands-only).
- **Pinch-onset damping:** when `select`/`pinchValue` is rising past a low threshold, transiently
  *lower* the min-cutoff (more smoothing) for a few frames so the press/pinch doesn't yank the
  aim off-target the instant the user commits — this is the documented Meta behavior. Cheap:
  scale `minCutoff` by a factor while `pinchStrength ∈ (0.1, on)`.
- Trade-off: ~1 frame of latency. Ray pointing tolerates this far better than a carried window
  does; default it **on** for aim (unlike carry which defaults off).

### Stage C — Snap / magnetism cone *(medium)*

"Sticky ray": within an angular cone of the ray, pull the *effective* hit point toward the
nearest interactive **chrome** region (bar/corner), Bubble-Cursor-style, so the user doesn't
have to thread the exact rect. Implementation: after the exact ray cast, if no grabbable region
is hit but a chrome region lies within `openxr:magnet_angle` of the ray, treat the ray as
hitting that region for **hover/highlight/grab-eligibility** (reuse the existing cone machinery,
which already computes exactly this at grab-time — promote it to run every frame for hover, not
just on the squeeze edge). Add gentle "slow-on-target" by damping the cursor's uv motion while
snapped (magnetic-cursor behavior).

**Position (important):** apply magnetism to **CHROME regions only** (bar/corners), **never to
BODY content clicks.** Desktop clicking wants pixel-accurate, unwarped aim; magnetism there
would make links/buttons feel like they "jump." So: content = precision, chrome = magnetism.
This keeps the aggressive assist where targets are small and hand-authored (our chrome) and
keeps 1:1 pointing where the user expects a real cursor (the desktop).

### Stage D — Beam rendering *(optional, medium)*

A world-space beam needs geometry, not a swapchain draw. Options:
1. **Two extra quad layers** (one per hand), a thin long quad pose-composed along the aim ray
   each frame, with a gradient/fade texture (near-opaque at the hand, fading to the endpoint).
   Layer count is fine (we already submit N monitor quads). Straight beam is trivial; a curved
   beam needs a strip/mesh which `XrCompositionLayerQuad` can't express — would need a projection
   layer (heavier). Recommend **straight** if we do this at all.
2. **Defer beams to the `hypxrchrome` companion** (research 07) — a companion client can render
   arbitrary geometry (curved beams, particles, shadows) without touching the compositor's quad
   pipeline. This is the cleaner long-term home for premium visuals.
3. **Skip beams entirely** and rely on the Stage-A endpoint cursor (visionOS precedent). Given
   our quad-only architecture and that the cursor is the part users track, **this is the
   recommended default**; add beams only if users specifically want the controller-laser look.

---

## 5. Config sketch

Following research 07's theming direction (per-state colors, angular sizing). All hot-reloadable
where cheap.

```
# ---- ray cursor (Stage A) ----
openxr:cursor                 = true          # draw an endpoint cursor at the ray hit
openxr:cursor_size            = 0.9   (0.1..5) # angular diameter in degrees (distance-independent)
openxr:cursor_press_scale     = 1.6   (1..4)   # grow factor at full press/pinch strength
openxr:cursor_col_idle        = 0x80ffffff     # over body/margin
openxr:cursor_col_grabbable   = 0xcc66aaff     # over bar/corner (matches chrome_col_hover)
openxr:cursor_col_press       = 0xffffffff
openxr:cursor_col_grab        = 0xff66aaff
openxr:cursor_per_hand_tint   = true           # distinguish L/R rays

# ---- hover feel (Stage A) ----
openxr:hover_hysteresis_deg   = 0.6   (0..3)   # sticky-target exit margin
openxr:haptics                = true           # master haptics toggle
openxr:haptic_hover           = true           # tick on grabbable-region enter
openxr:haptic_amplitude       = 0.5   (0..1)

# ---- aim stabilization (Stage B) ----
openxr:aim_filter             = true           # 1€ filter the aim pose before hit-test (both devices)
openxr:aim_filter_min_cutoff  = 1.5   (0.01..10)
openxr:aim_filter_beta        = 0.01  (0..1)
openxr:aim_pinch_damping      = 0.4   (0..1)   # min-cutoff multiplier during press/pinch onset (lower = more damping)

# ---- magnetism (Stage C), chrome-only ----
openxr:magnet                 = true
openxr:magnet_angle           = 2.0   (0..8)   # cone half-angle to snap to a chrome region
openxr:magnet_content         = false          # POSITION: keep desktop clicks precise (do NOT magnetize BODY)

# ---- beam (Stage D, optional) ----
openxr:beam                   = false          # off by default; endpoint cursor is enough
openxr:beam_col               = 0x8066aaff
openxr:beam_length            = 5.0   (0.5..20) # clamp length when the ray misses all quads

# ---- new chrome-size defaults (fold the live bump in) ----
openxr:chrome_margin          = 0.10           # was 0.04; keeps 0.09 corners un-clamped
openxr:chrome_bar_height      = 0.08           # was 0.05  (~1.8-3.1° across 1.5-2.5 m)
openxr:chrome_corner_size     = 0.09           # was 0.06  (clamp headroom under margin 0.10)
```

---

## 6. WP sketch

| WP | Scope | Headless-testable? | Notes |
|---|---|---|---|
| **WP-A1** | Endpoint cursor draw (`drawCursor` in `XRGraphics`), export per-hand hit uv from `processPointer` | Partly — cursor-uv computation is pure (gtest the hit-uv export + state selection); the GL draw needs the compositor (visual, live Quest) | Reuses `drawChrome` infra; no new layers/threads |
| **WP-A2** | Hover hysteresis / sticky region (region Schmitt) | **Yes** — pure state machine over scripted ray poses via the remote driver; gtest the region-latch truth table | Mirrors `SXRSchmitt` |
| **WP-A3** | Hover haptic tick on grabbable-enter + `openxr:haptics` gate | Partly — edge logic is testable headless (assert `hapticTick` called on the transition via a seam); actuator effect is live-only | Actuator no-op under Monado null |
| **WP-A4** | Align hover test with grab cone (surface slack in highlight) | **Yes** — pure classify-with-slack, gtest against exact vs cone | Near-free |
| **WP-B** | 1€ aim-pose filter + pinch-onset damping | **Yes** — `oneEuroStepPose` already gtest-verified; add scripted-aim jitter test asserting reduced variance + latency bound | Both devices |
| **WP-C** | Chrome-only magnetism cone (promote grab cone to per-frame hover) | **Yes** — scripted near-miss rays assert snap-to-region; assert BODY never magnetized | Position: content precision preserved |
| **WP-D** | Optional straight beam (2 quad layers) or defer to `hypxrchrome` | No — visual/feel, **live Quest** | Recommend deferring; cursor first |
| **WP-E** | Fold size bump into defaults + document the corner↔margin clamp coupling | **Yes** — `makeChromeGeometry` gtest already covers clamp; add a default-values assertion | Trivial |

**Headless coverage:** the *logic* of A2, A4, B, C, and E is fully exercisable with the existing
`hyprtester --xr` remote-driver harness (scripted aim poses + squeeze), because the hit-test,
classification, hysteresis, filter, and magnetism are all pure functions over poses. Only the
*rendering* (A1 cursor, D beam) and the *tactile* effect (A3 actuator) need a live headset to
judge — and A1/A3's decision logic still has a headless seam.

---

## 7. Open questions for the user

1. **Beam: yes or no?** visionOS ships none; Quest controllers ship one. Recommendation: **no
   beam initially** — an endpoint cursor is the part users track and it fits our quad-only
   architecture with zero new layers. Add a straight beam later (Stage D / `hypxrchrome`) only
   if the controller-laser look is missed. Your call on whether the laser is part of the desired
   aesthetic.
2. **Cursor style?** Dot, ring, or ring-that-fills-on-press? Per-hand color? Recommendation:
   a **small ring that grows + fills on press/pinch-strength**, per-hand tinted, state-colored
   to match the chrome palette.
3. **Haptic strength/pattern on hover?** A single 10 ms 0.5-amplitude tick on grabbable-enter
   (as A3), or something softer/continuous? Continuous hover buzz gets annoying fast —
   recommend a one-shot tick on enter only.
4. **Magnetism aggressiveness — and where?** Strong magnetism helps small targets but feels
   *wrong* for precise desktop clicking (buttons "jump"). **Position taken: magnetism for CHROME
   regions only (bar/corners), precision 1:1 for BODY content clicks** (`openxr:magnet_content =
   false` default). Do you agree, or do you also want light assist on desktop UI (e.g. snapping
   near window-edge controls)?
5. **Keep the live size bump as the new defaults?** The math (§2) says yes — the bumped bar/
   corner sizes sit inside the comfortable 1.5–2.5 m band while the defaults fall below it. But
   with a cursor + hysteresis + magnetism we *could* shrink chrome back toward the original,
   less-intrusive footprint. Bigger-and-simpler vs smaller-with-assist — your preference?
6. **Aim filtering default on?** Recommend **on** for aim (unlike the carry filter which is off
   by default): ~1 frame latency is imperceptible for pointing and the jitter reduction is large,
   especially for hand-tracking. Any objection to it being on out of the box?

---

## 8. Files referenced

- `src/openxr/XRInput.cpp` / `.hpp` — ray cast, hover, grab machine, cone forgiveness
  (`XRInput.cpp:493`, `:584`, `:593`), haptics (`:391`, call sites `:633/669/766`).
- `src/openxr/XRMath.hpp` — `rayQuadIntersect` (`:350`), `classifyQuadHit` (`:610`),
  `makeChromeGeometry` + corner clamp (`:574/579`), `grabActionForRegion` (`:430`), 1€ filter
  (`:267/314`), `XR_GRAB_CONE_DEG` (`XRInput.hpp:80`).
- `src/openxr/XRGraphics.cpp` / `.hpp` — `drawChrome` (`:562`) — the only aiming visual today;
  `blitBuffer`, `snapshotSwapchain`. **No cursor/beam draw exists.**
- `src/openxr/OpenXRManager.cpp` — target build (`:1033`), `processPointer` call + chrome
  publish (`:1054-1074`).
- `src/openxr/XRAnchor.cpp:200` — the 1€ filter's *only* current use (grab carry, not aim).
- `src/config/values/ConfigValues.cpp:715-778` — chrome / grab / pointer config defaults.
- Related research: `docs/openxr/research/04-grabbable-borders.md` (grab machine, 1€, release
  latch), `07-premium-chrome.md` (hover haptics, `hypxrchrome` companion, cursor/beam prior art).
```
