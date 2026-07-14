# Multi-Machine XR — working two computers through one headset

**Status:** research / decision-support. Nothing implemented. This memo explores the design
space for using **both** of the user's laptops at the same time through **one** Quest 3 (via
WiVRn), without re-donning or re-pairing when he switches machines — ideally a single unified XR
workspace where monitors from both computers float side by side.

It follows the house style of `docs/openxr/research/`: every approach gets an honest
pro/con with latency / bandwidth / input / effort, a comparison table, a recommendation that
separates the *cheap win now* from the *ambitious end-state*, sized WP sketches, and open
questions. Evidence is cited to code paths (HypXRland `docs/openxr/00–07`, the read-only WiVRn
tree at `~/code/wivrn`) and to external sources with URLs.

---

## 1. The goal, restated

- **Two machines.** This AMD+NVIDIA Framework desktop-replacement (runs HypXRland + WiVRn today)
  and the Intel Lunar Lake laptop (also fishfood-provisioned for HypXRland). One Quest 3, WiVRn.
- **What he wants (best case):** monitors from *both* machines visible and usable in *one*
  headset session at the same time — a unified XR desktop spanning two computers.
- **What he wants (minimum):** instant switching between the two, with **no re-don and no
  re-pair** — glance/gesture/hotkey and the other machine is in front of him.
- **Where we are today:** HypXRland renders **local** Hyprland outputs as XR quads
  (`docs/openxr/02-virtual-monitors.md`). WiVRn streams **one** compositor's session to the
  headset — the headset client connects to exactly one server at a time. Using the other machine
  means stopping WiVRn here and connecting the headset to the other machine's WiVRn. That is the
  friction we are trying to kill.

The single most important architectural fact for everything below:

> **An XR monitor is just an Aquamarine *headless* `CMonitor`.** HypXRland takes whatever dmabuf
> that output presents and blits it into an `XrCompositionLayerQuad` with a pose, chrome, grab,
> anchoring, mirroring, and naming — all for free (`docs/openxr/02-virtual-monitors.md`,
> "An **XR monitor** is an ordinary Aquamarine **headless output**"). **Anything that can make
> machine B's content appear as a `CMonitor` on machine A automatically becomes a first-class XR
> quad** with zero new XR code. This collapses most of the "unified workspace" problem into a
> much older, better-understood problem: *get a remote desktop onto a local output.*

The second important fact is about the network (§8): the user's Wi-Fi is **half-duplex and
already contended** — a single WiVRn video downlink (default **50 Mbps**,
`~/code/wivrn/client/configuration.h:65`) already starves the mic uplink (observed this session).
Any design that puts **two** video streams on the headset's wireless leg is fighting physics.

---

## 2. Map of the design space

Seven families, grouped by *who owns the headset* and *where the second machine's pixels get
composited*:

| # | Family | One-liner | Headset sees | New XR code? |
|---|--------|-----------|--------------|--------------|
| A | **Single XR host + remote content** | One machine owns the headset; the other enters as a remote desktop/app composited into an XR quad | **one** stream (host's) | none → small |
| B | **Headset-side fast switch** | Client holds N known servers, flips between them by gesture/hotkey, no re-pair | one stream at a time | client-only |
| C | **Two servers → one client, simultaneous** | Modified WiVRn client composites two live streams at once | two streams | large, deep |
| D | **Aggregator / network monitor source** | A headless compositor owns the headset; both machines submit their monitor-quads to it over the network | one stream (aggregator's) | medium, native |
| E | **Input-led single seat** | One machine owns the headset; a shared cursor (lan-mouse) crosses into the other machine's monitors | depends on pairing with A/D | none (input only) |
| F | **Network reality** | Wired vs wireless topology; per-approach bandwidth budgets | — | — |
| G | **Prior art** | How Immersed / Virtual Desktop / SteamVR / Workrooms do it | — | — |

Families A, B, D, E are *complementary*, not exclusive — the recommendation (§10) stacks them.
C is the seductive-but-wrong one. F and G inform all of them.

---

## 3. Family A — single XR host + remote content (the pragmatic core)

**Model.** Machine A stays the sole XR host: it runs WiVRn + HypXRland, owns the Quest, and emits
the *only* stream the headset ever sees. Machine B's desktop is pulled *into* A as remote content
and composited into an XR quad exactly like a local monitor. The headset never knows B exists.

This is attractive because it inherits the whole XR stack (anchoring, grab-chrome, mirroring,
naming, plug-lifecycle) and keeps the headset on **one** wireless stream. The only real question
is *which transport* carries B's pixels — and, critically, its *input back-channel* (see the
input note at the end of this section). Four candidate transports:

### 3.1 waypipe — B's apps on A's Hyprland (network-transparent Wayland)

Run individual Wayland apps *from* B but displayed *on* A's compositor. `waypipe ssh
user@machineB app` starts a waypipe server on B and client on A; the app's surfaces appear as
native windows in A's Hyprland, land on A's workspaces, and therefore on A's XR quads — **zero XR
code**. Compression is selectable (`-c none|lz4|zstd`, `zstd=14` for slow links)
([waypipe(1)](https://man.archlinux.org/man/extra/waypipe/waypipe.1.en),
[mstoeckl.com notes](https://mstoeckl.com/notes/gsoc/blog.html)).

- **Latency:** best of the four for interactive apps — no video re-encode of B's desktop; Wayland
  buffers shuttle over the LAN. Text stays pixel-crisp (it's real surfaces, not a video codec).
- **Bandwidth:** variable and can be *heavy* — GPU/SHM buffer contents cross the network per
  damage; a full-screen video playing in a waypiped app is worst-case. zstd helps text/UI, not
  video.
- **Input:** native. Wayland input events flow back to B through the same pipe; A's XR ray →
  `motionAbsolute` → the app's surface → B, transparently. **This is the big win** — the input
  back-channel is the protocol itself.
- **Cons:** *per-app, not per-desktop* — you get B's apps, not B's Hyprland/waybar/WM. No B-side
  window management, workspaces, or layer-shell. xdg-portal/clipboard/drag quirks. Some apps
  break; "not for untrusted servers." Two GPUs' buffer formats/modifiers must be interchangeable
  over the wire.
- **Effort:** ~zero (it's an existing tool); the work is ergonomics (launchers, per-app scripts).
- **Verdict:** the **highest-fidelity** way to bring *specific B apps* (an editor, a terminal, a
  browser) into the XR space. Not a way to bring B's *whole desktop*.

### 3.2 Remote-desktop stream on a quad — Moonlight/Sunshine, wayvnc, wlx (whole B desktop)

B runs a desktop-capture streamer (Sunshine, wayvnc); A runs the client (Moonlight, a VNC viewer)
**fullscreen on a dedicated XR monitor**. B's entire desktop becomes one quad. **Zero XR code** —
it's just a fullscreen app pinned to an `XR-*` workspace.

- **Latency:** good. Moonlight/Sunshine host-processing ~6 ms at high FPS; Wayland adds ~1 frame
  vs Xorg ([moonlight-qt #1032](https://github.com/moonlight-stream/moonlight-qt/issues/1032),
  [farnoy.dev latency](https://farnoy.dev/posts/linux-latency)). But note the **double hop** on
  the host path: B encodes → A decodes → A *re-encodes* into the WiVRn headset stream. Two codec
  round-trips stack.
- **Bandwidth:** B→A is a full desktop video stream. **If B→A runs over Ethernet (§8) this is
  free of the headset's wireless budget** — the headset still sees one stream. This is the key to
  making it viable.
- **Input:** native to the protocol — Moonlight/VNC carry mouse+keyboard back to B. A's XR ray
  drives the fullscreen client window, which forwards to B. Works today.
- **Cons:** B's desktop is a *video* — crisp text needs high bitrate; the double-encode adds
  latency and a second quality-loss stage; HDR/scaling mismatches. wayvnc/VNC is lower-fidelity
  but lighter; Sunshine/Moonlight is higher-fidelity/heavier.
- **Effort:** ~zero to try **today**. This is the fastest path to "B's whole screen is a floating
  monitor next to A's."
- **Verdict:** the **cheapest whole-desktop win**, viable *if B→A is wired*.

### 3.3 PipeWire-over-network capture of B's outputs

Capture B's outputs via the ScreenCast portal + `pipewiresrc`, ship over the LAN (gstreamer
webrtc/udp), decode into a surface/output on A.

- Roc (`pipewire-roc`) is **audio-only**; video-over-network is DIY gstreamer with no built-in
  input channel ([Arch PipeWire](https://wiki.archlinux.org/title/PipeWire),
  [GStreamer WebRTC](https://discourse.gstreamer.org/t/send-screencapture-data-from-pipewire-to-webrtc/5426)).
- **No input back-channel** — you'd bolt on lan-mouse (§7) separately.
- **Verdict:** strictly worse than 3.2 for this use case (reinvents Moonlight without the input
  channel). Interesting only if you want *audio* routed from B, where roc shines.

### 3.4 The unifying trick: decode-into-a-headless-output daemon

3.2's "fullscreen client on an XR workspace" is the crude version. The clean version is a small
companion daemon on A (a `hypxrpaper` sibling) that **creates a headless `CMonitor` per remote
output** and feeds it B's decoded frames directly, so B's monitors appear as *named XR monitors*
(`XR-B-1`, `XR-B-2`) rather than as one app window. This is where Family A meets Family D (§5) —
same idea, described from A's side. Covered under the recommendation.

**Input, in general, for Family A:** HypXRland's XR pointer already resolves the ray-hit monitor
and sets `m_boundOutput` to it before emitting `motionAbsolute`
(`docs/openxr/04-input.md:481`, ":486 "Setting `m_boundOutput`… is the load-bearing detail""). So
pointing at *any* quad — local or remote-backed — routes input to that output's surface on A. The
**only** new requirement is that whatever transport backs the remote quad **forwards that input to
B**. waypipe and Moonlight/VNC do this natively; a raw PipeWire capture does not. **Choose a
transport that carries input back to B, and A's XR ray → B's cursor "just works."** That single
observation removes input as a hard problem for the whole family.

---

## 4. Family B — headset-side fast switching (the cheap "instant switch")

If "both at once" is too much, "instant switch with no re-pair" is *nearly free today*, because
the WiVRn client already manages **N known servers with persisted credentials**:

- The client config holds `std::map<std::string, server_data>`; each `server_data` carries
  `autoconnect`, `manual`, `visible`, `compatible`, and the discovered `service` (with its
  pairing `pin`/cookie) — `~/code/wivrn/client/configuration.h:50–60`,
  `~/code/wivrn/client/wivrn_discover.h`.
- The lobby has an explicit **server list** with add-server, per-server `autoconnect`, and
  connect/disconnect flow (`~/code/wivrn/client/scenes/lobby.h:52–129, 235–236`).
- Discovery is Avahi/DNS-SD (`_wivrn._tcp`, `~/code/wivrn/server/avahi_publisher.cpp`,
  `~/code/wivrn/client/wivrn_discover.cpp`), so both laptops running `wivrn-server` show up
  automatically; manual IP works when Avahi doesn't.

**The consequence:** once both machines are paired, switching is "disconnect from A, pick B in the
lobby" — **no re-pairing**, because the cookie is stored per server. The gaps to a *good* instant
switch are purely UX and speed:

- **No re-don needed** already — you stay in the headset; you drop to the WiVRn lobby and pick.
  But it's a menu interaction, not a hotkey/gesture.
- **Not sub-second.** Each switch tears down one compositor session and brings up another
  (WiVRn/Monado session begin, HypXRland `XR_STATE_STARTING`→visible, monitor re-plug settle
  `openxr:monitor_plug_settle_ms` default 1500 ms — `docs/openxr/02-virtual-monitors.md`). Realistic
  switch is a few seconds, not instant.
- **The user already builds the client APK** (the "boundaryless" WiVRn client), so a client patch
  is in scope: a bound controller gesture / voice verb that calls the existing
  `connect(server_data)` for a pinned "other machine," skipping the lobby GUI. That turns a menu
  dance into a one-gesture flip — still a reconnect, but hands-free.
- **Known reconnect sharp edge:** WiVRn has historically had connect/disconnect races
  ([WiVRn #206](https://github.com/WiVRn/WiVRn/issues/206)) — a fast-switch patch must exercise
  the teardown/bring-up path carefully. This dovetails with HypXRland's own **L4 session-loss
  reconnect** hardening (`PLATFORM-LIFECYCLE-PERFORMANCE.md §1`), which already lands a lost
  session in `UNAVAILABLE` + reprobe.

**Verdict:** Family B is the **cheapest "switch" win** and is *mostly a UX/APK patch on tech that
already exists*. It does **not** give simultaneity, and it can't beat ~seconds per switch because a
full compositor session is recycled.

---

## 5. Family D — aggregator / network monitor source (the most HypXRland-native)

This is the ambitious end-state and the idea that fits HypXRland's grain best. Restating the core
fact: the compositor already composites **N quads, each = {dmabuf content + pose}**
(`docs/openxr/01-session-graphics.md`, frame-thread loop; `02-virtual-monitors.md`). **Make some
of those quads *remote-sourced*.**

Two ways to realize it, both landing at the same visual result — B's monitors as named XR quads
next to A's:

### 5.1 Realization D-lite: exporter on B + decode-into-headless-output on A

- **On B:** a lightweight "XR monitor exporter" — for each Hyprland output, capture (screencopy /
  ext-image-copy) + hardware-encode (VAAPI on Lunar Lake) and ship the stream to A. B does *not*
  run WiVRn or own any headset; it just publishes its outputs. It can even publish *headless* XR
  outputs it creates for the purpose (so B contributes monitors that don't physically exist on B).
- **On A:** the companion daemon from §3.4 creates one headless `CMonitor` per remote output
  (`XR-B-1`…), decodes B's stream into each output's presented buffer, and lets the **existing**
  XR machinery turn them into quads — anchoring, grab-chrome, mirroring, cap policy, naming, all
  free. Input flows A's XR ray → `motionAbsolute` on `XR-B-1` → forwarded over the exporter's
  input back-channel to B.
- **This needs essentially no changes to the XR frame loop** — the novelty is entirely in the
  companion daemon (userspace) plus a thin "feed a headless output an external buffer" path. It's
  the most *incremental* route to a true unified two-machine workspace, and it reuses the
  direct-scanout groundwork (`PLATFORM-LIFECYCLE-PERFORMANCE.md §2`, Z-series) for the
  buffer-injection question.

### 5.2 Realization D-full: a network quad-source protocol into the compositor

A first-class protocol where a remote machine registers a "network monitor source" and streams
`{encoded frame, damage, mode}` that the compositor imports directly as a layer's content, plus a
reverse input channel. More elegant, more invasive (new protocol, security, versioning, a new
buffer path inside `CXRMonitorLayer`). Only worth it if D-lite's per-output-decoder-daemon proves
too heavy. **Recommend D-lite first; D-full is a later optimization, not a starting point.**

**Honest cons of Family D:**
- **Double-encode on the video path** (B encodes → A decodes → A re-encodes into WiVRn) unless
  D-full + WiVRn cooperate to pass B's already-encoded stream through — a deep, cross-project
  change. For v1, accept the double hop; keep B→A wired (§8) so only CPU/GPU cost, not headset
  bandwidth, is spent.
- **The exporter is new software** on B (though small, and analogous to Sunshine).
- **Clock/pose ownership stays clean** — A owns the single OpenXR session and all poses; B is a
  pure pixel/inputs source. This is *why* D is sane and C (§6) is not.

**Verdict:** the **most native and most ambitious** direction, and the correct end-state for a
genuine unified workspace. D-lite is a medium effort concentrated in a userspace daemon, not in
the load-bearing XR frame thread — which is exactly where you want new risk to live.

---

## 6. Family C — two servers composited on the headset simultaneously (assess honestly: don't)

Could a modified WiVRn client take **two** live server streams and composite both into one headset
view? On paper it's "the most simultaneous." In practice it fights the platform:

- **One runtime owns the display.** The headset runs a single OpenXR runtime (WiVRn/Monado's
  client compositor) that owns `xrWaitFrame`/pose prediction/reprojection/the display swapchain.
  Two servers means two independent frame clocks and two pose-ownership claimers feeding one
  reprojector — you'd have to demote one server to a *layer source* with no timing authority,
  which is… exactly Family D, but done on the bandwidth-starved headset instead of on wired A.
- **Two video decoders + two wireless downlinks on the headset.** Doubles the wireless budget the
  user already can't afford (§8), on the weakest device.
- **No platform precedent.** SteamVR explicitly **cannot** drive one headset from two PCs at once
  ([Steam discussion](https://steamcommunity.com/app/250820/discussions/0/4036979713106043529/));
  Virtual Desktop multi-monitor is **single-PC only**
  ([UploadVR](https://www.uploadvr.com/virtual-desktop-multiple-monitors-update/)). The only
  shipping product that does simultaneous multi-PC (Immersed, §9) does it with a **native headset
  app that composites, plus one lightweight agent per PC** — i.e. the aggregator pattern, not
  two-runtimes-on-headset.

**Verdict:** technically possible, strategically wrong. Every hard part of C is better solved by
moving the compositing off the headset (Family D) where there's a wire and a real GPU. **Do not
pursue C.**

---

## 7. Family E — input-led single seat (a cheap complement, not a solution)

Synergy/Barrier/**lan-mouse**/input-leap-style: one keyboard+mouse (here, A's XR ray) whose cursor
*crosses* into the second machine's screens. On Wayland this specifically means **lan-mouse**
(feschber/lan-mouse) — **input-leap is broken/crashy on Wayland**
([KDE discuss](https://discuss.kde.org/t/feature-seamless-keyboard-and-mouse-sharing-integrate-input-leap/29512),
[lan-mouse](https://github.com/feschber/lan-mouse)).

- **On its own it does not put B's pixels in the headset** — it only moves input. So E is never a
  standalone answer to "see both machines." It's the **input glue** for a design where B's pixels
  arrive by some *other* path.
- But note §3's finding: the good transports (waypipe, Moonlight/VNC) **already carry input**. So
  E is only needed when B's pixels arrive over a transport *without* an input channel (e.g. raw
  PipeWire capture, or D-full before its reverse channel exists). In the recommended designs, E is
  redundant.
- **Where E genuinely helps:** as a *stopgap today* before any XR work — lan-mouse lets A's real
  keyboard/mouse drive B while B's screen shows on a Moonlight quad, even if the XR-ray→B path
  isn't wired yet. Sub-20 ms input over Ethernet.

**Verdict:** useful glue and a today-stopgap, not a destination.

---

## 8. Family F — the network reality (this decides everything)

The user's Wi-Fi is half-duplex and already saturated: a single WiVRn downlink (**default 50 Mbps**,
`~/code/wivrn/client/configuration.h:65`; users push 100–150 Mbps for quality) already induces mic
uplink loss (observed this session). Consequences that *rank the whole design space*:

1. **The headset must remain a single wireless stream.** Any design that adds a second video
   stream *to the headset* (Family C, or Family A/D done wirelessly B→headset) is out. This is the
   strongest single constraint and it eliminates C outright.
2. **Wire the machine-to-machine leg.** If B→A goes over **Ethernet** (both laptops on a switch;
   the Quest is the *only* wireless device), then B's desktop stream — however heavy — **never
   touches the contended wireless budget.** A composites B into the one WiVRn stream the Quest
   already receives. This is the decisive argument for Family A/D over C, and for *wired B→A* over
   any wireless B path.
3. **Budget sketch (order-of-magnitude):**
   - Quest wireless downlink: ~50–100 Mbps (WiVRn) — **unchanged** in Families A/D. This is the
     scarce resource; keep it a single stream.
   - B→A over GbE: a full-desktop HEVC/AV1 stream at high quality is ~30–80 Mbps — **trivial on
     1 GbE, invisible to the Quest link.**
   - waypipe B→A: bursty, damage-driven, zstd-compressed — fine on GbE.
   - lan-mouse input: kilobits. Negligible.
4. **The double-encode is a CPU/GPU cost, not a bandwidth cost** — and it's paid on wall power (A
   is a desktop-replacement), not on the headset. Acceptable.

**Verdict:** *Wire the two laptops together; keep the Quest as the sole wireless leg; composite on
A.* Every recommended approach assumes this topology.

---

## 9. Family G — prior art (what's real vs vapor on Linux)

| System | Multi-PC in one headset? | How | Lesson |
|--------|-------------------------|-----|--------|
| **Immersed** | **Yes** — the proof it's doable | Native headset app + **one lightweight agent per PC**; headset composites all PCs' monitors into one space (up to ~5 screens total); wired-GbE-to-router recommended | **This is Family D.** Aggregate on the headset-owning app; one agent per machine. 5 ms on Wi-Fi 5, ~1 ms USB ([clevcode](https://clevcode.org/low-latency-vr-desktop-with-immersed/), [immersed.com/faq](https://immersed.com/faq)) |
| **Virtual Desktop** (Quest) | **No** — single PC | Multi-*monitor* from one PC; up to 3 on Quest 3 | Multi-monitor ≠ multi-PC; even the best commercial streamer didn't attempt multi-PC ([UploadVR](https://www.uploadvr.com/virtual-desktop-multiple-monitors-update/)) |
| **Meta Horizon Workrooms** | Partial | Multiple virtual screens, tied to Meta ecosystem/one host | Not a general Linux answer ([Meta help](https://www.meta.com/help/quest/articles/horizon/getting-started-in-horizon-workrooms/multiple-screens-virtual-screens-workrooms/)) |
| **SteamVR** | **No** | One headset cannot be driven by two PCs simultaneously | Confirms C is against the grain ([Steam](https://steamcommunity.com/app/250820/discussions/0/4036979713106043529/)) |
| **WiVRn** | **Not yet** — but the client already stores N servers | PC-to-PC/wired streaming is experimental; multi-server *switch* is latent in the config (§4) | Fast-switch (B) is low-hanging; simultaneous is unbuilt upstream |
| **wlx-overlay-s** | N/A (single PC) | Desktop-in-VR overlay, dual OpenXR/OpenVR backends | Architectural cousin for the overlay/quad model (already cited in `PLATFORM-LIFECYCLE-PERFORMANCE.md §3`) |

**The single biggest signal:** the *only* product that ships true simultaneous multi-PC in one
headset (**Immersed**) does it exactly as **Family D** — a headset-owning compositor app plus a
thin per-PC agent — **not** as two runtimes on the headset (C). That's independent validation of
the recommended direction.

---

## 10. Recommendation

Two horizons, stacked. They share the §8 topology assumption (wire the laptops together, Quest is
the only wireless device).

### 10a. Cheapest win NOW (this week, ~zero XR code)

**Machine A stays the XR host; put machine B's desktop on an XR quad via Moonlight/Sunshine over
Ethernet, and cross-drive input with lan-mouse if needed.**

- On B: `sunshine` (or `wayvnc`). On A: `moonlight` **fullscreen on a dedicated XR monitor**
  (create `xrmonitor XR-B` and pin the client to its workspace). B's whole desktop is now a
  floating monitor in the headset next to A's monitors — **today, no HypXRland changes**.
- B→A over the GbE switch → the Quest still sees one 50 Mbps WiVRn stream (§8). Input rides
  Moonlight's own channel; A's XR ray drives the Moonlight window → B. Add **lan-mouse** only if
  you want A's physical keyboard/mouse to cross over too.
- Cost: one double-encode hop's latency (~a frame or two) and some GPU on A. Acceptable on wall
  power.

**And, in parallel, the cheapest *switch*:** pair **both** laptops in the WiVRn client once (§4);
thereafter switching is a lobby pick with **no re-pair**. If you want it hands-free, a small patch
to your self-built client binds a controller gesture / voice verb to `connect(pinned_other_server)`
— still a ~seconds reconnect, but no re-don, no menu.

### 10b. Ambitious end-state (the real thing) — **Family D-lite**

**A network monitor source: an exporter daemon on B + a decode-into-headless-output companion
daemon on A**, so B's monitors appear as *named XR monitors* (`XR-B-1`…) alongside A's — a true
unified two-machine workspace, with grab/anchor/mirror/naming all inherited from the existing XR
stack, and input routed A-ray → `motionAbsolute` → back to B over the exporter's input channel.
This is the Immersed pattern, built the HypXRland-native way (headless outputs = quads), with the
new risk confined to a userspace daemon rather than the load-bearing frame thread.

Do **not** build Family C. Consider **D-full** (in-compositor network-quad protocol, single-encode
passthrough) only later, if the per-output decoder daemon proves too heavy.

---

## 11. WP sketch (sized)

**Track M — "monitor bridge" (recommended):**

| WP | Effort | What |
|----|--------|------|
| M0 | **S** | *Validate the cheap win.* Sunshine on B + Moonlight fullscreen on `XR-B` on A, both laptops on a GbE switch; confirm the Quest link stays single-stream and input round-trips. Document the recipe in `docs/openxr/`. (No code.) |
| M1 | **S** | Client fast-switch: pair both servers; add a gesture/voice→`connect(pinned)` binding in the self-built WiVRn client. Harden against the reconnect race ([#206]) alongside L4. |
| M2 | **M** | *Exporter on B:* per-output screencopy + VAAPI encode + network publish (+ input receive). A standalone daemon; model on Sunshine but headless-output-aware. |
| M3 | **M** | *Companion daemon on A:* create one headless `CMonitor` per remote output, decode into its presented buffer, forward `motionAbsolute`/keys back to B. Reuses `hyprctl output create headless` + the presented-buffer path. |
| M4 | **S** | Naming/UX: `XR-B-*` naming convention, per-machine anchoring presets (B's monitors as a docked cluster), status surfacing in `hyprctl openxr status`. |
| M5 | **L** | *Optional D-full:* in-compositor network-quad-source protocol + reverse input channel + (stretch) encoded-stream passthrough to kill the double-encode. Only if M3's decoder daemon is too heavy. |
| M6 | **XL** | *Stretch:* cooperate with WiVRn so B's already-encoded stream is muxed into the headset stream without A re-encoding — single-encode unified workspace. Cross-project. |

Ordering: **M0 → M1** (both this week, no XR code) → **M2 ∥ M3 → M4** (the real bridge) → M5/M6
only if justified.

---

## 12. Open questions for the user

1. **"Both at once" vs "instant switch"** — which do you actually want day-to-day? If switching is
   fine, M0+M1 may be the whole project. If you want B's monitors *permanently* floating beside
   A's, it's M2–M4.
2. **Wired B↔A OK?** The entire recommendation rests on putting both laptops on an Ethernet switch
   so the Quest is the only wireless device (§8). Is a switch/cable acceptable at your desk, or
   must B→A also be wireless (which reopens the bandwidth problem)?
3. **Whole desktop or specific apps?** If you mostly want *a few B apps* in XR, **waypipe** (§3.1)
   is higher-fidelity and even simpler than the video path. If you want B's *entire* Hyprland,
   it's the video/monitor-bridge path.
4. **Which machine is the host?** The AMD+NVIDIA Framework is the stronger encoder host and
   currently owns WiVRn — keep it as A? Or does the Lunar Lake box ever need to be the headset
   owner?
5. **Double-encode tolerance.** For v1 (M2–M4) B's desktop takes two codec hops. Is that latency
   acceptable, or is single-encode (M6, cross-project WiVRn work) a hard requirement?
6. **Fast-switch APK appetite.** You already build the boundaryless client — is a small
   gesture→`connect(pinned_server)` patch (M1) in scope, or keep the switch as a lobby pick?

---

## 13. Sources

- HypXRland internals: `docs/openxr/00-06`, esp. `01-session-graphics.md` (frame loop, blit),
  `02-virtual-monitors.md` (headless-output = quad; presented-buffer handoff; plug lifecycle),
  `04-input.md:481–490` (ray→monitor `motionAbsolute` routing);
  `research/PLATFORM-LIFECYCLE-PERFORMANCE.md` (L4 reconnect, Z-series direct-scanout, OpenVR/wlx).
- WiVRn (read-only `~/code/wivrn`): `client/configuration.h:50–65` (server map + 50 Mbps default),
  `client/wivrn_discover.{h,cpp}`, `server/avahi_publisher.cpp`, `client/scenes/lobby.h`.
- waypipe: [man page](https://man.archlinux.org/man/extra/waypipe/waypipe.1.en),
  [mstoeckl notes](https://mstoeckl.com/notes/gsoc/blog.html).
- Moonlight/Sunshine latency: [moonlight-qt #1032](https://github.com/moonlight-stream/moonlight-qt/issues/1032),
  [farnoy.dev](https://farnoy.dev/posts/linux-latency).
- lan-mouse / input-leap on Wayland: [lan-mouse](https://github.com/feschber/lan-mouse),
  [KDE discuss](https://discuss.kde.org/t/feature-seamless-keyboard-and-mouse-sharing-integrate-input-leap/29512).
- PipeWire network: [ArchWiki](https://wiki.archlinux.org/title/PipeWire),
  [GStreamer WebRTC](https://discourse.gstreamer.org/t/send-screencapture-data-from-pipewire-to-webrtc/5426).
- Prior art: [Immersed FAQ](https://immersed.com/faq),
  [clevcode Immersed latency](https://clevcode.org/low-latency-vr-desktop-with-immersed/),
  [Virtual Desktop multi-monitor](https://www.uploadvr.com/virtual-desktop-multiple-monitors-update/),
  [SteamVR two-PC](https://steamcommunity.com/app/250820/discussions/0/4036979713106043529/),
  [WiVRn #206 reconnect](https://github.com/WiVRn/WiVRn/issues/206).
</content>
</invoke>
