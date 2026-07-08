# Research: Compositing HypXRland's Monitors With Other VR Applications

Research memo (2026-07-03). Question: what would it take for HypXRland's floating
desktop monitors to composite ON TOP of another running VR application — e.g. the
user's desktop living inside a Steam VR game? Research only; no code was changed.

Evidence base: the vendored Monado tree at `subprojects/monado` (pinned
`c2ddab59dc41366fe520dc4e8abcfea257ecf0b8`, the exact runtime our test suite runs
against), our own `src/openxr/` sources and `docs/openxr/00..07`, and web research
on SteamVR/OpenVR, WiVRn, wlx-overlay-s, and WayVR (URLs cited inline; all web
claims sourced 2026-07).

## TL;DR

1. Monado is already a multi-client XR compositor: it implements `XR_EXTX_overlay`, and its IPC server marks overlay sessions **always visible AND focused** — our FOCUSED-gated input keeps working unmodified while a game runs underneath.
2. The minimum change to HypXRland is tiny: chain `XrSessionCreateInfoOverlayEXTX` into `xrCreateSession` (one struct in `src/openxr/XRSession.cpp`) + enable the extension — everything else (quads, anchors, ray input) is composition-layer-based and carries over as-is.
3. This works on Monado local **and** WiVRn+Quest (WiVRn is Monado-based; wlx-overlay-s proves the path daily), including Proton games via OpenComposite/xrizer.
4. SteamVR-Linux does NOT support `XR_EXTX_overlay`; desktop-in-SteamVR requires a separate **OpenVR `IVROverlay` backend** (the wlx-overlay-s dual-backend model) — a much larger (L/XL) porting effort with a good architectural fit for our per-monitor quads/anchors but a second runtime API to maintain.
5. Recommendation: prototype **Option A (Monado/WiVRn overlay session)** first — it's an S-sized spike with high expected payoff; treat the OpenVR backend as a later, separately-scoped project, and treat input arbitration (game vs desktop toggle) as the main real design problem, not session plumbing.

## Background: how multi-client XR composition works

A VR runtime owns the display; apps submit *composition layers* (projection layers
for 3D scenes, quad layers for panels) each frame via `xrEndFrame`. Single-client
runtimes assume one session. Multi-client composition needs a *system compositor*
that accepts layers from several processes and merges them by z-order — exactly
what a 2D display server does for windows.

Two ecosystems solved this differently:

- **OpenXR world (Monado):** the experimental `XR_EXTX_overlay` instance extension
  (extension #34, authored by LunarG/Epic/PlutoVR — still `EXTX`, i.e. provisional,
  never promoted to EXT/KHR;
  [registry](https://registry.khronos.org/OpenXR/specs/1.0/man/html/XR_EXTX_overlay.html),
  [LunarG reference layer](https://github.com/LunarG/OpenXR-OverlayLayer/blob/master/README.md)).
  A client chains `XrSessionCreateInfoOverlayEXTX{ createFlags, sessionLayersPlacement }`
  into `xrCreateSession`; its layers are composited over the "main" application's
  at the z given by `sessionLayersPlacement`. Monado implements this natively in
  its out-of-process service via `comp_multi` (details below) —
  [Collabora announcement](https://www.collabora.com/news-and-blog/news-and-events/monado-multi-application-support-with-xr-extx-overlay.html).
- **OpenVR world (SteamVR):** overlays are a first-class, *separate* API —
  `IVROverlay` ([overview](https://github.com/ValveSoftware/openvr/wiki/IVROverlay_Overview)).
  Any external process creates overlay handles, pushes textures, sets transforms
  (absolute or tracked-device-relative), and receives laser-mouse input events.
  SteamVR's OpenXR runtime does **not** support `XR_EXTX_overlay`
  ([unanswered feature request since 2020](https://steamcommunity.com/app/250820/discussions/8/2448217320142811491/)).

### What the pinned Monado tree actually does (primary-source evidence)

All paths below are under `subprojects/monado` at `c2ddab59`.

**Extension is real, on by default.** `CMakeLists.txt:396`:
`option(XRT_FEATURE_OPENXR_OVERLAY "Enable XR_EXTX_overlay" ON)`, wired through
`src/xrt/state_trackers/oxr/extension_support/oxr_extension_support.py:115`
(`['XR_EXTX_overlay', 'XRT_FEATURE_OPENXR_OVERLAY']`). Requires the IPC service
(`XRT_FEATURE_SERVICE`, `CMakeLists.txt:291`) — in-process Monado cannot
multi-client. Our test orchestration already runs `monado-service`, i.e. service
mode.

**Session creation.** `src/xrt/state_trackers/oxr/oxr_session.c:1448-1454`: the
overlay struct is read from the `next` chain; `xsi.is_overlay = true`,
`xsi.z_order = overlay_info->sessionLayersPlacement`. No other behavioral flags.

**Layer composition and z-order.**
`src/xrt/compositor/multi/comp_multi_system.c`:
- `transfer_layers_locked()` (line 255) gathers every client's delivered frame,
  skips non-visible clients (line 284), sorts by `state.z_order`
  (`overlay_sort_func`, line 212), and replays **all layer types** — projection,
  quad, cylinder, equirect — bottom-to-top into the real compositor (line 319+).
  Our quads would simply be composited after (on top of) the game's projection
  layers. Max simultaneous clients: `MULTI_MAX_CLIENTS` = 64 sessions.
- Environment blend mode comes from the *focused, bottom-most* client
  (`find_active_blend_mode`, line 229) — the game's OPAQUE background wins; our
  own `XR_ENVIRONMENT_BLEND_MODE_OPAQUE` in `xrEndFrame`
  (`src/openxr/OpenXRManager.cpp:887`) is effectively ignored when we're an
  overlay, so quads blend over the game via their alpha as normal layers.

**Session state / input semantics — the key finding.**
`src/xrt/ipc/server/ipc_server_process.c:431-462`
(`handle_focused_client_events`): the *primary* (active) client gets
`visible=true, focused=true, z_order=INT64_MIN` (bottom). Then, unconditionally:

```c
// Set all overlays to always active and focused.
if (ics->client_state.session_overlay) {
    visible = true;
    focused = true;
    z_order = ics->client_state.z_order;
}
```

So on Monado, an overlay session is **permanently VISIBLE *and* FOCUSED**, even
while a game is the primary app and also focused. Both sessions' `xrSyncActions`
return live controller state simultaneously — Monado does *not* arbitrate input
between primary and overlays (this differs from the generic OpenXR expectation
that only one session is FOCUSED; the EXTX spec left input arbitration
explicitly unresolved, and Monado chose "everyone focused"). Consequences for us:

- Our input system's FOCUSED gating (`src/openxr/XRInput.cpp:264-278`,
  `XR_STATE_RUNNING_FOCUSED` in `src/openxr/OpenXRManager.cpp`) keeps working
  **unchanged** as an overlay.
- The flip side: the game *also* keeps receiving our "clicks" (trigger pulls)
  because input is duplicated, not routed. "Point at desktop without shooting"
  is OUR problem to solve (see Input arbitration below).

**Primary-app selection.** `ipc_server_process.c:482-540`
(`update_server_state_locked`): the "active" client is always a **non-overlay**
client; overlays can never become primary. If the primary exits, Monado falls
back to another non-overlay client or an idle wallpaper. Overlays are notified of
main-app comings/goings via `XrEventDataMainSessionVisibilityChangedEXTX`
(`handle_overlay_client_events`, lines 400-428; event push in
`src/xrt/state_trackers/oxr/oxr_event.c:247-268`).

**Runtime-side arbitration levers exist.** `libmonado`
(`src/xrt/targets/libmonado/monado.h`, `monado.c:348-370`) exposes
`mnd_root_set_client_primary()` / `mnd_root_set_client_focused()` and per-client
flags (`MND_CLIENT_PRIMARY_APP`, `MND_CLIENT_SESSION_FOCUSED`) — `monado-ctl`
uses these. If we ever want "mute the game's input while the desktop is grabbed",
driving libmonado from HypXRland is a (Monado-specific) option.

**Implemented vs stubbed:** the whole path above is implemented, not stubbed —
comp_multi replays all layer types, the IPC server tracks per-client state, and
Collabora demoed cross-process, cross-graphics-API composition with it. What's
absent: any input routing/arbitration, per-layer (vs per-session) z interleaving,
and any notion of overlay "capture input" requests.

## Option A — Run HypXRland as an `XR_EXTX_overlay` session on Monado/WiVRn

**What it enables.** The full vision on the FOSS stack: any OpenXR game (native
Linux OpenXR, Proton+OpenXR, or OpenVR-only via OpenComposite/xrizer) runs as
Monado's primary client; HypXRland connects as overlay client; our monitor quads
float over the game world, ray input and grab keep working. Works identically on
WiVRn for Quest-class headsets (WiVRn is Monado-based and inherits multi-client;
wlx-overlay-s runs on it natively —
[wlx-overlay-s README](https://github.com/galister/wlx-overlay-s/blob/main/README.md),
[WiVRn](https://github.com/WiVRn/WiVRn)).

**Architecture sketch.** Nothing structural changes. HypXRland remains an
independent OpenXR client process (the compositor); the game is another client;
Monado's service composites. Diagram delta vs `docs/openxr/00-overview.md`: none —
only the session-create call and lifecycle-state interpretation change.

**Required changes (specific):**
1. `src/openxr/XRSession.cpp` `createInstance()` (~line 43-79): probe/enable
   `"XR_EXTX_overlay"` (optional ext, availability flag `m_hasOverlay` alongside
   `m_hasLocalFloor` etc.).
2. `src/openxr/XRSession.cpp` `createSession()` (~line 116-128): when overlay
   mode is requested and available, chain
   `XrSessionCreateInfoOverlayEXTX{ .next = &binding, .createFlags = 0,
   .sessionLayersPlacement = <openxr:overlay_z> }` between `sessionInfo` and the
   EGL binding struct.
3. `src/config/values/ConfigValues.cpp`: new vars `openxr:overlay` (int: 0 =
   exclusive/current, 1 = overlay, maybe 2 = auto = overlay-if-available) and
   `openxr:overlay_z` (int, default e.g. 1000; `sessionLayersPlacement` is
   uint32, 0 = bottom).
4. `src/openxr/XRSession.cpp` `pollEvents()`: handle
   `XR_TYPE_EVENT_DATA_MAIN_SESSION_VISIBILITY_CHANGED_EXTX` → forward over
   channel [C] → new socket2 payload (e.g. `openxrmainapp visible|hidden`) in
   `src/openxr/XRIpc.cpp` so bars can show "game running". (Optional but cheap.)
5. Docs/state-machine note in `docs/openxr/00-overview.md`: on Monado, overlay
   sessions sit at FOCUSED permanently; idle-inhibit (`openxr:inhibit_idle`)
   therefore stays active whenever the service runs us — acceptable, maybe worth
   a note.
6. Tests: a WP12-style hyprtester case launching a second trivial OpenXR client
   as primary (e.g. `hello_xr` or a tiny null client) and asserting we still
   reach FOCUSED and submit; the existing Monado orchestration
   (`hyprtester/src/xr/MonadoOrchestrator.*`) already runs service mode.

Caveat on portability: `sessionLayersPlacement`'s spec wording says higher =
composited later (on top); Monado maps it straight into `z_order` and sorts
ascending, with the primary pinned to `INT64_MIN` (`ipc_server_process.c:434,444`)
— so any placement value puts us above the game there; other runtimes may differ.

**Runtime support matrix:** Monado local: YES (native). WiVRn+Quest: YES
(inherited; verify against WiVRn's pinned Monado). SteamVR-Linux: NO
(extension absent).

**Effort: S** (code); S-M with tests/docs. **Risks:** EXTX is provisional —
semantics could shift in future Monado versions (we pin, so controlled);
input duplication with the game (see Option D — the real work); performance —
comp_multi latches each client's frame and re-blits, an extra composition hop
whose cost on WiVRn (encode budget) is unmeasured; our EGL-fence contract
(95c541a8) was tuned against the null compositor — behavior under the *real*
compositor with a heavyweight primary client needs a soak test; teardown
"corrupted double-linked list" at exit (known open issue) may get noisier with a
second client.

## Option B — OpenVR `IVROverlay` backend for SteamVR-Linux

**What it enables.** Desktop monitors floating over a game running under real
SteamVR on Linux (Half-Life: Alyx et al. under Valve's runtime) — the one stack
Option A cannot touch.

**Architecture sketch.** A second, parallel "XR backend" implementing the same
role as `CXRSession`+frame loop but speaking OpenVR: one `IVROverlay` handle per
virtual monitor. Mapping is surprisingly direct:

| HypXRland concept | IVROverlay primitive |
|---|---|
| `CXRMonitorLayer` quad + swapchain blit | overlay handle + `SetOverlayTexture` (`TextureType_Vulkan`; GL works on Linux with interop caveats) or `SetOverlayRaw` (CPU) |
| world anchor (`docs/openxr/03-anchoring.md`) | `SetOverlayTransformAbsolute` |
| head/device anchors | `SetOverlayTransformTrackedDeviceRelative` (HMD or controller index) |
| body-leash anchor | app-side math (as today) + `SetOverlayTransformAbsolute` per frame |
| `CXRInput` ray + `CXRPointerDevice` | overlay laser-mouse: `VREvent_MouseMove/MouseButtonDown/Up`, `ScrollDiscrete`, `ComputeOverlayIntersection` — SteamVR does the ray-hit + focus arbitration FOR us |
| grab-to-move | app-side: poll controller poses via `IVRSystem`, recompute transform |
| session states | OpenVR has no VISIBLE/FOCUSED ladder; overlay visibility + dashboard events instead — `COpenXRManager` lifecycle needs a per-backend abstraction |

Note the input story is *better* than Option A's: SteamVR routes controller input
to overlays modally (laser mouse when pointing at an overlay / dashboard open),
so game-vs-desktop arbitration is handled by the runtime.

**Required changes:** an abstraction seam in `COpenXRManager` splitting
"frame producer/anchor solver/input" from "XR transport" — new
`src/openxr/backends/` with the current OpenXR path and an OpenVR path
(`openvr_api` dependency, new CMake option `WITH_OPENVR`); the blit pipeline
(`CXRGraphics`) must gain a Vulkan (or GL-with-interop) texture export instead
of GLES-swapchain images; the frame→main event queue gains OpenVR event types;
config gains `openxr:backend = openxr|openvr|auto`. Essentially every file in
`src/openxr/` is touched; `XRAnchor`/`XRMonitorConfig`/`XRIpc` survive mostly
intact.

**Runtime matrix:** Monado local: unnecessary (Option A better). WiVRn+Quest: NO.
SteamVR-Linux: YES — this is the only route there; proven by wlx-overlay-s
running as an external non-Steam process
([SteamVR-for-Linux](https://github.com/ValveSoftware/SteamVR-for-Linux) is
still a "development release" but actively patched, v2.16.1 2026-04).

**Effort: L-XL.** **Risks:** maintaining two runtime APIs forever; OpenVR is
proprietary-runtime-defined (header is open, runtime is not); SteamVR-Linux
fragility (GL `SetOverlayTexture` performance issues, NVIDIA quirks); our
GLES/EGL pipeline vs OpenVR's Vulkan preference forces new interop code — the
exact class of cross-driver pain we just spent WP-cycles taming.

## Option C — Game runs under Monado/WiVRn via OpenComposite/xrizer (Option A's reach extended to Steam games)

**What it enables.** "Steam VR game + HypXRland desktop" *without SteamVR*: run
the game's OpenVR API against
[OpenComposite](https://gitlab.com/znixian/OpenOVR) /
[xrizer](https://github.com/Supreeeme/xrizer) which translate to OpenXR, on
Monado/WiVRn; game becomes a normal primary client; HypXRland overlays via
Option A. Zero additional HypXRland code beyond Option A.

**Runtime matrix:** Monado local: YES. WiVRn+Quest: YES (this is the standard
FOSS "play Steam VR games on Quest via WiVRn" stack today). SteamVR: N/A by
construction.

**Effort: none beyond A** (documentation/recipe only). **Risks:** per-game
compatibility lottery (OpenComposite is mature-but-undertested, xrizer young);
anticheat/DRM edge cases; users must configure Proton env vars — a docs/support
burden, not an engineering one.

## Option D — Input arbitration layer (needed by A/C; the real design work)

Monado gives both clients live input simultaneously (evidence above). Without
mitigation, clicking a desktop window fires the game's trigger action too.
Approaches, combinable:

1. **Modal toggle owned by us (recommended baseline):** a "desktop mode" flag
   toggled by a Hyprland dispatcher/bind (existing `xrmonitor` dispatcher
   family, `src/openxr/XRIpc.cpp`) and/or a reserved XR gesture (e.g. double
   system-button, long-press menu — sampled in `CXRInput`). When OFF: suppress
   ray/pointer injection entirely (quads stay visible, maybe dimmed — layer
   alpha). When ON: inject input as today, and optionally *mute the game* via
   libmonado `mnd_root_set_client_focused()` (Monado-only, needs linking
   `libmonado` + talking to the service — S effort, best-effort).
   This mirrors the SteamVR-dashboard UX (modal desktop) and wlx-overlay-s
   ("show/hide" binding).
2. **Passive coexistence:** desktop interaction only via *pointing at a quad* —
   we already ray-test every frame (`CXRInput::processPointer`); only consume
   clicks when the ray hits one of our quads. Game still sees the trigger
   (duplicated) — acceptable for some games, terrible for shooters. Zero new
   mechanism; could be the day-1 default with (1) added next.
3. **Runtime-level routing (future/upstream):** propose per-client input focus
   upstream in Monado (the EXTX spec's acknowledged gap). Long-term, not for us
   to block on.

**Effort:** (2) is ~free; (1) is S-M. Fits entirely inside existing files:
`XRInput.cpp` (gesture/suppression), `XRIpc.cpp` (dispatcher + socket2 event),
`ConfigValues.cpp` (e.g. `openxr:input_mode`, toggle bind docs).

## Prior art (lessons)

**wlx-overlay-s** ([repo](https://github.com/galister/wlx-overlay-s)) — the
closest existing thing to this vision, and proof both target stacks work. Rust;
**dual backend**: OpenXR (Monado/WiVRn, via `XR_EXTX_overlay` — the predecessor
`wlx-overlay-x` was the OpenXR-only prototype) and OpenVR (`IVROverlay` for
SteamVR), auto-selected at startup. One overlay per captured screen plus
auxiliary overlays (keyboard, "watch"). Capture: PipeWire and wlr-screencopy on
Wayland, X11 fallback (no zero-copy there). Input: color-coded laser per action
(left/right/middle click via different controller modes), a wrist-"watch" panel
as the control hub, and a **show/hide toggle bind (double-tap B/Y)** as the
game-vs-desktop arbitration — i.e. exactly Option D-1's modal model, implemented
app-side because Monado duplicates input. Lesson: users accept a modal toggle;
per-panel laser interaction only while overlays are shown.

**WayVR** ([archived repo](https://github.com/olekolek1000/wayvr), merged into
wlx-overlay-s Oct 2024) — embeds a **Smithay-based Wayland compositor** so real
Wayland apps render *directly* into VR panels (dma-buf zero-copy, XWayland via
cage) instead of capturing an existing desktop. Lesson: "be the compositor" is
the architecturally superior end of this design space — and HypXRland is already
there natively (our quads come straight from Hyprland's own outputs, no capture,
no second compositor). WayVR is validation, not competition: it had to bolt a
compositor *into* an overlay app; we're bolting overlay *onto* a compositor.

**OpenKneeboard** ([internals](https://openkneeboard.com/internals/README/),
[third-party docs](https://openkneeboard.com/faq/third-party-developers/)) —
Windows sim-racing/flight overlay. Radically different mechanism: an **OpenXR
API layer** inside the game's process that appends a quad layer to the *game's
own* `xrEndFrame`, fed via shared-memory textures from the main app. Works on
runtimes with no multi-client support at all (it needs none). Lessons: (a) an
API layer is the escape hatch when the runtime won't composite for you — a
Linux equivalent (`XR_API_LAYER_PATH` layer appending our quads inside the game
process) is *possible* but fragile and per-game; listed for completeness,
rejected; (b) layer *registration order* determines z-stacking among such tools
— invisible-to-user ordering is a UX trap Option A avoids by having explicit
`sessionLayersPlacement`.

**SteamVR dashboard / Desktop+**
([DesktopPlus](https://github.com/elvissteinjr/DesktopPlus),
[OVR Advanced Settings](https://github.com/OpenVR-Advanced-Settings/OpenVR-AdvancedSettings)) —
the canonical modal model: system button summons the dashboard, the game
**loses input focus while it's up** (games receive `input_focus` lost events),
keyboard input goes to the last-clicked ("focused") overlay, two-handed
gesture-drag repositions/scales panels. Desktop+ is a mature per-window Windows
equivalent of our per-monitor quads, built entirely on `IVROverlay`. Lessons:
runtime-arbitrated modality (game demonstrably unfocused) is the most legible
UX; last-clicked-panel keyboard focus maps directly onto Hyprland's existing
focus model; gesture-drag ≈ our grab machine — concepts transfer 1:1 to an
OpenVR backend (Option B).

**Immersed / Virtual Desktop** ([immersed.com](https://immersed.com/)) —
proprietary productivity apps: a desktop *agent* captures/encodes screens and
streams to a standalone headset app that renders panels in its own world. They
are **exclusive** VR apps, not overlays — you live in *their* environment, no
game underneath. Lessons: (a) the market validated multi-virtual-monitor VR
productivity (Immersed: 5 virtual displays, resize/curve/place); (b) their
Linux virtual-monitor support is weak-to-absent — a real gap HypXRland fills
natively; (c) exclusive-mode ergonomics (curved screens, environment theming)
matter to users and stay relevant to our existing non-overlay mode.

## Recommendation

Prototype **Option A now** (with Option D-2 semantics as the default and D-1 as
the follow-up), document Option C as the supported "Steam games" recipe, and
defer Option B unless/until SteamVR-under-Valve's-runtime becomes a hard user
requirement.

**De-risking spike (1-2 days, all on existing infra):**
1. Hand-patch `XRSession.cpp` to chain `XrSessionCreateInfoOverlayEXTX`
   (hardcoded placement) and enable the extension.
2. Launch monado-service (normal compositor or null), start any primary client
   (`hello_xr` -G Vulkan is fine; on this box beware the known cross-GPU EGL
   constraints — `openxr:gpu` matched to Monado's device), then start HypXRland.
3. Verify: we reach FOCUSED while the primary runs (per
   `ipc_server_process.c:447-452` we must); quads composite above the primary's
   scene; ray input still drives Hyprland; watch for frame-pacing regressions
   (comp_multi latching) and the exit-time heap corruption getting worse.
4. If the null compositor can't visually confirm stacking, repeat under
   `scripts/preview-xr.sh` (windowed Monado) and eyeball z-order.
5. Measure: added latency of the extra composition hop (Monado tracing), and
   whether `scheduleFrame` pacing stays sane when the primary owns the frame
   clock.

Success criteria: FOCUSED-while-primary-runs confirmed, quads visibly over the
primary's layers, no new teardown crashes. That validates the entire Option A
stack; the remainder is config plumbing + Option D UX.

## Open questions for the user

1. Is SteamVR-proper (Valve runtime) a must-have target, or is
   Monado/WiVRn + OpenComposite acceptable as *the* supported game path? (This
   solely decides whether Option B ever happens.)
2. Desired input UX: modal "desktop mode" bind (D-1), passive point-at-quad
   (D-2), or both? Which physical gesture/bind should toggle?
3. Should HypXRland *itself* stay capable of exclusive mode (current opaque
   background) with overlay as opt-in (`openxr:overlay=auto`?), or become
   overlay-by-default when the extension exists?
4. Do we want the libmonado dependency for game-input muting (Monado-only,
   tighter coupling to one runtime), or stay pure-OpenXR?
5. WiVRn validation: is Quest-over-WiVRn in scope for the spike, or is local
   Monado enough initially?

## Source index

Monado (vendored, `subprojects/monado` @ `c2ddab59`):
- `CMakeLists.txt:291,396` — service + `XRT_FEATURE_OPENXR_OVERLAY` (default ON)
- `src/xrt/state_trackers/oxr/extension_support/oxr_extension_support.py:115`
- `src/xrt/state_trackers/oxr/oxr_session.c:1448-1454` — overlay create-info handling
- `src/xrt/compositor/multi/comp_multi_system.c:212-330` — z-sort, blend-mode pick, layer replay; `:595-641` — set_state/set_z_order/main-app-visibility
- `src/xrt/ipc/server/ipc_server_process.c:400-462` — overlays always visible+focused; `:482-540` — primary-client selection (non-overlay only)
- `src/xrt/state_trackers/oxr/oxr_event.c:247-268` — MainSessionVisibilityChanged event
- `src/xrt/targets/libmonado/monado.h`, `monado.c:330-370` — client flags, set-primary/set-focused

HypXRland: `src/openxr/XRSession.cpp:43-138` (instance/session create),
`src/openxr/XRInput.cpp:241-278` (xrSyncActions/FOCUSED), 
`src/openxr/OpenXRManager.cpp:884-895` (xrEndFrame/blend mode),
`docs/openxr/00-overview.md`, `01-session-graphics.md`, `03-anchoring.md`, `04-input.md`.

Web:
- XR_EXTX_overlay registry: https://registry.khronos.org/OpenXR/specs/1.0/man/html/XR_EXTX_overlay.html
- LunarG reference overlay layer (status, open input-focus issue): https://github.com/LunarG/OpenXR-OverlayLayer/blob/master/README.md
- Collabora Monado multi-app demo: https://www.collabora.com/news-and-blog/news-and-events/monado-multi-application-support-with-xr-extx-overlay.html
- Monado service-mode requirement: https://monado.freedesktop.org/getting-started.html
- SteamVR: no XR_EXTX_overlay (open request since 2020): https://steamcommunity.com/app/250820/discussions/8/2448217320142811491/
- IVROverlay overview / SetOverlayTexture / Vulkan notes: https://github.com/ValveSoftware/openvr/wiki/IVROverlay_Overview , https://github.com/ValveSoftware/openvr/wiki/IVROverlay::SetOverlayTexture , https://github.com/ValveSoftware/openvr/wiki/Vulkan
- SteamVR-Linux status: https://github.com/ValveSoftware/SteamVR-for-Linux , https://www.gamingonlinux.com/2026/04/steamvr-beta-brings-a-number-of-fixes-for-linux-gamers/
- WiVRn: https://github.com/WiVRn/WiVRn , https://wivrn.github.io/
- wlx-overlay-s: https://github.com/galister/wlx-overlay-s
- OpenComposite / xrizer: https://gitlab.com/znixian/OpenOVR , https://github.com/Supreeeme/xrizer
- WayVR (Smithay compositor-in-VR, merged into wlx-overlay-s): https://github.com/olekolek1000/wayvr
- OpenKneeboard internals (OpenXR API-layer approach): https://openkneeboard.com/internals/README/ , https://openkneeboard.com/faq/third-party-developers/
- Desktop+ (SteamVR IVROverlay desktop, input-focus model): https://github.com/elvissteinjr/DesktopPlus
- OVR Advanced Settings (dashboard overlay prior art): https://github.com/OpenVR-Advanced-Settings/OpenVR-AdvancedSettings
- Immersed: https://immersed.com/
- OpenXR session states / xrSyncActions: https://registry.khronos.org/OpenXR/specs/1.1/man/html/XrSessionState.html , https://registry.khronos.org/OpenXR/specs/1.1/man/html/xrSyncActions.html
