# 22 — Spatial persistence and *locations*: a house-fixed coordinate system that survives don/doff

**Status:** research / decision-support. **Nothing here is implemented.** No code changes, no builds,
no live actuation — every `hyprctl` call in the evidence base was read-only, and the Quest was not
reachable over adb this session. Author: research pass 2026-08-03. Base commit: `c3bdf3aa`.

The ask (user's words, distilled): *every don is a gamble.* Monitors re-evaluate their positions
against a fresh session origin and land in "extremely non-ideal positions." The desired end state is
a **house-wide fixed coordinate system** — a monitor placed at a position **stays there** across dons
and across sessions — plus the nuance that monitors can deliberately **migrate** with the user
between working spaces. The missing abstraction is **locations**: named frames (desk, couch,
kitchen) that objects bind to, instead of raw session coordinates.

Cross-refs (source of truth):
- `docs/openxr/03-anchoring.md` §8 (reference-space change, recenter-on-plug), §9 (layout
  serialization); `docs/openxr/02-virtual-monitors.md` (plug lifecycle); `docs/openxr/05-configuration.md`
  §3.5 (`xrrule`) and its IPC section (`hyprctl openxr gaze at <ms>`).
- `research/archive/13-adaptive-anchoring.md` — **shipped**; the dock↔roam decorator, geofence and
  eased blend that this design's migration layer is built on.
- `research/archive/18-monitor-plugged-state.md` — **shipped, then evolved**; owns the don edge.
- `research/LAYOUT-AND-NAMING.md` — grids as a shared parent transform; a location is arguably a grid
  parent transform with an extent.
- `research/MULTI-MACHINE.md` (§8 below), `research/XREAL-3DOF.md` (the 3DoF degradation in §5.4).

Evidence base (all read-only):
- **Live compositor**: `hyprctl openxr status`, `hyprctl openxr layout`, `hyprctl monitors`;
  `~/.config/hypr/hyprland-xr.conf` and its git history; `~/.config/wivrn/config.json`.
- **HypXRland** `src/openxr/*` — `XRSession.cpp`, `XRAnchor.{hpp,cpp}`, `OpenXRManager.{hpp,cpp}`,
  `XRMonitorConfig.cpp`, `XRRule.{hpp,cpp}`, `XRIpc.cpp`; `tests/xr/*`.
- **Vendored monado** `subprojects/monado` — `u_space_overseer.c`, the generated extension table,
  `targets/ctl/main.c`, `src/xrt/ipc/*`.
- **WiVRn 26.6.2** (`~/code/wivrn-26.6.2`, incl. its fetched+patched monado at
  `build-server/_deps/monado-src`), our fork `~/code/wivrn` (branches `guardian-*`, `boundaryless`),
  and `~/code/wivrn-wt-client-mic`.
- **hypxrvoice** `~/code/hypxrvoice` — the deixis/gaze-ring contract the bronze rung reuses.
- Web: OpenXR registry + spec, Meta developer docs, monado GitLab, WiVRn GitHub.

---

## TL;DR — RECOMMENDATION

1. **The make-or-break finding on anchors: there is no persistent-anchor support anywhere in this
   stack, and it is *entirely* a Quest-client-side project.** The WiVRn client requests one required
   and ~30 optional OpenXR extensions, and **not one** is an anchor/spatial-entity/scene extension
   (`client/application.cpp:1231-1272`). The wire protocol has no anchor message. The WiVRn/monado
   runtime HypXRland actually talks to implements **zero** anchor extensions. **So HypXRland can
   never call `xrCreateSpatialAnchor*` itself, no matter what we do** — any anchor must be created
   and resolved by the Quest-side client against the Meta runtime and shipped over the wire.

2. **But anchors are the wrong target. There is a Khronos extension purpose-built for this exact
   problem, and Meta already ships it.** `XR_EXT_stationary_reference_space` (ext 743, ratified into
   **OpenXR 1.1.59, 2026-04-30**) defines a reference space that is *by specification* unaffected by
   system recenter, unaffected by the user redefining the room boundary, able to **regain its
   location after tracking loss, after the headset is removed and put back on**, and **persistent
   across app restarts and device reboots** — plus a `generationId` UUID that tells you when
   relocalization failed and your saved layout is stale. Meta ships the experimental predecessor
   **`XR_EXTX2_stationary_reference_space`** in its OpenXR SDK preview headers (since v77, still in
   v85), gated behind the *same* `com.oculus.experimental.enabled` manifest lever **we have already
   proven works on this headset** (the boundary-visibility precedent). This is a far smaller, far
   more upstreamable client patch than plumbing the anchor API, needs no `USE_ANCHOR_API` permission,
   and dodges the untrusted-sideloaded-package gate that would otherwise block anchors on our APK.
   **This is the gold rung.** See §3.1.

3. **A second surprise: we can already set the runtime's LOCAL origin from Linux, today, with no
   patches to anything.** WiVRn ships **libmonado** and advertises it in its runtime manifest —
   verified live on this box: `/usr/share/openxr/1/openxr_wivrn.json` carries
   `"MND_libmonado_path": "../../../lib/wivrn/libmonado_wivrn.so"`, and that library exports
   `mnd_root_create`, `mnd_root_get_reference_space_offset`, **`mnd_root_set_reference_space_offset`**,
   `mnd_root_recenter_local_spaces` and `mnd_root_set_tracking_origin_offset`. This is a supported,
   maintainer-blessed out-of-band channel (monado MR !2284, merged 2024-07-19, explicitly "to open
   the door for 3rd party apps to do space manipulation and calibration"). §4.2.

4. **Ship the bronze rung first — it is the immediate gamble-killer and needs nothing from anyone.**
   A single `xrlocation register desk` keybind (or the voice phrase "this is my desk") captures the
   current head pose, and one point + gravity-aligned yaw **provably** determines the transform:
   OpenXR *guarantees* LOCAL/LOCAL_FLOOR/STATIONARY are gravity-aligned with +Y up (spec 1.1 §7.1),
   so the relative rotation must be about ŷ and the transform has exactly **4 DoF** — 3 equations
   from the point, 1 from the yaw, uniquely determined. That is ~2 seconds of ritual per don in
   exchange for monitors that land where you left them; it degrades perfectly to XREAL 3DoF
   (yaw-only); it is fully headless-testable; and it feeds the **same applicator** every higher rung
   feeds. Silver and gold only automate the ritual away.

5. **Locations are the right abstraction and they belong in a state file, not the config.** A
   location is `{name, 4-DoF solve, extent, layout}`; monitors bind to one by name and store their
   pose **in that location's frame**. Two things fall out free: (a) `hyprctl openxr layout` finally
   emits *durable* text, because a location-relative pose means the same thing next session; (b)
   migration is adaptive anchoring's shipped geofence with the redock target swapped — leave location
   A's extent while roaming, enter B's, redock into B's frame.

6. **Two cheap fixes fall out of this research regardless of the rest, and both should be done
   immediately:**
   - **Recenter teleports your monitors.** monado emits its reference-space-change with
     **`pose_valid = false`, `pose_in_previous_space = IDENTITY`** (`u_space_overseer.c:873-876`) even
     though it just computed the exact delta; HypXRland correctly leaves poses untouched when the
     pose is invalid (`03-anchoring.md:466`). Net effect: **every Quest system recenter moves the
     world out from under the monitors.** ~20 lines in a tree we already carry 8 patches on. §4.3.
   - **A boundary re-setup shifts everything silently.** WiVRn's client filters
     `REFERENCE_SPACE_CHANGE_PENDING` on `== LOCAL` (`client/scenes/stream.cpp:1298-1302`) and
     ignores STAGE entirely — but the client streams every pose **in STAGE**
     (`client/application.cpp:1351`). So a guardian redraw or space re-setup moves the whole world and
     the server is never told. §3.2.

7. **Recommended order:** **§4.3 + S1 (cheap fixes)** → **B1–B5 (bronze ritual + location model +
   state file + don-edge)** → **B6–B8 (xrrule, migration, voice)** → **G1 (measure what enumerates on
   the Quest) → G2–G4 (stationary reference space over the wire)**. Bronze buys most of the felt
   benefit for a fraction of the effort; gold removes the last two seconds and, as a bonus, gives a
   genuine cross-machine house frame (§8).

Ranked on ROI: **bronze > cheap fixes > silver > gold**. Ranked on end-state quality: **gold
(stationary space) > gold (anchors) > bronze > silver**. §9 sizes all of them.

---

## 1. The gamble, measured

This is not a vague complaint — the live session shows the exact magnitude.

The user's config declares one monitor (`~/.config/hypr/hyprland-xr.conf:100`):

```ini
xrmonitor = XR-main, 2560x1440@90, anchor:local pos:0.017,1.457,-1.408 yaw:5.3 pitch:3.0 adaptive:on roam:body, size:2.23
```

`hyprctl openxr layout` on the live session right now returns:

```
xrmonitor = XR-main, 2560x1440@90, anchor:local pos:-0.415,1.684,1.639 yaw:136.8 pitch:18.3 adaptive:on roam:body, size:2.23
xrmonitor = XR-2,    2560x1440@90, anchor:local pos:-0.575,0.434,1.829 yaw:137.8 pitch:-28.5, size:1.60
xrmonitor = XR-3,    2560x1440@90, anchor:local pos:-0.861,0.988,-0.094 yaw:58.2  pitch:-6.8,  size:1.60
```

The declared and live poses for the *same monitor* differ by **≈3.1 m of translation and 131° of
yaw**. That is not drift; it is a different coordinate system. Three facts produce it:

1. **The world frame *is* the session origin.** `CXRSession::createSpaces`
   (`src/openxr/XRSession.cpp:252-279`) creates exactly two spaces: `LOCAL_FLOOR` (or `LOCAL` if
   `XR_EXT_local_floor` is absent) and `VIEW`. **STAGE is never created anywhere in the codebase**,
   and there is no boundary/play-area concept at all. `anchor:local` means "relative to whatever the
   runtime's LOCAL_FLOOR origin happened to be at `xrCreateSession`."
2. **Under WiVRn boundaryless/standby that origin is arbitrary and non-reproducible.** The codebase
   says so itself, in the comment on `recenterLocalToHead` (`src/openxr/XRAnchor.hpp:392-399`):
   *"Under WiVRn boundaryless/standby the runtime's LOCAL_FLOOR origin is arbitrary, so a monitor
   declared at e.g. `pos:0,1.5,-1.5` lands wherever that origin happens to be — often far from the
   user."*
3. **`openxr:recenter_on_plug` (default on) papers over (2) by throwing away the world frame
   entirely.** On the **first** confirmed plug of a session, the main thread arms an atomic
   (`OpenXRManager.cpp:3069-3075`) and the frame thread calls `recenterLocalToHead` on every layer
   (`OpenXRManager.cpp:1689-1704`):

   ```cpp
   const float   yaw = qYawOf(view.rot, 0.F);
   const SXRPose frame{Vec3{view.pos.x, 0.F, view.pos.z}, qFromYaw(yaw)};
   const SXRPose W    = poseCompose(frame, declared.anchorPose);
   m_state.anchorPose = W;                       // warp, deliberately not a glide
   ```

   i.e. the **declared** pose is reinterpreted as *head-relative*, in a yaw-only floor-projected
   frame, using the head pose of whatever instant you happened to be looking when the plug settled.

### What this means

`recenter_on_plug` is a **good** mitigation of a **bad** situation, and it is the closest thing to a
"location" the codebase has. Note carefully what it already gets right, because the design below
generalizes it rather than replacing it:

- it re-poses **all** monitors with the **same** head pose, so the group transforms **rigidly** and
  the relative arrangement survives;
- it is **yaw-only and floor-projected** — no pitch/roll leaks in, declared `y` survives as a real
  floor height;
- it re-docks adaptive monitors and re-captures the desk seat (`XRAnchor.cpp:1114-1122`).

What it gets wrong is only the *source* of the frame: **your head at an arbitrary instant** instead
of **a remembered place in the house**. Everything else about it is the machinery this report wants.

Four secondary consequences, all observed:

- **Saving a layout is pointless, and the user has empirically stopped doing it.** `git log` on
  `hyprland-xr.conf` shows the `xrmonitor` line was written **once**, at the config's creation, and
  never re-saved across 13 subsequent commits. `hyprctl openxr layout` emits the *live solved world
  pose* for a plain `anchor:local` monitor (`OpenXRManager.cpp:4176-4180`) — a number in a frame that
  will not exist next session. There is nothing worth pasting.
- **The head pose at the plug instant is a terrible frame.** The settle window
  (`openxr:monitor_plug_settle_ms`, default 1500 ms) guards against a runtime that sprints to
  VISIBLE while doffed, but nothing guarantees you are *seated at your desk facing forward* when it
  fires. Donning while standing, mid-turn, or holding the headset in your hand all bake a wrong yaw
  into the whole rig — and yaw error is the one that hurts, because it swings monitors through the
  room.
- **A doff→don inside a live session does not re-pose at all.** `m_recenteredThisSession`
  (`OpenXRManager.hpp:762-769`) is reset only by `resetPresenceState()`. Since WiVRn keeps the
  session alive on the shelf (that is exactly why `monitors_follow_session` had to move to
  visibility+presence, report 18 addendum 2), **"carry the headset to the couch and put it back on"
  produces no edge and no re-pose** — the monitors are still nailed to wherever the desk frame was.
  This is the *other* half of the user's complaint, and it is the half locations are really for.
- **Runtime-created monitors have no persistence at all.** `XR-2` and `XR-3` above were created by
  voice (`hypxrvoice`'s `create_monitor`) and exist only in-process; a session stop with
  `openxr:destroy_monitors_on_stop` ends them, and even without it their poses are never written
  anywhere. There is **no disk persistence of XR spatial state whatsoever** — verified:
  `grep -rniE "XDG_STATE_HOME|\.local/state|ofstream" src/openxr/` returns zero hits.

---

## 2. The conceptual model, tested against the evidence

The user's model is:

1. **House frame** — a persistent coordinate system maintained by solving `session_origin → house` at
   each don.
2. **Locations** — named pose+extent sub-frames in the house frame, each owning its remembered
   monitor layout.
3. **Objects bind to locations**; adaptive anchoring is the within/between behavior layer, and
   leaving one location's geofence in follow-mode while entering another's extent is **migration**.

**The evidence supports this model, with three amendments.**

### 2.1 Amendment A — the house frame should be *derived*, not stored

There is no need for a separate house frame *and* per-location frames. A location's pose in the
house frame is only ever used to compose with the house solve. If instead you **solve directly to the
location you are in** — "you are at the desk, and here is `desk → session_origin`" — the house frame
degenerates into "the union of all locations," and:

- you never need to know where the kitchen is relative to the desk (which is the hardest thing to
  measure and the thing that drifts most);
- a location whose transform is stale only breaks *that* location;
- the bronze ritual becomes per-location (register the desk while at the desk) instead of requiring
  one global survey of the house.

**Keep the house frame as a concept** (it is what makes "the kitchen monitor is over there,
summonable" expressible) but **make it optional**: locations that have been co-registered in the same
session get a house-relative pose for free; locations registered independently simply have none, and
that is a supported state. §6.4 covers what "no house relation" costs.

### 2.2 Amendment B — the transform is 4-DoF, and that is a feature

Both frames are gravity-aligned, and this is a **spec guarantee, not an assumption**. OpenXR 1.1
§7.1 defines LOCAL as establishing "a world-locked origin, **gravity-aligned to exclude pitch and
roll**, with +Y up, +X right, -Z forward," with identical language for LOCAL_FLOOR and STATIONARY,
and STAGE as +Y up with X/Z along the rectangle edges. A rotation R ∈ SO(3) that maps ŷ→ŷ *is* a
rotation about ŷ. Therefore `session_origin → location` has exactly **four** degrees of freedom:
`x`, `y`, `z`, `yaw`. Pitch and roll are *pinned by gravity* and must never be solved for — a solve
that produces them has an error, not a measurement.

This is not a simplification, it is a robustness property:

- **One point + one yaw determines it exactly.** A single head-pose sample at a known place supplies
  both: the position gives 3 equations (`x, y, z`), the head's horizontal facing gives the 4th. Four
  equations, four unknowns, unique solution. No least-squares fit, no multi-point registration, no
  ICP. (The general unconstrained problem is Horn's quaternion method, *JOSA A* 4(4):629, 1987; the
  gravity-constrained (3+1)-DoF specialization is a studied modern problem — Li et al., *ISPRS J.
  Photogramm. Remote Sens.* 199:118-132, 2023, DOI `10.1016/j.isprsjprs.2023.03.022`.)
- **But note it is exactly determined, with zero redundancy** — every bit of sample noise lands
  directly in the answer. A point alone constrains nothing about yaw (rotation about an axis through
  the point is free), so the yaw datum must be genuinely independent, and it is the term with the
  worst error amplification (§3.3). **Hardening, if the ritual proves noisy:** average N frames of a
  short dwell (nearly free — we already have a 90 Hz pose ring and `dwellStep`), or take **two**
  points and derive yaw from the horizontal bearing between them ("register desk, then register the
  desk's right edge"), which is the standard robust construction.
- **It is exactly what monado's recenter already computes** — `recenter_local_spaces`
  (`u_space_overseer.c:846-866`) explicitly zeroes the quaternion's `x` and `z` components ("Only save
  the rotation around y axis") and takes only `position.x`/`position.z`, keeping `y`. (Caveat worth
  knowing: zeroing x/z is a *projection*, exact only when pitch/roll are already ≈0. Prefer a proper
  yaw extraction — which `qYawOf` already is.)
- **It is exactly what `recenterLocalToHead` already computes** (`XRAnchor.cpp:1084-1123`) — `qYawOf`
  + floor projection.
- **It degrades to XREAL 3DoF by dropping the translation.** On a 3DoF rig there is no position at
  all, so the solve is yaw-only and the ritual is "face the desk and press the key." §5.4.

So the whole ladder — gold, silver, bronze — is **three ways of obtaining the same 4-tuple**, feeding
one shared applicator. That is the single most important structural decision in this report.

### 2.3 Amendment C — "location" and "grid" are the same object, and should not be built twice

`LAYOUT-AND-NAMING.md` already identifies the missing primitive: *"the one missing primitive is a
shared parent transform / named layout object. `CXRAnchor` is per-layer today; there is no object
that solves an origin once and composes children."* That is `CXRGrid` (report 03, WP-G1/G2,
unbuilt).

A location is **a named parent transform with an extent and a persistence key**. A grid is **a named
parent transform with a cell geometry**. They differ only in what they add on top. Building
locations as `CXRGrid`-shaped from day one (a `parentWorld` pose resolved once per frame, children
composing against it via a new anchor mode) means grids get their runtime for free later — and
building them *differently* means the codebase ends up with two parent-transform mechanisms.

**Recommendation: implement the location frame as the two-phase parent-transform solve that report 03
WP-G2 specifies, and let grids land later as "a location with cells."** This is the one place where
this report should deliberately expand its scope, because the alternative is rework.

---

## 3. The relocalization ladder — ground truth

Three rungs, in descending order of magic and ascending order of "we can build this next week."

### 3.1 GOLD — a runtime-maintained persistent frame

**Verdict: nothing is plumbed, but the client is greenfield, the manifest lever is already proven on
this exact hardware, and — the key finding — the right target is *not* the anchor API. It is
`XR_EXT_stationary_reference_space`, a Khronos extension written for precisely this problem, whose
Meta experimental predecessor ships today.**

#### What exists

| Layer | Anchor support | Evidence |
|---|---|---|
| OpenXR headers we compile against | **All of it** | `wivrn-fetchcontent/openxr_loader-src/include/openxr/openxr.h` is `XR_CURRENT_API_VERSION 1.1.58` and defines `XR_FB_spatial_entity` (`:4241`), `_query` (`:5793`), `_storage` (`:5893`), `_container` (`:6201`), `XR_FB_scene_capture` (`:6171`), `XR_META_spatial_entity_discovery/_persistence/_mesh` (`:7083/:7199/:7333`), `XR_MSFT_spatial_anchor` (`:2580`) + `_persistence` (`:5616`), and the cross-vendor `XR_EXT_spatial_entity` (`:11845`) / `XR_EXT_spatial_anchor` (`:12368`) / `XR_EXT_spatial_persistence` (`:12410`). **Zero header work required.** |
| WiVRn **client** (the OpenXR app on the Quest) | **None requested** | The one and only requested-extension vector is `client/application.cpp:1231-1272`. Required: `XR_KHR_convert_timespec_time`, alone. Optional (~30): KHR composition/locate_spaces/maintenance1/visibility_mask, EXT eye-gaze/hand/palm/perf/user_presence, FB hand-mesh/body/depth-test/layer-settings/refresh-rate/face2/passthrough/swapchain-update, HTC passthrough/facial, META body-tracking/local-dimming, ANDROID face, BD body. `grep -rIn -i "anchor" client common server` → **0 hits**. `grep -rIn -i "spatial"` → **1 hit**, `.direct_spatial_mv_pred_flag` in the H.264 encoder. |
| WiVRn **wire protocol** | **None** | `common/wivrn_packets.h`: `from_headset::packets` (`:568-596`) and `to_headset::packets` (`:809-828`) — no anchor, no space identity, no boundary. |
| WiVRn **server** / monado runtime | **None** | The generated `oxr_extension_support.h` has **0** hits for `spatial`/`anchor`. Independently confirmed on HypXRland's own vendored monado: `oxr_extension_support.py` enumerates 82 extensions and not one is an anchor/entity/scene extension. Nearest neighbours present: `XR_EXT_plane_detection`, `XR_MSFT_unbounded_reference_space`, `XR_MNDX_xdev_space`. |
| `AndroidManifest.xml` | **No anchor permission** | Present: OPENXR, OPENXR_SYSTEM, RECORD_AUDIO, INTERNET, WIFI/NETWORK state, WAKE_LOCK, hand/eye/face/body tracking (oculus + picovr + magicleap + android), RENDER_MODEL. **Absent:** `com.oculus.permission.USE_ANCHOR_API`, `USE_SCENE`, `ACCESS_SPATIAL_DATA`, `horizonos.permission.*`, any `SPATIAL_ANCHOR` feature. |

**The structural consequence, stated plainly:** HypXRland is an OpenXR app on the *monado* runtime.
Monado implements no anchor extension. Therefore **the compositor can never create or resolve a
persistent anchor itself.** The anchor must live in the *client*, which is an OpenXR app on the
*Meta* runtime, and its resolved pose must cross the WiVRn wire. This is not a limitation we can
patch around on the Linux side — it is the shape of the system.

#### The manifest lever is already proven

This is the finding that turns gold from "blocked on Meta" into "an APK we already build":

- `~/code/wivrn` branch `guardian-research` (commit `ddb5a8ce`) adds
  `<uses-feature android:name="com.oculus.feature.BOUNDARYLESS_APP" android:required="false"/>`.
- `~/code/wivrn-wt-client-mic` (commit `044fea16`, marked "TEST ONLY") adds three more:
  ```xml
  <uses-feature   android:name="com.oculus.experimental.enabled" android:required="true" />
  <uses-permission android:name="com.oculus.permission.BOUNDARY_VISIBILITY" />
  <uses-permission android:name="com.oculus.permission.ACCESS_TRACKING_ENV" />
  ```
  Its commit message records the result: *"Copied from the only Meta OpenXR SDK sample that calls
  `xrRequestBoundaryVisibilityMETA`… **Confirmed enumerated=true on retail Horizon OS v206 with
  device experimental mode off.**"*

So we have **already demonstrated, on this headset, that adding the right manifest entries makes a
Meta preview-tree extension enumerate on a retail runtime for a sideloaded, self-signed APK.** The
anchor extensions are gated by the same mechanism (`USE_ANCHOR_API` / `USE_SCENE`). The
`guardian-runtime` branch even carries the template for the rest of the work: a hand-written
`common/xr/meta_boundary_visibility.h` (57 lines of extension prototypes, because the extension is
not in the public headers) plus a `client/xr/system.cpp` capability probe. For anchors we do not even
need the hand-written header — the symbols are in the 1.1.58 loader headers already.

#### 3.1.1 The right target: `XR_EXT_stationary_reference_space`

This is the single most useful thing the external research turned up, and it reframes the whole gold
rung.

**`XR_EXT_stationary_reference_space`** — extension number 743, revision 1, landed in **OpenXR
1.1.59 on 2026-04-30**. Contributors span Microsoft, **Meta**, Valve, NVIDIA and Google. Its stated
motivations read like this report's requirements document, verbatim:

> Use a reference space that is related to the physical world and is **unaffected by the 'recenter'
> operation or user-defined room boundary**. Use a reference space that can **regain its location in
> the physical world after tracking is lost, after the headset is removed and put back on**, or after
> the device is suspended and resumed. Use a reference space that **persists its origin location in
> the physical world across app restarts or device reboots**.

The normative guarantees that matter here:

- `XR_REFERENCE_SPACE_TYPE_STATIONARY_EXT` is **gravity-aligned, +Y up**. Forward direction and Y
  height are explicitly **undefined** — you must not assume floor level or facing. (That is fine: our
  registration ritual supplies exactly the missing yaw and height, §2.2.)
- The runtime **must not** relate a system-level recenter to STATIONARY, and **must not** raise
  `XrEventDataReferenceSpaceChangePending` for STATIONARY merely because the user recentered.
- The origin **must not move only because the user redefines the room boundary**.
- Small origin adjustments while walking are permitted *without* an event; the runtime "must maintain
  an accurate location of the space origin when the user is returning near the space origin."
- On **relocalization failure** (different room, radically changed lighting) it raises a
  change-pending event with `poseValid = XR_FALSE` **and a new `generationId`**, retrievable via
  `xrGetStationaryReferenceSpaceGenerationIdEXT`. **That UUID is exactly the "is my saved layout
  still valid?" primitive** this design needs for §3.2's epoch check — a first-class, spec-mandated
  version of the `sessionEpoch`/`recenterEpoch` hack.

**Meta ships the experimental predecessor today.** `XR_EXTX2_stationary_reference_space` (spec
version 1, `XR_REFERENCE_SPACE_TYPE_STATIONARY_EXTX2 = 1000742000`,
`xrGetStationaryReferenceSpaceIdEXTX2` returning an `XrUuid` generation id) lives in
`meta-quest/Meta-OpenXR-SDK` under `OpenXR/meta_openxr_preview/extx2_stationary_reference_space.h`,
present since **SDK v77 (2025-06-06)** and still in **v85 (2026-02-12)**. Godot's OpenXR vendors
plugin implemented it (PR #418, **merged 2026-01-27**) and documents the gate:
`<uses-feature android:name="com.oculus.experimental.enabled" android:required="true"/>` in the
manifest, plus `adb shell setprop debug.oculus.experimentalEnabled 1` on the device.

**That gate is the one we have already cleared.** Our `wivrn-wt-client-mic` commit `044fea16` added
exactly `com.oculus.experimental.enabled` (plus two `com.oculus.permission.*` entries) and recorded
*"Confirmed enumerated=true on retail Horizon OS v206 with device experimental mode off."*

Why this beats the anchor API for our purposes, point by point:

| | Stationary reference space | Persistent spatial anchors |
|---|---|---|
| Client code | Create one more reference space; locate poses in it; read a generation UUID | Full async create/save/discover/retrieve state machine over `XrAsyncRequestIdFB` + 4–5 event types |
| Permissions | `com.oculus.experimental.enabled` only (proven on our APK) | `com.oculus.permission.USE_ANCHOR_API`, plus **the untrusted-package gate** below |
| Survives recenter | **Spec-guaranteed** | Yes |
| Survives boundary redraw | **Spec-guaranteed** | **No** — clearing space history destroys anchors |
| Survives reboot | **Spec-guaranteed** | Yes |
| Tells you when it failed | **`generationId` UUID** | Only via error codes at query time |
| Wire cost | Poses already cross; add a space selector + a generation-id field | New packet type + UUID lifecycle both directions |
| Upstreamability to WiVRn | **Good** — a standards-track Khronos reference space is exactly the kind of thing the maintainers say vendors should provide | **Poor** — see the maintainer stance in §3.2 |

**The untrusted-package gate is a real risk for the anchor path specifically.** Meta's own docs state
that for spatial-data APIs, *"For Developers Only, the logged in Meta account must be registered with
a developer team. This will allow untrusted applications to use spatial data… Applications downloaded
from the Store or App Lab do not have this restriction."* WiVRn is distributed as a sideloaded APK
and we build our own, so it is an untrusted package. Community reports of `ERROR_PACKAGE_UNTRUSTED`
on anchor save/load for sideloaded apps are consistent with this (unverified — Meta's forums refused
automated fetching). The stationary reference space has no such spatial-data permission and should
not hit that gate; WP-G1 measures it.

#### 3.1.2 The anchor API, if we want it anyway

For completeness, because it is the fallback if stationary space does not enumerate, and because the
room-model use cases (plane detection, scene understanding) need it eventually. **The current Meta
recommendation as of SDK v85 is not the FB storage extensions:**

| Extension | Role | Status |
|---|---|---|
| `XR_FB_spatial_entity` | foundational entity/component model; `xrCreateSpatialAnchorFB` | current |
| `XR_META_spatial_entity_persistence` | **save/erase** (`xrSaveSpacesMETA` / `xrEraseSpacesMETA`) | current |
| `XR_META_spatial_entity_discovery` | **load** (`xrDiscoverSpacesMETA`) | current |
| `XR_FB_spatial_entity_storage` / `_storage_batch` | old save path | **explicitly obsoleted by Meta** |
| `XR_FB_spatial_entity_query` | old load path | superseded in practice by `xrDiscoverSpacesMETA` |
| `XR_EXT_spatial_entity` / `_anchor` / `_persistence` | ratified cross-vendor family (OpenXR 1.1.49, 2025-06-10) | **implemented by Android XR and PICO; no evidence Horizon OS exposes it** |

Note two corrections to common assumptions: the FB→**META** migration is real and vendor-stated,
while the FB→**EXT** migration has *not* happened on Horizon OS; and the registry name is
`XR_EXT_spatial_entity`, singular.

Flow, for the record: `xrCreateSpatialAnchorFB` → `XrEventDataSpatialAnchorCreateCompleteFB{uuid}` →
`xrSetSpaceComponentStatusFB(LOCATABLE|STORABLE)` → `xrSaveSpacesMETA` → *you* persist the
`XrUuidEXT` yourself (Meta stores the anchor, not your anchor→content association) → next session
`xrDiscoverSpacesMETA` → `XrEventDataSpaceDiscoveryResultsAvailableMETA` →
`xrRetrieveSpaceDiscoveryResultsMETA` → `xrLocateSpace`.

Practical constraints that shape the design if we go this route:
- **Per-app and private.** Meta: anchors are "created and owned by the application, remaining private
  within its context." Only *scene* anchors are system-wide.
- **Meta's own accuracy guidance is a 3-metre rule**: "Create or reuse a spatial anchor within three
  meters of the object you want to anchor… Drift occurs when the user moves away from the anchor."
  ⇒ **one anchor per location, not one anchor for the house** — which is exactly the §2.1 amendment,
  independently arrived at.
- **A space-history clear destroys them**, and Meta's own remedy for MR drift is to clear space
  history and "replace previously placed objects." So the re-registration path must exist regardless.
- Failure codes worth handling: `XR_ERROR_SPACE_INSUFFICIENT_VIEW_META`, `_TOO_DARK_META`,
  `_TOO_BRIGHT_META`, `_PERMISSION_INSUFFICIENT_META`, `_RATE_LIMITED_META`,
  `_STORAGE_AT_CAPACITY_META`.

#### 3.1.3 What the gold rung actually requires

Four pieces, none individually large, and materially smaller for the stationary-space route:

1. **Manifest** (XS): `com.oculus.experimental.enabled` — already proven. (Plus
   `com.oculus.permission.USE_ANCHOR_API` only on the anchor route.)
2. **Client** (S for stationary space; M/L for anchors): probe and create
   `XR_REFERENCE_SPACE_TYPE_STATIONARY_EXTX2` (falling back to `_EXT` when Meta promotes it), locate
   the streamed poses' origin in it, and read the generation UUID. The `guardian-runtime` branch is
   the template — it already carries a hand-written extension header
   (`common/xr/meta_boundary_visibility.h`, 57 lines) plus a `client/xr/system.cpp` capability probe
   for exactly this kind of preview extension.
3. **Wire** (S): the client already streams every pose in a chosen space
   (`application.cpp:1351`, hardcoded STAGE). Add a per-session space selector plus a generation-UUID
   field, or a small `from_headset::stationary_frame{XrPosef, uuid[16]}` packet. **The wire is nearly
   free to extend**: serialization is compile-time reflection over aggregates via Boost.PFR
   (`common/wivrn_serialization.h:534-544`), variant dispatch is a `uint8_t` index tag (`:807-865` —
   so **append only**), and the protocol version is an **automatically computed FNV-1a hash over the
   whole type tree** (`common/protocol_version.h:7-9`), so adding a struct bumps the handshake by
   itself; `protocol_revision` (`wivrn_packets.h:42`) is only for semantic changes. The one manual
   step is an `operator()` overload in `server/driver/wivrn_session.h`, which the code comments demand
   at `wivrn_packets.h:567`. Note this *is* a protocol-breaking change (the hash changes), so client
   and server must be updated together — which for us they always are.
4. **Server/compositor** (S): treat the stationary frame as the house frame — either rebase in the
   compositor (§4.1) or set the LOCAL offset through libmonado (§4.2).

**The precedent for all four already exists in one commit-shaped example** — `from_headset::tracking`'s
`state_flags::recentered` bit (§4.2) is literally "client observes a runtime space fact → one field on
the wire → server mutates monado's space graph → compositor sees a reference-space change."

### 3.2 SILVER — boundary / space identity, and reuse-the-old-solve

**Verdict: essentially nothing usable crosses the wire today, and the one signal that does is
actively harmful in its current form. Silver is worth building not as a *localization* mechanism but
as a *validity* mechanism for a bronze/gold solve.**

#### What crosses the wire about space

Exactly one thing: a single bit.

```cpp
// common/wivrn_packets.h:301-304
enum state_flags : uint8_t { recentered = 1 << 0, };
```

carried in `from_headset::tracking::state_flags` (`:329`). That is the entire vocabulary. There is no
space identifier, no guardian geometry, no play-area extents, no room UUID.

#### Boundary is doubly unavailable

- **Client side:** never calls `xrGetReferenceSpaceBoundsRect`, never uses `XR_FB_boundary`/
  `XR_FB_scene`/`XR_FB_scene_capture`. Zero hits. And of course the user runs the **boundaryless**
  build, which is the whole point of the `guardian-*` branch line — there is deliberately no
  guardian to identify a room by.
- **Server side:** `xrGetReferenceSpaceBoundsRect` *is* implemented in monado's state tracker
  (`oxr_api_space.c:156-177` → `oxr_space.c:196-221`) and forwards to
  `xrt_comp_get_reference_bounds_rect` — but **no WiVRn compositor ever assigns that function
  pointer**, so `xrt_compositor.h:1926` returns `XRT_ERROR_NOT_IMPLEMENTED`, which
  `oxr_space.c:213-216` maps to `XR_SPACE_BOUNDS_UNAVAILABLE` with zeroed extents. **Apps on the
  WiVRn runtime always see "no play area."**

So "same room as last session?" cannot be answered from boundary geometry, today or after any small
patch. A room-identity signal has to come from the gold layer — a resolved anchor UUID, or better a
stationary-space `generationId` (§3.1.1), *is* a room identity.

#### The STAGE trap (and a genuine silent-corruption bug)

There is one more space fact on the wire and it deserves its own heading, because it is both an
opportunity and an active bug.

**The client streams every pose in STAGE**, hardcoded, with no fallback — it throws if STAGE is
unavailable (`client/application.cpp:1351`). STAGE on a Quest is tied to the Guardian origin, which
is the closest thing the platform has to a room-fixed frame; that is exactly why ALVR's
`RecenteringMode::Stage` is its only mode that persists across sessions. And **HypXRland never
creates a STAGE reference space at all** (`XRSession.cpp:252-279` creates LOCAL_FLOOR/LOCAL and VIEW;
`grep -i STAGE src/openxr/` finds nothing). Whether monado's server-side STAGE, fed by WiVRn's
STAGE-space poses, is actually stable across sessions on a **boundaryless** headset is an open,
*cheaply measurable* question — WP-B0 in §9.

Two things poison the well today, though:

1. **A STAGE change is swallowed.** The client latches its recenter bit only for
   `XR_REFERENCE_SPACE_TYPE_LOCAL` (`client/scenes/stream.cpp:1298-1302`), and its main event switch
   has an empty `break` for the STAGE case. So when the Quest raises
   `REFERENCE_SPACE_CHANGE_PENDING` for **STAGE** — a guardian redraw, a room re-scan, a space
   re-setup — **the whole streamed world shifts and the server is never told.** There is no protocol
   field that could even carry `poseInPreviousSpace`. This is a silent-corruption bug, it is
   unquestionably ours to fix in a client patch, and it is the single most likely explanation for
   "my monitors moved and I have no idea why."
2. **The OpenXR 1.1 request changed STAGE's meaning mid-history.** WiVRn commit `2ba3d78` "Try to
   request OpenXR 1.1" (v25.8, 2025-07-02) crossed Meta's threshold where "apps building against
   OpenXR 1.0.24 or higher can enable STAGE space when a stationary Guardian is configured." Before
   it, the Quest handed WiVRn a *fake* STAGE that was really LOCAL, so system recenter appeared to
   move everything; after it, STAGE became real and immovable, which users read as "recenter broke"
   (upstream issues #458, #474, #561, #806). The shipped workaround is a **monado env var that is
   undocumented in WiVRn**: `OXR_RECENTER_STAGE=1` (monado MR !2588, merged 2025-10-01) makes monado
   report STAGE as unsupported and hand apps LOCAL_FLOOR instead. The name lies — it does not
   recenter the stage.

#### Upstream stance (WiVRn) — matters for what we can push back

The maintainers' position on runtime-side spatial workarounds is explicit and settled. xytovl
(2024-08-23, issue #112) on setting the floor: *"I don't think setting the floor is something we
should be doing. **It's for vendors to fix their software.**"* galister (2025-10-07): *"we shouldn't
need to implement workarounds for something that the headset vendor is supposed to add to their
runtime."* ImSapphire (2026-02-25, #806): *"We recentre LOCAL spaces when requested by the client,
STAGE is not recentred by us."* Across WiVRn's entire history there is **not one** issue, PR or
discussion about spatial anchors, anchor persistence, scene/room models, or colocation.

**Read for us:** a PR that plumbs Meta's proprietary anchor API through WiVRn is against the grain. A
PR that adopts a **ratified Khronos reference space** (`XR_EXT_stationary_reference_space`) is
squarely *with* it — it is the vendor fixing their software, and WiVRn merely exposing it. That is a
second, independent argument for §3.1.1's target. The STAGE-swallow fix (1 above) should be
uncontroversial on its own merits.

#### What silver *is* good for

Reuse-the-last-solve. Concretely: persist the last successful `session_origin → location` transform
along with the evidence that produced it, and on the next don **assume it is still valid** unless
something invalidates it. Under WiVRn this is much stronger than it sounds, because of an
architectural quirk:

> WiVRn keeps the session alive across doffs. The LOCAL_FLOOR origin is established once per
> *session*, not per don. So for a doff→don inside one session, the old solve is not merely a good
> guess — it is **exactly correct**, and today we throw it away by not re-posing at all.

The invalidators are enumerable:

| Event | Detectable? | Effect on a stored solve |
|---|---|---|
| New WiVRn session (client reconnect, server restart) | Yes — `start()`/session begin | **Invalid.** New arbitrary origin. |
| Quest system recenter (long-press Meta) | **Yes** — full chain, §4.2 | **Invalid** by exactly the recenter delta (which we are not told, §4.3). |
| Tracking loss + relocalization on the Quest | Not signalled | Silently invalid. Manifests as everything having shifted. |
| Doff → don within a session | Yes (presence/visibility) | **Still valid** (see above). |
| Machine changed (laptop vs desktop) | Yes | Invalid — different session origin entirely (§8). |
| Physical furniture moved | No | The stored solve is right; reality moved. User re-registers. |

**Design consequence:** a stored solve should carry a `sessionEpoch` (a monotonic counter bumped in
`start()`) and a `recenterEpoch` (bumped on every `onReferenceSpaceChanged`). Reuse it when both
match; otherwise fall to the next rung. That is cheap, honest, and makes the doff→don case — which
is the *most common* one and currently the *worst* one — free and instant.

### 3.3 BRONZE — the registration ritual

**Verdict: build this first. It works on every runtime, needs zero WiVRn changes, and it is the only
rung that can ship without touching an APK.**

The ritual is: **be at the place, then tell the compositor this is that place.** One head-pose sample
at a known spot yields the full 4-tuple (§2.2).

Three input modalities, all already available:

1. **Keybind (v1, recommended).** `bind = SUPER SHIFT, D, xrlocation, register desk` — sit at the
   desk, face the desk's forward direction, press. Reads the live head pose on the frame thread
   exactly as `recenterLocalToHead` does. **This is ~2 seconds and zero new subsystems.**
2. **Voice, at word time.** `hypxrvoice` already resolves deixis at the *word* timestamp via
   `hyprctl -j openxr gaze at <ms>` against a rolling **8192-sample, ~91-second, 90 Hz pose ring**
   (`docs/openxr/05-configuration.md` IPC section) that returns `head.pos`, `head.quat`,
   `head.forward`, plus a `gaze.hitPoint`/`hitDistM` when the gaze ray strikes a monitor. So *"this
   is my desk"* is a new intent verb on an existing, validated substrate — no compositor work beyond
   an `xrlocation register <name> at <pose>` verb that accepts an explicit pose (which the existing
   `openxr place <name> at x,y,z` already precedents). The 200–600 ms gaze-leads-speech correction
   and the stability-window vote are already implemented in the daemon.
3. **Look-at-a-physical-marker.** Rejected for v1, but *not* permanently. We cannot do it ourselves —
   Quest passthrough cameras are not exposed to apps on Horizon OS without an enterprise entitlement,
   so we would need a hand-placed virtual marker, which is strictly worse than just using the head
   pose. But the ratified **`XR_EXT_spatial_marker_tracking`** (ext #744, OpenXR 1.1.49) defines
   runtime-side tracking of QR / Micro-QR / ArUco / AprilTag markers. If Horizon OS ever exposes it,
   a printed tag taped to the desk becomes a zero-ritual, zero-drift registration source that beats
   every other rung — a 2-D marker gives position *and* yaw in one observation, with no
   zero-redundancy problem. Worth re-checking whenever WP-G1 is re-run.

#### Error characteristics

The ritual's error is dominated by **how repeatably you can put your head in the same place and
orientation**, not by tracking noise:

- **Yaw is the sensitive term.** An error of `Δyaw` at radius `r` displaces a monitor by `r·Δyaw`.
  At the user's typical 1.4–2.2 m distances, **1° ≈ 2.5–4 cm** — fine. **10° ≈ 25–38 cm** — very
  noticeable. Seated-at-a-desk head yaw is repeatable to a few degrees; standing in a doorway is not.
- **Position error translates 1:1** and is far more forgiving — 5 cm of seat error is 5 cm of monitor
  error regardless of distance.
- **Therefore: register from a seated, well-defined posture, and prefer *facing a physical
  reference*** (the desk edge, the monitor stand, the window) over "wherever I happen to be looking."
  A `register` verb that also captures gaze-`hitPoint` could later refine yaw by pointing at a fixed
  real feature — noted as a v2 refinement, not v1.
- **Quest 3's tracking is not the problem.** A robot-arm study against a TECHMAN TM5-900 (±0.05 mm
  reference) measured **mean translational RMSE 0.346 mm** (3D RMSE 0.621 mm) and **rotational RMSE
  0.143°** over 2848 samples (*Sensors* 26(8):2285, April 2026). That is sub-millimetre — three
  orders of magnitude below the ritual's own repeatability. **Caveat on scope:** those are 200 mm
  single-axis motions over 2.5 minutes, so the number bounds *jitter and short-range relative error*
  and says nothing about session-scale absolute origin drift, for which **no published Quest 3 figure
  exists**. Empirically it is small; across a relocalization event it is unbounded.
- **The real enemy is the discrete jump, not the slow drift** — a tracking loss and re-localization,
  a doff/don wake, a recenter. The ritual costs nothing to repeat, which is the right answer:
  `xrlocation register desk` again. Design for re-registration as a first-class path (§6.4), because
  even Meta's own first-party anchors die on a space-history clear and Meta's documented remedy for
  MR drift is "clear space history, then replace previously placed objects."
- **Zero redundancy means cheap hardening is worth it** (§2.2): average the pose over a short dwell
  rather than sampling one frame. We already have `dwellStep` and a 90 Hz ring; this is a handful of
  lines and it removes the single-sample-noise objection entirely.

#### Why bronze is not a consolation prize

The ritual and the gold rung produce **the same 4-tuple into the same applicator**. Gold's advantage
is solely that it produces it *automatically*. Every line of §5 and §6 — the location model, the
state file, migration, `xrrule`, the don-edge flow — is rung-independent. Bronze is the *whole
feature* minus the automation.

### 3.4 Ladder summary

| Rung | Source of the 4-tuple | Requires | Automatic? | Effort | Risk |
|---|---|---|---|---|---|
| **Bronze** | User's head pose at a `register` verb | Compositor only | No (~2 s per new place) | **S–M** | **Low** — pure math, headless-testable, no external dependency |
| **Silver** | Last session's stored solve + epoch validity | Compositor only | Yes, when valid | **S** | Low — but only covers the in-session doff case, and wants §4.3 |
| **Silver+** | Anchor the frame to the runtime's **STAGE** (Guardian origin) | Compositor: create a STAGE space; measure whether it is stable on a boundaryless Quest | Yes, when STAGE is real | **S** to measure | **Medium/unknown** — this is what ALVR's `RecenteringMode::Stage` does; breaks on guardian redraw, which we are not told about (§3.2) |
| **Gold (stationary space)** | `XR_EXT(X2)_stationary_reference_space` located client-side, `generationId` as the validity key | Manifest flag + client patch + one wire field | **Yes** | **M** | Medium — needs the extension to enumerate (WP-G1 measures); protocol-breaking but we own both ends |
| **Gold (anchors)** | Meta persistent anchor resolved client-side | Manifest + `USE_ANCHOR_API` + dev-team account + async anchor lifecycle + wire | Yes | **L** | **Higher** — untrusted-package gate, dies on space-history clear, against WiVRn's grain |

**They all feed one applicator.** Build the applicator once.

---

## 4. Where the solve gets applied — the insertion points

Given a 4-tuple `T = session_origin → location`, someone has to apply it. There are exactly three
places, and the choice matters more than it looks.

### 4.1 Compositor-side: rebase every anchor (recommended for v1)

HypXRland stores each monitor's pose in the location's frame and composes:
`world = poseCompose(locationWorldPose, monitorLocalPose)`, where `locationWorldPose` is `inv(T)`.

- **Pros:** entirely ours; no WiVRn changes; works identically on monado-direct (XREAL) and WiVRn;
  reuses `poseCompose`/`poseInverse` (`XRMath.hpp:232/236`) and the rigid-group property that
  `recenterLocalToHead` already demonstrates; testable in gtests with zero runtime.
- **Cons:** only HypXRland's own quads are rebased — a VR app running beside us (overlay mode,
  `hypxrpaper`'s environment) still sits in the raw session frame. For a desktop compositor whose
  entire content is its own quads, this is close to irrelevant, but note it.
- **This is the `CXRGrid`-shaped parent transform of §2.3.** One `parentWorld` pose resolved once per
  frame, children composed against it.

### 4.2 Server-side: redefine LOCAL via libmonado — **available today, no patches**

Monado can be told to move LOCAL/LOCAL_FLOOR arbitrarily, out of process, **right now**, through a
supported public API. This is stronger than it first appears, and it is confirmed on this box.

**Verified live:**

```
$ cat /usr/share/openxr/1/openxr_wivrn.json
{ "runtime": { "name": "Monado",
               "library_path": "../../../lib/wivrn/libopenxr_wivrn.so",
               "MND_libmonado_path": "../../../lib/wivrn/libmonado_wivrn.so" } }

$ nm -D --defined-only /usr/lib/wivrn/libmonado_wivrn.so | grep -o 'mnd_root_[a-z_]*'
… mnd_root_create  mnd_root_get_reference_space_offset  mnd_root_set_reference_space_offset
   mnd_root_recenter_local_spaces  mnd_root_get_tracking_origin_offset
   mnd_root_set_tracking_origin_offset …
```

So: **the runtime manifest HypXRland already reads to find the runtime also tells it where to find a
library that can set the tracking origin.** `dlopen` it, `mnd_root_create()`, call
`mnd_root_set_reference_space_offset(root, LOCAL, pose)`. It is a separate Unix socket, independent of
our `XrSession`. This is not a hack: monado MR **!2284 "ipc, u/space_overseer: space offset api"**
(merged 2024-07-19) exists explicitly "to open the door for 3rd party apps to do space manipulation
and calibration," having superseded a narrower `apply_stage_offset` proposal; the API shipped publicly
in **monado 25.0.0 (2025-04-18)** at `MND_API_VERSION` 1.3+, and the `MND_libmonado_path` manifest key
is the sanctioned discovery mechanism.

**There is direct prior art**: galister's **`motoc`** (github.com/galister/motoc) uses exactly this
API to calibrate and *persist tracking origins across sessions* — i.e. someone has already built the
neighbouring half of this feature against this interface.

The underlying primitives:

- `u_space_overseer.c:794 recenter_local_spaces()` — recenters to the current view: takes the
  view-in-parent relation, **zeroes the quaternion's x/z** ("Only save the rotation around y axis",
  `:846-851`), takes only `position.x`/`position.z` (keeping `y`), writes both LOCAL and LOCAL_FLOOR
  offsets, then pushes `XRT_SESSION_EVENT_REFERENCE_SPACE_CHANGE_PENDING` for both.
- `u_space_overseer.c:1027 set_reference_space_offset(xso, type, offset)` — an **arbitrary** offset
  for LOCAL or STAGE (it refuses LOCAL_FLOOR directly, but a LOCAL offset drives LOCAL_FLOOR's yaw
  and x/z, `:1053-1066`). This is precisely a 4-DoF setter.
- Both are exposed over monado's **IPC** (`ipc_client_space_overseer.c:282/313`,
  `ipc_server_handler.c:998/1039`, proto `50-space.json:88`) and via **libmonado**
  (`mnd_root_recenter_local_spaces`, `targets/libmonado/monado.c:590`). `monado-ctl -c` triggers a
  recenter out of process (`targets/ctl/main.c:194/344/407`), and `/usr/bin/monado-ctl` is installed
  on this box.
- The guard flag `can_do_local_spaces_recenter` (`u_space_overseer.c:123`) appears **never assigned**
  — it is zero-initialized, so both guards pass.

And the WiVRn server already uses this path. The complete, working recenter chain:

```
Quest long-press Meta
  → Meta runtime: XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING (LOCAL)
  → client/scenes/stream.cpp:1298-1302     recenter_requested = true
  → client/scenes/stream_tracking.cpp:456  tracking.state_flags |= recentered
  → server/driver/wivrn_session.cpp:605-611  xrt_space_overseer_recenter_local_spaces(space_overseer)
  → u_space_overseer.c:868-890             LOCAL + LOCAL_FLOOR offsets updated, ref-change events pushed
  → oxr_session.c → oxr_event.c            XrEventDataReferenceSpaceChangePending to HypXRland
  → XRSession.cpp:399-411                  m_recenterPending / m_recenterPose
  → OpenXRManager.cpp:1292-1302            CXRAnchor::onReferenceSpaceChanged on every anchor
```

**Applying the house solve here would mean:** every OpenXR app on the runtime lands in the house
frame, including overlay VR apps and `hypxrpaper`; `anchor:local` poses become house poses with *no
compositor change at all*; and the existing `onReferenceSpaceChanged` handling absorbs the change
without a pop. It is architecturally the most elegant answer.

- **Pros:** universal; **no patches to anything** (the API is public, shipped, and discoverable from
  the manifest); zero new pose math in the compositor; the event path already exists and is already
  correct; proven by `motoc`.
- **Cons and gotchas:**
  - **It makes the house frame a *global singleton*.** There is exactly one LOCAL offset. That is
    fatal for the location model the moment you want two locations expressed at once (the kitchen
    monitor shown at a distance while you sit at the desk — §5.5's `distant` policy), and it means a
    location switch is a runtime-wide event rather than a compositor-internal one.
  - `set_reference_space_offset(LOCAL_FLOOR, …)` is **always rejected** (`XRT_ERROR_UNSUPPORTED_SPACE_TYPE`)
    — LOCAL_FLOOR is derived from LOCAL and STAGE. STAGE is rejected whenever a driver owns the stage
    pose. **LOCAL is the only reliable knob.**
  - **Monado rebuilds LOCAL at a hardcoded stage + 1.6 m on every service start**, so any stored
    offset must be re-applied at every session — it is not persisted anywhere by monado
    (`~/.config/monado/config_v0.json` stores camera calibration and tracking overrides only).
  - It couples us to a runtime we do not always use (the XREAL path is monado-direct, which *would*
    also work, but a future SteamVR backend would not).
  - `can_do_local_spaces_recenter` (`u_space_overseer.c:123`, upstream now `b_space_overseer.c`) is
    **never assigned** anywhere, so it is permanently false and both guards fall through — meaning
    both ops succeed today. The polarity reads inverted; an upstream "fix" could break this. **Do not
    depend on the flag's name.**
- **Verdict: do not use it for the *location* frame** — §4.1 is the right home for that, because
  locations are plural and the LOCAL offset is singular. **But it is the right delivery mechanism for
  a single house-frame solve** if we ever want overlay VR apps and `hypxrpaper` to share it, and it is
  a genuinely free experiment (a dozen lines of `dlopen` + one call) that would answer "does moving
  LOCAL do what we think?" in an afternoon. Note it as WP-B0's sibling.

### 4.3 A latent bug worth fixing regardless — invalid recenter poses

Look closely at what monado pushes:

```c
// u_space_overseer.c:873-876 (and again at :1071-1074 in set_reference_space_offset)
xse.ref_change.event_type            = XRT_SESSION_EVENT_REFERENCE_SPACE_CHANGE_PENDING;
xse.ref_change.pose_valid            = false;
xse.ref_change.pose_in_previous_space = (struct xrt_pose)XRT_POSE_IDENTITY;
```

`pose_valid = false`. And HypXRland's documented, deliberate behavior
(`docs/openxr/03-anchoring.md:466`) is: *"If the event's pose is invalid the stored pose is left
untouched."* — which is the correct conservative choice given no information.

**But the information exists and is simply not sent.** `recenter_local_spaces` computes the exact
delta (it is the `rel.pose` it just applied). The result today: **a Quest system recenter moves the
world under the monitors, and the monitors keep their old numeric coordinates, so they visibly
teleport relative to the room.** This is very likely a contributor to the "extremely non-ideal
positions" complaint, independent of everything else in this report — and it is the exact opposite of
what a user pressing "recenter" expects (they expect the *view* to recenter and the *content* to be
re-placed in front of them, or at minimum to stay put in the room).

**Fix:** populate `pose_in_previous_space` with the applied delta and set `pose_valid = true` — a
~20-line patch to `u_space_overseer.c`, in `patches/monado/` alongside the eight we already carry
(one of which, `0006-st-oxr-push-XrEventDataInteractionProfileChanged`, is precedent for injecting
OpenXR events from the WiVRn side). Upstreamable on its own merits. **File this as WP-S2 and do it
early — it is cheap and it may be a large fraction of the felt problem.**

---

### 4.4 Insertion-point comparison

| # | Where the 4-tuple is applied | Effort | Risk | Covers non-HypXRland content | Multiple frames at once | Verdict |
|---|---|---|---|---|---|---|
| **4.1** | **Compositor: `world = poseCompose(locationWorld, poseInLocation)`** | **M** (a parent-transform solve we want anyway for grids) | **Low** — pure pose math, gtest-able, no cross-project dependency | No | **Yes** | **Recommended for the location frame** |
| 4.2 | Server: `mnd_root_set_reference_space_offset(LOCAL, T)` via libmonado — **no patches, shipped API, confirmed present on this box** | **S** (`dlopen` + one call) | Medium — LOCAL rebuilt at stage+1.6 m every service start so it must be re-applied; LOCAL_FLOOR/STAGE setters rejected; runtime-coupled | **Yes** | **No** — one global origin | Reject for *locations*; **the right answer for a single global house frame**, and a cheap experiment |
| 4.3 | Fix monado's invalid recenter pose so the *existing* path stops teleporting monitors | **S** (~20 lines) | **Low** — upstreamable, no new concepts | Yes | n/a | **Do it now, independently** |

---

## 5. The location model

### 5.1 What a location is

```
location := { name, pose (in the house frame, optional), extent, solve, layout }
```

- **`name`** — user-chosen: `desk`, `couch`, `kitchen`, `standing-desk`.
- **`pose`** — 4-DoF in the house frame. Optional (§2.1): present only when co-registered with
  another location in one session.
- **`extent`** — a horizontal radius (v1) around the location origin, in meters. This is the
  migration geofence and the "am I here?" test. A radius, not a polygon: adaptive anchoring's geofence
  is already XZ-radius-based with two-radius hysteresis (`XRAnchor.cpp:489-513`), and reusing that
  shape means reusing that (gtested) code. A polygon/box extent is a later refinement.
- **`solve`** — the last known `session_origin → location` 4-tuple, plus `{sessionEpoch,
  recenterEpoch, method: bronze|silver|gold, anchorUuid?, timestamp}`. This is the silver-rung state.
- **`layout`** — the set of `{monitorName → pose-in-this-location's-frame}` bindings.

### 5.2 Config-declared vs runtime-named: **both, in different files**

This is the design's most consequential storage question, and the answer follows from an existing
split the codebase already makes.

`CXRMonitorLayer` holds **two** anchor states (`XRMonitorLayer.hpp:91-93`):

```cpp
OpenXR::CXRAnchor       m_anchor;          // live, spring-mutating engine
OpenXR::SXRAnchorState  m_declaredAnchor;  // last state DECLARED by the config keyword
```

`m_declaredAnchor` is the reconcile baseline; `m_anchor` is the truth on screen. **Locations should
mirror exactly this split.**

| | Config (`~/.config/hypr/*.conf`) | State file (`$XDG_STATE_HOME/hypr/xr-locations.json`) |
|---|---|---|
| **Declares** | that a location *exists*, its extent, its default layout, and any `xrrule`s that reference it | the *measured* `solve`, the anchor UUID, the live layout as last arranged |
| **Written by** | the user, by hand | the compositor, automatically |
| **Survives** | forever, in git (the user's dotfiles are versioned) | until the furniture moves |
| **Keyword** | `xrlocation = desk, extent:2.0` | — |
| **Analogy** | `m_declaredAnchor` | `m_anchor` |

Rationale for splitting rather than picking one:

- **The measured solve must not live in config.** It changes every session, it is machine-specific,
  it is meaningless to a human, and writing it back into a hand-edited, git-tracked config file is
  exactly the "compositor rewrites your dotfiles" behavior Hyprland deliberately avoids. The stance is
  visible in the tree: the compositor's only file writes are the runtime lock
  (`src/Compositor.cpp:782-790`) and a *generated default* config when none exists
  (`src/config/legacy/ConfigManager.cpp:2267`) — it never rewrites a config the user owns. And
  `hyprctl openxr layout` deliberately **prints for the user to paste** rather than writing. That is
  the house stance and it should hold.
- **The declaration must not live only in state.** A location the user never declared cannot be
  referenced by an `xrrule` or a keybind at config-parse time, and a corrupt/deleted state file
  should not destroy the user's intent.
- **Runtime naming ("call this place the kitchen") creates a state-file-only location** with no
  config declaration. That is a supported, second-class state: it works, it persists, it just cannot
  be named by an `xrrule` until the user adds a declaration line. `hyprctl openxr layout` should grow
  a companion that prints the `xrlocation = …` lines to paste — same pattern, same ergonomics.

**Format:** JSON, one file, atomically replaced (write-temp + `rename`). Not the config's hyprlang
grammar — this is machine-written data with nested structure and it should look like it. Debounced
writes (a location's layout changes on every grab release; do not fsync per frame).

### 5.3 Per-location layouts, and the relation to grids

Each location owns `{monitorName → pose-in-location-frame}`. Consequences:

- **`hyprctl openxr layout` finally becomes durable.** For a location-bound monitor the serialized
  pose is location-relative, which means the same thing next session. Today's output is
  session-origin-relative and therefore disposable (§1). This alone justifies the model.
- **Roll is still dropped** by `serializeXRMonitorLine` (`XRMonitorConfig.cpp:588-595`, yaw/pitch
  degrees only). Fine for locations (a location frame is yaw-only by construction), but the *monitor*
  pose within a location can carry roll from a grab. Either extend the grammar with `roll:` or accept
  the existing documented loss — note it, do not silently change it.
- **A location is a grid parent transform with an extent** (§2.3). When grids land, `xrgrid` gains a
  `location:` binding and its origin composes with the location frame instead of LOCAL_FLOOR. Build
  the parent-transform solve once, in the location WP.

### 5.4 Degradation to XREAL 3DoF

On a 3DoF rig (XREAL Air 2 Ultra via monado's `xreal_air` driver, `research/XREAL-3DOF.md`) there is
no head *position* — only orientation. The location model still works, reduced:

- The solve is **yaw-only**; `x`, `y`, `z` are all zero because there is no positional frame to
  differ in. Registration is "face the desk's forward direction, press the key."
- **Extent/geofence is meaningless** (no position ⇒ no distance ⇒ adaptive anchoring's geofence
  already cannot function on 3DoF). Locations on a 3DoF rig are therefore **explicitly switched**,
  never auto-migrated: `xrlocation enter kitchen`. This is a *feature* — it is the honest behavior,
  and it matches what report XREAL-3DOF already concludes about 3DoF's limits.
- The stored solve's `y` should be treated as "unknown, use `openxr:floor_offset`", matching the
  existing LOCAL-fallback floor shim (`OpenXRManager.cpp:1616-1617`).

**Design rule:** the applicator takes a 4-tuple; a 3DoF source supplies `(0, 0, 0, yaw)`. No branching
in the applicator, one branch in the geofence.

### 5.5 Migration semantics

Migration = "the monitors come with me to the couch." Built on the *shipped* adaptive machinery
(`XRAnchor.cpp:483-570`), which already has: XZ geofence, two-radius hysteresis, directional dwell
(400 ms out / 800 ms in), the `m_adLeftSinceDock` latch so a manual undock at the desk stays roaming,
a phase machine with **bidirectional reverse-interrupt** that preserves progress (`m_adT = 1 - m_adT`),
and an eased pose blend (`envAdvance` + `easeApply` + `lerpPose`).

The **only** change migration needs is: *when a roaming monitor redocks, dock into the frame of the
location whose extent you are now inside, not the location you left.*

```
DOCKED@desk ──leave desk extent (dwell)──▶ UNDOCKING ──▶ ROAMING
                                                            │
                                    enter couch extent (dwell)
                                                            ▼
                                                       REDOCKING@couch ──▶ DOCKED@couch
```

Concretely, `adaptiveStep`'s redock target changes from "the saved desk pose" to "the saved pose for
this monitor **in the location I am entering**." Two sub-cases:

- **The entered location has a saved pose for this monitor** → redock to it. This is the good case
  and it is what makes "my editor is always at eye level whether I'm at the desk or the couch"
  work.
- **It does not** (this monitor has never been to the kitchen) → three policies, pick one as default:
  1. **Adopt** — dock at the current roam pose, expressed in the new location's frame, and save it.
     Self-teaching: the first time you carry a monitor to the couch, wherever you leave it becomes
     its couch pose. **Recommended default.**
  2. **Stay roaming** — never auto-dock into an unknown location; require an explicit `dock`.
     Predictable, but means new locations start empty and stay empty.
  3. **Mirror** — copy the pose from the previous location. Wrong in general, for exactly the reason
     report 13 §4.4 gives for not carrying the desk offset into roam: a desk-height pose at the couch
     is the wrong pose, and pretending otherwise is what the eased re-pose exists to avoid.

**Explicit "come with me."** The geofence handles the walking case, but the user also wants deliberate
summoning. Two verbs:

- `xrmonitor follow` / `xrmonitor undock` — already exist (`adaptiveForceUndock`); a monitor forced
  into roam will migrate on the next extent entry.
- `xrlocation summon <location> [<monitor>|all]` — bring a location's monitors *to me*, i.e. re-dock
  them into the location I am currently in. This is the "I want my desk setup on the couch"
  one-shot, and it is a natural voice verb.

**Monitors whose home location you are not in.** Three options, and this one should be a per-monitor
or per-location policy, not a global:

| Policy | Behavior | When right |
|---|---|---|
| **hidden** (recommended default) | Unplugged, exactly as `monitors_follow_session` already unplugs on doff — workspaces evacuate to remaining monitors via Hyprland's software-hotplug path (report 18 §4) | Most monitors. Avoids invisible quads eating focus and cursor. |
| **shown-at-distance** | Rendered at its real location pose, so the kitchen monitor is genuinely across the house, tiny | Immersive; useful with a house frame; useless without one (§2.1) |
| **summonable** | Hidden, but listed in `hyprctl openxr status` and nameable by `xrlocation summon` | The default's companion — hidden must not mean forgotten |

The **hidden** default reuses machinery that already exists and is already correct: `setMonitorsPlugged`
drives `onConnect(true)`/`onDisconnect()`, which is Hyprland's own hotplug path with full workspace
evacuation and return-by-name. Location-based plugging is the same funnel with a different predicate.

### 5.6 `xrrule` gains `location:`

The `xrrule` engine (`src/openxr/XRRule.{hpp,cpp}`, shipped `ff326132`/`8d49d05a`) is a clean fit.
Adding a matcher touches exactly three places:

1. `SXRRuleConds` — one `std::optional<...>` field (`XRRule.hpp`).
2. `parseXRRuleConds` — one `else if` in the chain at `XRRule.cpp:180-226`; the trailing
   `else` already emits a hard config error listing the valid keys, so the error message updates for
   free.
3. `SXRRuleContext` (`XRRule.hpp:53-64`) — one `std::string location` field, filled by
   `buildRuleContext`.

Semantics, mirroring the existing keys:

```ini
location:<regex>          # RE2 PartialMatch, like monitor:/focusclass: — NOT a full match
```

Two distinct questions a rule might want to ask, and they need different keys:

- **`location:`** — *which location this monitor is bound to.* A property of the monitor. Lets you
  write "kitchen monitors are always dimmer."
- **`here:`** (`yes|no`, or `at:<regex>` for the user's current location) — *whether the user is
  presently in that location.* A property of the situation. Lets you write the genuinely useful rule:

  ```ini
  # Monitors that belong somewhere else, seen from here, are ghosts.
  xrrule = alpha 0.35, here:no
  ```

Recommend shipping **both**, because `location:` alone cannot express the situational case and
situational transparency is exactly what `xrrule` is for.

**Threading note.** `xrrule` evaluation is *main thread, all of it* (`OpenXRManager.hpp:615-620`,
annotated as such). A monitor's bound location is a main-thread fact (set by a verb) — free. But
*"which location is the user in"* is derived from the head pose, which is frame-thread. It therefore
needs the established pattern: the frame thread edge-detects the location change and publishes it
through the existing frame→main `SXRStateEvent` queue, and `dispatchStateEvent` calls
`requestEffectEval()` — exactly as the adaptive dock/undock edge already does
(`OpenXRManager.cpp:1144`). Do not read a string config value or touch a hyprutils SP on the frame
thread; publish an interned location **id** (`std::atomic<uint32_t>`), not a name, following the
`publishAdaptiveStringTuning` precedent (`OpenXRManager.cpp:3302-3311`).

### 5.7 Config and verb surface (sketch)

```ini
# Declare locations. Extent is the migration geofence radius (XZ, meters).
xrlocation = desk,    extent:2.0
xrlocation = couch,   extent:2.5
xrlocation = kitchen, extent:3.0, offscreen:hidden      # hidden | distant | summonable

# Bind a monitor to a location; pos: is now in the LOCATION's frame and is durable.
xrmonitor = XR-main, 2560x1440@90, anchor:local location:desk pos:0.02,1.46,-1.41 yaw:5.3 pitch:3.0 adaptive:on roam:body, size:2.23

# Situational rules
xrrule = alpha 0.35, here:no
xrrule = alpha 0.55, anchorstate:follow        # the existing walking safety rule — keep it
xrrule = blackalpha off, location:kitchen      # kitchen screen is a recipe display, no keying
```

```
# Global knobs (openxr: section, numeric ⇒ frame-thread-safe, hot via readAnchorTuning())
openxr:location_persist        = true      # write/read the state file at all
openxr:location_auto_migrate   = true      # geofence migration on/off (force false on 3DoF)
openxr:location_adopt_unknown  = true      # §5.5 policy 1 as default
openxr:location_reuse_solve    = true      # the silver rung
openxr:location_blend_ms       = 700       # re-pose easing; defaults to adaptive_transition_ms
```

| Verb (`xrlocation` dispatcher + `hyprctl openxr location …`) | Args | Semantics |
|---|---|---|
| `register` | `<name> [at <x,y,z> yaw <deg>]` | Solve `session_origin → name` from the current head pose (or an explicit pose, for the voice path). Creates the location if new. **The bronze ritual.** |
| `enter` | `<name>` | Force "I am here" without a geofence (the 3DoF path, and the manual override). |
| `forget` | `<name>` | Drop the stored solve (keep the declaration). |
| `summon` | `<name>\|<monitor>\|all` | Re-dock into the current location. |
| `list` | — | Names, extents, solve age/method/validity — the diagnostic. |
| `save` | — | Print `xrlocation = …` + location-relative `xrmonitor = …` lines to paste (the `layout` sibling). |

Both transports funnel into one `cmd*` set on `COpenXRManager` and run main-thread under
`m_layersMu`, as every existing verb does (`DispatcherTranslator.cpp:806-871`, `XRIpc.cpp:287-411`).

---

## 6. Don-time flow

### 6.1 Where the solve hooks in

The edge already exists. `COpenXRManager::updateMonitorsPlugged(bool allowGrace)`
(`OpenXRManager.cpp:3039-3110`, main thread) is entered from `dispatchStateEvent` on both
SESSION_STATE and USER_PRESENCE (`:1094-1122`), and on the plug edge it already does exactly the
right shape of thing:

```cpp
if (firstPlug && !m_recenteredThisSession) {
    if (*PRECENTER) { m_recenteredThisSession = true; m_recenterArmed.store(true, ...); }
}
```

— main thread **arms an atomic**, frame thread (which owns the head pose) **consumes it on the next
valid-view frame** (`:1689-1704`). That handoff is the template; the location solve uses it verbatim.

**Two changes to the edge:**

1. **Re-armable.** Today `m_recenteredThisSession` makes it once-per-session, which is precisely why
   carrying the headset to another room and re-donning does nothing (§1). A location solve should
   re-arm on **every** don edge (visibility+presence transition), because that is when the "am I
   somewhere else now?" question needs answering. The anti-flap already exists in the surrounding
   grace/settle timers.
2. **`recenter_on_plug` becomes the fallback, not the default path.** When a location solve lands,
   `recenterLocalToHead` must not also run — they would compose and double-transform. Make
   `recenter_on_plug` the explicit no-rung-solved behavior (§6.4).

### 6.2 The don-edge decision ladder

```
don edge (visibility ∧ presence, after settle)
  │
  ├─ 1. GOLD:   a stationary-space (or anchor) pose arrived from the client, and its generationId
  │             matches the one stored with this location?          → use its 4-tuple
  ├─ 2. SILVER: stored solve whose {sessionEpoch, recenterEpoch} still match?   → reuse it
  ├─ 3. BRONZE: a location was registered in this session and we have not left its extent?
  │             → reuse it   (within one session this is identical to silver)
  └─ 4. NONE:   today's behavior — recenterLocalToHead(view, declared), i.e. the head-relative rig
                + a HUD prompt: "no location here — SUPER+SHIFT+D registers this spot"
```

Rungs 1–3 all produce a 4-tuple; rung 4 produces today's outcome. **The fallback being exactly
today's behavior is a design requirement**, not a nicety: it makes the whole feature safe to ship
incrementally and safe to disable. Note that rung 1's `generationId` check is *strictly better* than
rung 2's epoch heuristic — it is the runtime telling us "I relocalized successfully / I did not,"
which is the exact question, rather than our inference from proxy events.

### 6.3 What re-poses, and how

When a solve lands, **every location-bound anchor re-poses atomically in one frame**, then the
*display* eases:

- **Atomically** because the rigid-group property is the entire point — `recenterLocalToHead` already
  passes the same `view` to every layer for exactly this reason (`OpenXRManager.cpp:1696-1703`), and a
  staggered re-pose would visibly shear the arrangement.
- **Eased, not warped**, unlike `recenterLocalToHead`'s deliberate warp. The machinery exists twice
  already and both are the same shape: adaptive's `m_adFrom`/`m_adT`/`envAdvance`/`easeApply`/
  `lerpPose` (`XRAnchor.cpp:464-481`, `:534-565`) and the xrrule effect envelope `SXRFxEnv`
  (`XRRule.hpp:172-202`), whose `retarget()` restarts from the *current* value so an interrupted
  blend never snaps back. Reuse the adaptive envelope: freeze `from = m_lastWorld`, set
  `to = composed location pose`, run `openxr:location_blend_ms` (default = `adaptive_transition_ms`,
  700 ms).
- **No spring kick on hand-off** — copy `beginUndock`'s idiom (`m_springInit = false;
  m_seedLeashAtTarget = true;`), which exists precisely to stop the leash from lurching when the goal
  changes discontinuously.
- **Adaptive monitors** re-seat their dock seat to the new location origin, exactly as
  `recenterLocalToHead` already re-seats it to the head (`XRAnchor.cpp:1114-1122`).
- **Head/body/device-anchored monitors are untouched** — their offsets are user-relative and a house
  frame is irrelevant to them. Same as the existing reference-space-change handling.

### 6.4 Failure behavior

| Failure | Behavior |
|---|---|
| No rung solves | `recenter_on_plug` (today's behavior) + a one-line HUD prompt offering `register`. **Never** leave monitors at raw session coordinates — that is the "extremely non-ideal position" outcome. |
| Solve exists but is stale (epoch mismatch) | Do not use it silently. Fall through to rung 4, and mark the location "needs re-registration" in `xrlocation list` / `hyprctl openxr status`. |
| Location known, but user is not inside any location's extent | Treat as roaming: adaptive monitors go to roam; docked non-adaptive monitors stay in their location and are subject to the `offscreen:` policy (§5.5). |
| Two locations' extents overlap | Nearest origin wins, with the same hysteresis the geofence already uses. Warn at config-parse time if declared extents overlap by more than half a radius. |
| Tracking invalid at the don edge | Do nothing; the frame thread already only consumes the armed flag on a `viewValid` frame (`OpenXRManager.cpp:1689`). Retry next frame. |
| State file missing/corrupt | Log, start empty, keep the declarations. Never fail a session start on it. |

---

## 7. What this does *not* solve

Stated to keep the scope honest:

- **Physical drift within a session.** Quest relocalization jumps after tracking loss shift the
  session origin without any event we can see (§3.2). The mitigation is that re-registration is a
  two-second keybind.
- **Furniture moving.** The solve is right; the room changed. Same mitigation.
- **Non-HypXRland content.** Overlay VR apps and `hypxrpaper`'s environment stay in the raw session
  frame under the §4.1 design. Only a §4.2 server-side solve would fix that, and §4.2 is rejected for
  the location frame.
- **Sub-centimeter precision.** Nothing here is a survey instrument. The target is "my monitors are
  where I left them, within a few centimeters and a couple of degrees."

---

## 8. Multi-machine

`MULTI-MACHINE.md` establishes the topology: the AMD+NVIDIA Framework is host A, the Lunar Lake box
is B, one Quest 3, and the recommended end-state is Family D-lite (an exporter on B, a
decode-into-headless-output daemon on A) with A owning the single OpenXR session.

For locations this is simplifying rather than complicating:

- **The house frame is a property of the room, not the machine — but the *solve* is a property of the
  session, and sessions are per-machine.** `session_origin → desk` measured on A is meaningless on B,
  because B's WiVRn session has its own arbitrary origin. So the **solve must be per-machine**.
- **The declarations and the layout are worth sharing.** "There is a location called `desk` with
  extent 2.0 m, and `XR-main` sits at `(0.02, 1.46, -1.41) yaw 5.3` within it" is machine-independent
  and is exactly the kind of thing the user's git-tracked dotfiles should carry.
- **Therefore split the state file the same way the config/state split works** (§5.2), one more
  time:

  | Datum | Where | Shared across machines? |
  |---|---|---|
  | Location declarations (name, extent, policy) | config (`xrlocation =`), dotfiles | **Yes** (git) |
  | Monitor poses within a location | state file, *or* config if the user pastes them | **Yes**, and worth syncing |
  | The measured `solve` + epochs + anchor UUID | state file, per machine | **No** — machine-local |

  Concretely: `$XDG_STATE_HOME/hypr/xr-locations.json` holds a `layouts` object (syncable) and a
  `solves` object keyed by machine/session (not syncable). Put the machine key in the file rather
  than in the filename, so a naive `rsync` of the state file does not silently import a foreign
  solve.
- **The gold rung is the exception that could unify them.** A Meta persistent anchor lives on the
  *headset* and is keyed by app; if both machines run the *same* WiVRn client APK, both resolve the
  same anchor UUID, and both get a correct solve to the same physical point. That is a genuine
  cross-machine house frame, for free, and it is a strong secondary argument for gold. Note: it is
  unconfirmed whether Horizon OS scopes anchor storage per-app-install or per-app-identity; WP-G1
  measures it.
- **Under D-lite there is only one XR session anyway** (A owns the headset; B contributes pixels),
  so at that point the multi-machine question mostly dissolves: B's monitors are headless outputs on
  A and bind to A's locations like any other.

---

## 9. Work packages

Sized one-subagent-each, in dependency order. **B-series is the recommended first ship.**

### Track B — bronze ritual + the location model (the gamble-killer)

| WP | What | Effort | Deps | Headless? |
|----|------|--------|------|-----------|
| **B0** | **Measurement, no product code.** Two half-day experiments that de-risk everything downstream: (a) create a **STAGE** reference space alongside LOCAL_FLOOR and log the STAGE↔LOCAL_FLOOR relation across a doff/don, a session restart, and a Quest recenter — does the user's *boundaryless* Quest expose a real, stable STAGE (§3.2), which would make silver nearly free? (b) `dlopen` `libmonado_wivrn.so` from the runtime manifest and call `mnd_root_get_reference_space_offset(LOCAL)` (read-only first), then a set, and confirm the resulting `REFERENCE_SPACE_CHANGE_PENDING` arrives with `poseValid=false` as §4.3 predicts. Write the findings into this report. | **S** | none | needs the Quest |
| **B1** | **Pure location math + the applicator.** `SXRLocation` POD; `solveFromHead(view) → 4-tuple` (yaw-only, floor-projected — generalize `recenterLocalToHead`'s math); `applySolve(locations, layers)` composing `world = poseCompose(locationWorld, poseInLocation)`; extent test with two-radius hysteresis + dwell reusing `dwellStep`; the eased re-pose envelope reusing `envAdvance`/`easeApply`/`lerpPose`. `tests/xr/location.cpp`: rigid-group invariance, 3DoF (yaw-only) reduction, round-trip `solve∘apply == identity`, hysteresis/dwell, reverse-interrupt. | **M** | none | **Yes**, fully |
| **B2** | **Location registry + parent-transform solve.** `CXRLocation` registry on `COpenXRManager` (main thread, `m_layersMu`); two-phase frame solve (resolve each location's world pose once, then compose children) — **built to report 03 WP-G2's shape so grids inherit it** (§2.3); `location:` token on `xrmonitor`; per-monitor binding; `xrlocation` keyword + reconcile against declarations (model on `reconcileDeclaredMonitors`). | **M/L** | B1 | mostly |
| **B3** | **Verbs + IPC + events.** `xrlocation register/enter/forget/summon/list/save` on both transports; `xrlocationentered`/`xrlocationleft` socket2 events from the frame→main queue; `hyprctl openxr status` location block; `hyprctl openxr location save` printing paste-ready lines. Frame→main publishes an interned location **id** atomic, never a string. | **M** | B2 | partly |
| **B4** | **State file.** `$XDG_STATE_HOME/hypr/xr-locations.json`; atomic replace; debounced writes; schema versioning; machine-keyed `solves` vs shared `layouts` (§8); load at `init()`, before `reconcileDeclaredMonitors`. Corrupt/missing ⇒ warn and continue. | **S/M** | B2 | **Yes** |
| **B5** | **Don-edge integration.** Make the plug-edge solve re-armable per don; the §6.2 decision ladder; `recenter_on_plug` demoted to the no-solve fallback; the eased atomic group re-pose; adaptive seat re-seat. | **M** | B1–B4 | wire-and-test headless; **feel LIVE** |
| **B6** | **`xrrule` `location:` + `here:`.** One `SXRRuleConds` field each, one `else if` each in `XRRule.cpp:180-226`, two `SXRRuleContext` fields, `requestEffectEval()` on the location edge. `tests/xr/xrrule.cpp` cases. | **S** | B3 | **Yes** |
| **B7** | **Migration.** Redock target = entered location's saved pose; the adopt-unknown policy; `summon`; the `offscreen:` plug policy routed through `setMonitorsPlugged`. | **M** | B5 | logic gtest; **feel LIVE** |
| **B8** | **Voice verb.** `hypxrvoice` intent `register_location` ("this is my desk", "call this place the kitchen") → `xrlocation register <name>` using the existing word-time gaze ring. Repo-side only; no compositor change beyond B3's explicit-pose form. | **S** | B3 | daemon tests |

### Track S — silver

| WP | What | Effort | Deps |
|----|------|--------|------|
| **S1** | **Reuse-the-last-solve.** `sessionEpoch` (bumped in `start()`) + `recenterEpoch` (bumped in `onReferenceSpaceChanged`) on every stored solve; validity check in the §6.2 ladder; `xrlocation list` shows age/validity. **Makes the doff→don-within-a-session case instant and exact.** | **S** | B4 |
| **S2** | **The invalid-recenter-pose fix (§4.3).** Patch `u_space_overseer.c` to populate `pose_in_previous_space` with the applied delta and set `pose_valid = true`; carry in `patches/monado/`; upstream it. Verify HypXRland's `onReferenceSpaceChanged` then holds monitors still across a Quest recenter. | **S** | none — **do this early, independent of everything** |

### Track G — gold

| WP | What | Effort | Deps |
|----|------|--------|------|
| **G0** | **Fix the swallowed STAGE change (§3.2).** WiVRn client: handle `REFERENCE_SPACE_CHANGE_PENDING` for STAGE, not only LOCAL, and carry `poseInPreviousSpace` to the server as a new packet. Today a guardian redraw or space re-setup silently shifts the entire streamed world. **This is a bug fix, not a feature, and it is plausibly upstreamable on its own.** | **S/M** | none |
| **G1** | **Capability measurement (gates G2–G4).** Add `com.oculus.experimental.enabled` to the manifest (already proven, `044fea16`) and probe, via the client's existing enumerate-first filter (`application.cpp:1277-1284`): `XR_EXTX2_stationary_reference_space`, `XR_EXT_stationary_reference_space`, `XR_FB_spatial_entity` + `XR_META_spatial_entity_persistence`/`_discovery`, and the `XR_EXT_spatial_*` family. Log what enumerates, with and without `debug.oculus.experimentalEnabled 1`, and with/without `com.oculus.permission.USE_ANCHOR_API`. Also establish whether the untrusted-package developer-team gate bites our sideloaded APK. Mirror `guardian-runtime`'s `client/xr/system.cpp` probe. **Report the answer back into this document.** | **S** | manifest |
| **G2** | **Client: stationary reference space.** Create `XR_REFERENCE_SPACE_TYPE_STATIONARY_EXTX2` (falling back to `_EXT`), locate the streamed pose origin in it each session, and read the generation UUID (`xrGetStationaryReferenceSpaceIdEXTX2`). Hand-written extension header if needed — `common/xr/meta_boundary_visibility.h` is the 57-line template. | **M** | G1 (positive) |
| **G2′** | *(fallback if stationary space does not enumerate)* **Client: anchor lifecycle** against `XR_FB_spatial_entity` + `XR_META_spatial_entity_persistence`/`_discovery` — create → `STORABLE` → `xrSaveSpacesMETA` → next session `xrDiscoverSpacesMETA` → retrieve → locate. Handle the six documented failure codes. | **L** | G1 |
| **G3** | **Wire.** A `from_headset::stationary_frame{XrPosef originInStationary, uuid[16] generationId}` packet (or an anchor equivalent), plus a `to_headset` register/forget if we go the anchor route. `operator()` overload in `server/driver/wivrn_session.h`; append to the variants only; the protocol hash bumps itself. | **S** | G2 or G2′ |
| **G4** | **Compositor consumption.** Stationary pose (or anchor pose) → a location solve with `method: gold`; `generationId` stored per location and used as the §6.2 rung-1 validity key, superseding S1's epochs. | **S** | G3, B4 |

### Sequencing

```
S2  ── independent, ~20 lines, do now
B0  ── measurement (STAGE stability + libmonado), can run any time
   │
B1 → B2 → B3 → B4 → B5   ── FIRST SHIP: locations that survive don/doff ──▶
                     ├─ B6 (xrrule location:/here:)
                     ├─ B7 (migration)
                     └─ B8 (voice "this is my desk")
S1  ── after B4; small; the biggest felt win per line in the whole report
G0  ── independent bug fix; do whenever an APK build cycle happens
G1  ── measurement, parallel with all of B → G2 (or G2′) → G3 → G4
```

**Headless vs live.** B1, B4, B6 and the pure halves of B2/B7 are fully gtest-able (the applicator is
POD + `dt`, in the tradition of `tests/xr/adaptive.cpp` and `tests/xr/anchor_math.cpp`). The
hyprtester `--xr` scripted-head-pose driver can walk the head across extents to test migration
without a headset, exactly as report 13 WP-A6 does for the geofence. What genuinely needs a Quest:
B0 and G1 (both are pure measurement), registration *ergonomics* (is 2 s acceptable? is seated yaw
repeatable enough?), and the re-pose easing taste.

---

## 10. Recommendation

**Ship B1–B5 as one coherent feature: "locations."** That is the gamble-killer, it needs no WiVRn
changes, no APK, no Meta permissions, and no cooperation from anyone. Its user-visible contract is:

> Sit at your desk once, press a key. From then on — every don, every session, every reboot — your
> monitors are exactly where you left them. If you walk to the couch with a monitor following you and
> settle in, it re-docks into the couch's arrangement.

**Do S2 immediately and independently**, because it is ~20 lines on a tree we already patch, it is
upstreamable, and there is a good chance a meaningful share of the current "extremely non-ideal
positions" is simply monitors teleporting on every Quest system recenter (§4.3).

**Do S1 right after B4**, because the *most frequent* case in daily fishfood use — take the headset
off, put it back on twenty minutes later, same session — becomes instant and mathematically exact for
a handful of lines.

**Run B0 and G1 as measurements before committing to any of gold.** B0 is a half day and answers two
questions worth a lot: does this boundaryless Quest expose a real STAGE (which would make silver
nearly free), and does moving LOCAL via libmonado behave as §4.2 predicts. G1 answers the one
question that decides the whole gold rung: *does `XR_EXTX2_stationary_reference_space` enumerate on
our APK?* If it does, gold is an **M**, not an **L** — a client patch smaller than the boundary-
visibility one we already carry, no anchor permissions, no untrusted-package gate, a spec-mandated
`generationId` that replaces our epoch heuristic, and a genuine cross-machine house frame for free
(§8). If it does not, fall back to the anchor API only if the ritual proves genuinely annoying;
bronze + silver is already a good product.

**Do G0 whenever an APK build cycle happens anyway.** The swallowed STAGE change-pending event is a
silent-corruption bug in software we ship, and it is the most plausible explanation for "my monitors
moved and I have no idea why" that does not involve anything in this report's new machinery.

**Do not** build the house frame as a stored global before locations work (§2.1 — derive it). **Do
not** apply the location solve through libmonado's LOCAL offset (§4.2 — one global origin cannot
express plural locations), even though it is tempting because it needs no code on the other side.
**Do not** write measured solves into the user's git-tracked config (§5.2). **Do not** target the
Meta anchor API first — it is the heavier, more fragile, less upstreamable of the two gold routes,
and Meta's own guidance (3 m anchor proximity, anchors die on space-history clear) says it will not
remove the need for a re-registration path anyway.

---

## 11. Open questions for the user

1. **Ritual shape.** Is "sit down, press SUPER+SHIFT+D once per new room" acceptable as the v1
   contract, or is the ritual itself the thing you want gone (in which case G1 moves to the front)?
2. **Which locations do you actually have?** The design assumes 2–4 (desk, couch, kitchen, maybe
   standing desk). If it is really ~10, the extent-overlap and "which am I in" problems get harder and
   the polygon-extent question comes forward.
3. **Unknown-location policy** (§5.5): adopt-where-you-left-it (recommended), stay-roaming, or
   mirror-from-previous?
4. **Off-location monitors** (§5.5): hidden (recommended, reuses the plug path), shown-at-distance, or
   summonable-only? Per-monitor or global?
5. **`recenter_on_plug` after locations land.** Keep it as the no-solve fallback (recommended), or
   retire it entirely once every monitor is location-bound?
6. **Auto-migrate by geofence, or explicit "come with me" only?** The geofence is built and free; but
   it is also the thing most likely to feel surprising the first time it fires while you cross the
   room for a coffee.
7. **Sharing layouts across the two machines** (§8) — worth the state-file split now, or leave both
   machines fully independent until D-lite lands?
8. **APK appetite for G0/G1.** You already build the boundaryless client. Is a build cycle worth it
   now for (a) the STAGE change-pending bug fix and (b) a pure capability probe that tells us whether
   `XR_EXTX2_stationary_reference_space` enumerates? Both are small; G1 in particular changes the
   whole gold estimate from **L** to **M** if it comes back positive.
9. **Is your Meta account on a developer team?** This decides whether the *anchor* route is even
   available to a sideloaded APK (Meta gates spatial-data APIs for untrusted packages behind
   developer-team registration). It does not affect the stationary-reference-space route, which is
   another reason to prefer it.
10. **Where does the state file live, and is it in your dotfiles?** §8 proposes splitting shared
    `layouts` from machine-local `solves` inside one JSON. If you would rather sync nothing, the split
    can be dropped and the file kept fully machine-local.

---

## 12. Sources

**HypXRland** (worktree `/home/ajg/code/Hyprland-wt-research22`, `c3bdf3aa`):
`src/openxr/XRSession.cpp:85-95,252-279,399-411`; `src/openxr/XRAnchor.hpp:153-221,392-399,518-532`;
`src/openxr/XRAnchor.cpp:215-354,359-437,464-570,573-624,1049-1123`;
`src/openxr/OpenXRManager.cpp:1094-1144,1292-1302,1616-1704,2086-2108,3039-3110,3302-3311,3782-3824,3903-3919,4134-4187`;
`src/openxr/OpenXRManager.hpp:615-620,631,646,750-787`; `src/openxr/XRMonitorConfig.cpp:137-262,570-651`;
`src/openxr/XRMonitorConfig.hpp:126-196`; `src/openxr/XRMonitorLayer.hpp:34-46,91-93`;
`src/openxr/XRRule.hpp:41-64,172-202`; `src/openxr/XRRule.cpp:32-55,110-135,144-226,231-304`;
`src/openxr/XRIpc.cpp:180-186,232-278,287-411`; `src/openxr/XRMath.hpp:26-90,232-236,995-1056`;
`src/config/values/ConfigValues.cpp:714-968`; `src/config/legacy/ConfigManager.cpp:634-635,1500-1541`;
`src/config/legacy/DispatcherTranslator.cpp:806-871`; `tests/xr/{adaptive,plugged,parser,xrrule}.cpp`;
`docs/openxr/02-virtual-monitors.md`, `03-anchoring.md:359-500`, `05-configuration.md:372-470` + IPC.

**Vendored monado** (`subprojects/monado`): `src/xrt/auxiliary/util/u_space_overseer.c:117-126,794-890,1027-1074`;
`src/xrt/state_trackers/oxr/extension_support/oxr_extension_support.py`;
`src/xrt/targets/ctl/main.c:194,344,407`; `src/xrt/ipc/{client/ipc_client_space_overseer.c:282,313, server/ipc_server_handler.c:998,1039, shared/proto/50-space.json:88}`;
`src/xrt/targets/libmonado/monado.c:590`; `src/xrt/include/xrt/xrt_space.h:244,292`.

**WiVRn 26.6.2** (`~/code/wivrn-26.6.2`, monado pin `1b526bb3a`): `client/application.cpp:1231-1284,1351`;
`client/xr/instance.cpp:68-134,152-207`; `client/scenes/stream.cpp:1298-1312`;
`client/scenes/stream_tracking.cpp:456-457`; `common/wivrn_packets.h:42,201,216-227,289-329,520-528,567-596,809-828`;
`common/wivrn_serialization.h:48-87,534-544,807-865`; `common/protocol_version.h:7-9`;
`server/driver/wivrn_session.h:75,175,230-239`; `server/driver/wivrn_session.cpp:292-323,375-425,605-611`;
`server/driver/wivrn_connection.cpp:169`; `server/wivrn_ipc.h:47-67`; `AndroidManifest.xml:7-112`;
`patches/monado/0001…0008`. Fork branches: `~/code/wivrn` `guardian-research` (`ddb5a8ce`),
`guardian-runtime`, `boundaryless`; `~/code/wivrn-wt-client-mic` (`044fea16`);
`~/code/wivrn/hypxr/GUARDIAN-DISABLE.md`.

**OpenXR headers**: `~/code/wivrn-fetchcontent/openxr_loader-src/include/openxr/openxr.h`
(`XR_CURRENT_API_VERSION 1.1.58`) — `:2580,4241,5616,5793,5893,6171,6201,7083,7199,7333,11845,12368,12410`.

**hypxrvoice** (`~/code/hypxrvoice`): `README.md` (deixis contract, word-time gaze),
`docs/DEIXIS-SEMANTICS.md` (`hyprctl -j openxr gaze at <ms>`, lead/window/vote, `resolveSelected`).

**Live read-only**: `hyprctl openxr status`, `hyprctl openxr layout`, `hyprctl monitors`,
`~/.config/hypr/hyprland-xr.conf` (+ `git log`), `~/.config/wivrn/config.json`,
`journalctl --user -u wivrn`. Quest at `192.168.50.233:5555` was **not reachable** over adb this
session (`No route to host`), so no on-device permission/runtime probing was possible — G1 covers it.

**Web sources**: see §13.

---

## 13. Web sources and external facts

Facts below are tagged **[SPEC]** (Khronos registry / spec text / source read directly), **[DOC]**
(vendor documentation), **[PRESS]**, **[FORUM]** or **[UNCONFIRMED]**.

### The stationary reference space (§3.1.1)

- `XR_EXT_stationary_reference_space` — ext **743**, rev 1, marked *Not ratified*, published in
  **OpenXR 1.1.59, 2026-04-30**. Contributors: Microsoft, **Meta** (Andreas Selvik, Yuichi Taguchi),
  Valve, NVIDIA, Google. Guarantees: gravity-aligned +Y up with undefined forward/height; unaffected
  by recenter; unaffected by boundary redefinition; best-effort relocalization across tracking loss,
  doff/don, suspend, session restart and reboot; `generationId` UUID via
  `xrGetStationaryReferenceSpaceGenerationIdEXT`, changing (with `poseValid = XR_FALSE`) when
  relocalization fails. **[SPEC]** — registry.khronos.org/OpenXR/specs/1.1/html/xrspec.html;
  `xr.xml` @ KhronosGroup/OpenXR-SDK; `OpenXR-Docs/CHANGELOG.Docs.md`.
- `XR_EXTX2_stationary_reference_space` — Meta's shipping experimental predecessor.
  `XR_REFERENCE_SPACE_TYPE_STATIONARY_EXTX2 = 1000742000`, `xrGetStationaryReferenceSpaceIdEXTX2`,
  `XrStationaryReferenceSpaceIdResultEXTX2{generationId: XrUuid}`. In
  `meta-quest/Meta-OpenXR-SDK` under `OpenXR/meta_openxr_preview/extx2_stationary_reference_space.h`
  since **SDK v77 (2025-06-06)**, still present at **v85 (2026-02-12)**. **[SPEC]**
- Godot OpenXR vendors plugin **PR #418, merged 2026-01-27**, implements it and documents the gate:
  `<uses-feature android:name="com.oculus.experimental.enabled" android:required="true"/>` plus
  `adb shell setprop debug.oculus.experimentalEnabled 1`. github.com/GodotVR/godot_openxr_vendors/pull/418 **[SPEC]**

### Meta anchors and permissions (§3.1.2)

- Current recommended set: `XR_FB_spatial_entity` + `XR_META_spatial_entity_persistence` (save/erase)
  + `XR_META_spatial_entity_discovery` (load). Meta states verbatim that
  `XR_META_spatial_entity_persistence` "is the next version of" and renders **obsolete**
  `XR_FB_spatial_entity_storage` and `_storage_batch`.
  developers.meta.com/horizon/documentation/native/android/openxr-spatial-anchors-api-ref/ **[DOC]**
- Per-app scoping: anchors are "created and owned by the application, remaining private within its
  context"; only *scene* anchors are system-wide.
  developers.meta.com/horizon/documentation/unity/unity-spatial-anchors-overview/ **[DOC]**
- **3-metre proximity rule**: "Create or reuse a spatial anchor within three meters of the object you
  want to anchor… Drift occurs when the user moves away from the anchor."
  developers.meta.com/horizon/documentation/unity/unity-spatial-anchors-best-practices/ **[DOC]**
- **Space-history clear destroys anchors**, and is Meta's own prescribed remedy for MR drift ("you
  will need to set up your space again and replace previously placed objects").
  meta.com/help/quest/888109563496119/ and /625635239532590/ **[DOC]**
- Permissions, verified against Meta's own sample manifest
  (`oculus-samples/Unity-SharedSpatialAnchors/Assets/Plugins/Android/AndroidManifest.xml`) **[SPEC]**:
  `com.oculus.permission.USE_ANCHOR_API` (install-time), `com.oculus.permission.USE_SCENE`
  (**runtime**-requested since ~v62, one-time explainer + consent dialog —
  developers.meta.com/horizon/documentation/unity/unity-spatial-data-perm/ **[DOC]**),
  `IMPORT_EXPORT_IOT_MAP_DATA` + `USE_COLOCATION_DISCOVERY_API` (sharing only),
  `BOUNDARY_VISIBILITY`, and `<uses-feature com.oculus.experimental.enabled>`. **No**
  `horizonos.permission.USE_SCENE` or `ACCESS_SPATIAL_DATA` exists — those names appear to be wrong;
  `horizonos.permission.HEADSET_CAMERA` does exist, so the namespace is real. **[UNCONFIRMED negative]**
- **Untrusted-package gate**: "For Developers Only, the logged in Meta account must be registered
  with a developer team. This will allow untrusted applications to use spatial data… Applications
  downloaded from the Store or App Lab do not have this restriction."
  developers.meta.com/horizon/documentation/native/android/mobile-dynamic-object-tracker/ **[DOC]**
  Community failure signatures `ERROR_PACKAGE_UNTRUSTED` / `rpcAllowed: false` **[FORUM, unverified —
  Meta's forums refuse automated fetching]**.
- Meta v66 dev blog (2024-07-11): Space Setup persists up to **15 rooms on-device**; world-lock over
  spaces up to ~200 m². **[DOC]**

### The ratified EXT spatial family (§3.1.2)

- `XR_EXT_spatial_entity` (#741), `_plane_tracking` (#742), `_marker_tracking` (#744),
  `XR_EXT_spatial_anchor` (#763), `XR_EXT_spatial_persistence` (#764), `_persistence_operations`
  (#782) — all rev 1, all `ratified="openxr"`, published **OpenXR 1.1.49, 2025-06-10**, all depending
  on `XR_EXT_future`. Note `_marker_tracking` covers **QR / Micro-QR / ArUco / AprilTag** — the
  printed-marker ritual of §3.3, if a runtime ever exposes it. Registry marks the MSFT anchor extensions `obsoletedby` these. Persistence
  scopes: `XR_SPATIAL_PERSISTENCE_SCOPE_LOCAL_ANCHORS_EXT` (same device/user/**app**) and
  `_SYSTEM_MANAGED_EXT` (read-only, UUIDs stable across sessions and reboots). **[SPEC]**
- **Android XR implements** `XR_EXT_spatial_entity`, `_anchor`, `_persistence`
  (developer.android.com/develop/xr/openxr/extensions, updated 2026-07-31) **[DOC]**;
  **PICO** "among the first" (khronos.org/blog/openxr-spatial-entities-extensions-released-for-developer-feedback,
  2025-06-10) **[PRESS]**; **Meta** "thrilled to soon support" but no evidence Horizon OS ships it as
  of mid-2026 **[UNCONFIRMED]**; **SteamVR** no evidence **[UNCONFIRMED]**. Note the registry name is
  `XR_EXT_spatial_entity`, **singular** — the plural form seen in some write-ups is wrong.

### Monado externals (§4.2)

- `xrt_space_overseer` in `src/xrt/include/xrt/xrt_space.h`; default impl **renamed upstream** from
  `auxiliary/util/u_space_overseer.c` to `src/xrt/base/b_space_overseer.c` (our pin still has the old
  path). LOCAL is created at a hardcoded stage + 1.6 m identity pose **every run**; nothing spatial is
  persisted (`~/.config/monado/config_v0.json` holds calibration/tracking overrides only);
  `xrGetReferenceSpaceBoundsRect` yields `XR_SPACE_BOUNDS_UNAVAILABLE` (open issue #164 since 2022).
  **[SPEC, gitlab.freedesktop.org/monado/monado]**
- **MR !2284 "ipc, u/space_overseer: space offset api", merged 2024-07-19**, explicitly to "open the
  door for 3rd party apps to do space manipulation and calibration"; superseded galister's narrower
  !2231 `apply_stage_offset`. Shipped publicly in **monado 25.0.0 (2025-04-18)**, `MND_API_VERSION`
  1.3+. Discovery via the `MND_libmonado_path` key in the OpenXR runtime manifest. **[SPEC]**
- **`motoc`** (github.com/galister/motoc) uses this API to calibrate and persist tracking origins
  across sessions — direct prior art for §4.2. **[SPEC]**
- `OXR_RECENTER_STAGE=1` (monado MR !2588, merged 2025-10-01) makes monado report STAGE as
  unsupported and hand apps LOCAL_FLOOR instead. Undocumented in WiVRn; the name is misleading. **[SPEC]**

### WiVRn upstream (§3.2)

- Zero issues/PRs/discussions on spatial anchors, persistence, scene models or colocation, across the
  project's whole history. Maintainers: xytovl (Patrick Nicolas, lead), Meumeu (Guillaume Meunier),
  ImSapphire, galister. **[SPEC]**
- Stated positions: xytovl, **issue #112, 2024-08-23** — *"I don't think setting the floor is
  something we should be doing. It's for vendors to fix their software."*; galister, 2025-10-07 —
  *"we shouldn't need to implement workarounds for something that the headset vendor is supposed to
  add to their runtime."*; ImSapphire, **#806, 2026-02-25** — *"We recentre LOCAL spaces when
  requested by the client, STAGE is not recentred by us."* **[SPEC]**
- Relevant history: **#90** "origin lost after HMD sleep" (2024-07) fixed by **PR #121** (2024-09-05)
  by switching the streamed pose space to STAGE; commit **`2ba3d78`** "Try to request OpenXR 1.1"
  (2025-07-02, shipped v25.8) crossed Meta's threshold where STAGE becomes real and immovable, which
  users read as broken recentering — **#458, #474, #561, #806**. **[SPEC]**

### Prior art (§7-adjacent)

- **Immersed** — no anchor evidence; its documented fix for disoriented screens is a headset
  recenter, the signature of an origin-relative layout. **[DOC/FORUM]**
- **Virtual Desktop** — full changelog v1.28.1 (2023-08-19) → v1.34.16 (2026-04-01) contains **no**
  anchor/persist entries; v1.32.5 (2024-05-27) added *"auto-arrange monitors on recenter (enabled by
  default)"* — the opposite design choice. **[SPEC, github.com/guygodin/VirtualDesktop/releases]**
- **Steam Link** — Valve staff (danw, 2023-12-27): "Recentering is handled by the headset, not
  SteamVR." Its 2025 windowed mode runs as an ordinary Horizon OS 2D panel, inheriting OS pinning.
  **[FORUM/PRESS]**
- **ALVR** (source-verified at master, 2026-07-16) — no anchor extensions; STAGE-based with
  `RecenteringMode {Stage, LocalFloor, Local, Tilted}` (default `LocalFloor`); recenter recomputes an
  in-memory yaw-only offset, nothing persisted. Only `Stage` mode persists, and only via Meta's
  Guardian origin. **[SPEC]**
- **Horizon OS "AppPinning"** shipped in **v81** (blog 2025-10-28): 3 pinned windows in passthrough +
  3 in Immersive Home, which "stay where you left them" across app switches and reboots. Whether it
  is built on spatial anchors is **[UNCONFIRMED]** — Meta has published no mechanism statement.
  "Augments" (announced Connect 2023) never shipped; Bosworth (2025-08-19) said the original
  architecture "was wrong." **[PRESS]**
- **Net:** no PC-streaming client does world-anchored session persistence. The universal pragmatic
  tier is "persist poses in STAGE and let Guardian carry the world lock" — which fails exactly when
  Guardian is redrawn, i.e. the user's observed failure mode (§3.2).

### Registration math and drift (§2.2, §3.3)

- Gravity alignment is normative: OpenXR 1.1 §7.1 defines LOCAL as "gravity-aligned to exclude pitch
  and roll, with +Y up, +X right, -Z forward," with the same language for LOCAL_FLOOR and STATIONARY.
  **[SPEC]**
- Unconstrained absolute-orientation closed form: B.K.P. Horn, "Closed-form solution of absolute
  orientation using unit quaternions," *JOSA A* **4**(4):629, 1987, DOI `10.1364/josaa.4.000629`. The
  gravity-constrained (3+1)-DoF case: Li, Liu, Xia et al., "Fast and deterministic (3+1)DOF point set
  registration with gravity prior," *ISPRS J. Photogramm. Remote Sens.* **199**:118-132, 2023, DOI
  `10.1016/j.isprsjprs.2023.03.022` (Crossref-verified). **[SPEC]**
- Quest 3 short-horizon accuracy: robot-arm study against a TECHMAN TM5-900 (±0.05 mm) — mean
  translational RMSE **0.346 mm** (3D 0.621 mm), rotational RMSE **0.143°**, 2848 samples / 151.5 s,
  Pearson r > 0.9999 on all axes. *Sensors* **26**(8):2285, April 2026, mdpi.com/1424-8220/26/8/2285.
  **Scope caveat:** 200 mm single-axis motions over 2.5 min — bounds jitter and short-range relative
  error only. **No published session-scale absolute drift figure for Quest 3 exists. [UNCONFIRMED]**
- OpenXR explicitly permits inside-out runtimes to "introduce slight adjustments to the origin of each
  space on a continuous basis," with **no event**, and warns that `xrLocateSpace` results "may change
  over time, even for spaces that track static objects." On tracking loss, runtimes serve inferred
  poses with `POSITION_VALID_BIT` set but `POSITION_TRACKED_BIT` cleared — **check the tracked bit**.
  **[SPEC]**
