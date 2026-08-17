# 27 — Full-fidelity capture: record at the sources, compose offline

**Status: design only — implementation deliberately held** (user directive 2026-08-17). This memo
records the architecture, the evidence that the pose/clock chain closes, and the phasing, so that
implementation can start from decisions rather than research.

## 1. The problem, and the reframe

Today's recorder (the `hypxrrecorder` tier validated 2026-08-15/16) records what Meta's system
compositor hands to a MediaProjection virtual display: the *streamed, composited, re-encoded* view.
Every fidelity ceiling in that pipeline is baked into the recording at capture time:

- the WiVRn video stream's bitrate and any network-induced degradation during the take;
- the capture surface's geometry (500×800 portrait by default; sysprop reshaping is
  characterized separately on the `hypxrrecorder-capture-tuning` branch);
- the Quest-side re-encode of an already-encoded stream;
- mono, unless Meta's undocumented stereo capture path pans out;
- no audio at all today.

The reframe: **capture is data acquisition, composition is rendering, and they need not happen in
the same place or at the same time.** Record each source *at its origin*, each at its native best
fidelity, each stamped into a common timeline — then compose the final video offline, where network
conditions, radio airtime, and the XR2's encode budget are irrelevant. The existing on-device
recorder remains as the instant-preview tier; this architecture is the filming tier.

A second-order win falls out for free: because composition is deferred rendering against recorded
poses, the final framing is a *choice made in post* — wearer's-eye view, stabilized wearer view,
either eye, stereo SBS, or a novel camera path that never existed at capture time.

## 2. The four sources

| # | Source | Where | Native ceiling | Distance (est.) |
|---|---|---|---|---|
| S1 | Passthrough cameras | Quest 3, via Meta PCA (Camera2) | 1280×960/eye @30 (v83: 1280×1280); 40–60 ms latency; not the full displayed-passthrough FoV | medium — 2-3 rounds + attended characterization |
| S2 | XR overlay content | HypXRland host | full render resolution, **with alpha**, lossless-class encode; or per-monitor content + telemetry for full re-render | small — 1-2 rounds |
| S3 | Application + system audio | host (app audio), Quest (system sounds, optional) | pristine PCM pre-encode | small |
| S4 | Microphone | Quest (raw PCM, best) or host (from the already-streamed mic) | raw device PCM | small |

### S1 — passthrough cameras (Meta Passthrough Camera Access)

Horizon OS v74+ exposes the forward RGB cameras through the standard Android Camera2 API, gated by
the runtime permission `horizonos.permission.HEADSET_CAMERA` (fine for the sideloaded
`org.meumeu.wivrn.local` client). Both eye cameras stream at up to 1280×960@30 (1280×1280 from
v83 with a taller FoV — still less than the wearer's displayed passthrough FoV). Recording both
cameras means two more encode sessions on an XR2 that is simultaneously decoding the WiVRn stream;
the characterization run must include the judder check, exactly as the capture-tuning matrix does.

Honest framing on "fidelity": the raw cameras are *lower* resolution than the displayed passthrough
is reconstructed at, but they are the true source signal, undistorted by Meta's reprojection, and
paired with intrinsics they let the offline compositor do its own — typically better — projection.
1280×960@30 per eye is the ceiling Meta sets; nothing downstream can manufacture more.

### S2 — XR overlay content (host)

Two grades, not mutually exclusive:

- **Grade A — composited-with-alpha tap.** In a `blend_mode = alpha` session the composited eye
  buffer's alpha channel *is* the overlay matte. Tap the eye buffers pre-encode in the wivrn-xg
  compositor, write RGBA at full render resolution with a lossless-class codec (FFV1 / ProRes 4444 /
  x264rgb — the host has disk and CPU to burn), one file per eye, plus the per-frame stamped pose
  record (§4). This alone upgrades the overlay from "streamed then re-encoded" to "pixel-exact".
- **Grade B — content + telemetry, re-render in post.** Screencopy each XR monitor's headless
  output (ordinary Hyprland capture; no XR machinery involved) and record the pose/layout telemetry.
  The offline compositor then re-renders the quads itself at arbitrary resolution and framing. This
  is what makes novel-camera composition possible, and it is also the only grade that survives a
  wish like "re-run the take at 4K with different monitor transparency".

Grade A is the pragmatic v1 matte; Grade B's capture side is equally cheap and should be recorded
from day one even if the re-renderer comes later — telemetry not recorded is gone forever.

### S3/S4 — audio

The host possesses the pristine application audio (it encodes what it streams) — tap it pre-encode.
The mic's best copy is raw PCM on the device; its cheap copy is the compressed stream the host
already receives. System sounds (Meta UI) exist only on-device and are optional. All tracks carry
timeline stamps (§4); mixing is a compositor-stage decision, not a capture-stage one, which is
exactly the point — mic gain, ducking, and inclusion become post decisions.

## 3. Does the pose chain close? (yes — with three footnotes)

The question that decides whether offline composition is geometry or guesswork: can every recorded
pixel be placed in the *same* coordinate system?

**Camera side.** Camera2 provides per-frame sensor timestamps and per-camera calibration:
intrinsics (focal, principal point, distortion) and **extrinsics — the lens pose relative to the
device origin** (`LENS_POSE_TRANSLATION` / `LENS_POSE_ROTATION`), factory-calibrated. The WiVRn
client runs an OpenXR session against Meta's runtime and can locate the head pose at any given
time. Therefore:

```
camera_in_tracking_space(t) = head_pose(t) ∘ head_to_camera_extrinsic
```

**Space unification.** The client expresses the head/controller poses it streams to the server in
its tracking space; the server's LOCAL_FLOOR — the space every HypXRland quad pose and every
stamped render pose lives in — is *derived from exactly those packets*. There is no third frame to
solve for: the camera pose lands in the same coordinate family as the host's recorded overlay
poses by construction.

**Clock unification.** Camera2 sensor timestamps are in the device monotonic clock domain; the
client already maps device time into the stream timeline (streaming could not work otherwise), and
WiVRn continuously estimates the host↔device clock offset. Recording that offset series alongside
the captures gives the compositor one timeline for all four sources.

**The three footnotes:**

1. **Sample the head at mid-exposure.** Locate the head pose at the Camera2 sensor timestamp
   (plus half the exposure), not "whenever the frame callback ran" — at 30 fps with head motion the
   difference is visible in the composite.
2. **Record `stage_correction`.** The 2026-08-17 periphery-clip investigation proved the host
   applies a session-lifetime pose correction to stamped poses (`wivrn_session::stage_correction`),
   and that it can drift. Whatever its fix's final semantics, the *applied value per frame* must be
   in the telemetry, or the compositor re-derives geometry the renderer didn't use.
3. **Rolling shutter is the real fidelity limiter.** Fast head rotation skews the camera image
   across its readout. v1 accepts the artifact (it is present in every Quest capture today);
   the eventual mitigation is an IMU-rate pose track and per-scanline correction in the
   compositor — recordable from day one (poses are cheap), correctable later.

## 4. The telemetry record

One capture session produces a bundle (working name: `.hypxrtake/`):

- `cameras/{left,right}.mp4` + `cameras.json` (intrinsics, extrinsics, per-frame sensor timestamps,
  exposure) — S1;
- `overlay/{left,right}.mkv` (RGBA, lossless-class) — S2 Grade A;
- `monitors/<name>.mp4` + screencopy timing — S2 Grade B;
- `telemetry.jsonl` — per-frame: head pose, per-eye render poses + FoV as stamped,
  `stage_correction`, quad poses/sizes/visibility, blend mode, timeline stamps;
- `audio/{app.flac,mic.flac,system.flac}` with start stamps — S3/S4;
- `clock.jsonl` — the host↔device offset series;
- `manifest.json` — versions, GIT_DESC, protocol hash, capture settings, and which sources ran.

Nothing in the bundle is derived; everything is source. Any future compositor improvement replays
old takes better — the same property that made "commit eagerly, compose at rebase" work for code.

## 5. `hypxrcompose` — the offline compositor

Inputs: one `.hypxrtake` bundle. Output: a finished video (and optionally a stereo pair).

- **v1 — reprojection composite.** Background: the passthrough eye videos, projected through their
  recorded intrinsics/extrinsics into the chosen output camera. Foreground: Grade-A RGBA overlay
  reprojected from its stamped render poses into the same output camera. This is the same math
  family as `surfaceRelativeViewpoint` (doc 10) and the same GPU toolbox as the portal demo's
  shader path — a known quantity in this codebase, not research.
- **v2 — replayed quads.** Replace the Grade-A matte with true re-rendering from Grade-B monitor
  content + telemetry: arbitrary output resolution, per-monitor restyling in post, novel framings.
- **Audio** is a mix stage over stamped tracks (app/mic/system gains, optional ducking), trivially
  deterministic.

Output-framing menu enabled by the pose record: wearer's view (as-lived), *stabilized* wearer's
view (low-pass the head track — the single biggest watchability win for filmed VR), fixed tripod,
or orbit. None of these are capture-time decisions.

## 6. Phasing (held — no implementation yet)

- **Phase 1 — host taps (S2 Grade A + S3 + telemetry).** No APK change, no protocol change, no
  live-path risk (taps, not tees). Immediately yields pixel-exact overlay + pristine audio, and the
  bundle format. Composite v0 = overlay over black, or over the existing on-device capture as a
  placeholder background. ~1-2 agent-rounds.
- **Phase 2 — device captures (S1 + S4 raw mic + S3 system).** PCA Camera2 capture in the client
  APK + clock stamping; attended characterization (resolutions, judder budget, thermals). Rides a
  matched-pair deploy. ~2-3 rounds.
- **Phase 3 — `hypxrcompose` v1.** Reprojection composite + audio mix. ~2-4 rounds, then
  taste iteration.
- **Phase 4 — Grade B replay, stabilized/novel framings, rolling-shutter correction.** Open-ended.

Ordering rationale: Phase 1 is pure win with zero deploy coupling; Phase 2's characterization can
share an attended session with the capture-tuning matrix already staged on
`hypxrrecorder-capture-tuning`; Phase 3 only makes sense once real bundles exist.

## 7. Relationship to existing work

- The **on-device recorder** (validated, incl. control-channel pull and the LEDBAT scavenger)
  remains the instant tier: one file, zero post, good enough for bug reports and quick shares. The
  `.hypxrtake` bundle rides the same `recorder pull` machinery for transfer (it is just more files).
- The **capture-tuning branch** (staged) is complementary, not superseded: its sysprop
  characterization decides how good the *instant* tier can get, and its runtime-parameters plumbing
  is the natural carrier for Phase 2's capture toggles.
- The **client audio-tee debate** (2026-08-17) is resolved by this architecture: audio is captured
  at its origins (host app audio, device mic), not tee'd through the latency-critical client
  playback path.

## 8. Open questions for the implementation rounds

1. PCA under our client: does Camera2 capture coexist with an active immersive OpenXR session at
   full camera rate, and what is the real judder/thermal budget? (Attended; pairs with the
   capture-tuning matrix session.)
2. Lossless-class RGBA on the host: FFV1 vs ProRes 4444 vs x264rgb — disk rate at 2064×2208×2 eyes
   ×90 Hz is nontrivial even losslessly compressed; measure, and decide whether 45 Hz overlay
   capture is acceptable (overlay content rarely changes at 90 Hz).
3. Whether the overlay tap records pre- or post-foveation buffers (pre, ideally — post bakes in
   the stream's foveation).
4. Bundle transfer pacing: a take is GBs, not MBs — the scavenger's ceiling likely wants a
   "docked/charging" fast lane.
5. Exact Camera2 timestamp domain on Horizon OS (SENSOR_INFO_TIMESTAMP_SOURCE — REALTIME vs
   UNKNOWN) — measure, don't assume.
