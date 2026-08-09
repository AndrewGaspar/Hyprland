# HypXRland OpenXR research

This directory holds design/research memos for the OpenXR extension. It is organised into
**live planning docs** (the current triage surface) and an **`archive/`** of the original
per-investigation reports (kept verbatim for deep detail and provenance).

If you are looking for "what should we build next," read the live planning docs. If you are
looking for "why did we decide X" or the full evidence behind a shipped feature, read the
archived original.

---

## Live planning docs

Central, coalesced surfaces. Each carries a capability vision, the design decisions that
survive from its source reports, a merged work-package backlog, and pointers back to the
archived originals.

| Doc | Covers | Coalesces |
|-----|--------|-----------|
| [INTERACTION.md](INTERACTION.md) | aiming, direct manipulation, gaze grabs | archive 14, 15, 16 (builds on shipped 04) |
| [LAYOUT-AND-NAMING.md](LAYOUT-AND-NAMING.md) | grids, auto-layout, 2D-plane sync, auto-names | archive 03, 08, 11, 12 |
| [VISUALS.md](VISUALS.md) | premium chrome, transparency, view-bounding | archive 07, 09, 10 |
| [PLATFORM-LIFECYCLE-PERFORMANCE.md](PLATFORM-LIFECYCLE-PERFORMANCE.md) | lifecycle & perf remainders, SteamVR backend | archive 17 (L4/L5), 19 (direct-scanout), 01 (OpenVR) |
| [05-xr-screenkey.md](05-xr-screenkey.md) | head-locked keystroke/command display (`hypxrkeys`) | standalone — unimplemented, does not cluster |
| [VOICE-CONTROL.md](VOICE-CONTROL.md) | cascaded voice control of XR monitors + app launch (`hypxrvoice`) | standalone — unimplemented, does not cluster |

`05-xr-screenkey.md` is a standalone design for a new companion tool (a sibling to
`hypxrpaper`). Nothing is implemented; it is kept live in place because it does not overlap
the four clusters above. Its WPs are K1–K12.

`VOICE-CONTROL.md` is a standalone design for a new companion daemon `hypxrvoice` (another
`hypxrpaper`/`hypxrkeys` sibling): natural-language voice control over the existing
`hyprctl openxr`/dispatcher IPC, with a four-tier cascaded activation architecture
(contextual/presence gate → local wake word → local ASR + intent gate → local-or-cloud
reasoning) to keep the mic closed and the cloud bill near zero. Nothing is implemented. Its
WPs are V1–V12.

---

## Standalone numbered reports (20+)

Post-reorg investigations. Each is self-contained: ground truth with `file:line`, options with
honest sizing, a recommendation, and a WP ladder. They are *not* coalesced into the live docs
above; when one ships, move it to `archive/` with a disposition line like the table below.

| Report | Covers | Status |
|--------|--------|--------|
| [20-wivrn-idle-inhibit](20-wivrn-idle-inhibit.md) | idle/sleep-inhibition policy for WiVRn sessions | **shipped** — `openxr:inhibit_idle` is now `off\|focused\|present` (`e1a99138`) |
| [21-wivrn-variable-bitrate](21-wivrn-variable-bitrate.md) | variable-rate encoding for WiVRn streaming (+ erratum `e66b91b4`) | unimplemented |
| [22-spatial-persistence-locations](22-spatial-persistence-locations.md) | spatial persistence and named locations | unimplemented |
| [23-xr-native-launcher](23-xr-native-launcher.md) | head-leashed presentation of transient layer-shell UI (walker, mako, OSDs) | unimplemented — WPs N0–N10 |
| [24-stereo-content-and-depth-desktop](24-stereo-content-and-depth-desktop.md) | a flat side-by-side **stereo output** as a generic Hyprland feature (one monitor, logical = one pane, mode = the pack), stereoscopic client content, and "depth" as a first-class window/layer styling axis | unimplemented — WPs F0–F5, S0–S8, D0–D7, X1–X4 (rev. 2026-08-08: flat SBS is the **primary** presenter, OpenXR the upgrade tier) |

---

## Archive — original reports and their disposition

`archive/` holds the 18 numbered reports that have either **shipped** or been **coalesced**
into a live doc above. One line each: what happened, commit refs where easy, and what
supersedes it.

### Shipped (content has landed in the product)

| Report | Status | Evidence / supersedes |
|--------|--------|-----------------------|
| [04-grabbable-borders](archive/04-grabbable-borders.md) | **SHIPPED in full** | WP-G1…G6: grabbable chrome (bar/corners), per-hand pinch/grasp grabs, `SXRGrabRing` release latch, 1€ carry filter. Live-tuned defaults `ba584867`; hand pinch `4f6bb2e7`; 1€ filter `8ac5e007`. Config: `openxr:chrome_*`, `hand_grab*`, `grab_*`. No remainder. |
| [06-podman-isolation](archive/06-podman-isolation.md) | **SHIPPED** | `containers/` tree + `scripts/xr-container.sh`: base image `fcf92059`, hermetic suite `e727fbb4`, Omarchy session `8e0488a9`, GPU/CDI + docs `539fe2d7`, split-GPU WiVRn `124e5413`. |
| [13-adaptive-anchoring](archive/13-adaptive-anchoring.md) | **SHIPPED** | dock↔follow decorator on `anchor:local`, XZ geofence + dwell + eased pose blend. `6ced4f9f`. Config: `openxr:adaptive_*`; `xrmonitor adaptive/dock/undock` verbs. |
| [18-monitor-plugged-state](archive/18-monitor-plugged-state.md) | **SHIPPED, then EVOLVED** | Shipped as option (b) (`f1a79d5d`), then the gate moved from session *existence* to **visibility + user-presence**: `9fa7de00` (visibility), `2de0e9b7` (presence), `d2ba3026` (phantom-plug/doff/reprobe fixes). `openxr:monitors_follow_session` is now a mode `off\|session\|visible` (default `visible`) + `monitor_unplug_grace_ms`/`monitor_plug_settle_ms`. |
| [02-3d-environments](archive/02-3d-environments.md) | **SHIPPED as a separate project** | Ambient backgrounds became `hypxrpaper` (vendored `subprojects/hypxrpaper`): equirect2 panorama + glTF scene modes. Hyprland-side integration `e3fb0064` (`619c9c66` overlay path it composites through). The compositor deliberately ships no environment renderer (the doc's recommended pluggable split). |
| [01-vr-app-composition](archive/01-vr-app-composition.md) | **PARTIAL — overlay shipped** | Monado/WiVRn overlay session `619c9c66` (`openxr:overlay`, `openxr:overlay_z`). **Remaining:** the SteamVR OpenVR `IVROverlay` backend (L/XL) → tracked in [PLATFORM-LIFECYCLE-PERFORMANCE.md](PLATFORM-LIFECYCLE-PERFORMANCE.md) §3. |
| [17-late-runtime-lifecycle](archive/17-late-runtime-lifecycle.md) | **MOSTLY SHIPPED** | L1 context restore `ef4e0921`, L2 dmabuf modifiers `b93279dd`, L3 dormant reprobe `d2ba3026`/`f220ec65`, L6 (partial) observability, L7 reload-from-UNAVAILABLE. **Remaining:** L4 session-loss reconnect (mechanism appears wired — loss lands UNAVAILABLE `OpenXRManager.cpp:529` → reprobe; needs a regression test), L5 `wivrn` autostart → [PLATFORM-LIFECYCLE-PERFORMANCE.md](PLATFORM-LIFECYCLE-PERFORMANCE.md) §1. |
| [19-zero-copy-game-path](archive/19-zero-copy-game-path.md) | **PREREQUISITE SHIPPED** | The linear cross-GPU allocation + dmabuf modifier import prereq shipped (`349da50e`, `f12b946a`, `b93279dd`). **Remaining:** the direct-scanout Z-series (Z1–Z7) → [PLATFORM-LIFECYCLE-PERFORMANCE.md](PLATFORM-LIFECYCLE-PERFORMANCE.md) §2. |

### Coalesced (pending design; live surface is now a central doc)

| Report | Now planned in |
|--------|----------------|
| [03-monitor-grids](archive/03-monitor-grids.md) | [LAYOUT-AND-NAMING.md](LAYOUT-AND-NAMING.md) |
| [08-auto-layout](archive/08-auto-layout.md) | [LAYOUT-AND-NAMING.md](LAYOUT-AND-NAMING.md) |
| [11-dynamic-monitor-naming](archive/11-dynamic-monitor-naming.md) | [LAYOUT-AND-NAMING.md](LAYOUT-AND-NAMING.md) |
| [12-spatial-2d-layout](archive/12-spatial-2d-layout.md) | [LAYOUT-AND-NAMING.md](LAYOUT-AND-NAMING.md) |
| [07-premium-chrome](archive/07-premium-chrome.md) | [VISUALS.md](VISUALS.md) |
| [09-monitor-transparency](archive/09-monitor-transparency.md) | [VISUALS.md](VISUALS.md) |
| [10-view-bounding](archive/10-view-bounding.md) | [VISUALS.md](VISUALS.md) |
| [14-ray-aim-assist](archive/14-ray-aim-assist.md) | [INTERACTION.md](INTERACTION.md) |
| [15-direct-manipulation](archive/15-direct-manipulation.md) | [INTERACTION.md](INTERACTION.md) |
| [16-gaze-grab](archive/16-gaze-grab.md) | [INTERACTION.md](INTERACTION.md) |

---

## Notes

- Reports are numbered in creation order; the numbers are historical, not a priority.
- The archived originals are unmodified copies (moved via `git mv`) — no content was edited in
  the reorg, so every claim, citation, and open-question list remains reachable.
- The top-level `docs/openxr/*.md` pages are the authoritative shipped-feature docs; a couple
  of them still link to `research/01-vr-app-composition.md` at its old path (it now lives under
  `archive/`). Those links are maintained by the top-level docs, not here.
