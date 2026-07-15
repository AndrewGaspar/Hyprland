# Research: XREAL Air 2 Ultra as a 3DoF OpenXR display via Monado

Research memo (2026-07-14). **No implementation, no live XR experiments.** This
memo evaluates supporting the user's **XREAL Air 2 Ultra** birdbath glasses (USB
`3318:0426`, currently plugged in and enumerated as ordinary monitor **DP-5
"Nreal Air 2 Ultra"**) as a **3DoF OpenXR head-mounted display** driven by the
vendored Monado's `drivers/xreal_air`. The user reviews this before any code.

Evidence base, all inspected read-only for this memo on the live box:

- **Vendored Monado** `subprojects/monado` @ `c2ddab59` (committed 2026-02-19),
  the exact runtime the XR suite validates against — driver
  `src/xrt/drivers/xreal_air/{xreal_air_hmd.c,.h,xreal_air_interface.h,xreal_air_packet.c}`,
  builder `src/xrt/targets/common/target_builder_xreal_air.c`, IPC + compositor
  trees, plus a shallow fetch of `origin/main` to diff the driver since our pin.
- **Host DRM topology / EDID** from `/sys/class/drm` and `edid-decode`.
- **HypXRland** `src/protocols/DRMLease.cpp`, `src/output/Monitor.cpp` (the lease
  gate), `scripts/build-monado.sh`.
- **Live PipeWire** (`wpctl status`, `pw-dump`) and `$XDG_RUNTIME_DIR` socket
  inspection.
- Web research on the driver's upstream history, community 6DoF work,
  wheaney's XRLinuxDriver / Breezy Desktop, and XREAL's proximity sensor (URLs
  cited inline).

---

## TL;DR

1. **The easy path is real and single-GPU.** `/sys` topology proves DP-5 is
   `card2-DP-5` on **amdgpu** (PCI `1002:150E`, the AMD 890M iGPU, render node
   `renderD129`); the NVIDIA RTX 5070 is a *different* card (`card1`,
   `renderD128`). An xreal session touches only the AMD iGPU for both compute and
   scanout — no cross-GPU, no NVIDIA. This is the reliable AMD path.
2. **The driver at our pin already fully supports the Ultra** — including the
   Ultra-specific USB interface layout (handle iface **2**, control iface **0**,
   512-byte sensor buffer, vs iface 3/4/64 on Air 1/2/Pro). It does **3DoF** IMU
   fusion, reads factory calibration over HID, and drives the glasses' **display
   mode over HID** (2D mirrored 1920×1080 mono ↔ 3D SBS 3840×1080 stereo, FOV
   46°×46°). Since our pin, upstream has **exactly one** xreal commit — a trivial
   log-format fix. **Nothing to cherry-pick.**
3. **Display path decision: run monado's `comp_main` as a fullscreen Wayland
   window on DP-5. DRM-lease direct mode is *closed to us today.*** Hyprland only
   offers a monitor for lease when `m_output->nonDesktop` is true
   (`Monitor.cpp:289–296`), and DP-5's EDID does **not** set the non-desktop
   quirk (there is no `/sys/class/drm/card2-DP-5/non_desktop` node at all — it is
   a normal desktop monitor with an active workspace). So `drm-lease-v1` never
   advertises it. The Wayland-window path needs **zero compositor changes** and is
   the correct v1.
4. **The two runtimes do NOT collide at the socket level.** The real monado
   driver never binds TCP 4242 (that is *only* the remote test driver,
   `target_builder_remote.c:86`). Monado's IPC socket is
   `$XDG_RUNTIME_DIR/monado_comp_ipc` (`CMakeLists.txt:375`); WiVRn uses its own
   `$XDG_RUNTIME_DIR/wivrn/comp_ipc` (confirmed live — only a `wivrn/` dir +
   `wivrn.pid` exist in the runtime dir). Different names ⇒ both services can be
   *present*. Selection is purely which `active_runtime.json` an app resolves —
   **per-session `XR_RUNTIME_JSON` is the v1 selector**, immune to WiVRn's
   `active_runtime` flipping.
5. **3DoF is the honest v1; 6DoF is a research project, not a feature.** The
   Ultra's stereo cameras are live on the box (two UVC V4L2 nodes), and a
   community stack (`DeskUnreal/xreal-vio-vr`, Basalt fisheye624 VIO → Monado pose
   injection) exists — but it is explicitly *"not plug-and-play yet,"* uses a
   patched Basalt, and has no releases. Ship 3DoF; keep 6DoF as a documented
   stretch.
6. **The input story flips to our strengths.** No controllers, no hands (the
   driver exposes an HMD only). On 3DoF glasses, **keyboard + `hypxrvoice` + gaze
   become THE interaction model** — exactly the muscle HypXRland already has.

**RECOMMENDATION: build V1 as a no-compositor-change effort** — a second Monado
build flavor with the xreal driver + real Vulkan compositor, a udev rule, a
session profile that force-fullscreens `comp_main` on DP-5, a launcher that sets
`XR_RUNTIME_JSON`, and a 3DoF-aware layout profile. Defer the `openxr:runtime_json`
config key and any DRM-lease knob to V2.

---

## 1. Hardware & topology (the load-bearing `/sys` evidence)

`lsusb`: `Bus 003 Device 008: ID 3318:0426 XREAL XREAL Air 2 Ultra` — the exact
PID the vendored driver targets (`XREAL_AIR_2_ULTRA_PID 0x0426`,
`xreal_air_interface.h`).

**Which GPU drives DP-5** (`/sys/class/drm/*/device/uevent`, render-node
symlinks):

| card | driver | PCI ID | render node | DP-5? |
|------|--------|--------|-------------|-------|
| card1 | `nvidia` | `10DE:2D58` (RTX 5070) | `renderD128` | no |
| card2 | `amdgpu` | `1002:150E` (890M iGPU) | `renderD129` | **yes** |

`DP-5 → ../0000:c2:00.0/drm/card2/card2-DP-5`, and `card2 → amdgpu`. **The
glasses hang off the AMD iGPU.** A monado session on DP-5 is single-GPU AMD end
to end — the reliable path, and it sidesteps every NVIDIA XR caveat.

**EDID** (`edid-decode /sys/class/drm/card2-DP-5/edid`): EDID 1.3, **single
128-byte base block, no CTA-861 extension**. Manufacturer `MRG` (Nreal), product
name `"Air 2 Ultra"`, serial `0x88888800`, made week 8 2023, image size 12×7 cm.
Three detailed timings, all 1920×1080: **60 / 90 / 120 Hz** (matches
`hyprctl monitors`: `availableModes: 1920x1080@60/120/90`). Note the display is
in its **2D mode** right now — there is **no 3840×1080 SBS mode in the EDID**,
because SBS is entered by an out-of-band **HID command**, not advertised as a
DRM mode (see §4). Because the EDID carries **no non-desktop quirk**, the kernel
never sets the connector's non-desktop property (there is no
`/sys/class/drm/card2-DP-5/non_desktop` file), so Hyprland treats it as an
ordinary desktop monitor — decisive for §3.

**Peripherals present live** (`wpctl status`, `pw-dump`):

- Audio **sink** `81 "XREAL Air 2 Ultra Analog Stereo"` + **source**
  `83` (device `79`, ALSA `USB3318:0426` at `usb-0000:c4:00.0-1`, serial
  `ZBBM5DZFMP`). Speakers + mic work as a normal USB audio device today.
- **Two UVC V4L2 camera nodes** (`32`, `72` — `"XREAL Air 2 Ultra: UVC Camera
  0"`): the stereo 6DoF cameras, already exposed to the box. Relevant to §2.

---

## 2. Driver maturity, 3DoF fusion, and the 6DoF question

### 2.1 What the vendored driver actually does

`drivers/xreal_air` (author Tobias Frisch / *TheJackiMonster*, the maintainer of
the well-regarded open `nrealAirLinuxDriver`; SDK is open, not XREAL's closed
one):

- **3DoF orientation only.** `struct m_imu_3dof fusion;` (`xreal_air_hmd.c:111`),
  updated `m_imu_3dof_update(&hmd->fusion, ts, &accel, &gyro)`
  (`:312`). The magnetometer **is read** (`hmd->read.mag`, and calibration
  includes mag) but is **not fed into the fusion** — `m_imu_3dof` takes only
  accel+gyro. Consequence: **no absolute yaw reference → slow yaw drift** is
  expected, the classic birdbath-glasses behaviour. Community reports across the
  XREAL/Monado ecosystem describe exactly this (mitigated by frequent recenter).
- **Factory calibration read over HID** (`MSG_GET_CAL_DATA_LENGTH 0x14`,
  `CAL_DATA_GET_NEXT_SEGMENT 0x15`, …; `xreal_air_parsed_calibration`) — gyro/accel
  bias + scale come from the device, so fusion starts from a calibrated IMU.
- **Display-mode + brightness control over HID** (§4). `switch_display_mode`
  (`:976`) configures a **split side-by-side** stereo device via
  `u_device_setup_split_side_by_side`, 46°×46° FOV.
- **Distortion:** includes `u_distortion_mesh.h`; for a birdbath the optical
  distortion is minimal, so this is effectively an identity/none mesh — no heavy
  per-eye correction needed (unlike a Fresnel HMD).
- **No display/EDID ownership.** The driver is a *pure HID device* (`hidapi`, VID
  `0x3318`; enumerated by `target_builder_xreal_air.c`). It sets up the *virtual
  HMD geometry* but has **no knowledge of the DP connector or EDID** — the pixels
  reach the glasses entirely through the separate DisplayPort/DRM path. **This
  clean separation is what makes §3 a compositor-side decision, not a driver
  one.**

### 2.2 Upstream delta since our pin — nothing to cherry-pick

Our pin `c2ddab59` (2026-02-19) already contains the meaningful xreal work:
`80040ff32 d/xreal_air: Implement changes to support Xreal Air 2 Ultra`,
`ef0f552c3 c/compositor: support compensation for rolling scanout hmds`, and
`56648de41 xrt+c/main: get compositor info for a display mode`. Diffing the
driver path `c2ddab59..origin/main`:

- `drivers/xreal_air/`: **one** commit — `a12663cc1` (`fix t_dead_reckoning
  timestamp print format modifier`), cosmetic. **Skip.**
- `compositor/main/`: `95b320d41 comp: add scanout compensation to graphics
  compositor` — a general latency nicety, **optional** for V2, not xreal-specific.

**Conclusion:** the pinned driver is current for the Ultra; no cherry-pick is
required for v1.

### 2.3 The 6DoF question — realistic v1 is 3DoF

The Ultra ships dual environment cameras + IMU for 6DoF SLAM, and those cameras
are already visible on the box as UVC nodes (§1). Monado has mature Basalt VIO,
and a community project — **`DeskUnreal/xreal-vio-vr`** — wires *"Camera + IMU
access via UVC and USB serial → patched Basalt (fisheye624) VIO → pose injection
into Monado."* But it is self-described **experimental / "not plug-and-play.
Yet,"** with **no releases** and a *patched* Basalt. The vendored `xreal_air`
driver is **IMU-only** — it never opens the cameras.

**Verdict:** **3DoF is the honest v1; 6DoF is a research project, not vaporware
but far from a feature.** A future WP could feed the UVC streams to Monado's
Basalt with a fisheye624 calibration and inject pose — but that is a large,
speculative effort (camera calibration, extrinsics, SLAM tuning) and should not
gate v1. Document it as a stretch and move on.

---

## 3. Display pipeline — the core architecture decision

Three candidate paths. The evidence closes two of them for v1.

### (a) `comp_main` as a fullscreen Wayland window on DP-5 — **CHOSEN for V1**

Monado's real Vulkan compositor `comp_main` builds a Wayland window backend:
`comp_window_wayland.c` is compiled whenever `XRT_HAVE_WAYLAND`
(`compositor/main/CMakeLists.txt:66–67`), and `XRT_COMPOSITOR_FORCE_WAYLAND=1`
forces monado to use it. So `monado-service` opens an ordinary `xdg_toplevel`
Wayland surface, which HypXRland manages like any window. We pin+fullscreen it on
DP-5 with window rules (workspace-bind to the DP-5 workspace, `fullscreen`,
`noborder`, no animations), and the compositor scans it out to the glasses.

- **Pros:** *zero compositor changes*; single-GPU AMD; DP-5's native modes
  (60/90/120 Hz) are already what we want; trivially coexists with the desktop on
  eDP-2; matches how the wider XREAL+SBS community actually runs stereo today
  (move a window to the glasses' screen, fullscreen it).
- **Cons:** one extra compositor hop vs direct scanout — a frame of latency and a
  vsync governed by Hyprland's present loop rather than monado owning the
  flip. For a *seated 3DoF* birdbath (no positional reprojection, low
  head-velocity), this is acceptable; it is the standard Monado-on-Wayland
  tradeoff. Getting a **tear-free, low-latency, ideally direct-scanout** fullscreen
  on DP-5 is the one thing to validate (Hyprland already reports
  `directScanoutTo`/`solitary` on DP-5 — a single fullscreen surface should
  qualify).

### (b) DRM-lease direct mode — **IMPLEMENTED as opt-in (V2.2); window path stays default**

> **V2.2 update (implemented).** Option 2 below (the Hyprland config knob) is now shipped as the
> `lease` monitor-rule flag: `monitor = DP-5, …, lease` makes the `Monitor.cpp` gate ALSO offer that
> named desktop output to `drm-lease-v1` (and NOT configure it as a desktop), reusing the existing
> non-desktop code path. It is **off by default** (a stock config is byte-identical), tightly gated to
> the named connector, respects the same-DRM-backend constraint, and toggling it drives a clean
> offer↔reclaim transition (`MonitorRuleManager` + a new `CDRMLeaseProtocol::reclaim()` that withdraws
> the offer before the output returns to being a desktop). Monado needs **no code change** — its
> `comp_window_direct_wayland` backend is already compiled (`XRT_HAVE_WAYLAND_DIRECT`) and is selected
> by env (`XRT_COMPOSITOR_FORCE_WAYLAND_DIRECT=1`, connector via `XRT_COMPOSITOR_WAYLAND_CONNECTOR`).
> Driven by `scripts/xreal-mode.sh xr --direct`; `flat` reclaims DP-5 as a desktop. **Rationale for
> keeping the window path the default:** direct mode is the lower-latency path (Monado owns the flip,
> no second compositor hop), but (i) while leased DP-5 is NOT a usable desktop, so a failed XR bring-up
> leaves no fallback screen — the window path always leaves DP-5 a real monitor; (ii) direct mode was
> not yet worn-validated at ship time (needs the build+relog + Vulkan direct-display on the leased
> connector). It is opt-in until validated; promoting it to default afterwards is a one-line script
> change. The original analysis, still accurate, follows.

_Original analysis:_

Read of `DRMLease.cpp` + `Monitor.cpp`: Hyprland *has* full `drm-lease-v1`
(`ProtocolManager.cpp:224` builds a `CDRMLeaseProtocol` per DRM backend), but a
monitor is only ever advertised to lease clients through **one call site**:

```
// src/output/Monitor.cpp:289
if (m_output->nonDesktop) {
    ...  for (auto& [name, lease] : PROTO::lease) { ... lease->offer(m_self.lock()); }
    return;   // and it is NOT configured as a desktop output
}
```

and `CDRMLeaseProtocol::offer()` additionally rejects a monitor on a different
DRM backend (`DRMLease.cpp:334–355`). **DP-5 is not non-desktop** (no
`/sys/.../non_desktop`; it has an active workspace in `hyprctl monitors`), so it
is *never offered for lease* and is instead configured as a desktop output.
Monado's DRM direct-mode backends (`comp_window_direct_wayland.c`,
`comp_window_direct_randr.c`, `comp_window_vk_display.c`) would want exactly such
a leased/non-desktop connector.

Two ways to open this later:

1. **Kernel EDID non-desktop quirk** for `MRG/Air 2 Ultra` — matches how *real*
   VR HMDs (Vive, Index, Quest-link headsets) are auto-recognised: `drm_edid.c`'s
   `EDID_QUIRK_NON_DESKTOP` list, or a matching `/usr/lib/udev/hwdb.d` entry, sets
   the connector's non-desktop property, after which Sway/KWin/GNOME/Hyprland all
   *automatically* route it to `drm-lease-v1` and never paint a desktop on it.
   This is the upstream-correct fix but changes desktop behaviour (DP-5 stops
   being a usable monitor) and is a system-level quirk, not a compositor change.
   The birdbath community mostly does **not** apply it, precisely because they
   *want* the glasses usable as a flat monitor.
2. **A Hyprland config knob** — e.g. `monitor = DP-5, ..., lease` or an
   `openxr`-section allowlist — to force-offer a *named* desktop output for lease
   on demand. Small, additive change to the `Monitor.cpp:289` gate. **This is the
   V2 lever** if window-mode latency proves inadequate. Upstream precedent: no
   compositor currently exposes a "lease this desktop output" toggle — they all
   key off the non-desktop EDID bit — so this would be a HypXRland-specific
   affordance.

**For v1, (a) sidesteps all of this.**

### (c) Which GPU — confirmed single-GPU AMD

Already established in §1: DP-5 is on `amdgpu`/`card2`/`renderD129`. Point
`monado-service` at the AMD render node (it will pick the DP-5 output's GPU
naturally under Wayland), and the whole session is AMD. **No NVIDIA involvement,
no PRIME, no cross-GPU copies.**

---

## 4. Stereo / SBS output

> **RESOLVED live 2026-07-14 (Framework 16).** This §4 "highest risk" is de-risked. Correction to the
> assumption below: the glasses do **NOT** auto-advertise a 3840×1080 EDID mode after the HID 3D switch
> on this host — the connector re-probes but still exposes only its native 1920×1080 DTDs. The working
> answer is to **force an unadvertised 3840×1080 CVT-RB modeline**
> (`266.50 3840 3888 3920 4000 1080 1083 1093 1111 +hsync -vsync`); DP-5 accepted it and stayed
> `connected`, the glasses hardware-split it L/R, monado's `comp_main` came up **3840×1080 fullscreen**
> with a 2-view stereo device, and `openxr status` reached `focused`. Two extra facts learned live:
> (1) the HID `mode 3d` must precede the monado start (the driver latches view geometry at create time);
> (2) `comp_main` renders the surface at **half resolution** under `XRT_COMPOSITOR_FORCE_WAYLAND`
> (`comp_settings.c` `preferred /= 2`) → the SBS split is geometrically correct but soft until that
> `/= 2` is patched out (needs a monado rebuild). Full write-up + the corrected toggle: `07-xreal.md` §0.

`switch_display_mode` (`xreal_air_hmd.c:976–1015`) is unambiguous about how the
Ultra does stereo:

- `XREAL_AIR_DISPLAY_MODE_2D` (`0x1`): panel presents **1920×1080 mirrored/mono**;
  the driver sets view[0] full-width and **collapses view[1] to 1×1** — a mono
  device.
- `XREAL_AIR_DISPLAY_MODE_3D` (`0x3`): `info.display.w_pixels *= 2` → **3840×1080
  SBS**, then `u_device_setup_split_side_by_side` splits it into two 1920×1080
  viewports, one per OLED. FOV 46°×46°, `info.display.w_meters` doubled and lens
  separation doubled to match.

The **mode switch is a HID command**, not a DRM mode change:
`MSG_W_DISP_MODE 0x08` / `adjust_display_mode` (`:1080`), acknowledged via
`MSG_R_DISP_MODE 0x07`. (Users can also toggle it manually by holding
POWER+Brightness-Up, which the driver observes as `BUTTON_VIRT_MODE_3D 0xB`.)
So when monado brings up the xreal device in stereo, it **commands the glasses
into 3840×1080 SBS over HID**, and then the DP link must actually carry a
3840×1080 signal.

**Open validation point:** DP-5's *current* EDID advertises only 1920×1080 modes
(§1) because it is in 2D mode. In SBS mode XREAL glasses re-present a 3840×1080
EDID/mode (community confirms: after enabling SBS a *"3840×1080 monitor"* appears
in display settings). We must confirm the AMD DP link + Hyprland pick up that
mode after the HID switch, and that `comp_main` renders the two 1920-wide
viewports into that 3840-wide surface. **Distortion:** birdbath optics need
essentially none, so no per-eye warp burden — a plus for the window path.

**How xreal+monado users run this today:** the mainstream community pattern is
actually the *flat* route (SBS monitor + a 3DoF tool, §8); native
monado-`comp_main`-on-xreal is comparatively rare and lightly documented, which
means **the SBS-mode-switch + 3840 fullscreen flow is the single most important
thing to prototype and de-risk early**.

---

## 5. Runtime coexistence with the live WiVRn service

The "one XR service per box" rule stems from the **remote test driver's TCP
4242** — grep confirms `4242` appears *only* in `target_builder_remote.c:86` (and
Tracy/arduino noise). **A real xreal `monado-service` binds no TCP port.** So the
TCP concern does not apply.

The genuine shared resources are (1) the IPC socket and (2)
`active_runtime.json`:

- **IPC socket:** `XRT_IPC_MSG_SOCK_FILENAME = monado_comp_ipc`
  (`CMakeLists.txt:375`), created in `$XDG_RUNTIME_DIR` by
  `ipc_server_mainloop_linux.c`. WiVRn compiles its own socket name and, live,
  uses **`$XDG_RUNTIME_DIR/wivrn/comp_ipc`** — confirmed: the runtime dir
  contains only a `wivrn/` directory + `wivrn.pid`, **no `monado_comp_ipc`**.
  Different names ⇒ **a stock xreal `monado-service` and `wivrn-server` do not
  fight over the socket.** (If we ever want belt-and-suspenders isolation, build
  the xreal flavor with `-DXRT_IPC_MSG_SOCK_FILENAME=xreal_comp_ipc`.)
- **`active_runtime.json`:** the live file
  `~/.config/openxr/1/active_runtime.json` points at WiVRn's
  `libopenxr_wivrn.so`, and WiVRn now owns it permanently (per tonight's fix).
  **Runtime selection for an xreal app is therefore per-process
  `XR_RUNTIME_JSON=/path/to/openxr_monado-dev.json`** — the OpenXR loader honours
  that env var over `active_runtime.json`. This makes the xreal path **immune to
  WiVRn's `active_runtime` flipping during its forks** (tonight's lesson): the
  xreal launcher never reads `active_runtime.json` at all.

**v2 config key sketch** — teach the compositor to launch an XR client under a
chosen runtime without the caller managing env:

```
# hyprland-xr.conf
openxr {
    runtime_json = /home/ajg/code/Hyprland/subprojects/monado/build/openxr_monado-dev.json
    # optional per-app override table later
}
```

Implementation follows HypXRland's existing pattern of **main-thread config read
→ atomics** (never a STRING read on the frame thread — the session-killer rule);
the value is only consumed when spawning/relaunching the xreal session, so a
plain `CConfigValue<Hyprlang::STRING>` read at spawn time is fine. This is small
and additive; it is a convenience over `XR_RUNTIME_JSON`, not a prerequisite.

---

## 6. 3DoF UX in HypXRland — what degrades, what needs guards

A 3DoF (orientation-only) session breaks every assumption that depends on head
*position*. Enumerated against HypXRland's XR features:

| Feature | 3DoF behaviour | Guard needed |
|---------|----------------|--------------|
| **Roam / geofence / leash** | Position is frozen at origin; roam is meaningless, leash never triggers | Detect 3DoF (no positional tracking cap) and **disable roam/geofence/leash**; pin the layout to a fixed head anchor |
| **Recenter** | Must be **yaw-only** (a full 6DoF recenter has no translation to zero) | Recenter = zero yaw + pitch-level; ignore translation. Bind to a key and to voice |
| **Adaptive layout at head position** | Curved arc must sit at a **fixed radius around the (0,0,0) head**, not follow the body | Ship a dedicated **3DoF layout profile** (§below) |
| **Gaze features** | **Fine** — gaze is orientation, fully available | none |
| **Hand / controller input** | **Absent** — driver exposes an HMD only, no `xrt_device` for hands/controllers | UI must not require hands; **keyboard + `hypxrvoice` + gaze become primary** |
| **Plugged-state / presence gating** | See below | Fallback path |

**Presence / don-doff (the plugged-state gate).** The Air 2 has a real proximity
**wear sensor** (it powers the display off seconds after removal, on
near-instantly when worn). **But the vendored driver does not expose it.** The
HID parser has `MSG_P_DISPLAY_TOGGLED 0x6C04` (the physical display button) and
`MSG_P_BUTTON_PRESSED 0x6C05`, and a `bool display_on` — but **no worn/proximity
event**, and the device is a plain `XRT_DEVICE_GENERIC_HMD` with **no
`XR_EXT_user_presence`** capability (`xreal_air_hmd.c:1193`). So on this session:

- The OpenXR session is **always `VISIBLE`/`FOCUSED`** — the compositor's
  presence-based gating (used for WiVRn don/doff) gets no signal. HypXRland's
  visibility+settle fallback will treat the glasses as *always donned*.
- Don/doff is therefore **not reliably observable** in v1. Options: (i) accept
  always-on (fine for a seated desk display); (ii) infer doff from the firmware
  display-off (the panel goes dark and DP may drop — observable at the DRM
  connector layer, not via OpenXR); (iii) a future driver patch to surface the
  proximity byte as `XR_EXT_user_presence` (upstreamable, but out of v1 scope).
  **v1 recommendation: treat the xreal session as always-present; do not wire it
  to the WiVRn-style plugged-state machine.**

**Recommended default 3DoF layout profile** — a **curved arc at a fixed head
anchor**: 3–5 panels on a cylinder of ~1.2–1.5 m radius, ±~40–50° horizontal
spread (inside the 46° FOV per eye, so the focused panel fills the lens), slight
downward pitch for seated ergonomics, **body-locked to the recenter yaw** (not
head-locked — head-locked induces sim-sickness). Config profile keys (following
existing XR-layout config conventions): a `layout_profile = xr_3dof_arc` selector
plus `arc_radius`, `arc_span_deg`, `arc_pitch_deg`, `panel_count`, and a
`yaw_only_recenter = true` flag. Keep panels sparse — the 46° FOV is narrow, so
fewer, larger panels beat a dense grid.

---

## 7. Peripherals

- **Audio:** works today as a plain USB device — sink `81` + source `83` in
  PipeWire (§1). No XR work needed; optionally the launcher can auto-route the
  default sink to the glasses when the session starts and restore on exit.
- **Brightness / display controls:** the driver already exposes brightness over
  HID (0–100 scale, `adjust_brightness`/`MSG_W_BRIGHTNESS 0x04`,
  `xreal_air_hmd.c:1054–1076`) and the 2D/3D display-mode switch (§4). These are
  driven *by monado while the session owns the HID device* — so brightness/mode
  become monado-mediated, not separate tools. (Standalone tweaking without a
  session would use wheaney's XRLinuxDriver or `nrealAirLinuxDriver`, but that
  contends for the same HID device — don't run both.)
- **udev rule for HID access** (needed so `monado-service` can open the sensor +
  control HID interfaces without root). Standard community form, placed at
  `/etc/udev/rules.d/70-xreal.rules` (or `99-`):

  ```
  # XREAL / Nreal Air family — allow user access to the HID interfaces
  SUBSYSTEM=="hidraw", ATTRS{idVendor}=="3318", MODE="0666", TAG+="uaccess"
  KERNEL=="hidraw*", ATTRS{idVendor}=="3318", MODE="0666", TAG+="uaccess"
  ```

  (`TAG+="uaccess"` grants the logged-in seat user access via logind — cleaner
  than a blanket `0666`; keep both for robustness. The Monado project and the
  XREAL Linux driver communities both ship a rule of this shape.) **Do not test
  this live in this research pass** — writing a udev rule + `udevadm trigger`
  touches the device the live WiVRn box may care about; it is a V1 build step.

---

## 8. Alternative considered — wheaney's XRLinuxDriver / Breezy Desktop

- **XRLinuxDriver** *(GPL, open)* explicitly supports the **Air 2 Ultra**. It is
  **3DoF-only** and, by itself, **not a display** — it converts head motion into
  mouse movement / a UDP broadcast for consumers (OpenTrack, games). Uses XREAL's
  **closed SDK** for most devices. **Not OpenXR/Monado.**
- **Breezy Desktop** *(freemium, paid license)* is the display layer on top: a
  **KWin plugin (Plasma 6)** or **GNOME extension (45–50)** implementing **its
  own** virtual-display management — **not OpenXR, not Monado**. Features:
  multiple virtual monitors, **Smooth Follow** (damped body-follow), SBS 3D,
  **multi-tap recenter (2 taps) / recalibrate (3 taps)**, auto-recenter, optional
  6DoF.

**Why native OpenXR/Monado still fits HypXRland better:** Breezy is a
KWin/GNOME-coupled, partly-proprietary stack — it has no path into a Hyprland
compositor, and it is a *flat desktop projector*, not an XR scene graph.
HypXRland already owns an OpenXR session model, layout engine, gaze, voice, and
HUD; the xreal glasses should plug into *that*, as one more Monado device, so
they share the roam/layout/voice/HUD machinery WiVRn already uses. The right move
is to **steal Breezy's UX ideas**, not its architecture:

1. **Smooth-follow / body-lock** damping as the default 3DoF anchor (§6) — avoids
   the nausea of head-locked panels.
2. **Tap-count gestures via the IMU** (double-tap the temple = recenter): the
   accel spike is already in the driver's stream; a future HUD-side gesture would
   give a controller-free recenter — a natural fit with our voice-first input.
3. **Multiple virtual monitors** framing for the layout profile.

On IMU quality: Breezy/XRLinuxDriver and Monado's `xreal_air` driver **share the
same lineage** — TheJackiMonster authored both the open `nrealAirLinuxDriver` and
the Monado driver — so Monado's fusion is the *good* open path, not a downgrade.

---

## 9. Work-package breakdown

### V1 — 3DoF xreal display, **no compositor changes** (target: it just works)

| # | Item | Size |
|---|------|------|
| V1.1 | **Second Monado build flavor.** Add an xreal variant to `scripts/build-monado.sh` (or a sibling script): `-DXRT_BUILD_DRIVER_XREAL_AIR=ON` (deps `XRT_HAVE_HIDAPI` — hidapi is on the box), **real `comp_main`** (not the null-compositor test flavor), Wayland enabled. Produce a dedicated `openxr_monado-dev.json`. | **M** |
| V1.2 | **udev rule** for `3318:*` hidraw access (§7), documented as a one-time install step. | **S** |
| V1.3 | **Session profile + launcher.** A script that: (a) sets `XR_RUNTIME_JSON` to the xreal runtime json (bypasses `active_runtime.json`, immune to WiVRn flipping); (b) starts the xreal `monado-service`; (c) launches `comp_main` as a Wayland client with `XRT_COMPOSITOR_FORCE_WAYLAND=1`; (d) cleans up on exit **by tracked PID only** (never by process name — the live-compositor rule). | **M** |
| V1.4 | **Window rules to pin `comp_main` fullscreen on DP-5** (workspace-bind, `fullscreen`, `noborder`, no anims, tearing/immediate present). Validate tear-free + acceptable latency + that the SBS **3840×1080** mode comes up after monado's HID mode switch. | **M** |
| V1.5 | **3DoF layout profile** `xr_3dof_arc` + yaw-only recenter, roam/geofence/leash disabled when no positional cap (§6). Keyboard/voice/gaze as the input model. | **M** |

**V1 acceptance:** plug in the glasses → run the launcher → a curved 3-panel arc
appears in stereo on the Ultra at a fixed head anchor; head-turn looks around the
arc smoothly; a keybind/voice command recenters yaw; WiVRn/Quest sessions are
unaffected (separate socket, separate runtime json); no NVIDIA involvement.

### V2 — polish & compositor affordances

| # | Item | Size |
|---|------|------|
| V2.1 | **`openxr:runtime_json` config key** (§5) — main-thread read → spawn-time consumption; convenience over `XR_RUNTIME_JSON`. | **S** |
| V2.2 | **DONE — DRM-lease force-offer knob** (`monitor=DP-5,…,lease`) at the `Monitor.cpp` gate + `CDRMLeaseProtocol::reclaim()` + `MonitorRuleManager` offer↔reclaim transition; unlocks monado direct mode (`xreal-mode.sh xr --direct`, monado env-only). Off by default; window path stays default. Code-verified; needs relog+wear to confirm end-to-end. | **M** |
| V2.3 | **Tracking-caps-aware UX**: detect 3DoF vs 6DoF at runtime and switch layout/roam behaviour, so a future 6DoF path lights up automatically. | **M** |
| V2.4 | **Presence/brightness niceties**: optional auto sink-routing; optional driver patch to surface the proximity byte as `XR_EXT_user_presence` (upstreamable). | **M** |
| V2.5 | **Cherry-pick eval** of `95b320d41` (scanout compensation) if latency needs it. | **S** |

### Risks

- **SBS mode-switch + 3840 DP link (highest).** The whole stereo path hinges on
  the glasses re-presenting a 3840×1080 mode after monado's HID command and the
  AMD DP link + Hyprland accepting it. Prototype this *first*. Fallback: run the
  device in **2D/mono** (still a usable single-panel XR display) if SBS proves
  fiddly.
- **Window-mode latency/vsync.** Acceptable for seated 3DoF but must be measured;
  V2.2 (lease knob) is the escape hatch.
- **HID contention.** Only one owner of the `3318:0426` HID device at a time — do
  not run XRLinuxDriver/`nrealAirLinuxDriver` alongside the monado session.
- **Yaw drift.** Inherent to accel+gyro-only fusion (mag unused); mitigate with
  easy, frequent recenter (key + voice + future tap gesture). Not fixable without
  driver work.
- **No don/doff.** Session is always-present; do not wire it to the WiVRn
  plugged-state machine (§6).

---

## Open questions for the user

1. **Display mode preference:** stereo **SBS 3D** (true per-eye, the driver's
   full path, but depends on the 3840 mode coming up cleanly) vs **2D mono** (dead
   simple, still a head-tracked floating display)? V1 can ship mono first and add
   SBS once de-risked — acceptable?
2. **Desktop-vs-XR for DP-5:** do you want DP-5 to *remain* a usable flat desktop
   monitor (favours the window path + never applying the non-desktop quirk), or
   are the glasses XR-only (which would make a kernel non-desktop quirk / lease
   path cleaner in V2)?
3. **How married to native OpenXR are you here** vs just wanting a good floating
   display? If the latter, the window-path v1 is basically that already; if the
   former, the roam/layout/voice/HUD integration (V1.5) is where the value is.
4. **6DoF appetite:** worth a *research* WP later to attempt the
   `xreal-vio-vr`/Basalt path with the (live) UVC cameras, or firmly park 6DoF?
5. **Presence:** is "always-on while plugged" fine for v1, or do you want the
   firmware display-off (doff) observed at the DRM layer to pause the session?
