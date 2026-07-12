# VISUALS — premium chrome, transparency, and view-bounding

**Live planning surface.** Consolidates the presentation/appearance research into one
backlog. These three reports share one enabling primitive
(`XrCompositionLayerColorScaleBias`, verified WiVRn/Monado-supported) and one architectural
question (how much presentation to offload OUT of the compositor into a themeable companion).
Deep detail is in the archived originals.

Sources coalesced here:
- [`archive/07-premium-chrome.md`](archive/07-premium-chrome.md) — richer chrome geometry, motion effects, power modes, distro-themeable bundles + a `hypxrchrome` companion.
- [`archive/09-monitor-transparency.md`](archive/09-monitor-transparency.md) — static + dynamic (gaze/grab/focus/distance-reactive) per-monitor transparency.
- [`archive/10-view-bounding.md`](archive/10-view-bounding.md) — keep head/body-anchored monitors from drifting out of view.

---

## Capability vision

Make XR monitors *feel* premium and stay usable, while pushing as much presentation as
possible out of the compositor so distributions (Omarchy, CachyOS, Noctalia) can ship and
switch their own XR chrome themes without patching Hyprland:

- **Premium chrome** — tilt/scale/bob/spring motion on grab and hover, rounded/pill chrome
  geometry, sheen/glow, per-layer dim/tint, power modes, optional cylinder monitors, and a
  themeable bundle format with an optional companion client for shadows/glints/sound.
- **Transparency** — static per-monitor opacity plus dynamic effects: head-anchored HUD
  panels that are "fairly transparent" and solidify when looked at / focused / approached.
- **View-bounding** — head/body-leashed monitors that can never end up (or lag) outside the
  field of view, so no follower monitor gets "lost."

**The shared free primitive:** chaining `XrCompositionLayerColorScaleBiasKHR` onto each
quad's `next` gives per-layer dim/tint/glow/fade at zero GPU cost (the runtime does the
shader). Report 07 (WP-C1), report 09 (WP-T1), and INTERACTION's proximity fade (report 15
WP-D4) are **the same plumbing** — enable the ext once, chain once, feed one `m_alpha`/scale.
Do it once and have the others rebase.

**The architectural split (report 07):** motion and pose effects are *free and must stay in
the compositor* (quads are posed every frame pre-submit; a tilt/spring is a quaternion
multiply). Rich *presentation* (shadows, glints, contact blobs, sound) belongs in a companion
overlay client (`hypxrchrome`, hypxrpaper pattern) fed by an event-granular IPC feed, so
themes ship out-of-tree.

---

## Consolidated design decisions

### Motion & chrome (from 07)
- **Pre-submit motion is the single highest-ROI premium effect** and is nearly free: apply an
  `SXRChromeMotion` to `quadCenterPose`/`size` before submit — tilt-on-grab, hover-scale,
  select-pulse, release-spring (reuse the `CXRAnchor` leash spring). Pure-math, unit-testable.
- **Per-layer dim/tint/glow is free** via `colorScaleBias` (the shared primitive).
- **Themeable chrome:** an `SXRChromeTheme` struct + `theme.toml` parser + discovery search +
  hot-reload; default = today's look. A themed `drawChrome` v2 replaces scissor-fills with an
  SDF/atlas pass (rounded corners, pill bar, gradient, border, sprite atlas). Margin-only draw
  is preserved; the content rect is never touched; the fade envelope still gates redraw.
- **Power modes:** `openxr:chrome_power=high/low/auto` with a frame-pacing headroom estimator
  (`XR_FB_display_refresh_rate`) + hysteresis; `low` disables the expensive effects.
- **Companion track:** an event-granular IPC feed (extend socket2 + `hyprctl openxr layout`)
  drives a `hypxrchrome` overlay client rendering contact-shadow blobs, focus vignette,
  hover glints, and spatial UI sound — all out-of-tree, degrading cleanly if absent.
- Optional per-monitor `curve` → `XrCompositionLayerCylinderKHR` (hit-test against the arc;
  falls back to quad if the ext is absent — WiVRn live-advertise unverified).

### Transparency (from 09)
- **Per-layer transparency WORKS over WiVRn because WiVRn composites SERVER-SIDE** (a full
  Monado-derived compositor in `wivrn-server`), so the "video codecs drop alpha" worry does
  not apply. `colorScaleBias` uniform alpha is the recommended, free mechanism.
- **Blend-mode dependence must be stated loudly:** transparency only *shows through* in `alpha`
  (passthrough) blend or over a hypxrpaper background; in `opaque` the runtime shows black
  behind. Document this everywhere.
- **Z-sorted overlapping monitors are already correct for alpha** (the shipped z-sort +
  premultiplied path).
- **Chrome fades with content by default** (position: yes) — a transparent monitor with solid
  chrome looks broken.
- **Text-legibility floor** — cite HUD guidance; don't let dynamic effects drop a focused
  panel below readable.
- Dynamic effects (gaze/grab/focus/distance-reactive) are the interesting part; a separate
  opt-in **baked per-pixel desktop alpha** ("passthrough desktop" mode, WP-T7) carries the
  desktop's own window alpha — future, clearly distinct from uniform opacity. Reject
  dither/punch-through/stipple.

### View-bounding (from 10)
- **Five ways a head/body monitor leaves the FOV today:** unbounded configured/grab-set offset
  eccentricity (the core bug — nothing clamps offset *direction*, only distance/width),
  deadzone hysteresis parking the quad off-target after a turn, spring lag during sustained
  rotation (transient), body mode ignoring pitch by design (look up ⇒ gone), and body height
  staying fixed when you sit/stand.
- **Recommendation: MRTK-style cone clamp on the solve TARGET (`T.pos`), pre-deadzone**, for
  head (and the yaw part of body); plus **soft recall** (timeout then chase) for body pitch;
  plus optional **edge indicator** instead of movement. Compose with the shipped grab-release
  latch (the clamp applies to the frame *after* release re-seeds spring + offset).
- Complementary **set-time validation** so a configured offset can't be placed out of view in
  the first place.

### Shared threading & config contract
`colorScaleBias` values, motion params, and clamp bounds published from main → frame as plain
values (frame thread does zero hyprutils refcount ops; numeric `CConfigValue` reads only —
MEMORY 2026-07-07). Chrome vars already have the `parseKeyword` hot-reload special-case
(`openxr:chrome_*`); new visual vars needing live tuning follow the same pattern. Runtimes
lacking `color_scale_bias` skip gracefully (opaque, no regression).

---

## Merged work-package backlog

**Do the shared `colorScaleBias` plumbing first** (07 WP-C1 ≡ 09 WP-T1 ≡ INTERACTION WP-D4) —
one struct-chaining change that unblocks three features. Then the tracks parallelize.

### Shared enabler
| WP | What | Live? |
|----|------|-------|
| CSB | Enable `XR_KHR_composition_layer_color_scale_bias`; chain `XrCompositionLayerColorScaleBiasKHR` per quad; feed one `m_alpha`/scale; graceful skip when unadvertised | No (struct + premult math gtest) |

### Track C — Premium chrome (report 07)
| WP | What | Live? |
|----|------|-------|
| C1 | color_scale_bias plumbing + `focus_dim` *(= the shared CSB enabler)* | No |
| C2 | Pre-submit motion hook (`SXRChromeMotion`: tilt/hover-scale/pulse/spring) | feel |
| C3 | Theme model + `theme.toml` loader + discovery + validation | No |
| C4 | Themed `drawChrome` v2 (SDF/atlas: rounded corners, pill bar, gradient, sprites) | look |
| C5 | Sheen + glow effects, power-gated | look |
| C6 | Power modes (`chrome_power=high/low/auto`, headroom estimator, hysteresis) | auto tuning |
| C7 | Cylinder monitors (opt-in `curve` → cylinder layer, arc hit-test) | WiVRn advertise |
| C8 | Themed haptics (`XR_FB_haptic_pcm` waveforms) | haptics |
| C9 | Companion IPC feed (event-granular geometry/state over socket2 + `openxr layout`) | No |
| C10 | `hypxrchrome` companion skeleton (overlay client: shadow blobs + focus vignette) | compositing |
| C11 | Companion glints + PipeWire spatial UI sound | look/sound |
| C12 | Omarchy theme bundle + docs (`theme-set` integration) | No |

Critical path C1→C2→C3→C4; C5/C6/C7/C8 parallelize after C4; C9→C10→C11 (companion) after C9; C12 last.

### Track T — Transparency (report 09)
| WP | What | Live? |
|----|------|-------|
| T1 | colorScaleBias plumbing + new `m_alpha` *(shared with C1 — do once)* | No |
| T2 | Static per-monitor opacity (`openxr:...opacity`, `xrmonitor opacity` verb) | No |
| T3 | Dynamic effects: gaze/grab/focus/distance-reactive opacity envelopes | feel |
| T4 | Chrome-fades-with-content coupling | No |
| T5 | Theme-bundle alignment with report 07 (share the theme format) | look |
| T6 | Text-legibility floor + status surfacing | No |
| T7 | Baked per-pixel "passthrough desktop" alpha (opt-in, future) | passthrough |

T1-T2, T4, T6 headless-testable on the Monado remote-driver; T3/T5 need live Quest.

### Track B — View-bounding (report 10)
| WP | What | Effort |
|----|------|--------|
| B1 | Cone-clamp math + plumbing (pure view-space yaw/pitch box clamp on solve TARGET, pre-deadzone; tuning vars; gtests) | S |
| B2 | FOV capture (`xrLocateViews`), soft recall timing, body pitch band, IPC status | S/M · dep B1 |
| B3 | Optional edge indicator (`bound_mode=indicate`) — rides Track C's chrome decisions | M · dep B1, C |

### Cross-track de-dup
- **CSB / C1 / T1 / INTERACTION-D4 are one change.** Land it once; everything else chains onto it.
- **Theme format** is shared between C3 and T5 — one `theme.toml` schema covers chrome geometry
  *and* transparency envelopes.
- **B3 edge indicator** must follow, not lead, C's chrome-render decisions (it draws chrome).
- **Focus-dim** (C1) and **focus-reactive opacity** (T3) overlap — decide one "focus" signal
  and drive both from it.

---

## Open questions carried forward (for user triage)
- Default bar style: pill (visionOS-like) vs flat vs distro-sprite as the built-in?
- Ship the `hypxrchrome` companion at all, or keep everything compositor-side for now?
- Transparency: default static opacity + which dynamic effects on by default (focus? gaze?
  distance?) — and how loud to make the blend-mode caveat in the UI/docs.
- View-bounding default: cone-clamp on by default for head/body, or opt-in per monitor? What
  comfort half-angle defines "in view"?
