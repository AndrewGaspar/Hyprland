# 07 — Premium XR Window Chrome (research, no implementation)

Status: research only. No code changed, no live runs. Author: research pass 2026-07-08.
Scope: what it would take for HypXRland to support a *premium-feeling* XR window
experience — richer chrome geometry, 3D interaction effects (tilt/glint/shadow/spring),
low-power vs high-power modes — while offloading as much *presentation* as possible OUT of
HypXRland so distributions (Omarchy, CachyOS, Hyprland+Noctalia) can ship and switch their
own XR chrome themes without patching the compositor.

Cross-refs: `04-grabbable-borders.md` (§1 current chrome, §5.5/§9 what shipped),
`01-vr-app-composition.md` (overlay/companion pattern), `02-3d-environments.md`
(equirect2/cylinder), `03-monitor-grids.md` (anchor transforms). Source of truth for what
exists today: `src/openxr/XRGraphics.cpp` `drawChrome`, `src/openxr/OpenXRManager.cpp`
frame-submit loop (`:929-1093`), `src/openxr/XRAnchor.{hpp,cpp}`, `src/openxr/XRIpc.cpp`.

---

## TL;DR (read this)

1. **Tilt/scale/bob/spring motion is already free** — quads are individually posed every
   frame in `OpenXRManager.cpp:1009-1017`; adding a hover/grab tilt or a settle-spring is a
   quaternion multiply on `quadCenterPose` before submit, zero GPU cost, and reuses the
   existing leash spring (`CXRAnchor` §3). This is the single highest ROI premium effect.
2. **Per-layer dim/tint/glow is free** — chain `XrCompositionLayerColorScaleBiasKHR`
   (advertised by WiVRn/Monado, strings-verified) onto each quad; the runtime does the
   modulation. Enables focus-dimming of background monitors and a hover brightness bump with
   no fragment work.
3. **Richer *in-swapchain* chrome (rounded corners, gradient, sheen sweep, SDF glow) costs
   only fragment ALU over the small margin region** and — critically — rides the quad's
   late-latched pose *exactly*, so it never lags the window. This is where a themeable look
   must live.
4. **The current IPC cannot feed 90 Hz poses to a companion client.** `hyprctl openxr
   status` is a poll snapshot; socket2 emits discrete events (`xrmonitorgrab`, `xrmonitoradded`).
   Therefore anything *rigidly attached* to a moving quad MUST stay in the compositor; a
   companion client can only own *ambient / world-locked / latency-tolerant* elements.
5. **Recommended architecture = "theme engine in compositor + optional ambient companion"**:
   HypXRland renders rigidly-attached chrome from **distro-supplied declarative theme assets**
   (a `theme.toml` + PNG/atlas sprites + bounded motion params, NO arbitrary runtime shaders),
   so distros differentiate *without patching HypXRland*; a separate `hypxrchrome` overlay
   client (hypxrpaper pattern) owns glint particles, contact-shadow blobs, ornaments and
   sound, which tolerate IPC latency.
6. **Distro theming maps 1:1 to Omarchy's symlink-swap**: theme lives at
   `~/.config/omarchy/current/theme/xr-chrome/` (a symlink Omarchy already swaps on
   `theme-set`); discovery via `openxr:chrome_theme` + XDG search; hot-reload piggybacks the
   existing `props_refreshed` swapchain-recreate path.
7. **Power modes = `openxr:chrome_power = low|high|auto`.** `auto` keys off *measured frame
   pacing headroom* + `XR_FB_display_refresh_rate` (the only portable signals);
   **HMD battery is NOT portably queryable through OpenXR** — do not promise battery-auto.
8. **12 work packages (WP-C1…C12)**, 5 doable headless on Monado remote-driver, 4 need a
   live Quest 3 for feel/latency, 3 are the companion app + theming plumbing.

---

## 1. Prior art — what makes XR window chrome feel premium

Legend for "our arch": **QUAD-2D** = drawable into the quad's own swapchain (rides pose
exactly, cheap); **POSE** = per-frame quad pose/size math (free); **CSB** =
`XrCompositionLayerColorScaleBias` per-layer modulation (free); **LAYER** = needs a separate
composition layer (companion client or extra compositor layer; painter-order, no depth);
**PROJ-3D** = needs a true stereo projection layer with real geometry (companion only);
**N/A** = not reproducible on composited layers.

| Product | Premium ingredient | Portable to our arch? |
|---|---|---|
| **visionOS** (Vision Pro) | Glass material windows (regular/thin), specular sheen, vibrancy | QUAD-2D approximation only — real glass is a per-pixel refraction/blur of the *environment behind*, which we cannot sample (no scene texture). A frosted-edge + gradient + sheen sweep in-swapchain reads as "glassy" without true refraction. |
| visionOS | Window **grabber bar** below each window; appears on gaze, expands on hover | QUAD-2D (bar) + POSE (subtle scale on hover). We have the bar (`drawChrome`); premium = rounded pill + hover-expand + sheen. |
| visionOS | Corner resize: cursor hover morphs the corner, window follows with spring | QUAD-2D (corner sprite swap) + POSE (spring, we have `CXRAnchor`). Morph = sprite/SDF state blend. |
| visionOS | **Contact + ambient shadow** windows cast on each other / the room | LAYER (fake): a soft dark quad behind+below the window, composited first. True cast shadow needs scene depth we lack. This is the canonical "fake it with an offset shadow layer" trick. |
| visionOS | Windows **tilt/turn to face you**, settle with spring when moved | POSE — already partly done (leash keeps facing viewer); premium tilt-on-grab is a pose compose. |
| visionOS | Spatial **audio** UI feedback (hover ticks, window open/close) | Companion (sound is not a graphics layer). OpenXR has no audio; a companion process plays spatialized audio via PipeWire. |
| **Meta Horizon OS** system UI / Quest | Curved panels, soft drop shadows, hover highlight glow, haptic pulse on controller | Curve = **cylinder layer** (WiVRn/Monado expose `XR_KHR_composition_layer_cylinder`); glow = QUAD-2D/CSB; haptic = we already fire haptics on grab (`XRInput`). |
| **Immersed / Virtual Desktop** | Curved single/multi-monitor, thin bezel, environment | Cylinder layer for curve; bezel = QUAD-2D margin (we have margins). |
| **OVR Toolkit / XSOverlay** (SteamVR) | Flat/curved desktop panels, hover glow + edge highlight, grab handles, **haptic + click sound** on UI, world-locked or wrist-docked | These are OpenVR `IVROverlay` quads — same model as our quads. Glow/handles = QUAD-2D; curve = cylinder; sound/haptic = companion + existing haptics. Confirms composited-2D is the mainstream approach, not full 3D. |
| **Stardust XR** | Everything is real 3D grabbable geometry, physics, models as UI | PROJ-3D only — full custom renderer. NOT our model (we composite the desktop as flat quads). Informs the *companion* ceiling, not the compositor. |
| **wlx-overlay-s / WayVR** (Linux) | Wayland surfaces as OpenXR/OpenVR quad overlays, laser pointer, curved option, watch/ornament panel | Same composited-quad model as us; the "watch" ornament = a body/wrist-anchored small quad — directly reproducible as another anchor mode or a companion layer. |

**Cross-product conclusions (both research passes agree):**

1. **Composited quads win on sharpness — stay on them.** SteamVR/Meta pass: rendering the
   desktop as a runtime-composited quad/cylinder keeps text razor-sharp (no double sampling),
   which is *the biggest perceptual quality lever*, and it's exactly what OVR Toolkit,
   XSOverlay, wlx-overlay-s and Virtual Desktop's "sharp mode" (Meta `OVROverlay` cylinder)
   all do. Full-3D self-rendered scenes (Stardust XR / StereoKit) trade text crispness for
   unlimited geometry. We are a compositor emitting quads — we are on the winning side of this
   for the *desktop content*, and we should keep premium chrome from dragging us off it.
2. **Glow / glint / drop-shadow / lit-bevel must be baked into the quad's own texture each
   frame** — the runtime compositor will NOT add them; no surveyed overlay product does rich
   glow/shadow on a bare quad. This is precisely our in-swapchain `drawChrome` path (QUAD-2D):
   the theme engine belongs there. Anything needing true depth/parallax/physics crosses into
   Stardust-style projection-layer territory (→ our companion).
3. **The cheapest premium wins are rendering-independent**: **hover haptics** (XSOverlay's
   single most-cited tactile touch — a global "haptic strength for hovering over buttons"),
   **UI sound** (typing clicks, notification audio), and **spring-animated pose transitions**.
   We already fire haptics and pose quads per frame — these are near-free for us.
4. **visionOS's most "premium" effects are the true-3D ones**: grounding/contact shadows onto
   the real room, real z-depth + occlusion between windows (with subtle modal push-back),
   billboard tilt toward the head, and spatialized UI sound anchored to each control. Of these,
   tilt is free for us (POSE), shadows are faked (LAYER), depth-order we already do, sound is
   the companion.
5. **The "glass sheen" is the highest-leverage single effect** and it's a hybrid: a *flat*
   surface whose shader is fed real-time environment lighting + head pose. We cannot sample the
   real environment, but a head-pose-driven sheen sweep + gradient in-swapchain is a credible,
   cheap approximation.
6. **visionOS renders hover/highlight OUTSIDE the app process** (for gaze privacy) — i.e. the
   *system compositor* owns the chrome look, apps don't. This is direct external validation of
   our "theme engine in the compositor" recommendation (§3): the compositor is the right owner
   of welded chrome; the app/distro supplies data, not per-frame draws.

Almost none of the premium feel requires per-pixel 3D in the compositor — it is pose math,
per-layer color modulation, cheap in-swapchain fragment work, one cylinder layer type, and a
companion for shadows/particles/sound. Very favorable for our architecture. Per-claim source
URLs in §9.

---

## 2. Capability matrix — what our stack can and cannot do (verified)

Runtime probed: WiVRn 26.6.1 (`/usr/lib/wivrn/libopenxr_wivrn.so`, Monado-derived) and the
vendored Monado (`subprojects/monado`, pin c2ddab59). Extension names below are
strings-present in the WiVRn client lib; **live-advertise on WiVRn is verified only where
noted** (MEMORY: overlay/equirect2 strings-present but historically untested live on WiVRn —
same caveat applies to cylinder/color_scale_bias; the reliable target is vendored Monado,
where these are the standard compositor path).

| Capability | Verdict | Evidence |
|---|---|---|
| **Per-frame per-quad pose** (tilt, bob, face-user, spring) | YES, free | `OpenXRManager.cpp:1009-1017` builds one `XrCompositionLayerQuad` per layer from a solved pose; a pre-submit rotate/scale is pure math. `CXRAnchor` already runs a leash spring (`XRAnchor.hpp` `m_springPos/Vel/smoothedRot`). |
| **Per-quad scale/size animation** (select pulse, hover grow) | YES, free | `quad.size = {quadW, quadH}` set per frame (`:1017`). Animate a scalar. |
| **Per-layer color scale + bias** (dim background, hover brighten, fade, tint) | YES (add ext) | `XrCompositionLayerColorScaleBiasKHR` chained on `next`; `colorScale`/`colorBias` are `XrColor4f`. Strings-verified in WiVRn lib (`XR_KHR_composition_layer_color_scale_bias`, `XR_TYPE_COMPOSITION_LAYER_COLOR_SCALE_BIAS_KHR`). Spec: registry.khronos.org `XrCompositionLayerColorScaleBiasKHR`. **Not currently enabled** — must be added to the `exts` list in `XRSession.cpp:71-83` and chained at submit. |
| **In-swapchain 2D chrome** (rounded corners, gradient, sheen sweep, SDF glow, animated) | YES, cheap | We already draw chrome into the swapchain margin (`drawChrome`, scissor-clear rects). Upgrading to a textured/SDF shader pass touches only margin pixels; the desktop content rect is untouched. Rides the quad's late-latched pose exactly (no lag). |
| **Curved monitor** (cylinder) | YES (add ext), untested-live-on-WiVRn | `XR_KHR_composition_layer_cylinder` + `XrCompositionLayerCylinderKHR` strings-present. Monado compositor renders it (`comp_render_*`). Would replace `XrCompositionLayerQuad` with `XrCompositionLayerCylinderKHR` for opted-in monitors. Note the Monado internal string `Call to xrt_comp_layer_cylinder failed` guards array-index validity, not support. |
| **Panorama / dome background** (equirect2) | YES (used by hypxrpaper) | `XR_KHR_composition_layer_equirect2`; already the basis of `hypxrpaper` panorama mode. |
| **Per-pixel depth between layers / true occlusion** | NO | OpenXR composites layers in **submission order** (painter's algorithm). We already sort by distance each frame (`:939-951`). `XR_KHR_composition_layer_depth` only feeds *reprojection*, NOT inter-layer occlusion. So a companion's glints/shadows cannot per-pixel occlude/be-occluded by a quad — must be faked by ordering + offset. |
| **True lighting / cast shadows** | NO (fake only) | No scene, no shadow pass. visionOS-style shadow is faked: a soft dark quad placed behind+below, composited before the window (LAYER). |
| **Glass refraction** (sample environment behind window) | NO | We never render the environment to a texture we can sample; passthrough/equirect are composited by the runtime, not available to our fragment shader. Frosted-edge + gradient + sheen approximate it. |
| **Overlay session (companion over the desktop)** | YES (verified alone + w/ primary on Monado) | `XR_EXTX_overlay`, `sessionLayersPlacement` z-order (`XRSession.cpp:179-185`, monado `oxr_session.c:1449-1453`). hypxrpaper verified as primary; a chrome companion would be a *second* overlay client with higher z. |
| **Layer count budget** | 16 guaranteed, up to 128 on Monado/Linux | `XRSession.cpp:127-129` clamps `maxLayerCount` to ≥16 from `graphicsProperties.maxLayerCount`. Monado Linux `XRT_MAX_LAYERS=128` (`subprojects/monado/.../xrt_limits.h:86`); WiVRn may report fewer. Each overlay client has its own per-session budget. |
| **Concurrent overlay clients** | up to 64 | Monado `MULTI_MAX_CLIENTS=64` (`comp_multi_private.h:33`). Practical limit: one `monado-service`/WiVRn server per box (MEMORY) — companion + compositor + hypxrpaper all share it. |
| **Hand pinch pose** (precise pinch-driven affordances) | YES on Quest+WiVRn | `XR_EXT_hand_interaction` (`ext/hand_interaction_ext` profile strings). Already wired for grab (WP-G5). |
| **Custom haptic waveforms** (rich haptic-visual language) | YES on Quest+WiVRn | `XR_FB_haptic_pcm` strings-present; today we fire simple haptic ticks on grab (`XRInput`). |
| **Display refresh rate signal** (power mode input) | YES | `XR_FB_display_refresh_rate` + `xrEnumerate/RequestDisplayRefreshRateFB`. Query current Hz as a coarse power-state proxy. |
| **HMD battery / thermal signal** (power-mode auto) | **NO (portably)** | OpenXR core has none. `XR_EXT_interaction_profile_battery_state_display` is *controller/interaction-profile* battery for display only; Monado string `get_battery_status is not implemented` / `XR_TYPE_BATTERY_STATE_DISPLAY_EXT` confirms it's device-plumbing, not a reliable HMD-battery query. `XR_ANDROID_performance_metrics` is Android-only. **Do not promise battery-driven auto.** |
| **Frame-pacing headroom signal** (power-mode auto) | YES (self-measured) | `xrWaitFrame`→`predictedDisplayPeriod` + our own frame CPU/GPU timing; detect skipped `predictedDisplayTime` deltas. This is the honest "am I keeping up?" signal. |

### 2.1 Frame-budget cost model (compositor frame thread)

- **Pose tilt/scale/bob**: ~1 quaternion mul + vector adds per quad. Negligible (µs). FREE.
- **color_scale_bias**: fill one struct per quad, chain on `next`. Zero GPU cost on our side
  (runtime modulates). FREE.
- **In-swapchain chrome upgrade** (SDF/gradient/sheen shader over the margin): fragment work
  is bounded by margin pixel count. For a 1920×1080 content quad with a `chrome_margin` of a
  few cm the margin band is a small single-digit % of the swapchain area; a handful of extra
  ALU ops there is <<1 ms even on the AMD iGPU path. Already gated by the fade envelope (only
  redraws when alpha/region/grab changed — `XRMonitorLayer.hpp:155-161`).
- **Cylinder** vs quad: same submit cost to us; the runtime's compositor does slightly more
  work. Opt-in per monitor.
- **Everything expensive** (particle glints, soft-shadow blur, real 3D ornaments) belongs in
  the **companion**, which has its OWN session + pacing + GPU budget and never touches our
  frame thread.

---

## 3. Architecture recommendation

### 3.1 The hard constraint that decides the split

There is **no per-frame pose feed** out of HypXRland today. `hyprctl openxr status`
(`XRIpc.cpp:16-93`) is a poll snapshot of `CXRAnchor::lastWorld()`; socket2 emits discrete
events only (`OpenXRManager.cpp:152,160,466-467,1301-1302,1382-1383`:
`openxrsessionstate`, `openxractive`, `xrmonitorgrab`, `xrmonitoradded/removed`). Even if we
added a pose-stream socket, a companion reading it and re-submitting its own layer would be
≥1 IPC round-trip behind — visibly wrong for anything meant to be *welded* to a moving
window at 90 Hz, and impossible to match the runtime's **late-latching** of grip-space quads
during a grab (`OpenXRManager.cpp:981-992`).

**Therefore:** rigidly-attached, per-frame-synced chrome (the bar, borders, corner handles,
tilt, hover glow, the window's own drop-shadow if welded) MUST be rendered by the compositor.
A companion can only safely own **world-locked or slowly-moving, latency-tolerant** elements.

This directly shapes how the "offload presentation so distros can theme it" value is met: we
cannot offload the *rendering* of welded chrome, but we CAN offload the *look* by making the
compositor a **theme engine driven by distro-supplied declarative assets**, plus a genuinely
separate companion for ambient effects.

### 3.2 The three candidates, evaluated on "how does a distro ship & switch a theme"

**(a) Status-quo++ : richer in-swapchain chrome, themed by declarative assets.**
HypXRland keeps drawing chrome into the quad swapchain, but `drawChrome` is upgraded from
scissor-fills to a small textured/SDF pass whose *appearance is fully data-driven* by a theme
bundle (colors, corner radius, bar sprite, glow sprite/params, motion tuning). No compositor
patching to restyle.
- Distro ship/switch: **excellent.** A theme is a directory of assets + one TOML. Omarchy's
  `theme-set` already swaps `~/.config/omarchy/current/theme` by symlink; put the XR bundle at
  `…/current/theme/xr-chrome/` and it switches for free. Hot-reload via existing config path.
- Ceiling: 2D-in-margin (rounded corners, gradients, sheen sweep, SDF glow, hover/grab state
  sprites). Rides pose exactly. No particles/shadows/sound.
- Risk: expressiveness is bounded by what the fixed shader interprets. Arbitrary distro GLSL
  is **not recommended** (runtime shader compilation on the frame thread = stability +
  security surface; a malformed shader crashes the XR frame loop). Bounded params + textures
  give 90% of the look with none of the risk.

**(b) Chrome entirely as a separate overlay-client app (`hypxrchrome`).**
A companion subscribes to socket2 XR events + reads geometry, and draws bar/handles/glints as
its own composition layers.
- Distro ship/switch: **excellent in principle** (swap the whole binary/theme), BUT
- Fatal latency problem for welded chrome (§3.1): its bar/handles would lag the window and
  can't match grip-space late-latch during grabs. Only viable for world-locked ambient
  elements. **Rejected as the primary chrome owner.**

**(c) Hybrid (RECOMMENDED).**
- **Compositor (HypXRland)** owns and renders, from a theme bundle: the welded chrome
  (bar/handles/border), tilt/scale/bob/spring motion, hover glow, per-layer
  color_scale_bias dim/brighten, optional cylinder curvature. It is a *theme engine*, not a
  fixed look.
- **Companion (`hypxrchrome`, optional)** owns: contact-shadow blobs (offset dark quads),
  glint/particle bursts on hover (its own layers), ambient ornaments/"watch" panels, and
  **sound** (PipeWire spatial audio) + optional rich haptics. All of these are either
  world/body-locked or explicitly latency-tolerant, and degrade gracefully if the companion
  is absent.
- Distro ship/switch: a distro ships an `xr-chrome/` theme bundle (styles the welded chrome)
  AND optionally its own companion or a companion theme. Both switch via the same
  symlink-swap. **This is the recommendation.**

### 3.3 The contract HypXRland must expose (what stays in the compositor)

1. **Hit-testing & region classification** — `OpenXR::classifyQuadHit` (already exists,
   `XRMath.hpp`); the geometry contract (`SXRChromeGeometry`, `XRMonitorConfig.hpp`) is the
   single source of truth for bar/corner/body regions.
2. **Grab semantics** — begin/carry/release, resize, release-latch ring (`CXRAnchor`, WP-G1…G6).
3. **Per-frame pose animation hooks** — a new `SXRChromeMotion` applied to `quadCenterPose`
   and `quad.size` just before submit (`OpenXRManager.cpp:1007-1017`): tilt-on-grab,
   hover-scale, select-pulse, release-spring. Reuses `CXRAnchor` spring math.
4. **The theming surface** — a loaded `SXRChromeTheme` (colors, radii, sprites, glow, motion
   tuning, power-tier gates) that `drawChrome` and the motion hook read. Loaded from the
   theme bundle; hot-reloadable.
5. **A geometry/event feed for the companion** — a socket2 event stream + a queryable
   layout (extend `hyprctl openxr layout`/`status`) giving per-monitor world pose, size,
   grab/hover state, at event granularity (NOT per-frame). Enough for world-locked shadows,
   ambient glints, and sound triggers.

### 3.4 Theming file format + discovery + hot-reload (concrete)

**Bundle layout** (a directory, so distros ship art + config together):

```
xr-chrome/
  theme.toml          # the declarative theme (see below)
  bar.png             # optional 9-slice or atlas sprite for the move-bar
  corner.png          # optional corner-handle sprite (idle/hover/grab strip)
  glow.png            # optional radial glow sprite for hover/border
  README
```

**`theme.toml`** (sketch — parsed by the compositor into `SXRChromeTheme`):

```toml
[meta]
name = "Omarchy Tokyo Night XR"
version = 1

[color]                     # ARGB hex, premultiplied at load like the current col_* vars
idle  = "0x66aaaaaa"
hover = "0xcc66aaff"
grab  = "0xff66aaff"
focus_dim = 0.6             # color_scale applied to NON-focused monitors (0..1)

[shape]
bar_style = "pill"          # pill | flat | sprite(bar.png)
bar_radius_m = 0.01
corner_style = "rounded"    # square | rounded | sprite(corner.png)
corner_radius_m = 0.008
border = true
border_width_m = 0.004

[effect]                    # each gated by power tier (see §4)
sheen = true                # animated highlight sweep across the bar on hover
sheen_period_ms = 1400
glow = "sdf"                # off | sdf | sprite(glow.png)
glow_radius_m = 0.02

[motion]                    # bounded ranges enforced by the loader
tilt_on_grab_deg = 6.0      # quad pitches toward viewer while grabbed (0..15)
hover_scale = 1.02          # 1.0..1.1
select_pulse = 1.05
spring_response_s = 0.18    # reuses CXRAnchor spring; clamp 0.05..1.0

[power]                     # which effects survive in low mode (see §4)
low_disables = ["sheen", "glow", "tilt_on_grab", "hover_scale"]
```

**Discovery** (new config var `openxr:chrome_theme`, default empty = built-in look):
resolution order when set to a bare name or empty:
1. `openxr:chrome_theme` absolute path, else
2. `$XDG_CONFIG_HOME/hypr/xr-chrome/theme.toml`, else
3. `~/.config/omarchy/current/theme/xr-chrome/theme.toml` (Omarchy symlink — swaps on
   `omarchy theme-set`), else a distro dir (`/usr/share/hypr/xr-chrome/`), else the built-in
   defaults (identical to today's `openxr:chrome_col_*`). *The Omarchy step is a documented
   convention, not a hardcode — the search list is itself configurable; other distros
   (CachyOS, Noctalia) point `openxr:chrome_theme` at their own theme dir.*

**Hot-reload**: the theme bundle is re-read on Hyprland config reload. Colors/motion/effects
that don't change geometry apply on the next frame (they're just cached reads, like today's
`chrome_col_*`). Changes to `chrome_margin`/`bar_height`/`corner_size`-class geometry recreate
swapchains through the **existing** reconcile path (`props_refreshed` → swapchain recreate,
already implemented for chrome geometry). Distro `theme-set` hooks that `hyprctl reload` (or
touch the config) get XR chrome restyled with everything else. **No new hot-reload machinery.**

**Why not user GLSL:** runtime-compiling distro-supplied shaders on the XR frame thread risks
a compile stall / driver crash that takes down the session, and is an code-execution surface.
The declarative TOML + sprite-atlas + a fixed, well-tested SDF/sheen shader gives distros a
rich, swappable look with none of that. (If a future "power user" escape hatch is wanted, it
should run in the *companion*, not the compositor.)

---

## 4. Motion design + power modes

### 4.1 Motion effects and where they hook

| Effect | Where it hooks | Cost | Notes |
|---|---|---|---|
| **Tilt-on-grab** (quad pitches ~6° to face viewer) | pre-submit pose compose, `OpenXRManager.cpp:1007` | free | Compose a small pitch quat onto `quadCenterPose.rot` when `m_grabbedNow`. Ease in/out. |
| **Hover scale** (window grows ~2%) | `quad.size` + pose, `:1017` | free | Scale about content center; ease from `m_hoverRegion != NONE`. |
| **Select pulse** (brief scale bump on click) | `quad.size` | free | One-shot ease triggered by pointer button-down event. |
| **Release spring / settle** | `CXRAnchor` reanchor + spring | free | On grab release, seed the existing leash spring so the window overshoots-and-settles instead of snapping. Reuse `m_springPos/Vel`. |
| **Sheen sweep** on hovered bar | `drawChrome` fragment | ~µs | Animate a highlight band's UV offset by `predictedDisplayTime`; already have per-frame time. |
| **Border/edge glow** on hover/grab | `drawChrome` SDF, or CSB | cheap/free | SDF glow in margin, OR a whole-quad brightness bump via `color_scale_bias`. |
| **Focus dimming** (background monitors dim) | CSB per quad | free | `colorScale < 1` on non-focused monitors; the focused one at 1.0. Big perceived-polish win, zero cost. |
| **Contact shadow** under window | companion LAYER (or an extra compositor quad) | cheap | Soft dark quad behind+below, composited first. Fake, no true lighting. |
| **Glint particles** on hover | companion LAYER | companion budget | Not welded; a small burst is fine slightly delayed. |
| **Sound** (hover/select/grab/open) | companion (PipeWire) | companion | OpenXR has no audio. |
| **Haptics** (rich waveforms) | compositor (`XR_FB_haptic_pcm`) | free | Already fire ticks; upgrade to themed waveforms. |

### 4.2 Power modes

New var `openxr:chrome_power = high | low | auto` (default `high`; `auto` recommended once
the signal is proven).

- **high**: all effects the theme enables — sheen, glow, tilt, hover-scale, select-pulse,
  spring settle, focus dimming, companion glints/shadows/sound.
- **low**: welded chrome only, flat/rounded fill, instant fade, no sheen/glow, no
  tilt/scale/pulse, focus dimming still on (it's free and helps), companion effects paused.
  The theme's `[power].low_disables` list drives exactly what drops.
- **auto**: start `high`; if measured frame pacing shows sustained missed frames (compare
  successive `predictedDisplayTime` deltas vs `predictedDisplayPeriod` from `xrWaitFrame`, or
  a rolling GPU-time estimate) OR `XR_FB_display_refresh_rate` reports a drop to a low tier,
  step down to `low` with hysteresis; step back up when headroom returns. **Battery is not an
  input** (§2 — not portably queryable); document this and let a distro script flip
  `openxr:chrome_power=low` from an external battery watcher if it wants battery behavior.

Frame-budget rationale: the compositor-side high-mode effects are all free/near-free
(§2.1), so `low` mode's real savings come from (a) pausing the **companion** (its separate
GPU cost) and (b) skipping the in-swapchain SDF/sheen fragment pass. The compositor stepping
the companion down is a socket2 event (`openxrchromepower low|high`).

---

## 5. Work-package breakdown (WP-C1 … C12)

One-subagent-sized, ordered by dependency. "Live Quest" = needs a real headset for
feel/latency validation beyond headless Monado remote-driver correctness.

| WP | Title | Summary | Acceptance | Live Quest? |
|---|---|---|---|---|
| **C1** | color_scale_bias plumbing | Enable `XR_KHR_composition_layer_color_scale_bias`; chain `XrCompositionLayerColorScaleBiasKHR` per quad; add `focus_dim`. | Background monitors dim when another is focused; gtest for the struct-build; no regression on runtimes lacking the ext (skip gracefully). | No (headless verifies dim value; feel = No) |
| **C2** | Pre-submit motion hook | Add `SXRChromeMotion` applied to `quadCenterPose`/`size` before submit: tilt-on-grab, hover-scale, select-pulse, release-spring (reuse `CXRAnchor` spring). | Pure-math unit tests (like `anchor_math.cpp`); visual: window tilts on grab, settles with spring. | Yes (feel) |
| **C3** | Theme model + loader | `SXRChromeTheme` struct + `theme.toml` parser + discovery search (§3.4) + validation/clamping; default = today's look. | gtest: parse a sample TOML, clamp out-of-range motion, fall back to defaults on missing file. | No |
| **C4** | Themed `drawChrome` v2 | Replace scissor-fills with an SDF/atlas pass: rounded corners, pill bar, gradient, border; driven by `SXRChromeTheme`; sprite-atlas support (bar/corner/glow PNG). | Visual parity with theme; margin-only draw preserved; content rect untouched; fade envelope still gates redraw. | No (correctness); Yes (look on device) |
| **C5** | Sheen + glow effects | Animated sheen sweep on hovered bar; SDF glow on hover/grab; gated by power tier. | Sheen animates from `predictedDisplayTime`; disabled in `low`. | Yes (look) |
| **C6** | Power modes | `openxr:chrome_power=high/low/auto`; frame-pacing headroom estimator + `XR_FB_display_refresh_rate` read; hysteresis; `[power].low_disables`. | Headless: forced `low` disables the right effects; auto steps down under synthetic frame stalls. | Yes (auto threshold tuning) |
| **C7** | Cylinder monitors (opt-in) | Per-monitor `openxr:...curve` → submit `XrCompositionLayerCylinderKHR` instead of quad; hit-test against the cylinder arc. | Curved monitor renders; ray hit-test matches curvature; falls back to quad if ext absent. | Yes (WiVRn live-advertise unverified) |
| **C8** | Themed haptics | `XR_FB_haptic_pcm` waveforms for hover/grab/release/resize from the theme. | Distinct waveforms fire; graceful fallback to simple pulse. | Yes (haptics need device) |
| **C9** | Companion IPC feed | Extend socket2 + `hyprctl openxr layout` with an event-granular geometry/state feed (pose, size, grab/hover, focus, `chromepower`) for a companion. | A test consumer receives events on grab/move/focus; documented schema. | No |
| **C10** | `hypxrchrome` companion skeleton | New repo (hypxrpaper pattern): overlay OpenXR client, subscribes to C9 feed, renders contact-shadow blobs + focus vignette as world-locked layers. | Shadows appear under monitors, track on move (event granularity), degrade if absent. | Yes (compositing order w/ primary) |
| **C11** | Companion glints + sound | Hover glint particle bursts (companion layers) + PipeWire spatial UI sounds (hover/select/grab/open). | Glints on hover, sounds on events; both pausable via `chromepower`. | Yes |
| **C12** | Omarchy theme bundle + docs | Ship a reference `xr-chrome/` bundle; wiki notes; Omarchy `theme-set` integration doc; `example/openxr.conf` theme stanza. | A `theme-set` swaps the XR look; docs cover CachyOS/Noctalia discovery. | No |

Critical path: C1→C2→C3→C4 (core themeable welded chrome). C5/C6/C7/C8 parallelize after C4.
C9→C10→C11 (companion track) parallelize after C9. C12 last.

Headless-testable (Monado remote driver): C1, C3, C6 (synthetic stalls), C9, plus the
pure-math parts of C2/C4. Need live Quest for feel/latency/haptics/curve: C2, C5, C7, C8,
C10, C11.

---

## 6. Open questions for the user (taste + scope decisions)

1. **Bar style**: pill (visionOS-like) vs flat vs distro-sprite as the *default*? (Theme can
   override; what ships built-in?)
2. **Tilt magnitude & when**: tilt on grab only, or also a subtle constant tilt-to-face for
   off-axis monitors? Default degrees (proposed 6°, range 0–15)?
3. **Focus dimming**: on by default? How aggressive (`focus_dim` default 0.6)? Some users
   will hate background dimming.
4. **Sound**: ship UI sounds at all? If yes, in the companion only (opt-in), and does a
   default sound set ship or is it distro-supplied?
5. **Companion scope**: do we build `hypxrchrome` now (C10–C11) or ship compositor-only
   premium (C1–C8) first and leave the companion as a documented extension point?
6. **Curved monitors** (C7): worth it given WiVRn live-advertise is unverified? Or defer
   until confirmed on the Quest?
7. **Theme escape hatch**: strictly declarative TOML+sprites (recommended), or do we ever
   want a companion-side shader escape hatch for power users?
8. **Power `auto`**: acceptable that battery is NOT an input (frame-pacing + refresh-rate
   only)? Should we document the external-script hook for distros that want battery behavior?
9. **Theme discovery**: is the Omarchy `~/.config/omarchy/current/theme/xr-chrome/` step an
   acceptable *documented default* in the search list, or must all distro paths be purely
   config-driven with no built-in convention?

---

## 7. Summary of the recommendation

Build **premium chrome as a theme engine in the compositor + an optional ambient companion**.
The compositor keeps ownership of everything welded to a moving window (bar, handles, border,
tilt, hover glow, spring, per-layer dim) because there is no low-latency per-frame pose feed
to a companion — but it renders that chrome from **distro-supplied declarative theme bundles**
(`theme.toml` + sprites + bounded motion params), so Omarchy/CachyOS/Noctalia ship and swap a
complete XR look via the same symlink-swap + `theme-set` flow they already use, with zero
HypXRland patching and hot-reload for free on the existing config path. A separate
`hypxrchrome` overlay client (hypxrpaper pattern) owns latency-tolerant extras — contact
shadows, glint particles, ornaments, spatial sound — and is itself swappable per distro.
Highest-ROI effects: tilt/spring motion (free), focus-dimming via color_scale_bias (free),
themed in-swapchain sheen/glow (cheap). Power modes key off frame-pacing headroom +
`XR_FB_display_refresh_rate`; battery is not portably queryable and is left to an external
distro hook. 12 WPs, ~5 headless-testable, ~6 needing a live Quest.

---

## 8. Sources

Spec / registry:
- XrCompositionLayerColorScaleBiasKHR — https://registry.khronos.org/OpenXR/specs/1.0/man/html/XrCompositionLayerColorScaleBiasKHR.html
- XrCompositionLayerBaseHeader (next-chain composition) — https://registry.khronos.org/OpenXR/specs/1.1/man/html/XrCompositionLayerBaseHeader.html
- XR_FB_display_refresh_rate — https://registry.khronos.org/OpenXR/specs/1.0/man/html/XR_FB_display_refresh_rate.html
- xrRequestDisplayRefreshRateFB — https://registry.khronos.org/OpenXR/specs/1.0/man/html/xrRequestDisplayRefreshRateFB.html
- XR_KHR_composition_layer_cylinder — https://registry.khronos.org/OpenXR/specs/1.0/man/html/XR_KHR_composition_layer_cylinder.html (registry index)
- OpenXR 1.1 full spec (layer composition order, blend modes) — https://registry.khronos.org/OpenXR/specs/1.1/html/xrspec.html
- Battery peripherals thread (confirms no general HMD battery query) — https://community.khronos.org/t/how-to-get-battery-status-for-peripherals/110652

Local probes / source (paths relative to repo root):
- `src/openxr/XRGraphics.cpp` `drawChrome`/`snapshotSwapchain` (in-swapchain chrome)
- `src/openxr/OpenXRManager.cpp:929-1093` (per-frame quad build + submit + z-sort)
- `src/openxr/XRSession.cpp:55-130,179-198` (extension enable, maxLayerCount, overlay)
- `src/openxr/XRAnchor.hpp` (leash spring, grab, release-latch ring)
- `src/openxr/XRIpc.cpp` (status/layout IPC — poll/event, no per-frame feed)
- `src/openxr/XRMonitorLayer.hpp` (frame-thread threading rules, chrome state contract)
- `src/config/values/ConfigValues.cpp:709-786` (openxr config surface, chrome_* vars)
- WiVRn strings: `/usr/lib/wivrn/libopenxr_wivrn.so` (color_scale_bias, cylinder, depth,
  equirect2, overlay, hand_interaction, FB_haptic_pcm, FB_display_refresh_rate present)
- Monado caps: `subprojects/monado/src/xrt/compositor/multi/comp_multi_private.h:33`
  (MULTI_MAX_CLIENTS=64), `.../include/xrt/xrt_limits.h:86` (XRT_MAX_LAYERS=128 Linux),
  `.../state_trackers/oxr/oxr_session.c:1449-1453` (overlay z-order)

Prior-art product sources: see §9 (populated from the two prior-art research passes).

## 9. Prior-art source URLs

**visionOS (Apple):**
- Apple Support — Move/resize/close windows (window bar, close, resize corners): https://support.apple.com/guide/apple-vision-pro/move-resize-and-close-app-windows-dev009366408/visionos
- HIG — Materials (glass, vibrancy): https://developer.apple.com/design/human-interface-guidelines/materials
- HIG — Ornaments (tab bars / floating controls): https://developer.apple.com/design/human-interface-guidelines/ornaments
- WWDC23 10072 — Design for spatial user interfaces (bar, face-user tilt, shadows, modal push-back, spatial audio): https://developer.apple.com/videos/play/wwdc2023/10072/
- WWDC23 10110 — Elevate your windowed app for spatial computing (glass default, ornaments, tab bar): https://developer.apple.com/videos/play/wwdc2023/10110/
- WWDC25 303 — Design hover interactions (instant/delayed/ramp; hover rendered outside app process): https://developer.apple.com/videos/play/wwdc2025/303/
- Apple Newsroom — Liquid Glass / real-time speculars "born from visionOS": https://www.apple.com/newsroom/2025/06/apple-introduces-a-delightful-and-elegant-new-software-design/
- createwithswift — Legibility & materials/vibrancy: https://www.createwithswift.com/ensuring-interface-legibility-and-contrast-in-visionos/
- createwithswift — Ornaments: https://www.createwithswift.com/creating-ornaments-in-visionos/
- Step Into Vision — Animation (spring/damping bridged to RealityKit): https://stepinto.vision/articles/deep-dive-into-animation-on-visionos/
- Unity PolySpatial — Grounding shadows (only downward grounding shadow, no depth-map shadows): https://docs.unity3d.com/Packages/com.unity.polyspatial.visionos@1.1/manual/GroundingShadow.html
- USPTO 12561042 — spatialized UI feedback sounds: https://image-ppubs.uspto.gov/dirsearch-public/print/downloadPdf/12561042
- Cult of Mac — spatial UI ("subtly glow and shine" on gaze): https://cultofmac.com/819590/apple-explains-how-spatial-user-interfaces-work-in-visionos

**SteamVR / Meta / Linux overlays:**
- OVR Toolkit (Steam): https://store.steampowered.com/app/1068820/OVR_Toolkit__Desktop_Overlay/
- XSOverlay (Steam): https://store.steampowered.com/app/1173510/XSOverlay/ ; settings/haptics docs: https://xsoverlay.vercel.app/settings
- Immersed: https://immersed.com/
- Virtual Desktop ultrawide/curved (Meta cylinder layer): https://www.uploadvr.com/quest-3-windows-11-remote-desktop-ultrawide-mode/
- wlx-overlay-s (GitHub, Vulkan WlxGraphics, OpenXR+OpenVR quads, laser colors, watch): https://github.com/galister/wlx-overlay-s
- WayVR / WayVR Dashboard: https://github.com/wayvr-org/wayvr ; https://wayvr.org/
- Stardust XR (full-3D StereoKit projection layer, Wayland-surface-as-3D-panel): https://stardustxr.org/ ; server: https://github.com/StardustXR/server ; Khronos write-up: https://www.khronos.org/developers/linkto/what-monitor-stardust-and-building-a-mobile-xr-desktop-pc
- OpenVR `IVROverlay::SetOverlayCurvature`/`SetOverlayTexture` (curvature = one float; app supplies texture): https://github.com/ValveSoftware/openvr/wiki/IVROverlay::SetOverlayTexture ; overview: https://github.com/ValveSoftware/openvr/wiki/IVROverlay_Overview
- Meta compositor layers (cylinder layer + sharper text rationale): https://developers.meta.com/horizon/documentation/unity/unity-ovroverlay/
