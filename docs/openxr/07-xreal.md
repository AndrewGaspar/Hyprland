# 07 — XREAL Air 2 Ultra as a 3DoF OpenXR display (WP-XR1)

Running the **XREAL Air 2 Ultra** birdbath glasses (USB `3318:0426`) as a **3DoF, seated,
orientation-only OpenXR display** driven by the vendored Monado's `xreal_air` driver, with an
in-session **toggle** between XR mode and ordinary flat head-locked use of the glasses' DisplayPort
output. This is the operational companion to the research memo
[`research/XREAL-3DOF.md`](research/XREAL-3DOF.md), which carries the full evidence and rationale.

**Scope (v1):** 3DoF (no positional tracking), stereo **SBS 3840×1080** with a **mono 1920×1080
fallback**, keyboard + gaze + voice input (no controllers, no hand tracking — the driver exposes an
HMD only). Two display paths: the default **fullscreen-window** path (no compositor patches beyond the
`openxr:runtime_json` selector) and, **new in V2.2**, an opt-in **DRM-lease / direct mode**
(`xreal-mode.sh xr --direct`) in which HypXRland leases the glasses' DP connector to Monado so it owns
the flip (one fewer compositor hop → lower latency). Flat/window mode is fully preserved — the lease
knob is off by default. **Out of scope:** 6DoF (the stereo cameras / Basalt VIO path).

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

9b. **FIXED for real — factory IMU cal now applied EVERY boot (2026-07-15).** The de-rotation parse
   (commit `1d521ab80`) parsed *offline* buffer dumps but the LIVE runtime still fell back to default
   zero-bias cal on most boots. Root-caused from FRESH live captures (10 boots, `XREAL_CAL_DUMP=1`):
   - `GET_CAL_DATA_LENGTH` reports **55845 = the JSON document length**. But the flash calibration
     region is zero-padded up to a **504-byte segment boundary**, so its true period is
     **F = 55944 = 111×504 = document(55845) + 99 zero-pad**.
   - The firmware's segment read cursor is **not reset** by `GET_CAL_DATA_LENGTH` and **drifts ~504
     bytes per boot** (observed: the 99-zero pad's offset marched 51309→50805→…→46269 across ten
     consecutive boots), wrapping at F.
   - Reading only `length` (55845) bytes therefore returns a window **99 bytes short of one full
     period**: unless the cursor happens to sit exactly at the document start, the window includes the
     zero-pad **and permanently OMITS 99 real document bytes** — a loss no de-rotation can undo. It
     silently produced corrupt-but-sometimes-parseable JSON (IMU survived by luck) or dropped to
     default. Proven over all 111 cursor positions: read-`length` hard-fails ~13 % and corrupts the
     rest; **read-full-period recovers the exact document 111/111.**
   - **Fix** (`xreal_air_hmd.c`, `handle_sensor_control_get_cal_data_length`): round the reported
     length **up to the segment-payload boundary** and read that many bytes (55944), capturing the
     whole period `[document + zero-pad]` regardless of cursor; `xreal_air_parse_calibration_buffer()`
     then de-rotates around the 99-zero pad and recovers the complete document every time. (Reading
     exactly one period also leaves the firmware cursor where it started.)
   - **Verified live, headless, across 13 monado-service runs + the systemd service:** every boot logs
     `Factory calibration parsed: gyro_bias=[-0.02333 0.00093 -0.02513] accel_bias=[0.01041 -0.20677
     0.02486]` (the real factory biases) with **zero** `using default IMU calibration`, while the pad
     offset keeps drifting (proving robustness, not a lucky cursor). `hyprctl openxr status` reaches
     and sustains `state: focused`. The per-eye **FOV now parses too**: `Factory display: res=1920x1080
     per-eye FOVh: left=42.23° right=42.16°` (still gated behind `XREAL_AIR_USE_FACTORY_FOV`, default
     off — A/B worn), and the 32×18 distortion meshes are detected (still not wired to
     `compute_distortion`). *Honest limit:* this proves the factory cal is now APPLIED; whether worn
     yaw-drift subjectively improves is the user's to feel.

**Still needs the glasses WORN to judge:** stereo *comfort* and recenter feel. Recenter uses the current
head pose, so recenter **while wearing them and looking straight ahead at where you want the screen** — a
recenter captured with the glasses held/tilted on the desk places the panel at a skewed, too-close pose
(which reads as "jank" that is really just placement). The factory optical/display cal is now **parsed
but not applied by default**: the per-eye FOV derived from the blob's pinhole intrinsics is ≈**42.2°H**
(vs the hardcoded 46°), available behind `XREAL_AIR_USE_FACTORY_FOV` (default off — it changes on-screen
scale ~9 % and can only be validated worn); the driver still runs `u_distortion_mesh_none` and the
`left_display`/`right_display` 32×18 distortion meshes are detected but **not** consumed (a future
improvement could feed them to `compute_distortion`).

**Not validated by the author (cannot see through the glasses):** absolute distortion/convergence quality.
The remaining "does it look right when worn" is the user's call.

10. **Known wart — the glasses' DP output (`DP-5`) is still a distinct addressable Hyprland monitor.** In a
    fully-integrated solution only `eDP-2` (laptop) and the virtual `XR-main` would be addressable; `DP-5`
    would be a pure scanout target for the Monado surface. Today `DP-5` is an ordinary Hyprland output that
    the `openxr` window is fullscreened onto, so it appears in `hyprctl monitors`, can be cycled to, and
    can (mis)receive stray windows/workspaces. The clean fix is a **dedicated/leased output** for the
    compositor surface — now available as **`xreal-mode.sh xr --direct`** (V2.2 DRM-lease mode, §2): while
    leased, `DP-5` leaves `hyprctl monitors` entirely and cannot receive stray windows. This wart applies
    only to the default **window** path; pragmatic mitigations there: bind a dedicated empty workspace to
    `DP-5` and keep it out of your normal workspace-switch binds so nothing drifts there, and/or rely on the
    `openxr` windowrule keeping that output occupied by the Monado surface.

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

### 1.4b setcap — DO NOT cap monado-service (it breaks Vulkan GPU selection)

**Do not put `cap_sys_nice` (or any file capability) on `monado-service`.** We tried it to unlock
amdgpu's REALTIME GPU queue priority for direct mode, and it broke everything, subtly:

any file capability makes the kernel exec the binary in **secure-execution mode** (`AT_SECURE=1`),
and the Vulkan loader deliberately **ignores `VK_DRIVER_FILES` / `VK_ICD_FILENAMES` for privileged
processes**. Our AMD ICD pin (§0) therefore silently stopped applying: monado enumerated every ICD
and picked the NVIDIA dGPU instead of the AMD iGPU that owns DP-5. Direct mode then fail-looped on
`vkGetDrmDisplayEXT: VK_ERROR_UNKNOWN` (AMD lease fd vs NVIDIA physical device), and the ~7 s retry
churn of full NVIDIA device create/destroy cycles hitched/starved the whole machine (both
2026-07-15 incidents — see §1.4c). Journal fingerprint of the broken state: `create_device` logs
`name: NVIDIA GeForce RTX 5070 Laptop GPU` instead of `AMD Radeon 890M Graphics (RADV STRIX1)`.

Rebuilds drop file capabilities, so a plain `scripts/build-monado.sh --xreal` run heals a capped
binary (the script now WARNS if it detects one instead of re-applying). If you have a leftover
`/etc/sudoers.d/10-xreal-setcap` rule from the earlier approach, delete it; to strip a cap without
rebuilding: `sudo setcap -r <monado-service>`.

If a REALTIME-queue experiment is ever revisited, the capability alone is not enough — it also
needs (a) `XRT_COMPOSITOR_VK_GLOBAL_PRIORITY=realtime` (§1.4c), (b) a **monado-level** GPU pin that
survives secure execution (the loader env pin will not), and (c) the watchdog/second-machine rule.
Note the judder fix (§8) was validated entirely on a MEDIUM queue — REALTIME has never contributed
a validated improvement.

### 1.4c 2026-07-15 starvation incidents — root cause + the realtime env gates (default off)

Two hitching/starvation incidents on 2026-07-15 (one ended in a hard power-cycle) both trace to the
`cap_sys_nice+ep` experiment. **Root cause (confirmed by A/B against the journals): the capability
broke Vulkan GPU selection**, not realtime priority per se — see §1.4b. Sequence: secure execution
made the loader ignore the AMD ICD pin → monado selected the NVIDIA dGPU → `vkGetDrmDisplayEXT`
failed `VK_ERROR_UNKNOWN` against the AMD DP-5 lease → direct-mode bring-up fail-looped, each ~7 s
retry creating and destroying a full NVIDIA Vulkan device — and that churn hitched/starved the
whole machine. A second attempt with the realtime paths gated OFF (`..._MEDIUM` confirmed in the
journal) hitched the same way, proving the churn — not the queue priority — is the starvation
mechanism. Uncapped binaries select `AMD Radeon 890M Graphics (RADV STRIX1)` and direct mode works.

The capability had also made two previously-dead realtime paths live for the first time; they are
kept env-gated as defense in depth (REALTIME rode along on the first incident and plausibly turned
recoverable hitching into the full hang):

1. **Vulkan queue global priority.** `comp_vulkan` walks `REALTIME -> HIGH -> MEDIUM`; with
   CAP_SYS_NICE the REALTIME request succeeds. Every validated session — including the judder fix —
   ran **MEDIUM**.
2. **SCHED_FIFO priority 99.** `comp_multi_system`'s *Multi Client Module* thread calls
   `u_linux_try_to_set_realtime_priority_on_thread` (max realtime priority). It never engaged during
   the incidents (system creation kept failing before the thread started) but is a latent starvation
   bomb on any capped binary.

**Fix — both paths are now env-gated in the vendored monado, defaulting to the conservative behavior:**

- `XRT_COMPOSITOR_VK_GLOBAL_PRIORITY=realtime|high|medium` caps the **maximum** global priority
  `create_device()` may request (the fallback walk down the list is unchanged — the cap only chooses
  where it starts). **Default `medium`.** The chosen cap is logged at INFO (`VK_INFO`, visible with
  the default `XRT_COMPOSITOR_LOG=info`): `Vulkan queue global-priority cap: QUEUE_GLOBAL_PRIORITY_MEDIUM`.
  `xreal-mode.sh --direct` writes `Environment=XRT_COMPOSITOR_VK_GLOBAL_PRIORITY=medium` into the
  direct-mode drop-in explicitly (self-documenting; medium is also the default).
- `XRT_COMPOSITOR_THREAD_REALTIME=true|false` gates the SCHED_FIFO promotion. **Default `false`**; when
  off a one-line INFO (`SCHED_FIFO promotion skipped ...`) records it (global `U_LOG_I`, so it needs
  `XRT_LOG=info` to appear — the Vulkan cap line above is the always-visible signal).

**Opting in later:** set the env var(s) in the service drop-in (or `~/.config/xreal/monado.env`). Because
the target GPU is shared with the desktop, any REALTIME/HIGH or SCHED_FIFO experiment MUST run with a
watchdog or on a second machine you can SSH into — a starved iGPU takes the whole session down and the
only recovery observed on 2026-07-15 was a hard power-cycle.

**USB preflight.** `xreal-mode.sh xr` now refuses to (re)start monado unless the glasses' USB device
`3318:0426` is enumerated (dependency-free sysfs scan of `/sys/bus/usb/devices/*/id{Vendor,Product}`).
A missing device otherwise makes monado's prober silently fall back to a Simulated HMD. This guards the
genuinely-unplugged / USB-wedged case (which needs a physical replug) — it would NOT have prevented the
2026-07-15 hang, where the glasses were present.

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
scripts/xreal-mode.sh xr           # 3D SBS arc, fullscreen-WINDOW path (default). --mono = 1920×1080 mono
scripts/xreal-mode.sh xr --direct  # 3D SBS via DRM-LEASE direct mode (V2.2): Monado leases DP-5, owns the flip
scripts/xreal-mode.sh flat         # back to an ordinary head-locked 1920×1080 monitor on the DP output
scripts/xreal-mode.sh status       # detection + current DP mode + hyprctl openxr status
scripts/xreal-mode.sh xr --dry-run # print every action without doing anything
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

**`xr --direct`** (V2.2 DRM-lease / direct mode) takes the same HID/gpu/runtime steps but, instead of
setting a DP mode and placing a window, it **flips the HypXRland `lease` monitor-rule flag on DP-5**
(`hyprctl keyword monitor DP-5,preferred,auto,1,lease`) so HypXRland stops driving DP-5 as a desktop and
**offers the connector via `wp_drm_lease_v1`**. It also writes a systemd drop-in
(`monado-xreal.service.d/10-xreal-direct.conf`) that **`UnsetEnvironment=`s `XRT_COMPOSITOR_FORCE_WAYLAND`**
(the base unit sets it `=1`) and sets `XRT_COMPOSITOR_FORCE_WAYLAND_DIRECT=1` +
`XRT_COMPOSITOR_WAYLAND_CONNECTOR=DP-5`, so Monado's `comp_window_direct_wayland` backend
**leases DP-5 and owns the flip** — removing the second compositor
hop (Monado→Wayland-window→Hyprland→scanout) for lower latency and a vsync Monado controls. No `openxr`
window exists in this mode; verify with `hyprctl monitors` (DP-5 should be **absent** as a desktop
output) and the Monado journal (`connector id …`). The `lease` flag is a **HypXRland-specific**
affordance (upstream compositors only offer EDID-flagged non-desktop connectors); it is **off by
default**, so a stock desktop config is byte-identical. Requires the running HypXRland binary to carry
the `lease` support (build + relog).

> **Gotcha — `UnsetEnvironment`, not `Environment=…=` (empty):** Monado reads its `XRT_*` options with
> plain `getenv()` (`u_debug.c` `get_option_raw`), and `debug_string_to_bool("")` returns **TRUE** (an
> empty string matches none of the `false`/`0` cases). So an *empty* `XRT_COMPOSITOR_FORCE_WAYLAND=` is
> still truthy, and since `comp_settings.c` checks `force_wayland` **after** `force_wayland_direct`, it
> clobbers `target_identifier` back to the windowed `wayland` backend — Monado then falls back to a
> `VK_KHR_wayland_surface` toplevel (an `openxr` window) and never leases the connector. The drop-in must
> therefore **`UnsetEnvironment=XRT_COMPOSITOR_FORCE_WAYLAND`** to remove it entirely (so `getenv()` →
> NULL → `false`). This was the V2.2 direct-mode regression: window fallback despite the lease being
> offered.

**`flat`** reverses it (and is the rollback for either path): it disables the XR session **only if** the
active session is the xreal runtime (a WiVRn/Quest session is left alone) → stops the xreal Monado unit
(releasing any DRM lease) + **removes the direct drop-in** → `xreal-ctl mode 2d` → **restores DP-5 to a
normal 1920×1080 desktop monitor** (re-issuing a `monitor=DP-5,…` rule *without* `lease` reclaims the
lease offer and reconnects it as a desktop).

Machine-agnostic knobs (env): `XREAL_MONITOR` (connector name; else auto-detected from the EDID
description), `MONADO_XREAL_BUILD` (build dir), `XREAL_CTL`, `XREAL_FLAT_MODE`. Idempotent, safe when
the glasses are unplugged, and it **never** touches `wivrn.service`.

> ### ⚠️ Direct-mode teardown ordering — the lease MUST be released before any XR-disable / DP modeset
> A DRM lease makes Monado the **DRM master of DP-5's CRTC**. If HypXRland disables the XR session
> (`hyprctl openxr disable`) or reclaims/modesets DP-5 **while the lease is still held**, the compositor
> **deadlocks and the whole session hangs** (hard reboot): `openxr disable` tears the session down via a
> **synchronous OpenXR IPC** (`xrDestroySession`/`xrDestroyInstance`) on Hyprland's main thread, and in
> direct mode Monado can only answer that after finishing/releasing its leased DP-5 flip — which needs
> Hyprland's wayland thread, i.e. the very thread blocked in the IPC. Cross-process deadlock.
>
> **Two independent safeguards now prevent this (belt + suspenders):**
> - **Script (`xreal-mode.sh`, belt):** `xr`, `xr --direct` and `flat` all detect an active direct mode
>   (`direct_mode_active`: drop-in present **and** the unit active) and **stop Monado FIRST**
>   (`stop_monado_for_lease_release`), waiting for the lease to drop, *before* `openxr disable` or any DP
>   modeset. This also makes **re-running `xr --direct` while already in direct mode** a safe clean
>   teardown instead of the re-entry hang.
> - **Compositor (suspenders):** `CMonitorRuleManager::ensureMonitorStatus()` **skips any output that is
>   actively leased** (`m_isBeingLeased`) — it never `applyMonitorRule()`s or `onConnect()`s a leased
>   connector (with defense-in-depth bails in `CMonitor::applyMonitorRule` and the `onConnect` lease
>   gate). So a held lease **cannot** be modeset out from under Monado no matter what re-drives the rule
>   pass. **This also makes direct mode survive a config reload** (e.g. an Omarchy theme change re-applying
>   `monitor=DP-5,…` from the file *without* the runtime `lease` flag): the leased output is left untouched,
>   keeping its offer/lease, so DP-5 stays leased instead of reverting to a desktop and breaking the session.
>   When the lease is finally released (Monado exits), the lease-destroy listener schedules a rule pass so
>   DP-5 reconciles back to a desktop (or re-offers) on its own.

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
