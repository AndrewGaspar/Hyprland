# 28 — The spatial model: frames, hydration, and what a placement actually *means*

**Status:** research / problem-shaping. **Nothing here is implemented.** No code changes, no builds, no
live actuation — every `hyprctl` call in the evidence base was read-only, the live fishfood session was
never touched, and no process was signalled. Author: research pass 2026-08-26. Base commit: `67200a838`.

**Relationship to report 22.** [22-spatial-persistence-locations](22-spatial-persistence-locations.md)
(2026-08-03, base `c3bdf3aa`) asked *"how do we obtain a stable frame?"* and answered with the
relocalization ladder (bronze ritual / silver boundary identity / gold stationary reference space).
That report is still correct and still unimplemented; this one does not replace it. This report asks the
**other** half of the question, which 22 assumed away: *"given a frame, what is a placement, and what
happens to it on every event?"* It is the object model, the hydration semantics, and the event taxonomy —
plus three root causes found this pass that 22 could not have seen, because two of them were introduced
by the fixes that shipped after it and one of them lives in WiVRn's client.

The ask (user's words, distilled): *slay the world-placement/restore/don/doff demon.* The wanted end
state is a combination of (a) **true** world placement — quads and panes that really stay in the exact
same place in world space, and **if that world space is unfindable they are not hydrated**; (b) objects
**attached to your body**; (c) objects whose world placement is **refreshed relative to you on don/doff**
— persistent monitors you expect to just *be there*. The user's own sketch: possibly a **hierarchy of
spaces**, where (c) is a space we re-seat on recenter or on re-don after inactivity. The user explicitly
asked to be pushed back on. §7 does that in six places.

Cross-refs (source of truth):
- `docs/openxr/03-anchoring.md` §§4, 6, 8 (modes, adaptive decorator, the recenter/restore/re-seat
  ladder); `docs/openxr/05-configuration.md` §3 (the `xrmonitor` grammar) and the `openxr:` table;
  `docs/openxr/02-virtual-monitors.md` (plug lifecycle).
- `research/22-spatial-persistence-locations.md` — the relocalization ladder and the *location* model.
- `research/archive/13-adaptive-anchoring.md` (shipped), `research/archive/18-monitor-plugged-state.md`
  (shipped, then evolved), `research/LAYOUT-AND-NAMING.md` (the missing parent-transform primitive),
  `research/XREAL-3DOF.md` (the 3DoF degradation).

Evidence base (all read-only):
- **HypXRland** `src/openxr/*` at `67200a838`, `tests/xr/*`, `hyprtester/src/tests/xr/*`; the user's live
  `~/.config/hypr/hyprland-xr.conf`.
- **monado** `~/code/monado` (`v25.1.0-271-gc2ddab59d`) — `u_space_overseer.c`, `u_builders.c`,
  `oxr_space.c`, `oxr_session.c`, `oxr_instance.c`.
- **WiVRn** `~/code/wivrn` (our fork, `v26.6.1-2-ge54b56fe`) — `client/application.cpp`,
  `client/scenes/stream.cpp`, `server/driver/wivrn_session.cpp`, `common/wivrn_packets.h`.
- Web: OpenXR registry + spec 1.1, Apple/Meta/Microsoft/Magic Leap/Valve developer documentation, and
  the vendor extension registries. §11 lists every URL.

---

## TL;DR — RECOMMENDATION

1. **The one-line diagnosis: we built the world on the seat.** OpenXR separates `LOCAL`/`LOCAL_FLOOR` —
   *by specification* the space the runtime re-establishes at app start and on every recenter — from
   `STAGE`, the room-anchored, floor-level frame. `CXRSession::createSpaces` (`XRSession.cpp:265-293`)
   creates only the first, and **`XR_REFERENCE_SPACE_TYPE_STAGE` appears nowhere in `src/`**. Every
   "world" coordinate we have ever stored has been named against a frame the spec licenses the runtime
   to move. Both reported symptoms are fully spec-legal behaviour for `LOCAL`. §1.1, §5.2.

2. **And the room frame is available today, for free, with no patch to anything.** In every WiVRn session
   on this box `XR_REFERENCE_SPACE_TYPE_STAGE` is enumerated and resolves to monado's *root* — which is
   fed directly by the Quest's own STAGE poses (`client/application.cpp:1351`,
   `u_space_overseer.c:1266-1273`, `oxr_system.c:334-336`). Monado's recenter offsets only LOCAL and
   LOCAL_FLOOR, so **STAGE is immune to recenter, immune to re-don, and survives a WiVRn reconnect** —
   three of the four events that have been throwing monitors across the house. Meta confirms the
   semantics in its own docs: *"On Quest, the Stage tracking origin will not directly respond to user
   recentering."* On the deployed `wivrn-xg` fork it is additionally *stabilised* across in-session stage changes
   (§2.4). One `xrCreateReferenceSpace` call. §2.6, §4.2.

3. **The missing invariant is not a frame — it is frame *identity*.** Every stored placement in
   HypXRland is a number without a frame, silently reinterpreted in whatever origin happens to be
   current. That is the whole "restored way off in the bedroom" bug in one sentence, and the fix is
   provenance — `{frameId, generation, capturedAt, evidence}` on every stored pose, and a **refusal**
   when it does not match. Khronos says the same thing normatively (*"poses are always described as the
   relationship between two spaces"*), and standardised the token four months ago as
   `XR_EXT_stationary_reference_space`'s `generationId`. §1.2, §5.1, §5.5.

4. **Symptom B is not a bug, it is a design defect, and it is one line.** `xrGroupSeatFrame`
   (`XRAnchor.hpp:350-353`) sets the re-seat distance to `clamp(|(head−centroid)·n|, 0.3, 5.0)` — *the
   distance the group currently happens to be from you*. After a bad restore parks the group 20 m away,
   `reseat` computes 20, clamps to 5.0, and plants it **exactly five metres in front of your face**.
   `tests/xr/anchor_math.cpp:1274-1288` asserts this on purpose. The verb inherits the corruption it
   exists to repair, because **the group is not an object** — the parent transform is emergent, re-derived
   from its children every time. §3.2.

5. **Symptom A's likeliest cause is that the capture records the walk-away, not the workspace.** The
   restore offset is re-derived *every frame* while visible+wearing, last-write-wins, with no plausibility
   bound and no snapshot at the doff edge (`OpenXRManager.cpp:2169-2194`). Stand up, walk out while still
   wearing, then doff — and a 6 m displacement is written verbatim into the "memory". That also answers
   the user's parenthetical: *working* there was never required; **walking** there while wearing was
   enough. §3.3.

6. **The one signal that says "your world moved by an unknown amount" already exists — and the
   compositor is the only component that throws it away.** Upstream WiVRn drops the Quest's STAGE change
   at the client. **Our deployed fork does not**: `wivrn-xg` forwards it over the wire, corrects for it by
   parking the tracking origin when the pose is valid, and — when it is *invalid*, which is exactly the
   different-room case — pushes a STAGE `REFERENCE_SPACE_CHANGE_PENDING` so that *"content anchored in the
   room is at least known to be suspect rather than silently wrong"*. monado delivers it. Then
   `src/openxr/XRSession.cpp:416-417` filters on the space type we happen to have created and **discards
   it without so much as a log line.** The work is not "build a detector"; it is *stop dropping the one we
   already built*. §2.4, W4.

7. **The recommended model: two frames and a seat object.** A `world` frame (STAGE now, a stationary
   reference space later) and a `seat` frame (derived from the wearer at defined moments), with every
   binding naming its frame and carrying provenance, and hydration gated on the frame's resolution state.
   **This turns out to be the spec's own advice** — `XR_EXT_stationary_reference_space` §"composability"
   tells applications to *"use the `LOCAL` space location in `STATIONARY` reference space to reason about
   the recenter operation, or place the UI at a comfortable position in front of the user according to
   recentering."* Store world content in the room frame; locate the seat inside it. Hold and follow, from
   one pair of `xrLocateSpace` calls, with no policy conflict. §5.5, §6.4.

8. **Two shipping platforms converged on exactly the user's three modes, and put the choice on the
   object, not on a space.** Meta's window control bar offers *"follow me" / "theater view" / "pin to
   space"* (v77 body-lock, v81 world-pin); visionOS 26 locks windows to rooms via a snap gesture, exempt
   from recenter, and lets the same app be locked in several rooms. And Apple ships the user's
   "don't hydrate when unfindable" as a *structural default*: a RealityKit anchored entity is **inactive
   until anchored** and "might not show up in your scene at all". Our nearest Linux neighbour has landed
   in the same place from the other end: WayVR's `Positioning` enum is three world-ish modes that all
   "stay in place" and differ **only in what happens on recenter**, plus a separate body-follow axis with
   one lag knob — and its persisted transform is parent-relative and yaw-snapped while the live one is
   explicitly *not* saved. §4.1, §4.3, §4.14.

9. **Where the brief needs pushing back** (§7, six places): "true world placement" is achievable but
   *"unfindable" is currently undetectable*, so the generation token must come before the frame; a
   hierarchy of spaces is the right **coordinate** model and the wrong **policy** carrier — policy belongs
   on the binding, with frame-level defaults; **"after a period of inactivity" is not measurable** and
   should not be the trigger, because a doff is a total observational blackout — *re-seat when the
   evidence does not vouch for the place, hold when it does*, which makes today's `recenter = hold`
   default wrong (it holds a coordinate system, not a room); body-attached objects are already built and
   the gap is at the seam; and the bedroom bug is not bad data but **unlabelled** data.

10. **Order of work.** `M0` — measure whether the Quest still publishes a stable STAGE while our fork
    suppresses the boundary; this one attended session gates everything. Then, independent of it:
    `H1-H5` honest failure (check the `TRACKED` bits — **we check only `VALID`, everywhere**,
    `:1956-1957` — finiteness, commit-at-the-edge, provenance, and mint our own generation ID) and
    `S1-S2` the seat as an object with an authored distance. Then `W1-W5` the world frame, with `W4` —
    accept the STAGE event our own fork already sends — costing hours rather than days. **If only three
    things get built: W4, H2, S1** — the blindness under both symptoms, symptom A's likeliest cause, and
    symptom B exactly. §10.

11. **Runtime vs client** — the layering question, since the runtime may itself become a product (§5.8): the runtime owes a world frame with a
    stable identity and generation, honest locatability, a *true* `poseInPreviousSpace` on recenter
    (monado computes the exact delta and then hard-codes `pose_valid = false`,
    `u_space_overseer.c:874-876`), presence, and boundary geometry. The client owns layout, body-attached
    follow and its tuning, the seat, re-seat policy, hydration policy and the refusal UX. The test:
    *if two clients would legitimately want different answers it is client policy; if they would want the
    same answer and getting it wrong is a correctness bug, it is the runtime's.* If the runtime becomes a
    product, the flagship is `XR_EXT_stationary_reference_space` — four symbols, spec text that reads like
    this report's requirements document, a co-author who founded monado, headers already installed on
    this box — and **no runtime in the world ships it yet.**

---

## 1. Ground truth — what a "placement" is today

### 1.1 There is one frame, and the spec says it is allowed to move

`CXRSession::createSpaces` (`src/openxr/XRSession.cpp:265-293`) creates exactly two reference spaces:

```cpp
info.referenceSpaceType = m_usingLocalFloor ? XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR_EXT
                                            : XR_REFERENCE_SPACE_TYPE_LOCAL;   // :274
info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;                        // :277
```

`XR_REFERENCE_SPACE_TYPE_STAGE` **does not appear anywhere in `src/`**. Every world coordinate in the
system — `anchor:local pos:`, the adaptive desk seat, the 2D-plane reference, the viewpoint content
pose — is named against `m_refSpace`, i.e. against `LOCAL_FLOOR`.

That is the single most consequential line in the codebase, because of what OpenXR says those two
space types are *for*:

- **LOCAL / LOCAL_FLOOR** is the **app's seat**. The spec establishes it "when the application is
  started" or when the user **recenters**, and explicitly permits the runtime to move it, announcing
  the move with `XrEventDataReferenceSpaceChangePending`.
- **STAGE** is the **room**. It is defined by the play area, is floor-level, and its origin changes only
  when the *stage definition* changes — a boundary redraw, a Space re-setup, a switch to a different
  recognised space.

We asked for a seat and then spent a year trying to make it behave like a room.

### 1.2 A stored pose does not record which frame it was measured in

There is **no disk persistence of XR spatial state whatsoever** — re-verified at `67200a838`:
`rg -niE "XDG_STATE_HOME|\.local/state|ofstream|fopen\(|std::filesystem" src/openxr/` returns zero
hits. The only file read in the whole subsystem is `/proc/sys/kernel/tainted`
(`XRMonitorConfig.cpp:628-638`). Everything spatial is in-process and dies with the compositor.

Within a process, a monitor carries **three** different poses, in three different frames, and no field
anywhere says which frame any of them belongs to:

| field | frame | lives | survives |
|---|---|---|---|
| `CXRMonitorLayer::m_declaredAnchor.anchorPose` | *by convention* the wearer's rig for a config-declared monitor; *actually* dead-session `LOCAL_FLOOR` coordinates for an `openxr create` monitor (`OpenXRManager.cpp:2786-2801`) | layer | config reload |
| `m_anchor.state().anchorPose` (`SXRAnchorState`, `XRAnchor.hpp:213-235`) | means four different things by `mode`: `LOCAL_FLOOR` world / VIEW offset / body-frame offset / grip offset (`XRAnchor.hpp:217-222`) | layer | session restart |
| `m_restoreOffset` + `m_restoreValid` (`XRMonitorLayer.hpp:137-138`) | the wearer's yaw-only floor frame (`xrPoseInHeadFrame`, `XRMath.hpp:323-325`) | layer | session restart, **not** compositor restart |

Four consumers pick between them by three different rules — `layoutDump` prefers live for non-adaptive
LOCAL (`OpenXRManager.cpp:5747-5748`), `monitorInfos` prefers live when grabbed/gazing/roaming
(`:5617-5618`), the GROUP re-seat uses the **persistent** pose (`:2118`), the capture uses the
**persistent** pose (`:2192`). That inconsistency is a symptom, not the disease.

**The disease, in one sentence: every stored placement in HypXRland is a number without a frame, so it
is silently reinterpreted in whatever origin happens to be current.** "Restored way off in the bedroom"
is exactly what that sentence predicts.

### 1.3 The durable representation is head-relative, which is a *seat*, not a *world*

Because the only frame is one the runtime moves, the only thing that survives is a pose measured
against the one physical object that moves *with* the user:

```
offset = inv(xrHeadFrame(head)) ∘ anchorPose        // xrPoseInHeadFrame, XRMath.hpp:323-325
```

`xrHeadFrame` (`XRMath.hpp:310-312`) is `{pos.x, 0, pos.z}` + `qFromYaw(qYawOf(head.rot, 0))` — yaw-only,
floor-projected. It is reference-space independent (gtest `CaptureIsReferenceSpaceIndependent`), and that
is precisely why it cannot express "on that wall". **HypXRland today can only say *where relative to
you*. It has no vocabulary for *where in the room*.** The user's request (a) is therefore not a bug
report; it is a request for a representation that does not exist.

### 1.4 The capture is a live measurement, not a memory

`OpenXRManager.cpp:2169-2195`, frame thread, under `m_layersMu`, immediately after re-seat consumption:

```cpp
if (viewValid && m_restoreCapture.load(relaxed)) {
    headFrameInv = poseInverse(xrHeadFrame(viewPose));
    for (l : m_layers) { ... l->m_restoreOffset = poseCompose(headFrameInv, anchorPose);
                             l->m_restoreValid  = true; }
}
```

Gates: `viewValid`; `m_restoreCapture` = `sessionVisible() && (!presenceSupported || !presenceKnown ||
userPresent)` (`:4213-4214`); mode must be `LOCAL`; adaptive must be `DOCKED`; and the live pose must
have diverged bit-exactly from the declaration (`xrPoseIdentical`, `XRMath.hpp:332-334`).

Doc 03 states the consequence outright (`03-anchoring.md:588-593`): while the headset is worn, the
stored offset "is not a memory of where they *put* it, it is a live measurement of where it *is relative
to them*. It becomes a memory only when the gate shuts."

**Last write wins, and nothing validates the last write.** There is no plausibility check, no clamp, no
rejection of a large frame-to-frame delta, no snapshot-on-doff. §3 shows what that costs.

### 1.5 The event taxonomy, as implemented

| real-world event | what the code sees | distinguishable? |
|---|---|---|
| headset donned | `XrEventDataUserPresenceChangedEXT{true}` (`XRSession.cpp:403-412`) **and/or** session → VISIBLE | yes |
| headset doffed | presence `false` **and/or** VISIBLE drops. `wantXRMonitorsPlugged` requires **both** because WiVRn's presence *sticks* `present` in standby (`XRMonitorConfig.hpp:198-211`) | yes, with a caveat |
| doffed *and carried to another room* | nothing. Tracking is off; there is no odometry across a doff | **no** |
| user pressed recenter on the headset | `XrEventDataReferenceSpaceChangePending` for LOCAL + LOCAL_FLOOR | **not** distinguishable from a guardian re-derive or a re-don — doc 03 §8.1 says so, `ConfigValues.cpp:750-755` says so |
| guardian / Space re-setup, room switch | **nothing at all.** §2.3 | **no** |
| WiVRn client reconnect | our instance is lost → `stop()` → `start()` | not distinguishable from a compositor-side session recycle |
| `wivrn-server` restart | same path | no |
| compositor restart | fresh process | trivially yes, and everything spatial is gone |
| tracking loss / recovery | `viewValid` false for some frames. **`XR_SPACE_LOCATION_*_TRACKED_BIT` is never checked anywhere in the tree** — only the `VALID` bits (`OpenXRManager.cpp:1956-1957`, `XRInput.cpp:344`) | **no** — an extrapolated or last-known pose from a doffed headset is accepted as truth |
| "a period of inactivity" | not represented at all | no |

Session states are collapsed further: `mapSessionState` (`OpenXRManager.cpp:1153-1160`) folds
`IDLE / READY / SYNCHRONIZED / STOPPING` into one `RUNNING_IDLE` value.

### 1.6 The primitives that exist

Four persistent anchor modes (`XRAnchor.hpp:19-24`): `LOCAL` (world pose in LOCAL_FLOOR), `HEAD` (offset
in VIEW space, spring + deadzone leash), `BODY` (offset in a yaw-only body frame at a stored
`bodyHeight`), `DEVICE` (offset in a controller grip space, submitted in the grip `XrSpace` so the
runtime late-latches it). Plus four runtime overrides checked in order inside `solve()`
(`XRAnchor.cpp:215-307`): move-grab, gaze-carry, the **adaptive decorator**, then the mode.

The adaptive decorator (`XRAnchor.cpp:483-571`) is a `DOCKED → UNDOCKING → ROAMING → REDOCKING → DOCKED`
machine over a hysteretic geofence (`adaptive_leave_radius` 1.5 m / `adaptive_return_radius` 1.0 m) with
dwell timers (400/800 ms), an eased transition (700 ms), a `roam:head|body` mode and an
`m_adLeftSinceDock` latch so a manual undock at the desk stays roaming. Its "seat" is `m_dockHeadPos`,
captured on the first `DOCKED` frame with a valid view (`:496-499`) and **not persisted**.

Two kinds of re-seat, ordered `NONE < GROUP < RESTORE` by a CAS-max arming (`OpenXRManager.cpp:4228-4237`):

- **RESTORE** — first plug of a session, or the §8.1 tracking-gap fallback. Plants each monitor's stored
  head-relative offset, or its declared rig if there is none (`xrReseatSource`, `XRAnchor.hpp:258-262`).
- **GROUP** — `xrmonitor reseat` (SUPER+CTRL+Home), or every reference-space change under
  `openxr:recenter = follow`. Moves the **live** arrangement rigidly onto the current head via a seat
  frame derived from the group itself (`xrGroupSeatFrame`, `XRAnchor.hpp:320-359`).

The verbs and knobs: `openxr:recenter = hold|follow`, `openxr:recenter_on_plug` (default 1),
`openxr:monitors_follow_session = off|session|visible` (default `visible`, plus
`monitor_plug_settle_ms` 1500 and `monitor_unplug_grace_ms` 20000), `openxr:default_distance` 1.5,
`openxr:floor_offset` 1.5 (only when `XR_EXT_local_floor` is missing).

**Read that inventory again with §1.3 in mind.** `HEAD`, `BODY`, `DEVICE` and the adaptive decorator are
a complete, shipped, live-validated implementation of the user's request (b), *body-attached objects*.
The user's request (c), *re-seat on don*, is `recenter_on_plug` + RESTORE, also shipped. It is only
request (a) — **true world placement** — that has no implementation, and it has none because it has no
representation. Everything else in this system is a workaround for its absence.

---

## 2. The runtime beneath us — what WiVRn/monado actually does

This section is new evidence; report 22 audited the *extension* surface and found it empty, which is
still true, but did not trace what happens to the origin.

### 2.1 The chain

```
Quest OpenXR runtime  ──STAGE poses──▶  WiVRn client APK  ──wire──▶  wivrn-server
      │                                                                    │
      │                                                       xrt_device (wivrn_hmd)
      │                                                                    │
      └────────────────────────────────────────────────  monado space overseer  ──▶ HypXRland
```

- The client creates its world space **once**, as STAGE:
  `spaces[world] = xr_session.create_reference_space(XR_REFERENCE_SPACE_TYPE_STAGE)`
  (`client/application.cpp:1351`). Every pose it streams is in that space, for the life of the process.
- The server builds a space overseer per session with
  `t_builder_create_space_overseer_legacy(..., root_is_unbounded=false, per_app_local_spaces=false, ...)`
  (`server/driver/wivrn_session.cpp:298-310`).
- In monado, `u_builder_create_space_overseer_legacy` sets `T_stage_local = {identity, y = 1.6}`
  (`u_builders.c:216-227`). `u_space_overseer_legacy_setup` then makes STAGE a **null space equal to
  root** when the head device does not advertise `supported.stage` — and WiVRn's HMD does not
  (`u_space_overseer.c:1266-1273`; no `supported.stage` or `XRT_INPUT_GENERIC_STAGE_SPACE_POSE` anywhere
  in WiVRn's server). LOCAL is root offset by `+1.6 m` in Y; LOCAL_FLOOR is root offset by LOCAL's
  X/Z/yaw with `y = 0` (`u_space_overseer.c:1281-1290`).

**Therefore, in every WiVRn session on this box: `XR_REFERENCE_SPACE_TYPE_STAGE` == root == the Quest's
own STAGE origin, exactly.** And at session start, before any recenter, `LOCAL_FLOOR == STAGE`.

### 2.2 Recenter moves LOCAL and LOCAL_FLOOR. It does not move STAGE.

`xrt_space_overseer_recenter_local_spaces` (`u_space_overseer.c:~830-895`) recomputes the LOCAL and
LOCAL_FLOOR **offsets** from the current head (yaw only — it zeroes the quaternion's x and z, which is a
projection rather than a yaw extraction), writes them, and pushes the change event for
`XRT_SPACE_REFERENCE_TYPE_LOCAL` **and** `LOCAL_FLOOR`. STAGE is untouched.

Two consequences we have been paying for without naming:

1. **The event carries nothing.** `xse.ref_change.pose_valid = false;
   xse.ref_change.pose_in_previous_space = XRT_POSE_IDENTITY;` — hard-coded, immediately after the exact
   delta was computed (`u_space_overseer.c:874-876`). The compositor reconstructs it from a head pair
   (`solveReferenceSpaceChangeFromHead`, `XRMath.hpp:264-273`), which needs a tracked head sample within
   500 ms on *both* sides of the swap and yields the identity — silently — when the pre-change sample
   was already taken in the new space.
2. **A "recenter" on the PC side is not the Quest's recenter.** The Quest emits its own
   reference-space change; the WiVRn client turns that into a bare boolean
   (`client/scenes/stream.cpp:1299-1302` → `recenter_requested`), the server sees a
   `tracking::recentered` flag and calls monado's recenter (`server/driver/wivrn_session.cpp:596-601`),
   and monado then re-derives LOCAL **from wherever the head is at that instant on the PC side**. The
   headset's delta and the PC's delta are two different transforms that merely happen to correlate.

### 2.3 The Quest's STAGE change is dropped on the floor — twice

The spec requires the runtime to queue `XrEventDataReferenceSpaceChangePending` when the user redefines
the stage's origin or bounds, or when the runtime switches to a new stage definition. On the Quest that
covers Space re-setup, a boundary redraw, and re-recognising a different saved Space. The WiVRn client
receives that event and:

```cpp
// client/application.cpp:1932
case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
        break;

// client/scenes/stream.cpp:1299-1302
case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
        if (event.space_changed_pending.referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL)
                recenter_requested = true;
        break;
```

**It is filtered to LOCAL. STAGE — the space every streamed pose is expressed in — is ignored
entirely.** So when the Quest re-establishes its stage, the whole world silently slides underneath the
PC, no event reaches monado, and nothing downstream can know. The pose numbers were fine; the frame they
were measured in had been swapped out from under them.

**But that is the *upstream* fork, and it is not what runs here.** §2.4.

### 2.4 The deployed fork already fixes two thirds of this — and we drop the third

The runtime actually in the fishfood session is `~/code/wivrn-xg` (branch `hypxr`, the "one branch that
builds both deployed halves"), not `~/code/wivrn`. It carries the XG stage-correction work — landed
`c8910153` (2026-08-03), corrected `38a8cdfe` (2026-08-17) after the periphery-clip investigation — and
it does the right thing at both ends of the wire:

- **Client** (`wivrn-xg/client/scenes/stream.cpp:1329-1359`) no longer filters to LOCAL. On a STAGE
  change it logs the delta (or warns loudly that the pose is invalid) and sends a new control packet
  `from_headset::reference_space_changed { space, change_time, pose_valid, pose_in_previous_space }`
  (`common/wivrn_packets.h:628-634`).
- **Server** (`wivrn-xg/server/driver/wivrn_session.cpp:873-960`) composes the deltas into a
  `stage_correction` — *"maps the client's CURRENT stage frame to the stage frame this session started
  in"* — and **parks the WiVRn tracking origin** at that pose via
  `xrt_space_overseer_set_tracking_origin_offset`, so head, controllers, hands and body trackers all move
  together and *"application content stays where the room put it"*. Ingress only: the egress half was the
  spurious `C⁻¹` that rotated the streamed layer and produced the 5-10 % right-periphery passthrough
  wedge, and `38a8cdfe` deleted it unconditionally. It is explicitly dead reckoning
  (`wivrn_session.h:112-132`) and is reset to identity on reconnect (`:1901`, "headset reconnected").

So for a STAGE change **with a valid pose, mid-session, on one unbroken connection**, the deployed stack
already holds the room. That is a real and underappreciated piece of engineering, and it means monado's
root here is not the raw Quest stage but a *stabilised* one: **"the stage frame this session started
in"**. Better than what §2.2 alone implies.

**And when the pose is invalid — which is exactly the case that matters, because a switch to a different
recognised Space has no relatable origin — the server does the honest thing:**

```cpp
// wivrn-xg/server/driver/wivrn_session.cpp:894-912
U_LOG_W("headset STAGE reference space changed with an INVALID pose: the streamed world "
        "has shifted by an unknown amount and cannot be corrected");
xrt_session_event ev{ .ref_change = { .event_type = XRT_SESSION_EVENT_REFERENCE_SPACE_CHANGE_PENDING,
                                      .ref_type   = XRT_SPACE_REFERENCE_TYPE_STAGE,
                                      … .pose_valid = false, } };
xrt_session_event_sink_push(&xrt_system.broadcast, &ev);
```

…with the comment *"still tell the applications, so that content anchored in the room is at least known
to be suspect rather than silently wrong."* monado forwards it to every session regardless of which
spaces that session created (`oxr_session.c:150-200`).

**HypXRland throws it away.**

```cpp
// src/openxr/XRSession.cpp:416-417
const auto ourType = m_usingLocalFloor ? XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR_EXT : XR_REFERENCE_SPACE_TYPE_LOCAL;
if (ev->referenceSpaceType == ourType) { … }        // …and otherwise: nothing. Not even a log line.
```

**The one signal the entire stack produces that means "your world has moved by an unknown amount" is
generated correctly by our own fork, delivered correctly by monado, and discarded silently by the
compositor at the last hop.** That is the single most actionable line in this report. The work is not
"build a detector"; it is "stop dropping the one we already built."

### 2.5 A reconnect resets the recenter offset to identity, silently

`per_app_local_spaces = false` and a fresh space overseer per session mean the accumulated LOCAL /
LOCAL_FLOOR offset is **global to the WiVRn session** and **reset to identity on reconnect**. So a
`wivrn-server` restart teleports LOCAL_FLOOR back by the whole accumulated recenter offset — and because
our XR instance is lost and rebuilt at the same moment, it does not even look like a change. Anything
that crossed that boundary holding LOCAL_FLOOR coordinates (the frozen `m_declaredAnchor` of an
`openxr create` monitor; a `m_restoreOffset` captured from a poisoned pose) is now wrong by exactly that
offset. This is the machinery behind the 2026-08-16 report of monitors "spun way off, outside my house".

**The stage correction resets at the same boundary**, and for the same reason: it is dead-reckoned, so a
gap in observation destroys it. A STAGE change that happens *while disconnected* — the user carries the
headset to another room and reconnects there — is therefore unobservable by construction, in the fork
and in the compositor alike. **No amount of client work fixes that one; only a frame identity does.**
§6.2, §5.5.

### 2.6 What the runtime does and does not give us — definitive

| capability | available today over WiVRn/monado? | evidence |
|---|---|---|
| `VIEW`, `LOCAL`, `LOCAL_FLOOR` | **yes** | `XRSession.cpp:86-95`, `:265-293` |
| `STAGE` | **yes, and on the deployed fork it is a *stabilised* room origin** | enumerated whenever `xso->semantic.stage != NULL` (`oxr_system.c:334-336`), and the legacy builder always creates it — as a *pose* space if the head driver advertises `supported.stage`, else as a **null space equal to root** (`u_space_overseer.c:1266-1273`). WiVRn's HMD sets neither `supported.stage` nor `XRT_INPUT_GENERIC_STAGE_SPACE_POSE`, so **STAGE == root**; and root is fed by the client's poses, which are streamed in the Quest's own STAGE (`client/application.cpp:1351`). Resolution at `oxr_space.c:113-118`. On `wivrn-xg` the tracking origin is additionally *parked* so root stays "the stage frame this session started in" across in-session STAGE changes (§2.4). |
| STAGE immune to recenter | **yes** | `u_space_overseer.c:~830-895` offsets local + local_floor only. Independently confirmed by the platform owner: *"On Quest, the Stage tracking origin will not directly respond to user recentering"* (§4.2). |
| `UNBOUNDED_MSFT` | **no** | WiVRn passes `root_is_unbounded = false` (`wivrn_session.cpp:298-310`) → `semantic.unbounded == NULL` → not enumerated (`oxr_system.c:338-342`) |
| the `OXR_RECENTER_STAGE` quirk (aliases STAGE→LOCAL_FLOOR, `oxr_instance.c:57`, `:238`) | **not set on this box** | absent from `~/.config/**`, `/etc/environment`, `~/code/hypxr-pkgs`, `~/code/wivrn`, and every `env =` line in the hypr config |
| notification when STAGE changes | **yes on the deployed fork — and *we* drop it** | `wivrn-xg/client/scenes/stream.cpp:1329-1359` forwards it; `wivrn-xg/server/driver/wivrn_session.cpp:873-960` corrects it, or pushes a STAGE `REFERENCE_SPACE_CHANGE_PENDING` when the pose is invalid; monado delivers it to every session (`oxr_session.c:150-200`); **`src/openxr/XRSession.cpp:416-417` discards it with no log**. Upstream `~/code/wivrn` still drops it at the client (§2.3). |
| a usable delta on a LOCAL recenter | **no** — always `pose_valid = false` | `u_space_overseer.c:874-876` |
| play-area extents (`xrGetReferenceSpaceBoundsRect`) | **no** — WiVRn implements no `get_reference_bounds_rect` | no hit in `server/`; monado forwards to the compositor (`oxr_space.c:193-215`) |
| boundary polygon / guardian geometry | **no** — not in the wire protocol | `common/wivrn_packets.h` |
| any anchor / spatial-entity / persistence extension | **no** — zero, at every layer | report 22 §3.1, re-verified |
| room / Space identity (UUID) | **no** | — |
| a stage-change delta while *disconnected* | **no, by construction** — the correction is dead-reckoned and reset on reconnect | `wivrn-xg/server/driver/wivrn_session.h:112-132`, `wivrn_session.cpp:1901` |
| `XR_EXT_user_presence` (don/doff) | **yes**, forwarded end to end | `wivrn_packets.h:525`, `client/scenes/stream.cpp:1327`, `server/driver/wivrn_session.cpp:757` |
| `XR_EXT_local_floor` | **yes** | `XRSession.cpp:86-95` |
| out-of-band origin control | **yes** — libmonado `mnd_root_set_reference_space_offset` (report 22 §4.2) | `/usr/share/openxr/1/openxr_wivrn.json` |

**Two lines matter.** First: **a genuine room-anchored frame is one `xrCreateReferenceSpace` call away
and we have never made it** — immune to recenter, immune to re-don, stabilised across in-session stage
changes by our own fork, and surviving a WiVRn reconnect whenever the Quest's stage is unchanged.
Second: **the honest "your world moved and I cannot say by how much" signal already exists end to end and
the compositor is the only component that throws it away** (§2.4). Neither of those is a research
programme; both are small, and between them they cover three of the four events that have been throwing
monitors across the house.

**One risk gates this and must be measured before anything is built on it (§9, WP-M0):** the user's
WiVRn fork suppresses the Quest boundary (`BOUNDARYLESS_APP` manifest feature, `ddb5a8ce`; the
upstreamable successor is `XR_META_boundary_visibility`, researched in `hypxr/GUARDIAN-DISABLE.md`,
`e54b56fe`). Neither that research nor anything else in the tree establishes whether the Quest still
publishes a stable, room-persistent STAGE when the boundary is suppressed, or falls back to a
session-arbitrary origin. If it is the latter, STAGE is worth exactly as much as LOCAL_FLOOR and the
recommendation in §7 loses its cheapest rung. **The measurement is two log lines and one session.**

---

## 3. The bug catalogue, and the open one dissected

### 3.0 Everything we have hit, in order

Reconstructed from `docs/openxr/03-anchoring.md` §8, `research/22` §1, the commit history and the
fishfood findings log. Every entry is the *same* defect wearing a different coat: a pose whose frame
changed underneath it.

| when | symptom (user's words where recorded) | root cause | disposition |
|---|---|---|---|
| ongoing, measured 2026-08-03 | the declared `xrmonitor` line and `hyprctl openxr layout` disagreed by **≈3.1 m and 131° of yaw** for the same monitor — *"not drift; a different coordinate system"* (report 22 §1) | `anchor:local` means "relative to whatever `LOCAL_FLOOR` was at `xrCreateSession`", and that origin is arbitrary under WiVRn boundaryless/standby | still true; §1.1 |
| — | *"monitors thrown in a random direction across the room"* on a mid-session recenter; one session logged the head frame moving **8.25 m and ~155°** with nothing applied to any anchor | monado always sends `pose_valid = false`, so the old, conservative handling had no delta to apply and left every anchor in a dead frame | fixed `d8aedec6e` (reconstruct `M` from a head pair) + `8a33dfae2` (why call order, not `changeTime`, is the ground truth) |
| 2026-08-16 | ad-hoc monitors *"spun way off, outside my house"* after a session restart — **the monitor lottery** | an `openxr create` monitor's "declared" anchor is the world pose it was created at, in a `LOCAL_FLOOR` that no longer exists; a re-seat composed those dead coordinates into the head frame. Log arithmetic: session one latched at eye `[4.23, 1.04, 5.75]`, a mid-session recenter then moved the origin by 7.13 m, re-expressing the live anchors but **not** the frozen declared copies | fixed by the head-relative capture — `0b29106f3`, `97d946879`, `3b4ce808e`, `4bd9a3fa3`; §1.4 |
| 2026-08-17 | recurring *"5-10 % right periphery shows passthrough after don"* — **the stage-correction saga** | not FoV at all: `wivrn_session::n`, the XG stage correction, was applied a second time on **egress** (stamped pose × C⁻¹). The stamp is already C-free on both paths, so the "undo" *was* the rotation; it accumulated across reference-space changes, which fire at don, and rotated the streamed world-space layer away from the head until a `wivrn-server` restart cleared it. The bisection is worth keeping: app-session recycle no effect, doff/re-don no effect, WiVRn-app reconnect no effect, **server restart clears** | fixed `38a8cdfe` in `wivrn-xg` (egress deleted unconditionally, plus `reset_stage_correction()` at reconnect) |
| 2026-08-17 | *"the Quest's recenter pressed over and over with the monitors staying put… If I pivoted only slightly, everything would land exactly where it already was… we really need a better approach here"* | under `recenter = hold` the ladder correctly holds the room, so the headset's recenter button correctly does nothing to the monitors — a control that moves an origin the user cannot see. And a re-seat built on the stored offsets is `hf(H) ∘ inv(hf(H)) ∘ pose`, the identity | fixed `93d88ff99` (`xrmonitor reseat`) + `4ca9b75ea` (why there must be **two** kinds of re-seat) + `c09d34f4d` (doc 03 §8.4) |
| **2026-08-20** | **open.** See below | see below | **open** |

Two structural observations fall out of the table. First, **every fix has been a compensator applied at
the point of failure**, and each was correct; the failures kept coming because none of them could address
the missing representation (§1.3). Second, the *doff→don inside a live session* case has never been
addressed at all: `m_recenteredThisSession` latches once per session and WiVRn keeps the session alive on
the shelf, so *"carry the headset to the couch and put it back on"* produces **no edge and no re-pose**
(report 22 §1). That is the other half of the complaint, and the half a seat frame is really for.

### 3.1 The open bug

Two symptoms were reported on 2026-08-20 and parked with the verdict *"fundamentally broken… attack
another day"*:

- **A.** monitors *"initially restored WAY off in the bedroom (a spot user never worked from)"*;
- **B.** *"after wivrn service restart + SUPER+CTRL+Home reseat: group landed in FRONT of user but far in
  the distance"* — the user then manually grabbed and pulled them closer.

The parked note asks one question directly: *"reseat centroid/depth derivation wrong — distance clamp
0.3-5 m should have prevented this? check whether clamp applies to seat frame vs per-monitor offsets"*.

**Answer: the clamp applies to the seat frame, and it is the cause rather than the prevention.** §3.2.

These are not two bugs. **A poisons the stored geometry; B is the re-seat faithfully preserving the
poisoned geometry's distance, clamped.** Every step below is in the code today.

### 3.2 B is a designed behaviour, and that is the problem

`xrGroupSeatFrame` (`XRAnchor.hpp:320-359`) derives where the group's implied viewer stands:

```cpp
const Vec3  toHead{head.pos.x - centroid.x, 0.F, head.pos.z - centroid.z};
const float dist = std::clamp(std::fabs(toHead.x * normal.x + toHead.z * normal.z), minDist, maxDist);
//                                                                    XR_DISTANCE_MIN = 0.3
//                                                                    XR_DISTANCE_MAX = 5.0
```

**The seat distance is the distance the group currently happens to be from you.** Re-seating a group
that a bad restore parked 20 m away computes 20, clamps to 5.0, and plants it **exactly 5 metres in
front of your face**. `tests/xr/anchor_math.cpp:1274-1288` asserts this on purpose. A 2.23 m-wide monitor
at 5 m is "waaaay off in the distance", verbatim. And because the re-seat is *rigid*, a group that was
spread over 10 m of bad geometry stays spread over 10 m: individual monitors land well beyond 5 m, and
there is no per-monitor distance normalisation anywhere in the path.

The design intent was a fixed point — press again and nothing moves (`03-anchoring.md:707-715`) — and it
achieves that. But the fixed-point property was bought by making the verb **inherit the state it is
supposed to repair**. `reseat` has no notion of an *authored* distance to return to, because **the group
is not an object**. It is re-derived from its children's live poses every time it is invoked. There is
nothing that remembers "this arrangement was authored to be viewed from 1.4 m".

That is the architectural defect, stated as a fact about the code: *the parent transform is emergent,
not stored.* `research/LAYOUT-AND-NAMING.md` already identified the missing primitive, and report 22
§2.3 already recommended building it. It is still unbuilt.

### 3.3 A has three sufficient causes, and they compose

Ranked by plausibility; any one of them produces the symptom, and they are not mutually exclusive.

**#1 — The capture records the walk-away, not the workspace.** `OpenXRManager.cpp:2169-2194`.
`m_restoreOffset` is re-derived **every frame** while `sessionVisible() && wearing`. Stand up, walk out
of the room while still wearing, *then* doff — and the last frames captured are of you standing
somewhere else facing something else. A 6 m displacement and a 180° yaw are written verbatim into the
"memory" with **no plausibility check, no clamp, no rejection of a large frame-to-frame delta, and no
snapshot at the doff edge**. The next restore replants exactly that.

This also explains the parenthetical the user added — *"never worked from there"*. Working there was
never required. **Walking there while wearing was enough.** The capture cannot tell the difference
between "I have moved my desk" and "I am carrying the headset to the kitchen", because those are the
same measurement.

Sub-case: the frames *between* the physical doff and the visibility/presence drop are the removal motion
— headset tilted, rotated, lowered, set down. That is precisely when `qYawOf`'s near-vertical fallback
(`XRMath.hpp:170-175`) can fire and stamp a yaw of **0** in the runtime's arbitrary frame.

**#2 — A frame change that nothing observed correctly.** Three concrete holes in the ladder
(`OpenXRManager.cpp:1978-2029`):
- **Ordering race in `SOLVE_FROM_HEAD`.** `headOld` is the *previous* iteration's locate. If the runtime
  installed the new origin before that locate and only delivered the event this iteration, `headOld` is
  already in the new space, `M ≈ identity`, `xrRecenterIsNoOp` fires, and every anchor keeps coordinates
  in a dead frame — while logging that the change was the identity. Historically measured origin jumps
  are 7.13 m and 8.25 m (`XRMath.hpp:241-248`, `03-anchoring.md:566-570`).
- **`RESEAT_TO_HEAD` refused.** With `recenter_on_plug = 0` under `hold`, `:2013-2017` WARNs and leaves
  anchors in a dead frame — and **the capture is not suppressed for those frames.**
- **Silent type filter — and it is not hypothetical.** `XRSession.cpp:416-417` drops a change announced
  for a reference-space type we did not create, with no log at all. monado pushes both LOCAL and
  LOCAL_FLOOR, so the recenter path survives; but the deployed `wivrn-xg` server pushes a **STAGE**
  change whenever the headset's stage moved by an amount it could not compute (§2.4), and that one is
  discarded. **The stack's only "your world is now suspect" message dies at this line.**

**#3 — The dead-frame declared fallback is still reachable.** `:2149` + `:2801`. Whenever
`m_restoreValid` is false for an `openxr create` monitor — the mode was switched away and back, a
declared-anchor reload cleared it (`:5454`), or the session ended before the capture ever validated —
`recenterLocalToHead` composes `xrHeadFrame(head) ∘ deadWorldPose`. That is the original monitor lottery,
and its magnitude is the whole origin offset.

Underneath all three sits §2.3: if the Quest re-established its stage while we were not looking, *every*
pose in the process is in the wrong frame and every repair mechanism above is repairing the wrong thing.

### 3.4 Systemic gaps the dissection exposes

These are not the proximate cause of one bug; they are the reason bugs of this class keep recurring.

1. **`XR_SPACE_LOCATION_POSITION_TRACKED_BIT` / `ORIENTATION_TRACKED_BIT` are never checked anywhere in
   the tree.** `OpenXRManager.cpp:1956-1957` and `XRInput.cpp:344` test only the `VALID` bits. OpenXR
   distinguishes "this pose is meaningful" (VALID) from "this pose is being actively tracked right now"
   (TRACKED) exactly so an app can refuse to trust an inferred or last-known pose. We accept an
   extrapolated pose from a doffed, hand-held, or tracking-lost headset as ground truth — **including as
   the input to the capture that becomes next session's memory.**
2. **No finiteness checks.** `xrToPose` (`XRSession.hpp:35-37`) is a raw field copy; nothing between the
   runtime and `m_restoreOffset` tests `isfinite`. `springStep` and `qSlerp` both propagate NaN, and
   `serializeXRMonitorLine` would happily emit `nan`.
3. **`m_lastVerbCtx` is never reset** (`OpenXRManager.cpp:2086`, read at `:5360-5363`). After a session
   dies it retains `viewValid == true` with the previous session's head pose forever, so `openxr create`
   / `center` / `place` while the session is down place against a dead frame — and seed
   `m_restoreOffset` from it.
4. **Near-cancelling group normals.** `xrGroupSeatFrame` rejects `nl < 1e-3`, but `nl` between 1e-3 and
   ~0.05 passes validity and yields an arbitrary `normal`, hence an arbitrary re-seat direction. Reachable
   on a wrap-around layout.
5. **The stored restore offset carries a full quaternion** (`:2192`) while the re-seat is yaw-only and
   `serializeXRMonitorLine` drops roll entirely (`XRMonitorConfig.cpp:735-742`). The in-memory restore
   and the config round-trip disagree about what a placement *is*.
6. **Absolute heights cross a re-derived floor.** Both `xrHeadFrame` and the seat frame are built at
   `y = 0`, so heights pass through absolutely. If one session ran on `LOCAL_FLOOR` and another on the
   `LOCAL` + `floor_offset = 1.5` fallback, a restored monitor is off by up to 1.5 m vertically, and
   nothing records which regime the number came from. (Another instance of §1.2.)
7. **No introspection of provenance.** `hyprctl openxr status` prints `restore [x,y,z] (head-relative)`
   (`XRIpc.cpp:267-269`) but not *when* it was captured, not the `m_declaredAnchor` alongside it, and not
   the seat frame the next `reseat` would compute. Symptom B would have been self-diagnosing in one
   command if the last of those existed.

### 3.5 Why this is hard — the honest list

Distilled from the above, and this is the list any new model has to answer to:

1. **We reason in a frame the spec permits the runtime to move, and the runtime moves it without telling
   us how much.** (§1.1, §2.2)
2. **Stored poses do not name their frame,** so a frame change silently changes their meaning. (§1.2)
3. **The one durable representation is head-relative,** which by construction cannot express "on that
   wall". (§1.3)
4. **Don and doff bound a period of total observational blackout.** No odometry, no tracking, no
   presence-of-place. Any inference about what happened in between is a guess with no error bound. (§1.5)
5. **Deliberate and involuntary origin changes are indistinguishable** at the API. (§1.5)
6. **The parent transform is emergent, not stored,** so group-level operations inherit corrupted state
   instead of repairing it. (§3.2)
7. **The system never refuses.** There is no representation of "I don't know where this goes", so every
   path ends in a guess, and a guess at 5 m looks exactly like a bug. (§3.2, §3.4)
8. **The evidence budget is zero.** No anchors, no room identity, no boundary, no play area. Whatever
   the model is, it has to work when the only sensor is a head pose. (§2.6)

---

## 4. Prior art — what shipped systems actually do

Two independent platforms spent four years converging on the same answer, and it is the user's three-part
request, exactly. That convergence is the strongest argument in this report that the shape is right.

### 4.1 The convergence: three placement modes, chosen per surface, at the surface's own chrome

**Meta Horizon OS** puts it on the window control bar. Meta's own design documentation lists the actions
as *"**'follow me,' 'theater view,' 'pin to space,'** and 'close'"*
(https://developers.meta.com/horizon/design/windows/), and the release notes date each rung:

| version | date | shipped |
|---|---|---|
| v67 / v69 | 2024-07…09 | free window placement, three hinged panels + up to three free-placed |
| **v77** | 2025-05 | **"Move with you windows — You can now select windows that follow you around as you move. This update removes the need for you to constantly reset your view each time you move around the room."** |
| **v81** | 2025-10 | **"Pin Windows to Worlds — You can now pin windows to specific locations in MR and VR Home through the control bar pin."** |
| v2.1 | 2026-02 | "Apps, windows, and panels now automatically **snap and align to nearby walls**" |

(https://www.meta.com/en-us/help/quest/articles/whats-new/release-notes/)

**Apple visionOS** puts it on a gesture. visionOS 1.0 windows float in a device-relative frame that
survives doff/don but nothing else; visionOS 2.0 added relative restore across reboot behind Settings ›
General › *Reopen Apps After Restart*; **visionOS 26 (2025-09) added room-locked persistence**:

> "people can now persist windows, volumes, and even the new widgets by **locking them to particular rooms
> in their physical surroundings** … **These locked windows are tied to the room they were used in. Come
> back to that room at a later time, and the windows spring back to life.**"
> — WWDC25 session 290, https://developer.apple.com/videos/play/wwdc2025/290/

> "The app remains where you lock it, **even if you re-center your space**." … "You can lock the same app
> in multiple rooms … **When you walk in either room, the app appears right where you locked it.**"
> — https://support.apple.com/en-us/118515 ,
> https://support.apple.com/guide/apple-vision-pro/move-resize-and-close-app-windows-dev009366408/visionos

The opt-in is the **snap gesture**, not a settings toggle: "People can snap windows and volumes to their
physical environment … by **gently moving the window close to the surface**. **For restorable windows, this
is what locks them in place for persistence.**" Unlock is "drag it away from its locked position".

**Three lessons for us.** (i) The mode is a **per-surface** property with a **direct-manipulation**
affordance, not a config key buried in a file — and we have exactly the wrong shape today, where
`anchor:local` is a config token and `adaptive:on` is a decorator. (ii) Free placement shipped *years*
before world-locking on both platforms; the world-locked tier is the hard part everywhere, not just here.
(iii) **Locked beats the global setting** — Apple: "If you've locked an app in place, it will reappear
after your Apple Vision Pro restarts, **even if you've turned off Reopen Apps After Restart**." A
per-object policy overrides a global one, which is §7.2's argument in shipped form.

### 4.2 Meta confirms the STAGE finding in its own developer documentation

> "**On Quest, the Stage tracking origin will not directly respond to user recentering.**"
> — https://developers.meta.com/horizon/documentation/unity/unity-ovrcamerarig/

Same page: recenter resets x/y/z to origin and "**the y rotation is reset to 0, but the x and z rotation
are unchanged** to maintain a consistent ground plane" — i.e. a yaw-only re-plant of LOCAL/LOCAL_FLOOR at
the current head. Anchors are untouched. This is independent vendor confirmation of §2.2 and §2.6: on
this hardware, **STAGE is the room frame and LOCAL is the seat frame**, and the platform owner says so.

The v77 release note is also a tell about the shell's own history: follow-me windows "removes the need for
you to **constantly reset your view each time you move around the room**". That sentence only parses if
pre-v77 shell windows lived in LOCAL and Reset View was the normal way to retrieve them. Meta's shell had
our bug, and fixed it by adding a body-locked mode and then a world-locked one.

And the counter-consideration, stated plainly because it is a real design cost: **if world content lives
in STAGE, the headset's recenter button will not gather it** — which reads as a bug to a Quest-native
user. That is not an argument against STAGE; it is the argument for *per-object* policy (§7.2) and for
`xrmonitor reseat` as a first-class verb (§5.7).

### 4.3 "Do not hydrate when the world is unfindable" is shipped, by Apple, as the default

RealityKit's `AnchoringComponent` implements the user's request (a) as a structural property:

> "The entity with `AnchoringComponent` is **inactive when created**. RealityKit anchors and **activates**
> the entity when it finds an anchor that meets the target requirements … Similarly, RealityKit
> **unanchors the entity if the target disappears** or no longer meets the target requirements."
> — https://developer.apple.com/documentation/realitykit/anchoringcomponent

> "Some anchor entities **might not show up in your scene at all** if RealityKit fails to detect an
> appropriate place for them." — https://developer.apple.com/documentation/realitykit/anchorentity

`AnchoringComponent.TrackingMode` adds the policy axis we would need: `.continuous` "**hides the entity
when the target is no longer in frame**", `.once` "Anchors the entity to the target on the **first frame**
the target is found", `.predicted`.

And Apple's failure doctrine is explicit: **hide, never fallback-place.**

> "Some types of anchors are also trackable. **When a trackable anchor is not being tracked, you should
> hide any virtual content that you have anchored with it.**" — WWDC23 10082,
> https://developer.apple.com/videos/play/wwdc2023/10082/

Meta's guidance takes the other branch, and the contrast is worth putting in front of the user because it
is the one genuinely contested design decision in this whole survey:

> "**If you have saved content + anchor UUID, and the anchor can no longer be found, then prompt the user
> to reposition the content (or auto-reposition it using the scene).**"
> — https://developers.meta.com/horizon/documentation/unity/unity-spatial-anchors-best-practices/

**Apple hides. Meta re-places, with a prompt.** For a *desktop* — where an unhydrated monitor means
workspaces with nowhere to go — the honest synthesis is Apple's default with Meta's escape hatch: refuse
by default, offer one unambiguous verb to re-place, and never re-place silently.

### 4.4 Rooms are a first-class, bounded, best-effort store on both platforms

**Apple** ships `RoomTrackingProvider` (visionOS 2.0+): `currentRoom: RoomAnchor?`, `roomAnchors`,
`anchorUpdates`; `RoomAnchor` carries a UUID, `isCurrentRoom`, geometry and `contains(_ point:)`.

> "ARKit can recognize transitions between rooms. When you enter a new area, it will switch to delivering
> data for the space that you now occupy." — WWDC24 10100,
> https://developer.apple.com/videos/play/wwdc2024/10100/

Crucially **`currentRoom` is optional** — populated only "if ARKit determines that you're in a confined
space". An honest three-state, not a guess. Underneath, the map itself is location-keyed and swapped:

> "**Maps are location based**, so when you take your device to a new location — for instance, from home to
> the office — the map of your home will be unloaded, and then a different map will be localized for the
> office … Upon returning home, ARKit will recognize that the location has changed, and we will **begin
> the process of relocalizing** … **If we find one**, we will localize with it, and all of the anchors that
> you previously added at home will become **tracked once again**." — WWDC23 10082

**Meta** ships the same shape with published numbers: Space Setup maintains **up to 15 rooms**, up to
**200 m²**, and — the phrasing matters —

> "The OS can maintain up to 15 rooms, and **may locate some or all of the rooms depending on the user's
> current location.**" — https://developers.meta.com/horizon/documentation/unity/unity-scene-overview/

**On Quest, room identity *is* "which room's anchors relocalized". There is no separate room-recognition
event.** Which is exactly the identification-vs-localisation distinction of §7.1, from the other side: the
platform does not identify the room and then localise — it localises, and identity falls out.

Both platforms also publish the *environmental* preconditions in **consumer-facing** copy, which is worth
copying as a UX pattern rather than treated as an embarrassment: Apple — "**Increase the lighting in your
room** to make sure Apple Vision Pro can detect the surfaces around you"
(https://support.apple.com/en-us/124816); Meta — "**Look around the room when drawing your boundary.**
Only looking in one direction … reduces the likelihood of remembering your boundary in the future"
(https://www.meta.com/help/quest/637588533755549/).

### 4.5 Persist identity, not content — and expect capacity errors

> "**Only WorldAnchor identifiers and transforms are persisted.** No other data, such as your virtual
> content, is included. **It is up to you to maintain a mapping of WorldAnchor identifiers to any virtual
> content.**" — WWDC23 10082

That is the binding record of §6.2, in Apple's words. Both platforms also make *capacity* a first-class
failure: Apple's `WorldTrackingProvider.Error.Code.worldAnchorLimitReached`; Meta's
`XR_ERROR_SPACE_STORAGE_AT_CAPACITY_META`. Apple's anchors are device-local and app-scoped, and are
**lost on iCloud restore or device change** (Apple engineer,
https://developer.apple.com/forums/thread/756829) — so even on the most integrated platform in the world,
spatial layout does not sync. Worth remembering before designing a multi-machine story (report 22 §8).

### 4.6 Head-locking is universally discouraged; body-locking is smoothed lazy-follow

> "**Avoid locking HUD style content to the user's head movements.**" / "**Anchor information and digital
> content to a space, or loosely follow the user using smoothing animation.**"
> — https://developers.meta.com/horizon/design/mr-design-guideline/

> "**Avoid anchoring content to the wearer's head.** … anchoring content so that it remains statically in
> front of someone can make them feel **stuck, confined, and uncomfortable** … Instead, **anchor content in
> people's space**." — https://developer.apple.com/design/human-interface-guidelines/spatial-layout

There is also a hard compositor cost: "**Head-locked overlays bypass TimeWarp** and exactly follow head
motion. The exception being small UI elements like a gaze cursor or targeting reticle."
(https://developers.meta.com/horizon/documentation/unity/unity-ovroverlay/) Nothing in either shell's
window system is head-locked; the only sanctioned head-locked content is reticles and cursors.

**Our `HEAD` mode with its deadzone + critically-damped spring is already the sanctioned shape** — this is
"loosely follow with smoothing", built and tuned (`03-anchoring.md` §4.2). Comfort numbers worth
recording: Meta places windows at ~70 cm with grab-to-reposition, ≥0.5 m fixation, ~1 m for menus; the
`XR_DISTANCE_MIN/MAX` = 0.3/5.0 clamp is generous at both ends by that standard, and §3.2's 5 m parking
is far outside anything either vendor considers usable.

### 4.7 The recenter event exists on visionOS, because stored world poses go stale

> "people can **long press the digital crown to recenter the app's experience around them**. If your app
> uses ARKit data, **this can invalidate positions you might have stored for later use**. You can listen to
> the world recentering event with the new `onWorldRecenter` view modifier … useful to **recompute and
> store positions based on the new coordinate system**." — WWDC25 290

And the mechanism, from WWDC23 10082, is precisely our GROUP re-seat:

> "**When recentering occurs, the app's origin will be moved to your current location.** Notice that the
> blue cube, which is not anchored, relocates to maintain its relative placement to the app's origin;
> while the red cube, which is anchored, **remains fixed relative to the real world**."

**Apple's recenter is a rigid re-plant of the app origin onto the head, broadcast as an event, with
world-anchored content excluded.** That is the seat-frame model of §6.2 shipping on a commercial platform.
Note also what Apple tells developers *not* to build: "**Rely on the Digital Crown to help people recenter
windows in their field of view** … **Your app doesn't need to do anything to support this action.**" On
visionOS the runtime owns the seat. On our stack there is no recenter API at all (§5.7), so we must own
it — which is the §5.8 boundary, drawn from the other direction.

### 4.8 The seam that rots is the transition, not the steady state

The two worst bugs in the entire survey are both transitions, and both look like ours.

- **Apple, FB19610114** (https://developer.apple.com/forums/thread/796861): walk room A → room B, recenter,
  walk back to A, recenter → **translation manipulation dies system-wide**, in every app including
  Apple's, until reboot. Reproduced by Apple, unfixed across visionOS 26.0 → 26.4. A user isolated the
  trigger: "I have a **photos widget on my wall** … **Deleted the widget on my wall. EVERYTHING WORKS. Add
  the widget back. Entities are stuck.**" — i.e. room-scoped persistent content and world-tracking state
  are coupled, and the room-transition + recenter path is where it rots.
- **Meta**: two independent developer reports of spatial-anchor poses going **stale and never
  re-converging** after a sleep/wake cycle.

**Neither vendor documents what happens to placement across a doff.** Apple's single sentence — "When you
take off your Apple Vision Pro, your apps stay where you placed them and your space will appear as you
left it when you put Apple Vision Pro back on" (https://support.apple.com/en-us/118515) — covers a
two-minute doff and an overnight one identically. Meta documents nothing at all. **That is a gap in the
industry, not a gap in this survey**, and it is precisely the gap §7.3 argues we should fill with
evidence rather than a timer.

Two further hazards worth stealing as acceptance tests:

- **Partial hydration of a dependent set.** WWDC25 290: "**immersive spaces are not restored** … However,
  if someone had locked the tools window in their space, **it would show up all alone with nothing to
  modify.**" Apple's answer is an opt-out — `restorationBehavior(.disabled)`,
  `defaultLaunchBehavior(.suppressed)`. Our analogue is exact: an XR monitor hydrated without the
  workspace or the application it existed to display.
- **Duplicate accumulation from failed relocalization.** https://developer.apple.com/forums/thread/749716
  — `removeAnchor()` completes without error, the anchor returns next launch, "anchors are **not always
  found** … When an anchor is not found our App will add an anchor … **Anchors accumulate and it becomes
  difficult to track.**" Filed as FB13713944. **Hydration must be idempotent and keyed on identity**, or a
  refusing system quietly becomes a duplicating one.

### 4.9 SteamVR's "universes" — the closest thing to a shipped room identity, and why it fails for us

SteamVR is the only desktop-class system that has carried a persistent room identity for a decade, and
its design is a direct precedent for §6.2's frame identity.

- **A universe is `{ set of base-station serials, their relative poses, gravity tilt }`**, recorded in
  `lighthousedb.json` (`known_universes[]`) — *not* in `chaperone_info.vrchap`, which merely keys its
  play-area and bounds records by universe ID. **Identity lives with the tracking system; content is
  keyed off identity.** That is exactly the layering this report proposes: a frame registry that owns
  identity, and bindings that name a frame.
- **Assignment and auto-detection are documented by Valve's OpenVR lead** (ValveSoftware/openvr#149): the
  Lighthouse driver mints an internally-generated ID for each *set* of base stations, a new one for a new
  set, and **returns to the old ID when you return to the old room**. The API is candid about the
  latency: *"We attempt to update the universe when basestations are moved… These functions will only
  return valid data once we've determined which known universe you are currently in."* IDs are opaque
  creation timestamps in practice; hand-authored files carry `"30"`, so do not parse them.
- **Current SteamVR has an undocumented second generation of this** (binary strings, snapshot
  2026-08-10): `persistentMapFromMap` relocalisation corrections that are **eased unless the jump is too
  large, in which case they snap**; a `/chaperone/last_relocalization_time`; and — squarely on our path —
  *"Rejecting new committed universe since IMU fallback is active"*. **Valve ships our
  "never persist state derived from a degraded pose" rule** (invariant I3).

**And here is why it does not save us.** SteamVR's identity comes from *external* base stations. For
**inside-out runtimes there is no such set**, so SteamVR pins a single universe ID (Oculus = 1,
ALVR/OpenHMD = 2) that simply **follows the user between rooms** — which makes the wrong-room-restore
reports real, and unfixable from the UI. **That is precisely our situation**: WiVRn is an inside-out
runtime with one implicit universe. The lesson is not "copy universes"; it is that *a room identity
derived from the tracking rig only works when the rig is in the room*, and ours is on the user's head.
Which is the argument for taking the identity from the runtime's relocaliser (§5.5) rather than
synthesising one.

Two anti-patterns to avoid, both visible in real dumps: **21 accumulated universes, never
garbage-collected**, and **no user-facing list of them** — so a user who is in the wrong room has no way
to see it, name it, or forget it. Any frame registry we build needs `list` and `forget` from day one
(§9.1). And a third, from OpenVR's `IVRSpatialAnchors`: it defines a three-state can't-place vocabulary
(`NotYetAvailable` / `NotAvailableInThisUniverse` / `PermanentlyUnavailable`) — good vocabulary — but
Valve's own wiki admits several documented states are **never emitted**. **Ship only the states you
actually emit.**

### 4.10 HoloLens is the one shipping precedent for "refuse, and tell the user"

Every desktop window manager, asked to restore a window to a display that is not present, **clamps,
defers or drops** — there is no refuse-and-tell instance anywhere in that lineage. Exactly one shipping
XR system does it properly, and it is a decade old: HoloLens shows **"Finding your space" → "Still
looking for your space" → Limited mode**, in which *"you can't place holograms or see holograms that were
placed previously"*, plus a per-space wipe (*Remove nearby holograms* / *Remove all holograms*). That is
scenario S2's UX, already validated on real users.

Microsoft also names the failure modes, and the vocabulary is worth adopting wholesale: **holes,
hallucinations, and wormholes** — a wormhole being *"HoloLens 'loses' part of the spatial map by thinking
it is in a different part of the map than it actually is."* **"The bedroom" is a wormhole**, and having a
name for it is worth more than it sounds: it is the failure that a confident system produces, and no
amount of validation on the *pose* detects it.

The best implementation pattern also comes from this lineage — Unity's `WorldAnchor`: handle
`OnTrackingChanged` with `SetActiveRecursively(located)`, and **"call the `OnTrackingChanged` handler with
the initial `IsLocated` state after attaching an anchor"**. Synthesising the initial edge means there is
**exactly one hydration code path**, taken on an edge, rather than a separate "initial placement" branch
that drifts out of sync with the update branch. Our `RESTORE`/`GROUP`/first-plug/reload tangle (§1.6) is
what happens without that discipline.

Two further traps recorded from this survey, both cheap to avoid and expensive to hit:

- **Never rewrite persistence from the frame path.** SteamVR does, and it is a known bug
  (ValveSoftware/openvr#994). Our equivalent is `m_restoreOffset` being written by the frame thread every
  frame (§1.4) — the same anti-pattern, and H2 removes it.
- **`XR_EXT_local_floor` carries no persistence guarantee**, and it fires
  `ReferenceSpaceChangePending` on *floor-estimate* changes as well as origin changes. A floor re-estimate
  and a recenter are indistinguishable at our current handler.

### 4.11 Where prior art says "this does not work"

- **Room fingerprinting is a *prior*, never a localiser — and the one product that shipped it says so.**
  HoloLens correlates map data with a Wi-Fi fingerprint to *speed recognition*, layered over a real visual
  localiser, and Microsoft documents the failure directly: *"If the Wi-Fi signals change significantly,
  the device may think it is in a different space altogether."* §7.1's verdict, validated by the only
  shipping implementation.
- **The one cheap signal with genuine pose content is degenerate.** A play area is, in OpenVR, literally
  a width × depth rectangle (`GetPlayAreaSize` returns two floats; real `.vrchap` dumps show
  `"play_area": [2.90000081, 2.40000105]`), and even a full boundary polygon leaves a 180° yaw ambiguity
  in a symmetric room — which is most rooms.
- **And the argument does not depend on fingerprints being bad.** Even a perfect identifier over a real
  localiser must fail closed, because relocalisation itself has a nonzero false-positive rate and the
  literature is unanimous that a single one is unrecoverable: AEROS (arXiv:2110.02018) — *"a single
  false-positive loop-closure… or even for the optimisation to fail entirely"*; ROVER
  (arXiv:2508.13488) — *"can be fatal"*; Sattler et al., CVPR 2018 (arXiv:1707.09092) — visual
  localisation is *"far from solved"*.
- **The cloud tier of this prior art is dead.** Azure Spatial Anchors was retired 2024-11-20. Only
  on-device stores survive, which quietly settles the multi-machine question (report 22 §8) for now.
- **ARCore deprecated its "no match" error** and moved the gate to *capture* time via
  `FeatureMapQuality` — telling the user a spot is poor **while they are standing there**. That is
  directly transferable and is the right home for a future `xrframe register` ritual: refuse a bad
  registration at the moment it is made, not a week later when it fails to resolve.

### 4.12 The per-environment scope, and other small primitives worth copying

- **Placement stores are scoped per environment.** On Quest, "your pinned passthrough windows are
  **separate from** your pinned VR home windows"; Apple's widgets "only snap to **physical** surfaces, they
  won't attach or persist in virtual environments". If we ever pair XR monitors with a virtual
  environment (`hypxrpaper`), the store needs that key.
- **Surface classification is permission-gated, `isSnapped` is not.** Apple's `SurfaceSnappingInfo` lets an
  app know *that* it is snapped without knowing *to what* — a good separation for a compositor that wants
  to render chrome differently when docked to a wall without demanding scene permissions.
- **An explicit nuke.** Quest ships "**Clear physical space history**", which removes all boundaries, space
  setup **and spatial anchors** for all profiles. Any persistent store needs the equivalent verb, and —
  per report 22 §3.1.2 — Meta's own remedy for MR drift *is* to clear space history, so a
  re-registration path must exist regardless of how good the anchors are.

---

### 4.13 The streaming-desktop neighbours — nobody has solved this

The systems closest to us are the least help, and it is worth saying so plainly rather than implying a
standard we are failing to meet. Report 22 §13 surveyed the field and concluded:

> **No PC-streaming client does world-anchored session persistence. The universal pragmatic tier is
> "persist poses in STAGE and let Guardian carry the world lock" — which fails exactly when Guardian is
> redrawn, i.e. the user's observed failure mode.**

- **ALVR** (source-verified) requests no anchor extensions and persists nothing; its
  `RecenteringMode { Stage, LocalFloor, Local, Tilted }` **defaults to `LocalFloor`** — the same choice we
  made, with the same consequences — and only `Stage` persists at all, and only via Guardian.
- **Virtual Desktop** has no anchor or persistence entry anywhere in its changelog; v1.32.5 added
  *"auto-arrange monitors on recenter"*, **enabled by default** (§7.3).
- **Immersed** has no anchor evidence; the documented remedy for misplaced screens is *recenter the
  headset*.
- **Meta's own shell** did not ship world-pinned windows until **v81, October 2025** (§4.1).

So the honest framing for the user is: **we are not behind the field; the field has not attempted this,
and the two platform owners that did took four years and needed runtime support we do not have.** What is
genuinely available to us that the neighbours lack is (a) our own fork of the streaming client, which is
already doing more than any of them (§2.4), and (b) a compositor we control end to end.

### 4.14 The Linux XR shells — and the one enum worth stealing outright

**WayVR** (formerly `wlx-overlay-s`; the repo now redirects to `wayvr-org/wayvr`, so any doc naming
`~/.config/wlxoverlay/` is stale) has independently arrived at this report's model, and its
`Positioning` enum is the single most transferable artifact in the whole survey
(`wlx-common/src/windowing.rs:6-26`) — note that the doc comments *are* the design:

```rust
pub enum Positioning {
    /// Stays in place, recenters relative to HMD
    #[default] Floating,
    /// Stays in place, recenters relative to anchor. Follows anchor during anchor grab.
    Anchored,
    /// Stays in place, no recentering
    Static,
    /// Following HMD
    FollowHead { lerp: f32 },
    /// Following hand
    FollowHand { hand: LeftRight, lerp: f32 },
}
```

**All three world-ish modes "stay in place" and differ only in what happens on recenter.** That is §7.2's
argument — policy on the object, not membership of a space — expressed as five lines of Rust. Body
attachment is a separate axis carrying exactly one tuning knob.

Two more of its decisions match invariants this report derives independently:

- **The live world pose is deliberately ephemeral and the stored one is parent-relative.** In
  `OverlayWindowState`, `transform` is `#[serde(skip)]`; what persists is `saved_transform`, expressed
  against a parent chosen by the `Positioning` tag — and the parent is **yaw-snapped**
  (`snap_upright(hmd, Y)`), so a head tilt at save time cannot bake roll into the stored pose. That is
  invariant I4 (authored beats live) and I7 (gravity), arrived at from the other direction.
- **Placements are keyed by name and scoped to a named set** — `~/.config/wayvr/conf.d/zz-saved-state.json5`
  holds `HashMap<Arc<str>, OverlayWindowState>` per set, plus an `inactive_overlays` bucket that retains
  state for windows not yet seen this session. The `zz-` prefix is deliberate: drop-ins load
  alphabetically so machine-written state wins over hand-written config. That settles report 22 §5.2's
  config-vs-state split the same way, in a shipped product.

And one hard-won lesson: **WayVR's `save_state()` has exactly two call sites** (leaving edit mode, clean
shutdown), and issue #529 is the post-mortem — the maintainer confirms saving "*seems to not happen when
wivrn is not exiting cleanly*", and the reporter's summary is *"If I make changes I need to 'commit' by
recentering via double-Y at least once per session… it's not exactly intuitive."* **Commit at the edge,
but also debounce-and-write continuously.** Folded into H2.

Its recenter story is also instructive: WayVR creates **only STAGE and VIEW** — no LOCAL, no LOCAL_FLOOR —
**does not handle `ReferenceSpaceChangePending` at all**, and owns recentering out of band through
**libmonado** (`get/set_reference_space_offset(Stage)`, yaw-only, X/Z-only), with every playspace feature
silently no-op on SteamVR. When the playspace moves it computes `correction = after.inverse() * before`
and applies it to the anchor and every overlay, so world-locked content appears to stay put. That is a
second, independent confirmation that **STAGE is the right frame for desktop content on this stack**, and
it is the strongest single argument that the M0 measurement will come back positive.

**StardustXR** is the only one with a real persistence protocol, and its shape is worth noting because it
inverts ours: `ClientState { data: bytes?, root: id, spatial_anchors: map<string,id> }` — *"Spatials that
will be in the same place you left them"* — with `save_state` as a **client-side method the server calls**
(100 ms budget), state written to `~/.local/state/stardust/{app}-{nanoid}.toml`, and a
`STARDUST_STARTUP_TOKEN` handed to a relaunched process. Its spatial graph has the one operation a window
manager genuinely needs and we do not have: **`set_spatial_parent` (keep local transform) vs
`set_spatial_parent_in_place` (keep global transform)** — reparent-and-move vs reparent-and-stay, which is
exactly the `rehome` verb of §6.2. But it stores anchors as **world-space `Mat4`** against a default
`LOCAL` reference space, so a runtime recenter shifts its entire persisted world. Steal the reparenting
vocabulary; do not steal the frame choice.

**xrdesktop** is in maintenance and has **no position persistence at all** (`org.xrdesktop` GSettings has
no pose key); its one reusable idea is **shake compensation** — `shake-compensation-threshold = 2.0` (% of
on-window cursor travel per unit distance) and `shake-compensation-duration-ms = 180` before a press
reinterprets as a drag — which belongs with our aim-assist and redraw dead-band work.
**SimulaVR** persists *processes*, via an xpra server, not placements.

⚠️ **"Pin" is a false friend across all three**: a *visibility filter* in xrdesktop
(`xrd_window_set_pin`), *sticky across workspaces* in SimulaVR (`sendToWorkspacePersistent`), and
*spatially anchored* in OVR Toolkit. Likewise "anchor" (a runtime-tracked physical point in OpenXR/WMR; a
user-movable virtual reference object in WayVR) and "recenter" (move the world origin in Monado/ALVR;
re-grab this panel in WiVRn's controller bindings). **Define these loudly in our config vocabulary or
avoid the words.**

Finally, a precedent from our own runtime worth knowing: **WiVRn's lobby already implements a seat.**
`scenes::lobby::on_focused()` sets `recenter_gui = true`, and the GUI pose is then re-derived as
`head_position + initial_gui_distance * head_direction`. Doffing a Quest drops focus; donning restores
it — **so the lobby panel re-derives its pose from your head on every don**, "always find me" rather than
"stay where I was". The stream scene goes further and implements a **dual anchor with a no-jump frame
conversion**: head-locked tabs and world-locked (STAGE) tabs, converting the pose between frames on the
transition so the panel does not teleport. That is `rehome`, shipped, in the codebase next door.


## 5. What the OpenXR specification actually promises

Quotes are from the spec sources at `KhronosGroup/OpenXR-Docs@main`, **OpenXR 1.1.62 (2026-07-31)**. Note
that the local SDK checkout `~/code/OpenXR-SDK-Source` is **1.1.57 (2026-02-12)** — old enough to be
missing the single most relevant extension (§5.5). Rendered spec:
`https://registry.khronos.org/OpenXR/specs/1.1/html/xrspec.html`.

### 5.1 There is no world origin, and that is deliberate

The framing paragraph belongs at the top of any design doc in this area (`spaces.adoc:50-58`):

> In other XR APIs, it is common to report the "pose" of an object relative to some presumed underlying
> global space. **This API is careful to not explicitly define such an underlying global space, because
> it does not apply to all systems.** … To satisfy this wide variability, "poses" are always described
> as the relationship between two spaces.

And (`spaces.adoc:33-38`, `:60-63`):

> Runtimes whose tracking systems improve their understanding of the world over time **may:** track
> spaces independently … **may:** introduce slight adjustments to the origin of each space on a
> continuous basis …
> The location returned by `xrLocateSpace` in later frames **may:** change over time, **even for spaces
> that track static objects**, as either the target space or base space adjusts its origin.

**A stored `(x, y, z)` is meaningless without also storing which space it was in and which generation of
that space.** That is invariant I1 (§8.1), stated by Khronos.

### 5.2 `LOCAL` guarantees almost nothing we relied on

> This space locks in both its initial position and orientation, **which the runtime may: define to be
> either the initial position at application launch or some other calibrated zero position.**
> When a user needs to recenter the `LOCAL` space, a runtime **may:** offer some system-level
> recentering interaction **that is transparent to the application** …

| question | answer |
|---|---|
| gravity-aligned? | **yes** — pitch/roll excluded. Only yaw + position are free. |
| is the yaw meaningful? | **no** — "the initial position at application launch" is a permitted definition |
| stable within a session? | only until a recenter or a tracking event |
| **stable across a process restart?** | **not guaranteed at all** |
| stable across rooms? | nothing is promised |

**Both reported symptoms are fully spec-legal behaviour for `LOCAL`.** `LOCAL_FLOOR` inherits every one
of these weaknesses — "The orientation of the `LOCAL_FLOOR` space **must:** match the `LOCAL` space", and
a `LOCAL` recenter recenters it. It adds exactly one thing: a floor-level Y.

By contrast, `STAGE` is the only core space whose orientation means something physical: "The origin is on
the floor at the center of the rectangle, with +Y up, and **the X and Z axes aligned with the rectangle
edges**." Its support is `optional:`, and it must raise a change event "When the user redefines the
origin or bounds of the current `STAGE` space, **or the runtime otherwise switches to a new `STAGE` space
definition**".

One caution that report 22 also flagged and that our §2 evidence confirms: **the LOCAL↔STAGE transform is
not required to be constant.** They are independently tracked and independently adjustable. So "convert
at hydration" must re-derive the transform per frame and handle the not-locatable case; it is not a
constant to cache across sessions.

### 5.3 The event, and the crux of hold-vs-follow

`XrEventDataReferenceSpaceChangePending` carries `changeTime` (**in the future**, "to allow for a deep
render pipeline to present frames that are already in flight using the previous definition of the
space"), `poseValid`, and `poseInPreviousSpace`. Two traps:

> The `poseInPreviousSpace` provided here **must:** only describe the change in the natural origin … and
> **must: not** incorporate any origin offsets specified by the application …
> **If the runtime does not know the location of the space's new origin relative to its previous origin,
> `poseValid` must: be false, and the position and orientation of `poseInPreviousSpace` are undefined.**

The policy fork is entirely ours:

- **Follow** — do nothing. Poses are in `LOCAL`; `LOCAL` moved; content moves with it, rigidly, free.
- **Hold** — rewrite each stored pose as `inverse(poseInPreviousSpace) ∘ oldPose`, applied to frames at
  `t >= changeTime`.
- **`poseValid == false`** — **hold is unimplementable.** There is no correct transform; anything
  computed is a fabrication. The honest responses are: park head-relative, or refuse to hydrate and say
  so. *"Silently reusing the old numbers is what produces 'restored in the wrong room' and 'way off in
  the distance'."*

**One field caveat before leaning on it.** Godot's issue #99157 documents that Quest sends *one* recenter
event where SteamVR sends *several*, that the event struct's contents differ across runtimes, and that the
event can arrive *before* new tracking data is available — with the maintainers' proposed fix being
literally to **"disregard the `poseValid` property"** and re-derive after fresh tracking data. So
`poseValid == true` is a hint worth using and `poseValid == false` is a hard barrier worth honouring, but
neither should be the *only* input; the head-pair reconstruction we already have (§2.2) is the right
cross-check, not a replacement.

`XR_MSFT_unbounded_reference_space` (2019) already made `poseValid` the wire-level wrong-room detector:
`poseValid = false` for "regained tracking in a new room", `poseValid = true` for a float-precision
re-origin. **The primitive has existed since 2019 and is criminally underused — including by us, and
including by monado, which hard-codes it to false (§2.2).**

### 5.4 `XrSpaceLocationFlags` — the honest-failure primitive we never read

> `XR_SPACE_LOCATION_POSITION_VALID_BIT` … When a space location loses tracking, runtimes **should:**
> continue to provide valid but untracked `position` values **that are inferred or last-known** …
> clearing `XR_SPACE_LOCATION_POSITION_TRACKED_BIT` until positional tracking is recovered.

- `VALID && TRACKED` → observed. Safe to **commit** placements.
- `VALID && !TRACKED` → the runtime's best guess (neck model, dead reckoning, last-known). **Render, do
  not commit, do not first-hydrate.**
- `!VALID` → "the runtime **should:** return a location with no position … if the runtime has not yet
  observed even a last-known pose … **or the two spaces are in disconnected fragments of the runtime's
  tracked volume**". *Disconnected fragments of the tracked volume* is the spec's phrase for **different
  rooms**. This is the do-not-hydrate signal at the locate level.

We check only the `VALID` bits (§3.4.1). Fixing that is invariant I3 and work package H1.

The ceiling of this primitive is worth stating: it is per-call and per-frame. It says *"right now I am
guessing"*. It does **not** say *"the thing you saved last Tuesday is in a different building"*. For that
you need §5.5.

### 5.5 `XR_EXT_stationary_reference_space` — written for this bug

Extension **742**, revision 1, last modified 2026-04-22, shipped in **OpenXR 1.1.59, 2026-04-30**.
Contributors: Yin Li (Microsoft), Andreas Selvik + Yuichi Taguchi (Meta), Nathan Nuber (Valve),
**Jakob Bornecrantz (NVIDIA — monado's founder)**, Jared Finder (Google).

**And it is already on this box.** `/usr/include/openxr/openxr.h` is `XR_CURRENT_API_VERSION 1.1.60` and
carries the whole surface at `:12610-12634`:
`XR_REFERENCE_SPACE_TYPE_STATIONARY_EXT = 1000742000` (`:1012`),
`XrStationaryReferenceSpaceGenerationIdGetInfoEXT` / `…ResultEXT` (`:934-935`), and
`xrGetStationaryReferenceSpaceGenerationIdEXT`. Note the enum value: **1000742000 — the same number Meta
squatted for `XR_EXTX2`**, so the two differ only in the extension-name string and the function name, not
in the reference-space enum. (The `~/code/OpenXR-SDK-Source` checkout is older, at 1.1.57, and lacks it;
the system headers do not. Extension *743* is `XR_EXT_spatial_marker_tracking`.)

Its motivation section is, almost verbatim, this report's requirements:

> 1. Use a reference space that is related to the physical world and is **unaffected by the "recenter"
>    operation or user-defined room boundary.**
> 2. Use a reference space that **can regain its location in the physical world after tracking is lost,
>    after the headset is removed and put back on**, or after the device is suspended and resumed …
> 3. Use a reference space that **persists its origin location in the physical world across app restarts
>    or device reboots.**

The `must: not`s are the substance:

> The runtime **must: not** relate a system-level "recenter" operation to the `STATIONARY` reference
> space, nor raise an `XrEventDataReferenceSpaceChangePending` event for `STATIONARY` … only because the
> user did a recenter operation.
> The origin of `STATIONARY` space **must: not** move only because the user redefines the room boundary.

**And the composability note is precisely the model recommended in §6** — the spec's own sanctioned
pattern for having hold and follow at the same time:

> * Applications can use the `LOCAL_FLOOR` space location in `STATIONARY` reference space to obtain the
>   floor height.
> * Applications can use the `VIEW` space location in `STATIONARY` reference space to compute the user's
>   eye level height and forward direction at a specified time.
> * **Applications can use the `LOCAL` space location in `STATIONARY` reference space to reason about the
>   recenter operation, or place the UI at a comfortable position in front of the user according to
>   recentering.**

*Store world content in the stationary/room frame; locate the seat frame inside it.* Two frames, one
pair of `xrLocateSpace` calls, no policy conflict. **That is the recommendation of §6.4, and it turns out
to be the spec's own advice.** It also works today, substituting `STAGE` for `STATIONARY` (§2.6) — which
is why the recommendation has a rung that needs nothing from anyone.

**`generationId`** (`xrGetStationaryReferenceSpaceGenerationIdEXT`, an `XrUuid`) is the missing
invariant, standardised:

> To detect whether the `STATIONARY` reference space origin has changed during a period in which the
> session was not running, an application gets the `generationId` after a successful `xrBeginSession` and
> compares the `XrUuid` with a `generationId` from a previous session. … **The equality of `generationId`
> is a reliable indicator … across multiple sessions, application restarts, or device restarts.
> However, this does not imply that when `generationId` is different, the origin is not at the same
> location.**

Note the asymmetry: **same ID ⇒ same origin (reliable); different ID ⇒ merely unknown.** That is exactly
the right shape for a hydration policy — a sound *permission* to hydrate, and a *suspicion* rather than a
proof of relocation. And the relocation-in-progress rule is what a runtime does instead of lying:

> During this relocation phase, the runtime **must:** return the previous `generationId` …, **not** raise
> the event, and **return untracked data for all space locate functions** when using the `STATIONARY`
> space.

Meta ships the predecessor **today** as `XR_EXTX2_stationary_reference_space`
(`XR_REFERENCE_SPACE_TYPE_STATIONARY_EXTX2 = 1000742000`, `xrGetStationaryReferenceSpaceIdEXTX2`), in its
licensed preview headers since SDK v77 (2025-06-06), still present at v85, and observed in a live Quest
runtime extension dump. A client probes both names and calls whichever is advertised; the space enum is
identical either way.

**The degraded path is the part that makes this actionable now.** The spec explicitly allows a runtime
that cannot relocalise to "return a different generation ID for each session" — which means the
*protocol* is well defined even on a stack that can never satisfy the guarantee. So the compositor can
mint its own generation ID today, on exactly those semantics: a fresh UUID per session (because a
recenterable `LOCAL_FLOOR` genuinely is a new origin each time), persisted beside every placement,
compared on startup, with **nothing world-locked auto-hydrating on a mismatch**. That is
`XR_EXT_stationary_reference_space`'s client half, implementable with zero support from WiVRn or monado,
and it upgrades in place the day a runtime supplies a real ID.

### 5.6 Persistence: ratified on paper, absent from our runtime

The cross-vendor family `XR_EXT_spatial_entity` (741) / `_spatial_anchor` (763) / `_spatial_persistence`
(764) / `_persistence_operations` (782) was **ratified into 1.1.49 on 2025-06-10**, and Meta's runtime
advertises all of it. Its persistence×tracking cross-product is the four-state hydration decision this
report keeps arguing for, written into a specification:

| `XrSpatialPersistenceStateEXT` | `XrSpatialEntityTrackingStateEXT` | meaning | correct action |
|---|---|---|---|
| `LOADED` | `TRACKING` | saved and relocalized here, now | **hydrate** |
| `LOADED` | `PAUSED` | saved but **not currently relocalized** — possibly a different room | **do not hydrate; keep the record; wait** |
| `NOT_FOUND` | `STOPPED` ("will never resume") | gone from storage | **delete the saved placement** |
| *absent from snapshot* | — | runtime could not determine | **unknown; retry later; do not delete** |

Three caveats bite a compositor specifically: **EXT anchors are not `XrSpace`s** (there is no core or EXT
path from a spatial entity to a space — Google had to define `XR_ANDROID_spatial_anchor_space` to fill
the gap), the only writable persistence scope is same-device/same-user/**same-app**, and **monado
implements none of the family.** For our stack today this is aspirational.

Two other models are worth copying rather than reinventing. `XR_ML_localization_map` (Magic Leap) treats
maps as first-class named UUID'd objects with a four-state machine
(`NOT_LOCALIZED`/`LOCALIZED`/`LOCALIZATION_PENDING`/`SLEEPING_BEFORE_RETRY`), a confidence enum
(`POOR`…`EXCELLENT`, where `GOOD` = "persistent content should be stable"), and diagnosable failure flags
(`OUT_OF_MAPPED_AREA`, `LOW_FEATURE_COUNT`, `EXCESSIVE_MOTION`, `LOW_LIGHT`, `HEADPOSE`) — plus the
strongest cross-session origin guarantee anywhere in OpenXR: for a given map UUID the origin is identical
"across more than one `XrInstance`, **including for different users and different hardware**". And WMR's
`SpatialLocatability` separates `PositionalTrackingActivating` (warming up) from
`PositionalTrackingInhibited` (structurally cannot) — the one distinction OpenXR's two bits genuinely
cannot make.

### 5.7 There is no recenter API. At all.

**The string "recenter" appears zero times in `xr.xml`, across all 862 extensions.** No
`xrRequestRecenterEXT`, no `XR_META_recenter`. Recentering is exclusively a runtime/system-UI action, and
the only channel to an app is `XrEventDataReferenceSpaceChangePending`.

**Consequence:** "bring my desktop to me" *cannot* be an OpenXR call. It is either our own application-level
re-seat, or a runtime feature we build if the runtime is ours. There is no third option and no portable
one. `xrmonitor reseat` is therefore not a workaround for a missing API — it is the only correct shape,
and it should be treated as a first-class primitive rather than an escape hatch.

### 5.8 The runtime / client boundary

The spec draws this line consistently, and the coordinator's amendment can be answered from it directly.

**Runtime-side, unambiguously** — the app gets no override: defining reference-space origins; recentering
("transparent to the application", with no API at all); continuous origin drift and its suppression of
events; locatability and its honesty (`XrSpaceLocationFlags` are outputs); relocalization; anchor
tracking and correction; persistent storage; **layer stabilisation and reprojection** ("`space` is the
`XrSpace` in which the layer will be kept stable over time"); head-locking (a VIEW-space layer is
"**implicitly** head-locked").

**App-side, unambiguously** — the spec hands over a knob and says nothing about how to turn it: where
content goes (`poseInReferenceSpace` is respected but never interpreted); layout; **hold vs follow on
recenter** (the event delivers the delta; nothing says what to do with it); hydration policy (persistence
gives `LOADED`/`PAUSED`/`NOT_FOUND` and stops); remembering IDs across sessions ("**the application is
responsible for remembering the ID from a previous session**"); what to do with an inferred pose ("so long
as it is still reasonable for the application to use that pose").

**Is there precedent for a runtime-provided body-attached or re-seat-on-don space? No.** Every
`XrReferenceSpaceType` in the registry — `VIEW`, `LOCAL`, `STAGE`, `LOCAL_FLOOR`, `UNBOUNDED_MSFT`,
`UNBOUNDED_ANDROID`, `STATIONARY_EXT`, `LOCALIZATION_MAP_ML`, `COMBINED_EYE_VARJO` — and not one is
body-, torso-, or hip-attached. The closest the ecosystem comes is
`XR_MSFT_composition_layer_reprojection`'s hint — "This mode works better for **body-locked content that
should follow the user**" — which lets the app *declare* it is body-locking so the runtime reprojects
better, and pointedly does **not** provide the behaviour.

**So the split for our two programs is:**

| concern | side | why |
|---|---|---|
| a world frame with a stable identity and generation | **runtime** | every client wants the same answer, and getting it wrong is a correctness bug, not a taste difference |
| honest locatability and relocalization state | **runtime** | only the runtime has the sensors |
| correct `poseInPreviousSpace` on recenter | **runtime** | monado already computes it and throws it away (§2.2) — R2 |
| don/doff presence | **runtime** | already there (`XR_EXT_user_presence`) |
| play-area / boundary geometry | **runtime** (and the WiVRn wire) | a *disambiguator*, not a localiser (§7.1) |
| anchors + persistence, when there is a reason | **runtime** | `XR_EXT_spatial_*`; and the `XrSpace`-from-anchor bridge is the ecosystem's sharpest unstandardised gap — a runtime that filled it would be first |
| which objects exist, and their layout | **client** | |
| body-attached follow and its tuning | **client** | no spec precedent; deadzone/lag/spring are taste and per-app comfort |
| the seat, and re-seat policy | **client** | there is no recenter API to defer to (§5.7) |
| hydration policy and the refusal UX | **client** | the runtime supplies states; the meaning is the app's |

**The test to apply at the boundary:** *if two different clients would legitimately want different
answers, it is client policy; if two clients would want the same answer and getting it wrong is a
correctness bug, it is the runtime's.* "Where is the desk" is the runtime's. "Should my monitor come to
me when I put the headset on" is ours.

**If the runtime becomes a product, the flagship is `XR_EXT_stationary_reference_space`.** It is a
four-symbol API over relocalization machinery the runtime needs anyway, its spec text is an unusually
complete design document for exactly this problem, one of its co-authors founded monado — and no runtime
in the world ships the ratified name yet.

---

## 6. The model

### 6.1 The reframing that makes the rest fall out

OpenXR already contains the user's three-part request, and it made them three **different reference
spaces**:

| the user asked for | OpenXR's answer | what we did |
|---|---|---|
| (a) objects that really stay in the same place in the world | **STAGE** (and, since 1.1.61, `XR_EXT_stationary_reference_space`) — room-anchored, floor-level, changes only when the *room definition* changes | never created it |
| (c) objects refreshed relative to you on don / recenter | **LOCAL / LOCAL_FLOOR** — *by specification* the space that is re-established when the app starts and when the user recenters | built the world on it |
| (b) objects attached to your body | **VIEW** space, plus app policy | built it correctly, as `HEAD`/`BODY`/`DEVICE` + the adaptive decorator |

**We collapsed three frames onto one, and then spent a year building compensators for the collapse.**
`recenter_on_plug`, the head-pair delta reconstruction, the head-relative capture, RESTORE, GROUP
re-seat — every one of them is a way of re-deriving a placement from the wearer because the wearer is
the only frame we kept. Each is well built. Together they are a chain of guesses, and §3 is what happens
when a guess is fed a bad input.

The model below is therefore not a rewrite of the anchor engine. It is **the introduction of the frames
the engine has been missing**, plus the provenance and refusal semantics that make a frame mean
something.

### 6.2 Vocabulary

These are the words the rest of this report (and, if it ships, the code and config) uses. They are
chosen to match OpenXR and the platform prior art rather than to invent.

- **Frame** — a named, gravity-aligned coordinate system with an **identity** (a stable id) and a
  **generation** (a counter that increments whenever the frame's relationship to the physical world may
  have changed). 4-DoF: `x, y, z, yaw`. Pitch and roll are pinned by gravity and are never solved for
  (report 22 §2.2; OpenXR 1.1 §7.1 guarantees the gravity alignment).
- **Frame kind** —
  - `session` — the runtime's `LOCAL_FLOOR`. Ephemeral by definition; its generation bumps on every
    reference-space change and on every session. **Nothing durable may be stored in it.**
  - `world` — a room-anchored frame. Today: `STAGE`. Later: a stationary reference space, or an anchor.
    Durable, but only as durable as its evidence.
  - `seat` — a frame derived from the wearer at a defined moment (first don, an explicit re-seat, a
    recenter under `follow`). Explicitly ephemeral, explicitly *re-derived*, and — this is the change —
    **an object in its own right, with a stored pose.**
  - `body` / `head` / `device` — frames that ride the user. Already implemented.
- **Resolution state** of a frame, borrowed from the honest-failure prior art:
  `RESOLVED` (we have a current transform into the session frame, with a named evidence source) /
  `STALE` (we have a transform, but its generation no longer matches / the evidence is old) /
  `UNRESOLVED` (the frame is known to exist and we cannot currently place it) /
  `UNKNOWN` (never seen).
- **Binding** — what replaces "an anchor" as the persisted unit: `{ frameRef, pose-in-that-frame,
  hydration policy, don policy }`. A binding is the durable record. `SXRAnchorState` becomes the *solved*
  form of a binding, not the stored form.
- **Hydration** — making an object present and visible. An object is hydrated only when its binding's
  frame is `RESOLVED`, unless its policy says otherwise. **Not hydrating is a first-class, visible,
  explicable outcome**, not an error. (This is not novel: RealityKit's anchored entities are *inactive
  until anchored* and "might not show up in your scene at all", and `XR_EXT_spatial_persistence` models
  the same thing as a `{LOADED, NOT_FOUND} × {TRACKING, PAUSED, STOPPED}` cross-product with a fourth
  "could not determine" outcome expressed as **absence from the snapshot**. §4.3, §5.6.)
- **Provenance** — `{frameId, generation, capturedAt, evidence}` stamped on every stored pose. This is
  the missing invariant of §1.2 and the single highest-value change in this report.
- **Re-seat** — moving a `seat` frame onto the wearer. One operation, on one object. Children keep their
  authored poses, including their authored distance.

⚠️ **Two words to avoid or define loudly.** "**Pin**" already means three incompatible things in the
Linux XR neighbourhood — a visibility filter (xrdesktop), sticky-across-workspaces (SimulaVR), and
spatially anchored (OVR Toolkit). "**Anchor**" means a runtime-tracked physical point in OpenXR and WMR
but a user-movable virtual reference object in WayVR — and it is already our config keyword for something
closer to the second. This report uses **frame**, **binding** and **re-seat** precisely to sidestep that.

The vocabulary is also not invented here: WayVR's shipped `Positioning` enum draws exactly this line —
three world-ish modes that "stay in place" and differ *only* in recenter policy, plus a separate
body-attachment axis with a lag parameter (§4.14).
- **Rehome** — moving a binding from one frame to another, preserving world position where possible.
  ("Migration" in report 22's vocabulary is rehoming between two `world` frames.)

### 6.3 Three candidate models

#### Model 1 — *Harden the seat* (do not introduce a world frame)

Keep exactly today's architecture: one frame, head-relative durable representation. Add provenance,
refusal, and an authored pose.

- Stamp every stored pose with `{sessionEpoch, capturedAt, evidence}`; refuse to restore from a capture
  taken with an untracked head, a non-finite pose, or across an unrepaired frame change.
- Snapshot the capture **at the doff edge** rather than last-write-wins, and reject a capture whose
  delta from the previous accepted one exceeds a plausibility bound.
- Give the group an **authored** seat distance so `reseat` returns to it instead of inheriting the live
  one (§3.2).
- Add a `refused` state to `hyprctl openxr status` and a notification.

**Runtime/client split:** 100 % client. Nothing is asked of any runtime.
**Ceiling:** the demon is tamed, not slain. Monitors reliably come back *in front of you, as you left
them relative to you*. They can never be "on that wall", and multi-room is meaningless.
**Size:** small. This is the phase-0 of every other model and should ship regardless.

#### Model 2 — *Two frames and a seat object* (RECOMMENDED)

Introduce `world` alongside `session`, make `seat` a real object, and gate hydration on resolution.

- Create `STAGE` at session start; publish `T_session←world` every frame and its stability over time.
- A binding names a frame. `anchor:world` becomes expressible for the first time; `anchor:local` is
  retained as a compatibility spelling for `anchor:seat` (§8).
- The **seat** is one object per session with a stored pose in the world frame when one is resolved, and
  a stored pose relative to the wearer when one is not. `reseat` moves the seat. Objects bound to the
  seat travel with it rigidly and keep their authored distances — which structurally kills §3.2.
- Hydration: a `world`-bound object with an `UNRESOLVED` world frame is **not hydrated**. Its Hyprland
  output stays unplugged (the existing `monitors_follow_session` machinery already models exactly this:
  "XR monitors behave like unplugged external monitors"), its workspaces stay evacuated, and
  `hyprctl openxr status` says which frame it is waiting for.
- Evidence ladder for resolving a `world` frame, best first — this is report 22's ladder, unchanged, now
  with somewhere to plug in:
  1. a runtime stationary/anchor-backed space with a matching identity (needs runtime work — §5);
  2. `STAGE` with an unchanged generation (available today — §2.6);
  3. a user ritual: `xrframe register desk` captures one head pose = 4 DoF exactly (report 22 §2.2);
  4. nothing → `UNRESOLVED`.

**Runtime/client split:** everything above is client-side over standard OpenXR spaces and works today.
What the runtime owes us — and what we would implement first if the runtime is ours — is exactly the
*evidence*: a world frame with a **stable identity and a generation**, and honest signalling when it
moves or cannot be found. §5.8 details this.
**Ceiling:** true world placement to the fidelity of the evidence available, with honest refusal below
it. Multi-room becomes expressible (two `world` frames) without being solved (we still cannot tell them
apart without evidence).
**Size:** medium. The frame registry and the seat object are the new code; the anchor engine is reused
almost unchanged.

#### Model 3 — *The space graph* (the user's sketch, taken literally)

Model 2 plus arbitrary nesting: `house → room → desk → grid → object`, with policies inherited down the
tree and rehoming between siblings. This is report 22's *locations* generalised, and it subsumes the
unbuilt `CXRGrid`.

**Runtime/client split:** identical to Model 2 — the extra levels are pure client policy; no runtime
knows or cares about a "desk inside a room".
**Ceiling:** the best end state. Per-location layouts, migration, multi-machine (report 22 §8).
**Size:** large, and — critically — **it buys nothing that Model 2 does not, until there is evidence to
resolve more than one world frame.** With one room and no anchors, a three-level hierarchy is three
levels of one.

### 6.4 Recommendation

**Build Model 2, with Model 1 as its first phase, shaped so Model 3 is an extension rather than a
rewrite.**

Concretely: one level of parent frame now (`world` and `seat`, both first-class objects in a frame
registry), a binding record that names its frame and carries provenance, hydration gated on resolution,
and the re-seat re-expressed as an operation on the seat object. Nesting stays *possible* — a frame's
parent is a field, and the resolver is already a two-phase "solve the parent once, compose children"
walk (report 03 WP-G2's shape) — but only two kinds are instantiated until there is evidence worth
hanging a third on.

The reason to prefer 2 over 3 is not effort. It is that **hierarchy is not the scarce resource;
evidence is.** A deeper tree with nothing to resolve it is a more elaborate way of guessing.

---

## 7. Pushback on the brief

The user asked to be pushed back on. Five places where the brief needs correcting, in descending order of
how much they change the plan — plus one (§7.6) where it *understates* the problem.

### 7.1 "True world placement" is achievable, but not the way the phrasing implies — and the honest fallback is *not* room fingerprinting

The good news is better than expected: a room-anchored frame is available **today, for free** (§2.6).
`STAGE` is the Quest's own stage origin, immune to recenter, immune to re-don, and it survives a WiVRn
reconnect. Placing world content in STAGE would immediately have made the recenter button a no-op for
world content — which is the correct semantics — with no upstream patch at all.

The bad news is the part the phrasing hides, and it splits into two cases that need different answers.

**Case 1 — the stage moves while we are watching.** Handled, already, and better than expected: the
deployed fork corrects it out when the runtime supplies a delta, and *tells us* when it cannot (§2.4).
The only defect is that we discard the telling. That is a fix, not a research problem.

**Case 2 — the stage moves while we are *not* watching.** The user doffs, walks to another room, and
re-dons; or the client disconnects and reconnects elsewhere. There is no delta to forward because nothing
observed the change, the dead-reckoned correction is reset at exactly that boundary, and no signal exists
at any layer. So the honest statement is:

> We can hold a world frame while the session is unbroken. Across a break we cannot tell whether the
> world we are about to hydrate into is the same one we saved. Until we can, "not hydrated when
> unfindable" is unimplementable in that case, because we never learn that it is unfindable.

**That is exactly what a frame identity is for, and it is why the generation token — not the frame — is
the thing to build first.** `XR_EXT_stationary_reference_space`'s `generationId` is the standardised
answer (§5.5) and its whole design is the asymmetry this case needs: same ID ⇒ same origin, reliably;
different ID ⇒ merely unknown. Until a runtime supplies one, the compositor can mint its own — persist an
ID beside every placement, invalidate it on every event that *could* have moved the world (including the
STAGE change we currently drop, and every reconnect), and **refuse rather than guess** when it cannot be
vouched for. That protocol needs nothing from anybody and is implementable this week.

On the fallback: **room fingerprinting is the wrong instinct, and it is worth being blunt about why.**
It conflates two different problems:

- **Identification** — "which of my known places is this?" Cheap signals (play-area extents, floor
  height, WiFi BSSID set, connected displays, time of day) can answer this at *room* granularity.
- **Localisation** — "what is the 4-DoF transform from my stored frame into the current session frame?"
  No cheap signal answers this at all. It needs centimetres and degrees.

Identification without localisation is useless for placement: knowing you are in the office does not
tell you where the office's origin is this session. And localisation without identification is what
STAGE already gives us. So fingerprinting can only ever be a **disambiguator** — "of my three stored
world frames, try the office one" — layered *on top of* a real localisation source. It is not a
fallback for one; it is a refinement once there are several. Deprioritise it accordingly.

Three pieces of evidence make that verdict hard to argue with (§4.11): the only shipping product that
tried Wi-Fi fingerprinting for room identity — HoloLens — uses it strictly as a **prior over a real
visual localiser** and documents the misfire (*"if the Wi-Fi signals change significantly, the device may
think it is in a different space altogether"*); the one cheap signal with genuine pose content, the play
area, is in OpenVR literally a **width × depth rectangle**, and even a full polygon leaves a 180° yaw
ambiguity in a symmetric room; and, decisively, **the argument does not depend on fingerprints being
bad** — even a perfect identifier over a real localiser must fail closed, because relocalisation
false-positives are unrecoverable and the loop-closure literature is unanimous on it. Guessing the room
is not a cheaper way to be right; it is a more confident way to be wrong. Microsoft even has the name for
that outcome — a **wormhole** — and "the bedroom" is one.

### 7.2 A hierarchy of spaces is the right *data* model and the wrong *primary* abstraction

The user's sketch fuses two things that vary independently:

- **coordinates** — what is this pose relative to? That genuinely wants a parent-transform hierarchy,
  and building it is overdue (§3.2; `LAYOUT-AND-NAMING.md`; report 22 §2.3).
- **event policy** — what should happen to this object on don, on recenter, on frame loss? That is
  **per object**, not per space. Two objects in the same room legitimately want different answers: a
  persistent monitor should come to you on don; a wall-mounted clock should stay on the wall; a
  reference sheet should follow your body. Making "re-seat on don" a property of a *space* forces every
  object in that space to agree, and the first time you want an exception you either duplicate the space
  or add a per-object override — at which point the space was never the carrier.

**Counter-proposal:** a frame has a **resolution strategy** (how do I find this frame?). A binding has a
**hydration policy** and a **don policy** (what happens to me when my frame is/isn't found, and when the
user arrives?). A frame may carry *defaults* for its children's policies — that is where the user's
intuition is right and it costs nothing — but the policy lives on the binding.

**Two tiers, not a general hierarchy.** A few coarse, identity-bearing frames — the level at which
refusal lives — plus many composable per-object policy components. Three independent arguments land in
the same place: MRTK's Solvers are per-object components that compose through a shared goal transform,
never a parent space; **you cannot express a dead-band in a parent-child transform** — the deadzone,
spring and hysteresis that make body-following comfortable (§4.6) are behaviour, not geometry, and a
transform chain has nowhere to put them; and Khronos itself models spatial containment as a **flat UUID
list** (`XrRoomLayoutFB { floorUuid, ceilingUuid, wallUuids[] }`) rather than a transform tree. There is
also a safety argument: in a deep tree, one re-identified room silently teleports everything beneath it,
which is precisely the blast radius invariant I10 exists to bound.

**Both shipping platforms agree, and they agree by construction.** Meta puts the choice on the window's
own control bar — *"follow me" / "theater view" / "pin to space"* — and Apple puts it on a snap gesture
applied to the individual window, with the per-object choice explicitly overriding the global setting
("locked … will reappear after your Apple Vision Pro restarts, **even if you've turned off Reopen Apps
After Restart**"). Neither ships a *space* whose membership decides an object's don behaviour. §4.1.

Under that split, the user's "space we re-seat every time the user re-centers or re-dons after
inactivity" is not a new concept at all: it is **the `seat` frame**, whose resolution strategy is
*derive from the wearer at the don edge*. Which is to say — it is `LOCAL`, correctly used (§6.1).

### 7.3 "After a period of inactivity" is not measurable, and should not be the trigger

We can measure: time since presence went absent, time since the session stopped being VISIBLE, time
since the last tracked head sample. We **cannot** measure whether the user moved, because a doff is a
total observational blackout — no tracking, no odometry, no place sense. A five-second doff at the desk
and a five-second doff followed by walking to the bedroom produce byte-identical evidence.

So a time threshold is a proxy for "did the place change" with **no error bound**. Worse, it is
asymmetric in cost: re-seating unnecessarily is mildly annoying; *failing* to re-seat when the place
changed is the reported catastrophe. Which yields a rule that is both simpler and more defensible:

> **Re-seat when the world evidence does not vouch for the place. Hold when it does.**
> With no evidence at all — today's situation — that reduces to *always re-seat on don*, which is
> exactly the safe behaviour, and it is a **stronger** default than the current `recenter = hold`.

**The market agrees, twice over.** Virtual Desktop v1.32.5 added *"auto-arrange monitors on recenter"* and
shipped it **enabled by default** — the exact opposite of our default. And Meta's v77 note ("removes the
need for you to **constantly reset your view each time you move around the room**") is the same admission
from the shell side: pre-v77 the expected recovery from a moved user was a recenter that gathered the
windows. Report 22's survey of every PC-streaming client reached the same conclusion from the other
direction: *"No PC-streaming client does world-anchored session persistence. The universal pragmatic tier
is 'persist poses in STAGE and let Guardian carry the world lock' — which fails exactly when Guardian is
redrawn, i.e. the user's observed failure mode."* ALVR's `RecenteringMode` defaults to `LocalFloor`, not
`Stage`, and persists nothing.

**And the strongest counter-example, stated fairly, because it is a good one.** Sightful's Spacetop
release notes record the opposite verdict as a *bug fix*: v1.8.75.0 (2026-08-05) — **"Putting the glasses
back on no longer restarts calibration or reorients your workspace."** Auto-re-seat on don was shipped,
disliked, and removed. WiVRn's own lobby does the reverse and re-derives its panel from the head on every
don (§4.14). Both are defensible, and the difference is **what the content is**: an entry UI you need to
*find* should come to you; a workspace you were mid-task in should not be rearranged behind your back.
Our XR monitors are the second kind. So the recommendation is **not** "always re-seat" but *"re-seat when
the evidence does not vouch for the place"* — and the reason the fallback is re-seat rather than hold is
only that today the evidence is absent. As evidence improves, `auto` should re-seat **less** often, not
more. If that reads as a risk, `hold` remains one keyword away and the re-seat is one keybind away.

That is a direct pushback on the shipped default. `hold` was chosen to mean "keep my monitors in the
room". But with no verified world frame, `hold` does not hold a room — **it holds a coordinate system**,
and a coordinate system is precisely the thing that keeps being swapped. `hold` should become
*conditional*: hold if the world frame's generation is unchanged, otherwise re-seat and say so.

Time then enters in exactly one modest place: a `seat` frame older than some interval is not worth
holding on to across a session, because the odds that the wearer is still in the same chair decay. That
is a staleness heuristic on an explicitly ephemeral object, not a placement decision.

### 7.4 Body-attached objects are already built; the gap is at the seam, not the primitive

`HEAD` / `BODY` / `DEVICE` plus the adaptive dock↔roam decorator are shipped, tuned and live-validated
(`03-anchoring.md` §§4, 6). The MRTK "Solvers" taxonomy that the wider industry uses for this —
orbital / radial-view / follow / surface-magnetism / in-between / hand-constraint — maps onto what
exists with one or two genuine gaps (surface magnetism needs plane detection we do not have; "in-between"
is trivially expressible once a parent frame exists). **Do not rebuild this.**

The real defects are at the seam with the world/seat machinery, and they are all instances of §1.2:

- the adaptive **dock seat** (`m_dockHeadPos`) is a placement, in a frame, that is *not persisted* and
  carries no provenance — so a docked monitor's "desk" is forgotten on every session;
- capture is skipped while roaming (correct), which means the restore can replant a `DOCKED` capture
  that is minutes old;
- `BODY`'s `bodyHeight` is re-captured on the first tracked view pose and never serialised
  (`03-anchoring.md:768`), so a body layout silently re-anchors to the height of first wear.

Fixing those is a small part of the frame/provenance work, not a new subsystem.

Published tuning defaults worth having on hand when the body-attached layer is next touched, since we
currently have none to compare against: MRTK's `Follow` solver uses a 30°/20° view cone, a **60° reorient
deadzone** ("the element will not reorient until the angle between the forward vector and vector to the
controller is greater than this value"), a 0.3 / 0.7 / 0.9 m distance band and 0.1 s lerp times; Android
XR's `FollowBehavior.Soft` defaults to **1500 ms**; Oculus's guidance is content at 2–2.5 m within the
middle third of the field of view. Note also MRTK's implementation wart, which we should not copy:
`Vector3.Lerp(source, goal, deltaTime / lerpTime)` is frame-rate dependent — our critically-damped
`springStep` is already the correct form.

### 7.5 The bedroom bug is *not* the model working as designed on bad data

The user's own hypothesis, offered as a challenge, deserves a sharper answer. The data was not bad — the
numbers were an accurate description of an arrangement in the frame they were measured in. **The data
was unlabelled.** Nothing recorded which frame that was, so a frame swap changed the meaning of every
number in the process without changing a single bit of it.

That distinction matters because it picks the fix. "Bad data" suggests validation and clamping —
worthwhile (§3.4) but palliative. "Unlabelled data" points at provenance, which is a correctness fix:
with a frame id and a generation on every stored pose, the bedroom restore does not happen at all,
because the restore *refuses*.

### 7.6 One place the brief understates the problem

The brief treats don/doff/recenter as the event set. There is a fourth event that has caused at least as
much damage and is invisible in the framing: **the WiVRn reconnect** (§2.5), which resets the runtime's
accumulated recenter offset to identity, tears down and rebuilds our XR instance, and looks like nothing
at all. Any model that handles don/doff/recenter but treats "the session restarted" as a benign fresh
start will keep producing symptom B.

---

## 8. Invariants and acceptance scenarios

### 8.1 Invariants a correct system must satisfy

| # | invariant | today |
|---|---|---|
| **I1** | **Framed storage.** Every persisted pose carries `{frameId, generation, capturedAt, evidence}`. There are no bare poses. | violated — §1.2 |
| **I2** | **Refusal over guessing.** An object whose frame is not `RESOLVED` is not hydrated at its stored pose. A fallback may be applied only if the binding's policy asks for it, and the substitution is visible. | violated — no refusal exists |
| **I3** | **Measurement validity.** No durable state is ever derived from a pose that is not `POSITION_TRACKED` **and** `ORIENTATION_TRACKED` **and** finite. | violated — `TRACKED` bits unchecked (§3.4.1), no finiteness checks (§3.4.2) |
| **I4** | **Authored beats live.** An arrangement has an authored pose that user actions change and system repairs restore toward. No repair derives its target from the live pose it is repairing. | violated — §3.2 |
| **I5** | **Idempotence.** Applying any repair twice is the identity. | holds for `reseat` (gtest) |
| **I6** | **Rigidity.** A group repair preserves the relative arrangement exactly. | holds |
| **I7** | **Gravity.** Every frame-to-frame transform is 4-DoF; pitch and roll are never solved for. | holds in the solve; violated in the *stored* restore offset, which carries a full quaternion (§3.4.5) |
| **I8** | **Explicability before the fact.** For every object, `hyprctl openxr` can answer *before* the next don: which frame, which generation, when captured, and what will happen when I put the headset on. | partly — `restore` is shown, provenance and the prospective outcome are not |
| **I9** | **No silent drops.** Every discarded event, refused capture and skipped repair is logged with its reason. | violated — the reference-space type filter drops silently (§3.3 #2) |
| **I10** | **Bounded blast radius.** An unresolvable frame breaks only the objects bound to it. | n/a — one frame today, so every failure is global |
| **I11** | **Durable state is never written from the frame path.** Persistence is a main-thread, edge-triggered commit. | violated — `m_restoreOffset` is written by the frame thread every frame (§1.4). SteamVR has the same bug (ValveSoftware/openvr#994); H2 removes ours. |
| **I12** | **One hydration path, taken on an edge.** The initial state is delivered as a synthesised edge rather than handled by a separate branch. | violated — first-plug RESTORE, the recenter fallback, `GROUP`, and reload reconciliation are four branches (§1.6). Unity's `WorldAnchor` pattern is the fix: *"call the `OnTrackingChanged` handler with the initial `IsLocated` state after attaching an anchor."* |
| **I13** | **Only states we actually emit are documented.** No aspirational vocabulary. | n/a — but worth pinning now: OpenVR's `IVRSpatialAnchors` defines can't-place states its own runtime never emits (§4.9). |
| **I14** | **A repair never puts content out of reach.** Any placement a system action produces must be within grab range and within the field of view from the pose that produced it. | violated — the 5 m parking of §3.2, and the same trap is Immersed's most-reported failure ("reset screen positions places screens in invisible locations"). |

### 8.2 Acceptance scenarios

The seed corpus is the set of situations that have actually broken, plus the ones the current tests
already cover. `hyprtester/src/tests/xr/anchors.cpp` (`xr_anchor_restore_across_session`,
`xr_reseat_verb`) shows these are scriptable headlessly against the null runtime, and
`tests/xr/anchor_math.cpp` covers the pure math — so most of this is a gtest + hyprtester exercise, not a
headset exercise. Scenarios marked **[HMD]** need the real Quest.

| # | scenario | expected under the recommended model |
|---|---|---|
| S1 | Place a monitor. Doff at the desk. Don 30 s later, same chair. | Unchanged. World frame generation matches → hold. No motion at all. |
| S2 | Place a monitor. Doff. Walk to another room **while doffed**. Don. | World frame unresolvable or generation changed → world-bound objects **not hydrated** (outputs stay unplugged, workspaces stay evacuated, status says why); seat-bound objects re-seat in front of the user. Never a monitor in the bedroom. |
| S3 | Stand up, **walk out of the room while still wearing**, then doff. Don at the desk later. | The capture must not have recorded the walk. Authored pose is what returns. (This is §3.3 #1 and is the single most important regression test.) |
| S4 | Press the headset's recenter button, mid-session, having turned 90°. | World-bound objects do not move (they are in STAGE, which recenter does not touch). Seat-bound objects follow the seat. Both outcomes are stated in `status`, not inferred. |
| S5 | `wivrn-server` restart with the compositor untouched. | Instance loss → new session → world frame re-resolved by generation. If it matches: everything comes back exactly where it was, with no re-seat. If not: refusal per S2. **Never** the 5 m parking of §3.2. |
| S6 | After any bad restore, press `xrmonitor reseat`. | The group returns to its **authored** viewing distance, not to `clamp(current, 0.3, 5)`. Pressing again is the identity. |
| S7 | Compositor restart (config unchanged). | Declared bindings re-hydrate from config; runtime-created bindings re-hydrate from the state file if their frame resolves, otherwise are listed as dormant rather than lost. (Requires the state file — none exists today, §1.2.) |
| S8 | Don with the headset in your hand, face-down on the desk, tracking valid-but-not-tracked. | No capture, no re-seat, no durable write. `status` says the head sample was refused. (I3) |
| S9 | Config reload that changes a declared `pos:`. | The new declaration wins; provenance is re-stamped; the prior capture is discarded — and *says* it was discarded. |
| S10 | An adaptive monitor is roaming when the session ends. | The **docked** pose, with its own provenance, is what persists. Roaming is never remembered. |
| S11 | Two world frames declared (desk, couch); user at the couch. | Only the couch frame resolves; desk-bound objects are dormant, not mis-placed. (Model 3 territory; must not be *wrong* under Model 2.) |
| S12 | **[HMD]** Quest Space re-setup / boundary redraw **mid-session, connection unbroken**. | With a valid delta: the fork corrects it and nothing moves (this already works — §2.4). With an invalid one: the generation bumps, world-bound objects are marked stale and refuse or re-seat per policy. **Today the invalid case is silent at the compositor** — this is W4's acceptance test. |
| S12b | **[HMD]** Doff, carry the headset to another room, don. Or disconnect and reconnect there. | Nothing observed the change and the dead-reckoned correction was reset, so there is *no delta at any layer*. The generation must be treated as unknown and world-bound objects must **refuse**. This is the case that only a frame identity can serve (§7.1), and the one the whole "unfindable ⇒ unhydrated" requirement exists for. |
| S13 | **[HMD]** Carry a donned headset to another room without doffing. | Continuous tracking, so the world frame is still valid — objects stay in the old room, correctly, and the geofence/adaptive layer is what brings anything with you. Distinguishes "the model held" from "the model got lucky". |
| S14 | XREAL 3DoF session (no position). | World frames degrade to yaw-only; anything requiring translation refuses rather than guesses. (`XREAL-3DOF.md`.) |
| S15 | Runtime lacks `XR_EXT_local_floor`; `floor_offset` fallback in play. | The floor regime is part of provenance; a pose captured under one regime is not silently restored under the other. (§3.4.6) |
| S16 | Hydrate the same binding twice — a replug inside the unplug grace, a double don edge, a RESTORE racing a GROUP re-seat. | **Idempotent.** No duplicate monitor, no duplicate binding, no drift per repetition. Apple's FB13713944 (§4.8) is what happens when a refusing system is not idempotent: it quietly becomes a duplicating one. |
| S17 | A world-bound monitor hydrates, but the workspace or application it existed to display does not. | Named and surfaced, never silently half-restored. This is Apple's "the tools window shows up all alone with nothing to modify" (§4.8); the answer is a per-binding opt-out, the analogue of `restorationBehavior(.disabled)`. |

---

## 9. Migration — what breaks, what is config, what is a rewrite

### 9.1 The config surface maps almost entirely by aliasing

| today | under the model | user-visible change |
|---|---|---|
| `xrmonitor = …, anchor:local pos:x,y,z yaw: pitch:` | `anchor:seat pos:…` — *the same semantics it already has* (a rig relative to the wearer, §1.3). `local` stays an accepted spelling forever. | **none** |
| — | **new** `anchor:world [frame:<name>] pos:x,y,z yaw: pitch:` — durable, refuses rather than guesses | additive |
| `adaptive:on roam:body roam_offset: …` | unchanged; the dock seat becomes a framed, persisted placement | none |
| `anchor:head` / `anchor:body` / `anchor:device:left` | unchanged | none |
| `openxr:recenter = hold\|follow` | gains `auto` (recommended default): *hold if the world frame's generation is unchanged, else re-seat and say so*. `hold`/`follow` keep their exact meanings. | default changes; §7.3 argues it should |
| `openxr:recenter_on_plug = bool` | becomes the default **don policy** for seat-bound objects (`reseat`), with `hold` and `refuse` as the other values. The boolean keeps parsing. | none |
| `openxr:default_distance` | becomes the **authored** distance a new binding is created at, and what `reseat` returns to (§3.2) | none, but the verb starts behaving |
| `hyprctl openxr layout` | emits frame-qualified lines. For a `world` binding the numbers are finally durable — which is the first time this command has been worth pasting (report 22 §1) | additive |
| `hl.xr_monitor{ anchor = "local", … }` (Lua, `00d808379`) | same aliasing; `frame = "desk"` added | additive |
| — | **new** state file `$XDG_STATE_HOME/hypr/xr-spatial.json`: frames (id, kind, generation, last solve, evidence) + bindings (frame ref, pose, provenance, policies). There is no disk state today at all (§1.2). | additive |
| — | **new verbs** `hyprctl openxr frame list\|register <name>\|forget <name>`, `xrmonitor rehome <frame>`. `xrmonitor reseat` keeps its spelling and gains correct behaviour. | additive |

**Net: no breaking config change.** Every existing `hyprland-xr.conf` keeps working with identical
behaviour, because today's `anchor:local` genuinely *is* a seat binding — the rename is telling the truth
about what it already does.

### 9.2 What is a rewrite

- **`SXRAnchorState` splits.** Today one struct is both the stored record and the solved state, with
  `anchorPose` meaning four different things by mode (`XRAnchor.hpp:213-235`). It becomes a **binding**
  (frame ref + pose + provenance + policies, serialisable, main-thread-owned) and a **solve output**
  (frame-thread-owned, POD, non-owning). This also cleans up the four-call-sites/three-rules mess of
  §1.2 by making "which pose do I mean" a type distinction rather than a convention.
- **A frame registry** appears — the object report 03 WP-G2 and `LAYOUT-AND-NAMING.md` have both asked
  for. Two-phase resolve per frame: solve the parent once, compose children. Frame-thread-safe by the
  usual rules (POD, no refcounts, no string config reads — `xr-threading-rules`).
- **Hydration gating reuses machinery that already exists.** `monitors_follow_session = visible` already
  implements "an XR monitor behaves like an unplugged external monitor, workspaces evacuate and return
  by name" (`05-configuration.md:197`). "Do not hydrate" is that same path with a different predicate.
  This is the single biggest reason the user's request (a) is cheaper than it sounds.
- **The re-seat inverts.** From "compute a seat frame from the children and replant each child" to "move
  the seat object; children compose". `xrGroupSeatFrame` survives only as the *initialiser* for a seat
  that has never been authored.

### 9.3 What genuinely does not carry over

- The **`m_restoreOffset` capture-every-frame** design (§1.4) is replaced by an authored pose plus an
  explicit commit at the doff edge. Existing in-memory offsets are discarded on upgrade; the first
  session after upgrade behaves like a first session.
- **Flat SBS stereo (XReal)** stays outside the model. It is a scanout packing on an ordinary output
  (`ConfigManager.cpp:1543-1548`), with no pose at all. The one decision to record: a "world" for a 3DoF
  or 0DoF display is yaw-only or degenerate, and the model must be able to say *this frame kind is not
  available here* rather than fabricate one (`XREAL-3DOF.md`, scenario S14).

---

## 10. Work plan

Sized in the usual coarse buckets. **M0 gates P3 and P5**; P1 and P2 are independent and should ship
regardless of what M0 finds.

### Track M — measure first

- **M0 — the STAGE probe.** *(tiny; one session)* Create `XR_REFERENCE_SPACE_TYPE_STAGE` alongside the
  existing spaces, log `T_localfloor←stage` at session start, on every reference-space change, on every
  don/doff edge and every N seconds, and log whether `xrGetReferenceSpaceBoundsRect` returns anything.
  Then run one attended session that does: recenter ×2, doff/don, `wivrn` reconnect, and — if the user is
  willing — a Quest Space re-setup. **This answers the one question the whole recommendation hangs on:
  does the Quest publish a stable, room-persistent STAGE while our fork suppresses the boundary?**
  (§2.6.) Everything else in Track W is contingent on it.

### Track H — honest failure (Model 1; ship regardless)

- **H1 — validity gates.** *(small)* Require `POSITION_TRACKED_BIT | ORIENTATION_TRACKED_BIT` and
  finiteness on every head/grip sample that feeds durable state; refuse and log otherwise. (I3; §3.4.1-2)
- **H2 — commit at the edge, *and* debounce-write continuously.** *(small)* Replace last-write-wins
  capture with an explicit commit on the doff / visibility-drop / session-stop edge, plus a plausibility
  bound on the delta from the previous committed value. But do **not** make edges the only trigger:
  WayVR's `save_state()` has exactly two call sites and its issue #529 is the post-mortem — an unclean
  WiVRn exit loses the session (§4.14). A debounced periodic write alongside the edge commit costs
  nothing and removes a whole class of "it didn't save my layout" report. (§3.3 #1 — this is the fix for
  symptom A's most likely cause.)
- **H3 — provenance.** *(small-medium)* `{frameId, generation, capturedAt, evidence}` on every stored
  pose; surfaced in `hyprctl openxr status` and `-j`. (I1, I8)
- **H4 — no silent drops.** *(tiny)* Log the reference-space type filter rejection; log every refused
  capture and skipped repair with its reason; reset `m_lastVerbCtx` in `stop()`. (I9; §3.4.3)

- **H5 — mint our own generation ID.** *(small)* A UUID per session, on
  `XR_EXT_stationary_reference_space`'s published semantics (§5.5): persisted beside every placement,
  compared at startup, invalidated by any event that could have moved the world — the STAGE change of W4,
  a reconnect, a recenter we could not reconstruct. Nothing world-locked hydrates on a mismatch. This is
  the client half of the extension, needs nothing from WiVRn or monado, and **upgrades in place** the day
  a runtime supplies a real ID. It is also the only thing that addresses scenario S12b, which no amount
  of event plumbing can reach.

### Track S — the seat as an object (Model 1→2 bridge)

- **S1 — authored distance.** *(small)* Give a group an authored seat pose; `reseat` returns to it rather
  than to `clamp(live, 0.3, 5)`. Kills symptom B outright. (§3.2, I4, scenario S6)
- **S2 — seat object + registry skeleton.** *(medium)* One `seat` frame per session; bindings compose
  against it; `reseat` moves the frame. `xrGroupSeatFrame` demoted to an initialiser.

### Track W — the world frame (Model 2 proper; gated on M0)

- **W1 — STAGE + `anchor:world`.** *(medium)* Create the space, add the binding kind, plumb the two-phase
  resolve.
- **W2 — the discontinuity detector.** *(small, and it works today with no upstream change)* A STAGE swap
  moves *root*, so both LOCAL_FLOOR and STAGE move together and their relative transform is unchanged —
  the swap is invisible in the transform. But it is **not** invisible in the head: across two
  consecutively-tracked frames, a head displacement larger than physically possible is a frame swap, not
  a person. Bump the generation on it. This is the backstop for the changes the runtime **does not**
  announce — the spec explicitly permits a runtime to "introduce slight adjustments to the origin of each
  space on a continuous basis" with no event at all (§5.1) — and for any layer between us and the headset
  that loses one. It cannot catch a swap that happens across a doff or a reconnect; nothing observational
  can, which is why W1's generation token exists (§7.1).
- **W3 — hydration gating, on one edge.** *(medium)* `UNRESOLVED` world frame → bound outputs stay
  unplugged, workspaces stay evacuated, status explains. Reuses the `monitors_follow_session` path.
  Build it as a single locatability-edge handler with the initial state delivered as a synthesised edge
  (I12, Unity's `WorldAnchor` pattern, §4.10) rather than as another branch beside RESTORE and GROUP.
  (I2, S2/S11, S16)
- **W4 — accept the STAGE change we are already being sent.** *(tiny; entirely in our tree)* The client
  and server halves are **already deployed** in `wivrn-xg` (§2.4). Widen the filter at
  `src/openxr/XRSession.cpp:416-417` to accept `XR_REFERENCE_SPACE_TYPE_STAGE`, treat it as a generation
  bump on the world frame, and log every rejection instead of returning silently. **Highest
  leverage-to-size ratio in this report** — it converts an already-detected corruption from silent into
  actionable. Companions, both optional and both larger: upstream the `wivrn-xg` client/server halves to
  `~/code/wivrn` (they are squarely with upstream's grain — this is "stop dropping a spec-mandated event",
  not runtime-side origin fudging); and implement `get_reference_bounds_rect` so the play-area extents
  reach the PC at all (a room *disambiguator* per §7.1, not a localiser).
- **W5 — persistence.** *(medium)* The state file; `hyprctl openxr layout` durability; survive a
  compositor restart. Ship `frame list` / `frame forget` **in the same change** — SteamVR accumulated 21
  never-garbage-collected universes with no user-facing list, and a user in the wrong room could neither
  see it nor clear it (§4.9). (Scenario S7)

### Track R — runtime (only if the runtime becomes ours; see §5.8)

- **R1 — `XR_EXT_stationary_reference_space`.** Implement the ratified extension rather than inventing
  one. This is the single primitive that makes request (a) *correct* rather than *best-effort*.
- **R2 — tell the truth on recenter.** Emit `poseValid = true` with the real `poseInPreviousSpace` that
  `recenter_local_spaces` already computed and throws away (`u_space_overseer.c:874-876`). This is a bug
  fix, not an extension, and it deletes the entire head-pair reconstruction path
  (`solveReferenceSpaceChangeFromHead`) from every client on the platform.
- **R3 — frame identity/generation** on reference spaces, and the anchor/persistence family
  (`XR_EXT_spatial_entity` / `_anchor` / `_persistence`) when there is a reason.
- **Explicitly NOT in the runtime:** body-attached follow behaviour, seat policy, hydration policy,
  layout. §5.8 argues the boundary.

### Sequencing

```
M0 ──┬──────────────────────────────▶ W1 ─▶ W2 ─▶ W3 ─▶ W5
     │        (W4 is independent of M0: the event already arrives — accept it now)
H1 ─▶ H2 ─▶ H3 ─▶ H4 ─▶ H5   (independent, ship first)
S1 ─▶ S2                     (independent, ship second)
                                              …later: Model 3 / locations / grids (report 22 §9)
```

**If only three things are built:** W4 (accept the STAGE event — hours, not days, and it is the one
place where the rest of the stack is ahead of the compositor), H2 (commit at the edge), S1 (authored
distance). Those three address the blindness underneath both symptoms, symptom A's likeliest cause, and
symptom B exactly.
---

## 11. Do not

A short section, because three plausible-sounding directions would cost months and buy little.

- **Do not build room fingerprinting** (WiFi BSSID sets, play-area shape matching, magnetometer, ambient
  audio) as a way to find the world. It answers *identification* and the problem is *localisation* (§7.1).
  It becomes worth building only when there are several stored world frames and a real localiser to pick
  between them — which is a later phase of a later phase.
- **Do not chase Meta's anchor API.** Report 22 §3.1.2 established the case and nothing since changes it:
  anchors are per-app and private, Meta's own accuracy guidance is a three-metre rule, a space-history
  clear destroys them, the sideloaded-package gate requires a developer-team account, and — decisively —
  the compositor can never call the API at all, because monado implements no anchor extension. A
  Khronos reference space is a far smaller, far more upstreamable client patch, and §5.5 makes it the
  strictly better target.
- **Do not deepen the hierarchy before there is evidence to resolve it.** Model 3 (§6.3) is the right end
  state and it buys nothing over Model 2 while there is exactly one room and no anchors. A three-level
  tree with one resolvable frame is a more elaborate way of guessing (§6.4).
- **Do not rebuild body-attached anchoring.** It is shipped, tuned, live-validated and matches both
  vendors' published guidance (§4.6, §7.4). The defects are at the seam — an unpersisted dock seat, an
  unserialised `bodyHeight` — not in the primitive.
- **Do not write the measured solve back into the user's config.** Report 22 §5.2 settled this and it
  still holds: the compositor writes a state file, `hyprctl openxr layout` prints for pasting, and
  `~/.config/hypr` stays the user's.

---

## 12. Open questions for the user

1. **`M0` needs one attended session.** Are you willing to run a build that creates a STAGE space and
   logs `T_localfloor←stage` across a recenter, a doff/don, a WiVRn reconnect, and — the important one —
   a Quest **Space re-setup**? Everything in Track W is contingent on the answer, and there is no way to
   get it from the desk. (If a Space re-setup is too disruptive, the first three still settle most of it.)

2. **What should an unhydrated monitor look like?** The recommendation is that it stays *unplugged* —
   workspaces evacuated, exactly as `monitors_follow_session = visible` already does on a doff. That is
   the honest behaviour and it reuses shipped machinery. But it means that after a room change you may
   don the headset and see **nothing**, with an explanation available only via `hyprctl openxr status`.
   Alternatives: hydrate a single "recovery" monitor in the seat frame carrying a message; or hydrate
   everything into the seat frame and mark it provisional (ghosted chrome). Apple hides; Meta prompts and
   offers to re-place (§4.3). **This is the one place where the two shipping platforms disagree, and it
   is your call, not the evidence's.** My inclination is HoloLens's answer (§4.10), which is the only one
   validated on real users over a decade: a named, visible **limited mode** — "still looking for your
   space", you cannot see previously placed content, and there is a per-space forget — rather than either
   silence or a silent re-place.

3. **Should `openxr:recenter` default change from `hold` to `auto`?** §7.3 argues yes: `hold` currently
   holds a coordinate system rather than a room, and with no verified world frame the safe default is to
   re-seat. Under `auto` the headset's recenter button would gather seat-bound monitors and leave
   world-bound ones alone — which is also what a Quest-native user expects (§4.2). The cost is that the
   shipped default changes behaviour for you on day one.

4. **Do you want the `wivrn-xg` stage-correction work upstreamed?** The compositor half (W4) is ours and
   uncontroversial. The client/server halves are already written, deployed and — on the evidence of
   report 22 §3.2 — squarely with upstream's grain: maintainers there have rejected *runtime-side* origin
   fudging ("It's for vendors to fix their software"), while this is strictly "stop dropping a
   spec-mandated event and tell applications when you cannot compensate". But it adds a control packet and
   touches the streaming event pump, so it is a real PR, not a patch to sit on.

5. **How many rooms do you actually want?** The design supports N world frames, but the ritual, the
   disambiguation and the per-location layouts (report 22 §5) are a large fraction of the remaining work.
   If the honest answer is "the office, and occasionally the couch in the same room", Model 2 with one
   world frame plus the seat is the whole feature and Track W ends at W5.

6. **Do you want the `xrframe register` ritual at all, or only the automatic rungs?** Report 22 called the
   ritual the gamble-killer and recommended shipping it first. With STAGE available (§2.6) the automatic
   rung may be good enough on its own, and the ritual becomes the fallback for when M0 says STAGE is
   unusable. That inverts report 22's sequencing, and it is worth confirming you agree before B-track work
   is planned around it.

7. **Report 22 should get a dated pointer to this report** (house convention: a banner, not a rewrite),
   and `research/README.md` needs a row for 28. Neither is done here — this branch carries only the new
   file. Say the word and both are a two-line follow-up.
---

## 13. Sources

### 13.1 This tree (`67200a838`)

| claim | file:line |
|---|---|
| only `LOCAL_FLOOR`/`LOCAL` + `VIEW` created; no STAGE anywhere in `src/` | `src/openxr/XRSession.cpp:265-293` |
| reference-space change event handling + type filter | `src/openxr/XRSession.cpp:413-424` |
| user-presence event | `src/openxr/XRSession.cpp:403-412` |
| session-state collapse | `src/openxr/OpenXRManager.cpp:1153-1160` |
| head locate — `VALID` bits only, never `TRACKED` | `src/openxr/OpenXRManager.cpp:1956-1957`; grips `src/openxr/XRInput.cpp:344`; views `:1671` |
| the three-rung recenter ladder | `src/openxr/OpenXRManager.cpp:1517-1543`, `:1978-2029`; `xrRecenterFix` `src/openxr/XRMath.hpp:290-296` |
| head-pair delta reconstruction + the monado rationale | `src/openxr/XRMath.hpp:241-273` |
| re-seat arming (CAS-max `NONE < GROUP < RESTORE`) | `src/openxr/OpenXRManager.cpp:4228-4237`; rationale `OpenXRManager.hpp:985-988` |
| re-seat consumption; RESTORE plants `m_restoreOffset` else `m_declaredAnchor` | `src/openxr/OpenXRManager.cpp:2101-2158`, esp. `:2149-2155` |
| **the group seat frame and its distance clamp** | `src/openxr/XRAnchor.hpp:320-359`, esp. `:350-353` |
| the clamp asserted as intended behaviour | `tests/xr/anchor_math.cpp:1274-1288` (`XRGroupSeat.ViewingDistanceIsClamped`) |
| the every-frame restore capture and its gates | `src/openxr/OpenXRManager.cpp:2169-2195`; gate fold `:4197-4222` |
| `xrHeadFrame` / `xrPoseInHeadFrame` | `src/openxr/XRMath.hpp:310-312`, `:323-325`; yaw fallback `:170-175` |
| `SXRAnchorState` — one struct, four meanings | `src/openxr/XRAnchor.hpp:213-235` |
| the monitor-lottery commentary | `src/openxr/XRAnchor.hpp:237-262`, `:392-399` |
| ad-hoc monitor stores a dead-frame pose as its "declared" anchor | `src/openxr/OpenXRManager.cpp:2786-2801` |
| adaptive dock↔roam machine, seat capture, hysteresis | `src/openxr/XRAnchor.cpp:483-571`, esp. `:496-513` |
| `m_lastVerbCtx` written, read, never reset | `src/openxr/OpenXRManager.cpp:2086`, `:5360-5363` |
| serialization drops roll; yaw/pitch to 0.1° | `src/openxr/XRMonitorConfig.cpp:717-770`, esp. `:735-742` |
| plug gate needs visibility **and** presence (sticky-presence rationale) | `src/openxr/XRMonitorConfig.hpp:198-211`; predicate `XRMonitorConfig.cpp:366-389` |
| no disk persistence of spatial state | `rg -niE "XDG_STATE_HOME\|\.local/state\|ofstream\|fopen\(\|std::filesystem" src/openxr/` → 0 hits |
| existing headless coverage for restore + reseat | `hyprtester/src/tests/xr/anchors.cpp` (`xr_anchor_restore_across_session`, `xr_reseat_verb`) |
| the live config | `~/.config/hypr/hyprland-xr.conf:155`, `:175` |

### 13.2 monado (`~/code/monado`, `v25.1.0-271-gc2ddab59d`)

| claim | file:line |
|---|---|
| recenter offsets LOCAL + LOCAL_FLOOR only; STAGE untouched | `src/xrt/auxiliary/util/u_space_overseer.c:~830-895` |
| **`pose_valid = false` + identity pose, hard-coded** | `src/xrt/auxiliary/util/u_space_overseer.c:874-876` |
| events pushed for both LOCAL and LOCAL_FLOOR | `src/xrt/auxiliary/util/u_space_overseer.c:879-891` |
| STAGE = a pose space if the driver has it, else a **null space equal to root** | `src/xrt/auxiliary/util/u_space_overseer.c:1266-1273` |
| LOCAL = root + 1.6 m Y; LOCAL_FLOOR = root + local's X/Z/yaw at y = 0 | `src/xrt/auxiliary/util/u_builders.c:216-227`; `u_space_overseer.c:1281-1290` |
| STAGE enumerated iff `semantic.stage != NULL`; UNBOUNDED iff `semantic.unbounded != NULL` | `src/xrt/state_trackers/oxr/oxr_system.c:295-342` |
| STAGE resolution, and the `map_stage_to_local_floor` quirk | `src/xrt/state_trackers/oxr/oxr_space.c:113-118`; quirk `oxr_instance.c:57`, `:238` (env `OXR_RECENTER_STAGE`) |
| bounds forwarded to the compositor (which WiVRn does not implement) | `src/xrt/state_trackers/oxr/oxr_space.c:193-215` |
| what monado actually implements | `src/xrt/state_trackers/oxr/extension_support/oxr_extension_support.py` |

### 13.3 WiVRn (`~/code/wivrn`, the upstream-tracking fork, `v26.6.1-2-ge54b56fe`) — *not the deployed one*

| claim | file:line |
|---|---|
| the client's world space is **STAGE**, created once | `client/application.cpp:1351` |
| **the reference-space change event is swallowed** | `client/application.cpp:1932` (bare `break;`) |
| **…and filtered to `== LOCAL` in the stream scene** | `client/scenes/stream.cpp:1299-1302` |
| the recenter flag on the wire (one bit) | `common/wivrn_packets.h:301-304`; set at `client/scenes/stream_tracking.cpp:456` |
| server turns that flag into a monado recenter | `server/driver/wivrn_session.cpp:596-601` |
| `per_app_local_spaces = false`, `root_is_unbounded = false`, fresh overseer per session | `server/driver/wivrn_session.cpp:298-310` |
| user presence forwarded end to end | `common/wivrn_packets.h:525`; `client/scenes/stream.cpp:1327`; `server/driver/wivrn_session.cpp:757` |
| no `get_reference_bounds_rect`, no boundary, no anchors, no space identity | absent from `server/`, `common/wivrn_packets.h` |
| boundary suppression (the M0 risk) | `ddb5a8ce` (`BOUNDARYLESS_APP`), `e54b56fe` (`hypxr/GUARDIAN-DISABLE.md`, `XR_META_boundary_visibility`) |

### 13.4 WiVRn-XG (`~/code/wivrn-xg`, branch `hypxr` — **the deployed lineage**)

| claim | file:line |
|---|---|
| the client forwards the STAGE change instead of dropping it | `client/scenes/stream.cpp:1329-1359` |
| the wire packet | `common/wivrn_packets.h:628-634` (`from_headset::reference_space_changed`) |
| the server composes the correction and parks the tracking origin | `server/driver/wivrn_session.cpp:873-960` |
| …and pushes a STAGE `REFERENCE_SPACE_CHANGE_PENDING` when the pose is invalid | `server/driver/wivrn_session.cpp:894-912` |
| the contract: ingress only, dead reckoning, reset at the boundaries | `server/driver/wivrn_session.h:112-132` |
| reset on reconnect | `server/driver/wivrn_session.cpp:842-871`, called at `:1901` |
| provenance: `c8910153` (2026-08-03) added it; `38a8cdfe` (2026-08-17) deleted the spurious egress half | fishfood findings log, 2026-08-17 |

### 13.5 OpenXR specification and registry

**System headers on this box: `/usr/include/openxr/openxr.h`, `XR_CURRENT_API_VERSION 1.1.60`** — it
already defines `XR_REFERENCE_SPACE_TYPE_STATIONARY_EXT = 1000742000` (`:1012`), the two generation-ID
structs (`:934-935`) and `xrGetStationaryReferenceSpaceGenerationIdEXT` (`:12610-12634`). Extension **742**;
743 is `XR_EXT_spatial_marker_tracking`.

Spec sources: `KhronosGroup/OpenXR-Docs@main` = **OpenXR 1.1.62 (2026-07-31)**; rendered at
https://registry.khronos.org/OpenXR/specs/1.1/html/xrspec.html (403s to automated fetch; the asciidoc
sources carry identical normative text). Local SDK `~/code/OpenXR-SDK-Source` is **1.1.57**, which
predates `XR_EXT_stationary_reference_space`.

- Spaces, the no-global-origin statement, continuous origin adjustment, `xrLocateSpace` semantics and the
  four location flags — `specification/sources/chapters/spaces.adoc`.
- `LOCAL` / `LOCAL_FLOOR` / `STAGE` definitions and their change-event obligations — same file; man pages
  at https://registry.khronos.org/OpenXR/specs/1.1/man/html/XrReferenceSpaceType.html and
  `.../XrEventDataReferenceSpaceChangePending.html`.
- Session states and the `may:`-only don/doff hints — `chapters/session.adoc`.
- `XR_EXT_user_presence` (rev 1, 2023-04-22) — `chapters/extensions/ext/ext_user_presence.adoc`.
- **`XR_EXT_stationary_reference_space`** — ext **743**, rev 1, **OpenXR 1.1.59, 2026-04-30**,
  `supported="openxr"` (not yet `ratified`); `chapters/extensions/ext/ext_stationary_reference_space.adoc`;
  provenance in `CHANGELOG.Docs.md`. Meta's predecessor `XR_EXTX2_stationary_reference_space`
  (`XR_REFERENCE_SPACE_TYPE_STATIONARY_EXTX2 = 1000742000`) ships in
  `meta-quest/Meta-OpenXR-SDK` → `OpenXR/meta_openxr_preview/extx2_stationary_reference_space.h`, SDK v77
  (2025-06-06) through v85; Godot wrapper in `godot_openxr_vendors` PR #418.
- **The ratified cross-vendor family** — `XR_EXT_spatial_entity` (741), `_spatial_anchor` (763),
  `_spatial_persistence` (764), `_persistence_operations` (782), all **ratified into 1.1.49, 2025-06-10**;
  `chapters/extensions/ext/ext_spatial_*.adoc`.
- `XR_MSFT_unbounded_reference_space` (2019-07-30) — the `poseValid` true/false split that is the original
  wrong-room detector.
- `XR_ML_localization_map` (2023-09-14) — localization state, confidence, error flags, and the
  cross-instance origin guarantee; `chapters/extensions/ml/ml_localization_map.adoc`.
- `XR_MSFT_spatial_anchor` + `_persistence`, `XR_FB_spatial_entity` + `_storage`/`_query`/`_sharing`,
  `XR_META_spatial_entity_persistence`/`_discovery`, `XR_HTC_anchor`, `XR_BD_spatial_anchor`,
  `XR_ANDROID_device_anchor_persistence` (deprecated) — see §5.6.
- `XR_EXTX_overlay` — still `provisional="true"`, untouched since 2021-01-13.
- **"recenter" appears zero times in `xr.xml`, across all 862 extensions.**

### 13.6 Apple visionOS

- https://support.apple.com/en-us/118515 — "apps stay where you placed them"; lock-in-place; "even if you
  re-center your space"; locked beats *Reopen Apps After Restart*.
- https://support.apple.com/guide/apple-vision-pro/move-resize-and-close-app-windows-dev009366408/visionos
  — locking the same app in multiple rooms.
- https://support.apple.com/en-us/124816 — widgets per room/space; "widgets need to be attached to a
  surface"; "increase the lighting in your room".
- https://support.apple.com/guide/apple-vision-pro/recenter-your-view-tan5f2b0eb70/visionos — recenter.
- WWDC23 10082 https://developer.apple.com/videos/play/wwdc2023/10082/ — recenter moves the app origin;
  anchors excluded; only IDs+transforms persisted; location-based maps and relocalization; hide untracked
  content.
- WWDC24 10100 https://developer.apple.com/videos/play/wwdc2024/10100/ — `RoomTrackingProvider`, room
  transitions, orientation-only degradation.
- WWDC25 290 https://developer.apple.com/videos/play/wwdc2025/290/ — lock to room, snapping,
  `onWorldRecenter`, immersive spaces not restored.
- WWDC25 255 / 317 — spatial widgets, mounting styles, proximity level-of-detail.
- API: https://developer.apple.com/documentation/arkit/worldtrackingprovider ,
  `.../worldanchor` , `.../roomtrackingprovider` , `.../roomanchor` ,
  https://developer.apple.com/documentation/realitykit/anchoringcomponent (**inactive until anchored**),
  `.../anchorentity` , `.../anchoringcomponent/trackingmode-swift.struct` ,
  https://developer.apple.com/documentation/swiftui/scenerestorationbehavior ,
  `.../surfacesnappinginfo` , `.../environmentvalues/worldtrackinglimitations` .
- HIG: https://developer.apple.com/design/human-interface-guidelines/spatial-layout — avoid head-anchoring;
  rely on the Digital Crown.
- Anchor scope (Apple engineer): https://developer.apple.com/forums/thread/756829 .
- **FB19610114** — room A→B→A + recenter kills translation system-wide:
  https://developer.apple.com/forums/thread/796861 , https://developer.apple.com/forums/thread/790041 .
- Duplicate accumulation from failed relocalization (FB13713944):
  https://developer.apple.com/forums/thread/749716 .

### 13.7 Meta Horizon OS

- **https://developers.meta.com/horizon/documentation/unity/unity-ovrcamerarig/ — "On Quest, the Stage
  tracking origin will not directly respond to user recentering"**; recenter is a yaw-only re-plant.
- https://developers.meta.com/horizon/design/windows/ — the control bar's *"follow me" / "theater view" /
  "pin to space"*.
- https://www.meta.com/en-us/help/quest/articles/whats-new/release-notes/ — v77 "Move with you windows";
  v81 "Pin Windows to Worlds"; v2.1 wall snapping; v62/63 fifteen spaces.
- https://developers.meta.com/horizon/documentation/unity/unity-scene-overview/ — the scene model, "up to
  15 rooms", "may locate some or all of the rooms depending on the user's current location".
- https://developers.meta.com/horizon/blog/v66-multi-room-support-spatial-anchors-api-improvements-quest-developers/
  — multi-room, 200 m², VPS.
- https://developers.meta.com/horizon/documentation/unity/unity-spatial-anchors-best-practices/ — the
  three-metre rule; "prompt the user to reposition the content".
- https://developers.meta.com/horizon/design/mr-design-guideline/ , `.../comfort/` , `.../display/` ,
  `.../head/` — no head-locked HUDs; loose follow with smoothing; ~70 cm windows; ≥0.5 m fixation.
- https://developers.meta.com/horizon/documentation/unity/unity-ovroverlay/ — head-locked overlays bypass
  TimeWarp.
- https://www.meta.com/help/quest/637588533755549/ — why the headset forgets a boundary; "look around the
  room when drawing your boundary".
- https://www.meta.com/help/quest/625635239532590/ — what is stored; "clear physical space history".
- https://developers.meta.com/horizon/documentation/native/android/mobile-guardian/ — "the system
  automatically identifies the user's room and associated Guardian bounds".
- Journalist-only, hedged, on reboot persistence of pinned windows:
  https://www.uploadvr.com/quest-v81-new-immersive-home-window-anchoring-quickplay/ .

### 13.8 SteamVR / OpenVR, HoloLens, and the failure literature

- **Universe identity**: `lighthousedb.json` `known_universes[] = { id, base_stations[{serial, relative
  pose}], tilt }`; `chaperone_info.vrchap` keys play-area and bounds records off that id. Assignment and
  auto-detection answered by Valve's OpenVR lead in
  https://github.com/ValveSoftware/openvr/issues/149 — a new id per new base-station set, returning to
  the old id on returning to the old room; *"These functions will only return valid data once we've
  determined which known universe you are currently in."* IDs are opaque creation timestamps in practice;
  the driver-side `.vrchap` documentation disagrees with real dumps in four places, so cite dumps.
  Inside-out runtimes pin one universe (Oculus = 1, ALVR/OpenHMD = 2) that follows the user between
  rooms. Persistence rewritten from the frame path is a known bug:
  https://github.com/ValveSoftware/openvr/issues/994 . `IVRSpatialAnchors` defines
  `NotYetAvailable` / `NotAvailableInThisUniverse` / `PermanentlyUnavailable`, several of which Valve's
  own wiki says are never emitted. Undocumented current-SteamVR binary strings (snapshot 2026-08-10):
  `persistentMapFromMap`, eased-unless-too-large relocalisation corrections,
  `/chaperone/last_relocalization_time`, *"Rejecting new committed universe since IMU fallback is
  active"*.
- **HoloLens**: "Finding your space" → "Still looking for your space" → **Limited mode** (cannot place,
  cannot see previously placed holograms), with per-space *Remove nearby / Remove all holograms*. Map
  failure modes named **holes, hallucinations and wormholes** — a wormhole being the device *"thinking it
  is in a different part of the map than it actually is."* Wi-Fi fingerprinting is used as a **prior** over
  the visual localiser: *"If the Wi-Fi signals change significantly, the device may think it is in a
  different space altogether."* Unity's `WorldAnchor` guidance — handle `OnTrackingChanged` with
  `SetActiveRecursively(located)` and *"call the `OnTrackingChanged` handler with the initial `IsLocated`
  state after attaching an anchor"* — is the single-hydration-path pattern of invariant I12.
  **Azure Spatial Anchors was retired 2024-11-20**, so the cloud tier of this prior art is gone.
- **ARCore** deprecated its "no match" resolution error and moved the gate to capture time via
  `FeatureMapQuality` — refuse a poor registration while the user is standing there.
- **Relocalisation failure literature**: AEROS, arXiv:2110.02018 (*"a single false-positive
  loop-closure… or even for the optimisation to fail entirely"*); ROVER, arXiv:2508.13488 (*"can be
  fatal"*); Sattler et al., CVPR 2018, arXiv:1707.09092 (visual localisation *"far from solved"*).

### 13.9 Linux XR shells and VR-desktop applications

- **WayVR** (ex-`wlx-overlay-s`; `github.com/galister/wlx-overlay-s` now 301s to
  https://github.com/wayvr-org/wayvr): the `Positioning` enum at `wlx-common/src/windowing.rs:6-26`;
  parent selection at `wayvr/src/windowing/window.rs:318-328`; state at
  `~/.config/wayvr/conf.d/zz-saved-state.json5` and `~/.config/wayvr/playspace.json5`; libmonado
  recentering at `backend/openxr/playspace.rs:241-314`; `shift_world()` in
  `backend/playspace_common.rs`. Issues: #529 (save-on-event loses data on unclean exit), #629
  (auto-billboarding, the "sphere prison" and its posture ritual), #592, #602.
- **StardustXR**: https://github.com/StardustXR/core — `spatial.kdl` (`set_spatial_parent` vs
  `set_spatial_parent_in_place`, `export_spatial`/`import_spatial_ref`), `root.kdl` (`ClientState`,
  `spatial_anchors`, `generate_state_token`, `STARDUST_STARTUP_TOKEN`); state under
  `~/.local/state/stardust/`.
- **xrdesktop**: https://gitlab.freedesktop.org/xrdesktop/xrdesktop — `xrd_window_set_pin` (a visibility
  filter), `org.xrdesktop` GSettings (no pose keys), `shake-compensation-threshold = 2.0` /
  `shake-compensation-duration-ms = 180`.
- **SimulaVR**: https://github.com/SimulaVR/Simula — `sendToWorkspacePersistent` is sticky-across-
  workspaces, not spatial; persistence is an xpra server, not placement.
- **SteamVR overlay desktops**: OVR Toolkit
  https://store.steampowered.com/app/1068820/OVR_Toolkit/ (world-pin vs hand/head follow; optional
  re-centering on entering Edit Mode); XSOverlay https://store.steampowered.com/app/1173510/XSOverlay/
  (window-space re-centering, grid view, layout save/load); OpenVR Advanced Settings
  https://github.com/OpenVR-Advanced-Settings/OpenVR-AdvancedSettings (user-named chaperone profiles).
  The drift anecdote and the dev's diagnosis ("the space is moving more-so than the overlay itself"):
  https://steamcommunity.com/app/1068820/discussions/0/7222135514704628375
- **ALVR**: `alvr/session/src/settings.rs` (`RecenteringMode { Stage, LocalFloor, Local, Tilted }`,
  default `LocalFloor`), math at `server_core/src/tracking/mod.rs:71-108`; `client_openxr/src/lib.rs:447-449`
  consumes `XR_EXT_user_presence`. Issue https://github.com/alvr-org/ALVR/issues/1031 (yaw wrong after a
  power cycle).
- **WiVRn upstream behaviours**: the lobby's re-derive-on-focus seat (`client/scenes/lobby.cpp:1105`);
  the dual-anchor no-jump frame conversion (`client/scenes/stream_gui.cpp`); PR
  https://github.com/WiVRn/WiVRn/pull/121 (LOCAL as an intent channel); issue
  https://github.com/WiVRn/WiVRn/issues/112 (refusing to synthesise a floor — *"It's for vendors to fix
  their software"*, plus the render-a-floor-pattern proposal); issue
  https://github.com/WiVRn/WiVRn/issues/474 (`OXR_RECENTER_STAGE`); issue
  https://github.com/WiVRn/WiVRn/issues/998 (a compositor fast path silently aliasing head-locked quads
  to world-locked).
- **Recenter-event unreliability in the field**: https://github.com/godotengine/godot/issues/99157 —
  differing event counts and contents across runtimes, arrival before fresh tracking data, and the
  maintainers' proposed *"disregard the `poseValid` property"*.
- **Immersed**: https://immersed.helpscoutdocs.com/article/33-my-screens-and-seating-arrangement-are-disoriented-after-putting-my-headset-down
  — the entire documented remedy is "reset or re-center your headset view". Height-after-don dominates
  the user-report corpus.
- **Virtual Desktop**: https://github.com/guygodin/VirtualDesktop/releases — v1.32.5 "auto-arrange
  monitors on recenter (enabled by default)"; the "Center to Play Space (Stage Tracking)" toggle.
- **Bigscreen**: layout persistence arrived as a bug fix (2018); recenter moves the *environment*, not
  the screen; head-lock refused on comfort grounds.
- **Sightful Spacetop**: https://help.sightful.com/en/articles/10841035-spacetop-software-releases-download
  — v1.8.75.0 "**Putting the glasses back on no longer restarts calibration or reorients your
  workspace**"; Travel Mode tracking against the laptop screen; the compass indicator for off-view
  windows.
- **Xreal One**: https://us.shop.xreal.com/blogs/buying-guide/user-guide_xreal-one-series — Anchor mode
  ("fixes the screen in mid-air") vs Follow mode ("fixes the screen in front of your eyes") with a
  Stabilizer toggle; long-press X to recenter; default 4 m.
- **MRTK Solvers**: https://learn.microsoft.com/en-us/windows/mixed-reality/mrtk-unity/mrtk2/features/ux-building-blocks/solvers/solver
  and `Follow.cs` / `RadialView.cs` for the tuning defaults quoted in §7.4. Microsoft's tag-along prose:
  https://learn.microsoft.com/en-us/windows/mixed-reality/design/billboarding-and-tag-along .
  `SpatialLocatability`:
  https://learn.microsoft.com/en-us/uwp/api/windows.perception.spatial.spatiallocatability .
  HoloLens environment considerations (Wi-Fi fingerprint, wormholes):
  https://learn.microsoft.com/en-us/hololens/hololens-environment-considerations .
- **OpenVR**: `headers/openvr.h` — `ETrackingUniverseOrigin`, `ResetZeroPose`,
  `VROverlayTransformType`, `Prop_CurrentUniverseId_Uint64`, `VREvent_ChaperoneUniverseHasChanged`,
  `ChaperoneCalibrationState`, `IVRSpatialAnchors` + `EVRSpatialAnchorError`.
- **Oculus Best Practices** (2–2.5 m focal, middle third of the FOV, the neck "head model"):
  https://static.oculus.com/documentation/pdfs/intro-vr/latest/bp.pdf

### 13.10 Prior HypXRland research

`research/22-spatial-persistence-locations.md` (the direct predecessor — §1 measured gamble, §2
amendments, §3 relocalization ladder, §4.2 libmonado, §5 the location model, §6 the don-edge ladder);
`research/archive/13-adaptive-anchoring.md` (shipped — the geofence, the desk seat, the grab-release
policy); `research/archive/18-monitor-plugged-state.md` (the plug gate, and its reversal);
`research/archive/12-spatial-2d-layout.md` (the latched reference frame — the same frame problem in
miniature); `research/archive/03-monitor-grids.md` (**unbuilt** — the two-phase parent-transform solve
this report depends on, and `xrgrid recenter`, the direct ancestor of a frame re-seat);
`research/archive/10-view-bounding.md` (**unbuilt** — "clamp the target, not the output", and the
display-time-constraint vs stored-identity distinction); `research/XREAL-3DOF.md` §6 (the 3DoF
degradation contract); `research/25-staging-container-headset-loop.md` §3.3 (a headset that leaves for
another server is a session *loss*, so the 20 s unplug grace does not apply).
