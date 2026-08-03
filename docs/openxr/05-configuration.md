# 05 — Configuration & Control Reference

The complete user-facing control surface of HypXRland: the `openxr:*` config section, the
`xrmonitor` and `xrrule` keywords, the `xrmonitor` dispatcher, the `hyprctl openxr` command, the
socket2 events, idle integration, and consumer recipes (waybar, hypridle, shell). A ready-to-copy
config with every variable and example binds lives at `example/openxr.conf`; run
`hyprctl descriptions | grep openxr` for the live descriptions including min/max clamps.

`openxr { }`, `xrmonitor =` and `xrrule =` are classic-hyprlang config keywords; there is no Lua
config binding for them.

---

## 1. Enabling OpenXR

Requires Hyprland built with OpenXR support (`cmake` reports `OpenXR support enabled (...)`).
On a build without the `openxr` package the feature compiles away: `hyprctl openxr` returns
hyprctl's stock unknown-request error and the `xrmonitor` dispatcher returns a clean
"Hyprland was built without OpenXR support" error — user configs parse identically either way.

```ini
openxr {
    enabled = true
}
```

`openxr:enabled` is a **hot** toggle — `hyprctl keyword openxr:enabled 1` (or `0`), or editing
the config and reloading, starts/stops the session live. The session only actually comes up
when an OpenXR runtime is available (a headset + its runtime, or Monado for testing); otherwise
the state settles at `unavailable`, not an error, and — with `openxr:reprobe` (default on) — a
backoff timer brings the session up automatically once the runtime starts or the headset is
donned.

**`hyprctl openxr enable`/`disable` vs `openxr:enabled`.** The `enable`/`disable` subcommands
start/stop the session **without** touching the config value, so a subsequent config reload
re-applies whatever `openxr:enabled` says in the file. To persist a change, edit the config or
use `hyprctl keyword openxr:enabled ...` (which does the same thing live).

---

## 2. Config variables (`openxr:` section)

Every variable, grouped by area. The "applies" column notes when a change takes effect:
**hot** = live (on reload or `hyprctl keyword`, including values re-read every frame);
**start** = read once at session start, so `hyprctl openxr disable && hyprctl openxr enable`
(or toggling `openxr:enabled`) re-applies it.

### Session & runtime

| Variable | Type | Default | Meaning | Applies |
|---|---|---|---|---|
| `enabled` | bool | `false` | Master switch (§1). | hot |
| `reprobe` | bool | `true` | While enabled but no runtime/headset is available yet, keep re-probing in the background so the session comes up automatically once the runtime starts or the headset is donned. Also drives auto-reconnect after a session/runtime loss. | hot |
| `reprobe_interval_ms` | int | `2000` | Base interval for the reprobe backoff. "Waiting for the runtime" grows the delay from this base up to 30s; "waiting for the headset" (runtime up, not donned) polls at this fixed cadence. | hot |
| `reprobe_watch` | bool | `true` | While dormant in `unavailable`, also **inotify-watch** `$XDG_RUNTIME_DIR` for the runtime materializing and probe **immediately** when it does — instead of waiting out the backoff. Triggers: the IPC socket appearing (`monado_comp_ipc`, `wivrn/comp_ipc`) **and the pid file being created or rewritten** (`monado.pid`, `wivrn.pid`) — the latter is WiVRn's only don-time filesystem signal (its socket is pre-created at service start and inherited by the compositor server forked at headset-connect). A trigger also resets the backoff to the base interval, and any relevant watched-dir activity keeps it capped at base for the next 60s (the runtime is materializing — poll hard). The `reprobe`/`reprobe_interval_ms` timer stays as the fallback (inotify can miss across mount namespaces, and needs `$XDG_RUNTIME_DIR` set); each timer probe also stats the trigger paths and logs once per dormant period if one exists that the watch never saw (silent-miss guard). | hot (next reprobe cycle) |
| `gpu` | string | `""` | DRM render node for the XR EGL context (e.g. `/dev/dri/renderD128`). Empty = follow Hyprland's primary GPU. Set it on hybrid/multi-GPU machines where the runtime composites on a different GPU than Hyprland — a detected mismatch is refused at startup rather than crashing the graphics driver (see the session/graphics doc). | start |
| `runtime_json` | string | `""` | Path to the OpenXR runtime manifest (`openxr_*.json`) the session should handshake against, overriding `XR_RUNTIME_JSON` / `active_runtime.json` **for this compositor's XR session only**. Empty = leave the login environment untouched (loader default). Read on the **main thread** at each session start and applied to the loader's `XR_RUNTIME_JSON` before the handshake; clearing it back to empty restores the runtime the process launched with, so it round-trips cleanly. This is how the XREAL flat↔XR toggle (`scripts/xreal-mode.sh`) selects the xreal-flavor Monado without disturbing a WiVRn `active_runtime`. Set live with `hyprctl keyword openxr:runtime_json <path>` then `hyprctl openxr disable && hyprctl openxr enable` to re-handshake. Surfaced in `status` as `runtime json:`. See the XREAL rig doc (07). | start |
| `force_linear` | string | `auto` | Allocate LINEAR (cross-GPU-importable) buffers for XR monitors: `auto` (only when a cross-GPU split is detected) / `on` / `off`. Needed together with `gpu` when the runtime GPU differs from the desktop's render GPU. Linear costs some compositing throughput. | start (applied at monitor bind) |
| `blend_mode` | string | `auto` | Environment blend mode: `auto` (runtime's preferred) / `opaque` (monitors over a black void) / `alpha` (**passthrough** — monitors over your real room, on runtimes that support it, e.g. WiVRn on Quest 3) / `additive` (optical see-through). An explicit mode the runtime doesn't advertise falls back to the preferred one with a warning. | start |
| `overlay` | bool | `false` | Run as an `XR_EXTX_overlay` session so monitors composite **on top of another VR app** (a game, or `hypxrpaper`) instead of owning the view. Needs a runtime with the extension — Monado and WiVRn have it; SteamVR-Linux does not (downgrades to a normal exclusive session with a warning). See §8. | start |
| `overlay_z` | int | `1` | Overlay stacking order (`sessionLayersPlacement`); higher composites later / on top. The primary app is always beneath. Only meaningful with `overlay = true`. | start |
| `inhibit_idle` | string | `present` | **When** a live session inhibits idle (hypridle etc.) — `off` \| `focused` \| `present` (§7). `focused` = the session has input focus (the pre-research/20 behavior). `present` = the headset is actually **worn**, which also covers worn-but-not-focused (runtime dashboard in front, overlay mode). Legacy values still parse: `0`/`false` → `off`, `1`/`true` → **`focused`** (an existing explicit opt-in keeps its exact old meaning). | hot |
| `floor_offset` | float (m) | `1.5` | Fallback eye height, used only when the runtime lacks `XR_EXT_local_floor`. | start |

### Monitors & plug lifecycle

| Variable | Type | Default | Meaning | Applies |
|---|---|---|---|---|
| `default_size` | float (m) | `1.6` | Default quad width for a new XR monitor with no explicit `size:`. | hot |
| `default_distance` | float (m) | `1.5` | Default placement distance for a new / re-centered monitor. | hot |
| `recenter_on_plug` | bool | `true` | On the **first** don of a session, re-seat `anchor:local` monitors relative to your current head pose instead of the runtime's (often arbitrary) `LOCAL_FLOOR` origin — they land in front of you at their configured height/distance. Multi-monitor layouts are recentered rigidly (relative arrangement preserved). A brief doff-and-don within the same session does not re-seat. | hot |
| `monitors_follow_session` | string | `visible` | When XR monitors behave like **unplugged external monitors** (held disabled — workspaces evacuate to your remaining monitors, then return by name on replug, exactly like a physical display): `off` = never; `session` = while no OpenXR session exists; `visible` (default) = while the headset is not actually **worn**. The `visible` plug gate needs BOTH session visibility AND — when the runtime exposes `XR_EXT_user_presence` (e.g. WiVRn) — user presence, so a session created with the headset on the shelf never plugs. Legacy values parse: `false/0` → `off`, `true/1` → `session`. | hot |
| `monitor_unplug_grace_ms` | int | `20000` | Under `visible`: how long the headset must stay doffed before its monitors unplug and workspaces evacuate — anti-flap so a quick doff-and-don or proximity-sensor churn never rearranges workspaces. Donning re-plugs immediately regardless. | hot |
| `monitor_plug_settle_ms` | int | `1500` | Under `visible` on a runtime **without** `XR_EXT_user_presence`: how long the session must stay continuously visible before the **first** plug of a session — suppresses the session-create visibility blip (some runtimes sprint to `focused` at startup while still doffed). Ignored once presence is available and after the first plug. | hot |
| `destroy_monitors_on_stop` | bool | `false` | Destroy XR-created monitors when the session stops, instead of keeping them (unplugged per `monitors_follow_session`, plain headless outputs with `off`). Declared `xrmonitor`s re-materialize on the next session start. | hot |

### Anchoring & leash

Hot-live — re-read every frame, so you can tune them in-headset with `hyprctl keyword` and
they apply immediately.

| Variable | Type | Default | Meaning |
|---|---|---|---|
| `leash_response` | float (s) | `0.35` | Head/body leash spring response time — smaller is snappier. |
| `leash_deadzone_angle` | float (deg) | `15.0` | Head/body leash angular deadzone. |
| `leash_deadzone_distance` | float (m) | `0.25` | Head/body leash positional deadzone. |
| `body_leash_follow_height` | bool | `false` | Body-leashed monitors also follow vertical head movement (standing up / sitting down). |

### Adaptive anchoring

An `anchor:local` monitor with `adaptive:on` (§3) stays docked at your desk, follows you when
you walk away, and re-docks when you return — see the anchoring doc. All hot-live.

| Variable | Type | Default | Meaning |
|---|---|---|---|
| `adaptive_leave_radius` | float (m) | `1.5` | Horizontal (XZ) distance from the desk seat before a docked monitor undocks and follows (R_out). |
| `adaptive_return_radius` | float (m) | `1.0` | Distance back within the seat before it re-docks (R_in; keep `< leave_radius` for hysteresis). |
| `adaptive_leave_dwell_ms` | int | `400` | Hold beyond `leave_radius` this long before undocking (anti-flap). |
| `adaptive_return_dwell_ms` | int | `800` | Hold within `return_radius` this long before re-docking. |
| `adaptive_transition_ms` | int | `700` | Dock↔roam eased pose-blend duration. |
| `adaptive_transition_ease` | string | `smoothstep` | Easing curve: `smoothstep` / `linear` / `ease_out`. |
| `adaptive_roam_mode` | string | `body` | Follow behavior while roaming: `body` (yaw-only, comfortable height) / `head` (pinned to gaze). |
| `adaptive_use_height` | bool | `false` | Include vertical head movement in the geofence distance. |
| `adaptive_carry_offset` | bool | `false` | Roam at the head-relative offset captured on undock, instead of the configured comfortable roam offset. |

### Pointer & scroll

| Variable | Type | Default | Meaning |
|---|---|---|---|
| `pointer` | bool | `true` | Enable the synthetic XR ray-pointer device. Hot — created/destroyed live. |
| `pointer_trigger_threshold` | float | `0.7` | Trigger analog value that presses the pointer button. |
| `pointer_trigger_threshold_release` | float | `0.4` | Trigger value that releases it (hysteresis — analog jitter around the threshold never double-clicks). |
| `scroll_speed` | float | `1.0` | Thumbstick scroll multiplier on XR monitors. |

### Grab (move / resize)

| Variable | Type | Default | Meaning |
|---|---|---|---|
| `grab_threshold` | float | `0.7` | Squeeze analog value that starts a grab. |
| `grab_threshold_release` | float | `0.4` | Squeeze value that ends a grab (hysteresis). |
| `grab_anywhere` | bool | `true` | **Controllers:** a grip on a monitor's content body moves it. `false` confines moving to the move-bar and resizing to the corner handles. The bar and corners always grab. |
| `hand_grab` | string | `both` | Hand-tracking grab gesture (`XR_EXT_hand_interaction`): `pinch` (thumb-index, anchored to the stable pinch pose so opening the pinch to release doesn't lurch the window) / `grasp` (fist curl, wrist-anchored) / `both`. Hands grab from the move-bar/corners; controllers are unaffected. |
| `hand_grab_anywhere` | string | `grasp` | Which hand **gesture** may grab from a monitor's content body (the hand analog of `grab_anywhere`, keyed on the gesture that triggered the grab): `none` / `grasp` (default — a fist grabs anywhere, a pinch stays chrome-only and keeps its click) / `pinch` / `both`. Caveat for `pinch`/`both`: a body pinch both clicks and grabs. |
| `grab_release_latency_ms` | int | `100` | On release, re-anchor from the pose sampled this many ms **before** the release edge — rejects the release-motion lurch. `0` uses the release frame. |
| `grab_release_velocity_reject` | float | `3.0` | Release-jerk rejection as a **ratio** (not m/s): if the panel's peak speed in the release window exceeds this multiple of its typical carry speed, re-anchor from before the jerk. Being relative, a deliberately fast flick is kept. `0` disables. |
| `grab_filter` | bool | `true` | Smooth the move-grab carry with a 1-euro low-pass filter (removes tracking jitter, costs ~1 frame of latency). |
| `grab_filter_scope` | string | `all` | Which devices the carry filter covers: `all` (hands **and** controllers) / `hands` (controllers keep their zero-latency device-space late-latch). |
| `grab_filter_min_cutoff` | float | `1.0` | 1-euro minimum cutoff (Hz). Lower = more smoothing when nearly still (more lag). |
| `grab_filter_beta` | float | `0.025` | 1-euro speed coefficient. Higher = fast moves lag less (jitter returns sooner). |

### Chrome (on-panel move-bar & resize handles)

| Variable | Type | Default | Meaning |
|---|---|---|---|
| `chrome_enabled` | bool | `true` | Master toggle. When off the margins collapse to 0 and no chrome is drawn. Hot (recreates swapchains). |
| `chrome_margin` | float (m) | `0.10` | Transparent margin added around each monitor's content. `size:` still means **content** meters; the desktop blits into the inner content rect, so no pixel is ever covered. `0` disables chrome. |
| `chrome_bar_height` | float (m) | `0.08` | Height of the move-bar, below the content inside the bottom margin. |
| `chrome_bar_width_frac` | float | `0.8` | Move-bar width as a fraction of the content width (centered). |
| `chrome_corner_size` | float (m) | `0.09` | Size of each corner resize handle (square, clamped to the margin). |
| `chrome_hide_delay_ms` | int | `1500` | Delay before the chrome fades out after the ray stops hovering/grabbing. |
| `chrome_fade_ms` | int | `150` | Chrome fade in/out duration. |
| `chrome_col_idle` | color | `0x66aaaaaa` | Chrome color at rest (visible while hovering the quad). |
| `chrome_col_hover` | color | `0xcc66aaff` | Chrome color for the element (bar or corner) the ray points at. |
| `chrome_col_grab` | color | `0xff66aaff` | Chrome color while the quad is grabbed. |

### Luma-keyed transparency ("black-as-alpha")

Turns the dark parts of your desktop see-through, so a monitor reads as an **AR overlay** on your
real room instead of an opaque panel: each content pixel's alpha is derived from its own Rec.709
luma, so black dissolves while text and windows stay solid. Both variables are hot — tune them
in-headset with `hyprctl keyword` and the monitors re-blit immediately.

| Variable | Type | Default | Meaning | Applies |
|---|---|---|---|---|
| `black_alpha` | float | `1.0` | Alpha given to **pure black** content pixels. `1.0` = feature **off** (every pixel opaque, exactly the historic behavior); `0.0` = black is fully transparent; in between = translucent black. Brighter pixels are untouched (see `black_alpha_knee`). **This is the DEFAULT layer** for per-monitor resolution — an `xrrule` or a manual override can change it per monitor (see [§xrrule](#the-xrrule-keyword--situational-per-monitor-transparency)). | hot |
| `black_alpha_knee` | float | `0.10` | The luma at which content becomes **fully opaque**. Pure black gets `black_alpha`, luma ≥ this is left alone, `smoothstep` ramp between. Lower = only near-black dissolves; higher = dark greys go see-through too. Also the default layer for per-monitor resolution. | hot |
| `transparency_blend_ms` | int | `600` | How long any per-monitor transparency change (uniform `alpha` or luma key, from a rule, a manual verb or a reload) takes to ease in. Nothing pops: the value rides a smoothstep envelope and an interrupted transition continues from wherever it is. `0` = snap. | hot |

The curve is `alpha = mix(black_alpha, 1.0, smoothstep(0.0, black_alpha_knee, luma))`, and the
result is written **premultiplied** (rgb is scaled by the same alpha) because XR quads composite
premultiplied — this is what keeps a transparent monitor from glowing additively over passthrough.

**Blend-mode gating (read this first).** Transparency only *reveals* something when the runtime
composites our layers over something other than a black void, i.e. `blend_mode = alpha`
(passthrough) or `additive`. Under `blend_mode = opaque` — including WiVRn's usual auto-picked mode
— the runtime paints black behind the layers, so keying would only make monitors look dim and
dirty. It is therefore **force-disabled under `opaque`**, with one warning in the log, and
`hyprctl openxr status` says so:

```
blend mode: opaque
black alpha: off — 0.20 ignored under blend mode opaque
```

To actually use it: set `openxr:blend_mode = alpha` and restart the session
(`hyprctl openxr disable && hyprctl openxr enable`) — `blend_mode` is read once at session start.

Caveats:

- **Dark themes lose contrast.** A `#1a1a1a` terminal background sits near the default knee, so
  most dark themes will go partly see-through and text contrast drops against a bright room. Lower
  `black_alpha_knee` (e.g. `0.04`) to dissolve only near-black, or raise `black_alpha` (e.g. `0.5`)
  to keep a dimming scrim behind the text.
- **Text fringes.** Antialiased glyph edges are dark-grey pixels blended toward the background, so
  they land on the ramp and become partly transparent — thin text over passthrough can look thinner
  or haloed. A low knee keeps the fringe pixels opaque; subpixel/heavier fonts help.
- **Chrome is never keyed.** The move-bar and corner handles are drawn into the transparent margin
  at their configured `chrome_col_*` alpha, so a fully-dissolved monitor still has grabbable chrome.
- Any content that is *actually* black loses its ability to occlude: video letterboxing, a black
  wallpaper or a not-yet-painted monitor become windows onto the room. That is the feature working.
- **These two variables are the DEFAULTS**, not the final word: they seed the per-monitor resolution
  that `xrrule` and `hyprctl openxr blackalpha` layer on top of. A config with no `xrrule` and no
  manual override behaves exactly as it did before that feature existed.

### Ray aim assist (cursor, magnetism, sticky hover, aim filter)

See [`04-input.md` §6.1](04-input.md). All hot (read per-frame).

| Variable | Type | Default | Meaning |
|---|---|---|---|
| `cursor` | bool | `true` | Draw a small endpoint cursor where each hand's aim ray hits a monitor. Zero cost when no ray hovers a quad. |
| `cursor_size` | float (m) | `0.02` | Cursor diameter — a constant physical size, so it looks the same on any monitor. |
| `cursor_press_scale` | float | `1.6` | Cursor growth factor while a button/pinch is pressed. |
| `cursor_col_idle` | color | `0x80ffffff` | Cursor color over content/margin. |
| `cursor_col_grabbable` | color | `0xcc66aaff` | Cursor color over a grab handle (move-bar or corner). |
| `cursor_col_press` | color | `0xffffffff` | Cursor color while a button/pinch is pressed. |
| `cursor_col_grab` | color | `0xff66aaff` | Cursor color reserved for an active grab. |
| `cursor_per_hand_tint` | bool | `true` | Tint the left hand's cursor cooler so two rays are distinguishable. |
| `magnet` | bool | `true` | Chrome-only aim magnetism: a near-miss of a grab handle still highlights/grab-enables it (never magnetizes content — desktop clicks stay pixel-precise). |
| `magnet_angle` | float (°) | `2.0` | Magnetism cone half-angle: how far off a handle the ray may point and still snap to it. |
| `hover_hysteresis` | float (m) | `0.03` | Sticky-hover exit margin: once the ray lands a handle, keep it highlighted/grab-eligible until the ray leaves the handle expanded by this much. `0` disables stickiness. |
| `hover_dropout_frames` | int | `2` | Hold the highlighted handle across up to this many frames where the ray misses every quad. |
| `aim_filter` | bool | `true` | 1-euro filter the aim pose before hit-testing (controllers **and** hands); ~1 frame latency, steadier ray/cursor. |
| `aim_filter_min_cutoff` | float (Hz) | `1.5` | Aim 1-euro minimum cutoff. Lower = steadier when aiming slowly (more lag). |
| `aim_filter_beta` | float | `0.01` | Aim 1-euro speed coefficient. Higher = fast sweeps lag less (jitter returns sooner). |
| `aim_pinch_damping` | float | `0.4` | Aim min-cutoff multiplier while a press/pinch is ramping up, so committing a click/grab doesn't yank the aim on the commit frame. `1` disables onset damping. |

### Haptics

| Variable | Type | Default | Meaning |
|---|---|---|---|
| `haptics` | bool | `true` | Master controller-haptics toggle (grab/press ticks + the hover tick). Hands have no actuator and are unaffected. |
| `haptic_hover` | bool | `true` | Fire a short controller tick when the ray first enters a grab handle. |
| `haptic_amplitude` | float | `0.5` | Controller haptic tick amplitude (0–1). |

### Conditional hand input (research/16 Part A)

Stops accidental hand-tracking gestures (pinch/grab clicks) while you type. **Controllers are never
gated by this** — only hand tracking. The gate is resolved every frame; a gated-off hand is fully
inert (no ray/hover/cursor/click/grab; an in-progress hand grab is ended via the release latch).

| Variable | Type | Default | Meaning |
|---|---|---|---|
| `hand_input` | string | `auto` | `on` (hands always) / `off` (never; controllers still work) / `auto`. `auto` enables hands only when you're **away from the keyboard** (`hand_input_idle_s` of silence) or **roaming** (`hand_input_roam_enables`). Hot-applies (special-cased in `parseKeyword`, change-detected so a runtime `handinput` dispatcher change survives an unrelated reload). |
| `hand_input_idle_s` | float | `15` | `auto`: seconds of physical-keyboard silence before hands re-enable; any keypress re-gates instantly. |
| `hand_input_roam_enables` | bool | `true` | `auto`: also enable hands whenever the head is beyond the adaptive seat geofence (roaming), so wandering the house always has hands even mid-typing-timeout. |

The `xrmonitor handinput on|off|auto|toggle` dispatcher overrides the policy at runtime; **`toggle`**
is the key-chord you bind to flip hands on/off while at the keyboard (it lands in `auto` + a manual
latch, so a later `handinput auto` returns to the pure keyboard/roam gate). `hyprctl openxr status`
shows `hand input: active | gated (keyboard) | gated (manual) | off`.

### Gaze grab (research/16 Part B)

Keyboard-driven monitor manipulation steered by where you **look** (VIEW = head-forward center-FOV
ray; no eye tracking needed). Look at a monitor, tap the grab key (it follows your gaze), tap keys
to push/pull it, tap again to drop it. See the `gaze*` dispatcher verbs in §4.

| Variable | Type | Default | Meaning |
|---|---|---|---|
| `gaze_source` | string | `view` | `view` (head forward) / `eye` (reserved; auto-falls-back to `view` — no eye HW on Quest 3). |
| `gaze_dwell_ms` | int | `200` | Rest this long on a monitor before it becomes the grab-eligible candidate (anti-saccade; also the selection hysteresis). | hot |
| `gaze_hysteresis_deg` | float | `3.0` | Sticky-selection exit margin: the selected monitor is hit-tested with this much extra angular slack so adjacent boundaries don't flicker. | hot |
| `gaze_follow` | bool | `true` | `true`: the monitor rides the live gaze ray while held. `false`: it detaches at grab and only the push/pull keys move it (deliberate reposition). | hot |
| `gaze_dist_step` | float | `0.1` | Default push/pull step (m) for `gazepush` with no argument (bind with `binde` for stepped repeats). | hot |
| `gaze_filter` | bool | `true` | 1€-filter the gaze pose before hit-testing so the selection is steady. | hot |
| `gaze_filter_min_cutoff` | float | `1.5` | Gaze 1€ filter min cutoff (Hz); lower = steadier when looking slowly. | hot |
| `gaze_filter_beta` | float | `0.01` | Gaze 1€ filter speed coefficient; higher = the cutoff rises faster with head motion. | hot |
| `gaze_cursor` | bool | `true` | Draw a distinct cursor dot at the gaze point on the carried monitor (the candidate is shown via the chrome highlight). | hot |
| `gaze_cursor_col` | color | `0xffcc66ff` | Gaze cursor dot color. | hot |

Feedback: the dwell-stable monitor you're looking at is highlighted via the normal chrome (its
move-bar lights up in the hover color); a gaze-carried monitor glows in the grab color and shows the
gaze cursor. A gaze carry and a hand/controller grab can't fight over one monitor — first wins.

---

## 3. The `xrmonitor` keyword

Declares that a virtual monitor should exist and where it lives in 3D space. Ordinary
`monitor =` / `monitorv2` rules still apply to it by name (scale, VRR, bit depth, mirror, …);
`xrmonitor` only controls existence and 3D placement.

```
xrmonitor = <name>, <WxH>[@Hz], <anchor-spec>[, size:<meters>]
```

- `<name>` — output name, conventionally `XR-`-prefixed (e.g. `XR-code`).
- `<WxH>[@Hz]` — pixel mode; refresh optional (default 60).
- `<anchor-spec>` — space-separated `key:value` sub-tokens. Coordinates are meters in the
  OpenXR convention: **+X right, +Y up, −Z forward**.
- `size:` — the quad's content width in meters (default `openxr:default_size`).

| Anchor mode | Spec | Meaning |
|---|---|---|
| `anchor:local pos:x,y,z [yaw:deg] [pitch:deg]` | Fixed in the room (`LOCAL_FLOOR` space); `pos` required. | Bolted in place — doesn't move with you. |
| `anchor:head offset:x,y,z` | Offset in view space; orientation is always look-at-driven (no yaw/pitch). | A HUD that follows your gaze with a deadzone + spring. |
| `anchor:body offset:x,y,z` | Offset in the yaw-only body frame (only `yaw:` meaningful). | Turns with you but not with head pitch; fixed height unless `body_leash_follow_height`. |
| `anchor:device:left\|right offset:x,y,z [yaw:deg] [pitch:deg]` | Offset in the named controller's grip frame. | Rigidly locked to a controller — a palette you hold up. |

```ini
xrmonitor = XR-code,    2560x1440@90, anchor:local pos:0,1.4,-1.5 yaw:0, size:1.8
xrmonitor = XR-chat,    1280x720,     anchor:head offset:0.4,-0.2,-1.0, size:0.6
xrmonitor = XR-music,   1920x1080@60, anchor:body offset:-0.8,1.2,-1.2, size:0.9
xrmonitor = XR-palette, 800x800,      anchor:device:left offset:0,0.08,-0.05, size:0.25
```

### Adaptive tokens (on `anchor:local` only)

Add these to an `anchor:local` line to make it follow you around (see the anchoring doc);
each overrides the matching `openxr:adaptive_*` global:

| Token | Meaning |
|---|---|
| `adaptive:on\|off` | Enable the desk↔follow decorator (default off). |
| `roam:head\|body` | Follow behavior while roaming. |
| `roam_offset:x,y,z` / `roam_yaw:deg` | The comfortable follow placement. |
| `leave:R` / `return:R` | Per-monitor geofence radii (`return` must be `< leave`). |
| `carry:on\|off` | Roam at the offset captured on undock instead of the roam offset. |

```ini
xrmonitor = XR-video, 1920x1080@60, anchor:local pos:0.3,1.05,-1.2 yaw:0, size:1.8, adaptive:on roam:body roam_offset:0,1.35,-1.2 roam_yaw:0
```

### Reload behavior

Adding/removing/changing an `xrmonitor` line and reloading creates/destroys/re-anchors exactly
the diff; unchanged lines don't flicker, and a changed pixel mode triggers a swapchain
recreate. **Monitors created at runtime** (dispatcher / `hyprctl openxr create`) **are never
touched by reconciliation** — removing a config line only affects monitors that came from a
config line. Declared monitors materialize even with no session and bind their quads when one
starts. A dynamic `hyprctl keyword xrmonitor ...` also reconciles (batched, so a
`hyprctl --batch` of several lines reconciles once).

### 3.1 Pixel-mode precedence (`monitor =` vs the requested mode)

An XR monitor's pixel mode can be asked for in three places. The order, highest first:

1. **An explicit mode in a `monitor =` rule matching the name** — `monitor = XR-code,
   2560x1440@90, auto, 1.25`. If such a rule exists *when the monitor is created*, it owns the
   mode outright and nothing below is applied. (`preferred` / `highres` / `highrr` / `maxwidth`
   are **not** explicit modes — they defer, so `monitor = XR-2, preferred, auto, 1.25` is the
   normal way to set scale without claiming the mode.)
2. **The mode on the `xrmonitor =` line, or the create args** — `xrmonitor = XR-code,
   2560x1440@90, …` and `hyprctl openxr create XR-2 2560x1440@60` are equivalent here. The
   compositor installs this as a persistent named monitor rule so it survives plug/unplug cycles
   **and config reloads**, for declared and runtime-created monitors alike. Non-mode fields of a
   matching `monitor =` rule (scale, transform, VRR, …) are preserved — only the mode is taken
   over.
3. **The headless default**, 1920x1080@60, when no mode was requested anywhere.

Refresh is optional wherever a mode is: `2560x1440` means `2560x1440@60`.

Scale is deliberately *not* an `xrmonitor` / `create` argument — `monitor = <name>, preferred,
auto, <scale>` is the mechanism, and it composes with rule 2 above. Pre-declaring such a line for
names a script or voice daemon will mint later is harmless while the monitor doesn't exist and
gives the created monitor a sane scale the moment it appears.

### Mirroring onto a physical screen

No XR-specific mechanism — the ordinary mirror recipe, matched by name:

```ini
monitor = eDP-1, preferred, auto, 1, mirror, XR-code
```

Anyone at the physical screen sees exactly what's on the XR monitor.

### Persistent workspace assignments

The in-session unplug/replug cycle already returns workspaces to their XR monitor by name, but
that memory is runtime-only. To pin a workspace across restarts and logouts, use an ordinary
workspace rule (XR monitors are name-keyed like any output); it re-applies every time the
monitors plug in:

```ini
workspace = 9, monitor:XR-code, default:true
```

---

## 3.5 The `xrrule` keyword — situational per-monitor transparency

Makes each XR monitor's transparency **react to what is happening**: ghost the panels while you
walk away from the desk, go fully opaque for a fullscreen game, disable the luma key for a video
monitor. Same idiom as `windowrule`: a flat `effects, conditions` line.

```ini
xrrule = <effects>, <conditions>
```

### The model

- The **monitor** is the unit of *effect*. Windows are only a source of *conditions* — there is no
  such thing as "make this window transparent in XR"; you make the monitor it is on transparent.
- Two effects compose per pixel: `final_alpha = uniform_alpha × lumakey_alpha(pixel)`. The uniform
  alpha fades the whole panel; the luma key (`blackalpha`, [above](#luma-keyed-transparency-black-as-alpha))
  dissolves its dark pixels. Both are written premultiplied, so they never glow additively over
  passthrough.
- **Three precedence layers, resolved per effect**:
  1. **defaults** — `alpha` is `1.0` (opaque); `blackalpha`/`blackalpha_knee` come from
     `openxr:black_alpha` / `openxr:black_alpha_knee`;
  2. **rules** — every matching `xrrule`, in **config order**; a later match overrides an earlier
     one *per effect* (a rule that only sets `blackalpha` leaves an earlier rule's `alpha` alone);
  3. **manual override** — `hyprctl openxr alpha|blackalpha`, sticky until cleared with `auto`.
     Manual always wins, exactly like the `handinput` manual-over-auto latch.
- Every change **eases** over `openxr:transparency_blend_ms` (default 600ms). An interrupted
  transition continues from wherever it currently is — it never snaps back and re-runs.
- A ghosted monitor ghosts **entirely**: the uniform alpha is applied over the finished panel, so
  the move-bar and resize handles fade with the content. (A solid bar under a near-invisible screen
  looks broken; report 09 §3.3.) The luma key still leaves chrome alone, so a *dissolved* — as
  opposed to *faded* — monitor keeps a grabbable bar.

### Effects (space-separated `name value`)

| Effect | Value | Meaning |
|---|---|---|
| `alpha` | `0..1` | Uniform monitor opacity. `1.0` = opaque. Applies under **every** blend mode (under `opaque` it reads as a dim rather than see-through — there is nothing behind us to reveal). |
| `blackalpha` | `0..1` or `off` | Per-monitor `openxr:black_alpha`. `off` == `1.0` == keying disabled. Gated on the blend mode exactly like the global (see below). |
| `blackalpha_knee` | float | Per-monitor `openxr:black_alpha_knee`. |

At least one effect is required.

### Conditions (space-separated `key:value`, **all** must match)

| Condition | Value | Matches |
|---|---|---|
| `monitor` | regex | The XR monitor's name. |
| `anchorstate` | `docked` \| `follow` \| `carried` | How the monitor is anchored *right now* (see below). |
| `focusclass` | regex | The class of the monitor's focused window. |
| `focustitle` | regex | The title of the monitor's focused window. |
| `fullscreen` | `0` \| `1` | Whether the monitor's focused window is fullscreen. |

An omitted condition is a **wildcard**; omitting the whole list (no comma at all) matches every
monitor. Every monitor is evaluated against its **own** context tuple — that is what makes
`monitor:` a filter rather than a selector.

**Regexes are RE2 (the same engine as `windowrule`) matched as a SEARCH, not a full match.** So
`focusclass:^steam_app_` is a prefix test and `focusclass:(mpv|vlc)` finds a substring; anchor with
`^…$` (`monitor:^XR-main$`) when you want an exact match. This differs deliberately from
`windowrule`, whose regexes are full matches. Values are whitespace-split, so a value that needs a
space must be quoted: `focustitle:"Mozilla Firefox"`.

**`anchorstate`**, in priority order:

| State | When |
|---|---|
| `carried` | A hand grab, corner resize or gaze carry owns the monitor right now. |
| `follow` | The monitor is leashed to you: `anchor:head`, `anchor:body`, `anchor:device`, or an adaptive monitor that has left its desk pose (adaptive phase ≠ docked). |
| `docked` | Everything else — world-fixed where you left it. |

**"The focused window of a monitor"** is the fullscreen window on the monitor's active workspace if
there is one, else that workspace's last-focused window (`CWorkspace::m_lastFocusedWindow`).
Deliberately *per workspace*, not the compositor's global focus: every XR monitor must resolve its
own situation, so a rule can dim the monitor you are **not** looking at. A monitor with no window at
all can never match `focusclass:`/`focustitle:` (there is nothing to match), but it *does* match
`fullscreen:0` — that is a property of the monitor's situation, not a claim that a window exists.

### Examples

```ini
# Fullscreen games get the panel back at full strength, no keying.
xrrule = alpha 1.0 blackalpha off, monitor:.* anchorstate:docked focusclass:^steam_app_ fullscreen:1

# A dedicated video monitor should never dissolve its letterboxing.
xrrule = blackalpha off, monitor:XR-media focusclass:(mpv|vlc)

# THE SAFETY RULE — see below. Put this in your config.
xrrule = alpha 0.55, anchorstate:follow
```

### The walking rule (ship this)

```ini
xrrule = alpha 0.55, anchorstate:follow
```

**Add this rule if you use head/body leashing or adaptive anchoring at all.** When a monitor is
leashed to your body it travels with you — which means a large, opaque panel is pinned in front of
your face while you walk through a room you cannot fully see. That is the one genuinely unsafe
configuration this feature exists to fix: at `alpha 0.55` the desktop stays readable *and* you can
see the doorway, the coffee table and the cat. It is **not** hardcoded (a default that silently
changes what your monitors look like would be worse), so it lives here and in
`example/openxr.conf` — copy it.

Tune to taste: lower (`0.4`) sees the room better, higher (`0.7`) keeps text crisper. Report 09
§3.4 cites Meta's MR guidance that UI over passthrough needs contrast; do not drive it so low that
you are squinting, and remember `alpha` under `blend_mode = opaque` dims toward black rather than
revealing anything.

### Blend-mode gating

`blackalpha` from a rule (or a manual override) rides the **same** gate as the global: keying only
does anything under `blend_mode = alpha` (passthrough) or `additive`, and is forced off under
`opaque` with one warning. `alpha` is **not** gated — dimming a monitor toward the background is a
legitimate de-emphasis cue under any blend mode.

### Reload & runtime behavior

- Rules are re-parsed on every config reload; a malformed rule is reported as a config error
  (naming the offending effect/condition) and that line is dropped, the rest still load.
- Rules are re-evaluated, on the main thread, on: focus change, fullscreen change, window close,
  active-workspace change, window **title** change (only when some rule actually uses
  `focustitle:` — titles change on every browser tab switch and we refuse to pay for that
  otherwise), anchor-state transitions (grab begin/end, adaptive dock/undock, `xrmonitor anchor`),
  monitor add/remove/create, session start, and config reload. A burst of events collapses into a
  single evaluation.
- `hyprctl keyword xrrule …` works and takes effect immediately, but **appends** to the current rule
  set (there is no name to replace) — reload the config to start from a clean slate.

### Manual override

```
hyprctl openxr alpha      <name|active> <0..1|auto>
hyprctl openxr blackalpha <name|active> <0..1|off|auto>
```

`active` is the currently selected monitor (same resolution as every other verb). The value is
**sticky** — it outranks every rule until you clear it with `auto`, which hands the monitor straight
back to rule control (re-resolved for its *current* situation, so it may move again immediately).

`hyprctl openxr status` reports the effective values and where each came from:

```
monitor XR-main (ID 3): 2560x1440@72.00 size 1.60m anchor body … adaptive: roaming (roam body, seat 2.30m)
  XR-main: alpha 0.55 (rule), blackalpha 0.20 (default, knee 0.10), anchorstate follow
```

A live transition shows as `alpha 0.83 -> 0.55`. The JSON form carries the same data per monitor
under `transparency` (`alpha`, `alphaTarget`, `alphaSource`, `blackAlpha`, `blackAlphaTarget`,
`blackAlphaSource`, `knee`, `kneeSource`, `transitioning`) plus a top-level `anchorState`. The
older top-level `black alpha:` line still reports the **global default**, not any monitor's
resolved value — read the per-monitor lines.

### Cost

Zero when unused: no rules and no manual override means every monitor resolves to the defaults, the
uniform-fade pass is skipped entirely (it is a no-op at `alpha 1.0`) and the envelope timer stays
disarmed. While a **uniform alpha** transition runs, the affected monitor recomposes its own XR
panel per frame — no desktop re-render. While a **luma key** transition runs, the monitor is damaged
each tick so the desktop re-renders and the blit can re-key it (the key is per-pixel and must be
baked while the source buffer is in hand); this is bounded by the blend duration.

---

## 4. The `xrmonitor` dispatcher

`bind = MODS, KEY, xrmonitor, <verb> [args…]` — also callable as
`hyprctl dispatch xrmonitor <verb> [args…]` and `hyprctl openxr <verb> [args…]`. Three
transports, one implementation.

| Verb | Args | Effect |
|---|---|---|
| `create` | `<name> [WxH[@Hz]] [anchor-spec]` | Create a runtime-owned XR monitor. Mode defaults to 1920x1080@60 and is durable across reloads (§3.1 for how it ranks against a `monitor =` rule); anchor defaults to `anchor:local` placed at `openxr:default_distance` facing you. |
| `destroy` | `<name\|active>` | Destroy the named monitor, or the selected one. |
| `select` | `<name\|next\|prev>` | Set the explicit selection target for the verbs below. |
| `anchor` | `<name\|active> <mode-spec>` | Re-anchor without moving the quad visually: `local`, `head [offset:x,y,z]`, `body [offset:x,y,z]`, `device:left\|right [offset:x,y,z]`. |
| `move` | `<dx> <dy> <dz>` | Translate in the anchor's own frame, meters. |
| `rotate` | `<dyaw> [dpitch]` | Rotate, degrees (pitch clamped ±85°). |
| `scale` | `<f\|+d\|-d>` | Bare number multiplies the width; signed number adds/subtracts meters. Clamped 0.2–4.0 m. |
| `distance` | `<±m>` | Push/pull along the view ray. Clamped 0.3–5.0 m. |
| `center` | *(none)* | Re-place centered in view at `openxr:default_distance` (anchor mode preserved). |
| `place` | `<name\|active> at <x>,<y>,<z>` | Re-anchor `<name>` to `local`, **moving** its center to the given `LOCAL_FLOOR` point (meters), facing the headset. Unlike `anchor local` (which freezes the quad at its current pose), `place` teleports it to a supplied point — exactly the point `hyprctl openxr gaze` returns, so a voice/script layer can drop a monitor where the user was looking. |
| `adaptive` | `on\|off\|toggle` | Enable/disable the adaptive decorator on the selected monitor (recaptures the desk seat on enable). |
| `undock` | *(none)* | Force the selected adaptive monitor to pick up and follow now (skips the geofence dwell). |
| `dock` | `[here]` | Force it back to the saved desk pose now; `dock here` redefines the desk pose to where it is now and recaptures the seat. |
| `roam` | `head\|body` | Change the follow behavior live. |
| `gazegrab` | *(none)* | **Toggle**: grab the monitor you're looking at (dwell-stable candidate) so it follows your gaze; if a gaze carry is active, release it. Errors cleanly if you're not looking at a monitor, or it's already grabbed by a hand/controller. |
| `gazegrab` | `<name\|active>` | **Targeted**: begin a gaze carry on the *named* monitor regardless of the live dwell candidate (so a voice/script layer can carry the monitor it resolved seconds earlier, not whatever the head happens to point at now). Idempotent when already carrying that monitor; errors cleanly if a *different* carry is active, the name is unknown, head tracking is unavailable, the monitor isn't placed yet, or it's already hand/controller-grabbed. |
| `gazerelease` | *(none)* | Explicit release of the gaze carry (no-op if none). For a `bindr` hold-to-carry pattern. |
| `gazepush` | `<±m>` | Push/pull the gaze-carried monitor along the gaze ray (or, when not carrying, the gaze-selected monitor along the view ray). No arg = `openxr:gaze_dist_step`. Designed for `binde` repeats. |
| `handinput` | `on\|off\|auto\|toggle` | Set the conditional hand-input policy (§2). `toggle` is the key-chord to flip hands on/off at the keyboard. |
| `alpha` | `<name\|active> <0..1\|auto>` | Manual uniform-transparency override (§3.5). Sticky — outranks every `xrrule` until cleared with `auto`. |
| `blackalpha` | `<name\|active> <0..1\|off\|auto>` | Manual luma-key override (§3.5). `off` disables keying on that monitor; `auto` hands it back to the rules. |

**Selected-target resolution** (for `active` / omitted targets): explicit `select` > last
ray-hovered monitor > focused-monitor-if-XR — else the verb errors with "no XR monitor
selected".

```ini
bind  = SUPER,       X,            xrmonitor, create XR-scratch
bind  = SUPER SHIFT, X,            xrmonitor, destroy active
bind  = SUPER,       bracketright, xrmonitor, select next
bind  = SUPER,       bracketleft,  xrmonitor, select prev
bind  = SUPER,       Home,         xrmonitor, center
binde = SUPER,       equal,        xrmonitor, distance -0.25
binde = SUPER,       minus,        xrmonitor, distance +0.25
bind  = SUPER,       H,            xrmonitor, anchor active head
bind  = SUPER,       L,            xrmonitor, anchor active local
bind  = SUPER,       A,            xrmonitor, adaptive toggle
```

(`binde` = repeat-on-hold, natural for `distance`/`scale`/`rotate`.)

---

## 5. `hyprctl openxr`

```
hyprctl openxr [status]        # default subcommand
hyprctl -j openxr              # JSON
hyprctl openxr enable|disable  # start/stop the session (does not touch openxr:enabled)
hyprctl openxr <verb> …        # the §4 verbs: create destroy select anchor move rotate
                               #   scale distance center place alpha blackalpha adaptive
                               #   dock undock roam gazegrab gazerelease gazepush handinput
hyprctl openxr gaze [at <ms>]  # read-only head ray + timestamped gaze history (see below)
hyprctl openxr layout          # dump the CURRENT live layout as paste-ready xrmonitor= lines
```

`hyprctl openxr layout` walks every live XR monitor (declared and runtime-created) and prints
lines you can paste straight into your config to reproduce the arrangement — including a
monitor that was moved at runtime with the dispatcher or a controller grab. `anchor:local`
lines reflect the live world pose; `head`/`body`/`device` lines reflect the configured offset
for that mode (their live position tracks you by design, so freezing it into a config line
wouldn't mean what you'd expect). Roll is not representable in the serialization (yaw + pitch
only).

### `gaze` — head ray & timestamped gaze history

`hyprctl openxr gaze` is a **read-only** query that returns the current head pose (position +
orientation + forward ray) and the dwell-stable gaze candidate — the monitor you're looking at,
as computed by the same gaze-selection machine `gazegrab` uses (§2 "Gaze grab"). It exists so a
companion voice daemon (`hypxrvoice`,
[research/VOICE-CONTROL.md](research/VOICE-CONTROL.md)) can resolve pointing-deixis like "drop
this monitor **here**" or "that one over there."

The interesting part is the **timestamped** form:

```
hyprctl openxr gaze               # newest sample (current head ray + gaze candidate)
hyprctl -j openxr gaze            # JSON
hyprctl openxr gaze at <ms>       # nearest sample to a CLOCK_MONOTONIC millisecond timestamp
hyprctl openxr gaze --at-ms <ms>  # same (explicit flag form)
```

The compositor keeps a rolling ~91-second ring (8192 samples, one per XR frame at 90 Hz; longer
at lower refresh rates — sized so even a long utterance plus ASR/intent latency never falls off
the retained window at full frame-rate resolution, for ~512 KB) of head poses +
gaze candidates. Speech recognition takes 1–3 s, so by the time an utterance is parsed the head
has usually moved on (often to a feedback HUD) — querying the pose *now* would be a systematic
bug. Instead the daemon captures a monotonic timestamp at **speech onset** (`clock_gettime(
CLOCK_MONOTONIC)` → milliseconds) and asks `gaze at <that ms>`; the reply carries
`matchedTimestampMs` and `ageMs` (`requested − matched`) so the caller sees exactly how stale the
match is, and whether the request clamped to the ends of the retained window.

**Clock contract (for the daemon team):** `timestampMs` is milliseconds on **`CLOCK_MONOTONIC`**.
The compositor stamps samples via `Time::steadyNow()`, and `std::chrono::steady_clock ==
CLOCK_MONOTONIC` on Linux/libstdc++, so a daemon reading `clock_gettime(CLOCK_MONOTONIC)` and
dividing to milliseconds is in the same clock domain — no conversion, no epoch translation.
Targets older/newer than the retained window clamp to the oldest/newest sample (never an error);
ties resolve to the newer sample. `ok:false` is returned only before any XR frame has run.

JSON (`gaze at` adds the `query` block; a bare `gaze` omits it):

```json
{
    "ok": true,
    "timestampMs": 84213765,
    "viewValid": true,
    "head": {
        "pos": [0.0120, 1.4300, -0.0050],
        "quat": [0.00000, 0.38268, 0.00000, 0.92388],
        "forward": [0.7071, 0.0000, -0.7071]
    },
    "gaze": {
        "monitorId": 3,
        "name": "XR-1",
        "selected": true,
        "dwellSec": 0.000,
        "hitPoint": [0.2100, 1.3300, -2.1400],
        "hitDistM": 2.1400
    },
    "query": {
        "requestedTimestampMs": 84212300,
        "matchedTimestampMs": 84213765,
        "ageMs": -1465
    }
}
```

Text form:

```
gaze: looking at XR-1 (id 3, dwell 0.00s)
  timestampMs: 84213765  viewValid: yes
  head pos [0.012, 1.430, -0.005]  forward [0.707, 0.000, -0.707]
  hit [0.210, 1.330, -2.140] at 2.140m
  query: requested 84212300  matched 84213765  age -1465ms
```

`monitorId` is `-1` (`selected:false`, text "looking at passthrough") when the gaze ray misses
every quad. `pos`/`forward` are in `LOCAL_FLOOR` space (meters); `quat` is `[x,y,z,w]`.

**`hitPoint` / `hitDistM` (optional).** Where the gaze ray actually *met* the selected monitor's
quad — `hitPoint` in `LOCAL_FLOOR` meters, `hitDistM` the distance along the ray to it. This is
the exact space and units `xrmonitor place <name> at x,y,z` consumes, so "put it where I was
looking" is a straight hand-off with no projection and no conversion. Both fields are **omitted**
(and the `hit` line is absent from the text form) unless the ray was on the selected quad in that
sample, so a consumer must treat them as optional:

- nothing is selected (`selected:false`) — there is no surface to report; or
- the sample is mid-dwell: the ray has already left the selected monitor but the dwell switch
  hasn't committed yet, so `monitorId` still names the old one while nothing is being hit.

When they're absent the caller falls back to projecting along `head.forward` at its own preferred
distance. The point is computed on the frame thread from the same (1€-filtered) ray that chose
`monitorId`, so it is always consistent with the reported candidate — a mid-dwell sample reports
the *selected* monitor's intersection, never a nearer quad's. A caller placing a monitor there
should still clamp the distance from `head.pos`: a hit point can be closer than is comfortable.

### `status` output

Text form (one field per line):

```
state: focused
runtime: Monado(XRT) by Collabora et al.
system: Simulated HMD
runtime gpu: AMD Radeon Graphics (drm 226:128)
runtime json: (loader default)
blend mode: opaque
black alpha: off
overlay: no
selected: XR-code
monitors follow session: visible
visible: yes
presence: yes
idle inhibited: yes
input: left controllers, right hands (pinch, filtered)
monitor XR-code (ID 3): 2560x1440@90.00 size 1.80m anchor local pos [0.00, 1.40, -1.50] grabbed: no (none) hovered: yes (body) plugged: yes content: <path>
```

While dormant the state line carries the reprobe hint, e.g.
`state: unavailable (waiting for headset, retrying in 1800ms)`. When the event-driven watch is
armed (`reprobe_watch`, either wait mode) the hint adds `, watching socket`, e.g.
`state: unavailable (waiting for headset, retrying in 2000ms, watching socket)` — the delay is just
the fallback; a socket/pid trigger probes within ~150ms. A WiVRn runtime whose service is up but
headset undonned reads as `waiting for headset` (its degraded pre-don IPC answers but lacks the
required extensions — classified as a headset wait, fixed base cadence). While a grace-period
unplug is pending, the follow line reads e.g. `visible (unplug in 12000ms)`.

JSON (`hyprctl -j openxr`) — all keys always present:

```json
{
    "state": "focused",
    "runtimeName": "Monado(XRT) by Collabora et al.",
    "systemName": "Simulated HMD",
    "runtimeGpu": "AMD Radeon Graphics (drm 226:128)",
    "runtimeJson": "",
    "blendMode": "opaque",
    "blackAlpha": { "configured": 1.000, "effective": 1.000, "knee": 0.100, "active": false, "gatedOff": false },
    "overlay": false,
    "selected": "XR-code",
    "monitorsFollowSession": "visible",
    "monitorUnplugPendingMs": -1,
    "userPresence": "yes",
    "visible": "yes",
    "reprobeWaiting": "",
    "reprobePendingMs": -1,
    "reprobeWatching": false,
    "inhibitingIdle": true,
    "idleInhibitMode": "present",
    "input": {
        "left":  { "kind": "controllers", "gesture": "grasp", "filtered": false },
        "right": { "kind": "hands",       "gesture": "pinch", "filtered": true  }
    },
    "monitors": [
        {
            "name": "XR-code",
            "id": 3,
            "size_m": 1.80,
            "anchor": { "mode": "local", "pose": { "pos": [0.0, 1.4, -1.5], "quat": [0.0, 0.0, 0.0, 1.0] } },
            "grabbed": false,
            "grabKind": "none",
            "hovered": true,
            "region": "body",
            "plugged": true,
            "contentPath": "",
            "linear": false,
            "adaptive": { "enabled": false, "phase": "docked", "roamMode": "body", "seatDistM": 0.0, "transitionT": 0.0 }
        }
    ]
}
```

Field notes:

- `state` — `disabled` | `unavailable` | `starting` | `idle` | `visible` | `focused` |
  `stopping`.
- `runtimeName` / `systemName` — from the runtime; empty when there is no session.
- `runtimeGpu` — the GPU the runtime composites on, resolved by the cross-GPU probe (see the
  session/graphics doc); empty when undeterminable (text form shows `unknown`).
- `blendMode` — the active environment blend mode (`opaque` when there is no session).
- `blackAlpha` — the luma-keyed transparency state: `configured` is `openxr:black_alpha` as set,
  `effective` is what the blit is actually applying (forced to `1.0` unless the blend mode shows
  through), `knee` is `openxr:black_alpha_knee`, `active` means the key is really keying, and
  `gatedOff` means a value < 1 is being ignored because the blend mode is `opaque`. The text form
  collapses this to one `black alpha:` line — `0.20 (knee 0.10)`, `off`, or
  `off — 0.20 ignored under blend mode opaque`.
- `overlay` — the **actual** session type, not the config request: `false` with no session, or
  when an overlay request was downgraded to exclusive.
- `selected` — the concrete monitor an `active`/omitted target resolves to right now (explicit
  `select` > last ray-hovered > focused-if-XR), or `""` (text form: `(none)`) when none
  resolves. Lets a consumer name the verb target without replicating the resolution order.
- `monitorsFollowSession` / `monitorUnplugPendingMs` — the active follow mode, and ms until a
  pending grace-period unplug fires (`-1` when none pending).
- `userPresence` — the `XR_EXT_user_presence` signal driving the `visible`-mode plug gate:
  `yes`/`no` (donned/doffed) when the runtime supports it, `unknown` before the first presence
  event of a session, `unsupported` otherwise.
- `visible` — the raw session-visibility signal, the other half of the plug gate (which needs
  both): `yes` while VISIBLE/FOCUSED, `no` while a session exists but isn't visible
  (doffed/standby), `n/a` with no session.
- `reprobeWaiting` / `reprobePendingMs` — while dormant in `unavailable`: what the reprobe is
  waiting for (`runtime` | `headset` | `""`) and ms until the next probe (`-1` when none
  armed). `headset` covers both xrGetSystem `FORM_FACTOR_UNAVAILABLE` (stock monado, HMD absent)
  and a reachable-but-degraded runtime (WiVRn service up, headset undonned) — both poll at the
  fixed base cadence, never the grown backoff.
- `reprobeWatching` — whether the event-driven inotify watch on `$XDG_RUNTIME_DIR` is armed
  (`openxr:reprobe_watch`, `$XDG_RUNTIME_DIR` resolvable). When `true`, a socket/pid trigger
  (`monado_comp_ipc`, `wivrn/comp_ipc`, `monado.pid`, `wivrn.pid` created or rewritten) fires a
  probe within ~150ms and `reprobePendingMs` is only the fallback timer.
- `inhibitingIdle` — whether XR currently raises the idle-inhibit bit **right now** (the live
  fold of `idleInhibitMode` with `state` / `visible` / `userPresence`; text form:
  `idle inhibited: yes (mode present)`).
- `idleInhibitMode` — the resolved `openxr:inhibit_idle` mode (`off` | `focused` | `present`),
  after legacy-boolean normalization. Together with `visible` and `userPresence` this makes
  "why is it (not) inhibiting" answerable from one status call.
- `input.left` / `input.right` — each hand's active device (`controllers` or `hands`), the
  grab gesture, and whether the 1-euro carry filter applies to it.
- Per monitor: `id` (Hyprland monitor ID, `-1` if unmapped), `plugged` (backing output
  currently enabled), `contentPath` (diagnostic: what the output is scanning out), `linear`
  (cross-GPU linear buffers active — text form appends `(linear)`), `grabbed`/`grabKind`,
  `hovered`, and the adaptive sub-state (`phase`, `roamMode`, `seatDistM`, `transitionT`).
- `anchor.pose` — for `local`, the world pose; for `head`/`body`/`device`, the configured
  offset — **except while `grabbed` is `true`, when it is always the live world pose**, so a
  status-polling consumer tracks the controller during a grab. `quat` order is `[x, y, z, w]`.

---

## 6. socket2 events

Posted on the compositor's socket2 (`.socket2.sock`), standard `EVENT>>DATA` wire format:

| Event | Payload | Meaning |
|---|---|---|
| `openxrsessionstate` | `disabled`/`unavailable`/`starting`/`idle`/`visible`/`focused`/`stopping` | Every lifecycle transition. |
| `openxractive` | `1` / `0` | Derived: active ⇔ state ∈ {`visible`, `focused`}. Posted only on flip — the "is someone in the headset" signal for bars. |
| `xrmonitoradded` | `<name>` | An XR monitor finished creation (any origin). The stock `monitoradded` also fires for the underlying output. |
| `xrmonitorremoved` | `<name>` | An XR monitor was destroyed (any origin). |
| `xrmonitorselect` | `<name>` | The explicit selection changed via the `select` verb (`select <name>`/`next`/`prev`). Lets a consumer keep its notion of "the selected monitor" push-driven instead of polling `status` after every selection. |
| `xrmonitoranchor` | `<name>,<mode>` | The anchor **mode** changed — the `anchor` verb, or a reload that changed a declared mode. A plain grab/release with an unchanged mode does not fire this. |
| `xrmonitorgrab` | `<name>,1` / `<name>,0` | A grab began / ended on a monitor. |
| `xrmonitorquad` | `<name>,1` / `<name>,0` | A monitor's quad was reactivated / suspended under the runtime layer cap (the monitor keeps rendering as a headless output). |
| `xrmonitorundocked` | `<name>` | An adaptive monitor picked itself up and began following. |
| `xrmonitordocked` | `<name>` | An adaptive monitor re-docked to its desk pose. |

### Bar recipe (polling)

```jsonc
// waybar
"custom/openxr": {
    "exec": "hyprctl -j openxr | jq -r '\"\\(.state) \\(.monitors | length)\"'",
    "interval": 5,
    "format": "XR: {}",
    "exec-if": "hyprctl -j openxr | jq -e '.state != \"disabled\"'"
}
```

`hyprctl -j openxr` is a cheap main-thread snapshot (no XR calls), so polling at 1–5 s is fine.

### Event-driven recipe

```sh
socat -U - "UNIX-CONNECT:$XDG_RUNTIME_DIR/hypr/$HYPRLAND_INSTANCE_SIGNATURE/.socket2.sock" \
  | grep --line-buffered -E '^(openxrsessionstate|openxractive|xrmonitor)' \
  | while IFS='>>' read -r ev _ data; do
        echo "XR event: $ev = $data"   # notify-send, waybar signal, …
    done
```

---

## 7. Idle integration

Nothing to configure for the default behavior: `openxr:inhibit_idle = present` raises the
compositor's normal idle-inhibit bit while the headset is **actually being worn**. hypridle
obeys inhibitors by default via `ext-idle-notify-v1`, so screens don't blank and the lock
doesn't kick in while you're in the headset. Using XR controllers also resets idle timers
exactly like a physical mouse, because the ray pointer injects through the normal input-device
path.

### 7.1 Modes

`openxr:inhibit_idle` takes a **mode**, not a boolean (research/20 phase 2):

| Value | Inhibits while… |
|---|---|
| `off` | never. XR never raises the bit (window rules and the Wayland idle-inhibit protocol still do). |
| `focused` | a session exists **and** it has input focus (`state == focused`). This is the historical behavior: `visible` alone — a runtime dashboard in front, an overlay session with another app focused — deliberately does **not** inhibit. |
| `present` (default) | the headset is **worn**. On a runtime that exposes `XR_EXT_user_presence` (WiVRn does) this means `visible` **and** `userPresence == yes`; on a runtime that does not (stock Monado on an XREAL rig, null/remote drivers) it falls back to exactly the `focused` predicate. |

Why `present` requires **both** visibility and presence: WiVRn's `user_presence` *sticks* at
`yes` while the headset sits doffed in standby (the same finding that shaped the
`monitors_follow_session = visible` plug gate). Presence alone would therefore pin the desktop
awake forever on a doffed headset. Visibility is the reliable doff signal; presence is what
keeps the session-create visibility sprint (WiVRn hits `visible` within ~40 ms, even doffed)
from raising the bit. Both must agree. Read them straight off `hyprctl openxr status`:

```
$ hyprctl openxr status | grep -E 'state|visible|presence|idle'
state: focused
visible: yes
presence: yes
idle inhibited: yes (mode present)
```

`present` is strictly **wider** than `focused` in the worn case: it closes the gap where you
are wearing the headset reading a runtime dashboard (or running in `overlay` mode under a VR
app) and the desktop quietly counts down to a lock behind you.

**Legacy configs.** The variable used to be a bool. Old values still parse, and `true` maps to
`focused`, *not* to the new default — an existing explicit `inhibit_idle = true` keeps exactly
the behavior it opted into:

| Old value | Now means |
|---|---|
| `0`, `false`, `no`, `off`, `none` | `off` |
| `1`, `true`, `yes`, `on` | `focused` |
| anything unrecognized / unset | `present` |

Only a config that never mentioned `inhibit_idle` picks up the widened default. `hyprctl
keyword openxr:inhibit_idle <mode>` applies live (it triggers an idle recheck), and so does a
don/doff edge — the presence event re-folds the bit directly.

**Interaction with WiVRn's own inhibitor.** `wivrn-server` holds a logind
`Inhibit("sleep:idle", …, "block")` for the whole TCP-connected lifetime of a session, and
hypridle honours that regardless of what the compositor does — so while an unpatched WiVRn is
connected, this setting has no observable effect at all (the desktop never idles, donned or
doffed). See `docs/openxr/research/20-wivrn-idle-inhibit.md`: the server-side half (phase 1)
splits that inhibitor and wear-gates only its `idle` component; `inhibit_idle = present` is the
compositor-side belt-and-braces half, and also the *only* protection on rigs with no WiVRn at
all (XREAL over direct Monado).

Want the opposite, or extra behavior? Script off the events:

```sh
# Lock the desktop the moment the headset goes on
socat -U - "UNIX-CONNECT:$XDG_RUNTIME_DIR/hypr/$HYPRLAND_INSTANCE_SIGNATURE/.socket2.sock" \
  | grep --line-buffered '^openxractive>>1' \
  | while read -r _; do loginctl lock-session; done
```

```sh
# Blank physical outputs while in XR, restore on exit
#   openxractive>>1  ->  hyprctl dispatch dpms off
#   openxractive>>0  ->  hyprctl dispatch dpms on
```

---

## 8. Compose over another VR app / hypxrpaper (overlay mode)

By default HypXRland owns the headset view — monitors float over a black void (or over
passthrough with `blend_mode = alpha`). With `openxr:overlay = true` it runs as an
`XR_EXTX_overlay` session and its monitors composite **on top of another OpenXR application**: a
VR game, or `hypxrpaper` for an ambient sky/room backdrop. Requires Monado or WiVRn
(SteamVR-Linux lacks the extension and runs exclusive with a warning).

```ini
openxr {
    enabled   = true
    overlay   = true             # composite over whatever else is running
    overlay_z = 1                # stacking order; higher = on top (primary always beneath)
    # gpu = /dev/dri/renderD128  # pin to the runtime's GPU on multi-GPU boxes
}
```

Notes:

- On Monado an overlay session is held **visible + focused** the whole time the service runs —
  ray input, grab, and idle-inhibit behave exactly as in exclusive mode, and it reaches
  `focused` even with no primary app (monitors over black until one starts).
- **Input is not arbitrated between the two apps.** The runtime delivers controller input to
  the game *and* to HypXRland simultaneously — a trigger pull that clicks a desktop window
  also reaches the game.
- `hyprctl openxr status` reports the real session type (`overlay: yes|no`), so you can
  confirm the runtime accepted it.
- `overlay`/`overlay_z` take effect on the next session start
  (`hyprctl openxr disable && hyprctl openxr enable`).

### hypxrpaper — the ambient-background app

`hypxrpaper` is a small standalone OpenXR client that draws only an ambient backdrop, so
HypXRland's overlay has something to float over. It is a **separate project** (build it from
its own repo; `cmake -B build && cmake --build build` produces `hypxrpaper`) and, like any
OpenXR app, needs `XR_RUNTIME_JSON` pointed at your runtime's manifest.

```
hypxrpaper                              # built-in gradient sky (no args)
hypxrpaper ~/pictures/sky.hdr           # equirectangular panorama (.hdr/.png/.jpg)
hypxrpaper --scene forest-clearing      # a bundled 3D scene (stereo projection layer)
hypxrpaper --scene path/to/model.glb    # any .glb/.gltf, scene.json, or bundled scene name
hypxrpaper --gpu /dev/dri/renderD128    # pin to the runtime's GPU on multi-GPU boxes
```

Recipe:

1. Start the runtime service (Monado: `monado-service`; WiVRn: its server).
2. Start the **primary** app — the thing underneath. For an ambient backdrop that's
   `hypxrpaper`; for a game, launch it normally against the same runtime.
3. Enable HypXRland (`hyprctl openxr enable`, or `openxr:enabled = 1`). Your monitors appear
   over the primary app's scene.

Or let Hyprland autostart the backdrop:

```ini
openxr {
    enabled = true
    overlay = true
}
exec-once = hypxrpaper --scene forest-clearing
# exec-once = hypxrpaper ~/pictures/panoramas/sunset.hdr   # a panorama instead
# exec-once = hypxrpaper                                    # gradient sky instead
```

`hypxrpaper` inherits Hyprland's environment; if your session doesn't export
`XR_RUNTIME_JSON`, prefix it:
`exec-once = env XR_RUNTIME_JSON=/path/to/runtime.json hypxrpaper --scene forest-clearing`.

---

## 9. Known limitations

- **Classic-config only.** `openxr { }` and `xrmonitor =` have no Lua config binding.
- **`khr/simple_controller` (no analog squeeze) cannot grab a monitor** — select-click works,
  grab-to-move does not on that profile.
- **Roll is not representable** in `xrmonitor=` / `hyprctl openxr layout` serialization
  (yaw + pitch only) — a monitor that picked up roll during a grab loses it when
  re-serialized.
- **No stereo/3D content.** Every XR monitor is a flat quad; there is no per-eye rendering.
- **Overlay input is unarbitrated** (§8).
