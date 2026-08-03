# Research: head-leashed presentation of transient layer-shell UI (walker, mako, OSDs)

**Status:** research / decision-support. **Nothing is implemented.** This memo asks how the
walker launcher — and the wider family of transient layer-shell UI (mako notifications,
swayosd, the WiVRn menu) — should be presented while an XR session is live, instead of being
composited into whichever XR monitor happened to be focused when the client bound its layer
surface. It follows the house style of `research/20`/`research/21`: ground truth first (with
file:line), honest pro/con per architecture, a scorecard, then a recommendation split into
"the cheap correct thing" and "the expensive general thing", plus a WP breakdown.

Evidence base (all read-only, worktree at `c3bdf3aa` "monitor: re-arrange layer surfaces when
a monitor moves"):

- **Compositor, layer shell**: `src/desktop/view/LayerSurface.{hpp,cpp}`,
  `src/protocols/LayerShell.{hpp,cpp}`, `src/desktop/rule/layerRule/*`,
  `src/desktop/rule/Rule.cpp`, `src/desktop/state/{FocusState,LayerState,ViewHitTester}.cpp`,
  `src/render/Renderer.cpp` (`renderLayer`, `arrangeLayerArray`, `damageSurface`),
  `src/managers/input/InputManager.cpp`, `src/output/Monitor.{hpp,cpp}`.
- **Compositor, XR**: `src/openxr/OpenXRManager.cpp` (frame loop, quad assembly,
  `createXRMonitor`, `setMonitorsPlugged`, `recomputeQuadActive`, `dispatchInputEvent`),
  `src/openxr/XRMonitorLayer.hpp`, `src/openxr/XRAnchor.hpp`, `src/openxr/XRRule.hpp`,
  `src/openxr/XRPointerDevice.*`.
- **Docs**: `docs/openxr/03-anchoring.md` (anchor modes + leash math),
  `docs/openxr/05-configuration.md` (`xrmonitor` / `xrrule` / verbs / socket2 events),
  `research/HYPXRHUD.md` (overlay-HUD architecture, WP-H9/H10), `research/VISUALS.md`
  (compositor-vs-companion doctrine, view-bounding), `research/INTERACTION.md`.
- **Companion repos**: `~/code/hypxrhud` @ `4799bc4` (`src/Session.cpp`, `src/Slots.cpp`,
  `src/Config.hpp`), `~/code/hypxrpaper` (`src/Session.{hpp,cpp}`).
- **Live session** (read-only `hyprctl` + `$XDG_RUNTIME_DIR/hypr/*/hyprland.log`): walker's
  namespace/level/monitor history, the full namespace census, `hyprctl openxr status`,
  `~/.config/walker/config.toml`, `~/.local/share/omarchy/default/hypr/apps/walker.conf`,
  `omarchy-launch-walker`.
- **The launcher stack itself** (read-only `pacman -Qi/-Ql` + `file` + `strings` on the
  installed binaries): `walker 2.16.2` and `elephant 2.21.0` — language, dependencies, the
  UI/provider split, the embedded config schema. See §4.D-ground-truth. Facts derived from a
  *stripped* binary are flagged as such and carry a confirmation step in §12.

---

## TL;DR — RECOMMENDATION

> **Do not build a "layer quad". Build a *layer host monitor*.** The thing that makes a
> head-leashed launcher expensive is not the quad — we already submit N quads per frame with
> anchors, transparency, chrome, ray/gaze input and grabs — it is that **every service a
> layer-shell client needs is keyed off a monitor's logical rectangle**, most fatally
> `damageSurface`, which schedules `wl_surface.frame` callbacks by *geometric overlap with
> monitor boxes* (`Renderer.cpp:2683-2695`). A surface that lives on no monitor plane stops
> being repainted by its own client. So the right primitive is a small, persistent,
> compositor-managed headless XR monitor with `anchor:head`, onto which matched layer
> surfaces are **steered at bind time** (a 3-line change at the single site that writes
> `CLayerSurface::m_monitor`, `LayerSurface.cpp:54`), whose quad is hidden by the existing
> eased-alpha envelope while nothing is mapped on it.
>
> Decomposed into three independently useful pieces, two of which are generic Hyprland
> features with no XR in them:
>
> ```ini
> layerrule = monitor XR-launcher, match:namespace ^(walker)$   # NEW generic layer effect
> xrmonitor = XR-launcher, 900x520, anchor:head offset:0,-0.05,-0.75, size:0.55, hideempty:on
> xrrule    = blackalpha 0.0, monitor:XR-launcher               # already shipped
> ```
>
> Effort **M** (≈450–650 lines across 8 WPs), versus **XL** for genuine per-surface
> extraction, which must re-implement damage scheduling, frame callbacks, an offscreen
> swapchain, pointer routing and the anchor/rule/chrome generalisation from "monitor" to
> "quad" — for the sole benefit of not having a phantom entry in `hyprctl monitors`.
>
> **Before writing any of it, run the free experiment (§4.D0):** walker already has an
> `as_window` option and Hyprland already ships `windowrule = monitor <name>`, so the whole
> ergonomic hypothesis — distance, leash deadzone, luma-key legibility, tolerance for an
> always-present empty host — can be tested tonight with pure config.
>
> **Owning the launcher (fork walker, or build a native `hypxrlauncher` on hypxrhud +
> elephant — §4.D) is rejected on generality**, not on difficulty: walker is only 40 of the
> 133 transient layer surfaces in the live census (mako 78, swayosd 15), and neither mako nor
> swayosd will ever be rewritten as an XR daemon. One compositor mechanism fixes all three.

Ranked: **B′ (recommended) > B > A′ > A > D2 > D1 > C > E**, with **D0 as a zero-code
ergonomics spike to run first** (`walker as_window = true` + the already-shipped
`windowrule = monitor …` — §4.D0). See §6.

---

## 1. What happens today — the causal chain

### 1.1 Walker is an ordinary overlay layer surface, bound to the focused monitor

Live ground truth from the session log (`$XDG_RUNTIME_DIR/hypr/8c5edbb3…_1785642591_…/hyprland.log`),
which contains 21 walker maps across one afternoon:

```
21:52:04  LayerSurface 557e00e23450 (namespace walker layer 3) created on monitor XR-main
21:52:07  LayerSurface 557e01099c10 (namespace walker layer 3) created on monitor XR-2
22:20:53  LayerSurface 557e011aafa0 (namespace walker layer 3) created on monitor XR-3
00:08:07  LayerSurface 557e0140f5e0 (namespace walker layer 3) created on monitor eDP-2
```

So: namespace `walker`, layer **3 (overlay)**, and the monitor is **whichever one was focused**
at bind time — four different outputs in one session. Each launch is a *fresh* layer surface
(different pointers); walker's `--gapplication-service` persists but the surface does not.

The user config confirms the identifiers: `~/.local/share/omarchy/default/hypr/apps/walker.conf`
is a single line, `layerrule = no_anim on, match:namespace walker`, and
`~/.config/walker/config.toml` sets `force_keyboard_focus = true`. The launcher is invoked as
`walker --width 644 --maxheight 300 --minheight 300` (`omarchy-launch-walker`), i.e. it asks
for a **fixed 644×300 surface**, and its GTK4 layout centres itself (`halign/valign = center`
in `themes/omarchy-default/layout.xml`) with **no layer-shell anchors**.

### 1.2 Why "focused monitor" is where it lands

Two hops:

1. `zwlr_layer_shell_v1.get_layer_surface` is called by gtk4-layer-shell with a **NULL
   `wl_output`**. `CLayerShellResource` therefore records an *empty monitor name*
   (`LayerShell.cpp:25`, `m_monitor = pMonitor ? pMonitor->m_name : ""`).
2. `CLayerSurface::create` resolves that to the focused monitor:

   ```cpp
   // src/desktop/view/LayerSurface.cpp:26
   auto pMonitor = resource->m_monitor.empty() ? Desktop::focusState()->monitor()
                                               : State::monitorState()->query().name(resource->m_monitor).run();
   ```
   and binds it at `:54-55`:
   ```cpp
   pLS->m_monitor = pMonitor;
   pMonitor->m_layerSurfaceLayers[resource->m_current.layer].emplace_back(pLS);
   ```

`LayerSurface.cpp:54` is the **only** write to `m_monitor` in the entire tree. There is no
`moveToMonitor`, no migration path, and monitor removal does not re-home layers — `onDisconnect`
sends `closed` to every layer on the output and clears the arrays (`Monitor.cpp:477-483`).

### 1.3 Why that is wrong in a headset

With three XR monitors anchored around the user (live `hyprctl openxr status`: `XR-main` at
`[-0.41, 1.68, 1.64]`, `XR-2` at `[-0.58, 0.43, 1.83]`, `XR-3` at `[-0.86, 0.99, -0.09]`),
"the focused monitor" is a *place in the room*. Pressing `SUPER+SPACE` while looking at the
desk therefore opens the launcher on a panel that may be behind you, above you, or 2 m away
and 26° off-axis. Walker is keyboard-first, so the interaction still *works* — you just can't
see it. Worse, walker is an **overlay (layer 3)** surface with keyboard interactivity, so the
compositor hands it focus and every keystroke goes somewhere invisible.

The same critique applies verbatim to the other transient namespaces observed live (§8).

---

## 2. What the XR side already gives us

The unit of everything in HypXRland is the **virtual monitor**. Per XR monitor we already
have, for free:

| Capability | Where | Granularity |
|---|---|---|
| One `XrCompositionLayerQuad` per frame, depth-sorted | `OpenXRManager.cpp:1761-1880` | per monitor |
| Head leash (view-space offset, deadzone latch, critically-damped spring, per-frame `lookAtNoRoll` re-aim + slerp low-pass) | `XRAnchor.hpp:19-23` (`XR_ANCHOR_HEAD`), doc `03-anchoring.md` §4.2 | per monitor |
| Body leash, device (grip) lock, adaptive dock↔roam | same enum + doc §4.3/§4.4/§6 | per monitor |
| Uniform alpha + luma-key transparency, **eased** through `SXRFxEnv` over `transparency_blend_ms` (600 ms) | `XRMonitorLayer.hpp` (`m_fxAlphaEnv`, `m_fxAlpha`), `XRRule.hpp` | per monitor |
| Situational rules (`xrrule`) matching name / anchor state / focused class / title / fullscreen | `XRRule.hpp:100-135`, doc `05-configuration.md` §xrrule | per monitor |
| Grabbable chrome (move bar, corner resize), hover/grab colours, fade envelope | `XRMonitorLayer.hpp` chrome block, WP-G1…G6 | per monitor |
| Ray pointer + endpoint cursor + magnet/aim assist; gaze select + gaze carry | `OpenXRManager.cpp:1785-1934`, `XRInput.cpp` | per monitor |
| Absolute pointer injection into the compositor | `OpenXRManager.cpp:1176-1179` — `m_pointerDevice->m_boundOutput = layer->m_monitorName;` then a 0..1 `motionAbsolute` | **keyed by monitor name** |
| Runtime create/destroy, plug/unplug, layer cap, swapchain (re)creation on mode change | `createXRMonitor` :2057, `setMonitorsPlugged` :3128, `recomputeQuadActive` :3943 | per monitor |

Two numbers bound the design space:

- **Layer budget is not a constraint.** `m_maxLayerCount` is floored at the spec minimum 16
  (`XRSession.cpp:174-177`); today's session uses 3. A launcher quad is the 4th.
- **Quad activation is already a cheap boolean.** `m_quadActive` (`XRMonitorLayer.hpp`) is
  consulted at `OpenXRManager.cpp:1795` and simply drops the quad from the submitted array.
  Nothing else in the pipeline needs to change to make a quad appear and disappear.

What does **not** exist at any granularity below a monitor: `xrrule` states the doctrine
explicitly (`05-configuration.md:384-385`) — *"the monitor is the unit of effect. Windows are
only a source of conditions — there is no such thing as 'make this window transparent in XR'."*

---

## 3. The five compositor facts that decide this

These are the load-bearing findings. Everything in §4 falls out of them.

### F1 — Damage *and frame callbacks* are geometric, not ownership-based

`IHyprRenderer::damageSurface` (`Renderer.cpp:2673-2727`):

```cpp
// :2683-2695 — frame-callback scheduling
if (!WLSURF->resource()->m_current.callbacks.empty() && pSurface->m_hlSurface) {
    const auto BOX = pSurface->m_hlSurface->getSurfaceBoxGlobal();
    for (auto const& m : State::monitorState()->monitors())
        if (BOX->overlaps(m->logicalBox()))
            m->scheduleFrame(AQ_SCHEDULE_NEEDS_FRAME);
}
...
for (auto const& m : State::monitorState()->monitors())      // :2713-2719
    if (EXTENTS.overlaps(m->logicalBox()))
        m->addDamage(...);
```

`m_monitor` is never consulted. **A layer surface whose global box overlaps no monitor stops
receiving `wl_surface.frame` callbacks** and therefore stops repainting — a GTK client in that
state freezes: no cursor blink, no list update as you type, no animation. This single fact
kills every "float the surface off the desktop plane and render it into a bespoke swapchain"
design unless that design also writes its own damage + frame-callback scheduler.

### F2 — Keyboard focus for a layer surface is monitor-independent

`CLayerSurface::onMap` (`LayerSurface.cpp:185-206`) computes
`GRABSFOCUS = EXCLUSIVE || (interactivity != NONE && …)` and then calls
`Desktop::focusState()->rawSurfaceFocus(m_wlSurface->resource())`. `rawSurfaceFocus`
(`FocusState.cpp:223-271`) checks the session lock and any seat grab and then calls
`setKeyboardFocus` — **it never looks at a monitor**. `m_exclusiveLSes`
(`InputManager.hpp:188`) is likewise a global override.

So a keyboard-first launcher works correctly on *any* monitor, including one the cursor is
nowhere near. This is what makes the whole "host it on a private monitor" family viable.

Two caveats:
- Pointer *acquisition* IS monitor-bound: `mouseMoveUnified` resolves the monitor from the
  cursor position (`InputManager.cpp:279`) and only searches that monitor's
  `m_layerSurfaceLayers`. A hidden host monitor is unreachable by the physical mouse — but it
  is perfectly reachable by the XR ray, which routes by `m_boundOutput` + absolute UV
  (`OpenXRManager.cpp:1176-1179`), i.e. exactly the mechanism that already targets XR quads.
- `onMap`'s `LOCAL` computation at `LayerSurface.cpp:203` adds `PMONITOR->m_position` to
  `m_geometry`, which is *already global* (§F4). Latent bug, self-corrects on the next
  `mouseMoveUnified`; do not copy that expression.

### F3 — The render/hit-test predicate is array membership, not `m_monitor`

`renderAllClientsForWorkspace` iterates `pMonitor->m_layerSurfaceLayers[level]`
(`Renderer.cpp:1136-1156` no-workspace path, `:1164-1173` + `:1234-1253` normal path,
`:1660-1668` lockscreen). The hit-tester does the same (`ViewHitTester.cpp:329-350`). So
"which output composites this layer" is decided purely by which monitor's array holds it —
i.e. by the one line `LayerSurface.cpp:55`.

The only existing per-call render skip is `above_lock` (`Renderer.cpp:943-945`). `no_screen_share`
is *not* a skip: it renders normally and paints a black rect over the box afterwards
(`ScreenshareFrame.cpp:240-261`).

### F4 — `m_geometry` is global, cached, and rarely recomputed

`arrangeLayerArray` (`Renderer.cpp:2556-2640`) computes the box in **global** coordinates from
`full_area = monitor position + size` (`:2557`), resolves anchors (`:2578-2601`; *no anchors ⇒
centred*, which is walker's case), applies margins, then writes `ls->m_geometry = box` (`:2631`),
`configure(box.size())` (`:2635-2636`) and `setBox(box)` (`:2638`). It is re-run only on
layer-shell **state** commits (`LayerSurface.cpp:340`), map/unmap/destroy, mode apply, and —
new in `c3bdf3aa` — `CMonitor::moveTo` (`Monitor.cpp:1789-1790`). A plain buffer commit does
**not** re-arrange (`LayerSurface.cpp:344-353`).

Consequence for a host monitor: place it in the layout like any other output, and *never move
it* casually; if it moves, `arrangeLayersForMonitor` must run (which `moveTo` now does).

### F5 — There is no `monitor` layerrule, and layer rules match only `namespace`

The complete effect list is `LayerRuleEffectContainer.cpp:12-25`, guarded by
`static_assert(LAYER_RULE_EFFECT_LAST_STATIC == 11)`:

`no_anim`, `blur`, `blur_popups`, `ignore_alpha`, `dim_around`, `xray`, `animation`, `order`,
`above_lock`, `no_screen_share`.

`CLayerRule::matches` (`LayerRule.cpp:96-114`) honours exactly one match property —
`RULE_PROP_NAMESPACE` — and logs+skips everything else. The shared match-prop table lives at
`Rule.cpp:27-45` (`namespace` is the last entry). Effect values are a
`std::variant<monostate,bool,int64_t,float,std::string>` (`LayerRule.hpp:11`), so a
string-valued effect (a monitor name) is already expressible.

Adding one effect costs: enum entry (`LayerRuleEffectContainer.hpp`), string entry + bumped
`static_assert` (`.cpp:12-29`), a `parseEffect` case (`LayerRule.cpp`), a `DEFINE_PROP`
(`LayerRuleApplicator.hpp:50-62`), the reset tuple (`LayerRuleApplicator.cpp:34`), an
`applyDynamicRule` case (`:98-100` is the template), and a Lua mirror
(`LuaBindingsConfigRules.cpp:192`). ≈40 lines, entirely mechanical.

---

## 4. Options

### A. True per-surface extraction — a layer surface gets its own swapchain and quad

The compositor grows a second quad-source type beside `CXRMonitorLayer`: a
`CXRSurfaceLayer` that owns an `Aquamarine::CSwapchain`, renders one layer surface into it
each frame, and submits it as a quad with its own `CXRAnchor`.

The rendering half is genuinely tractable — the primitives exist:
- `beginRenderToBuffer(PHLMONITOR, CRegion&, SP<IHLBuffer>, simple)` (`Renderer.cpp:3017`),
  already used by `CCursorshareSession::copy` (`CursorshareSession.cpp:157`);
- `makeSnapshotFB(PHLLS)` (`Renderer.cpp:3098-3138`) is a working, self-contained example of
  rendering exactly one layer surface into an arbitrary framebuffer;
- an owned `Aquamarine::CSwapchain` has precedent in `PointerManager.cpp:477` (the cursor
  swapchain), and would need `SSwapchainOptions::multigpu` set the same way
  `m_forceLinearSwapchain` drives it for XR outputs (`Monitor.cpp:2743-2745`).

What it costs, beyond the render:

1. **A damage + frame-callback scheduler (F1).** The surface must keep getting
   `wl_surface.frame`, and the extracted quad must know when to re-blit. Today both fall out
   of monitor overlap; extracted, both must be written and maintained.
2. **A pointer path that isn't `m_boundOutput` (F2).** `dispatchInputEvent`'s absolute-motion
   path is monitor-name-keyed. Routing the ray into a non-monitor surface means either
   synthesising a global cursor position inside `m_geometry` (which means the surface still
   needs a monitor plane — circular) or bypassing `mouseMoveUnified` entirely and calling
   `setPointerFocus(surface, local) + sendPointerMotion` from quad UV. The latter is clean in
   principle (the hit-tester's contract is literally `local = global − geometry.pos()`,
   `ViewHitTester.cpp:335`) but it is a second, parallel input path with its own
   enter/leave/grab/constraint semantics to keep correct.
3. **Generalising anchor, `xrrule`, chrome, grab, gaze from "monitor" to "quad".** Every one
   of those subsystems is keyed by `MONITORID` today (`layerByMonitorID` :1213,
   `SXRPointerTarget.id`, `chromeHoverRegion(mid)`, `SXRRuleContext.monitorName`). Widening
   the key is mechanical but touches ~everything in `src/openxr/`.
4. **Suppressing the surface on its origin monitor** — the easy part: one early-out in
   `renderLayer` at `Renderer.cpp:941`, mirroring `above_lock` (`:943-945`), which covers all
   six call sites including the unmap snapshot.

- **Pro:** the only design that yields a quad *exactly* the size of the surface with no
  phantom monitor, no workspace, no wallpaper/bar clients spawning on it, and correct
  behaviour for surfaces whose size changes per map (mako notifications).
- **Con:** re-implements three subsystems the compositor already has, for a cosmetic win.
  **XL.** Also a maintenance liability against upstream: a second quad-source type doubles
  the surface area every future renderer refactor has to be checked against.

### A′. Extraction with a borrowed monitor plane

Same as A, but the surface stays in a real monitor's `m_layerSurfaceLayers` (so damage, frame
callbacks, arrangement, hit-testing and popup constraint-solving all keep working), and the
XR side merely *also* renders it into a private swapchain while `renderLayer` skips it on that
monitor.

- **Pro:** kills cost (1) and most of (2); keeps the exact-fit quad.
- **Con:** the surface now has a schizophrenic identity — arranged and hit-tested against a
  monitor it is deliberately never composited into. The pointer still has to be routed by hand,
  since the physical cursor would have to be teleported into the origin monitor's box to reach
  it. Every "why is this invisible" bug becomes a two-place investigation. **L.**

### B. A user-configured head-anchored micro-monitor

Declare a small XR monitor with `anchor:head` and steer walker onto it. Purely config, given
one new generic layer effect:

```ini
layerrule = monitor XR-launcher, match:namespace ^(walker)$
xrmonitor = XR-launcher, 900x520, anchor:head offset:0,-0.05,-0.75, size:0.55
xrrule    = blackalpha 0.0, monitor:XR-launcher
```

Everything in §2 then applies to the launcher for free: leash, spring, deadzone, eased
transparency, chrome you can grab and reposition, ray + gaze targeting, the layer cap, plug
lifecycle.

- **Pro:** tiny. The steering effect is ≈40 mechanical lines (F5) plus a 3-line change at
  `LayerSurface.cpp:26/54` to consult it. Zero new XR code.
- **Con:** the quad is **always there** — an empty 0.55 m panel welded in front of your face
  whenever the launcher is closed, which is most of the time. That is not shippable.
  Secondary problems: the host monitor is a full citizen (workspace, `focusmonitor` target,
  wallpaper and bar clients bind to it, appears in `hyprctl monitors`), and it must be sized
  by hand to whatever walker asks for.

### B′. Compositor-managed layer host — **recommended**

B, plus the one piece that makes it usable: **the quad is hidden while nothing is mapped on
the host.** Add a `hideempty` token to `xrmonitor` (or `openxr:hide_empty_quads`) meaning:
when the host monitor has no mapped layer surface and no window, drive `m_fxAlpha` to 0
through the existing `SXRFxEnv` envelope, and once it settles, clear `m_quadActive` so the
quad leaves the submitted array entirely.

Both halves already exist and are already main-thread-safe:
- `m_fxAlphaEnv` / `m_fxAlpha` with `transparency_blend_ms` easing
  (`XRMonitorLayer.hpp`, `advanceEffectEnvelopes` on an 8 ms tick) — this *is* the fade-in on
  map and fade-out on unmap, for free, with no new animation code;
- `m_quadActive` (`OpenXRManager.cpp:1795`, set by `recomputeQuadActive` :3943) — the quad
  simply stops being submitted; the headless output keeps compositing at zero cost because
  nothing is on it.

The trigger points are the two events that already exist: `CLayerSurface::onMap` /
`onUnmap` (which already post `openlayer` / `closelayer` with the namespace,
`LayerSurface.cpp:225`, `:352`).

Optional refinement (v2): on map, resize the host monitor to the layer's `desiredSize` so the
quad is *exactly* the launcher with no margin. `applyMonitorRule` → mode change →
`m_swapchainDirty` → the frame thread recreates the swapchain on its next pass; all of that
plumbing is already exercised by every `xrmonitor` mode change.

- **Pro:** the launcher appears head-leashed at a comfortable distance with an eased fade,
  disappears when dismissed, respects `xrrule`, can be grabbed and re-parked mid-session,
  keeps full keyboard focus semantics (F2), and is reachable by ray and gaze — all using
  shipped machinery. **Nothing in `src/openxr/` needs a new object type.**
- **Con:** the phantom monitor is real and must be managed (§7.3): it takes a workspace, it is
  a `focusmonitor` target, and wallpaper/bar clients that bind every output will spawn on it.

### C. Run walker as an hypxrhud client

hypxrhud already owns an `XR_EXTX_overlay` session with six themed slots
(`~/code/hypxrhud/src/Session.cpp:162-169`, `src/Slots.cpp:9-18`). Superficially "the HUD
daemon should show the launcher".

It is the wrong tool, three times over, and the evidence is in the repo:

1. **No buffer import.** Panel content is `kind = text|gauges` rasterised CPU-side with
   stb_truetype. There is no wl_buffer / dmabuf / texture ingress at all. Presenting a real
   Wayland client through it means inventing one.
2. **No input.** Zero matches for `keyboard|xkb|libinput|evdev` across `src/`; the D-Bus
   interface is write-only presentation (`CreatePanel`/`UpdatePanel`/`DismissPanel`). The
   design doc is explicit that head-locked panels are display-only — *"there is no comfortable
   head-locked dismiss interaction"* (`HYPXRHUD.md:732-745`). A launcher is nothing but input.
3. **No leash.** Panels are rigid VIEW-space quads with identity orientation and no smoothing
   anywhere (`Session.cpp:365-388`). Welding a search field to your eyeballs is worse than
   leashing it; the leash lives in `CXRAnchor` and would have to be re-implemented.

`VISUALS.md:36-41` already states the doctrine this obeys: *"motion and pose effects are free
and must stay in the compositor … rich presentation belongs in a companion overlay client."*
A launcher is pose + input, i.e. compositor-side by that rule.

Worth keeping from C: hypxrhud's **`blackalpha`-free alpha discipline** for passthrough
(`src/BlendMode.hpp`) and its `rise/hold/fade` envelope defaults (110/2600/450 ms, ceiling
opacity 0.92) as sanity references for §7.2's fade timing.

### D. Own the launcher

A different axis entirely: instead of teaching the compositor to present *someone else's*
surface well, change or replace the launcher. Three rungs, cheapest first.

#### D-ground-truth: what walker actually is

Read off the installed packages (`pacman -Qi`, `strings`) — worth stating precisely, because
the maintenance argument turns on it:

| | walker 2.16.2 | elephant 2.21.0 |
|---|---|---|
| Upstream | `github.com/abenz1267/walker` | `github.com/abenz1267/elephant` |
| Language | **Rust** (cargo build; `gtk4-layer-shell-0.5.0` crate, `protobuf-3.7.2` crate) | **Go** (`google.golang.org/protobuf` in the binary) |
| Runtime deps | `gtk4-layer-shell`, `poppler-glib`, `cairo` | none |
| Installed size | 6.5 MiB | 20.9 MiB |
| Role | **UI shell only** — GTK4 window, XML layout + CSS theme, keybinds, config | **all provider logic + execution** — "general purpose datasource and executor" |
| IPC | unix socket, protobuf (`internal/comm/client.socket` on the Go side) | same |

That is a *clean* split, and it is the single most useful fact in this section: **all of the
launcher's intelligence — desktop applications, files, calc, symbols, clipboard, websearch,
providerlist, and the *execution* of the chosen item — lives in elephant, behind a stable
protobuf-over-unix-socket API that is not GTK, not Wayland and not walker.** The Omarchy
provider set is pure config (`~/.config/walker/config.toml` `[providers]` + `[[providers.prefixes]]`);
`omarchy-launch-walker` merely ensures `elephant` and `walker --gapplication-service` are up
and then runs `walker --width 644 --maxheight 300 --minheight 300`.

Walker's own config also matters here. The binary's embedded config schema contains a
`struct Shell` with exactly six fields — `exclusive_zone`, `layer` (`top`|`overlay`),
`anchor_top`, `anchor_bottom`, `anchor_left`, `anchor_right` — and a top-level
**`as_window`** boolean (alongside `force_keyboard_focus`, `close_when_open`,
`click_to_close`, `disable_mouse`, `ext_background_effect_blur`). So walker can already be
told to render as an **ordinary xdg-toplevel window** instead of a layer surface. It exposes
no output selector in that schema.

#### D0. Zero code today: `as_window` + `windowrule = monitor` — **do this first**

Hyprland already ships a `monitor` **window** rule effect
(`src/desktop/rule/windowRule/WindowRuleEffectContainer.cpp:23`, enum
`WINDOW_RULE_EFFECT_MONITOR`). So:

```ini
# ~/.config/walker/config.toml
as_window = true

# hyprland
xrmonitor  = XR-launcher, 900x520, anchor:head offset:0,-0.05,-0.75, size:0.55
windowrule = monitor XR-launcher, match:class ^(walker)$
windowrule = float,               match:class ^(walker)$
windowrule = center,              match:class ^(walker)$
xrrule     = blackalpha 0.0, monitor:XR-launcher
```

This is **not the recommendation**, but it is the highest-value thing in this memo: it lets
the user answer the questions that actually decide the design — *is 0.75 m the right
distance? does a 15° deadzone feel right for a thing you read for two seconds? does the luma
key make a dark launcher theme unreadable? is an always-present empty host intolerable?* —
tonight, with no compiler involved.

- **Pro:** zero code. Reuses shipped rules on both sides. Immediately validates or kills the
  whole B′ hypothesis.
- **Con:** as a *window* the launcher acquires a workspace, tiling/floating semantics, and
  window focus rather than layer-shell focus — so dismissal, `close_when_open` and
  above-fullscreen behaviour all change subtly. It does not generalise to mako/swayosd at all
  (they have no `as_window`). And the always-visible empty host (§4.B's flaw) is still there.
  Treat it as an **experiment**, not a shipping configuration.

#### D1. Fork walker

The project already carries forks of Hyprland, WiVRn and Monado, so the bar is not "is
forking scary" but "is the delta small and stable". Honest answer: **the delta would be
small; the value is near zero.**

What a fork could add that config cannot:

| Idea | Verdict |
|---|---|
| An `--output <name>` flag (pass a real `wl_output` to `get_layer_surface`) | ~20 lines in Rust. **But this is upstreamable, not fork-worthy** — it is an obviously correct feature with no XR in it. File it upstream; carry a patch meanwhile |
| XR-aware sizing / DPI (bigger hit targets, larger type at 0.75 m) | Already expressible: walker's theme is XML layout + CSS in `~/.local/share/omarchy/default/walker/themes/`, and `--width/--maxheight/--minheight` are CLI. An XR theme is **config, not code** |
| "Render into a surface the compositor treats specially" | There is no such thing to opt into. The compositor's specialness is keyed off namespace/class rules that already match walker unmodified. A fork would be adding a marker the compositor could read from `m_namespace` anyway |
| "Hide the flat surface while presenting the quad" | The flat surface *is* the quad's content — there is no second copy to hide (§4.A item 4 is a one-line renderer skip if it were ever needed, and it lives in the compositor, not walker) |
| Curved / XR-native result layout | GTK4 renders to a flat buffer. A curved panel is a *compositor* concern (an equirect or a mesh layer), not something a GTK client can do |

- **Pro:** total control; the `--output` patch is genuinely tiny.
- **Con:** every capability worth having is either upstreamable or already config. A fork buys
  a permanent tracking cost (walker is on a fast 2.x release cadence — 2.16.2 today — and
  Omarchy pins its own theme/config integration on top) in exchange for approximately nothing.
  It also fixes exactly one client, which is the wrong shape for the problem (§8).
- **Verdict:** file the `--output` feature request upstream (§10 WP-N9); do not fork.

#### D2. A purpose-built `hypxrlauncher` daemon

A family member beside hypxrvoice / hypxrhud / hypxrkeys: a native-XR launcher that renders
its own quad (reusing hypxrhud's overlay session, theming and slot machinery) and queries
**elephant** directly over its protobuf socket for results and execution — i.e. keep the
brain, replace the GTK shell with an XR-native one.

This is the most *appealing* option on paper and it has one hard problem.

**The crux: getting keystrokes into a daemon-owned XR panel.** hypxrhud panels are output-only
(§4.C); the user's keyboard focus lives in the compositor's seat. Four real mechanisms:

1. **An invisible focused layer surface owned by the daemon.** The daemon opens its own
   `zwlr_layer_surface_v1`, 1×1, fully transparent, `keyboard_interactivity = EXCLUSIVE`,
   commits one buffer, and never draws again — while rendering its actual UI as an XR quad in
   its overlay session. It then receives the full `wl_keyboard` stream (keymap fd + keycodes)
   and composes text with xkbcommon.
   - **This works today with zero compositor changes**, and the semantics are exactly right:
     `m_exclusiveLSes` (`InputManager.hpp:188`) makes the compositor refuse keyboard focus to
     every window while it is up (`FocusState.cpp:109-112`) — i.e. free modality — and unmapping
     the surface restores focus through the normal path.
   - Costs: an xkbcommon compose/repeat implementation in the daemon (the `05-xr-screenkey.md`
     design already works this problem for hypxrkeys, so the family would only solve it once);
     one more Wayland client with a layer surface; and the 1×1 surface must sit on a real
     monitor plane to map (F1) — trivially satisfied.
   - **This is the answer.** Every other option below is worse.
2. **Compositor forwards a key grab over IPC.** Precedent exists in the tree
   (`src/protocols/{GlobalShortcuts,Hotkey,ShortcutsInhibit,InputCapture}.cpp`), but all of it
   is *discrete shortcut* plumbing. Text entry over `socket2` (a line-based text stream with no
   keymap, no modifiers, no repeat) is a non-starter; a new binary protocol for "stream me the
   keyboard" is a security-relevant compositor feature that duplicates what option 1 gets from
   standard Wayland.
3. **Input-method / virtual-keyboard protocols.** `zwp_input_method_v2` (`InputMethodV2.cpp`)
   and `zwp_virtual_keyboard_v1` (`VirtualKeyboard.cpp`) are both **injection** paths — they
   let a client *produce* text, not *consume* keystrokes. Wrong direction. (They are, however,
   exactly what a *voice* launcher would use to type into the focused app, which is a separate
   and genuinely good idea.)
4. **Voice-first, keyboard-optional.** hypxrvoice already ships a `launch` verb behind a strict
   allowlist (`~/code/hypxrvoice/README.md:82`, `:509-511`) and a gaze/deixis model. A
   gaze+voice launcher is a legitimate product framing — "say the app name, look at where it
   should go" — and it needs **no** keyboard path at all. But it is an *addition* to a
   keyboard launcher, not a replacement: the user is a full-time keyboard-driven Omarchy user
   and `SUPER+SPACE` is muscle memory.

Beyond input, D2 must also build: a text renderer good enough for a fuzzy-match result list
(hypxrhud rasterises with stb_truetype and its content model is `text|gauges` only — a result
list with icons is a new content kind), scrolling/selection, icon loading (desktop-entry icon
themes), the elephant protobuf client, theming that tracks Omarchy's current theme (hypxrhud
has this, `src/Theme*.cpp`), and per-provider result formatting that walker currently gets from
14 XML item templates in `/etc/xdg/walker/themes/default/`.

- **Pro:** the only option that can ever produce genuinely XR-native presentation — a curved
  or multi-plane result panel, results placed in depth, gaze-to-select rows, a launcher that
  knows *which monitor you are looking at* so "open Firefox here" means something. Reuses the
  elephant brain, so none of the provider logic is reimplemented. Fits the established family
  pattern.
- **Con:** **it does not help mako, swayosd, or any other transient UI.** That is likely the
  decisive point (§5). It is also the longest path to usable — realistically L/XL, most of it
  spent re-earning walker's polish (icons, item templates, fuzzy ranking presentation) rather
  than on anything XR-native. And the SPOF/lifecycle burden hypxrhud already documents
  (`HYPXRHUD.md:682-716`) doubles.
- **Verdict:** not now. Revisit *after* B′ ships, when the question is "how do we make the
  launcher XR-native" rather than "how do we make it visible". Record the invisible-exclusive-
  layer-surface trick (mechanism 1) as the family's answer to daemon keyboard input — it also
  unblocks any future hypxrhud panel that needs input, which `HYPXRHUD.md:732-745` currently
  declares impossible.

### E. Userspace only — script the existing verbs off socket2

`openlayer>>walker` / `closelayer>>walker` are already emitted (`LayerSurface.cpp:225`, `:352`),
and `hyprctl openxr create|destroy|anchor|alpha` already exist. A daemon could create a
head-anchored monitor on `openlayer`.

- **Pro:** zero compositor code.
- **Con:** **it cannot work.** The monitor is chosen at *bind* time (`LayerSurface.cpp:26`),
  which precedes the map event the script would react to; by the time `openlayer` fires,
  walker is already on the wrong output and cannot be moved (F5, §1.2). Also `createXRMonitor`
  on every launch would run a full headless-output create + workspace churn per `SUPER+SPACE`.
  Include for completeness; reject.

---

## 5. Scorecard

Four axes matter, and the task framing named three of them explicitly: **input fidelity**,
**generality** (does it also fix mako/swayosd?), **maintenance**, **time-to-usable**.

| Option | Size | Input fidelity | Generality | Maintenance | Time to usable |
|---|---|---|---|---|---|
| A extraction | **XL** | new parallel pointer path to write + keep correct | **full** | a second render+input path forever | weeks |
| A′ borrowed plane | L | same, plus split-brain geometry | full | two-place debugging | weeks |
| B config micro-monitor | **S** | **native** (real seat, real ray) | full | ~nil | days |
| **B′ managed layer host** | **M** | **native** | **full** | ~nil (no new XR object type) | **days** |
| C hypxrhud client | L | **none** — panels cannot take input | none | +1 daemon | weeks |
| D0 `as_window` + `windowrule` | **0** | native, but *window* not layer semantics | **none** | nil | **tonight** |
| D1 fork walker | S | native | **none** | permanent upstream tracking | days |
| D2 `hypxrlauncher` daemon | **L/XL** | native *if* the invisible-exclusive-LS trick is used | **none** | +1 daemon, +1 SPOF | weeks–months |
| E socket2 script | — | n/a | n/a | nil | **never (broken)** |

Mechanism detail behind the same rows:

| Option | New XR object type? | Frame callbacks work? | Ray/gaze works? | Fade in/out | Exact-fit quad | Phantom monitor |
|---|---|---|---|---|---|---|
| A | yes | **must be written** | must be written | must be written | **yes** | no |
| A′ | yes | free | must be written | must be written | **yes** | no |
| B | no | free | free | **no — always visible** | no | yes |
| **B′** | **no** | **free** | **free** | **free (`SXRFxEnv`)** | v2 (mode change) | yes |
| C | n/a | n/a | **impossible today** | free | yes | no |
| D0 | no | free | free | no | no | yes |
| D1 | no | free | free | no | no | yes |
| D2 | daemon-side | n/a (daemon draws) | daemon-side | daemon-side | **yes** | no |
| E | no | free | free | no | no | yes |

Failure-mode summary:

| Option | Main failure mode |
|---|---|
| A | A whole second rendering + input path to keep correct forever; the first upstream renderer refactor breaks it silently |
| A′ | Two-place debugging for every invisible-surface bug |
| B | An empty panel welded in front of your face 99 % of the time |
| **B′** | The host monitor is a full compositor citizen — wallpaper/bar clients, a workspace, a `focusmonitor` target (§7.3) |
| C | No input, no buffer ingress, no leash — three subsystems to invent |
| D0 | Window semantics ≠ layer semantics; helps nothing but walker |
| D1 | Permanent tracking cost for capabilities that are upstreamable or already config |
| D2 | Re-earns walker's polish (icons, item templates, ranking UI) before doing anything XR-native; helps nothing but walker |
| E | Cannot work: the monitor is bound before the event fires |

---

## 6. Recommendation

**Build B′.** The decisive argument is F1: **a layer-shell client that is not on a monitor
plane stops getting `wl_surface.frame` callbacks and freezes** (`Renderer.cpp:2683-2695`).
Every "extract the surface into its own quad" architecture must therefore either keep a
monitor plane anyway (A′ — at which point the monitor is doing the work and the extraction is
decoration) or write a private damage + frame-callback scheduler (A — a permanent parallel
path). Since a *monitor* is exactly "a rectangle of desktop with damage tracking, frame
scheduling, a compositing pass and a presented dmabuf", and the XR side already turns any such
rectangle into an anchored, transparent, grabbable, ray-targetable quad, the cheapest correct
"launcher quad" is a small monitor with `hideempty` on it.

The second argument is scope: B′'s three pieces are independently useful and two of them have
no XR in them at all. `layerrule = monitor <name>` is a plainly useful generic Hyprland feature
("put my notifications on the vertical display") with a fair upstream story. `hideempty` is a
one-boolean XR feature. `xrrule = blackalpha …` already ships.

The third argument is **generality, and it is what rules out the whole "own the launcher"
family (§4.D)**. Walker is 40 of the 133 transient layer surfaces in the live census; mako is
78 and swayosd 15 (§8). D1 (fork walker) and D2 (a native `hypxrlauncher`) fix exactly one of those
and, by construction, can never fix the others — mako will not be rewritten as an XR daemon,
and neither will swayosd. B′ fixes all three with one rule per namespace. Given that the *same*
compositor mechanism serves every current and future layer-shell client, spending L/XL on a
single-client solution is the wrong trade even though D2 is the more exciting artefact.

**Phase 0 (do this tonight, no code): D0.** `as_window = true` in walker's config plus
`windowrule = monitor XR-launcher` against a hand-declared `xrmonitor` (§4.D0). It answers the
comfort questions — distance, deadzone feel, luma-key legibility, whether an always-present
empty host is tolerable — before a line of C++ is written, and it de-risks every number in
§7.2/§7.4.

**Phase 1 (do this): WP-N1…N5** — the steering effect, the host-monitor lifecycle, the empty
gate + fade, the config surface, tests. Ships walker head-leashed.

**Phase 2 (do this next): WP-N6…N8** — auto-fit sizing, mako/OSD generalisation, focus
restore. Turns it into the general capability.

**Phase 3 (defer, maybe never): true extraction (A).** Revisit only if a concrete requirement
appears that a host monitor genuinely cannot serve. The two candidates are (a) surfaces whose
size varies wildly per map and per instance — a stack of three mako notifications of different
heights is the real case — and (b) wanting *several* independent transient panels at once
without one host monitor each. Note that (b) is also solvable by simply declaring several
hosts, and the layer budget (≥16) is nowhere near binding.

**Explicitly not recommended:**

- **C** — hypxrhud cannot take input or ingest a client buffer, and its panels have no leash
  (§4.C).
- **D1 (fork walker)** — every capability a fork would buy is either upstreamable (`--output`)
  or already config (XR theme, sizes). It trades permanent tracking cost for ~nothing, and
  fixes one client (§4.D1).
- **D2 (`hypxrlauncher`)** — the most attractive artefact and the wrong next move: L/XL, most
  of it re-earning walker's polish rather than doing anything XR-native, and it helps neither
  mako nor swayosd. Its one durable contribution is the **input mechanism** — an invisible 1×1
  `EXCLUSIVE` layer surface owned by the daemon gives a companion process the full `wl_keyboard`
  stream and free modality with **zero compositor changes** — which is worth recording now,
  because it also refutes `HYPXRHUD.md:732-745`'s standing claim that head-locked panels can
  never be addressable (§4.D2, WP-N10).
- **E** — structurally impossible (§4.E).

**What D would additionally buy, as future work.** Once B′ ships and the launcher is merely
*visible*, the remaining wins are presentational and only D2 can deliver them: a curved or
multi-plane result panel (a GTK client renders to a flat buffer; curvature is a compositor or
overlay-client concern), results laid out in depth, gaze-to-select rows with dwell, and
context the launcher cannot have today — "open Firefox **here**", resolved against the quad
you are looking at, which hypxrvoice's deixis ring already publishes
(`recordPoseSample`, `OpenXRManager.cpp:1941`). That is the right time to revisit D2, and by
then the elephant protobuf client is the only genuinely new component, because the keyboard
question will already be answered.

---

## 7. Design detail

### 7.1 Config surface

Preferred — three orthogonal pieces, using the existing keyword families:

```ini
# generic Hyprland: steer a layer surface to a named output at bind time
layerrule = monitor XR-launcher, match:namespace ^(walker)$
layerrule = monitor XR-hud,      match:namespace ^(notifications|swayosd)$

# XR: the hosts themselves (xrmonitor grammar, doc 05 §xrmonitor, + one new token)
xrmonitor = XR-launcher, 900x520,  anchor:head offset:0,-0.05,-0.75, size:0.55, hideempty:on
xrmonitor = XR-hud,      700x1000, anchor:body offset:0.45,0.1,-0.9, size:0.35, hideempty:on

# XR: presentation (already shipped)
xrrule = blackalpha 0.0, monitor:^XR-(launcher|hud)$
```

Rejected alternative — a dedicated `xrlayerrule = headleash, namespace:walker` keyword. It
reads well but it fuses two concerns (*where does this surface live* and *how is that place
posed*) that the existing grammar already separates cleanly, and it would need its own
matcher, its own reconcile-on-reload path, and its own `hyprctl` surface, duplicating
`xrrule`'s. A `xrlayerrule` also has nowhere sensible to put the pixel mode, which the host
needs. If a single-line convenience form is wanted later, it can desugar to the three lines
above.

Also worth adding (cheap, orthogonal): an `xrrule` condition `empty:0|1` so a user can express
"ghost the launcher host when it has nothing on it" declaratively instead of via `hideempty`.
That would make `hideempty` sugar for `xrrule = alpha 0.0, monitor:X empty:1`. Decide during
WP-N4; the sugar is friendlier, the rule is more composable.

### 7.2 Lifecycle and timing

| Event | Action |
|---|---|
| Session start / host declared | `createXRMonitor` as today; quad starts at `alpha 0`, `m_quadActive=false` |
| Layer binds, steering rule matches, host exists **and is enabled** | `m_monitor = host`; push into `host->m_layerSurfaceLayers[level]` |
| Steering rule matches but host missing/unplugged | fall through to `focusState()->monitor()` — i.e. today's behaviour, verbatim |
| `onMap` | host becomes non-empty ⇒ `m_quadActive = true`, retarget `m_fxAlphaEnv` to the host's resolved alpha; fade in over `transparency_blend_ms` |
| `onUnmap` | host becomes empty ⇒ retarget `m_fxAlphaEnv` to 0; when the envelope settles, `m_quadActive = false` |
| Session end / doff | existing `setMonitorsPlugged(false)` unplugs the host, which `sendClosed()`s anything on it (`Monitor.cpp:477-483`) and evacuates its workspace. New layer surfaces then land on the focused real monitor — the fallback is automatic |
| Config reload | existing `xrmonitor` reconcile (doc 05 §2.5) |

Fade timing: `transparency_blend_ms` defaults to 600 ms, which is right for a *situational*
ghosting change but sluggish for a launcher. hypxrhud's shipped envelope is 110 ms rise /
450 ms fall (`~/code/hypxrhud/src/Config.hpp:42-46`). Recommend a separate
`openxr:hide_empty_fade_ms` defaulting to ~120 ms in / ~200 ms out rather than reusing the
600 ms situational constant. (The `SXRFxEnv::retarget`/`advance` pair already takes a
per-call duration, so this is a parameter, not new machinery.)

Comfort placement: `anchor:head` with `offset:0,-0.05,-0.75` puts the panel 0.75 m out and
5 cm below eye level. The leash defaults (`leash_response 0.35 s`, `leash_deadzone_angle 15°`,
`leash_deadzone_distance 0.25 m`, doc `03-anchoring.md` §4.2) mean it parks while you read and
follows only when you turn — which is exactly the desired "it's where I look, but it isn't
glued to my face" feel. **Do not** use a device (grip) anchor: walker is keyboard-driven and
your hands are on the keyboard.

### 7.3 The phantom-monitor tax, and how to pay it

A host monitor is a full `CMonitor`. Consequences and mitigations:

| Consequence | Mitigation |
|---|---|
| It gets a workspace, appears in `hyprctl monitors` / `workspaces` | Accept, and document. Optionally give it a bound workspace name (`workspace = name:xrhost, monitor:XR-launcher, default:true`) so it never steals a numbered one |
| `focusmonitor`/`movewindow mon:` can target it; `cyclenext` across monitors visits it | Warrants a future `CMonitor` flag ("not a placement target"). None exists today (`Monitor.hpp:80-128` has `m_createdByUser`, `m_isUnsafeFallback`, `m_xrManagedPlug` — no such concept). Note as an open question, not a blocker |
| Wallpaper and bar clients bind every output ⇒ a `wallpaper` layer and a `waybar` layer spawn on it, and would then be composited into the launcher quad | Two options: (a) per-client config — waybar takes an `output` list, most wallpaper daemons take per-output config; (b) a generic negative form of the same new effect, `layerrule = monitor "", match:namespace ^(waybar\|wallpaper)$` is *not* expressive enough — better is an explicit `layerrule = block_monitor XR-launcher, …`. **Simplest v1: choose (a).** Live census (§8) says the daemons in play are exactly `waybar` and `wallpaper` |
| Hyprland's own background (splash/logo) renders under an empty host | `misc:disable_hyprland_logo` + a transparent `background_color`, or rely on `blackalpha 0.0`. Verify: `renderBackground` runs before the layer loop (`Renderer.cpp:1134`, `:1162`) |
| Physical mouse can never reach it (F2) | By design. Ray + gaze reach it |

The wallpaper/bar issue is the one that will actually bite in the user's setup, because both
daemons are running live (`ps`: `mako`, `waybar`) and the namespace census shows `wallpaper`
and `waybar` surfaces on every output.

### 7.4 Sizing and the luma-key interaction

Walker asks for **644×300** and sets no anchors, so `arrangeLayerArray` centres it in the host
(`Renderer.cpp:2588`, `:2601`). Two v1 choices:

- **Fixed generous host (900×520) + `blackalpha 0.0`.** The empty margin is pure black and the
  luma key dissolves it, so the quad *looks* exactly like the launcher. Requires the `alpha`
  environment blend mode — which this session has (`hyprctl openxr status`: `blend mode: alpha`).
  **Caveat, and it is a real one:** the luma key cannot distinguish "monitor background black"
  from "walker's dark theme background". Omarchy's walker theme is dark, so it will become
  partly transparent too. That may read as premium glass or as unreadable, depending on
  `blackalpha_knee`; `VISUALS.md:76-77` warns about exactly this ("don't let dynamic effects
  drop a focused panel below readable").
- **Auto-fit (v2).** On map, apply a monitor rule sized to the layer's `desiredSize`
  (`CLayerShellResource::SState::desiredSize`, `LayerShell.hpp:54-68`), so there is no margin
  and no need for the luma key at all. Costs one mode change + swapchain recreate per open
  (both already exercised by `xrmonitor` mode changes; `m_swapchainDirty` handles it). This is
  the right end state and it is what makes mako (variable-height notifications) work.

Recommend shipping fixed-size first (it is zero XR code) and treating auto-fit as WP-N6.

### 7.5 Threading

Everything in B′ is **main thread**, which is what keeps it inside the established rules
(`XRMonitorLayer.hpp` thread-safety block; `MEMORY.md` "no refcount ops, no STRING config
reads on the frame thread"):

- Steering runs in `CLayerSurface::create` — main thread, no XR involvement at all.
- Emptiness tracking runs in `onMap`/`onUnmap` — main thread; it reads
  `host->m_layerSurfaceLayers` and writes the layer's envelope target under `m_layersMu`,
  exactly like `evaluateMonitorEffects` / `publishLayerEffects` do today
  (`OpenXRManager.cpp:3456-3538`).
- The frame thread sees only the two existing plain atomics `m_fxAlpha` and the
  lock-protected `m_quadActive` — no new cross-thread state, no new strings, no new refcounts.
- The `hideempty` flag itself is parsed on the main thread at config time into a `bool` on the
  layer, following the `publishGrabStringTuning` / `publishBlackAlphaTuning` precedent
  (`OpenXRManager.cpp:3312`, `:3612`).

### 7.6 Focus semantics

Map-time focus is already correct and monitor-independent (F2). **Unmap-time focus is not.**
`CLayerSurface::onUnmap` calls `refocusLastWindow(PMONITOR)` (`LayerSurface.cpp:270`) with
`PMONITOR` = the host. `refocusLastWindow` (`InputManager.cpp:1760-1810`) hit-tests that
monitor's overlay/top layers at the cursor, then checks
`focusState()->window()->m_monitor == pMonitor` — which fails for a host with no windows — and
falls through to the generic `refocus()`. Today walker unmaps on the monitor you were using,
so focus reliably returns there; with a host monitor it will return to wherever `refocus()`
lands.

Fix (WP-N8): remember the focused window/monitor when a steered layer maps, and restore it on
unmap. This is small but it is the difference between "the launcher feels native" and "every
launch shuffles my focus". It should be gated to steered surfaces only, so it cannot change
behaviour for anyone else.

---

## 8. Who else wants this

Full live namespace census across the six most recent session logs
(`grep -oh "namespace … layer N"`, counts = surfaces created):

| namespace | layer | count | Verdict |
|---|---|---|---|
| `waybar` | 2 (top) | 148 | **No.** Per-monitor status bar; it is *about* its monitor |
| `wallpaper` | 0 (background) | 137 | **No.** Same, and it must fill the output |
| `notifications` (mako) | 2 and 3 | 62 + 16 | **Yes** — the strongest secondary case. See below |
| `walker` | 3 | 40 | **Yes** — the subject of this memo |
| `swayosd` | 3 | 15 | **Yes** — volume/brightness OSD is exactly transient head-relative UI |
| `selection` (slurp/hyprshot) | 3 | 1 | **Never.** A region selector is inherently bound to the output being captured; extracting it would break the tool |
| `hyprpicker` | 3 | 1 | **Never**, same reason |

Not in the census but in scope: the **WiVRn menu** (runtime-side, not a Wayland layer — out of
reach of this mechanism entirely) and `hyprlock` (a session-lock surface, not a layer surface;
`ext-session-lock` is all-outputs-or-nothing, `research/20` §3.2 — out of scope).

### Relationship to HYPXRHUD WP-H10 (notifications-mirror)

`HYPXRHUD.md:369-405` proposes mirroring notifications into a head-locked HUD toast slot by
snooping D-Bus with `org.freedesktop.DBus.Monitoring.BecomeMonitor`, deliberately keeping
mako's own popup on a monitor quad as *the addressable surface* (`:732-745`), because HUD
panels cannot be clicked or dismissed.

B′ **complements and partly obsoletes** H10:

- **Obsoletes** its motivation. H10 exists because mako's popup lands wherever the focused
  monitor is — the same bug as walker's. Steering `notifications` to a leashed host puts the
  *real, addressable, themed* mako popup in front of you, clickable by the XR ray. That is
  strictly better than a read-only mirror toast, and it needs no D-Bus snooping at all.
- **Does not obsolete** the H10 blocker analysis, which is worth keeping regardless: the
  `BecomeMonitor`-under-dbus-broker verification (`:398-405`) is now optional rather than
  "step zero".
- **Complements** it for the case H10 was actually best at: notifications while a *VR game* is
  fullscreen and HypXRland is an overlay with no monitor quads visible. A HUD toast still wins
  there.

Recommendation for the HUD backlog: **re-scope H10 from "mirror" to "presence-gated
suppression"** — when a leashed notifications host is live, hypxrhud does nothing; when it is
not (game overlay), it mirrors. And note that H9 (hypxrkeys as a HUD client) is unaffected —
it is an input *observer*, not a consumer.

---

## 9. `xrrule` / transparency interplay

The extracted panel is a monitor, so it inherits the whole shipped effect stack with no new
plumbing (`XRRule.hpp`, doc 05 §xrrule). Three interactions to get right:

1. **The walking-safety rule already covers it.** The shipped recommendation
   `xrrule = alpha 0.55, anchorstate:follow` (doc 05, and `d…`/`a3f304d0` "ship the walking
   safety rule") matches on `anchorstate:follow`, and a `anchor:head` host is *always*
   `XR_ANCHORSTATE_FOLLOW` (`layerAnchorState`, `OpenXRManager.cpp:3409`). So a head-leashed
   launcher is automatically ghosted while walking, for free, with no new rule. Verify this is
   desirable — a 55 %-alpha launcher is still readable, and the alternative (an opaque panel
   pinned to your face while you walk) is the exact hazard that rule exists to prevent.
2. **Precedence must not fight the empty gate.** `hideempty`'s fade drives the same
   `m_fxAlphaEnv` that `xrrule` and the manual `hyprctl openxr alpha` override drive
   (`XRMonitorLayer.hpp`, `xrResolveEffects`). Cleanest resolution: make emptiness a *factor*,
   not a *source* — resolve `alpha` normally through defaults → rules → manual, then multiply
   by the empty gate (1.0 or 0.0, itself eased). That keeps `hyprctl openxr status`'
   provenance reporting (`XR_EFFSRC_RULE` / `_MANUAL`) honest, and means "ghost the launcher to
   0.7" and "hide it when empty" compose instead of racing. If instead `empty:` becomes an
   `xrrule` condition (§7.1), the same care applies in reverse: a later rule that sets `alpha`
   unconditionally would un-hide an empty host.
3. **Luma key vs. dark launcher themes** — see §7.4. `blackalpha 0.0` on the host is what makes
   a fixed-size host look like a floating panel, but it also keys walker's own background.
   Tune with `blackalpha_knee`, or sidestep entirely with auto-fit (WP-N6).

Also note: the chrome/grab machinery applies, so the launcher quad gets a move bar and corner
handles and can be dragged to a new spot — but a *head-anchored* quad that you grab enters
`XR_ANCHORSTATE_CARRIED` and, on release, resumes leashing from the new offset
(`XRAnchor.cpp` grab paths). That is the right behaviour ("park the launcher a bit lower") and
should be tested explicitly (WP-N5).

---

## 10. Work packages

Sizes: XS ≤ 50 lines, S ≤ 150, M ≤ 400, L ≤ 1000.

| WP | Size | What |
|---|---|---|
| **N0** | **0** | **Ergonomics spike, no code** (§4.D0): `as_window = true` + `windowrule = monitor XR-launcher` + a hand-declared `xrmonitor`/`xrrule`. Record answers to open questions 4, 5 and 7 before N2–N4 freeze any defaults. Revert afterwards |
| **N1** | S | **`layerrule = monitor <name>`** — new generic layer effect. Enum + string + `static_assert` bump (`LayerRuleEffectContainer.{hpp,cpp}`), `parseEffect` string case (`LayerRule.cpp`), `DEFINE_PROP` + reset tuple + `applyDynamicRule` case (`LayerRuleApplicator.{hpp,cpp}`), Lua mirror (`LuaBindingsConfigRules.cpp:192`). Plus the consumer: in `CLayerSurface::create`, move the namespace assignment above the monitor resolution and consult the rule engine at `LayerSurface.cpp:26`, with a hard fallback to `focusState()->monitor()` when the named monitor is absent or `!m_enabled`. Wiki-shaped, upstreamable on its own |
| **N2** | XS | **`hideempty:on\|off` token** in the `xrmonitor` grammar (`XRMonitorConfig.cpp` parser + `SXRMonitorParams` + serialisation in `hyprctl openxr layout`), stored as a plain `bool` on `CXRMonitorLayer` (main thread) |
| **N3** | S | **The empty gate.** A main-thread predicate "host has no mapped layer surface and no window", recomputed from `onMap`/`onUnmap`/window-move; multiply it into the resolved alpha (§9.2); retarget `m_fxAlphaEnv` with the new `openxr:hide_empty_fade_ms`; clear/set `m_quadActive` under `m_layersMu` when the envelope settles/starts. Reuses `advanceEffectEnvelopes` wholesale |
| **N4** | XS | **Config + docs.** `openxr:hide_empty_fade_ms`; doc `05-configuration.md` §xrmonitor token table + a worked launcher example; decide `hideempty` vs. an `xrrule` `empty:` condition (§7.1) |
| **N5** | M | **Tests.** `tests/xr/` unit: `hideempty` parse round-trip; the empty-gate × `xrrule` × manual-alpha precedence fold. `hyprtester` integration: reuse the new `hyprtester/clients/layer-surface.cpp` harness (added in `c3bdf3aa`) — assert a steered surface lands on the host, that the quad activates/deactivates on map/unmap, that an absent host falls back to the focused monitor, and that a grab on the host re-parks it |
| **N6** | M | **Auto-fit (v2).** On map of a steered surface, size the host to `desiredSize` via a monitor rule + `applyMonitorRule`; restore on unmap. Removes the luma-key dependency and is the prerequisite for variable-height notifications |
| **N7** | S | **Generalise to mako/swayosd.** A second host (`XR-hud`, `anchor:body`, off to one side), stacking policy when several notifications map at once (they stack vertically inside the host by mako's own anchors — verify against `arrangeLayerArray`'s margin handling, `Renderer.cpp:2604-2622`), and the HUD-backlog re-scope of WP-H10 (§8) |
| **N8** | S | **Focus restore.** Remember focused window+monitor on map of a steered surface; restore on unmap instead of falling through `refocusLastWindow(host)` → `refocus()` (§7.6). Gated to steered surfaces |
| **N9** | XS | **Ergonomics polish + upstream.** A `socket2` event when a layer host shows/hides (`xrlayerhost <name>,1|0`) so hypxrvoice/hypxrhud can react; file the walker `--output` feature request upstream (§4.D1) and the `layerrule = monitor` effect (N1) as a Hyprland PR — it has no XR in it |
| **N10** | XS | **Record the daemon-keyboard mechanism** (§4.D2 item 1) in `research/HYPXRHUD.md`: an invisible 1×1 fully-transparent `EXCLUSIVE` layer surface gives a companion daemon the whole `wl_keyboard` stream and free modality with zero compositor changes. This refutes the doc's standing "head-locked panels can never be addressable" premise (`HYPXRHUD.md:732-745`) and unblocks any future interactive HUD panel — docs only, no code |

Suggested order: **N0** → N1 → N2 → N3 → N4 → N5 (ship phase 1) → N8 → N6 → N7 → N9 → N10.

Rough total for phase 1 (N1–N5): **≈450–650 lines**, of which ~40 are the mechanical layerrule
boilerplate and the rest is the empty gate + tests. No new files in `src/openxr/`. N0 and N10
are free.

---

## 11. Failure modes to watch

1. **Host unplugged mid-use.** `setMonitorsPlugged(false)` `sendClosed()`s every layer on the
   host (`Monitor.cpp:477-483`) — so a doff while walker is open kills walker. Correct, but it
   must not happen *spuriously*; the plug gate's grace timers (`monitor_unplug_grace_ms`
   20 s) already guard this.
2. **Steering to a disabled host.** If the fallback check (`!mon->m_enabled`) is missed,
   `pMonitor->m_layerSurfaceLayers` on a disabled output silently swallows the surface: it is
   arranged, never rendered, never damaged, and the client freezes (F1). This is the single
   most likely bug in N1 and deserves its own test.
3. **Popup constraint solving.** Layer popups resolve their monitor *by position*
   (`Popup.cpp:207-208`, `:376` — `query().vec(COORDS)`), not by owner. A host monitor placed
   in an odd corner of the layout is fine as long as it is a real rectangle in the plane;
   placing it outside every monitor is not. Walker appears not to use popups, but GTK menus
   and tooltips arrive as xdg-popups on the layer — check `popupsCount()`
   (`LayerSurface.cpp:416`) on a live launcher before assuming.
4. **`arrangeLayersForMonitor` on host resize (N6).** A mode change re-arranges, which
   re-`configure`s the layer to a new size, which walker will honour — potentially fighting its
   own `--width`. Verify the configure/ack loop settles in one round trip
   (`LayerShell.cpp:189-197`, `:132-144`).
5. **Direct scanout.** A non-empty overlay array sets `SC_OVERLAYS` and blocks DS
   (`Monitor.cpp:1931-1943`). On a headless XR host that is irrelevant, but it is a reason not
   to steer transient UI onto a *real* monitor that a game is scanning out from.
6. **Wallpaper/bar clients on the host** (§7.3) — will otherwise be composited into the
   launcher quad and look like a bug.
7. **The walking rule ghosting the launcher to 55 %** (§9.1) — desirable but surprising the
   first time; document it.

---

## 12. Open questions

1. **Does walker create xdg-popups?** Cheap to answer on a live launcher
   (`hyprctl layers` + `popupsCount()`); decides whether §11.3 is a real risk or a footnote.
2. **`hideempty` flag vs. `xrrule empty:` condition** (§7.1). The flag is friendlier; the
   condition composes. Could ship the flag as sugar over the condition.
3. **Should a host monitor be excluded from placement?** No `CMonitor` flag exists for "not a
   `focusmonitor`/`movewindow` target" (`Monitor.hpp:80-128`). Worth adding generally, or is a
   bound workspace + user discipline enough?
4. **Comfort geometry.** `VISUALS.md:167` still asks "what comfort half-angle defines 'in
   view'?", and the view-bounding cone clamp (WP-B1/B2) is unimplemented — so a head-leashed
   host has no FOV clamp today. For a 0.55 m panel at 0.75 m the leash deadzone (15°) probably
   suffices, but this should be checked in-headset before defaults are frozen.
5. **Fade timing.** Is 120 ms in / 200 ms out right, or should it match hypxrhud's shipped
   110/450 for family consistency?
6. **Multiple simultaneous transient surfaces on one host.** Three mako notifications map at
   once: do they stack correctly inside a fixed host (mako anchors top-right by default, so
   yes), and what happens under auto-fit (N6) when the second one maps mid-fade?
7. **Does the luma key on a dark walker theme read as premium or as broken?** Purely
   subjective; settle in-headset before choosing fixed-size-plus-key vs. auto-fit as the
   default (§7.4).
8. **Overlay mode.** In `openxr:overlay = true` with a VR game in front, should a steered
   launcher host still show? Probably yes (it is the only way to launch anything), but it
   interacts with the HUD re-scope in §8.
9. **Does walker's `as_window` actually exist and behave as read?** The field was read out of
   the stripped 2.16.2 binary's embedded config schema, not from upstream documentation.
   N0 confirms it in five minutes; if it does not exist, the spike falls back to
   `layerrule` + a hand-built host and simply waits for N1.
10. **Does walker expose an output selector at all in 2.x?** No such field appears in the
    embedded schema and the CLI could not be enumerated from a stripped binary. Confirm before
    filing the upstream request in N9 — it may already exist under a name that did not survive
    stripping.
11. **Is the invisible-`EXCLUSIVE`-layer-surface trick acceptable as a family pattern?** It is
    a legitimate use of the protocol, but it makes a daemon able to swallow the entire keyboard.
    Worth a deliberate decision (and probably a `SecurityContext` note) before hypxrkeys or a
    future `hypxrlauncher` relies on it.
