# Research: 3D Environments / "Wallpapers" behind XR monitors

Research doc, HypXRland. Status: **research only** — no code written, no runtimes
launched. Question: what would it take to render a *scenic environment* (skybox,
vista, room) behind the virtual-monitor quads, ideally via a **pluggable** mechanism
like the way Hyprland delegates wallpapers to hyprpaper (the compositor ships no
wallpaper code; an external client draws the background over `wlr-layer-shell`)?

Companion to the passthrough / environment-blend-mode work (a "none / see-through"
mode) and to the sibling research task on compositing *other VR applications* under
us — §Option C keeps hooks compatible with both.

---

## TL;DR

1. **Cheapest, works today: submit one `XrCompositionLayerEquirect2` (a 360° HDRI
   panorama) or `XrCompositionLayerCylinder` as the bottom layer of our existing
   `xrEndFrame` array.** Upload once, zero per-frame GL, runtime does the spherical
   projection. The pinned Monado tree renders **equirect2 + cylinder end-to-end**
   (evidence below); **cube and equirect1 are NOT rendered** (build-off *and* no
   renderer case). This is a **Small–Medium** change and the recommended v1.
2. A **self-rendered projection-layer skybox / glTF scene** (Option B) gives true
   parallax and arbitrary geometry but forces us to take on per-eye stereo rendering
   in the frame thread — the explicit **non-goal** of doc 00. Large, defer.
3. A **pluggable external "hyprxrpaper" daemon** (Option C) is the honest analog of
   hyprpaper, but there is no `wlr-layer-shell` equivalent in XR: the clean version
   depends on multi-client/overlay compositing (the sibling task). A dmabuf-handoff
   variant (C-b) is buildable now and reuses our monitor-blit path.
4. Content for v1 is a solved problem: **Poly Haven / HDRI-Haven equirectangular
   `.hdr`/`.exr`** panoramas, loaded once, tonemapped to the swapchain.
5. Recommended: ship **Option A** (`openxr:environment = /path/to/pano.hdr` +
   rotation/brightness), gate it behind the blend-mode "opaque" path, and keep the
   config/IPC surface forward-compatible with C so a daemon can later take over.

---

## Background: where a background layer plugs in

Our frame loop assembles a `std::vector<XrCompositionLayerQuad>` and a parallel
`std::vector<const XrCompositionLayerBaseHeader*> layerPtrs`, sorts quads
far-to-near, and submits them in `xrEndFrame` with
`environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE`
(`src/openxr/OpenXRManager.cpp:769-896`). OpenXR composites the layer array in
**submission order** — element 0 is drawn first, i.e. **behind** everything else.

So "a background" is mechanically trivial: build one more layer struct and
`push_back` its header pointer at **index 0** of `layerPtrs`, before the quad
pointers. Everything below is about *what* that layer is, who renders it, and
whether the runtime will show it.

Two hard constraints shape every option:

- **Blend mode.** A background only makes sense in `OPAQUE` blend. In the parallel
  passthrough work (`ALPHA_BLEND` / `ADDITIVE`, "see the real room"), the
  environment must be **suppressed** — they are mutually exclusive UX. The config
  surface (§Config) must express `environment` and `passthrough` as one
  either/or "what's behind the monitors" setting.
- **Non-goal in doc 00.** Doc 00 explicitly says "No stereo 3D compositor… each
  monitor is a flat quad composited by the XR runtime" and "No per-eye parallax."
  Option A **honors** this (the runtime does the projection; we still submit only
  runtime-composited layers). Option B **breaks** it (we'd render per-eye) — that's
  the main reason B is deferred, not merely effort.

### The hyprpaper analogy — and why XR breaks it

hyprpaper is a standalone daemon: it reads its own config, loads an image with
hyprgraphics, and draws it onto a **`wlr-layer-shell`** `background` surface; the
compositor contributes *zero* wallpaper code and just composites that layer surface
at the bottom of the scene
([hyprpaper wiki](https://wiki.hypr.land/Hypr-Ecosystem/hyprpaper/),
[repo](https://github.com/hyprwm/hyprpaper)). The pluggability comes entirely from a
**standard protocol** (`layer-shell`) that lets an unprivileged client claim the
"behind everything" slot.

XR has **no such protocol inside a single OpenXR session**. An OpenXR *session* is
single-application; the layer array is ours alone. A second process cannot "add a
layer" to our session. To reproduce hyprpaper's isolation you need one of:
(a) the *runtime* to composite multiple clients (Monado's multi-client compositor /
overlay sessions — the sibling task), or (b) an out-of-band buffer handoff into
*our* session (our existing dmabuf blit path, aimed at a background layer). This is
the crux of Option C.

---

## Runtime support matrix — evidence (pinned Monado `c2ddab59d`)

I checked the vendored tree end-to-end: extension advertisement → oxr layer
verification → **actual renderer**. A layer type is only usable if all three exist.

| Layer type | Extension advertised? | Built by default? | Renderer draws it? | **Usable now?** |
|---|---|---|---|---|
| **`equirect2`** (`XR_KHR_composition_layer_equirect2`) | yes | **ON** | **yes** | **YES** |
| **`cylinder`** (`XR_KHR_composition_layer_cylinder`) | yes | **ON** | **yes** | **YES** |
| `equirect` / equirect1 | yes | **OFF** | **no case** | no |
| `cube` (`XR_KHR_composition_layer_cube`) | yes | **OFF** | **no case** | no |
| `quad`, `projection` | yes | ON | yes | yes (what we use) |

Evidence:

- Extension→feature map: `subprojects/monado/src/xrt/state_trackers/oxr/extension_support/oxr_extension_support.py:48-52`
  (cube, cylinder, depth, equirect1, equirect2).
- **Build defaults:** `subprojects/monado/CMakeLists.txt:430-433` —
  `XRT_FEATURE_OPENXR_LAYER_CUBE … OFF`, `…_CYLINDER … ON`, `…_EQUIRECT2 … ON`,
  `…_EQUIRECT1 … OFF`.
- **As actually built** in the vendored tree:
  `subprojects/monado/build/src/xrt/include/xrt/xrt_config_build.h:73-77` —
  `CUBE` and `EQUIRECT1` are `#undef`; `CYLINDER` and `EQUIRECT2` are `#define`.
- **Renderer (graphics path)** `subprojects/monado/src/xrt/compositor/util/comp_render_gfx.c:763-806`:
  `switch(data->type)` has cases only for `CYLINDER`, `EQUIRECT2`,
  `PROJECTION[_DEPTH]`, `QUAD`, then `default: break;` — **cube and equirect1 are
  silently dropped even if you compiled the extension in.**
- **Renderer (compute path)** `subprojects/monado/src/xrt/compositor/util/comp_render_cs.c:64-67,602-664`:
  same set (`QUAD`, `CYLINDER`, `EQUIRECT2`, `PROJECTION`). Confirmed via grep:
  `XRT_LAYER_CUBE` / `XRT_LAYER_EQUIRECT1` appear in **neither** renderer.
- oxr acceptance is `#ifdef`-gated on the feature:
  `oxr_session_frame_end.c:752` (`verify_cube_layer`, `#ifndef OXR_HAVE_KHR_composition_layer_cube` → error),
  `:930` (equirect1), `:1028` (equirect2). So with the shipped flags, submitting a
  cube/equirect1 layer is **rejected at `xrEndFrame`**; equirect2/cylinder pass.

**Null compositor (our headless test rig).** `null_compositor.c:453`
(`null_compositor_layer_commit`) drives `comp_base`'s `layer_accum`
(`c->base.layer_accum`); the generic `comp_base` layer entrypoints accept whatever
features are compiled, and the null compositor **displays nothing**. Consequence for
the XR test suite: a headless test can assert that an equirect2/cylinder background
layer is **accepted** (no `xrEndFrame` error, session stays FOCUSED), but **cannot**
assert it renders correctly — visual correctness needs a real compositor or a human
in a headset.

**WiVRn (Quest, wireless).** WiVRn is a Monado **fork** with its own streaming
compositor that flattens the layer array server-side (Monado's `comp_render`
lineage) and encodes/streams the result; it recently reworked that compositor
([WiVRn #865](https://github.com/WiVRn/WiVRn/issues/865)). It therefore inherits
Monado's layer set — **quad, projection, cylinder, equirect2** — but the exact build
flags are WiVRn's, version-sensitive, and I could not confirm equirect2 is enabled
in a given WiVRn release from source in the time budget. Treat WiVRn equirect2/
cylinder as **"probably yes, verify per-release."** quad (our monitors) is
guaranteed.

Design implication: **equirect2 and cylinder are the only portable environment
primitives.** Anything cube-map-based is a non-starter on this Monado build and must
be resampled to equirect first.

---

## Option A — Static panorama as a runtime layer (equirect2 / cylinder)

**What the user sees.** They set `openxr:environment = ~/pano/venice_sunset.hdr`.
On session start, a 360° photographic sky/vista surrounds the monitors, correctly
view-independent (turning the head reveals the whole sphere). A cylinder variant
gives a seamless 360° "wall" band without top/bottom coverage — cheaper and often
enough behind a row of monitors.

**Architecture.** Add a `CXREnvironment` (owned by `COpenXRManager`, lives on the
frame thread like `CXRMonitorLayer`):
- On load: decode the panorama on the **main thread** (image I/O off the frame
  thread), upload once into a dedicated `XrSwapchain` (one image, sized to the
  panorama, e.g. 4096×2048), tonemap/color-convert during the upload blit, release
  the image once. No per-frame acquire after that (a released swapchain image
  re-presents every frame — same trick doc 01 already uses for idle monitors).
- Per frame: build one `XrCompositionLayerEquirect2KHR` (`space = m_refSpace`, an
  identity or yaw-only `pose` for `openxr:environment_rotation`, full-sphere
  `centralHorizontalAngle = 2π`) and insert its header at **`layerPtrs[0]`**, ahead
  of the quads, in `OpenXRManager.cpp` around line 864.
- Enumerate the equirect2 extension in `CXRSession` (doc 01 extension table) and set
  a `m_hasEquirect2` flag; if absent, fall back to cylinder, else to opaque black
  (today's behavior) and warn.

**Required changes (files/subsystems).**
- `src/openxr/` new `XREnvironment.{hpp,cpp}` (loader + swapchain + layer builder).
- `src/openxr/XRSession.*`: request `XR_KHR_composition_layer_equirect2` /
  `_cylinder`; expose availability flags (mirrors doc 01 §Extensions).
- `src/openxr/OpenXRManager.cpp`: own the env object; insert its layer at index 0
  in the `layerPtrs` assembly (~L864); skip when blend mode ≠ opaque.
- `src/config/values/ConfigValues.cpp`: `openxr:environment*` vars (§Config), added
  to the existing `openxr:` block at L712-729.
- `src/openxr/XRIpc.*`: `hyprctl openxr environment <path>` verb + hot-reload.
- **Image decode:** reuse **hyprgraphics** (already a Hyprland dep, used by
  hyprpaper) for PNG/JPEG; for **`.hdr`/`.exr`** (the panorama-native formats) add a
  small loader — `stb_image.h` does Radiance `.hdr` in one header; `.exr` needs
  tinyexr (one header) or libdeflate+OpenEXR. Start with `.hdr` + LDR via
  hyprgraphics; add `.exr` if wanted.

**Color space.** HDRIs are linear scene-referred (`.hdr` = RGBE, `.exr` = linear
half-float). Our swapchain format preference is `GL_SRGB8_ALPHA8` (doc 01
§Swapchain format). Upload path must **tonemap** HDR → sRGB (an exposure multiplier
from `openxr:environment_brightness` + a simple Reinhard/AgX curve) during the blit;
sampling an SRGB8 swapchain then feeds the runtime linear values it expects. LDR
JPEG/PNG panoramas are already display-referred — upload as-is into the SRGB target.

**Runtime support.** Monado local: **equirect2 ✓, cylinder ✓** (evidence above).
WiVRn/Quest: quad-safe; equirect2/cylinder **probably**, verify per release, with a
graceful cylinder→black fallback. Headless test: acceptance-only (null compositor
shows nothing).

**Effort: S–M.** The layer plumbing is a few dozen lines; the loader/tonemap and
the extension-availability fallbacks are the real work.

**Risks.**
- Equirect2 unsupported on some target runtime → need the cylinder→black cascade and
  a clear `hyprctl openxr status` line saying which is active.
- No parallax: a photographic room looks like a painted backdrop when you lean —
  fine for skies/vistas, weaker for "a room." This is inherent to panoramas.
- Seam/pole artifacts at low panorama resolution; mitigate with ≥4K width.
- Tonemapping is a taste knob; ship a sane default + brightness/rotation only for v1.

---

## Option B — Self-rendered environment (projection layer: skybox or glTF)

**What the user sees.** A real 3D scene with **parallax** — a skybox cube, or an
actual room / terrain from a glTF file — that shifts correctly as they move their
head. Monitors still float as sharp runtime quads *in front of* it.

**Architecture.** Add a stereo **`XrCompositionLayerProjection`** as `layerPtrs[0]`:
- Create two per-eye swapchains at the HMD's recommended eye resolution
  (`xrEnumerateViewConfigurationViews`).
- Each frame, `xrLocateViews` to get per-eye pose+FOV, and in the frame thread's GL
  context render the environment twice (once per eye) into those swapchains, then
  submit a 2-view projection layer under the quads.
- Environment content: a **skybox** (6-face cubemap sampled by view ray — trivial
  shader, no depth) is the cheap case; a **glTF scene** needs a mini glTF renderer
  (**tinygltf** to load; our own GLES draw loop, materials, a depth buffer).

**Required changes.** Everything in A **plus**: per-eye swapchain management, an
`xrLocateViews` call and view math in the frame loop, a skybox/glTF GLES renderer in
`CXRGraphics` (new shaders, VBOs, depth attachment, texture streaming), and a
frame-time budget now dominated by env rendering at HMD resolution.

**This contradicts doc 00's non-goals** ("No stereo 3D compositor", "No per-eye
parallax"). Adopting B is a **project-scope decision**, not just an effort call — it
turns HypXRland into a (minimal) 3D engine. Quads stay runtime-composited and sharp
(good — text stays crisp because only the *background* is app-rendered), but we now
own eye-buffer rendering, its perf, and its correctness.

**Runtime support.** Projection layers are the **most universal** OpenXR layer —
Monado local ✓, WiVRn/Quest ✓, everything ✓ (better portability than equirect2!).
The cost is entirely on our side.

**Effort: L (skybox) – XL (glTF).** Skybox alone is L; a credible glTF renderer with
lighting is XL and a maintenance commitment.

**Risks.** Frame-thread render budget (env at full HMD res competes with the blit of
N monitors); GL state-machine complexity in a thread that must also keep the
context-unbound-between-bursts discipline (doc 01 §EGL ownership); scope creep; glTF
is a deep format. **Recommendation: if B ever happens, do skybox-cubemap-only** (ray
sample, no depth, no geometry) — it captures most of the "parallax-correct sky" value
at a fraction of glTF's cost, and a cubemap→skybox shader is well-trodden.

---

## Option C — Pluggable external client (the hyprpaper analog: "hyprxrpaper")

Three sub-variants, increasing isolation, increasing dependency on other work.

### C-a — Environment app as a second OpenXR client (runtime-composited)

The environment is just **another OpenXR application** that submits an equirect2/
skybox layer as an **overlay/underlay** in the *same runtime*, composited by
**Monado's multi-client compositor** beneath our session. This is the truest
hyprpaper analog: the compositor (here, the *runtime*) composites an independent
client's background; we contribute no environment code.

- **Depends entirely on the sibling task** (multi-client / overlay-session
  compositing under HypXRland). OpenXR's `XR_EXTX_overlay` / Monado's multi-client
  layering is the mechanism; z-ordering us above the env client is the open problem.
- **Isolation: excellent** (separate process, separate session, crash-isolated —
  exactly hyprpaper's property).
- **Pluggability: excellent** (any OpenXR app that submits a low layer works;
  third parties, incl. StardustXR-style home apps, could serve environments).
- **Latency: runtime-composited, effectively free to us.**
- **Effort: depends on sibling task; the env client itself is small.**
- **Keep-compatible hook:** define the environment as *the bottommost consumer of
  the composited stack* so whichever mechanism the sibling task lands (overlay
  sessions vs. our own multi-client muxing) can host it.

### C-b — Environment daemon renders into buffers we composite (dmabuf handoff)

A `hyprxrpaper` daemon renders a skybox/panorama and hands us **dmabufs** (a set of
cube faces, or one equirect texture) over a small Hyprland IPC/Wayland protocol; we
import them exactly like a monitor buffer (`CXRGraphics::blitBuffer`, the dmabuf→
`EGLImageKHR`→`samplerExternalOES` path doc 01 already ships) into the **background
swapchain** and submit an equirect2/quad layer.

- **Reuses our proven blit path** — no new runtime dependency, works on the pinned
  Monado today.
- **Isolation: good** (separate process; a daemon crash just freezes/blanks the
  background, doesn't touch our session).
- **Pluggability: good, and self-defined** — we own the protocol (like hyprpaper's
  socket), third parties can implement it (an animated-sky daemon, a
  weather/time-of-day daemon, a video-360 player).
- **Latency: one extra buffer hop**, negligible for a slowly-changing background.
- **Effort: M** (protocol + import wiring), **minus** the loader (the daemon owns
  decoding — cleanest separation of concerns, and keeps `.exr`/glTF deps *out* of
  the compositor).
- Essentially "Option A, but the pixels come from a socket instead of a file." A and
  C-b can share the same background-layer submission code; the source is swappable.

### C-c — In-process loader plugin API

Config points at a shared object implementing an `IXREnvironmentProvider`
(`renderInto(swapchain image, viewPose, dt)`); we `dlopen` it. Loaders for HDRI,
skybox, animated shaders, etc. live as plugins.

- **Isolation: poor** (in-process — a plugin crash takes down the compositor).
  Against Hyprland's grain (plugins exist, but a *background* is not worth compositor
  crashes).
- **Pluggability: high for developers, low for users.**
- **Effort: M**, but the ABI is a long-term maintenance burden.
- Weakest option; note for completeness, don't pursue.

**Comparison.**

| Variant | Isolation / crash-safety | 3rd-party pluggable | Latency | Depends on | Effort |
|---|---|---|---|---|---|
| C-a client | excellent | excellent | free (runtime) | **sibling multi-client task** | S + sibling |
| C-b daemon+dmabuf | good | good (our protocol) | 1 buffer hop | nothing new | M |
| C-c dlopen plugin | poor | dev-only | free | nothing | M |

---

## Content ecosystem — where environments come from

| Source | Format | Portable? | v1-realistic? |
|---|---|---|---|
| **Poly Haven / HDRI-Haven** | equirectangular `.hdr` / `.exr`, 1K–16K | ✓ (equirect2-native, CC0) | **Yes — the v1 target** |
| Photographer 360 panos | equirect `.jpg`/`.png` | ✓ | Yes (LDR path) |
| 360 video | equirect frames | ✓ (needs a feeder → C-b) | Later |
| Blender exports | render to equirect, or glTF | ✓ as equirect; glTF → Option B | equirect yes / glTF no |
| glTF scenes (Sketchfab, KhronosGroup samples) | `.gltf`/`.glb` | needs Option B renderer | No (v1) |
| Cubemap / KTX2 skyboxes | 6-face / KTX2 | **needs resample to equirect** (cube layer unsupported, §matrix) or Option B skybox | No (v1) |
| VRChat / "worlds" | Unity-proprietary | **Not portable** | No |
| visionOS "Environments" | Apple-proprietary | No — **UX reference only** | UX ref |
| SteamVR Home / "The Void" | Valve-proprietary `.vrenv` | No — UX reference | UX ref |
| **StardustXR** home/skybox packages | its own | (separate server) | **prior-art ref** — [maps to C-a](https://github.com/wayvr-org) |
| wlx-overlay-s | desktop overlay, **no environment feature** | — | prior-art: shows the "desktop-in-VR, no skybox" gap we'd fill |

Realistic v1 content story: **"drop a Poly Haven `.hdr` in your config, get a sky."**
Aspirational: animated/time-of-day skies (C-b daemon), true rooms (Option B skybox),
360 video (C-b feeder).

UX references worth stealing: visionOS Environments' *dial-in* (a slider that dims
the real room and fades the environment in — maps directly onto our blend-mode axis
+ `environment_brightness`); SteamVR Home's per-user default environment.

---

## Config surface sketch

Add to the `openxr:` block (`src/config/values/ConfigValues.cpp:712-729`). Model it
as **one "what's behind the monitors" setting** so environment and passthrough are
mutually exclusive and hot-swappable:

```ini
openxr {
    # empty = opaque black (today). A path = static panorama (Option A).
    # Special value "passthrough" hands the background to the blend-mode work.
    environment = ~/Pictures/panoramas/venice_sunset_4k.hdr

    environment_brightness = 1.0   # exposure/EV multiplier before tonemap (0.05..10)
    environment_rotation   = 0.0   # yaw degrees, aligns the pano to your layout (0..360)
    environment_shape      = auto  # auto | equirect | cylinder | black  (fallback ladder)
    # (future, Option C-b) environment_source = daemon  # pixels come from hyprxrpaper
}
```

- **Hot-reload:** `environment` is a plain string var; on change, main thread decodes
  the new image and stage-swaps the frame thread's env swapchain (same
  `m_swapchainDirty`-style barrier the monitors use, doc 02). Note the memory rule:
  the legacy `hyprctl keyword` path never fires `config.props_refreshed`, so — like
  `openxr:enabled`/`inhibit_idle` — a hot-swappable `openxr:environment` needs the
  same special-case in `ConfigManager.cpp parseKeyword`.
- **hyprctl verbs:** `hyprctl openxr environment <path|none|passthrough>` (live set);
  `hyprctl openxr status` gains a line: `environment: equirect2 (venice_sunset_4k.hdr)`
  or `environment: unsupported→black`.
- **Coexistence with passthrough / blend-mode:** the frame loop chooses, in order:
  passthrough blend active → submit no background, set `environmentBlendMode` to the
  passthrough value; else `environment` set + supported → submit the env layer at
  index 0, `OPAQUE`; else → opaque black (today). One switch, three outcomes.
- **socket2:** optionally emit `openxrenvironment>>equirect2,venice_sunset_4k.hdr`
  for bars, mirroring the existing `openxrsessionstate` event.

---

## Recommendation

**Ship Option A (static equirect2 panorama, with a cylinder→black fallback ladder),
structured so its background-layer submission code is reusable by Option C-b later.**

Rationale: it is the only option that (1) works on the pinned Monado build **today**
(evidence: equirect2 + cylinder render end-to-end), (2) honors doc 00's "no stereo
3D compositor" non-goal, (3) has a zero-friction content story (Poly Haven CC0
HDRIs), and (4) is S–M effort. It delivers the headline "scenic vista behind my
monitors" experience immediately.

**What v1 defers:**
- Parallax / true 3D rooms → Option B (skybox-only if ever), gated on relaxing the
  non-goal.
- Process isolation / third-party environment servers → Option C-a (needs the
  sibling multi-client task) or C-b (a `hyprxrpaper` daemon) once there's demand.
- `.exr` and 360-video → additive to A's loader / a C-b feeder.

**Keep-compatible now, at no cost:** define the background as "layer index 0, chosen
by one blend-mode-aware switch," and make `environment` accept the sentinel values
`none` / `passthrough` / `daemon`. That single indirection lets C-b (daemon pixels)
and the passthrough work slot in without re-plumbing the frame loop.

### De-risking spike (½–1 day, no product code)

The one unknown worth burning down before committing is **"does equirect2 actually
render, not just get accepted, on the real hardware path?"** (the null compositor
can't tell us — it displays nothing). Spike:

1. In a throwaway branch, hard-code a single equirect2 layer built from a solid
   2-color test texture (magenta/green hemispheres) uploaded to one swapchain image,
   inserted at `layerPtrs[0]` in `OpenXRManager.cpp:864`. No config, no loader.
2. Run against a **real** Monado compositor (not `XRT_COMPOSITOR_NULL`) — the desktop
   preview rig (`scripts/preview-xr.sh`, windowed monado-service) or a headset — and
   confirm the sphere is visible behind the quads and rotates correctly with head
   yaw. (Do **not** kill Hyprland by name; use the tracked-PID / full-path rule.)
3. Repeat the acceptance check under the null compositor in the `--xr` suite to
   confirm no `xrEndFrame` regression (this is the CI-able part).
4. If a WiVRn/Quest is available, submit the same layer and check both render **and**
   the graceful path when equirect2 is a runtime no-op.

Success = the test sphere renders on ≥1 real compositor. Then build A for real:
loader → tonemap → config → IPC.

---

## Open questions for the user

1. **Is relaxing doc 00's "no stereo 3D compositor" non-goal on the table at all?**
   If never, Option B is permanently out and the ceiling is panoramas (A) + daemon
   pixels (C-b). If someday, a **skybox-cubemap** projection layer (not glTF) is the
   sane first step.
2. **Priority vs. the sibling multi-client task.** If compositing *other VR apps*
   under us lands first, Option **C-a** makes environments nearly free and third-
   party-pluggable — is it worth waiting for, or ship A now and migrate later? (A's
   layer-0 switch makes the migration cheap either way.)
3. **HDR pipeline appetite.** LDR (JPEG/PNG via hyprgraphics) + Radiance `.hdr` (stb,
   one header) covers ~all of Poly Haven. Is `.exr` (tinyexr / OpenEXR dep) wanted
   for v1, or additive later?
4. **Daemon or not.** Do you want the *pluggable* story (`hyprxrpaper` over a socket,
   C-b) as an explicit v1 goal, or is a config-file path (A) enough until there's
   third-party demand? This decides whether we design the IPC protocol now.
5. **WiVRn/Quest is a target?** If yes, I should verify equirect2 is enabled in the
   WiVRn release you run (its build flags, not Monado's) before we lean on it; the
   cylinder→black ladder is the safety net regardless.

---

## Sources

Code (vendored, pinned Monado `c2ddab59d`):
- `subprojects/monado/src/xrt/state_trackers/oxr/extension_support/oxr_extension_support.py:48-52`
- `subprojects/monado/CMakeLists.txt:430-433` (layer feature defaults)
- `subprojects/monado/build/src/xrt/include/xrt/xrt_config_build.h:73-77` (as built)
- `subprojects/monado/src/xrt/compositor/util/comp_render_gfx.c:763-806` (gfx renderer switch)
- `subprojects/monado/src/xrt/compositor/util/comp_render_cs.c:64-67, 602-664` (compute renderer switch)
- `subprojects/monado/src/xrt/state_trackers/oxr/oxr_session_frame_end.c:752, 930, 1028` (layer verify + feature gates)
- `subprojects/monado/src/xrt/compositor/null/null_compositor.c:453` (null layer_commit; comp_base accum)
- `src/openxr/OpenXRManager.cpp:769-896` (our layer-array assembly + xrEndFrame)
- `src/config/values/ConfigValues.cpp:712-729` (existing `openxr:` config block)
- `docs/openxr/00-overview.md` (non-goals), `docs/openxr/01-session-graphics.md` (frame loop, EGL discipline, swapchain format)

Web:
- hyprpaper: <https://wiki.hypr.land/Hypr-Ecosystem/hyprpaper/>, <https://github.com/hyprwm/hyprpaper>
- OpenXR composition layer types (overview): <https://tuncle.blog/en/composition_layer/index.html>, <https://registry.khronos.org/OpenXR/specs/1.1/html/xrspec.html>
- Monado equirect1 MR history: <https://gitlab.freedesktop.org/monado/monado/-/merge_requests/620>
- Monado compositor render docs: <https://monado.pages.freedesktop.org/monado/group__comp__render.html>
- WiVRn: <https://github.com/WiVRn/WiVRn>, compositor rework regression <https://github.com/WiVRn/WiVRn/issues/865>
- Prior art (Linux VR desktop / environments): StardustXR & wlx-overlay-s — <https://github.com/wayvr-org/wayvr>, <https://github.com/galister/wlx-overlay-s>
- Content: Poly Haven / HDRI-Haven (equirect `.hdr`/`.exr`, CC0)
