# PLATFORM, LIFECYCLE & PERFORMANCE — the remainders backlog

**Live planning surface.** A catch-all for the still-pending remnants of three reports whose
*bulk* has already shipped. Each section names exactly what landed vs what's left, so nothing
falls through the cracks. Deep detail is in the archived originals.

Sources whose remainders live here:
- [`archive/17-late-runtime-lifecycle.md`](archive/17-late-runtime-lifecycle.md) — mostly shipped; L4 (session-loss reconnect) + L5 (wivrn autostart) remain.
- [`archive/19-zero-copy-game-path.md`](archive/19-zero-copy-game-path.md) — the linear-alloc prerequisite shipped; the direct-scanout Z-series remains.
- [`archive/01-vr-app-composition.md`](archive/01-vr-app-composition.md) — Monado/WiVRn overlay shipped; the SteamVR OpenVR backend remains.

---

## Capability vision

Keep the XR session robust across a messy real-world lifecycle (runtime absent at login,
runtime/session lost mid-use, wrong GPU) and cheap enough to run a fullscreen game in-headset
without a cross-GPU round trip — and, eventually, composite our monitors over a SteamVR game.

---

## 1. Late/absent-runtime lifecycle (report 17) — MOSTLY SHIPPED

### Shipped
- **L1 — main-thread GL context save/restore** (`ef4e0921`, "fail closed on unusable
  openxr:gpu (+ EGL context save/restore)").
- **L2 — dmabuf modifier attribs on EGL import** (`b93279dd`, cross-GPU black-screen fix).
  This is also the hard prerequisite for the zero-copy game path below.
- **L3 — dormant self-healing reprobe:** `XR_STATE_UNAVAILABLE` is dormant not terminal; a
  `CEventLoopTimer` re-attempts `start()` while `openxr:enabled` + `openxr:reprobe` (default
  on). Two-phase probe (instance → `xrGetSystem`) with `waiting: runtime` (growing backoff,
  base `openxr:reprobe_interval_ms`, cap 30s) vs `waiting: headset` sub-states (`d2ba3026`,
  `f220ec65`). Config: `openxr:reprobe`, `openxr:reprobe_interval_ms`.
- **L6 (partial) — observability:** `hyprctl openxr status` surfaces the reprobe hint and
  the raw `visible:`/`presence:` signals; log-once for import failures.
- **L7 — reload:** `onConfigReload()` now starts from `UNAVAILABLE` as well as `DISABLED`, so
  `keyword openxr:enabled 1` retries instead of silently no-oping.

### Remaining
- **L4 — session-loss → auto-reconnect (regression test only).** *Status nuance:* the
  reconnect **mechanism appears functionally shipped** — a lost session lands in `UNAVAILABLE`
  (`OpenXRManager.cpp:529`, `setState(lost ? XR_STATE_UNAVAILABLE : XR_STATE_DISABLED)`), and
  the reprobe timer explicitly rearms from there (comment at `OpenXRManager.cpp:1975-1977`:
  "Session loss / a failed start both land here (UNAVAILABLE)"; `ConfigValues.cpp:715` says
  reprobe "Also drives auto-reconnect after a session/runtime loss"). What is genuinely
  missing is the **dedicated `xr_session_loss` hyprtester case** (kill monado-service /
  wivrn-server mid-session, assert clean teardown → unaided reconnect) and a deliberate audit
  that the `LOSS_PENDING`/`INSTANCE_LOST` teardown routes cleanly (monitors per
  `destroy_monitors_on_stop`/report-18 policy, no crash). Effort: S. Treat as verify + harden,
  not build-from-scratch.
- **L5 — on-demand runtime autostart.** `openxr:autostart_runtime = off|wivrn` (string,
  default off): once per dormancy episode, `systemctl --user start wivrn.service` on a
  runtime-absent probe result; systemctl failure degrades to plain reprobe. Not implemented
  (no `openxr:autostart_runtime` var exists). Effort: S. Turns "login with enabled=1, runtime
  down" into a fully hands-free session bring-up when combined with the shipped L3 reprobe.
- **L6 completion (optional):** per-layer content-path state (`ok/cpu/black`) in status +
  transition event; finish rate-limiting all per-frame XR WARN/ERR paths.

---

## 2. Zero-copy game path (report 19) — PREREQUISITE SHIPPED

The user's workload: a flat game fullscreen on an XR monitor in-headset, on a hybrid laptop
where a PRIME-offloaded NVIDIA game's frames make a NVIDIA→AMD→NVIDIA round trip before the
encoder. Two stacking mechanisms kill the round trip: an **XR direct-scanout fast path** (hand
the client's own dmabuf to our blit) and **per-XR-monitor GPU affinity** (born on NVIDIA).

### Shipped (prerequisite)
- **WP-L2 (from report 17) — dmabuf modifier attribs on import** (`b93279dd`), plus the
  actual **linear-swapchain allocation for cross-GPU monitors** (`349da50e`, `f12b946a`).
  This is the hard prerequisite for any NVIDIA import; the direct-scanout path is now
  unblocked but not built.

### Remaining — the direct-scanout Z-series
| WP | dep | what | effort |
|----|-----|------|--------|
| Z1 | — | Confirm headless `commit()`/`test()` accept an arbitrary client dmabuf and re-present it verbatim (`presented`); decide the Z-A vs Z-B approach | S |
| Z2 | Z1 | Per-XR-monitor DS enable: a `CMonitor` flag + `isDSBlocked` branch treating a flagged headless output as `direct_scanout=enable` without touching the global; `xrmonitor ... direct:on` + `openxr:direct_scanout` master (default on) | M |
| Z3 | Z2, L2 | Blit consumes the DS'd client buffer natively; verify content-rect remap, alpha-pin, click pixel-exactness unchanged | S |
| Z4 | Z2 | Fallback dance: break solitary (popup/notification/un-fullscreen) → DS leaves → composite resumes → re-fullscreen re-engages; ≤1-frame seam, no crash | S |
| Z5 | Z3 | Synchronization: ensure the client buffer render completes before our EGLImage read (explicit acquire wait if implicit fencing is insufficient cross-vendor) | S–M |
| Z6 | — | GPU affinity baseline: allocate the XR output swapchain on `openxr:gpu` so the runtime-facing buffer is NVIDIA-native for the windowed case; keep AQ_DRM_DEVICES iGPU-first | M–L |
| Z7 | Z6 | Measurement + guardrails: per-layer "content-path: direct/composited/affinity" in status; document the AQ_DRM_DEVICES reorder hazard | S |

Ordering: Z1 → Z2 → (Z3 ∥ Z4) → Z5; then Z6 → Z7 only if measurement justifies affinity.
**Z2-Z4 is the shippable game-path win; Z6 is optional.** (Report 19's WP-Z* namespace — distinct
from INTERACTION's gaze WP-Z* and report 03's grid WP-G*.)

---

## 3. Compositing over other VR apps (report 01) — OVERLAY SHIPPED

### Shipped
- **Monado/WiVRn overlay session** (`619c9c66`, "support overlay sessions for compositing
  over other XR clients"): `openxr:overlay` + `openxr:overlay_z` chain
  `XrSessionCreateInfoOverlayEXTX` into `xrCreateSession`. Monado marks overlay sessions
  always visible AND focused, so our FOCUSED-gated input keeps working while a game runs
  underneath. Works on Monado local and WiVRn+Quest.

### Remaining — the SteamVR OpenVR `IVROverlay` backend (L/XL)
- SteamVR-Linux does **not** support `XR_EXTX_overlay`; desktop-in-SteamVR requires a separate
  OpenVR `IVROverlay` backend (the wlx-overlay-s dual-backend model): one `IVROverlay` handle
  per XR monitor, an OpenVR-speaking sibling to `CXRSession` + frame loop, and a per-backend
  lifecycle abstraction (`src/openxr/backends/` with an OpenXR path and an OpenVR path).
- **Architecturally a good fit** — our per-monitor quads/anchors map onto overlays, and the
  input story is *better* (SteamVR routes controller ray-hit + focus arbitration for us via
  `VREvent_Mouse*`/`ComputeOverlayIntersection`). But it is a **second runtime API to
  maintain** and a large, separately-scoped project. Not a near-term item; parked here so it
  isn't lost. The main real design problem (even for the shipped overlay path) is **input
  arbitration** — game vs desktop focus toggle — not session plumbing.

---

## Suggested sequencing across sections
1. **L5 autostart** (S) + **L4 regression test** (S) — small, close out the lifecycle story so
   fishfood login is fully hands-free and loss-resilient with test coverage.
2. **Z1→Z4 direct-scanout** — the concrete performance win the user actually hits (fullscreen
   game in-headset); L2 is already shipped so it's unblocked.
3. **Z6-Z7 affinity** — only if measurement justifies it.
4. **OpenVR backend** — a standalone future project, revisit if SteamVR-on-Linux
   desktop-over-game becomes a priority.
