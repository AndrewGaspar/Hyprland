# Design: `hypxrhud` — a shared XR HUD daemon (one overlay session, D-Bus API)

Design memo (2026-07-13). **No implementation.** Proposes a new standalone
daemon, `hypxrhud`, that owns **one** OpenXR overlay session and exposes a
**D-Bus API**, so XR utilities — hypxrvoice's feedback HUD, the planned
`hypxrkeys` screenkey overlay (doc `05`), and in-headset notification toasts —
stop each reimplementing session lifecycle / EGL / swapchain / fade machinery.
The user reviews this before any code.

Evidence base, all read for this memo: hypxrvoice's already-shipped in-process
HUD (`/home/ajg/code/hypxrvoice`, branch `wp-v5`, commit `4646249`) — which is
**already 80 % of hypxrhud**, just wired as a private per-daemon subprocess over
a stdin pipe; the vendored Monado tree (`subprojects/monado` @ `c2ddab59d`, the
exact runtime the suite runs against); HypXRland's `src/openxr/OpenXRManager.cpp`
(session-lifecycle / re-probe machinery); doc `05-xr-screenkey.md` (the second
overlay client); the Omarchy 3.8.2 autostart; and web research on Monado
multi-overlay support + D-Bus monitoring privileges (URLs cited inline).

---

## TL;DR

1. **The extraction target already exists and already works.** hypxrvoice's
   WP-V5 HUD is a clean three-layer split: a **pure view model + rasteriser**
   (`HudModel`/`HudText`/`HudMessage`, no GL), a **thin overlay subprocess**
   (`hud/HudSession.cpp` + `hud/Egl.cpp` + `hud/hud_main.cpp`) that owns the XR
   session and renders one VIEW-space quad, and a **daemon-side manager**
   (`HudOverlay.cpp`) that spawns the subprocess and streams `SHudView` JSON
   lines to its stdin. hypxrhud = **promote that subprocess into a shared
   daemon**, replace the private stdin pipe with a D-Bus multiplexer, and grow
   the single quad into an N-panel scene. Most of the hard XR code lifts almost
   verbatim.
2. **The per-frame layer budget is generous.** On the Linux host that runs
   Monado/WiVRn's compositor, `maxLayerCount` = `info->max_layers`
   (`oxr_system.c:526`), which is `render_max_layers_capable(...)` **floored at
   16, capped at `XRT_MAX_LAYERS = 128`** (`render_util.c:102,136`;
   `xrt_limits.h:86`). So one quad per panel, ~8–12 panels, is comfortably
   inside budget. **One quad per panel** (not one composited mega-quad) is the
   right call: it gives free per-panel opacity/fade via `color_scale_bias` and
   keeps the "upload only on change" rule — a static panel never re-rasters,
   `xrEndFrame` just re-submits its swapchain (the cursor-dead-band lesson).
3. **The overlay session survives a compositor restart.** Monado's multi-system
   compositor runs its own dedicated render thread (`multi_main_loop`,
   `comp_multi_system.c:479`, driven by the native compositor `msc->xcn`, **not**
   by any client). `transfer_layers_locked` composites every client that is
   `delivered.active && state.visible && session_active` (`:279-302`); a dead
   primary (HypXRland restarting) simply goes inactive and is skipped, and the
   overlays — including hypxrhud — **keep rendering**. No dependency on the
   primary staying alive.
4. **D-Bus fits the single-threaded XR loop.** sd-bus (already on the box via
   systemd) exposes its fd; drop it into the same `poll()` set as the XR frame
   pacing — a fourth fd, no threads, the Monado fence contract stays satisfied by
   construction. High-frequency updates (per-word transcript) use
   `NO_REPLY_EXPECTED` → fire-and-forget, no round-trip, thousands/sec headroom.
5. **Notifications: presence-gated mirror, mako keeps the name.** (Revised per
   user 2026-07-13: the separate XR SDDM login is a *temporary artifact* of not
   being upstreamed — the design must assume ONE session long-term, with mako
   the notification daemon while the headset is doffed and hypxrhud taking over
   only while **donned**.) mako owns `org.freedesktop.Notifications`
   permanently; hypxrhud watches the bus via **BecomeMonitor** and, on the
   presence edge (donned), renders mirror toasts in-headset while suppressing
   mako's 2D popups via `makoctl mode`; on doff it stands down and mako resumes.
   Action/dismiss round-trips go **through** mako (`makoctl invoke/dismiss -n
   <id>`), so no ownership churn ever happens. Recommend: **(b′)** — the
   spec only when configured as the session daemon.
6. **Recommend a new standalone repo** `~/code/hypxrhud` (BSD-3, hypxrpaper/
   hypxrvoice conventions). The render core lifts *out* of hypxrvoice into
   hypxrhud; hypxrvoice becomes a client. hypxrkeys becomes the second client
   (this memo is doc `05`'s **WP-K0**).
7. **Staged plan: WP-H1…H10** below. Critical path H1→H2→H3→H4; slots (H5),
   theming (H6), packaging (H7) parallelise; H8 migrates hypxrvoice; H9 lands
   hypxrkeys; H10 adds Notifications ownership.

---

## Architecture sketch

```
   ┌───────────────────────────────────────────────────────────────────┐
   │           Monado / WiVRn multi-system compositor                    │
   │   own render thread (multi_main_loop); sorts clients by z_order;    │
   │   composites every active+visible session's layers each frame       │
   └────────▲──────────────────▲───────────────────────▲────────────────┘
    z_order  │ INT64_MIN        │ openxr:overlay_z=1     │ hudZ=20 (top)
   (bot→top) │                  │                        │
       ┌─────┴──────┐   ┌───────┴────────┐      ┌────────┴─────────────────┐
       │ hypxrpaper │   │  HypXRland     │      │  hypxrhud (THIS DAEMON)  │
       │  primary   │   │  (primary OR   │      │  ONE overlay session      │
       │  bg / glTF │   │   overlay)     │      │  N head-locked quads      │
       └────────────┘   │  monitor quads │      │  ┌─────────────────────┐ │
                        └────────────────┘      │  │ scene: panel table  │ │
                                                │  │  id→{swapchain,      │ │
                                                │  │   quad,csb,slot,     │ │
                                                │  │   owner-unique-name} │ │
                                                │  └─────────────────────┘ │
        clients (D-Bus) ─────────────────────► │  sd-bus fd in the XR      │
   ┌──────────────┐ ┌──────────────┐ ┌────────┐│  poll() loop; NameOwner-  │
   │ hypxrvoiced  │ │ hypxrkeys    │ │ any app││  Changed → auto-dismiss   │
   │ CreatePanel  │ │ CreatePanel  │ │ notify-││  its client's panels      │
   │ /UpdatePanel │ │ (keys+ipc)   │ │ send → ││                           │
   └──────────────┘ └──────────────┘ │ o.f.d. ││  render core (lifted from │
                                     │ Notif. ││  hypxrvoice HUD): HudModel │
                                     └────────┘│  /HudText/stb_truetype/    │
                                                │  bundled OFL font          │
                                                └────────────────────────────┘
```

Everything below cites the source that establishes each claim.

---

## 1 — Session + layer architecture

### 1.1 One session, N panels, one quad each

The wp-v5 subprocess already submits a single `XrCompositionLayerQuad` in a
VIEW-space reference space with a chained `XrCompositionLayerColorScaleBiasKHR`
for free fades (`hud/HudSession.cpp:317-341`; `createSpace` uses
`XR_REFERENCE_SPACE_TYPE_VIEW` at `:126-131`). hypxrhud generalises this to a
**panel table**: each panel owns its own swapchain + quad + color-scale-bias, and
`renderFrame` pushes `layers.push_back(&quad)` once per visible panel, then one
`xrEndFrame` with `layerCount = N`.

**Layer budget (verified).** `xrGetSystemProperties` fills
`graphicsProperties.maxLayerCount`. On the host that runs the compositor (the
Linux box, for both Monado-null and WiVRn's server-side compositor):

- `oxr_system.c:525-529` — `maxLayerCount = info->max_layers` when a compositor
  is present, else `XRT_MAX_LAYERS`.
- `comp_compositor.c:1161-1164` sets `max_layers = render_max_layers_capable(vk,
  use_compute, XRT_MAX_LAYERS)`.
- `render_util.c:102,127-136` — the result is **floored at 16** (spec minimum)
  and capped at the requested `XRT_MAX_LAYERS`.
- `xrt_limits.h:83-88` — `XRT_MAX_LAYERS` is **128** on Linux/Windows/OSX, 32 on
  Android. WiVRn's server compositor runs on the Linux host → 128; the OpenXR
  minimum guarantee is 16 regardless.

So a HUD with up to ~a dozen panels is never near the ceiling. hypxrhud should
still **query `maxLayerCount` at startup** and refuse (or coalesce) panels beyond
a conservative internal cap (say `min(maxLayerCount, 16)`), so a
runtime-that-only-guarantees-16 never gets an over-budget `xrEndFrame` (Monado
rejects an over-count frame with `XR_ERROR_LAYER_LIMIT_EXCEEDED`,
`oxr_api_session.c:193-195`). The per-client multi-compositor limit
`MULTI_MAX_LAYERS = XRT_MAX_LAYERS` (`comp_multi_private.h:41`) is the same 128,
so other overlay clients don't eat into hypxrhud's budget.

### 1.2 One quad per panel vs one composited quad

| | one quad per panel *(recommend)* | one big composited quad |
|---|---|---|
| opacity/fade | free per-panel via `color_scale_bias.a`, no re-upload (`HudSession.cpp:334-340`) | fade couples all panels; independent fade needs a re-raster |
| damage/redraw | upload only the panel whose content changed; static panels re-submit for free | any sub-panel change re-rasters the whole texture |
| z within HUD | submission order in the `layers[]` array = stacking (later = top) | manual painter's-order compositing on CPU |
| space per panel | each panel can be VIEW *or* LOCAL independently | one space for the whole surface |
| cost | one small swapchain per panel (768×384-ish); tiny | one big swapchain, re-rastered often |

The composited-quad option only wins if you exceed the layer budget, which we
don't. **One quad per panel.** This directly preserves the "upload once,
re-present every frame" idle-monitor trick the HUD already relies on
(`HudSession.cpp:238-248` uploads only when `readStdin` produced new content;
`renderFrame` never re-uploads for a fade).

### 1.3 VIEW vs LOCAL per use case

`verify_space` accepts any non-null space and `handle_space` has an explicit
head-lock fast-path for VIEW (`oxr_session_frame_end.c:1207-1215`, per doc `05`
§Decision 4), so VIEW-space quads are head-locked with **zero per-frame
`xrLocateSpace`** on our side.

- **Voice HUD, toasts, keystrokes → VIEW** (head-locked, transient, glanceable).
  This is what wp-v5 already ships (`hud/HudSession.hpp:33`, pose `0,-0.25,-1.0`).
- **Pinned status widgets → LOCAL** (world-fixed; you can look away). hypxrhud
  should expose the space per panel (`space: "view"|"local"`), defaulting to
  VIEW. A LOCAL panel needs a LOCAL reference space and a stable pose — cheap to
  add, deferred to a later WP.

### 1.4 z-order across the three XR clients

Three independent OpenXR processes; the runtime merges their layers by
`sessionLayersPlacement → xrt_session_info.z_order` (doc `05` §Decision 1, verified
`oxr_session.c:1447-1454`, `overlay_sort_func` `comp_multi_system.c:211-227`,
qsort `:306`). Assignments:

- **hypxrpaper** — primary session, pinned `z_order = INT64_MIN`
  (`ipc_server_process.c:430-461`), bottom.
- **HypXRland** — `openxr:overlay_z` **default 1** (`ConfigValues.cpp:905`) when
  run as an overlay; or the primary itself when `openxr:overlay` is unset (the
  current `hyprland-xr.conf` does **not** set it → HypXRland is primary and
  hypxrpaper isn't present unless launched).
- **hypxrhud** — `hudZ` **default 20** (already the wp-v5 default,
  `Config.hpp:73`, `HudSession.hpp:36`), so the HUD sits **on top** of monitors.

**Rule (from doc `05`): keep all three distinct** — equal z_order makes
`overlay_sort_func` return 0 and `qsort` is not stable → indeterminate stacking.
Document the trio (paper < land < hud) as `INT64_MIN` / 1 / 20.

---

## 2 — D-Bus interface design

### 2.1 Names

- **Bus name:** `dev.hypxr.Hud` (well-known). *(Bikeshed: `org.hypxrland.Hud` /
  `io.github.andrewgaspar.hypxrhud` — pick a reverse-DNS you control; the
  memo uses `dev.hypxr.Hud`.)*
- **Object path:** `/dev/hypxr/Hud` (the manager).
- **Interface:** `dev.hypxr.Hud1` (versioned suffix, xdg-portal convention).
- **Per-panel objects (optional):** `/dev/hypxr/Hud/panel/<id>` if we want
  per-panel signals; v1 can stay handle-based (methods take a `u id`) to keep it
  simple. xdg-desktop-portal uses per-Request/Session objects with handle
  tokens; that's heavier than a HUD needs.

### 2.2 Methods (manager interface `dev.hypxr.Hud1`)

```
# Declarative panel lifecycle. props/hints are a{sv} — extensible, mako/notify style.
CreatePanel(in  a{sv} props)            -> (out u id)
UpdatePanel(in  u id, in a{sv} props)   -> ()      # flag NO_REPLY_EXPECTED for hot paths
DismissPanel(in u id)                   -> ()

# Convenience: a freedesktop-shaped toast without the full spec surface.
Toast(in s summary, in s body, in a{sv} hints) -> (out u id)

# Discovery.
Capabilities()  -> (out a{sv})          # maxPanels, hasColorScale, spaces[], slots[],
                                        #  runtimeName, runtimePresent, notificationsOwned
```

`props` / `hints` keys (all optional, defaulted):

| key | type | meaning |
|---|---|---|
| `slot` | `s` | named placement slot (`voice`,`keys`,`toast`,`status`,…); arbiter resolves collisions (§4) |
| `space` | `s` | `view` (default) / `local` |
| `pose` | `(ddd)` | override centre in metres (else slot default) |
| `size` | `d` | quad width, metres (height from texture aspect) |
| `lines` | `a(su)` | structured text: `(text, colorRole)` — colorRole ∈ semantic enum (§5) |
| `title` | `s` | big line |
| `confidence` | `d` | `[0,1]` → draws a certainty bar (voice) |
| `urgency` | `y` | `0` low / `1` normal / `2` critical → slot priority + default expiry |
| `expire_ms` | `i` | auto-dismiss after N ms; `-1` = until UpdatePanel/DismissPanel; `0` = server default |
| `rise_ms`/`hold_ms`/`fade_ms` | `i` | fade envelope (else server default; the wp-v5 `SHudView` fields) |
| `opacity` | `d` | per-panel ceiling |

This is deliberately the wp-v5 `SHudView` (`HudModel.hpp:44-72`) re-expressed as
`a{sv}` — the pure model already carries exactly these fields (lines+colorRole,
confidence, approximated/dryRun, rise/hold/fade, opacityCeil), so the marshalling
is mechanical.

### 2.3 Signals & properties

```
signal RuntimeStateChanged(s state)     # "present" | "absent" | "headset-undonned"
signal PanelDismissed(u id, s reason)   # "expired" | "client" | "preempted" | "client-gone"
property b   RuntimePresent   (read)
property s   RuntimeName      (read)
property u   MaxPanels        (read)
property b   NotificationsOwned (read)  # is hypxrhud the o.f.d.Notifications owner here?
```

`RuntimeStateChanged` is what lets a client fall back to notify-send while the
headset is absent (the current degrade path, `HudOverlay.cpp:degrade`).

### 2.4 Client lifetime — auto-dismiss on disconnect

Each panel records its creator's **unique bus name** (`sender` on the incoming
method call). hypxrhud subscribes to
`org.freedesktop.DBus.NameOwnerChanged`; when a creator's unique name drops
(`new_owner == ""`), **auto-dismiss all its panels** with reason `client-gone`.
This is the behaviour a HUD wants and the freedesktop Notifications spec does
*not* give (notifications persist past sender death) — so hypxrhud adds it. A
crashed hypxrvoiced must not leave a stale "listening…" panel stuck in the
headset.

### 2.5 Activation & systemd wiring

Follow **mako's** model (mako ships both a D-Bus service file and is
socket/bus-activated). Two files:

- `/usr/share/dbus-1/services/dev.hypxr.Hud.service`:
  ```
  [D-BUS Service]
  Name=dev.hypxr.Hud
  Exec=/usr/bin/hypxrhud
  SystemdService=hypxrhud.service
  ```
- `~/.config/systemd/user/hypxrhud.service` (Type=dbus, `BusName=dev.hypxr.Hud`,
  `PartOf=graphical-session.target`) — mirror the shipped
  `hypxrvoice.service` (`systemd/hypxrvoice.service`), including the
  `import-environment` note so `HYPRLAND_INSTANCE_SIGNATURE` / `WAYLAND_DISPLAY`
  reach it. Bus-activated on the first `CreatePanel`/`Toast`, so a session that
  never shows a HUD pays nothing (same laziness as the wp-v5 subprocess spawn).

**Convention theft:**
- *mako / freedesktop Notifications*: the `a{sv}` hints dict (extensible without
  an ABI break), `expire_timeout`, `GetCapabilities`. hypxrhud's `hints` mirror
  these so the `Toast` path and the spec path share a vocabulary.
- *dunst*: keeps `org.freedesktop.Notifications` **and** adds a private control
  interface (`org.dunstproject.cmd0`) for `dunstctl`. hypxrhud does the same
  split: spec surface (optional) + private `dev.hypxr.Hud1` for rich panels.
- *xdg-desktop-portal*: versioned interface suffix (`…1`), `a{sv}` options,
  handle tokens. Steal the version suffix; skip the Request/Session object
  ceremony (overkill for a HUD).
- *waybar/hyprctl*: **not** D-Bus (waybar consumes socket2). Not a model to copy
  for the API, but a reminder that the IPC-echo lane (doc `05` §8) stays on
  socket2 — hypxrkeys reads socket2 directly for that, independent of hypxrhud.

---

## 3 — `org.freedesktop.Notifications` (mako coexistence)

**The constraint.** One well-known name, one owner. mako is launched by Omarchy
via `exec-once = uwsm-app -- mako`
(`~/.local/share/omarchy/default/hypr/autostart.conf:2`), and — crucially —
`hyprland-xr.conf` **sources `hyprland.conf`** (line 11), which pulls in that same
autostart. So today the XR session *also* launches mako, and mako owns the name
there.

**Constraint revision (user, 2026-07-13).** An earlier draft leaned on the XR
desktop being a separate SDDM login (its own session bus → per-session daemon
choice). The user corrected this: the separate login is a **temporary artifact
of HypXRland not being upstreamed** (kept so a stock 0.55.4 Hyprland remains
available for stability), and the required behavior is **dynamic, within one
session**: mako is the notification daemon while the headset is **doffed**;
hypxrhud takes over the in-headset notification experience only while
**donned**. Any design that assumes two session buses is scaffolding-dependent
and rejected. Note that D-Bus name juggling on the presence edge is NOT viable
as the mechanism: mako does not request the name with `ALLOW_REPLACEMENT` and
has no release verb, so trading ownership would mean killing/restarting mako on
every don/doff — fragile, racy, and it churns notification history.

Options:

- **(a) hypxrhud owns the name in the XR session and renders in-headset,
  forwarding nothing to a 2D daemon.** Clean when the XR session is the *only*
  session (headset-primary desktop-replacement box). hypxrhud implements
  `Notify`/`CloseNotification`/`GetCapabilities`/`GetServerInformation` +
  `NotificationClosed`/`ActionInvoked`, mapping each `Notify` to a `toast`-slot
  panel. **Downside:** it must actually implement the spec well enough
  (urgency→slot, expire_timeout, replaces_id, actions best-effort). Effort S–M.
- **(b) mako keeps the name; hypxrhud mirrors via BecomeMonitor.** The session
  owner **may monitor their own session bus** without extra privilege (dbus
  reference impl: "each user may monitor their own session bus" —
  freedesktop `BecomeMonitor` doc; the GNOME Discourse thread shows exactly
  `BecomeMonitor` matching `interface='org.freedesktop.Notifications'` working
  from a normal user). hypxrhud calls
  `org.freedesktop.DBus.Monitoring.BecomeMonitor` with a rule for
  `member='Notify'` and renders a mirror panel. **Caveats:** (1) a monitor is
  **read-only** — it can't emit `NotificationClosed`/`ActionInvoked`, so actions
  and dismissal don't round-trip; (2) **double render** (mako's 2D surface + the
  HUD) unless mako is suppressed; (3) **dbus-broker** (Arch/systemd default,
  which Omarchy uses) is stricter than the reference daemon about monitor
  filtering (dbus-broker#210) — needs a live confirmation before relying on it.
  Good for the *nested-preview / shared-host-bus* case where you can't replace
  mako; weak as the primary design.
- **(c) hypxrhud implements the spec only when configured as the daemon.** A
  config knob `notifications = own | mirror | off` (default `off`). When `own`,
  the XR session's autostart runs **hypxrhud in place of mako** (edit that one
  `exec-once` line, or ship an Omarchy XR-session override), and hypxrhud owns
  the name. When `mirror`, it uses (b) as a best-effort fallback (nested
  preview). When `off`, only the custom `Toast` API is available.
- **(d) skip the freedesktop spec in v1; custom `Toast` only.** Simplest;
  hypxrvoice already calls `notify-send` directly (`Feedback.cpp:notify`), so a
  `Toast` method covers the in-house clients. But third-party apps
  (`notify-send`, browser notifications) wouldn't reach the headset.

**Recommendation (revised): (b′) — presence-gated mirror with makoctl
round-trip. mako keeps the name permanently; there is no handover of ownership,
only of *rendering*.**

The (b) caveats dissolve one by one under the single-session/donned-gate
constraint:

1. **Read-only monitor → no action/close round-trip.** Solved by going *through*
   mako instead of around it: a monitor connection sees both the `Notify` method
   call **and its method-return** (monitors receive replies), so hypxrhud
   correlates the reply serial to learn each notification's **id**. A toast
   interaction then shells `makoctl invoke -n <id> [action]` / `makoctl dismiss
   -n <id>` — mako emits the authoritative `ActionInvoked`/`NotificationClosed`
   signals itself, and the sending app sees exactly the daemon it expects.
2. **Double render.** On the donned edge hypxrhud applies a configurable
   suppression command (default `makoctl mode -a do-not-disturb`; Omarchy
   already defines the mode and `omarchy-toggle-notifications` toggles it) so
   mako stops popping 2D surfaces onto the monitor quads while the mirror
   renders head-locked toasts; on doff it removes the mode and stands down.
   Notifications continue to land in mako's history the whole time — nothing is
   lost across the transition, and dnd-suppressed `Notify` calls are still fully
   visible to the monitor connection.
3. **Presence edge.** The compositor already tracks `XR_EXT_user_presence` (the
   plugged-state gate). v1: hypxrhud polls `hyprctl -j openxr status` at 1 Hz
   (the hypxrvoice pattern); follow-up: a tiny compositor addition emitting a
   socket2 `openxrpresence>>yes|no` event makes the handover edge-triggered —
   worth bundling into whichever compositor WP touches XRIpc next.

**The dbus-broker BecomeMonitor caveat is now load-bearing and must be verified
STEP ZERO of WP-H10** (`busctl monitor` working on this box is strong evidence —
it uses the same interface — but confirm the narrow
`interface='org.freedesktop.Notifications',member='Notify'` match rule and that
method-returns are delivered under dbus-broker before building on it). If
monitoring fails verification, the fallback is (c) *static* ownership with its
acknowledged regression (hypxrhud would own the name always and render doffed
notifications onto a 2D fallback surface or via mako-as-history only) — a
clearly worse shape, which is why the verification comes first.

(a)/(c)-style ownership remains the right answer only for a true
headset-primary session with no 2D use; keep it as a config mode
(`notifications = mirror | own | off`, default `mirror`), not the default.

---

## 4 — Real-estate arbitration (slots)

### 4.1 Slot model

A **slot** is a named VIEW-space anchor with a default pose, a stacking rule, and
an occupancy policy. Proposed defaults (metres, VIEW space, `-z` forward):

| slot | default pose | stacking | occupancy |
|---|---|---|---|
| `voice` | `0,-0.28,-1.0` (bottom-centre) | single | last-writer-wins (one live voice panel) |
| `keys` | `0,-0.14,-1.0` (just above voice) | single | last-writer-wins |
| `toast` | `0,+0.30,-1.1` (top-centre) | **stack** (newest lowest, older pushed up, bounded N=3) | multi |
| `status` | `+0.55,+0.20,-1.2` (upper-right, LOCAL-capable) | single | pinned |

Each slot is config-overridable (`pose`, `size`, `space`, `max`, `priority-mode`)
in `~/.config/hypxrhud/hypxrhud.conf` (hyprlang syntax, matching the doc `05`
recommendation for the family).

### 4.2 Collision policy

The daemon is the **single owner of the scene**, so arbitration is centralised
(unlike three independent sessions fighting for head-space today). Two clients
targeting one **single** slot:

1. **Priority** — higher `urgency` wins the slot; the loser is either (a) queued
   (shown when the winner dismisses) or (b) nudged to an overflow offset,
   configurable per slot. Critical urgency always preempts and emits
   `PanelDismissed(loser, "preempted")`.
2. **Equal priority** — last-writer-wins for singleton slots (matches the voice
   HUD's own semantics: a new action replaces the listening panel,
   `Feedback.cpp:emitAction`).
3. **`toast` slot** is a bounded stack, so "collision" is normal — panels offset
   vertically, oldest evicted past `max`.

A client may pass `slot: ""` + explicit `pose` to bypass slotting entirely
(free placement, its own responsibility for overlap).

---

## 5 — Rendering + theming core

### 5.1 What lifts out of wp-v5 vs what's new

**Lifts almost verbatim (into a shared `hud_core`):**
- `HudModel.hpp/.cpp` — the pure view model (`SHudView`, `SHudLine`, `EHudColor`,
  `hudOpacity`, the builders). Generalise `SHudView` → a generic `SPanel`; the
  fields already match the D-Bus `props` (§2.2).
- `HudText.hpp/.cpp` — stb_truetype rasteriser → premultiplied RGBA
  (`renderHud`, top-row-first, rounded panel hugging content). Reused unchanged;
  it's CPU-only and already the offline-test surface.
- `hud/HudSession.cpp` + `hud/Egl.cpp` — the overlay session: instance/system/
  session/space/swapchain bring-up, the color-scale-bias fade math
  (`renderFrame:298-341`), the EGL-held-current fence discipline
  (`init:368`), the `SIGINT`→`xrRequestExitSession` clean exit (`hud_main.cpp`).
  This becomes the daemon's XR core, extended from one quad to the panel table.
- The bundled OFL font + `stb_impl.cpp` vendoring
  (`third_party/fonts/LiberationMono`, commit `d3d9880`) — carry over.

**New:**
- **Multi-panel scene manager** — the panel table (id → swapchain/quad/csb/slot/
  owner), per-panel dirty flags + upload-on-change, N-quad submission, layer
  budget guard.
- **D-Bus front end** — sd-bus vtable, `CreatePanel`/`Update`/`Dismiss`/`Toast`/
  `Capabilities`, signals/properties, `NameOwnerChanged` tracking, fd folded into
  the XR poll loop.
- **Slot arbiter** (§4).
- **Per-client budgets** — cap panels per unique-name (e.g. 4) so one buggy
  client can't exhaust the scene.
- **Lifecycle/backoff** (§6) — the daemon must *not* exit on runtime loss (the
  current subprocess does; `HudSession.cpp:291` sets `m_exit`).

`HudMessage.hpp/.cpp` (the stdin JSON wire format) is **retired** — the D-Bus
`a{sv}` replaces it. The *model* it serialised stays.

### 5.2 Theming — Omarchy integration

`EHudColor` (`HudModel.hpp:35-42`) is already a semantic palette
(`Normal/Dim/Accent/Good/Warn/Bad`) mapped to concrete RGBA in `HudText` — the
right seam for theming. Omarchy exposes the **current** theme under
`~/.config/omarchy/current/theme/` (a symlink; e.g. `mako.ini` there
`include`s `~/.local/share/omarchy/default/mako/core.ini` —
`~/.config/omarchy/current/theme/mako.ini:1`). Clean approach:

- Read theme colours from `~/.config/omarchy/current/theme/` at start (parse the
  mako/hyprland colour tokens, or a dedicated colours file), map to the
  `EHudColor` roles. Font default stays the bundled OFL mono (no runtime font
  dependency).
- **Live reload:** Omarchy's theme switch runs `omarchy-restart-mako` (`makoctl
  reload`) — hypxrhud has no equivalent yet, so watch the
  `~/.config/omarchy/current/theme` symlink (inotify on the parent) **or** accept
  a `Reload()` D-Bus method / `SIGHUP`, and re-map colours. The nested-config
  generator already sources the Omarchy theme; hypxrhud reads the same source of
  truth directly.
- Colours remain overridable in `hypxrhud.conf` (per-role hex), theme is the
  default.

---

## 6 — Lifecycle

The daemon must replicate the **class** of handling in
`src/openxr/OpenXRManager.cpp`, but as a standalone process. The current wp-v5
subprocess is deliberately *disposable* (exits on any loss so the daemon
re-spawns); hypxrhud is *persistent* and owns the state, so it must loop instead
of exit.

### 6.1 Runtime absent at login

Copy the compositor's UNAVAILABLE re-probe (`OpenXRManager.cpp:2083-2140`,
`OpenXR::xrReprobeBackoffMs` at `XRMonitorConfig.cpp:384-398`):

- On start, try `xrCreateInstance` + `xrGetSystem`. Missing runtime →
  **grow a backoff** base→30 s (doubling: `xrReprobeBackoffMs(attempt, base,
  30000)`), so a permanently-absent runtime is cheap.
- `xrGetSystem` returning `XR_ERROR_FORM_FACTOR_UNAVAILABLE` (runtime up, headset
  not donned) → poll at a **gentle fixed cadence** (don't grow the backoff, or
  donning feels laggy — the exact distinction the compositor draws at
  `OpenXRManager.cpp:2097-2100`).
- Panels created while absent are **accepted and held pending**; publish
  `RuntimePresent = false` + `RuntimeStateChanged("absent"|"headset-undonned")`
  so clients keep their notify-send fallback (`Feedback.cpp` already has this
  branch). When the session comes up, raster + submit the pending panels.

hypxrhud can reuse `xrReprobeBackoffMs` verbatim (it's a pure, gtested function)
— vendor a copy or share it.

### 6.2 WiVRn disconnect / reconnect

The subprocess today sets `m_exit` on `LOSS_PENDING`/`EXITING`/`INSTANCE_LOST`/
`SESSION_LOST` (`HudSession.cpp:272-284,291,296,350`). The **daemon must not
exit** — instead: tear down the session (`xrDestroySession`/`Instance`), keep the
panel table, re-enter §6.1 probe/backoff, and on reconnect rebuild the session +
swapchains and **re-raster every live panel** (they were upload-on-change, so
force-dirty them). Surface a `RuntimeStateChanged` around the gap.

### 6.3 Compositor restart (primary death)

**Verified: the overlay outlives it.** Monado's `multi_main_loop`
(`comp_multi_system.c:479`) runs on its own thread driven by the native
compositor, and `transfer_layers_locked` (`:255-303`) composites every
active+visible client independently; a dead primary just fails the
`delivered.active` check (`:279`) and is skipped. So when HypXRland (the primary)
restarts, hypxrhud keeps rendering its panels over passthrough/blank the whole
time — **no reconnect needed on the hypxrhud side**. (One nuance:
`find_active_blend_mode` picks the blend mode from the bottom-most active client
`:229-236`, so with no primary the HUD's own blend mode applies — set it
sensibly, e.g. alpha for passthrough.)

### 6.4 Daemon crash recovery

hypxrhud is the authoritative scene owner, so a crash loses panel state. Recovery:
bus-activation restarts it on the next `CreatePanel`/`Toast`
(`SystemdService=`); clients should treat calls as best-effort and **re-declare
panels** on reconnect (watch `dev.hypxr.Hud`'s `NameOwnerChanged`, or just retry
with backoff). The **notify-send fallback stays the safety net** — it already is,
in `Feedback.cpp`. Argue the SPOF the other way (§8): one audited persistent
process is *less* fragile than three utilities each re-implementing the fence
contract.

---

## 7 — Repo + migration plan

### 7.1 Repo: new standalone `~/code/hypxrhud`

**Recommend a new BSD-3 repo**, hypxrpaper/hypxrvoice conventions (`src/`,
`third_party/`, `cmake/`, `systemd/`, hand-rolled argv, `--gpu` pin, vendored
stb + OFL font). Rationale:

- It's **shared** by voice + keys + toasts. Living inside hypxrvoice would force
  hypxrkeys and a generic toast client to depend on the whole voice stack
  (whisper.cpp, llama.cpp, pipewire) — wrong coupling.
- The render core **moves out of** hypxrvoice into hypxrhud; hypxrvoice then
  *links nothing XR* (it already doesn't — the daemon never links OpenXR/EGL,
  only the subprocess does, `HudOverlay.hpp` comment). Post-migration hypxrvoice
  is a pure D-Bus client, simpler than today.
- Matches the family shape (hypxrpaper, hypxrkeys, hypxrvoice all standalone).

### 7.2 Work packages

Critical path **H1→H2→H3→H4**. H5/H6/H7 parallelise after H3. H8 (voice
migration) after H4. H9 (keys) after H5. H10 (Notifications) after H7.

- **WP-H1 `[M]` — Repo skeleton + overlay core extraction.** New `hypxrhud`
  repo; lift `hud/HudSession`, `hud/Egl`, `HudModel`, `HudText`, `HudMessage`,
  stb + font from hypxrvoice into a `hud_core` lib + a daemon `main` that owns
  **one** overlay session and renders one panel from an internal test source.
  *Accept:* reaches FOCUSED as an overlay over a running primary under vendored
  Monado; quad visible; `--gpu` honoured; clean `SIGTERM` exit.
- **WP-H2 `[M]` — Multi-panel scene.** Panel table, per-panel swapchain + quad +
  color-scale-bias, per-panel dirty/upload-on-change, N-quad `xrEndFrame`, layer
  budget query + guard. *Accept:* two panels with independent fade envelopes;
  only the changed panel re-uploads (assert via a raster-count/`HYPXRHUD_DUMP`
  hook); over-budget rejected gracefully.
- **WP-H3 `[L]` — D-Bus front end.** sd-bus vtable for
  `CreatePanel`/`UpdatePanel`/`DismissPanel`/`Toast`/`Capabilities`, signals +
  properties, `NameOwnerChanged` auto-dismiss, sd-bus fd folded into the XR
  `poll()` loop (no threads). `NO_REPLY_EXPECTED` on `UpdatePanel`. *Accept:*
  `busctl call` drives panels live; killing a client auto-dismisses its panels;
  a headless unit test drives the vtable against a stub scene (no XR).
- **WP-H4 `[M]` — Lifecycle + backoff.** Port `xrReprobeBackoffMs` +
  runtime-absent/headset-undonned distinction; **daemon survives** WiVRn
  disconnect/reconnect (rebuild session, re-raster live panels); `RuntimePresent`
  property + `RuntimeStateChanged` signal. *Accept:* start with runtime absent →
  `CreatePanel` accepted + pending + `absent` signal; bring the runtime up →
  panels appear; kill/restart the runtime → panels survive the gap; compositor
  restart → HUD keeps rendering (per §6.3).
- **WP-H5 `[M]` — Slot model + arbiter.** Named slots with default poses,
  `slot`/`urgency`/priority policy, toast stacking, per-client panel cap.
  *Accept:* two clients contest `voice` → priority/last-writer resolves +
  `PanelDismissed("preempted")`; three toasts stack and evict past `max`.
- **WP-H6 `[M]` — Theming.** Load Omarchy `current/theme` colours → `EHudColor`
  roles; live reload (symlink inotify or `Reload()`/`SIGHUP`); per-role hex
  overrides in `hypxrhud.conf`; font override. *Accept:* `omarchy-theme-set`
  changes HUD colours without restart; conf override wins.
- **WP-H7 `[S/M]` — Packaging.** D-Bus service file + `hypxrhud.service`
  (Type=dbus, bus-activation), `--self-test` live-check mode, README with the
  privacy/lifecycle story. *Accept:* `busctl call dev.hypxr.Hud …` cold-starts
  the daemon via activation; `systemctl --user status hypxrhud` clean.
- **WP-H8 `[M]` — hypxrvoice migration.** Replace `CHudOverlay`'s stdin-pipe
  subprocess with a `dev.hypxr.Hud1` client: `Feedback::emitAction` →
  `CreatePanel`/`UpdatePanel` (per-word transcript = `UpdatePanel` with
  `NO_REPLY_EXPECTED`); `onListeningStart/Stop` → panel create/dismiss;
  notify-send fallback triggered on `RuntimePresent=false` / daemon-absent
  instead of subprocess-degrade. Delete `hud/`, `HudMessage`, and the render core
  from hypxrvoice (now provided by hypxrhud). *Accept:* voice HUD renders via
  hypxrhud identically to wp-v5; daemon absent → notify-send exactly as today;
  hypxrvoice no longer builds any XR/EGL code.
- **WP-H9 `[M/L]` — hypxrkeys as second client (doc `05` WP-K0..).** Screenkey
  key lane + IPC-echo lane become **hypxrhud panels** instead of hypxrkeys owning
  its own session — hypxrkeys keeps only libinput/xkb capture + socket2 read, and
  pushes rendered lane text via `CreatePanel`/`UpdatePanel` (slots `keys`,
  `status`). *Accept:* keys + voice + a toast coexist in one hypxrhud session
  with correct slotting; killing hypxrkeys auto-dismisses its lanes.
- **WP-H10 `[M]` — presence-gated Notifications mirror (mako keeps the name).**
  Step 0: VERIFY BecomeMonitor under dbus-broker (narrow match rule +
  method-return delivery) — everything else gates on this. Then: monitor
  connection correlating Notify replies→ids; donned-edge activation from
  compositor presence (1 Hz status poll, socket2 event as follow-up);
  configurable mako suppression (`makoctl mode -a do-not-disturb`) applied on
  don / removed on doff; toast interaction round-trip via `makoctl invoke/
  dismiss -n <id>`; `own` mode kept as a non-default config for headset-primary
  setups. *Accept:* with headset donned, `notify-send "hi"` shows a headset
  toast and NO mako 2D popup; doffing restores mako popups within one presence
  poll; invoking a toast action fires the app's `ActionInvoked` handler; mako
  history intact across don/doff cycles.

---

## 8 — Risks

### 8.1 D-Bus round-trip latency for live transcript

Target: per-word transcript updates at speech rate (~3–6 words/s). A local
**session-bus method call** on dbus-broker is ~tens of microseconds to low
hundreds of µs round-trip; thousands of calls/sec is routine. At 6 words/s this
is **~4 orders of magnitude** inside budget. **Mitigation baked into the API:**
`UpdatePanel` is marked `NO_REPLY_EXPECTED` (fire-and-forget) → there is *no*
round-trip at all, just a one-way send the daemon drains from its bus fd on the
next poll tick (the same loop that paces frames). The only synchronous call is
`CreatePanel` (needs the returned `id`), which happens once per utterance, not
per word. **Net: latency is a non-issue by construction.** (A ballpark
measurement would require standing up the daemon + a live bus; the design
sidesteps needing it. If desired later, `busctl`-timing a stub vtable is the
cheap confirmation.)

### 8.2 Single point of failure

One daemon, one session — a crash drops every HUD. **Mitigations:** bus-activation
restart; clients keep the notify-send fallback (already present); the daemon is
small, single-threaded, and the XR core is the *already-proven* wp-v5 code.
**Counter-argument (the stronger point):** today each utility that wants a
head-locked panel must independently get the Monado GL-fence contract right
(commit `95c541a8`), manage its own EGL/GBM `--gpu` pin, and consume one of the
64 overlay client slots. Three utilities = three chances to corrupt Monado's
heap, three sessions, three re-probe loops. Consolidating that fragile machinery
into **one audited process** is a **net risk reduction**, not just a convenience.
The SPOF is real but smaller than the status quo's triplicated fragility.

### 8.3 Monado / WiVRn multi-overlay quirks

- **Overlays need the multi-compositor, not a primary.** Verified (§6.3): the
  system compositor thread runs independently; hypxrhud composites with or
  without a primary alive. But it *does* need Monado's **multi** compositor —
  running Monado in **debug mode disables overlays entirely** (Collabora blog;
  Monado getting-started). Our vendored/ WiVRn path uses the multi compositor, so
  fine; a bare `monado-service` debug run is not.
- **Tie-break indeterminism** on equal `z_order` (doc `05`) — keep paper/land/hud
  distinct (INT64_MIN/1/20).
- **Multi-overlay is a less-travelled path.** Web research surfaced active WiVRn
  performance regressions (WiVRn#681, #91) and connection issues (#900) but
  **nothing specific to multiple simultaneous overlay clients** — the feature is
  advertised and the source is clean (64 clients, per doc `05`), but expect it to
  be under-exercised. hypxrhud's consolidation actually *reduces* the number of
  concurrent overlay clients from 3 to 1 (paper primary + land + hud → paper +
  land + hud, but voice/keys/toasts collapse into the single hud), which is
  strictly friendlier to the runtime.
- **BecomeMonitor under dbus-broker** (§3 option b) needs a live confirmation
  before the `mirror` mode is trusted — the reference daemon allows
  session-owner self-monitoring, dbus-broker may filter differently
  (dbus-broker#210). Gate `mirror` behind a runtime capability check + SKIP.

---

## Open questions for the user

1. **Repo:** confirm a new standalone `~/code/hypxrhud` (recommended) vs a module
   inside hypxrvoice. The migration deletes the render core from hypxrvoice — OK?
2. **Bus name:** `dev.hypxr.Hud` vs `org.hypxrland.Hud` vs
   `io.github.andrewgaspar.hypxrhud` — which reverse-DNS do you want to own?
3. **Notifications strategy (revised):** accept **(b′)** — mako owns the name
   permanently; hypxrhud mirrors via BecomeMonitor only while DONNED, suppresses
   mako's 2D popups via `makoctl mode` for the duration, and round-trips
   actions/dismissals through `makoctl invoke/dismiss`? (Per your one-session
   constraint; `own` mode kept as non-default config. dbus-broker monitor
   verification is step 0.)
4. **Suppression scope while donned:** default `do-not-disturb` mode hides ALL
   mako popups in-headset (hypxrhud's toasts replace them). Alternative: leave
   mako visible too (popups appear on the monitor quads) and skip suppression —
   redundant but zero-risk. Default to suppress?
5. **Slots:** are the four default slots (`voice`/`keys`/`toast`/`status`) and
   their poses the right starting set? Any others (e.g. a `media` now-playing
   widget, a `battery`/headset-status pinned corner)?
6. **hyprlang config dependency:** OK to take libhyprlang for
   `hypxrhud.conf` (family-consistent), or keep the tool dependency-minimal with
   a plain key=value parser?
7. **Theme live-reload mechanism:** inotify on the `omarchy/current/theme`
   symlink, or a `Reload()` D-Bus method / `SIGHUP` you wire into
   `omarchy-theme-set` alongside `omarchy-restart-mako`?
8. **Migration sequencing:** land hypxrhud + migrate hypxrvoice (H1–H8) as the
   first milestone and defer hypxrkeys (H9) / Notifications (H10), or bundle
   hypxrkeys in since doc `05` is ready to start?
9. **Per-client panel cap:** default 4 panels/client reasonable, or do you
   foresee a client (a dashboard app) legitimately wanting more?

---

## Sources

**Monado / OpenXR (vendored `subprojects/monado` @ `c2ddab59d`):**
`oxr_system.c:515-533` (maxLayerCount = info->max_layers, else XRT_MAX_LAYERS),
`compositor/render/render_util.c:91-136` (`render_max_layers_capable`, floor 16),
`compositor/main/comp_compositor.c:1161-1164` (max_layers = render cap),
`include/xrt/xrt_limits.h:83-88` (`XRT_MAX_LAYERS` 128 Linux / 32 Android),
`state_trackers/oxr/oxr_api_session.c:188-195` (`XR_ERROR_LAYER_LIMIT_EXCEEDED`
guard), `compositor/multi/comp_multi_system.c:211-227` (`overlay_sort_func`),
`:255-306` (`transfer_layers_locked` — active/visible/session_active gating +
qsort), `:479` (`multi_main_loop` own thread, native-compositor-driven),
`:229-236` (`find_active_blend_mode`), `comp_multi_private.h:41`
(`MULTI_MAX_LAYERS`); + doc `05`'s citations for placement→z_order
(`oxr_session.c:1447-1454`), overlay/primary z + visibility policy
(`ipc_server_process.c:430-461`), `MULTI_MAX_CLIENTS 64`
(`comp_multi_private.h:33`), VIEW-space head-lock
(`oxr_session_frame_end.c:1207-1215`), color-scale-bias default-on
(`monado/CMakeLists.txt:429`, frame-end fill `:206-223,1279`).

**hypxrvoice (read-only, branch `wp-v5` @ `4646249`):**
`src/HudModel.hpp/.cpp` (pure view model), `src/HudText.hpp/.cpp`
(stb_truetype raster), `src/HudMessage.hpp` (stdin wire format — retired),
`src/hud/HudSession.hpp/.cpp` (overlay session: `createSpace` VIEW `:126-131`,
`createSession` overlay chain `:97-124`, `renderFrame` quad + color-scale-bias
`:287-352`, fence-held EGL `init:368`, loss→exit `:272-296`, upload-on-change
`:207-249`), `src/hud/hud_main.cpp` (argv + SIGINT clean exit),
`src/hud/Egl.hpp/.cpp` (GBM/`--gpu`), `src/HudOverlay.hpp/.cpp` (subprocess spawn
+ degrade — the migration seam), `src/Feedback.hpp/.cpp` (`emitAction`/
`emitTranscript`/`onListening*` + notify-send fallback), `src/Config.hpp:60-82`
(feedback fields incl. hudPose/hudZ=20/hudOpacity), `systemd/hypxrvoice.service`
(user-unit template + import-environment note).

**HypXRland (in-tree, branch `hypxrland` @ `0a4e9409`):**
`src/openxr/OpenXRManager.cpp:2083-2140` (dormant re-probe, backoff arm/cancel,
headset-vs-runtime cadence), `src/openxr/XRMonitorConfig.cpp:384-398`
(`xrReprobeBackoffMs` pure fn), `src/config/values/ConfigValues.cpp:713-905`
(`openxr:reprobe`, `reprobe_interval_ms`, `overlay`, `overlay_z` default 1);
doc `05-xr-screenkey.md` (the second overlay client, WP-K0 hook).

**Omarchy / system (this box):**
`~/.local/share/omarchy/default/hypr/autostart.conf:2`
(`exec-once = uwsm-app -- mako`), `~/.config/omarchy/current/theme/mako.ini:1`
(theme include chain), `~/.config/hypr/hyprland-xr.conf:1-11,55` (XR = SDDM
login, sources hyprland.conf, own session bus).

**Web (2026-07):**
- Monado multi-app / `XR_EXTX_overlay` —
  https://www.collabora.com/news-and-blog/news-and-events/monado-multi-application-support-with-xr-extx-overlay.html ,
  https://www.phoronix.com/news/Monado-XR_EXTX_overlay ,
  https://monado.freedesktop.org/getting-started.html (debug mode disables
  overlays).
- WiVRn issue tracker (no multi-overlay-specific bugs; perf/connection only) —
  https://github.com/WiVRn/WiVRn/issues/681 , /91 , /900 .
- D-Bus `BecomeMonitor` / session-bus self-monitoring —
  https://cgit.freedesktop.org/dbus/dbus/patch/doc?id=9fce7380331d24e8dd5fb9203eb8275ebb49e1d8 ,
  https://discourse.gnome.org/t/use-gdbus-to-eavesdrop-notifications-using-becomemonitor/22952 ,
  https://dbus.freedesktop.org/doc/dbus-monitor.1.html ,
  https://github.com/bus1/dbus-broker/issues/210 (broker filtering caveat).
