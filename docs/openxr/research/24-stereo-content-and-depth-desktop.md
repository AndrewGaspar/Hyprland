# Research: stereoscopic 3D content + a depth-styled desktop

**Status:** research / decision-support. **Nothing is implemented.** This memo answers two
questions that turn out to be the same question:

- **Q1 — stereo content.** When a window or a monitor contains side-by-side / half-SBS /
  over-under stereo content (the user's Dead Space mod, a 3D movie in mpv, a YouTube 3D clip),
  HypXRland should present it as *true* stereo in the headset: left half to the left eye, right
  half to the right eye, at the right aspect ratio.
- **Q2 — the depth desktop.** Desktop chrome with real depth: the focused window subtly but
  decidedly raised off the monitor plane, the status bar hovering above the page, "depth" as a
  first-class styling axis beside border colour and width, animated — and degrading gracefully
  down the stack to the head-locked XREAL SBS path, without a full per-window OpenXR scene.

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
- **External** (surveyed for §3.2/§3.5/§7.2/§9; URLs inline): the 2017 `zwp_stereoscopy_v1`
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
> | | | **XR**: two `XrCompositionLayerQuad`s at the same pose, `eyeVisibility = LEFT`/`RIGHT`, each with its own `subImage.imageRect` |
> | | | **Flat SBS**: a "stereo mirror" — blit the two panes into the two halves of a 3840-wide scanout |
>
> **The single load-bearing finding is that the XR presenter is nearly free.** HypXRland does
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
> any layer** (§3.2): the one Wayland protocol that was written and implemented in Weston
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
> 2. **Explicit user rules and verbs** for everything that will never cooperate:
>    `xrrule = stereo sbs, focusclass:^(mpv)$ fullscreen:1` (a new `xrrule` *effect*, reusing the
>    shipped condition set verbatim) plus `hyprctl openxr stereo <name> sbs|hsbs|tab|htab|off|auto`.
> 3. **Title/filename heuristics** as an opt-in convenience rule set shipped in
>    `example/openxr.conf`, not as compositor logic.
>
> **Auto pixel-correlation detection is rejected for v1** — not because it is hard (a 32×18
> mip-level SAD is ~free on the frame thread) but because the false-positive set is exactly the
> user's daily desktop: a tiled editor, a two-pane terminal, a diff view and a symmetric
> wallpaper all correlate strongly across the vertical midline, and the failure mode
> ("half my desktop just went to one eye") is a *session-ruining* one, not a cosmetic one. §3.4
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
> of injection stereo (Depth3D/ReShade holes, halos, HUD splatter — §9.2) simply does not apply.
>
> **The comfort constraint is not what it looks like** (§7.2). Tasteful depth is *single-digit
> pixels* of disparity, so the first implementation risk is sub-pixel quantisation, not
> eye strain. And the binding limit is not the 1° whole-scene rule — it is the **≈0.1° foveal
> fusion limit**, which constrains the *step* between adjacent depths, not the absolute offset.
> Hence: a **small ladder of depth tiers** (0.0 / 0.2 / 0.6 / 0.8), not a continuum — which is
> also, independently, what Android XR shipped (16/32/56 dp elevation). Raise toward the viewer,
> never push behind the plane: Shibata et al. measured 2–3 % of screen width of headroom crossed
> versus 1–2 % uncrossed.
>
> **Where the user's "even head-locked SBS should enjoy this" hope holds and where it bends:**
> it **holds** — the XREAL runs the same OpenXR quad path, so the XR presenter covers it with no
> extra work, and 3DoF costs nothing here because binocular disparity is not motion parallax.
> It **bends** in exactly two places: (a) a *non-OpenXR* flat-SBS output needs a monitor whose
> logical width is half its pixel width, which Hyprland cannot express — solvable, but only via
> the stereo-mirror detour in §5.5; and (b) the depth desktop costs a **second full composite of
> the monitor per frame** and largely defeats partial-damage repaint. Neither is a wall; both
> are line items.
>
> Order: **S1 → S2 → S3** (stereo content, XR presenter, ~2 weeks of evenings) then
> **D1 → D5** (the depth desktop). S1–S3 are Quest- *and* XREAL-testable and give the user's
> Dead Space mod true stereo before any of the depth work starts.

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
content, §4.2.)

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
choose to crop at blit time rather than via `imageRect` (§4.1 argues for `imageRect`).

### F5 — XR monitors are headless, so the cursor is *in* the buffer; flat monitors default to a hardware plane

XR monitors are created as headless Aquamarine outputs (`OpenXRManager.cpp:2159-2183`), which
have no cursor plane, so the software cursor is composited into the frame the XR side blits.
On a real output, `CMonitor::shouldUseSoftwareCursors()` (`Monitor.cpp:2345-2364`) returns false
by default (`cursor:no_hardware_cursors` defaults to `2` = "SW only on nvidia+mgpu/VRR",
`ConfigValues.cpp:589`), and `renderSoftwareCursorsFor` bails immediately when the hardware
cursor is live (`PointerManager.cpp:647-651`).

**Both facts bite.** On the XR path the desktop cursor is drawn *once*, at one monitor
coordinate, which lands in exactly one pane — a **monocular cursor**, which is one of the most
reliably uncomfortable things you can put in a stereo display. On a flat SBS output the hardware
cursor would appear once, in one half, for the same reason. §4.4 handles both; the flat fix is
`Pointer::mgr()->lockSoftwareAll()`, whose exact precedent is the zoom code at
`Renderer.cpp:2091-2098`.

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
See §3.2 for the exact convention to adopt.

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
(`ElementRenderer.cpp:446`, `:475`). Two caveats that will bite (§5.3): it is **not** applied to
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
window's primary surface to `[0,0.5)` or `[0.5,1)`. ~10 lines, and it answers §4.3's question
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
                                            │ XR:   2 quads, eyeVisibility L/R,      │
                                            │       subImage.imageRect = each pane   │
                                            │ FLAT: blit panes into halves of a      │
                                            │       3840-wide SBS scanout (§5.5)     │
                                            │ NONE: flat monitor — pane R discarded  │
                                            └────────────────────────────────────────┘
```

Three properties of this framing earn it:

1. **It makes Q1 and Q2 the same feature.** The user asked two questions; the compositor gets
   one mechanism, one config vocabulary, one test surface.
2. **It composes.** A stereo game window on a depth desktop is "pane *i* = composite with eye
   sign *i*, in which that one window samples half of its buffer" (F7's UV crop). No special
   case.
3. **It degrades by dropping a stage**, not by branching. A flat monitor simply has no
   presenter and never asks for pane R, so the producer never runs and the cost is exactly zero.

The *declaration* that a monitor is a pane-pair monitor is per-monitor state on the XR layer /
monitor, resolved on the main thread and read by the frame thread as a plain atomic — exactly
the `publishBlackAlphaTuning` / `m_fxAlpha` discipline already in force
(`OpenXRManager.cpp:3410`, `:3554-3560`; `XRMonitorLayer.hpp` thread-safety block). No strings
cross to the frame thread (`MEMORY.md`'s session-killer rule).

---

## 3. Q1 — detecting stereo content

### 3.1 The tiers, and what each is for

| Tier | Mechanism | Reliability | Who it serves |
|---|---|---|---|
| **A. Cooperative** | client sets `xdg-toplevel-tag-v1` to `stereo:<layout>` | exact | the Dead Space mod; any app the user can patch; future upstream apps |
| **B. Explicit** | `xrrule = stereo …` / `hyprctl openxr stereo …` / a keybind | exact, manual | mpv/VLC/browser, per-session toggles, "this movie is over-under" |
| **C. Heuristic** | title/filename regex, `content:video`, fullscreen state | good-ish | shipped as *example config*, not compositor logic |
| **D. Automatic** | split-half image correlation on the composited frame | dangerous | deferred behind `stereo:auto` (§3.4) |

Tiers A and B are the recommendation. C is a config file. D is designed but not built.

### 3.2 The cooperative channel — adopt `xdg-toplevel-tag-v1`, do not invent a protocol

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

`stereo:sbs` and `stereo:hsbs` differ **only in aspect handling** (§4.2), which is why the
distinction has to be declared and cannot be inferred from pixels.

**Upstreamability.** The tag convention costs upstream nothing (it is a string), and the
*general* feature — "a compositor may present a tagged toplevel as stereo" — is a plausible
future wayland-protocols conversation but not one to start now. The right upstream contribution
from this work, if any, is a `windowrule = stereo` in mainline Hyprland for people with 3D TVs;
that is a §5.5 by-product, not a prerequisite.

### 3.3 Heuristics (tier C) — ship as config, never as compositor logic

**This is not a guess about what people name files — it is what the entire shipping VR-video
ecosystem actually runs on** (§9.2): DeoVR, HereSphere, Skybox, Pigasus, Plex and Kodi all
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

### 3.4 Automatic detection (tier D) — designed, deferred, and here is why

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

### 3.5 External metadata channels — the file knows, so ask the player

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
that observe `video-params/stereo-in` on `file-loaded` and run
`hyprctl openxr stereo <monitor> hsbs|sbs|tab|off` give the user *exact* detection for every 3D
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

## 4. Q1 — presentation mechanics

### 4.1 The crop: `imageRect`, not a shader

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
   is a one-liner. **Recommend this for S1**, with option 1 as S6.
3. Submit a third `BOTH`-eye quad for the chrome. Rejected: it would float the chrome at the
   screen plane while the content sits at the quad plane, which reads as broken.

### 4.2 Aspect correction — the whole difference between `sbs` and `hsbs`

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

### 4.3 Per-window stereo on an otherwise-mono monitor — yes, and cheaply

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
windows** — and it composes with depth (the window can be raised *and* stereo). The catch is
that it only pays off when the monitor is already producing a pane pair (Q2's double composite),
because otherwise there is only one pane to sample into. So: **per-window stereo is a natural
by-product of the depth desktop, not of Q1.** Sequence it after D2, not in S1.

### 4.4 The cursor

Three cursors are in play, and each needs a decision.

| Cursor | Today | With a pane pair |
|---|---|---|
| **Desktop pointer** (software, composited into the frame on headless XR outputs — F5) | one copy at one monitor coordinate | With Q1's crop it lands in **one eye only** → suppress it on a monitor in a client-stereo mode and let the XR-side cursor take over. With Q2's double composite it is drawn once per pane, so it works automatically **if** the second composite includes it (`renderSoftwareCursorsFor` takes an `overridePos`, `PointerManager.cpp:641`, precedent `ScreenshareFrame.cpp:312`) |
| **XR ray/endpoint cursor** (`drawCursor`, `XRGraphics.cpp:797`) | drawn once into the swapchain at the ray-hit uv | draw **once per pane**, with an added disparity so it sits at the depth of what it is over. Trivial: it is already a scissored quad at a computed uv (`:855-864`) |
| **Hardware cursor plane** (flat outputs only) | default on (`Monitor.cpp:2345-2364`) | must be forced to software while a flat SBS presenter is active — `Pointer::mgr()->lockSoftwareAll()` (`PointerManager.cpp:64`), zoom's exact pattern at `Renderer.cpp:2091-2098` |

**Cursor depth is not optional.** A cursor drawn at zero disparity over a raised window sits
*behind* the thing it is pointing at, which reads as a depth-conflict and is genuinely
unpleasant (it is the classic "subtitle behind the object" problem from stereo cinema). Give the
cursor the depth of whatever it is over: the hit-test that resolves that already runs every
frame (`ViewHitTester`, and on the XR side `processPointer`'s target classification). Recommend
easing the cursor's depth (a short 60–100 ms ramp) so crossing a window edge does not snap it.

### 4.5 Interaction with the shipped effect stack

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
- **Chrome / grabs.** §4.1: suppress in v1.

### 4.6 Pointer routing must un-map the pane

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

### 4.7 XREAL specifics

Nothing changes (F3). Two notes:

- In 3D mode each eye is a full 1920×1080, so **full-SBS content at 3840 is lossless** through
  the whole chain: game → monitor buffer → swapchain → runtime → panel. That is the best
  stereo-content path the user has, better than the Quest's (which re-encodes).
- The user's flat/head-locked *non-Monado* mode (glasses as a plain 1920×1080 desktop monitor,
  `xreal-mode.sh flat`) has **no OpenXR runtime at all** and therefore no XR presenter. That is
  the only configuration in the user's stack that needs §5.5's flat presenter — and it also
  needs the HID switch to 3840 first, or each eye gets 960 horizontal samples.

---

## 5. Q2 — the 2.5D depth mechanism

### 5.1 Why compositing twice is the *right* algorithm, not a hack

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
  which at the disparities §7 recommends is 1–2 px. **Recommend the clamp**, and note that the
  HMD case is one the cinema literature does not cover: our "frame" is partly the quad edge and
  partly the head-relative FOV boundary, which moves.

### 5.2 Where it hooks — ranked

Combining F7 with the render-path map:

| # | Injection point | Buys | Costs |
|---|---|---|---|
| **1** | **A `CWindow::m_depth` animated float folded in at the 14 `m_floatingOffset` sites**, plus the eye sign in `m_renderData` | True per-window parallax; decorations, blur cutout, damage and visibility all follow because they already follow `m_floatingOffset`; animation for free | 14 mechanical call sites; must mirror into `damageWindow` (`Renderer.cpp:2729`) and `shouldRenderWindow` (`:246-286`) |
| 2 | `CRendererHintsPassElement` + `RMOD_TYPE_TRANSLATE` pushed/popped around each window (pattern at `Renderer.cpp:1116-1129`) | Zero per-decoration edits; nestable | Breaks `simplify()` occlusion (needs `noSimplify`, `Pass.cpp:174`), and `renderLayer`'s unconditional `clipBox` (`Renderer.cpp:971`) scissors offset layers back |
| 3 | Two `renderWorkspace(..., geometry)` calls at half-width | Reuses shipped translate+scale | The aspect guard at `Renderer.cpp:2471` rejects half-width geometry, and the scale it derives squeezes the desktop rather than paning it |
| 4 | The final offloaded-FB blit in `CHyprOpenGLImpl::end()` (`OpenGL.cpp:785-829`; `applyZoomTransform` at `:793` proves `monbox` is freely mutable) | The cheapest possible SBS *output* | Produces a **flat** pair — no per-window disparity. Useful only as the §5.5 presenter |

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
desktop stays 1920×1080, only the swapchain doubles. That is why the XR presenter has no
"logical vs pixel size" problem and the flat one does (§5.5).

### 5.3 Damage — the honest part

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
   edge silently loses damage. With the §5.1 clamp this cannot happen.

**Bring-up strategy:** hold `m_forceFullFrames` while a stereo producer is active
(`Monitor.hpp:93`, consumed at `Renderer.cpp:2117-2129`; precedents set it to 3 and 5 at
`Monitor.cpp:365` and `CursorManager.cpp:320`) — i.e. accept full repaints, measure, then
optimise. Given that the second composite already doubles the work, partial damage is a
second-order saving and should not gate the feature.

### 5.4 Cost

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

### 5.5 The flat SBS presenter — and the only place the "no OpenXR" hope bends

For a non-OpenXR SBS display (XREAL in 3D HID mode used as a plain monitor, a 3D TV), the
compositor must scan out the pane pair itself. The obvious formulation — "a 3840×1080 output
whose desktop is 1920×1080 wide" — requires a monitor whose **logical width is half its pixel
width**, and Hyprland has no anisotropic logical/pixel decoupling: logical size derives from
`m_transformedSize / m_scale`, one scalar. Adding one is invasive (layout, input, layer
arrangement, popup constraint solving all read the logical box).

**The detour that avoids it entirely: a stereo mirror.** The tree already supports one monitor
rendering another's content — `IHyprRenderer::renderMirrored()` (`Renderer.cpp:1961`, called at
`:2138-2143`) draws `mirrored->resources()->getMirrorTexture()` into `monbox` with
`useMirrorProjection`, and `saveBufferForMirror(monbox)` (`OpenGL.cpp:2533`, called `:802`)
captures the source composite. So:

- The desktop lives on a normal 1920×1080 monitor (headless, or the glasses' own mono mode).
- That monitor produces a pane pair (§5.2) into two mirror-style textures.
- DP-5 at 3840×1080 renders **nothing of its own** and blits pane L into `{0,0,1920,1080}` and
  pane R into `{1920,0,1920,1080}` — two `CTexPassElement`s, which is what `renderMirrored`
  already does with one.

Cost: a "stereo mirror" mode on the mirroring path plus the pane-pair producer (shared with the
XR path). No logical/pixel surgery. Mirrors already skip cursor rendering
(`renderCursor = false`, `Renderer.cpp:2143`), which is correct here — the cursor must be in the
panes, and hardware cursors must be locked off (§4.4).

**This is the one place the user's "even head-locked SBS should enjoy this" hope costs real
work** — and note it is *not* needed for the user's actual XREAL setup, which runs Monado and
therefore gets the XR presenter. Treat §5.5 as optional (WP D6), valuable mainly for
3D-TV-shaped users and as an upstreamable generic Hyprland feature.

---

## 6. Q2 — depth as a decoration axis

### 6.1 One coherent home for the config

Both rule families exist and both are tempting. The split that respects the shipped doctrine:

```ini
# 1. WHETHER a monitor builds a pane pair, and from what  (per-monitor: xrmonitor / xrrule)
xrmonitor = XR-main, 1920x1080, anchor:local pos:0,1.4,-1.5, size:1.6, stereo:depth
xrrule    = stereo sbs,   focusclass:^(deadspace)$ fullscreen:1
xrrule    = stereo depth, anchorstate:docked            # depth only while parked at the desk

# 2. HOW HIGH each thing floats  (per-view styling: windowrule / layerrule — generic Hyprland)
windowrule = depth 0.6, match:focus 1
windowrule = depth 0.2, match:class ^(Alacritty)$
layerrule  = depth 0.8, match:namespace ^(waybar)$
layerrule  = depth 1.0, match:namespace ^(walker|notifications)$
```

Why this split and not the alternatives:

- **A new `stereorule` keyword: rejected.** It would need its own matcher, reconcile path,
  `hyprctl` surface and Lua binding, duplicating two engines that already do exactly these jobs.
  `research/23` rejected `xrlayerrule` for the same reason and the argument transfers.
- **Depth as an `xrrule` effect: rejected.** `xrrule` is per-*monitor* by design and by
  documented doctrine (`05-configuration.md:384-385`). Per-window depth in an `xrrule` would be
  the first exception and would immediately need a window selector, i.e. `windowrule` with extra
  steps.
- **Stereo mode as a `windowrule`: rejected for the monitor-level switch**, but note that a
  *window* can still be tagged (§4.3) — `windowrule = stereo sbs, match:xdg_tag ^stereo:sbs$`
  is the per-window form and it is the same effect name in the other family. Consistent naming
  across families is worth more than avoiding the duplication.

**Effort:** a window-rule effect is 7 lines across 5 files plus the Lua mirror
(`WindowRuleEffectContainer.hpp:71`, `.cpp:70` + `static_assert` bump at `:74-76`,
`WindowRule.cpp:~298` parse case, `WindowRuleApplicator.hpp:~120` `DEFINE_PROP`,
`.cpp:~58` reset tuple + `~225` apply case, `LuaBindingsInternal.hpp:~105`). The layer-rule
mirror is the same shape in `layerRule/`. **The Lua mirror line is the one non-mechanical trap:**
`WINDOW_RULE_EFFECT_DESCS[]` is name-keyed and not enum-ordered, so omitting an entry silently
means "unavailable from Lua" with no compile error.

### 6.2 Zero-config defaults

Rules are for tuning; the feature should work with none. Recommend a `decoration:` block, which
is where border/shadow/dim live and which reads naturally beside them:

```ini
decoration {
    depth_focused   = 0.6     # focused window rises to this depth
    depth_unfocused = 0.2     # ordinary windows sit just off the page
    depth_layers    = 0.8     # top/overlay layer surfaces (waybar, notifications)
    depth_scale     = 0.12    # metres of rise at depth 1.0 (the comfort knob, §7)
}
```

The four values are a **ladder, not a range** — §7.2 shows that what the eye cares about is the
*step* between adjacent depths in the foveal field, not the absolute offset, and every shipping
XR OS that publishes numbers (Android XR's 16/32/56 dp elevation ladder) made the same choice.
Wallpaper stays pinned at 0.0; the whole span is ~12′ of angular disparity, ~20 % of the 1°
budget.

with `animation = windowsDepth, 1, 4, easeOutQuint` — a new node costing exactly one line
(`m_animationTree.createNode("windowsDepth", "windows");`, `AnimationTree.cpp:~37`; unknown
names are rejected at `ConfigManager.cpp:1590`, so the node must exist before the config does).

### 6.3 Which elements get depth, and what feels right

| Element | Recommended default | Rationale |
|---|---|---|
| Focused window | rise to `depth_focused` (0.6) | The whole point. Eased over ~200 ms; the rise *is* the focus indicator |
| Unfocused windows | 0.2 | One small step off the page, so the *page* is the wallpaper and windows are cards on it |
| Special workspace | above everything (1.0) | It already renders as an overlay with a scale animation; depth is the natural 3D reading of "it's on top" |
| `top`/`overlay` layer surfaces (waybar, mako, walker) | 0.8 | This is the "hovering above the page" the user asked for; layers are already anchored to edges so the §5.1 clamp matters most here |
| `background`/`bottom` layers (wallpaper) | 0.0, pinned | Anything else destroys the sense of a *page* |
| Cursor | depth of whatever it is over, eased (§4.4) | Non-negotiable |
| Fullscreen window | 0.0 | A fullscreen window *is* the plane; raising it just moves the whole panel |
| Drag/grab in progress | +0.2 on top of resting depth | Lifting while dragging is the single most legible depth cue in every prior art system |

**Deliberately not animating depth on hover** — pointer-driven depth changes at 60 Hz produce
constant vergence micro-adjustments and are the most likely source of eye strain in this design.
Depth should change on *discrete* events (focus, drag, fullscreen), eased.

---

## 7. Disparity, geometry, and comfort

### 7.1 The formula

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

### 7.2 Comfort limits — four numbers, and the one that actually binds us

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
   must render at the same depth as or nearer than the object it is over** (§4.4 was right).
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

### 7.3 Frame violation

Covered mechanically in §5.1 (including the vertical-vs-horizontal-edge asymmetry and the
Dynamic Floating Window). The operative rule from stereoscopic practice — ISU's second Golden
Rule — is: **no part of the spatial image may be cut by the stereo window, except free-floating
foreground elements.** Our "stereo window" is the quad's content rect; our floating-window
budget is the transparent chrome margin. v1: clamp each element's per-pane shift to
`≤ margin_px`, and prefer reducing depth near edges over clipping.

---

## 8. The full-XR upgrade path

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

## 9. Prior art

### 9.1 Stereo desktops — everyone tried, nobody shipped

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

### 9.2 Injection-based stereo for flat games — the artifact taxonomy

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
the wrong depth is the most objectionable artifact. Our equivalent is the cursor (§4.4) and the
wallpaper (pinned at 0.0 in §6.3).

### 9.3 Modern XR OS design language for flat panels

The best-documented numbers, useful as sanity checks on §6.2/§7:

- **Android XR** ([spatial UI guidelines](https://developer.android.com/design/ui/xr/guides/spatial-ui))
  is the only one with a complete numeric spec: spatial panels live **0.75 m–5 m**, hold
  **constant apparent size from 0.75–1.75 m** and only then begin to shrink (0.5 m of scale per
  metre), **default spawn 1.75 m with the panel centre 5° below eye level**, optimal-comfort FOV
  **41°**, and — directly relevant to §6.3 — an explicit **elevation ladder in dp**: orbiter 16,
  popup 32, dialog 56, with orbiter Z-elevation 15 dp. *A discrete elevation ladder is what a
  shipping XR OS chose*, which is independent support for §7.2's "few tiers, small steps".
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
  world placement. **That inference is exactly the §8 tier-2 architecture**, and it is worth
  noting that the most polished spatial OS in existence went straight to real geometry rather
  than to disparity tricks.
- **SteamVR** ships no numeric depth guidance and its stereo "theater" mode exists only as an
  undocumented CLI toggle (`vrcmd --mailboxcmd systemui_dashboard toggle_theater_stereo`).

### 9.4 What shipping VR video players do about detection

Summarised in §3.3/§3.4; the headline is that **the entire ecosystem runs on filename tokens**
(DeoVR: *"DeoVR gets info from file names, so with the right naming convention you don't need to
set anything"*; HereSphere, Skybox, Pigasus, Plex, Kodi all the same), **Bigscreen, Virtual
Desktop, SteamVR and the Quest browser require a manual menu choice**, container metadata is
mostly ignored and sometimes actively harmful, and **exactly one shipping product (vorpX,
2026) does pixel-content detection — behind a toggle, with its author flagging false
positives.** Our tier ordering (cooperative tag → explicit rule → filename heuristic → optional
auto) is a strict superset of every one of them.

---

## 10. The degradation matrix

| Path | Tracking | Presenter | Q1 stereo content | Q2 depth desktop | Testable how |
|---|---|---|---|---|---|
| **Quest 3 / WiVRn** | 6DoF | XR quad pair | ✅ free (F2) | ✅ (double composite) | Quest |
| **XREAL + Monado, windowed** | 3DoF | XR quad pair | ✅ free (F2, F3) | ✅ | XREAL |
| **XREAL + Monado, DRM-lease direct** | 3DoF | XR quad pair | ✅ free (F3) | ✅ | XREAL |
| **XREAL flat, HID 3D mode, no runtime** | none (head-locked) | flat SBS mirror (§5.5) | ✅ after D6 | ✅ after D6 | XREAL |
| **XREAL flat, HID 2D mode** | none | — | ❌ (nowhere to put the second eye) | ❌ | — |
| **Plain flat monitor** | — | — | ❌ by design | ❌ by design | — |
| **Null runtime (hyprtester)** | — | quad array assertions only | ✅ assert 2 quads + rects | ✅ assert 2 panes | **flat** (CI) |

The last row matters for the WP ladder: **most of this is testable with no headset at all.** The
null-runtime XR suite can assert that a stereo monitor submits two quads with the right
`eyeVisibility` and `imageRect`, and the pane-pair producer can be asserted by screenshotting the
composite. Only comfort and "does it look right" need a device.

---

## 11. Risks and failure modes

1. **Half-submitted pair.** If the layer budget or an early `continue` drops one quad of a pair,
   the user gets content in one eye — instantly nauseating. Guard the pair atomically (§1 F2).
2. **Sub-pixel quantisation** (§7.1). If the render path rounds boxes to integers before the
   disparity is applied, depth becomes a 3-step staircase. Check `ElementRenderer.cpp:255` early.
3. **Wrong pointer mapping** (§4.6). Two producers, two mappings. Unit-test both.
4. **Monocular cursor** (§4.4). The single most likely "why does this feel awful" bug.
5. **`stereo` effect + Lua config** (F8). If the effect lands on `Legacy::CConfigManager`'s
   store it will silently vanish for anyone on `.lua`. Put it in a shared manager.
6. **False-positive auto-detection** (§3.4). Do not enable by default.
7. **Damage regressions on non-stereo monitors.** Every change in §5.2/§5.3 touches the shared
   render path. The producer must be a hard no-op when no element has non-zero depth (§5.4.1),
   and that no-op needs a test.
8. **Chrome disappearing on stereo monitors** (§4.1). Expected in v1; document it, or the user
   will report a bug when they cannot grab a monitor showing a movie.
9. **Aspect confusion** (§4.2). `sbs` vs `hsbs` mis-declared gives a subtly-wrong image that is
   hard to name. Surface the resolved layout in `hyprctl openxr status`.
10. **XREAL mode-switch ordering.** The driver latches stereo geometry at device-create time
    (`07-xreal.md:56-62`), and a non-native modeline stripes (`:36-54`). Nothing in this memo
    changes that, but any live test must follow the existing toggle discipline.

---

## 12. Work packages

Sizes: XS ≤ 50 lines, S ≤ 150, M ≤ 400, L ≤ 1000. **Device** column = where live validation
happens (flat = no headset needed).

### Phase S — stereo content (Q1)

| WP | Size | Device | What |
|---|---|---|---|
| **S0** | **0** | XREAL | **Zero-code spike.** Put a known SBS video fullscreen on an XR monitor and confirm the *current* build shows it doubled (it will). Then confirm the XREAL's 3840 mode + the mod's SBS output line up. Answers "is full-SBS at 3840 actually lossless end to end" before any code |
| **S1** | M | flat + XREAL | **The stereo effect and the quad pair.** `stereo` as an `xrrule` effect + `xrmonitor` token + `hyprctl openxr stereo <name> <mode>`; the pane-pair declaration on `CXRMonitorLayer` (atomics); the quad-assembly change (pair emission, `eyeVisibility`, `imageRect`, pair-aware budget check, pair-aware depth sort); aspect scale `k` (§4.2); chrome suppressed while stereo ≠ off. **Store the rule in a shared config manager** (F8) |
| **S2** | S | flat | **Tests.** gtests: aspect/`k` math, pane→monitor UV mapping both directions, rule fold + provenance for an enum effect, pair budget. hyprtester (null runtime): a stereo monitor submits exactly 2 quads with the expected rects and eye bits; `stereo off` submits 1 |
| **S3** | S | — | **Config surface + docs.** `05-configuration.md` §xrrule effect table, §xrmonitor token table, the `xdg_tag` convention (§3.2), the HSBS resolution caveat; `example/openxr.conf` heuristic block (§3.3); status/JSON fields |
| **S4** | XS | Quest + XREAL | **The cursor.** Suppress the desktop cursor on client-stereo monitors; draw the XR cursor into both panes with content-depth disparity (`drawCursor` twice) |
| **S5** | XS | — | **`contrib/mpv-hypxr-stereo.lua`** (§3.5) — observe mpv's stereo property, call `hyprctl openxr stereo` |
| **S6** | M | Quest | **Chrome in stereo.** Double-wide swapchain with two margined panes, `drawChrome` per pane, per-pane grab geometry. Removes S1's chrome suppression |
| **S7** | S | Quest | **Per-window stereo** (§4.3) via the UV crop. Requires D2 (a pane pair from the depth producer). `windowrule = stereo sbs, match:xdg_tag …` |
| **S8** | M | flat + Quest | **`stereo:auto`** (§3.4): downsampled NCC detector, fullscreen precondition, hysteresis, decision surfaced in status. Default **off** |

### Phase D — the depth desktop (Q2)

| WP | Size | Device | What |
|---|---|---|---|
| **D0** | **0** | XREAL/Quest | **Ergonomics spike, no code.** Two static XR monitors 12 cm apart in depth, both docked, and a day of use. Answers: is 12 cm the right `depth_scale`? Is a raised *bar* pleasant or distracting? Does 3DoF change the answer? Freeze §6.2's defaults after this, not before |
| **D1** | S | flat | **`windowrule = depth` + `layerrule = depth`** — the rule effects and storage, no rendering. Plus `CWindow::m_depth` / layer equivalent as animated floats, the `windowsDepth` animation node, and `decoration:depth_*` defaults. Observable via `hyprctl clients` |
| **D2** | M | flat | **The producer.** Second composite into a per-monitor pane buffer with the eye sign in render data; depth folded in at the 14 `m_floatingOffset` sites; the edge clamp (§5.1); the "no depth anywhere ⇒ single pane" fast path (§5.4.1); `m_forceFullFrames` while active. **Sub-pixel check (§7.1) happens here** |
| **D3** | S | Quest + XREAL | **The XR presenter for depth panes.** Double-wide swapchain, two `blitBuffer` calls, pair submission (reuses S1 wholesale); pointer un-mapping for the depth producer (§4.6) |
| **D4** | S | flat | **Tests + docs.** gtests for disparity math and the clamp; hyprtester for the single-pane fast path and the two-pane submission; wiki-shaped docs for `depth` |
| **D5** | S | Quest + XREAL | **Polish.** Cursor depth easing; drag lift; special-workspace depth; per-monitor `depth_scale` override; a `socket2` event when a monitor enters/leaves stereo |
| **D6** | M | XREAL | **Flat SBS presenter** (§5.5): stereo-mirror mode, `lockSoftwareAll`, monitor-rule syntax. Optional — only needed for the no-runtime XREAL mode and 3D TVs |
| **D7** | S | — | **Lua-config migration** of `xrrule`/`xrmonitor` into a shared manager + `hl.xr_rule{}` binding (F8). Not strictly this project's job, but S1 makes it cheap and 0.57 makes it mandatory |

**Suggested order:** S0 → S1 → S2 → S3 → S4 → S5 → **ship Q1** → D0 → D1 → D2 → D3 → D4 →
**ship Q2** → D5 → S6 → S7 → D6/D7/S8 as appetite allows.

Rough totals: **Phase S core (S1–S5) ≈ 550–750 lines**, of which the quad-pair change is under
100. **Phase D core (D1–D4) ≈ 700–900 lines**, dominated by the producer and the 14 call sites.

---

## 13. Open questions

1. **Does the XREAL's hardware L/R scanline split interact badly with a *quad* that only covers
   half of each eye's viewport?** It should not — Monado composites into the 3840 target and the
   glasses split scanlines regardless of content — but it is the one runtime assumption in this
   memo that has not been observed live. S0 answers it.
2. **What does the Dead Space mod actually emit** — full SBS at 2× width, or half-SBS at the
   monitor's width? It changes only `k` (§4.2), but it decides which is the *default* when the
   tag says just `stereo`.
3. **Sub-pixel disparity** (§7.1): does the current pass round window boxes to integers before or
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
9. **Does WiVRn's foveation attenuate small disparities enough to matter** (§7.2)? Measurable
   with a disparity ramp test image.
10. **Upstreamability of `windowrule = depth`.** It is a plainly useful generic feature for
    anyone with a 3D display, and it has no XR in it. Worth an upstream conversation after D4 —
    but only if D6 exists, since without a flat presenter mainline users have nothing to see.
11. **Is the ≈0.1° foveal fusion limit the right constraint for *reading* on a raised window?**
    The stereoscopic-text literature is thin: one lexical-decision study is commonly cited for
    an "effective fusional range of about one character space", but the primary source could not
    be pinned down, and **no dedicated study of text-panel disparity versus reading speed or
    fatigue surfaced at all.** This looks like a genuine gap. Do not invent a number; measure it
    during D0 by reading a page at each tier for five minutes.
12. **Does the tier ladder survive a busy screen?** Four tiers is fine with three windows. With
    ten overlapping floating windows, adjacent-in-space windows may be several tiers apart, which
    is exactly the depth-step case §7.2 warns about. Possible mitigation: derive depth from
    *stacking order proximity* rather than a flat category, so neighbours are always one step
    apart. Decide after D0.
13. **Should the mpv script set a per-*window* stereo rule rather than a per-monitor mode?**
    Per-window (S7) is strictly better once it exists — the video keeps playing correctly when
    it is not fullscreen — but it needs the depth producer. Sequencing question, not a design
    question.
