# Research: a headset-in-the-loop STAGING container

**Status:** research / decision-support. **Nothing is implemented.** All findings are
code-read against the tree at `b11f423ba` (HypXRland), WiVRn 26.6.2 at
`~/code/wivrn-26.6.2` (branch `hypxr-patches-26.6.2`), and a read-only probe of the live
host on 2026-08-09.

The ask, verbatim:

> "it would be really great if I could test this type of stuff before we decided to deploy
> to fishfooding build - e.g. setting up a container with the capabilities, then have me
> connect from my headset into that container instead. Then I can provide feedback directly
> on that before churning my true session. Go and noodle on it. Better if it can somehow be
> supported concurrently with my live session, though don't bend over backwards to support
> it"

---

## 0. Executive summary

There are **two** designs here, and the cheap one is much closer to done than expected.

**The stopgap works today with zero code.** `scripts/xr-container.sh session --wivrn`
already boots a full Omarchy + candidate-HypXRland desktop in a container and drives a real
Quest 3 — it just borrows the *host's* WiVRn server over a bind-mounted `comp_ipc` socket
(`scripts/xr-container.sh:823-842`, `containers/session/session-launch.sh:136-155`). The
missing piece was never the container; it was knowing how to hand the headset over without
wrecking the live session. That answer is one hot keyword:
`hyprctl keyword openxr:monitors_follow_session off` makes
`OpenXR::wantXRMonitorsPlugged()` return unconditional `true`
(`src/openxr/XRMonitorConfig.cpp:356`), so the live compositor's XR monitors stay plugged and
its workspaces never evacuate even when its OpenXR session is torn down. Combined with
`hyprctl openxr disable` to release the runtime, the user can lend the headset to a
candidate build and take it back in seconds, with the live compositor never restarting and
every window exactly where it was. **This is Path A, and it should be validated first —
it may satisfy 80% of the ask for one agent-task of work.**

**The full design is Path B: a second WiVRn server inside the container**, which the headset
connects to as its own entry in the Quest lobby. Recommended shape: a
`hypxrland-ctr:staging` image layer (`:session` + `pacman -S wivrn-server`, which pulls every
runtime dep the host binary needs — ffmpeg, x264, libva, avahi, boost); the container keeps
its **own network namespace** (rootless pasta) and publishes container TCP `9757` on host
port **`9758`**, so it is *physically incapable* of colliding with the live server's socket;
`"publish-service": null` so it never touches mDNS, and the headset reaches it through a
**manual server entry** in the lobby (`client/scenes/lobby_gui.cpp:283-381`) — a permanent,
one-tap "HypXRland Staging" button; **NVENC on the idle RTX 5070** as the encoder, which
contends with nothing (the live session encodes VAAPI/h265 on the iGPU's single VCN); and
`/src` pointed at the candidate **git worktree** by simply running
`xr-container.sh` *from that worktree*, with a fresh container per candidate so the
overlay-snapshot caveat never applies. Turnaround is a ccache-warm incremental build,
minutes not hours.

True simultaneity — live XR *and* staging XR rendering at the same moment — is **not
achievable and not worth chasing**: the Quest runs one WiVRn client session at a time. The
achievable and valuable form of "concurrent" is that **the live compositor keeps running,
un-restarted, with its window layout frozen**, while the headset time-shares. Both paths
deliver that.

---

## 1. What already exists (verified, not remembered)

| Piece | Where | State |
|---|---|---|
| Container driver | `scripts/xr-container.sh` (1002 lines) | `build`, `shell`, `check-gpu`, `test`, `session`, `exec` |
| Session launcher | `containers/session/session-launch.sh` | windowed-Monado or host-WiVRn modes |
| Config merge | `containers/session/merge-conf.sh` | sources a base conf, neutralizes monitors, appends `openxr{}` + 2 default `xrmonitor`s |
| Images | `hypxrland-ctr:{base,pkgs,session}` | `:session` 4.69 GB, built 2026-08-09 |
| In-container build | `containers/build-in-ctr.sh` | Hyprland + hyprtester + monado + hypxrpaper into `/build` |
| Docs | `containers/README.md`, `docs/openxr/06-testing.md` §8 | §8.2 covers `session --wivrn` |

Two facts from this that drive everything below:

- **`/src` is `$REPO` — the directory the script lives in.**
  `REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"` (`scripts/xr-container.sh:121`),
  mounted `-v "$REPO:/src:O"` (`:759`). Running the script **from a candidate worktree**
  therefore mounts *that worktree* — candidate injection is already free, no `podman cp`.
- **The container never runs `wivrn-server`.** `--wivrn` mounts the host's
  `/usr/lib/wivrn`, the manifest, and `$XDG_RUNTIME_DIR/wivrn/comp_ipc`
  (`scripts/xr-container.sh:833-842`), and `session-launch.sh:140-144` symlinks that socket
  onto the path the WiVRn client library hardcodes. A second server is genuinely new.

### 1.1 The `/src` overlay caveat is a non-issue for staging

The known caveat is that the `:O` overlay snapshots at container **create**, showing new
files but not later edits to existing ones. The staging loop **recreates the container per
candidate**, so it always gets a fresh snapshot. The caveat only bites long-lived containers
being edited underneath, which staging never is. No `podman cp`, no md5 verification, no
in-place patching. This is worth stating loudly because it inverts the assumption in the
task framing.

### 1.2 A git worktree at `/src` builds fine

A worktree's `.git` is a *file* (`gitdir: /home/ajg/code/Hyprland/.git/worktrees/<id>`), which
does not resolve inside the container. That does **not** break the build: `CMakeLists.txt:249-274`
reads `GIT_COMMIT_HASH` / `GIT_BRANCH` / `GIT_COMMIT_MESSAGE` / `GIT_COMMIT_DATE` /
`GIT_DIRTY` **from the environment** and falls back to the literal `"unknown"` when unset.
`find_package(Git QUIET)` at `:249` is advisory.

**Turn this into a feature:** have the staging driver resolve those five values on the host
(`git -C "$REPO" rev-parse …`) and pass them as `podman run -e`, so `hyprctl version` inside
the staging session names the exact candidate under test. That is the difference between
"the headset shows something" and "the headset shows commit `abc1234` of branch `foo`".

---

## 2. Question 1 — the network path Quest → container

### 2.1 Discovery

WiVRn advertises DNS-SD service type **`_wivrn._tcp`** via **Avahi** (libavahi-client over
the *system* D-Bus, not the unix socket):

- Publish call: `server/main.cpp:353-375`, `publisher.emplace(poll_api, configuration.hostname, "_wivrn._tcp", configuration.port, TXT)` with TXT keys `protocol`, `version`, `cookie`.
- Avahi client: `server/avahi_publisher.cpp:148` (`avahi_client_new`), `:67` (`avahi_entry_group_add_service_strlst`).
- The **client** does *not* use Avahi — it ships its own mDNS responder (`external/mdns.h`, `client/wivrn_discover.cpp:406,471` on port 5353) and queries `_wivrn._tcp.local.` (`client/wivrn_discover.h:49`).

Instance name = `configuration.hostname`, defaulting to `wivrn::hostname()` which reads
`org.freedesktop.hostname1` (`server/hostname.cpp`) — on this box, `framework`. It is
overridable by the config key `"hostname"` (`server/driver/configuration.cpp:221`).

**Collision:** two servers publishing the same instance name is *handled*, not fatal — Avahi
returns `AVAHI_ERR_COLLISION` and WiVRn loops on `avahi_alternative_service_name()`
(`server/avahi_publisher.cpp:67-73`, `:107`), producing `framework #2`. But relying on that
is ugly and gives the user two near-identical entries in the lobby.

**Recommendation: don't publish at all.** `--no-publish-service` (`server/main.cpp:1131`,
applied `:1160-1163`) or `"publish-service": null` (`server/driver/configuration.cpp:224-229`).
Rationale: the container has no Avahi daemon and multicast through a rootless netns is
unreliable anyway; more importantly, a **manual** lobby entry is *better UX* than a discovered
one — it is permanent, named whatever we want, and sorts next to the live server.

### 2.2 Ports

**One port, TCP and UDP, default 9757** — `common/wivrn_config.h.in:26`:

```c
// Default port for server to listen, both TCP and UDP
static const int default_port = 9757;
```

- TCP listener: `server/main.cpp:334`, and again in the forked child at `server/accept_connection.cpp:36`. `TCPListener` binds `in6addr_any` with `SO_REUSEADDR`, backlog 1 (`common/wivrn_sockets.cpp:218-251`).
- UDP stream uses the **same** port but binds the *specific* local address of the accepted TCP socket (`server/driver/wivrn_connection.cpp:70-94`), and the port is handed to the client in the handshake (`:251`).
- `"tcp-only": true` sets `port = -1` so no UDP socket is created at all (`wivrn_connection.cpp:86-89`). **The live server already runs `tcp-only: true`** (see §3.1) — so staging can too, and only one TCP port ever needs exposing.
- Configurable **only** via config.json `"port"` (`server/driver/configuration.cpp:218-219`). There is no CLI flag and no env var.

**The dangerous timing detail:** the server **stops listening the moment a headset connects**
and re-listens when the session ends — `headset_connected()` does `stop_listening(); stop_publishing();`
(`server/main.cpp:492-493`), and `update_fsm()` re-arms both (`:408-417`). `SO_REUSEADDR` does
**not** permit two simultaneous listeners (that needs `SO_REUSEPORT`). So a naive same-port,
same-netns setup would *appear to work* — the staging server binds 9757 successfully while the
user is donned — and then break at doff, when the live server tries to re-bind and gets
`EADDRINUSE`. **This is the single most dangerous failure mode in the whole design**, and it
is why §5 recommends netns isolation over `--network host`.

### 2.3 Cleanest LAN exposure — the three candidates

| Option | Verdict |
|---|---|
| **macvlan** (own LAN IP, default port, clean mDNS) | **Impossible here.** The only LAN interface is `wlan0` (192.168.50.189/24, 5 GHz, no ethernet at all). macvlan does not work over 802.11 station mode, and rootless podman cannot create one anyway. Off the table. |
| **`--network host`** + non-default port | Works, lowest latency, but puts the staging server in the *same netns as the live one*. Protection is then purely a config value, and §2.2's delayed-`EADDRINUSE` trap is one typo away. **Fallback only.** |
| **Rootless default netns (pasta) + published port** | **Recommended.** `-p 9758:9757/tcp`. The container keeps the stock 9757 internally (no config port needed); the host exposes 9758 on `0.0.0.0`, reachable from the Quest at `192.168.50.189:9758`. The container is *physically incapable* of binding the host's 9757 — the isolation is structural, not a config discipline. |

Host state confirms pasta is the only rootless option: `podman info` reports rootless `true`,
backend `netavark`, `/usr/bin/pasta` 2026_06_11 present, **`slirp4netns` not installed**. The
existing `--publish-remote` path already uses `-p` (`scripts/xr-container.sh:864`), so the
mechanism is proven in this codebase.

Pasta forwards TCP with `splice()`, so overhead is small; and with `tcp-only: true` there is no
UDP path to worry about. Note that forwarded connections appear to come from the container
gateway rather than the real client IP — WiVRn does not care (pairing is key-based, not
IP-based).

### 2.4 Manual connect — yes, and it is the right UX

The Quest client has a full **Add server** dialog with **Name / Address / Port** fields plus a
**TCP only** checkbox: `client/scenes/lobby_gui.cpp:283-381`, port field at `:325`, TCP-only at
`:330`, button at `:1567`. Saved entries are keyed `"manual-" + name`
(`client/scenes/lobby_gui.cpp:368`), so **multiple manual servers coexist** as long as their
names differ. Manual entries bypass the protocol/TXT compatibility gate entirely:

```cpp
bool enable_connect_button = (data.visible and data.compatible) or data.manual;
```
— `client/scenes/lobby_gui.cpp:447`; `connect_to_session()` skips the TXT check and just
`getaddrinfo()`s the hostname (`client/scenes/lobby.cpp:204-255`).

Persistence is client-side in `<app files dir>/client.json`
(`client/configuration.cpp:118`, `:282`) — i.e. `/data/data/org.meumeu.wivrn/files/client.json`.
Discovered servers are keyed by the TXT `cookie` (`client/scenes/lobby.cpp:363-370`), and each
server generates its own random cookie, so **a containerized server can never clobber the live
server's lobby entry** even if we did publish.

There is also a **deep link**: `wivrn://host[:port]` and `wivrn+tcp://host[:port]`, with an
optional PIN in the userinfo password field — `AndroidManifest.xml:146-147`, parsed at
`client/application.cpp:1457-1476`, auto-connected at `client/scenes/lobby.cpp:890-897`. The
desktop dashboard already emits `wivrn+tcp://:<pin>@127.0.0.1:9757` (`dashboard/adb.cpp:324`).
This is the scripted, zero-typing way to point the headset at staging if we ever want an
`adb`-driven "stage it" button.

**Answer to "does the mDNS name collide": moot by construction** — we don't publish, and the
lobby keys manual entries by name and discovered ones by cookie.

### 2.5 Pairing and its persistence

Mechanism: X448 key exchange + SMP socialist-millionaire PIN verification, then AES-128-CTR.
Handshake states at `server/driver/wivrn_connection.cpp:184-245`, client side
`client/wivrn_client.cpp:111-187`.

**Server-side state** — `server/driver/configuration.cpp:52-53`:

```cpp
static std::filesystem::path known_keys_file = resolve_path(xdg_config_home() / "wivrn" / "known_keys.json");
static std::filesystem::path cookie_file     = resolve_path(xdg_config_home() / "wivrn" / "cookie");
```

Both derive purely from `XDG_CONFIG_HOME`/`HOME`, so a container with its own `HOME` is
**automatically isolated** from the host's pairing state.

Bootstrap is friendly: a server with an empty key list **auto-enters pairing mode at startup**
(`server/main.cpp:999-1001`) and prints the 6-digit PIN to stderr unconditionally
(`server/main.cpp:683-691`) — i.e. straight into `podman logs`. Pairing auto-disables after
one successful connection (`server/main.cpp:437-438`). `wivrnctl pair [-d minutes]`
(`tools/wivrnctl/main.cpp:196-224`) re-enables it, but **cannot** while a session is active
(`server/main.cpp:764-771`).

**Recreate-per-candidate would re-pair every time** — unacceptable friction. Two fixes:

1. **Recommended:** persist `~/.config/wivrn/` in a named volume `hypxrland-staging-wivrn`.
   One-time 6-digit PIN entry, then every future staging container is instant. Also keeps the
   `cookie` stable.
2. **Escape hatch:** `--no-encrypt` (`server/main.cpp:1132`, `:1157-1158`) — the server replies
   `encryption_disabled`, skips the known-key check entirely, and installs no AES keys
   (`server/driver/wivrn_connection.cpp:186-191`). Zero friction forever, at the cost of an
   unencrypted video stream on the user's own WPA-protected WLAN.

Take (1) as the default and expose (2) as a flag. The client's own `private_key.pem`
(`client/scenes/lobby.cpp:176-187`) survives regardless, so even a lost volume is a one-sided
re-pair — six digits, headset-side.

---

## 3. Question 2 — concurrency with the live session

### 3.1 The live session, as measured

```
Hyprland   PID 1820  /home/ajg/code/hypxrland/build/Hyprland --config ~/.config/hypr/hyprland-xr.conf
wivrn      PID 130266,130934  /home/ajg/code/wivrn-26.6.2/build-server/server/wivrn-server
```

`~/.config/wivrn/config.json`:

```json
{"encoder":{"encoder":"vaapi","codec":"h265","device":"/dev/dri/renderD129"},
 "bit-depth":8,"tcp-only":true,"inhibit":"worn"}
```

`~/.config/hypr/hyprland-xr.conf`: `env = AQ_DRM_DEVICES,/dev/dri/card2` (AMD),
`openxr { enabled=1; gpu=/dev/dri/renderD129; blend_mode=alpha; black_alpha=0.2; hand_input=off }`,
one `xrmonitor = XR-main, 2560x1440@90, …`, three `xrrule`s, and three `exec-once`:
`systemctl --user start wivrn.service`, `~/.local/bin/hypxrva-watcher`,
`~/.config/hypr/scripts/hypxr-display-autopilot.sh`.

**Render node numbering is inverted from the usual guess** and must not be assumed:

| Node | PCI | GPU |
|---|---|---|
| `renderD128` | `0000:c1:00.0` `10DE:2D58` | **NVIDIA RTX 5070 Laptop, 8 GB** |
| `renderD129` | `0000:c2:00.0` `1002:150E` | **AMD Radeon 890M iGPU** |

`scripts/lib/gpu.sh`'s vendor scan already handles this correctly; nothing hardcodes names.

### 3.2 Shared-resource inventory

| Resource | Conflict? | Mitigation | Evidence |
|---|---|---|---|
| **TCP/UDP 9757** | **YES — the top risk.** `SO_REUSEADDR` ≠ two listeners, and the live server releases/re-binds its listener around each session, so a collision surfaces *later*, at doff. | Own netns (pasta) + publish host **9758**. Hard-refuse `9757` in the driver. | `common/wivrn_sockets.cpp:218-251`; `server/main.cpp:492-493`, `:408-417` |
| **Avahi / mDNS** | Would produce `framework #2`; container has no Avahi daemon. | `"publish-service": null`; manual lobby entry. | `server/avahi_publisher.cpp:67-73`; `server/driver/configuration.cpp:224-229` |
| **`XDG_RUNTIME_DIR`** | **Would be severe if shared** — the server *unlinks* what it thinks is a stale `monado_comp_ipc`, and the live compositor's inotify reprobe watches `…/wivrn/comp_ipc`. | **Already safe:** the container's `/run/user/1000` is its own systemd tmpfs. Never bind-mount the host's. | `server/main.cpp:74-127`; `src/openxr/XRMonitorConfig.cpp:470-487` |
| **D-Bus name `io.github.wivrn.Server`** | Single-owner; a second claimant silently loses it. | Container has a private system+session bus by construction (PID-1 systemd, `machinectl shell`). | `server/main.cpp:1093-1100`; `containers/Containerfile.base:5-9` |
| **`active_runtime.json`** | Writes `$XDG_CONFIG_HOME/openxr/1/active_runtime*.json` and `openvrpaths.vrpath`, with backup/restore in the dtor. | Moot — container's own `HOME`. Pass `--no-manage-active-runtime` anyway (belt and braces). | `server/active_runtime.cpp:158-161`, `:176-193`; `server/main.cpp:1127` |
| **VCN video encoder (iGPU)** | **YES** if staging uses VAAPI — one VCN, and the live encode owns it. This is precisely what `hypxrva` exists to arbitrate. | Use NVENC on the dGPU. Never `"encoder":"vaapi"` for staging. | live `config.json`; `hypxrva` memory |
| **GPU render contexts** | No — GPUs multiplex across processes. RTX 5070 currently at 20% util / 12.5 W. | — | `nvidia-smi` probe |
| **`/dev/input`, `/dev/uinput`** | No. Container has no `/dev/input`; WiVRn `hid-forwarding` defaults false. | — | `containers/README.md:19-20`; `server/driver/configuration.cpp:203` |
| **PipeWire** | No — already solved. Host sockets bind-mounted **read-only** and symlinked onto default discovery paths; in-container daemons masked. Headset audio appears as ordinary `wivrn.sink`/`wivrn.source` devices. | Existing `session-launch.sh:76-99`. **Do not open capture streams on the live `wivrn.source`.** | `containers/README.md:81-114` |
| **logind inhibitors** | Minor — `inhibit` defaults to `session` on this branch and would fight host power management. | `"inhibit": "none"` in the staging config. | `server/driver/configuration.cpp:231-237` |
| **`hypxrva` decode gating** | Indirect — see §3.4. | Accept; note it. | — |

### 3.3 Protecting the live session across the handoff — the key finding

The instinct is to lean on the 20-second unplug grace. **That does not work.** The grace is
real — `openxr:monitor_unplug_grace_ms`, default `20000`, range 0–600000
(`src/config/values/ConfigValues.cpp:943-947`) — but it is only armed on a *doff while the
session survives* (`src/openxr/OpenXRManager.cpp:3234-3242`). A headset that leaves for
another server is a **session loss**: `markRuntimeLost()` → frame loop exits → `stop()` →
`updateMonitorsPlugged(/*allowGrace=*/false)` with `sessionExists()==false` → **immediate
unplug, no grace** (`src/openxr/XRSession.cpp:417-434`; `src/openxr/OpenXRManager.cpp:1005-1012`,
`:3244-3245`). Workspaces evacuate at once.

The correct lever is `monitors_follow_session`. `OpenXR::wantXRMonitorsPlugged()` returns
**unconditional `true`** for the `off` mode:

```
off      -> always true (never unplugs, even with no session)   XRMonitorConfig.cpp:356
session  -> sessionUp
visible  -> sessionUp && sessionVisible && (presence ? presenceKnown && userPresent : true)
```
— `src/openxr/XRMonitorConfig.cpp:345-367`.

So, on the **live** compositor, immediately before handing the headset over:

```sh
hyprctl keyword openxr:monitors_follow_session off
```

and afterwards restore `visible`. Both are hot-applied — `ConfigManager.cpp:1161`
special-cases `monitors_follow_session`, `monitor_unplug_grace_ms` and `monitor_plug_settle_ms`
so a bare `hyprctl keyword` actually takes effect. `openxr:destroy_monitors_on_stop` is already
`false` by default (`ConfigValues.cpp:953-956`), so the XR monitor layer objects, anchors and
poses survive the session teardown intact (`OpenXRManager.cpp:1006-1009`).

Coming back is fully automatic. `UNAVAILABLE` is dormant, not terminal: `setState()` arms a
reprobe (`OpenXRManager.cpp:281-287`), `openxr:reprobe` defaults true
(`ConfigValues.cpp:732-735`), and `openxr:reprobe_watch` (default true, `:740-744`) inotify-watches
`$XDG_RUNTIME_DIR` for `wivrn.pid` / `monado_comp_ipc` / `wivrn/comp_ipc`
(`XRMonitorConfig.cpp:470-487`), firing `start()` within ~150 ms of the headset returning
(`OpenXRManager.cpp:3105-3116`). **No manual `hyprctl openxr enable` is required.**

The trade-off of `off` is honest and should be documented: while the headset is away, the XR
monitors remain plugged as outputs the user cannot see. Nothing moves, which is the point, but
those windows are unreachable until the headset returns. The display-autopilot `exec-once`
turning eDP back on at doff means the user still has a usable screen meanwhile.

### 3.4 Second-order effects worth knowing

- **`hypxrva`**: the watcher gates VA-API *decode* on XR-monitor-plugged state
  (`/run/user/1000/hypxr/va-decode-block`, currently absent — no XR monitor plugged as of the
  probe). With `monitors_follow_session off` the monitors stay plugged, so decode stays gated
  for the duration of a staging session. Harmless (video decode falls back to software), but
  surprising if unexplained.
- **The dGPU never sleeps today.** Task #117's D3cold concern is currently moot: despite
  `AQ_DRM_DEVICES=/dev/dri/card2`, `lsof` shows the live Hyprland (PID 1820) holding **11 fds on
  `/dev/nvidia0`/`/dev/nvidiactl`** and 5 on `renderD128`, with `runtime_status: active` and
  12.46 W draw. Recommending NVENC therefore **wakes nothing and creates no re-sleep obligation**
  — it uses a GPU that is already up. (Separately: the config file's own comment says this
  finding would falsify the AMD-only pin. It is falsified. Worth its own task, out of scope here.)

### 3.5 Encoder ranking

**(b) NVENC on the RTX 5070 — RECOMMENDED.**
`encoder_nvenc = "nvenc"` (`server/encoder/video_encoder_nvenc.cpp`, name at
`server/encoder/video_encoder.h:43-47`), auto-selected for `vendorID == 0x10DE`
(`server/encoder/encoder_settings.cpp:225-272`). Zero contention with the live VCN, hardware
quality, no CPU cost, dGPU already awake. `nvidia-container-toolkit 1.19.1` is installed and the
`--gpu nvidia` CDI path already exists in the tooling (`scripts/xr-container.sh:260`). The CDI
spec **does** inject the encode libraries (`libnvidia-encode.so.610.43.02`, `libnvcuvid`,
`libcuda` all present in `/etc/cdi/nvidia.yaml`).
**Blocker:** the spec is **stale** — baked `host-driver-version=610.43.02`, loaded driver
`610.43.03`. Needs `sudo nvidia-ctk cdi generate --output=/etc/cdi/nvidia.yaml` (§7).

**(a) x264 software — fallback.**
`encoder_x264 = "x264"`, hardcoded `x264_param_default_preset(&param, "ultrafast", "zerolatency")`,
`i_slice_count = 32`, profile `main`, ABR at the client-supplied bitrate
(`server/encoder/video_encoder_x264.cpp:138-166`). **Neither preset nor tune is exposed via
config `options`** — only `min-qp`, and that only for nvenc/vaapi
(`server/encoder/encoder_settings.cpp:47-60`). It also does a host-visible luma/chroma copy per
frame (`:174-190`).
The box is a Ryzen AI 9 HX 370, **12C/24T** — genuinely capable, but the CPU is the same
package feeding the iGPU that is encoding the live stream, and it is also running ccache
builds. Viable, not free.
**Important knob location:** resolution and bitrate are **client-side**, not server-side —
`client/configuration.h` carries `float resolution_scale = 1.0` and
`uint32_t bitrate_bps = 50'000'000`, applied at `client/scenes/stream.cpp:221-227`, `:246`; the
server merely aligns to 64 px and splits the total across three encoders (left/right/alpha) by
pixel-count weight (`server/encoder/encoder_settings.cpp:64-89`, `:293-295`). So "lower the
staging resolution" is a **headset-side setting on the staging server's profile**, and it is the
right lever if x264 is ever used. Staging does not need production quality.

**(c) Sharing the VCN — DO NOT.** Direct contention with the live encode on the single video
engine. This is the exact problem `hypxrva` was built to avoid; re-creating it for a test rig
would be a poor trade. Not recommended even for short tests.

---

## 4. Question 3 — the candidate-build injection loop

Target: agent finishes a worktree branch → user says "stage it" → headset shows it, in minutes,
with one user action.

```
$ hypxr-stage /home/ajg/code/Hyprland/.claude/worktrees/agent-XXXX
   (or: hypxr-stage <branch>, which resolves the worktree)
```

What happens:

1. **Resolve** the worktree path; refuse if it is the live fishfood worktree (`~/code/hypxrland`).
2. **Guard the live session** (Path B / concurrent variant only): read the current
   `openxr:monitors_follow_session` from `hyprctl openxr status -j`, set it to `off`, and install
   an EXIT/INT/TERM trap that restores it. Refuse if the requested host port is `9757`.
3. **Create a fresh container** from `hypxrland-ctr:staging`, mounting the worktree at `/src:O`,
   the dedicated `hypxrland-staging-build` volume at `/build`, the shared `hypxrland-ccache`, and
   `hypxrland-staging-wivrn` at `~/.config/wivrn`. Pass `GIT_COMMIT_HASH` etc. from the host.
   `-p 9758:9757/tcp`.
4. **Build** — `bash /src/containers/build-in-ctr.sh` (already exists;
   `cmake --build … -j"$JOBS"` at `containers/build-in-ctr.sh:66`). Warm ccache across candidates
   because the volume persists.
5. **Write the staging `config.json`** into the container and start `wivrn-server`; wait for its
   listener; print the pairing PIN from the logs if it is a first pair.
6. **Launch** `session-launch.sh` in a new `staging` mode — identical to `wivrn` mode minus the
   host-socket symlink (the in-container server creates `$XDG_RUNTIME_DIR/wivrn/comp_ipc` itself).
7. **User action:** tap "HypXRland Staging" in the Quest lobby. That is the one action.
8. **Exit** → `podman rm -f` reaps the whole tree (existing `cleanup_session` trap,
   `scripts/xr-container.sh:869-870`); trap restores the live keyword.

**Why not `podman cp` + in-place rebuild?** Because recreate-per-candidate is *both* simpler and
faster here: the overlay caveat disappears (§1.1), the ccache volume makes the rebuild
incremental anyway, and container create is seconds. `podman cp` only wins for a long-lived
container, which we explicitly do not want.

**Why not build on the host and copy the binary in?** The container is Arch and so is the host,
so it would probably work — but "probably" against a mismatched `hyprutils`/`aquamarine` is
exactly the class of bug that wastes a headset session. In-container build is the primary; note
host-build-and-inject as an unvalidated speed experiment.

**Turnaround estimate:** container create + `pacman`-free boot ≈ 20-40 s; ccache-warm incremental
Hyprland build ≈ 2-10 min depending on how much of `src/openxr` moved; wivrn-server start ≈ 5 s.
Comfortably "minutes not hours". Every heavy build still goes through
`flock -w 7200 /tmp/hypxrland-build.lock` with `-j8`.

---

## 5. Question 4 — session ergonomics

### 5.1 Switching the headset

- **Path A (borrow the host server):** the headset never moves. It stays connected to the live
  WiVRn server the whole time; only *which compositor* holds the OpenXR session changes. Switching
  is two host commands and zero headset interaction. This is the lowest-friction flow by a wide
  margin.
- **Path B (own server):** the Quest lobby shows the live server (discovered, `framework`) and a
  permanent manual entry ("HypXRland Staging"). Switching is: back out of the current session →
  tap the other entry. Both entries persist in `client.json` (`client/configuration.cpp:264-273`),
  so this is a two-tap operation forever after the first setup.

### 5.2 What happens to the live session's XR monitors

Covered in §3.3. With `monitors_follow_session off`: nothing moves, monitors stay plugged,
windows stay put, and the headset's return auto-replugs within ~150 ms via the inotify reprobe.
Without it: immediate evacuation to the first enabled monitor (`src/output/Monitor.cpp:449-457`),
with workspaces tagged `m_lastMonitor` (`:497-508`) and restored by name on replug
(`:338-403`) — recoverable, but a visible churn of the live layout, which is what the user asked
to avoid.

### 5.3 The staging config

**Location: `containers/session/staging-xr.conf`, checked into the repo** — so it travels with
the candidate branch and an agent can change it in the same PR as the feature it exercises. It
must **never** touch `~/.config/hypr/`.

What it must *omit* relative to the live `hyprland-xr.conf`:

- `source = ~/.config/hypr/hyprland.conf` — host paths; the container's Omarchy base conf is
  already sourced by `merge-conf.sh:30`.
- All three `exec-once` lines — `wivrn.service` (the driver starts the server itself),
  `hypxrva-watcher` (no VA-API arbitration needed; staging encodes on NVENC), and the display
  autopilot (no eDP in the container).
- `env = AQ_DRM_DEVICES,…` — the wrapper sets this per split-GPU role
  (`session-launch.sh:109-110`).
- `openxr:gpu` — the wrapper injects the in-container-resolved node
  (`merge-conf.sh:38`).

What it must *keep*, to make staging comparable to live: the `xrmonitor` line(s), the `xrrule`
transparency rules, `blend_mode`, `black_alpha`, `hand_input`.

One small change to `merge-conf.sh` is needed: it unconditionally appends two default
`xrmonitor` lines (`merge-conf.sh:45-47`), which would fight a staging conf that declares its
own. Gate that on a `XR_DEFAULT_MONITORS=0` env.

Keep the existing `--conf FILE` flag (`scripts/xr-container.sh:701`, `:779-784`) working for
one-off experiments.

### 5.4 Driving the staging session from outside

`xr-container.sh exec` already works (`:965-974`). The full `hyprctl openxr` surface is
available inside: `status enable disable create destroy select layout anchor move rotate scale
distance center place alpha blackalpha adaptive dock undock roam gazegrab gazerelease gazepush
handinput sync-layout gaze` (`src/openxr/XRIpc.cpp:323-447`). `status -j` exposes
`unplugPendingMs` (`XRIpc.cpp:175`), `presence`, `visible`, `reprobeWait` — enough for the
driver to wait on "session is focused" rather than sleeping blindly.

---

## 6. Question 5 — risks and what NOT to attempt

**Top 3 risks.**

1. **Killing the live WiVRn server by port collision.** `SO_REUSEADDR` does not allow two
   listeners, and the live server drops and re-acquires its listener around every session
   (`server/main.cpp:492-493`, `:408-417`), so the failure is *delayed* — it looks fine while
   donned and breaks at doff. **Mitigation:** own netns (never `--network host` for staging),
   publish host `9758`, and a hard refusal on `9757` in the driver. Structural, not disciplinary.
2. **Churning the live layout on handoff.** A headset leaving is a session *loss*, which bypasses
   the 20 s grace entirely (`OpenXRManager.cpp:1005-1012`, `:3244-3245`). **Mitigation:**
   `openxr:monitors_follow_session off` before, restore after, with a shell trap so a crashed
   driver still restores it. Note this is the one place staging *writes to the live compositor* —
   it should be a single, auditable, trap-protected pair of `hyprctl keyword` calls and nothing
   else.
3. **GPU/CDI mismatch.** The CDI spec is stale (610.43.02 vs loaded 610.43.03) and must be
   regenerated before NVENC will work. Separately, the container's Vulkan must select **NVIDIA**
   for the WiVRn compositor so it agrees with `openxr:gpu`; a mismatch is the documented
   `xrCreateSwapchain` SEGV class (`docs/openxr/06-testing.md` §8.3, guarded by the
   `xr_gpu_mismatch_fails_closed` test). The live server pins this with
   `VK_DRIVER_FILES=/usr/share/vulkan/icd.d/radeon_icd.json`; staging needs the NVIDIA ICD
   equivalent.

**Non-risks — verified moot, do not spend work on them.**

- **Two servers claiming `active_runtime.json`:** the path derives from `XDG_CONFIG_HOME`/`HOME`
  (`server/active_runtime.cpp:158-161`), and the container has its own. Pass
  `--no-manage-active-runtime` as cheap insurance; do not architect around it.
- **`XDG_RUNTIME_DIR` / monado IPC socket collision:** the container's `/run/user/1000` is its own
  systemd tmpfs. This *would* be severe if shared (the server unlinks "stale" sockets,
  `server/main.cpp:82-127`) — so the rule is simply *never bind-mount the host's runtime dir*,
  which no existing mode does.
- **D-Bus name collision:** private buses by construction.
- **GPU context conflicts:** GPUs multiplex; the RTX 5070 sits at 20% util.

**What NOT to attempt.**

- **macvlan.** WiFi-only host, rootless podman. Dead end; do not spend a task on it.
- **`--network host` for the staging server.** Trades the structural guarantee for a config value.
- **VAAPI/VCN for the staging encoder.** Re-creates the exact contention `hypxrva` exists to solve.
- **Two simultaneous rendering XR sessions.** The Quest runs one WiVRn client session; this is a
  client-side limit, not something a server-side design can route around.
- **Sharing `~/.config/wivrn` between host and container.** Would let the two servers fight over
  `known_keys.json`, the cookie, and the active-runtime backups.
- **Anything touching the live session beyond the one keyword pair.** No `hyprctl dispatch`, no
  restarting `wivrn.service`, no capture streams on `wivrn.source`, no writes to `~/.config/hypr`.

---

## 7. Recommended architecture

**Primary: Path A first, then Path B.** They are not alternatives — A is the fast validation of
the whole premise and remains the daily-driver flow for most candidates; B is the upgrade that
removes A's one real limitation (live XR must be off while staging runs).

### Path A — borrow the host runtime (zero new code)

```sh
# on the LIVE session
hyprctl keyword openxr:monitors_follow_session off   # freeze the layout
hyprctl openxr disable                               # release the runtime

# stage the candidate (from its worktree)
/path/to/worktree/scripts/xr-container.sh session --wivrn

# ... user tests in the headset, gives feedback ...
# exit the session, then on the LIVE session:
hyprctl openxr enable
hyprctl keyword openxr:monitors_follow_session visible
```

Why this works: the compositor is entirely runtime-agnostic — a `grep` across `src/` for
`wivrn|comp_ipc|WIVRN_` finds only a `stat()` for presence classification and the inotify watch;
**no host, port or address is parsed anywhere**. The only inputs are `XR_RUNTIME_JSON`
(`OpenXRManager.cpp:326-358`) and `$XDG_RUNTIME_DIR` (`XRMonitorConfig.cpp:497-504`). So
"which compositor owns the headset" is decided purely by which one holds the OpenXR session.

Limitations: live XR is disabled for the duration (though the *compositor* keeps running and the
layout is frozen); the staging session shares the host's iGPU VAAPI encoder (fine — the live XR
stream is off); and it depends on the host `comp_ipc` socket surviving the live compositor's
disconnect, which is the main thing WP-ST0 must verify.

### Path B — a second WiVRn server in the container (the concurrent variant)

```
Quest 3 ── WiVRn client, two lobby entries ──┐
                                             ├─ "framework" (discovered, mDNS)  → host :9757 → LIVE wivrn-server
                                             │                                                 VAAPI h265 on renderD129 (iGPU VCN)
                                             │                                                 → LIVE Hyprland (fishfood)
                                             │
                                             └─ "HypXRland Staging" (manual, 192.168.50.189:9758)
                                                     │  pasta netns, -p 9758:9757/tcp
                                                     ▼
                                       ┌─────────────────────────────────────────────┐
                                       │ hypxrland-ctr:staging  (rootless podman)    │
                                       │  own netns / own /run/user/1000 / own buses │
                                       │                                             │
                                       │  wivrn-server                               │
                                       │    port 9757 (internal), tcp-only           │
                                       │    publish-service: null                    │
                                       │    encoder: nvenc h265   ← RTX 5070 via CDI │
                                       │    inhibit: none, --no-manage-active-runtime│
                                       │       │ $XDG_RUNTIME_DIR/wivrn/comp_ipc     │
                                       │       ▼                                     │
                                       │  CANDIDATE Hyprland (/build, from /src:O)   │
                                       │    AQ_DRM_DEVICES = renderD129 (AMD)  ──────┼──► nested window on the live desktop
                                       │    openxr:gpu     = renderD128 (NVIDIA)     │    (keyboard/mouse, flat view)
                                       │    --config staging-xr.conf (merged)        │
                                       └─────────────────────────────────────────────┘
                                          volumes: staging-build, ccache, staging-wivrn (pairing)
```

The GPU role split is **exactly the existing, validated `--gpu split`** shape
(`scripts/xr-container.sh:736-745`, `resolve_split_gpu`): nested compositor on the
host-compositor GPU because it renders into the host's Wayland socket, XR/encode on NVIDIA. The
one new wire is pinning the staging `wivrn-server`'s Vulkan ICD to NVIDIA so the runtime's device
agrees with `openxr:gpu`.

Keeping the nested window on the live desktop (rather than nesting into a headless in-container
labwc as `test` mode does) is a deliberate ergonomic choice: it gives the user a place to type
and click while wearing the headset, and it reuses the code path that is already proven for
`--wivrn`. A headless-labwc variant is a later option if full detachment is ever wanted.

**Image:** `hypxrland-ctr:staging` = `:session` + `pacman -S wivrn-server`. The distro package is
`wivrn-server 26.6.2-1` (already on the host, owning `/usr/lib/wivrn`), and installing it pulls
every runtime dep the host's patched binary links against — verified by `ldd` on
`~/code/wivrn-26.6.2/build-server/server/wivrn-server`: `libavcodec.so.62`, `libavutil.so.60`,
`libx264.so.165`, `libva`/`libva-drm`, `libavahi-{client,common,glib}`,
`libboost_{iostreams,random,regex}.so.1.91.0`, `libsystemd`, `libgio-2.0`. **None of these are in
`:session` today** (`containers/Containerfile.base:43-95` has no ffmpeg, x264, libva, avahi or
boost), so this layer is required.

**Which server binary?** Prefer bind-mounting the host's *patched* build read-only
(`~/code/wivrn-26.6.2/build-server`) over using the distro binary, because the branch carries the
**STAGE reference-space correction** (`d8664477`, `c8910153`, `c5c30878`, `7e2473f2`) — which
affects where monitors appear in space. Staging that does not match live on pose is staging that
lies. The distro package is then present for its shared libraries and manifest, not its binary.
Both are 26.6.2, so the client protocol version matches either way.

**Fallbacks, in order:** NVENC fails or CDI cannot be regenerated → x264 with a reduced
headset-side `resolution_scale`/`bitrate` on the staging profile. Pasta port-forwarding proves
lossy → `--network host` with a hard-coded non-9757 port and an explicit pre-flight check that
the live server is not mid-rebind. Pairing friction → `--no-encrypt`.

---

## 8. Workplan

Sized in agent-tasks. **M** = minimal non-concurrent variant, **C** = concurrent-with-live
variant. Pick M, or M+C.

| WP | Scope | Tasks | Variant |
|---|---|---|---|
| **WP-ST0** | **Validate Path A end-to-end.** Confirm the host `comp_ipc` socket survives the live compositor's `hyprctl openxr disable`; confirm `monitors_follow_session off` really freezes the live layout across a full session teardown; confirm the container session reaches `focused` on the real headset; measure how long the round trip takes. Produce a `scripts/hypxr-stage.sh` wrapper that does the whole A dance with a restore trap. **Headset-in-the-loop; needs the user.** | 1 | M |
| **WP-ST1** | **`hypxrland-ctr:staging` image layer.** Extend `xr-container.sh build` with a `--staging` stage committing `:session` + `pacman -S wivrn-server`. Validate `wivrn-server --version` and `ldd` clean inside. | 1 | M |
| **WP-ST2** | **`xr-container.sh staging` subcommand.** Fresh container from `$REPO` (the candidate worktree), own netns + `-p <port>:9757/tcp` with a hard refusal on 9757, split-GPU resolution, `GIT_*` env passthrough, `hypxrland-staging-{build,wivrn}` volumes, generated in-container `config.json` (port / hostname / `publish-service: null` / `tcp-only` / `inhibit: none` / nvenc), host patched-server bind mount, `--no-manage-active-runtime`, listener wait, PIN surfacing from logs. Plus a `staging` mode in `session-launch.sh` (the `wivrn` path minus the host-socket symlink) and `VK_DRIVER_FILES` pinned to the NVIDIA ICD. | 2 | M |
| **WP-ST3** | **Staging config.** `containers/session/staging-xr.conf` (mirrors the live `xrmonitor`/`xrrule` block, drops all host-specific `exec-once`/`env`/`gpu`), plus the `XR_DEFAULT_MONITORS=0` gate in `merge-conf.sh:45-47`. Document the "keep in sync with `hyprland-xr.conf`" obligation. | 1 | M |
| **WP-ST4** | **Live-session guard.** Trap-protected `openxr:monitors_follow_session off` → restore, reading the prior value from `hyprctl openxr status -j` rather than assuming `visible`. Opt-in flag (`--guard-live`), refuses to run against anything but the live instance, restores on every exit path including SIGKILL-of-parent (systemd-run scope or a watchdog). This is the only code that writes to the live compositor — it gets its own task and its own review. | 1 | **C** |
| **WP-ST5** | **Encoder validation.** NVENC on the regenerated CDI spec: confirm `libnvidia-encode` resolves in-container, measure latency/quality vs the live VAAPI baseline, and measure x264 CPU cost at the staging resolution as the fallback. Wire the `--encoder nvenc\|x264` flag. **Headset-in-the-loop.** | 1 | **C** |
| **WP-ST6** | **Docs + one-liner.** `containers/README.md` staging section, `docs/openxr/06-testing.md` §8.4, and the `hypxr-stage <worktree\|branch>` front door that picks Path A or B by flag. | 1 | M |

**Minimal variant (M): WP-ST0, ST1, ST2, ST3, ST6 — 6 agent-tasks**, and ST0 alone (1 task) may
be enough to unblock the user this week.
**Concurrent variant (M+C): + WP-ST4, ST5 — 8 agent-tasks total.**

**Prerequisite, not an agent task:** regenerate the CDI spec (§7 risk 3). Requires root.

---

## 9. Open questions for the user

1. **CDI regeneration.** `/etc/cdi/nvidia.yaml` is baked at `host-driver-version=610.43.02` but
   the loaded driver is `610.43.03`. NVENC staging needs
   `sudo nvidia-ctk cdi generate --output=/etc/cdi/nvidia.yaml`. Do you want to run it, or should
   the staging driver detect the drift and prompt? (An agent cannot sudo.)
2. **Is Path A's limitation acceptable?** Path A disables live XR while staging runs — the
   compositor keeps running and the layout is frozen, but the live XR monitors go dark until you
   come back. If yes, Path A is ~1 task and Path B becomes optional polish. If you specifically
   want the live XR session *alive* while you evaluate a candidate, that is Path B — and note the
   headset still cannot render both at once, so "alive" means "instantly resumable", not
   "simultaneously visible".
3. **Pairing vs `--no-encrypt`** for the staging server: persist a pairing volume (one-time
   6-digit PIN, encrypted stream) or run trust-all on your own WLAN for zero friction forever?
4. **Should the staging config mirror your live `xrmonitor` layout exactly** (best for
   apples-to-apples feedback, but needs manual sync when you retune) or use a fixed simple layout
   (self-maintaining, but pose feedback won't transfer)?
