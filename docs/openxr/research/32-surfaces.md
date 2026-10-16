# 32 — Surfaces: plane detection for dropping windows onto tables

Status: research + offline feasibility (2026-10-16). No code yet. Goal: let a window be released onto a
real table (or wall) and stay there. Needs horizontal-plane detection with good-enough height and
extent; dense depth is NOT required for placement.

## TL;DR

- **API.** The ratified standard is `XR_EXT_spatial_entity` + `XR_EXT_spatial_plane_tracking`
  (Khronos, June 2025; labels include `TABLE`). Shipped by Quest 3 (runtime 205.x), Android XR and
  Pico 4 Ultra. `XR_EXT_plane_detection` was never ratified and is marked deprecated by it, but
  **Monado already has its plumbing** (MR !2439: state tracker, IPC, `xrt_plane_detector.h` vtable on
  the head xdev), off by default (`XRT_FEATURE_OPENXR_PLANE_DETECTION`), with no driver behind it.
  There is no cross-vendor raycast/hit-test; HypXRland does its own ray-vs-polygon test.
- **XREAL (offline, living-room-02):** feasible. Coffee table and near floor come out as stable
  horizontal planes; table 0.44 m above floor; floor patches agree within 5 mm. Stereo SGBM at
  2–5 Hz costs ~0.1 core at half resolution.
- **Quest 3:** the headset already has labelled planes (Space Setup). WiVRn forwards nothing scene-
  related today; forwarding planes is a small client + packet + server-xdev change.
- **Plan:** build the Hyprland side first against a fake config-declared plane (C0–C2), then XREAL
  planes in Monado (A0–A1), then Quest planes through WiVRn (B1).

## 1. API landscape (2026-10)

| Extension | Status | Runtimes | Use here |
|---|---|---|---|
| `XR_EXT_spatial_entity` + `_spatial_plane_tracking` | ratified | Quest 3, Android XR, Pico 4 Ultra | long-term consumer API; implement in Monado's `st/oxr` over the same xrt plane vtable |
| `XR_EXT_plane_detection` | provisional, deprecated | Magic Leap 2; **Monado plumbing** | short-term consumer API (driver work only) |
| `XR_EXT_spatial_anchor` / `_persistence` | ratified | same three | later: persisting placements (research 22/28) |
| `XR_META_environment_raycast`, `XR_ANDROID_raycast` | vendor | Quest 3 / Android XR | only inside the WiVRn client, optional |
| `XR_META_environment_depth`, `XR_ANDROID_depth_texture` | vendor | Quest 3/3S / Android XR | not needed for placement; defer |
| `XR_FB_scene`, `XR_FB_scene_capture`, `XR_META_spatial_entity_discovery` | vendor | Quest | WiVRn client fallback for older Quest OS |

HypXRland only talks to Monado (XREAL flavour or WiVRn's), so vendor extensions matter only inside
the WiVRn client. The client side should sit behind a small plane-source interface with an
`EXT_plane_detection` backend now and an `EXT_spatial` backend later.

**Overlay sessions.** Plane detectors are per-session objects and neither spec excludes overlay
sessions; Monado routes `xrCreatePlaneDetectorEXT` to the head xdev over IPC. The driver keeps ONE
shared plane map and answers every session from it. Planes are located against a base space at
query time: re-locate after recenter, never cache world coordinates.

**Feeding planes in Monado.** Set `supported.planes` + `plane_capability_flags` on the head xdev and
implement `begin_plane_detection_ext` / `get_plane_detection_state_ext` / `get_plane_detections_ext`
/ `destroy_plane_detection_ext` from a mutex-protected plane map. The poll shape maps onto the
`EXT_spatial` discovery snapshot, so one backend can serve both APIs. Upstreamable (Collabora wrote
the interface and co-signed spatial entities).

## 2. XREAL offline feasibility (living-room-02)

Data: `~/xreal-datasets/plane-eval/` (scripts, plots, results). Trajectory from Basalt
(`kb4_meas_x5_grid35_cam8ms`); no ground truth, so heights are cross-checked between patches,
against sparse corners, and by back-projecting each plane into the images.

Setup: rectified 400×560 pinhole pair, f = 170 px, baseline 13.7 cm, cameras toe out 10.9°; +8 ms
camera time shift. Basalt world z is gravity-up (accelerometer rotated to world at 76 s:
(−0.07, 0.05, 9.81)).

**Use the factory fisheye624 model for dense stereo, not the KB4 fit.** KB4's 1.5 px RMS on cam0 is
harmless for VIO but costs ~1 px disparity: floor height drifts from −0.99 m at 1.25 m to −1.16 m at
3.25 m with KB4, and stays at −0.99…−1.04 m with fisheye624.

| | table | floor (three patches) |
|---|---|---|
| map height (world z) | −0.576 | −1.014 / −1.018 / −1.019 |
| above floor | 0.44 m (0.44–0.46 across runs) | — |
| tilt | 1.2° | 0.2–3.1° |
| extent | 0.46 m², 1.45 × 0.90 m | 0.39 + 0.24 + 0.21 m² (not merged) |
| per-frame height std / MAD | 27 / 9 mm | 22 / 12 mm |
| seen in | 419 / 671 frames | 565 / 671 frames |

Convergence: the table appears at 5 s, 0.32 m² by 25 s, settles at −0.576 / 0.46 m² after the user
leans toward it (70–82 s). Sparse corners (~88/frame, a stand-in for Basalt landmarks) give sharp
height peaks at −0.566 and −1.010 (1–2 cm wide): excellent heights, too few points for extents.

**What failed or misleads**

- Per-pixel SGBM normals scatter 20–40°: take the normal from VIO gravity and fit only height and
  polygon.
- A per-frame height-band histogram chained wall strips and edges across the room; a world-space
  column map fixed it.
- Glossy black table top: only 15–20 % of its pixels at the true height ±5 cm; 17–49 % land BELOW it
  (mirror reflections). Taking the upper height mode for a column helps specular tops.
- Floor beyond ~2 m: height σ 4.7 cm at 1–1.5 m, 6–9.5 cm at 1.5–2.5 m, 12–15 cm at 3–3.5 m.
- Real-but-unwanted horizontals: lap/laptop/forearms (−0.46 m), blanket/sofa seat (−0.55 m, 2.5 cm
  from the table — separable only by position), sofa arm and back. Dropping points nearer than
  ~0.6 m and lower than ~0.3 m below the head removes the hands/lap class.
- Depth Anything V2 Small (CPU ONNX, ~310 ms) fills the floor and table top, but per-frame
  scale+offset from stereo is unstable (34 % inliers, 38 % scale spread). Useful later as a
  segmenter/hole-filler, not as metric depth.

**Cost** (HX 370, 4 threads, Python/OpenCV): rectify 0.6 ms; SGBM full-res 22 ms, half-res ~4 ms
(half-res had better table coverage, 57 % vs 34 % valid, and the same heights); map update 0.5 ms;
extraction a few ms in C++. Budget: SGBM at 2–5 Hz ≈ 0.1 core, beside ~0.3 core for Basalt.

## 3. Monado design (XREAL)

- **Thread:** low priority, fed the same stereo pairs as SLAM, 2–5 Hz. Rectify with fisheye624 →
  half-res SGBM (pad the left edge so the disparity range doesn't kill ~96 px) → points into the
  SLAM world with the mid-exposure pose → drop near-field (<0.6 m) and σh > 8 cm points.
- **Map:** gravity-aligned 5 cm columns, each a 1 cm-bin height histogram weighted 1/σh².
- **Extraction (~1 Hz):** keep columns with a sharp peak (≥50 % of the ±20 cm mass within ±3.5 cm);
  grow regions across neighbours < 2 cm apart (8 cm total), allowing 2-cell gaps; merge same-height
  planes (±1.5 cm); normal = gravity; output a hull polygon.
- **Sparse heights:** add Basalt landmarks to the same map (VIT `POSE_FEATURES` already returns
  `{u, v, depth}` per keypoint, `basalt/src/vit/vit_tracker.cpp:703`); nearly free, very accurate in
  height. A0 can start with landmarks alone.
- **SLAM resets:** the map belongs to the current SLAM world. On a fresh tracker (doff/don, divergence
  restart) drop it, or keep heights and re-anchor xy if gravity survives; expire unseen columns.
  Bump plane ids / detection generation; report a plane only after N cells held for ≥ 5 s.
- **Labels:** geometric. Lowest large horizontal = FLOOR; horizontal-up 0.25–0.8 m below the head
  and within reach = TABLE (PLATFORM under `EXT_plane_detection`); vertical = WALL.
- Put it in `auxiliary/tracking/t_plane_*` (frames + poses in, plane map out) so other drivers and
  upstream can use it. Libraries: OpenCV (Apache-2.0), CAPE (MIT) if organized-depth extraction is
  wanted; avoid PEAC (AGPL / non-commercial).

## 4. Quest 3 through WiVRn

The Quest already computes labelled planes (Space Setup: walls, floor, ceiling, desks/tables, couches;
up to 15 rooms; an app can launch Space Setup with `xrRequestSceneCaptureFB`). Live depth
(`XR_META_environment_depth`, passthrough required) and an environment raycast also exist.

WiVRn forwards none of it (no scene/depth packets; manifest lacks `com.oculus.permission.USE_SCENE`).
Forwarding planes:

1. **Client:** `USE_SCENE` (+ `USE_ANCHOR_API`) and a runtime permission request; a scene-plane
   reader preferring `XR_EXT_spatial_plane_tracking`, falling back to `XR_FB_scene` +
   `XR_META_spatial_entity_discovery`; planes located in the tracking packets' reference space; a
   `scene_planes` packet (id, pose, extents, label, polygon — a few KB, reliable channel, on change)
   gated by `feature_control`. Precedent: `visibility_mask_changed` already carries variable-size
   geometry.
2. **Server:** the WiVRn HMD xdev answers the plane vtable from the cached packet; build with
   `XRT_FEATURE_OPENXR_PLANE_DETECTION ON` (WiVRn CMake + HypXR PKGBUILD).
3. **Open questions:** which planes `EXT_spatial_plane_tracking` returns on Quest (Space Setup only or
   live), whether scene anchors relocalize under our boundary suppression (research 28), whether
   `USE_SCENE` gates the raycast.

Raycast forwarding (a round trip per query) and depth forwarding (~12 MB/s raw at full rate) are
optional/deferred.

## 5. Hyprland client

- **C0:** plane-source interface + a synthetic provider (a config-declared table plane) so the
  interaction can be built and tested before any sensing exists.
- **C1:** `XR_EXT_plane_detection` consumer: poll on the MAIN thread, publish a POD snapshot to the
  frame thread (threading rules: no refcounting, no string config reads there); re-locate on recenter.
- **C2:** interaction. While grabbing, ray/hand vs plane polygons on the CPU; ghost preview when the
  window's bottom edge comes within ~5–10 cm of an upward plane; snap on release — "stand" (default:
  bottom edge on the surface, facing the user, ~10–15° back-tilt) or "lie flat" (modifier); flush on
  walls. Store the placement plane-relative (plane id + offset) in the research-28 model, falling
  back to the world pose if the plane disappears.
- **C3:** `XR_EXT_spatial_plane_tracking` backend; later persistence via `XR_EXT_spatial_anchor`.

## 6. Order and next steps

1. C0–C2 (Hyprland, fake plane) — testable in the container.
2. A0 (Basalt landmarks → height map → plane vtable) — then A1 (half-res SGBM) if extents are poor.
3. B1 (WiVRn plane forwarding).
4. Data: a standing user, a brighter room, a desk scene, and tape-measured table/eye heights for
   absolute accuracy.

## 7. What C0–C2 implemented (2026-10-16)

Code: `src/openxr/XRSurfaces.{hpp,cpp}` (pure plane model + snap math), `XRPlaneSource.{hpp,cpp}`
(the source interface, the synthetic provider, the merger), `XRPlaneDetection.{hpp,cpp}` (the
`XR_EXT_plane_detection` consumer), `OpenXRManagerSurfaces.cpp` (threads, publish, frame pass). Tests:
`tests/xr/surfaces.cpp`, `hyprtester/src/tests/xr/surfaces.cpp`. Options: `openxr:surfaces*` (doc 05).

- **Plane record.** POD, fixed capacity (64-vertex contour, 32 planes): id, pose, extents, contour,
  label, orientation class (always derived from the normal vs gravity), source, generation. Canonical
  convention: the plane is its pose's local X-Y plane, +Z is the normal. The `EXT_plane_detection`
  spec leaves the axis to prose, so the consumer assumes +Z and **verifies it against the orientation
  the runtime declared**: a pose whose +Y fits the declared class and whose +Z does not is rotated
  (`xrCanonicalPlane`, logged once). The Monado driver work should still pick +Z.
- **Threads.** Sources live on the main thread (timer, `openxr:surfaces_poll_ms`, 750 ms default) and
  each detection step is one short IPC (begin → state → get spread over polls; a completed detection is
  re-located on every poll, a fresh one begun every 8). The merged set is published under a mutex with
  a generation and the reference-space **epoch** it was located in; the frame thread bumps the epoch on
  every recenter/discontinuity and refuses a stale publish, re-expresses its own copy across a known
  recenter, and drops it on an unknown one. `xrCreatePlaneDetectorEXT` → `FEATURE_UNSUPPORTED` is a
  permanent "unsupported" for the session; repeated errors give up after 5.
- **Interaction.** Each frame a carried window (hand/controller MOVE grab, or gaze carry) is tested
  against upward planes: lowest visible corner within `snap_distance` above (or 25 cm through, or
  any depth while the content centre is still above it — §8) the surface and the bottom-edge centre
  inside the contour; horizontal beats a wall, the higher of two horizontal surfaces wins, and the
  aim/gaze ray breaks the remaining ties. Within it the window is **shown snapped** (the preview is the
  final pose), with 1.5× hysteresis. The release re-decides from the WP-G4 latched pose (the ring now
  records the carried pose, not the preview) against the armed plane: stand (bottom edge + bar on the
  surface, facing the head, `stand_tilt` back), flat (controller trigger held, or carried >45° face-up),
  or flush on a wall. The result is `placeLocalAt` (anchor:local, warp) and a plane-relative
  attachment: followed when the plane is refined (< 25 cm / 20°), held when it vanishes, dropped when
  it jumps or anything else moves the window.
- **Not done.** C3 (`XR_EXT_spatial_plane_tracking`) — a second `IXRPlaneSource`. No visual beyond the
  snapped preview (no ghost/shadow quad). Synthetic planes follow the LOCAL_FLOOR origin literally
  (re-published after each reference-space change), so they are a fixture, not room geometry.

## 8. First live run (2026-10-16, XREAL Air 2 Ultra + Monado planes)

Plane detection worked end to end (19–21 planes, the coffee table as PLATFORM `0x42` at
y 1.13, 1.68 × 1.23 m; head and planes are in the same LOCAL_FLOOR space — Monado locates both
through the xdev's tracking origin), yet moving XR-main "through the table" with the keyboard gaze
binds never landed it. The log shows why, in two parts:

- **The pushes were nudges, not a carry.** `gazepush` ×10 ran with no carry active (`gazegrab` is a
  toggle and came after them), i.e. the gazed-monitor nudge along the head→monitor ray, which never
  consulted the planes. Fix: a nudge is a one-step carry — `requestSurfaceSettle()`, and the frame
  pass settles the window onto a surface it was pushed into exactly as a release would.
- **The fixed 25 cm penetration limit.** A 1.32 m monitor's visible bottom edge (content half-height
  + margin + bar) is ~0.59 m under its centre, so it hung 0.27 m under the table top at rest. When the
  carry did start, it previewed at once (bottom edge 0.134 m under the top) and dropped the preview
  half a second later — most likely as the bottom edge passed −0.25 m (the log had no numbers to
  say), with the window still visibly standing in the table. Fix: deeper penetration counts while
  the content centre is above the surface, and when the window is over two qualifying horizontal
  surfaces the higher wins, even over the plane the gaze ray hits (the ray runs through the window
  centre and on past the table's edge to the floor behind it; the floor under a table is under the
  table).

So the next live failure is readable from `hyprland.log`, a **carry trace** (Log::DEBUG, `surfaces
trace:`) records, every 500 ms while a window is carried, on every preview edge and on every nudge,
the window (content pose, half-width, bottom extent, head) and the six planes nearest its bottom
edge: label, orientation, height, lowest-corner and centre height above it, over it or not, and the
verdict (`too far above`, `too deep through`, `not over it`, …). The frame thread writes POD records
into a 4-slot ring; the main thread formats them from the poll timer and before a gaze release,
which logs what it re-decides against.

**Clutter.** The live set held 21 planes: duplicates, "unknown" upward patches 1.6–2.3 m up, and
narrow sofa-arm platforms. Detected planes (declared ones are taken as declared) are now ignored as
snap candidates when smaller than 0.08 m² or narrower than 0.20 m on their short side, and upward
ones when less than 0.10 m below the eyes (`xrPlaneClutter`; constants, no options). The trace lists
them as `clutter: …` and counts rather than lists them.
