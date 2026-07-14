# Research: IMU-based don/doff (wear detection) for the XREAL Air 2 Ultra

Research memo (2026-07-14). Evaluates whether the **XREAL Air 2 Ultra**'s IMU can
serve as a **don/doff (wear-detection)** signal to drive HypXRland's XR
monitor plugged-state on a device with **no hardware proximity sensor**. Today the
3DoF rig ([`07-xreal.md`](../07-xreal.md), WP-XR1) tears down only on **USB
unplug**; the ask is a **softer, faster** signal — glasses set down on the desk =
monitors unplug/idle even while still plugged in.

This memo includes **live, read-only IMU capture** from the glasses (they were
plugged in and enumerated during the session, driving DP-5). No display,
brightness, or display-mode writes were issued; the DP-5 output was verified
`connected` and undisturbed after every capture.

Evidence base:

- **Vendored Monado** `subprojects/monado` (the runtime the XR suite validates
  against) — driver `src/xrt/drivers/xreal_air/{xreal_air_hmd.c,.h,xreal_air_packet.c}`,
  builder `src/xrt/targets/common/target_builder_xreal_air.c`, the shared fusion
  `src/xrt/auxiliary/math/m_imu_3dof.{c,h}`, and the OpenXR user-presence plumbing
  `src/xrt/state_trackers/oxr/{oxr_session.c,oxr_event.c,oxr_system.c}`,
  `src/xrt/include/xrt/{xrt_device.h,xrt_session.h}`, plus the Rift/PSVR2 drivers
  as the presence-emitting template.
- **HypXRland** `src/openxr/{XRSession.cpp,XRMonitorConfig.cpp,XRIpc.cpp}` and
  `src/config/values/ConfigValues.cpp` — the existing `XR_EXT_user_presence`-gated
  plug gate (report-19/20).
- **Live capture** off `/dev/hidraw9` (Ultra sensor interface = USB interface 2),
  three purpose-built read-only probes (source retained in the session
  scratchpad): a stats characterizer, a raw dumper, and a per-second windowed
  stillness monitor. USB id `3318:0426`, sensor stream started with the driver's
  own `START_IMU_DATA` (msgid `0x19`) command byte `0x01`.

---

## TL;DR — RECOMMENDATION

**Power/perf verdict (load-bearing): IMU wear-detection is essentially FREE — but
ONLY if the classifier rides Monado's existing IMU read loop. It is expensive and
wrong to do any other way.**

- The XReal IMU streams at **1 kHz** (measured: 12002 samples in 12.00 s). While
  an XR session runs, Monado's `xreal_air` read thread is **already** reading and
  fusing every one of those samples (`xreal_air_hmd.c:634 read_thread` →
  `sensor_read_one_packet` → `update_fusion` → `m_imu_3dof_update`). A wear
  classifier that consumes that already-decoded stream costs **a few float ops per
  sample and one timer** — no new device traffic, no new USB interrupts, no new
  wakeful thread. The fusion **already computes the exact feature we need**
  (`m_imu_3dof.last.gyro_length`, and a per-sample `grav.is_rotating` boolean).
- A **separate hidraw reader** (external daemon or compositor-side opener) is the
  opposite: it re-opens interface 2, re-issues `START_IMU`, and pulls **1000 USB
  interrupt transfers per second** on its own wakeful thread — redundant radio/USB
  traffic and ~1 kHz of scheduler wakeups purely to recompute what Monado already
  has. **Reject this.**

**Where it should live: inside the Monado `xreal_air` driver, emitted as
`XR_EXT_user_presence`.** If the driver synthesizes presence from IMU stillness,
HypXRland's **existing** presence-gated plug gate
(`XRMonitorConfig.cpp:wantXRMonitorsPlugged`, report-19/20) drives the unplug
**with zero compositor changes**. This is the elegant answer.

**One caveat that sizes the work (see §5):** in the vendored Monado, a driver's
`get_presence` is polled **only once, at session begin** (`oxr_session.c:352`);
there is **no periodic poll**, so a driver setting `supported.presence` gets you a
correct *initial* presence but **no live don/doff events**. Live wear-detection
therefore needs a second, generic piece: a per-frame presence poll +
`XRT_SESSION_EVENT_USER_PRESENCE_CHANGE` broadcast (WiVRn delivers live presence
because it is a *different* runtime that already does this; Monado does not).

**Honest default:** the IMU-auto path is a **clear, cheap win for the "auto-idle
when I set them down" half**, but the false-positive risk (sitting very still while
reading must not read as doffed) means it must be **corroborated and dwell-gated**,
never instantaneous. My recommendation is a **staged** rollout:

1. **Ship a manual toggle first** (keybind + voice "doff"): trivial, reliable,
   zero-guess, and it's the honest floor every auto-heuristic is measured against.
2. **Then add IMU-auto as a driver-side presence emitter** (M-sized, mostly
   Monado) with a conservative stillness threshold + a **long dwell equal to the
   existing `monitor_unplug_grace_ms` (20 s)**. It only ever *adds* an unplug
   trigger the current rig lacks; it cannot make things worse than USB-unplug
   because donning re-plugs immediately and the dwell absorbs reading-stillness.

Do **not** build a standalone don/doff daemon.

---

## 1. Live signal characterization (read-only capture)

The glasses sat **on the desk** for the whole session (the user was on another
display), so this capture *is* the "doffed, set down flat, still" state — exactly
the state we want to detect.

**Stream / units** (from `xreal_air_packet.c:read_sample` and
`xreal_air_hmd.c:236 read_sample_and_apply_calibration`):

- **Rate: 1000 Hz.** 12002 samples / 12.00 s wall; device timestamps advance ~1 ms
  per sample. This is a 1 kHz IMU (ICM-42688-P class; temperature LSB cited at
  `xreal_air_hmd.c:573`).
- Each 512-byte report (`buffer[0]==1`) carries per-sample `gyro_multiplier/divisor`
  and `accel_multiplier/divisor` (measured `accel 32/16777216`, `gyro
  4000/16777216`). Accel raw→g→m/s² via `× MATH_GRAVITY_M_S2`
  (`xreal_air_hmd.c:276`); gyro raw→deg/s→rad/s via `× π/180`
  (`xreal_air_hmd.c:279`).

**Accelerometer at rest** (raw factor, no calibration bias applied):

| Metric | Value |
|---|---|
| `\|accel\|` mean | **9.94 m/s²** (≈ g — device is stationary) |
| `\|accel\|` std | 0.043 m/s² (~0.4%) |
| `\|accel\|` min / max | 9.65 / 10.24 m/s² |
| mean gravity vector | **(0.09, −6.83, 7.21) m/s²**, constant to 2 dp across 20 s |
| resting tilt | pitch ≈ 0°, roll ≈ **−43°** |

The gravity vector is **rock-steady** and sits at a **characteristic tilt** (roll
−43°): glasses resting on brow/nose-pads lie at a fixed, non-head-like pitch/roll.
This is a usable *corroborating* orientation signal (a set-down pose differs from
the near-level, slowly-drifting pose of a worn head), though it is **surface- and
placement-dependent** and must not be the primary discriminator.

**Gyroscope at rest** (the primary discriminator):

| Metric | Value |
|---|---|
| per-axis noise std | (0.32, 0.18, 0.15) deg/s |
| mean bias (uncalibrated) | (0.024, 0.032, 0.0001) rad/s → \|bias\| ≈ **0.04 rad/s = 2.3 deg/s** |
| sample-to-sample \|gyro\| jitter | 0.096 deg/s |
| **windowed AC gyro-RMS (bias-removed)** | **~0.15 deg/s quiet floor**, excursions to ~1.1 deg/s |

The per-second windowed monitor showed the AC (DC-bias-removed) gyro-RMS settle to
**~0.15 deg/s** when the room was quiet, rising to **~1.1 deg/s** when the desk
picked up vibration (typing / HVAC). The DC bias (~2.3 deg/s of apparent rate) is a
**fixed offset**, not motion — any stillness detector must key on the **AC
component** (variance / bias-removed magnitude), not raw rate, or it will read a
still-but-biased gyro as "moving."

**What the three wear states look like (measured floor + physics):**

- **Doffed, flat on desk, still** *(measured)*: gravity vector fixed to 2 dp;
  AC gyro-RMS **0.15–1.1 deg/s**; `\|accel\|` pinned at g ± 0.4%. Dead.
- **Worn, even sitting still (reading)** *(physics)*: a live human head never holds
  0.15 deg/s. Postural sway, breathing, pulse, and saccade-driven micro-corrections
  put continuous head angular rate in the **~0.5–3 deg/s RMS** band with frequent
  brief excursions to tens of deg/s; the gravity vector **slowly wanders** as the
  head re-levels. This is the **hard case** — the margin over a vibrating desk is
  only ~1 order of magnitude, which is why dwell + hysteresis is mandatory.
- **Doffed, in motion (being carried / on a lanyard)** *(physics)*: large,
  irregular gyro/accel (`\|accel\|` swings well off g). Reads as "moving," i.e.
  *not* still — correctly **does not** trigger the set-down idle. That's fine: the
  set-down trigger fires on the *transition to sustained stillness*, not on motion.

The clean, robust separator is therefore **sustained stillness of the AC gyro
signal**, corroborated by a **stable gravity vector**. Absolute orientation
(roll −43°) is a weak tie-breaker, not a gate.

---

## 2. Detection algorithm (cheapest robust classifier)

**Feature:** AC gyro energy over a short sliding window, i.e. bias-removed
`gyro_length` (or its variance). Monado **already computes this per sample** — see
§3. Accel gives a corroborating "gravity vector stable AND ≈ g" test.

**Classifier (worn ↔ doffed):** a two-threshold (hysteresis) still-detector with a
dwell timer:

```
per sample (rides the existing fusion update):
  moving = fusion.last.gyro_length >= GYRO_MOVE   // ~0.10 rad/s (5.7 deg/s)
  if moving: last_motion_ts = now; state = WORN (re-don is instant)
  else if (now - last_motion_ts) >= DOFF_DWELL:   // ~20 s, == monitor_unplug_grace_ms
           and gravity_vector_stable_for(DOFF_DWELL)
           and |accel_mean - g| < GRAV_TOL:
      state = DOFFED
```

- **`GYRO_MOVE`** can reuse Monado's own `gyro_tolerance = 0.1 rad/s`
  (`m_imu_3dof.c:121`) — the very threshold the fusion uses for `grav.is_rotating`.
  My measured doffed-still `gyro_length` ≈ 0.04 rad/s sits **cleanly below** it,
  and any real head motion sits above it. Hysteresis: don (→WORN) fires on the
  first `moving` sample (instant re-plug); doff (→DOFFED) requires the full dwell of
  no-motion. Asymmetric by design — donning must be immediate, doffing patient.
- **`DOFF_DWELL` = 20 s** deliberately equals the existing
  `openxr:monitor_unplug_grace_ms` default (`ConfigValues.cpp:902`). This is the
  single most important knob: it is what stops "reading very still for 8 s" from
  evacuating your workspaces. Reading produces *occasional* micro-motion that keeps
  re-arming `last_motion_ts`; a true set-down produces *zero* for 20 s straight.
- **Gravity corroboration** (optional, cheap): require `|accel_mean| ≈ g` and a
  gravity direction that hasn't wandered over the dwell. A worn-but-still head still
  produces tiny gravity-vector wander; a set-down device does not. This mainly
  guards against a pathological "held perfectly rigid in the hand" case.

**False-positive risk & mitigation:** the only dangerous FP is *worn + very still →
misread as doffed*. Mitigations, in order of leverage: (1) the 20 s dwell; (2) key
on **AC** energy not raw rate (bias-immune); (3) gravity-wander corroboration.
False-*negative* (doffed but reads worn, e.g. set on a shaking desk near a subwoofer)
is harmless — it just means you fall back to the existing USB-unplug teardown.

**Piggyback vs. compute-our-own:** *piggyback.* `m_imu_3dof` already maintains, per
sample and for free:

- `f->last.gyro_length` — gyro magnitude (`m_imu_3dof.c`, u_var-exposed at `:76`).
- `f->grav.is_rotating = gyro_length >= 0.1 rad/s` (`m_imu_3dof.c:133`).
- `f->grav.is_accel = |accel_length − 9.82| >= 0.9` (`m_imu_3dof.c:132`).
- `f->rot` — the fused orientation quaternion (gravity direction for free).

A wear classifier is then literally: watch `grav.is_rotating`; when it has been
`false` continuously for the dwell (and `is_accel` false), declare doffed. **No new
math, no new device access.**

---

## 3. Power / performance — the load-bearing analysis

Three candidate homes, cheapest to worst:

**(A) Inside the `xreal_air` Monado driver (RECOMMENDED).** The read thread already
runs at 1 kHz doing `os_hid_read` + parse + `m_imu_3dof_update` per sample
(`xreal_air_hmd.c:611-655`). Adding a stillness accumulator is **~5 float compares
and a timestamp subtract per sample** on a thread that already woke for that
sample. **Marginal cost ≈ 0.** No new USB traffic (rides the existing interrupt
IN transfers), no new thread, no new wakeups. The device is *already* streaming
these samples whenever an XR session is up; we are consuming exhaust.

**(B) Compositor consumes pose deltas.** No Monado patch, but HypXRland would have
to derive stillness from the XR head-pose stream it already receives — cheaper than
a new device reader, but the pose stream is delivered at the *display* rate (~60–90
Hz), post-fusion, and the compositor would need new plumbing + its own thresholds.
And it still needs a signal path for the result. Middling. The only reason to pick
this is to avoid touching Monado — but see (A)+§5, which touches Monado in exactly
the place that *also* fixes it for every other headset.

**(C) Standalone external daemon reading hidraw directly (REJECT).** This re-opens
interface 2, re-issues `START_IMU`, and pulls the **full 1 kHz stream itself**:
~1000 USB interrupt transfers/s + a wakeful poll/read thread spinning at 1 kHz,
**purely to recompute a number Monado already has**. On a battery-conscious laptop
(the Framework 16 host) that is real, continuous, pointless power draw and radio/USB
keep-busy. It also races Monado for the device (only one opener gets the IMU
cleanly). Every property that makes (A) free makes (C) expensive. **Do not build
this.**

**Decimation note:** even (A) needn't inspect all 1000 samples/s — a stillness
detector is happy at 50–100 Hz. But since the thread already touches every sample,
there is no saving in skipping; the per-sample cost is already noise.

**When is the stream even on?** Only while something reads interface 2 — i.e. while
a Monado XR session is running. When the glasses are "just a monitor" (flat DP-5
mode, no session), *nobody* reads the IMU and it costs nothing. That is exactly
right: don/doff only matters **during** an XR session, which is precisely when the
read loop exists. (Confirmed live: with no session, opening the interface myself and
sending `START_IMU 0x01` was required to get any samples; `0xAA` alone returns only
a control ack.)

---

## 4. The `XR_EXT_user_presence` path (why the driver is the right home)

HypXRland **already** consumes user presence to gate the XR monitor plug state:

- `XRSession.cpp:106-111` enables `XR_EXT_user_presence` when advertised;
  `:163-182` reads `XrSystemUserPresencePropertiesEXT.supportsUserPresence`;
  `:390-397` handles `XR_TYPE_EVENT_DATA_USER_PRESENCE_CHANGED_EXT`
  (donned/doffed).
- `XRMonitorConfig.cpp:345 wantXRMonitorsPlugged(...)` folds `userPresent` into the
  plug decision, and `XRIpc.cpp:32-35` surfaces it (report-19/20).

So if the `xreal_air` driver advertised presence and emitted don/doff from IMU
stillness, **the compositor needs zero new code** — the existing gate fires. The
Monado template is small and already in-tree (Rift CV1,
`drivers/rift/rift_driver.c`):

- advertise: `hmd->base.supported.presence = true;` (`rift_driver.c:664`) →
  propagates to `supportsUserPresence` via `oxr_system.c:697`.
- implement: `hmd->base.get_presence = ..._get_presence;` (`rift_driver.c:636`,
  returns a cached bool).
- populate: in the read loop set the bool — Rift uses a hardware proximity sensor
  (`hmd->presence = report.cv1.presence_sensor > 3;`, `rift_driver.c:123`); **for
  XReal we substitute the IMU stillness classifier from §2** (the XReal has no
  proximity sensor — this is the whole point).

PSVR2 (`drivers/psvr2/psvr2.c:206,1257`) and `blubur_s1` are the same shape. This
is a **~40-line, self-contained driver change** that mirrors an existing pattern.

---

## 5. The runtime-propagation gap (this sizes the WP)

Setting `supported.presence` + `get_presence` is **necessary but not sufficient for
live don/doff** in the vendored Monado:

- `xrt_device_get_presence` is called from **exactly one place**: `oxr_session.c:352`,
  **at session begin only**. There is **no periodic poll** anywhere in the tree
  (grepped: only oxr session-begin + the IPC passthrough call it).
- Runtime presence *changes* are delivered via a
  `XRT_SESSION_EVENT_USER_PRESENCE_CHANGE` session event
  (`xrt_session.h:71`, consumed at `oxr_session.c:630`) — but **nothing in Monado
  pushes one**. Session events are broadcast by the multi-compositor
  (`comp_multi_system.c:608-656` pushes STATE_CHANGE / OVERLAY / LOSS_PENDING) and
  **presence is not among them**. The Rift driver, despite implementing
  `get_presence`, never emits a change event — so even Rift presence is a
  session-begin snapshot in this snapshot.
- WiVRn delivers **live** don/doff to HypXRland today only because WiVRn is a
  **separate OpenXR runtime** that implements its own presence-change emission. The
  XReal rig runs **Monado**, which does not.

So the WP has two Monado-side parts: **(A)** the driver emitter (§4, small), and
**(B)** a generic runtime propagation path — either a per-frame poll of the head
device's `get_presence` in `comp_multi_system` that broadcasts
`XRT_SESSION_EVENT_USER_PRESENCE_CHANGE` on change (generic; also fixes Rift/PSVR2
live presence), or a driver→session-sink push. Part (B) is the "real" work and the
reason this is M-sized, not S.

*(This gap is also worth a note back to WP-XR1: the 3DoF rig's
`monitors_follow_session=session` teardown is not merely a preference — with
Monado's XReal path, `visible` mode has no donned signal to gate on because there is
no proximity sensor and no presence emitter, so only session-death (USB unplug)
distinguishes states today. This WP is what would make `visible` mode meaningful for
XReal.)*

---

## 6. Idle-state interaction — does "doffed" also blank the display?

When doffed-on-desk is detected, HypXRland unplugs the XR monitors (workspaces
evacuate). But **DP-5 is still active** — the glasses' panels stay lit, drawing
power and shining at the ceiling. Two levels:

- **Monitors unplug, display stays on** (what the presence gate does today). Clean,
  reversible, but the birdbath panels keep burning power while set down.
- **Display also sleeps.** The driver *can* speak display commands over the control
  interface — brightness (`XREAL_AIR_MSG_W_BRIGHTNESS 0x04`) and display mode
  (`XREAL_AIR_MSG_W_DISP_MODE 0x08`), `xreal_air_hmd.h:23-26`. There is a
  `display_on` bool and a `DISPLAY_TOGGLED` async event
  (`XREAL_AIR_MSG_P_DISPLAY_TOGGLED 0x6C04`), i.e. the physical button can toggle
  the panels. **I did not find a documented "panel sleep/off" HID write** in the
  driver beyond brightness (min brightness ≠ off) and mode; the toggle is exposed as
  an *input* event, not a command we send. Driving panel-off would need either the
  brightness-to-minimum hack or reverse-engineering a display-off control frame
  (out of scope, and a *write* the safety rules forbid probing here).

**Recommendation:** phase 1 = **monitors unplug, display stays on** (matches the
existing presence semantics, fully reversible, zero new HID writes). Treat
**panel-sleep as a separate, later, opt-in** — the power saving is real but it
needs a verified display-off path and careful re-wake on don. Don't couple them;
an over-eager panel-off on a false-positive is far more jarring (black glasses) than
a workspace evacuation.

---

## 7. Comparison to alternatives

| Approach | Cost | Reliability | FP risk | Verdict |
|---|---|---|---|---|
| **USB unplug** (current) | zero | perfect (physical) | none | Keep as the backstop. Crude: requires physically pulling the cable. |
| **Manual keybind / voice "doff"** | trivial | perfect (explicit) | none (user intent) | **Ship first.** Zero-guess, no heuristic, works today with the existing plug gate if wired to a synthetic presence/visibility toggle. |
| **IMU auto (this)** | ~free *if* driver-side (§3A) | good, dwell-gated | low w/ 20 s dwell + AC-energy + gravity corroboration | **Ship second.** The only *automatic* "set them down → idle" trigger. Worth it, but must be conservative. |
| Standalone hidraw daemon | **high** (1 kHz USB + wakeups) | good | low | **Reject** — same signal, needless power. |

**Is IMU-auto worth the complexity over manual + USB-unplug?** For the *auto-idle*
UX (you set the glasses on the desk and your workspaces come back to your laptop
screen without touching anything) it is a **genuine, unique win** — neither
alternative does it hands-free. And priced correctly (driver-side, §3A) it is
nearly free at runtime. The complexity is **not** in the classifier (tiny) — it is
in Monado's missing runtime presence-propagation (§5). So the honest framing:
**the heuristic is cheap and good; the plumbing is the cost.** Given the plumbing
is generic (helps Rift/PSVR2 too) and the classifier reuses fields the fusion
already computes, it clears the bar — **as phase 2, behind a phase-1 manual toggle.**

---

## 8. WP sketch — "XReal IMU don/doff → user-presence" (size **M**)

Almost entirely a **Monado-driver** change; **no HypXRland compositor code** if the
presence path is completed (the plug gate already consumes presence).

1. **Driver stillness classifier** *(S, `xreal_air_hmd.c`)*: in the read/fusion
   path, accumulate `time-since-last-motion` from `hmd->fusion.grav.is_rotating`
   (or `last.gyro_length` vs `0.1 rad/s`), plus a gravity-stable check from
   `hmd->fusion.rot` / `grav.is_accel`. Maintain `hmd->presence`
   (worn = recent motion; doffed = still ≥ dwell). Config the dwell + threshold via
   `XREAL_AIR_*` env/debug knobs to start.
2. **Advertise presence** *(XS)*: `base.supported.presence = true`,
   `base.get_presence = xreal_air_get_presence` (return `hmd->presence`) — mirror
   `rift_driver.c:636,664`.
3. **Runtime propagation** *(M, the real work)*: add a per-frame poll of the head
   device's `get_presence` in `comp_multi_system` (or a device→session-sink push)
   that broadcasts `XRT_SESSION_EVENT_USER_PRESENCE_CHANGE` on change, so
   `oxr_session.c:630` → `oxr_event.c:348` emits
   `XR_TYPE_EVENT_DATA_USER_PRESENCE_CHANGED_EXT`. Generic; fixes live presence for
   all presence-capable drivers.
4. **Compositor: nothing** — `XRSession.cpp:390` already handles the event and
   `XRMonitorConfig.cpp:345` already gates on it. Optionally flip the XReal rig's
   documented default from `monitors_follow_session=session` to `visible` once
   presence is live, so set-down idles without USB unplug.
5. **Manual toggle (ship first, independent)** *(S)*: a keybind/voice "doff" that
   drives the same plug gate (synthetic presence-absent), giving the reliable
   zero-guess path immediately and a ground truth to validate the auto-heuristic
   against.
6. **Later / opt-in** *(separate WP)*: panel-sleep on doff (needs a verified
   display-off HID path; do not couple to the unplug).

---

## 9. Open questions

- **Runtime propagation shape (§5):** per-frame `get_presence` poll in
  `comp_multi_system` vs. a device-pushed session event — which does upstream
  Monado prefer? (The poll is simpler and generic; confirm it's acceptable and
  doesn't add per-frame cost when no presence-capable device is bound.)
- **Worn-still ground truth:** I could not capture a *worn* trace (glasses were on
  the desk, and wear-state can't be commanded without disturbing the user's
  session). The 0.5–3 deg/s worn-still figure is from physics + literature, not this
  device. A one-off worn capture during a real session would let us tune
  `GYRO_MOVE` / dwell empirically rather than borrowing the fusion's 0.1 rad/s.
- **Gravity-corroboration necessity:** is the gyro-dwell alone sufficient, or is the
  gravity-stable check worth its (tiny) extra state? Decide with the worn trace.
- **Panel-sleep path:** does a non-brightness "display off" control frame exist for
  the Ultra, and does it re-wake cleanly on don? (Requires a *write* probe — out of
  scope here.)
- **Desk-vibration floor:** my quiet floor was ~0.15 deg/s but desk coupling pushed
  it to ~1.1 deg/s. On a resonant desk near a subwoofer the doffed floor could climb
  toward the worn band — acceptable (fails safe to "worn"/no-idle), but worth noting
  the environment-dependence of the *absolute* threshold (another reason to key on
  the transition-to-stillness with a long dwell rather than an absolute cutoff).
