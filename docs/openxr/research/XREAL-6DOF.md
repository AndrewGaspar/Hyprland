# Research: 6DoF tracking for the XREAL Air 2 Ultra on Linux / Monado

Feasibility deep-dive (2026-07-14). **No implementation, no live XR, no device
claim.** Companion to `XREAL-3DOF.md` (@ `89ebf969`), which found the 3DoF
display path viable and parked 6DoF as "a research project, not vaporware but far
from a feature." This memo goes deep where that one skimmed: the camera hardware
path, factory calibration, the community stack, Monado's SLAM plumbing at our
pin, Basalt practicalities, XREAL's own software, and a GO/NO-GO threshold. The
user is **not** pursuing this now; the job of this doc is to make a future
decision takeable in minutes.

> **UPDATE 2026-10-12 — REASSESSMENT: NO-GO → GO for a prototype.**
> A fresh web survey + local code audit (2026-10-12) finds that **all three original
> structural blockers are now cleared or have a known solution**, so the verdict
> flips to **GO for a bounded prototype** (not yet GO for a shipped feature):
>
> - **Calibration — SOLVED** (2026-07-14, `XREAL-CAMERA-CALIB.md`): the full factory
>   stereo calibration is the `SLAM_camera` object in the HID JSON blob the driver
>   already reads (`fisheye624`, 12 `kc` coeffs, ~137.3 mm baseline,
>   `rolling_shutter_time = 0` ⇒ global shutter). §3's "no camera calibration
>   anywhere" is **superseded**. *(CERTAIN — extracted live.)*
> - **Sync — tractable, one check left**: the device-ns frame timestamp (R1, §2.1)
>   stands; still pending one simultaneous camera+IMU **epoch** check. *(HIGH.)*
> - **Pixel layout — PUBLICLY DECODED** (§2.2, new). The "YUYV 640×241" stream is
>   really **640×482 8-bit grayscale**: rows 0–479 are the image, scrambled as
>   **128 chunks of 2400 bytes** under a fixed permutation (`CHUNK_MAP[128]`) with a
>   per-frame rotating start; row 480 = telemetry, row 481 = padding. No unlock
>   command; the stream is live on enumerate. Three independent public decoders
>   agree. The old "vendor Y/U/V-lane interleave" guess (§1, §2.1) is **superseded**.
>   *(HIGH — three independent implementations; not yet re-run on this box.)*
> - **Prior art exists**: Benjamin Berg + Luka Panio's WIP Monado branch
>   (`gitlab.freedesktop.org/benzea/monado` `main`, 15 commits 2025-07..2026-04,
>   no MR) already has a camera source, a ~730-line Basalt tracker, and a
>   fisheye→KB4 calibration fit for this exact device (§4.1, new). No public
>   quality numbers.
> - **Quality ceiling (GO-cond-3) is still NOT met by any public demo** — reframed
>   from "wait for someone else" to **"measure it ourselves"** (§8). Risk to keep in
>   view: 6DoF may trade 3DoF's slow **yaw drift** (§2.3, new — mostly a fixable gyro
>   bias problem) for **cm-scale swim** (vendor stack RPE 1.29 cm / APE 8.44 cm,
>   ~doubling in low-feature scenes).
>
> **Status / plan:** monado branch `xreal-camera-record` (camera source + EuRoC
> recording via Monado's existing `t_euroc_recorder`) is **in progress**, to build a
> dataset library *before* SLAM integration; monado branch `xreal-gyro-bias`
> (stillness-gated online gyro-bias estimation, opt-in) is in progress for the 3DoF
> drift. **Revised effort: prototype ~1–2 weeks** (port Berg's tracker onto our pin);
> **daily-usable: open-ended** (quality-tuning bound). Revised WP table in §8.
> Historical text below is kept, with superseded passages marked.

> **UPDATE 2026-07-14 — R1 empirically resolved (live capture on this box).**
> Streamed the Ultra's `video2` (+ the `video3` UVC-metadata node) and decoded the
> 241st row. **It carries a per-frame device-clock timestamp: a 48-bit little-endian
> nanosecond counter at row-240 byte offset 0, epoch = device power-on, running 1:1
> with real nanoseconds** (≈227 ppm off host `CLOCK_MONOTONIC`, i.e. the device's own
> oscillator). This is exactly the "one thing that could rescue" the §2 sync blocker.
> The camera↔IMU-sync verdict softens from "structural, needs online `td` eating USB
> jitter" to **"tractable — the frame timestamp lands each image on the device clock
> *before* USB transport, leaving at most a constant camera↔IMU epoch offset to
> measure once"** (the IMU HID counter is *also* a device-ns counter per the driver;
> a bit-exact epoch match couldn't be confirmed this pass — hidraw was root-only, no
> sudo). **The overall verdict stays NO-GO**: calibration (§3 / GO-cond-1) and
> demonstrated desktop quality (GO-cond-3) are untouched. Full byte map + regression
> in **§2.1**.

Evidence base (all read-only this pass):

- **Live USB descriptors** of the plugged-in Ultra (`3318:0426`) via `lsusb -v`
  (descriptor read, no claim, no stream) and **V4L2 node listing**
  (`/dev/v4l/by-id`, `by-path`).
- **Live V4L2 capture** (2026-07-14 follow-up, R1): 300 frames of `video2` (YUYV
  640×241 @60) with per-frame `CLOCK_MONOTONIC` timestamps + the `video3`
  UVC-metadata node, via a ctypes V4L2 mmap script. Used to decode the 241st row.
- **Vendored Monado** `subprojects/monado` @ `c2ddab59` — `drivers/xreal_air`,
  `drivers/wmr` (the SLAM-feed template), `drivers/twrap` (generic SLAM wrapper),
  `auxiliary/tracking/t_tracker_slam.cpp`, `auxiliary/util/u_sink.h`, and the VIT
  plugin header `external/vit_includes/vit/vit_interface.h`.
- **Community stack** `DeskUnreal/xreal-vio-vr` — full repo tree + key files
  (README, ROADMAP, run.sh, the calibration template, Basalt patch set).
- **Basalt-for-Monado** forks (`CIFASIS/basalt-xr`, `ChristophHaag/basalt-monado`).
- **Academic ground truth**: *XR Reality Check* (arXiv 2508.08642), which
  benchmarks the Air 2 Ultra's tracking against Apple Vision Pro et al.
- **XREAL developer docs** (NRSDK / XREAL SDK 3.0). URLs cited inline.
- **2026-10-12 reassessment** (web survey + local code audit): the three public
  pixel decoders (`DeskUnreal/xreal-vio-vr` `parts/xreal.py`,
  `sean9061/xreal-air2ultra-camera`, `mazeasdamien/myXreal`), Berg/Panio's WIP
  Monado branch (`benzea/monado`), upstream Monado MRs, canonical Basalt
  (`mateosss/basalt`), the Monado SLAM Dataset paper (arXiv 2508.00088), XREAL SDK
  3.1.0, and a read of the vendored `xreal_air` + `m_imu_3dof.c` for the drift
  question. Sources at the end.

---

## TL;DR / verdict

> **2026-10-12 revised verdict: GO for a prototype.** Items 3 and 4 below are
> **superseded** (calibration is on-device; the community stack now includes a
> working pixel decoder and a WIP Monado tracker by Berg/Panio), item 1's "YUYV"
> reading is superseded by the 640×482-grayscale descramble (§2.2), and item 6's
> quality ceiling is now something **we measure** rather than a reason to wait.
> The original 2026-07-14 items follow unchanged for history.

1. **The cameras are real, live, and enumerated — as ONE UVC stream, not two.**
   The Ultra exposes a single UVC video-streaming interface offering exactly one
   format: **YUYV, 640×241, 60 fps** (`dwFrameInterval 166667` × 100 ns = 16.67 ms;
   `dwMaxVideoFrameBufferSize 308480 = 640×241×2`). Each frame is ≈240 image rows +
   **1 embedded metadata/timestamp row** (confirmed live, §2.1) — **not** two
   independent camera nodes. The two grayscale-IR eyes arrive as **consecutive
   frames** (60 fps = 30 stereo pairs/s; R1 revises the earlier "packed side-by-side"
   guess). On this box it is `video2`
   (`usb-XREAL...-video-index0`); `video3` is the paired UVC metadata node. A
   vendor **extension unit** (`GUID {a29e7641-de04-47e3-8b2b-f4341aff003b}`) sits on
   the VideoControl interface — the undocumented control surface for
   enabling/configuring the cameras.
2. **The cameras DO expose a device-clock timestamp (R1, confirmed live) — the
   camera↔IMU sync problem is smaller than it first looked.** Original concern:
   IMU arrives over **HID**, cameras over **UVC** (separate USB pipes), both merely
   kernel-timestamped at host arrival with **no exposed hardware sync**, so a
   camera-IMU offset (`td`) would have to be estimated online while eating USB
   scheduling jitter. **But the 241st row carries a per-frame device timestamp**
   (48-bit LE nanosecond counter, epoch = power-on, 1:1 with real ns — §2.1). That
   stamps each frame on the *device* clock **before** USB transport, so USB jitter
   no longer corrupts the frame time. The IMU HID counter is *also* a device-ns
   counter (`xreal_air_hmd.c` treats its delta as ns). If the two share the on-device
   oscillator's epoch — the strong hypothesis, since the two on-device clocks I *did*
   observe differ only by a **constant** offset — alignment reduces to a **one-time
   epoch measurement**, not per-frame online `td`. Not yet bit-confirmed (IMU HID was
   root-only; no sudo this pass). Net: this dropped from *the* structural blocker to
   a tractable, mostly-de-risked item.
3. *[SUPERSEDED 2026-07-14 by `XREAL-CAMERA-CALIB.md`: the blob has a full
   `SLAM_camera` object; the driver parser just discards it.]*
   **Factory calibration over HID is IMU-only — there is NO camera calibration
   anywhere we can read.** `xreal_air_hmd.h:76–109`: the factory blob is accel/gyro/
   **mag** bias, scale, cross-axis quats, and `imu_noises[4]` — and nothing else. No
   intrinsics, no fisheye params, no stereo baseline, no `T_imu_cam`. **Camera
   calibration would have to be produced per-device by hand (Kalibr).**
4. *[PARTLY SUPERSEDED 2026-10-12: `xreal-vio-vr` is still a stub apart from its
   descrambler, but Berg/Panio's `benzea/monado` branch is a real WIP Monado
   integration — §4.1.]*
   **The community stack is a stub, not a shortcut, and it doesn't even target
   Monado.** `DeskUnreal/xreal-vio-vr` (GPL-3, ~15 stars, 16 commits, *"Not
   stable — everything is subject to change"*) is a **prototype**: `scripts/run.sh`
   is a one-line TODO, its calibration template is **all placeholders**
   (`intrinsics: [fx, fy, cx, cy]`), its MVP is only GStreamer frame+IMU streaming,
   and its actual pose sink is a **custom SteamVR/OpenVR driver** (`openvr/`,
   `xr/steamvr-bridge/`) fed by **ORB-SLAM3 or patched Basalt** — **not** Monado's
   `t_slam`. It gives us reference calibration *structure* and a Basalt-fisheye624
   patch pointer, nothing runnable.
5. **Monado's SLAM plumbing is clean and the driver work is well-templated — but
   it is 100% ours to write.** At `c2ddab59` Monado uses the modern **VIT plugin
   interface** (`vit_interface.h` v2.0.1): Basalt is an external `libbasalt.so`
   loaded via `VIT_SYSTEM_LIBRARY_PATH`, fed programmatic per-camera/IMU
   calibration. `drivers/wmr` is the exact template (open camera source → push IMU
   → fill `t_slam_camera_calibration`/`t_slam_imu_calibration` → `t_slam_create` →
   connect sinks), and `u_sink_stereo_sbs_split_create` already exists to split the
   Ultra's single combined frame into left/right. The `xreal_air` driver has
   **zero** camera code today (`grep` for camera/uvc/v4l2/slam in the driver =
   empty). This is a multi-week driver project.
6. **The quality ceiling is mediocre — and worst precisely at a desk.** *XR
   Reality Check* measures the Air 2 Ultra at **APE 8.44 cm / RPE 1.29 cm average**,
   ~**233% worse APE than Apple Vision Pro**, and it **degrades sharply in
   low-feature scenes** (APE 9.06 → **18.19 cm**, +101%). A user seated facing flat
   monitors and a blank wall *is* the low-feature failure case. So the one workload
   we'd want it for — anchored desktop monitors — is close to its worst case.

**VERDICT (2026-10-12): GO for a prototype** — record a dataset library first
(`xreal-camera-record`), then port Berg's tracker + Basalt, then *measure* anchored-
monitor swim against 3DoF+bias-fix before deciding whether it ships. The
2026-07-14 verdict below is retained for history.

**~~VERDICT~~ (2026-07-14, superseded): NO-GO for now — park it, don't build it.** *(R1 update: one of the three
original structural blockers — camera↔IMU sync — is now substantially de-risked by
the confirmed device-clock frame timestamp (§2.1); the verdict is unchanged because
the other two stand.)* The remaining blockers are structural (**no factory camera
calibration, per-device Kalibr required**) and the payoff is a ~cm-drift pose that is
weakest in exactly the seated-desk scenario. 3DoF + easy recenter (per `XREAL-3DOF.md`) is the honest
product. **Revisit only if all three GO conditions in §8 flip true** — most
plausibly if XREAL or the community ships a *bundled per-device camera calibration
path* and someone demonstrates usable desktop-anchored 6DoF on this exact hardware.

---

## 1. The camera hardware path (RQ1)

**Enumeration (live).** `/dev/v4l/by-id` shows two nodes on one USB function
(`...usb-0:1:1.3`): `usb-XREAL_XREAL_Air_2_Ultra_ZBBM5DZFMP-video-index0` →
`video2` and `-index1` → `video3`. This is **one UVC device**, not two cameras:
`video2` is the streaming node, `video3` the UVC metadata node (the standard UVC-1.5
two-node layout). The audio + HID interfaces on the same device are separate
functions.

**Formats offered (from `lsusb -v`, descriptor read only):**

- VideoStreaming interface, **one** format, **one** frame size:
  - `guidFormat {32595559-0000-0010-8000-00AA00389B71}` → FourCC **"YUY2"/YUYV**
    (packed 16 bpp).
  - `wWidth 640`, `wHeight 241`, `dwFrameInterval 166667` (100 ns units) → **60 fps**.
  - `dwMaxVideoFrameBufferSize 308480` = 640 × 241 × 2, i.e. the whole stereo pair
    rides in **one** frame.
- A VideoControl **extension unit**, `guidExtensionCode
  {a29e7641-de04-47e3-8b2b-f4341aff003b}` — vendor-proprietary controls (almost
  certainly camera enable / mode / possibly IMU-camera sync). Undocumented.

**Interpretation.** 640×**241** is the tell: ≈240 rows of image + **1 row of
embedded frame metadata** (a common VIO-camera trick that carries a per-frame
device timestamp and exposure info). **→ Confirmed live (§2.1): the 241st row IS a
telemetry row carrying a device-clock nanosecond timestamp, a stereo-pair counter,
and exposure/gain.** The eye packing was originally guessed as side-by-side ~320-wide
views or YUYV-lane column-interleave; the live capture **revises** this — the two
eyes arrive as **consecutive frames** (60 fps = 30 stereo pairs/s), and the intra-frame
pixel layout is a **vendor Y/U/V-lane interleave that does not resolve as a clean
320+320 grayscale raster** (§2.1). So `u_sink_stereo_sbs_split_create` is *not* a
drop-in; the exact de-interleave must be reverse-engineered first. `u_sink_create_..._or_l8`
still applies for YUYV→L8 once the layout is known.

> **[SUPERSEDED 2026-10-12 — §2.2]** The "Y/U/V-lane interleave" guess was wrong.
> The YUYV label is a container: the 308,480-byte frame is **640×482 8-bit
> grayscale** (rows 0–479 image in 128 permuted 2400-byte chunks; row 480 telemetry;
> row 481 padding). The "YUYV→L8" sink and the SBS splitter are both the wrong tool —
> the pipeline is *reinterpret as L8 640×482 → unpermute chunks → crop 480 rows →
> route by camera ID*. One eye per frame (consecutive-frame stereo stands).

**[2026-10-12] Shutter — RESOLVED** by the factory calibration
(`rolling_shutter_time = 0`, `XREAL-CAMERA-CALIB.md` §1.4): **global shutter /
global-equivalent**. The "leaning rolling" paragraph below is superseded.

**Shutter type.** Not stated in any descriptor or teardown found. *XR Reality
Check* explicitly notes manufacturers restrict this disclosure; it uses the
RealSense D435i (global-shutter) only as a *reference* platform, and does **not**
attribute global shutter to the Ultra. For an 83 g birdbath at 60 fps the pragmatic
assumption is **rolling shutter** — a real VIO-quality handicap (Basalt can model a
rolling shutter readout but it costs accuracy). Treat "global vs rolling" as an
**unresolved risk**, leaning rolling.

**Resolution/rate discrepancy.** *XR Reality Check* reports "640×480 grayscale,
30 fps" for the Ultra's cameras; the live descriptor says **640×241 @ 60 fps** in
one combined frame. These reconcile if the paper reports the *de-interleaved* view
(two 640×240-ish images) at an effective per-eye rate, or reconfigured via the
vendor XU. Trust the descriptor for what the kernel will actually deliver.
**[RESOLVED 2026-10-12]** Exactly the first reconciliation: each 60 Hz UVC frame is
*one* eye's 640×480 grayscale image (sensor rotated 90° → the calibration's
`[480, 640]` w×h portrait readout), alternating eyes = 30 stereo pairs/s.

---

## 2. IMU–camera time sync (RQ1, the load-bearing risk)

The driver's IMU path (`xreal_air_hmd.c`):

- IMU samples arrive over **HID** with a **device-side counter** (`s->timestamp`,
  rollover-handled at `:575`), but the driver **anchors them to the host clock**:
  `now_ns = os_monotonic_get_ns(); timestamp_ns = now_ns - inter_sample_duration`
  (`:557,587`), then forces monotonicity (`:590`). IMU rate is ~200 Hz (community /
  *XR Reality Check*).
- Cameras arrive over a **different USB pipe** (UVC), kernel-timestamped at DMA
  completion.

There is **no host-visible shared hardware clock** across the HID IMU and the UVC
camera. Consequently a VIO backend must **estimate the camera-IMU offset `td`
online** (Basalt supports this) while eating **USB-scheduling jitter** on both
streams. Contrast WMR/Rift-S/Index, where camera and IMU are delivered on a single
synchronized device stream — which is *why* those are the drivers that feed SLAM
today. The Ultra's split-bus design is the structurally hard part.

**The one thing that could rescue this**: if the 241st metadata row (§1) contains
a device-clocked frame timestamp on the *same* counter the IMU HID reports, then
camera and IMU can be aligned in device time and jitter collapses. **This is worth
checking first in any future attempt** — it is the difference between "hard but
good" and "hard and jittery." **→ Checked (R1, §2.1): the row DOES carry a
device-clock nanosecond timestamp. The "same counter as the IMU" half is the strong
hypothesis but not yet bit-confirmed.**

---

## 2.1 R1 resolved — the 241st row is a device-timestamp/telemetry row (live, 2026-07-14)

Streamed `video2` (YUYV 640×241 @60) for 300 frames via a ctypes V4L2 mmap capture,
recording each frame's `CLOCK_MONOTONIC` kernel timestamp (`VIDIOC_DQBUF`) alongside
row 0 (image) and **row 240** (the "241st" row). Captured the `video3` UVC-metadata
node in parallel. Scripts in the agent scratch (`cap.py`, `decode.py`, `decode2.py`,
`img.py`); nothing from them is committed.

**The 241st row is not image data — it is a per-frame metadata/telemetry row.** Of
its 1280 bytes, only the first ~70 are structured fields (the tail mirrors image
bytes / is undecoded). Little-endian byte map, row 240:

| offset | width | field | evidence | confidence |
|---|---|---|---|---|
| **0** | 6 (u48; u64 with top zero) | **device timestamp, nanoseconds, epoch = power-on** | per-frame Δ = 16.667 ms (60 Hz); span/host-span = **1.000242**; regression slope vs host ns = **1.000227** (device oscillator, ~227 ppm fast); value ≈ 4.310×10¹² ns ≈ **71.8 min uptime** = time since the glasses were plugged in | **HIGH** |
| 18 | 1 (u8) | **stereo-pair counter** (increments once per **2** frames → 30 Hz) | 1,1,2,2,3,3,… lock-step with frame pairs | **HIGH** |
| 22 | 3 (u24) | **exposure** (≈ ns; 7,992,500 ≈ 7.99 ms) | constant across capture, zero during warm-up | MED |
| 26 | 1 (u8) | **gain / exposure index** (=100) | constant, appears with exposure | MED-LOW |
| 51–55 | 5 | static magic/config (`80 02 e0 01 80`) | never changes | — |
| 62 | 5 | **second device-ns timestamp**, per-*pair* | identical within a pair, +33.333 ms per pair; **constant** ≈3298.5 s offset from the offset-0 clock | MED-HIGH |
| 6–17, 27–61, 63+ | — | zero / static / undecoded (image spill in tail) | — | — |

**What the numbers establish (the load-bearing result):**

- **Row 240 offset 0 is a real device-clock timestamp in nanoseconds.** Its per-frame
  delta is 16.667 ms and it tracks host `CLOCK_MONOTONIC` at 1.000227 slope — a fixed
  ~227 ppm crystal offset, i.e. it is the **device's own free-running oscillator**
  exposed to the host, not a host-derived or arbitrary tick. The magnitude equals the
  device's power-on uptime.
- **Two on-device clocks, one oscillator.** The offset-0 and offset-62 timestamps run
  at the same rate and differ by a **constant** ~3298.5 s (drift < 0.5 ms over the
  5 s capture). That is the fingerprint of two counters off the *same* oscillator with
  *different epochs* — which is precisely the situation to expect for a
  camera-timestamp vs an IMU-timestamp on this single-MCU device.

**Bonus finding — frame/eye structure (revises §1).** The pair-counter (byte 18)
increments every **two** UVC frames: the device delivers **60 frames/s = 30 stereo
pairs/s**, the two eyes as **consecutive frames**, not side-by-side in one frame. The
two frames of a pair are exposed within **~±15 µs** of each other (offset-0 timestamps
nearly equal within a pair, then +33.333 ms to the next pair). This reconciles *XR
Reality Check*'s "30 fps" with the descriptor's "60 fps". Frame mean-brightness
alternates cleanly (≈64.6 vs ≈71.9) frame-to-frame, consistent with two cameras of
slightly different response. *[2026-10-12: the rest of this paragraph is
**superseded by §2.2** — the packing is a chunk permutation of 8-bit grayscale, not
a Y/U/V-lane interleave; the "row-striping" seen here is the 2400-byte chunk
scramble (2400 B = 3.75 rows of 640).]* **Exact intra-frame pixel packing stays open**: a naïve
YUYV-luma raster does not resolve to a clean grayscale image (persistent horizontal
row-striping even after row de-interleave), so the eye image is a **vendor interleave**
of the Y/U/V lanes, **not** the clean 320+320 side-by-side the original §1 guessed.
Decoding that packing is out of R1's scope (it does not affect the sync verdict) and
is left as a first task for any real driver work (Monado's `u_sink` splitters assume a
known layout).

**Clock-domain verdict: SAME-DOMAIN is the strong hypothesis; not yet bit-confirmed.**
The camera side is proven: frames self-timestamp on a device-ns clock **before** USB
transport, so USB scheduling jitter no longer corrupts the frame time (the original
§2 fear). The IMU side is *also* a device-ns counter (`xreal_air_hmd.c:575` /
`xreal_air_packet.c:300` — a `u64` the driver's delta logic treats directly as ns).
Because both live on one MCU/oscillator, the worst case is a **constant** camera↔IMU
epoch offset (exactly like the offset-0/offset-62 pair above), which is measured
**once**, not tracked per-frame. **What's missing to close it fully:** a simultaneous
read of a camera frame timestamp and an IMU HID timestamp to confirm the epoch
relationship. That needs IMU HID access — blocked this pass (the four XREAL
`hidraw` nodes are `crw------- root:root` with no udev rule; task scope forbids
`sudo`). So: **sync is de-risked from "structural" to "one measurement from
confirmed," but honesty requires flagging that final measurement as still TODO.**

**A timestamp does not un-block calibration.** This flips *only* the sync sub-blocker.
Per-device camera calibration (§3) and the low-feature desktop quality ceiling (§8)
are untouched, so the overall verdict does not move (see revised §8).
*[2026-10-12: calibration has since been solved (`XREAL-CAMERA-CALIB.md`) and the
verdict has moved — see the top update block and §8.]*

**[2026-10-12] Byte-map cross-check vs public decoders.** Because 640×241×2 =
640×482, YUYV "row 240" here is grayscale **row 480** in §2.2's framing, so the
offsets above carry over unchanged. Public decoders agree on: u64 device-ns
timestamp @0 (matches); **pair counter @18–19** (u16, i.e. the u8 @18 above plus its
high byte); a timestamp @**0x3e (=62)** that sean9061 documents as **µs**, not ns
(our "second device-ns timestamp" reading is therefore *disputed* — re-decode before
using it). The static `80 02 e0 01 80` @51 reads naturally as u16 **640** + u16
**480** (+ a byte) — width/height of the descrambled image *(MED, interpretation)*.
Camera-ID byte: see §2.2.

---

## 2.2 Pixel layout — publicly decoded (2026-10-12 survey)

Three independent public implementations agree on the layout *(HIGH; not yet
re-run on this box — first task of `xreal-camera-record`)*:

- `DeskUnreal/xreal-vio-vr` **`parts/xreal.py`** — an `xrealultra2dec` GStreamer
  element (credited **Benjamin Berg + Ani**, 2025-05-07). The only non-stub part of
  that repo.
- `sean9061/xreal-air2ultra-camera` **`docs/PROTOCOL.md`** — macOS, MIT, 2026-07.
- `mazeasdamien/myXreal` — Windows C++, origin of the `CHUNK_MAP` name, 2026-08.

**Layout:**

| item | fact |
|---|---|
| container | UVC advertises YUYV 640×241; reinterpret the 308,480 bytes as **640×482 L8** |
| image | rows **0–479** = 307,200 B = **128 chunks × 2400 B**, shuffled by a **fixed** permutation table `CHUNK_MAP[128]` |
| rotation | the permutation **start rotates per frame**: start = the chunk whose **first 128 bytes have the smallest sum** (i.e. the black fisheye border) |
| row 480 | telemetry (§2.1 byte map) |
| row 481 | padding |
| orientation | sensors rotated **90°**; the two cameras are mounted **180° opposed** to each other (one needs a 180° flip relative to the other) |
| enable | **no unlock/XU command** — the stream is live on enumerate (resolves `XREAL-CAMERA-CALIB.md` open question 4) |
| exposure | UVC `ExposureTimeAbs` **1–159** (0.1–15.9 ms); auto mode is **shutter-priority only** |
| noise | **8-tap column fixed-pattern noise** — worth a per-column offset correction before feature detection |

**Telemetry (row 480):** u64 device-ns timestamp @0; pair counter @18–19, **shared
by both frames of a pair**; µs timestamp @0x3e. **Camera-ID byte is disputed:**
sean9061 says **column 58 = `0x20`/`0x21`**; Berg/DeskUnreal say **byte `0x3b`
(=59) nonzero ⇒ right**. These may be the same field read at different
granularity — verify on our captures.

**Arrival order is NOT a reliable camera identity.** One public capture saw 48
pairs in one order and 10 in the other. Route frames by the telemetry ID byte, never
by "first frame of a pair = left".

---

## 2.3 3DoF drift — what it actually is (2026-10-12 code audit)

The slow drift seen in today's 3DoF mode is **primarily yaw integrating residual
gyro bias**, and it is a fixable 3DoF problem independent of 6DoF *(HIGH — code
read; drift magnitude MED, one hobbyist capture)*:

- `m_imu_3dof.c` `gyro_biasing()` only runs when the debug-GUI **`manually_fire`**
  flag is set — and **nothing sets it** in normal operation.
- `xreal_air` applies only the **static factory** gyro bias (not even the
  temperature-indexed `gyro_bias_temp_data` table the blob carries —
  `XREAL-CAMERA-CALIB.md` §1.4).
- Gravity corrects **pitch/roll only**; yaw is unobserved.
- The magnetometer is read but **unfused** — and prior art deliberately excludes it
  indoors (desk steel / monitors), so it is not the fix.

**Prior-art numbers:** XREAL 1S ≈ **2.15°/min** still-yaw drift, ≈ **0.03°/s** bias
walk over a 10-min warm-up (`kevindurb/xreal-linux` `docs/findings.md`, single
hobbyist capture). **Breezy / XRLinuxDriver**: connect-time calibration + online
stationary bias estimation (Fusion library); it hardcodes `provides_position=false`
for XREAL devices.

**Fix in progress:** monado branch **`xreal-gyro-bias`** — stillness-gated online
bias estimation (Fusion/`FusionOffset`-style), behind an opt-in flag. This is the
baseline 6DoF must beat: if 3DoF + online bias is stable enough at a desk, 6DoF's
value is roaming, not drift.

---

## 3. Factory calibration (RQ2)

> **[SUPERSEDED 2026-07-14 — see `XREAL-CAMERA-CALIB.md`.]** The camera calibration
> *is* on the device: a top-level `SLAM_camera` object in the same HID JSON blob
> (`fisheye624`, `fc`/`cc`, 12 `kc` coeffs, per-camera `imu_p_cam`/`imu_q_cam` (JPL),
> ~137.3 mm baseline, `rolling_shutter_time = 0`). The `xreal_air` parser keeps only
> `IMU` and discards the rest. Kalibr is demoted to an optional backstop. The
> analysis below is kept for history only.
>
> **Model-conversion note (2026-10-12):** Monado's `t_tracking.h` distortion enum has
> **no fisheye624**, and VIT 2.0 offers only NONE/RT4/RT5/RT8/**KB4**. Options: fit
> KB4 to the factory fisheye624 (Berg's approach, §4.1 — tinyceres over a 21×21 grid,
> as Rift S does), or hand Basalt a calib file using its native `Fisheye624`
> (in `basalt-headers` since 2025-06-30). Berg's code treats `kc` as **8** coeffs;
> our unit's blob has **12** — check before porting.

**IMU: yes. Cameras: no.** The HID factory blob the driver reads
(`MSG_GET_CAL_DATA_LENGTH 0x14` / `CAL_DATA_GET_NEXT_SEGMENT 0x15`, parsed into
`struct xreal_air_parsed_calibration`, `xreal_air_hmd.h:76–109`) contains **only**
inertial data: `accel_bias`, `gyro_bias`, `mag_bias`, `scale_{accel,gyro,mag}`,
cross-axis quats (`accel_q_gyro`, `gyro_q_mag`), and `imu_noises[4]`. **No camera
intrinsics, no fisheye/KB4 coefficients, no stereo baseline, no `T_imu_cam`.**

So a 6DoF build must obtain camera calibration some *other* way. Options, from the
evidence:

1. **Per-device Kalibr** — the honest route today. `xreal-vio-vr`'s
   `config/TEMPLATE_xreal_ultra.yaml` is **all placeholders**
   (`intrinsics: [fx, fy, cx, cy]`, `distortion_coeffs: [k1,k2,k3,k4]`, identity
   `T_imu_cam`) — i.e. the community project *also* expects you to calibrate your
   own unit. Kalibr on a 640×240-ish IR stereo rig with an AprilGrid is a fiddly
   half-day, per device.
2. **A vendor calibration blob** possibly readable through the XU or a different HID
   report — **unknown/undocumented**, would require reverse engineering the
   `{a29e7641...}` extension unit. High-effort, uncertain, ToS-gray (§7).
3. **Bundled generic intrinsics** — a single averaged calibration shipped for all
   units. Cheap but caps quality (per-unit lens/mount variance on cheap optics is
   real). Not available today; nobody has published one.

**Net:** camera calibration is an *unsolved acquisition problem*, not just a
plumbing gap. This is a first-order blocker distinct from the driver work.

---

## 4. The community stack, dissected (RQ3)

> **[2026-10-12]** The landscape changed: see **§4.1** (Berg/Panio's WIP Monado
> branch — the real prior art) and **§4.2** (upstream Monado status). The
> `xreal-vio-vr` dissection below remains accurate except that its
> `parts/xreal.py` is a **working descrambler** (§2.2); last commit 2025-06-29,
> otherwise still a stub.

`DeskUnreal/xreal-vio-vr` — GPL-3.0, ~15 stars / 1 fork, 16 commits, **0
releases**, self-described *"This stack is not plug-and-play. Yet."* and (ROADMAP)
*"Not stable — everything is subject to change."*

**Actual architecture (from the repo tree, correcting the 3DoF memo's read):**

- **IMU**: reuses `drivers/nrealAirLinuxDriver` (submodule) — the same TheJackiMonster
  lineage as Monado's `xreal_air`.
- **Camera**: UVC via **GStreamer** (the ROADMAP's completed "MVP: frame + IMU
  streaming pipeline").
- **VIO**: **patched Basalt** with a **fisheye624** camera model
  (`patches/Basalt patches--by_kukikap/basalt_fisheye624.patch` +
  `basalt_headers_fisheye624.patch`), *and* ORB-SLAM3 is named as the in-progress
  path in the ROADMAP — the project is mid-swap between the two, with `libs/DBoW3`
  + `libs/Pangolin` submodules for ORB-SLAM3.
- **Pose sink**: a **custom SteamVR/OpenVR driver** — `openvr/src/HMDDevice.cpp`,
  `xr/steamvr-bridge/`, a `ground_calibration_fsm`. **It injects into SteamVR, not
  Monado.** (Web summaries that say "pose injection into Monado" are wrong; the
  code says OpenVR.)
- **Input**: **Joy-Cons** over HID; overlay via `wlx-overlay-s` submodule.
- **Also present**: an `ar_runtime/` (outdoor AR / GPS / dead-reckoning
  experiments) — the project's scope is broader and less focused than "desktop
  6DoF."

**Build state**: **not buildable end-to-end.** `scripts/run.sh` is a stub
(`echo "🚀 Launching..."` + `# TODO: Start SLAM, input bridge, UI shell`); calib is
placeholder; no CI producing artifacts. `scripts/{build,build_libs,install_deps}.sh`
exist but the integration doesn't.

**Reported quality / responsiveness**: none published — no drift/jitter/latency
numbers, 0 issues, single maintainer, low cadence.

**Upstreamable?** No. It's a SteamVR-targeted, ORB-SLAM3/Basalt-in-flux, Joy-Con
prototype with hand-rolled calibration. Nothing here drops into HypXRland's
Monado-native model. Its *only* transferable assets are (a) the **fisheye624 Basalt
patch** as a starting reference, and (b) the **calibration YAML structure**. Both
are pointers, not code we'd adopt.

**Sibling search** (nreal/xreal SLAM Linux, OpenVINS/VINS-Fusion xreal, "Monado
SLAM glasses"): no other maintained project wires the Air 2 Ultra cameras to
Monado. The mature Monado SLAM drivers remain WMR / Rift S / Vive-Index /
RealSense. The Ultra is unserved on the Monado side. *[2026-10-12: **superseded**
— the 2026-07 search missed Berg/Panio's branch, §4.1. Still true for upstream.]*

### 4.1 Prior art: Berg/Panio WIP Monado branch (2026-10-12)

`gitlab.freedesktop.org/benzea/monado`, branch `main` — **Benjamin Berg + Luka
Panio**, **15 WIP commits, 2025-07-25 → 2026-04-27, no MR opened**. Exactly the
driver work §5 sized, for this device *(HIGH that it exists and its shape; quality
UNKNOWN — no public numbers)*:

- **`xreal_air_camera.c`** — camera source: finds the UVC node **by product
  string**, descrambles (§2.2).
- **`xreal_air_tracker.c`** (~730 lines) — `t_slam_create` with `cam_count = 2`,
  device↔host clock-offset estimation, Basalt→OpenXR pose-frame correction.
- **`xreal_air_util.cpp`** — **tinyceres** fit of the factory fisheye model to
  **KB4** on a 21×21 grid (the Rift S approach), so VIT's KB4 suffices.
- `SLAM_camera` parse from the HID blob (same finding as `XREAL-CAMERA-CALIB.md`).

**Self-declared rough edges (FIXMEs):** made-up IMU noise parameters; *"SLAM frames
every 2nd frame… is this correct?"*; *"this is completely wrong"* about the stereo
calibration; April-2026 fixes to focal length and camera z. Treats `kc` as **8**
coefficients (our blob has 12). Berg's upstream Monado **!2529** (prober fix) notes
*"Without this, I cannot detect an Xreal Ultra 2"* — i.e. this targets the **Ultra 2**
(our unit is an Air 2 Ultra; layout appears shared per the public decoders —
verify).

**Use:** port onto our pin as the R5/R6 starting point rather than writing from the
WMR template; fix the FIXMEs with our real factory values (12-coeff `kc`, real IMU
noises from the blob's `imu_noises`).

### 4.2 Upstream Monado status (2026-10-12)

- **Still 3DoF for every XREAL device.** **!2436** (Ultra IMU support, merged
  2025-07-04, in 25.1.0) explicitly *"does not add any support for the devices depth
  sensor"*. Open XREAL MRs: **!2435**, **!2580**, **!2964**, **!2833** (One Pro, IMU
  only). None add cameras.
- **SLAM**: **!2686** merged 2025-12 (Basalt linear velocity → less prediction lag) —
  worth having on our pin.
- `t_tracking.h` distortion enum has **no fisheye624**; VIT 2.0 has
  NONE/RT4/RT5/RT8/KB4 only (see §3 note).

**XREAL official (2026-10-12):** still **no Linux/PC SDK**. SDK **3.1.0**
(2025-11-25) is Unity/Android, 6DoF for the **One series + Eye**. The Ultra is
reportedly **discontinued** *(LOW — secondhand, MoguraVR)*. Nothing to wait for
from the vendor.

---

## 5. Monado's SLAM plumbing at `c2ddab59` (RQ4)

**Interface shape.** Modern Monado (our pin) uses a **VIT plugin boundary**
(`external/vit_includes/vit/vit_interface.h`, `VIT_HEADER_VERSION 2.0.1`): the VIO
system (Basalt) is an **external shared library** implementing `vit_tracker_*`,
loaded at runtime. Calibration is pushed **programmatically** —
`vit_tracker_add_camera_calibration` (`vit_camera_calibration_t`: `fx,fy,cx,cy`,
distortion model incl. **Kannala-Brandt/OpenCV-fisheye 4-param**, and a row-major
4×4 `T_imu_cam`) and `vit_tracker_add_imu_calibration` (`vit_imu_calibration_t`).
So a driver can hand calibration in code — **no on-disk JSON required** if the
driver supplies it (this is how WMR does it).

**Env/config surface**: `VIT_SYSTEM_LIBRARY_PATH` (path to `libbasalt.so`; default
`/usr/lib/libbasalt.so`); `SLAM_CONFIG` (Basalt TOML → calib JSON) for the
file-based route; per-driver toggles like `WMR_SLAM`, `SLAM_SUBMIT_FROM_START`.

**Which drivers feed SLAM today** (the pattern to copy): **WMR**, **Rift S**,
**Vive/Index**, **RealSense** (`grep XRT_FEATURE_SLAM` across `drivers/`). `twrap`
(`drivers/twrap/twrap_slam.c`) is a *generic* "tiny xrt_device exposing SLAM"
wrapper (with a `use_3dof` fallback and a Basalt pose-frame correction) — a
lighter-weight alternative to hand-rolling the device.

**The WMR template, concretely** (`drivers/wmr/wmr_hmd.c`):

- `wmr_source_create()` returns an `xrt_fs` streaming the camera USB
  (`wmr_source.h`); IMU is fed in via `wmr_source_push_imu_packet(xfs, t, accel,
  gyro)` (**raw** samples — "Basalt can calibrate them").
- `wmr_hmd_fill_slam_calibration()` fills `t_slam_camera_calibration` (per cam:
  intrinsics, distortion, `T_imu_cam`) and `t_slam_imu_calibration` **from device
  config in code** (`:1370–1465`).
- `wmr_hmd_slam_track()` → `t_slam_fill_default_config()` → set `cam_count` +
  `slam_calib` → `t_slam_create(xfctx, &config, &slam, &sinks)` →
  `t_slam_start()` (`:1488–1517`). The returned `sinks` are then wired to the
  frameserver output.
- Runtime pose: `xrt_tracked_slam_get_tracked_pose()` with a 3DoF fallback
  (`slam_over_3dof`, `:1139–1186`).

> **[2026-10-12] Missing-items list, revised** (original kept below for history):
>
> 1. **Camera source** — IN PROGRESS (monado `xreal-camera-record`): V4L2 open,
>    reinterpret as L8 640×482, chunk-unpermute (§2.2), telemetry parse, route by
>    camera-ID byte. Berg's `xreal_air_camera.c` is the reference.
> 2. **Frame pipeline** — *revised*: no YUYV→L8 convert, no SBS split (one eye per
>    frame). Plus EuRoC recording via the existing `t_euroc_recorder` for a dataset
>    library before any SLAM.
> 3. **IMU-into-source on the camera clock** — device-ns on both sides; residual =
>    one simultaneous **epoch check**.
> 4. **Calibration fill** — **DONE as data** (`SLAM_camera`); code = parse + KB4 fit
>    (Berg) or Basalt calib file, JPL→Hamilton.
> 5. **SLAM device wiring** — port Berg's `xreal_air_tracker.c`.
> 6. ~~Vendor XU handling~~ — **not needed**: stream is live on enumerate.

**What `drivers/xreal_air` is MISSING for 6DoF** (all of it — the driver is a pure
HID/3DoF device today; camera/uvc/v4l2/slam/sink grep = **empty**):

1. **A camera source** — an `xrt_fs` opening `video2`. Reuse `drivers/v4l2`
   (`v4l2_fs_create`, `v4l2_interface.h`) rather than writing UVC from scratch;
   select the single YUYV 640×241 @60 mode.
2. **A frame pipeline** — `u_sink_create_..._or_l8` (YUYV→grayscale) →
   `u_sink_stereo_sbs_split_create` (combined frame → left/right) → the two SLAM
   camera sinks; strip/parse the metadata row.
3. **IMU-into-source** — push the existing HID IMU samples (already at
   `xreal_air_hmd.c:312`) into the SLAM IMU sink with timestamps aligned to the
   camera clock (the §2 problem).
4. **Programmatic calibration fill** — the `t_slam_camera_calibration` +
   `t_slam_imu_calibration` structs. **Blocked on §3**: IMU part is readable from
   the factory blob; the **camera part has no source** and must come from a
   per-device Kalibr YAML loaded at startup (or a bundled generic set).
5. **SLAM device wiring** — either extend the HMD to a `slam_over_3dof` device
   (WMR style) or attach a `twrap` SLAM device; add `XRT_BUILD_DRIVER_XREAL_AIR`
   ⇒ SLAM enablement in the builder (`target_builder_xreal_air.c`, currently
   camera-blind).
6. **Vendor XU handling** — likely need to poke the `{a29e7641...}` extension unit
   to enable the camera stream and/or read sync info; undocumented, RE required.

Sizing: items 1–2 are mechanical (existing Monado utilities). Items 3–4–6 are the
research risk (sync + calibration + reverse engineering). Item 5 is
straightforward once 1–4 exist.

---

## 6. Basalt / VIO practicalities (RQ5)

> **[2026-10-12] Fork status corrected.** The canonical Monado Basalt is now
> **`gitlab.freedesktop.org/mateosss/basalt`** (active; tags 2026-01-28 and
> 2026-07-22). **`CIFASIS/basalt-xr` on GitHub is a stale mirror** (last commit
> 2024-07) — the bullet below is superseded. `basalt-headers` has had a native
> **`Fisheye624`** camera class since 2025-06-30, so the community fisheye624 patch
> is no longer needed for the file-based calib route.
>
> **Backend choice, with evidence:** the *Monado SLAM Dataset* paper (arXiv
> 2508.00088) found **Basalt and OKVIS2 most accurate, Basalt fastest**;
> OpenVINS/HybVIO/DPVO **failed** on easy sequences. But absolute numbers are
> sobering: **>50% of runs had ATE > 5 cm**, many RTE > 1 cm (the authors call it
> "too high"). Secondhand user reports of WMR+Basalt describe jank and slow drift.
> **Recommendation unchanged in substance: Basalt via Monado VIT / `t_slam`** — but
> expect cm-scale error and plan to measure it (§8).

- **Fork status** *(2026-07-14, superseded above)*: the maintained Monado-flavored Basalt is **`CIFASIS/basalt-xr`**
  (implements the VIT plugin, "improved for tracking XR devices with Monado";
  `ChristophHaag/basalt-monado` is an out-of-date mirror). Loaded via
  `VIT_SYSTEM_LIBRARY_PATH`. License BSD-3.
- **CPU cost**: Basalt is the fast option — "~**33 ms** needed for real-time on VR
  headsets" per frame-budget analyses. The Ultra's images are **small** (640×~240,
  half WMR's 640×480), so per-frame cost is *below* the WMR reference. On the
  **AMD HX 370** (this box) or **Lunar Lake** (the other box), 60 fps stereo VIO is
  comfortably within budget — **CPU is not the bottleneck**; sync and calibration
  are. *[2026-10-12: per-eye images are actually 480×640 at 30 stereo pairs/s
  (§2.2) — same pixel count as WMR; CPU conclusion unchanged.]*
- **Latency**: VIO pose is inherently a frame or two behind (16–33 ms) plus fusion
  lag; fine for seated desktop, poor for fast head motion (which the Ultra's
  numbers confirm degrade — §7).
- **Config surface**: a Basalt TOML pointing at a calib JSON (`SLAM_CONFIG`), or
  programmatic calibration via the VIT `add_*_calibration` calls. `fisheye624`
  (the community patch) vs the stock **KB4/Kannala-Brandt** the VIT header already
  exposes — KB4 is likely sufficient and avoids maintaining a Basalt patch.
- **Alternatives**: **ORB-SLAM3** (what `xreal-vio-vr` is moving to; heavier, loop
  closure) and **Kimera-VIO** are the integrated-into-Monado-history options;
  **OpenVINS / VINS-Fusion** have no maintained Monado VIT wiring. For our purposes
  Basalt-xr is the default; nothing else is better-supported on Monado.

---

## 7. XREAL's own software & the legal angle (RQ6)

- **No official Linux path.** NRSDK and its successor **XREAL SDK 3.0** are
  **Android/Unity** only (BeamPro, Android phones). `developer.xreal.com` /
  `docs.xreal.com` show no Linux SDK, no desktop 6DoF API, and NRSDK 2.1.0 *dropped*
  dev-kit support. The Air 2 Ultra's "developer" positioning produced **nothing
  Linux-usable**; its 6DoF/SLAM runs on-device via the closed Android stack.
- **Beam / Nebula**: XREAL's own compute/companion software — closed, not a Linux
  6DoF source.
- **Legal/ToS**: any Linux 6DoF path is **reverse-engineered**. The IMU/HID and 2D/3D
  display control are already community-RE'd (TheJackiMonster's open drivers, which
  Monado vendors) and low-risk in practice. The **camera XU** (`{a29e7641...}`) and
  any vendor camera-calibration blob are **undocumented** and would require fresh
  RE — a firmware/interface reverse-engineering effort with the usual EULA-gray
  status (no DMCA-circumvention of DRM involved; it's undocumented-USB territory,
  which the Linux community routinely does, but XREAL offers no blessing). A
  self-calibrated approach redistributes no XREAL firmware/calibration, which keeps
  §3-option-1 the cleanest legally as well as technically.

---

## 8. Effort, quality ceiling, and GO/NO-GO (RQ7)

### Quality ceiling — the deciding factor

*XR Reality Check* (arXiv 2508.08642), Air 2 Ultra:

- **APE 8.44 cm** average, **RPE 1.29 cm** average across trajectories.
- **~233% worse APE / ~251% worse RPE than Apple Vision Pro.**
- **Low-feature degradation**: APE **9.06 → 18.19 cm** (+101%) under fast inspection
  in a featureless scene; RPE 1.20 → 1.69 cm (+41%).

For **desktop-anchored monitors**, local stability (RPE ~1.3 cm) is *borderline*
usable — but two things bite: (a) ~cm-scale jitter on a virtual monitor 1–1.5 m
away is visible swim; (b) **a desk facing flat panels and a blank wall is the
low-feature worst case**, exactly where APE doubles. The workload we'd want 6DoF
for is close to the device's failure mode. This is why the verdict is NO-GO rather
than "hard but worth it."

**[2026-10-12] Reframed:** these vendor-stack numbers are still the only published
quality data for this hardware — no public Linux demo (Berg's included) reports
any. Rather than wait for one, **measure it ourselves**: record a EuRoC dataset
library at the actual desk (`xreal-camera-record`), run Basalt offline, and compare
anchored-monitor swim against 3DoF + online gyro bias (§2.3). The risk to watch is
that 6DoF **swaps slow yaw drift for cm-scale swim** (RPE 1.29 cm / APE 8.44 cm,
~doubling in low-feature scenes); the Monado SLAM Dataset paper's ">50% ATE > 5 cm"
(§6) points the same way.

### Effort — revised 2026-10-12

| WP | Item | Status | Size | Risk |
|----|------|--------|------|------|
| R1 | Telemetry row + pixel layout | **DONE** (§2.1 live; §2.2 public decoders). *Residual*: camera↔IMU **epoch check** (one simultaneous read) + settle the **camera-ID byte** (col 58 vs 0x3b) on our captures. | residual S | low |
| R2 | Camera calibration | **DONE** — factory `SLAM_camera` (`XREAL-CAMERA-CALIB.md`); Kalibr = optional backstop | — | — |
| R3 | Camera source + EuRoC recording (dataset library) | **IN PROGRESS** (monado `xreal-camera-record`) | S–M | low |
| R4 | Camera/IMU time alignment | = R1 epoch check; device-ns both sides | S | low–med |
| R5 | SLAM wiring — **port Berg's `xreal_air_tracker.c`** onto our pin, fix its FIXMEs (12-coeff `kc`, real IMU noises, stereo calib) | TODO | M | med |
| R6 | Basalt (`mateosss/basalt`) deployment + tuning; KB4 fit vs native Fisheye624; offline runs on R3 datasets | TODO | M | **high** (quality) |
| R7 | HypXRland 6DoF UX (roam/geofence/leash, `XREAL-3DOF.md` V2.3 hook) | TODO | M | low |
| — | 3DoF yaw-drift fix (online gyro bias, §2.3) — the baseline 6DoF must beat | IN PROGRESS (monado `xreal-gyro-bias`) | S | low |

**Prototype: ~1–2 weeks** (R1 residual + R3 + R5 + first R6 pass, porting Berg's
work). **Daily-usable: open-ended** — bounded by R6 quality tuning, not plumbing.

### Effort (2026-07-14, superseded — kept for history)

| WP | Item | Size | Risk |
|----|------|------|------|
| R1 | ~~Confirm camera packing + metadata-row timestamp~~ **DONE (§2.1)**: metadata row carries a device-ns frame timestamp → §2 sync tractable. *Residual*: confirm IMU↔camera epoch (one simultaneous read; IMU HID was root-only this pass) + reverse the intra-frame eye pixel-interleave. | ~~S~~ / residual S | ~~gates everything~~ **sync gate cleared** |
| R2 | **Per-device Kalibr calibration** of the IR stereo rig + IMU (AprilGrid, `T_imu_cam`, `td`). Repeatable but manual, per unit. | M | quality-capping |
| R3 | **Camera source in `xreal_air`** — `v4l2_fs` open, YUYV→L8, `u_sink_stereo_sbs_split`, metadata strip. | M | low (existing utils) |
| R4 | **IMU/camera timestamp alignment** — feed HID IMU into the SLAM sink on the camera clock; online `td`. | M | **high** (the §2 risk) |
| R5 | **SLAM wiring** — fill `t_slam_*_calibration`, `t_slam_create`/`start`, `slam_over_3dof` device, builder enablement. | M | low (WMR template) |
| R6 | **Basalt-xr deployment + tuning** — `VIT_SYSTEM_LIBRARY_PATH`, KB4 vs fisheye624, rolling-shutter model, drift/jitter tuning. | M | med |
| R7 | **HypXRland 6DoF UX** — light up roam/geofence/leash + 6DoF layout once a positional cap exists (the `XREAL-3DOF.md` V2.3 hook). | M | low |

Total: a **multi-week research project** with two first-order blockers (R2
calibration acquisition, R4 sync) that no upstream or community work de-risks for
us.

### GO / NO-GO threshold

**[2026-10-12] GO for a prototype.** Condition status:

| # | Condition (2026-07-14) | Status 2026-10-12 | Confidence |
|---|---|---|---|
| 1 | Calibration without per-unit Kalibr | **MET** — factory `SLAM_camera` in the HID blob (`XREAL-CAMERA-CALIB.md`) | CERTAIN |
| 2 | Camera-IMU sync tractable | **MET, one check left** — device-ns frame timestamp; pending one simultaneous camera+IMU epoch check | HIGH |
| — | *(implicit)* Pixel layout known | **MET** — publicly decoded (§2.2), three independent implementations | HIGH |
| 3 | Demonstrated desktop-usable quality | **NOT MET by anyone publicly** — **reframed: we measure it** on our own recorded datasets, against 3DoF + online gyro bias | — |

**Ship gate (new):** a 6DoF mode ships only if, at the user's desk, anchored-monitor
swim is visibly better than 3DoF + bias-fix — i.e. condition 3, evaluated by us.

*(2026-07-14 text, superseded:)* **NO-GO today.** Reconsider only when **all three** flip true:

1. **Calibration is solved without per-unit Kalibr** — XREAL exposes camera
   intrinsics/extrinsics, or the community ships a validated bundled/auto
   calibration for this hardware (removes R2 as a per-device tax).
2. **Camera-IMU sync is shown tractable** — either the metadata row carries a
   device-clock timestamp aligned to the IMU counter (R1 success), or someone
   demonstrates stable VIO on the Ultra's split-bus design (de-risks R4).
   **→ MOSTLY MET (R1, §2.1):** the metadata row *does* carry a device-clock ns
   timestamp; the only residual is a one-time confirmation that the IMU HID counter
   shares its epoch (strong hypothesis — single oscillator). This condition is no
   longer the hard gate; **conditions 1 and 3 now carry the NO-GO.**
3. **Demonstrated desktop-usable quality** — a public result showing sub-jitter,
   low-drift **anchored-monitor** 6DoF on the Air 2 Ultra specifically (not a
   walking-around demo), clearing the low-feature-scene concern.

Until then, ship 3DoF (`XREAL-3DOF.md`) with easy yaw recenter; that is the honest,
low-risk product and it already covers the seated-desk use case without SLAM's
failure modes.

---

## Open questions

*(Updated 2026-10-12.)*

0. **New, gating the prototype:**
   - **Camera↔IMU epoch** — one simultaneous read of a frame's @0 timestamp and an
     IMU HID timestamp (R1 residual).
   - **Camera-ID byte** — column 58 (`0x20`/`0x21`, sean9061) vs byte `0x3b`
     nonzero ⇒ right (Berg/DeskUnreal); arrival order is not reliable.
   - **@0x3e timestamp units** — µs (sean9061) vs our ns reading (§2.1).
   - **Berg's `kc` = 8 vs our 12** — does the Ultra 2 schema differ, or is it a
     truncation bug? Does Berg's descramble/tracker apply unchanged to the Air 2
     Ultra?
   - **Quality at the desk** — anchored-monitor swim vs 3DoF + online gyro bias.

1. ~~**Camera packing & metadata row**~~ **RESOLVED (R1, §2.1).** The 241st row
   carries a **device-clock nanosecond timestamp** (offset 0, 48-bit LE, epoch =
   power-on), plus a stereo-pair counter and exposure/gain. Eyes arrive as
   **consecutive frames** (60 fps = 30 pairs/s), ~±15 µs apart — **not** side-by-side
   in one frame. ~~Intra-frame pixel packing is a vendor Y/U/V-lane interleave (does not
   resolve as clean 320+320) — **still open**~~ **RESOLVED 2026-10-12 (§2.2)**:
   640×482 L8, 128 permuted 2400-byte chunks, rotating start.
2. ~~**Shutter type**~~ **RESOLVED** — `rolling_shutter_time = 0` ⇒ global
   (`XREAL-CAMERA-CALIB.md`).
3. ~~**Vendor XU `{a29e7641...}`**~~ **MOOT** — not needed for calibration
   (HID blob) or stream enable (live on enumerate, §2.2).
4. ~~**Is there any readable camera-calibration blob**~~ **RESOLVED** — yes,
   `SLAM_camera` in the HID JSON blob.
5. **Would XREAL developer support** *(low value now; vendor ships no PC SDK and the
   Ultra is reportedly discontinued)* provide Linux calibration/interface docs on
   request (`developer@xreal.com`)? Cheapest possible unblock of §3/§7 — untested.

## Sources

- arXiv 2508.08642 *XR Reality Check* — https://arxiv.org/html/2508.08642v1
- `DeskUnreal/xreal-vio-vr` — https://github.com/DeskUnreal/xreal-vio-vr/
- `CIFASIS/basalt-xr` — https://github.com/CIFASIS/basalt-xr *(stale mirror, last 2024-07; see `mateosss/basalt` below)*
- `ChristophHaag/basalt-monado` — https://github.com/ChristophHaag/basalt-monado
- Collabora, *Visual-inertial tracking for Monado* — https://www.collabora.com/news-and-blog/blog/2022/04/05/visual-inertial-tracking-support-for-monado-openxr/
- Mateo de Mayo, *SLAM for Monado* — https://mateosss.github.io/blog/monado-slam
- XREAL developer docs — https://developer.xreal.com/ , https://docs.xreal.com/XREALDevices/XREAL%20Glasses
- Vendored Monado @ `c2ddab59` (`drivers/{xreal_air,wmr,twrap,v4l2}`, `vit_interface.h`, `u_sink.h`) and live USB/V4L2 descriptors on the box (cited inline).

**Added 2026-10-12:**

- `XREAL-CAMERA-CALIB.md` (this dir) — factory `SLAM_camera` calibration extraction.
- Berg/Panio WIP Monado branch — https://gitlab.freedesktop.org/benzea/monado (branch `main`; `xreal_air_camera.c`, `xreal_air_tracker.c`, `xreal_air_util.cpp`)
- Monado MRs — https://gitlab.freedesktop.org/monado/monado/-/merge_requests/2529 (prober, Ultra 2), !2436 (Ultra IMU, merged), !2435, !2580, !2964, !2833 (open), !2686 (Basalt linear velocity, merged)
- `DeskUnreal/xreal-vio-vr` `parts/xreal.py` (`xrealultra2dec`) — https://github.com/DeskUnreal/xreal-vio-vr/blob/main/parts/xreal.py
- `sean9061/xreal-air2ultra-camera` `docs/PROTOCOL.md` — https://github.com/sean9061/xreal-air2ultra-camera
- `mazeasdamien/myXreal` (`CHUNK_MAP`) — https://github.com/mazeasdamien/myXreal
- Canonical Basalt for Monado — https://gitlab.freedesktop.org/mateosss/basalt (+ `basalt-headers` `Fisheye624`)
- *Monado SLAM Dataset* — arXiv 2508.00088, https://arxiv.org/abs/2508.00088
- `kevindurb/xreal-linux` `docs/findings.md` (1S yaw drift / bias walk) — https://github.com/kevindurb/xreal-linux
- Breezy Desktop / XRLinuxDriver — https://github.com/wheaney/XRLinuxDriver
- XREAL SDK 3.1.0 release notes — https://developer.xreal.com/ ; Ultra discontinuation report (MoguraVR, secondhand)
- Vendored Monado `src/xrt/auxiliary/math/m_imu_3dof.c` (`gyro_biasing()` / `manually_fire`) and `drivers/xreal_air` (static factory bias only).
