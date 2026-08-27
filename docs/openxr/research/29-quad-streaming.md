# 29 — Decomposed XR streaming: forward the layers, compose on the headset

**Status: research. No implementation.** This memo evaluates replacing the "composite two eye
buffers on the host and encode them 90 times a second" pipeline with one that forwards the
*composition layer stack* to the headset and lets the headset's own compositor place it — with a
seamless fallback to today's path whenever a foreign application needs it.

It also answers two structural questions the user raised while it was being written: whether
HypXRland should become "just an app" against a standalone runtime product, and whether some quads
should carry *content* rather than *pixels*.

---

## 0. Executive summary

**The idea is right. The project as scoped is roughly ten times larger than it needs to be, because
four of its six pieces already exist in this tree.**

**The problem, in one line.** HypXRland already does zero GPU work for a monitor that did not change
(`src/openxr/OpenXRManager.cpp:1820`). All of that damage information is then thrown away, because
what gets encoded is not the desktop but a *view of* the desktop, and the view changes every frame
even when the desktop does not. A perfectly static five-monitor desktop costs **≈1.6 Gpx/s** of
composite-and-encode and, before the QP floor landed, ~28 Mbit/s of radio — against a genuine content
change rate, while typing, of about **2 kpx/s**.

**What is already true, and inverts the plan.**

1. Hyprland **already submits one `XrCompositionLayerQuad` per monitor** and has never rendered an eye
   buffer. Its *submission model* is already standard OpenXR — most of the second thing the user asked
   for is structurally done (with one exception, below).
2. The flattening happens in **exactly one function we already fork**:
   `layer_squasher::do_layers`, `wivrn-xg/server/compositor/layer_squasher.cpp:523`.
3. The **quad wire format already exists, shipped and field-debugged** — `struct take_quad`, written
   every recorded frame off the live layer stack, and already consumed by `hypxrcompose`.
4. The **pose trap is already discovered and documented**: OpenXR re-expresses every layer pose as
   head-relative before the compositor sees it, so naive forwarding makes world-locked quads swim.
5. WiVRn **already performs a seamless, mid-session, un-negotiated pipeline switch** — and the reason
   generalises: **the geometry is per-frame metadata, not session state.** `view_info` rides the first
   shard of every frame, so nothing is renegotiated. `view_info.alpha` already starts and stops an
   entire encoder mid-session. The "seamless switchover" is not a research question here.
6. **Dynamic-count, sub-rect, per-stream-codec video streaming shipped in this repository and was
   deleted** — commit `5c50b9a1`, 53 files, −1359/+503, including a client-side compositor that
   blitted N decoded sub-rect streams. The transport half is `git show`, run in reverse — and its
   sub-rect tiling is also, directly, the atlas transport that makes N panes cost one decoder.

**And one thing that is *not* true, which the earlier drafts of this memo asserted.** HypXRland is a
standard-OpenXR client in its *submission model* but not in its *graphics binding*:
`XRSession.cpp:75-82` makes **`XR_MNDX_egl_enable` a hard requirement**, and it is a Monado vendor
provisional extension nobody else implements. HypXRland cannot start on SteamVR or Meta's PC runtime
today. That is one line, and it is the highest-leverage change for the runtime-as-product goal.

**The constraint that reframes the design.** Meta's compositor draws **16 layers per frame**, shared
with Meta's own system layers, and our stack is already about fifteen. **Server-side squashing is
exactly what makes the layer count invisible today** — it is what lets us budget against 128 while the
headset accepts 16. Forwarding trades a bandwidth win for an 8× tighter budget and makes the layer cap
the system's primary scarce resource. So the project is not *"forward the quads"*; it is:

> **Forward the few layers that benefit most, and keep squashing the rest** — ranked by
> **area × staleness**. Which picks its own first target, and it is not a desktop pane: **hypxrpaper's
> static, full-FoV equirect2 environment.** Full-screen cost, zero update rate, native Quest support,
> one layer of sixteen, and nothing important breaks if it fails.

**Recommendation.** Extend Monado/WiVRn; do **not** write a runtime, and do **not** write a client.
Sequence:

- **Now, unconditionally, three things.** (i) **Feed the damage we already compute downward into the
  encoder.** VA-API has taken damage-shaped input for years — `VAConfigAttribEncDirtyRect`, `EncROI`,
  `EncSkipFrame` — and we hand it none; ROI is reachable *today* through WiVRn's existing ffmpeg path
  in about twenty lines. It touches neither pacing nor timewarp and creates none of the idle hazards.
  (ii) `research/21`'s **"option B"** — damage-gate the whole `xrWaitFrame`/`xrEndFrame` cycle inside
  HypXRland: zero protocol change, zero WiVRn change, one repository. This is the one that buys
  *engine time*. It needs a **minimum-frame-rate floor** beneath it (a frame every 100–200 ms at
  ~0.2 Mbit/s) — a plain keepalive provably does not work. `fps_divider = 2` tests the client half
  tonight, with no code. (iii) **Fix the multi-client defects** — the merged layer accumulator has no
  bounds check while each client is told it may submit 128, and `set_z_order` unlocks a mutex it never
  locked on every connect and app swap. Both are latent corruption sitting directly on this road.
- **Then capability negotiation (≈2 days), before any quad work**, so iteration does not cost a
  lockstep server rebuild plus an APK sideload every time.
- **Then the smallest proof**: hypxrpaper's equirect2 forwarded as its own layer, left duplicated in
  the squash at first so divergence is visible.
- **Then selective forwarding of N layers**, restoring `5c50b9a1` as an atlas transport and adding the
  layer-budget arbitration that the 16-layer ceiling forces.

**On the content tier**, the arithmetic settles it: a typed character is ~320 bytes as a lossless
dirty rect and ~48 as a glyph run — both noise — while the case that actually hurts is scrolling, and
**a copy-rect primitive captures 98.9 % of what semantic rendering would win there** for one 24-byte
message. Semantic *desktop* rendering buys nothing and costs font-fidelity divergence in a workload
that is entirely text; FreeRDP's own default after twenty-five years is `GLYPH_SUPPORT_NONE`, with a
warning to *"expect visual artefacts"*. But the ambition is right for **first-party UI**: `hypxrhud`
already speaks semantics over D-Bus and already renders with a font baked into its own binary, so it
is font-hermetic by construction, and roughly a quarter of its core code relocates to the headset untouched. **Desktop panes
are pixels; first-party UI is content; the line is whether we own the renderer and its fonts.**

**The biggest assumption I am challenging: that this is primarily a *bandwidth* project.** It is not,
and there is now a measurement that settles it. The bandwidth half of the complaint is largely already
fixed — the deployed `default_min_qp = 18` cuts a static eye from 4.97 to 1.69 Mbit/s, on a link
measured at 700 Mbit/s and 1.3 ms RTT. And **a static frame still costs 60–80 % of a moving frame in
VCN engine time, while its bit cost swings by ~28×.** Bits were never the scarce resource; **engine
time on a single unified VCN ring is** — the same ring one 1080p60 browser decode can already disturb
enough to judder head motion. What remains is therefore **VCN engine occupancy**, **GPU composite**,
and — the one nobody has been counting — **image quality**. Forwarding removes *two resamples and
head-driven (not gaze-driven) foveation* from an all-text workload, and that matters more than it
looked: **no AMD ASIC can encode 4:4:4 or screen content at all**, so removing resamples is the
principal text-quality lever available on this hardware, permanently. The pitch that will survive
contact with a Tuesday afternoon is *"the text gets sharper, and the video engine stops running"*, in
that order.

**Second challenge: the win is not unconditional, and the worst case is a regression.** A 2.23 m pane
at 1.4 m occupies about 1,425 px of a 2,064 px eye buffer while its source is 2,560 px — so a
*changing* pane streamed natively sends **1.8× more** pixels than today. Five panes all animating
would cost *more* than the current pipeline. The entire win is in the rate of change. Two consequences
the design must carry from day one: per-quad stream resolution comes from
**`XR_META_recommended_layer_resolution`** (the Quest runtime will tell us, including GPU-pressure
adaptation we cannot compute), and the full-eye fallback is a **load-shedding valve**, not just a game
switch.

**Corroboration, and a calibration on ambition.** Virtual Desktop — the only shipping per-pane system
found anywhere — caps at **three** monitors on Quest 3, drops per-monitor resolution from 4K to 1440p
when you go multi, and *splits* one 120 Mbit/s budget across them. Meta, NVIDIA, Microsoft, Valve,
ALVR, WiVRn and upstream Monado all flatten; Meta and Microsoft had complete layer knowledge on the PC
side and chose not to forward it. **Plan for a handful of layers, not a wall of them.**

---

## 1. The problem, quantified

### 1.1 The pipeline as it actually runs on this box

Live configuration (`~/.config/hypr/hyprland-xr.conf`): `XR-main` declared at `2560x1440@90`,
`size:2.23` m, `depth_desktop = 1`, `overlay = 1`, `blend_mode = alpha`, runtime pinned to
`/dev/dri/renderD129`. With `depth_desktop` on, the declared resolution is **one pane**, so the
headless output runs at **5120×1440** and the OpenXR swapchain, inflated by the chrome margins, is
**5580×1761** — a **+33.3 %** area tax on every pixel-touching pass
(`src/openxr/OpenXRManager.cpp:2661-2675`, `src/output/StereoPacking.hpp:63-71`).

Downstream, per `docs/openxr/research/26-wivrn-multi-gpu-client-render.md:43,84-89` and
`21-wivrn-variable-bitrate.md`:

| Stage | Geometry | Rate | Throughput |
|---|---|---|---|
| WiVRn layer squash (Vulkan compute, `layer.comp`) | 2064×2208 per eye | 90 Hz | **820 Mpx/s** |
| VAAPI HEVC encode, streams 0/1 | 1856×1920 per eye | 90 fps | **641 Mpx/s** |
| VAAPI HEVC encode, stream 2 (passthrough alpha) | 1856×960 | 90 fps | **160 Mpx/s** |
| **Total steady-state pixel work** | | | **≈ 1.62 Gpx/s** |

That work is done **every frame, unconditionally, whether or not a single desktop pixel changed**.
There is no damage, dirty-region, or skip-on-static path anywhere in the WiVRn server: a repo-wide
search of `server/` for `damage|dirty|resend` returns nothing. The only three non-encode paths are
alpha gating when the blend mode is not `ALPHA_BLEND`, IDR-recovery skip, and a whole-frame drop
when the encoder thread is behind.

Bandwidth, measured rather than estimated (`research/21` §2): **28.1 Mbit/s sustained over a 399 s
window against a 30.0 Mbit/s CBR target — 94 % utilisation — on an ordinary, mostly-static
desktop**, with the TCP socket busy 93.3 % of wall clock. The non-video floor when the session goes
non-visible is 0.58–0.60 Mbit/s.

The link itself is *not* the problem and it is worth saying so plainly: **700 Mbit/s, 1.3 ms
minimum RTT, 0.0006 % retransmits** (`research/21:593-594,802-803`). Nothing in this memo is
motivated by running out of network.

### 1.2 The load-bearing sentence

Here is the whole argument in one line, and it is already written down in this tree
(`research/21:445-448`):

> *the encoder input is a function of head pose, and head pose is never exactly constant.*

The compositor knows perfectly well that nothing changed. HypXRland **already** does zero GPU work
for an unchanged monitor — `src/openxr/OpenXRManager.cpp:1820-1821`:

```cpp
if (!buf && !wantAnimTick && l->m_hasContent)
    continue;
```

with the comment three lines above spelling out the intent: *"This is what keeps a static desktop
with hidden chrome at zero GPU cost — the quad re-presents the most recently released image every
runtime frame."* The upstream chain that feeds it is fully damage-aware too: `renderMonitor` early
-returns on `!needsFrame && !m_damage.hasChanged()` (`src/render/Renderer.cpp:2454`), so no render →
no commit → no presented buffer → the `continue` above.

**And then all of that is thrown away**, because the thing being encoded is not the desktop — it is
a *view of* the desktop, re-projected through a head pose that moves every frame. A perfectly static
five-monitor desktop still costs 1.62 Gpx/s of composite-and-encode and ~28 Mbit/s of radio.

The ratio is the headline. Typing at ten characters a second dirties roughly a 10×20 px cell each
time: **≈ 2 kpx/s of genuine content change against 1.6 Gpx/s of pixel work.** Five to six orders of
magnitude. Even a *worst-case* single pane scrolling continuously at 60 Hz is 2560×1440×60 =
221 Mpx/s — still seven times less than what the pipeline burns while idle.

### 1.3 Where damage exists and exactly where it dies

Per-quad damage at **monitor granularity already exists and is already exploited**: `m_haveNewFrame`
/ `takeLatestBuffer()` (`src/openxr/XRMonitorLayer.cpp:226-234`) is precisely the predicate "did
quad *k* change this frame".

Per-quad damage at **region granularity exists and is destroyed one function call before the XR
layer could see it**:

1. `frameDamage` is computed, transformed, and stereo-folded across panes at
   `src/render/Renderer.cpp:2718-2748`, then handed to aquamarine at `:2751`.
2. Aquamarine's `IOutput::state->state().damage` is a real `CRegion`
   (`/usr/include/aquamarine/output/Output.hpp:72`) — but **`onCommit()` clears it** (`:116`).
3. `CMonitor::m_events.presented` carries a `Time::steady_tp` and **nothing else**
   (`src/output/Monitor.hpp:238`).
4. `CXRMonitorLayer` therefore has no damage field at all, and `blitBuffer` is unconditionally
   full-content, preceded by a full-swapchain `glClear` (`src/openxr/XRGraphics.cpp:520-580`).

Recovering it is roughly one line at `Renderer.cpp:2751` plus a field on the layer. Nothing forces
full damage on the compositor side.

One more discarded signal: **`XrFrameState::shouldRender` is never read** — `shouldRender` does not
appear anywhere in `src/openxr/`. The runtime already tells us when a frame need not be rendered and
we ignore it.

### 1.4 The three costs, separated — because they have different answers

The user's framing is "heavy on GPU, codec, and network". Those decompose differently and it matters:

| Cost | Magnitude today | Already mitigated? |
|---|---|---|
| **Network bits** | 28.1 Mbit/s static | **Largely, yes.** The deployed `default_min_qp = 18` (`wivrn-xg/server/encoder/encoder_settings.h:60`) collapses a static eye from 4.97 → **1.69 Mbit/s** at a 14.8 Mbit/s target, and 10.04 → **2.10** at 49.4. Peak frame 158,236 B → 10,183 B. Moving content is byte-identical with the floor on or off. |
| **Codec occupancy** | 800 Mpx/s on a single VCN | **No.** A hardware encoder does near-constant work per frame regardless of content. This is why `hypxrva` had to exist: the single VCN on the 890M could not serve WiVRn encode and desktop video decode at once. |
| **GPU composite** | 820 Mpx/s of compute squash | **No.** |
| **Image quality** | two resamples + head-tracked foveation | **No — and this is the underrated one.** See §1.5. |

### 1.5 The quality argument, which is stronger than the cost argument

A desktop pixel today makes this journey: composited at 2560×1440 → blitted into a 5580×1761
swapchain → **resampled by the squasher into a 2064×2208 eye buffer** → **resampled again by WiVRn's
dynamic foveation into an 1856×1920 encode surface** → HEVC 4:2:0 → un-foveation mesh warp on the
client → the Quest compositor's own distortion/timewarp resample.

Two of those resamples exist only because we flatten. And the foveation one is actively hostile to
this workload: WiVRn's foveation *does* consult `device_id::EYE_GAZE` when a runtime supplies it
(`wivrn-xg/server/compositor/foveation.cpp:426-432`), but **the Quest 3 has no eye tracking, so the
"fovea" falls back to head direction** with a fixed downward adjustment (`:366-371`) — see also
`research/21` §4.4. Reading a terminal in the corner of your view while facing forward is
exactly the case foveated encoding degrades — and it is exactly what a five-monitor desktop asks you
to do all day.

Forwarding a quad to the headset's compositor removes both resamples and removes foveation from the
desktop path entirely. Meta's own guidance is that quad layers render sharper than the same content
drawn into an eye buffer, for precisely this reason (§3).

So the honest pitch is not only "cheaper". It is **"cheaper *and* the text gets sharper"**, and the
second half may be what the user actually notices.

---

## 2. Six things that are already true

These were all verified in the tree during this research, and several of them invert the shape of
the project. Read this section before the option matrix; it does most of the work.

### 2.1 HypXRland already submits quads. It has never rendered an eye buffer.

Each XR monitor is submitted as **one `XrCompositionLayerQuad`** — two, at one pose with opposite
`eyeVisibility` and `imageRect`s, for a stereo pair (`src/openxr/OpenXRManager.cpp:2411-2440`,
`:2472`; `docs/openxr/00-overview.md:15-19`). `xrLocateViews` is called only for viewpoint-portal
telemetry, never for rendering.

`depth_desktop` is a *third*, separate thing: it makes **Hyprland** composite the desktop twice, once
per eye, into a double-wide output. It is not an eye buffer in the OpenXR sense.

**The per-eye composite belongs to the runtime, not to us.** Nothing on the Hyprland side needs to
be disassembled.

**One important caveat, because it bounds the "already just an app" claim** (§6.9): the *layer
submission model* is pure standard OpenXR, but the *graphics binding* is not.
`src/openxr/XRSession.cpp:75-82` makes **`XR_MNDX_egl_enable` and `XR_KHR_opengl_es_enable` hard
requirements** — bring-up refuses without them. `XR_MNDX_egl_enable` is a **Monado vendor provisional
extension** that no other runtime implements. So HypXRland today cannot start against SteamVR, Meta's
PC runtime, or anything conformant. That is one line, and it is the single highest-leverage change if
"other platforms build against my runtime" is a real goal.

### 2.2 The flattening happens in exactly one function, and we own the file

`layer_squasher::do_layers(...)` — `wivrn-xg/server/compositor/layer_squasher.cpp:523`, called from
`compositor.cpp:360`. It takes the entire `comp_layer_accum` and returns `{2 poses, 2 fovs,
2 rects}`. Everything else dies there, in one compute dispatch per eye (`:775`).

WiVRn does not implement a Monado `comp_target`; `class compositor : public comp_base`
(`server/compositor/compositor.h:51`) **replaces Monado's native compositor wholesale**. Monado's
`comp_main`/`comp_renderer` are not in the picture. So the single most important graft point for
this entire project is one function in a file we already maintain a fork of.

### 2.3 The quad wire format already exists, shipped and field-debugged

The recorder subsystem walks the live `layer_accum` every recorded frame and serializes every quad
(`wivrn-xg/server/compositor/compositor.cpp:911-953`) into
`struct take_quad` (`common/hypxr_take_bundle.h:120-149`):

```
index (composition order) | name | pose | size[2] metres | eyeVisibility
view_space | swapchain id | image | array_layer | rect[4] texels
```

with `take_telemetry_record` (`:153-183`) adding the head pose, both eye poses+fovs, the stage
correction, `alpha_blend`, and `dropped`. `hypxrcompose` consumes the same shape as `SQuadRecord`
(`/home/ajg/code/hypxrcompose/src/Bundle.hpp:113-151`), with the semantics-encoding helpers
`worldPose(head)` and `composedInEye(eye)`.

This is not a sketch. It is a descriptor that has been argued over, pinned, versioned, and debugged
against real takes. **A quad-forwarding protocol's core message is already designed.**

### 2.4 The pose trap is already discovered and documented

`common/hypxr_take_bundle.h:110-119` records the thing that would otherwise be a multi-week bug:

> *the pose is HEAD-RELATIVE, not world: OpenXR's state tracker re-expresses every layer pose
> relative to the head device before the compositor ever sees it, whatever reference space the
> application submitted it in.*

Naive layer forwarding therefore ships poses computed against the server's **predicted** head pose.
Re-anchoring those on the client's **actual** head pose makes every world-locked quad swim with head
motion — the classic remote-composition failure. The fix is already encoded in the format: compose
with the recorded `head` pose to recover world space, and carry `view_space` to distinguish quads
that are *meant* to stay head-locked.

### 2.5 The seamless mode switch already exists — in the other direction

WiVRn already performs a dynamic, mid-session, un-negotiated switch between two fundamentally
different pipelines:

```cpp
// wivrn-xg/server/compositor/compositor.cpp:335-337
if (layer_accum.layer_count == 1 and
    (layer_accum.layers[0].data.type == XRT_LAYER_PROJECTION or
     layer_accum.layers[0].data.type == XRT_LAYER_PROJECTION_DEPTH))
```

Note the condition precisely: **exactly one layer in the *merged, cross-client* stack**, and it is a
projection layer. Nothing about FoV, nothing about view count. So **hypxrhud submitting a single panel
is enough to disable the fast path system-wide** — which is worth knowing before designing triggers
around it.

**Nothing is renegotiated across that switch, and the reason generalises perfectly.** Encode images
are allocated once from `encoder_settings` (`compositor.cpp:1015`), the squasher target once
(`:1020`), foveation once (`:1021`); `send_video_stream_description()` fires at construction
(`:1074`) and on refresh-rate change (`:1261`) and **never on a mode switch**. Both paths converge at
`foveation.foveate(..., src, src_rect, src_fov, alpha)` (`:447-455`) and then at the same encoders.
The geometry travels as **per-frame metadata**: `view_info{display_time, pose[2], fov[2],
foveation[2], alpha}` rides the first shard of every frame (`wivrn_packets.h:863-876`).

> **The switch is seamless because the geometry is per-frame metadata, not session state.** That is
> the single most important design lesson available for the quad protocol: put quad geometry in
> per-frame metadata exactly the way `view_info` carries FoV, and never in the stream description.

A second existence proof in the same file: `view_info.alpha` toggles with the blend mode, and when it
does, **encoder stream 2 starts and stops mid-session** (`:457-459`, `:674`) while the client flips
passthrough (`client/scenes/stream.cpp:1064-1067`). *A whole video stream appearing and disappearing
mid-session already ships.*

The observable glitch is not geometric — the FoV travels with the frame, so reprojection stays
correct. What changes is sampling density, so the artefact is a **sharpness pop**, and on a one-frame
flip, a one-frame shimmer.

**Correction to a claim this memo made in an earlier draft, and which the code does not support.** The
squasher *appears* to shrink the encoded viewport to the union FoV of all layers
(`layer_squasher.cpp:708-718`), but line 711 assigns `fovs[view] = all_layers_fov` **before** `w2` is
derived from `fovs[view]`, so `w1 == w2` and `h1 == h2` exactly and **the viewport never shrinks**.
This is byte-identical in `upstream/master`, so it is an upstream WiVRn bug, not a fork regression.
The FoV narrowing itself does take effect (it feeds `pre_transform` and is stamped into
`view_info.fov`), so today the squasher renders the union-FoV region at **full render extent** — it
supersamples. The picture is correct, arguably sharper; the compute and bandwidth saving is what is
lost. **The optimisation everyone assumed was running has never once run**, which also means nobody
has seen its failure modes.

The user asked whether seamless switchover between quad-mode and full-eye-mode is realistic. **This
codebase already ships two mid-session, un-negotiated pipeline switches**, one of which starts and
stops an entire encoder. The question is not whether it can be done; it is which triggers and how much
hysteresis — and the existing switch has *none*, because it is free and the new one will not be.

### 2.6 Multi-stream sub-rect video shipped here once, and was deleted

Commit **`5c50b9a1` "WIP: fixed encoder layout"** (2025-12-07), an ancestor of `v26.6.2` and
therefore inherited by our fork, replaced this:

```cpp
struct video_stream_description {
    enum class channels_t { colour, alpha };
    struct item {
        uint16_t width, height, video_width, video_height, offset_x, offset_y;
        video_codec codec;  channels_t channels;  uint8_t subsampling;
        std::optional<VkSamplerYcbcrRange> range;
        std::optional<VkSamplerYcbcrModelConversion> color_model;
    };
    std::vector<item> items;      // DYNAMIC COUNT
};
```

with today's fixed `std::array<video_codec, 3>` (left/right/alpha), dropped the shard
`{start_of_slice, end_of_slice, end_of_frame}` flags, and **deleted
`client/scenes/blitter.{h,cpp}` — 364 lines of client-side compositor that blitted N decoded
sub-rect streams into one eye texture by offset and size.**

Dynamic-count, per-stream-codec, sub-rect video streaming is not hypothetical in this repository. It
is `git show 5c50b9a1`, run in reverse. Its one gap is that it tiled sub-rects into an eye image
rather than submitting independent OpenXR layers — it is the transport half of what we need, not the
composition half.

### 2.7 (bonus) The client already submits quad layers in production

`scene::add_quad_layer(flags, space, eyeVisibility, subImage, pose, size)` —
`wivrn-xg/client/scene.cpp:348-368` — with three live call sites: lobby GUI quads with colour-scale
dimming and depth test (`client/scenes/lobby.cpp:1072`), a head-locked recenter tip
(`:1100`, bound to `xr::spaces::view` at `:1250`), and **in-stream GUI quads composited over the live
video projection layer** (`client/scenes/stream_gui.cpp:1026-1033`). They are already *sub-rect*
quads sharing one swapchain, with z-order (`client/constants.h:106-113`).

**Both ends of the graft are built. The middle is the work, and the middle is transport plumbing —
dynamic stream count, sub-rect encode, per-stream decoders — not novel graphics.**

---

## 3. Headset-side capabilities

The question this section decides: does *"compose on the headset"* mean **"hand quads to Meta's
compositor as ordinary OpenXR layers and get per-layer reprojection for free"**, or **"write our own
compositor inside an Android app"**? The answer is the former, with one hard limit.

**Answer: the former, decisively — and the platform gives more than expected.** The authoritative
inventory is Khronos' conformance-derived list rather than Meta's documentation:
[`meta_quest_3_mobile.json`](https://raw.githubusercontent.com/KhronosGroup/OpenXR-Inventory/main/runtimes/meta_quest_3_mobile.json)
("Oculus 205.206.0", conformance submission 47) — **101 extensions**, rendered at
[extension_support.html](https://github.khronos.org/OpenXR-Inventory/extension_support.html).

### 3.1 What Meta's compositor gives a third-party app

| Capability | Status | Source |
|---|---|---|
| `XrCompositionLayerQuad` | supported, and the recommended way to draw text/UI | Meta [Compositor Layers](https://developers.meta.com/horizon/documentation/unity/os-compositor-layers/) |
| Cylinder, equirect, cubemap layers | supported, with "no more than one cylinder and one cubemap per scene" in Unity's wrapper | Meta [OVROverlay](https://developers.meta.com/horizon/documentation/unity/unity-ovroverlay/) |
| **Layer count** | **16 per frame; layers beyond that are not rendered** (Unity's wrapper caps at 15) | ibid. |
| **Per-layer cost** | **~0.1 ms/layer** at CPU/GPU level 4 on Quest 2; **~0.6 ms** for a fullscreen layer; head-locked `FIXED_TO_VIEW` quads merge at **zero** additional cost | ibid. |
| **Per-layer reprojection** | layers render *"at the framerate of the compositor, which is always greater than or equal to the framerate of your application"* | ibid. |
| **Sharpness** | *"Textures appear sharper due to skipping a layer of sampling"*; *"you can achieve high clarity even if using smaller fonts"* | ibid. |
| Layer filtering / sharpening / supersampling | `XR_FB_composition_layer_settings` — **already requested by our client** (`client/application.cpp:1261`) and already applied to the video layer (`client/scenes/stream.cpp:1095-1097`). *"Requires more GPU resources … may lead to frame drops."* | Meta [layer filtering](https://developers.meta.com/horizon/documentation/native/android/mobile-openxr-composition-layer-filtering/) |
| Depth test between layers | `XR_FB_composition_layer_depth_test` — already requested and used (`client/scenes/stream_gui.cpp:1021,1029`) | client source |
| Colour scale/bias | `XR_KHR_composition_layer_color_scale_bias` — already requested and used | client source |
| Passthrough | `XR_FB_passthrough` — already requested; `XR_META_boundary_visibility` added by the fork | client source |
| Underlays | *"more bandwidth-intensive because the compositor must punch a hole in the eye buffer with an alpha mask"*; *"texture bandwidth is often a VR bottleneck"* | Meta docs |
| `XR_FB_space_warp` (ASW) | **not used anywhere in our client** | client source |

The answer is therefore unambiguous: **hand quads to Meta's compositor.** Per-layer reprojection,
compositor-rate presentation, and the sharpness win are all things the platform already does and
that we get by submitting a standard layer type our client already submits in production (§2.7).

### 3.2 The layer budget — better than the docs say, but verify it yourself

| Source | `maxLayerCount` |
|---|---|
| OpenXR spec floor, `XR_MIN_COMPOSITION_LAYERS_SUPPORTED` | 16 (`/usr/include/openxr/openxr.h:38`) |
| Meta's own Unity/Unreal docs | *"up to 16 compositor layers per frame"* |
| Unity `OVROverlay` wrapper | 15 |
| Quest 2 runtime dump (community) | 16, swapchain 4096×4096 |
| Quest 3, two independent runtime extension dumps (Oculus 203.100.0 and 206.134.0) | **16** |
| Quest 3, runtime v206.153.0, one community on-device probe | 32, swapchain 8192×8192, recommended per eye 1680×1760 |

**The sources conflict, and the weight is on 16**: Meta's documentation, the spec minimum, and two
independent runtime dumps all say 16; one community probe says 32. **Design to 16** and re-verify with
one `xrGetSystemProperties` call before relying on anything more.

**And 16 is not even 16 for us**, because it is shared with system layers: Meta's own `LogLayers`
output shows `com.oculus.ovrmonitormetricsservice` injecting a quad into the same composition, and
notifications do too. Budget a system reserve.

The per-layer cost is *flat plus coverage* — ~0.1 ms per layer plus a coverage-proportional term
(~0.6 ms fullscreen) — and it is **paid even when the layer is fully transparent**, which matters for
a HUD of mostly-empty panels.

Remember stereo monitors cost **two** layers each (`eyeVisibility` LEFT/RIGHT,
`src/openxr/OpenXRManager.cpp:2436`). Our stack: hypxrpaper's equirect (1) + monitors (1–2 each) +
hypxrhud (≤6) + a game projection (1).

The *host* is not the constraint: `XRT_MAX_LAYERS = 128` on Linux
(`wivrn-xg/build-server/_deps/monado-src/src/xrt/include/xrt/xrt_limits.h:89-95`). Nor is composite
cost — 16 × 0.1 ms sits comfortably inside an 11.1 ms budget.

Three consequences:

1. **Layer arbitration must move into the runtime** (§5.4). Three clients independently policing
   themselves against a shared budget cannot be correct.
2. **Head-locked HUD quads are nearly free** — `FIXED_TO_VIEW` quads merge at zero additional cost —
   so hypxrhud is the cheapest part of the stack, not the most expensive.
3. **Overflow needs a host-side spill path**: panes beyond budget composite into one combined quad.
   Same machinery as the full-eye fallback.

### 3.3 The runtime will tell you each layer's correct resolution

`XR_META_recommended_layer_resolution` (extension 255, present on Quest 3): hand it a layer plus a
predicted display time, get back `recommendedImageDimensions`. Meta: *"the runtime may use any
factors… static properties such as screen resolution and HMD type, but also dynamic ones such as
layer positioning and system-wide GPU utilization."*

**This replaces the angular-size arithmetic of §7.3 with a first-party answer**, including
GPU-pressure adaptation we could not compute ourselves. The client queries it per quad per frame and
reports the number upstream; the host sizes the stream accordingly. It is the single most useful
extension found in this research.

Adjacent: `XR_FB_swapchain_update_state_vulkan` / `_opengl_es` give per-swapchain compositor-side
sampler control (min/mag filter, mipmap mode, wrap, `maxAnisotropy`, border colour, swizzle) —
directly relevant to text quality on a quad.

Measured readability on Quest 3 for calibration (community, 2560×1440 layer 50° wide at 1.5 m =
51.2 PPD): **20 px comfortable, 18 px readable, 16 px borderline, below 16 px unreadable.** The
threshold is *angular glyph size, not pixel density* — the correct fix for small text is to **widen
the layer, not scale the desktop up**.

### 3.4 The zero-copy path that removes the client GPU pass entirely

`XR_KHR_android_surface_swapchain` is present on Quest 3 (measured v4 on runtime v206.153.0).
`xrCreateSwapchainAndroidSurfaceKHR` returns an Android `Surface`; hand it to MediaCodec and
**the compositor consumes decoded frames directly — no application GPU work whatsoever.**

Today both ALVR and WiVRn instead run `AImageReader` → `AHardwareBuffer` → Vulkan import → one
sample-and-draw pass into an OpenXR swapchain (`client/decoder/android/android_decoder.cpp:120-136`).
That pass exists because they must **de-foveate** and colour-convert. **A rectilinear desktop quad
needs neither.**

Companions worth having: `XR_FB_android_surface_swapchain_create` adds
`XR_ANDROID_SURFACE_SWAPCHAIN_USE_TIMESTAMPS_BIT_FB` — *"the compositor should acquire the most
recent buffer whose presentation timestamp is not greater than the expected display time"*, i.e. the
compositor picks the right frame for the right vsync for free — and `SYNCHRONOUS_BIT_FB` to queue
rather than always replace. `XR_FB_swapchain_update_state_android_surface` **resizes the surface
without recreating the swapchain**, so changing a virtual display's resolution needs no stream
reconnect.

Constraints (spec + measured): only `xrDestroySwapchain` may be called — never acquire/wait/release;
the producer owns the size; writing outside `VISIBLE`/`FOCUSED` is undefined; and
`xrCreateSwapchainAndroidSurfaceKHR` returns `XR_ERROR_VALIDATION_FAILURE` unless `format`,
`sampleCount`, `faceCount`, `arraySize` and `mipCount` are **all zero**. **Open item: confirm it works
under Vulkan** — the deprecated VrApi equivalent was GLES-only.

### 3.5 Decode capacity — the numbers, and the sleeper optimisation

No Quest 3 `media_codecs.xml` dump exists publicly. Two independent primary sources for **SM8550
"Kalama"**, the silicon XR2 Gen 2 derives from, agree:

- Qualcomm's shipped `media_codecs_kalama.xml`: `c2.qti.{avc,hevc,av1}.decoder` →
  **`concurrent-instances = 16`**, max 8192×8192, `blocks-per-second` up to 7,776,000, 160–220 Mbps.
- Upstream kernel `iris` driver, `sm8550_data`: `.max_session_count = 16`, `.num_vpp_pipe = 4`,
  `.max_core_mbps = 7,776,000` (`drivers/media/platform/qcom/iris/iris_platform_vpu3x.c`).

**But instance count is not the real limit — fixed per-instance cost is.** The driver's frequency
model carries a resolution-independent term `fw_cycles = fps × 489,583`, i.e. **≈44 MHz per decoder
instance at 90 fps even for a 16×16 stream**. Against a 533 MHz core that is **≈12 instances maximum
from firmware overhead alone, before decoding a pixel.** Modelled core load:

| Scenario | Core load |
|---|---|
| 9 × 960×540 @90 as separate streams | 74 % |
| **the same 9 tiles as one 2880×1620 atlas @90** | **18 %** |
| 9 × 1920×960 separate | 74 % |
| single 4096×4096 @90 | 65 % |
| today's WiVRn 3 streams @90 | 36 % |

Rows 1 and 3 are identical despite 4× the pixels: **below roughly 1080p per window you are paying
almost pure per-instance overhead.**

Android's *documented guarantee* is stricter still. `getSupportedPerformancePoints()` semantics are
"use the highest pixel count and add the frame rates" — applied to SM8550's points that is **5 streams
at 1080p90 with the normal HEVC decoder, and only 2 with the low-latency variant.** Design to the
guarantee, treat 16 as headroom.

**And the low-latency codec is a trap worth knowing about.** On Qualcomm,
`createDecoderByType("video/hevc")` returns `c2.qti.hevc.decoder`, which does **not** advertise
`FEATURE_LowLatency`; you must `createByCodecName("c2.qti.hevc.decoder.low_latency")` — and that
variant **halves the block rate (3,916,800) and caps at 70 Mbps.** Moonlight documents exactly this
(`decoder-errata.txt` item 15). WiVRn currently has the Qualcomm vendor key commented out and relies
on async callbacks + `operating-rate` + `priority = 0`
(`client/decoder/android/android_decoder.cpp:139-144`); ALVR takes the opposite approach with
`vendor.qti-ext-dec-low-latency.enable = 1`. Neither is documented as measured.

Reclaim mechanics matter for robustness: `ERROR_INSUFFICIENT_RESOURCE` (1100) and `ERROR_RECLAIMED`
(1101, *"the codec must be released, as it has moved to terminal state"*). A foreground XR app wins
reclaim contests but can still exhaust itself.

#### The sleeper: one decoder, many layers

From the OpenXR spec on `xrEndFrame`:

> *a specific swapchain (and by extension a specific swapchain image index) **may be referenced in
> `XrFrameEndInfo` multiple times**… for example to render a side by side image into a single
> swapchain image and referencing it twice **with differing image rectangles in different layers**.*

**So N desktop windows can arrive over ONE decoder as a tiled atlas and still become N
independently-posed, independently-reprojected quad layers.** Given the fixed ~44 MHz per instance,
a 9-window atlas is roughly **4× cheaper** than nine decoders.

This is important enough to be a first-class architectural option (§5.1(a′)), and it has a pleasing
property: **the deleted `5c50b9a1` transport tiled sub-rects into one image already** (§2.6). The
revert produces the atlas transport almost directly — what changes is that the client fans the tiles
out to *layers* instead of blitting them into an eye buffer.

Costs, honestly: one shared frame cadence and one IDR schedule across every tile in an atlas — which
is precisely what WiVRn's 3-stream split exists to avoid — and mipmap bleed at tile edges, so **each
tile needs a gutter**. The right answer is probably hybrid: **one atlas for the quiet text panes, plus
a dedicated instance for anything genuinely high-rate** (a video pane, a game).

No project was found doing N-window-atlas → N-quad-layers. ALVR atlases two eyes into one decoder and
splits app-side; WiVRn deliberately went the other way.

### 3.6 The exclusivity cost — and why it is *not* the blocker it looks like

**Meta does not implement `XR_EXTX_overlay`, and nothing suggests it will.** The extension is
provisional (revision 5, last modified 2021-01-13, unratified); Khronos' matrix shows Monado, Pico and
Snapdragon Spaces implementing it and every Meta runtime not. Zero of Quest 3's 101 extensions contain
"overlay". Horizon OS's model is instead **exactly one immersive OpenXR app plus N shell-owned 2D
panels** (v69 allowed three windows during immersive experiences; v81 up to twelve — all shell-owned).
Direction is one-way: Meta's system UI overlays you; you never overlay anyone.

**This sounds fatal for HypXRland's overlay model and is not, because our multi-client merge happens
on the host, not on the headset.** HypXRland, hypxrhud, hypxrpaper and a game are four OpenXR clients
against *Monado*, merged by `comp_multi` (§5.5); the Quest-side WiVRn client is, and remains, a single
exclusive immersive app. Decomposition changes what that one app submits — a merged stack of N quads
plus a projection instead of one projection — not how many apps exist on the headset. **The overlay
semantics stay where they already are.**

What genuinely is lost is any hope of coexisting with *Meta's own* apps, which is already the
situation today.

Two related lifecycle facts worth designing to:

- **When Meta's system menu opens, you go `FOCUSED` → `VISIBLE`** — your layers keep being composited
  and reprojected. Focus awareness has been mandatory for store apps since 2021. But `shouldRender`
  may go `XR_FALSE` when system UI *fully covers* the app, and whether Meta's runtime does that for
  the universal menu is undocumented — instrument it.
- **The real gate is the Android Activity lifecycle**, not OpenXR: `onPause` / surface destroyed ⇒
  `xrEndSession`. System menu = still visible; another immersive app launched = gone.

### 3.7 Two smaller findings that matter operationally

- **The manifest gotcha that fails silently.** Without `oculus.software.handtracking` +
  `com.oculus.permission.HAND_TRACKING`, Horizon OS **blocks launch of an immersive app whenever the
  user is hands-only** (`RequiresControllersLaunchInterceptor`); `am start` reports success and the
  process dies. **For a keyboard-at-a-desk productivity app, hands-only is the default case.** Declare
  it whether or not it is used. Likewise, no `com.oculus.feature.PASSTHROUGH` ⇒ `XR_FB_passthrough` is
  not even enumerated, which looks exactly like unsupported hardware.
- **Default refresh is 72 Hz.** Quest 3 supports 72/80/90/120, but *"the default display refresh rate
  for apps is 72 Hz"* — `xrRequestDisplayRefreshRateFB` is required for more, and a request is only a
  request. Enumerate at runtime; the enumeration is fixed for the session's lifetime, so an OS-level
  120 Hz toggle flipped mid-session is invisible.
- Quest 3 has **no local dimming** (`XR_META_local_dimming` absent; Quest Pro has the hardware).
- **ASW / SpaceWarp is projection-layer-only** (`XrCompositionLayerSpaceWarpInfoFB` chains onto
  `XrCompositionLayerProjectionView`; likewise the ratified `XrFrameSynthesisInfoEXT`). Quad layers
  can never use it — and do not need it, since they are reprojected from their own pose anyway.

### 3.8 Prior art to read before building

[`github.com/vr-meta/linux-vr`](https://github.com/vr-meta/linux-vr) — a community sideloaded Quest 3
app doing `KMS capture → VAAPI H.264 → network → MediaCodec → Surface → composition layer`,
*"zero-copy the whole way"*, with published device probes, readability measurements, latency budget and
manifest gotchas. It is a working existence proof of most of the thesis in this memo, at small scale.
Its own summary of the reprojection property is the cleanest statement of the case:

> *the stream rate does not have to match the headset rate. The layer is world-locked and the
> compositor reprojects it at 90 Hz whether or not a new desktop frame has arrived. A 60 Hz monitor
> looks correct in a 90 Hz headset, and a hiccup in the stream does not make the world judder.*

(Single-source community project; treat its numbers as leads to verify, not as measurements.)

---

## 4. Prior art

### 4.1 The uncomfortable headline

**Six independent teams that own both ends of an XR stream all chose to flatten. Exactly one
shipping product streams per-pane, and it caps at three panes.**

| System | Streams | Evidence |
|---|---|---|
| **Meta Quest Link / Air Link** | flattened eye textures | *"Rather than rendering a backbuffer for the headset display, the PC compositor encodes the **eye textures** … the remote streaming client is responsible for decoding, rectifying and submitting the frame to the compositor, **acting as a proxy for the PC application**."* ([Meta](https://developers.meta.com/horizon/blog/how-does-oculus-link-work-the-architecture-pipeline-and-aadt-explained/)) |
| **NVIDIA CloudXR** | flattened frames | sits *below* the OpenVR/OpenXR compositor as an HMD driver, so it never sees layers; foveation operates "across regions of the frame" ([docs](https://docs.nvidia.com/cloudxr-sdk/release/4.0/overview/overview.html)) |
| **Microsoft Holographic Remoting** | flattened frames **+ a depth channel** | the remoting runtime *is* an OpenXR runtime on the PC — it receives the app's layers and still flattens, then streams depth at half resolution for device-side reprojection ([docs](https://learn.microsoft.com/en-us/windows/mixed-reality/develop/native/holographic-remoting-overview)) |
| **ALVR** | flattened | *"On Windows ALVR takes responsibility for compositing layers returned by SteamVR"* ([wiki](https://github.com/alvr-org/ALVR/wiki/How-ALVR-works)) |
| **WiVRn** | flattened | `layer_squasher.cpp` (§2.2) |
| **Monado network driver** (MR !1557) | flattened | "a compositor target, backed by a pseudo-swapchain which encodes frames in h264/h265" |
| **Steam Link** | flattened (inferred) | uses foveated encoding — a frame-based technique |
| **Virtual Desktop** | **per-pane** | see 4.2 |

**Meta and Microsoft are the load-bearing negatives.** Both had complete layer knowledge on the PC
side and chose not to forward it. Meta's optimisation energy went into AADT — *pre-distorting* the
flattened frame to save ~70 % of the codec budget — rather than decomposing it.

A second negative worth recording: GitHub searches across `alvr-org/ALVR` and `WiVRn/WiVRn` for
"quad layer", "composition layer", "stream layers" returned **zero** proposals to forward layers.
Nobody in the FOSS VR-streaming space has asked for this.

### 4.2 Virtual Desktop — the only shipping per-pane system, and its ceiling

Multi-monitor desktop mode genuinely runs N streams composed on-device. Its published limits are the
best available predictor of where this design tops out:

- **Quest 3: up to 3 monitors.** Quest 2 / Pro / Pico 4 / Focus 3 / XR Elite: 2. Quest 1: 1.
- Resolution collapses with count: 4K single-monitor, **1440p per monitor at 2–3**.
- **Bitrate is a shared budget, not per-pane**: *"one monitor at 120 Mbps, two at 60 each, or three
  at 40 each"*; release notes cap desktop bitrate at *"120 Mbps total for all monitors"*.
- The client-side bound is decode/GPU: *"only Quest 3's XR2 Gen 2 possesses sufficient GPU capability
  to handle three monitors simultaneously"*. The host-side bound is the hardware encoder.

([UploadVR](https://www.uploadvr.com/virtual-desktop-multiple-monitors-update/), plus Godin's release
notes.) **Per-pane streaming did not buy Virtual Desktop *more* — it bought *arrangement*, and cost
resolution.** Plan for 3–6 panes, not ten, and expect the bitrate budget to be split rather than
multiplied.

### 4.3 The mature answer from remote desktop: per-region codecs in one frame, not N streams

This is the most important design lesson in the report and it argues against the obvious design.

**RDP's `MS-RDPEGFX`** is the best-specified per-surface prior art anywhere:
`RDPGFX_WIRE_TO_SURFACE_PDU_1 { surfaceId, codecId, pixelFormat, destRect, bitmapData }` — **each PDU
names a destination surface *and its own codec***. `codecId` spans `UNCOMPRESSED`, `CAVIDEO`
(RemoteFX), `CLEARCODEC`, `PLANAR`, `AVC420`, `ALPHA`, `AVC444`, `AVC444V2`
([spec](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpegfx/fb919fce-cc97-4d2b-8cf5-a737a00ef1a6)).
Within one logical frame the server sends multiple PDUs using different codecs: ClearCodec for the
syntax-highlighted editor (pixel-perfect text), H.264 for the video playing beside it. Microsoft's
own VDI telemetry puts **~80 % of typical remote session content at text**.

**Citrix HDX Thinwire** does the same and documents the downsides honestly: H.264/HEVC/AV1 on moving
regions, **MDRLE lossless for static text**, "build to lossless" refinement once motion stops. But:
progressive sharpening *"reduces cache efficiency and increases bandwidth usage"* and is now
**disabled by default**; 4:2:0 chroma makes high-contrast text *"fuzzy and harder to read"*;
selective encoding *"performs poorly when screens change constantly"* and degenerates to full-screen
encoding anyway ([docs](https://docs.citrix.com/en-us/citrix-virtual-apps-desktops/2407/graphics/thinwire.html)).

**The implication for us is a genuine fork in the road.** The twenty-year-refined answer to "mixed
text and video on a remote desktop" is *per-region codec selection inside one surface*, not N
independent video streams. That is a materially cheaper design that captures most of the text-quality
win — and on Wayland the classification signal is **already free**: `wp_content_type_v1` lets a
client declare its content type, and PipeWire's `SPA_META_VideoDamage` carries damage regions
(Sunshine already drops frames on "zero PTS delta and no damage",
[PR #4768](https://github.com/LizardByte/Sunshine/pull/4768)).

It does *not*, however, deliver the thing only decomposition delivers: **per-layer reprojection on
the headset**, and therefore the removal of the pose-critical path from desktop content. §4.5.

### 4.4 waypipe — the cautionary tale, and the design rule it produces

waypipe forwards *surfaces and buffers* with damage diffing. When Stoeckl added per-buffer video
encoding, two things went wrong, both recorded in his GSoC notes
([mstoeckl.com](https://mstoeckl.com/notes/gsoc/blog.html)):

> *"each image buffer has its own attached video stream, and the application alternates between
> buffers"* → *"a video encoded window will appear to **flicker** as it switches rapidly between the
> underlying buffers, each of whose video streams has different encoding artifacts."*

and

> SuperTuxKart at 2048×1024 used about **5 W / 25 % more power** with hardware encoding than with
> plain buffer replication.

**Design rule that falls out: bind one encoder to one *pane identity*, never to a buffer.** Our
`take_quad` descriptor already assigns a stable `swapchain` id in order of first appearance
(`compositor.cpp:948`) — that is exactly the right key, and it is already there. Second rule: expect
N hardware encode contexts to cost real power, not just real silicon.

### 4.5 The spec sentence that is the entire economic case

From the OpenXR specification:

- Layers composite **in submission order**, painter's algorithm, 0th first.
- **Every layer must be resubmitted every frame**: *"All composition layers to be drawn must be
  submitted with every `xrEndFrame` call. A layer that is omitted in this call will not be drawn."*
- **But re-rendering is not required**: *"`xrEndFrame` will use the most recently released swapchain
  image."*

That last clause is the prize. **An unchanged pane costs zero render and zero encode — you resubmit
sixty-four bytes of descriptor.** There is no equivalent for a flattened eye buffer, because the eye
buffer is a function of head pose and head pose is never constant (§1.2).

There is **no damage or dirty-rect concept anywhere in OpenXR.** `XrSwapchainSubImage::imageRect` is
*"the valid portion of the image to use"* — a static sub-region selector for atlasing, explicitly not
a damage region. Incremental pane updates are entirely our own transport problem.

One documented runtime risk on layer reuse: SteamVR (Index/Pico) incorrectly *required*
acquire/update/release every frame even for unchanged content, causing stutter for an overlay that
updated once a second; fixed in an August 2023 beta. We control WiVRn, but this is the class of bug
to expect.

### 4.6 What the Quest compositor actually costs

Meta's own numbers ([Compositor
Layers](https://developers.meta.com/horizon/documentation/unity/os-compositor-layers/)):

- **Up to 16 compositor layers per frame; layers beyond that are not rendered.** Unity's `OVROverlay`
  caps at 15, with "no more than one cylinder and one cubemap layer per scene".
- **~0.1 ms per layer** on Quest 2 at CPU/GPU level 4; **~0.6 ms for a fullscreen layer**;
  head-locked `FIXED_TO_VIEW` quads *"can be merged with zero additional cost"*.
- Quality: *"Textures appear sharper due to skipping a layer of sampling"*; *"By using compositor
  layers for rendering text and user interfaces, you can achieve high clarity even if using smaller
  fonts"*; layers render *"at the framerate of the compositor, which is always greater than or equal
  to the framerate of your application"*.
- Underlays *"are more bandwidth-intensive because the compositor must punch a hole in the eye buffer
  with an alpha mask"*; *"texture bandwidth is often a VR bottleneck"*.

The **16-layer budget is the binding constraint on this whole design**, and it collides directly with
our layer stack: hypxrpaper's equirect (1) + hypxrhud (up to 6) + N monitors (1–2 each) + a game
projection (1). For comparison, the *host* side is not the constraint at all —
`XRT_MAX_LAYERS = 128` on Linux (32 on Android),
`wivrn-xg/build-server/_deps/monado-src/src/xrt/include/xrt/xrt_limits.h:89-95`.

That the 16-layer ceiling and Virtual Desktop's independently-derived 3-monitor cap converge on the
same order of magnitude is not a coincidence.

The **WebXR Layers** explainer makes the same case with an explicit power argument: *"Due to reduced
rendering pipeline, lack of double sampling, **no need to update the layer's rendering each frame**,
the power consumption is expected to be improved."*
([explainer](https://github.com/immersive-web/layers/blob/main/explainer.md))

### 4.7 The local-only relatives: wlx-overlay-s and xrdesktop

**`wlx-overlay-s`** ([source](https://github.com/galister/wlx-overlay-s)) is the closest existing
architecture to our target, minus transport. Read from source: **one swapchain per overlay**,
recreated only when extent or stereo config changes; per-screen
`CompositionLayerQuad::new().layer_flags(..).pose(..).sub_image(..).eye_visibility(..).space(&xr.stage)`,
plus `CompositionLayerCylinderKHR` for curved screens; **no projection layer at all** — a pure stack
of quads/cylinders/equirects; layers sorted back-to-front by
`dist_sq = (hmd - overlay).length_squared() + (100 - z_order)`, descending. Its ordering rule and
per-overlay-swapchain pattern are directly liftable. (HypXRland's own sort at
`OpenXRManager.cpp:2292-2304` is already the same idea.)

**xrdesktop / wxrd** (Collabora, Valve-sponsored) puts X/Wayland windows in VR through OpenVR
overlays or a custom Vulkan renderer, using `VK_KHR_external_memory` for zero-copy window textures.
Their framing is ours: *"The open Linux model allows xrdesktop to individually manipulate windows
without needing a 'monitor' model."*

### 4.8 Where an API layer cannot help

OpenXR API layers *"can intercept any functions dispatched to an instance"* but *"must not add or
modify the definition of OpenXR functions"*. The blocker is not interception but **ownership**: after
`xrEndFrame` the application must not touch the swapchain image — ownership transfers to the runtime.
An API layer lives in the *application's* process, so rerouting a quad to a remote compositor means
exporting that swapchain image cross-process, which is exactly what Monado's IPC already does with
dma-buf fd passing. **Do this in the runtime, which we already own; not as an API layer.**

---

## 5. The architecture option matrix

### 5.0 The decomposition, stated once

Strip away the framing and there is exactly one change of substance:

> **Move part of the `layer_squasher` from the host to the headset.**

Everything else — codec policy, protocol fields, mode switching, the content tier, the runtime
product — is a consequence of that sentence or an orthogonal concern that got attached to it.

That framing matters because it tells you who has to change: **not Hyprland** (it already emits the
right thing, §2.1), and **not the OpenXR contract** (quad layers are standard). Only the two halves
of WiVRn, which we already fork.

The word **"part"** is load-bearing, and it is the concession §5.6 forces: the headset composites
16 layers and we already have about fifteen, so the squasher does not go away — it becomes the
**fallback path for everything not worth a layer**, running alongside a small forwarded set. Every
option below inherits that.

### 5.1 The options

**(a) Quad-layer forwarding inside WiVRn/Monado** — intercept at `layer_squasher::do_layers`
(`layer_squasher.cpp:523`); serialize the quad stack using the already-shipped `take_quad` shape
(§2.3); stream each quad's swapchain as its own video/image stream; the client submits them as
`XrCompositionLayerQuad`s to Horizon OS. Restore the deleted dynamic-stream protocol from `5c50b9a1`
(§2.6) as the transport. Keep the existing squash path intact as the fallback (§2.5).

**(a′) The same, but N panes arrive over ONE decoder as a tiled atlas** and fan out to N quad layers
via `imageRect` (§3.5). Same graft point, same descriptor, same client layer path — but one video
stream instead of N. Roughly **4× cheaper on the headset's video core** for small tiles, and it maps
almost directly onto the deleted `5c50b9a1` transport, which already tiled sub-rects into one image.
Costs one shared cadence and one IDR schedule across the atlas, and needs gutters between tiles. **This
is not an alternative to (a) so much as the right *first* transport for (a)** — with a dedicated
stream broken out for any genuinely high-rate pane.

**(b) Our own host-side runtime, reusing WiVRn's transport and client.** Discards Monado's
session/action/space/IPC machinery and the 19 fork patches to gain access to a function we already
own. See §6.1.

**(c) Full custom server + Android client** (the user's sketch). (b)'s cost plus discarding the
client's zero-copy decode path, passthrough, boundary work, extension fallback logic, and the deploy
pipeline. See §6.6.

**(d) No runtime change: Hyprland exports quads as OpenXR composition layers and the runtime streams
layers natively.** This is a *description of the current state plus a wish*. Hyprland already does
its half (§2.1). "The runtime streams layers natively" **is** option (a). (d) is not a distinct
option; it is (a) with the work assigned to nobody.

**(e) Option B — damage-gated frame-cycle throttle in HypXRland.** Not a decomposition at all: skip
`xrWaitFrame`/`xrEndFrame` when nothing changed and the head is still. Zero protocol change, zero
WiVRn change (§6.7). *Regime B* in §7.2.0's terms — buys the most, pays every idle-related hazard.

**(e′) Damage → ROI / dirty-rect, on a frame still emitted every vsync.** Feed the damage the
compositor already computes *downward* into VA-API, which has taken damage-shaped input for years and
is currently being handed none (§7.2.0). **Reachable today through WiVRn's existing ffmpeg VA-API path
in roughly twenty lines** (`AV_FRAME_DATA_REGIONS_OF_INTEREST`). Touches neither pacing nor timewarp,
and avoids every hazard (e) creates. *Regime A.* Collapses bits, not engine time — which §7.6 shows is
the resource that actually matters, so it is a complement to (e), not a substitute for it.

**(f) Hybrid, phased: (e′) and (e) plus the multi-client bug fixes now; (a′) then (a) staged
selectively behind them.** Recommended.

### 5.2 The matrix

| | (a) forward layers in WiVRn | (b) own host runtime | (c) full custom stack | (e) option B throttle |
|---|---|---|---|---|
| **Where the work lands** | `layer_squasher` + protocol + client stream/layer path | everything (b) replaces, then (a) anyway | (b) + the whole APK | `src/openxr/OpenXRManager.cpp` frame loop |
| **Hyprland changes** | none required; per-quad damage + angular-resolution hints are optional upside | none | none | small, one file |
| **Reuses `take_quad` descriptor** | yes, directly | yes | yes | n/a |
| **Reuses `5c50b9a1` transport** | yes, revert-and-extend | yes | no | n/a |
| **Games / xrizer** | unchanged — projection layers keep the squash path (§2.5) | must reimplement | must reimplement | unchanged |
| **Quality win (§1.5)** | **full** | full | full | none |
| **VCN-occupancy win** | full | full | full | **large** |
| **Bandwidth win** | full | full | full | large |
| **Cursor latency** | **improves** (§6.8) | improves | improves | unchanged |
| **Constrained by Quest's 16 layers** | **yes — the primary constraint** (§5.6) | yes | yes | **no** |
| **Worst-case regression risk** | real (§6.4) — needs the fallback | real | real | low; reverts by config |
| **Effort** | L–XL (multiple rounds across two repos + APK) | XXL | XXL+ | **S** |
| **Protocol churn** | yes — lockstep server+APK per change until negotiation exists (§8.9) | yes | yes | **none** |
| **Serves "runtime as product"** | yes, and naturally | nominally | nominally | neutral |

### 5.3 Recommendation

**(f): do (e′), (e) and the Phase-0b bug fixes immediately and unconditionally; stage (a′) then (a)
selectively, behind gates.**

(e′) is the lowest-risk item in the memo — twenty lines, no pacing change, no idle hazards — and
should land first regardless. (e) is nearly free, is reversible by config, and is the only thing that
buys **engine time**, the resource §7.6 shows is actually scarce. Critically, (e) is also *the
experiment that de-risks (a)*, because it manufactures exactly the long-static-gap regime that WiVRn
issue #618 says is dangerous. If the client cannot tolerate gaps, (a) has a much harder problem than
anyone has priced in, and we learn that for the cost of setting `fps_divider = 2`.

(a) is then the strategic build, and it is far cheaper than the user's framing assumes because §2
found that four of its six pieces already exist: the descriptor, the graft point, the transport (in
git history), and the client's layer-submission path. But §5.6 constrains *how much* of it gets built:
the 16-layer budget means selective forwarding, not wholesale decomposition.

**Reject (b) and (c) outright.** They buy control we already have at the price of everything else.

### 5.4 The runtime-as-product axis, and where the line goes

Given §6.9 — the boundary is nearly clean, with one blocking line — the useful question is not *how do
we extract a runtime* but *which responsibilities should move INTO it as it becomes decomposed*.
Drawing that line deliberately:

| Responsibility | Today | Belongs | Why |
|---|---|---|---|
| Layer composition, per-layer reprojection | runtime | **runtime** | it is the runtime's job and Horizon OS already does it per-layer |
| Streaming policy: which quad at which tier, resolution, codec | n/a | **runtime** | it is the only component that sees the whole layer stack, the link, and the encoder budget |
| Quad↔full-eye mode arbitration | runtime (implicitly, §2.5) | **runtime** | must be invisible to clients, and must consider *all* clients at once |
| Codec instance allocation | runtime | **runtime** | a shared scarce resource across clients |
| Layer-cap arbitration | **client** (`OpenXRManager.cpp:5482-5515`) | **runtime** — but exposed | today each client independently suspends its own layers against `maxLayerCount`, and hypxrhud does its own budget math too. With three clients that is three uncoordinated policies against one shared budget. The runtime should arbitrate; clients should be *told* their allocation. |
| Input routing between clients | **nowhere** — every overlay is unconditionally focused (§5.5) | **runtime** | today a click passes through a hypxrhud panel floating in front of a monitor, because HypXRland hit-tests only its own quads. Only the runtime sees all of them. |
| Ray hit-testing | client (`XRMath.hpp`) | **runtime** | same argument: every quad-submitting client reimplements it, and only the runtime has the full stack |
| Anchoring / pose solve (`CXRAnchor`, 1155 lines) | client | **split** | the *solve* — spring, leash, deadzone, geofence — is spatial policy every XR shell reimplements and is a candidate for `XR_HYPXR_anchored_layer` (submit `{mode, target, tuning}`, the runtime solves per frame). Moving it also dissolves the grab-latency question of §6.8b entirely. The *persistence identity* ("this is DP-3, restore where the user left it") is window semantics and stays client-side. |
| Grab state machine | client | **client** — and it already delegates the hard part | entry gating, the release-latch ring and the velocity-reject heuristic carry window-semantic history. But the *carry* is already the runtime's: `OpenXRManager.cpp:2360-2375` submits the grabbed quad in the controller's ActionSpace and the runtime late-latches it (§6.8b). |
| Cursor dot | client, drawn **into** the quad's pixels | **runtime**, reluctantly | a pointer tracking a ray with sub-frame latency is presentation — the same argument that puts a 2D cursor in a hardware plane. Today it forces a full re-encode of a 2560×1440 desktop on every hand tremor, which is why `openxr:cursor_redraw_epsilon` exists at all. |
| Chrome (move-bar, corners) | client | **client** | pure window decoration — though the *fade* should ride `XrCompositionLayerColorScaleBiasKHR` rather than pixels (§6.8) |
| Monitor plug/unplug lifecycle | client, keyed on session visibility + `XR_EXT_user_presence` | **client** | already keyed on standard signals — though the signals themselves are a runtime quality problem (`XR_EXT_user_presence` sticks at `present` on WiVRn while doffed in standby) |

The honest summary: **presentation, input arbitration and resource arbitration migrate into the
runtime; everything about *what a window is* stays in the client.** That line is close to where it
already sits. The genuine misplacements are **layer-cap arbitration**, **cross-client input routing**
(which today is not implemented anywhere at all), and **ray hit-testing**.

### 5.5 Multi-client is already the situation, not a future requirement

A busy session today is **four independent OpenXR clients** against one runtime, merged by Monado's
`comp_multi`:

| Client | Layers | Placement |
|---|---|---|
| `hypxrpaper` | 1 `XrCompositionLayerEquirect2KHR` (panorama) or 1 projection (glTF scene) | primary session, `z_order` `INT64_MIN` |
| **HypXRland** | N quads (1–2 per XR monitor) | overlay, `overlay_z = 1` |
| `hypxrhud` | up to 6 quads + `XrCompositionLayerColorScaleBiasKHR` | overlay, `hud_z = 20` |
| a Proton/xrizer game | 1 projection + its own overlay layers | primary |

`transfer_layers_locked` (`monado/src/xrt/compositor/multi/comp_multi_system.c:263`) collects
visible+active clients, qsorts by `z_order`, and re-issues every client's layers into **one flat
back-to-front array**. `XRT_MAX_LAYERS` is 128 on Linux (`xrt_limits.h:93`), so the host side has
plenty of headroom — the binding constraint is the *headset* compositor's layer budget (§3).

The structural news is good: **WiVRn sits *below* `comp_multi`** — `wivrn_session.cpp:317-324` calls
`comp_multi_create_system_compositor()` with WiVRn's `compositor : public comp_base` as the single
`xrt_compositor_native` — so multi-client already works with no WiVRn changes for the merge itself.

But the multi-client substrate has real defects, and they become load-bearing the moment "many
clients, one runtime" is a product claim rather than an accident:

| Defect | Where | Consequence |
|---|---|---|
| **Advertised budget ≠ sink budget** | each client is told `maxLayerCount = 128` (`layer_squasher.cpp:790-797` → `compositor.cpp:1198`), enforcement is **per-client, in the client process** (`oxr_api_session.c:185-196`), and the merged sink is one `layers[XRT_MAX_LAYERS]` array whose push helpers (`comp_layer_accum.c:22-32`, `:60-78`) have **no bounds check at all** — verified: `uint32_t layer_id = cla->layer_count; struct comp_layer *layer = &cla->layers[layer_id]; ... cla->layer_count++;` with nothing comparing `layer_id` to `XRT_MAX_LAYERS` | two clients at budget = 256 writes into 128 slots ⇒ **heap corruption in `wivrn-server`**. Latent today (~15 layers in practice), but it sits directly on the road being proposed |
| **`system_compositor_set_z_order` unlocks a mutex it never locked** | `comp_multi_system.c:672-683`, upstream. Verified verbatim in the vendored tree: a `//! @todo Locking?` comment, then `mc->state.z_order = z_order;` followed by `os_mutex_unlock(&msc->list_and_timing_lock);` — with no matching lock anywhere in the function | UB on a non-recursive pthread mutex, on **every session create and every focus flush** — i.e. every connect, disconnect and app swap |
| **No input arbitration whatsoever** | `ipc_server_process.c:431-462`: every overlay is unconditionally `visible = true; focused = true` | the game **and** all three overlays receive identical, simultaneous controller/hand/pose state. No capture, no exclusive grab, no routing. `system_set_focused_client` is a **stub that returns success** (`ipc_server_handler.c:1622-1628`) |
| **Blend mode is winner-takes-all** | `comp_multi_system.c:228-260` (fork patch `0003`): any visible OPAQUE client forces the whole frame OPAQUE | **one native OPAQUE game kills passthrough for the desktop and for hypxrpaper.** No per-client override |
| Unstable overlay↔overlay tie ordering | `overlay_sort_func`, `:211-226` — a non-stable `qsort` on `z_order` alone | give every client a distinct placement; today HypXRland is 1 and hypxrhud is 20, which is fine and fragile |
| `createFlags` is written and never read | `oxr_session.c:1452-1459` | `XR_OVERLAY_SESSION_CREATE_RELAXED_DISPLAY_TIME_BIT_EXTX` is a no-op |
| `MULTI_MAX_CLIENTS 64` is **soft and silent** | `comp_multi_compositor.c:1058-1067` — *"If we have too many clients, just ignore it"* | the 65th session looks healthy and renders nothing, forever |
| One session per connection | `ipc_client_compositor.c:1000-1002` | a single process cannot hold both a primary and an overlay session — **four clients means four processes** |
| Frame pacing is coupled across clients | WiVRn patch `0001`: `wait_frame` blocks until every active client's scheduled display time matches | one slow client removes the early-wake benefit for everyone; degrades rather than hangs |

**And client identity does not survive `comp_multi`.** `multi_layer_entry` carries `{xdev, xscs[],
data}` and no client field (`comp_multi_private.h:60-79`); `multi_compositor`'s `xrt_session_info` —
which *does* have `is_overlay`, `z_order`, `debug_client_id`, `debug_application_name` — never
crosses. The only provenance handle at the squasher is the raw swapchain pointer, which the fork
already exploits for `take_swapchain_id` (`compositor.h:143-145`). Attributing a quad to an
application — needed for per-client codec budgets, per-client tiers, and any policy at all — requires
a small Monado patch. **The fork already carries 19 of those; this is not a new class of cost.**

### 5.6 The reframe the layer budget forces

Put §3.2 and §5.5 together and the shape of the project changes.

**Server-side squashing is precisely what makes the layer count invisible to the headset today.** It
is what lets HypXRland budget against 128 while the headset accepts 16. Quad forwarding trades a
bandwidth win for an **8× tighter layer budget**, and converts the layer cap from a comfortable
non-issue into **the system's primary scarce resource**.

This does not kill the idea. It changes what the idea *is*:

> **Not "forward the quads". "Forward the few layers that benefit most, and keep squashing the
> rest."**

Which was always going to be a hybrid anyway (§5.5, §6.4), so the mechanism is unchanged — the
*policy* just has to be much more selective. The ranking function is obvious once stated: **forward
by (area × staleness), squash by churn.** A large static surface is worth a layer; a small busy one
is not.

That ranking picks its own first target, and it is not a desktop pane at all:

> **hypxrpaper's static equirect2 environment is the single highest-value layer to forward.** It is
> full-FoV, so it dominates the squash cost and the encode area; it is *static*, so after one
> transmission it costs literally nothing; Quest supports `equirect2` natively (it does **not** support
> `equirect` v1, and hypxrpaper already uses v2 — `hypxrpaper/src/Session.hpp:93`); and it costs one
> layer of sixteen. Highest saving-to-risk ratio in the system, and if it fails nothing important
> breaks.

---

## 6. Where I push back

The user asked to be challenged. Ten places, roughly in order of how much they change the plan.
(The eleventh — that the headset can composite as many layers as we care to send — is large enough
that it lives in §5.6 rather than here.)

### 6.1 "We need our own OpenXR runtime" — no, and the reason is structural

The premise assumes the runtime is where the flattening decision is trapped. It is not. **WiVRn does
not extend Monado's compositor; it replaces it** (`class compositor : public comp_base`,
`wivrn-xg/server/compositor/compositor.h:51`) — Monado's `comp_main`, `comp_renderer` and
`comp_target` are not in the picture at all. The flattening we object to is **one function in a file
we already fork**: `layer_squasher::do_layers`, `layer_squasher.cpp:523`.

Writing a runtime from scratch means re-implementing, before writing a single line of the thing we
actually want: the OpenXR loader contract and conformance, the session/state machine, swapchain
management across GLES / Vulkan / D3D-through-Wine, the action system and every interaction profile,
reference-space and space-relation math, hand tracking, the multi-client IPC layer, cross-GPU
allocation — plus re-deriving the 19 Monado patches this fork already carries. None of that is where
the value is. **Extending is not 10× cheaper than rewriting here; it is more like 30×, and the
rewrite's output is strictly worse for years.**

The correct reading of the user's instinct is: *we need runtime-level control over composition and
streaming policy.* We already have it. It is `layer_squasher.cpp`.

### 6.2 "Heavy on network" — that part is mostly already fixed, and selling the project on bandwidth is a mistake

Static desktop, one eye, measured in this tree: **4.97 → 1.69 Mbit/s** once `default_min_qp = 18`
(deployed) is active; **10.04 → 2.10** at the higher target. Peak frame 158,236 B → 10,183 B. The
link is 700 Mbit/s at 1.3 ms RTT with 0.0006 % retransmits.

If the pitch is "we are spending 30 Mbit/s to send a still image", the honest answer is *we mostly
already stopped doing that*. The costs that remain are **VCN occupancy**, **GPU squash**, and
**image quality** — and those are the ones to justify the work with.

**The measurement that settles it**: a static frame costs **60–80 % of a moving frame in VCN engine
time**, while its bit cost swings by ~28× (§7.6). **Bits were never the scarce resource; engine time
is** — and engine time is exactly what the QP floor did *not* touch. On a single unified VCN ring that
one 1080p60 browser decode can already disturb, that is the number the project should be judged on.

### 6.3 The biggest win is quality, not cost — and the project should be framed that way

Two resamples and head-tracked (not gaze-tracked) foveation are removed from the desktop path by
forwarding quads. On a five-monitor text workload with no eye tracking, foveation degrades exactly
the panes you are reading with your peripheral gaze. §1.5 has the chain. **"The text gets sharper"
is a claim the user will notice within thirty seconds of donning the headset.** "The GPU is less
busy" is not.

### 6.4 "Reduces host requirements" — true on average, false in the worst case, and the design must own that

A 2.23 m-wide quad at ~1.4 m subtends ≈76° of a ≈110° horizontal FoV, i.e. roughly 1,425 px of a
2,064 px eye buffer. Its source is 2,560 px. **Today that quad is being downsampled 1.8× on the way
into the eye buffer. Streaming it at native resolution sends 1.8× more pixels per changed frame than
the eye path does.**

The win is therefore *entirely* in the rate of change, not in pixel count. Concretely:

- 5 static panes → today 1.62 Gpx/s, quad mode ≈ 0. **Enormous win.**
- 1 pane scrolling at 60 Hz → today 1.62 Gpx/s, quad mode 221 Mpx/s. **Still a big win.**
- 5 panes all animating at 90 Hz → today 1.62 Gpx/s, quad mode **1.66 Gpx/s and rising with
  resolution**. **Quad mode is now the *more* expensive option.**

Two consequences the design must carry from day one: **(a) per-quad stream resolution must be driven
by angular size, not desktop mode** — and Meta's `XR_META_recommended_layer_resolution` answers this
directly, including GPU-pressure adaptation we could not compute ourselves (§7.3); **(b) the fallback
to full-eye is not just for games, it is the load-shedding valve** for the pathological case.

### 6.5 "Timesharing one codec instance beats today" — correct, and here is why, plus the caveat

Validated, and the mechanism is worth stating precisely. `hypxrva`'s README puts the hardware fact on
record: *"Strix Point has one VCN 4.x video engine with a unified queue. Decode jobs and encode jobs
do not run on separate hardware; they serialize against each other on the same engine."* Measured
consequence: a 1080p60 YouTube video with hardware decode makes head motion visibly judder, because
the encode that should have happened at once waits behind a decode job.

Today **every** encode sits on the head-motion critical path. In quad mode, only the rare full-eye
path does; a quad encode that arrives 40 ms late is still composited at the correct pose by the
headset, and reads as *stale content*, not as *judder*. That is the user's key insight and it holds.
Serialization between quad encodes is then a non-issue: they are throughput work, not latency work.

**The caveat is real and must be closed before shipping.** WiVRn upstream issue #618 is a live,
unexplained report of latency spiking to 300–500 ms *specifically when the head is held still*, worse
at lower bitrates (`research/21` §4.2) — i.e. exactly the regime a "send nothing when static" design
manufactures deliberately. Understanding #618 is a gate, not a footnote.

### 6.6 "Our own Android client replacing WiVRn" — no, for the same reason as 6.1

The existing client already has: production `XrCompositionLayerQuad` submission with sub-rect
swapchain sharing and z-ordering (`client/scene.cpp:348-368`; three live call sites), zero-copy
`AImageReader` → `AHardwareBuffer` → Vulkan decode import
(`client/decoder/android/android_decoder.cpp:120-136`), passthrough, boundary suppression, the
guardian work, the OpenXR 1.1-then-1.0 fallback with promoted-extension stripping
(`client/xr/instance.cpp:49-140`), and an APK build-and-deploy pipeline the user already operates.

Replacing it discards all of that to gain nothing that a fork cannot gain. The client changes needed
are **a dynamic decoder count** (today a compile-time 3, `client/scenes/stream.h:58-59`) and **a
layer-submission path driven by a received quad list** — both additions to code that exists.

### 6.7 The cheapest lever is not this project, and it should be gate zero

`research/21` §4.2 already identified **option B**: throttle the whole
`xrWaitFrame`/`xrBeginFrame`/`xrEndFrame` cycle in HypXRland when *(no layer released a swapchain
image)* **and** *(head-pose delta below threshold)*. `layer_commit` is then never called, nothing is
squashed, nothing is encoded, and the Quest reprojects its last frame. **Zero protocol change, zero
WiVRn change, entirely inside our repository.** And upstream's shipped `fps_divider` (PR #862) is the
existence proof that the client tolerates a reduced encode rate while the display stays at refresh —
so `fps_divider = 2` tests the client half of the hypothesis *tonight, with no code at all*.

Option B captures a large share of the **VCN-occupancy and GPU-squash** win — the two costs §6.2 says
are the real ones — for a fraction of a percent of quad streaming's effort. It does **not** capture
the quality win (§6.3), which is the strategic prize.

That ordering matters: option B is not a competitor to this project, it is its **risk-free
first payment**, and it also happens to be the experiment that tells us whether the client tolerates
long gaps at all (issue #618, §6.5). **Do option B first regardless of what is decided about quad
streaming.**

### 6.8 The cursor inverts the user's latency-class claim — and then inverts back into a win

"Quads are off the latency-critical path" is true for *content* and false for the *cursor*. The
endpoint cursor is drawn **into the quad's swapchain by the host** (`openxr:cursor`, with the
`cursor_redraw_epsilon` dead-band at `ConfigValues.cpp:918-922` existing precisely because idle
cursor hover once caused a macroblock storm). In a naive quad-forwarding design, **moving the mouse
turns an otherwise-static 2560×1440 pane into a high-rate stream**, destroying the entire premise.

But the fix is better than today, not worse: **the client already has the aim ray locally** — it is
the client's own controller or hand. A client-drawn endpoint cursor has *zero* network latency, which
is strictly better than the current host-drawn one. The host stays authoritative for where the click
actually lands; only the *dot* moves locally.

So: promote the cursor out of the pane's pixels and into the client. This is one of the clearest
arguments for the decomposition, and it only appears once you take the decomposition seriously. (It
costs one layer of sixteen — §3.2 — which is the recurring tax on every good idea in this memo.)

The same trick applies to the chrome fade (`OpenXRManager.cpp:1810-1821`): a 150 ms alpha ramp that
currently forces a full re-composite per frame can move into
`XrCompositionLayerColorScaleBiasKHR`, which the client already supports (`client/scene.cpp:370-378`)
and Quest already exposes. **Zero pixels, zero encode, zero extra layers.**

### 6.8b The grab drag is *better* under forwarding — but the default config throws it away

I expected host-authoritative pose during a drag to be a latency disaster. It is not, and the reason
is instructive: **the runtime already owns the carry.** `OpenXRManager.cpp:2360-2375` submits a
MOVE-grabbed quad **in the controller's grip (or pinch) `ActionSpace`** with a fixed offset
(`XRAnchor.cpp:264`, `res.pose = m_grabOffset`), and the runtime late-latches it at display time.
Doc 03 calls this "zero added latency" and it is right. **Under forwarding it gets strictly better,
because the headset *is* the tracking source** — a grip-anchored quad tracks the controller with no
network in the loop at all. Only the pixels are stale, and a dragged window's pixels are static.

**But the shipped default discards exactly that property.** `XRAnchor.cpp:255-264`:

```cpp
if (in.grabFilter && (m_grabHandActive || in.grabFilterScopeAll) && dev) {
    world     = oneEuroStepPose(m_carryFilter, world, dt, ...);
    res.space = XR_SPACE_LOCAL_FLOOR;   // drops the device-space late-latch
```

`openxr:grab_filter` defaults **true** and `grab_filter_scope` defaults **`all`** (live-tuned
2026-07-09 for controller jitter), so **every drag today submits a world pose in `LOCAL_FLOOR`** —
which under forwarding must cross the network before the pane moves.

Measured RTT on this rig is **3–5 ms** when WiFi is healthy (the pathological state was 200–1258 ms).
Pose-to-photon for a `LOCAL_FLOOR` quad would be roughly **25–40 ms** (host frame + net + decode +
headset frame); for a grip-space quad, effectively zero.

**The design, in order of preference:**

1. **Force `grab_filter` off for the duration of a grab in quad-forward mode** and submit in
   grip/pinch space. The drag is pinned to the hand, indistinguishable from today. Cost: the jitter
   the 1€ filter removes comes back.
2. **If that jitter bites, move the 1€ filter to the headset** — same algorithm, same two parameters,
   applied after the hop to the runtime's own late-latched pose. `oneEuroStepPose` in `XRMath.hpp` is
   allocation-free POD and ports trivially. This is the right answer and it is cheap.
3. **Do not move the grab machine into the runtime.** Entry gating, the release-latch ring, the
   velocity-reject heuristic and re-anchoring into a persistent mode carry 500 ms of window-semantic
   history each.

What the user would feel: naive world-pose port → the pane trails the hand by ~30 ms with visible
rubber-banding, wrong immediately. Device-space → identical to today, slightly shakier. Device-space
plus a headset-side 1€ filter → identical to today, including the smoothing.

### 6.9 "The runtime as a standalone product" — one blocking line, then discipline, not a rewrite

The user's second framing — *HypXRland should be just an app; others should build against my runtime*
— is **most of the way true, with one line standing in the way**.

What is already true: HypXRland's *submission model* is pure standard OpenXR. It submits only
`XrCompositionLayerQuad`, gates every optional extension on `hasExt()` with documented fallbacks
(`XRSession.cpp:84-135`), negotiates blend mode from `xrEnumerateEnvironmentBlendModes`, and handles
`XrEventDataReferenceSpaceChangePending` properly. There is no private channel between compositor and
runtime; hypxrpaper, hypxrhud and a Proton game are three further independent clients against the same
runtime.

What is **not** true: `XRSession.cpp:75-82` makes `XR_MNDX_egl_enable` a **hard requirement**, and it
is a Monado vendor provisional extension that nothing else implements. **HypXRland cannot start
against SteamVR, Meta's PC runtime, or any conformant runtime.** Making the runtime accept a plain
GLES or Vulkan binding without it is the single highest-leverage change for this ambition, and no
amount of documentation substitutes for it.

Beyond that one blocker, an inventory of runtime-specific dependencies in `src/openxr/` sorts into
three classes: a dozen **scars from Monado/WiVRn/Mesa bugs** (the unbind-across-runtime-GL-calls
contract at `XRGraphics.cpp:60-73`; the acquire→release context contract at
`OpenXRManager.cpp:1826-1832`, where a missing current context makes Monado's `eglCreateSyncKHR` fence
fail *silently* and fall back to an unsynchronised path that **corrupts our heap in-process**; the
whole bring-up on a bounded helper thread after a sick DP link froze the desktop on 2026-07-15,
`OpenXRManager.cpp:809-822`), a handful of legitimately client-side defences, and — the interesting
class — **six genuine gaps in OpenXR itself** that a runtime product would fill with named extensions:

1. a **compositor-GPU query** (today a two-tier probe, `XRGpuProbe.hpp:36-67`, because
   `vulkan_enable2` answers a different question and is wrong on WiVRn's split GPU mode);
2. **buffer import constraints** (today `openxr:force_linear`; fork patch `0016` literally is this);
3. a **runtime-availability signal** (today inotify on hard-coded `monado.pid` / `wivrn/comp_ipc`
   basenames, `XRMonitorConfig.cpp:500-524`, because the loader offers nothing);
4. **per-instance runtime selection** (today `setenv("XR_RUNTIME_JSON")` before the first loader call,
   with a glibc `environ`-realloc hazard, `OpenXRManager.cpp:407-430`);
5. the **GL interop contract**, which is real, load-bearing, and entirely undocumented;
6. **cross-client layer arbitration, input routing and per-client blend mode** — §5.5.

Those six are the actual content of "a runtime other desktops can target". They are also, notably,
things we discovered by being bitten.

Put sharply: **the *interface* HypXRland uses is `XrCompositionLayerQuad` plus
`XR_KHR_composition_layer_color_scale_bias`, and a GNOME or KDE XR shell could drive wivrn-xg with
essentially that alone.** The standalone-runtime ambition is not something to invent. It is something
to **unblock (one line), document, and stop breaking**.

The extraction the user is asking for is therefore **not a code migration — it is a documentation,
versioning and testing commitment.** That is worth doing, and §5 places it, but it should not be
allowed to justify a rewrite, and it should be sized honestly: the addressable market for "other
desktop environments targeting our runtime" is currently zero, and the cost of a stable public
surface is paid on every future change. Do the parts that are free (keep the boundary clean, write
the extension specs down, version the wire format properly) and defer the parts that are not (a
conformance suite, ABI stability guarantees, third-party support).

**And there is a trap inside the ambition worth naming.** A *content-typed-layer OpenXR extension* —
the most obvious way to express the tier ladder as a public runtime feature — would make the runtime
**less** portable, not more: a single-vendor semantic-layer extension that no other runtime implements
is not a standard, it is lock-in that other desktops must write a special path for. The way to serve
the ambition is the opposite: **derive the tiers from what any standard client already does** (§8.10)
so that a shell which has never heard of us gets damage-driven quad streaming for free.

One real gap cuts the other way, and it is worse than "provisional". `XR_EXTX_overlay` is extension 34,
`provisional="true"`, **revision 5, not ratified, last modified 2021-01-13**, filed in the spec's *List
of Provisional Extensions*. The spec chapter has seen only mechanical release and copyright-bump
commits since 2024. Two open, unresolved issues target exactly the semantics we depend on —
[OpenXR-Docs#160](https://github.com/KhronosGroup/OpenXR-Docs/issues/160) *"clarify `xrEndFrame`
behavior for overlay sessions"* (open since 2023) and
[#162](https://github.com/KhronosGroup/OpenXR-Docs/issues/162) *"No way to transition out of
SYNCHRONIZED from the application side"* — and the loader-side implementation attempt
[OpenXR-SDK-Source#268](https://github.com/KhronosGroup/OpenXR-SDK-Source/pull/268) was **closed
unmerged**. No implementer besides Monado could be found, and definitively not Quest.

**The entire multi-client story rests on a five-year-abandoned provisional extension with one
implementer.** That is not a reason to abandon it — *we are the implementer, and it works.* It is a
reason to stop treating the semantics as something Khronos will one day bless, and start treating them
as **ours to specify**: write `XR_HYPXR_overlay` with the input routing, shared layer budget and
per-client blend mode that `EXTX` never got, and ship it. That is the honest version of "a runtime
other desktops can target".

---

## 7. Codec and transport design

### 7.1 The latency-class insight, validated with one correction

The user's central claim — *quads are off the latency-critical path, so a quad can hitch without
reading as jitter* — is **correct, and it is the most important idea in the proposal.**

The mechanism, precisely: a composition layer is reprojected by the headset compositor **against its
own pose, at compositor rate, every frame, whether or not new content arrived**. A quad whose pixels
are 40 ms stale is still drawn in exactly the right place. A flattened eye buffer that is 40 ms stale
is drawn in the *wrong* place, and no amount of timewarp fully recovers translation. Today every
encode on this box sits on the head-motion critical path, which is why `hypxrva` had to exist at all
(§6.5).

**The correction is the cursor** (§6.8): it is the one desktop element where staleness reads as lag
rather than as staleness, and it must be lifted out of the pane's pixels and drawn client-side. With
that correction the claim holds completely.

**A second correction: the mouse-move problem is bigger than the cursor glyph.** Hover effects,
focus rings, text-selection highlights and tooltips all dirty the pane on pointer motion. A design
that only promotes the cursor dot will still see panes go hot whenever the pointer crosses them.
This is a real limit on how quiet a "static" desktop can be made, and it should be measured before
being designed around.

### 7.2 Damage-driven encode: the policy

#### 7.2.0 The encoder below us already takes damage — and nobody is feeding it

The single most actionable finding in the codec thread, and it sits *underneath* WiVRn rather than
inside it. VA-API has carried damage-shaped inputs for years:

| Mechanism | What it does |
|---|---|
| `VAConfigAttribEncDirtyRect` + `VAEncMiscParameterBufferDirtyRect` | hand the encoder a list of changed rectangles; **"areas not covered are assumed unchanged"** (`va.h:2937-2951`) |
| `VAConfigAttribEncROI` (with the qp-delta flag) | per-region QP bias — spend bits where the user is looking, starve the rest |
| `VAConfigAttribEncSkipFrame` | encode a frame *as skipped*: near-zero bytes, **cadence preserved** |

Mesa 26.2.1's gallium layer carries `PIPE_VIDEO_CAP_ENC_DIRTY_RECTS`, `_MOVE_RECTS`, `_QP_MAPS` and
friends, and radeonsi's VCN path has ROI machinery in-tree.

**And ROI is reachable today, through WiVRn's existing ffmpeg VA-API path, in roughly twenty lines** —
`AV_FRAME_DATA_REGIONS_OF_INTEREST` side data on the frame, with adaptive quantisation enabled.
Dirty-rect and skip-frame need direct VA calls or a small libavcodec patch.

This opens a **third regime** that this memo did not have before, sitting between "change nothing" and
"forward quads":

> **Regime A — damage → ROI/dirty-rect, on a frame still emitted every vsync.** Touches neither frame
> pacing nor timewarp. Avoids *every* idle-related hazard: no congestion-window collapse, no WiFi
> power-save transition, no head-of-line surprise, no reference-frame decay. Bits collapse; engine
> time does not.
>
> **Regime B — damage → skip the frame entirely** (option B, §6.7). Buys more, and pays all of those
> costs.

Given §7.6's finding that **engine time, not bits, is the scarce resource**, Regime A does *not*
subsume Regime B — but it is strictly safer, lands sooner, and is the right thing to measure first.
`VAConfigAttribEncSkipFrame` is the interesting middle: near-zero bytes *with* cadence preserved,
which is most of B's saving without B's hazards.

#### 7.2.1 The per-pane policy

Given a per-quad stream keyed on a stable **pane identity** (§4.4 — never on a buffer):

- **No damage → send nothing** (or a skip-frame, per §7.2.0). The client resubmits the layer
  descriptor and the Quest compositor keeps reprojecting the last decoded image. This is the entire
  economic case (§4.5).
- **Damage → encode the pane**, with the damage rects handed *down* to VA as dirty-rects rather than
  only being used to decide whether to encode. Sub-rect encode is the optimisation *after* that;
  whole-pane encode on damage already captures most of the win because the frequency drops by orders
  of magnitude, not the size.
- **Keyframe policy per pane.** A newly-visible or newly-created pane needs an IDR. A pane that has
  been quiet needs nothing. Today every backend runs an **infinite GOP** with IDR forced only by
  feedback (`video_encoder_va.cpp:317`, `x264:149`, `nvenc:228`) and the three IDR state machines are
  already independent per stream (`compositor.cpp:1264-1272`) — so per-pane IDR is not a new concept
  here, only more instances of an existing one.
- **Loss recovery must be per-pane.** On TCP (this box) there is no loss; the concern is a future UDP
  transport, where a lost slice must trigger an IDR for *that pane only*. The existing per-stream
  feedback routing already has the right shape.
- **The `intraRefreshPeriod` trap.** WiVRn enables a rolling intra-refresh wave at a 50 % duty cycle,
  forever (`research/21` §1.3 erratum), and A/B measurement showed it accounts for **~97 % of the
  steady-state cost of a static frame** on the NVENC path. A per-pane design that leaves that on
  would spend its entire budget refreshing panes nobody is looking at. **Intra-refresh must be off,
  or period-extended, for quiet panes.** This is also, independently, a large lever on today's
  pipeline.

### 7.3 Per-quad resolution — ask the runtime, don't compute it

From §6.4: a 2.23 m quad at 1.4 m subtends ~76° of a ~110° FoV — about 1,425 px of a 2,064 px eye
buffer — while its source is 2,560 px. Streaming natively sends 1.8× more pixels per changed frame
than today's path allocates to it, so per-quad stream resolution must adapt.

**And Meta already answers this question for us.** `XR_META_recommended_layer_resolution` (§3.3) takes
a layer plus a predicted display time and returns `recommendedImageDimensions`, computed from *"static
properties such as screen resolution and HMD type, but also dynamic ones such as layer positioning and
system-wide GPU utilization."* The client queries it per quad and reports the number upstream; the host
sizes the stream to it. That is strictly better than any arithmetic we could do, because it includes
GPU-pressure adaptation we cannot see.

Fallback for a headset without the extension (and a sanity clamp on top of it):

```
stream_px = clamp(content_px, min_px, ceil(angular_extent_deg × headset_px_per_deg × oversample))
```

with `oversample ≈ 1.2–1.4` to keep the sharpness win rather than throwing it away. The host already
solves each quad's pose and size every frame (`SXRSolveResult`, `src/openxr/XRAnchor.hpp:448-454`), so
the inputs are free. Transitions need hysteresis; a resolution change is a natural IDR point, and
`XR_FB_swapchain_update_state_android_surface` can resize a surface swapchain **without recreating it**
(§3.4), so a resolution change need not cost a stream reconnect.

This also gives the load-shedding valve a graceful first move: **before** falling back to full-eye,
drop `oversample` toward 1.0.

Calibration to keep in mind: measured readability on Quest 3 is **20 px comfortable / 16 px borderline**
at 51.2 PPD (§3.3), and the threshold is angular glyph size. The correct response to unreadable text is
to **widen the layer, not to scale the desktop up** — which is a *layout* decision the client owns, not
a streaming one.

### 7.4 Chroma and text — the honest constraint

Citrix's finding (§4.3) is the one to take seriously: **4:2:0 chroma makes high-contrast text fuzzy**,
and that is the exact workload here. Today's pipeline pays it. Options, in increasing order of cost:

1. **Keep 4:2:0 but remove the two resamples and foveation** (§1.5). This alone is a large visible
   improvement and requires no codec change.
2. **Per-pane codec/quality selection** — the RDP lesson (§4.3). A quiet text pane can afford a very
   low QP because it is sent once; a video pane runs normal rate control.
3. **Lossless or near-lossless for quiet text panes.** Once a pane is static, the marginal cost of
   sending it *perfectly* is a one-off. This is Citrix's "build to lossless", and it is the single
   most promising quality idea in the report — with Citrix's own warning attached: their progressive
   mode is disabled by default because refinement hurt cache efficiency and bandwidth.
4. ~~**4:4:4 / screen-content coding.**~~ **Ruled out on this hardware.** `libva` defines
   `VAProfileHEVCSccMain`, `SccMain10`, `SccMain444`, `SccMain444_10`
   (`/usr/include/va/va.h:534-539`), but the AMD encoder does not implement them: **VCN 4.0.5 HEVC
   encode is Main / Main10, 4:2:0 only, and no AMD ASIC offers 4:4:4 or SCC *encode* at all.** AV1
   encode is present but Profile 0, also 4:2:0. 4:4:4 encode would mean Intel iHD Gen12+ or software —
   neither of which is on the table here.

   **This matters more than it looks.** 4:4:4 chroma is exactly what makes small text crisp, and it is
   *permanently unavailable* on this box. That removes the most direct quality lever and leaves
   option 1 — **removing the two resamples and foveation** — as the principal quality win available
   at all. It is a further argument for the decomposition, arrived at from the opposite direction.

### 7.5 Transport

**Head-of-line blocking is real, and there is in-tree proof.** Today three streams share one TCP
connection, but all three are equally urgent and equally paced, so nothing starves. With N panes of
wildly different sizes and urgencies, a large video-pane burst can block a 300-byte text-pane update
behind it. This is not speculative: **the recorder's transfer pacer chunks at a 40 ms interval
precisely so its bulk traffic does not block the video stream on the shared socket.** The fork already
discovered this problem and already worked around it once.

Recommendation: **do not put N panes on one TCP connection.** Either UDP shards, or exactly **two** TCP
connections split by latency class — interactive versus bulk — with oldest-deadline-first scheduling
and chunked bursts above ~32 kB. Given the link is 700 Mbit/s at 1.3 ms with 0.0006 % retransmits,
this is a scheduling and fairness concern, not a throughput one.

**The burst profile changes character, and it is worse in a specific, fixable way.** Today: a
metronomic burst every 11.1 ms, socket busy 93.3 % of wall clock (`research/21` §2). With quads: long
silence, then an occasional large burst. Long silence is good for radio power and airtime, and bad for
three separate mechanisms:

1. **TCP congestion-window collapse.** `tcp_slow_start_after_idle = 1` collapses cwnd after more than
   one RTO of idle, so the first burst after a quiet period ramps from scratch — exactly when the user
   has just started scrolling. There is a sysctl fix.
2. **WiFi power-save and rate adaptation.** WMM / U-APSD transitions and rate-adaptation state both
   decay without recent traffic; the station has to climb back.
3. **Reference-frame decay** on the encoder side.

**A keepalive is the wrong shape and does not work.** Congestion-window validation decays on
*application-limited* flows too, so trickling bytes does not preserve cwnd. The correct mechanism is a
**minimum frame rate**: emit a frame every 100–200 ms even at zero damage — about **0.2 Mbit/s**, and
one mechanism doing four jobs at once (station stays awake, rate adaptation stays fed, cwnd stays
alive, references stay fresh). This is also, neatly, the natural implementation of §7.2.0's Regime A.

**The LEDBAT pacer is the template for the per-pane senders.** `hypxr_transfer_pacer` (wired at
`wivrn_session.cpp:1312-1366`) is an already-unit-tested, delay-based controller operating on this
exact socket with measured link constants — a 40 ms standing-queue target, yield-fast / take-back-
slowly, and a 400 KiB/s floor established as harmless. Any per-pane sender that must yield to
interactive updates should be built on it rather than beside it.

All three decay mechanisms above are, most likely, the story behind WiVRn issue #618 (300–500 ms
latency spikes *specifically when the head is held still*, worse at lower bitrates — §6.5).
**Investigating #618 is therefore not a side quest; it is the direct study of this design's steady
state — and the minimum-frame-rate floor is the leading candidate fix.**

### 7.6 Codec instance budget

**The headline, and it reframes §6.2 one more time: engine time, not bits, is the scarce resource.**

A static frame costs roughly **60–80 % of a moving frame in VCN engine time**, while its bit cost
swings by ~28×. The rate-control work this project has already shipped (the QP floor) moved the number
that was never the constraint. **Damage-gating *bits* changes almost nothing about engine pressure;
only not encoding at all does.** That is the strongest argument in the memo for Regime B (§7.2.0) over
Regime A — and the reason the two are complements rather than substitutes.

- **Host.** The 890M has **one VCN 4.0.5 engine on a single unified `vcn_unified_0` ring**; decode and
  encode serialise against each other on it (`hypxrva/README.md`), and it is already fully spoken for
  — one 1080p60 Chrome decode is enough to make head motion judder. Today's three encoders already run
  their `encode()` calls **sequentially on one thread** (`compositor.h:169`, `compositor.cpp:658-685`)
  with a single process-wide sender thread. **N quad streams would serialise on that same ring**, and
  because per-frame engine cost is dominated by fixed work rather than by pixels, **N panes multiply
  engine pressure even at a constant total pixel count.** This is the mirror image of the headset's
  per-decoder-instance overhead (§3.5), and it points the same way: **prefer the atlas.**
- **Context lifecycle.** A VA-API encode context costs tens of megabytes and tens of milliseconds on
  first create. **Allocate a fixed pool at session start and never churn it per pane** — a design that
  spins encoder contexts up and down as panes come and go would stall the ring at exactly the wrong
  moments. (Closes an open question the earlier draft carried.)
- **Device.** §3.5 has the numbers: nominally 16 concurrent instances, ~12 in practice from a fixed
  ~44 MHz-per-instance firmware cost at 90 fps, and only **5 at 1080p90 under Android's documented
  guarantee** (2 with the low-latency codec variant). The client's decoder count is a compile-time 3
  today (`client/scenes/stream.h:58-59`) and must become dynamic. Each decoder already does zero-copy
  `AImageReader` → `AHardwareBuffer` → Vulkan import — and for rectilinear panes,
  `XR_KHR_android_surface_swapchain` removes even that (§3.4).
- **Consequence:** the design must include a **pane-to-stream allocator**, and the *first* thing it
  should reach for is not a second codec instance but **the atlas** (§3.5, option a′): below ~1080p per
  pane you pay almost pure per-instance overhead, so nine tiles in one stream is ~4× cheaper than nine
  streams. Break out a dedicated instance only for a genuinely high-rate pane. Degrade gracefully — not
  fail — when instances run out (`ERROR_INSUFFICIENT_RESOURCE` is a real code you will see).
  The user's intuition that *"even timesharing a single codec instance beats today"* is right, and it
  should be the **default** assumption rather than the fallback.

### 7.7 Audio and upstream input

Unchanged. Pose, controller and hand data flow upstream independently of the video path, and audio
rides its own channels. Nothing in this proposal touches them. This is worth stating explicitly
because it bounds the blast radius: **the decomposition is a change to the downstream video path
only.**

---

## 8. The content tier above pixels

> The user's amendment: *"apps that are 'native' to my OpenXR implementation and better optimized for
> it — the HUD would be designed to be optimal on our platform. This could eventually even look like
> offloading text rendering, etc. to the headset, too, and not requiring a video encode to display
> content."*

The ambition is right about first-party apps and right about the runtime as a product. It is a trap
for the desktop. And the T2-vs-T3 debate turns out to be the wrong debate.

### 8.1 The tier ladder

| Tier | What crosses the wire | Fidelity | For |
|---|---|---|---|
| **T0** | flattened eye buffers | 4:2:0, twice resampled | foreign apps, games, the fallback |
| **T1** | per-pane video, damage-gated | 4:2:0 | anything moving |
| **T2** | per-pane lossless dirty rects | **pixel-exact** | a pane that has gone quiet |
| **T2.5** | T2 + a copy-rect / scroll primitive | pixel-exact | scrolling text |
| **T2.75** | host-rasterized **glyph masks**, cached on the headset | pixel-exact | see §8.4 — right design, no source |
| **T3** | glyph runs rendered *by* the headset | divergent | — |
| **T4** | the app runs on the headset | our font, so exact | first-party UI |

### 8.2 The arithmetic

Reference pane 2560×1440, terminal cell ~10×20 px, zstd ≈ 12× on rendered text, 100 Mbit/s link.

| Event | T2 | T2.5 | T3 |
|---|---|---|---|
| One character typed | ~320 B | ~320 B | ~48 B |
| …at 15 cps | **38 kbit/s** | 38 kbit/s | **5.8 kbit/s** |
| Scroll one text line | 1,228,800 B | 24 B CopyRect + 17.1 kB strip | 3.1 kB |
| …at 60 lines/s | **590 Mbit/s** ❌ | **8.2 Mbit/s** ✅ | **1.5 Mbit/s** ✅ |
| Idle second, 5 static panes | **0 pixel bytes** | 0 | 0 |

**First: T2 already makes static text free, and T3's bandwidth argument is irrelevant.** Typing costs
38 kbit/s at T2 and 5.8 kbit/s at T3 — a difference of **32 kbit/s, 0.032 % of the link.** You would
pay T3's entire correctness bill (§8.3) to reclaim three hundredths of one percent.

**Second, and this is the finding: copy-rect captures almost all of the rest.**

> **(590 − 8.2) / (590 − 1.5) = 98.9 %.** T2.5's copy-rect captures **98.9 % of the total win that T3
> offers over T2** — for one 24-byte message and a scroll detector.

T2's pathological case is worse than bandwidth alone suggests: full-pane repaint at 1.23 MB/frame
tops out around **5–10 fps** even given the whole link, and burns roughly **one x86 core of zstd-1 per
scrolling pane** (14 MiB/frame at 30 fps ≈ 442 MB/s). Meanwhile a 1080p30 *video* window at T2 would
need **1.24 Gbit/s** against H.265's ~15 Mbit/s — **83× worse.** Tier switching is not an
optimisation; it is a correctness requirement.

### 8.3 Why T3 is a trap for the desktop

**The epitaph is in FreeRDP's source.** `[MS-RDPEGDI]`'s glyph machinery is real and well designed —
ten glyph caches of 254 entries with cell sizes 4…256 bytes, plus a 256-entry fragment cache so a
repeated word costs one order (`libfreerdp/core/settings.c:1145-1188`). Twenty-five years later:

```c
!freerdp_settings_set_uint32(settings, FreeRDP_GlyphSupportLevel, GLYPH_SUPPORT_NONE))   // settings.c:1134
WLog_WARN(TAG, "[experimental] enabled GlyphSupportLevel %s, expect visual artefacts!");  // settings.c:388-392
```

**The leading open-source RDP client ships glyph remoting off by default, marked experimental, warning
that it produces visual artefacts.**

**The cause is not bandwidth — the semantic channel was deliberately deleted.** GDI order remoting
required the server to *see* GDI calls. DWM's composited desktop removed the serialised order stream;
DirectWrite made a glyph a three-channel, position- and background-dependent thing rather than the
1-bit mask the cache design assumes. X11 core fonts died the same way, for reasons Keith Packard wrote
down in 2001: the core protocol *"made high-quality antialiased rendering impossible at the server
level"*. SPICE shipped `DRAW_TEXT` with glyph strings and lost to video anyway. NX3's differential
X-protocol caching became NoMachine 4's framebuffer encoder.

**Hyprland is already on the far side of that same change.** Every Wayland client hands the compositor
a texture, not a glyph run. **We would be starting where Microsoft ended up**, and getting the channel
back means patching every application.

Two honesty checks:

- Every fatal divergence is a *rasterizer* divergence — two FreeType builds, two hinting
  configurations, two gamma choices. Shaping divergence (ligatures, kerning, bidi, complex scripts) is
  avoidable by sending positioned glyph IDs rather than text; rasterizer divergence is not.
- **Subpixel AA is already destroyed in VR at every tier**, by lens distortion plus per-channel
  chromatic-aberration correction. *"My terminal must look identical"* was never on the table. T2 is
  still strictly closest — but the claim is *"closest available"*, not *"exact"*.

**Verdict: do not build T3 for desktop panes.**

### 8.4 T2.75 — the design that is right and has no source

X11's answer was not to abandon server-side glyphs but to **invert ownership**: with RENDER the
*client* rasterizes and uploads each glyph as *"an 'alpha' mask (a rectangular image of opacity
values) covering the glyph shape"*, which the server caches and shares across applications,
downloaded incrementally. It is the only server-side text-remoting scheme that never died —
`XRenderCompositeGlyphs` still ships.

Applied here, **T2.75** is: the *host* rasterizes, so the user's fonts, hinting and gamma win and
§8.3's divergence vanishes entirely; the wire carries cached glyph *masks* plus positioned runs. It
has T3's bandwidth and T2's fidelity. It is genuinely the right design.

**And it is dead on arrival for the same reason T3 is: there is no semantic source.** A Wayland
compositor receives a finished texture. Worth writing down anyway: if a first-party terminal ever
volunteers its glyph runs over a Wayland extension, **T2.75, not T3, is the shape to build.**

### 8.5 The tier-switching policy — steal Citrix's

Citrix HDX Thinwire's **Selective Encoder** is the default mode of a product that has served
enterprise text workloads for two decades: *"Text and simple graphics"* → lossless; complex/
photographic → JPEG; motion → *"video codec, H.264 / H.265 / AV1 in YUV 4:2:0"*; plus **Intelligent
Build to Lossless**, a *"pixel perfect image once movement has stopped"*
([docs](https://docs.citrix.com/en-us/citrix-virtual-apps-desktops/graphics/thinwire/thinwire-selective-encoder.html)).
That is exactly the T1/T2 policy this design needs. **Do not invent it.**

**Classifier**, per quad per frame, from data we already have:

1. `dirty_fraction` = damaged pixels ÷ pane pixels.
2. **entropy proxy** = the zstd-level-1 ratio on the damage payload *we were going to send anyway*.
   Ratio > 6 ⇒ text/UI; ratio < 2.5 ⇒ photographic.
3. `dirty_fraction < 0.05 && ratio > 6` → **T2**; else if a scroll vector was detected → **T2.5**;
   else → **T1**.

**Hysteresis, asymmetric**: up (T2 → T1) after ~150 ms / 10 frames — a starting video must not
stutter. Down (T1 → T2) only after ~2 s — flapping on a scroll burst is worse than staying on video.

**Scroll detection without a protocol**: hash each row of the pane (one GPU pass, 1440 u64s) and
search `dy ∈ [−H, H]` for a maximal matching run. Sub-millisecond, and it nails terminals and editors.
*Skeptical note worth keeping:* a video encoder's motion estimation computes this for free, so if a
pane is already on T1, copy-rect buys nothing. **Copy-rect's entire value is letting a pane stay on
T2 through a scroll** instead of being forced onto lossy video.

**Glitch-free tier change** — the quad and its swapchain never change; only the *fill path* does:

- **T2 → T1**: the first video frame must be a full-pane IDR. Until it decodes, **do not release a new
  swapchain image** — OpenXR explicitly permits resubmitting a layer without a new release (§8.10) —
  so the quad holds its last good frame for one to three frames instead of flashing.
- **T1 → T2**: seed the T2 canvas by GPU-blitting the last decoded video frame, then apply damage
  rects on top. No full-pane retransmit, which is what makes aggressive down-switching free and
  therefore safe.
- **Artifact cleanup after a down-switch**: dribble a tiled lossless refresh at low priority —
  1.23 MB over 2 s = 5 Mbit/s. Citrix's Intelligent Build to Lossless, verbatim.

### 8.6 Where the ambition is genuinely right: T4, for first-party surfaces

The target is **T4, not T3**, and the scope is **first-party UI, not the desktop**. The discriminator
is exactly one thing: **who chose the font.**

`hypxrhud` is the proof, and it is further along than anyone intended:

- **It draws glyphs and vector primitives, never images** — a stb_truetype glyph loop plus a rounded
  panel, a confidence bar and dual gauges (`hypxrhud/src/PanelText.cpp:124-150`, `:240`, `:271-273`).
- **The font is ours and is baked into the binary** — bundled OFL Liberation Mono, *"no runtime
  font-path dependency, no system font libs"* (`src/PanelText.cpp:10-13`). **This single fact is why
  §8.3's divergence argument does not apply to it.**
- **Content is already semantic on the wire**: `SPanelContent{kind, vector<SLine{text, EColor, big}>,
  vector<SGauge{label, percent, charging}>, confidence}` (`src/Panel.hpp:36-72`), with colour as an
  abstract *role* enum (`EColor::Normal/Dim/Accent/Good/Warn/Bad`, `src/Panel.hpp:25-32`), delivered
  as D-Bus `a{sv}` (`src/Dbus.cpp:191-198`).
- **Quad size 768×384 px at 0.42 m** (`src/Config.hpp:45`, `src/Slots.hpp:27`) — one RGBA upload is
  **1.125 MiB** (`src/Session.cpp:269`). Updates are epoch-gated (`src/Session.cpp:355-360`); battery
  polls every 30 s and only on a rounded-gauge change; the keys overlay bursts at ~10 Hz.

**The saving.** Today the HUD costs *zero* marginal bytes, because everything is squashed and both eye
buffers are encoded regardless. Decomposed, it becomes its own quad: at T2 a mostly-transparent
1.125 MiB panel compresses ~30–50× → **25–40 kB per change**, and the keys overlay at 10 Hz is
**2–3 Mbit/s**. At T4 the same panel is *three lines × ~40 chars × 8 B ≈ **1 kB*** — roughly
**1000× smaller**, and immune to 4:2:0 chroma.

**So the HUD is T3/T4's best case and the desktop is its worst**, and they differ only in font
ownership.

**How much of hypxrhud survives a T4 port.** Of roughly 5,000 lines of core daemon code (`src/` is
~8,600 lines in total, the remainder being producers and tooling that are unaffected):

| Fate | Approx. lines |
|---|---|
| Moves to the headset **verbatim** (pure C++; stb_truetype builds on the NDK) — `Panel`, `Slots`, `Scene`, `Theme`, `PanelText`, `BlendMode` | ~1,250 |
| Stays on the host **unchanged**, becoming the T4 protocol front end — `Dbus`, `Props`, `Wire`, `Config`, `Daemon`, `battery/`, `keys/`, `cmdlog/` | ~2,250 |
| **Rewritten** for Android — `Session`, `Egl` | ~850 |
| Test/tooling, survives host-side | ~640 |

So roughly a quarter relocates untouched, roughly half never moves, and the rewrite is confined to the
two files that touch EGL and the OpenXR session.

**The structural finding is the interesting one: the daemon already has, at the file level, exactly the
split a T4 port needs.** `Props.cpp` maps D-Bus `a{sv}` → `SUpsert` (`src/Scene.hpp:51-74`); a T4 wire
would map `SUpsert` → a packet — *the same struct on both sides of the network*. Nobody designed it
that way and it fell out anyway, which is a genuine signal that the boundary is right. The D-Bus API
to hypxrvoice, battery, keys and cmdlog would not change at all.

**And T3 is dominated by T4.** They need nearly identical headset-side machinery — a content renderer
plus a protocol client; for hypxrhud the "state machine" is `Scene` + `Slots` + the fade envelope,
about 600 lines of portable C++. T3 carries T4's fidelity risk and client complexity **without** T4's
one unique win:

> **A HUD that can display *"host disconnected, reconnecting…"* must run on the headset.** That is not
> a bandwidth or a latency argument. It is an availability argument, and it is the only argument for
> T4 that T1/T2 cannot answer.

### 8.7 The third path already in the tree — and its real ceiling

`hypxr_viewpoint_v1` (`docs/openxr/10-view-dependent-surfaces.md:18-21`) hands an ordinary *desktop*
client head-tracked per-eye viewpoints while HypXRland keeps the OpenXR session, the surface and
spatial policy.

**Viewpoint beats T4** when the app needs host resources (dGPU, filesystem, Proton), when it is an
*existing* desktop app being XR-enhanced, when the content is pixel-heavy anyway so relocation saves
no bytes and loses the dGPU, and when you want one session owner and consistent window management.

**T4 beats viewpoint** when content is tiny and semantic, or must survive host disconnect.

**And viewpoint has a ceiling the design memo does not yet cost.** The per-eye points go to the host,
the host renders, the buffer comes back — so **parallax updates at full network round-trip latency,
not display latency**. Unlike pose error, the runtime cannot reproject this away, *because the
parallax is content*. For a strong-parallax portal that will read as swim. Doc 10 notes that
sample/target timestamps and pose-age telemetry are **not implemented** (`:63-67`), so today it cannot
even be measured. **Instrument that before scaling viewpoint up.**

### 8.8 What the protocol must reserve now

```
quad_desc {
  u32 quad_id;          // stable ACROSS TIER SWITCHES — the unit of continuity
  u32 generation;       // bumps on any structural change (size / format / tier)
  pose pose; vec2 size_m; u16 width_px, height_px;
  u8  content_class;    // OPAQUE_VIDEO | UI_TEXT | UI_MIXED | STATIC_IMAGE | SEMANTIC
  u8  tier;             // advisory; client may NAK
  u16 format;           // codec / pixel format, sparse — room for 4:4:4, AV1, zstd-raw
  u8  color_space;      // sRGB | scRGB-linear | Rec709 | Rec2020-PQ
  u8  alpha_mode;       // premultiplied | unpremultiplied | opaque
  u8  damage_semantics; // FULL | DIRTY_RECTS | DIRTY_RECTS_PLUS_COPY | NONE(persistent)
  u8  eye_visibility; i32 z_order; f32 color_scale[4], color_bias[4];
  u16 ext_count; ext_tlv ext[];   // {u16 id, u16 len, bytes} — UNKNOWN IDS SKIPPED, NEVER FATAL
}
```

Three fields that are cheap now and a flag day later: **`color_space`** (the deleted `5c50b9a1` struct
already carried `VkSamplerYcbcrRange` and `VkSamplerYcbcrModelConversion` — do not lose them twice),
**`generation`**, and the **skip-unknown `ext_tlv`**.

### 8.9 Capability negotiation — fix this before anything else

Today `protocol_version` is an FNV-1a walk over the *entire* packet variant
(`common/protocol_version.h:8`; `common/wivrn_serialization.h:48-61,545-558`; seeded by
`protocol_revision = 2` at `wivrn_packets.h:42`), compared for exact equality at
`server/driver/wivrn_connection.cpp:179`. **Adding one `u8` anywhere hard-refuses the connection**,
costing a lockstep server rebuild *and* an APK sideload. For a phase whose entire content is
"iterate on a new quad descriptor", that is the dominant tax.

Recommended, roughly two days:

1. **Scope the hash.** Split into `core_packets` (handshake, pose, frame timing, haptics — anywhere
   silent ABI skew corrupts tracking) and `ext_packets`; hash only `core_packets`. Core stays
   lockstep, which is correct; everything else becomes additive.
2. **Add a capability list to the handshake**: `[{name, version, struct_hash}]` from each side, bound
   at `min(client_max, server_max)`. **Use Wayland's global/interface-version model** — not RDP's flat
   capability struct (same flag-day problem we already have), not WebRTC's SDP re-offer machinery (far
   more than we need). The per-capability `struct_hash` gives **per-extension** structural checking, so
   a mismatch disables *one capability* instead of refusing the session — strictly better than today.
3. **Per-quad tier is not a handshake concern.** Announce optimistically; the client replies
   `unsupported{quad_id, tier}` and the host falls back. Negative-ack only.
4. **Land `ext_blob{u32 ext_id, u32 ext_version, bytes}` in `core_packets` today**, with "unknown
   ext_id → drop and count". One hash bump now buys APK-compatible experimentation forever.

Honest cost: the automatic whole-protocol structural check is lost on ext packets and each extension
must be versioned by hand; the per-capability `struct_hash` recovers most of it. Net: strongly
positive.

### 8.10 Custom OpenXR extensions — one candidate, and it is optional

**(i) A damage-region extension: no, and it is worse than merely unnecessary.** A sweep of the entire
OpenXR registry (`xr.xml`, 967 extensions) finds **no** damage/dirty-rect extension from any vendor.
`XrSwapchainSubImage::imageRect` is *"the valid portion of the image to use"* — a static crop that
*participates in UV mapping* — and the spec warns the compositor *"may bleed in pixels from outside the
bounds… due to mipmapping"*. Worse: **"the order in which images are acquired is undefined"**, and the
spec never guarantees an image's contents survive across acquisitions, so even given a damage rect you
could not legally write only the damaged region. A real extension would have to *also* mandate
per-index content preservation — a large, contentious ask.

**The extension-free solution is the standard one**: hold a persistent full-resolution texture
client-side, apply damage to it, and blit into whichever swapchain image you acquired, using
**buffer-age accounting** (apply the union of the last *N* damage sets) so the blit is itself
damage-bounded. That is exactly `wl_surface.damage` + `EGL_EXT_buffer_age`, which every Wayland
compositor already does. (`XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT` is *not* the answer — a static
swapchain may be released only once.)

**(ii) Persistent layers: core OpenXR already gives you this.**

> *"An application **may call `xrEndFrame` without having called `xrReleaseSwapchainImage` since the
> previous call to `xrEndFrame`** for any swapchain passed to `xrEndFrame`."*

Resubmitting a layer whose swapchain you never touched is explicitly legal and costs one struct copy.
Both HypXRland and hypxrhud already exploit it — `hypxrhud/src/Session.cpp:355-360` guards the
re-raster and upload behind `if (g->uploadedEpoch != p->epoch)`. What does *not* exist is "submit once
and stop calling `xrEndFrame`", and it cannot, because `xrEndFrame` is the frame-pacing and
pose-latching point. The genuinely useful "persistent" idea — not re-sending the layer list over the
network every frame (~184 kbit/s) — is a WiVRn transport concern (a layer-set generation counter), not
an OpenXR one.

**(iii) Content-typed layers: an extension here makes the runtime *less* portable, not more.** A
single-vendor semantic-layer extension that no other runtime implements is not a standard; it is
lock-in that other desktops must write a special path for. It defeats the goal it was meant to serve.

**Derive the tiers instead of being told them.** A client submitting an ordinary quad gets **T1**
automatically when the runtime observes the swapchain was not re-released (the persistent-layer signal
above — free, spec-blessed, works for *any* OpenXR client), and **T2** when the runtime computes
damage itself by hashing tiles (~1 ms on a modern GPU for a 2560×1440 pane, and only for panes that
changed). **This is the single most important design decision in this section**, because it means the
entire tier ladder works for a GNOME or KDE XR shell that has never heard of us.

**Exactly one extension is worth writing, later: `XR_HYPXR_swapchain_damage`** — a cooperative client
supplies damage rects and skips the hash pass. Its *absence* costs performance and never correctness,
which is the property that makes it safe to publish. Even then, **run it as a private side-channel for
six to twelve months first.** Extensions are forever; side-channels are not.

**Do not put T3/T4 in OpenXR at all.** A retained semantic scene is an *application* protocol, not a
rendering API. StardustXR — the most committed semantic-scene XR display server on Linux — puts its
scene graph in flatbuffers over a **Unix socket**, not an OpenXR extension, and textures ordinary 2D
apps as pixels onto quads through a built-in Wayland compositor. Our equivalent already exists and
already has the right shape: hypxrhud's `a{sv}` panel props.

---

## 9. Risks

Ranked by the product of likelihood and damage.

| # | Risk | Why it is real | Mitigation / gate |
|---|---|---|---|
| R0 | **The 16-layer Quest budget reframes the project** | Squashing is what makes the layer count invisible today; forwarding trades bandwidth for an **8× tighter budget**, shared with Meta's own system layers, on a stack that is already ~15 layers. | §5.6: forward *selectively* by (area × staleness), squash the rest. Start with hypxrpaper's static equirect2. |
| R1 | **Cross-client layer overflow is latent heap corruption** | Each client is told `maxLayerCount = 128`; enforcement is per-client, in-process; the merged sink is one 128-entry array with **no bounds check** (`comp_layer_accum.c:22-96`). Two clients at budget = 256 writes into 128 slots. | Bound-check the accumulator and advertise a **shared** budget — the same mechanism R0 forces anyway. Fix before adding clients, not after. |
| R1b | **`system_compositor_set_z_order` unlocks a mutex it never locked** (`comp_multi_system.c:680`, upstream) | UB on a non-recursive mutex, on every session create and focus flush — i.e. every connect, disconnect and app swap. | One-line upstream fix; do it in Stage 0. |
| R1c | **No cross-client input arbitration** | Every overlay is unconditionally visible *and* focused (`ipc_server_process.c:431-462`); all clients receive identical controller/hand/pose state. `system_set_focused_client` is a stub. Today HypXRland will route a click *through* a hypxrhud panel floating in front of it. | Cross-client ray hit-testing belongs in the runtime (§5.4) — only the runtime sees all the quads. |
| R1d | **Blend mode is winner-takes-all** | Any visible OPAQUE client forces the whole frame OPAQUE (`comp_multi_system.c:228-260`). **One native OPAQUE game kills passthrough for the desktop and the wallpaper.** | Per-client blend-mode override; part of `XR_HYPXR_overlay` (§6.9). |
| R2 | **Static-gap latency (WiVRn #618)** | A live, unexplained upstream report of 300–500 ms spikes *specifically when the head is held still* — the exact regime this design manufactures. | **Gate 0.** Reproduce with `fps_divider = 2` before writing any protocol code. A keepalive floor (§7.5) is the likely fix. |
| R3 | **The pathological case is worse than today** | §6.4: N panes all animating at native resolution exceeds the eye-buffer pixel count. | Angular-resolution policy (§7.3) + the full-eye fallback as a load-shedding valve, not just a game switch. |
| R4 | **Protocol lockstep** | WiVRn versions by an FNV-1a hash over the whole packet variant; mismatch is a hard refusal. Every field addition forces a simultaneous server + APK redeploy. The fork was *just* consolidated. | Introduce capability negotiation *before* the quad work, not during. This is the single highest-leverage protocol change and it pays for itself immediately. |
| R5 | **Mouse-motion dirties everything** | Hover effects, focus rings, selection highlights and tooltips all dirty a pane on pointer motion (§7.1). A "static" desktop may be much less static than assumed. | **Measure first** (Gate 1). Client-side cursor (§6.8) fixes the glyph but not the hover. |
| R6 | **Per-encoder state divergence across panes** | waypipe shipped exactly this and got visible flicker between buffers with different encoder artefacts (§4.4). | Bind one encoder to one *pane identity* — the `take_quad.swapchain` id already does this. |
| R7 | **Hybrid depth/occlusion with a game** — *better than feared* | Quad-vs-quad occlusion is **free**: for geometric primitive layers *"the runtime computes the depth of the sample directly from the layer parameters"*, so chaining `XrCompositionLayerDepthTestFB` (already requested and used at `client/scenes/stream_gui.cpp:1021,1029`) just works, with no depth swapchain. Quad-*inside*-game needs the game's depth forwarded, and the fork already found `XR_KHR_composition_layer_depth` **unavailable cross-GPU** (RADV will not export depth as LINEAR dma-buf, XG round 2). WiVRn issue #998 is a live bug here: VIEW-space quads mis-positioned when a projection layer is co-submitted. | Ship whole-layer ordering (works today, free). **Scope pixel-level interpenetration out.** |
| R7b | **The Android manifest fails silently** | Without `oculus.software.handtracking` + `com.oculus.permission.HAND_TRACKING`, Horizon OS blocks launch of an immersive app whenever the user is hands-only — `am start` reports success and the process dies. For a keyboard-at-a-desk app, hands-only is the *default* case. | Declare it whether or not it is used. Same for `com.oculus.feature.PASSTHROUGH`, whose absence makes `XR_FB_passthrough` un-enumerable. |
| R7c | **`XR_MNDX_egl_enable` is a hard requirement** (`XRSession.cpp:75-82`) | HypXRland cannot start on any non-Monado runtime, which contradicts the runtime-as-product claim. | One line. Accept a plain GLES/Vulkan binding. Highest-leverage change for that ambition (§6.9). |
| R8 | **Client-app maintenance burden grows** | A dynamic decoder pool, a layer-submission path driven by wire data, and per-stream state are real ongoing surface in an APK the user sideloads and maintains alone. | Keep changes additive to the existing client; never fork away from upstream WiVRn's client structure. |
| R9 | **Encoder-thread serialisation** | All encodes already run sequentially on one thread with one process-wide sender (§7.6). N panes multiply the work on that thread. | Must never block the compositor frame path; needs explicit queueing and drop policy. |
| R10 | **`XR_EXTX_overlay` is provisional** | Essentially Monado-only. The multi-client model — which is now a first-class requirement (§5.5) — rests on it. | Either accept it as a private extension of *our* runtime and spec it ourselves, or push standardisation. Do not build a public product story on it silently. |
| R11 | **Text may not actually get sharper** | The quality argument (§1.5) is a prediction, not a measurement. 4:2:0 chroma still applies. | Gate 2 is a subjective A/B in the headset before committing to the large build. |
| R12 | **Testing covers pose, not pixels, and barely covers multi-client** | 17 gtest files under `tests/xr/` and ~38 `hyprtester --xr` cases exist, but **exactly one multi-client test** (`xr_overlay_composition`, opt-in behind `$HYPRTESTER_HYPXRPAPER`), and it asserts only `overlay: true` — not layer budget, ordering, input routing, or blend-mode interaction. | Most of the suite *survives*: everything asserting on pose reads JSON, not pixels. Four new tests needed — shared layer budget, input routing, mode-switch continuity (the `[fov] stream: geometry …` WARN at `compositor.cpp:746-753` is already a machine-parsable transition marker), and per-quad latency accounting (the `feedback{encode_begin…displayed, times_displayed}` wire record already exists per frame per stream — extend `stream_index` to quad ids and it is free). |
| R13 | **Nothing on the host can see the merged layer stack** | Monado logs no per-client layer attribution; `hyprctl openxr status` reports only our own `"quads": N`. The first multi-client bug will be very hard to diagnose. | Quest-side oracles exist and are free for attended work: `debug.oculus.logLayers 1` + `logcat -s CompositorClient` dumps the actual per-frame layer list; `debug.oculus.layerProperties 1` + `logcat -s CompositorVR` prints per-layer filtering decisions. (Attended only — this project's rules forbid `adb` in automation.) |

---

## 10. Phased roadmap, decision gates, and the smallest proof

The shape: **buy the cheap wins first, and let them buy the information that decides the expensive
one.** Every gate below is a real stop, not a formality.

### Phase 0 — measure, and fix what is already broken (days)

**0a. Measure — no new code.**

- **`fps_divider = 2`.** Costs nothing, reverts instantly, answers *"does the client tolerate a
  reduced encode rate?"* — the client half of everything downstream.
- **Reproduce or refute WiVRn #618** (300–500 ms latency spikes when the head is held still). This
  design manufactures that regime deliberately.
- **Histogram `m_haveNewFrame` per layer per frame** across a real working hour. **How much of a
  typical hour is actually static, once hover effects, cursor motion and blinking cursors are
  counted?** *This is the number the whole business case rests on and nobody has it.*
- **Verify `maxLayerCount` on the Quest** with one `xrGetSystemProperties` call — the sources conflict
  16 vs 32 (§3.2) and the answer changes the policy.
- **Confirm `XR_KHR_android_surface_swapchain` works under Vulkan** (§3.4) — the deprecated VrApi
  equivalent was GLES-only.
- **Install `libva-utils` and run `vainfo`** — the single highest-value ten-minute experiment in this
  memo. It confirms which of `VAConfigAttribEncDirtyRect`, `VAConfigAttribEncROI` and
  `VAConfigAttribEncSkipFrame` radeonsi actually advertises on VCN 4.0.5, which decides how much of
  Phase 1a lands as twenty lines and how much needs a libavcodec patch. (4:4:4/SCC encode is already
  known absent — §7.4.)
- **`amdgpu_top`** for the true static-vs-moving VCN engine-time ratio (the 60–80 % figure in §7.6 is
  an estimate, and it is the number the business case now rests on). `WIVRN_DUMP_TIMINGS` for the
  encode→send→decode→display chain including `times_displayed > 1`.

**0b. Fix the multi-client defects — independent of everything below, and worth doing anyway.**
Bound-check `comp_layer_accum`; fix the `set_z_order` mutex UB (`comp_multi_system.c:680`), which
runs on every connect, disconnect and app swap; validate `slot_id`/`layer_count` from client-writable
shm. **This ships value immediately: HypXRland + hypxrhud + a game stops carrying a
memory-corruption class** (R1, R1b).

> **GATE 0.** If long idle gaps cause multi-hundred-millisecond spikes and the cause is not
> understood, **stop** — every design below manufactures that regime.
> If a typical hour is less than ~50 % static at pane granularity, **the economic case weakens
> sharply** and the effort should go to §4.3's per-region approach instead.

### Phase 1 — spend the damage we already compute (small, and two ways)

**1a — Regime A: damage → ROI, inside WiVRn's existing encoder.** Feed the compositor's damage
downward as `AV_FRAME_DATA_REGIONS_OF_INTEREST` on the ffmpeg VA-API path, with adaptive quantisation
on — roughly twenty lines (§7.2.0). Touches neither frame pacing nor timewarp, creates none of the
idle hazards, and is trivially revertible. **Do this first: it is the lowest-risk item in the entire
memo.** Then evaluate `VAConfigAttribEncSkipFrame` (near-zero bytes *with* cadence preserved) and
dirty-rects, which need direct VA or a small libavcodec patch.

**1b — Regime B: option B, the damage-gated frame throttle.** Throttle the whole
`xrWaitFrame`/`xrBeginFrame`/`xrEndFrame` cycle in HypXRland when no layer released a swapchain image
**and** the head-pose delta is below threshold (§6.7). Zero protocol change, zero WiVRn change,
entirely in `src/openxr/`. This is the one that actually buys **engine time** — the scarce resource
(§7.6) — and the one that pays the idle costs, so it needs 1c beneath it.

**1c — the minimum-frame-rate floor.** A frame every 100–200 ms at zero damage, ≈0.2 Mbit/s: one
mechanism keeping the station awake, rate adaptation fed, cwnd alive and references fresh (§7.5). A
plain keepalive does **not** substitute — congestion-window validation decays on application-limited
flows too. Also the leading candidate fix for WiVRn #618.

Two adjacent one-liners with outsized payoff:

- **Extend or disable `intraRefreshPeriod`** (§7.2) — A/B measurement attributes ~97 % of a static
  frame's steady-state cost to the rolling refresh wave on the NVENC path.
- **Move the chrome fade into `XrCompositionLayerColorScaleBiasKHR`** (§6.8) — zero pixels, zero
  encode.

> **GATE 1.** Measure **VCN engine occupancy** (not bitrate — §7.6), host power, and *subjective*
> wake-up latency on first scroll, with 1a and 1b evaluated separately so their contributions are
> distinguishable. If Phase 1 delivers most of the engine-time win and the wake-up hitch is
> imperceptible, **the urgency of Phase 4+ drops from "project" to "quality initiative"** — which
> changes what it must be justified by (§6.3), not whether it is worth doing.

### Phase 2 — unlock iteration: capability negotiation (≈2 days, do it before any quad work)

Scope the protocol hash to `core_packets`, add a Wayland-style per-capability list with per-capability
`struct_hash`, and land `ext_blob` today (§8.9). **Every subsequent experiment is gated on this**: as
things stand, adding one `u8` anywhere forces a lockstep server rebuild *and* an APK sideload.

Reserve the §8.8 descriptor fields now even though nothing uses them — especially `color_space`,
`generation`, and the skip-unknown `ext_tlv`.

### Phase 3 — the smallest end-to-end proof, and it is not a desktop pane

**The milestone: hypxrpaper's static equirect2 environment arrives at the Quest as its own
`XrCompositionLayerEquirect2KHR`, composited by Horizon OS beside WiVRn's existing eye stream, and
stays correct when the head moves.**

Why this and not a desktop quad: it is full-FoV so it dominates the squash and encode cost; it is
*static* so after one transmission it costs literally nothing; Quest supports `equirect2` natively and
hypxrpaper already uses v2 rather than the unsupported v1 (`hypxrpaper/src/Session.hpp:93`); it costs
one layer of sixteen; and **if it fails, nothing important breaks.** Highest saving-to-risk ratio in
the system.

The minimum path:

1. Tap `layer_squasher::do_layers` (`layer_squasher.cpp:523`) to emit the layer stack — reuse the
   `take_quad` serializer that already runs there (§2.3), extended for equirect2.
2. Carry the geometry as **per-frame metadata, exactly the way `view_info` does** (§2.5) — never in
   the stream description. One extra stream, one hardcoded extra layer.
3. Client: one extra decoder, one `add_quad_layer`-shaped call using the existing production path
   (`client/scene.cpp:348-368`).
4. **Leave the layer in the host squash too, at first.** Seeing the same content twice in the same
   place is the clearest possible correctness signal — and the moment the two *diverge* as the head
   moves is the pose bug of §2.4 announcing itself.
5. Then remove it from the squash and compare sharpness side by side.

Follow immediately with **one static desktop pane** by the same route; that exercises `eyeVisibility`,
`imageRect` and the stereo pair (each pair costs 2 of 16).

> **GATE 2.** Attended, in the headset. Two questions: **is the forwarded content visibly sharper?**
> and **does it stay put when you move your head?** If the first answer is "not really", the project
> loses its strategic justification (§6.3) and should be re-scoped to Phase 1 plus §4.3's per-region
> codec work.

### Phase 4 — N layers, selective forwarding, the allocator

Restore and extend `5c50b9a1` (§2.6 — 53 files, −1359/+503; a revert of a simplification, so the
"before" is known-working). Its sub-rect tiling is also, directly, the **atlas transport** of §3.5
(option a′): start there, and break out a dedicated stream only for a genuinely high-rate pane.

Then: dynamic client decoder pool; the pane→stream allocator with timesharing (§7.6);
`XR_META_recommended_layer_resolution`-driven stream sizing (§7.3); per-pane IDR and damage policy
(§7.2); the client-side cursor and the grab-filter change (§6.8, §6.8b).

**And the policy that Phase 0 taught us we need: forward selectively by (area × staleness), squash the
rest** (§5.6). Runtime-side layer arbitration against a *shared, advertised* budget replaces the three
uncoordinated per-client policies (R0, R1).

### Phase 5 — the seamless switch, made explicit

Today's implicit fast-path/squash switch (§2.5) becomes a deliberate, hysteretic policy:

- **→ full-eye: immediate, no hysteresis.** Any projection layer in the merged stack. xrizer makes
  this unambiguous: in the normal case it submits **exactly one `XrCompositionLayerProjection`**, two
  views over one array swapchain, `EnvironmentBlendMode::OPAQUE` hardcoded, and never any depth
  (`xrizer/src/compositor.rs:1351-1373`; `XR_KHR_composition_layer_depth` is not in its extension list
  at `openxr_data.rs:122-163`). It *does* submit real quad/cylinder/equirect2 layers when a game uses
  OpenVR `IVROverlay` (`xrizer/src/overlay.rs:256-322`), so a `1 + N` frame is possible — which the
  trigger must tolerate rather than be confused by.
- **→ quad-forward:** no projection layer for ~90 consecutive frames, so an alt-tab does not thrash
  the encoder.
- **Layer-pressure trigger:** forwarded layer count approaching 16 minus system reserve → demote the
  churniest layers back to the squash path.
- **Codec-pressure trigger:** `feedback.times_displayed` climbing (the client is reprojecting stale
  frames) → fall back. Windowed minimum, like the existing LEDBAT ramp.
- **State that transfers: none**, if the geometry stays in per-frame metadata. Both modes then coexist
  in one session trivially — which is exactly how `view_info.alpha` already starts and stops encoder
  stream 2 mid-session.

### Phase 6 — the runtime as a product, and the content tier

Unblock the one line that makes it possible (`XR_MNDX_egl_enable`, §6.9). Write down the six
unspecified contracts as versioned extension documents — `XR_HYPXR_compositor_device`,
`XR_HYPXR_import_constraints`, and above all **`XR_HYPXR_overlay`** with the input routing, shared
layer budget and per-client blend-mode override that `XR_EXTX_overlay` never got. Derive the tiers
from what any standard client already does (§8.10) so a shell that has never heard of us benefits.

Then the content tier's first client, which should be `hypxrhud` at **T4** — already semantically
driven, already font-hermetic, and roughly a quarter of its core code relocates untouched (§8.6). Possibly one optional
extension, `XR_HYPXR_swapchain_damage`, after six to twelve months as a private side-channel.

### What is explicitly *not* on this roadmap

- A new OpenXR runtime (§6.1).
- A new Android client (§6.6).
- A T3 semantic tier for desktop panes (§8.3).
- Pixel-level interpenetration between desktop quads and game content (R7).
- Splitting the fork into two products. **Publish the surface; do not extract the repo.**
  `src/openxr/` is 20,728 lines with 108 config keys and 13 `hyprctl` verbs, but the bleed outside it
  is 21 files and shallow — only `ConfigManager.cpp` (34 refs), `DispatcherTranslator.cpp` (25) and
  `PointerManager.cpp` (13) are non-trivial, and `Monitor.hpp:142` is a single bool. **The heavy
  coupling runs the other way**: `src/openxr/` depends on `CMonitor`, the buffer path and `IPointer`,
  which is exactly what a client should do. There is nothing to extract; there is a surface to
  document.
- Any change to Hyprland's layer submission model (§2.1). The optional additions are per-quad damage
  (one line at `Renderer.cpp:2751`) and reporting the runtime's recommended layer resolution — both
  upside, neither a prerequisite.

---

## 11. What this research could not determine

Recorded so the next round starts from the gaps rather than rediscovering them. Everything here is
measurable on this hardware; none of it is researchable. Struck-through rows were closed during this
research and are kept for their answers.

| # | Open question | Why it matters | How to close it |
|---|---|---|---|
| Q1 | **What fraction of a real working hour is actually static, at pane granularity?** | The entire business case. Nobody has this number. | Histogram `m_haveNewFrame` per layer per frame (Phase 0a) |
| Q2 | **Quest 3's real `maxLayerCount`** — 16 (docs, spec floor, two runtime dumps) or 32 (one community probe)? | Decides whether selective forwarding is a tight squeeze or comfortable | One `xrGetSystemProperties` call |
| ~~Q3~~ | **ANSWERED: a static frame costs ~60–80 % of a moving frame in VCN engine time**, while bits swing ~28×. Engine time, not bits, is the scarce resource (§7.6). | Reframes the whole case: damage-gating *bits* barely touches engine pressure; only not encoding does | Estimate — confirm the exact ratio with `amdgpu_top` (Phase 0a) |
| ~~Q4~~ | **ANSWERED, negatively: no AMD ASIC offers 4:4:4 or SCC *encode*.** VCN 4.0.5 HEVC encode is Main/Main10 4:2:0 only; AV1 is Profile 0, also 4:2:0 (§7.4) | Removes the most direct text-quality lever *permanently* on this hardware, leaving "remove the two resamples and foveation" as the principal quality win available at all | Closed |
| ~~Q5~~ | **ANSWERED: tens of MB and tens of ms on first create.** Allocate a fixed pool at session start and never churn per pane (§7.6) | A design that spun contexts up and down per pane would stall the single VCN ring at exactly the wrong moments | Closed |
| ~~Q6~~ | **ANSWERED: yes, and a keepalive does not fix it.** `tcp_slow_start_after_idle = 1` collapses cwnd after >1 RTO; congestion-window validation decays on *application-limited* flows too, so trickling bytes does not help. WiFi WMM/U-APSD and rate adaptation decay independently. The correct mechanism is a **minimum frame rate** — a frame every 100–200 ms at ~0.2 Mbit/s doing four jobs at once (§7.5) | Leading candidate fix for WiVRn #618 | Remaining: Quest-side power-save behaviour needs an AP-side capture |
| ~~Q7~~ | **ANSWERED: yes, with in-tree proof** — the recorder's transfer pacer chunks at 40 ms precisely to avoid blocking the video stream on the shared socket. Do not put N panes on one TCP connection: UDP shards, or exactly two connections split interactive/bulk, oldest-deadline-first, chunked above ~32 kB (§7.5) | | Closed |
| Q8 | **Does `XR_KHR_android_surface_swapchain` work under Vulkan on Quest 3?** The deprecated VrApi equivalent was GLES-only | Decides whether the zero-app-GPU-work decode path (§3.4) is available to our Vulkan client | On-device probe |
| Q9 | **How much does pointer motion actually dirty a "static" desktop?** Hover effects, focus rings, selection highlights and tooltips are not covered by promoting the cursor glyph (§7.1) | Bounds how quiet a quiet desktop can be made | Falls out of Q1's instrumentation |
| Q10 | **Whether forwarded content is *visibly* sharper** (§6.3) | The project's strategic justification | Gate 2 — attended A/B in the headset |
| **Q11** | **Which VA-API damage attributes radeonsi actually advertises on VCN 4.0.5** — `VAConfigAttribEncDirtyRect`, `VAConfigAttribEncROI`, `VAConfigAttribEncSkipFrame` | Decides how much of Phase 1a is twenty lines through ffmpeg and how much needs direct VA or a libavcodec patch | **Install `libva-utils`, run `vainfo`. Ten minutes, and the highest value-per-minute item in this memo.** |
