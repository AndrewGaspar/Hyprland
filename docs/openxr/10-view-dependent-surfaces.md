# 10 — View-dependent Wayland surfaces (portal design)

> **Status: experimental prototype; the protocol, compositor pose path, and native synthetic client
> are implemented, but the ABI is not stable.**
>
> Landed behavior and deferred work are called out explicitly below. `hypxr_viewpoint_v1` remains
> an experimental name, not a compatibility promise. This page is both the design contract and the
> acceptance plan for iterating beyond the synthetic proof.

This design lets an ordinary Wayland window behave like a tracked window into a 3D scene. Moving
the viewer's eyes relative to the window changes the scene perspective through it, while the
application remains a desktop client and HypXRland remains the only OpenXR client. The initial use
case is a Dead Space renderer shim producing a head-tracked “TV portal”, but the compositor-facing
contract is deliberately game-independent.

The central boundary is:

> The application renders a **view-dependent Wayland surface**. HypXRland owns the OpenXR session,
> the physical surface, and spatial policy.

This is not an immersive-VR mode. It does not give head motion to gameplay, take over controller
input, replace the game's camera, or make the game own an OpenXR session.

## 1. Why this architecture

HypXRland already turns a real headless output into a movable, anchorable OpenXR quad. Keeping the
application on that output preserves behavior that a native game-owned OpenXR session would have to
reimplement or abandon:

- monitor anchoring, moving, resizing, recentering, follow modes, and workspace policy;
- ordinary Wayland composition, including overlays, passthrough, chrome, window rules, and capture;
- composition inside a larger desktop or a protocol-aware nested compositor;
- one OpenXR session owner rather than a game contending with HypXRland for the headset;
- a small client integration surface that can be reused by game shims, emulators, remote viewers,
  CAD applications, and other renderers.

A direct OpenXR backend remains the better boundary for a truly immersive application that needs
projection layers, action input, compositor depth submission, or the lowest possible
motion-to-photon latency. Those are non-goals for the portal prototype.

## 2. Landed baseline and missing pieces

The following exists today:

- An XR monitor is an Aquamarine headless output paired with `CXRMonitorLayer`; the desktop buffer
  is copied to an OpenXR swapchain and submitted as a quad. See docs 01 and 02.
- The XR frame thread calls `xrWaitFrame`, obtains `predictedDisplayTime`, locates the `VIEW` space
  in the session reference space, solves the monitor anchor, and submits the final quad pose.
- Stereo content can declare an exact `stereo:sbs`, `stereo:hsbs`, `stereo:tab`, or `stereo:htab`
  layout through `xdg-toplevel-tag-v1`. A user `windowrule = stereo auto, match:xdg_tag
  ^stereo:.*` opts into honoring that declaration.
- A packed XR monitor can be submitted as an atomic left/right quad pair: two quads at one pose,
  selecting different image rectangles and using left/right `eyeVisibility`.
- Depth-desktop and stereo-content presentation create binocular disparity, but the submitted
  images contain no viewer-dependent projection. As documented in §9 of doc 05, they provide no
  head-motion parallax.
- The experimental `hypxr_viewpoint_v1` protocol can expose pair-latched, per-eye points for one
  explicitly authorized fullscreen surface per XR monitor. Its frame-to-main transport is a
  coalesced per-layer POD mailbox; Wayland protocol objects remain on the main thread.
- HypXRland locates the primary-stereo views at `predictedDisplayTime`, transforms their positions
  into the solved content rectangle, and rejects tracking loss, grabs, carries, and late-latched
  anchors.
- `rendered(epoch, sample_id)` is associated with the next newly attached non-null `wl_buffer`, including
  through the surface-state queue. The native `viewpoint-demo` client produces one pair-latched
  full-SBS buffer from the newest accepted sample and falls back to zero-disparity SBS when
  inactive.

The following does **not** exist today:

- Wine does not expose a compositor-provided viewpoint to injected Windows code;
- sample/target timestamps and pose-age telemetry are not implemented; the timestamp capability is
  not advertised and the wire fields are zero rather than raw `XrTime` values;
- the surface-state association is not yet carried through the renderer into an exact output-buffer
  generation. HypXRland therefore cannot yet prove which tagged client buffer produced a presented
  XR image or report its presentation age;
- HypXRland trusts a capable client to honor the pair-latched simulation-state guarantee. The
  synthetic client tests that invariant, but the compositor cannot infer it from pixels;
- nested forwarding, transformed/clipped portal rectangles, and game-specific HUD/culling/effects
  classification remain future work.

Further increments must add the remaining pieces without silently changing existing stereo,
depth, monitor, or tag behavior.

## 3. Requirements and non-goals

### 3.1 Required behavior

1. HypXRland remains the sole OpenXR application for the desktop and owns the final surface pose.
2. The client receives left/right eye points expressed relative to the **presented content
   rectangle**, not a global room or runtime reference space.
3. One viewpoint sample is latched across both rendered eyes.
4. Head movement changes only ephemeral render-view matrices. It never changes gameplay state,
   aiming, input, animation, physics, weapon rays, or the authoritative gameplay camera.
5. A missing, denied, stale, or inactive viewpoint safely degrades to ordinary stereo or mono
   presentation.
6. Viewer data is exposed only to an explicitly authorized surface.
7. The design can be forwarded by a protocol-aware nested compositor without exposing the
   upstream compositor's world coordinates.

### 3.2 Initial non-goals

- head orientation controlling the game camera;
- immersive first-person controls or OpenXR actions in the game;
- compositor-provided scene depth or positional timewarp of the scene inside the portal;
- arbitrary window transforms, clipping, decorations, subsurface trees, or multiple simultaneous
  portal rectangles in the first prototype;
- automatic portal detection from pixels, process names, titles, or an untrusted tag;
- solving application-specific culling, HUD, shadow, particle, or screen-space effects in the
  compositor.

## 4. Coordinate contract

The protocol should export **eye points relative to the final content rectangle**, not a full head
pose in `LOCAL_FLOOR`. This is the smallest useful and least revealing primitive.

For a front-facing surface-local frame:

- origin: center of the presented content rectangle;
- +X: right across the surface;
- +Y: up across the surface;
- +Z: out of the surface toward its visible side and nominal viewer;
- units: meters;
- orientation: the content rectangle's final orientation after compositor placement;
- extent: physical width and height of that content rectangle, excluding chrome margins.

Each eye is a point `(x, y, z)` in this frame. The client does not need head orientation to derive
an off-axis frustum. Omitting orientation also makes it harder to accidentally turn head motion into
camera or aim rotation.

Surface-relative coordinates make anchor movement well-defined. If a user carries, recenters, or
leashes a monitor while keeping the same relation between face and panel, the values remain stable;
the client need not know whether the panel is local-, head-, body-, or device-anchored. A runtime
reference-space recenter is similarly invisible after both eye and panel poses are expressed in the
same frame.

### 4.1 Authoritative rectangle

HypXRland owns the physical rectangle because it knows the solved anchor, content-vs-chrome
geometry, monitor aspect, and final OpenXR quad. The client may control its game-space calibration
and parallax gain, but must not independently guess where the physical panel is.

For version 1, the eligible rectangle is deliberately narrow:

- one undecorated, fullscreen surface covering one XR monitor;
- the surface's logical destination is exactly the monitor content rectangle;
- either a native-size full-SBS buffer or an aspect-preserving lower-resolution full-SBS buffer:
  the buffer width is even, the whole packed buffer has exactly the destination aspect, and
  `wp_viewport` supplies only that full logical destination. Each eye therefore has the aspect of
  half the logical destination, and both panes are scaled together;
- no `wp_viewport` source/crop, implicit sizing, non-normal buffer transform, buffer scale other
  than 1, or compositor effect that changes the rectangle geometry;
- the quad pose used for the sample is the same reference-space pose submitted to OpenXR;
- no `anchor:device`, grip/pinch late-latch, or active carry/grab. Those paths submit an action
  space whose final display-time pose the runtime can update after HypXRland sampled it;
- one active portal surface per XR monitor.

If any condition stops holding, feedback becomes inactive before the compositor treats another
rectangle as equivalent.

## 5. Negotiation and transport

A property or `xdg-toplevel-tag-v1` string is sufficient to declare static semantics, but not to
stream per-frame data or establish synchronization. The design therefore separates declaration
from feedback.

### 5.1 Existing stereo declaration stays unchanged

The current `stereo:*` tag remains the exact pixel-layout ABI from doc 05. It answers only “how are
the eyes packed in this committed buffer?” It must not gain version suffixes, pose data, permissions,
or portal-specific fields. HypXRland's current implementation stores one toplevel-tag string, so a
second `viewpoint:*` tag would also displace the stereo declaration. The viewpoint protocol object,
not another tag, is the client request.

### 5.2 Experimental Wayland object

The working protocol name is `hypxr_viewpoint_v1`, separate from `hyprland-surface-v1` and
`xdg-toplevel-tag-v1`. Conceptually:

1. A client binds the manager and requests a viewpoint object for its own `wl_surface` or
   `xdg_toplevel`.
2. The client declares its supported layouts and whether it can produce pair-latched stereo.
3. Compositor policy accepts or denies the request and reports the active presentation rectangle.
4. While active, the compositor emits viewpoint samples; time words are meaningful only when their
   capability and clock domain are advertised.
5. The client associates a committed buffer with the sample it rendered.
6. Deactivation or tracking loss invalidates feedback explicitly; the client falls back without
   reinterpreting an old sample as current.

The landed experimental XML is capability-based and versioned; `protocols/hypxr-viewpoint-v1.xml`
is authoritative. Its abbreviated event/request shape is:

```text
manager.get_viewpoint(new_id, wl_surface)

client -> compositor:
  set_capabilities(layouts, pair_latched)
  set_enabled(bool)
  rendered(epoch, sample_id)          # applies to the next newly attached non-null buffer

compositor -> client:
  capabilities(flags)
  active(epoch, geometry_id, width_um, height_um, layout, flags)
  sample(epoch, sample_id, geometry_id, sample_time_hi, sample_time_lo,
         target_time_hi, target_time_lo,
         left_x_um, left_y_um, left_z_um,
         right_x_um, right_y_um, right_z_um,
         validity_flags)
  inactive(epoch, reason)
```

This is a semantic sketch, not settled request numbering. Coordinate fields should be signed
32-bit **micrometers** (about ±2147 m of range); rectangle extents can be unsigned 32-bit
micrometers. This avoids `wl_fixed`'s unsuitable precision/range tradeoff while retaining a simple,
language-neutral integer ABI. Timestamps are separate 64-bit quantities carried as explicit
high/low words, with their clock domain defined by the protocol; they are never packed into the
coordinate representation.

### 5.3 Per-buffer sample association

`sample_id` is monotonic within one viewpoint-object activation. The client latches a complete
sample, renders both eyes from it, sends `rendered(epoch, sample_id)`, then commits the buffer. The request
is consumed only by the next `wl_surface.commit` carrying a newly attached non-null buffer.
Bufferless commits and commits carrying a null buffer do not consume a staged request. A newly
attached non-null buffer without a staged `rendered` request explicitly clears the association
inherited by bufferless commits; it must not silently retain the previous buffer's sample ID.

The completed design must eventually record:

- the sample ID the buffer claims;
- sample and target times;
- commit/presentation time;
- whether the buffer was presented, superseded, or rejected;
- pose age when the buffer was consumed by the XR frame.

The prototype currently retains only the activation epoch and sample ID on the `wl_surface` state;
the remaining timing and presentation observations are deferred.

An unknown, already-consumed, future, or activation-old ID is a protocol error or an explicitly
untracked commit; it must never be silently paired with the newest pose.

The prototype lands the association through `wl_surface` state and queued commits, but not through
the compositor renderer's output-buffer generation. Reading the surface's current association from
an output `presented` callback would be incorrect: a newer surface commit can already be current
while the callback refers to an older composite. Exact presentation attribution therefore remains
deferred until the render/output path carries commit generation and association together.

### 5.4 Event stream before shared memory

At headset rates, small Wayland events are adequate for the first measurement-driven prototype.
The XR frame thread must not enqueue every 90 Hz sample in the existing bounded state-event queue:
a delayed Wayland main thread would create stale-pose backlog and could crowd out lifecycle/input
events. Each eligible layer instead gets a coalesced **latest-value POD mailbox**. The frame thread
publishes one complete sample with release ordering and wakes the main thread on an empty-to-pending
edge; the main thread consumes the newest value, discarding superseded samples, and alone touches
the protocol objects. Surface association and authorization flow to the frame thread as plain
snapshotted state; no Wayland object or Hyprland refcount crosses the boundary.

Do not start with a globally named shared-memory object, shell polling, or a privileged global
socket. If profiling shows Wayland event dispatch is material, Wayland can negotiate and pass an fd
for a bounded single-producer/single-consumer ring. Wayland still owns surface association,
permission, activation epochs, and sample acknowledgement.

`hyprctl openxr gaze` is useful for manual inspection and a coarse proof, but command parsing and
shell/process latency make it unsuitable as the rendering backend.

## 6. Timing model

The compositor currently learns `predictedDisplayTime` after `xrWaitFrame`. A Wayland client cannot
normally receive that sample, render, commit, be composited, and reach the same XR frame. The first
implementation must therefore acknowledge an additional frame of internal-portal latency instead
of claiming same-frame prediction.

The likely initial cadence is:

```text
XR frame N:
  xrWaitFrame -> target time N
  locate views / solve portal rectangle
  publish surface-relative sample S
  submit latest already-composited client buffer

Wayland render opportunity:
  client receives S
  renders left + right from S and commits SBS buffer tagged S

XR frame N+1 or later:
  HypXRland consumes buffer tagged S
  record target/presentation delta and sample age
```

The physical quad can still benefit from runtime composition and the newest solved pose while the
scene visible through it reflects an older eye sample. During fast head translation that mismatch
can look like the world “swims” behind stable glass. Timestamps and observability are therefore
part of version 1, not future polish.

Both timestamps must have documented clock domains. `XrTime` must not be exposed as though it were
`CLOCK_MONOTONIC` unless the runtime conversion extension or an explicit clock mapping establishes
that relationship. A protocol can either expose a compositor monotonic sample time plus target
delta, or negotiate a clock-domain identifier and conversion. Sample IDs remain authoritative even
when a timestamp cannot be mapped exactly.

The current prototype deliberately does not advertise monotonic timestamps and sends zero in all
sample/target time words. This is an explicit unavailable value, not a claim that raw `XrTime` is a
POSIX monotonic timestamp.

Velocity-based prediction and client-specific render-time estimates can be evaluated only after
pose-age measurements exist. Prediction is not required for the first proof.

## 7. Stereo pair atomicity

The target buffer is one full-SBS image containing left and right eyes rendered from:

- one simulation state;
- one authoritative gameplay-camera state;
- one viewpoint `sample_id`;
- one calibration state.

HypXRland's current content producer already submits both eye quads back-to-back from one swapchain
image. That gives the compositor-side atomic presentation needed by the portal. The client must
provide the matching producer-side guarantee.

If an early game shim can only render alternating eyes on consecutive game frames, it must freeze
the sample across the pair and label the assembled buffer only when both are complete. This is a
prototype compromise: scene animation and effects may still disagree between eyes. Resampling the
viewer between eyes is forbidden because it turns ordinary head movement into an unstable stereo
baseline.

A compositor must never submit only one member of a portal pair. Under layer-budget pressure it
drops the pair or degrades according to negotiated policy, matching the existing all-or-nothing XR
stereo-pair behavior.

## 8. Client rendering contract

### 8.1 Two camera states

The client integration maintains a hard boundary:

```text
authoritative gameplay camera
  position/orientation, aim, animation, physics, input, weapon ray
  never modified by viewpoint feedback

ephemeral render-view camera, per eye
  translated eye origin
  asymmetric off-axis projection through the portal plane
  installed only around the classified world-render pass, then restored
```

Head orientation is not added to game-camera yaw or pitch. Looking or leaning through a physical
window changes the eye point and frustum; it does not rotate the world behind the window.

One expected consequence is that the authoritative aim point need not remain at the center of the
view as the viewer leans sideways. That is correct. A world-space weapon laser should remain on the
true gameplay ray and visibly move across the portal image. A center-screen reticle would become
misleading and should be suppressed or projected from the authoritative aim state.

### 8.2 Off-axis projection

For each eye, the client derives a generalized asymmetric frustum through the portal rectangle.
Let the eye point and four rectangle edges be expressed in one camera-oriented coordinate frame.
The left/right/top/bottom frustum extents at the near plane are proportional to the vectors from the
eye to those edges divided by the perpendicular eye-to-plane distance. Horizontal shear alone is
insufficient: vertical lean needs top/bottom asymmetry, and moving toward or away from the panel
changes the apparent field of view.

The implementation should use a tested generalized off-axis projection helper rather than
accumulating yaw/pitch corrections. Reject or clamp samples at/behind the portal plane, where a
forward-looking frustum becomes singular or inverted.

### 8.3 Neutral calibration and gain

A geometrically literal window uses the physical panel size and eye distance directly. That can
cause portal activation to change the apparent game FOV sharply if the game's configured FOV does
not match the panel's angular size. The prototype should instead support a neutral calibration:

1. Latch the surface-relative eye midpoint when portal mode activates.
2. Preserve the game's original symmetric projection at that neutral pose.
3. Apply deviations from neutral as bounded render-eye translation and off-axis shear.
4. Map physical meters to game-space meters with an application-owned gain.

The mapping is conceptually:

```text
render_eye = gameplay_camera_origin
           + camera_right    * delta.x * gain_x
           + camera_up       * delta.y * gain_y
           + camera_backward * delta.z * gain_z
```

The exact sign of the camera-forward term follows the game's camera convention and must be covered
by tests. Start with a ±10–15 cm physical translation box, conservative Z gain, and no rotational
input. Clamps are comfort and content-safety policy, not a substitute for correct frustum math.

### 8.4 Culling, HUD, and effects

Render-camera translation may reveal geometry outside the gameplay camera's original view or behind
nearby walls. The first safe policies are a union/padded world-culling frustum or retaining the
authoritative gameplay culling result where possible. Changing the authoritative culling origin is
not allowed if that affects simulation or visibility-dependent gameplay.

HUD and effects must be classified rather than globally “corrected”:

- world geometry: render from the eye-shifted view;
- world-space aim indicators and laser sights: remain attached to authoritative gameplay state,
  but render consistently in the shifted view;
- flat HUD, menus, subtitles, and loading UI: render after restoring the gameplay camera, or into a
  separate surface-attached pass;
- shadows, particles, reflections, deferred lights, temporal effects, and CPU-projected elements:
  opt in only after each consumer is understood.

Portal-off must restore the unmodified rendering path exactly.

## 9. Wine ownership seam

An injected Windows game DLL does not own its Wayland `wl_surface`; Wine's display driver does.
The durable design therefore needs a Wine-side bridge:

```text
HypXRland Wayland protocol object
        -> Wine Wayland/X11 driver or helper
        -> process-local, versioned C ABI
        -> injected game renderer shim
```

The Wine component binds the viewpoint object to the correct game surface, receives events, and
publishes the latest complete sample to game code. The game shim returns the consumed `sample_id`
through the same bridge before the corresponding buffer commit.

For the first proof, a narrowly scoped compositor IPC endpoint plus a process-local shared page is
acceptable if it is keyed to one resolved surface/client and carries activation epochs. It is
temporary scaffolding, not the long-term ABI. A global named shared-memory region, PID-only matching,
or polling `hyprctl` is not acceptable as the final bridge.

The reusable outcome should be a small client library with a plain C ABI so other Wine shims and
native applications do not reproduce Wayland plumbing.

## 10. Authorization and privacy

Head and eye motion is sensitive behavioral data. A client-controlled tag, class, title, executable
path, or command line can identify a surface for policy matching, but cannot authorize access.

The intended policy is:

- client request: “this surface can render viewpoint-dependent content”;
- compositor/user rule: “this concrete surface may receive viewpoint feedback”;
- per-surface protocol object: the only place feedback is delivered;
- least information: eye points relative to that surface, not global head pose, room origin,
  controller poses, gaze target, or other monitors;
- explicit invalidation when the surface is hidden, not on an XR monitor, not eligible, the headset
  is not present/visible under policy, tracking is invalid, or permission is revoked;
- no data inheritance when a `wl_surface`, toplevel, process, or activation epoch is replaced.

Status should make authorization, active/inactive reason, most recent sample ID, committed sample
ID, and pose age inspectable without exposing coordinates to unrelated clients.

## 11. Nesting and composition

The portal remains ordinary Wayland content, so it can be placed among other windows, carried as an
XR monitor, shown over passthrough, and included in a larger composition. The surface-relative
contract is also forwardable: a protocol-aware nested compositor can transform the upstream eye
points into a child's final content rectangle and issue its own sample IDs.

Nesting is not automatic. An unaware intermediary sees only packed pixels and flattens the portal.
A protocol-aware intermediary must:

1. know the final visible child rectangle after scale, crop, rotation, and nesting transforms;
2. transform both eye points into that rectangle's local frame;
3. preserve activation and permission boundaries;
4. translate sample/commit association without claiming a newer pose than the child rendered;
5. reject a degenerate, non-planar, occluded, or ambiguous presentation.

The direct fullscreen XR-monitor case comes first. Arbitrary floating windows, subsurfaces,
decorations, clipping, rotated planes, and portal surfaces rendered into application textures are
later protocol/implementation versions.

## 12. Capability and fallback ladder

A client and compositor should negotiate the highest mutually safe level:

| Level | Presentation |
|---|---|
| 0 | ordinary mono Wayland surface |
| 1 | current declared packed stereo, no head-motion parallax |
| 2 | head-center translation with one latched sample, optionally mono |
| 3 | per-eye surface-relative points and one pair-latched SBS commit |

Loss of tracking, permission, eligibility, protocol support, or an XR session moves down the ladder
without changing gameplay. A stale sample does not remain active indefinitely. The exact stale-time
threshold should be measured and configurable during prototyping, then fixed or bounded before the
protocol is considered stable.

## 13. Staged implementation

Each stage should land independently with its own rollback and evidence.

The synthetic prototype covers the compositor/client slices of Stages 0, 2, and 3. The bullets
below remain completion criteria for the general design; unchecked game, timing, observability,
and renderer-carrier work is not implied by the native proof.

### Stage 0 — math and game invariants, no live transport

- Add an OpenXR-header-free pure HypXRland geometry helper that transforms synthetic world-space
  eye points through a solved content pose into the §4 surface-local coordinate contract. Cover
  rotated/translated surfaces, content-vs-chrome geometry, sign conventions, and invalid planes in
  unit tests before connecting it to the frame thread.
- Feed recorded/synthetic surface-relative traces to the game shim.
- Implement horizontal and vertical off-axis projection, depth/FOV response, neutral calibration,
  gain, and clamps.
- Prove render state is restored and authoritative gameplay/aim state is unchanged.

### Stage 1 — bounded prototype bridge

- Export head-center position relative to one dedicated fullscreen XR monitor.
- Use one activation epoch and monotonic sample ID.
- Keep the bridge local, surface/client-scoped, removable, and visibly experimental.
- Render both eyes from one frozen sample; permit consecutive engine frames only as a documented
  temporary limitation.

### Stage 2 — experimental Wayland protocol

- Add `hypxr_viewpoint_v1` negotiation, authorization, active/inactive events, timestamps, and
  per-buffer sample association.
- Add status counters for samples, commit association, drops, and pose age.
- Add a native synthetic client before integrating Wine.

### Stage 3 — per-eye view location and atomic client frames

- Locate the primary-stereo views with `xrLocateViews` at the chosen target time.
- Express each eye in the solved surface frame.
- Produce both game eyes from one simulation state into one full-SBS commit.
- Submit only complete, associated pairs.

### Stage 4 — Wine bridge and renderer classification

- Bind a game surface through Wine and expose a versioned process-local C ABI.
- Classify HUD, culling, shadows, particles, deferred lighting, temporal effects, and CPU-projected
  consumers one at a time.
- Retain a one-switch return to current static SBS.

### Stage 5 — nesting and broader clients

- Forward the protocol through a nested compositor.
- Define transformed/clipped content rectangles.
- Validate a second native client or game integration before considering a compositor-neutral
  protocol proposal.

### 13.1 Build and run the native proof

Build the client without configuring the full compositor tree:

```sh
cmake -S hyprtester -B build-viewpoint-demo -DCMAKE_BUILD_TYPE=Debug
cmake --build build-viewpoint-demo --target viewpoint-demo -j
```

The renderer can produce deterministic active and fallback images without Wayland or OpenXR:

```sh
./build-viewpoint-demo/viewpoint-demo --render /tmp/viewpoint-active.ppm
./build-viewpoint-demo/viewpoint-demo --render-fallback /tmp/viewpoint-fallback.ppm
```

Live feedback is privacy-gated by an explicit window rule. The client already declares
`stereo:sbs`; the stereo rule opts into honoring that declaration, while `viewpoint on` authorizes
head-pose-relative feedback only for this app ID:

```ini
windowrule = stereo auto, match:xdg_tag ^stereo:sbs$
windowrule = viewpoint on, match:class ^(hypxr-viewpoint-demo)$
```

There are two different kinds of “tag” in common configurations:

- the demo's **xdg toplevel tag** is `stereo:sbs`, set by the client through
  `xdg-toplevel-tag-v1`; `match:xdg_tag` matches it;
- a plain Hyprland **window tag** such as `stereo-sbs` is a user policy selector and is matched by
  `match:tag`. It is not set by the demo.

If the existing configuration uses the latter policy style, use these rules instead and apply the
policy tag after the window maps:

```ini
windowrule = stereo sbs always, match:tag stereo-sbs
windowrule = viewpoint on, match:class ^(hypxr-viewpoint-demo)$

# Resolve the address from `hyprctl clients`; do not copy this placeholder literally.
hyprctl dispatch tagwindow +stereo-sbs address:0xWINDOW_ADDRESS
```

Do not configure both stereo rules merely as a troubleshooting reflex; one matching authorization
path is enough. If the image looks like a doubled/mispacked SBS surface, inspect the client in
`hyprctl -j clients`: `xdgTag` should equal `stereo:sbs`, the ordinary `tags` list
should include `stereo-sbs` only when using the second recipe, and the resolved `stereo` field must
be `sbs`, not `off`. An output name such as `XR-4` is not evidence that it is non-XR; confirm the
monitor in `hyprctl -j openxr`.

Launch the demo on one dedicated XR monitor, with its anchor local and docked, then run:

```sh
./build-viewpoint-demo/viewpoint-demo --debug
```

The default per-eye render budget is at most 256×144. The client chooses an exact-aspect size from
the configured destination: a 3840×1080 packed destination uses 256×144 per eye and a 512×144
full-SBS buffer, while 1920×1080 uses 128×144 per eye and a 256×144 full-SBS buffer.
`wp_viewporter` scales the whole packed buffer to the exact fullscreen destination. `--width` and
`--height` change the per-eye upper bounds. `--windowed` is useful for fallback inspection but is
intentionally ineligible for feedback. In a successful live proof, lateral head movement shifts
near, middle, and far geometry by different amounts, the cyan portal reticle stays
surface-centered, and the red authoritative world-space aim impact changes projection without
changing its world coordinate. Stereo disparity without that depth-dependent motion is not a
successful portal-parallax result.

An active hand/controller grab or gaze carry intentionally makes the v1 surface ineligible: the
final runtime-latched panel pose is not knowable at the point where the sample is produced. The
demo therefore falls back to static SBS while the monitor is being moved and should reactivate
with a new epoch after release. Seeing the portal stop responding *during* the move is expected;
remaining static after release is not. Check the demo's `--debug` output for a fresh `active`
event/sample stream and re-check fullscreen coverage, the resolved `stereo` field, and the
`viewpoint on` authorization before relaunching anything.

## 14. Tests and acceptance criteria

### 14.1 Pure math

- world-to-surface-to-world point round trips across translated and rotated quads;
- applying one rigid transform to both the eye points and quad leaves the surface-local points
  unchanged;
- left/right/up/down/forward viewer motion produces the expected asymmetric frustum signs;
- neutral pose reproduces the original projection within tolerance;
- eye at or behind the plane is rejected/clamped without NaNs or an inverted projection;
- IPD and portal-size cases preserve left/right ordering and units;
- gain and translation clamps are deterministic;
- one sample produces both eye matrices.

### 14.2 Protocol and compositor, headless where possible

- only the owning, authorized surface receives samples;
- denial, revocation, surface destruction, remap, and XR session loss invalidate the object;
- sample IDs are monotonic within an activation and cannot cross activation epochs;
- `rendered(epoch, sample_id)` applies to exactly the next commit carrying a newly attached non-null
  buffer; bufferless and null-buffer commits do not consume it;
- a newly attached, untagged non-null buffer clears the association retained across bufferless
  commits;
- malformed, reused, unknown, and future sample IDs fail safely;
- a runtime reference-space recenter preserves surface-relative eye coordinates, while moving only
  the monitor produces the expected local-coordinate delta;
- content-vs-chrome rectangle dimensions are correct;
- device/grip/pinch late-latched anchors and active carries invalidate v1 feedback;
- one stereo commit produces two coincident eye-restricted quads or no pair, never one eye;
- existing `stereo:*` tags and non-viewpoint clients are behaviorally unchanged.

The native synthetic client should render a calibration grid and deterministic near/mid/far
geometry. The Monado remote driver can provide scripted head translations for integration tests;
no attended headset test is needed until the math and protocol assertions pass.

### 14.3 Game invariants

With identical game input and simulation state, move the synthetic/headset viewpoint and verify:

- Isaac's position, orientation, animation state, and heading do not change;
- aim vector, weapon ray, projectile/impact coordinates, and gameplay camera remain identical;
- only temporary render-view/projection matrices and an explicitly allowed culling envelope change;
- both eyes report the same sample ID and simulation frame;
- HUD/menus remain attached to the portal according to their classified policy;
- disabling portal mode restores the original rendering exactly.

### 14.4 Guarded live acceptance

Only after the lower tiers pass:

- use one dedicated monitor, one synthetic scene or already-characterized game, and a ±15 cm head
  box;
- establish runtime, GPU/Xid, fence, and frame-time baselines as required by doc 06;
- confirm the physical quad stays locked while the scene shows plausible window parallax;
- measure committed-sample age and look for “swimming behind glass” during slow and fast motion;
- stop on the first rendering, session, GPU, or cleanup failure.

## 15. Open design questions

These should be answered with prototype evidence before freezing an XML ABI:

- Is a `wl_surface` association sufficient, or must the protocol bind an `xdg_toplevel` to make
  eligibility and Wine ownership unambiguous?
- Should the compositor send only eye points, or also the head midpoint as a convenience and mono
  fallback?
- What target-time representation is portable across OpenXR runtimes and Wayland clients?
- Can normal Wayland event delivery meet the latency budget, or is an fd-backed ring justified?
- How should buffer/sample association compose with explicit synchronization and presentation-time
  protocols?
- What stale-sample threshold produces a safe fallback without visible flapping?
- Does neutral-calibrated projection remain convincing under monitor resize and follow motion, or
  should resizing explicitly re-latch calibration?
- Which culling policy preserves Dead Space correctness without exposing large amounts of unseen
  scene work?
- What is the smallest Wine driver extension that works for both Wayland and XWayland game windows?
- Which semantics are general enough to move from a HypXRland-prefixed experiment toward a shared
  Wayland protocol after multiple implementations exist?

## 16. Likely implementation touchpoints

This is a routing guide, not a claim that the files already implement viewpoint feedback:

| Area | Current owner | Proposed responsibility |
|---|---|---|
| predicted XR timing and `VIEW` location | `src/openxr/OpenXRManager.cpp`, `XRSession.*` | locate primary stereo views and publish a complete frame-thread sample |
| final monitor/content pose and meters | `CXRAnchor`, `CXRMonitorLayer`, stereo/chrome geometry | transform eye points into the authoritative content-local frame |
| Wayland object and surface lifetime | `src/protocols/` | negotiate and authorize a per-surface viewpoint object on the main thread |
| frame → main delivery | new per-layer coalesced latest-value POD mailbox + wake | overwrite stale samples rather than queueing 90 Hz events; keep protocol objects and sends main-thread-only |
| buffer/sample association | Wayland surface commit path | consume staged `sample_id` state only with the next newly attached non-null buffer; retain it across bufferless/null-buffer commits and clear the prior association on an untagged new buffer |
| packed content declaration | `xdg-toplevel-tag-v1`, `StereoContent.hpp` | remain unchanged; describe only the client's pixel layout |
| XR pair submission | `OpenXRManager.cpp`, `XRStereoPair.hpp` | reuse the existing atomic two-quad presentation after validating sample association |
| game/Wine integration | owning Wine and game-mod repositories | receive samples and generate one pair-latched SBS buffer without changing gameplay state |

Implementation changes belong in their owning repositories. A Wine or game-shim change is committed
there first; only its corresponding HypXRland protocol, tests, or documentation lands here.
