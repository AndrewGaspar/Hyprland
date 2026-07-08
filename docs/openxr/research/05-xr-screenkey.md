# Design: "screenkey for OpenXR" — a head-locked keystroke display

Design memo (2026-07-07). **No implementation.** Proposes a new standalone
OpenXR utility, a sibling to `hypxrpaper`, that displays the user's keystrokes as
a head-locked composition layer, composited alongside HypXRland's monitor quads
and hypxrpaper's ambient background. The user reviews this before any code.

Evidence base: the vendored Monado tree at `subprojects/monado` (pinned
`c2ddab59d`, the exact runtime the test suite runs against); `hypxrpaper`
(`/home/ajg/code/hypxrpaper`); HypXRland's `src/openxr/`; and web research on
`wshowkeys` / `showmethekey` / `screenkey` (URLs cited inline, sourced 2026-07).

---

## TL;DR

1. **Yes, it composites cleanly.** Monado's multi-client compositor accepts **up
   to 64 concurrent overlay sessions** over one primary, sorted purely by
   `sessionLayersPlacement → xrt_session_info.z_order` (ascending; primary pinned
   to `INT64_MIN`). WiVRn bundles the *same* Monado compositor, so a third overlay
   client (hypxrpaper primary + HypXRland overlay + this tool overlay) works on
   the real Quest 3 too. Give each overlay a **distinct** placement — ties are
   `qsort`-indeterminate.
2. **Head-lock is native and free.** Monado has an explicit code path: a
   `XrCompositionLayerQuad` whose `space` is a `XR_REFERENCE_SPACE_TYPE_VIEW`
   reference space is treated as head-locked with zero per-frame math on our side.
   No `xrLocateSpace` leash needed for v1.
3. **Global opacity + fade are native and free too.** Monado enables
   `XR_KHR_composition_layer_color_scale_bias` **by default**; chaining
   `XrCompositionLayerColorScaleBiasKHR` with `colorScale.a = opacity` gives a
   per-frame global-alpha/fade knob without ever re-uploading the texture.
4. **Key capture is the one genuinely hard part.** Wayland deliberately gives no
   client global key events. The clean modern path is **libinput on a logind seat**
   (no setuid, no `input`-group hack), exactly what `showmethekey`/`wshowkeys`
   evolved toward. Keycode→label via **libxkbcommon** with the user's layout.
5. **Recommended name: `hypxrkeys`** (see §6). New standalone BSD-3 repo, same
   shape as hypxrpaper (single-threaded, EGL/GBM, `--gpu` pin, stb vendored).
6. **v1 scope:** panel of recent keystrokes, chords, coalescing, timeout fade,
   opacity, z-order, position/size, mods-only. Deferred: mouse, per-app secure
   masking beyond a global hotkey, IME/text, the HypXRland "enhanced mode" IPC.

---

## Architecture sketch

```
                          ┌─────────────────────────────────────────────┐
                          │            Monado / WiVRn compositor          │
                          │        (multi-client, sorts by z_order)       │
                          └───────────────▲───────────────▲──────────────┘
   z_order (bottom→top)                   │               │
                                          │ xrEndFrame     │ xrEndFrame
   INT64_MIN  primary  ───────────────────┼──────────┐    │
   (e.g. 10)  overlay  ───────────────┐   │          │    │
   (e.g. 20)  overlay  ──────────┐    │   │          │    │
                                 │    │   │          │    │
        ┌────────────────────────┴──┐ │   │  ┌───────┴────┴──────────┐
        │  hypxrkeys (THIS TOOL)    │ │   │  │  hypxrpaper (primary) │
        │  overlay session, z=20    │ │   │  │  equirect2 / glTF     │
        │  1× Quad in VIEW space    │ │   │  └───────────────────────┘
        │  head-locked, opacity α   │ │   │
        │  ColorScaleBias fade      │ │ ┌─┴──────────────────────────┐
        │                           │ │ │  HypXRland (overlay, z=10) │
        │  ┌─────────────────────┐  │ │ │  monitor quads in LOCAL    │
        │  │ libinput (logind    │  │ │ └────────────────────────────┘
        │  │  seat) → evdev keys │  │ │
        │  │ → xkbcommon → label │  │ │        Each app is an independent
        │  └─────────────────────┘  │ │        OpenXR process. They never
        │  → stb_truetype atlas     │ │        talk to each other; the
        │  → RGBA swapchain (static │ │        runtime merges their layers.
        │     re-upload on change)  │ │
        └───────────────────────────┘ │
                                       │
    single-threaded main loop: poll libinput → rebuild text on change →
    upload to swapchain → submit head-locked quad every xrEndFrame
    (EGL context held current for the whole session — hypxrpaper's fence rule)
```

Key property inherited from hypxrpaper: **single-threaded, EGL context held
current across the whole session**, which satisfies Monado's GL-fence contract by
construction (HypXRland commit `95c541a8`). All OpenXR + GL + libinput pumping
happens on one thread.

---

## The 7 design decisions

### Decision 1 — Session model: a second overlay session

**Options considered**

- **(A) Overlay session over the primary, distinct `sessionLayersPlacement`.**
- (B) A single process that *is* both hypxrpaper and screenkey (fewer sessions).
- (C) Draw the keystrokes as an extra HypXRland monitor-like quad (couples to fork).

**Recommendation: (A).** hypxrkeys opens its own `XrSession` with
`XrSessionCreateInfoOverlayEXTX` chained into `xrCreateSession`, exactly like
HypXRland's `src/openxr/XRSession.cpp:179-186`, with `createFlags = 0` and
`sessionLayersPlacement` set from `--overlay-z` (default above HypXRland's).

**Rationale + verified mechanism (Monado `c2ddab59d`):**

- `sessionLayersPlacement` is copied verbatim into `xrt_session_info.z_order`:
  `subprojects/monado/src/xrt/state_trackers/oxr/oxr_session.c:1447-1454`
  (`xsi.is_overlay = true; xsi.z_order = overlay_info->sessionLayersPlacement;`).
- The compositor sorts every visible+active client by that value:
  `overlay_sort_func` (ascending `int64_t` compare) at
  `src/xrt/compositor/multi/comp_multi_system.c:211-227`, applied by the `qsort`
  in `transfer_layers_locked` at `comp_multi_system.c:306`. **Lowest = bottom,
  highest = top.**
- The IPC server pins the **primary** app to `z_order = INT64_MIN` and marks every
  **overlay** session `visible=true, focused=true` with its own placement:
  `src/xrt/ipc/server/ipc_server_process.c:430-461`. So overlays are always
  FOCUSED — matching HypXRland's FOCUSED-gated behavior and doc `01`.
- **N overlays coexist**, bounded only by `MULTI_MAX_CLIENTS = 64`
  (`comp_multi_private.h:33`); clients beyond that are silently dropped at insert
  (`comp_multi_compositor.c:1024-1032`). Three concurrent clients is trivial.
- **Tie-break is indeterminate**: equal z_order → `overlay_sort_func` returns 0
  and `qsort` is not stable. **Design rule: hypxrkeys must default its
  `--overlay-z` to a value distinct from HypXRland's `openxr:overlay_z`** (e.g.
  HypXRland uses some value V; hypxrkeys defaults to V+10 so the key display sits
  above the monitors). Document both defaults so they don't collide.

**WiVRn risk — resolved to LOW.** WiVRn 26.6.1 vendors the *same* Monado
multi-compositor: the client lib `/usr/lib/wivrn/libopenxr_wivrn.so` exports
`XR_EXTX_overlay` / `XR_TYPE_SESSION_CREATE_INFO_OVERLAY_EXTX`, and its bundled
server source (`/usr/src/debug/wivrn-server/.../monado-src/`) has the identical
`overlay_sort_func`, the identical "all overlays always active and focused" policy
(`ipc_server_process.c:447-448`), and `MULTI_MAX_CLIENTS = 64`. So N overlays over
one primary should behave the same on the Quest 3.
*Cheap live confirmation:* with `wivrn-server` up and the headset connected, run a
primary + **two** overlay clients with distinct placements (e.g. 10 and 20),
confirm both reach FOCUSED and composite, then swap the two values and confirm the
stacking flips. (Only one `wivrn-server` binds at a time — serialize live runs, per
MEMORY.)

---

### Decision 2 — Key input capture on Wayland

**The constraint.** Wayland has *no* protocol for a normal client to receive
global key events; this is an intentional anti-keylogger design. `wlr-layer-shell`
gives surface *placement* but not global input, and we aren't even a Wayland
surface here — we're an OpenXR client with no `wl_keyboard`. So capture must come
from below Wayland (evdev) or from the compositor's cooperation.

**Options considered**

- **(A) libinput on a logind seat** — open input devices via the systemd-logind
  D-Bus `TakeDevice`/session seat, let libinput read `/dev/input/event*`, and
  translate with xkbcommon. No setuid, no `input`-group membership; works for the
  user's active graphical session.
- **(B) libinput via direct `/dev/input` path interface** — open the device nodes
  directly with libinput's `path` backend. Needs read access: either the `input`
  group or a small setuid helper. Simpler code, worse security posture.
- **(C) A setuid-root helper that reads evdev and pipes sanitized events to an
  unprivileged UI** — the `showmethekey` architecture.
- **(D) HypXRland "enhanced mode": the fork emits keysyms over a socket** —
  privacy-scoped (keysym-only, no text), but couples the tool to our fork.
- (E) Wayland protocols — *ruled out*, none exist for global keylogging by design.

**Recommendation: (A) libinput on a logind seat as the primary mechanism, with
(B) `input`-group fallback**, structured as a **privilege-separated reader** so the
OpenXR/EGL half never runs elevated, and **(D) offered as an optional, opt-in
enhanced mode** — which is, notably, the *cleanest* option and the one the tools
research explicitly recommended for a tool that already controls its compositor.

**Rationale + how the reference tools actually do it (verified with citations):**

- `wshowkeys` (original `git.sr.ht/~sircmpwn/wshowkeys`, UNMAINTAINED; forks
  `ammen99`/`DreamMaoMao`) reads Linux **evdev** under `/dev/input` and ships
  **setuid root**, dropping privileges immediately after opening the devices. Its
  README is the canonical note: *"wshowkeys must be configured as setuid during
  installation. It requires root permissions to read input events. These
  permissions are dropped after startup."* `wlr-layer-shell` is only used for
  window *placement* — it grants **no** input. Forks use **libinput + xkbcommon**.
  This is the crude end of option (B)/(C) — avoid setuid-on-the-whole-binary.
- `showmethekey` (`AlynxZhou/showmethekey`) is the clean privilege-separated model:
  a small privileged backend `showmethekey-cli` (a rewrite of libinput's
  `debug-events`, on **libinput + libudev + libevdev**) reads evdev and emits **one
  JSON object per event** on stdout (`{"event_name":"KEYBOARD_KEY","key_code":46,
  "state_name":"PRESSED",...}`); an unprivileged GTK UI parses the pipe. Crucially
  it escalates via **`pkexec`/polkit, NOT setuid** (Wayland toolkits refuse to run
  a GUI under `sudo`), and translates with **libxkbcommon**. This is the shape to
  copy for our privileged reader → unprivileged XR renderer split.
- `screenkey` (`wavexx/screenkey`, X11) captures via the X **RECORD** extension
  (`libXtst`: `XRecordCreateContext`/`XRecordEnableContextAsync` in
  `Screenkey/xlib.py`) — **not** XInput2 — and its README states it **cannot
  capture native Wayland clients** (only X11/XWayland). Not a viable capture path
  here, but its *UX* is the reference (chords, coalescing, timeout, mods-only,
  manual stealth mute; see §3).

Why (A) over (C) for a *new* tool in 2026: logind already brokers device access
for the user's session — `libinput_udev_create_context(seat0)` opens `/dev/input`
fds through logind's `TakeDevice` because we own the session: **no setuid, no
persistent root helper, no `input`-group grant.** It fails gracefully outside a
real session (headless CI), where we fall back to (B) behind an explicit
`input`-group requirement, and ultimately to (D). We do **not** ship a setuid
binary by default. Regardless of source, keep the libinput reader in a separable
unit so a future privileged-helper split (showmethekey-style) is a small change,
not a rewrite.

**Enhanced mode (D) — the cleanest option, optional.** The tools research flags
"compositor cooperation" as the *only* route that sidesteps device-access privilege
**and** fixes the layout-divergence correctness gap (§Decision 3) — and we already
control the compositor. HypXRland owns the keyboard, already runs xkb translation
(`src/managers/KeybindManager.cpp:368` uses `xkb_state_key_get_one_sym` with the
*authoritative* keymap), and already has a `socket2` event bus
(`src/managers/EventManager.cpp`) plus a hotkey protocol. A future, *opt-in*
HypXRland feature could emit **keysym-only** events (never UTF-8 text, never
password-field content) on a dedicated privacy-scoped socket that hypxrkeys reads
with `--source hyprland`. This removes hypxrkeys' need for any device access, gives
correct layout for free, and lets the *compositor* enforce secure-input suppression
centrally (it can see focused-app hints a raw evdev reader never can). The reason it
is **not** the v1 default: it couples the tool to our fork, whereas libinput keeps
hypxrkeys a compositor-agnostic sibling of hypxrpaper. Deferred past v1, but the CLI
should reserve `--source {libinput,hyprland}` (default `libinput`).

Note the Wayland reality is confirmed: there is **no** standard protocol for a
client to receive global keys (intentional anti-keylogger design — clients get keys
only for their own focused surfaces). Existing "Wayland keyloggers" work only by
`LD_PRELOAD` inside the victim process or `ptrace` on the compositor, i.e. not a
client-facing API. So evdev-from-below or compositor-cooperation are the only honest
routes.

**Security / privacy posture (v1).** This is, by construction, a benign keylogger.
Sane defaults:
- **A global toggle hotkey** (default e.g. `--toggle-key` bound to a chord) that
  suspends capture+display instantly — the `screenkey` "off switch."
- **`--mods-only`**: show only modifier chords and non-character keys (never the
  letters/digits you type) — the discreet mode for demos.
- **`--no-secret` heuristic (stretch, opt-in):** when a hint of secure input is
  detectable, blank the display. On pure Wayland a client can't see focused-app
  password fields; the honest v1 story is "use the toggle hotkey / mods-only, and
  prefer enhanced mode (D) later where the compositor can mask." Do **not**
  over-promise password detection in v1.
- **No persistence:** never write captured keys to disk; the display buffer is
  in-memory and time-bounded by `--timeout`.
- README must state plainly that it reads global input and how (device access),
  so users make an informed choice — mirroring `showmethekey`'s disclosure.

---

### Decision 3 — Keymap translation (evdev keycode → label)

**Mechanism.** libinput delivers **evdev keycodes**; translate with
**libxkbcommon**: compile an `xkb_keymap` from the user's RMLVO, keep an
`xkb_state`, feed each event to `xkb_state_update_key` (tracks modifier
latch/lock), and read `xkb_state_key_get_one_sym` + `xkb_state_key_get_utf8` for
the label. This is exactly what HypXRland does internally
(`KeybindManager.cpp:368-369`), so the pattern is proven in-tree. **Gotcha:**
xkbcommon expects keycodes offset by **+8** from evdev
(`xkb_keycode = evdev_code + 8`) — a classic off-by-8 bug source.

- **Getting the active layout without the compositor — the one real correctness
  gap.** A headless evdev client gets no `wl_keyboard` keymap, so it must be told
  the RMLVO. Options, in priority order:
  1. `XKB_DEFAULT_RULES/_MODEL/_LAYOUT/_VARIANT/_OPTIONS` env vars —
     `xkb_keymap_new_from_names` with an empty RMLVO reads these by default;
     covers most single-layout setups.
  2. `--layout us`, `--variant`, `--options` CLI flags to override.
  3. (enhanced mode) receive the authoritative keymap from HypXRland later.
  Default: build from env (rules `evdev`), which Just Works for the common case.
  **The gap:** if the user set their layout via Hyprland's `input:kb_layout`
  config rather than `XKB_DEFAULT_*`, a standalone evdev reader's layout can
  *silently diverge* from the compositor's — labels would be wrong. This is the
  strongest correctness argument for enhanced mode (D); for v1, document that
  `XKB_DEFAULT_LAYOUT` must match, or pass `--layout`.
- **Chord rendering** (`SUPER+SHIFT+E`): track the current modifier mask from
  `xkb_state` and, on a non-modifier press, render the held mods as prefixes
  (`Super`, `Shift`, `Ctrl`, `Alt`) joined by `+`. Standalone modifier presses can
  optionally be shown (mods-only mode) or suppressed.
- **Key-repeat suppression.** evdev auto-repeat arrives as repeated press events
  (value 2 on the raw device, or libinput repeats); suppress repeats so a held key
  doesn't spam. Coalesce identical consecutive keys into `a ×3` style counts (the
  `screenkey` behavior).
- **Coalescing window.** A short idle gap (e.g. 1 s) starts a new "line"; within a
  burst, characters concatenate into a running string like a caption.

---

### Decision 4 — Rendering: head-locked quad + baked text atlas

**Head-lock: VIEW-space quad (verified native).**

- Create a `XR_REFERENCE_SPACE_TYPE_VIEW` reference space (HypXRland already makes
  one: `XRSession.cpp:214-215`). Submit a single `XrCompositionLayerQuad` with
  `layer.space = viewSpace`.
- Monado has an **explicit head-lock path**: `handle_space` in
  `src/xrt/state_trackers/oxr/oxr_session_frame_end.c:1207-1215` — *"poses in view
  space are already in the space the compositor expects"* — so a VIEW-space quad is
  head-locked with **zero per-frame `xrLocateSpace` on our side**. `verify_space`
  (`oxr_session_frame_end.c`) accepts any non-null space, so quads in VIEW space
  are valid input, contrary to the LOCAL-only habit in hypxrpaper/HypXRland.
- **Comfort note.** A rigidly head-locked HUD can feel "stuck to your eyeballs."
  v1 ships the simple fixed VIEW-space quad (it's a transient keystroke caption,
  not persistent chrome, so rigidity is acceptable and simplest). A later comfort
  option: recompose a LOCAL-space pose each frame from `xrLocateSpace(VIEW)` with a
  smoothing/leash so it lags slightly — deferred (§v1 scope).

**Global opacity + fade: `XrCompositionLayerColorScaleBiasKHR` (verified native).**

- Monado enables `XR_KHR_composition_layer_color_scale_bias` **by default**
  (`subprojects/monado/CMakeLists.txt:429`,
  `XRT_FEATURE_OPENXR_LAYER_COLOR_SCALE_BIAS ON`); the frame-end path reads it and
  applies `colorScale`/`colorBias` per layer
  (`oxr_session_frame_end.c:206-223`, called for quad layers at line 1279).
- Chain it into the quad's `next`: `colorScale = {1,1,1, opacity*fade}`,
  `colorBias = {0,0,0,0}`. This gives **global transparency (`--opacity`) and the
  idle fade animation** as a cheap per-frame scalar — **no texture re-upload for
  fades**. Gate on `hasExt(...color_scale_bias)`; if absent (unlikely on
  Monado/WiVRn), fall back to baking opacity into the texture alpha.

**Text texture: stb_truetype baked atlas → one RGBA swapchain.**

- hypxrpaper already vendors `stb_image.h`; add `stb_truetype.h` (same vendoring
  pattern, `third_party/`, `-w` on its impl TU). Bake a glyph atlas once at
  startup from `--font` (default: a bundled monospace, or a system font path).
- Render the current caption string into a CPU RGBA buffer (dark rounded
  background box + light glyphs, or transparent bg + outlined glyphs), then
  `glTexSubImage2D` into the swapchain image — **only when the text changes**
  (exactly hypxrpaper's "upload once, re-present every frame" idle-monitor trick,
  `Session.cpp:216-255`). Static frames cost nothing; xrEndFrame just re-submits.
- **Alpha.** Use **premultiplied alpha** in the texture and set
  `XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT` in `layer.layerFlags` so
  the transparent regions show the world/monitors behind. (HypXRland forces
  alpha=1 to avoid XRGB garbage-alpha holes; here alpha is *intentional* and must
  be authored correctly.) Do **not** rely on `XrCompositionLayerAlphaBlendFB` —
  it's an FB vendor extension, not portable; the color-scale-bias `.a` handles
  global opacity portably.
- **Re-upload cadence.** Re-bake+upload on: new keystroke, coalesce-count change,
  fade tick (only if we bake fade into alpha — we don't, color-scale-bias handles
  fade), or timeout clear. In practice: upload on text change only. A single
  swapchain image, acquired/released around each upload (or a small ring if the
  runtime needs it).

**Swapchain sizing.** Fixed texture (e.g. 1024×256, or sized to `--size` at a DPI
that keeps glyphs crisp). The physical size in meters is set by the quad's
`size` field, independent of texture resolution.

---

### Decision 5 — CLI / config surface (hypxrpaper conventions)

hypxrpaper parses argv by hand (`main.cpp:142-174`), uses `--gpu <path>` for the
dual-GPU pin, positional file args, and env overrides. Mirror that:

```
hypxrkeys [options]

  --opacity <0..1>       Global layer opacity (default 0.85). Applied via
                         ColorScaleBias.a.
  --position <x,y,z>     VIEW-space offset of the panel centre, metres
                         (default 0,-0.25,-1.0 = slightly down & 1 m ahead,
                         the screenkey "bottom-centre" analogue).
  --size <w[,h]>         Panel size. Suffix 'm' = metres (e.g. 0.5m), 'd' =
                         degrees of FOV; default 0.4m wide, height from aspect.
  --timeout <s>          Fade the panel out after N seconds idle (default 3;
                         0 = never fade).
  --font <path|name>     TTF/OTF for the glyph atlas (default: bundled mono).
  --font-size <px>       Atlas pixel size (default 48).
  --overlay-z <int>      sessionLayersPlacement (default: HypXRland's + 10, so
                         keys sit above the monitors). MUST differ from
                         HypXRland's openxr:overlay_z.
  --mods-only            Show only modifier chords / non-character keys.
  --toggle-key <chord>   Hotkey to suspend/resume capture (default e.g.
                         Super+Shift+K). Recognised from the captured stream.
  --source {libinput,hyprland}   Input source (default libinput; hyprland =
                         future enhanced mode, §Decision 2).
  --layout/--variant/--options   Override the xkb keymap (default: env).
  --gpu <path>           DRM render node, same semantics as hypxrpaper --gpu.
  --mouse                (stretch) also show mouse button presses.
  -h, --help
```

**Fade animation — where.** At the **layer level** via
`colorScale.a = opacity * fadeFactor`, recomputed each frame from a
`steady_clock` timer since the last key. No texture work per fade tick — this is
strictly better than re-baking alpha. On a new key, snap `fadeFactor` to 1 and
restart the timer; ramp down after `--timeout`.

**Config file.** v1 is CLI-only (like hypxrpaper). A `~/.config/hypxrkeys.conf`
mirroring the flags is a trivial later add; not needed for v1.

---

### Decision 6 — Name

Family: `hyprland` → `hypxrland`, `hyprpaper` → `hypxrpaper` (the `xr` infix on the
`hypr`/`hyprland` root). Candidates:

- **`hypxrkeys`** — parallels `hypxrpaper`, short, unambiguous. **Recommended.**
- `hypxrscreenkey` — most descriptive, but long and awkward to type.
- `hypxrkey` (singular) — fine but `hypxrkeys` reads better as "shows your keys."

**Recommendation: `hypxrkeys`**, in a **separate BSD-3 repo**
(`github.com/AndrewGaspar/hypxrkeys`), same layout as hypxrpaper (`src/`,
`third_party/`, `CMakeLists.txt`, `Log`/`Egl`/`Session` reused nearly verbatim).

---

### Decision 7 — Testing

Two tiers, mirroring the existing XR test story.

**(a) Standalone headless/nested preview** (like `preview-xr.sh`):

- Extend the desktop preview so hypxrkeys can be launched as a **third overlay
  client** alongside monado-service + hypxrpaper + nested HypXRland (add e.g.
  `preview-xr.sh --keys`, discovering `$HYPXRKEYS_BIN` → PATH, passing the same
  `--gpu` pin, and a distinct `--overlay-z`). Visual confirmation: the key panel
  floats over the monitors in the Monado window.
- **Synthetic key injection for headless CI:** create a **uinput** virtual
  keyboard (`/dev/uinput`) and emit scripted `EV_KEY` events; libinput picks it up
  from the seat exactly like a real device. This lets a test type a known string
  and assert the tool captured it (e.g. hypxrkeys logs the coalesced caption to
  stderr in a `--selftest`/`HYPXRKEYS_DUMP` mode, matching hypxrpaper's
  `HYPXRPAPER_DUMP_FRAME` convention). uinput needs the same device access as
  reading evdev (logind seat or `input` group) — gate the test on it and SKIP
  otherwise (the harness's established "SKIP on missing capability" pattern).

**(b) HypXRland hyprtester integration hook** (optional, opt-in), following
`hyprtester/src/tests/xr/overlay.cpp`:

- A new `xr_keys_composition` test, gated on `$HYPRTESTER_HYPXRKEYS`, that brings
  up hypxrkeys as a second overlay over the suite's session (or over
  hypxrpaper+HypXRland), asserts hypxrkeys reaches FOCUSED and its layer
  composites (it's a standalone process, so the assertion is "it stays alive +
  reaches a presenting state" via its own status/log, not HypXRland's `j/openxr`).
  Same RAII guard + PID-tracked kill + SKIP-on-env-instability discipline as
  `overlay.cpp`. Because it's a third client, mind `MULTI_MAX_CLIENTS` (fine) and
  distinct placements.
- The self-contained uinput+capture test (a) is the higher-value one; (b) mainly
  proves three-way composition doesn't regress.

**Process-cleanup rule (MEMORY, CRITICAL):** any live-run test/script kills the
tool **only by tracked PID** or full path — never `pkill hypxrkeys`-style by bare
name that could match unrelated processes, and never anything resembling
`pkill Hyprland` (host-session hazard).

---

## Privacy section (consolidated)

This tool captures **all** global key input while running — treat it as a
disclosed, benign keylogger:

- **Disclosure:** README states, up top, that it reads global input via evdev and
  what access that needs.
- **Off switch:** `--toggle-key` suspends capture+display instantly (default on).
- **Discreet mode:** `--mods-only` never renders typed characters.
- **No persistence:** in-memory, time-bounded buffer; nothing written to disk.
- **Least privilege:** default to logind-seat libinput (no setuid, no root helper);
  `input`-group fallback is explicit; a setuid helper is *not* shipped by default.
- **Future central masking:** enhanced mode (§Decision 2 option D) moves capture
  into HypXRland, which can (a) send keysym-only data and (b) suppress display
  around password fields it knows about — the correct long-term privacy design,
  but deferred.
- **Honest v1 limitation:** a pure-Wayland client cannot see other apps'
  password-field focus, so v1 does **not** claim automatic secret masking — it
  offers the toggle + mods-only instead.

---

## v1 scope cut (explicitly deferred)

**In v1:** overlay session with distinct z; head-locked VIEW-space quad; global
opacity + idle-timeout fade via color-scale-bias; stb_truetype caption of recent
keystrokes; chord rendering; repeat suppression + `×N` coalescing; `--mods-only`;
`--toggle-key`; libinput-on-logind-seat capture with `input`-group fallback;
xkb-from-env keymap with CLI override; `--gpu` pin; `preview-xr.sh --keys`;
uinput selftest.

**Deferred:**

- Mouse-button display (`--mouse` reserved, not implemented).
- HypXRland "enhanced mode" IPC source (`--source hyprland` reserved).
- Automatic password-field / secure-input masking.
- Body/wrist-locked or leashed (smoothed) placement modes; grabbable repositioning.
- IME / dead-key / compose / full UTF-8 text reconstruction beyond single keysyms.
- Config file, theming beyond font/size/opacity, per-key colouring.
- OpenVR/SteamVR backend (same story as doc `01`: out of scope, separate project).

---

## Work-package breakdown (one subagent each)

Sized like the HypXRland WPs; each is independently reviewable with a crisp
acceptance test. Critical path: **K1 → K2 → K3 → (K4 ∥ K5) → K6 → K7 → K8**.

- **WP-K1 — Repo skeleton + XR overlay session bring-up.**
  New `hypxrkeys` repo (BSD-3), CMake + `Log`/`Egl` copied from hypxrpaper, a
  `Session` that opens an **overlay** session (`XrSessionCreateInfoOverlayEXTX`,
  `--overlay-z`), one VIEW-space reference space, and submits an empty/solid test
  quad. *Accept:* reaches FOCUSED under vendored Monado as a second overlay over a
  running primary; quad visible in the Monado window; `--gpu` honored.

- **WP-K2 — libinput capture on a logind seat (+ input-group fallback).**
  Bring up libinput (udev+logind seat; fallback path backend), pump `EV_KEY`,
  emit raw keycode+state events to an internal queue. Clean teardown. *Accept:* a
  `--selftest` prints each pressed keycode; works from a real session; SKIPs
  cleanly with a clear message when no device access.

- **WP-K3 — xkbcommon keymap + label/chord model.**
  Compile keymap from env (+ `--layout/--variant/--options`), track `xkb_state`,
  produce labels, modifier chords, repeat suppression, `×N` coalescing, the
  caption/line model, and the `--mods-only` filter + `--toggle-key` suspend.
  *Accept:* unit-level: a scripted keycode sequence yields the expected caption
  strings (incl. `Super+Shift+E`, `a ×3`); mods-only hides characters.

- **WP-K4 — Text rendering (stb_truetype atlas → RGBA swapchain).**
  Vendor `stb_truetype.h`; bake atlas from `--font`; render caption to a CPU RGBA
  buffer (bg box + glyphs, premultiplied alpha); upload-on-change into the
  swapchain; wire `--size`, texture DPI. *Accept:* `HYPXRKEYS_DUMP` writes a PPM of
  the panel showing a known string; upload happens only on text change.

- **WP-K5 — Opacity + fade via color-scale-bias, positioning.**
  Chain `XrCompositionLayerColorScaleBiasKHR` (feature-detect; texture-alpha
  fallback); `--opacity`; idle `--timeout` fade ramp at layer level; `--position`.
  `XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT`. *Accept:* panel fades out
  after timeout and snaps back on keypress; `--opacity 0.5` visibly semi-transparent
  over hypxrpaper; no texture re-upload during fade.

- **WP-K6 — CLI/UX polish + README + privacy disclosure.**
  Full argv parser (hypxrpaper style), `--help`, defaults, the privacy section in
  README, distinct-`overlay-z` guidance vs HypXRland. *Accept:* `--help` documents
  every flag; bad args exit 2 with a message; README states the capture model.

- **WP-K7 — `preview-xr.sh --keys` (three-way composition).**
  Extend HypXRland's preview to launch hypxrkeys as a third overlay
  (`$HYPXRKEYS_BIN`, `--gpu` pin, distinct `--overlay-z`). *Accept:* running
  `preview-xr.sh --env pano --keys` shows monitors + ambient bg + key panel all
  composited, correct z-order.

- **WP-K8 — Tests: uinput selftest + optional hyprtester hook.**
  A uinput-driven capture test (SKIP without device access); optional
  `xr_keys_composition` in hyprtester gated on `$HYPRTESTER_HYPXRKEYS`, mirroring
  `overlay.cpp`. *Accept:* uinput test types a known string and asserts the
  captured caption; hyprtester hook reaches three-way FOCUSED composition or SKIPs.

---

## Open questions for the user

1. **Repo:** confirm a new standalone `hypxrkeys` repo (vs. living in the
   hypxrpaper repo as a second binary). Recommendation: separate, like hypxrpaper.
2. **Name:** `hypxrkeys` OK, or prefer `hypxrscreenkey` / other?
3. **Default placement:** what is HypXRland's current default `openxr:overlay_z`,
   so hypxrkeys can default to *above* it? (I'll set default = that + 10.)
4. **Enhanced mode priority:** is the HypXRland keysym-over-IPC source (option D)
   something you want scoped now as a follow-up WP, or parked indefinitely?
5. **Secure-input masking:** acceptable for v1 to ship only the toggle hotkey +
   mods-only (no automatic password masking), documenting the limitation?
6. **Capture access default:** OK to require a logind session (normal desktop use)
   and treat `input`-group as the documented fallback, shipping **no** setuid
   binary?
7. **Head-lock feel:** ship the simple rigid VIEW-space quad for v1, with a
   smoothed/leashed mode deferred — acceptable?

---

## Sources

**Monado / OpenXR (vendored `subprojects/monado` @ `c2ddab59d`):**
`oxr_session.c:1447-1454` (placement→z_order), `comp_multi_system.c:211-227,306`
(`overlay_sort_func` + qsort), `ipc_server_process.c:430-461` (overlay/primary
z_order+visibility policy), `comp_multi_private.h:33` (`MULTI_MAX_CLIENTS 64`),
`oxr_session_frame_end.c:1207-1215` (VIEW-space head-lock path), `:206-223,1279`
(color-scale-bias fill), `monado/CMakeLists.txt:429`
(`XRT_FEATURE_OPENXR_LAYER_COLOR_SCALE_BIAS ON`). WiVRn 26.6.1:
`/usr/lib/wivrn/libopenxr_wivrn.so` exports `XR_EXTX_overlay`; bundled monado
source under `/usr/src/debug/wivrn-server/.../monado-src/` has the identical stack.

**On-screen key tools (web, 2026-07):**
- wshowkeys — https://git.sr.ht/~sircmpwn/wshowkeys ,
  https://github.com/DreamMaoMao/wshowkeys , https://wiki.archlinux.org/title/Keyboard_input
  (setuid evdev reader; libinput+xkbcommon; layer-shell = placement only).
- showmethekey — https://github.com/AlynxZhou/showmethekey (+ README): privileged
  libinput/libevdev CLI → JSON pipe → unprivileged GTK UI; escalates via
  `pkexec`/polkit, not setuid; xkbcommon.
- screenkey — https://github.com/wavexx/screenkey (README.rst,
  `Screenkey/xlib.py`), issue #61: X RECORD via libXtst; no native Wayland; UX =
  chord styles, mods-only, repeat compression (`×N`), auto-hide/persistent, manual
  stealth mute (no automatic password detection).
- Wayland keylogging reality —
  https://dec05eba.com/2021/09/19/x11-security-preventing-global-keylogging/ ,
  https://github.com/Aishou/wayland-keylogger ,
  https://github.com/schauveau/sway-keylogger (no client-facing global-key API).
- xkbcommon — https://xkbcommon.org/doc/current/md_doc_2quick-guide.html
  (`xkb_state_key_get_one_sym`/`_get_utf8`, `+8` keycode offset, `XKB_DEFAULT_*`).

**HypXRland / hypxrpaper (in-tree):** `src/openxr/XRSession.cpp:179-186` (overlay
create), `:214-215` (VIEW space), `KeybindManager.cpp:368-369` (xkb translate),
`EventManager.cpp` (socket2 bus); hypxrpaper `src/Session.cpp:216-255` (upload-once
swapchain), `src/Egl.cpp` (GBM/`--gpu` node), `src/main.cpp:142-174` (argv style);
`scripts/preview-xr.sh`, `hyprtester/src/tests/xr/overlay.cpp` (overlay test model).
```
