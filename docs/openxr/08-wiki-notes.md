# 08 — Wiki notes (ready-to-paste)

Part of the HypXRland design doc set (`docs/openxr/`). Hyprland's user-facing documentation
lives on an external wiki (wiki.hypr.land), not in this repository — there is no in-tree wiki
source to edit. This doc is a **ready-to-paste reference** for whoever adds the OpenXR extension
to that wiki: config vars, the `xrmonitor` keyword grammar, dispatcher verbs, `hyprctl openxr`
usage, events for status-bar integration, and a hypridle recipe. Content is authoritative as of
WP13 (final polish pass) — cross-check against `docs/openxr/00-06.md` and the source if the
implementation moves again after this.

`docs/hyprctl.1.rst`/`.1` (the man page source/generated page) got a short `openxr` entry in the
INFO COMMANDS section pointing back at this file for the full reference — see that file for the
one-paragraph man-page version.

---

## 1. Enabling OpenXR

Requires Hyprland built with OpenXR support (`cmake` reports `OpenXR support enabled (...)`; a
build without the `openxr` package skips it silently — `hyprctl openxr` then returns hyprctl's
stock unknown-request error and the `xrmonitor` dispatcher returns a clean
"Hyprland was built without OpenXR support" error, no crash).

```ini
openxr {
    enabled = true
}
```

This is a **hot** toggle — `hyprctl keyword openxr:enabled 1` (or `0`) starts/stops the session
immediately, no restart needed, same as flipping it in the config and reloading. The session only
actually comes up if an OpenXR runtime is available (e.g. an OpenXR-compatible headset + its
runtime, or a runtime like Monado for testing) — otherwise the state settles at `unavailable`,
not an error.

**`hyprctl openxr enable`/`disable` vs `openxr:enabled` (the subtlety to call out on the wiki):**
`hyprctl openxr enable`/`disable` start/stop the session *without* touching the config value. A
subsequent config reload re-applies whatever `openxr:enabled` says in the file — so a manual
`hyprctl openxr disable` doesn't "stick" across a reload if the config still says `enabled = true`.
To persist a change, edit the config (or use `hyprctl keyword openxr:enabled ...`, which does the
same thing live).

---

## 2. Config variables (`openxr:` section)

| Variable | Type | Default | Meaning |
|---|---|---|---|
| `openxr:enabled` | bool | `false` | Master switch, hot toggle (see above). |
| `openxr:gpu` | string | `""` | DRM render node for the XR EGL context (e.g. `/dev/dri/renderD128`). Empty = follow Hyprland's primary GPU. Set explicitly on hybrid/multi-GPU machines where the OpenXR runtime uses a different GPU than Hyprland — a mismatch can crash the runtime at swapchain creation. Read at session start only. |
| `openxr:force_linear` | string | `"auto"` | Allocate LINEAR (multi-GPU-shareable) buffers for XR monitors so the runtime's GPU can import them: `auto` (only when a cross-GPU split is detected) / `on` / `off`. Needed together with `openxr:gpu` on hybrid machines where the runtime GPU ≠ the desktop's render GPU — without it the runtime's EGL rejects the compositor's native-tiled buffers and the monitor renders black. Linear is slower to composite (the accepted multi-GPU cost). Applied at monitor bind. |
| `openxr:blend_mode` | string | `"auto"` | Environment blend mode: `auto` \| `opaque` \| `alpha` \| `additive`. `auto` uses the runtime's preferred mode. `alpha` enables **passthrough** on runtimes that support it (e.g. WiVRn on Quest 3) — monitors composite over your real room instead of a black void. An explicit mode the runtime doesn't advertise falls back to the preferred one with a warning. Read at session start only (`hyprctl openxr disable && hyprctl openxr enable` to re-apply). |
| `openxr:floor_offset` | float (m) | `1.5` | Fallback eye height when the runtime lacks `XR_EXT_local_floor`. |
| `openxr:default_size` | float (m) | `1.6` | Default quad width for a new XR monitor when not given explicitly. |
| `openxr:default_distance` | float (m) | `1.5` | Default placement distance for a new/re-centered XR monitor. |
| `openxr:leash_response` | float (s) | `0.35` | Head/body leash spring response time — smaller is snappier. |
| `openxr:leash_deadzone_angle` | float (deg) | `15.0` | Head/body leash angular deadzone. |
| `openxr:leash_deadzone_distance` | float (m) | `0.25` | Head/body leash positional deadzone. |
| `openxr:body_leash_follow_height` | bool | `false` | Body-leashed monitors also follow vertical head movement. |
| `openxr:pointer` | bool | `true` | Enable the synthetic XR ray-pointer device. Hot — the device is created/destroyed live. |
| `openxr:pointer_trigger_threshold` | float | `0.7` | Trigger value that presses the pointer button. |
| `openxr:pointer_trigger_threshold_release` | float | `0.4` | Trigger value that releases it (hysteresis). |
| `openxr:grab_threshold` | float | `0.7` | Squeeze value that starts a grab. |
| `openxr:grab_threshold_release` | float | `0.4` | Squeeze value that ends a grab (hysteresis). |
| `openxr:scroll_speed` | float | `1.0` | Thumbstick scroll multiplier on XR monitors. |
| `openxr:inhibit_idle` | bool | `true` | Inhibit hypridle/idle while the session is `focused`. |
| `openxr:monitors_follow_session` | string | `visible` | when XR monitors behave like **unplugged external monitors** (disabled — workspaces evacuate, then return by name like replugging a display). `off` = never (always-present); `session` = while no session exists; `visible` (default) = while the session is not VISIBLE/FOCUSED, so a **doffed/standby headset reads as unplugged** even though WiVRn keeps a session alive. Legacy `false/0`→off, `true/1`→session. |
| `openxr:monitor_unplug_grace_ms` | int | `20000` | with `monitors_follow_session = visible`, ms the headset must stay doffed before its monitors unplug (anti-flap for a quick doff-and-don). Donning re-plugs immediately. |
| `openxr:destroy_monitors_on_stop` | bool | `false` | Destroy XR-created monitors when the session stops instead of keeping them (unplugged / plain headless per `monitors_follow_session`). Declared `xrmonitor`s re-materialize on the next session start. |
| `openxr:overlay` | bool | `false` | Run as an `XR_EXTX_overlay` session so your monitors composite **on top of another VR app** (a game, or `hypxrpaper`) instead of owning the view. Needs a runtime that supports `XR_EXTX_overlay` — Monado and WiVRn do; **SteamVR-Linux does not** (the session is created as a normal exclusive one, with a warning). Read at session start only (`hyprctl openxr disable && hyprctl openxr enable` to re-apply). See §8. |
| `openxr:overlay_z` | int | `1` | Overlay stacking order (`sessionLayersPlacement`). Higher composites later / on top; the primary app is always beneath. Only meaningful with `openxr:overlay = 1`. Read at session start only. |

Full descriptions (including min/max clamps): `hyprctl descriptions | grep openxr`.

A ready-to-copy example config with every var + several `xrmonitor=` declarations + example
binds lives at `example/openxr.conf` in the source tree.

---

## 3. The `xrmonitor` config keyword

Classic-hyprlang only (v1) — declares that a virtual monitor should exist and where it lives in
3D space. Ordinary `monitor =` rules still apply to it by name (scale, VRR, bit depth, mirror,
...); `xrmonitor` only controls existence + placement.

```
xrmonitor = <name>, <WxH>[@Hz], <anchor-spec>[, size:<meters>]
```

Anchor-spec (space-separated `key:value` sub-tokens; coordinates are meters, OpenXR convention:
+X right, +Y up, −Z forward):

| Anchor mode | Spec | Meaning |
|---|---|---|
| `anchor:local pos:x,y,z [yaw:deg] [pitch:deg]` | Fixed in the room (`LOCAL_FLOOR` space). `pos` is required. | A monitor bolted in place — doesn't move with you. |
| `anchor:head offset:x,y,z` | Offset in view space, no yaw/pitch (orientation is always look-at-driven). | A HUD that follows your gaze with a deadzone + spring. |
| `anchor:body offset:x,y,z` | Offset in the yaw-only body frame. | Turns with you but not with head pitch; stays at a fixed height unless `body_leash_follow_height`. |
| `anchor:device:left\|right offset:x,y,z [yaw:deg] [pitch:deg]` | Offset in the named controller's grip frame. | Rigidly locked to a controller — a palette/toolbar you hold up. |

Four working examples (these round-trip through the parser and its unit tests):

```ini
xrmonitor = XR-code, 2560x1440@90, anchor:local pos:0,1.4,-1.5 yaw:0, size:1.8
xrmonitor = XR-chat, 1280x720, anchor:head offset:0.4,-0.2,-1.0, size:0.6
xrmonitor = XR-music, 1920x1080@60, anchor:body offset:-0.8,1.2,-1.2, size:0.9
xrmonitor = XR-palette, 800x800, anchor:device:left offset:0,0.08,-0.05, size:0.25
```

**Reload behavior:** adding/removing/changing an `xrmonitor` line and reloading creates/destroys/
re-anchors exactly the diff; unchanged lines don't flicker. Monitors created at runtime (via the
dispatcher or `hyprctl openxr create`) are never touched by reconciliation — removing a config
line only affects monitors that came from a config line in the first place.

### Mirroring an XR monitor onto a physical screen

No XR-specific mechanism needed — it's the ordinary mirror recipe:

```ini
monitor = eDP-1, preferred, auto, 1, mirror, XR-code
```

Anyone at the physical screen sees exactly what's on the XR monitor.

---

## 4. The `xrmonitor` dispatcher (keybinds)

`bind = MODS, KEY, xrmonitor, <verb> [args...]` (also callable as
`hyprctl dispatch xrmonitor <verb> [args...]`, and via `hyprctl openxr <verb> [args...]` — three
transports, one implementation).

| Verb | Args | Effect |
|---|---|---|
| `create` | `<name> [WxH[@Hz]] [anchor-spec]` | Create a runtime-owned XR monitor. Mode defaults 1920x1080@60; anchor defaults to `anchor:local` placed at `openxr:default_distance` facing you. |
| `destroy` | `<name\|active>` | Destroy the named monitor, or the currently-selected one. |
| `select` | `<name\|next\|prev>` | Set the explicit selection target for the verbs below. |
| `anchor` | `<name\|active> <mode-spec>` | Re-anchor without moving the quad visually: `local`, `head [offset:x,y,z]`, `body [offset:x,y,z]`, `device:left\|right [offset:x,y,z]`. |
| `move` | `<dx> <dy> <dz>` | Translate in the anchor's own frame, meters. |
| `rotate` | `<dyaw> [dpitch]` | Rotate, degrees (pitch clamped ±85°). |
| `scale` | `<f\|+d\|-d>` | Bare number multiplies width; signed number adds/subtracts meters. Clamped 0.2–4.0 m. |
| `distance` | `<±m>` | Push/pull along the view ray. Clamped 0.3–5.0 m. |
| `center` | *(none)* | Re-place centered in view at `openxr:default_distance`. |

"Selected" target resolution (for `active`/omitted target): explicit `select` > last ray-hovered
monitor > focused-if-XR monitor, else the verb errors with "no XR monitor selected".

Example binds:

```ini
bind = SUPER, X, xrmonitor, create XR-scratch
bind = SUPER SHIFT, X, xrmonitor, destroy active
bind = SUPER, bracketright, xrmonitor, select next
bind = SUPER, bracketleft, xrmonitor, select prev
bind = SUPER, Home, xrmonitor, center
binde = SUPER, equal, xrmonitor, distance -0.25
binde = SUPER, minus, xrmonitor, distance +0.25
bind = SUPER, H, xrmonitor, anchor active head
bind = SUPER, L, xrmonitor, anchor active local
```

---

## 5. `hyprctl openxr`

```
hyprctl openxr [status]        # default subcommand
hyprctl -j openxr              # JSON
hyprctl openxr enable|disable  # start/stop the session (does not touch openxr:enabled)
hyprctl openxr create|destroy|select|anchor|move|rotate|scale|distance|center ...   # same verbs as §4
hyprctl openxr layout          # dump the CURRENT live layout as paste-ready xrmonitor= lines
```

`hyprctl openxr layout` walks every live XR monitor (declared and runtime-created) and prints
lines you can paste straight into your config to reproduce the arrangement — including a monitor
that's been moved around at runtime with the dispatcher or a controller grab. `anchor:local`
lines reflect the live pose (so a grabbed-and-released monitor's new position is captured
correctly); `head`/`body`/`device` lines reflect the configured offset for that mode (the thing
that's actually meaningful to persist for a leashed monitor — its *live* position continuously
tracks your head/body/controller by design, so freezing that into a config line wouldn't mean
what you'd expect).

`hyprctl -j openxr` JSON shape:

```json
{
    "state": "focused",
    "runtimeName": "Monado(XRT) by Collabora et al.",
    "systemName": "Simulated HMD",
    "inhibitingIdle": true,
    "monitors": [
        {
            "name": "XR-code",
            "id": 3,
            "size_m": 1.80,
            "anchor": { "mode": "local", "pose": { "pos": [0.0, 1.4, -1.5], "quat": [0.0, 0.0, 0.0, 1.0] } },
            "grabbed": false,
            "hovered": true
        }
    ]
}
```

- `state`: `disabled` | `unavailable` | `starting` | `idle` | `visible` | `focused` | `stopping`.
- `inhibitingIdle`: whether the XR module currently has the idle-inhibit bit raised (mirrors
  `openxr:inhibit_idle && state == focused`).
- `anchor.pose`: for `local`, the world pose; for `head`/`body`/`device`, the configured offset —
  **except while `grabbed` is `true`, when it's always the live world pose regardless of mode.**
- `quat` order is `[x, y, z, w]`.

---

## 6. Events (for bars / scripts)

Posted on the compositor's socket2 (`.socket2.sock`), same mechanism as every other Hyprland
event (`EVENT>>DATA`):

| Event | Payload | Meaning |
|---|---|---|
| `openxrsessionstate` | one of `disabled`/`unavailable`/`starting`/`idle`/`visible`/`focused`/`stopping` | Every lifecycle transition. |
| `openxractive` | `1` or `0` | Derived: active ⇔ state ∈ {`visible`, `focused`}. Posted only on flip — "is someone in the headset" for a bar icon. |
| `xrmonitoradded` | `<name>` | An XR monitor finished creation (any origin). |
| `xrmonitorremoved` | `<name>` | An XR monitor was destroyed (any origin). |
| `xrmonitoranchor` | `<name>,<mode>` | The anchor **mode** of a monitor changed — the `anchor` verb, or a config reload that changed a declared anchor mode. **Not** fired by a plain grab/release where the mode stays the same. |
| `xrmonitorgrab` | `<name>,1` / `<name>,0` | A grab began / ended on a monitor. |
| `xrmonitorquad` | `<name>,1` / `<name>,0` | A monitor's quad was reactivated/suspended under the 16-layer runtime cap. |

### Bar module recipe (polling)

```jsonc
// waybar
"custom/openxr": {
    "exec": "hyprctl -j openxr | jq -r '\"\\(.state) \\(.monitors | length)\"'",
    "interval": 5,
    "format": "XR: {}",
    "exec-if": "hyprctl -j openxr | jq -e '.state != \"disabled\"'"
}
```

### Event-driven recipe

```sh
socat -U - "UNIX-CONNECT:$XDG_RUNTIME_DIR/hypr/$HYPRLAND_INSTANCE_SIGNATURE/.socket2.sock" \
  | grep --line-buffered -E '^(openxrsessionstate|openxractive|xrmonitor)' \
  | while IFS='>>' read -r ev _ data; do
        echo "XR event: $ev = $data"   # notify-send, waybar signal, etc.
    done
```

---

## 7. hypridle recipe

Nothing to configure for the default behavior: `openxr:inhibit_idle = true` (default) raises the
compositor's normal idle-inhibit bit while the session is `focused`, and hypridle already obeys
inhibitors by default via `ext-idle-notify-v1` — screens simply don't blank / lock while you're
in the headset, no hypridle.conf changes needed.

Want the opposite, or extra behavior? Script off the events instead:

```sh
# Lock the desktop the moment the headset goes on
socat -U - "UNIX-CONNECT:$XDG_RUNTIME_DIR/hypr/$HYPRLAND_INSTANCE_SIGNATURE/.socket2.sock" \
  | grep --line-buffered '^openxractive>>1' \
  | while read -r _; do loginctl lock-session; done
```

```sh
# Blank physical outputs while in XR, restore on exit (openxr:inhibit_idle still keeps
# hypridle quiet in the meantime)
... openxractive>>1 -> hyprctl dispatch dpms off
... openxractive>>0 -> hyprctl dispatch dpms on
```

---

## 8. Compose over another VR app / hypxrpaper (overlay mode)

By default HypXRland owns the headset view — your monitors float in a black void (or over
passthrough with `blend_mode = alpha`). With `openxr:overlay = 1` it instead runs as an
**`XR_EXTX_overlay`** session and its monitors composite **on top of another OpenXR application**:
a VR game, or `hypxrpaper` for an ambient sky/room backdrop. Requires a runtime that supports the
extension — **Monado** and **WiVRn** do; **SteamVR-Linux does not** (requesting overlay there just
logs a warning and runs exclusive as usual).

```ini
openxr {
    enabled  = true
    overlay  = true    # composite over whatever else is running
    overlay_z = 1       # stacking order; higher = on top (primary app is always beneath)
    # gpu = /dev/dri/renderD128   # pin to the runtime's GPU on multi-GPU boxes
}
```

### hypxrpaper — the ambient-background app

`hypxrpaper` is a tiny standalone OpenXR client that draws an ambient backdrop and nothing else,
so HypXRland's overlay has something to float over. It is a **separate project** (build it from
its own repo — see its `README.md`; `cmake -B build && cmake --build build` produces `hypxrpaper`)
and, like any OpenXR app, it needs `XR_RUNTIME_JSON` pointed at your runtime's manifest.

```
hypxrpaper [panorama.{hdr,png,jpg}]   # equirect panorama; omit for a built-in gradient sky
hypxrpaper --scene forest-clearing    # a bundled 3D scene (stereo projection layer)
hypxrpaper --scene path/to/model.glb  # any .glb/.gltf, scene.json, or bundled scene name
hypxrpaper --gpu /dev/dri/renderD128  # pin to the runtime's GPU on multi-GPU boxes
```

Recipe:

1. Start the runtime service first (Monado: `monado-service`; WiVRn: its server).
2. Start the **primary** app — the thing you want underneath. For an ambient backdrop that's
   `hypxrpaper` (`XR_RUNTIME_JSON=<runtime manifest> hypxrpaper` — no args = gradient sky;
   `--scene forest-clearing` for the bundled 3D scene; a panorama image path for a photo sky). For
   a game, launch it normally against the same runtime (Steam VR titles via OpenComposite/xrizer on
   Monado/WiVRn).
3. Enable HypXRland (`hyprctl openxr enable`, or start with `openxr:enabled = 1`). Your monitors
   appear over the primary app's scene.

### Autostart it from your Hyprland config

Let Hyprland launch the backdrop for you with `exec-once`, alongside `openxr:overlay = 1`:

```ini
openxr {
    enabled = true
    overlay = true          # composite our monitors over hypxrpaper
}

# 3D scene backdrop:
exec-once = hypxrpaper --scene forest-clearing
# ...or a panorama image instead:
# exec-once = hypxrpaper ~/pictures/panoramas/sunset.hdr
# ...or the built-in gradient sky (no args):
# exec-once = hypxrpaper
```

`hypxrpaper` inherits Hyprland's environment, so as long as your session has `XR_RUNTIME_JSON` set
(most OpenXR setups do) it just works; otherwise prefix it, e.g.
`exec-once = env XR_RUNTIME_JSON=/path/to/runtime.json hypxrpaper --scene forest-clearing`.

Notes:

- On Monado an overlay session is held **visible + focused** the whole time the service runs — so
  ray input, grab, and idle-inhibit behave exactly as in exclusive mode. It reaches `focused` even
  with no primary app running (you'll just see monitors over black until one starts).
- `overlay` is reported by `hyprctl openxr status` (`"overlay": true` in JSON, `overlay: yes` in
  text) — reflecting the real session type, so you can confirm the runtime accepted it.
- **Input is not arbitrated between the two apps.** Monado delivers controller input to the game
  *and* to HypXRland simultaneously, so a trigger pull that clicks a desktop window also reaches
  the game. A modal "desktop mode" toggle is future work (see
  `research/01-vr-app-composition.md`).
- Changing `overlay`/`overlay_z` takes effect on the next session start
  (`hyprctl openxr disable && hyprctl openxr enable`).

---

## Known v1 limitations worth calling out on the wiki

- **Classic-config only.** `openxr { }` and `xrmonitor =` are classic-hyprlang; there is no Lua
  config binding for OpenXR as of v1.
- **`khr/simple_controller` (no analog grab) cannot grab a monitor.** The long-press-select grab
  emulation planned for that profile was not implemented in v1 — select-click still works
  normally, only grab-to-move is unavailable on that profile.
- **Roll is not representable** in `xrmonitor=`/`hyprctl openxr layout` pose serialization — a
  grabbed-and-released `local`/`device` monitor that picked up roll during the grab loses it when
  re-serialized. Not a bug, a documented v1 grammar limitation (yaw + pitch only).
- **No stereo/3D rendering.** Every XR monitor is a flat quad; there is no per-eye content.
