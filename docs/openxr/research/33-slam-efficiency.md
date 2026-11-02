# 33 — SLAM efficiency: structural options for the XREAL 6DoF pipeline

Status: research + offline prototypes (2026-11-02). Goal: make the XREAL Air 2 Ultra 6DoF tracker
(Monado `xreal_air` → `t_slam` → Basalt via VIT) cheap enough to run with lots of headroom on the
Lunar Lake laptop's LP E-cores at power-saver clocks, and lower package power. The incremental
(micro-optimisation) work runs in parallel against the slam-eff harness; this doc is about
**structural** changes: doing less work, doing it in a cheaper representation, moving it elsewhere,
and scheduling it better.

Code refs: Monado `hypxrland` 76c63baca, Basalt `xreal-vit` e3dec7b. Prototype branch: Basalt
`slam-eff-struct` (env-gated patches + `scripts/klt_bench.cpp`, §5). Measurements: the slam-eff
harness (Monado `slam-eff-bench`, `scripts/slam-bench.py`) on the Ryzen AI 9 HX 370's Zen 5c cores,
real-time replays of `living-room-02` (lr02: 3353 stereo pairs, 112 s) and the gap-heavy
`living-room-01` (lr01). Nothing was measured on the Lunar Lake laptop itself.

## TL;DR

- **Cost today:** 69.4 M cycles per stereo pair (frontend 60.1, backend 9.1) at 30 Hz. At the 2.2 GHz
  power-saver cap that is ~27 ms of a Lion Cove core for the frontend alone and 30–39 ms on Skymont:
  the LP E-cores cannot carry it today. The target for real E-core headroom is **≲ 25 M per pair**.
- **30 % of the frontend is wasted:** cam1 detection runs FAST up to four times on every cell that a
  mask already excludes. Skipping covered cells: **−25 % SLAM total, quality unchanged** (measured,
  all guardrails pass). Do this first; it is a few lines.
- **The structural refactor to start is an 8-bit, fixed-point, SIMD KLT tracker** (OpenCV-style) in
  place of Basalt's float patch tracker, on one 8-bit pyramid fed straight from the descramble pass.
  A drop-in prototype cuts the frontend 60 → 18 M (−70 %) and SLAM total −51 % (the backend gets
  more tracks, +74 %). It fails the harness's trajectory-similarity guardrails (ATE 1.6 cm vs the
  baseline run, closure 2.6 cm), but those are a lottery for any result-changing change (the tune
  lane's finding); on the independent two-view metrics it is **better on revisit error (1.0–1.2 vs
  1.57 cm)** and slightly worse on closure error (1.6 ± 0.4 vs 1.26 cm). Promising, not yet proven:
  it needs the point cap (to give the backend cost back) and a ground-truth check. Done right, SLAM
  total lands at ~25–30 M per pair.
- **Motion-gated frame rate** (skip pairs while the head is slow, never after camera gaps) saves −34 %
  on average on these recordings but nothing in the worst case; its accuracy cost is small but
  probably real (revisit error +0.25 cm on the pairs it keeps, closure error 1.26 → 2.4 cm). Worth having later as a power mode, not as the main lever.
- **Fewer levels / fewer points** are config knobs that each cost measurable accuracy; not structural
  wins.
- **GPU (Xe2) offload** would remove the frontend from the CPU at < 1 ms GPU time per pair, but it is a
  weeks-long rewrite inside Basalt with compositor-contention risk, and after the KLT work there is
  little left to save. **NPU and IPU: no.** No alternative VIO is materially cheaper than Basalt with
  these changes.
- **Power/scheduling:** once the work fits, run it on the LP E-cores at low clock (steady beats
  race-to-idle for this periodic load); the Monado IMU hand-off thread wakes ~850×/s and can be batched
  to ~125/s for free; plane detection should only run while something wants planes.

## 1. What the pipeline does per stereo pair

**Camera.** One UVC stream that claims YUYV 640×241 @ 60 but carries 640×482 8-bit grey
(`xreal_air_camera.c:161`). Frames alternate between the two cameras, so each camera runs at
**30 Hz**; a pair shares an exposure counter. Each image is 480×640 (portrait, the sensor is mounted
rotated) **8-bit**; the factory calibration says `rolling_shutter_time = 0` (global shutter,
research/XREAL-6DOF.md §2), exposure ≈ 8 ms, baseline 13.7 cm, cameras toed out ~11°. IMU: 1 kHz.
The UVC mode list has a single mode; whether the frame interval can be lowered (`VIDIOC_S_PARM`) is
untested.

**Frame path, four full-image passes before any tracking:**

1. V4L2 mmap buffer → descramble the 128 chunks and rotate 90° into a fresh `XRT_FORMAT_L8` frame
   (`xreal_air_capture.c:395-399`; the rotation is a strided column write,
   `xreal_air_camera_decode.c:121`).
2. `t_slam` hands the frame pointer to VIT without a copy (`t_tracker_slam.cpp:1312`), and Basalt
   **widens every pixel to `uint16_t` (`<< 8`)** into a new `ManagedImage` through `cv::Mat::at`
   (`vit_tracker.cpp:514`). Basalt's whole frontend is `uint16_t` because some cameras are 12-bit.
3. The pyramid copies level 0 into a 1.5w × h mip-map and builds 3 more levels with a 5×5 integer
   Gaussian whose two passes both write with a stride (transposed, `image_pyr.h:105-136`), which defeats vectorisation.
4. FAST detection narrows each 35×35 cell back to 8 bit (`keypoints.cpp:164`) before `cv::FAST`.

**Frontend** (`FrameToFrameOpticalFlow`, config `data/xreal/xreal_air_config.json`: grid 35,
1 point per cell, pattern 51, 3 levels = 4 pyramid levels, 5 iterations):

- **Track** every point of each camera from the previous frame, forward then backward
  (`frame_to_frame_optical_flow.h:349,359`), keep it if the round trip is within 0.2 px. The initial
  guess is the IMU-predicted relative pose with a 2 m depth guess. Each `trackPoint` runs all 4 levels
  (`:383`), each level builds a 52-sample patch with gradients (`patch.h` `setDataJacSe2`, 3×52
  Jacobian, 3×3 LDLT) and runs **5 Gauss–Newton iterations with no convergence test** (`:408`).
  Pattern 51 is Pattern 52 scaled by 0.5 (`patterns.h:146`): 52 samples on a 1-px grid, ~8×8 px,
  every sample a bilinear lookup of 4 `uint16` pixels in float, SE2 warp (rotation included).
- **Detect** new points in empty cam0 cells: per empty cell, FAST at threshold 40, then 20, 10, 5
  until a point is found (`keypoints.cpp:170`).
- **Match** the new cam0 points into cam1 (forward + backward LK).
- **Detect in cam1** outside the cam0 overlap: the overlap is added as one mask rectangle **per cell**
  (`frame_to_frame_optical_flow.h:612-635,662`), but detection still runs FAST (up to four times) on
  every masked cell and rejects each corner against all ~200 rectangles.
- Epipolar filter of the cam1 points.

**Backend:** square-root VIO (`vio_max_states` 3, `vio_max_kfs` 7, 7 LM iterations). It blocks on
the keypoint queue and pops IMU only per frame, so it wakes ~30×/s.

**Monado side:** the driver's tracker thread is signalled for **every IMU sample**
(`xreal_air_slam.c:1178`) and forwards them one by one; Basalt allocates a `shared_ptr` per sample and
pushes it into two TBB queues. Plane detection (opt-in) runs a half-resolution SGBM at 4 Hz
(`t_plane_detector.cpp:428`, `t_plane_stereo.cpp:138-141`).

## 2. Cost model (measured)

Harness baseline (`$SCRATCH/baseline`, Basalt e3dec7b with the package's flags at `-march=x86-64-v3`,
OpenCV capped to AVX2, 5 runs, Zen 5c CPUs 4–11 at 3.22 GHz), lr02, 6DoF. Per-thread user-space
cycle counters. Zen 5 ≈ Lion Cove per clock, so cycles carry over to the Lunar Lake P-cores; Skymont
is assumed at 0.7–0.9× per clock (unmeasured).

| per stereo pair | Mcycles | share | ms at 3.22 GHz | notes |
|---|---:|---:|---:|---|
| **frontend (optical flow thread)** | **60.1** | 87 % of SLAM | 18.6 (p50) | 192 M instructions, IPC 3.2 |
| · track cam0 + cam1, forward + backward | 28.0 | 47 % of FE | 8.68 | ~900 `trackPoint` calls × 4 levels |
| · detect in cam1's non-overlap | 18.1 | 30 % | 5.63 | almost all wasted (§4.1 S1) |
| · match new cam0 points into cam1 | 7.8 | 13 % | 2.43 | |
| · detect cam0 | 3.8 | 6 % | 1.18 | |
| · pyramids, 2 cameras | 2.1 | 3.5 % | 0.65 | |
| **backend (VIO thread)** | **9.1** | 13 % | 2.75 (p99 8.2) | |
| Monado IMU-forwarding thread | 0.12 | | | ~850 wakeups/s (§4.4) |
| **SLAM total** | **69.4** | | | |
| plane detection (opt-in, 4 Hz) | +4.1 | +5.9 % | | rectify 2.0 + SGBM 6.8 ms per processed pair |
| replay harness (PNG decode, pacing) | 19.6 | | | replay-only; live: UVC + descramble instead |

Inside the patch tracker (`perf record` of the frontend thread, own build): the tracker proper
(`trackPointAtLevel`, `trackPoint`, `interpGrad`, the `H⁻¹Jᵀ·r` product, `SE2::exp`) ≈ 49 % of the
thread, Eigen's generic GEMM kernel ≈ 8 % (3×52 fixed-size products routed to `gebp_kernel`),
`cv::FAST` + `detectKeypointsWithCells` + sorting ≈ 31 %, pyramid ≈ 2 %; the `<< 8` widening
≈ 1 % (on the pushing thread).

**Lunar Lake** (harness projection at the 2.2 GHz power-saver cap): frontend 27.3 ms on Lion Cove,
30–39 ms on Skymont, against a 33.3 ms frame. That matches the observed ~76 % of a 2.2 GHz P-core,
and it means **the LP E-cores at power-saver clocks do not keep up today**. For real headroom there
(≤ ~40 % of an E-core at 2.2 GHz after a 0.8× IPC discount) SLAM total has to come down to
**≲ 25 M cycles per pair**, about 3× less than now.

Not in the table: the live-only work (UVC dequeue, the descramble/rotate pass over 0.3 MB per camera
frame, the HID IMU reader) — small, and unchanged by everything below except a camera-side rate cut.

## 3. Ranked options

Savings are on SLAM total cycles per input stereo pair vs the 69.4 M baseline (Zen 5 cycles ≈ Lion
Cove cycles). "Measured" = this doc's prototypes (§5);
"est." = reasoned from the profile. Quality is against the harness guardrails.

| # | option | saving | quality / latency risk | effort | deps | status |
|---|---|---|---|---|---|---|
| S1 | skip mask-covered cells in cam1 detection | **−17.4 M (−25 %)** measured; FE −29 % | none: all guardrails PASS | hours | — | prototype `BASALT_STRUCT_DETECT=2`; incremental agent (a) owns it |
| C1 | 8-bit fixed-point SIMD KLT (OpenCV-style) for tracking + matching, with S1 | **−35.6 M (−51 %)** measured (FE −70 %, BE +74 %) | fails the trajectory-similarity guardrails (ATE 1.6 cm, closure 2.6 cm); independent metrics: revisit error **better** (1.0–1.2 vs 1.57 cm), closure error 1.6 vs 1.26 cm; needs a point cap + ground truth | 1–2 weeks to production | OpenCV `video` module (already a dependency family) | prototype `BASALT_STRUCT_KLT`; the structural refactor to start |
| C2 | 8-bit end to end, fused decode → level 0/1, one pyramid for LK + FAST | ~2–4 M est. (pyramid, widening, narrowing) + halves the working set (fits the LP E-core 4 MB L2) | none | days | Monado + Basalt VIT change | enabler for C1/O1; incremental agent (c) |
| C3 | early-exit Gauss–Newton, no-GEMM products, precomputed gradients | 10–20 M est. on the patch tracker | none expected | days | — | incremental agent (b); moot if C1 lands |
| S2 | motion-gated vision rate (skip pairs while gyro < 0.3 rad/s, never after gaps) | **−24 M (−34 %)** measured on average; **0 % in the worst case** | ATE 0.9 cm vs baseline run; revisit error +0.25 cm on the kept pairs, closure error 1.26 → 2.4 cm; without the gap guard lr01 on-SLAM 54 → 32 % | days (driver-side) | time-based keyframe policy in Basalt | prototype `BASALT_STRUCT_SKIP_BELOW` + `_GAP_MS`; later |
| S2' | fixed 15 Hz | −35 M (−50 %) measured (solo, old harness) | ATE 1.0 cm, closure 1.7 cm; same gap problem | trivial | — | not recommended alone |
| S4 | fewer points (grid 45) | −25 M (−36 %) measured (solo, old harness) | closure 2.5 cm, ATE 0.9 cm, revisit error 2.45 cm (worse); init jitter (the reason for grid 35) | config | — | keep as a knob; adaptive budget is the real version |
| S3 | fewer pyramid levels (2 / 1) | −8 / −19 M (−11.5 / −27 %) measured (solo, old harness) | closure ~2 cm, ATE 0.7 / 1.2 cm, revisit 1.9 / 2.0 cm (worse) | config | — | not worth it |
| S5 | mono frames with periodic stereo | ~−10 M est. (half the tracking on mono frames) | medium (scale via IMU only between stereo frames) | 1–2 weeks | frontend + keyframe changes | after C1, if still needed |
| W1 | batch the IMU hand-off (signal every few samples / on frame push), batched VIT IMU push | ~0 cycles; −~650–750 wakeups/s | none (IMU leads frames by tens of ms) | hours | — | **done** by the incremental lane (Monado `eff-imgpath`, every 4 samples + before each pair) |
| P1 | place on LP E-cores; `uclamp_max` / EAS instead of hard pinning | power, not cycles | deadline only once S1 + C1 land | hours + measurement on the laptop | kernel ≥ 6.16, intel_pstate mode | measure on Lunar Lake after S1 |
| PL | plane detection: run only while a consumer wants planes, or after the head moved | −4.1 M → ~0 when idle | none | hours | — | small; do anytime |
| O1 | frontend on the Xe2 GPU (Vulkan compute) | −FE (−18 M after C1) +~0.5–1 ms GPU wall per pair | frame-pacing contention with the compositor (manageable with a low-priority compute queue) | 3–6 weeks + CPU fallback | Vulkan in Basalt or a VIT frontend hook | defer |
| O2 | NPU (OpenVINO) | negative for LK | — | — | NPU stack not in Fedora main repos | no; maybe later for plane depth |
| O3 | IPU / ISP | n/a (USB camera) | — | — | — | no |
| A | another VIO (OpenVINS, VINS, Kimera, ORB-SLAM3, SVO) | none beyond C1 | re-integration risk | months | — | no |

## 4. The options in detail

### 4.1 Do less work per second

**S1. Stop the wasted cam1 detection** (`frame_to_frame_optical_flow.h:612-664`, `keypoints.cpp:140-198`).
The cam0-overlap mask is one rectangle per grid cell, exactly aligned with the detector's cells, and
the detector does not look at it until after FAST has run up to four times (thresholds 40/20/10/5)
in each such cell and every corner has been tested against all ~200 rectangles. Skipping a cell that
one mask rectangle covers removes 97 % of the `detect1` stage: **−29 % frontend cycles, −25 % SLAM
total, every quality guardrail PASS** (§5, `BASALT_STRUCT_DETECT=2`). It is a bug-sized change and
the single largest no-risk saving. (Mode 1 of the prototype also replaced the threshold-halving loop
by one FAST pass at the lowest threshold; that made `detect0` 44 % slower — more candidates to score
and sort — so keep the loop.) Further options: one whole-image `cv::FAST` per camera binned into
cells, or no cam1-only detection at all (it only adds points in the part of cam1 that cam0 does not
see).

**S2. Motion-gated frame rate.** Vision only corrects drift; the pose the compositor uses is predicted
from the IMU (driver blend + `t_slam` prediction). When the head is slow, frame-to-frame flow is
tiny and a 15 Hz (or 10 Hz) vision rate loses little. The prototype skips a stereo pair in the VIT
push while the gyro has stayed below a threshold since the last processed pair, never two in a row
(`BASALT_STRUCT_SKIP_BELOW`, `_MAX_SKIP`). On the two living-room recordings the per-frame peak gyro
rate is below 0.3 rad/s (17°/s) in 72 % / 79 % of frame windows, so at most every other pair is
skipped for ~35 % fewer processed pairs. Costs: the saving is in **average** power, not in the
worst case (fast head motion still runs at 30 Hz, so the deadline problem on a slow core does not
go away); fewer frames per keyframe window changes Basalt's keyframe timing (`vio_min_frames_after_kf`
counts frames); slower recovery of drift; tracks survive less well across a skipped frame in the
moment motion starts (the gate looks back, not ahead). A cleaner production version lives in the
Monado driver (it already has the IMU stillness detector, `xreal_air_still_push`) and also skips the
descramble and the frame allocation. Fixed 15 Hz (`BASALT_STRUCT_DECIMATE=2`) is the upper bound of
the saving and the lower bound of quality.

Measured (§5): gating at 0.3 rad/s skips 36 % of the pairs on lr02 (−34 % SLAM cycles per input
pair) and fixed 15 Hz −50 %, both move the trajectory by ~0.9–1.0 cm ATE from the baseline run, and on the
independent metrics the motion-gated variant is slightly worse: revisit error 1.98 vs 1.73 cm on the
59 frame pairs it keeps both frames of (null-ensemble sd 0.28), closure error vs two-view 2.36 vs
1.26 cm. On the gap-heavy lr01
recording, skipping without a guard halved the time on SLAM (54 → 32–35 %): re-acquisition after a
camera gap needs every frame. With the guard (`BASALT_STRUCT_GAP_MS`: no skipping for 1 s after a gap)
lr01 is unchanged (it then never skips). So this is a real but not free saving; it needs Basalt's
keyframe policy to be made time-based rather than frame-count-based (`vio_min_frames_after_kf`) and a
re-evaluation on recordings with ground truth before it ships.

If the camera can be put in a slower mode (UVC frame interval, untested), even the USB transfer,
the decode and their wakeups go away when 15 Hz is enough.

**S3. Fewer pyramid levels.** The patch is the same 52 samples at every level, so each level costs the
same and LK cost is proportional to the number of levels (4 today). The coarse levels exist for
capture range, but every guess is already IMU-predicted (rotation exactly, translation through the
2 m depth guess), so the residual motion is mostly a few pixels. Levels 3 → 2 should remove 25 % of the
LK work, 3 → 1 50 %; both are config knobs. Measured (§5, solo, old harness): levels 2 −11.5 % SLAM
total, levels 1 −27 %, but closure 0.6 → 2.0 / 2.1 cm and ATE 0.66 / 1.15 cm vs the baseline. The
coarse levels earn their keep (likely because the 2 m depth guess is off for near and far points, so
the translation part of the guess is not as good as the rotation part); not a structural win on its
own.

**S4. Fewer points.** LK cost is linear in tracked points. Grid 35 gives ~234 cells per camera; grid
45 gives ~130 (−45 %). The XREAL config chose 35 because it cut initialisation jitter 8.7 → 1.9 mm,
so this is a quality trade, not a free win. Measured (§5, solo, old harness): grid 45 −36 % SLAM
total, closure 2.5 cm, ATE 0.88 cm. An adaptive budget (fewer points while tracking is good
and the scene is textured, more on demand) is the structural version; Basalt only needs enough
well-spread landmarks for its keyframes.

**S5. Mono frames with periodic stereo.** Track cam1 only every Nth frame (re-matching from cam0) and
cam0 every frame. Saves ~half of the tracking stage on mono frames; scale stays observable through the
IMU and the stereo frames. Needs a real change in the frontend (cam1 tracks do not persist across mono
frames) and keyframe handling. Medium effort, medium risk; S2 gives a similar saving more simply.

**S6. Half resolution.** Halving the image (240×320) only helps the pixel-proportional stages (pyramid,
detection), which after S1 are small; LK cost per point does not change. It costs angular accuracy.
Not worth it; dropping the finest pyramid level for tracking is the same idea and S3 covers it.

### 4.2 Do the same work cheaper

**C1. 8-bit, fixed-point, SIMD KLT.** Basalt's tracker is scalar float code over gathered bilinear
samples of a `uint16_t` image, an SE2 warp with per-patch mean normalisation, and Eigen products that
fall into the generic GEMM kernel (`gebp_kernel`, 6 % of the frontend in the profile: 3×52 products
exceed Eigen's coefficient-product threshold). The established cheap alternative is OpenCV's
`calcOpticalFlowPyrLK` (used by OpenVINS, VINS-Mono/Fusion, Kimera-VIO): 8-bit images, 14-bit
fixed-point bilinear weights shared by the whole window (translation-only warp), integer Scharr
gradients, SIMD rows. It drops the in-plane rotation and the exposure normalisation.

Measured (§5): the microbenchmark does Basalt's tracking + matching in 3.3 ms instead of 11.1 ms per
pair (3.4×); inside Basalt (`BASALT_STRUCT_KLT=9` + `BASALT_STRUCT_DETECT=2`) the frontend drops
**60.1 → 17.8 M cycles (−70 %)**, but the backend rises 9.1 → 15.8 M (+74 %: the KLT keeps more
and longer tracks, 45 M vs 25 M backend instructions) and **quality fails the guardrails**: ATE 1.6
cm, closure 2.6 cm, RPE1s 0.48 cm / 0.14° on lr02, ATE vs the gold reference 2.36 cm (baseline 1.26).
But the tune lane showed those guardrails fail for 3 of 7 *null* perturbations of the baseline (a
different trajectory ~0.5–1 cm away is the normal outcome of any change to which tracks are kept), so
they cannot judge a tracker swap. On the independent two-view metrics (`$SCRATCH/tune/closure/`):
revisit error over 118 frame pairs **1.20 / 1.04 cm (9×9 / 11×11) vs the baseline's 1.57** (null
ensemble 1.79 ± 0.28) with rotation error 0.25–0.32° vs 0.38°, and closure error vs two-view
1.66 ± 0.44 / 1.59 ± 0.33 cm vs 1.26. So the 8-bit KLT is at least not clearly worse, plausibly
better. What to do next, in order: cap the tracked points per cell (or the backend's landmarks per
keyframe) so the backend gets back to today's observation count and cost; check whether the 2–3 %
shorter path is lower pose jitter or a small scale bias; get a ground-truth recording; use OpenCV's
`err` / minimum-eigenvalue output to reject weak tracks; for scenes with auto-exposure changes (the
recordings have a fixed 8 ms exposure) add a per-patch gain/bias, which Basalt gets from its mean
normalisation and plain LK lacks. A bigger window is not needed: 11×11 costs more (FE 21.4 M) for
about the same result. The production version keeps one
8-bit pyramid (built with `cv::buildOpticalFlowPyramid` or a fused decode pass, below), uses it for
both LK and FAST, and drops the `uint16_t` pyramid altogether.

**C2. 8-bit end to end, fused frame preparation.** Today the image is touched four times before LK
(§1). The XREAL camera is 8-bit, so the `<< 8` widening, the 1.5×-wide 16-bit mip-map (≈ 0.9 MB per
camera; old + new pyramids for two cameras ≈ 3.7 MB) and the `>> 8` narrowing before FAST are pure
overhead, and the 16-bit working set does not fit comfortably in the LP E-core cluster's 4 MB L2
(no L3 on that island). An 8-bit pyramid halves it. The descramble/rotate pass can write the 8-bit
level 0 directly into Basalt's buffer and produce level 1 in the same pass (VIT already passes the
frame pointer through without a copy). Small in cycles (pyramid + conversion are ~3–4 % of the
frontend) but it is the enabler for C1 and for any GPU path.

**C3. Cheaper Gauss–Newton.** No convergence test in `trackPointAtLevel` (5 iterations always);
`H⁻¹Jᵀ` products through GEMM; the patch's gradient image recomputed per sample (12 pixel reads per
`interpGrad`). These are incremental-agent work (early exit at |Δ| < 0.01 px, `lazyProduct`, a
precomputed Scharr gradient level), listed here because they shrink the same stage C1 replaces.
If C1 lands, they are moot.

**C4. Hardware pyramid / ISP.** None. The cameras reach the host over USB UVC, so the Lunar Lake IPU7
(which ingests MIPI CSI-2 sensors) cannot process them, and the glasses send raw scrambled frames.

### 4.3 Offload

**O1. GPU (Vulkan compute on the shared Xe2).** In GPU terms the frontend is tiny: ~900 LK calls per
pair × 4 levels × (52 gradient samples + ≤ 5 × 52 samples) ≈ 1.1 M bilinear samples, plus FAST and a
pyramid over 2 × 0.3 Mpx. The Arc 140V (8 Xe2 cores, ~4 TFLOPS FP32) would do the arithmetic in
tens of µs; the real cost is the dependency chain (pyramid → track → cell counts → detect → match →
detect cam1), ~6 dependent steps per pair, each a dispatch plus a fence if the bookkeeping stays on
the CPU: ~0.5–1 ms of wall time per pair and well under 1 ms of GPU occupancy. Frame-pacing risk is
real but manageable: submit on a separate compute queue (Xe2 has compute engines; check ANV exposes
one under `xe`), at low global priority (`VK_KHR_global_priority`), with short dispatches; the
compositor's queue is high priority and already keeps the GPU awake at the display rate, so the
added GPU power is small. Effort is the problem: Basalt has no GPU code and no way to accept external
keypoints through VIT, so this is a rewrite of the frontend in Vulkan inside Basalt (or a new VIT
frontend hook), ~3–6 weeks, plus a CPU fallback forever. After C1 + S1 the CPU frontend is ~18 M
cycles per pair (§5), i.e. ~25 % of a 2.2 GHz core instead of ~80 %, which bounds what a GPU path
could still save. Defer; revisit only if the CPU path stalls above ~20 M cycles per pair.

**O2. NPU (NPU 4 via OpenVINO).** LK does not map: data-dependent sub-pixel gathers at a few hundred
points, iterative, tiny. A learned alternative (dense flow such as RAFT-small, or SuperPoint +
LightGlue) is 100–1000× more operations than LK for no accuracy need here, with ~1 ms+ dispatch
latency per inference and a packaging burden (the NPU user-space driver and OpenVINO are not in
Fedora's main repos as far as I know). Not for tracking. A plausible later use is a learned
stereo/mono depth for plane detection, which runs at 4 Hz and is latency-tolerant (research/32 tried
Depth Anything V2 on the CPU at ~310 ms).

**O3. IPU / camera.** No (C4).

### 4.4 Scheduling and power on the hybrid CPU

- **Place it on the LP E-cores, at the lowest clock that fits.** For a 30 Hz periodic job the energy
  per cycle is lower at low V/f, and the package cannot reach deep C-states between frames anyway
  (1 kHz IMU, display-rate compositor), so race-to-idle on a P-core loses to steady low-clock work on
  an E-core — **once the work fits**. Today it does not fit with margin (the harness projects the
  frontend at 31–40 ms per 33 ms frame on Skymont at 2.2 GHz); after S1 + C1 it is ~6–9 ms per frame
  there. Running only on the LP island also lets the compute tile's P-core ring and L3 power-gate when
  nothing else needs them, which is Lunar Lake's main low-power trick.
- **How to place it.** `XREAL_AIR_SLAM_CPUS` pins the tracker thread before Basalt starts, and Basalt's
  threads inherit the mask (`xreal_air_slam.c` `apply_cpu_affinity`). Hard pinning is simple and
  predictable. The softer route is `uclamp_max` on the tracker threads: the kernel's capacity-fit check
  honours it (capacity-aware scheduling is on by default on hybrid parts without SMT, per the
  `intel_pstate` docs), and Linux 6.16 added Energy Aware Scheduling for `intel_pstate` on such parts
  (Lunar Lake), by user reports only with `intel_pstate` in passive mode + schedutil and
  `CONFIG_ENERGY_MODEL`. In the default active/HWP mode `uclamp` does not steer frequency (HWP + EPP
  does). Check `/proc/sys/kernel/sched_energy_aware` and the `intel_pstate` mode on the laptop before
  relying on any of it. Mainline Linux does not use Thread Director's class hints for placement as far
  as I know.
- **Wakeups.** Basalt's own threads are fine: the frontend and backend block on their frame queues and
  pop IMU per frame (~30 wakeups/s each). The Monado driver's tracker thread is signalled for every
  IMU sample (`xreal_air_slam.c:1178`): measured 850–860 wakeups/s for that thread alone, on top of the IMU
source's own ~915/s (the replay main thread here, the HID reader live), against 30/s for each of
Basalt's five threads and 16/s for the plane detector (§5). IMU
  leads the frames by the camera's transfer latency (tens of ms), so signalling every 8 samples (or
  when a frame is pushed) costs no latency and cuts that thread to ~125 wakeups/s. Basalt also
  allocates a `shared_ptr` and does two queue pushes per IMU sample; a batched VIT IMU push would
  remove most of that. See §5 for counts.
- **Spin/poll.** None found in the steady state: the replay's 10 ms sleep (`cli_cmd_xreal_slam_replay.c:412`)
  is replay-only; `queues_printer` is off unless `print-queue`.

### 4.5 Alternative trackers

None is materially cheaper for the same accuracy, so Basalt-with-surgery is the right path:

- **OpenVINS** (MSCKF, OpenCV KLT frontend): frontend cost ~ what C1 gives Basalt; the filter backend
  is cheaper than Basalt's at small state sizes but more fragile to tune. Swapping costs the whole
  integration (Collabora kept an OpenVINS fork for Monado's SLAM interface, but the XREAL calibration,
  restart and guard work here was done against Basalt and its VIT extensions).
- **VINS-Fusion / Kimera-VIO:** KLT frontends too, heavier backends (Ceres / GTSAM), not cheaper.
- **ORB-SLAM3:** ORB extraction on both images every frame is several times Basalt's frontend.
- **SVO 2 / SVO Pro:** the cheapest frontend in the literature (sparse direct, a few ms per frame on
  ARM), but its VIO backend and maintenance status make it a riskier swap than C1.
- **On-device:** the Air 2 Ultra sends raw scrambled camera frames over UVC; XREAL's own 6DoF runs in
  NRSDK on the host (Beam Pro / supported Snapdragon phones), so nothing on the glasses can be reused.

Monado's own trackers are Basalt (via VIT) for SLAM; there is no cheaper in-tree VIO.

### 4.6 Plane detection

Opt-in, 4 Hz half-resolution SGBM (research/32: 3.7 ms SGBM + 2.6 ms map on the HX 370 at 5 GHz,
i.e. ~30 M cycles per plane frame, ~4 M cycles per tracked frame averaged, ~6 % of today's frontend).
Already cheap; structural ways to make it near-free:

- **Gate it on need:** run only while a placement consumer is active (Hyprland asks for planes) or
  until the map's planes have been stable for N seconds, then drop to 0.5–1 Hz for maintenance.
- **Gate on motion:** dense stereo of a still head adds no new map columns; run only after the head
  moved ~10 cm or turned ~10° since the last integrated frame.
- **Lower resolution** for the floor (it is large and far), keep half-res only near the table.
- **Reuse tracking depth:** Basalt's landmarks were measured to add nothing over dense stereo
  (`XREAL_AIR_PLANES_LANDMARKS`), so this is not a replacement, only a seed.

## 5. Prototype measurements

Branch `slam-eff-struct` of Basalt (`~/code/wt/basalt-eff-struct`), all env-gated, defaults identical
to `xreal-vit`:

| commit | env | what |
|---|---|---|
| 269d8a2, ae92b7e | `BASALT_STRUCT_DETECT=2` (1 = also one FAST pass per cell) | skip grid cells one mask rectangle covers (S1) |
| 269d8a2, bcdc195 | `BASALT_STRUCT_DECIMATE=N`, `BASALT_STRUCT_SKIP_BELOW=rad/s`, `_MAX_SKIP`, `_GAP_MS` | drop pairs in the VIT push: fixed, or while the gyro is slow, never for 1 s after a camera gap (S2) |
| 86d2ae0 | `BASALT_STRUCT_KLT=<window>` | OpenCV 8-bit pyramidal KLT in place of the patch tracker, same guesses and forward-backward check (C1) |
| 269d8a2 | — | `scripts/klt_bench.cpp`: standalone OpenCV KLT doing the frontend's work on a EuRoC recording |
| 62d3574 | — | cherry-pick of the harness's package-flag build script (`build-v3`) |

**In-pipeline results.** Current harness (OpenCV capped to AVX2), package-flag build of the branch,
3 runs per cell, solo on CPUs 4–11, `compare $SCRATCH/baseline` (5 runs):

| variant | dataset | FE Mcyc/pair | BE | SLAM total / input pair | Δ | quality verdict |
|---|---|---:|---:|---:|---:|---|
| baseline | lr02 | 60.15 | 9.13 | 69.43 | — | — |
| S1 `DETECT=2` | lr02 | 42.79 | 9.13 | 52.07 | **−25.0 %** | PASS (ATE 0.06 cm, closure 0.53 cm) |
| S1 | lr01 | 42.77 | 9.97 | 52.99 | −22.9 % | PASS |
| C1 `KLT=9` + S1 | lr02 | 17.79 | 15.85 | 33.79 | **−51.3 %** | **FAIL**: ATE 1.61 cm, RPE1s 0.48 cm / 0.14°, closure 2.58 cm, path −12 cm; ATE vs gold 2.36 (baseline 1.26) |
| C1 + S1 | lr01 | 17.48 | 16.50 | 34.22 | −50.2 % | FAIL: RPE1s 0.49 cm / 0.14°; on-SLAM unchanged |
| S2 `SKIP_BELOW=0.3` + `GAP_MS=60` | lr02 | 60.93 ×0.644 | 10.17 ×0.644 | 45.90 | **−33.9 %** | FAIL: 36 % fewer frames by design, ATE 0.86 cm, RPE1s 0.40 cm / 0.12°, closure 1.67 cm; ATE vs gold 1.66 |
| S2 | lr01 | 58.48 | 9.89 | 68.62 | 0 % | PASS (the gap guard keeps every frame on this recording) |
| C1 `KLT=11` + S1 (paired slot run, 4 pairs; its own paired baseline: FE 62.5, BE 9.8, SLAM 72.4) | lr02 | 21.36 | 18.73 | 40.25 | −44.4 % | FAIL: ATE 1.57 cm, RPE1s 0.49 cm / 0.15°, closure 2.1 cm, path −19 cm; ATE vs gold 2.21 (baseline 1.27) |

(Per processed frame the frontend cost is unchanged by S2; the ×0.644 is the share of pairs processed.)

Earlier solo runs (old harness, Basalt's own Release flags, 2 runs, compared with their own baseline
build `sbase`: FE 61.1, SLAM 70.7 M; trajectory diff "ATE" vs the harness reference):

| variant | SLAM total / input pair | Δ | closure cm | ATE cm | note |
|---|---:|---:|---:|---:|---|
| `DETECT=1` | 54.9 | −22.4 % | 0.67 | 0.51 | detect0 +44 % |
| fixed 15 Hz | 35.5 | −49.8 % | 1.68 | 1.00 | lr01 without gap guard: on-SLAM 31.9 % (baseline 54.1) |
| skip < 0.3 rad/s, ≤ 1 in a row | 45.6 | −35.5 % | 1.70 | 0.86 | |
| skip < 0.3 rad/s, ≤ 2 in a row | 38.2 | −46.0 % | 0.86 | 0.73 | |
| levels 2 | 62.6 | −11.5 % | 2.00 | 0.66 | |
| levels 1 | 51.7 | −26.9 % | 2.14 | 1.15 | |
| grid 45 | 45.3 | −36.0 % | 2.49 | 0.88 | |
| `DETECT=1` + skip + levels 2 | 29.1 | −58.8 % | 2.03 | 0.95 | lr01: on-SLAM 34.6 % (no gap guard) |

**Reading the quality columns.** The guardrails compare against the baseline's own trajectory. The
tune lane (`$SCRATCH/tune/NOTES.md`) showed that 3 of 7 null perturbations of the baseline (e.g.
`max_recovered_dist2` 0.04 → 0.0375) already fail them: any change to which tracks survive lands on a
different trajectory ~0.5–1 cm away. So for result-changing variants the FAILs above say "different",
not "worse". The independent metrics from that lane (two-view SIFT/PnP estimates, no reference to
the baseline) on lr02:

| variant | revisit error, 118 pairs (RMS cm) | rotation (deg) | closure error vs two-view (cm) |
|---|---:|---:|---:|
| baseline (10 runs) | 1.57 | 0.38 | 1.26 ± 0.02 |
| baseline null ensemble (tune lane, 8 configs) | 1.79 ± 0.28 | | |
| S1 `DETECT=2` | 1.57 | 0.38 | 1.24 |
| C1 `KLT=9` + S1 | **1.20** ± 0.09 | 0.25 | 1.66 ± 0.44 |
| C1 `KLT=11` + S1 (paired) | **1.04** ± 0.11 | 0.32 | 1.59 ± 0.33 (its paired baseline 1.32) |
| S2 skip + gap guard | 1.98 (59 kept pairs; baseline 1.73 on the same) | 0.38 | 2.36 ± 0.14 |
| levels 2 / levels 1 (solo, old) | 1.92 / 2.03 | 0.44 / 0.63 | |
| grid 45 (solo, old) | 2.45 | 0.38 | |

So: S1 is identical; the KLT is better on revisit error and a little worse on closure (one number,
noisy); S2, fewer levels and fewer points are somewhat worse. Ground truth would settle C1.

**KLT microbenchmark** (`klt_bench`, OpenCV capped to AVX2 — its `calcOpticalFlowPyrLK` is not
runtime-dispatched and runs at the SSE baseline of Arch's `-march=x86-64` build — one Zen 5c core,
lr02, zero-motion guesses, ms per pair):

| | pyramids | track fwd+bwd, 2 cams | detect | stereo match | total | points/pair |
|---|---:|---:|---:|---:|---:|---:|
| Basalt baseline (timing.csv) | 0.65 | 8.68 | 6.81 | 2.43 | 18.6 | — |
| OpenCV KLT, 9×9, 4 levels | 0.20 | 2.74 | 1.70 | 0.56 | 5.2 | 661 |
| OpenCV KLT, 11×11 | 0.20 | 4.45 | 1.59 | 0.79 | 7.0 | 700 |
| OpenCV KLT, 9×9, every other pair (15 Hz) | 0.20 | 2.85 | 1.82 | 0.62 | 5.5 | 632 |

A 15×15 window costs 10.3 ms for the tracking alone: the window size, not the 8-bit arithmetic, is
what makes a KLT expensive. At 15 Hz the 9×9 KLT still holds ~95 % of its points.

**Wakeups** (`/proc/<tid>/status` voluntary switches over 60 s of a lr02 replay, CPUs 4–11):

| thread | wakeups/s | CPU |
|---|---:|---:|
| replay main thread (stands in for the live HID IMU reader + camera) | 913–915 | 18 % |
| Monado `XREAL Air SLAM` IMU forwarding | 852–862 | 0.3 % |
| Basalt frontend / backend / 3 helper threads | 30 each | 56 % / 9–10 % / 0 |
| plane detector (planes on) | 16 | 3.9 % |
| total | ~1930 | |

## 6. Recommended sequencing

1. **Now (incremental agents, already assigned):** S1 masked-cell skip (agent a; reuse
   `BASALT_STRUCT_DETECT=2`, Basalt `slam-eff-struct` ae92b7e), patch-tracker kernels (agent b), patch
   caching / allocations / 8-bit input (agent c; its round 1 already batched the IMU hand-off, W1).
   Add PL (plane detection only on demand) to the queue.
2. **Start the structural refactor: C1 + C2 as one design** — an `OpticalFlow` variant in Basalt that
   owns one 8-bit pyramid per camera (built once, used by LK and FAST), tracks with a fixed-point SIMD
   KLT (OpenCV's `calcOpticalFlowPyrLK` first, own kernel later), and keeps Basalt's forward-backward
   check, IMU-predicted guesses and grid bookkeeping. Next steps: cap points so the backend cost
   comes back down, explain the shorter path, reject weak tracks on the LK error, add gain/bias for
   auto-exposure. Judge it on the independent metrics (revisit error, closure error vs two-view, a
   ground-truth recording), not on the trajectory-similarity guardrails, which any tracker swap
   fails. Agent (b)'s kernel work and this refactor overlap: if C1 lands, (b)'s work on the float patch tracker becomes
   the fallback path only, so sequence (b) on the parts C1 keeps (detection, pyramid, bookkeeping).
3. **Then measure on the Lunar Lake laptop:** pin to the LP E-cores (`XREAL_AIR_SLAM_CPUS`), power-saver,
   battery; package power via RAPL. Decide between hard pinning and `uclamp_max` (+ EAS if the kernel
   mode allows) there.
4. **Later, as a power mode:** S2 motion gating with the gap guard, moved into the Monado driver (skip
   before descramble), after Basalt's keyframe spacing is made time-based; evaluate on recordings with
   ground truth (e.g. a returned-to-start loop with a fiducial) before turning it on by default.
5. **Only if 1–4 leave SLAM total above ~25 M per pair:** S5 mono frames, then O1 GPU.

## 7. Open questions

- **Skymont per-clock throughput on this code** is the biggest unknown in every Lunar Lake number
  here. One replay pinned to an LP E-core on the laptop settles it.
- **Ground truth.** The quality calls for S2, S3, S4 and C1 rest on the two-view revisit and closure
  metrics, which are independent of the baseline but approximate. A recording with a real reference
  (fiducials, or a motion-capture session) would turn "differs by 1 cm" into "is worse by x cm".
- **Can the camera run slower?** The UVC device lists one mode; whether `VIDIOC_S_PARM` accepts a
  longer frame interval (or a HID command does) is untested. A camera-side 15 Hz would also remove the
  USB transfer, the descramble and their wakeups.
- **Why the KLT prototype keeps more tracks** (backend +74 %), whether that is also why its revisit
  error is lower, and what a point cap does to both.
- **Package power, not just cycles:** none of the savings here were measured in watts (no RAPL as a user
  on this box); the laptop measurement in step 3 should include the before/after of S1 and C1.
