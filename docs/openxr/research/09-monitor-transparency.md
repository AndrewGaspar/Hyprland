# 09 — Configurable XR-Monitor Transparency Effects (research, no implementation)

Status: research only. No code changed, no live runs, no commits. Author: research pass 2026-07-08.
Scope: how HypXRland could expose *configurable transparency* for XR monitors — static per-monitor
opacity and dynamic (gaze / grab / focus / distance-reactive) effects — so a user can, e.g., make
head-anchored monitors "fairly transparent" HUD panels that solidify when looked at.

Cross-refs: `07-premium-chrome.md` (verified `XrCompositionLayerColorScaleBias` support, theme-bundle
direction, focus-dim idea — this report reuses and extends its §2/§4), `03-anchoring.md`
(local/head/body/device anchor classes), `01-vr-app-composition.md` (overlay/passthrough),
`02-3d-environments.md` (hypxrpaper background). Source of truth for today's behavior:
`src/openxr/XRGraphics.cpp` (blit forces content alpha 1.0, premultiplied chrome margins),
`src/openxr/OpenXRManager.cpp:929-1093` (quad build + submit + z-sort), `src/openxr/XRMonitorLayer.hpp`
(frame-thread rules), `src/openxr/XRSession.cpp` (ext enable list), `src/openxr/XRIpc.cpp` (verbs),
`src/config/values/ConfigValues.cpp:709-786` (openxr config surface).

---

## TL;DR (read this first — the feasibility question is answered)

1. **Per-layer transparency WORKS over WiVRn, because WiVRn composites SERVER-SIDE, not on the
   headset.** The critical worry — "video codecs drop alpha, so per-layer alpha can't survive the
   stream" — does not apply. WiVRn's server binary contains a full Monado-derived compositor
   (`wivrn::compositor`, `Dispatching compositor_layer_sync`, `system compositor` strings in
   `/usr/bin/wivrn-server`) that runs the same layer shaders as vendored Monado — including
   `color_scale`/`color_bias` (`subprojects/monado/.../shaders/layer_quad.frag:27`). It composites
   ALL layers (quads, cylinder, equirect2) into the per-eye images and *then* encodes them
   (nvenc / vaapi / x264 / av1 strings present). So `XrCompositionLayerColorScaleBias`, quad layer
   alpha, and z-order are all applied by the server *before* encode — the codec only ever sees the
   already-composited eye image. This is confirmed by WiVRn's own docs statement that it "encodes each
   eye separately, and the alpha channel as one for both eyes" — passthrough compositing on the Quest
   is fed a *separate encoded alpha plane* (`comp_swapchain views alpha layer`, `alpha_width` strings),
   not per-layer alpha. **Conclusion: every transparency mechanism below is feasible on both
   Monado-local and WiVRn. This is the same free per-layer modulation 07 §2 already verified.**

2. **The exact fade math (premultiplied) must scale RGB *and* A by the same factor.** Our quads are
   submitted premultiplied (no `UNPREMULTIPLIED_ALPHA` flag; content alpha forced to 1.0 —
   `XRGraphics.cpp:274,466`), and Monado blends premultiplied layers with `src=ONE,
   dst=ONE_MINUS_SRC_ALPHA` (`render_gfx.c:411,766`). To fade a monitor to opacity `f`, set
   `colorScale = (f, f, f, f)`, `colorBias = 0`. Scaling only alpha (`(1,1,1,f)`) is WRONG — it
   de-premultiplies and produces an over-bright / additive halo (spelled out in §2.1).

3. **Transparency is only MEANINGFUL over passthrough (`blend_mode=alpha`) or a `hypxrpaper`
   background.** Over `blend_mode=opaque` (the default black void) a semi-transparent monitor just
   dims toward black — it reads as brightness reduction, not "see-through." Design and docs must gate
   the feature on passthrough/background context (§3.1).

4. **The highest-value effect for the user's stated want is gaze/ray-reactive opacity, and it is
   free and already half-built.** Per-layer hover + grab state already cross to the frame thread as
   plain atomics (`XRMonitorLayer.hpp:149-150`, written at `OpenXRManager.cpp:1069-1074`). "Head-anchored
   monitors fairly transparent, opaque when you look at them" = pick `colorScale` per quad from
   `m_hoverRegion`/`m_grabbedNow` — zero GPU cost, no new threading.

5. **Chrome should fade WITH content (take-a-position §3.3).** Because `colorScaleBias` modulates the
   whole quad, chrome fades too — which is *correct*: a ghosted monitor ghosts entirely, and the
   existing chrome hover-fade envelope + the "solidify-on-gaze" rule bring both content and chrome back
   to full opacity the instant you point at it. A decoupled always-solid-chrome mode is a niche escape
   hatch, not the default.

6. **Config surface**: per-monitor `alpha:0.x` keyword arg + `hyprctl openxr alpha <name> <v>` verb +
   per-anchor-class defaults (`openxr:head_default_alpha`, …) + a dynamic `openxr:transparency_mode =
   static|gaze|focus|distance` with a reused `chromeFadeAdvance`-style envelope and a **legibility
   floor** (`openxr:transparency_min`, default ~0.35 — Meta MR guidance: text over passthrough needs
   contrast, ≥14 px, no permanent head-locked wash). Aligns 1:1 with 07's theme-bundle via a new
   `[transparency]` stanza (§4.3).

7. **7 work packages (WP-T1…T7)**; T1-T2, T4, T6 headless-testable on Monado remote-driver; T3/T5 need a
   live Quest for feel; T7 (true per-pixel "passthrough desktop" carrying the desktop's OWN window
   alpha) is a larger, separable effort with real caveats (§2.2).

---

## 1. What exists today (baseline)

- Each XR monitor is one `XrCompositionLayerQuad` built per frame at `OpenXRManager.cpp:1009-1018`,
  with `layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT` and **no**
  `UNPREMULTIPLIED_ALPHA` bit → the quad is treated as **premultiplied**.
- The blit **forces content alpha to 1.0** (dmabuf shader `XRGraphics.cpp:274`; CPU path `:466`;
  clear path `:502`) specifically so XRGB garbage alpha does not punch holes under `ALPHA_BLEND`
  passthrough. So today every monitor is fully opaque by construction.
- Chrome (bar/handles) is drawn into the transparent swapchain margin as **premultiplied** fills
  (`drawChrome`, `XRGraphics.cpp:590-607`: `glClearColor(cr*ea, cg*ea, cb*ea, ea)`), fade-scaled by a
  per-frame envelope (`m_chromeAlpha`, advanced by `OpenXR::chromeFadeAdvance`).
- `colorScaleBias` is **not yet enabled** — the session enables only
  `XR_MNDX_egl_enable`, `XR_KHR_opengl_es_enable`, and optionally local_floor / hand_interaction /
  hand_tracking / overlay (`XRSession.cpp:71-93`). No color-scale ext in the list.
- There is **no per-XR-monitor focus concept** in the XR code (grep of `src/openxr/` for
  `m_lastMonitor`/focus finds only session-focus, `XRInput.cpp:326`). Hyprland focus lives on the main
  thread (`g_pCompositor->m_lastMonitor`); reaching it from the frame thread needs the plain-value
  handoff pattern (MEMORY refcount rule; `XRMonitorLayer.hpp:33-45`).

---

## 2. Mechanism inventory (each verified against Monado source / spec / probes)

### 2.1 (a) Per-layer uniform alpha via `XrCompositionLayerColorScaleBias` — RECOMMENDED, free

**Verified.** `XR_KHR_composition_layer_color_scale_bias` and
`XR_TYPE_COMPOSITION_LAYER_COLOR_SCALE_BIAS_KHR` are strings-present in
`/usr/lib/wivrn/libopenxr_wivrn.so`. Monado applies it in every layer fragment shader:

```glsl
// subprojects/monado/src/xrt/compositor/shaders/layer_quad.frag:26-27
out_color = texture(image, uv);
out_color = clamp(out_color * ubo.color_scale + ubo.color_bias, 0.0, 1.0);
```

(identical in `layer_cylinder.frag:29`, `layer_equirect2.frag:114`, `layer_projection.frag:28`.)

**Pre- or post-composite?** *Pre-composite, per-layer.* The scale/bias is applied to the sampled
texel inside the layer's own draw, *before* that layer is alpha-blended into the eye buffer. It never
touches other layers or the background — it modulates only this monitor's source.

**Premultiplied fade math (the load-bearing detail).** Monado's premultiplied blend
(`render_gfx.c:411` `dstColorBlendFactor = ONE_MINUS_SRC_ALPHA`, `:766` `srcColorBlendFactor = ONE`)
computes, for a source texel `S` over destination `D`:

```
out.rgb = S.rgb + D.rgb * (1 - S.a)      // premultiplied "over"
```

Our content texel is premultiplied with `a = 1`, i.e. `S = (r, g, b, 1)`. To fade the whole monitor to
opacity `f ∈ [0,1]`, the composited result we want is a linear crossfade to the background:
`out = content.rgb * f + D * (1 - f)`. That requires the post-scale source texel to be
`(r·f, g·f, b·f, f)` — i.e. **scale RGB and A by the SAME factor**:

```
colorScale = (f, f, f, f)     colorBias = (0, 0, 0, 0)
```

Then `S' = clamp((r,g,b,1)*(f,f,f,f), 0,1) = (r·f, g·f, b·f, f)`, and the blend gives exactly
`content·f + D·(1-f)`. Correct.

**Why scaling only alpha is WRONG.** `colorScale = (1,1,1,f)` yields `S' = (r, g, b, f)` with
`rgb > a` — no longer premultiplied. The premultiplied blend then computes
`out = content.rgb + D·(1-f)`: the content contributes at FULL brightness regardless of `f`, only the
background bleed-through changes → an over-bright, additive-looking wash that never actually dims the
monitor. This is the classic premultiplied-alpha bug and is precisely why the exact math must be
spelled out for the implementer.

**Interaction with `TEXTURE_SOURCE_ALPHA`.** `XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT`
(already set, `OpenXRManager.cpp:1010`) is what tells the runtime to honor the source alpha in the
inter-layer blend at all; without it the layer would composite as opaque. `colorScaleBias` runs before
that blend, so the two compose cleanly: scale sets `S'.a = f`, the flag makes the runtime use `S'.a` in
the `over`. Both are needed together.

**Chrome margins under this scheme.** The chrome fills are already premultiplied `(rgb·ea, ea)`. Scaling
the whole layer by `(f,f,f,f)` gives `(rgb·ea·f, ea·f)` — still valid premultiplied, chrome fades by the
same `f` as content. This is the basis for the §3.3 "chrome fades with content" position.

**Cost:** fill one `XrCompositionLayerColorScaleBiasKHR` struct per quad, chain on `quad.next`. Zero
GPU cost on our side (the runtime/server does the modulate). FREE — same conclusion as 07 §2/§2.1.

**Graceful degradation:** if a runtime does not advertise the ext, skip chaining it and treat all
monitors as opaque (log once). The null-compositor test path and any minimal runtime still work.

### 2.2 (b) Baked per-pixel alpha in the swapchain (carry the DESKTOP's own window alpha) — future, caveated

Today the blit deliberately destroys source alpha (`fragColor.a = 1.0`). One could instead *preserve*
real per-pixel alpha so a transparent terminal / rounded window corners show passthrough (or a
hypxrpaper background) *through the actual window shape*, not just a uniform monitor fade. Evaluation:

- **The monitor buffer does not currently carry meaningful alpha.** Hyprland composites each output to
  a scanout buffer that is effectively opaque: window opacity/blur is blended against the wallpaper /
  background *before* it reaches the monitor buffer, and the buffer format on the headless output is
  XRGB8888 (undefined alpha — the whole reason the blit pins it to 1.0; `XRGraphics.cpp:271-274,447`).
  So "terminal transparency" is already flattened away by the time we see it.
- **To carry real alpha you need three changes**, all bigger than a uniform-opacity feature:
  1. an **ARGB8888** headless output buffer with a defined alpha channel (Aquamarine headless output
     format change — verify the headless allocator can produce ARGB and that aquamarine/GL scanout
     honors it);
  2. a **transparent-backdrop render** of that monitor (no opaque wallpaper fill; the desktop's
     background cleared to `(0,0,0,0)` so window alpha survives to the buffer) — essentially a
     "transparent desktop" output mode;
  3. **premultiplied** output so the existing premultiplied quad blend stays correct, and NOT forcing
     `a = 1.0` in the blit.
- **What breaks / risks:** most apps assume an opaque desktop; a transparent backdrop makes any
  unpainted region see-through (could be jarring), blur/shadows that sample the backdrop misbehave, and
  bandwidth on WiVRn rises (the alpha plane is a *second* encoded stream — WiVRn does encode "the alpha
  channel as one for both eyes", so it is supported but not free). This also interacts badly with
  `blend_mode=opaque` (alpha means nothing over a void).
- **Verdict:** worth a *separate, opt-in* "passthrough desktop" mode (WP-T7), clearly distinct from the
  uniform/dynamic opacity feature the user asked for. Uniform opacity via `colorScaleBias` (§2.1)
  delivers the requested "fairly transparent head-anchored monitors" with none of this risk; per-pixel
  desktop alpha is a power-user follow-on.

### 2.3 (c) Dither / punch-through / stipple alpha — REJECTED

One could emulate transparency by discarding a stipple pattern of pixels in the blit (ordered dither
"screen-door transparency"). Rejected: it shimmers under reprojection and foveated encoding (WiVRn
encodes lossy video — a 1-px checkerboard becomes mush), destroys text legibility (the opposite of the
§3.4 goal), and buys nothing over the free, exact `colorScaleBias` path. Do not build it.

### 2.4 Mechanism summary

| Mechanism | Verdict | Where applied | Cost | Meaningful over |
|---|---|---|---|---|
| (a) `colorScaleBias` uniform layer alpha | **RECOMMENDED** | server/runtime compositor, per-layer, pre-composite | free | passthrough or hypxrpaper bg |
| (b) baked per-pixel desktop alpha | future opt-in (WP-T7) | our blit + ARGB output + transparent backdrop | blit ALU + 2nd encoded plane on WiVRn | passthrough or bg |
| (c) dither/stipple punch-through | **rejected** | our blit | cheap but ugly | n/a |

---

## 3. Interaction analysis

### 3.1 Blend-mode dependence (state this loudly in docs)

`endInfo.environmentBlendMode` is chosen once at session start from `openxr:blend_mode`
(`OpenXRManager.cpp:1084`; `pickBlendMode`, `XRMonitorConfig.hpp:43`). Transparency is only *meaningful*
when there is something behind the monitor:

- **`alpha` (passthrough):** the faded monitor reveals the real room — the intended HUD effect. Note
  MEMORY: on WiVRn today the picked mode is `opaque` (passthrough testable via `blend_mode=alpha` +
  disable/enable); Monado null advertises OPAQUE only.
- **`opaque` (default black void):** a faded monitor blends toward **black** → looks like a dimmer, not
  a see-through. Still useful as a "de-emphasize background monitors" cue, but must not be sold as
  transparency. **Exception:** if a `hypxrpaper` background/panorama layer is present, fading reveals
  *it* (composited behind, farther in z) — genuinely see-through onto the ambient scene even under
  opaque. So the honest rule is: *transparency reveals whatever is composited behind — passthrough, a
  hypxrpaper background, or (over opaque with no background) black.* Document exactly this.

### 3.2 Z-sorted overlapping monitors (already correct for alpha)

Alpha "over" compositing requires **back-to-front** submission. Our per-frame sort already composites
farther-from-viewer first (`OpenXRManager.cpp:941-951`, `da > db`), which is exactly the correct order
for semi-transparent layers to blend properly. So overlapping semi-transparent monitors compose
correctly *for free* — a nearer ghost monitor will show a farther monitor through it. Whether users
*want* to see monitors through each other is a taste call (open question §6); the compositing is right
either way. One caveat: the `m_zOrder` explicit override tier sorts *before* distance, so a pinned-front
transparent monitor could composite out of distance order and blend slightly wrong against a farther one
— acceptable, and only visible with deliberate z overrides + transparency + overlap.

### 3.3 Should chrome fade with content? — POSITION: yes, by default

Because `colorScaleBias` modulates the whole quad (§2.1), the bar/handles fade with the content at the
same `f`. Take the position that **this is correct and desirable**:

- A ghosted HUD monitor should ghost *entirely* — a solid bar floating under a near-invisible screen
  looks broken.
- The existing chrome hover-fade envelope (`m_chromeAlpha`) and the recommended **solidify-on-gaze**
  rule (§3.5 / effect G) mean that the instant you point the ray at a ghost monitor, the layer alpha
  ramps to `active` (≈opaque) AND the chrome fades in — content and chrome become legible together,
  which is exactly when you need the chrome. So chrome legibility is preserved *when it matters* without
  decoupling.
- **Escape hatch (niche):** if a user wants permanently-solid chrome on a permanently-ghost monitor,
  that needs decoupling — bake the fade into the *content* alpha in the blit (multiply content RGB+A by
  `f` in the shader) and draw chrome at full alpha in the swapchain, submitting the quad WITHOUT
  `colorScaleBias`. Costs a per-pixel multiply in the blit (cheap) and forfeits the free runtime
  modulation. Offer only if requested; not the default.

### 3.4 Text-legibility floor (cite HUD guidance)

Meta's MR design guidance is explicit that UI over passthrough must maintain contrast, that text should
be ≥14 px (≥18 px comfortable), and — directly relevant — to **avoid head-locked HUD content** and to
loosely follow the head with smoothing rather than rigidly locking (which our head/body leash already
does — `openxr:leash_response`). Background brightness varies wildly over passthrough, so a monitor faded
too far becomes unreadable. Therefore enforce a **minimum alpha floor** (`openxr:transparency_min`,
proposed default ~0.35) below which content is never driven, and clamp the idle target to it. This is a
legibility safety rail, not a taste knob (users can lower it but are warned). Sources §7.

### 3.5 Threading (which effects need the main→frame handoff)

- **Frame-thread-only (free, no new threading):** gaze/ray-reactive and grab-reactive — `m_hoverRegion`,
  `m_grabbedNow` are already frame-thread atomics on the layer (`XRMonitorLayer.hpp:149-150`).
  Distance/angle-reactive too — `viewPose` and the solved quad pose are both in-hand on the frame thread
  (`OpenXRManager.cpp:944-946`).
- **Needs main→frame plain-value handoff:** focus-reactive (Hyprland's `m_lastMonitor` is main-thread).
  Add a `std::atomic<bool> m_isFocused` on the layer, written on the main thread when focus changes
  (never a hyprutils refcount op — MEMORY rule), read on the frame thread. Cheap and rule-compliant.

---

## 4. Effect designs

Notation: `f` = the per-quad opacity fed to `colorScale=(f,f,f,f)` each frame; targets are eased by a
`chromeFadeAdvance`-style envelope keyed on `predictedDisplayTime` deltas.

### 4.1 Static per-monitor opacity

- Per-monitor constant `f = m_alpha` (default 1.0). Set via keyword `alpha:0.x`, verb, or per-class
  default (§5). Simplest; the building block for everything else. Free.

### 4.2 Dynamic effects (the interesting part)

| Effect | Trigger (already available?) | `f` behavior | Thread | Cost |
|---|---|---|---|---|
| **G. Gaze/ray-reactive** (RECOMMENDED default for HUD) | `m_hoverRegion != NONE` on this quad (yes) | ray on quad → ease to `active` (opaque); else ease to `idle` (transparent) | frame | free |
| **Grab-reactive** ("carry ghost", macOS-drag feel) | `m_grabbedNow` (yes) | while grabbed → ease to `carry` (e.g. 0.6) so you see placement behind it; release → back to base | frame | free |
| **Focus-reactive** (07's focus_dim, as alpha) | Hyprland focused monitor == this (needs handoff) | focused → `active`; unfocused XR monitors → `idle` | main→frame atomic | free |
| **Distance/angle HUD** (head/body-anchored) | angle between `viewPose` forward and quad-center dir; optionally distance | centered in FOV → more transparent (peripheral HUD); turn to face it → solidify | frame | free (one dot product) |

**G is the direct answer to the user's want.** "Head-anchored monitors fairly transparent" = set the
head-anchored class default `idle` low (e.g. 0.4) and `transparency_mode = gaze`; the monitor sits ghosted
in the periphery and snaps opaque the moment the ray (or, if `XR_EXT_eye_gaze_interaction` is present and
bound, the true gaze) lands on it. Ray/aim is the portable proxy we already have; eye-gaze is a future
upgrade (probe: not currently in the ext list; `XR_EXT_eye_gaze_interaction` would need adding + a
privacy note, per 07's note that visionOS renders gaze-hover outside the app process).

**Distance/angle curve (HUD).** On the frame thread compute
`cosθ = dot(normalize(quadCenter - viewPose.pos), viewForward)`. Map to `f` via a smoothstep:
`f = lerp(idle, active, smoothstep(cosθ_wide, cosθ_narrow, cosθ))` so a monitor dead-center (looked
straight at) is `active` and one at the edge of view is `idle`. This makes a head-locked panel behave like
a real HUD: unobtrusive until you orient to it. Combine with G (ray) for "look OR point."

**Envelope reuse.** All dynamic modes ease `f` toward a target with the existing
`OpenXR::chromeFadeAdvance` pure envelope (already frame-thread, time-delta driven) — add a second
instance per layer for content alpha alongside the chrome one, or generalize `chromeFadeAdvance` to a
reusable `advanceToward(cur, target, dt, ms)`. Fade durations from `openxr:transparency_fade_ms`.

### 4.3 Theme-bundle alignment with report 07

07's recommendation is a distro-swappable `xr-chrome/theme.toml` bundle. Transparency slots into the
same bundle as a `[transparency]` stanza so a distro ships opacity behavior with its look:

```toml
[transparency]
mode        = "gaze"     # static | gaze | focus | distance
idle_alpha  = 0.40       # background / unlooked-at / peripheral
active_alpha = 1.0       # looked-at / focused / centered
carry_alpha  = 0.60      # while grabbed
min_alpha    = 0.35      # legibility floor (clamps idle_alpha up)
fade_ms      = 180
```

07 already has a `[color] focus_dim` knob — this report *supersedes* that scalar with the richer
`[transparency]` block (focus_dim becomes `mode="focus"` + `idle_alpha`). Note in 07's eventual
implementation that the two must not both drive layer color-scale. This is the single section aligning
the two reports.

---

## 5. Config / theming surface (concrete)

**Per-monitor (keyword + verb + serialization):**
- Extend `parseXRMonitorLine` (`XRMonitorConfig.hpp:89`) to accept a `alpha:0.x` key-value (same kv slot
  as future chrome kvs). Store on `SXRMonitorParams` (new `std::optional<float> m_alpha`) → seed
  `CXRMonitorLayer::m_alpha`.
- New verb `hyprctl openxr alpha <name> <0..1>` — add to the `XRIpc.cpp` subcommand dispatch and the
  usage list (`XRIpc.cpp:162`). Mutates the layer under `m_layersMu`.
- Serialize in `serializeXRMonitorLine` / `layoutDump` (`XRMonitorConfig.hpp:63`, `XRIpc.cpp:134`) so
  `hyprctl openxr layout` emits paste-ready `alpha:` and `status` reports current effective `f`.

**Per-anchor-class defaults (new config vars, `ConfigValues.cpp` after :786):**
```
openxr:head_default_alpha    (float 0..1, default 0.4)   # head-leashed HUD panels ghost by default
openxr:body_default_alpha    (float, default 0.85)
openxr:local_default_alpha   (float, default 1.0)        # world-fixed monitors stay solid
openxr:device_default_alpha  (float, default 1.0)
```
Resolution order for a monitor's base alpha: explicit `alpha:`/verb > class default > 1.0.

**Dynamic-effect vars:**
```
openxr:transparency_mode      (string: static|gaze|focus|distance, default "static")
openxr:transparency_active    (float, default 1.0)   # target when engaged
openxr:transparency_min       (float, default 0.35)  # legibility floor, clamps idle up
openxr:transparency_fade_ms   (int,   default 180)
openxr:transparency_hud_wide_deg / _narrow_deg (distance mode angular curve endpoints)
```
Hot-reload: these are per-frame cached reads on the frame thread (benign race — same tolerance as the
existing `chrome_col_*` reads, `XRGraphics.cpp:574-576`). No swapchain recreate needed (alpha is submit-time
only), so no `props_refreshed` geometry path involved — simpler than chrome geometry changes. Note the
legacy `hyprctl keyword` path caveat (MEMORY): if any transparency var wants a hot-toggle it needs the
same `parseKeyword` special-case as `openxr:enabled`/`inhibit_idle`; but since these are read every frame,
a plain reload suffices — no special-case needed.

---

## 6. Work-package sketch (WP-T1 … T7)

| WP | Title | Summary | Acceptance | Live Quest? |
|---|---|---|---|---|
| **T1** | colorScaleBias plumbing | Enable `XR_KHR_composition_layer_color_scale_bias` in `XRSession.cpp` ext list (guard on advertise); chain `XrCompositionLayerColorScaleBiasKHR` on each quad's `next`; feed `colorScale=(f,f,f,f)` from a new `m_alpha`. (Shared with 07 WP-C1 — do once.) | gtest for the struct build + premultiplied `f` math; monitors dim correctly over passthrough/bg; runtimes lacking the ext skip gracefully (opaque). | No (value verifiable headless; look = optional) |
| **T2** | Static per-monitor alpha config | `alpha:` keyword kv + `SXRMonitorParams::m_alpha` + `hyprctl openxr alpha` verb + per-class defaults + `layout`/`status` serialization. | Parser round-trip gtest; verb sets alpha live; `layout` emits paste-ready `alpha:`. | No |
| **T3** | Dynamic engine: gaze + grab | `transparency_mode`; per-layer content-alpha envelope (reuse/generalize `chromeFadeAdvance`); gaze/ray-reactive (from `m_hoverRegion`) + grab-reactive (`m_grabbedNow`), all frame-thread. | Headless: forcing hover/grab flags drives the right target; feel: HUD solidifies on point, ghosts on look-away. | Yes (feel) |
| **T4** | Focus-reactive dimming | `std::atomic<bool> m_isFocused` written on main thread on focus change (no refcount op), read on frame thread; unfocused XR monitors ease to idle. | Headless: toggling focus flag drives alpha; no frame-thread refcount ops (audit vs MEMORY rule). | No (feel = optional) |
| **T5** | Distance/angle HUD reactive | Frame-thread `dot(view fwd, quad dir)` → smoothstep curve between `_wide_deg`/`_narrow_deg`; legibility-floor clamp; head/body classes. | Pure-math unit test on the curve; feel: peripheral panels transparent, centered solidifies. | Yes (feel) |
| **T6** | Legibility floor + theme stanza | `transparency_min` clamp everywhere; `[transparency]` block in the 07 theme bundle (supersedes `focus_dim`); docs on blend-mode dependence + Meta HUD guidance. | Floor enforced (can't drive below min except explicit); theme parse gtest; docs. | No |
| **T7** | (Opt-in, larger) Passthrough-desktop per-pixel alpha | ARGB8888 headless output + transparent backdrop render + stop forcing blit alpha to 1.0; carries the desktop's OWN window alpha to passthrough. | Real terminal transparency shows passthrough through the window shape; gated behind an opt-in var; documented caveats (bandwidth, backdrop, opaque-mode no-op). | Yes (look) |

Critical path: **T1 → T2** (static core), then **T3 / T4 / T5** parallelize (dynamic effects), **T6**
folds in the floor + theme alignment. **T7** is independent and deferrable. T1/T2/T4/T6 are
headless-testable on the Monado remote-driver harness; T3/T5/T7 need a live Quest for feel/look.

---

## 7. Open questions for the user

1. **Default for head-anchored monitors:** is `idle_alpha ≈ 0.4` + `mode = gaze` the right out-of-box
   feel, or should transparency be entirely opt-in (default 1.0 everywhere)?
2. **Default dynamic mode:** ship `static` (safest) or `gaze` (most impressive) as the global default?
3. **Legibility floor value:** is 0.35 the right minimum, and should users be *allowed* to go below it
   (with a warning) or hard-clamped?
4. **Overlapping transparent monitors:** acceptable that a near ghost monitor shows farther monitors
   through it (correct compositing), or do you want overlap to force the near one opaque?
5. **Chrome coupling:** agree chrome fades with content by default (§3.3), or must chrome always stay
   solid (needs the decoupled baked-content path)?
6. **Focus-reactive dimming:** wanted at all? Some users dislike background dimming (07 §6.3 raised the
   same for chrome).
7. **Eye-gaze vs ray for "gaze":** ray/aim proxy now, or invest in `XR_EXT_eye_gaze_interaction`
   (true gaze) — with the privacy note that visionOS renders gaze-hover outside the app process?
8. **T7 passthrough-desktop:** is per-pixel desktop transparency (transparent backdrop, real window
   alpha to passthrough) desired, or is uniform/dynamic monitor opacity enough?
9. **Opaque-mode behavior:** when `blend_mode=opaque` with no hypxrpaper background, should transparency
   be disabled (no-op, since it only dims to black) or left as a dimming cue?

---

## 8. Sources

**Local source / probes (paths relative to repo root):**
- `src/openxr/XRGraphics.cpp` — blit forces `fragColor.a = 1.0` (`:274,466,502`), premultiplied chrome
  fills (`:590-607`), `chrome_col_*` per-frame reads (`:574-576`).
- `src/openxr/OpenXRManager.cpp:1009-1018` — quad build, `TEXTURE_SOURCE_ALPHA` flag, no unpremult bit;
  `:941-951` back-to-front z-sort; `:1069-1074` hover/grab atomics publish; `:1084` blend mode.
- `src/openxr/XRSession.cpp:71-93` — ext enable list (color_scale_bias NOT present; add here).
- `src/openxr/XRMonitorLayer.hpp:33-45` (frame-thread refcount rule), `:149-150` (`m_hoverRegion`/
  `m_grabbedNow` atomics), `:156-161` (fade envelope state).
- `src/openxr/XRIpc.cpp:134,162` — `layout`/`status`, verb dispatch.
- `src/openxr/XRMonitorConfig.hpp:43,63,89` — `pickBlendMode`, `serializeXRMonitorLine`, `parseXRMonitorLine`.
- `src/config/values/ConfigValues.cpp:709-786` — openxr config surface.
- Monado (vendored, pin c2ddab59): `shaders/layer_quad.frag:26-27` (color_scale/bias math),
  `layer_cylinder.frag:29`, `layer_equirect2.frag:114`, `layer_projection.frag:28`;
  `render/render_gfx.c:411,560,766-767` (premultiplied vs unpremultiplied blend factors);
  `util/comp_render_gfx.c:45-46,236,839-858` (premultiplied_alphas per layer);
  `util/comp_render_helpers.h:112` (`is_layer_unpremultiplied`); `render/render_interface.h:912-961`
  (color_scale/color_bias in layer data).
- WiVRn probes: `/usr/lib/wivrn/libopenxr_wivrn.so` — `XR_KHR_composition_layer_color_scale_bias`,
  `XR_TYPE_COMPOSITION_LAYER_COLOR_SCALE_BIAS_KHR`, `XR_TYPE_COMPOSITION_LAYER_ALPHA_BLEND_FB` present.
  `/usr/bin/wivrn-server` — `wivrn::compositor`, `Dispatching compositor_layer_sync`, `system compositor`,
  `comp_swapchain views alpha layer`, `alpha_width`, nvenc/vaapi/x264/av1 encoder strings (server-side
  composition + encode confirmed).

**Spec / web:**
- XrCompositionLayerColorScaleBiasKHR — https://registry.khronos.org/OpenXR/specs/1.0/man/html/XrCompositionLayerColorScaleBiasKHR.html
- OpenXR 1.1 spec (layer composition order, blend modes, premultiplied alpha) — https://registry.khronos.org/OpenXR/specs/1.1/html/xrspec.html
- WiVRn (server-side composition, per-eye encode + shared alpha channel) — https://github.com/WiVRn/WiVRn ; config/architecture — https://github.com/WiVRn/WiVRn/blob/master/docs/configuration.md
- WiVRn latest release notes (streaming architecture) — https://www.gamingonlinux.com/2026/02/wireless-vr-streaming-levels-up-on-linux-with-the-latest-wivrn-release/
- Meta MR design — key considerations (avoid head-locked HUD, anchor/smooth follow) — https://developers.meta.com/horizon/design/mr-design-guideline/
- Meta typography (≥14 px min, ≥18 px comfortable; passthrough contrast interaction) — https://developers.meta.com/horizon/design/styles_typography/
- Meta display / contrast (UI over passthrough legibility) — https://developers.meta.com/horizon/design/display/
- Meta accessibility (WCAG 2.1 contrast minimums) — https://developers.meta.com/horizon/design/accessibility/
