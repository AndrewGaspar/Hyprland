# 07 — XREAL Air 2 Ultra as a 3DoF OpenXR display (WP-XR1)

Running the **XREAL Air 2 Ultra** birdbath glasses (USB `3318:0426`) as a **3DoF, seated,
orientation-only OpenXR display** driven by the vendored Monado's `xreal_air` driver, with an
in-session **toggle** between XR mode and ordinary flat head-locked use of the glasses' DisplayPort
output. This is the operational companion to the research memo
[`research/XREAL-3DOF.md`](research/XREAL-3DOF.md), which carries the full evidence and rationale.

**Scope (v1):** 3DoF (no positional tracking), stereo **SBS 3840×1080** with a **mono 1920×1080
fallback**, keyboard + gaze + voice input (no controllers, no hand tracking — the driver exposes an
HMD only), no compositor patches beyond the `openxr:runtime_json` selector. **Out of scope:** 6DoF
(the stereo cameras / Basalt VIO path), DRM-lease direct mode.

The pieces:

| Piece | What | Where |
|---|---|---|
| `openxr:runtime_json` | compositor key that picks the runtime manifest for the session (backs the toggle) | built in; docs/openxr/05-configuration.md |
| xreal Monado flavor | a second Monado build: `xreal_air` driver + real `comp_main` + Wayland backend | `scripts/build-monado.sh --xreal` → `subprojects/monado/build-xreal` |
| udev rule | hidraw access for the `3318:*` HID interfaces | `contrib/xreal/70-xreal.rules` |
| `xreal-ctl` | standalone HID helper (mode 2d/3d, brightness) for the flat side | `contrib/xreal/` |
| systemd unit | user unit for the xreal Monado runtime (à la wivrn) | `contrib/xreal/monado-xreal.service` |
| the toggle | `xreal-mode.sh {xr\|flat\|status}` | `scripts/xreal-mode.sh` |
| 3DoF profile | curved-arc layout + keyboard/gaze binds + window rules | `example/xreal.conf` |

---

## 0. Live bring-up on the Framework Laptop 16 (validated 2026-07-14)

The whole SBS path was **de-risked live** on the Framework 16 (Ryzen AI 300 + RTX 5070 Laptop, glasses
on the AMD 890M's `DP-5`). What actually works, and the gotchas that were NOT obvious from the research:

1. **The glasses DO re-advertise a native 3840×1080@60 EDID mode after the HID 3D switch — WAIT for it and
   USE it; the forced modeline is a last-resort fallback that STRIPES.** Corrected 2026-07-15 on the live
   device: after `xreal-ctl mode 3d` the DP-5 connector drops and returns (~2 s) and then advertises a
   **native `3840×1080@60` mode** (297 MHz DTD) alongside its 1920×1080 modes. That native timing is what
   the glasses' internal L/R scaler expects. Driving 3D SBS on a **forced/unadvertised CVT modeline**
   (e.g. `266.50 3840 …`) instead produces **diagonal striping across the panel and no left/right stereo
   alignment** — the scaler mis-samples the non-native timing. So `xreal-mode.sh xr` now **waits for the
   native wide mode and sets it**, e.g.:
   ```
   monitor = DP-5, 3840x1080@60, auto, 1
   ```
   and only falls back to the forced modeline (`XREAL_SBS_MODELINE`, retained for hosts that genuinely
   never re-present a wide EDID) if the native mode never appears within ~5 s. On the FW16 the native mode
   always appeared and looked correct; the earlier forced-modeline path was the source of the "super jank /
   striping" the user saw. **Keep 3D at ≤72 Hz on any forced fallback** — 3840-wide @90+ exceeds the 2-lane
   DP-alt budget and goes out-of-range/black (the native mode is 60 Hz, comfortably inside budget). **Also
   pin the DP output to `scale = 1.0`** — an `auto` scale on this output picked **1.25**, which makes
   Hyprland resample the 3840×1080 SBS frame (1 buffer px must map to 1 physical px, or the per-eye split
   softens and can skew). `xreal-mode.sh xr` now sets the mode with an explicit `,auto,1.0`.

2. **Mode-switch ordering is load-bearing.** The `xreal_air` driver picks stereo-vs-mono view geometry
   **once, at device-create time**, from whatever display mode the glasses report over HID
   (`control_display_mode` → `switch_display_mode`, `xreal_air_hmd.c`). So the HID `mode 3d` **must
   happen before monado (re)starts**, or `comp_main` comes up 1920-wide (mono) even though DP-5 is 3840.
   The toggle now does: end session → **stop monado** → `mode 3d` → force 3840 → **restart monado** →
   enable. Confirmed live: with this order, the `comp_main` window (class `openxr`, title `Monado`)
   comes up **3840×1080 fullscreen on DP-5** and `hyprctl openxr status` reaches `state: focused`.

3. **Two GPUs — `openxr:gpu` MUST point at the AMD node that scans out the glasses.** This box is NOT
   single-GPU (the research's §3(c)/§"GPU" bullet is wrong for the FW16; it was written for the AMD
   *desktop*). DP-5 hangs off the AMD 890M (`renderD129`); the box's default Vulkan ICD and the WiVRn
   config's `openxr:gpu` both point at the **NVIDIA** node (`renderD128`), which cross-GPU-crashes when
   monado's comp_main is scanned out by AMD. Two pins are required and the toggle now sets them
   automatically: (a) **monado onto AMD RADV** via `~/.config/xreal/monado.env`
   (`VK_DRIVER_FILES=/usr/share/vulkan/icd.d/radeon_icd.json` — note the **Arch filename has no
   `.x86_64` suffix** — plus `__GLX_VENDOR_LIBRARY_NAME=mesa`); (b) **HypXRland's OpenXR EGL/composite
   node** via `openxr:gpu`, which `xreal-mode.sh xr` auto-detects from the connector's DRM render node
   (`/dev/dri/renderD129`). Requires `sudo pacman -S vulkan-radeon`.

4. **`blend_mode` must be `opaque`.** The birdbath is additive/see-through, so Monado defaults to an
   additive blend and the black scene background stays transparent (and the surface can't go
   solitary/direct-scanout). `xreal-mode.sh xr` sets `openxr:blend_mode = opaque`.

5. **SBS is real stereo; `--mono` is a genuine single view, NOT a "squished" fallback.** In 2D the driver
   collapses the second view to 1×1 (a real mono 1920×1080 head-tracked panel). The earlier "tiny btop in
   the upper-left" photo was the **3-D scene seen through one eye plus an off recenter pose on the unworn
   glasses**, not a packing bug. Both paths are usable; SBS is the correct per-eye one.

6. **FIXED — full-resolution SBS.** Monado's Wayland window backend used to **halve** the surface under
   `XRT_COMPOSITOR_FORCE_WAYLAND` (`comp_settings.c`: *"HMD screen tends to be much larger than
   monitors"* → `preferred.width/height /= 2`), so the present surface was **1920×540** (per-eye 960×540)
   upscaled to 3840×1080 → soft. Patched 2026-07-15: the wayland halving is now **gated behind
   `XRT_COMPOSITOR_WAYLAND_HALVE_SURFACE` (default OFF)**, so the surface comes up at the HMD's native
   **3840×1080** (per-eye **1920×1080**). Verified live: `hyprctl clients` shows the `openxr` window at
   `size [3840,1080]`. The xcb path is unchanged.

7. **Panel sleeps when unworn in 2D.** After `xreal-mode.sh flat`, with no session and the glasses in 2D
   on the desk, the wear sensor powers the panel down and DP-5 may read `disconnected` at the DRM layer —
   this is normal and self-recovering (it comes back when worn or re-driven); the laptop panel (eDP-2) is
   unaffected. A dpms off/on nudge in the toggle re-lights the panel across mode switches.

8. **Monado lifecycle.** `monado-service` has no signal-driven clean shutdown with `XRT_NO_STDIN=1` (its
   IPC mainloop only stops on stdin EOF), so the unit now stops it with an immediate, whitelisted SIGKILL
   (`KillSignal=SIGKILL` + `SuccessExitStatus=SIGKILL`) → stop/restart is instant and the unit stays
   cleanly `inactive/dead` (previously a plain stop hung for the 90 s default timeout then SIGKILL'd →
   `failed`). The service is stateless, so SIGKILL is safe.

9. **FIXED — head tracking + factory IMU calibration parse (Air 2 Ultra).** The driver logged *"Failed
   parse calibration data!"* ~continuously (866× in one session) and — per the original code — only started
   the IMU data stream **on a successful parse**, so head tracking was dead and the log/HID bus were
   flooded. Root cause found by dumping the raw calibration blob (`/tmp/xreal_cal_dump.bin`): the Air 2
   Ultra returns a **~55 KB JSON** bundling the per-eye **display distortion meshes** (`left_display` /
   `right_display`, 32×18 grids) **and** the IMU calibration, and that blob does **not** reassemble into a
   byte-0-valid JSON document over the segmented HID transfer (the leading `{"left_display":{"data":[` is
   missing and there's a zero-filled gap before the `{"FSN":…}` root), so `cJSON_ParseWithLength()` over the
   whole buffer failed. The **embedded `"IMU"` object is intact**, though, and its schema is exactly what
   the existing parser expects (`accel_bias`, `accel_q_gyro`=[0,0,0,1], `gyro_bias`, `gyro_q_mag`=
   [-0.5,0.5,-0.5,0.5], `scale_*`=[1,1,1], `imu_noises`, real per-axis biases). Fix (vendored monado,
   `xreal_air_packet.c` / `xreal_air_hmd.c`):
   - `xreal_air_parse_calibration_buffer()` now **locates and parses just the embedded `"IMU"` object**
     (brace-matched, string-aware) when the whole-buffer parse fails → the **real factory IMU calibration**
     is applied (accurate bias + gyro/mag misalignment). The original whole-buffer path is kept for the
     Air 1.
   - The calibration is initialized to **sane defaults** (identity misalignment, unit scale, zero bias) at
     device create AND as a fallback, so the fusion is never fed the all-zero calloc'd calibration (which
     would zero the scale factors and freeze the pose).
   - Parse failure is now **non-fatal**: the IMU stream is started regardless (`0x01`) and the calibration
     is marked "done" so the driver stops re-fetching the 55 KB blob on every packet → the log flood stops.
   Verified live: after the patch the *"Failed to parse"* count is **0**, and the user confirmed **head
   motion moves the view** (picked the glasses up → the virtual panel swung with them). The Air 2 Ultra
   also streams IMU via the read thread's `0xAA` path even before the `0x01` enable, which is why motion
   appeared as soon as the parse stopped looping.

**Still needs the glasses WORN to judge:** stereo *comfort* and recenter feel. Recenter uses the current
head pose, so recenter **while wearing them and looking straight ahead at where you want the screen** — a
recenter captured with the glasses held/tilted on the desk places the panel at a skewed, too-close pose
(which reads as "jank" that is really just placement). There is **no factory optical/display cal applied**
(the driver runs `u_distortion_mesh_none` and hardcoded 46°H≈52° diagonal FOV / symmetric lens centers —
plausibly correct for the birdbath, but the `left_display`/`right_display` distortion meshes in the blob
are **not** consumed; a future improvement could feed them to `compute_distortion`).

**Not validated by the author (cannot see through the glasses):** absolute distortion/convergence quality.
The remaining "does it look right when worn" is the user's call.

10. **Known wart — the glasses' DP output (`DP-5`) is still a distinct addressable Hyprland monitor.** In a
    fully-integrated solution only `eDP-2` (laptop) and the virtual `XR-main` would be addressable; `DP-5`
    would be a pure scanout target for the Monado surface. Today `DP-5` is an ordinary Hyprland output that
    the `openxr` window is fullscreened onto, so it appears in `hyprctl monitors`, can be cycled to, and
    can (mis)receive stray windows/workspaces. The clean fix is a **dedicated/leased output** for the
    compositor surface (DRM-lease direct mode — explicitly out of scope here, see the top of this doc).
    Pragmatic mitigations until then: bind a dedicated empty workspace to `DP-5` and keep it out of your
    normal workspace-switch binds so nothing drifts there, and/or rely on the `openxr` windowrule keeping
    that output occupied by the Monado surface. Tracked as a follow-up.

---

## 1. One-time setup

### 1.1 udev rule (HID access)

`monado-service` and `xreal-ctl` must open the glasses' HID interfaces without root:

```sh
sudo install -m 0644 contrib/xreal/70-xreal.rules /etc/udev/rules.d/70-xreal.rules
sudo udevadm control --reload-rules && sudo udevadm trigger
# then re-plug the glasses (or log out/in) so the seat ACL applies.
```

`TAG+="uaccess"` grants the logged-in seat user access via logind (the modern, seat-scoped way);
the `MODE="0660"` is a belt-and-suspenders fallback. Covers the whole `3318` family.

Verify: `contrib/xreal/xreal-ctl detect` should print `XREAL Air 2 Ultra present (control interface
open)`. If it says *"found … but could not open its control interface"*, the rule isn't applied yet
(re-plug), or a Monado/XRLinuxDriver process is already holding the device.

### 1.2 Build the xreal Monado flavor

```sh
scripts/build-monado.sh --xreal
```

This builds a **separate** tree (`subprojects/monado/build-xreal`) with `XRT_BUILD_DRIVER_XREAL_AIR`,
the real `comp_main` (`XRT_MODULE_COMPOSITOR_MAIN`), and its Wayland window backend
(`XRT_HAVE_WAYLAND`) — the script fails loudly if hidapi or Wayland aren't found. It does **not**
touch the null test flavor (`subprojects/monado/build`) the XR suite depends on. Output manifest:
`subprojects/monado/build-xreal/openxr_monado-dev.json`.

### 1.3 Build `xreal-ctl` (flat-mode HID helper)

```sh
make -C contrib/xreal
sudo make -C contrib/xreal install   # optional: to /usr/local/bin (so it's on PATH)
```

Needs hidapi (`hidapi-hidraw` pkg-config module). If you don't install it, set `XREAL_CTL=` to the
in-repo `contrib/xreal/xreal-ctl` — the toggle already falls back to that path.

### 1.4 systemd user unit

```sh
mkdir -p ~/.config/systemd/user
install -m 0644 contrib/xreal/monado-xreal.service ~/.config/systemd/user/monado-xreal.service
# Point it at YOUR xreal build if the repo isn't ~/code/Hyprland (e.g. the laptop):
mkdir -p ~/.config/xreal
echo "MONADO_XREAL_SERVICE=$PWD/subprojects/monado/build-xreal/src/xrt/targets/service/monado-service" > ~/.config/xreal/monado.env
systemctl --user daemon-reload
```

The unit is wholly independent of `wivrn.service` (no ordering, no `Conflicts`) — the two runtimes use
different IPC socket names and never fight.

### 1.5 The 3DoF profile

Source `example/xreal.conf` from your `hyprland.conf` and bind the toggle to a key:

```
source = ~/code/Hyprland/example/xreal.conf
```

Edit the `monitor DP-5` window rules in that file to your glasses' connector name if it isn't `DP-5`
(the AMD box is DP-5; find yours with `hyprctl monitors`).

---

## 2. The toggle

```sh
scripts/xreal-mode.sh xr        # 3D SBS arc     (add --mono for the 1920×1080 mono fallback)
scripts/xreal-mode.sh flat      # back to an ordinary head-locked 1920×1080 monitor on the DP output
scripts/xreal-mode.sh status    # detection + current DP mode + hyprctl openxr status
scripts/xreal-mode.sh xr --dry-run   # print every action without doing anything
```

**`xr`** does, in order (see §0 for why this exact order): verify the glasses are present over HID →
**end any active session + stop monado** (free the HID device so the switch is uncontended and monado
re-reads the mode on restart) → `xreal-ctl mode 3d` (HID switch to SBS) → **wait for the native
3840×1080@60 mode** the glasses re-present and set it (fall back to the forced modeline only if it never
appears — see §0.1), verifying the connector comes up 3840-wide (revert + bail if not) → dpms nudge →
**auto-set `openxr:gpu`** to the connector's DRM render node + `openxr:blend_mode = opaque` →
**restart** `monado-xreal.service` (after importing `WAYLAND_DISPLAY`) → wait for monado's
`monado_comp_ipc` socket → `hyprctl keyword openxr:runtime_json <xreal manifest>` → `hyprctl openxr
enable` → **explicitly move+fullscreen** the `openxr` window on the glasses' output (by address, not just
a windowrule, so it never drifts to the laptop panel). The xreal Monado's `comp_main` opens a Wayland
toplevel (class `openxr`, title `Monado`), landed fullscreen on the glasses' DP output at 3840×1080.
`--mono` instead switches the
glasses to 2D and drives a single 1920×1080 head-tracked view.

**`flat`** reverses it: it disables the XR session **only if** the active session is the xreal runtime
(a WiVRn/Quest session is left alone) → stops the xreal Monado unit → `xreal-ctl mode 2d` → restores
the DP output to 1920×1080.

Machine-agnostic knobs (env): `XREAL_MONITOR` (connector name; else auto-detected from the EDID
description), `MONADO_XREAL_BUILD` (build dir), `XREAL_CTL`, `XREAL_FLAT_MODE`. Idempotent, safe when
the glasses are unplugged, and it **never** touches `wivrn.service`.

---

## 3. How it hangs together (design facts to know)

- **Runtime selection is per-session, not global.** The OpenXR loader resolves the runtime only from
  `getenv("XR_RUNTIME_JSON")`. `openxr:runtime_json` sets that env var on the **main thread**, once,
  *before* the handshake helper thread spawns (and after the in-flight-handshake guard, so no loader
  thread is racing `getenv`) — see the code comment in `COpenXRManager::start()`. Clearing the key
  restores the login value, so the toggle round-trips with no residual state. This bypasses WiVRn's
  `active_runtime.json` entirely, so the two runtimes coexist.

- **SBS is a HID command, not a DRM mode change.** `xreal-ctl mode 3d` sends `MSG_W_DISP_MODE` over
  the control HID interface; the glasses then re-present a 3840×1080 EDID/mode on the DP link. Monado's
  `comp_main` creates a 3840×1080 Wayland surface (it pins the toplevel min=max to the device width) and
  requests fullscreen; Hyprland scans that out to the DP output. The **risk** (research §4): the DP link
  must actually pick up the 3840-wide mode after the HID switch. The toggle waits for that mode to appear
  and, if it never does, tells you to retry or use `--mono`.

- **GPU: box-dependent — on the two-GPU Framework 16, `openxr:gpu` MUST be set (see §0.3).** The claim
  that both target boxes are single-GPU held for the standalone AMD desktop and the Lunar Lake laptop,
  but the **Framework Laptop 16** (the live fishfood box) is a hybrid AMD+NVIDIA machine: DP-5 is on the
  AMD 890M (`renderD129`) while the default Vulkan ICD and the running WiVRn config both point at the
  NVIDIA node (`renderD128`), which cross-GPU-crashes monado. So on the FW16 you must (a) pin
  **monado** onto AMD RADV (`~/.config/xreal/monado.env`, `VK_DRIVER_FILES=…/radeon_icd.json`) and
  (b) point **HypXRland's `openxr:gpu`** at the glasses' render node. `xreal-mode.sh xr` now
  auto-detects the connector's render node and sets `openxr:gpu` for you, so this is handled on any box:
  where the glasses share Hyprland's primary GPU it's a no-op; where they don't (FW16) it corrects it.
  The wrong-GPU guard in `start()` (`XRGpuProbe`) still refuses safely if the EGL node ever mismatches.

- **Doff detection: HARDWARE presence over HID (WP-XR-DONDOFF), USB-unplug is the backstop.** The Air 2
  Ultra's proximity sensor **does** surface wear on the control interface: every physical don/doff pushes
  a `XREAL_AIR_MSG_P_DISPLAY_TOGGLED` (0x6C04) event (don → state `0x01` "Open OLED 2D"; doff → `0x00`
  "Close OLED"). The vendored `xreal_air` driver now advertises `XR_EXT_user_presence`
  (`base.supported.presence = true`, `get_presence` returns the debounced `display_on`), and the vendored
  monado's multi-compositor polls that head device once per frame and broadcasts a
  `XRT_SESSION_EVENT_USER_PRESENCE_CHANGE` on change — so don/doff reaches HypXRland live as
  `XR_TYPE_EVENT_DATA_USER_PRESENCE_CHANGED_EXT`. (Vanilla Monado only reads `get_presence` once at
  session begin; this per-frame poll is the generic fix, and also gives Rift CV1 / PSVR2 live presence.)
  The profile therefore uses **`monitors_follow_session = visible`**: doffing (set the glasses on the
  desk) idles the XR monitors — their workspaces evacuate to your laptop — after a short
  `monitor_unplug_grace_ms` (3 s anti-flap), and donning re-plugs them **immediately**, no cable pull
  needed. Confirm with `hyprctl openxr status`: `presence: yes` when worn, `no` when doffed (it reads
  `unsupported` only on a runtime without the emit). **USB-unplug is still the hard backstop**: pulling
  the cable drops DP-5 **and** the `3318:0426` HID device, `monado-service` loses the HMD, the session
  ends, and the same session-loss unplug runs. A tiny driver-side debounce (~400 ms) keeps a jittery
  proximity threshold from flickering presence; the captured transitions were clean single edges.

- **3DoF UX guards.** No controllers, no hands: the profile sets `pointer = false` and
  `hand_input = off`. Roam/geofence/leash are irrelevant with no positional tracking, so the arc uses
  fixed `anchor:local` panels with adaptive **off**. `recenter_on_plug = true` re-seats the arc to your
  head pose on don (yaw-dominant, since there's no translation). Input is keyboard + gaze grab + voice
  (`hypxrvoice`).

---

## 4. Live validation checklist (the user runs this — both boxes)

Nothing below was run live during implementation (the glasses' HID + DP-5 are in daily use). Do this
on the **AMD desktop** and again on the **Lunar Lake laptop** (single-GPU; different connector name —
use `XREAL_MONITOR=` or rely on auto-detect, and set `~/.config/xreal/monado.env`).

1. **udev:** install the rule (§1.1), re-plug, then `contrib/xreal/xreal-ctl detect` →
   `XREAL Air 2 Ultra present (control interface open)`. If not, the ACL isn't applied or something
   else holds the HID device.
2. **Builds:** `scripts/build-monado.sh --xreal` completes; `make -C contrib/xreal` produces
   `xreal-ctl`.
3. **status baseline:** `scripts/xreal-mode.sh status` shows the glasses' DP output detected, current
   mode `1920x1080`, the runtime manifest as `(built)`, unit `inactive`.
4. **First `xreal-mode xr` (SBS):** the DP output should switch to **3840×1080**; a fullscreen
   `openxr` window should appear on the glasses; `hyprctl openxr status` should reach `state: focused`
   (or `visible`) with `runtime json:` pointing at `…/build-xreal/openxr_monado-dev.json` and `runtime:`
   naming Monado (not WiVRn). The 3-panel arc should render in stereo; turning your head looks around
   the fixed arc.
   - **If the DP output never reaches 3840×1080** (the toggle reports it waited and gave up): that is
     the SBS risk — fall back to **`xreal-mode xr --mono`** (a single head-tracked 1920×1080 panel,
     still a usable XR display) and note it for follow-up.
5. **Recenter:** re-don the glasses (or `xreal-mode flat` then `xr`) → the arc re-seats in front of
   you. Recenter with **`SUPER+SHIFT+Home`** (`SUPER+Home` is walker in Omarchy) or
   `hyprctl dispatch xrmonitor center` — do it **while wearing the glasses and looking straight ahead**,
   since recenter captures the live head pose (a recenter with the glasses tilted on the desk places the
   panel at a skewed, too-close pose).
6. **Gaze grab:** `SUPER+SHIFT+G` while looking at a panel grabs it (head-forward ray, no eye
   tracking); `SUPER+ALT+=/-` push/pull; tap again to drop.
7. **`xreal-mode flat`:** the `openxr` window closes, the DP output returns to 1920×1080, and the
   glasses are an ordinary head-locked monitor again. Run it a second time — it should be a safe no-op.
8. **Doff/don (hardware presence):** with `xreal-mode xr` active and the glasses **on**, `hyprctl openxr
   status` shows `presence: yes`. Set them on the desk (doff): after ~3 s (`monitor_unplug_grace_ms`)
   `presence: no` and the XR monitors unplug — their workspaces evacuate to your laptop — with the cable
   still connected. Put them back on (don): `presence: yes` and the monitors re-plug **immediately**.
9. **Unplug behavior (backstop):** with `xreal-mode xr` active, pull the USB cable. The XR session should
   end and the XR monitors unplug (workspaces evacuate to your other displays). Re-plug + `xreal-mode xr`
   restores. (This is the hard teardown even if presence were ever unavailable.)
10. **WiVRn untouched:** if you use WiVRn/Quest, confirm `xreal-mode flat/xr` never stops
    `wivrn.service` and a running WiVRn session is unaffected (separate socket + separate runtime json).
11. **Idempotence / unplugged safety:** `xreal-mode xr` with the glasses unplugged should refuse
    cleanly ("no XREAL device detected"); `xreal-mode flat` with nothing active should be a quiet no-op.

Report back the SBS result (step 4) especially — that's the one architectural risk we couldn't
de-risk without the live device.
