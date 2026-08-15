# Research: stereoscopic 3D content + a depth-styled desktop

**Status (updated 2026-08-14):** the core flat-SBS, stereo-window, depth-desktop, and OpenXR
pair paths described here are implemented. The authoritative user reference is doc 05 §§8.5–8.9;
this memo remains decision history plus the tuning/generalization backlog. It answers two questions
that turn out to be the same question:

- **Q1 — stereo content.** When a window or a monitor contains side-by-side / half-SBS /
  over-under stereo content (the user's Dead Space mod, a 3D movie in mpv, a YouTube 3D clip),
  HypXRland should present it as *true* stereo in the headset: left half to the left eye, right
  half to the right eye, at the right aspect ratio.
- **Q2 — the depth desktop.** Desktop chrome with real depth: the focused window subtly but
  decidedly raised off the monitor plane, the status bar hovering above the page, "depth" as a
  first-class styling axis beside border colour and width, animated — and degrading gracefully
  down the stack to the head-locked XREAL SBS path, without a full per-window OpenXR scene.

> **REVISION, 2026-08-08 — the presenter priority is inverted.** The first cut of this memo
> treated the flat side-by-side output (glasses in native SBS display mode, used as a plain
> head-locked external monitor, **no OpenXR runtime anywhere in the picture**) as an optional
> extra (old WP D6) and sidestepped its one hard problem. That was wrong for how the machine is
> actually used: the user runs the glasses in flat SBS mode *often*, without a runtime, and wants
> stereo content **and** the depth desktop there — *"a generic Hyprland feature that gets even
> more of a glow-up in VR/AR."*
>
> So: **the flat SBS output is the PRIMARY presenter tier** (new §3, and the spine of both WP
> ladders). The OpenXR quad pair is the **upgrade tier** — strictly cheaper to build, but it
> serves fewer of the user's hours and it cannot be the thing the feature is designed around.
> Everything in `src/openxr/` is behind a compile-time guard (`HAVE_OPENXR`, `XRIpc.cpp:2`;
> `option(WITH_OPENXR … ON)`, `CMakeLists.txt:224-225,630-631`) and every `xrrule` is silently
> inert under a Lua config (F8) — a primary-tier feature can live in neither. The user-facing
> vocabulary therefore moves out of the XR namespace and into `monitor =` / `windowrule =`.
>
> **A second decision, taken by the user and treated here as settled: no phantom monitors.**
> Hyprland must report exactly **one** logical monitor at its **presented (per-eye)** resolution —
> glasses scanning out 3840×1080 appear as a single 1920×1080 monitor in `hyprctl monitors`,
> to workspace routing, to waybar, and to input mapping — with SBS packing an internal scanout
> detail *below* the logical layer, conceptually the way `scale` already decouples logical size
> from pixel size. The "stereo mirror" shape (a headless desktop monitor + the physical output
> mirroring it) is **rejected** on that ground; §3.5 records what it would have cost and why the
> rejection is right on the technical merits too.

House style follows `research/22` and `research/23`: ground truth first with `file:line`, honest
effort/risk sizing, a recommendation split into "the cheap correct thing" and "the expensive
general thing", then a WP ladder.

Evidence base (all read-only, worktree at `3b527c0c` "setup: expand libva driverdir…"):

- **Compositor, XR**: `src/openxr/OpenXRManager.cpp` (frame loop, quad assembly, swapchain
  creation, effect resolution), `src/openxr/XRGraphics.{hpp,cpp}` (the blit / chrome / cursor
  GL passes), `src/openxr/XRRule.{hpp,cpp}` (the rule engine), `src/openxr/XRMonitorLayer.hpp`,
  `src/openxr/XRSession.cpp`, `src/openxr/XRIpc.cpp`.
- **Compositor, render**: `src/render/Renderer.cpp`, `src/render/OpenGL.cpp`,
  `src/render/ElementRenderer.cpp`, `src/render/pass/*`, `src/render/types.hpp`,
  `src/render/decorations/*`, `src/pointer/PointerManager.cpp`, `src/output/Monitor.cpp`,
  `src/output/DamageRing.cpp`.
- **Compositor, output/monitor plumbing** (added for the revision, all read-only):
  `src/output/Monitor.{hpp,cpp}` (the logical/pixel derivation, mode selection, swapchain update,
  direct scanout, mirroring), `src/output/MonitorResources.cpp`, `src/output/IMonitorGeometry.hpp`,
  `src/state/{MonitorState,MonitorLayoutController,MonitorPositionController,MonitorQueryCore}.cpp`,
  `src/protocols/core/Output.cpp`, `src/protocols/XDGOutput.cpp`, `src/protocols/LayerShell.cpp`,
  `src/desktop/view/LayerSurface.cpp`, `src/managers/ProtocolManager.cpp`, `src/debug/HyprCtl.cpp`,
  `src/config/shared/monitor/{Parser.cpp,MonitorRule.hpp,MonitorRuleManager.cpp}`,
  `src/pointer/PointerManager.cpp` (the whole hw-vs-sw cursor decision), `scripts/xreal-mode.sh`.
- **Compositor, config/rules**: `src/desktop/rule/{Rule.cpp,windowRule/*,layerRule/*}`,
  `src/config/values/ConfigValues.cpp`, `src/config/legacy/ConfigManager.cpp`,
  `src/config/lua/bindings/*`, `src/config/shared/animation/AnimationTree.cpp`.
- **Runtimes** (read from the shared checkout at the pinned submodule SHA
  `3752d437`): `subprojects/monado/src/xrt/compositor/util/comp_render_helpers.h`,
  `comp_render_cs.c`, `comp_render_gfx.c`, `state_trackers/oxr/oxr_session_frame_end.c`,
  `drivers/xreal_air/xreal_air_hmd.c`, `auxiliary/util/u_device.c`, `main/comp_renderer.c`;
  and `~/code/wivrn` @ `e54b56fe` `server/compositor/layer_squasher.cpp`.
- **Docs**: `docs/openxr/01-session-graphics.md`, `02-virtual-monitors.md`,
  `05-configuration.md`, `07-xreal.md`, `research/XREAL-3DOF.md`, `research/21` §"Verified,
  runtime side", `research/23` (the layer-host analysis).
- **External** (surveyed for §4.2/§4.5/§8.2/§10; URLs inline): the 2017 `zwp_stereoscopy_v1`
  patch series and its XML; the Vulkan registry (`vk.xml`) and Mesa's WSI; the Linux DRM mode
  UAPI and `amdgpu_dm.c`; RFC 9559 (Matroska), the Google Spatial Media v2 RFC, FFmpeg's
  `AVStereo3D`, mpv's `video-params/stereo-in`, GStreamer multiview caps; the detection
  documentation of DeoVR / HereSphere / Skybox / Pigasus / Kodi / Plex / Bigscreen / Quest
  Browser; vorpX and Depth3D/3Dmigoto/geo-11 artifact writeups; Android XR and Meta Horizon OS
  design guidelines; and the stereo-comfort literature (Terzić & Hansard's review,
  Shibata et al. 2011, Hoffman et al. 2008, Gardner's SPIE 2011 floating-window paper, ISU's
  Golden Rules, Meta's VR Best Practices, Google Daydream requirements). Claims that could not
  be verified to a primary source are flagged as such where they appear.

---

## TL;DR — RECOMMENDATION

> **Build one primitive — the *pane pair* — and both questions collapse into it.**
>
> A **pane pair** is: one monitor's frame delivered as *two* images, a left-eye pane and a
> right-eye pane, plus a declaration of what produced them. Everything else is plumbing:
>
> | | **Producer** (where the two panes come from) | **Presenter** (how they reach the eyes) |
> |---|---|---|
> | **Q1** | crop the client's already-packed SBS/TAB frame into halves — *no second render* | |
> | **Q2** | composite the monitor **twice** with per-window horizontal disparity | |
> | | | **TIER 1 — flat SBS output (§3)**: one monitor whose logical size is the *pane*, whose mode is the *pack*; the two panes are blitted into the two halves of the scanout buffer in `CHyprOpenGLImpl::end()`. **No OpenXR anywhere.** |
> | | | **TIER 2 — OpenXR (§5, §9)**: two `XrCompositionLayerQuad`s at the same pose, `eyeVisibility = LEFT`/`RIGHT`, each with its own `subImage.imageRect` |
>
> **The presenter that comes first is the flat one**, because it is the one that works with no
> runtime, no headset and no `WITH_OPENXR` build flag — and because it is the only one that makes
> this a *generic Hyprland feature* rather than an XR feature. §3 designs it in full: **one
> monitor object, logical size = one pane, mode = the packed scanout**, with the packing inserted
> as one more stage in the chain that already runs `logical → scale → transform → buffer`. The
> load-bearing discovery there is that defining the pack *below* `m_transformedSize` rather than
> above it **restores** the codebase's existing invariant (`m_size * m_scale == m_transformedSize`)
> instead of breaking it — which is why a change to Hyprland's most load-bearing geometry formula
> (`Monitor.cpp:1100-1102`) turns out to touch **single-digit line counts in about ten files**,
> leaves input mapping, layout, layer-shell and the aquamarine swapchain *completely untouched*,
> and needs no protocol and no aquamarine change at all.
>
> **The second load-bearing finding is that the XR presenter is nearly free.** HypXRland does
> **not** render eye buffers — it submits *quad layers* and the runtime does the stereo
> composition (`OpenXRManager.cpp:1887-1896`, and there is not one `xrLocateViews` /
> `XrCompositionLayerProjection` / eye-index loop anywhere in `src/`). `XrCompositionLayerQuad`
> carries an `eyeVisibility` field that we currently hard-code to
> `XR_EYE_VISIBILITY_BOTH` (`OpenXRManager.cpp:1890`), and **both** of our runtimes honour it
> *and* honour `subImage.imageRect` cropping: Monado at
> `comp_render_helpers.h:82-102` + `comp_render_cs.c:589` + `comp_render_gfx.c:763`, and WiVRn's
> own layer squasher at `server/compositor/layer_squasher.cpp:445` and `:741`. So "left half to
> the left eye" is **~40 lines in the quad-assembly loop** — no shader, no new swapchain format,
> no per-eye render loop, and it works identically on Quest-via-WiVRn (6DoF) and on the XREAL
> Air 2 Ultra via vendored Monado (3DoF, windowed *or* DRM-lease direct), because those are the
> *same code path* with different tracking.
>
> **Q1 recommendation.** The starting fact is that **Linux has no stereo declaration channel at
> any layer** (§4.2): the one Wayland protocol that was written and implemented in Weston
> (`zwp_stereoscopy_v1`, 2017) got *zero replies* on the list and died; Vulkan's only stereo
> extension is NVIDIA-vendor and direct-display-only, and Mesa hardcodes `maxImageArrayLayers = 1`
> on all three WSI backends; and DRM's HDMI-3D mode flags are real but `amdgpu` sets
> `stereo_allowed = false` unconditionally, so they do not exist on the user's hardware. If we
> want a channel we are choosing one. Three detection tiers, shipped in this order:
> 1. **Cooperative declaration via `xdg-toplevel-tag-v1`** — already implemented in this tree
>    (`src/protocols/XDGTag.cpp`, `CWindow::xdgTag()` at `Window.cpp:1791`) and **already a rule
>    match property** (`RULE_PROP_XDG_TAG`, `Rule.cpp:43` spelled `xdg_tag`,
>    matched at `WindowRule.cpp:451-453`). The Dead Space mod calls
>    `xdg_toplevel_tag_v1.set_toplevel_tag("stereo:sbs")` — one upstream-standard Wayland call,
>    zero new protocol — and the compositor matches it. **This is the ideal channel and it costs
>    us nothing to support.**
> 2. **Explicit user rules and verbs** for everything that will never cooperate. **The inversion
>    moves these out of the XR namespace**: the primary form is a generic
>    `windowrule = stereo sbs, match:class ^(mpv)$ match:fullscreen 1` plus
>    `hyprctl keyword monitor DP-5, …, stereo:sbs`, both of which exist in an
>    `-DWITH_OPENXR=OFF` build and under a Lua config; `xrrule = stereo …` survives as the
>    XR-tier convenience for forcing a *virtual* monitor's mode, not as the mechanism.
> 3. **Title/filename heuristics** as an opt-in convenience rule set shipped in
>    `example/openxr.conf`, not as compositor logic.
>
> **Auto pixel-correlation detection is rejected for v1** — not because it is hard (a 32×18
> mip-level SAD is ~free on the frame thread) but because the false-positive set is exactly the
> user's daily desktop: a tiled editor, a two-pane terminal, a diff view and a symmetric
> wallpaper all correlate strongly across the vertical midline, and the failure mode
> ("half my desktop just went to one eye") is a *session-ruining* one, not a cosmetic one. §4.4
> designs it properly anyway, behind `stereo:auto`, for the day the user wants it.
>
> **Q2 recommendation.** `windowrule = depth <z>` / `layerrule = depth <z>` — a new per-view
> styling property in the *existing* rule families (58 window-rule effects today,
> `static_assert(WINDOW_RULE_EFFECT_LAST_STATIC == 58)` at
> `WindowRuleEffectContainer.cpp:74-76`; adding one is 7 lines across 5 files), backed by an
> animated `CWindow::m_depth` float on a new `windowsDepth` animation node (one line in
> `AnimationTree.cpp`), with a zero-config default `decoration:depth_focused` so focus raise
> works out of the box. **Not** an `xrrule` — `xrrule`'s shipped doctrine is explicit that "the
> monitor is the unit of effect. Windows are only a source of conditions"
> (`05-configuration.md:384-385`), and depth is per-window styling. The monitor-level switch
> (`xrmonitor … stereo:depth`) says *whether* to build a pane pair; the window rule says *how
> high each window floats*.
>
> **The mechanism that makes Q2 cheap and correct**: re-compositing the whole scene per eye is
> **layered stereo, not depth-image warping — so it has no disocclusion holes at all.** When a
> raised window shifts right in the left eye, the sliver it vacates is filled by whatever is
> behind it, because the windows behind it are still being drawn. The entire artifact taxonomy
> of injection stereo (Depth3D/ReShade holes, halos, HUD splatter — §10.2) simply does not apply.
>
> **The comfort constraint is not what it looks like** (§8.2). Tasteful depth is *single-digit
> pixels* of disparity, so the first implementation risk is sub-pixel quantisation, not
> eye strain. And the binding limit is not the 1° whole-scene rule — it is the **≈0.1° foveal
> fusion limit**, which constrains the *step* between adjacent depths, not the absolute offset.
> Hence: a **small ladder of depth tiers** (0.0 / 0.2 / 0.6 / 0.8), not a continuum — which is
> also, independently, what Android XR shipped (16/32/56 dp elevation). Raise toward the viewer,
> never push behind the plane: Shibata et al. measured 2–3 % of screen width of headroom crossed
> versus 1–2 % uncrossed.
>
> **Where the user's "even head-locked SBS should enjoy this" hope holds and where it bends.**
> It **holds all the way down**, and after §3 it holds *without a runtime*: a plain 3840×1080
> output in SBS mode is a first-class stereo presenter, so the glasses get stereo content and the
> depth desktop whether or not Monado is running, and 3DoF (or zero DoF) costs nothing here
> because what sells raised windows is binocular disparity, not motion parallax. It **bends** in
> three places, all line items: (a) the hardware cursor plane must be forced off on a stereo
> output and the cursor drawn per pane — **mandatory work, not optional** (§3.6); (b) the depth
> desktop costs a **second full composite of the monitor per frame** whenever anything actually
> has depth (a mono desktop on a stereo output costs only one extra *blit*, §3.8); and (c) one
> protocol-visible fib — `wl_output.mode` stops matching the DRM mode (§3.4, item 8), which is a
> deliberate choice with a stated fallback.
>
> Order: **F0 → F1 → F2 → F3** (the flat SBS output — the presenter, the cursor, the tests) then
> **S1 → S3** (stereo content on it) then **X1** (the XR quad pair, which is now a small delta)
> then **D0 → D4** (the depth desktop). F1–F3 and S1–S3 need **no headset and no runtime**: a
> ordinary monitor shows the pane pair side by side and you can check correctness with your eyes,
> and the glasses in SBS mode are the real thing.

---

## 1. Ground truth — the seven facts that decide everything

### F1 — We submit quad layers. We have never rendered an eye buffer.

`COpenXRManager::frameLoop` acquires one swapchain image per monitor layer, blits the presented
desktop buffer into it (`OpenXRManager.cpp:1598` → `CXRGraphics::blitBuffer`), draws chrome
(`:1619`) and cursors (`:1629`), applies the uniform alpha (`:1637`), releases (`:1642`), and
then builds **one quad per layer**:

```cpp
// src/openxr/OpenXRManager.cpp:1887-1896
XrCompositionLayerQuad quad   = {XR_TYPE_COMPOSITION_LAYER_QUAD};
quad.layerFlags               = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
quad.space                    = quadSpace;
quad.eyeVisibility            = XR_EYE_VISIBILITY_BOTH;      // <- the hard-coded line
quad.subImage.swapchain       = l->m_swapchain;
quad.subImage.imageRect       = {{0, 0}, {w, h}};            // <- the whole image
quad.subImage.imageArrayIndex = 0;
quad.pose                     = xrFromPose(quadCenterPose);
quad.size                     = {quadW, quadH};
quads.push_back(quad);
```

There is exactly **one** pose query in the whole loop — `xrLocateSpace(m_session->m_viewSpace, …)`
at `:1677-1683`, a single VIEW-space head pose. `grep -r 'xrLocateViews\|XrCompositionLayerProjection\|XrView\b\|viewCount' src/` returns **nothing**. `createLayerSwapchain`
creates a plain 2D image: `faceCount = 1; arraySize = 1;` (`OpenXRManager.cpp:2045-2046`). The
only `XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO` uses are the blend-mode enumeration and
`xrBeginSession` (`XRSession.cpp:188,191,344`).

`research/21` already stated the consequence for encoding: *"HypXRland submits quad layers, not
a stereo projection layer. It never renders eye buffers. The stereo composition is done by the
runtime"* — the layer squasher renders the app's layer array into per-view scratch images using
that frame's head pose, every frame, and **quad layers always go through the squasher** (the
`fast_path` applies only to a single projection layer).

**This is good news, not bad.** It means the per-eye machinery we need already exists — it lives
in the runtime, one process over, and OpenXR exposes exactly the two knobs required to drive it.

### F2 — `eyeVisibility` and `subImage.imageRect` are honoured by *both* our runtimes

Monado's OpenXR state tracker converts the field for quads at
`oxr_session_frame_end.c:1275` (`data.quad.visibility = convert_eye_visibility(quad->eyeVisibility)`)
and normalises the sub-image rect at `:251-261` / `:1278`:

```c
// subprojects/monado/src/xrt/compositor/util/comp_render_helpers.h:82-102
static inline bool is_layer_view_visible(const struct xrt_layer_data *data, uint32_t view_index) {
    …
    case XRT_LAYER_QUAD: visibility = data->quad.visibility; break;
    …
    case XRT_LAYER_EYE_VISIBILITY_LEFT_BIT:  return !is_view_index_right(view_index);
    case XRT_LAYER_EYE_VISIBILITY_RIGHT_BIT: return  is_view_index_right(view_index);
```

and it is consulted in **both** render backends — the compute path
(`comp_render_cs.c:589`, which is the default on this rig: `USE_COMPUTE_DEFAULT true`,
`comp_settings.c:14-18`) and the graphics path (`comp_render_gfx.c:763`).

WiVRn is a Monado derivative with its **own** squasher, and it does the same:
`~/code/wivrn/server/compositor/layer_squasher.cpp:445` calls `is_layer_view_visible(&layer.data, view)`
and `:741` feeds `&q.sub.norm_rect` into the per-quad UBO.

**Therefore:** submitting two quads at one pose, one `LEFT` + one `RIGHT`, each cropped to half
the swapchain image, produces true stereo on Quest-over-WiVRn *and* on the XREAL via Monado,
with no runtime patches. This is the cheapest true-stereo mechanism available to us by a wide
margin.

Cost/risk notes on the quad-count doubling:
- `quads.size() >= m_session->m_maxLayerCount` (`OpenXRManager.cpp:1836`) becomes a *halved*
  budget for stereo monitors. `m_maxLayerCount` is floored at the spec minimum 16
  (`XRSession.cpp:174-177`) and a live session uses 3, so this is not binding — but the check
  must be widened to "will the *pair* fit" or a stereo monitor can be submitted half-drawn
  (left eye only), which is a nauseating failure mode. Guard it explicitly.
- `layerPtrs` is filled in a *second* loop after `quads` is complete
  (`OpenXRManager.cpp:1922-1923`), so pointer invalidation is already handled correctly; only
  the `reserve()` at `:1806` needs `* 2`.
- Depth ordering (`:1814-1826`) sorts by distance from the viewer. Both members of a pair share
  a pose, so they must be kept adjacent — sort the *pairs*, emit both.

### F3 — The XREAL path is the same path. There is no XReal-specific code in `src/openxr/`.

`docs/openxr/07-xreal.md:419-424`: **"SBS is a HID command, not a DRM mode change."**
`xreal-ctl mode 3d` sends `MSG_W_DISP_MODE`; the glasses re-present a native 3840×1080@60 EDID
and *hardware-split* every scanline into the two OLEDs. On the Monado side the geometry is
latched once at device-create time:

```c
// subprojects/monado/src/xrt/drivers/xreal_air/xreal_air_hmd.c:1167-1188
if (display_mode == XREAL_AIR_DISPLAY_MODE_3D) { info.display.w_pixels *= 2; }   // 1920 -> 3840
…
u_device_setup_split_side_by_side(&hmd->base, &info);
```

and `u_device_setup_split_side_by_side` (`auxiliary/util/u_device.c:218-275`) sets
`views[i].viewport.x_pixels = w_pixels * i` (`:256-264`) — view 0 at x=0, view 1 at x=1920 —
with asymmetric per-eye lens centres at `:233-236`. Monado's `calc_viewport_data`
(`main/comp_renderer.c:186-224`) then paints eye 0 into the left half and eye 1 into the right
half of the 3840-wide target. Distortion is **none** (`xreal_air_hmd.c:1447`,
`u_distortion_mesh_none`) — birdbath optics need essentially no warp.

DRM-lease **direct mode** changes only *who scans out the finished Monado frame*: the compositor
flips a `lease` monitor-rule flag, offers DP-5 over `wp_drm_lease_v1`, and Monado's
`comp_window_direct_wayland` leases the connector. Commit `966b355e` touched
`src/config/*`, `src/output/Monitor.cpp`, `src/protocols/DRMLease.*` — and **no file in
`src/openxr/`**.

**Consequence for the user's central hope:** the head-locked XREAL SBS path is not a lesser
path that needs a fallback. It is the *same* quad path with 3DoF tracking. Everything in this
memo that works on the Quest works there, unmodified. And 3DoF costs nothing for depth cues
specifically, because what sells raised windows is *binocular disparity*, not motion parallax.

(Two XREAL-specific numbers worth carrying: per-eye FOV is a hard-coded 46° horizontal
(`xreal_air_hmd.c:1173-1174`), and the panel is 1920×1080 per eye, so a stereo pane at full
XREAL resolution is 1920×1080 — no resolution loss versus mono. That is *not* true of half-SBS
content, §5.2.)

### F4 — The blit already renders into a sub-rect of a larger image

`CXRGraphics::blitBuffer` draws the desktop into the **inner content rect** of a swapchain that
also carries transparent chrome margins:

```cpp
// src/openxr/XRGraphics.cpp:489  (and :549, :577, :605 for the scissored variants)
glViewport(contentX, contentGL, (GLsizei)contentW, (GLsizei)contentH);
```

so "blit this source into *that* rectangle of a bigger image" is a shipped, load-bearing
capability, exercised on every frame of every session. `drawChrome` (`:722`) and `drawCursor`
(`:797`) already scissor into arbitrary sub-rects too (`:764`, `:864`).

What does **not** exist yet is a *source* crop: the blit's vertex shader synthesises UVs from a
fullscreen triangle with no uniform (`XRGraphics.cpp:282-293`), so it always samples the whole
source. Adding `uniform vec4 uSrcRect` and `vUV = uSrcRect.xy + vUV * uSrcRect.zw` is a **five-line
shader change** plus one `glUniform4f` — and that is the entire GL cost of Q1's crop if we
choose to crop at blit time rather than via `imageRect` (§5.1 argues for `imageRect`).

### F5 — XR monitors are headless, so the cursor is *in* the buffer; flat monitors default to a hardware plane

XR monitors are created as headless Aquamarine outputs (`OpenXRManager.cpp:2159-2183`), which
have no cursor plane, so the software cursor is composited into the frame the XR side blits.
On a real output, `CMonitor::shouldUseSoftwareCursors()` (`Monitor.cpp:2345-2364`) returns false
by default (`cursor:no_hardware_cursors` defaults to `2` = "SW only on nvidia+mgpu/VRR",
`ConfigValues.cpp:589`), and `renderSoftwareCursorsFor` bails immediately when the hardware
cursor is live (`PointerManager.cpp:647-651`).

**Both facts bite, and after the inversion the second one is the load-bearing one.** On the XR
path the desktop cursor is drawn *once*, at one monitor coordinate, which lands in exactly one
pane — a **monocular cursor**, which is one of the most reliably uncomfortable things you can put
in a stereo display. On a flat SBS output — now the *primary* presenter — the hardware plane
would place a single cursor in the left half, for the same reason, and the fix is no longer
optional cleanup but a required work package. The decision site is one line
(`PointerManager.cpp:324`) and the correct hook is the **per-output, refcounted**
`lockSoftwareForMonitor` (`PointerManager.cpp:78-96`), whose exact precedent is mirroring
(`Monitor.cpp:1437`) — *not* the global `lockSoftwareAll()` used by cursor zoom
(`Renderer.cpp:2091-2098`), which the first cut of this memo recommended in error. Full treatment
in §3.7; §5.4 covers the XR-side ray cursor.

### F6 — `xdg-toplevel-tag-v1` is already implemented *and* already a rule match property

`src/protocols/XDGTag.{hpp,cpp}`, registered at `ProtocolManager.cpp:214`. The tag reaches the
window object:

```cpp
// src/desktop/view/Window.cpp:1791-1796
std::optional<std::string> CWindow::xdgTag() {
    if (!m_xdgSurface || !m_xdgSurface->m_toplevel) return std::nullopt;
    return m_xdgSurface->m_toplevel->m_toplevelTag;
}
```

and is matchable from config as `xdg_tag` (`Rule.cpp:43`), evaluated at
`WindowRule.cpp:451-453`. Match-engine wiring for new *conditions* is generic — the Lua binding
walks the `match` sub-table by property name via `Rule::matchPropFromString` (`Rule.cpp:78-85`),
so conditions are automatically available from Lua while effects need a manual mirror line.

**This is the cooperative declaration channel, and it already exists.** A game that controls its
own Wayland client — which the Dead Space mod does — can announce `"stereo:sbs"` with one call
to an upstream-ratified protocol. Nothing needs to be invented, standardised, or upstreamed.
See §4.2 for the exact convention to adopt.

Also already present and useful for the semi-automatic tier: `content` as a match property
(`Rule.cpp:42`, `WindowRule.cpp:447-449`), backed by `wp_content_type_v1` — a client can declare
`video` or `game`, which is a strong prior for "this fullscreen thing might be stereo".

### F7 — The render pass has both a global translate hook and a per-surface UV crop

Two mechanisms already in the tree do most of Q2's and Q1-per-window's work:

**Global translate** — `SRenderModifData` (`src/render/types.hpp:54-68`) with
`RMOD_TYPE_TRANSLATE`, injected as a pass element:

```cpp
// src/render/Renderer.cpp:1116-1129 (renderAllClientsForWorkspace)
SRenderModifData RENDERMODIFDATA;
if (translate != Vector2D{0, 0})
    RENDERMODIFDATA.modifs.emplace_back(SRenderModifData::RMOD_TYPE_TRANSLATE, translate);
…
m_renderPass.add(makeUnique<CRendererHintsPassElement>(CRendererHintsPassElement::SData{RENDERMODIFDATA}));
```

applied to every drawn box (`OpenGL.cpp:2685`, `applyToBox`) and to damage regions
(`ElementRenderer.cpp:446`, `:475`). Two caveats that will bite (§6.3): it is **not** applied to
`IPassElement::boundingBox()`/`opaqueRegion()`, which `CRenderPass::simplify()` uses for
occlusion culling (`Pass.cpp:45-88`; the escape hatch is `m_renderData.noSimplify = true`,
honoured at `Pass.cpp:174`, precedent `ScreenshareFrame.cpp:200`), and it is not applied to
`m_renderData.clipBox`, which `renderLayer` sets to the whole monitor unconditionally
(`Renderer.cpp:971`) and which becomes the GL scissor (`OpenGL.cpp:1574-1589`).

**Per-window offset** — `CWindow::m_floatingOffset` (`Window.hpp:158`) is *already* "a Vector2D
that shifts one window and all of its decorations at render time". It is consumed at 14 sites:
the surface (`Renderer.cpp:629-630`, `:724`), border (`CHyprBorderDecoration.cpp:56`), shadow
(`CHyprDropShadowDecoration.cpp:58`, `:167`), inner glow (`CHyprInnerGlowDecoration.cpp:48`,
`:82`), groupbar (`CHyprGroupBarDecoration.cpp:158`, `:196`), damage (`Renderer.cpp:2734`),
visibility (`Renderer.cpp:284`) and the blur cutout (`OpenGL.cpp:2422`). **This is the exact
shape of a per-window depth disparity**, and the list of sites to touch is enumerable by
`grep -rn m_floatingOffset src/`.

**Per-surface UV crop** — `m_renderData.primarySurfaceUVTopLeft/BottomRight`
(`OpenGL.hpp:116-117`), computed in `IElementRenderer::calculateUVForSurface`
(`ElementRenderer.cpp:~60-153`) from the `wp_viewporter` source box and consumed at
`SurfacePassElement.cpp:160-175`. **This is how a stereo *window* (as opposed to a stereo
monitor) becomes possible**: while rendering pane *i*, override the U range of a stereo-tagged
window's primary surface to `[0,0.5)` or `[0.5,1)`. ~10 lines, and it answers §5.3's question
with "yes, per-window, at any size, not only fullscreen".

### F8 — `xrrule` and `xrmonitor` silently do nothing under a Lua config

`Config::Legacy::mgr()` returns `nullptr` unless the active config manager is the legacy one
(`src/config/legacy/ConfigManager.cpp:100-104`), and `COpenXRManager::reloadXRRules()` reads
`Config::Legacy::mgr() ? … : {}` (`OpenXRManager.cpp:3476`). Under a `.lua` config, therefore,
every `xrrule` and `xrmonitor` line evaluates to an empty list, **with no error and no warning**.
`05-configuration.md:1135` records the limitation ("Classic-config only") but the failure is
silent.

`openxr:*` **config values** survive automatically (both managers consume
`Values::CONFIG_VALUES`; the Lua manager rewrites `a:b` to `a.b` at
`lua/ConfigManager.cpp:410-412`), as do `hyprctl openxr` verbs (registered on `g_pHyprCtl`,
`XRIpc.cpp:459-467`) and the `xrmonitor` dispatcher.

**Design implication, and it applies to every new keyword in this memo:** put the parsed rule
storage in a **shared** manager under `src/config/shared/` — the pattern that makes
`monitor =` and `workspace =` survive both front-ends (`Config::monitorRuleMgr()`,
`Config::workspaceRuleMgr()`; the Lua side writes into the same store at
`LuaBindingsConfigRules.cpp:1160`) — and keep the string parser pure so the `.conf` handler and
a future `hl.xr_rule{…}` table binding can both populate the same struct. Doing this for the
*new* stereo effect is nearly free; doing it for the existing `xrrule`/`xrmonitor` is the
migration debt that task #128 will have to pay anyway. **Recommend paying it as part of S3**,
because it is much cheaper to build the stereo effect on a shared store than to move it later.

---

## 2. The primitive: a pane pair

Define the abstraction once, in the compositor, and every question in this memo becomes a
routing problem.

```
                       ┌────────────── PRODUCER ──────────────┐
  stereo client frame ─┤ crop halves (no second render)       ├─▶ ┌── pane L ──┐
  (Q1: SBS/HSBS/TAB)   └──────────────────────────────────────┘    │            │
                                                                   │            │
  depth desktop ───────┤ composite TWICE, per-window disparity ├─▶ └── pane R ──┘
  (Q2)                 └──────────────────────────────────────┘         │
                                                                        ▼
                                            ┌────────────── PRESENTER ──────────────┐
                                            │ TIER 1  FLAT SBS OUTPUT (§3)           │
                                            │   pack panes into the two halves of    │
                                            │   the scanout buffer. No OpenXR.       │
                                            │ TIER 2  OPENXR (§5)                    │
                                            │   2 quads, eyeVisibility L/R,          │
                                            │   subImage.imageRect = each pane       │
                                            │ NONE    ordinary monitor —             │
                                            │   pane R never produced, cost = 0      │
                                            └────────────────────────────────────────┘
```

Four properties of this framing earn it:

1. **It makes Q1 and Q2 the same feature.** The user asked two questions; the compositor gets
   one mechanism, one config vocabulary, one test surface.
2. **It composes.** A stereo game window on a depth desktop is "pane *i* = composite with eye
   sign *i*, in which that one window samples half of its buffer" (F7's UV crop). No special
   case.
3. **It degrades by dropping a stage**, not by branching. An ordinary monitor simply has no
   presenter and never asks for pane R, so the producer never runs and the cost is exactly zero.
4. **It has two presenters, and they are independent.** This is what the revision changed. The
   producer does not know or care whether the pair ends up in two halves of a DRM scanout buffer
   or in two OpenXR quads; §3 and §5 are two ~200-line consumers of the same pair. Building the
   flat one first costs the XR one nothing — it is the same producer plus a different final blit.

The *declaration* that a monitor is a pane-pair monitor is **per-monitor state on `CMonitor`**,
set from the monitor rule (`monitor = …, stereo:sbs`, §3.9) and resolved on the main thread.
Note the deliberate change of home: in the first cut this state lived on `CXRMonitorLayer`, which
does not exist in a build without OpenXR. On the XR tier it is *also* published to the frame
thread as a plain atomic — exactly the `publishBlackAlphaTuning` / `m_fxAlpha` discipline already
in force (`OpenXRManager.cpp:3410`, `:3554-3560`; `XRMonitorLayer.hpp` thread-safety block). No
strings cross to the frame thread (`MEMORY.md`'s session-killer rule).

---

## 3. The presenter that comes first — a flat SBS output, as ONE monitor

This section is the spine of the revision. It answers: *how does a plain 3840×1080 DisplayPort
output, with no OpenXR runtime in the process tree, present a pane pair — while Hyprland keeps
reporting exactly one 1920×1080 monitor to everything above the scanout?*

> **STATUS: IMPLEMENTED (Phase F core, commits `431e2eaa..bd99f734`).** `monitor = NAME,
> 3840x1080@60, 0x0, 1, stereo:sbs` ships. F1 (`431e2eaa`) landed the derivation, the pack in
> `CHyprOpenGLImpl::end()`, the damage fold, the pane-sized work/mirror/capture buffers, the
> `wl_output.mode` = pane decision (§3.6 item 8), `DS_BLOCK_STEREO`, the scale validation and the
> `stereo` monitor-rule token in all three config front-ends (positional, `monitorv2`, Lua); F2
> (`45fa12ef`) the forced-software, per-eye cursor (§3.7); F3 (`bd99f734`) extracted the pane
> geometry into `src/output/StereoPacking.hpp` as pure functions and covered it with 27 gtests
> plus a headless hyprtester case (644 → 671). F4 documents it (`docs/openxr/05-configuration.md`
> §8.5, `example/xreal.conf`). What is **not** yet built: `hsbs`/`tab`/`htab` layouts (§3.8 — the
> parser rejects them loudly rather than packing wrong), the item-15
> `wlr-output-management` write-path guard (the display-GUI hazard in §3.6 is documented, not
> fixed), and everything in §4 onwards (stereo *content* detection, the depth desktop, the XR-tier
> pane pair).

### 3.1 Why this tier is primary

Four reasons, in descending order of force:

1. **It is where the user's hours are.** The glasses spend a large fraction of their time in
   native SBS display mode driven as an ordinary head-locked external monitor —
   `xreal-mode.sh flat` territory (`scripts/xreal-mode.sh:7-9`, `:62`). No Monado, no WiVRn, no
   session. In that configuration the XR presenter does not exist, so a stereo feature built only
   on quads is a feature the user cannot use most of the time.
2. **The XR tier is not always compiled in.** All of `src/openxr/` sits behind `HAVE_OPENXR`
   (`src/openxr/XRIpc.cpp:2`, `#ifdef HAVE_OPENXR`), set only when
   `option(WITH_OPENXR "Build with OpenXR support" ON)` resolves and the dependency is found
   (`CMakeLists.txt:224-225`, `:630-631`). A feature whose *primary* mechanism lives there is
   a feature that vanishes from a `-DWITH_OPENXR=OFF` build. Add F8 — `xrrule`/`xrmonitor`
   silently evaluate to nothing under a Lua config — and the XR namespace fails the "primary
   mechanism" test twice over.
3. **It is the generic feature.** "Hyprland can drive a side-by-side stereo display, and windows
   have a depth" is a statement about a compositor, not about XR. It is useful to anyone with 3D
   glasses, a 3D TV, a Looking Glass-style display or a VR-video habit — and it is the version of
   this work that could ever be discussed upstream (§3.11). The XR tier is then exactly what the
   user asked for: the same feature *"with a glow-up"*.
4. **It is the most testable thing in this memo.** A pane pair on a flat output is visible on an
   ordinary desktop monitor: the two panes sit side by side and you can check the disparity, the
   crop, the cursor and the edge clamp with your eyes, at your desk, with no headset, no runtime
   and no `hyprctl openxr`. The glasses in SBS mode are then a *confirmation* step, not a
   development loop. Compare the XR tier, where the equivalent check needs a headset donned or a
   null-runtime harness assertion about quad structs.

### 3.2 The problem in one line, and the shape of the answer

A 3840×1080 SBS output must present a desktop that is 1920 logical pixels wide. Today Hyprland
derives logical size from the mode by a single scalar divide — this is *the* invariant, and it is
asserted in two places (`applyMonitorRule` and `applyMonitorRuleSoft`):

```cpp
// src/output/Monitor.cpp:1100-1102  (duplicated at :734-736)
Vector2D xfmd     = m_transform % 2 == 1 ? Vector2D{m_pixelSize.y, m_pixelSize.x} : m_pixelSize;
m_size            = (xfmd / m_scale).round();
m_transformedSize = xfmd;
```

Read that as a pipeline and the answer writes itself. The chain today is

```
m_size (logical)  ──×m_scale──▶  ──transform──▶  m_transformedSize  ==  m_pixelSize (mode)  ──▶ scanout
```

and the proposal is to insert exactly one more stage at the end:

```
m_size (logical)  ──×m_scale──▶  ──transform──▶  m_transformedSize (ONE PANE)  ──PACK──▶  m_pixelSize (mode) ──▶ scanout
                                                 = 1920×1080                             = 3840×1080
```

i.e. the derivation is inverted for stereo monitors:

```cpp
// proposed, same two sites
const Vector2D PANE = m_pixelSize / stereoPackDivisor();   // sbs {2,1}, tab {1,2}, off {1,1}
Vector2D xfmd       = m_transform % 2 == 1 ? Vector2D{PANE.y, PANE.x} : PANE;
m_size              = (xfmd / m_scale).round();
m_transformedSize   = xfmd;                                 // <- the PANE, not the mode
```

**The whole design turns on where the pack goes, and putting it *below* `m_transformedSize` is
what makes this cheap.** The codebase's split between "logical space" (`m_size`, `logicalBox()`,
`m_scale`) and "buffer space" (`m_pixelSize`, `m_transformedSize`) is already clean and almost
total; the only places that *round-trip* between them are `m_size * m_scale`
(`Renderer.cpp:971`) and `m_transformedSize / m_scale` (`PointerManager.cpp:697-698`). Define the
pane as `m_transformedSize` and those round-trips still hold — `m_size * m_scale ==
m_transformedSize` is true again, exactly as today. Define it the other way (logical shrinks,
`m_transformedSize` stays the mode) and both of them break, along with the damage ring, the layer
clip box and the software cursor box. **Same feature, two framings, one of which costs about ten
lines and one of which costs a rewrite.**

The precedent argument the user asked about therefore holds, and holds precisely: `scale` and
`transform` already decouple logical size from pixel size — `scale` by a scalar divide,
`transform` by an axis swap — and stereo packing is one more, strictly-below-them step in the
same chain. It differs from them in only one respect, which is the honest cost: `scale` and
`transform` are *visible to clients* through `wl_output`/`wp_fractional_scale`, and the pack has
no protocol to be visible through (§3.4 item 8).

**Two corollaries kill the "abuse an existing knob" shortcuts outright.**

- **Anisotropic scale (a 2×1 scale) is not expressible, at any layer.** `m_scale` is a scalar
  `float` in the monitor (`Monitor.hpp:77-78`), in the geometry interface
  (`IMonitorGeometry.hpp:14`, `virtual float scale() const = 0`) and in the rule
  (`MonitorRule.hpp:45`); the parser accepts a single float and nothing else
  (`Parser.cpp:193-209`, `isNumber(value, true)`); and — decisively — **the protocol has no way
  to say it**: `wl_output.scale` is an integer (`Output.cpp:80`, `sendScale(std::ceil(m_scale))`)
  and `wp_fractional_scale` is likewise a single number. Setting `m_scale = 2` to make
  3840/2 = 1920 come out right would also tell every client "render me 2× buffers", which is the
  opposite of what a stereo output wants. Dead end, on protocol grounds, not on effort grounds.
- **A new value in the transform enum is illegal.** `m_transform` is a `wl_output_transform`
  (`Monitor.hpp:95`) that is **sent to clients** in three places — `wl_output.geometry`
  (`Output.cpp:85`), `wlr-output-management` (`OutputManagement.cpp:136`, `:188`) and
  `wl_surface.preferred_buffer_transform` (`Compositor.cpp:353-367`, fed from
  `Window.cpp:529`, `LayerSurface.cpp:461`, `Subsurface.cpp:319`, `LayerShell.cpp:268`). There is
  no room in that enum for a compositor-private value. Stereo must be its own field.
- **And a third, which decides the *shape* of the implementation:** `scale` is the wrong model to
  copy. It is applied as a per-box logical→pixel conversion at **~29 call sites** across windows,
  layers, popups and capture (e.g. `SurfacePassElement.cpp:172`,
  `ScreenshareFrame.cpp:231,252,283`) — i.e. it is *smeared*, precisely because it lives above
  the render. The pack must be modelled on **zoom and mirror** instead: a single stage at the
  final blit, which the entire pipeline above is unaware of (§3.3).

**Does aquamarine need to know? No.** Swapchain allocation reads the mode and nothing else:

```cpp
// src/output/Monitor.cpp:2748, :2765  (CMonitorState::updateSwapchain)
const auto& MODE = STATE.mode ? STATE.mode : STATE.customMode;
…
options.size = MODE->pixelSize;
```

so the swapchain stays 3840×1080, the buffer Hyprland commits stays a full-mode buffer, and
aquamarine sees an ordinary output doing ordinary modesets. Stereo packing is **purely a
Hyprland-side arrangement of the final composite into a full-width buffer**. No aquamarine
change, no DRM stereo mode flags (which do not exist on amdgpu anyway, §4.2), no protocol.

This is not just "we hope it does not care" — aquamarine's output model has **no logical size, no
scale and no transform in it at all**. `SOutputMode` is `{pixelSize, refreshRate, preferred,
modeInfo}` (`/usr/include/aquamarine/output/Output.hpp:19-24`), `COutputState::SInternalState`
carries `lastModeSize`/`mode`/`customMode`/`buffer`/`ctm` and nothing geometric above that
(`:63-82`), and `commit()`/`test()` take no size arguments (`:161-162`). The buffer-equals-mode
rule is enforced **by Hyprland**, at `Monitor.cpp:2748` and `:2765`, not by aquamarine. On the DRM
side the primary plane's `SRC_W/H` (buffer) and `CRTC_W/H` (mode) both stay 3840×1080, so no plane
scaling is even requested. *(Honest caveat: aquamarine is a pkg-config dependency here
(`CMakeLists.txt:151`, `:537`), not vendored — only its headers are on this machine, so "commit()
performs no further buffer-vs-mode validation" is inferred from the public API and Hyprland's use
of it, not read from aquamarine's implementation. Verify against upstream before relying on it.)*

### 3.3 Where the packing actually happens

Two candidate seams, and the cheap one wins.

**(i) Pack at the final blit — recommended.** Hyprland already renders offscreen by default:
`begin()` binds `resources()->getUnusedWorkBuffer()` and sets `m_offloadedFramebuffer`, and
`end()` blits it into the output buffer (`OpenGL.cpp:752`, `:785-829`). So the pack is: size the
work buffers at the pane, and in `end()` emit **one blit per pane**, with
`monbox = {i * paneW, 0, paneW, h}`. The whole render pass above it runs at pane size, unchanged,
with the existing scissor/viewport/projection semantics intact.

**`applyZoomTransform` is the existence proof for this shape, and it is a strong one.** Look at
what `end()` already does:

```cpp
// src/render/OpenGL.cpp:789-793, :808-829  (abridged)
CBox monbox = {0, 0, m_renderData.pMonitor->m_transformedSize.x, m_renderData.pMonitor->m_transformedSize.y};
…
m_renderData.pMonitor->m_zoomController.applyZoomTransform(monbox, m_renderData);   // :793
…
const auto TEX = g_pHyprRenderer->m_renderData.currentFB->getTexture();             // :808
g_pHyprRenderer->bindFB(g_pHyprRenderer->m_renderData.outFB);                       // :809
…
renderTexture(TEX, monbox, {.finalMonitorCM = true});                               // :829
```

`applyZoomTransform` (`MonitorZoomController.cpp:92-118`) does nothing but mutate that one box —
`monbox.translate(-ZOOMCENTER).scale(ZOOM).translate(…)` at `:109` — and **the rest of the
compositor has no idea it happened.** Windows, layers, damage, protocols and clients are all
unaware; the only two other sites that know zoom exists are the cursor behaviour hook
(`InputManager.cpp:285`) and a nearest-neighbour filtering toggle (`OpenGL.cpp:796-797`). A whole
"render something other than a 1:1 logical view" feature, in one function, below the projection.
The stereo pack is the same manoeuvre with two draws instead of one — the one thing zoom does not
need and we do is a **source rect** per draw, since the two panes come from two different
framebuffers (or the same one twice).

One ordering detail that turns out to matter a lot (§3.6): the mirror/capture copy
(`saveBufferForMirror`) sits at `OpenGL.cpp:799-806`, i.e. **between** the zoom transform and the
final blit — so it captures the *pre-pack* image. That is exactly where we want it.

**(ii) Per-eye viewport into the full-mode buffer.** Render directly into the 3840 buffer with
`glViewport(i * 1920, 0, 1920, 1080)` and an `outputProjection(paneSize)`. Saves one blit.
**Rejected for v1** on one specific piece of evidence: the pass's clip box becomes a `glScissor`
(`m_renderData.clipBox` → `OpenGL.cpp:1574-1589`), and `glScissor` is in window coordinates —
it is *not* relative to the viewport — so every scissored draw in the tree would need the pane
offset threaded through it. That is the "smeared everywhere" failure mode the whole design is
trying to avoid. Revisit only if the extra blit ever measures.

With (i), the two panes are two *source* framebuffers, and the producer decides how they differ:

| Producer state | Panes | Cost above a mono monitor |
|---|---|---|
| nothing stereo, nothing raised | **one** composite, blitted **twice** | one extra full-pane blit per frame |
| Q1 client-stereo content | two composites (the stereo window samples a different half of its buffer in each — F7's UV crop) | one extra composite |
| Q2 depth | two composites with per-window disparity | one extra composite |

**That first row is the floor and it matters:** putting the glasses in SBS mode does not cost a
second composite. A mono desktop on a stereo output costs one extra 1920×1080 texture blit —
about 2 Mpx of fill per frame, ~124 Mpx/s at 60 Hz, which on the AMD 890M that already drives
this output is noise.

### 3.4 The complete touch-point list

Every site that must learn about the pack, with what it costs. This is the honest bill.

| # | Site | Change | Size |
|---|---|---|---|
| 1 | `src/output/Monitor.cpp:1100-1102` **and** the duplicate at `:734-736` | the derivation above; plus the `m_createdByUser` back-computation at `:738-743` and the `m_size`-is-temporarily-the-mode dance at `:946`/`:973`/`:996`→`:1020` | ~15 lines |
| 2 | `src/output/Monitor.cpp:1807-1813` `updateMatrix()` — `m_projOutputMatrix = Mat3x3::outputProjection(m_pixelSize, …)` | pane size instead of mode. **One line, and it fixes every pass element at once**: `getScaleMatrix()` (`:1803-1805`) feeds `projectBoxToTarget` (`Renderer.cpp:1813-1816`), which every drawn box goes through | 1 line |
| 3 | Viewport: `OpenGL.cpp:735` (`begin`), `:689` (`beginSimple`), `Renderer.cpp:1320`, and the FB-bind override at `gl/GLFramebuffer.cpp:100-103` (`const auto& size = …pMonitor ? …->m_pixelSize : m_size;` — binding *any* FB while a monitor is current forces the viewport to the mode) | pane size for the offload/work FBs, mode size for the scanout FB. **This is the one place where "which size" genuinely depends on which buffer is being bound**, so it needs a real distinction rather than a blanket substitution | ~8 lines |
| 4 | FB allocation: `Monitor.cpp:2868-2869` passes `m_pixelSize` to `CMonitorResources` | pass the pane size; stencil/work/mirror buffers follow (`MonitorResources.cpp:15`, `:24`, `:129`, `:156`). Note `MonitorResources.cpp:149` returns the *monitor's* `m_transformedSize` — a latent inconsistency today that this design **resolves**, because both become the pane | 2 lines |
| 5 | **The pack**: `CHyprOpenGLImpl::end()`, `OpenGL.cpp:785-829` | one blit per pane from one-or-two source FBs; destination boxes from the layout (§3.8). This is where the feature lives | ~80 lines |
| 6 | Output damage: `Renderer.cpp:2205-2221` (`frameDamage.transform(…, m_transformedSize)` → `state->addDamage`) | map pane damage into both halves before submitting: `D_L ∪ (D_R + paneW)` | ~10 lines |
| 7 | Direct scanout: `Monitor.cpp:2099-2102` | add `DS_BLOCK_STEREO` beside the existing `DS_BLOCK_MIRROR`. It already **fails safe** without this — `Monitor.cpp:2129-2133` requires `PSURFACE->m_current.bufferSize == m_pixelSize`, and a fullscreen client on a 1920-logical monitor commits 1920 — but make it explicit and cheap | 3 lines |
| 8 | Solitary-client fast path: `Renderer.cpp:2135-2136` (selection at `Monitor.cpp:1983-1994` is size-independent, so it still fires) | must not take the single-render shortcut on a stereo monitor, or the right eye silently goes stale | 2 lines |
| 9 | `wl_output.mode`: `src/protocols/core/Output.cpp:82` sends `m_pixelSize` | **decision, §3.6** | 1 line |
| 10 | Hardware cursor | force software per output — §3.7 | ~20 lines |
| 11 | Fractional-scale validation: `Monitor.cpp:1038-1039` `Vector2D logicalSize = m_pixelSize / m_scale;` | must divide the *pane*, or a stereo monitor at scale 1.5 validates 3840/1.5 instead of 1920/1.5 | 1 line |
| 12 | `hyprctl monitors` (`HyprCtl.cpp:313-345`) | report the stereo mode and the true scanout size as extra fields, so the packing is discoverable rather than a mystery | ~10 lines |
| 13 | The monitor rule: `Parser.cpp`, `MonitorRule.hpp`, `MonitorRuleManager`, `ConfigManager.cpp:594-616` (monitorv2 keys) + `:1456-1475` (legacy positional) + `LuaBindingsConfigRules.cpp` | a `stereo` token — §3.10 | ~40 lines |
| 14 | **Capture sizing**: `ScreenshareSession.cpp:116` (`m_bufferSize = PMONITOR->m_pixelSize` for `SHARE_MONITOR`), `ScreenshareFrame.cpp:203-217` (the blit box and source rect, both `m_pixelSize`), `:362` (clear region) | pane size — see §3.6. Every capture protocol in the tree resolves to these (`Screencopy.cpp:77,87-92`; `ImageCopyCapture.cpp:91-92`), so this is **one fix, four protocols** | ~6 lines |
| 15 | `wlr-output-management` write path: `OutputManagement.cpp:384-392` lets a config GUI set `newState.resolution` from the head's mode list | a GUI picking `1920x1080` on a stereo output silently un-stereos it (or worse, halves the desktop again). Needs a guard or a re-apply of the stereo token | ~5 lines |

And, just as important, **the list of things that need no change at all** — each verified, not
assumed:

- **The aquamarine swapchain** — mode-derived only (`Monitor.cpp:2732-2774`). Untouched.
- **All input mapping** — entirely logical: `MonitorQueryCore.cpp:87`, `:102`, `:113`
  (`logicalBox().containsPoint`), `PointerManager.cpp:855-868` (global mapped area from
  `m_position`/`m_size`), `:870-877` (`outputMappedArea` → `logicalBox()`), `:882-906`
  (tablet/touch/pointer bound-output), `Touch.cpp:33`, `InputManager.cpp:374`,
  `PointerManager.cpp:932`. A stereo monitor maps absolute devices onto the 1920 desktop **for
  free**, which is exactly the required behaviour.
- **`PointerManager.cpp:695-701` `getCursorPosForMonitor`** — literally computes
  `m_transformedSize / m_scale`, i.e. it *states* the invariant. Under this design it keeps
  yielding the logical size, so it stays correct. Under the naive framing it would have silently
  returned 3840 where logical space is 1920.
- **Layout, workspaces, layer-shell arrangement, popup constraints** — all logical.
- **`xdg_output.logical_size`** — `XDGOutput.cpp:125` already sends `m_size` = 1920. Correct
  as-is. And the XWayland `xwayland:force_zero_scaling` branch at `XDGOutput.cpp:123`, which
  sends `m_transformedSize`, also becomes correct — it sends the pane. Same for the two
  wayland↔X coordinate conversions that use the same expression
  (`XWaylandManager.cpp:188`, `:225`).
- **Input is not transform-aware at all**, which is worth stating because it removes a whole class
  of worry: grepping the tree, **no** input path applies `m_transform`; input lives entirely in
  logical global coordinates. The only inverse-transform on the pointer side is for the *hardware
  cursor plane* (`PointerManager.cpp:697-698`, `:708`), which §3.7 turns off anyway.
- **The damage ring** — sized `m_transformedSize` at `Monitor.cpp:329` and `:747`, and damage is
  fed to it in logical×scale coordinates (`Renderer.cpp:2769`, `:2717`;
  `PointerManager.cpp:297`, `:813`). With the pane definition the two agree and partial damage
  keeps working. *This is the single largest saving of the whole framing*: under the naive
  framing the ring is 3840 wide while damage never exceeds 1920, so the right eye would never
  repaint.
- **Pass-level clipping** — `Pass.cpp:37` intersects `m_transformedSize`; the layer clip box at
  `Renderer.cpp:971` is `m_size * m_scale`. Both become the pane, both correct.

### 3.5 The shapes that were rejected, and what they would have cost

Three shapes were on the table. The user has settled the verdict — **one monitor, no phantoms** —
so this subsection exists to record the alternatives honestly rather than to re-open them.

**(a) The stereo mirror — REJECTED.** The desktop lives on a *headless* 1920×1080 monitor; the
physical 3840×1080 output is set `mirror` of it and blits the pane pair into its two halves,
reusing `renderMirrored` (`Renderer.cpp:1961-1988`) and `saveBufferForMirror`
(`OpenGL.cpp:2533-2559`). It is genuinely tempting, and it is *fairer* than it looks:

- A mirror is **already** hidden from the ordinary monitor list — `MonitorState.cpp:31-46`
  re-derives `m_monitors` on every `monitor.layoutChanged` with `std::erase_if(m_monitors,
  isMirror)` (`:33`), so `monitors()` excludes it and only `hyprctl monitors all` shows it
  (`HyprCtl.cpp:327`, `:335`, which iterate `allMonitors ? allMonitors() : monitors()`).
- A mirror **loses its `wl_output` global entirely** (`ProtocolManager.cpp:110-125`, with the
  comment "mirrored outputs should have their global removed, as they are not physical parts of
  the layout", and `:140-144`), so waybar cannot even see it; and a layer surface explicitly
  bound to a mirror is redirected to the first real monitor (`LayerSurface.cpp:51-52`).
- The cursor is confined away from it for free — `PointerManager.cpp:926-940` skips mirrors when
  building `monitorBoxes`, and `setMirror` stacks the mirror at the source's position anyway
  (`Monitor.cpp:1428`).

So "an extra monitor in every monitor list" is **not** the honest cost; the honest costs are
these, and they are worse:

1. **Every user-facing name becomes a fiction.** `monitor = …`, `workspace = N, monitor:…`,
   waybar's `output:`, `hyprctl dispatch focusmonitor` — all of them would have to name a
   synthetic `HEADLESS-N`, while `DP-5`, the connector the user actually plugged in, becomes an
   invisible implementation detail. That is the thing the user's decision is really about, and no
   amount of list-filtering fixes it.
2. **Two frame clocks.** The headless source has its own presentation timing (non-DRM backends
   get `refreshRate` forced to 0 at `Monitor.cpp:962`), while the physical output has real
   vblank; mirrors are damaged only via `damageMirrorsWith` (`Renderer.cpp:2789-2812`) and
   scheduled separately. Judder here would be structural, not a bug you can fix.
3. **Workspace evacuation and a forced focus change at setup.** `setMirror` moves every workspace
   off the mirroring monitor to a backup (`Monitor.cpp:1414-1424`), clears its active workspace
   (`:1426`) and calls `rawMonitorFocus(monitors().front())` (`:1434`) — and it runs from inside
   `onConnect` (`:368-369`) before `monitor.added` is emitted (`:414`). That ordering is exactly
   the class of startup bug the XR code already fought (`Monitor.cpp:340-355`).
4. **A known aquamarine headless-output lifetime bug**, worked around in two places in this tree
   by deliberately leaking a reference (`FallbackState.cpp:119-136`,
   `OpenXRManager.cpp:2371-2390`). Building the *primary* display path on headless outputs means
   inheriting that.
5. It still costs the extra mirror framebuffer and the `saveBufferForMirror` pass, and it still
   forces software cursors — `setMirror` already does exactly that to the source
   (`Monitor.cpp:1437`) — so it does not even save the §3.7 work.

**(b) A true per-monitor stereo property — ADOPTED.** §3.2–§3.4. One `CMonitor`, logical = pane,
mode = pack.

**(c) Transform / scale abuse — DEAD, and not merely inelegant.** Both variants are killed by
protocol, not by taste: an anisotropic 2×1 scale cannot be expressed in `wl_output.scale` (an
integer) or `wp_fractional_scale` (a single number) and would tell clients to render 2× buffers;
and a new `wl_output_transform` value is illegal because the enum is sent to clients in three
places. Full evidence at the end of §3.2. The *idea* underneath it survives, though, and is worth
naming: the pack **is** best implemented as one more transform-like stage — just a
Hyprland-private one, applied at the final blit where `applyZoomTransform` already lives (§3.3),
rather than a value stuffed into a wire enum.

### 3.6 The protocol and UX decisions this forces, and the calls

**8. `wl_output.mode` — send the pane, not the mode.** `Output.cpp:82` sends `m_pixelSize`
(3840) while `xdg_output` sends `m_size` (1920) and `wl_output.scale` is an integer
(`Output.cpp:80`, `sendScale(std::ceil(m_scale))`). Leave it and a client that derives scale as
`mode / logical` computes 2.0 while the protocol says 1 — a disagreement with no legal way to
express it. **Recommend sending the pane size in `wl_output.mode`** and keeping the true refresh
rate. Rationale: the mode is a scanout detail *below* the logical layer, which is the whole
premise; every client that reads `wl_output.mode` is asking "how big is the screen I am drawing
on", and the honest answer is 1920×1080. The cost is that `wl_output.mode` stops matching the DRM
mode on exactly this one output, which is a fib nothing in-tree consumes. Mitigation: report the
**real** mode list honestly in `wlr-output-management` (so `wdisplays`/`nwg-displays` can still
select 3840×1080) and in `hyprctl monitors` (item 12), so the truth is one query away.

**And the trap if you *don't* do that.** Direct scanout compares the client's buffer against
`m_pixelSize` (`Monitor.cpp:2129-2133`). If `wl_output.mode` keeps advertising 3840, a fullscreen
client is entitled to believe it and submit a genuine 3840×1080 buffer — which then matches
`m_pixelSize` exactly, passes the check, and gets **scanned out unpacked**, i.e. the user's game
appears as one unstretched image spanning both eyes. Sending the pane size in `wl_output.mode`
removes the trap at the source; item 7's explicit `DS_BLOCK_STEREO` is the belt to that braces.

**Screenshots and screen capture — capture the LOGICAL view (one pane), not the packed frame.**
The *right* answer is easy; getting it is **not** free, and the first cut of this section was
wrong to imply otherwise. Capture in this tree funnels through one manager, and every monitor
capture sizes itself from the mode:

```cpp
// src/managers/screenshare/ScreenshareSession.cpp:114-116
switch (m_type) {
    case SHARE_MONITOR:
        m_bufferSize = PMONITOR->m_pixelSize;
```

with the blit box and source rect likewise `m_pixelSize` (`ScreenshareFrame.cpp:203-205`, `:217`)
and the clear region too (`:362`). Every capture protocol in the tree resolves to that one number
— wlr-screencopy (`Screencopy.cpp:77`, `:87-92`), ext-image-copy-capture
(`ImageCopyCapture.cpp:91-92`), `grim` (which uses one of those two), and the portal path. So
**left alone, a screenshot of a stereo monitor is the packed 3840 image with both eyes in it.**

Fix it at the source (item 14, ~6 lines) and it becomes the pane, because the copy those paths
read is taken *before* the pack: `ScreenshareFrame.cpp:182` samples
`resources()->getMirrorTexture()`, filled by `saveBufferForMirror` at `OpenGL.cpp:799-806` — which
sits between the zoom transform and the final blit (§3.3). Item 4 sizes that FB at the pane
(`MonitorResources.cpp:124-134` allocates from `CMonitorResources::m_size`, constructed from
whatever `Monitor.cpp:2868-2869` passes), so the content is already correct and only the
advertised size is wrong.

**The call: capture the left pane, at logical size.** A packed 3840 screenshot is useless to
every consumer of it — a bug report, a chat message, OCR — and "screenshot my desktop" means the
desktop, not the scanout. Note one pre-existing inconsistency this tidies up: the ext-image
capture-source already reports the *logical* box (`ImageCaptureSource.cpp:47-53` →
`logicalBox()`) while the same protocol advertises the *pixel* buffer size, so today those two
already disagree on any scaled monitor; making capture pane-sized makes them agree on stereo
monitors.

**Mirroring, in both directions, comes out right — once item 4 lands.** The mirror FB is
allocated at the resources' size (`MonitorResources.cpp:129`) and its damage basis is
`m_monitor->m_transformedSize` (`:148-150`); both become the pane, so a stereo monitor mirrored
*onto* an ordinary monitor sends a normal mono desktop rather than a letterboxed SBS pair. (Left
alone — resources still mode-sized — `renderMirrored` would fit a 3840-wide packed frame into the
destination with its aspect-preserving `std::min` scale, `Renderer.cpp:1969-1978`, and the other
screen would show both eyes squeezed side by side. There is **no source-rect mechanism** in
`renderMirrored` today, so "mirror just the left eye" is not available without adding one; the
pane-sized-resources route avoids ever needing it.) An ordinary monitor mirrored *onto* a stereo
monitor renders into `monitor->m_transformedSize`, i.e. the pane, and the pack duplicates it — a
stereo output showing a mono mirror at zero disparity, which is correct. Note only that
`renderMirrored` (`Renderer.cpp:1961-1988`) and `damageMirrorsWith` (`Renderer.cpp:2789-2812`)
duplicate the same box math and must stay in sync (the comment at `:2797` already says so).

**A config GUI can un-stereo the monitor behind your back.** `wlr-output-management` enumerates
modes straight off aquamarine (`OutputManagement.cpp:152-161`, `:247`) — so `wdisplays` /
`nwg-displays` correctly show `3840x1080` — but its *write* path takes the resolution from the
head's selected mode (`:384-392`) with no knowledge of the stereo token, so a user who touches
the display GUI at all can silently drop the packing or re-derive the wrong logical size. Item 15.
This is the one genuinely annoying externality of the design and it deserves a `hyprctl monitors`
field (item 12) so the state is at least visible.

### 3.7 The cursor — no longer optional, and this is the mandatory work

On the XR tier the cursor is software by accident: XR monitors are headless aquamarine outputs,
the headless backend does not advertise `AQ_BACKEND_CAPABILITY_POINTER`, so `attemptHardwareCursor`
bails at `PointerManager.cpp:384-385` and `hardwareFailed` is set. **On a real DisplayPort output
none of that applies.** The decision is one line:

```cpp
// src/pointer/PointerManager.cpp:324  (CPointerManager::updateCursorBackend)
if (state->softwareLocks > 0 || m->shouldUseSoftwareCursors() || !attemptHardwareCursor(state)) {
```

`CMonitor::shouldUseSoftwareCursors()` (`Monitor.cpp:2345-2364`) reads only globals —
`cursor:no_hardware_cursors` (`ConfigValues.cpp:589`, tri-state int defaulting to `2` = "SW only
on nvidia+mgpu/VRR"), `cursor:invisible`, and the tearing state. There is **no per-monitor cursor
option anywhere**: the `monitorv2` key list is `disabled, mode, position, scale, addreserved,
mirror, bitdepth, cm, sdr_eotf, sdrbrightness, sdrsaturation, vrr, transform,
supports_wide_color, supports_hdr, sdr_min_luminance, sdr_max_luminance, min_luminance,
max_luminance, max_avg_luminance, icc` (`ConfigManager.cpp:594-616`) and `Config::CMonitorRule`
(`MonitorRule.hpp:34-74`) has no cursor field.

Left alone, the hardware plane would place **one** cursor at
`getCursorPosForMonitor()` = logical×scale, i.e. somewhere in the *left* half of the 3840 buffer,
visible to the left eye only. A monocular cursor is one of the most reliably uncomfortable things
you can put in a stereo display (§5.4).

**The fix, and it is a shipped API with an exact precedent:** per-output, refcounted software
locks — `Pointer::mgr()->lockSoftwareForMonitor(mon)` / `unlockSoftwareForMonitor(mon)`
(`PointerManager.cpp:78-96`; state is `int softwareLocks`, `PointerManager.hpp:175`). The
precedent is **mirroring**, which does exactly this to a mirror source: `Monitor.cpp:1437` locks,
`Monitor.cpp:1399` and `:461` unlock. (The `cursor:zoom_factor` path uses the *global*
`lockSoftwareAll()` with a `static bool zoomLock` at `Renderer.cpp:2091-2098` — do not copy that
one; it is global and statefully toggled inside `renderMonitor`.) So: lock on entering stereo,
unlock on leaving, alongside the mirror lock. **~10 lines.**

Then the cursor must be *in* both panes, at the right depth. Three honest obstacles, all in
`renderSoftwareCursorsFor` (`PointerManager.cpp:641-693`), which appends a single
`CTexPassElement` (`:679-683`) after the workspace/lockscreen/overlay draws
(`Renderer.cpp:2181-2186`):

1. **You cannot simply call it twice.** `:653-656` returns immediately when
   `forceRender && (hardwareFailed || softwareLocks != 0)` — precisely our state. A second,
   offset draw needs an eye-aware parameter (or a relaxation of that guard).
2. **The leftover-damage bookkeeping records one box.** `state->swRendered` / `swRenderedBox`
   (`:686-689`) and the `damageSoftwareLeftover` lambda (`:292-298`) assume a single drawn
   rectangle; a second draw at a different X leaves an un-damaged ghost. `damageIfSoftware()`
   (`:797-816`) must widen to match.
3. **The box math itself is already right.** `:658-681` is monitor-local logical → ×`m_scale` →
   round, and `overridePos` (monitor-local logical, hotspot subtracted internally at `:663`) is
   exactly the hook for drawing at a shifted X. The precedent for using it is
   `ScreenshareFrame.cpp:312`, `:355`.

The clean shape, given §3.3(i): **the cursor is drawn inside each composite**, so pane L and pane
R each get their own cursor at their own disparity, and the pack does not have to know. When the
producer is in the one-composite fast path (nothing has depth, no stereo content) the cursor is
drawn once and duplicated by the pack at zero disparity — which is correct, because a flat
desktop's cursor belongs on the screen plane. **Cursor depth follows what it is over** (§5.4's
rule, and Google's Daydream requirement quoted in §8.2 item 3), eased over 60–100 ms.

Total cursor work: ~40 lines and one careful damage change. Call it the single most important
20 % of the flat presenter, because it is the thing that decides whether the result feels
pleasant or nauseating.

### 3.8 Pane geometry — full SBS, half SBS, over-under, and the XREAL mode trade

The pack in §3.3(i) is *a blit with a destination box*, which makes the layout question fall out
into two independent knobs instead of a special case:

- **pane render size** — what the desktop believes it is (`m_transformedSize`, and via `m_scale`,
  `m_size`);
- **pane destination boxes** — where each pane lands in the mode-sized scanout buffer.

That gives every layout with one mechanism:

| Layout | Mode | Pane render | Destination boxes | Per-eye samples |
|---|---|---|---|---|
| **full SBS** | 3840×1080 | 1920×1080 | `{0,0,1920,1080}`, `{1920,0,1920,1080}` | 1920 — **1:1, lossless** |
| **half SBS** (anamorphic) | 1920×1080 | **1920**×1080 | `{0,0,960,1080}`, `{960,0,960,1080}` | 960 (2:1 squeeze at blit time, linear filtered) |
| **over-under** | 1920×2160 | 1920×1080 | `{0,0,1920,1080}`, `{0,1080,1920,1080}` | full |
| **half over-under** | 1920×1080 | 1920×**1080** | `{0,0,1920,540}`, `{0,540,1920,540}` | 540 vertical |

Note what this buys: **half-SBS is not a different feature and it does not shrink the desktop.**
The logical desktop stays 1920×1080; only the destination box is squeezed. And §5.2's aspect
constant `k` — the whole `sbs`-vs-`hsbs` distinction on the XR tier — stops being an aspect fudge
and becomes just "the destination box is half as wide", which is the same parameter. One
mechanism, four layouts.

**The XREAL trade, as far as it can be determined from evidence in this tree:**

- After the HID `mode 3d` switch the glasses re-present a **native 3840×1080@60** EDID (297 MHz
  DTD) and hardware-split every scanline; driving a *forced, unadvertised* wide modeline instead
  produces "diagonal striping across the panel and no left/right stereo alignment"
  (`docs/openxr/07-xreal.md:36-42`). 3840-wide above ~72 Hz exceeds the 2-lane DP-alt budget and
  goes out of range (`:50`).
- The panel's native mono modes include **1920×1080@120** (297 MHz / 7.1 Gbit/s, explicitly
  called proven in `scripts/xreal-mode.sh:68`), and flat mode defaults to `1920x1080@90`
  (`scripts/xreal-mode.sh:62`).
- So the *tempting* trade is half-SBS at 1920×1080@120: 960 samples/eye at double the frame rate,
  versus 1920 samples/eye at 60 Hz. **Whether the glasses' L/R scaler accepts a 1920-wide signal
  while in HID 3D mode is UNVERIFIED**, and our own live findings say non-native timings in 3D
  mode stripe. Treat it as a live question for the F0 spike, not a plan.
- Even if it works, 960 horizontal samples across 46°/eye is ≈21 px/degree horizontally against
  ≈42 vertically (§5.2) — visibly soft on text. **Recommendation: full SBS at 3840×1080@60.**
- One hard constraint that this design must respect and does: the DP output must run at
  **`scale = 1.0`**, because 1 buffer pixel must map to 1 physical pixel or the per-eye split
  softens and can skew (`07-xreal.md:52-54`; an `auto` scale picked 1.25 on the FW16 and did
  exactly that). Under §3.2 that is the natural configuration — mode 3840, scale 1, logical 1920.
  A non-1.0 scale on a stereo output should be warned about, and `debug:disable_scale_checks`
  should not be needed (item 11 keeps the fractional-scale validator honest).

### 3.9 Damage and cost for the flat pair

**Partial damage survives.** This is the flat tier's advantage over §6.3's XR story. Damage lives
in pane space all the way down (the ring is pane-sized, item 4/§3.4); only the final submission
to the output needs the fold `D_L ∪ (D_R + paneW)` (item 6). Nothing about the packing forces
`m_forceFullFrames`. The things that *do* defeat partial damage are properties of the **depth
producer**, not the presenter — the occlusion-culling and un-offset-geometry breakages in §6.3 —
and they are the same on both tiers.

**Cost, measured against the mirror path that already exists** (the closest shipped analogue):

- An existing mirror costs one `saveBufferForMirror` pass, and it is **damage-limited**, not a
  full copy: `renderTexture` is handed `.damage = &m_renderData.finalDamage`
  (`OpenGL.cpp:2549`). Plus one extra full-size framebuffer per mirrored monitor
  (`MonitorResources.cpp:124-134`, `XRGB8888` unless 10-bit), and it is **shared with
  screencopy** (`needsACopyFB()`, `Monitor.cpp:2786-2788`), so if you are already screen-sharing
  the marginal cost is zero.
- Our pack costs **one extra pane-sized blit per frame** (the second half), also damage-scissored.
  At 1920×1080@60 that is ~124 Mpx/s of fill. In the same ballpark as an existing mirror, and
  strictly less than a mirror when nothing is damaged.
- The **second composite** — the real cost — is charged only when the producer needs two
  different panes (client-stereo content, or something with non-zero depth). §6.4's number
  applies unchanged, and §6.4's first mitigation ("only produce a pair when something actually
  has depth") is doubly important here, because it is what keeps the ordinary flat-SBS desktop at
  one composite.
- **Direct scanout is lost on a stereo output** (item 7). For a fullscreen game that is a real
  regression versus mono — but a fullscreen *stereo* game was never going to be scanned out
  directly anyway, and a mono fullscreen game on a stereo output is a configuration the user can
  fix by leaving stereo mode. Document it; do not try to be clever.

### 3.10 Config surface — generic, and outside the XR namespace

The declaration is a **monitor rule token**, because that is where "how this output is driven"
already lives, and because it must work in a build with no OpenXR and under a Lua config:

```ini
# classic, positional
monitor = DP-5, 3840x1080@60, 0x0, 1, stereo:sbs

# monitorv2
monitorv2 {
    output = DP-5
    mode   = 3840x1080@60
    scale  = 1
    stereo = sbs           # off | sbs | hsbs | tab | htab
}
```

Semantics, stated so nobody has to guess: **the resolution in the rule is always the mode you
scan out**, and the logical desktop is derived from it — exactly as `scale` behaves today
(`m_forceSize` / `m_activeMonitorRule.m_resolution` is the mode, `Monitor.cpp:760-761`). So
`3840x1080@60, stereo:sbs` means "scan out 3840, work on 1920".

Live toggling needs **no new IPC verb**: `hyprctl keyword monitor DP-5, 3840x1080@60, 0x0, 1,
stereo:sbs` already re-applies monitor rules. (`hyprctl output`-style sugar can come later.)
Wiring: the `stereo` key goes in `ConfigManager.cpp:594-616` (monitorv2 special values) and
`:1456-1475` (legacy positional), a field in `MonitorRule.hpp:34-74`, application in
`MonitorRuleManager`, plus the Lua mirror in `LuaBindingsConfigRules.cpp` — the same five-file
shape as `mirror` (`Parser.cpp:292-294`, `ConfigManager.cpp:1453-1456`, `:1091-1093`,
`LuaBindingsConfigRules.cpp:110`).

**Content declaration moves too.** §4's detection tiers are presenter-agnostic and unchanged, but
the *rule family* they land in is not: the primary form must be
`windowrule = stereo sbs, match:xdg_tag ^stereo:sbs$` — a generic window-rule effect, which works
with no OpenXR and under both config front-ends — with `xrrule = stereo …` surviving as the
XR-tier convenience for forcing a *virtual* monitor's mode. Likewise the mpv helper (§4.5) must
call something that exists without a runtime; `hyprctl openxr stereo …` cannot be the primary
verb. This is the single biggest correction the inversion makes to §4 and §5.

### 3.11 Upstreamability, honestly

Reframed as an XR-free feature — *"Hyprland can drive a side-by-side stereo display, and windows
can have a depth"* — this is a much better upstream proposition than anything in the first cut,
and it is still not an easy one.

What is in its favour: it needs **no new protocol**, **no aquamarine change**, and **no DRM
stereo modes** (which amdgpu does not offer anyway, §4.2); it is inert when off; it fails safe on
direct scanout; and its user surface is a single monitor-rule token. The generic framing has real
demand behind it — the entire flat-SBS ecosystem (VRto3D as an OpenVR driver rendering SbS/TaB on
flat displays; the XREAL Linux driver's Steam Deck SBS discussion, which is entirely about
getting *applications* to emit 3840-wide frames because no compositor will do it for them) exists
precisely because compositors do not offer this.

What is against it: the change edits Hyprland's most load-bearing geometry derivation, and
maintainers are rightly conservative about that. And the one recorded upstream data point is not
encouraging — **hyprwm/Hyprland#4125, "2d screen for vr support"** (2024), asked for very nearly
this (a head-locked 3DoF desktop plus "two overlapping viewports providing 3d effect for two
eyes, side by side 3d") and was closed with *"excuse me what? This is definitely not something I
will implement, but if I understand this correctly, you can make this a plugin."* A search of
`hyprwm/Hyprland` today returns **no** open or closed issue matching "stereoscopic". So the
demand signal upstream is one person, once, and the maintainer's answer was "plugin".

Note that "plugin" is not actually available for *this* part: a plugin can hook functions, but it
cannot redefine the `m_size`/`m_transformedSize` derivation or resize the monitor's work buffers
without patching the core. The depth half (§7, `windowrule = depth`) is far more plugin-shaped
and far more upstreamable in isolation.

**Verdict: build it as a fork feature; do not open an upstream conversation until it has run for
months.** When one is opened, lead with 3D TVs and 3D glasses, show it working on an ordinary
monitor, and never mention OpenXR in the first message. The realistic ask is small and specific:
a `stereo` monitor-rule token plus the ten-line derivation change, offered as a patch that is a
no-op for everyone who does not set it.

### 3.12 Testing this without a headset — and why it is the best-tested part of the memo

- **Correctness on an ordinary monitor.** Set a normal 3840- or 2560-wide desktop monitor to
  `stereo:sbs` and the two panes appear side by side. Every property that matters is then visible
  and screenshot-able: the crop is right, the disparity has the right sign, the cursor appears in
  *both* panes, the edge clamp holds, layer surfaces are clipped per pane. Cross your eyes and
  you can even fuse it.
- **The real thing.** Glasses in HID 3D mode at 3840×1080@60, `scale = 1.0`, per §3.8. No Monado,
  no WiVRn, no session — which also means no XR-lifecycle flakiness in the loop.
- **In CI.** `hyprtester` already launches Hyprland headless-only (`hyprtester/src/main.cpp:81-85`,
  `HYPRLAND_HEADLESS_ONLY=1`) and creates headless outputs at runtime
  (`/output create headless HEADLESS-2`, `main.cpp:490-493`), and headless outputs accept
  arbitrary custom modes (`Monitor.cpp:959-980`). So a test can create a 3840×1080 headless
  output, set `stereo:sbs`, and assert **structurally** that `hyprctl monitors` reports one
  monitor at 1920×1080 with a 3840 scanout, that a window opened on it gets a 1920-wide geometry,
  and that the damage submitted covers both halves. **Caveat, stated so nobody plans on it:**
  `hyprtester` has no pixel readback today (no screencopy client anywhere under `hyprtester/`),
  so *content* assertions need either a small screencopy helper or an eyeball. The structural
  assertions are free.

  > **STATUS: the caveat is spent — the helper exists (WP F3, then WP S2).**
  > `hyprtester/clients/screencopy-crop` asks "is a region capture a 1:1 crop of the full one?" and
  > `hyprtester/clients/screencopy-probe` asks "what colour is the pixel at x,y?", over the same
  > shm path grim and the screenshot portal take (`clients/screencopy-capture.hpp` is the shared
  > half). That was necessary, because the whole per-window CONTENT producer is invisible to every
  > structural assertion — same box, same geometry, same input region, same damage; only the texels
  > differ — so §5.3 could not be tested at all without pixels.
  >
  > The trick that makes it robust is to assert **shapes, not colours**. A client painted with four
  > distinct quadrants (`xdg-interactive --paint`) is probed at its four quadrant centres and the
  > result is reduced to which sampled the same colour as which: `abcd` for no crop, `aacc` for a
  > side-by-side crop (the left half stretched across the box), `abab` for over-under. The
  > composite, the monitor's colour management and the capture format all preserve "these two
  > match" and none preserves "this one is red". `stereoTaggedWindowSamplesOneHalfPerPane` gets
  > three different shapes out of the same client on the same monitor, which is what makes its
  > untagged control mean something.
  >
  > **Still not covered, precisely:** the packed scanout frame itself. Every capture protocol is
  > sized at the pane by design (§3.6), so the probe sees the eye-0 composite — never the two-pane
  > buffer the blit loop assembles. That gap is unchanged and is stated at the top of
  > `hyprtester/src/tests/main/stereo.cpp`.

---

## 4. Q1 — detecting stereo content

> **STATUS: IMPLEMENTED (Phase S, WP S1, commit `96cf9cdb`).** `windowrule = stereo <layout> [always|fullscreen]` is a
> generic window-rule effect (`WINDOW_RULE_EFFECT_STEREO`), matching every standard condition
> including `xdg_tag`, parsed once into a packed integer by the pure
> `src/render/StereoContent.hpp`, and mirrored into the Lua binding table — so it works under both
> config front-ends, which is F8's whole point. Tier A landed with it: `stereo auto` reads the
> client's `xdg-toplevel-tag-v1`, whose grammar is frozen as `stereo:{sbs,hsbs,tab,htab,mono}`
> (§4.2). §4.3's negative heuristic is in code as the DEFAULT gate — a rule engages only while the
> window is fullscreen-and-covering unless it says `always` or the client declared its own layout.
> Tier C ships as config (`example/xreal.conf`, `05-configuration.md` §8.6). Tier D (`stereo:auto`
> pixel detection, S8) is **not** built and is still not recommended as a default.
>
> The producer is the second half of the same WP: the crop lands at F7's seam
> (`IElementRenderer::calculateUVForSurface`) on the window's main surface only, reading the eye out
> of `m_renderData.stereoPane`. It shipped with a second-eye mechanism of its own — a replay of the
> frame's pass, self-gated on whether a declared surface had actually been drawn. **That mechanism
> is gone.** WP D2 needs the second eye to differ in GEOMETRY, which a replay of already-built pass
> elements cannot express, so the two producers were unified onto D2's: `renderMonitor` builds the
> scene once per pane, and the pane count is a predicate over the scene
> (`depthProducerActive(m) || stereoContentActive(m)`) rather than an outcome of drawing it. §3.3's
> producer table survives — an ordinary desktop on a stereo output with nothing declared still costs
> one composite and one extra blit — but it is now decided before the frame is built, which costs a
> conservative second composite for a declared window that is on the output and entirely occluded.
> The exact answer is still reported: `hyprctl monitors` carries both `stereoComposites` (what the
> frame cost) and `stereoContent` (whether the crop reached a surface). Both `sbs`/`hsbs` and
> `tab`/`htab` work; the `k` of §5.2 is deliberately unused on the flat presenter (see §5.2 below)
> and waits for X1.
>
> The unification also settles the pre-blur, which both WPs found independently. S1 marked
> `CPreBlurElement` non-replayable (it blurs what the framebuffer held BEFORE the pass reached it,
> and on a replay that is the finished first eye); D2 re-dirties `m_blurFBDirty` between panes when
> a background/bottom layer is raised. With the scene rebuilt per pane the first problem cannot
> arise, and D2's rule is the whole rule: **the pre-blur is recomputed per pane iff its SOURCE
> differs per pane, which is iff a `layerrule = depth` raised a background or bottom layer.** The
> content crop only ever touches a declared window's own surface, never the background the pre-blur
> samples, so a pane loop running for content alone leaves one pre-blur serving every pane.
>
> **Deviations from this memo, recorded:** §5.4's "suppress the desktop pointer on a
> client-stereo monitor" does not apply to the flat presenter — the cursor is drawn into each pane
> at the same position (it takes depth from what it is over, and a stereo window carries none), i.e.
> both eyes at screen depth, which is the correct behaviour and costs no code. And a stereo-declared
> window on a monitor that is not presenting a pane pair is left **exactly** as it is today (packed
> frame shown as-is), per §11's "❌ by design, cost 0" — not cropped to one half.
>
> **WP S2** then made the tier fold above testable without a compositor: the precedence is
> `Render::Stereo::resolveDeclaration(rule, tagged) -> {layout, gated}` and `CWindow::stereoLayout()`
> keeps only the two lookups and the fullscreen query — still asked *only* when the gate is in play,
> which is what keeps a per-surface-per-frame read cheap. Nine gtests spell the matrix out, including
> the two rows that are easy to get backwards: a client's `stereo:mono` suppresses `auto` but does
> not beat an explicit rule, and a client that declares its own packing lifts §4.3's gate even when
> the rule's layout disagrees with it. The producer itself is proved **in pixels** — see §3.12.

### 4.1 The tiers, and what each is for

| Tier | Mechanism | Reliability | Who it serves |
|---|---|---|---|
| **A. Cooperative** | client sets `xdg-toplevel-tag-v1` to `stereo:<layout>` | exact | the Dead Space mod; any app the user can patch; future upstream apps |
| **B. Explicit** | `windowrule = stereo …` / `monitor = …, stereo:…` / `hyprctl keyword` / a keybind — **generic, not XR** (`xrrule = stereo …` is the XR-tier convenience on top) | exact, manual | mpv/VLC/browser, per-session toggles, "this movie is over-under" |
| **C. Heuristic** | title/filename regex, `content:video`, fullscreen state | good-ish | shipped as *example config*, not compositor logic |
| **D. Automatic** | split-half image correlation on the composited frame | dangerous | deferred behind `stereo:auto` (§4.4) |

Tiers A and B are the recommendation. C is a config file. D is designed but not built.

**These tiers are presenter-agnostic and the inversion does not change them at all** — detecting
that a frame is packed SBS has nothing to do with how it reaches the eyes. What the inversion
*does* change is which rule family tier B lands in: it must be one that exists in a build without
OpenXR and under a Lua config, i.e. `windowrule`/`monitor`, not `xrrule`/`hyprctl openxr`. See
§3.10.

### 4.2 The cooperative channel — adopt `xdg-toplevel-tag-v1`, do not invent a protocol

**First, the finding that decides this: on Linux today there is no stereo declaration channel at
any layer.** All three plausible ones were checked and all three are dead ends:

- **Wayland.** One serious attempt exists and it was ignored to death. Emmanuel Gil Peyrot
  (Collabora) designed `zwp_stereoscopy_v1` — a global that advertises each output's native
  stereo layouts and a per-surface `zwp_stereoscopy_description_v1` with
  `set_layout(none|frame_packing|top_and_bottom|side_by_side)` and a `set_default_side(left|right)`
  hint *for compositing onto non-stereo outputs* — plus a 13-patch Weston implementation
  (gl-renderer, DRM backend, a `simple-stereo` client, and a patch literally titled *"compositor-drm:
  Cursors need to be broken with stereoscopy"*). Posted to wayland-devel on
  **2017-11-14** ([announce](https://lists.freedesktop.org/archives/wayland-devel/2017-November/035798.html),
  [design post](https://linkmauve.fr/blog/2017/09/08/adding-a-third-dimension-to-wayland/),
  [Phoronix](https://www.phoronix.com/news/Stereoscopy-3D-Wayland-Patches)) — and received **zero
  replies**. There are, as of today, **no stereo MRs or issues** in `wayland-protocols`, `weston`,
  `wlroots`, or `plasma-wayland-protocols`.
- **Vulkan.** The only stereo extension is `VK_NV_display_stereo` (ext #552, Vulkan 1.3.302,
  2024-11-21), it is scoped to `VK_KHR_display` **direct-display surfaces only** by design, and it
  requires `maxImageArrayLayers == 2` — which **Mesa hardcodes to 1 in all three WSI backends**
  (`wsi_common_wayland.c`, `wsi_common_x11.c`, `wsi_common_display.c`). `VK_KHR_multiview` is a
  render-pass feature (`gl_ViewIndex` broadcast), not a presentation declaration. **A Linux game
  has no Vulkan-level way to say "this is stereo."**
- **DRM/KMS.** The flags are real and atomic-usable (`DRM_MODE_FLAG_3D_{FRAME_PACKING,
  SIDE_BY_SIDE_FULL,SIDE_BY_SIDE_HALF,TOP_AND_BOTTOM,…}` in `drm_mode.h`, gated behind
  `DRM_CLIENT_CAP_STEREO_3D`, with `CRTC_STEREO_DOUBLE` timing help in the kernel), i915 and
  nouveau set `stereo_allowed = true` — but **`amdgpu` sets `aconnector->base.stereo_allowed = false`
  unconditionally** (`amdgpu_dm.c:9325`). On the user's AMD boxes the path does not exist at all.
  And it is a *scanout* feature anyway; our output is OpenXR.

So the choice is not "which existing channel" but "what convention do we adopt". Candidates:

| Channel | Verdict |
|---|---|
| **`xdg-toplevel-tag-v1`** | **Adopt.** Already implemented here (F6), already matchable as `xdg_tag`, upstream-ratified, one call, survives the Lua migration for free (conditions are generic). Costs the compositor **zero new protocol code** |
| Revive `zwp_stereoscopy_v1` | Not now, but keep it on the shelf: it is a *good* design (per-surface, typed, with a documented `wp_viewporter` interaction order and a mono-fallback eye hint) and reviving a written-and-implemented protocol is far cheaper than authoring one. Revisit if and only if a second compositor wants in — a protocol with one implementor is a string tag with extra steps |
| A new `hypr-stereo-v1` | Reject. Strictly worse than reviving the above |
| Window title convention (`… [SBS]`) | Reject as a *primary* channel — titles are user-visible and app-controlled in ways that break; keep as tier C |
| Env var + IPC handshake at launch | Reject. A `hyprctl` call from a game wrapper is a fine *user* workaround (and is tier B by another name), but as a channel it has no binding to the window that eventually appears |
| `wp_content_type_v1` | Keep as a *prior*, not a declaration — its enum is `none/photo/video/game` with no stereo member, but `content:video` + fullscreen is a good gate for tier C. It is also the structural template `zwp_stereoscopy_v1` should have been merged as |

**Recommended tag convention** (document it in `05-configuration.md`, and use it in the mod):

```
stereo:sbs      full side-by-side   — frame is 2x wide; each half is a correct-aspect eye view
stereo:hsbs     half side-by-side   — frame is normal width; each half is horizontally squeezed
stereo:tab      full over-under     — frame is 2x tall
stereo:htab     half over-under     — frame is normal height; each half vertically squeezed
stereo:mono     explicitly not stereo (suppresses heuristics)
```

matched with:

```ini
xrrule = stereo auto, focusclass:^(deadspace)$ fullscreen:1     # auto = read the tag
windowrule = ... , match:xdg_tag ^stereo:                        # generic-Hyprland uses too
```

`stereo:sbs` and `stereo:hsbs` differ **only in aspect handling** (§5.2), which is why the
distinction has to be declared and cannot be inferred from pixels.

**Upstreamability.** The tag convention costs upstream nothing (it is a string), and the
*general* feature — "a compositor may present a tagged toplevel as stereo" — is a plausible
future wayland-protocols conversation but not one to start now. The right upstream contribution
from this work, if any, is `windowrule = stereo` plus the `monitor = …, stereo:sbs` token in
mainline Hyprland, for people with 3D TVs and SBS glasses. **The inversion changes the standing
of that sentence:** it is no longer a by-product of an optional presenter, it is the whole
feature, and it is now sized and argued in §3.11 — including the one recorded upstream data
point, which is discouraging.

### 4.3 Heuristics (tier C) — ship as config, never as compositor logic

**This is not a guess about what people name files — it is what the entire shipping VR-video
ecosystem actually runs on** (§10.2): DeoVR, HereSphere, Skybox, Pigasus, Plex and Kodi all
detect stereo layout from *filename tokens*, and none of them documents reading container
metadata. The vocabulary descends from the Oculus/Meta video convention (`_180_LR`, `_360_TB`),
with per-player divergence. HereSphere's is the best-specified: tokens must be preceded by `_`,
`-` or space; `_LR`/`_3D` = side-by-side, `_RL` = reversed, `_TB`/`_3DV` = over-under, `_BT` =
reversed, a trailing `F` (`_LRF`) means **full**-frame rather than half, `_2D` forces mono.
Kodi's default requires *both* a `[-._]3d[-._]` tag and a `[-._]h?sbs[-._]`/`[-._]h?tab[-._]`
token. Plex uses the HTPC lineage's `H-SBS`/`Half-SBS`.

Everything here is expressible with the *existing* match vocabulary once the `stereo` effect
exists, so it belongs in `example/openxr.conf` where the user can edit it without a rebuild:

```ini
# Filename tokens, ordered so that the more specific rules win (xrrule folds in config order).
xrrule = stereo hsbs, focusclass:^(mpv|vlc)$ focustitle:[._ -](h-?sbs|half-?sbs|3dh?sbs|lr)[._ -]
xrrule = stereo sbs,  focusclass:^(mpv|vlc)$ focustitle:[._ -](f-?sbs|full-?sbs|lrf)[._ -]
xrrule = stereo htab, focusclass:^(mpv|vlc)$ focustitle:[._ -](h-?tab|half-?ou|h-?ou|tb)[._ -]
xrrule = stereo off,  focusclass:^(mpv|vlc)$ focustitle:[._ -]2d[._ -]
```

(Remember `xrrule` conditions are RE2 **search**, not full match — no `.*` wrapping needed.)

Two documented failure modes from the ecosystem, worth stealing rather than rediscovering:
HereSphere originally missed the near-universal `_180_sbs` spelling and silently defaulted to
mono, and Skybox still fails on combined tokens like `LR_180_FISHEYE`. Both are *ordering and
alternation* bugs in the regex, which is an argument for keeping the rules in config where the
user can fix them in ten seconds.

Note the shipped semantics that make this safe and that must be documented alongside:
`xrrule` conditions use **RE2 `PartialMatch` (search, not full match)** — a deliberate divergence
from `windowrule` (`XRRule.cpp:234-236`, doc `05-configuration.md:509-513`) — and
`focusclass`/`focustitle` can never match when the monitor has no focused window
(`XRRule.cpp:244-247`). Rules resolve **in config order, per effect**, so a later
`xrrule = stereo off, focustitle:.*[._ -]2d[._ -].*` cleanly overrides an earlier heuristic.

**The one heuristic worth putting in code** is the *negative* one: never engage stereo on a
monitor whose focused window is not fullscreen-on-that-monitor, unless a tag or a manual verb
says otherwise. Half-cropping a windowed desktop is the failure mode users will report as "the
compositor broke".

### 4.4 Automatic detection (tier D) — designed, deferred, and here is why

The idea: downsample the composited frame, split it vertically, and correlate the halves. If
`NCC(L, R)` is high *and* the residual is dominated by a small horizontal shift, it is SBS.

It is cheap. On the frame thread we already have the source as a GL texture inside a
`CScopedGLContext`; generating a 5-level mip and reading back a 32×18 luma pair is a few hundred
microseconds and could run every N-th frame (the `openxr:cursor_redraw_epsilon` precedent
(`ConfigValues.cpp:874`) shows the codebase is already comfortable with frame-thread thrift).
A defensible design:

- Work at 64×36 luma. Compute zero-mean NCC of the two halves; also compute NCC of the halves of
  the *top* and *bottom* thirds independently (real stereo correlates in all bands; a tiled
  editor with a sidebar does not).
- Require the best match to occur at a *small* horizontal offset (|d| < 4 % of half-width) and
  the NCC-vs-offset curve to have a single clear peak. Two terminals side by side correlate at
  d ≈ 0 with a *flat* curve; genuine stereo has structure.
- Hysteresis: engage after ~2 s (≈120 frames) above threshold, disengage after ~1 s below,
  and *never* engage while the focused window is not fullscreen.

**It also has real prior art, and the prior art agrees with the conclusion below.** vorpX shipped
exactly this in its Desktop Viewer: its author reported (forum, ~2026-02) spending two days on
*"an automatic detection system that continuously examines image content to determine whether
footage displays in side-by-side 3D format or standard 2D"*, called it *"super reliable even in
difficult edge cases with all 3D movies I checked"* — **and in the same post flagged "a small
remaining risk of false positives" and was undecided whether to enable it by default or gate it
behind a menu toggle.** That is the only confirmed shipping pixel-content stereo detector found
anywhere, and its author's own instinct was: build it, then hide it behind a switch.

And it is still the wrong thing to ship *first*, for one reason: **the false-positive population
is the user's actual desktop.** A tiled editor with two panes of the same file; a diff view; two
terminals; a symmetric wallpaper; a browser with a mirrored layout; the `hyprctl openxr layout`
output in two columns. The cost of a false positive is not a glitch — it is that half the
desktop goes to one eye and the other half to the other, which is instantly and severely
uncomfortable, and the user (in a headset, mid-task) has to find the toggle. The cost of a
*false negative* under tiers A–C is that the user presses a keybind.

**Recommendation:** implement it behind `stereo:auto` as a **late** WP (S8), with a hard
precondition of "fullscreen client on this monitor", and with the detector's decision surfaced
in `hyprctl openxr status` so it can be debugged without a headset. Do not make it a default.

### 4.5 External metadata channels — the file knows, so ask the player

The layout *is* recorded at the file level, in two vocabularies:

- **Matroska `StereoMode`** (element ID `0x53B8`, RFC 9559 Table 5): 15 values, `0` mono,
  `1` side-by-side **left first**, `2` top-bottom **right first**, `3` top-bottom **left first**,
  `11` side-by-side **right first**, plus interleaved/checkerboard/anaglyph forms. Note the trap:
  the left/right-first ordering convention is *inconsistent* between the SBS pair (1/11) and the
  TB pair (2/3). Note also that **it does not distinguish half from full** — that is inferred
  from the aspect ratio.
- **ISOBMFF `st3d`** (Google Spatial Media v2): a single byte, `0` mono, `1` top-bottom
  (left = top), `2` left-right (left = left), `3` custom, `4` right-left. Poorer than Matroska.
  Apple's MV-HEVC `eyes` box is richer (view, primary eye, baseline, horizontal disparity
  adjustment) and FFmpeg already parses all of them into `AVStereo3D`.
- **GStreamer** has the most complete model of all — `multiview-mode` negotiated in caps, with a
  dedicated `GST_VIDEO_MULTIVIEW_FLAGS_HALF_ASPECT` flag that is exactly the half-vs-full bit
  Matroska and `st3d` lack. Worth citing as the vocabulary to copy if we ever need a richer enum.

None of that is visible on any Wayland surface. But **mpv exposes it as a property**, and this
is the single clean structured signal available to a Linux compositor:

```
video-params/stereo-in     # read-only, player/command.c:2556
                           # values from mp_stereo3d_names[] in video/csputils.c, whose numeric
                           # indices MATCH the Matroska StereoMode integers by construction:
                           #   no=-1 mono=0 sbs2l=1 ab2r=2 ab2l=3 … sbs2r=11 …
```

readable over `--input-ipc-server` JSON IPC or libmpv. (There is no top-level
`--video-stereo-mode` option in current mpv; the writable counterpart is the filter option
`--vf=format:stereo-in=…`. **VLC does not read Matroska `StereoMode` at all** — FFmpeg and Bino3D
do. **MPRIS2 has no stereo field**; the only thing MPRIS gives you is `xesam:url`, i.e. the
filename, i.e. tier C by another route.)

**Concrete recommendation, and it is cheap: ship `contrib/mpv-hypxr-stereo.lua`.** Twenty lines
that observe `video-params/stereo-in` and set the window's stereo state — **shipped (S5) via the
generic route, not the XR one**: the script runs `hyprctl dispatch tagwindow ±stereo-sbs
pid:<self>` and four static `windowrule = stereo … always, match:tag stereo-…` lines do the rest,
which needs no OpenXR verb and no runtime. (The original sketch below said
`hyprctl openxr stereo <monitor> hsbs|sbs|tab|off`; the inversion made a *per-window*, XR-free
route both available and better.) Either way it gives the user *exact* detection for every 3D
file they own, with zero compositor heuristics and zero risk to the rest of the desktop. It is
strictly better than any pixel analysis and it is a weekend's work in another language.

**And one warning from the ecosystem that must survive into the docs:** in the shipping VR-video
world, *correct container metadata is a liability*. Bigscreen users repeatedly report that MKVs
carrying a `StereoMode` flag play wrong while the same content without it plays fine and lets the
manual toggle work — the community workaround is to *strip* `--stereo-mode` when muxing. So the
mpv script must be **advisory and overridable** (a later manual `hyprctl openxr stereo` must
win — which the `XR_EFFSRC_MANUAL` tier already guarantees, `XRRule.cpp:286-298`), never
authoritative.

---

## 5. Q1 — presentation mechanics

### 5.1 The crop: `imageRect`, not a shader

Two ways to get half the image into one eye:

- **(a) `subImage.imageRect` on two quads.** Keep one swapchain of the monitor's size; submit
  quad L with `imageRect = {contentX, contentY, contentW/2, contentH}` and
  `eyeVisibility = LEFT`, quad R with the other half and `RIGHT`. **No GL work at all.**
- **(b) Crop at blit time** with the `uSrcRect` uniform from F4, into a double-wide swapchain.

**Recommend (a) for Q1.** The content is already packed by the client; there is nothing to
re-render, and the runtime's sampler does the work it was going to do anyway. (b) is the right
answer for **Q2**, where the two panes genuinely have different pixels and must both exist in
the image.

The subtlety in (a) is the **chrome margin**. The swapchain is content + transparent margins,
and `m_contentSize`/`m_contentOffsetPx` locate the content rect inside it
(`XRMonitorLayer.hpp:141-146`, blit at `XRGraphics.cpp:424-489`). Splitting the *content* rect
means each eye quad covers only the content, so the grabbable move-bar and corner handles —
which live in the margins and whose hit geometry is `SXRChromeGeometry`
(`OpenXRManager.cpp:1874-1885`, `XRMath.hpp:617`) — fall outside both quads and vanish.

Three ways out, in increasing cost:

1. **Draw the chrome twice.** `drawChrome` already scissors into arbitrary rects
   (`XRGraphics.cpp:745-764`); call it once per pane with a pane-local geometry, and give each
   eye quad an `imageRect` covering *pane + its own margins*. This requires the swapchain to hold
   two margined panes, i.e. the double-wide swapchain of (b) — at which point (a) and (b) merge
   and the blit does two `glViewport`ed draws from two source halves. **This is the honest
   answer and it is still small** (§ the blit already does exactly one such draw).
2. **Suppress chrome on stereo monitors.** Cheap, and arguably correct: a monitor showing a
   fullscreen stereo game is not a thing you reposition mid-frag. `openxr:chrome_enabled` already
   exists per-session (`ConfigValues.cpp:836`); a per-monitor suppression while `stereo != off`
   is a one-liner. **Recommend this for X1**, with option 1 as X4. (Note this whole subsection is
XR-tier-only: on the flat presenter there is no chrome and no quad, so §5.1's problem does not
arise at all — another small way the primary tier is the simpler one.)
3. Submit a third `BOTH`-eye quad for the chrome. Rejected: it would float the chrome at the
   screen plane while the content sits at the quad plane, which reads as broken.

### 5.2 Aspect correction — the whole difference between `sbs` and `hsbs`

The quad's height is derived from the content's pixel aspect:

```cpp
// src/openxr/OpenXRManager.cpp:1795
results[i].heightMeters = results[i].widthMeters * (float)l->m_contentSize.y
                          / (float)std::max(1.0, l->m_contentSize.x);
```

With a pane pair this must use **pane** pixels times a layout-dependent constant. The rule, in
one line:

```
quadAspect(H/W) = paneH / (paneW · k)      k = 1 for full SBS, k = 2 for half SBS
                                           (axes swapped for TAB: k applies to paneH)
```

Spelled out, because getting it backwards produces a subtly-wrong, hard-to-name image:

**On the flat presenter this constant folds out, and that is a finding, not an omission.** There the
destination is the window's own box, which the user sized — not a quad whose height we derive — so
cropping half the buffer across an unchanged box already un-squeezes a half-packed frame by exactly
2 and leaves a full-packed frame at 1:1. `sbs` and `hsbs` are the *same* geometric operation on a
window and differ only in how many source samples each eye gets. `k` still ships, in
`Render::Stereo::aspectFactor`, because X1's quad pair genuinely needs it.

> **STATUS (WP S2):** the rule above is now applied, not merely stored —
> `Render::Stereo::presentedAspect(bufferSize, layout)` turns a packed buffer into the height/width
> one eye should present, `contentPaneSize()` being the "destination box" it derives from. The test
> that earns its keep asserts that a 3840×1080 `sbs` buffer, a 1920×1080 `hsbs` one, a 1920×2160
> `tab` one and a 1920×1080 `htab` one all land on the *same* number — and that `hsbs`, `htab` and
> mono are indistinguishable **by size**, which is this section's argument stated as an executable
> fact. X1 is still the only caller.

- **Full SBS** means the *frame* is double-width and each half is already correctly proportioned.
  A 3840×1080 SBS frame contains two 1920×1080 eye images. Quad aspect = pane aspect =
  `1080/1920`.
- **Half SBS** means the frame is normal width and each half is horizontally *squeezed* by 2.
  A 1920×1080 HSBS frame contains two 960×1080 halves that each represent a 1920×1080 image.
  Quad aspect = pane aspect × ½ = `1080/1920`.

So both land on the same presented aspect via a single constant driven entirely by the declared
layout. **This is the *only* reason `sbs` and `hsbs` must be distinguished — and it is why no
pixel analysis can substitute for the declaration.** It is also why the two dominant file-level
metadata vocabularies are insufficient on their own: neither Matroska's `StereoMode` nor
ISOBMFF's `st3d` encodes half-vs-full (only GStreamer's `GST_VIDEO_MULTIVIEW_FLAGS_HALF_ASPECT`
does), so players infer it from the aspect ratio — and so must we, if we ever accept a layout
declaration that omits it. A sane inference rule: if the frame's aspect is ≥ ~2.6:1, assume
full; otherwise assume half. Document it as a guess, and let the tag override it.

**Resolution consequence, and it is the honest cost of HSBS:** half-SBS content on a 1920-wide
monitor gives each eye 960 horizontal samples stretched across the full quad. On a 46°-per-eye
XREAL that is ~21 pixels/degree horizontally versus ~42 vertically — visibly soft, and the
reason full-SBS at 3840 is worth the mode switch. Say so in the docs; users will otherwise blame
the compositor.

### 5.3 Per-window stereo on an otherwise-mono monitor — yes, and cheaply

The obvious answer is "stereo is per-monitor; a stereo window must be fullscreen-on-monitor
first". That is the **v1** answer and it is defensible (it matches `xrrule`'s
monitor-is-the-unit doctrine and it is what the game will do anyway).

But F7's per-surface UV crop makes the general case tractable: while compositing pane *i*, a
window tagged `stereo:sbs` samples U ∈ `[0, 0.5)` for the left pane and `[0.5, 1)` for the right,
stretched across its unchanged destination box. `calculateUVForSurface`
(`ElementRenderer.cpp:100-152`) is already the single place UVs are decided, already handles
non-trivial ranges, and already has the `(-1,-1)` sentinel for "no mods". A per-window stereo
crop is ~10 lines there plus the eye sign in render data.

That means a stereo window can be **windowed, floating, on any monitor, alongside other
windows** — and it composes with depth (the window can be raised *and* stereo). It only pays off
when the monitor is already producing a pane pair, because otherwise there is only one pane to
sample into.

**The inversion changes the sequencing conclusion here, and this is one of the places the first
cut was actively wrong.** It reasoned that the only source of a pane pair was Q2's double
composite, and therefore that per-window stereo had to wait for the depth producer ("sequence it
after D2, not in S1"). But a monitor with `stereo:sbs` is producing a pane pair *by virtue of
being a stereo output* — the presenter asks for two panes whether or not anything has depth. So
per-window stereo is available as soon as Phase F exists, and it is now the **primary** form of
Q1 (WP S1), not a late by-product. Per-monitor stereo modes remain useful for the "this whole
screen is a 3D movie" case; they are no longer the only shape available.

### 5.4 The cursor

Three cursors are in play, and each needs a decision.

| Cursor | Today | With a pane pair |
|---|---|---|
| **Desktop pointer** (software, composited into the frame on headless XR outputs — F5) | one copy at one monitor coordinate | With Q1's crop it lands in **one eye only** → suppress it on a monitor in a client-stereo mode and let the XR-side cursor take over. With Q2's double composite it is drawn once per pane, so it works automatically **if** the second composite includes it (`renderSoftwareCursorsFor` takes an `overridePos`, `PointerManager.cpp:641`, precedent `ScreenshareFrame.cpp:312`) |
| **XR ray/endpoint cursor** (`drawCursor`, `XRGraphics.cpp:797`) | drawn once into the swapchain at the ray-hit uv | draw **once per pane**, with an added disparity so it sits at the depth of what it is over. Trivial: it is already a scissored quad at a computed uv (`:855-864`) |
| **Hardware cursor plane** (real outputs — i.e. the **primary** presenter) | default on (`Monitor.cpp:2345-2364`) | **mandatory work, §3.7.** Must be forced to software for that output only — `Pointer::mgr()->lockSoftwareForMonitor(mon)` (`PointerManager.cpp:78-96`), mirroring's exact pattern at `Monitor.cpp:1437`. Then the cursor must be drawn *inside each composite* so it carries per-pane disparity, which needs the `forceRender` guard at `PointerManager.cpp:653-656` and the single-box leftover damage at `:686-689` / `:292-298` to become eye-aware |

**Cursor depth is not optional.** A cursor drawn at zero disparity over a raised window sits
*behind* the thing it is pointing at, which reads as a depth-conflict and is genuinely
unpleasant (it is the classic "subtitle behind the object" problem from stereo cinema). Give the
cursor the depth of whatever it is over: the hit-test that resolves that already runs every
frame (`ViewHitTester`, and on the XR side `processPointer`'s target classification). Recommend
easing the cursor's depth (a short 60–100 ms ramp) so crossing a window edge does not snap it.

### 5.5 Interaction with the shipped effect stack

- **`blackalpha` (luma key).** Applied in the blit's fragment shader per pixel
  (`XRGraphics.cpp:312-325`), so it operates identically on both panes. No interaction — except
  that a keyed pane pair makes the *pane seam* visible if the margins differ; keep pane margins
  symmetric.
- **`alpha` (uniform fade).** `fadeTex` multiplies the whole finished image
  (`XRGraphics.cpp:631-653`). Two quads sampling that one image both inherit it. Fine.
- **Blend mode.** `XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND` (passthrough) composites both eye
  quads against the environment independently — no interaction.
- **`xrrule` precedence.** A `stereo` effect must fold like the others: defaults → rules in
  order → manual override, with provenance (`XR_EFFSRC_DEFAULT/_RULE/_MANUAL`,
  `XRRule.hpp:82-86`) so `hyprctl openxr status` can say *why* a monitor is in SBS. The existing
  fold at `XRRule.cpp:257-304` extends field-wise; the only novelty is that `stereo` is an
  **enum**, not a float, so `SXREffects` grows its first non-float member.
- **Chrome / grabs.** §5.1: suppress in v1.

### 5.6 Pointer routing must un-map the pane

Absolute pointer injection is keyed by monitor name plus a 0..1 UV
(`OpenXRManager.cpp:1176-1179`, `m_pointerDevice->m_boundOutput = layer->m_monitorName`). With a
pane pair, a ray hit is computed against a quad that now covers *half* the source image, so the
UV must be mapped back into full-monitor coordinates before injection — `u_monitor = u_pane / 2`
for the left pane, `0.5 + u_pane / 2` for the right… **except** that for Q2's double composite
both panes represent the *same* logical desktop, so `u_monitor = u_pane` and the disparity must
be *subtracted* instead. Two different mappings for the two producers; get this wrong and the
cursor is offset by half a screen. **Make the mapping a property of the producer**, not of the
presenter, and unit-test it (`tests/xr/` gtest, no headset needed).

Which pane does the ray hit? Both — they are coincident in space. Hit-test against the pair as
one target (the pose and size are shared), and use the *left* pane's mapping by convention.

> **STATUS (WP S2):** the CONTENT producer's half is built and unit-tested —
> `Render::Stereo::paneUVToContentUV` / `contentUVToPaneUV`, both directions, expressed through
> `cropForEye` so the crop the renderer runs and the un-map the pointer runs cannot drift apart.
> They are named for the producer rather than "stereoUnmap" precisely because of the warning above:
> the DEPTH producer's mapping is the identity, and inviting one function to serve both is how the
> cursor ends up half a screen out. X1/X3 are the callers; the flat presenter needs neither, since
> the destination box there is the window's own and input never leaves monitor coordinates.

### 5.7 XREAL specifics

Nothing changes (F3). Two notes:

- In 3D mode each eye is a full 1920×1080, so **full-SBS content at 3840 is lossless** through
  the whole chain: game → monitor buffer → swapchain → runtime → panel. That is the best
  stereo-content path the user has, better than the Quest's (which re-encodes).
- The user's flat/head-locked *non-Monado* mode (glasses as a plain desktop monitor on the DP
  output, `xreal-mode.sh flat`) has **no OpenXR runtime at all** and therefore no XR presenter.
  In the first cut this was described as "the only configuration that needs the flat presenter",
  filed as optional. **That framing is what the revision inverts**: it is a configuration the user
  is in *often*, it is the one that makes this a generic compositor feature, and it is now §3 —
  the primary tier. It needs the HID switch to 3840 first (`xreal-mode.sh xr`, or a manual
  `xreal-ctl mode 3d`), `scale = 1.0`, and nothing else; see §3.8 for the mode trade.

---

## 6. Q2 — the 2.5D depth mechanism

### 6.1 Why compositing twice is the *right* algorithm, not a hack

The naive fear is that shifting windows horizontally opens holes: move a raised window right in
the left eye and the pixels it vacated must show something. In image-space reprojection
(Depth3D/ReShade-style, and any depth-buffer warp) those pixels are genuinely unknown and get
smeared or inpainted — that is the characteristic artifact of the whole injection-stereo family.

**We have no such problem, because we are not warping an image — we are re-running the
composite.** Windows are drawn back-to-front from live buffers; when the raised window moves,
what is behind it is *drawn*, because it was always going to be drawn. This is layered stereo
(the Layered Depth Image idea), and for layered content it is exactly correct.

The artifact we *do* get is that each window is internally flat — a plane of pixels at one
depth, no internal parallax. Which is precisely the desired aesthetic: cards floating above a
page, not a fake-3D scene. It is also exactly what visionOS-style window chrome looks like.

Two real edge artifacts remain and both are addressable:

- **Frame/window violation at the panel edge — and this is a 74-year-old known problem, not a
  new one.** A window raised toward the viewer whose *left or right* edge sits at the panel edge
  is clipped differently in the two eyes: occlusion says "behind the frame", disparity says "in
  front of the screen". Gardner's SPIE paper on the Dynamic Floating Window (Proc. SPIE-IS&T EI
  7863, 78631A, 2011) identifies a *second and stronger* conflict in the same place —
  **retinal rivalry**, the sliver visible to only one eye, which *"wipes out all the associated
  stereoscopic parallax cues"* in that zone. The cinema fix, in continuous use since Norling and
  Spottiswoode's *The Black Swan* (1952) and animated per-shot since Disney's *Meet the
  Robinsons* (2006, where over half the shots used it), is a **per-eye asymmetric border mask**:
  mask offset right in the left eye and left in the right eye, so the *frame itself* floats
  ahead of the violating object. There is **no numeric rule** for how much offset — ISU's
  phrasing is "as far to the front as possible, but as far to the back as necessary" — so it
  will need tuning.
  **We have an unusually good fix already built: the chrome margin.** XR quads are content +
  transparent margins (`openxr:chrome_margin`, `ConfigValues.cpp:826`), so a few pixels of
  disparity have somewhere to go and a per-pane asymmetric mask is expressible with the existing
  scissored draw. **v1 recommendation: clamp**, so `|shift| ≤ margin_px` and the violation
  cannot occur; keep the floating-window mask in the back pocket for when depth gets ambitious.
  Note the asymmetry that decides where to spend effort: **vertical (left/right) edges are the
  severe case** because disparity is horizontal and a vertical cut removes *different* content
  from the two eyes; horizontal (top/bottom) edges are perpendicular to the disparity axis and
  are comparatively benign. Waybar's top edge is therefore the *cheap* one; a raised window's
  side edges are the expensive one.
- **Layer surfaces at the edge** (waybar is anchored to the top edge, full width, so both of its
  *vertical* edges are at the panel's vertical edges — the bad case). Options: clamp its
  disparity to the margin; shift waybar's *contents* rather than its box; or accept a sliver,
  which at the disparities §8 recommends is 1–2 px. **Recommend the clamp**, and note that the
  HMD case is one the cinema literature does not cover: our "frame" is partly the quad edge and
  partly the head-relative FOV boundary, which moves.

### 6.2 Where it hooks — ranked

Combining F7 with the render-path map:

| # | Injection point | Buys | Costs |
|---|---|---|---|
| **1** | **A `CWindow::m_depth` animated float folded in at the 14 `m_floatingOffset` sites**, plus the eye sign in `m_renderData` | True per-window parallax; decorations, blur cutout, damage and visibility all follow because they already follow `m_floatingOffset`; animation for free | 14 mechanical call sites; must mirror into `damageWindow` (`Renderer.cpp:2729`) and `shouldRenderWindow` (`:246-286`) |
| 2 | `CRendererHintsPassElement` + `RMOD_TYPE_TRANSLATE` pushed/popped around each window (pattern at `Renderer.cpp:1116-1129`) | Zero per-decoration edits; nestable | Breaks `simplify()` occlusion (needs `noSimplify`, `Pass.cpp:174`), and `renderLayer`'s unconditional `clipBox` (`Renderer.cpp:971`) scissors offset layers back |
| 3 | Two `renderWorkspace(..., geometry)` calls at half-width | Reuses shipped translate+scale | The aspect guard at `Renderer.cpp:2471` rejects half-width geometry, and the scale it derives squeezes the desktop rather than paning it |
| 4 | The final offloaded-FB blit in `CHyprOpenGLImpl::end()` (`OpenGL.cpp:785-829`; `applyZoomTransform` at `:793` proves `monbox` is freely mutable) | The cheapest possible SBS *output* | Produces a **flat** pair — no per-window disparity. **This is the flat presenter itself (§3.3)**, not an alternative to injection point 1: it packs whatever panes the producer hands it |

**Recommend 1 for the disparity, 4 for the flat presenter.** Option 1's shape is already proven
by `m_floatingOffset`, and its `grep` list is the implementation checklist.

The second composite itself: render the monitor's pass **twice** into two buffers. The
compositor already renders offscreen by default — `begin()` binds
`resources()->getUnusedWorkBuffer()` and sets `m_offloadedFramebuffer`, and `end()` blits it out
(`OpenGL.cpp:752`, `:785-829`) — and `beginRenderToBuffer` (`Renderer.cpp:3017`) is a shipped,
exercised path (cursorshare `CursorshareSession.cpp:157`, screenshare
`ScreenshareFrame.cpp:397`, snapshots `Renderer.cpp:3056-3140`). So "produce pane R into a
persistent per-monitor buffer" is an existing capability with a new caller, not new machinery.

On the XR path the pair is then handed to `blitBuffer` twice, into the two halves of a
**double-wide swapchain** (`createLayerSwapchain` already takes an arbitrary `fullSize`,
`OpenXRManager.cpp:2006-2058`, and swapchain recreation on size change is routine via
`m_swapchainDirty`, `:1450-1471`). **Note this needs no change to the monitor's mode**: the
desktop stays 1920×1080, only the swapchain doubles. That is the one genuine simplification the
XR tier enjoys over the flat one — it never has to reconcile a logical size with a scanout mode,
because its "scanout" is a swapchain image it sizes itself. §3.2 shows the flat tier's version of
that reconciliation costs about ten lines, so this is a difference in kind, not in cost.

### 6.3 Damage — the honest part

Three concrete breakages, from the render-path analysis:

1. **Occlusion culling uses un-offset geometry.** `CRenderPass::simplify()` reads
   `boundingBox()`/`opaqueRegion()` (`Pass.cpp:45-88`), which are computed from
   `m_data.pos - pMonitor->m_position` with no `renderModif` and no depth term
   (`SurfacePassElement.cpp:117-137`). A raised window will subtract damage from the wrong
   pixels. Fix: either teach those two methods the depth offset (correct, ~10 lines) or set
   `m_renderData.noSimplify = true` while depth is active (cheap, costs overdraw).
2. **Damage regions are in un-offset space.** `damageWindow` (`Renderer.cpp:2729`) already adds
   `m_floatingOffset` (`:2734`) — so folding depth into the same expression fixes this for free,
   which is another argument for injection point 1. Decoration `damageEntire()`s route through
   the same boxes.
3. **The damage ring clips to monitor size** (`DamageRing.cpp:25`), so content shifted past an
   edge silently loses damage. With the §6.1 clamp this cannot happen.

**Bring-up strategy:** hold `m_forceFullFrames` while a stereo producer is active
(`Monitor.hpp:93`, consumed at `Renderer.cpp:2117-2129`; precedents set it to 3 and 5 at
`Monitor.cpp:365` and `CursorManager.cpp:320`) — i.e. accept full repaints, measure, then
optimise. Given that the second composite already doubles the work, partial damage is a
second-order saving and should not gate the feature.

### 6.4 Cost

Per stereo-producing monitor, per frame: **one extra full composite** plus **one extra blit**
into the swapchain. For a 1920×1080 XR monitor on the user's hardware that is small in absolute
terms, but it is not free and it scales with monitor count — three XR monitors in depth mode is
six composites.

Mitigations, in order of value:
1. **Only produce a pane pair when something on the monitor actually has non-zero depth.** If
   every window is at depth 0 the two panes are identical; skip pane R and submit one `BOTH`
   quad. This is a cheap main-thread predicate and it means the cost is zero on monitors the
   user has not styled, and zero while nothing is focused-and-raised. **Do this from day one** —
   it also gives a free A/B toggle for the ergonomics spike.
2. Reuse pane L's buffer for anything that did not move (a future optimisation; not worth it
   before measurement).
3. Cap depth mode to the focused monitor.

### 6.5 The depth producer on the flat presenter — what changes, and what does not

**Superseded by §3.** The first cut of this subsection argued that a non-OpenXR SBS output was
blocked on "a monitor whose logical width is half its pixel width, which Hyprland cannot
express", and routed around it with a headless-monitor stereo mirror, filed as optional work.
Both halves of that were wrong: the expression is a ten-line change to one geometry formula once
the pack is placed *below* `m_transformedSize` (§3.2), and the mirror detour is rejected outright
(§3.5). **The flat presenter is now the primary tier and its design lives in §3.** What remains
here is only the part specific to the *depth producer* running on top of it.

Three deltas, and they are all in the producer's favour:

- **Partial damage survives on the flat tier.** The packing itself does not force full frames:
  damage stays in pane space and only the final submission needs the two-half fold (§3.9). What
  still defeats partial damage is §6.3's list — occlusion culling reading un-offset geometry, and
  decorations whose boxes do not yet carry the depth term. Those are producer bugs, identical on
  both presenters, and `m_forceFullFrames` remains the right bring-up crutch for *them*.
- **The single-pane fast path is more valuable here than on the XR tier.** On the flat presenter a
  mono desktop costs one extra *blit*, not one extra *composite* (§3.3), so §6.4's mitigation 1
  ("only produce a pair when something actually has non-zero depth") is what keeps the ordinary
  flat-SBS desktop free. Build it on day one, as §6.4 already says — the inversion just raises
  the stakes.
- **The cursor moves inside the producer.** On the flat tier the cursor must be composited into
  each pane with its own disparity (§3.7), which means the second composite has to include the
  software cursor draw rather than treating it as a post-pass. This is the one structural change
  the inversion makes to §6.2's injection point 1.

Injection point 4 in §6.2's table — the final offloaded-FB blit in `CHyprOpenGLImpl::end()` — is
no longer "useful only as the flat presenter". It *is* the flat presenter (§3.3), and the fact
that it produces a **flat** pair by itself is exactly why it needs the producer above it.

---

## 7. Q2 — depth as a decoration axis

### 7.1 One coherent home for the config

Both rule families exist and both are tempting. The split that respects the shipped doctrine:

```ini
# 1. WHETHER an OUTPUT presents a pane pair at all  (generic Hyprland — §3.10)
monitor = DP-5, 3840x1080@60, 0x0, 1, stereo:sbs        # the glasses, in native SBS mode

# 2. WHETHER a monitor's panes DIFFER, and from what  (generic, per-window or per-monitor)
windowrule = stereo sbs, match:xdg_tag ^stereo:sbs$      # a client-packed stereo window
windowrule = stereo sbs, match:class ^(mpv)$ match:fullscreen 1

# 2b. the XR tier's convenience layer over the same state (XR monitors only, classic config only)
xrmonitor = XR-main, 1920x1080, anchor:local pos:0,1.4,-1.5, size:1.6, stereo:depth
xrrule    = stereo depth, anchorstate:docked            # depth only while parked at the desk

# 3. HOW HIGH each thing floats  (per-view styling: windowrule / layerrule — generic Hyprland)
windowrule = depth 0.6, match:focus 1
windowrule = depth 0.2, match:class ^(Alacritty)$
layerrule  = depth 0.8, match:namespace ^(waybar)$
layerrule  = depth 1.0, match:namespace ^(walker|notifications)$
```

**Note the three-level split, which is what the inversion produced.** Level 1 is a property of an
*output* and belongs on `monitor =` because that is where "how this output is driven" lives.
Level 2 is a property of *content* and belongs on `windowrule` because it is per-window. Level 3
is *styling* and belongs on `windowrule`/`layerrule` beside border and shadow. Only level 2b is
XR-specific, and it is a convenience, not a mechanism — which is exactly the right amount of XR
in a generic feature.

Why this split and not the alternatives:

- **A new `stereorule` keyword: rejected.** It would need its own matcher, reconcile path,
  `hyprctl` surface and Lua binding, duplicating two engines that already do exactly these jobs.
  `research/23` rejected `xrlayerrule` for the same reason and the argument transfers.
- **Depth as an `xrrule` effect: rejected.** `xrrule` is per-*monitor* by design and by
  documented doctrine (`05-configuration.md:384-385`). Per-window depth in an `xrrule` would be
  the first exception and would immediately need a window selector, i.e. `windowrule` with extra
  steps.
- **Stereo mode as a `windowrule`: rejected for the *output*-level switch** — that is what
  `monitor = …, stereo:sbs` is for (§3.10) — **but adopted for the content-level one.**
  `windowrule = stereo sbs, match:xdg_tag ^stereo:sbs$` is the per-window form, it is the same
  effect name in the other family, and after the inversion it is the **primary** Q1 mechanism
  rather than a late refinement (§5.3). Consistent naming across families is worth more than
  avoiding the duplication.

**Effort:** a window-rule effect is 7 lines across 5 files plus the Lua mirror
(`WindowRuleEffectContainer.hpp:71`, `.cpp:70` + `static_assert` bump at `:74-76`,
`WindowRule.cpp:~298` parse case, `WindowRuleApplicator.hpp:~120` `DEFINE_PROP`,
`.cpp:~58` reset tuple + `~225` apply case, `LuaBindingsInternal.hpp:~105`). The layer-rule
mirror is the same shape in `layerRule/`. **The Lua mirror line is the one non-mechanical trap:**
`WINDOW_RULE_EFFECT_DESCS[]` is name-keyed and not enum-ordered, so omitting an entry silently
means "unavailable from Lua" with no compile error.

### 7.2 Zero-config defaults

Rules are for tuning; the feature should work with none. Recommend a `decoration:` block, which
is where border/shadow/dim live and which reads naturally beside them:

```ini
decoration {
    depth_focused   = 0.6     # focused window rises to this depth
    depth_unfocused = 0.2     # ordinary windows sit just off the page
    depth_layers    = 0.8     # top/overlay layer surfaces (waybar, notifications)
    depth_scale     = 0.12    # metres of rise at depth 1.0 (the comfort knob, §8)
}
```

The four values are a **ladder, not a range** — §8.2 shows that what the eye cares about is the
*step* between adjacent depths in the foveal field, not the absolute offset, and every shipping
XR OS that publishes numbers (Android XR's 16/32/56 dp elevation ladder) made the same choice.
Wallpaper stays pinned at 0.0; the whole span is ~12′ of angular disparity, ~20 % of the 1°
budget.

with `animation = windowsDepth, 1, 4, easeOutQuint` — a new node costing exactly one line
(`m_animationTree.createNode("windowsDepth", "windows");`, `AnimationTree.cpp:~37`; unknown
names are rejected at `ConfigManager.cpp:1590`, so the node must exist before the config does).

### 7.3 Which elements get depth, and what feels right

| Element | Recommended default | Rationale |
|---|---|---|
| Focused window | rise to `depth_focused` (0.6) | The whole point. Eased over ~200 ms; the rise *is* the focus indicator |
| Unfocused windows | 0.2 | One small step off the page, so the *page* is the wallpaper and windows are cards on it |
| Special workspace | above everything (1.0) | It already renders as an overlay with a scale animation; depth is the natural 3D reading of "it's on top" |
| `top`/`overlay` layer surfaces (waybar, mako, walker) | 0.8 | This is the "hovering above the page" the user asked for; layers are already anchored to edges so the §6.1 clamp matters most here |
| `background`/`bottom` layers (wallpaper) | 0.0, pinned | Anything else destroys the sense of a *page* |
| Cursor | depth of whatever it is over, eased (§5.4) | Non-negotiable |
| Fullscreen window | 0.0 | A fullscreen window *is* the plane; raising it just moves the whole panel |
| Drag/grab in progress | +0.2 on top of resting depth | Lifting while dragging is the single most legible depth cue in every prior art system |

**Deliberately not animating depth on hover** — pointer-driven depth changes at 60 Hz produce
constant vergence micro-adjustments and are the most likely source of eye strain in this design.
Depth should change on *discrete* events (focus, drag, fullscreen), eased.

---

## 8. Disparity, geometry, and comfort

### 8.1 The formula

For a virtual screen at distance **D** with interocular **b** (use 0.063 m), an element that
should appear at distance **d**, on a quad of width **W** metres carrying **P** horizontal
pixels:

```
screen-plane parallax   p  = b · (1 − D/d)          [metres; negative = in front of the screen]
per-pane pixel shift    Δ  = ∓ (p/2) · (P / W)      [left pane +, right pane −, for d < D]
angular disparity       θ  ≈ p / D                  [radians]
```

Worked against our defaults (`openxr:default_size` 1.6 m, `openxr:default_distance` 1.5 m,
`ConfigValues.cpp:749-750`; P = 1920 → 1200 px/m):

| Rise toward viewer | d | p | total px | per-pane px | θ |
|---|---|---|---|---|---|
| 2 cm | 1.48 | −0.85 mm | 1.0 | ±0.5 | 2.0′ |
| 5 cm | 1.45 | −2.2 mm | 2.6 | ±1.3 | 5.0′ |
| 12 cm (`depth_scale` default) | 1.38 | −5.5 mm | 6.6 | ±3.3 | 12.5′ |
| 30 cm | 1.20 | −15.8 mm | 19 | ±9.5 | 36′ |

**The load-bearing engineering consequence: tasteful depth is single-digit pixels of
disparity.** Human stereoacuity is on the order of arc*seconds* in ideal conditions and a few
arc-minutes in practice, so 2 arc-minutes is comfortably visible — but ±0.5 px is not
representable by an integer shift. **The disparity must be applied in floating point with
linear filtering**, or the depth ladder quantises into two or three visible steps and everything
between 0 and 0.3 looks identical. `SRenderModifData`/`m_floatingOffset` are `Vector2D` doubles
and the pass rounds only at the very end (`ElementRenderer.cpp:255`
`windowBox.scale(...); windowBox.round();`) — **that rounding is the thing to check first**, and
it may need to become a sub-pixel-preserving path for depth to feel continuous. Flag this as the
number-one implementation risk for D2.

### 8.2 Comfort limits — four numbers, and the one that actually binds us

The literature gives four budgets. Ordered from loosest to tightest, which is also the order in
which they stop mattering:

1. **The 1-degree rule.** Comfortable on-screen parallax ≤ **1° (60′)** of angular disparity.
   Attributed to Lambooij (per Terzić & Hansard, *Causes of discomfort in stereoscopic content:
   a review*, [arXiv:1703.04574](https://arxiv.org/pdf/1703.04574) — the best single citable
   aggregation of the folk rules). Mendiburu's **3 % of screen width** (*3D Movie Making*, 2009)
   and the **1/30 rule** (interaxial ≤ 1/30 of distance to the nearest subject ⇒ ≈1.9°, matching
   Brewster's original ~2°) are the same budget in other units. At D = 1.5 m, 1° ⇒ p ≈ 26 mm ⇒
   **≈31 px total** on our default quad.
2. **Shibata, Kim, Hoffman & Banks (2011), "The zone of comfort"** (*J Vis* 11(8):11,
   [doi:10.1167/11.8.11](https://jov.arvojournals.org/article.aspx?articleid=2121032)) —
   the measured version, and **asymmetric in a direction worth knowing**: comfortable limits of
   **2–3 % of screen width for crossed (toward-viewer) disparities and 1–2 % for uncrossed
   (behind-screen)** (figures via the Terzić & Hansard review; the paper's dioptric boundaries
   are behind a paywall and are **not** quoted here). Also: a conflict of a given dioptric
   magnitude is slightly less comfortable at far viewing distances, and the sign asymmetry flips
   with distance. **Design consequence: raise windows toward the viewer, do not push them
   behind the plane.** We were going to do that anyway; now it has a reason.
3. **VR-specific placement.** Meta's own VR Best Practices guide (citing Shibata) states *"the
   most comfortable range of depths … is between **0.75 and 3.5 meters**"*, that UIs should sit
   **2–3 m** away, and — the number that anchors everything — *"the current optics … are
   equivalent to looking at a screen approximately **1.3 meters** away"*, i.e. accommodation is
   pinned there regardless of what we do. Google's Daydream requirements add a minimum
   convergence distance of **> 0.5 m** and, notably for us, the explicit rule that **the cursor
   must render at the same depth as or nearer than the object it is over** (§5.4 was right).
4. **The one that actually binds: foveal fusion, ≈0.1°.** Terzić & Hansard: disparities up to
   ~0.5° can be fused in general, but **closer to 0.1° around the fovea** — and desktop windows
   are made of *text*, which is fixated foveally. 0.1° = 6′ ≈ **3 px total** at our defaults.

Reconciling (1) and (4) is the whole design question, and the resolution is that they measure
different things. Budget (1) is *how far the plane you are looking at may sit from the display
plane*: your eyes verge onto a raised window and fuse it as a single plane, so a uniformly
shifted window is fine well past 6′. Budget (4) is *how big a depth **step** may be between two
things in the foveal field at once* — a raised window's edge against the wallpaper behind it,
or two adjacent windows at different depths.

**Therefore the recommendation is not "keep depth small" but "keep depth STEPS small, and use
few of them":**

- Use a **small ladder of depth tiers** (0.0 background/wallpaper, 0.2 ordinary windows, 0.6
  focused window, 0.8 bar/overlay), not a continuum. Adjacent tiers differ by ~2–4′ — within the
  foveal fusion limit — while the total span stays around 12′, i.e. ~20 % of the 1° budget and
  well inside Shibata's 2–3 %.
- **Never place a large depth step across a boundary the user reads across**, e.g. do not raise
  a window that overlaps another window the user is diffing against.
- Time matters and the literature is about films, not 8-hour days. Hoffman, Girshick, Akeley &
  Banks (2008, *J Vis* 8(3):33) showed that *correct* focus cues improve fusion time,
  stereoacuity and fatigue — we cannot supply correct focus cues, so the conservative reading is
  to treat every published limit as a ceiling, not a target.

Two device notes:

- **XREAL Air 2 Ultra**: 46° per eye (`xreal_air_hmd.c:1173-1174`), collimated birdbath optics
  with a fixed virtual focal distance, no distortion mesh. The accommodation half of the conflict
  is fixed by the hardware; the vergence half is ours to keep small.
- **Quest 3 over WiVRn**: foveated re-sampling happens *after* composition
  (`research/21`: `compositor.cpp:411`), so a few pixels of disparity in the periphery may be
  partly resampled away. Depth will read strongest near the gaze centre — expected, not a bug.

### 8.3 Frame violation

Covered mechanically in §6.1 (including the vertical-vs-horizontal-edge asymmetry and the
Dynamic Floating Window). The operative rule from stereoscopic practice — ISU's second Golden
Rule — is: **no part of the spatial image may be cut by the stereo window, except free-floating
foreground elements.** Our "stereo window" is the quad's content rect; our floating-window
budget is the transparent chrome margin. v1: clamp each element's per-pane shift to
`≤ margin_px`, and prefer reducing depth near edges over clipping.

---

## 9. The full-XR upgrade path

The honest end state on the 6DoF path is not disparity — it is **per-window quads at real
depths**: each window its own `XrCompositionLayerQuad`, positioned in the room, with true
per-eye geometry, occlusion, and head-motion parallax.

`research/23` already costed the machinery this needs and found the wall: **a surface that lives
on no monitor plane stops receiving `wl_surface.frame` callbacks and freezes**
(`Renderer.cpp:2683-2695`), so per-surface extraction must either keep a monitor plane anyway or
re-implement damage scheduling, frame callbacks, an offscreen swapchain and pointer routing.
That analysis was for layer surfaces; it applies verbatim to windows, with the extra costs that
windows have decorations, tiling/floating semantics and popups.

**The good news is that the two tiers share a user-facing surface exactly.** `windowrule = depth
0.6` means "this window floats 0.6 above the plane". Tier 1 renders that as horizontal
disparity; tier 2 would render it as a quad displaced 0.6 · `depth_scale` metres toward the
viewer. Same config, same animation, same defaults — the user's rules light up progressively as
the implementation improves, and nothing has to be re-authored. That is the strongest argument
for putting depth in `windowrule` rather than in anything XR-specific.

Sequencing: **do not build tier 2.** Build tier 1, live with it for a month, and revisit only if
a concrete need appears that disparity cannot serve (the likely candidate: wanting to *place* a
window in the room independently of its monitor, which is a different feature — that is
`research/23`'s layer host generalised, and it should be driven by that need, not by depth).

---

## 10. Prior art

### 10.1 Stereo desktops — everyone tried, nobody shipped

- **Compiz/Beryl (2007-08)** had an experimental **Anaglyph plugin** that rendered the whole
  composited desktop red/cyan, with care taken to match per-eye brightness against ghosting. It
  was *whole-scene* anaglyph, not per-window disparity, and it died with the Compiz era. The
  famous Compiz 3D effects (Cube, Wobbly) are monoscopic perspective.
- **KWin**: KDE bug **335859** ("KWin composite + GLX quad-buffered stereo", filed 2014-06-06)
  produced an experimental branch from Fredrik Höglund in Dec 2014, self-described as "untested"
  and liable to "fail rather spectacularly". It was **closed RESOLVED INTENTIONAL in January
  2023** with the reasoning that Wayland has no stereo-buffer concept, X11 is frozen, and demand
  is too low. The pre-Wayland workaround was to *disable compositing entirely* so raw
  `GLX_STEREO` apps could reach quad buffers — i.e. the compositor was always the blocker.
- **NVIDIA's X `Stereo` option** exposed 14 stereo modes (quad-buffered, 3D Vision IR, HDMI 3D,
  DisplayPort in-band, interlaced/checkerboard variants) on Quadro hardware, and 3D Vision on
  Windows was per-*application*, never a stereo window manager. Final driver: **Release 418,
  April 2019**, with NVIDIA explicitly citing the shift to VR.
- **Windows 8 / DXGI 1.2** is the closest anyone came to real per-window OS stereo —
  `IDXGIFactory2::CreateSwapChainForHwnd` with a stereo swap chain, `IsWindowedStereoEnabled`,
  silent mono fallback — and app adoption was negligible.
- **Sun's Project Looking Glass** put windows at depths in 3D but monoscopically.

**Lesson we should take from all of it:** every previous attempt tried to make *the whole desktop*
stereo through a *display-level* mechanism (quad buffers, frame packing, anaglyph) and died on
the compositor/driver boundary. We are doing the opposite: the compositor is the stereo engine,
the display path is an OpenXR quad we already own, and the granularity is per-window. That is
the piece nobody had.

### 10.2 Injection-based stereo for flat games — the artifact taxonomy

Two families, and the tradeoff between them is the single most useful thing in this literature:

- **Depth-buffer reprojection (DIBR)** — Depth3D / SuperDepth3D on ReShade, vorpX's "Z-Buffer 3D".
  Warps one mono frame into two using the depth buffer. Universal, cheap, no per-title work.
  Documented artifacts: **disocclusion holes** (mitigated with a horizontal-tap fill kernel — it
  cannot invent unseen geometry), **halos** at depth discontinuities (upstream advice is to *mask*
  them with film grain), **HUD/UI splatter** (composited after depth, so it lands at wrong depths
  and needs per-game exclusion), and **skybox flicker**. vorpX's own comparison: *"almost twice as
  fast"*, near-universal compatibility, *"usually worse depth perception"*.
- **Geometry duplication** — NVIDIA 3D Vision, geo-11 (2022, built on 3Dmigoto), vorpX's
  "Geometry 3D". Every draw is issued twice; NVIDIA's archived docs give the exact clip-space
  footer, `PsInput.x += Separation * (PsInput_w - Convergence)`, which is also why objects at
  `w == Convergence` got zero separation — *that is how HUDs were pinned to the screen plane*.
  Correct, but every non-geometric pass (reflections, deferred composites, post, UI) must be
  found and patched per title, which is exactly why 3Dmigoto's "shader hunting" workflow exists.
  NVIDIA shipped per-game *stereo profiles* because the heuristics alone were not reliable.

**Why this matters to us:** our Q2 mechanism is neither of these. Re-compositing a layered scene
is closer to geometry duplication in correctness (no holes, no halos) and closer to reprojection
in cost (no per-title work), because our "geometry" is a handful of textured quads whose
compositing we already own. The entire artifact taxonomy above simply does not apply — **except**
the HUD lesson, which transfers exactly: NVIDIA pinned HUDs to the screen plane because UI at
the wrong depth is the most objectionable artifact. Our equivalent is the cursor (§5.4) and the
wallpaper (pinned at 0.0 in §7.3).

### 10.3 Modern XR OS design language for flat panels

The best-documented numbers, useful as sanity checks on §7.2/§8:

- **Android XR** ([spatial UI guidelines](https://developer.android.com/design/ui/xr/guides/spatial-ui))
  is the only one with a complete numeric spec: spatial panels live **0.75 m–5 m**, hold
  **constant apparent size from 0.75–1.75 m** and only then begin to shrink (0.5 m of scale per
  metre), **default spawn 1.75 m with the panel centre 5° below eye level**, optimal-comfort FOV
  **41°**, and — directly relevant to §7.3 — an explicit **elevation ladder in dp**: orbiter 16,
  popup 32, dialog 56, with orbiter Z-elevation 15 dp. *A discrete elevation ladder is what a
  shipping XR OS chose*, which is independent support for §8.2's "few tiers, small steps".
- **Meta Horizon OS** ([MR design guidelines](https://developers.meta.com/horizon/design/mr-design-guideline/)):
  **~45 cm** for direct-hand-interaction windows, **~70 cm** for hybrid hand+controller (paired
  with a grab affordance), **~1 m** for larger indirect-interaction screens. Compare our
  `openxr:default_distance` of 1.5 m — we are deliberately further out because our panels are
  desktop monitors, not app windows.
- **Apple visionOS**: two scale behaviours (Dynamic Scale — grows with distance to hold angular
  size; Fixed Scale — constant real size), guidance to place content *"slightly further away than
  arm's reach"* and to let users adjust depth, default window 1280×720 pt. **Caveat: the HIG is
  JS-rendered and could not be fetched directly**, so these are secondary; the widely-repeated
  "1–2 m window distance" could **not** be verified, and the one confirmed Apple 1.5 m figure is
  the ImmersiveSpace *safety boundary*, not a placement recommendation. Whether visionOS adds
  extra disparity to window *chrome* is **unresolved** — the reasonable inference is that it does
  not need to, because a visionOS window is a real 3D-placed quad and gets its disparity from
  world placement. **That inference is exactly the §9 tier-2 architecture**, and it is worth
  noting that the most polished spatial OS in existence went straight to real geometry rather
  than to disparity tricks.
- **SteamVR** ships no numeric depth guidance and its stereo "theater" mode exists only as an
  undocumented CLI toggle (`vrcmd --mailboxcmd systemui_dashboard toggle_theater_stereo`).

### 10.4 What shipping VR video players do about detection

Summarised in §4.3/§4.4; the headline is that **the entire ecosystem runs on filename tokens**
(DeoVR: *"DeoVR gets info from file names, so with the right naming convention you don't need to
set anything"*; HereSphere, Skybox, Pigasus, Plex, Kodi all the same), **Bigscreen, Virtual
Desktop, SteamVR and the Quest browser require a manual menu choice**, container metadata is
mostly ignored and sometimes actively harmful, and **exactly one shipping product (vorpX,
2026) does pixel-content detection — behind a toggle, with its author flagging false
positives.** Our tier ordering (cooperative tag → explicit rule → filename heuristic → optional
auto) is a strict superset of every one of them.

---

## 11. The degradation matrix

Reordered so the primary tier is first.

| Path | Tracking | Presenter | Q1 stereo content | Q2 depth desktop | Testable how |
|---|---|---|---|---|---|
| **XREAL flat, HID 3D mode, no runtime** | none (head-locked) | **flat SBS output (§3)** | ✅ after **F1+S1** | ✅ after **F1+D2** | XREAL, no runtime |
| **Any SBS display: 3D TV, other glasses** | none | **flat SBS output (§3)** | ✅ after F1+S1 | ✅ after F1+D2 | whatever the user owns |
| **An ordinary wide monitor, `stereo:sbs`** | — | **flat SBS output (§3)** | ✅ (panes visible side by side — the dev loop) | ✅ | **flat, at the desk** |
| **Quest 3 / WiVRn** | 6DoF | XR quad pair | ✅ free (F2) after X1 | ✅ (double composite) | Quest |
| **XREAL + Monado, windowed** | 3DoF | XR quad pair | ✅ free (F2, F3) after X1 | ✅ | XREAL |
| **XREAL + Monado, DRM-lease direct** | 3DoF | XR quad pair | ✅ free (F3) after X1 | ✅ | XREAL |
| **XREAL flat, HID 2D mode** | none | — | ❌ (nowhere to put the second eye) | ❌ | — |
| **Plain flat monitor, no stereo token** | — | — | ❌ by design, cost 0 | ❌ by design, cost 0 | — |
| **Headless (hyprtester)** | — | flat SBS output on a headless 3840 mode | ✅ structural assertions | ✅ structural assertions | **flat** (CI) |
| **Null runtime (hyprtester)** | — | quad array assertions only | ✅ assert 2 quads + rects | ✅ assert 2 panes | **flat** (CI) |

The top three rows are what the inversion buys: **the feature is now reachable, testable and
useful without a headset, without a runtime, and without `WITH_OPENXR`.** The third row in
particular is the development loop — an ordinary wide monitor in `stereo:sbs` shows both panes
side by side, so crop, disparity sign, cursor duplication and the edge clamp are all checkable
with your eyes. See §3.12 for what the harness can and cannot assert today (structural: yes;
pixel content: not without a new screencopy helper).

---

## 12. Risks and failure modes

1. **Half-submitted pair.** If the layer budget or an early `continue` drops one quad of a pair,
   the user gets content in one eye — instantly nauseating. Guard the pair atomically (§1 F2).
2. **Sub-pixel quantisation** (§8.1). If the render path rounds boxes to integers before the
   disparity is applied, depth becomes a 3-step staircase. Check `ElementRenderer.cpp:255` early.
3. **Wrong pointer mapping** (§5.6). Two producers, two mappings. Unit-test both.
4. **Monocular cursor** (§5.4, §3.7). The single most likely "why does this feel awful" bug —
   and on the flat presenter it is the *default* behaviour unless the hardware plane is
   explicitly locked off per output, so it is a guaranteed bug rather than a likely one.
5. **`stereo` effect + Lua config** (F8). If the effect lands on `Legacy::CConfigManager`'s
   store it will silently vanish for anyone on `.lua`. Put it in a shared manager.
6. **False-positive auto-detection** (§4.4). Do not enable by default.
7. **Damage regressions on non-stereo monitors.** Every change in §6.2/§6.3 touches the shared
   render path. The producer must be a hard no-op when no element has non-zero depth (§6.4.1),
   and that no-op needs a test.
8. **Chrome disappearing on stereo monitors** (§5.1). Expected in v1; document it, or the user
   will report a bug when they cannot grab a monitor showing a movie.
9. **Aspect confusion** (§5.2). `sbs` vs `hsbs` mis-declared gives a subtly-wrong image that is
   hard to name. Surface the resolved layout in `hyprctl monitors` (primary tier) as well as
   `hyprctl openxr status`.
11. **A display GUI silently un-stereos the monitor.** `wlr-output-management`'s write path sets
    the resolution from the head's mode list with no knowledge of the stereo token
    (`OutputManagement.cpp:384-392`), so one visit to `wdisplays` can drop the packing. §3.4
    item 15.
12. **A client scanning out unpacked.** If `wl_output.mode` keeps advertising the packed mode, a
    fullscreen client may submit a buffer that exactly matches `m_pixelSize`, satisfy the direct
    scanout check (`Monitor.cpp:2129-2133`) and be scanned out without packing — one image
    spanning both eyes. §3.6, plus the explicit `DS_BLOCK_STEREO` of §3.4 item 7.
13. **Capture regressions.** Screencopy/portal sizes come from `m_pixelSize`
    (`ScreenshareSession.cpp:116`); if item 14 is missed, every screenshot and every screen share
    of a stereo monitor silently becomes a packed double image. This will be reported as "OBS is
    broken", not as "stereo is broken".
10. **XREAL mode-switch ordering.** The driver latches stereo geometry at device-create time
    (`07-xreal.md:56-62`), and a non-native modeline stripes (`:36-54`). Nothing in this memo
    changes that, but any live test must follow the existing toggle discipline.

---

## 13. Work packages

Sizes: XS ≤ 50 lines, S ≤ 150, M ≤ 400, L ≤ 1000. **Device** column = where live validation
happens (flat = no headset needed).

**Rebalanced by the revision.** Four phases now, and the new **Phase F** comes first because it
is the primary presenter *and* the cheapest thing to validate. The old `S1` has been split: its
rule/declaration half moves earlier (it is generic config, not an XR effect), its quad-assembly
half moves later into **Phase X**. The old `D6` is deleted — it was the flat presenter, filed as
optional; it is now Phase F.

### Phase F — the flat SBS output (the primary presenter, generic Hyprland, no OpenXR)

| WP | Size | Device | What |
|---|---|---|---|
| **F0** | **0** | XREAL | **Zero-code spike.** `xreal-ctl mode 3d`, set DP-5 to the native `3840x1080@60` at `scale = 1.0`, and drive it as an ordinary monitor. Confirm: the glasses split scanlines as expected; a hand-made SBS test image fuses; `wl_output`/`xdg_output` values as-shipped; and — the open question — whether the 3D-mode scaler accepts a `1920x1080@120` signal at all (§3.8). Answers the mode trade before any code |
| **F1** | **M** | flat + XREAL | **The stereo output.** The pack: the derivation change at `Monitor.cpp:1100-1102`/`:734-736`, pane-sized projection/viewport/resources, the two-pane blit in `CHyprOpenGLImpl::end()`, the two-half damage fold, `DS_BLOCK_STEREO`, the solitary-client guard, `wl_output.mode`, fractional-scale validation, capture sizing, `hyprctl monitors` fields. The `monitor = …, stereo:sbs` token across both config front-ends. **Full checklist: §3.4, items 1–15.** Validate on an ordinary wide monitor first |
| **F2** | **S** | flat + XREAL | **The cursor** (§3.7). `lockSoftwareForMonitor` on entering stereo; eye-aware `renderSoftwareCursorsFor` (the `forceRender` guard at `PointerManager.cpp:653-656`, the single-box leftover damage at `:686-689`/`:292-298`, `damageIfSoftware` at `:797-816`); cursor drawn inside each composite. **Not optional** — without it the primary tier has a one-eyed cursor |
| **F3** | S | flat | **Tests.** gtests for the pane/pack derivation (incl. transform × scale × stereo combinations) and the damage fold. hyprtester: a headless `3840x1080` output with `stereo:sbs` reports one monitor at 1920×1080, a window on it gets 1920-wide geometry, damage covers both halves, and a `stereo:off` monitor is bit-identical to today. §3.12 for the pixel-readback caveat |
| **F4** | S | — | **Config surface + docs.** The `monitor = …, stereo:` token in the wiki-shaped docs and `05-configuration.md`; the `scale = 1.0` requirement; the capture/`wl_output.mode` semantics; the display-GUI hazard (§3.4 item 15); an `example/` snippet for the XREAL |
| **F5** | S | XREAL | **Layout generality** (§3.8): `hsbs`, `tab`, `htab` as destination-box variants, plus a `pane:WxH` override. Falls out of F1's design; separate WP only because each needs its own eyeball check |

### Phase S — stereo content (Q1), on the flat presenter

| WP | Size | Device | What |
|---|---|---|---|
| **S0** | **0** | XREAL | **Zero-code spike.** Put a known SBS video fullscreen and confirm the current build shows it doubled (it will). Confirm the mod's SBS output and the 3840 mode line up. Answers "is full-SBS at 3840 lossless end to end" |
| **S1** ✅ | **M** | flat | **The stereo declaration and the Q1 producer.** `windowrule = stereo <layout>` as a generic window-rule effect (7 lines × 5 files + the Lua mirror, §7.1) matching `xdg_tag`/`class`/`title`/`content`; per-window UV crop in `calculateUVForSurface` (F7) so the tagged window samples a different half per pane; the "fullscreen-on-this-monitor unless overridden" negative heuristic (§4.3). **Stored in a shared config manager** (F8). Visible immediately on F1's flat presenter |
| **S2** ✅ | S | flat | **Tests.** gtests: aspect/`k` from the destination box (`presentedAspect`), pane↔content UV mapping both directions (§5.6), and the precedence table as a pure function (`resolveDeclaration`, extracted from `CWindow::stereoLayout()`) — 700 → 718. hyprtester: `stereoTaggedWindowSamplesOneHalfPerPane` proves the crop **in pixels** via a new `screencopy-probe` client and a four-quadrant `xdg-interactive --paint` (§3.12), with an untagged control and the mono-monitor degradation; `stereoRuleFoldAndProvenance` covers the fold above the pure part. Stereo suite 6 → 8, headless on the host |
| **S3** ✅ | S | — | **Config surface + docs.** `05-configuration.md` §8.6 carries the whole content story: the layout table, the fullscreen gate and `always`, the exact `xdg-toplevel-tag-v1` grammar as a stated compatibility contract (with a mod-facing paragraph — one call, set it early, no suffixes, `stereo:auto` invalid), the HSBS sharpness caveat (~21 px/° against ~42 on an XREAL), the `hyprctl clients` `stereo` / `hyprctl monitors` `stereoContent` fields, the heuristic regex block, and what it deliberately does not do. `example/xreal.conf` ships the same three tiers commented out |
| **S5** ✅ | XS | — | **`contrib/mpv-hypxr-stereo.lua`** (§4.5) — observes `video-params/stereo-in` and tags mpv's own window (`hyprctl dispatch tagwindow ±stereo-sbs pid:<self>`), which four static `match:tag` rules turn into a layout. **Non-XR by construction**: a window tag and a windowrule, no `hyprctl openxr`. A tag change re-evaluates rules in place (`Actions::tag` → `propertiesChanged(RULE_PROP_TAG)`), and since the containers never record half-vs-full the script infers it from the display aspect. Advisory: it only ever adds a tag, so a hand-written `stereo off` still wins |
| **S8** | M | flat | **`stereo:auto`** (§4.4): downsampled NCC detector, fullscreen precondition, hysteresis, decision surfaced in status. Default **off** |

### Phase D — the depth desktop (Q2)

| WP | Size | Device | What |
|---|---|---|---|
| **D0** | **0** | XREAL/Quest | **Ergonomics spike, no code.** Two static monitors 12 cm apart in depth and a day of use. Answers: is 12 cm the right `depth_scale`? Is a raised *bar* pleasant or distracting? Freeze §7.2's defaults after this, not before. **Now runnable on the flat presenter too** once F1 exists — two SBS test images at different disparities, no headset |
| **D1** | S | flat | **`windowrule = depth` + `layerrule = depth`** — the rule effects and storage, no rendering. `CWindow::m_depth` / layer equivalent as animated floats, the `windowsDepth` animation node, `decoration:depth_*` defaults. Observable via `hyprctl clients` |
| **D2** | M | flat | **The producer.** Second composite into a per-monitor pane buffer with the eye sign in render data; depth folded in at the 14 `m_floatingOffset` sites; the edge clamp (§6.1); the "no depth anywhere ⇒ single pane" fast path (§6.4.1); the cursor drawn per pane (F2); `m_forceFullFrames` while active. **Sub-pixel check (§8.1) happens here.** Lands on F1's presenter — no headset in the loop |
| **D4** | S | flat | **Tests + docs.** gtests for disparity math and the clamp; hyprtester for the single-pane fast path; wiki-shaped docs for `depth` |
| **D5** | S | XREAL/Quest | **Polish.** Cursor depth easing; drag lift; special-workspace depth; per-monitor `depth_scale` override; a `socket2` event when a monitor enters/leaves stereo |
| **D7** | S | — | **Lua-config migration** of `xrrule`/`xrmonitor` into a shared manager + `hl.xr_rule{}` binding (F8). Not strictly this project's job, but 0.57 makes it mandatory |

### Phase X — the OpenXR upgrade tier

| WP | Size | Device | What |
|---|---|---|---|
| **X1** ✅ | S | Quest + XREAL | **The quad pair.** The quad-assembly change (pair emission, `eyeVisibility`, `imageRect`, pair-aware budget check, pair-aware depth sort), aspect `k` (§5.2), the pane-pair declaration published to the frame thread as atomics, chrome suppressed while stereo ≠ off, `xrmonitor`/`xrrule` tokens as a *convenience* layer over the generic state. Under 100 lines because §3 and Phase S did the producer |
| **X2** ✅ | S | flat | **Tests.** hyprtester (null runtime): a stereo monitor submits exactly 2 quads with the expected rects and eye bits; `stereo off` submits 1; the pair is never half-submitted |
| **X3** ✅ | S | Quest + XREAL | **The XR presenter for depth panes.** Double-wide swapchain, two `blitBuffer` calls, pair submission (reuses X1); pointer un-mapping for the depth producer (§5.6); the XR ray cursor drawn per pane with content-depth disparity |
| **X4** ✅ | M | Quest | **Chrome in stereo.** Double-wide swapchain with two margined panes, `drawChrome` per pane, per-pane grab geometry. Removes X1's chrome suppression |

> **STATUS (WP X1 + X2): IMPLEMENTED.** The pair is emitted from the quad-assembly loop, and the
> shape it took confirms §5.1(a)'s central claim — **there is no producer at all**. A fullscreen
> stereo client's packed frame already IS the swapchain's content rect, so nothing is rendered
> twice, no swapchain is reallocated when stereo engages or leaves, and the whole feature is an
> `imageRect`, an `eyeVisibility` and an aspect. `Render::Stereo::presentedPaneSize` (factored out
> of `presentedAspect`) feeds the anchor solve, so §5.2's `k` finally has the caller S2 reserved it
> for. Chrome and the XR ray cursor are suppressed while paired, per option 2.
>
> **Four things this WP had to add that §13's line item did not name**, all of them found by
> writing it:
>
> 1. **A coverage gate the XR tier has to ask for itself.** `CWindow::stereoLayout()` already
>    applies §4.3's fullscreen gate — but `always` and a client's own tag both legitimately *skip*
>    it, and the quad pair splits the monitor's WHOLE content rect rather than one window's texture.
>    A floating `always`-declared window would therefore send half the desktop to each eye. The gate
>    is a size/position match against the output (`SC_TRANSFORM`'s comparison), deliberately **not**
>    `m_solitaryClient`: solitary also drops out for notifications, DND, fadeouts and overlay
>    layers, and since pairing can change the quad's aspect, riding it would let a toast visibly
>    reshape the panel and reshape it back.
> 2. **The pointer un-map is mandatory, not X3 polish.** A paired quad shows one PANE, so a ray hit
>    is a pane uv while absolute injection wants packed-image coordinates — §5.6's "off by half a
>    screen", reached the moment the first pair is submitted. `paneUVToContentUV` (S2) is the fix and
>    `SXRPointerTarget` grew a layout field to carry it.
> 3. **`imageRect`'s origin is bottom-left for our swapchains, and it was worth proving.** A GL
>    client submits through `comp_gl_client.c`, which sets `flip_y`; `set_post_transform_rect` then
>    rewrites the normalized rect to `(x, y+h, w, -h)`, so `layer_quad.vert` samples the quad's top
>    row at `offset.y + extent.h`. This decides only which half an over-under pack gives the left
>    eye — a mistake swaps the eyes, which is uncomfortable without ever looking broken.
> 4. **Declared ≠ submitted, so status reports both.** A pair the layer budget refuses is dropped
>    from the frame, which looks exactly like a monitor that simply is not there. `hyprctl -j openxr`
>    carries `stereo` (what the main thread declared) *and* `quads` (what the frame thread actually
>    submitted: 0, 1 or 2, zeroed per frame so it can never read stale). The budget check is
>    per-PAIR and `continue`s rather than breaking, so a refused pair leaves the slot to a cheaper
>    monitor instead of half-submitting a left-eye-only frame.
>
> Deviations from the line item: the **depth sort needed no change** — pairs are pushed back to back
> inside one loop iteration and the sort orders layers, not quads, so adjacency is structural. And
> the `xrmonitor`/`xrrule` convenience tokens were **not** built: the generic `windowrule = stereo`
> already reaches XR monitors through Phase S, so an XR-only alias would be a second way to say the
> same thing (and `xrrule`'s enum-valued effect fold is still the open F8/D7 work). X3 and X4 remain
> as scoped.
>
> Attended Quest validation arrived on 2026-08-14: the depth desktop visibly raised a focused
> terminal, and the tagged full-SBS viewpoint demo resolved correctly through the per-eye path.
> XREAL validation remains open, including §14's question 1 about its scanline split interacting
> with a half-covering quad.

> **STATUS (WP X3 + X4): IMPLEMENTED**, shipped together because splitting them would have shipped
> a regression — see point 3. `openxr:depth_desktop`, default ON.
>
> The headline is that **X3 barely exists as rendering work**, and for the mirror-image reason X1
> barely existed. X1 found there was no producer; X3 found the producer was already written. Phase D
> composites a stereo monitor once per eye with the eye sign in render data, Phase F packs the panes
> at the final blit, and *neither looks at the backend* — both gate on `CMonitor::isStereo()`. So the
> XR presenter for depth panes is: make the XR monitor a stereo output. §6.2's "this needs no change
> to the monitor's mode" is the one line of the memo this WP had to contradict, and doing so is what
> the rest of it is about.
>
> **Six things the line items did not name:**
>
> 1. **The pack has TWO directions, and the token only knows one.** `monitor = …, stereo:sbs` names a
>    mode a panel really has and HALVES the logical desktop out of it. An XR output has no panel: it
>    invents its scanout, so the fixed quantity is the size the user declared and the mode must be
>    DOUBLED into existence. Halving a declared XR size would shrink every XR desktop the moment
>    depth engaged — a silent reflow of the whole session, far worse than the feature is good. Hence
>    `CMonitorRule::m_stereoVirtualMode` and `Monitor::Stereo::requestedMode`, the one place the two
>    kinds differ; everything downstream is shared. Three behaviours had to learn the difference and
>    all three for the same reason (no physical per-eye pixel grid): the scale is no longer pinned to
>    1.0, the scale ≠ 1 warning does not fire, and §3.4 item 15b's mode-list watch stays disarmed —
>    a headless output has no EDID to fall, and `watchAction` would read its absence as a fall and
>    re-modeset it every second forever.
> 2. **Precedence went the other way from §13's line item.** The item assumed a fullscreen stereo
>    client on a depth monitor would keep X1's cheap content pair ("the packed frame IS both eyes").
>    It cannot: on a packed monitor the swapchain content is already two panes, and splitting it a
>    second time halves the desktop. Recovering the cheap path would mean dropping the pack when a
>    window goes fullscreen — a **modeset on every fullscreen toggle of a 3D video**, which is worse
>    than the composite it saves. So the DEPTH producer owns the pair whenever the monitor is packed
>    and Phase S's per-surface crop un-packs the window *inside* the composite. That is strictly
>    better than the trade it replaces: it works while the window is merely WINDOWED (§5.3's general
>    case, which X1 structurally cannot do), it keeps chrome and cursor, and the quad never changes
>    shape — `presentedAspect` of a full pack is the monitor's own aspect, so `hsbs` no longer
>    reshapes the panel on the way into fullscreen. X1's tier is what an unpacked monitor does, and
>    `openxr:depth_desktop = 0` is how you ask for it.
> 3. **X4 is not polish, it is the thing that makes X3 shippable.** X1 suppressed chrome for a
>    fullscreen film, which is a real trade. The same suppression on EVERY monitor removes the
>    primary grab affordance from the whole session. The geometry is the whole of it: a depth
>    swapchain is **two independently margined panes**, because one ring around a double-wide content
>    rect puts the left eye's right-hand margin inside the right eye's picture. That makes each eye's
>    `imageRect` simply its half of the image, and it costs `blitBuffer` a loop — a contiguous
>    double-wide source cannot land in two SEPARATED destination rects with one draw. The hit
>    classifier needed **nothing**: it already works in full-quad uv, and each eye's quad IS one
>    margined pane, at the per-pane meters the anchor already solved.
> 4. **§5.6's "the disparity must be subtracted" has nothing to subtract**, and it is worth writing
>    down because the phrasing sends you looking for a hit test. The two eye quads are COINCIDENT —
>    one pose, one size — so a ray crosses them at one point, and what the wearer fuses there is the
>    cyclopean image, which sits at zero disparity by construction (pane 0 shifts a raised window
>    +s, pane 1 −s). The un-shifted desktop coordinate is exactly what they are pointing at, so the
>    DEPTH un-map is the identity, full stop. The disparity enters on the way OUT instead: the ray
>    cursor is DRAWN into each pane with the shift of whatever it is over (§5.4), eased over ~80 ms.
>    Same fact, other side.
> 5. **The declaration must carry the mode it describes.** A monitor mid-mode-change has a swapchain
>    from before and a declaration from after, and splitting across that mismatch shows each eye half
>    of a mono desktop. So the published word is one 64-bit `SPairDecl` (producer, layout, submit,
>    modeW, modeH) and the frame thread checks `describes()` before pairing. X1 needed none of this —
>    its swapchain never changes size when the pair engages — which is why the guard is opt-in on the
>    mode fields rather than mandatory.
> 6. **The kill switch degrades differently on a packed monitor.** `openxr:stereo_quad_pair 0` cannot
>    unpack it (that is a modeset), so it submits ONE quad of **pane 0** — a mono desktop at the right
>    shape. Submitting the whole image would put a side-by-side picture in both eyes, which is not a
>    degradation anyone can work in. Hence `submit` is a separate field from `layout`: the layout is a
>    property of the pixels, the switch is a property of the submission.
>
> **Cost, honestly.** The buffers are double-wide for the whole session, because sizing them per
> frame would mean a modeset every time a window took focus. So the fixed cost is a doubled blit and
> roughly **+100 MB per 2560×1440 monitor** (the XR swapchain and the output's own swapchain both
> double, ~44 MB each at three images, plus one pane-sized work buffer), and §6.4.1's fast path
> governs only the SECOND COMPOSITE — which, with `depth_unfocused` at 0.2, means any monitor with a
> window on it pays it. §6.4's mitigation 1 is therefore worth less on the XR tier than the memo
> expected: it saves the composite, never the pack. What does NOT change is the encode — the runtime
> renders its own eye views at their own resolution, so a WiVRn link sees no extra work. Two knobs,
> in order: `decoration:depth_scale = 0` (same ladder, no rise, one composite, still a pair) and
> `openxr:depth_desktop = 0` (no pack at all).
>
> **Observability** grew a third field for the same reason X1 needed a second: `stereo` is the split,
> `stereoProducer` is who made the panes (it decides the pointer un-map, so it is the field to check
> when the cursor lands somewhere odd), `quads` is what was submitted, and `chrome` is whether the
> chrome pass ran — otherwise "my monitor stopped being grabbable" is a bug report with nothing to
> look at.
>
> **Attended proof, not ergonomic certification.** On 2026-08-14 the wearer confirmed on Quest 3
> that a focused terminal was visibly raised from neighboring desktop content. That establishes
> that the depth pair and disparity reach the headset. It does not answer §14's open questions 4
> (`depth_scale` default), 11 (reading at a rung), comfort over time, capture behavior, or XREAL
> tuning; D0's systematic ergonomics pass remains open.

**Suggested order:** F0 → **F1** → F2 → F3 → F4 → S0 → **S1** → S2 → S3 → S5 → **ship Q1, flat**
→ X1 → X2 → **ship Q1, XR** → D0 → D1 → **D2** → D4 → **ship Q2, flat** → X3 → D5 → F5 → X4 →
D7/S8 as appetite allows.

Note what that order buys: **everything up to "ship Q1, flat" needs no headset, no runtime and no
`WITH_OPENXR`** — it is checkable on an ordinary wide monitor and confirmable on the glasses in
SBS mode. The XR tier then lands as a small delta on a producer that is already proven.

Rough totals: **Phase F core (F1–F3) ≈ 450–650 lines**, dominated by F1's fifteen touch points
and F2's cursor work. **Phase S core (S1–S3) is IMPLEMENTED** — S1, S2, S3 and S5 all landed, so
Q1 on the flat presenter is complete end to end: declaration, crop, tests, docs and the mpv
helper. **Phase D core (D1–D2, D4) is IMPLEMENTED**; the remaining D work is ergonomic tuning and
optional polish/generalization. **Phase X (X1–X4) is IMPLEMENTED.** X1 + X2 came in at the low end: the submission change is
~90 lines in the frame loop, on top of a ~135-line pure header and its tests. X3 + X4 overran it,
but not where the estimate expected — the pane geometry and the second producer are ~180 lines of
pure header, and the largest single piece is not OpenXR at all but the two-directional pack in
`CMonitor` (item 1 of the STATUS block above).

---

## 14. Open questions

1. **Does the XREAL's hardware L/R scanline split interact badly with a *quad* that only covers
   half of each eye's viewport?** It should not — Monado composites into the 3840 target and the
   glasses split scanlines regardless of content — but it is the one runtime assumption in this
   memo that has not been observed live. S0 answers it.
2. **What does the Dead Space mod actually emit** — full SBS at 2× width, or half-SBS at the
   monitor's width? It changes only `k` (§5.2), but it decides which is the *default* when the
   tag says just `stereo`.
3. **Sub-pixel disparity** (§8.1): does the current pass round window boxes to integers before or
   after the offset is applied, and if before, how invasive is preserving fractions?
4. **`depth_scale` default.** 0.12 m is a literature-derived guess at ~21 % of the 1° budget.
   D0 should confirm it in-headset on both devices; the XREAL's fixed focal distance may want
   less.
5. **Should depth apply on flat monitors as a shadow/scale cue?** A window at depth 0.6 could get
   a bigger shadow on a non-stereo output, so rules are not device-conditional. Tempting;
   possibly annoying. Decide during D1.
6. **Per-monitor vs per-eye `blackalpha`.** Under passthrough, does a keyed pane pair read as one
   floating panel or as two? Cheap to check once S1 exists.
7. **What happens to `xrrule = stereo …` when the monitor is unplugged/doffed** and re-plugged
   mid-video? The effect fold re-evaluates on plug (`OpenXRManager.cpp:2280`), so it should
   restore — but the swapchain size changes with the mode, and the pane geometry rides on it.
8. **Layer budget in a heavy session.** Three depth monitors + a stereo game monitor = 8 quads
   against a floor of 16. Fine today; worth a warning in status if a pair is ever dropped.
9. **Does WiVRn's foveation attenuate small disparities enough to matter** (§8.2)? Measurable
   with a disparity ramp test image.
10. **Upstreamability of `windowrule = depth`.** It is a plainly useful generic feature for
    anyone with a 3D display, and it has no XR in it. After the inversion the flat presenter
    (Phase F) *is* the thing mainline users would have to see it with, so the sequencing question
    dissolves — but §3.11's evidence says the appetite question does not. Revisit after months of
    daily use, not after D4.
11. **Is the ≈0.1° foveal fusion limit the right constraint for *reading* on a raised window?**
    The stereoscopic-text literature is thin: one lexical-decision study is commonly cited for
    an "effective fusional range of about one character space", but the primary source could not
    be pinned down, and **no dedicated study of text-panel disparity versus reading speed or
    fatigue surfaced at all.** This looks like a genuine gap. Do not invent a number; measure it
    during D0 by reading a page at each tier for five minutes.
12. **Does the tier ladder survive a busy screen?** Four tiers is fine with three windows. With
    ten overlapping floating windows, adjacent-in-space windows may be several tiers apart, which
    is exactly the depth-step case §8.2 warns about. Possible mitigation: derive depth from
    *stacking order proximity* rather than a flat category, so neighbours are always one step
    apart. Decide after D0.
13. **Should the mpv script set a per-*window* stereo rule rather than a per-monitor mode?**
    **Answered by the inversion: yes, per-window, and it is now the primary form** (S1). The
    generic `windowrule = stereo` works with no runtime and under both config front-ends, so the
    script has something to call that always exists. The old sequencing worry — that per-window
    stereo needed the depth producer first — was an artefact of assuming the XR presenter came
    first.
14. **Does the XREAL's 3D-mode scaler accept a non-3840 signal?** The half-SBS-at-120 Hz trade
    (§3.8) hangs entirely on this, and the evidence in-tree points both ways: the glasses
    re-present a native 3840×1080@60 EDID after the HID switch and non-native timings stripe
    (`07-xreal.md:36-42`), yet `1920x1080@120` is a proven native mono timing
    (`scripts/xreal-mode.sh:68`). **F0 answers it in ten minutes with no code.**
15. **What should `wl_output.mode` say?** §3.6 recommends the pane size, with the real modes still
    honest in `wlr-output-management` and `hyprctl monitors`. The counter-argument is that some
    tool somewhere reasons about `wl_output.mode` as the DRM mode. Nothing in-tree does; watch for
    it in XWayland-heavy and screen-recording workflows during F1 dogfooding.
16. **Should `stereo` be per-monitor only, or should a stereo *workspace* be possible?** Out of
    scope for v1, but worth noting: with the pack living at the final blit, "this workspace is
    stereo and that one is not" is expressible on the same output without a mode change. If it
    turns out the user wants to leave the glasses in 3D mode permanently and toggle stereo per
    task, that is the cheap version of the feature.
17. **Does anything break when a stereo monitor is unplugged and replugged, or DPMS-cycled?** The
    derivation runs in `applyMonitorRule`/`applyMonitorRuleSoft`, both of which re-run on connect
    (`Monitor.cpp:368-369`, `:749`), so it *should* restore — but the resources are reallocated at
    a size that depends on the token, and the plugged-state handling in this tree has a history of
    ordering bugs (`Monitor.cpp:340-355`). Test it explicitly in F3.
