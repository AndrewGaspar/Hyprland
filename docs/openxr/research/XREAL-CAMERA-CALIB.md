# Research: extracting the XREAL Air 2 Ultra stereo-camera calibration

Deep-dive (2026-07-14). **Read-only research + bounded live probing. No device
state changed, no firmware written, no long camera streams, no XR session
started.** This memo attacked the single remaining first-order blocker in the 6DoF
NO-GO (`XREAL-6DOF.md` §3 / GO-cond-1): obtaining the Ultra's **stereo-camera
calibration** — per-eye intrinsics, wide/fisheye distortion, stereo extrinsics, and
the camera↔IMU transform `T_imu_cam`.

> **RESULT — SOLVED. The full factory stereo-camera calibration was extracted
> live from the user's own Air 2 Ultra this pass (serial `G430X00412`).** It is a
> top-level `SLAM_camera` object inside the very JSON blob Monado's `xreal_air`
> driver already reads over HID — the driver was simply discarding every key except
> `IMU`. Both SLAM cameras: **`fisheye624`** model, 480×640, focal ≈ 240 px,
> principal ≈ (231/241, 328/317), a 12-coefficient distortion vector, and each
> camera's `imu_p_cam`/`imu_q_cam` (camera↔IMU pose). Stereo baseline derives to
> **137.3 mm**. `rolling_shutter_time = 0` (**global-shutter / global-equivalent**,
> resolving a long-standing open question). No Kalibr needed. **GO-condition-1 is
> dissolved.** Values in §1.4; raw dump at `scratch/xreal_ultra_G430X00412_calib.json`.

The device: USB `3318:0426`, one UVC stream (YUYV 640×241@60 = alternating stereo
eyes + a telemetry row), IMU over HID.

Evidence base (this pass):

- **Live extraction on the box**: replayed the driver's own `0x14`/`0x15`
  calibration-read sequence against `/dev/hidraw9` (USB iface 2) and captured the
  complete 55,845-byte JSON blob. Read-only HID report-descriptor enumeration
  (`HIDIOCGRDESC`) of all four XREAL hidraw nodes. Read-only UVC extension-unit
  probe (`UVCIOC_CTRL_QUERY`).
- **Vendored Monado** `subprojects/monado` @ `c2ddab59` — the `xreal_air` driver
  (HID calib read path + IMU-only JSON parser), and `wmr`/`rift_s` as precedent.
- **Web research** (three parallel sub-investigations): community protocol RE
  (`ar-drivers-rs`, XREAL's own `huyaokai-nreal/aisdk` test dumps, `0xcaff/xr-tools`,
  Void Computing), published/community calibration, and vendor-software mining.
  URLs cited in §8.

---

## TL;DR / verdict

1. **The factory camera calibration is IN the HID JSON blob the driver already
   reads — and we now have the user's actual values.** The blob is a JSON document
   read over HID (opcode `0x14` = length, repeated `0x15` = segments);
   `xreal_air_packet.c:xreal_air_parse_calibration_buffer()` parses it, keeps
   **only** `root["IMU"]["device_1"]`, and `cJSON_Delete`s the rest. The user's blob
   has top-level keys `['FSN','IMU','RGB_camera','SLAM_camera','display',
   'display_distortion','glasses_version','last_modified_time']`. **`SLAM_camera`
   carries the complete stereo VIO calibration** (§1.4). *(Confidence: CERTAIN —
   extracted live from `3318:0426`.)*

2. **This is the standard industry pattern (three codebases) — extraction just
   replays the driver's own reads.** Firmware JSON with inertial **and** camera
   calibration over HID is how Monado's **WMR** (`wmr_config.c:450` → `"Cameras"`)
   and **Rift S** (`rift_s_firmware.c` → `rift_s_parse_camera_calibration_block`)
   already work, and it is exactly the XREAL/Nreal schema
   (`badicsalex/ar-drivers-rs`, `0xcaff/xr-tools`, XREAL's own `huyaokai-nreal/aisdk`).
   The `xreal_air` parser being IMU-only is explained by it being a **3DoF** driver,
   not by cameras being absent — proven by the display-only **Air 1** blob having
   **no** camera keys while every camera-equipped sibling has them.

3. **The dump command set is read-only and driver-identical.** Only `0x14`/`0x15`
   were sent (the exact reads the shipping driver issues every boot) to
   `/dev/hidraw9`. No `0x16/0x17/0x18` (allocate/write/free), no display/brightness
   command, no feature-report writes. The display on DP-5 was undisturbed
   throughout.

4. **There is no other/larger report to find — the HID report descriptors prove
   it.** `HIDIOCGRDESC` on all four XREAL nodes: ifaces 0/1/2 (`hidraw7/8/9`) are
   **identical** 33-byte vendor descriptors — one 512-byte interrupt **IN** + one
   512-byte interrupt **OUT** report, **no report IDs, no feature reports at all**;
   iface 8 (`hidraw10`) is a Consumer-Control (media/brightness buttons) collection.
   So there is **no get-feature-report calibration surface**; the only data channel
   is the interrupt vendor protocol on iface 2, which we dumped in full. *(Confidence:
   CERTAIN — descriptors read live.)*

5. **The UVC vendor extension unit is unreachable read-only from userspace and is
   now moot.** Unit 4, GUID `{a29e7641-de04-47e3-8b2b-f4341aff003b}`, 24 controls,
   but `bmControls = 00 00 00` and it sits on a **dead-end media branch**, so every
   `UVCIOC_CTRL_QUERY` returns `ENOENT` (metadata node → `ENOTTY`). Reaching it
   needs libusb to claim the interface (root; detaches `uvcvideo`). Since HID gave us
   the full calibration, the XU is not needed (§2).

6. **No published Air-2-Ultra calibration exists to borrow — but we don't need
   one; the device carries per-unit factory values.** The only community stack
   (`xreal-vio-vr`) ships placeholders; NRSDK exposes only the RGB camera and gates
   SLAM behind an Enterprise SDK; no cloud/per-serial calibration server exists —
   the calibration lives **on the glasses**, which is exactly what we read (§3–§4).

**RECOMMENDATION (ranked, §6): DONE — the calibration is extracted. The remaining
work is (1) teach the Monado `xreal_air` driver to parse `SLAM_camera`/`RGB_camera`
out of the blob it already reads and fill `t_slam_camera_calibration`, and
(2) reverse the intra-frame pixel packing to feed Basalt.** Per-unit Kalibr
self-calibration (§5) drops from "the answer" to an **optional online-refinement
backstop**. The Windows/USBPcap runbook (Appendix A) is no longer needed for
calibration and is retained only as reference.

---

## 0. Device map (live, this box)

HID nodes for `3318:0426` (`udevadm info`), after the udev rule adding PID `0426`
`uaccess` landed this pass — all four now carry a `user:ajg:rw-` ACL:

| node | USB iface | driver role | HID report descriptor |
|---|---|---|---|
| `/dev/hidraw7` | 0 | **control** (display mode / brightness) | 512-B vendor IN+OUT, no report IDs |
| `/dev/hidraw8` | 1 | (aux HID) | identical 512-B vendor IN+OUT |
| `/dev/hidraw9` | 2 | **sensor / handle** — IMU + **calibration read** | identical 512-B vendor IN+OUT |
| `/dev/hidraw10` | 8 | consumer-control buttons | Consumer page, report ID `0x85` |

Interface roles from `target_builder_xreal_air.c`: Ultra `driver_handle_ifaces = 2`,
`driver_control_ifaces = 0`, `driver_max_sensor_buffer_sizes = 512`.
`send_payload_to_sensor()` writes to the handle/sensor device (iface 2 = `hidraw9`)
— where the `0x14/0x15` calibration read goes, and where the extraction ran.

UVC: one function, two V4L2 nodes on `media1` — `video2` (streaming YUYV 640×241@60)
and `video3` (UVC metadata). The VideoControl interface: Input Terminal (1) →
Processing Unit (2) → Output Terminal (3), plus Extension Unit (4, GUID
`a29e7641…`) on a side branch (§2).

---

## 1. Avenue 1 — the HID factory JSON blob (SOLVED, headline)

### 1.1 What the driver does, exactly

`xreal_air_hmd.c`: `request_sensor_control_get_cal_data_length()` sends
`XREAL_AIR_MSG_GET_CAL_DATA_LENGTH = 0x14` (`:363`); the reply's first 4 bytes are a
little-endian total length (`:404–411`). It then loops
`XREAL_AIR_MSG_CAL_DATA_GET_NEXT_SEGMENT = 0x15` (`:373`), copying up to
`buffer_size-8 = 504` bytes per reply into a `calloc`'d buffer (`:430–492`) until
`pos == len`, then calls `xreal_air_parse_calibration_buffer()` **and frees the
buffer** (`:471–489`).

`xreal_air_packet.c:254–273`:

```c
cJSON *root = cJSON_ParseWithLength(buffer, size);
cJSON *imu  = cJSON_GetObjectItem(root, "IMU");
if (imu) { cJSON *dev1 = cJSON_GetObjectItem(imu, "device_1");
           if (dev1) { parse_calibration_json(calibration, dev1); result = true; } }
cJSON_Delete(root);           // <-- SLAM_camera / RGB_camera / display all discarded
```

### 1.2 The extraction (live)

Script `scratch/dump_xreal_calib.py` (not committed) replays the driver's read
sequence exactly:

- Frames each command like `send_payload_to_sensor()`:
  `0xAA | crc32_le(body) | len_le16 | msgid | data`, `body = len_le16|msgid|data`,
  `packet_len = 3+len(data)`. The driver's `crc32_table` is the **standard zlib/PNG
  CRC32** (verified `table[1]=0x77073096`, `table[2]=0xEE0E612C`), so Python
  `zlib.crc32` is byte-identical.
- Sends `0x14`, reads length (**55,845 bytes**); loops `0x15`, appending
  `resp[8:8+min(504,rem)]` until full; `json.loads`.
- Writes go to `/dev/hidraw9` via `os.write` (matching monado's raw
  `os_hidraw_write` — `write(fd,…)`, no report-ID munging, since the descriptor
  declares report-ID-less 512-byte output reports).

**Only `0x14`/`0x15` were sent — both READ commands the driver issues every boot.**
No state-changing opcode was issued; DP-5 was undisturbed.

### 1.3 HID report-descriptor enumeration (read-only, all four nodes)

`HIDIOCGRDESC` results:

- **`hidraw7/8/9` (ifaces 0/1/2)** — identical 33-byte descriptor
  `05 41 09 00 a1 01  09 03 15 00 26 00ff 75 20 95 80 81 02  09 04 15 00 26 00ff 75 20 95 80 91 02  c0`:
  Usage Page `0x41` (vendor), one **Input** report (usage 3) and one **Output**
  report (usage 4), each **Report Size 32 × Report Count 128 = 512 bytes**, **no
  Report ID, no Feature report.** These are raw 512-byte interrupt pipes — the
  vendor protocol's only channel.
- **`hidraw10` (iface 8)** — Consumer Control page (`05 0c`), report ID `0x85`,
  media/volume/brightness button bits. Not calibration.

**Conclusion:** there is **no feature-report calibration surface anywhere** on the
device; the coordinator's "enumerate all get-feature-report IDs" resolves to "none
exist." All calibration flows through the interrupt `0x14/0x15` protocol on iface 2,
now fully dumped. (The identical descriptors on ifaces 0/1/2 mean the same vendor
protocol *could* be spoken on any of them; the driver uses iface 2 for
sensor/calibration and iface 0 for display/brightness control.)

### 1.4 The extracted calibration — user unit `G430X00412`

Top-level blob keys: `FSN, IMU, RGB_camera, SLAM_camera, display,
display_distortion, glasses_version(=6), last_modified_time(=2024-07-30)`.

**`SLAM_camera`** (`num_of_cameras = 2`; `device_1` = left, `device_2` = right).
Each camera `_comment`: *"camera_model includes 1) radial, 2) fisheye; 3)
fisheye624; imu_q_cam is JPL (qx,qy,qz,qw); imu_p_cam is (x,y,z)"*:

| field | device_1 (left) | device_2 (right) |
|---|---|---|
| `camera_model` | `fisheye624` | `fisheye624` |
| `resolution` [w,h] | `[480, 640]` | `[480, 640]` |
| `fc` (fx, fy) | `[240.4272, 240.6056]` | `[239.7080, 239.9345]` |
| `cc` (cx, cy) | `[231.1947, 327.8882]` | `[240.6507, 317.1070]` |
| `imu_p_cam` (m) | `[-0.0494910, 0.0044577, 0.0114669]` | `[0.0878093, 0.0047371, 0.0120577]` |
| `imu_q_cam` (JPL) | `[0.0721779, 0.0556512, 0.0022432, 0.9958355]` | `[0.0652011, -0.0397845, -0.0012148, 0.9970780]` |
| `rolling_shutter_time` | `0` | `0` |

`kc` (12-coeff `fisheye624` = KB4 radial `k1..k6` + tangential `p1,p2` + thin-prism
`s1..s4`):

- **device_1:** `[0.00575168, 0.0818962, -0.11146098, 0.06541732, -0.01985719,
  0.00248401, -0.00330444, 0.00393983, 0.00840755, 0.00066060, -0.00820340,
  -0.00076767]`
- **device_2:** `[0.00550788, 0.08576969, -0.11960663, 0.07314189, -0.02320008,
  0.00302972, -0.00229455, 0.00056204, 0.00280112, 0.00017154, -0.00095395,
  -0.00007145]`

Note: `resolution = [480, 640]` is **[width, height]** in this schema (portrait-oriented
sensor readout); `cc.y ≈ 320` sits at height/2, `cc.x ≈ 235` near width/2 — consistent.
This is the per-eye ~640-tall image the UVC stream delivers (§0), matching *XR Reality
Check*'s "640×480 grayscale" once de-interleaved.

**Stereo extrinsics.** This (`fisheye624`, `glasses_version 6`) schema variant has
no top-level `leftcam_p_rightcam` (an older `fisheye` sample did); derive the rig
from the two camera↔IMU poses. Camera positions in the IMU frame give
**baseline = device_2 − device_1 = [0.1373, 0.00028, 0.00059] m → ‖·‖ = 137.3 mm**
(the SLAM cameras sit at the outer front corners of the glasses, far wider than
IPD). The relative rotation composes the two JPL quaternions
(`q_rel = q_cam2 ⊗ q_cam1*`). For a Basalt/Kalibr `T_cn_cnm1`, both cameras'
`T_imu_cam` are given directly, so the stereo transform is exact, not estimated.

**Bonus calibration also in the blob** (beyond scope but valuable for the whole XR
stack): a rich **`IMU.device_1`** (`accel_bias, gyro_bias, mag_bias, scale_*,
skew_*, gyro_g_sensitivity, gyro_q_mag, imu_intrinsics, imu_noises`, plus a
**temperature-indexed gyro-bias table** `gyro_bias_temp_data` — more than the
driver's parser reads), and a **`display`** object (`k_left_display`,
`k_right_display`, `target_{p,q}_{left,right}_display`, `resolution`) = per-eye
display calibration, plus `display_distortion`. The whole 55.8 KB document is the
factory calibration file.

**How to load it into Monado 6DoF.** The natural path is to extend the `xreal_air`
driver to keep `root["SLAM_camera"]`/`["RGB_camera"]` when it parses the blob it
already reads, convert `fisheye624`→ the VIT/Basalt KB model (Basalt's `fisheye624`
is native; KB4 is the first four `kc`), and fill `t_slam_camera_calibration`
(`fc,cc,kc`, per-camera `T_imu_cam` from `imu_p_cam`/`imu_q_cam` with JPL→Hamilton
conversion) + `t_slam_imu_calibration` — exactly the WMR pattern. No on-disk YAML
needed.

*(Full raw blob: `scratch/xreal_ultra_G430X00412_calib.json`; camera-only extract:
`scratch/xreal_ultra_G430X00412_cameras.json`. Not committed — per-unit, and the
FSN is a device serial. Copy into the 6DoF driver work as desired.)*

### 1.5 Community precedent (web) — why this was near-certain even before the dump

- **`badicsalex/ar-drivers-rs`** parses this exact schema: `nreal_light.rs` reads
  `RGB_camera`/`SLAM_camera.device_1`/`device_2` (`fc,cc,kc,imu_p_cam,imu_q_cam`) and
  `SLAM_camera.leftcam_q_rightcam`; the **display-only `nreal_air.rs`** parses the
  same blob and finds only `display` + `IMU` — the control case proving cameras
  appear iff present.
- **XREAL's own `huyaokai-nreal/aisdk`** ships real factory samples
  `tests/test_data/glass_config_{400,624}.json` with identical top-level keys; the
  `624` sample is the `fisheye624` model matching the Ultra.
- **`0xcaff/xr-tools`** (`xreal_one_driver`) parses `SLAM_camera`/`imu_p_cam` from
  the XREAL One control-interface blob (`tests/data/with_camera.json`).
- **Void Computing** RE write-ups document the `0x14`(length)/`0x15`(segment) read
  yielding a JSON with camera + display + IMU calibration.
- **Opcode table** (Monado `xreal_air_hmd.h`): `0x14/0x15` = calib read;
  `0x16/0x17/0x18` = calib *write* path (driver `break`s, unused); `0x19` =
  START_IMU; `0x1A` = GET_STATIC_ID (a `u32` device id, **not** a cloud-calib key —
  the serial is already in-blob as `FSN`); **`0x1D` = defined but never sent/parsed,
  no evidence it returns anything.** There is no separate "get camera calibration"
  opcode — cameras are top-level keys in the one `0x14/0x15` blob.

---

## 2. Avenue 2 — the UVC extension unit (probed live: unreachable; now moot)

The VideoControl interface exposes a vendor Extension Unit: `bUnitID 4`, GUID
`{a29e7641-de04-47e3-8b2b-f4341aff003b}`, `bNumControls 24`, `bControlSize 3`,
**`bmControls = 00 00 00`** (all zero → `uvcvideo` registers no V4L2 controls).

Direct `UVCIOC_CTRL_QUERY` (read-only `GET_INFO`/`GET_LEN`; `scratch/xu_probe.py`)
for selectors 1–24 on unit 4 via `/dev/video2` → **every query `ENOENT`**; a unit
sweep 0–8 → all `ENOENT`; the metadata `video3` → `ENOTTY`. Root cause
(`media-ctl -d /dev/media1 -p`): the XU is a **dead-end branch** (`Processing 2 →
Extension 4`, Extension output pad unlinked; streaming chain is `Camera 1 →
Processing 2 → Output Terminal → video2`), so it is in no chain and
`uvc_ioctl_xu_ctrl_query` can't find unit 4. Reaching it needs **libusb claiming the
VideoControl interface** (root; detaches `uvcvideo`; kills the camera node).

**The XU is almost certainly the camera enable/mode surface** (you must poke it to
start the stream). It is now **moot for calibration** — HID delivered the full
calibration. If a future driver needs to enable the camera stream, enumerating the
24 XU selectors (via libusb, or observed in the Windows capture) is the task — but
that's a *streaming-enable* problem, not a calibration problem.

The 241st telemetry row (decoded in `XREAL-6DOF.md` §2.1) carries a device-clock
timestamp + exposure/gain, not calibration — unchanged.

---

## 3. Avenue 3 — vendor software mining (result: calib is on-device, which we read)

- **NRSDK** exposes only `NRFrame.GetDeviceIntrinsicMatrix(RGB_CAMERA)` /
  `GetDeviceDistortion(RGB_CAMERA)` — **RGB camera only**, read from the device at
  runtime (no bundled per-model asset). **Grayscale SLAM camera + IMU data are gated
  behind the Enterprise SDK** and deliberately withheld from the standard SDK. So an
  NRSDK app could log RGB intrinsics but **not** the stereo-SLAM intrinsics/extrinsics
  a VIO pipeline needs — which is precisely what we got straight off the device.
- **Nebula / desktop app / firmware tool**: no evidence of any per-serial
  calibration download. The only device-keyed server endpoints are OTA firmware
  (`ota.xreal.com/ultra-update`). Calibration is **on the glasses**.
- **Nreal Light era**: calibration was read from the **OV580 EEPROM/firmware** over
  the same `0x14/0x15` HID path — the direct precedent.
- **ToS**: reading *your own* device's calibration over USB for personal,
  non-redistributed use is the low-risk case and is what every open driver does.
  **Redistributing** the per-unit JSON (it contains unit-specific bias/serial) is
  the murkier and pointless case — hence we embed the numbers in this memo but keep
  the raw file uncommitted.

**Net:** the vendor-software route is inferior to the HID read we already did.

---

## 4. Avenue 4 — published / community calibration (result: none; not needed)

No published Air-2-Ultra (or sibling) camera calibration exists to borrow:

- `DeskUnreal/xreal-vio-vr` `config/TEMPLATE_xreal_ultra.yaml` is **all placeholders**
  (`intrinsics: [fx,fy,cx,cy]`, identity extrinsics); one abandoned fork, zero
  issues, no commit ever added real numbers. It gives the **schema Basalt expects**
  (`fisheye624`), not values.
- *XR Reality Check* (arXiv 2508.08642) consumes the device's **built-in pose API**
  and never calibrates the Ultra's cameras; its released code
  (`Duke-I3T-Lab/XR_Tracking_Evaluation`) calibrates only its RealSense reference rig.
- Monado GitLab: XREAL driver is IMU/3DoF only — no camera calib in-tree.
- Basalt/Kalibr/Zenodo/HuggingFace: example calibs for Vive/WMR/RealSense, none for
  xreal/nreal.
- Teardowns: dual grayscale ~2 MP SLAM cameras + IMU; no published sensor P/N,
  FOV, baseline, or calibration.

**But this avenue is moot:** the per-unit factory values are on the device, and we
read them. The XREAL SDK test samples (`glass_config_624.json`) also give a usable
**cross-check/prior** (same schema, `fc≈240`, `cc≈(240,322)`, 480×640) confirming
our extracted numbers are in-family.

---

## 5. Avenue 5 — self-calibration (demoted to optional backstop)

Previously "the honest answer"; now a **backstop**, since the factory calibration
is in hand. Still worth knowing:

- **Not required for a first 6DoF bring-up** — feed the factory `SLAM_camera` values
  straight into Basalt/VIT.
- **Online refinement**: Basalt can refine intrinsics + `td` at runtime from the
  factory prior — cheap insurance against unit aging / the fact that the factory
  `imu_p_cam`/`imu_q_cam` are the calibrated ground truth but the camera↔IMU **time
  offset** is not in the blob (the device-clock frame timestamp, `XREAL-6DOF.md`
  §2.1, plus Basalt's online `td` covers this).
- **Full Kalibr re-calibration** (AprilGrid, `pinhole-equidistant`/KB4, camera-IMU)
  remains available if the factory values ever prove inadequate — a ~half-day,
  one-time, per-unit. The factory `imu_noises`/`imu_intrinsics` seed the IMU model.
- **Prerequisite either way**: reverse the intra-frame Y/U/V pixel packing
  (`XREAL-6DOF.md` §2.1) to recover the two grayscale images — needed for *any*
  6DoF path, calibration or not. This is now the true next blocker, not calibration.

---

## 6. Recommendation — avenues ranked, and next actions

| # | Avenue | Outcome |
|---|--------|---------|
| **1** | **HID JSON dump** (§1) | **DONE — full factory stereo calibration extracted.** Highest payoff × tractability; realized this pass. |
| 2 | Self-calibration (§5) | Demoted to optional online-refinement backstop. |
| 3 | Windows/USBPcap RE (App. A) | No longer needed for calibration; kept for XU/stream-enable reference. |
| 4 | Vendor software (§3) | Inferior to the HID read; RGB-only + Enterprise-gated. |
| 5 | Published calib (§4) | None exist; not needed. |
| 6 | UVC XU via V4L2 (§2) | Unreachable read-only; moot for calibration (relevant later for stream-enable). |

**Calibration is no longer the blocker. The remaining 6DoF path (per `XREAL-6DOF.md`
§5 R3–R6):**

1. **Reverse the intra-frame pixel packing** (`XREAL-6DOF.md` §2.1 open item) to get
   left/right grayscale — the real next blocker.
2. **Enable the camera stream** — determine whether it free-runs or needs the XU
   (unit 4) poked (via libusb or an observed Windows capture).
3. **Teach `xreal_air` to parse `SLAM_camera`/`RGB_camera`** from the blob it already
   reads and fill `t_slam_camera_calibration` + `t_slam_imu_calibration` (WMR
   pattern), converting `fisheye624`/JPL to Basalt/VIT conventions.
4. **Feed Basalt-xr**, using the device-clock frame timestamp for temporal
   alignment; enable Basalt online `td`/intrinsic refinement.

**Single highest-value next action:** reverse the intra-frame stereo pixel packing
(§5 prerequisite) — with factory calibration solved, that is what stands between the
UVC stream and a Basalt 6DoF pose. The quality ceiling (`XREAL-6DOF.md` §8, ~cm
drift, worst in low-feature desk scenes) is unchanged and still governs whether 6DoF
is worth shipping over 3DoF.

---

## 7. Open questions (updated)

1. ~~Does the factory JSON contain cameras?~~ **RESOLVED — yes; extracted (§1.4).**
2. ~~Shutter type?~~ **RESOLVED — `rolling_shutter_time = 0` ⇒ global-shutter /
   global-equivalent readout.**
3. **Intra-frame Y/U/V pixel packing** — still open; now the gating item for 6DoF.
4. **Camera-stream enable** — does the stream free-run once opened, or must the XU
   (unit 4) be commanded? (libusb or Windows capture.)
5. **Camera↔IMU time offset** — not in the blob (only spatial `T_imu_cam`); rely on
   the device-clock frame timestamp + Basalt online `td`.

---

## Appendix A — Windows / USBPcap capture runbook (now reference-only)

No longer needed for calibration. Retained because it is still the way to observe
(a) the **XU selectors** the official app uses to **enable the camera stream** and
(b) any additional undocumented opcode.

- Windows box + Wireshark/USBPcap + the official XREAL app. Capture the matching
  USBPcap interface while the app initializes 6DoF/SLAM; save a `.pcapng`.
- **HID sensor iface**: interrupt OUT payloads beginning `AA <crc32> <len> 14` then
  a burst of `… 15` with large IN replies — reassemble (strip the 8-byte
  `hdr|crc|len|msgid` header) into the same JSON we already have (a cross-check).
  Watch for any opcode `>0x1A`.
- **UVC XU** (`a29e7641`, unit 4): class-specific `SET_CUR`/`GET_CUR` control
  transfers with `wIndex` high byte = 4, `wValue` = selector<<8 — tabulate which
  selector **enables the stream** (the useful bit now) and which returns
  float-looking blobs.
- Decode helpers: the HID framing + CRC are known (§1.2); the XU GUID + unit id are
  known (§2).

---

## 8. Sources

- **Live extraction** on `3318:0426`: `scratch/dump_xreal_calib.py` (CRC verified vs
  monado's `crc32_table` = standard zlib), producing
  `scratch/xreal_ultra_G430X00412_calib.json`; `HIDIOCGRDESC` on hidraw7/8/9/10;
  `UVCIOC_CTRL_QUERY` probe (`scratch/xu_probe.py`, `xu_scan.py`);
  `media-ctl -d /dev/media1 -p`; `lsusb -v -d 3318:0426`; `udevadm info`.
- Vendored Monado @ `c2ddab59`:
  `src/xrt/drivers/xreal_air/{xreal_air_hmd.c,.h,xreal_air_packet.c}`,
  `src/xrt/targets/common/target_builder_xreal_air.c`,
  `src/xrt/auxiliary/os/os_hid_hidraw.c`; precedent `src/xrt/drivers/wmr/wmr_config.c`,
  `src/xrt/drivers/rift_s/rift_s_firmware.c`.
- Community schema/RE: `badicsalex/ar-drivers-rs` (`src/nreal_light.rs` camera-bearing,
  `src/nreal_air.rs` display-only control case); XREAL `huyaokai-nreal/aisdk`
  `tests/test_data/glass_config_{400,624}.json`; `0xcaff/xr-tools` `xreal_one_driver`;
  Void Computing — https://voidcomputing.hu/blog/good-bad-ugly/ ,
  https://voidcomputing.hu/blog/worse-better-prettier/ .
- Vendor: NRSDK NRFrame intrinsics API (developer.xreal.com / nrealsdkdoc.readthedocs.io);
  OTA `ota.xreal.com/ultra-update`; XREAL ToS xreal.com/terms-of-service.
- Community stack / no-published-calib: `DeskUnreal/xreal-vio-vr`; *XR Reality Check*
  arXiv 2508.08642 + `Duke-I3T-Lab/XR_Tracking_Evaluation`; `CIFASIS/basalt-xr`;
  mateosss (Mateo de Mayo) Monado SLAM.
- Companion memos: `XREAL-3DOF.md`, `XREAL-6DOF.md` (§2.1 telemetry row, §3 IMU-only
  parser, §5 SLAM plumbing).
