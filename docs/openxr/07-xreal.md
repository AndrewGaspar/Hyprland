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

**`xr`** does, in order: verify the glasses are present over HID → `xreal-ctl mode 3d` (HID switch to
SBS) → wait for the DP connector to advertise a 3840-wide mode, then set it (`hyprctl keyword
monitor`) → `systemctl --user start monado-xreal.service` (after importing `WAYLAND_DISPLAY`) →
`hyprctl keyword openxr:runtime_json <xreal manifest>` → `hyprctl openxr disable && enable`. The
xreal Monado's `comp_main` opens a Wayland toplevel (app-id `openxr`), which the `example/xreal.conf`
window rules fullscreen on the glasses' DP output.

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

- **GPU: single-GPU on both target boxes, so `openxr:gpu` stays unset.** On the AMD desktop the
  glasses' DP output (DP-5) is on the **same** GPU (890M iGPU, `renderD129`) that Hyprland renders the
  desktop with — it *is* the primary, so following the primary is correct. The RTX 5070 is a different
  card that never touches this path. On the Lunar Lake laptop there is only one GPU. The wrong-GPU guard
  in `start()` (`XRGpuProbe`) verifies the XR EGL node matches the runtime's composite GPU and refuses
  safely if it's ever wrong — so leaving `openxr:gpu` unset is both correct and fail-safe here. Only set
  it if your glasses' DP connector is driven by a *different* GPU than Hyprland's primary.

- **Doff detection: there is none over HID — rely on UNPLUG.** The driver does not surface the wear
  sensor, and the device advertises no `XR_EXT_user_presence`, so the session always reads
  `visible`/present. There is no reliable "took the glasses off" signal. What *does* remove the display
  is **unplugging the USB cable**: that drops DP-5 (the connector disappears) **and** the `3318:0426`
  HID device, so `monado-service` loses the HMD, the OpenXR session ends, and HypXRland's
  **session-loss path** runs — under `monitors_follow_session = session` (the profile's setting) the XR
  monitors unplug and their workspaces evacuate to your remaining displays, exactly like yanking a
  physical monitor. Re-plugging + `xreal-mode xr` brings it back. This is why the profile uses
  `session` (not `visible`): `visible` gates on a presence/visibility signal this device can't provide,
  so it would never plug; `session` keeps the monitors up for the life of the (always-present) session
  and lets the physical unplug be the teardown trigger. Verified against the code: `session` mode keys
  purely on session existence (`OpenXRManager.cpp` follow-mode handling), and session teardown on
  runtime/device loss drives the same unplug that a physical disconnect would.

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
   you. `SUPER+Home` recenters the selected panel.
6. **Gaze grab:** `SUPER+SHIFT+G` while looking at a panel grabs it (head-forward ray, no eye
   tracking); `SUPER+ALT+=/-` push/pull; tap again to drop.
7. **`xreal-mode flat`:** the `openxr` window closes, the DP output returns to 1920×1080, and the
   glasses are an ordinary head-locked monitor again. Run it a second time — it should be a safe no-op.
8. **Unplug behavior:** with `xreal-mode xr` active, pull the USB cable. The XR session should end and
   the XR monitors unplug (workspaces evacuate to your other displays). Re-plug + `xreal-mode xr`
   restores. (This is the stand-in for doff — there is no HID doff signal.)
9. **WiVRn untouched:** if you use WiVRn/Quest, confirm `xreal-mode flat/xr` never stops
   `wivrn.service` and a running WiVRn session is unaffected (separate socket + separate runtime json).
10. **Idempotence / unplugged safety:** `xreal-mode xr` with the glasses unplugged should refuse
    cleanly ("no XREAL device detected"); `xreal-mode flat` with nothing active should be a quiet no-op.

Report back the SBS result (step 4) especially — that's the one architectural risk we couldn't
de-risk without the live device.
