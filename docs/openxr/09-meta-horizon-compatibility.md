# Meta Horizon PC VR compatibility status

This page records the known limitations of running Windows Meta Horizon PC VR applications on the
current HypXRland stack. It is a status report, not a claim of general compatibility. The results
below were obtained on one split-GPU machine with a Quest 3, WiVRn 26.6.2, an AMD Radeon 890M
runtime/compositor GPU, an NVIDIA RTX 5070 Laptop application GPU, GE-Proton10-32, DXVK 2.7.1,
WineOpenXR, and a custom Heroic Meta runner.

HypXRland is not the compatibility layer for these games. It is a second OpenXR client running as
an `XR_EXTX_overlay` session beside the game. The Windows application path is:

```text
Heroic Meta runner / OAF
  -> Windows game + OVRPlugin or legacy LibOVR
  -> D3D11 / DXVK
  -> WineOpenXR (for OpenXR-capable titles)
  -> Monado / WiVRn
  -> Quest

HypXRland -------------------------------------------------> Monado / WiVRn
                         (parallel overlay client)
```

A failure in the first path can leave HypXRland's monitors healthy. Conversely, a connected Quest
or a focused HypXRland overlay does not prove that the game reached OpenXR, submitted a frame, or
produced visible pixels.

## Current title status

| Title / API path | Status | Established result |
|---|---|---|
| Walkabout Mini Golf 6.6, build 780, OVRPlugin 1.92, patched separate-eye rendering | **Partial** | The guarded game process created shared color/depth textures, used native DXVK on NVIDIA, connected as the primary Unity client, and survived the bounded 20-second health window without a new NVIDIA Xid or an attributed fence failure. At least one attended launch was nevertheless reported as black in the headset with severe hitches, so process/swapchain health is not visual certification. |
| Walkabout Mini Golf 6.6, unmodified OculusXRPlugin, Single Pass Instanced | **Broken** | Unity and the Oculus eye layer both selected `Texture Array`, then reported `Failed to get layer textures`, `CreateEyeTextureStages failed`, and `Error on graphics thread: 1` before crashing. The original plugin remained byte-identical throughout. There was no new Xid and the exact Unity client had no persistent fence wait. |
| Spider-Man: Far From Home Virtual Reality, UE4.21 / OVRPlugin 1.28 | **Unsupported** | The application calls legacy `ovr_Create_Impl`, receives `ovrError_NoHmd` (`-1007`), and presents a fatal-error dialog. It never loads WineOpenXR or connects to WiVRn. |
| Psychonauts in the Rhombus of Ruin, UE4.12 OculusRift plugin | **Unsupported** | The application calls legacy LibOVR, receives `ovrError_NoHmd`, and renders a desktop message that no headset is attached. It never becomes a WiVRn client. |
| Other Meta Horizon PC titles | **Untested** | Store presence or successful installation says nothing about the title's VR API, graphics path, or compatibility. |

The Walkabout result is narrow. Build 6.7 was already available during testing but was not in the
test matrix. Game updates can replace plugins, change offsets, add formats, or select another
graphics API.

## Remaining deficiencies

### 1. There is no general LibOVR bridge

WineOpenXR can serve applications that actually use OpenXR. It does not implement the Oculus PC
LibOVR client API. Older titles can therefore talk successfully to the emulated Meta service and
still receive `NoHmd`: their VR calls never enter WineOpenXR, Monado, or WiVRn.

This is a separate compatibility lane from the Walkabout work. A future solution needs a real
LibOVR-to-OpenXR bridge, or a tested LibOVR-to-OpenVR-to-xrizer chain such as Revive plus xrizer.
Changing D32S8, cross-GPU transport, or HypXRland overlay code cannot fix a title that never reaches
OpenXR.

### 2. Walkabout still needs build-specific binary compatibility patches

The tested build has two one-byte workarounds:

| File | Tested build-780 change | Purpose / observed effect |
|---|---|---|
| `OVRPlugin.dll` | offset 790968: `e7` -> `cf` | Forces the runtime capability bit OVRPlugin currently requires before it will initialize through WineOpenXR. This should be replaced by a correct launcher/runtime capability path rather than retained as a permanent game patch. |
| `OculusXRPlugin.dll` | offset 41068: `85 c9` -> `31 c9` | Enters the plugin's non-Oculus compatibility branch and, in the tested build, makes Unity use separate per-eye textures instead of a texture array. It avoids the current Single Pass Instanced crash but increases rendering work. |

The valid A/B test restored the original `OculusXRPlugin.dll` and prevented Heroic from silently
reapplying its patch. Unity then honored the requested Single Pass Instanced mode and selected
`Texture Array`, proving that the patch had been forcing the `Separate` fallback. The unmodified
path failed before Unity could create its eye textures. The exact failing operation within the
OVRPlugin/OculusXRPlugin-to-WineOpenXR texture-array boundary has not yet been isolated.

Do not remove the separate-eye workaround merely because D32S8 and staged color now work. First add
a synthetic array-swapchain test, fix the texture acquisition failure, and require a guarded game
run with visible output. Patch offsets and hashes are version-specific and must never be applied to
an unknown build.

### 3. Direct split-GPU color plus depth can fault the NVIDIA channel

The native WiVRn color swapchain is an imported, explicit-linear cross-GPU image. Walkabout also
requires a client-local, optimal-tiling D32S8 depth image on NVIDIA. Each resource works in
isolation, and an ordinary NVIDIA-optimal color image works beside the same depth image. Binding
the imported linear color RTV and optimal depth DSV in one real D3D11 draw produced NVIDIA Xid 69
on the tested driver.

The current workaround is WineOpenXR's experimental D3D11 staged-color path, enabled with
`WINEOPENXR_STAGED_CROSS_GPU_COLOR=1`. The application renders to an ordinary NVIDIA-optimal DXVK
texture. On swapchain release, WineOpenXR copies every mip and array layer into the runtime's
linear transport image on the locked DXVK queue, restores the boundary layouts, and then releases
the transport image to Monado.

This workaround has material limits:

- it is D3D11-only and disabled by default;
- it rejects multisampled, cube/face, protected, and format-mismatched swapchains;
- it adds one full-resolution color copy per released eye image;
- it depends on a private WineOpenXR/DXVK interop interface and native `d3d11`/`dxgi` overrides;
- a failed eligibility check cannot safely fall back to the direct mixed-attachment path on this
  topology.

At 2064x2208 RGBA8 stereo, the copies move about 36.5 MB per frame, or 3.28 GB/s at 90 Hz before
protocol and cache effects. The synthetic Unity-parity test measured the first copies for the two
eyes at 1.365 ms and 1.541 ms. Those are not steady-state percentiles. Severe hitches were reported
during an attended Walkabout launch, but their cause has not been isolated between application
multi-pass work, staging copies, shader/pipeline activity, transport, and encode.

### 4. D32S8 is application-correct but compositor-local depth is not consumed

RADV cannot export the requested `VK_FORMAT_D32_SFLOAT_S8_UINT` image through the common linear
modifier on this topology. The current Monado/WiVRn path therefore gives the NVIDIA application
real optimal D32S8 images and uses tiny private AMD images only for compositor bookkeeping. This
restores the D3D11 depth/stencil lifecycle without a cross-GPU depth copy.

WiVRn's current projection path does not sample the submitted
`XR_KHR_composition_layer_depth` image. The runtime consequently submits the layer as color-only.
Depth testing inside the game is correct, but the compositor has no depth-aware positional
reprojection or composition. If WiVRn begins consuming projection depth, this local-depth
downgrade must be replaced by a real transfer or conversion path.

Detailed measurements and the format-capability rationale are in
[`research/26-wivrn-multi-gpu-client-render.md`](research/26-wivrn-multi-gpu-client-render.md).

### 5. A healthy process is not proof of visible game output

The guarded Walkabout harness proves launch environment, exact mapped artifacts, native DXVK,
fresh shared-depth texture handles, process lifetime, kernel Xids, and exact-client fence health.
It does not inspect the final pixels delivered to the headset. A bounded Walkabout run satisfied
those guards while an attended observer reported an all-black game.

Future certification needs a pixel-level leg: a compositor-side frame checksum/readback, a
non-invasive headset capture, or another signal that distinguishes valid changing content from
black or stale frames. Until then, `unity-survived-with-staged-color` means exactly what it says;
it is not a visual PASS.

### 6. The launch and deployment stack is tightly coupled

The working experiment is not an upstream, self-contained Heroic configuration. It currently
depends on:

- a custom Heroic Meta store/runner; the installed upstream Heroic bundle has no Meta runner;
- a shared Meta prefix and launch-time OAF/OVRServer orchestration;
- a custom GE-Proton build with WineOpenXR and DXVK changes;
- a matching Monado/WiVRn build for client-local depth and diagnostics;
- explicit native `d3d11=n;dxgi=n` overrides in direct `runinprefix` probes;
- the staged-color environment switch for the affected split-GPU path; and
- version-pinned game plugin checks.

WineOpenXR's PE and Unix libraries also contain a private generated dispatch ABI. They must be
built and deployed as one pair. A runtime handshake now rejects the known mismatches safely, but
matching public exports, ELF dependencies, or SONAMEs alone is insufficient. Similarly, a native
DXVK `d3d11.dll` must match its `dxgi.dll` and expose the private interop interface WineOpenXR
expects.

The Wine OpenXR registry cache is volatile and scoped to the live wineserver. Cold-prefix probes
must initialize it without launching a competing Xalia/xrizer session. This bootstrap complexity
is a test and launcher deficiency, not evidence that the headset is unavailable.

### 7. Desktop preview windows and overlay input are not arbitrated

Many VR games create an ordinary desktop preview window. Hyprland treats it like any other window:
it can take focus, appear on a virtual monitor, or cover desktop content while the game owns the
headset. Route preview windows to a dedicated workspace with normal window policy; the OpenXR
runtime does not identify or suppress them for HypXRland.

Monado also delivers controller input to both the primary application and an `XR_EXTX_overlay`
client. HypXRland does not yet arbitrate a modal "game input versus desktop input" owner. See
[`05-configuration.md`](05-configuration.md#8-compose-over-another-vr-app--hypxrpaper-overlay-mode)
for the overlay behavior and current input limitation.

## Evidence snapshot

The durable evidence from the 2026-08-13/14 campaign is stored under
`~/.local/state/hypxrland/walkabout-d32s8/`. The most useful bundles are:

| Bundle | Meaning |
|---|---|
| `synthetic-candidate-20260813-1044/draw` | Direct imported-linear color plus local-optimal D32S8 draw reproduced Xid 69. |
| `unity-42926c8-20260813-2146` | Staged-color Unity-parity test submitted 120 stereo projection/depth frames, recorded both first-copy timings, cleaned up naturally, and produced no Xid or attributed fence wait. |
| `walkabout-staged-20260813-230954` | Patched separate-eye Walkabout survived the bounded game harness with shared depth, native NVIDIA DXVK, no new Xid, and no exact-client fence failure. This is not pixel validation. |
| `walkabout-single-pass-20260814-073312` | Valid original-plugin A/B: `Texture Array`, immediate eye-texture acquisition failure and crash, no Xid, no exact-client fence failure. |

Two nearby runs are deliberately excluded from the A/B conclusion:
`walkabout-single-pass-20260814-013755` was silently repatched by the normal Heroic launch path,
and `walkabout-single-pass-20260814-014459` never launched the game because its controlled shadow
Heroic package lacked frontend assets.

## Exit criteria for calling a title supported

A title should not move from partial/unsupported to supported until a version-pinned test proves:

1. the title reaches the intended OpenXR path without a game-binary patch, or every remaining
   patch has a documented, version-safe justification;
2. all required color/depth swapchain variants create, render, release, and recreate;
3. the headset shows changing, correctly stereo pixels rather than merely a live process;
4. a meaningful play interval has no new kernel Xid, terminal/persistent exact-client fence wait,
   or device loss;
5. frame pacing and copy/encode cost are measured over steady state, not only startup; and
6. launcher, runtime, WineOpenXR, DXVK, and game artifacts are recorded and reproducible.

For Walkabout specifically, the next low-level target is texture-array acquisition. The current
separate-eye patch should remain until that path passes a synthetic test and one guarded,
visually-attended game run.
