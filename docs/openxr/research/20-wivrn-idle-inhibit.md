# Research: idle/sleep-inhibition policy for WiVRn sessions

**Status:** research / decision-support. **Nothing implemented.** This memo maps the design
space for *when a headset session should stop the desktop from dimming, locking and
suspending*, and recommends a policy. It follows the house style of
`docs/openxr/research/`: honest pro/con per approach, a behaviour matrix, upstreamability and
patch-size estimates, failure modes, then a recommendation split into "cheap correct fix now"
and "optional follow-up".

Evidence base (all read-only):

- **WiVRn** at `~/code/wivrn`, branch `hypxr-patches-26.6.2`: `server/sleep_inhibitor.{h,cpp}`,
  `server/main.cpp` (the FSM), `server/wivrn_ipc.h` (`from_monado`/`to_monado` packets),
  `server/driver/wivrn_session.cpp` (presence/session-state handlers, pause/resume),
  `server/driver/configuration.h`, `common/wivrn_packets.h`, `client/scenes/stream.cpp`.
- **HypXRland**: `src/openxr/OpenXRManager.{cpp,hpp}` (`shouldInhibitIdle`, presence state),
  `src/managers/input/IdleInhibitor.cpp`, `src/protocols/IdleNotify.cpp`,
  `src/render/Renderer.cpp` + `src/protocols/SessionLock.cpp` (lock rendering),
  `src/config/legacy/DispatcherTranslator.cpp` (`dpms` dispatcher),
  `docs/openxr/05-configuration.md` §7.
- **hypridle 0.1.7** (installed): binary strings + upstream `src/core/Hypridle.cpp`
  (`BlockInhibited` handling, `ignore_*` knobs, inhibit-lock gating).
- **User config**: `~/.config/hypr/hypridle.conf`, `omarchy-system-lock`,
  `omarchy-launch-screensaver`.
- **Upstream WiVRn**: issues [#417 "Suppress Autolock"](https://github.com/WiVRn/WiVRn/issues/417)
  (closed) and [#847 "Use Inhibit portal rather than login1"](https://github.com/WiVRn/WiVRn/issues/847)
  (open); commits `868fc55f` "Inhibit sleeping while a session is active", `b6516a3a`
  "Silence sleep inhibitor".

---

## TL;DR — RECOMMENDATION

> **Split WiVRn's single logind inhibitor into two, and gate only the `idle` half on
> wear (`XR_EXT_user_presence`), behind a `[server] inhibit` config knob (default
> `worn`).** `sleep` stays blocked for the whole session (protects against lid-close
> suspend and mid-game suspend); `idle` is held only while the headset is actually on your
> head. Everything else — dim, screensaver, lock, and the 10/15-minute grace before them —
> then falls out of the *existing* hypridle config with **zero** extra grace machinery,
> because **both** inhibit paths restart their timers from zero on release (verified in
> `IdleNotify.cpp:38-51` and hypridle's `onInhibit`). Do **not** try to make hypridle
> lock the laptop panel while XR keeps running: `ext-session-lock` in Hyprland is
> **all-outputs-or-nothing**, so locking the desktop necessarily locks the headset view too.
> The per-output privacy tool that *does* exist today is `hyprctl dispatch dpms off <name>`.

Ranked: **A′ (recommended) > C > A > B > D > E**. See §6.

---

## 1. What actually happens today (the causal chain)

Four independent mechanisms are in play. They are frequently confused; the whole design
hinges on keeping them apart.

| # | Mechanism | Owner | Scope | Who obeys it |
|---|-----------|-------|-------|--------------|
| 1 | logind `Inhibit("sleep:idle", …, "block")` | **wivrn-server** | system | logind's own `IdleAction`; **and hypridle**, via the `BlockInhibited` property |
| 2 | `ext-idle-notify-v1` inhibit bit | **Hyprland** (`PROTO::idle->setInhibit`) | compositor | any idle client using `get_idle_notification` (hypridle's default) |
| 3 | idle-timer *activity* resets | Hyprland input path | compositor | same |
| 4 | logind delay lock for `before_sleep_cmd` | **hypridle** (`inhibit_sleep = 3`) | system | logind |

**The observed symptom** — "hypridle never fires while the Quest is merely connected" — is
mechanism 1, and specifically its `idle` component:

- `sleep_inhibitor::sleep_inhibitor()` (`server/sleep_inhibitor.cpp:25`) calls
  `org.freedesktop.login1.Manager.Inhibit` with `What = "sleep:idle"`, `Mode = "block"`,
  `Why = "A WiVRn session is active"`, and holds the returned fd until destruction.
- In `server/main.cpp` the inhibitor is a plain `std::optional<sleep_inhibitor>` (line 222)
  that is `emplace()`d on `from_headset::headset_info_packet` **and** on
  `from_monado::headset_connected` (lines 513/536), and `reset()` **only** on
  `from_monado::headset_disconnected` (line 541). There is no other release path. Its
  lifetime is exactly **TCP connect → TCP disconnect**.
- hypridle sees this. It watches logind's `BlockInhibited` property and does a substring test
  for `":idle:"`; on a match it calls `onInhibit(true)`, which bumps `m_iInhibitLocks`. Every
  listener without `ignore_inhibit` then early-returns in `onIdled()`
  (`"Ignoring from onIdled(), inhibit locks: {}"`). **Both** of the user's listeners
  (screensaver at 600 s, lock at +302 s) are plain listeners, so **nothing fires at all**
  while the headset is connected — donned or doffed.

HypXRland's own inhibit is mechanism **2**, and it is *not* the cause:

- `COpenXRManager::shouldInhibitIdle()` (`OpenXRManager.cpp:197`) is
  `openxr:inhibit_idle && m_state == XR_STATE_RUNNING_FOCUSED` — deliberately **FOCUSED only**
  (`docs/openxr/05-configuration.md` §7: "`visible` alone, e.g. a runtime dashboard in front,
  deliberately does not inhibit").
- It is consumed by `CInputManager::recheckIdleInhibitorStatus()`
  (`IdleInhibitor.cpp:65`), which folds it in with the Wayland idle-inhibit protocol and
  window rules and writes `PROTO::idle->setInhibit()`. Rechecked on every session-state
  transition (`OpenXRManager.cpp:221-223`).
- `hyprctl openxr status` exposes it as `idle inhibited:` / `inhibitingIdle`
  (`XRIpc.cpp:45-52`) — which is why it correctly reads `no` while doffed, even though
  nothing actually times out.

So today the two owners are **redundant in the worn case and disagree in the doffed case**,
and WiVRn's coarser, higher-privilege one wins.

Mechanism **3** matters for a subtle reason: XR ray input injects through the normal pointer
path (`XRPointerDevice.hpp`: "attached through `CPointerManager::attachPointer` like a real
mouse — it resets idle timers"), and input is only synced while FOCUSED. So *using* the
headset already keeps the desktop awake by itself; *reading* in the headset does not.

## 2. What signals exist, and where

The critical structural fact: **wivrn-server is two processes**, and the wear signal is in the
wrong one.

```
Quest client ──TCP control──▶ monado child (wivrn_session)  ──unix dgram──▶ main FSM (main.cpp)
   XR_EXT_user_presence          from_headset::*                from_monado::*      ▲
   session_state_changed         (rich: presence, state,        (poor: connected,   │
   user_presence_changed          battery, tabs …)               disconnected,      └─ owns the
                                                                 error, battery…)      inhibitor
```

- The client advertises and consumes `XR_EXT_user_presence`
  (`client/application.cpp:1249`, `client/scenes/stream.cpp:251`) and forwards
  `XR_TYPE_EVENT_DATA_USER_PRESENCE_CHANGED_EXT` as
  `from_headset::user_presence_changed{present}` (`stream.cpp:1326-1328`).
- The monado child handles it in `wivrn_session::operator()(from_headset::user_presence_changed&&)`
  (`wivrn_session.cpp:769`): `hmd.update_presence()` then pushes
  `XRT_SESSION_EVENT_USER_PRESENCE_CHANGE` to OpenXR clients — **this is exactly the event
  HypXRland's plug gate already consumes** (report-18/19; `m_userPresent`, `m_presenceKnown`,
  `m_userPresenceSupported` in `OpenXRManager.hpp:607-614`). It is known-good in this setup.
- `headset_info_packet.user_presence` is a *capability* bit (does this headset report
  presence at all): `wivrn_hmd.cpp:84 .presence = info.user_presence`, and
  `wivrn_session.cpp:371/421` guard synthetic presence events on pause/resume with it. A
  presence-gated policy therefore has a natural, safe fallback: **capability absent ⇒ behave
  exactly as today**.
- `from_monado::packets` (`wivrn_ipc.h:50-58`) is
  `{headset_info_packet, settings_changed, start_app, stream_tab_changed, battery,
  headset_connected, headset_disconnected, server_error}`. **No presence, no session state.**
  That single missing message is the entire reason the inhibitor is connect/disconnect-scoped.
- `wivrn_session::pause_session()` / `resume_session()` are the **network reconnect** path
  (`run_net`, `wivrn_session.cpp:1009/1017`), *not* don/doff. Don't confuse them.
- Config precedent exists: `struct configuration` (`server/driver/configuration.h`) is a flat
  JSON-backed struct (`bit_depth`, `tcp_only`, `hid_forwarding`, `publication`,
  `openvr_compat_path`, …). Adding one enum-valued key is idiomatic and cheap.

## 3. The desktop side — what would happen if the block were lifted

The user's `~/.config/hypr/hypridle.conf` has **no suspend listener at all** and no DPMS
listener; only:

| Listener | Timeout | Action |
|---|---|---|
| screensaver | 600 s | `pidof hyprlock \|\| omarchy-launch-screensaver` |
| lock | +302 s (≈15 min total) | `omarchy-system-lock` (hyprlock, 1Password lock, then display + keyboard brightness off after 3 s) |

plus `before_sleep_cmd` / `inhibit_sleep = 3` for *externally triggered* suspend. Consequences:

1. **The `idle` half is the only half that changes user-visible behaviour on this machine.**
   Nothing here ever asks logind to suspend. The `sleep` half only matters for
   externally-initiated sleep — most importantly **lid close** (a `block`-mode `sleep`
   inhibitor makes logind refuse the automatic suspend). On a laptop used as a desktop
   replacement with the panel disabled and the lid shut, that is load-bearing. **Keep it.**
2. **Locking the desktop locks the headset.** `ext-session-lock` in Hyprland is global:
   `renderAllClientsForWorkspace` bails out for *every* monitor once the lock client is locked
   (`Renderer.cpp:1104-1112`), and `renderLockscreen` draws the per-monitor lock surface or the
   "lock missing" primer on *every* monitor (`Renderer.cpp:1637-1666`). XR monitors are
   ordinary headless `CMonitor`s, so they get lock surfaces like any other output. **There is
   no per-output lock today**, so "lock the laptop panel while I keep working in the headset"
   is not expressible. (It *is* survivable in the other direction: locked while worn, you can
   read hyprlock on an XR quad and type the password on the physical keyboard.)
3. **The screensaver is worse than the lock.** `omarchy-launch-screensaver` loops over
   `hyprctl monitors -j` and spawns a fullscreen terminal on **each** monitor, focus-hopping as
   it goes — i.e. an XR session that goes idle-unblocked gets a tte screensaver window on every
   XR quad and its focus moved. Anything that lets `idle` through *while worn* is not a
   cosmetic bug; it wrecks the workspace.
4. **Grace is free.** Releasing an inhibit does **not** fire a pending idle event:
   - Wayland path: `CIdleNotifyProtocol::setInhibit(false)` → `update(0)` → `reset()` +
     `updateTimeout(m_timeoutMs)` (`IdleNotify.cpp:38-51`) — the **full** timeout restarts.
   - logind path: hypridle destroys and re-creates its notifications when the last lock
     drops, so its timeouts also restart from scratch.
   So a doff yields a fresh 10-minute screensaver / 15-minute lock countdown, and a re-don
   inside that window cancels it. **No doff-grace timer needs to be written anywhere.** The
   "instant lock while glancing at my phone" failure mode does not exist.
5. **Per-output privacy does exist** — just not authentication. `dpms` takes an optional
   monitor argument (`DispatcherTranslator.cpp:638-655`, `dispatch dpms off eDP-1`), and
   `monitor=eDP-1,disable` is already part of the user's workflow. Doc 05 §7 already suggests
   exactly this pattern driven off the `openxractive` socket2 event.

## 4. Upstream stance (WiVRn)

- **#417 "Suppress Autolock"** (closed): the maintainer (xytovl) treats the login1
  `sleep:idle` block as *the* implementation of "don't lock while a headset is connected", and
  posted the same `systemd-inhibit --list` line the user is seeing. So the current behaviour is
  intentional, and a patch that simply removes `idle` (option B) will read upstream as a
  regression — for a plain VR-game user with no compositor-side inhibitor, it *is* one.
- **#847 "Use Inhibit portal rather than login1"** (open): asks to move to
  `org.freedesktop.portal.Inhibit` for Flatpak/non-systemd/least-privilege reasons. xytovl's
  only comment is "errors should not be fatal" — no objection to the idea, no work done.
  Relevant here because the portal's `Inhibit` flags are a bitmask (`LOGOUT|USER_SWITCH|SUSPEND|IDLE`),
  so a *split* of what we inhibit is forward-compatible with that migration and a
  presence-gated design would need to survive it.
- No issue or PR discusses wear-gating or granularity. There is no precedent to lean on, but
  also nothing hostile: `868fc55f` was a one-shot "inhibit while a session is active" commit and
  `b6516a3a` only silenced its logging.
- **Upstreamability read:** a *config knob* whose default preserves today's behaviour is very
  likely acceptable; a *behaviour change* by default is a coin flip; a bare removal of `idle`
  will be rejected.

## 5. Options

### A. WiVRn: presence/wear-gated inhibitor

Hold `sleep:idle` only while `user_presence == true`.

- **Patch:** add `struct user_presence { bool present; }` to `from_monado::packets`
  (`wivrn_ipc.h`); `send_to_main(...)` from `wivrn_session::operator()(user_presence_changed&&)`
  (`wivrn_session.cpp:769`) and once at session start from `headset_info.user_presence`;
  handle it in `main.cpp`'s `control_received` visitor by `emplace()`/`reset()`ing the
  inhibitor. Fallback: if `headset_info_packet.user_presence == false` (headset can't report
  presence), pin the inhibitor on for the session. **≈40 lines, 3 files.**
- **Pro:** correct signal, already proven reliable in this exact stack (HypXRland's plug gate
  rides it). No timers.
- **Con:** still all-or-nothing on `What` — a doff now also un-blocks **suspend**, so a
  headset lying on the desk while the lid is shut can suspend the machine and kill the
  session. That is a *worse* failure than the one we're fixing.

### A′. WiVRn: split `What`, wear-gate only `idle` — **recommended**

Two inhibitor objects: `sleep` for the connection lifetime (today's semantics), `idle` for the
worn lifetime.

- **Patch:** A, plus give `sleep_inhibitor` a `const char* what` ctor parameter and keep two
  optionals in `main.cpp`. **≈50 lines, 3 files.** `What` is a colon-list, so this is a
  free split — one extra `Inhibit()` call, one extra fd.
- **Pro:** every failure mode of A disappears. Lid-close still safe; a doffed headset no longer
  freezes the desktop; suspend still can't cut a live session.
- **Con:** two fds instead of one (irrelevant); slightly more state in the FSM.

### B. WiVRn: static split — always block `sleep`, never block `idle`

- **Patch:** change one string literal to `"sleep"`. **1 line.**
- **Pro:** trivial, unbreakable, no new IPC.
- **Con:** relies entirely on a compositor-side inhibitor existing. Correct *only* when
  HypXRland is the XR app and its predicate covers wear. For a WiVRn VR game (no
  Hyprland-side inhibitor for the game's own window) the desktop screensaver would spawn on
  every monitor mid-play. Unupstreamable as a default. Good as a one-line **local** stopgap.

### C. WiVRn: config knob `[server] inhibit = "session" | "worn" | "sleep-only" | "none"`

The upstreamable packaging of A/A′/B: `session` = today, `worn` = A′, `sleep-only` = B,
`none` = nothing.

- **Patch:** A′ + one enum in `configuration.{h,cpp}` + JSON parse + docs. **≈80 lines.**
- **Pro:** the only variant with a real shot upstream (default `session` = no behaviour
  change); lets the user pick per machine (`worn` on the laptop, `session` on a desktop).
- **Con:** four-way knobs invite bikeshedding; needs doc + UI (`wivrn-dashboard` exposes
  config).

### D. Do nothing in WiVRn; HypXRland owns the policy

Requires B anyway (WiVRn's block dominates whatever the compositor does), so D is really
"B + widen `openxr:inhibit_idle`".

- **Patch (compositor side):** turn `openxr:inhibit_idle` from `bool` into a mode
  `off | focused | present` (default `present`): inhibit while a session exists **and**
  `m_userPresent` (falling back to `focused` when `m_userPresenceSupported` is false).
  `shouldInhibitIdle()` is 4 lines; the state it needs is already there
  (`OpenXRManager.hpp:607-614`); `recheckIdleInhibitorStatus()` is already called on every
  state transition, but would additionally need calling from the presence-change handler.
  **≈30 lines** + config-value + doc + one `hyprtester` case (there is already
  `xr_idle_inhibit`, `docs/openxr/06-testing.md:302`).
- **Pro:** fixes a *real* latent gap independent of WiVRn: today a worn-but-not-FOCUSED
  session (runtime dashboard in front, overlay mode with another app focused, XREAL glasses
  over direct Monado) does **not** inhibit, and only WiVRn's coarse block is masking it.
- **Con:** as the *whole* answer it's wrong — it does nothing for non-HypXRland XR apps, and it
  can't take effect at all until WiVRn stops blocking `idle`.

### E. hypridle-side only

Two sub-variants, both bad:

- `general:ignore_systemd_inhibit = true` — hypridle stops honouring **all** logind block
  inhibitors (video players, package managers, backups). Global collateral damage; it also
  doesn't distinguish worn from doffed, so it locks you mid-session unless HypXRland's Wayland
  inhibit happens to be up.
- per-listener `ignore_inhibit = true` — worse than it looks: hypridle implements it by
  subscribing with `get_input_idle_notification`, which ignores **every** inhibitor including
  HypXRland's. Reading a document in the headset without touching an input device for 10
  minutes then locks the headset and spawns screensavers on every XR quad.
- **Verdict:** include for completeness only. Zero patch, but the semantics are wrong in both
  directions.

### F. (added) Session-level answer: end the WiVRn session on prolonged doff

Let the Quest's own auto-sleep (or a WiVRn doff-timeout) drop the connection; the inhibitor
then releases through the existing `headset_disconnected` path with no new policy code.

- **Pro:** zero new state; also saves encoder/GPU power, which matters on battery.
- **Con:** upstream WiVRn has no doff-timeout to configure, so this depends on Quest-side
  auto-sleep settings the user may deliberately have disabled; and a re-don then costs a full
  reconnect + HypXRland re-plug sprint (the exact friction the plugged-state work removed).
  Complementary to A′ at a much longer timescale (e.g. 30 min), not a substitute.

### G. (added, out of scope) Per-output session lock

The thing the user actually described — "lock the laptop, keep working in the headset" — needs
either a Hyprland extension to `ext-session-lock` handling that exempts flagged outputs (and a
matching hyprlock change), or an XR-native lock surface. Both are XL, both are upstream
conversations, and both raise a genuine security question (an unlocked XR session on the same
seat is a lock bypass unless physical input is gated). Noted as a future WP; **not** part of
this decision.

## 6. Behaviour matrix

State legend: **W** = worn + HypXRland focused · **W-nf** = worn, session visible but not
focused (dashboard / other VR app / overlay) · **D** = doffed, still connected · **X** =
disconnected. Cells describe the **desktop** (dim/screensaver/lock) and, in brackets, whether
**suspend** is blocked.

| | W | W-nf | D | X |
|---|---|---|---|---|
| **Today** (`sleep:idle`, session-scoped) | awake ✓ [no-suspend ✓] | awake ✓ [✓] | **awake ✗ (bug)** [✓] | normal ✓ [—] |
| **A** presence-gated `sleep:idle` | awake ✓ [✓] | awake ✓ [✓] | idles ✓ [**suspend allowed ✗**] | normal ✓ [—] |
| **A′** split, `idle` wear-gated | awake ✓ [✓] | awake ✓ [✓] | idles ✓ [✓] | normal ✓ [—] |
| **A′ + D** (+ `inhibit_idle = present`) | awake ✓ [✓] | awake ✓✓ (belt-and-braces) [✓] | idles ✓ [✓] | normal ✓ [—] |
| **B** static split | awake ✓ (via HypXRland FOCUSED) [✓] | **idles ✗** — screensaver on every XR quad | idles ✓ [✓] | normal ✓ [—] |
| **B + D** | awake ✓ [✓] | awake ✓ [✓] | idles ✓ [✓] | normal ✓ [—] |
| **C** = A′/B/today by config | per knob | per knob | per knob | normal ✓ |
| **E** ignore_systemd_inhibit | awake ✓ (Wayland inhibit) [✓] | **idles ✗** | idles ✓ [✓] | **other apps' inhibitors ignored ✗** |
| **E** per-listener ignore_inhibit | **idles ✗ if you stop moving** | **idles ✗** | idles ✓ [✓] | normal ✓ |
| **F** doff-timeout disconnect | awake ✓ [✓] | awake ✓ [✓] | awake for N min, then full ✓ [✓ then —] | normal ✓ |

Scorecard:

| Option | Patch size | Upstreamable? | Fixes doffed case | Keeps suspend safe | Works without HypXRland | Main failure mode |
|---|---|---|---|---|---|---|
| A | ~40 ln / 3 files | maybe (behaviour change) | yes | **no** | yes | doffed + lid shut → suspend kills session |
| **A′** | **~50 ln / 3 files** | maybe → yes with C | **yes** | **yes** | **yes** | presence event lost while worn → desktop idles under you (backstopped by HypXRland + pointer activity) |
| B | 1 line | no | yes | yes | **no** | worn-not-focused → screensaver on every XR quad |
| C | ~80 ln | **yes** (default = status quo) | yes (when set) | yes | yes | knob nobody sets; 4-way enum bikeshed |
| D (needs B) | ~30 ln compositor | n/a (our fork) | only with B | yes | no | non-HypXRland XR apps unprotected |
| E | 0 | n/a | yes | yes | n/a | breaks unrelated inhibitors / locks you while reading |
| F | 0–20 ln | yes | eventually | yes | yes | slow; costs a reconnect; Quest-side setting dependent |
| G | XL | long conversation | n/a | n/a | n/a | security model of an unlocked XR session |

## 7. Recommendation

**Phase 1 (do this): A′, shipped as C.** Patch the vendored WiVRn
(`hypxr-patches-26.6.2`) to split the inhibitor and wear-gate the `idle` half, behind
`[server] "inhibit": "session" | "worn" | "sleep-only" | "none"` with **upstream default
`session`** and **our config set to `worn`**. Rationale:

1. It fixes precisely the observed bug (doffed-but-connected freezes the desktop) and nothing
   else.
2. It keeps the `sleep` block unconditional, which is the half that actually protects a live
   session (lid close, external `systemctl suspend`), and which we have *no* compositor-side
   equivalent for — Hyprland's inhibit is `ext-idle-notify` only, it cannot stop logind.
3. It needs **no grace timer**: both release paths restart the idle countdown from zero
   (§3.4), so a doff buys a full 10 min before the screensaver and 15 before the lock, and a
   re-don inside that window silently cancels. A phone glance is free.
4. Wear-gating uses a signal this stack already depends on for XR-monitor plug/unplug, with a
   built-in capability fallback to today's behaviour for headsets that don't report presence.
5. Default-`session` packaging makes it a plausible upstream PR (and dovetails with the
   portal migration in #847, whose flags are separable the same way). If upstream declines,
   we carry ~50 lines on a branch we already carry patches on.

**Phase 2 (recommended, independent): D's compositor half.** Widen `openxr:inhibit_idle`
from `bool` to `off | focused | present`, default `present`. This is worth doing on its own
merits — it closes the worn-but-not-FOCUSED hole (runtime dashboard, overlay mode, XREAL
glasses on direct Monado, where **no** logind inhibitor exists at all) — and it gives phase 1
a belt-and-braces backstop if a presence event is ever dropped. Add a `hyprtester` case beside
the existing `xr_idle_inhibit`.

**Explicitly not recommended:**

- **B alone.** One line, but it silently regresses worn-not-focused and every non-HypXRland
  WiVRn use. Acceptable only as a throwaway 24-hour experiment to confirm the diagnosis.
- **E, either variant.** Wrong granularity, global blast radius, and `ignore_inhibit`'s
  input-idle implementation actively defeats HypXRland's own inhibitor.
- **Any attempt to lock the physical panel while XR keeps running.** Not expressible: the
  session lock covers every output including XR quads (`Renderer.cpp:1104-1112`, `1637-1666`).
  For the private-office workflow use what already works — keep `eDP-1` disabled, or drive
  `hyprctl dispatch dpms off eDP-1` off the `openxractive` socket2 event as doc 05 §7 shows.
  Understand that this is **privacy, not security**: the seat stays unlocked. If real
  lock-while-worn is wanted, that is option G and a separate design.

**Per-machine settings** that follow from the above:

| Machine | `inhibit` | Notes |
|---|---|---|
| Framework laptop (AMD+NVIDIA, daily driver) | `worn` | lid usually shut, panel often disabled — the `sleep` half is load-bearing; the `idle` half must release on doff |
| Lunar Lake laptop (on battery) | `worn` | consider adding a hypridle **suspend** listener; then also consider F (drop the session after ~30 min doffed) so the machine can actually sleep |
| Desktops | `worn` or `session` | `session` is harmless there; `worn` is still tidier |

## 8. Open questions

1. **Does the Quest reliably deliver `user_presence_changed{false}` before the client is
   frozen on doff?** HypXRland's plug gate says yes in practice, but the inhibitor is a
   *system* state — a missed event leaves the desktop awake forever (fail-safe) or, on the
   inverse, idles under you (fail-open). Worth a one-session log check of
   `wivrn_session.cpp:769` firing on both edges before trusting it.
2. **Overlay mode focus semantics.** In `openxr:overlay = true`, does HypXRland reach
   `RUNNING_FOCUSED`, or only `VISIBLE`? Phase 2's `present` mode makes this moot, but it
   determines how bad B-alone would be in the meantime.
3. **Should `sleep` be wear-gated too, with a long grace, on battery?** Argues for a fifth
   knob value or a `worn` + `idle_only_grace_s` pair. Deferred until a suspend listener
   actually exists in the user's hypridle config.
4. **Portal migration (#847).** If WiVRn moves to `org.freedesktop.portal.Inhibit`, the
   `SUSPEND`/`IDLE` flag split maps 1:1 onto A′ — but hypridle watches logind's
   `BlockInhibited`, and it is not obvious the portal's inhibitor surfaces there identically.
   Verify before assuming the fix survives that migration.
5. **Does anything else on the box hold `idle`?** `systemd-inhibit --list` should be re-read
   after the patch to confirm WiVRn was the only `idle` blocker.
