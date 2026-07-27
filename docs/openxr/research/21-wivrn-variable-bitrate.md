# Research: variable-rate / content-adaptive encoding for WiVRn streaming

**Status:** research / decision-support. **Nothing implemented.** The question is why a
mostly-static desktop (terminals, editors, a browser) costs a flat ~30 Mbit/s on the wire, and
what the realistic levers are to make the stream track content complexity instead of a fixed
budget. Follows the house style of `docs/openxr/research/`: ground truth first, an explicit
verified-vs-inferred split, honest pro/con per lever, an options table with effort /
upstreamability / risk, then a staged recommendation.

**Headline:** we are **not** "already variable and just always saturating". WiVRn's NVENC path is
pinned to `NV_ENC_PARAMS_RC_CBR` with **no quality ceiling of any kind** — no `minQP`, no
`targetQuality`, no `maxBitRate`. A constant-bitrate controller with an unbounded quality target
has exactly one behaviour: drive QP down until the budget is spent. It does not matter how boring
the content is. The measured 28.1 Mbit/s against a configured 30.0 Mbit/s is that controller
working as designed.

Evidence base (all read-only, live session never touched):

- **WiVRn server** at `~/code/wivrn-26.6.2`, branch `hypxr-patches-26.6.2` (= upstream `v26.6.2`
  = `8e03a05a`, plus 6 local patches):
  `server/encoder/video_encoder_nvenc.cpp`, `server/encoder/video_encoder.{h,cpp}`,
  `server/encoder/encoder_settings.cpp`, `server/encoder/idr_handler.{h,cpp}`,
  `server/compositor/compositor.cpp`, `server/compositor/foveation.{h,cpp}`,
  `server/driver/configuration.cpp`, `server/driver/wivrn_session.cpp`, `server/main.cpp`,
  `common/wivrn_packets.h`, `dbus/io.github.wivrn.Server.xml`,
  `external/ffnvcodec/nvEncodeAPI.h` (the vendored NVENC API the server actually compiles against).
- **WiVRn client** (same tree): `client/configuration.{h,cpp}`, `client/scenes/stream.cpp`,
  `client/scenes/stream_gui.cpp`, `client/scenes/lobby_gui.cpp`, `client/scenes/gui_common.cpp`.
- **Upstream WiVRn**: `upstream/master` @ `d747f4dd` (2026-07-26), the `upstream/feat/bitrate` and
  `upstream/ui-overhaul` branches, and the GitHub issue/PR record (§1.7, §4.5).
- **HypXRland**: `src/openxr/OpenXRManager.cpp` (frame thread), `src/openxr/XRMonitorLayer.cpp`,
  `src/render/Renderer.cpp` (damage early-out), `src/config/values/ConfigValues.cpp`
  (`openxr:cursor_redraw_epsilon`), `docs/openxr/01-session-graphics.md`,
  `docs/openxr/02-virtual-monitors.md`.
- **Vendored Monado**: `subprojects/monado/src/xrt/compositor/util/comp_render.h` (the layer
  squasher — this is the load-bearing architectural fact of §3).
- **Prior local work**: `docs/openxr/research/wivrn-tuning/README.md` — the right-eye IDR-drop
  diagnosis that produced the user's current config *and* the 30 Mbit/s client setting.
- **Live box, passive only**: `busctl --user` property reads on `io.github.wivrn.Server`,
  `ss -tni` byte counters on the one established stream socket, `journalctl --user -u wivrn.service`,
  `nvidia-smi` encoder stats. No process was started, stopped, signalled, or reconfigured.

---

## TL;DR — RECOMMENDATION

> **Do two things, in this order.**
>
> **Tonight, zero code:** set **`fps_divider = 2`** in the WiVRn app on the Quest (Settings →
> refresh rate → the "45 Hz" style entry that keeps the display at 90). This is a *shipped
> upstream feature* (PR #862) that halves the encode/transmit rate while the display stays at
> 90 Hz and the client reprojects the gaps. It should roughly halve the stream immediately and it
> costs nothing to try or to revert. Its real value is diagnostic: it tells you how much of your
> perceived quality actually depends on new frames arriving at 90 Hz, which is the single biggest
> unknown gating the harder options below.
>
> **The actual fix, ~5 lines:** add a **QP floor to the existing CBR configuration** in
> `server/encoder/video_encoder_nvenc.cpp::get_rc_params()` — `enableMinQP = 1` with
> `minQP = {qpInterP = 20, qpInterB = 20, qpIntra = 20}` (start at 16 and walk up), plus an
> explicit `enableFillerDataInsertion = 0` on the HEVC config as a belt-and-braces guard. This is
> the smallest change that converts "spend the whole budget always" into "spend up to the budget,
> stop when the picture is already good enough". It **keeps CBR**, so it keeps the VBV, the
> ULTRA_LOW_LATENCY tuning and the entire latency profile *byte-for-byte identical* — the only
> thing that changes is that the encoder is now allowed to undershoot. On a static desktop the
> bitrate should collapse; under scroll/motion it walks straight back up to 30 Mbit/s.
>
> **Do not start with an adaptive-bitrate controller.** Issue #540 wants one and upstream would
> probably take it, but it solves *congestion*, not *waste* — a controller that measures a healthy
> 700 Mbit/s LAN will simply conclude "30 Mbit/s is fine" and change nothing. **Do not use pure
> CQP / `targetQuality` with no cap** either: unbounded peaks on a `tcp-only` transport is
> bufferbloat, and bufferbloat in XR is motion-to-photon latency.
>
> Ranked: **A0 (free, tonight) → A (the fix) → B (damage-gated frame skip, if A isn't enough) →
> D (adaptive controller, different problem) → C, F (don't)**. See §6.

---

## 1. Ground truth — what WiVRn actually does today

Everything in this section is **verified in source** at the stated file and line, and cross-checked
against `upstream/master` where noted.

### 1.1 There is no server-side `bitrate` setting. It is a client setting.

The user's `~/.config/wivrn/config.json` has no `bitrate` field, and that is correct — **the key
does not exist any more**. `server/driver/configuration.cpp:166-241` parses exactly:
`grip-surface`, `encoder`, `application`, `hid-forwarding`, `debug-gui`, `use-steamvr-lh`,
`bit-depth`, `tcp-only`, `port`, `hostname`, `publish-service`, `inhibit`, `openvr-compat-path`.
No `bitrate`.

It used to. Upstream commit **`0b526cef` "configure bitrate on client only"** (2025-12-10) deleted
the `json.find("bitrate")` parse, the dashboard setting, and this stanza from
`docs/configuration.md`:

```
## `bitrate`
Default value: `50000000` (50Mb/s)
Bitrate of the video, in bit/s. Split among decoders based on size and codecs.
```

**Consequence worth internalising: a `"bitrate"` key in `config.json` today is silently ignored.**
No warning, no log line. (`known_keys.json` at `configuration.cpp:52` is about pairing keys, not
config keys.)

The authority is now entirely client-side:

| Fact | Location |
|---|---|
| Default **50 Mbit/s** | `client/configuration.h:65` — `uint32_t bitrate_bps = 50'000'000;` |
| Cap 200 Mbit/s, or 800 with `extended_config` | `client/configuration.h:121-128` |
| In-headset slider clamps to `[1 Mbit/s, max_bitrate()]` | `client/scenes/stream_gui.cpp:485` |
| Shipped to server in the headset info packet | `common/wivrn_packets.h:245` (`settings_changed::bitrate_bps`) |

**Our box is at 30 Mbit/s, not the 50 Mbit/s default** — read live off D-Bus
(`io.github.wivrn.Server.Bitrate = 30000000`, §2). That is not an accident: it is the deliberate
mitigation from `docs/openxr/research/wivrn-tuning/README.md` §5, which told the user to drop the
Quest bitrate 50 → 30 to shrink the IDR burst tail that was losing UDP shards and producing the
right-eye artifacts. **That workaround is now redundant**: the same report's escalation
(`tcp-only: true`) was subsequently adopted, and TCP retransmission eliminates the shard-loss
mechanism outright, so the burst-size argument for staying at 30 no longer applies. Worth flagging,
because it means the *current* 30 Mbit/s ceiling is a legacy artifact and the honest comparison for
any change below is "30 Mbit/s CBR", not "50".

### 1.2 NVENC is hard CBR with no quality ceiling — this is the whole story

`server/encoder/video_encoder_nvenc.cpp:133-142`, verbatim:

```cpp
NV_ENC_RC_PARAMS video_encoder_nvenc::get_rc_params(uint64_t bitrate, float framerate)
{
	return {
	        .rateControlMode = NV_ENC_PARAMS_RC_CBR,
	        .averageBitRate  = static_cast<uint32_t>(bitrate),
	        .vbvBufferSize   = static_cast<uint32_t>(bitrate / framerate * 2.0f),
	        .vbvInitialDelay = static_cast<uint32_t>(bitrate / framerate),
	        .enableLookahead = 0,
	        .lowDelayKeyFrameScale = 1,
	        .multiPass = NV_ENC_TWO_PASS_QUARTER_RESOLUTION};
}
```

and immediately after, `video_encoder_nvenc.cpp:202-223`:

```cpp
GUID presetGUID = NV_ENC_PRESET_P4_GUID;
NV_ENC_TUNING_INFO tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
...
config.rcParams = get_rc_params(bitrate, fps);
config.rcParams.enableTemporalAQ = 0;
config.rcParams.enableAQ         = 1;
config.rcParams.enableNonRefP    = 1;
config.gopLength    = NVENC_INFINITE_GOPLENGTH;
config.frameIntervalP = 1;
```

Read against the vendored `external/ffnvcodec/nvEncodeAPI.h:1525-1595`, the significant **absences**
are:

| Field | Set? | What it would do |
|---|---|---|
| `enableMinQP` / `minQP` | **no** | *"don't spend bits past this quality"* — the missing brake |
| `enableMaxQP` / `maxQP` | **no** | floor on quality (unused; not what we want) |
| `targetQuality` | **no** | "Target CQ (Constant Quality) level for **VBR** mode (range 0-51)" — `nvEncodeAPI.h:1562` |
| `maxBitRate` | **no** | header: *"used for VBR and **ignored for CBR** mode"* — `nvEncodeAPI.h:1533` |
| `aqStrength` | **no** (auto) | AQ aggressiveness 1-15 |
| `zeroReorderDelay` | **no** | already implied by `frameIntervalP = 1` |

A CBR controller with `averageBitRate = 30 Mbit/s` and **no `minQP`** has no defined stopping
point. Its job is to hit 30 Mbit/s. When the residual is tiny it lowers QP; there is nothing
telling it "QP 14 is already past the point of visible return, bank the savings." That is the
mechanism, and it is content-independent by construction.

Two secondary notes on this block:

- **`multiPass = NV_ENC_TWO_PASS_QUARTER_RESOLUTION`** exists specifically to hit the rate target
  accurately. It makes CBR *better at* saturating the budget. (It is also defensible on latency
  grounds: PR #279's measurements showed the extra encode time is roughly repaid in decode time.
  PR #644 tried to remove it and was talked out of it. Leave it alone.)
- **`enableAQ = 1`** (spatial AQ) redistributes bits *within* a frame toward high-detail blocks.
  For a text-heavy desktop this is actively good and should be kept under any option below.

### 1.3 GOP / IDR policy: on-demand only, no periodic keyframes

`config.gopLength = NVENC_INFINITE_GOPLENGTH` with `frameIntervalP = 1` — one IDR at stream start,
then P-frames forever. Keyframes are re-sent only when `default_idr_handler` decides one is needed
(`server/encoder/idr_handler.cpp:28-56`), driven by client feedback: a frame the client reports as
*not* `sent_to_decoder` (i.e. it never reassembled completely) flips the state machine to
`need_idr`, and `should_skip()` then suppresses frames until the new IDR is acknowledged.

**This is good news for the question at hand.** There is no periodic-IDR storm to eliminate, and
intra-refresh (`enableIntraRefresh`, `nvEncodeAPI.h:1845`) — the usual answer to "keyframes are
spiking my bitrate" — has **nothing to fix here**. On `tcp-only` the IDR path should be quiescent
by construction, since TCP always eventually delivers a complete frame; the prior report
(`wivrn-tuning/README.md` §5 escalation) predicted exactly that, and today's journal is consistent
with it (no `IDR frame dropped` lines in the current session).

`enableNonRefP = 1` plus `set_non_ref`/`is_non_ref_frame` (`idr_handler.cpp:88-97`, upstream PR
#644/#652/#653) means losing a *non-reference* P no longer forces a full IDR reset. Already in our
tree.

**ERRATUM (2026-07-27, lever-A implementation follow-up).** The paragraph above is wrong in one
load-bearing place: WiVRn **does** enable intra-refresh — `video_encoder_nvenc.cpp` sets
`intraRefreshPeriod = 100, intraRefreshCnt = 50` for all three codecs (upstream `62543863`). That
is a rolling refresh wave at a **50% duty cycle, forever**, and A/B measurement during the lever-A
work showed it accounts for **~97% of the steady-state cost of a static frame** (static desktop,
min-qp 18: 3.32 Mbit/s with the wave vs 0.10 Mbit/s without). Its purpose is error resilience,
which `tcp-only` largely obviates. Raising `intraRefreshPeriod` (100 → several hundred) is a
one-line change and plausibly a **larger lever than A** — filed as its own investigation (robustness
trade-off: recovery latency after a genuinely corrupted stream on UDP transports must be evaluated
before touching the default).

### 1.4 Three encoders, and how the budget is split

`server/driver/configuration.h:67` — `std::array<encoder, 3> encoders; // left, right, alpha`.
`encoder_settings.cpp:274-305` sizes them; index 2 (the passthrough alpha channel) gets half height.
`compositor.cpp:736-737` creates them with `stream_idx == i`. This mapping is already documented in
`wivrn-tuning/README.md` §1 and is unchanged. (Line numbers there were taken from the v26.6.1
checkout at `~/code/wivrn` and have since drifted; the numbers in *this* report are all against
`~/code/wivrn-26.6.2` @ `hypxr-patches-26.6.2`.)

The split (`encoder_settings.cpp:47-73`): weight = `width × height`, doubled for h264, multiplied by
`passthrough_bitrate_factor = 0.05` for stream 2, then normalised so the three
`bitrate_multiplier`s sum to 1 and `encoder.bitrate = multiplier × total`. With two equal-size
h265 eyes plus a half-height alpha at 5%: **left ≈ 49.4%, right ≈ 49.4%, alpha ≈ 1.2%** of the
30 Mbit/s. Each encoder runs its own independent CBR controller at its own share.

Consequence for §2's arithmetic: the alpha stream *is* active on this box (head-anchored monitors
over passthrough), so essentially the full budget is in play.

### 1.5 Runtime bitrate reconfiguration is fully plumbed — the policy is what's missing

The entire path exists and is codec-agnostic:

```
client GUI / settings_changed packet
  → server/main.cpp:605-608          (sets the GObject "bitrate" property)
  → server/main.cpp:824-829          on_bitrate → to_monado::set_bitrate over the IPC socket
  → server/driver/wivrn_session.cpp:987-990
  → server/compositor/compositor.cpp:785-789   compositor::set_bitrate
  → server/encoder/video_encoder.cpp:225-228   pending_bitrate = bps * bitrate_multiplier  (atomic)
  → picked up on the next encode:
      video_encoder_nvenc.cpp:488-524   (reconfigure + forceIDR)
      video_encoder_vulkan.cpp:854
      ffmpeg/video_encoder_ffmpeg.cpp:72
```

Upstream landed this deliberately: PR #257 (x264/VAAPI, merged 2025-02), issue #539 → **PR #559
"Change bitrate and fps at runtime on NVENC"** (merged 2025-10-27), PR #659 (in-stream GUI,
merged 2025-12-07). Known gap: issue #690, VAAPI doesn't actually honour runtime changes.

**So "make the bitrate move" is already solved. Nobody has written anything that decides *when* to
move it.** That is the shape of the gap, and it is worth being precise about it, because it means
option D (§4.5) is cheap in plumbing and expensive only in policy.

One caveat for anyone imagining an external daemon: the D-Bus `Bitrate` property is
**read-only** — `<property name="Bitrate" type="u" access="read"/>`
(`dbus/io.github.wivrn.Server.xml:48`, identical on master). A controller would have to live
inside the server, or the property would have to become `readwrite`.

Note also: `nvenc`'s reconfigure path forces an IDR (`video_encoder_nvenc.cpp:510`,
`.forceIDR = 1`). Any adaptive controller that nudges the bitrate frequently is therefore also
generating keyframes at that cadence. That is a real constraint on option D and a reason to prefer
§4.1, which changes rate *within* the existing controller and forces nothing.

### 1.6 There is already a per-frame skip path — but it is not content-driven

`video_encoder.h:88-95` defines slot states `idle / busy / skip`, and
`video_encoder.cpp:235-247`:

```cpp
void video_encoder::present_image(...)
{
	present_slot = (present_slot + 1) % num_slots;
	state[present_slot].wait(busy);
	if (idr->should_skip(frame_index)) { state[present_slot] = skip; return; }
	state[present_slot] = busy;
	...
}
```

plus `compositor.cpp:284-289`, which drops the whole frame if the encoder thread hasn't picked up
the previous one. **Both are back-pressure, not content awareness.** Nothing anywhere compares a
frame to its predecessor.

### 1.7 Upstream master changed nothing relevant

`v26.6.2..upstream/master` (@ `d747f4dd`, 2026-07-26) is 81 commits. Restricted to
`server/encoder` + `common/wivrn_packets.h` it is 8 commits, all input/tracking/tracing:
Perfetto instrumentation, body/gamepad forwarding, a `pose_flags` refactor. Verified by diff:

- `encoder_settings.{cpp,h}` — **byte-identical**.
- `video_encoder_nvenc.cpp` — +27 lines, 100% Perfetto scopes. **`get_rc_params()` is unchanged
  on master**: still CBR, still no `minQP`.
- `common/wivrn_packets.h` — 389 lines changed, **zero** hits for
  `bitrate|bandwidth|congest|rate|network|feedback|throttl|quality|adapt`. The `feedback` struct is
  unchanged.
- `docs/configuration.md` — no new encoder knobs. (New file `docs/profiling.md`, Perfetto usage.)

**So upgrading to master buys nothing here.** But two off-master branches matter a great deal:

- **`upstream/feat/bitrate`**, one commit — **`f56dc407` "WIP detect network link capacity"**
  (Patrick Nicolas / the lead maintainer, 2026-06-16, never merged, ~6 weeks stale). It adds
  `uint32_t data_size` to `struct feedback`, accumulates per-shard serialized size in
  `shard_accumulator::push_shard`, and in `client/scenes/stream_gui.cpp::accumulate_metrics`
  computes `cap = data_size / (received_last_packet − received_first_packet)` per eye-stream —
  with an overlap check that merges the two streams into one transmission window when their
  arrival intervals overlap — smooths it (`std::lerp(bandwidth_cap, cap, 0.01)`) and plots it as a
  "Link" series. **Measure-and-display only. Nothing feeds it back into any rate controller.**
  This is precisely the primitive option D needs, half-built by the maintainer himself.
- **`upstream/ui-overhaul`** commit **`3c56ff74` "Remove bitrate setting with the thumbstick"**
  (2026-07-26, i.e. today) deletes the in-stream thumbstick bitrate adjustment and the
  `bitrate_settings` tab, rolling `protocol_revision` 2→1. Upstream is currently moving *away*
  from manual live bitrate control in the headset UI — a mild signal that they'd rather it were
  automatic, and a reason not to build anything that depends on that tab.

The issue record (searched `bitrate`, `adaptive`, `VBR`, `CBR`, `bandwidth`, `congestion`,
`rate control`, `static`, `damage`, `frame skip`):

| Thread | State | Gist |
|---|---|---|
| [#540 Dynamic Bitrate](https://github.com/WiVRn/WiVRn/issues/540) | **open, zero comments since 2025-10** | Asks for min/max bitrate + target latency. *"Most major VR streamers… have some form of Dynamic/Adaptive Bitrate… With WiVRn only supporting static bitrates, some network environments either become constrained to the worst network conditions typically seen, or are rendered entirely unusable."* No maintainer reply in 9 months. **Not rejected — unclaimed.** |
| [#626 Traffic shaping / pacing](https://github.com/WiVRn/WiVRn/issues/626) | open | Maintainer: *"I am not sure this is something desirable, it will require tuning in order to not add latency and may highly depend on the network configuration. **If you have a working prototype I can consider it, but I will not invest time writing one.** … In general, I don't think chasing such high bitrates is a good idea."* = PRs welcome, won't build it. |
| PR #862 "add option for half fps" | **merged** | Became `fps_divider`. The one shipped bandwidth-reduction lever. |
| [#618 Stream lags when head is held still](https://github.com/WiVRn/WiVRn/issues/618) | open | The *inverse* symptom — 300-500 ms latency spikes when the head is still, cleared by moving. Thread went to Wi-Fi troubleshooting; nobody investigated a static-content interaction. Reporter found it **worse at lower bitrates** (15 s to trigger at 5 Mbit/s vs 2 min at 65). **Read this as a warning label on every option that makes the stream go quiet.** |
| [#694 Bitrate cap](https://github.com/WiVRn/WiVRn/issues/694), [#1031](https://github.com/WiVRn/WiVRn/issues/1031) | closed | Cap fights; #1031 rejected as a Vulkan spec violation. |

**Frame skipping / repeat-frame signalling / "don't encode unchanged content": zero threads.
Desktop-as-a-use-case: essentially absent.** Nobody upstream has framed the problem the way this
report does.

---

## 2. Live measurement — what we actually observed

**Method (strictly passive).** One `ESTAB` socket carries everything (`tcp-only: true`), so
`bytes_sent` on it is the whole downstream: `192.168.50.189:9757 → 192.168.50.233:45828`,
`wivrn-server` pid 105163 fd 12. Sampled `ss -tni` at 1 Hz for ~10 minutes; correlated with
`journalctl` wear transitions (the local `inhibit: worn` patch conveniently logs every don/doff)
and with D-Bus property reads.

**Configuration, read live:**

```
$ busctl --user introspect io.github.wivrn.Server /io/github/wivrn/Server
  .Bitrate               property  u        30000000
  .PreferredRefreshRate  property  d        90
  .SessionRunning        property  b        true
  .HeadsetConnected      property  b        true
  .SystemName            property  s        "Meta Quest 3"
  .SupportedCodecs       property  as       3 "av1" "h265" "h264"
  .JsonConfiguration     property  s        "{\"bit-depth\":8,\"encoder\":{\"codec\":\"h265\",
                                              \"encoder\":\"nvenc\"},\"inhibit\":\"worn\",\"tcp…"
$ nvidia-smi --query-gpu=encoder.stats.sessionCount --format=csv
  3                       # left + right + alpha, as predicted by §1.4
```

**The number.** Session up at 22:33:21. At the first sample (22:40:52.9) the socket reported
`bytes_sent = 1,405,871,361` and `busy = 421,536 ms` of 451.9 s elapsed (**93.3 % of wall-clock
spent with data queued** — the connection is essentially never idle). Two doff windows inside that
span (22:39:51→22:40:01 and 22:40:10→onward, from the journal) total 52.9 s at the measured
0.59 Mbit/s non-video floor ≈ 3.9 MB. Netting those out:

| | |
|---|---|
| Active streaming window | 399 s |
| Payload sent in it | ≈ 1,402 MB |
| **Sustained mean** | **28.1 Mbit/s** |
| Configured CBR target | 30.0 Mbit/s |
| **Fraction of target** | **94 %** (≈ 92 % for video alone, netting out the ~0.6 Mbit/s audio floor) |

The workload during that window was the user's ordinary desktop — terminals, editor, browser.
**A mostly-static desktop drew 94 % of the configured constant-bitrate budget, sustained, for
6½ minutes.** That is the ground truth the whole report rests on, and it is exactly what §1.2
predicts.

**The doffed floor.** From 22:40:10 onward the rate drops to a rock-steady **0.58-0.60 Mbit/s**
and stays there (verified across ~450 consecutive 1 Hz samples, plus two brief ~2.2 Mbit/s
excursions of ~40 s). This is the no-video floor: audio + tracking + control. It is a useful
datum in its own right — it confirms that **when the OpenXR session stops being visible, the
encode/transmit path goes fully quiet.** `wivrn_session.cpp:759-778` maps
`XR_SESSION_STATE_SYNCHRONIZED` to `visible = false`; Hyprland stops submitting; `layer_commit` is
never called; nothing is encoded. The "stop sending frames" machinery therefore already works end
to end at session granularity — §4.2 is asking to exercise the same path at frame granularity.

**What we could NOT measure, and it matters.** The headset stayed doffed for the entire
observation window, so **we have no instantaneous static-vs-scrolling A/B trace.** The 28.1 Mbit/s
figure is a windowed cumulative average, not a content-correlated time series. It is strong
evidence that the mean is pinned near target; it is *not* direct evidence about the variance. In
practice this does not change any conclusion — §1.2 makes the mechanism unambiguous from source
alone, and a CBR controller with a 2-frame VBV has very little room for variance by construction —
but the honest statement is: **mechanism verified in source; magnitude verified by cumulative
measurement; per-second content correlation not obtained.** §8 gives the recipe to get it.

Link health, for the record: `rtt 3.2-4.9 ms` (`minrtt 1.32`), `retrans 0/20` over the whole
session, `bytes_retrans 8,957` of 1.4 GB (0.0006 %), `cwnd` clamped small and `app_limited` set in
the idle state. **The network is not the constraint.** Whatever we send, this LAN will carry —
which is precisely why an adaptive-bitrate controller (option D) would find nothing to do.

---

## 3. Why a static desktop is *not* a static encoder input

This is the part that is easy to get wrong, and it kills the naive version of the obvious idea.

**Verified, compositor side: HypXRland's damage gating is already complete.** A static desktop
costs zero GPU work in Hyprland and zero swapchain writes. The chain:

1. The XR frame thread (`src/openxr/OpenXRManager.cpp:1216` `frameThread()`) enqueues
   `SCHEDULE_FRAMES` each iteration (`:1326-1327`); the main thread calls `scheduleFrame()` on each
   XR monitor (`:1099-1110`). That *schedules* a frame callback; it does not set `needsFrame`.
2. `src/render/Renderer.cpp:2044` early-returns when
   `!needsFrame && m_forceFullFrames == 0 && !m_damage.hasChanged()`. No damage → no render, no
   commit.
3. No commit → no aquamarine `present` → `XRMonitorLayer`'s `presented` listener
   (`src/openxr/XRMonitorLayer.cpp:32-56`) never stashes a buffer.
4. `OpenXRManager.cpp:1458`: `if (!buf && !wantAnimTick && l->m_hasContent) continue;` — no
   acquire, no blit, no release.

There is even prior art aimed squarely at this problem: the **cursor redraw dead-band**
(`OpenXRManager.cpp:1424-1431`, `XRMath.hpp:1239-1252`, config `openxr:cursor_redraw_epsilon` at
`src/config/values/ConfigValues.cpp:859-863`), added because idle hover tremor was forcing
full-swapchain redraws and producing a "dropped-IDR / macroblock storm" (report 14, live).

**And yet the quad layer is still submitted every single frame.** `OpenXRManager.cpp:1729-1787`
builds the `XrCompositionLayerQuad` unconditionally (gated only on
`m_quadActive && m_hasContent && solved`) and `xrEndFrame` runs at `:1892` every iteration. A quad
layer re-presents its most recently released swapchain image forever — stated explicitly in
`docs/openxr/01-session-graphics.md:295`. (Also noted: `xrWaitFrame`'s own `shouldRender` hint is
never read — grep for `shouldRender` in `src/openxr/` is empty.)

**Verified, runtime side — the load-bearing fact.** HypXRland submits **quad layers**, not a
stereo projection layer. It never renders eye buffers. The stereo composition is done by the
runtime: `subprojects/monado/src/xrt/compositor/util/comp_render.h:35,65-66,161-190` — the "layer
squasher" renders the app's layer array into per-view scratch images **using that frame's head
pose**, every frame. `comp_render.h:217`: the `fast_path` that avoids squashing applies only to a
single projection layer; **quad layers always go through the squasher.**

On top of that, WiVRn applies **dynamic foveation** before encode
(`compositor.cpp:411` `foveation.foveate(...)`): a nonlinear resample whose warp centre is
recomputed from the current head/gaze orientation each frame
(`foveation.cpp:339-376` `compute_params`, fed by `foveation::update_tracking` at `:412`; with no
eye tracking on a Quest 3, `is_zero_quat(gaze)` is true and it falls back to head direction plus a
10° natural-gaze pitch offset).

**Therefore: the encoder input is the composited per-eye view, warped by a gaze-dependent
foveation grid. It is a function of head pose, and head pose is never exactly constant** — IMU
noise alone guarantees sub-pixel motion. Hyprland's "nothing changed" is real, and already fully
exploited, but **it stops at the quad texture and never reaches the encoder input.**

**Inferred (well-founded, not measured):** what that head motion produces is a *global, smooth,
sub-pixel warp* of an otherwise identical image. That is the single friendliest possible case for
HEVC inter prediction — quarter-pel motion compensation with a near-uniform motion field and tiny
resampling residuals. Such a frame *ought* to code in single-digit Mbit/s at any sane QP. It codes
at 28 Mbit/s because **nothing tells the rate controller to stop**, not because the content is
genuinely expensive. This is the reasoning behind the §4.1 recommendation, and §8 says how to
falsify it.

---

## 4. The levers

### 4.1 Quality-capped rate control — add a QP floor (**the recommended fix**)

The minimal change. Keep CBR; add the missing brake.

```cpp
// server/encoder/video_encoder_nvenc.cpp
NV_ENC_RC_PARAMS video_encoder_nvenc::get_rc_params(uint64_t bitrate, float framerate)
{
	return {
	        .rateControlMode = NV_ENC_PARAMS_RC_CBR,          // unchanged
	        .averageBitRate  = static_cast<uint32_t>(bitrate),// unchanged
	        .vbvBufferSize   = static_cast<uint32_t>(bitrate / framerate * 2.0f), // unchanged
	        .vbvInitialDelay = static_cast<uint32_t>(bitrate / framerate),        // unchanged
	        .enableMinQP     = 1,                             // NEW
	        .enableLookahead = 0,
	        .lowDelayKeyFrameScale = 1,
	        .minQP = {.qpInterP = 20, .qpInterB = 20, .qpIntra = 20},  // NEW  (field is qpIntra)
	        .multiPass = NV_ENC_TWO_PASS_QUARTER_RESOLUTION};
}
```

plus, as a guard (§7 step 2), an explicit
`config.encodeCodecConfig.hevcConfig.enableFillerDataInsertion = 0`.

**Why this and not VBR.** `enableMinQP`/`minQP` are generic rate-control parameters
(`nvEncodeAPI.h:1536,1554`) — unlike `constQP` and `temporalLayerQP`, whose doc comments explicitly
say *"Applicable only for constant QP mode"*, and unlike `maxBitRate`, whose comment says
*"used for VBR and **ignored for CBR**"*. So a QP floor works **inside CBR**. That matters:

- **The latency profile does not move at all.** Same VBV (2 frames), same `vbvInitialDelay`
  (1 frame), same `NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY`, same P4 preset, same `enableLookahead = 0`.
  NVIDIA's low-latency tunings are documented and validated against CBR; switching to VBR would put
  us on a less-travelled path for no additional benefit.
- **No credit banking.** A worry with VBR is that undershooting now licenses overshooting later.
  With a 2-frame VBV there is nowhere to bank; the leaky bucket is one frame deep. Staying in CBR
  makes this a non-question rather than a thing to reason about.
- **It is a strictly smaller diff.** ~5 lines, one function, no semantic change to any other field.
  That matters for upstreaming (§6).

**The one thing that must be checked, not assumed.** `enableFillerDataInsertion`
(`nvEncodeAPI.h:1847`) *"will take effect only when **CBR** rate control mode is in use and both
`frameRateNum` and `frameRateDen` are set to non-zero values."* WiVRn **does** set both
(`video_encoder_nvenc.cpp:145-149` `set_init_params_fps`). The code takes the whole preset config
wholesale (`config = preset_config.presetCfg;` at `:214`) and only overrides `rcParams` and
`gopLength` — **it never touches `encodeCodecConfig`.** If the P4/ULTRA_LOW_LATENCY preset happens
to set that bit, the encoder would pad the bitstream back up to target with filler NALs and the QP
floor would save *nothing on the wire*. We could not determine the preset's default without running
the encoder. **Explicitly zeroing it is a one-line, zero-risk guard and should be part of the same
patch.** (Corollary: if a first attempt at this option shows QP rising but bitrate flat, this bit
is the prime suspect.)

**Choosing the floor.** `minQP` only binds when the controller *wanted* to go finer than it, so a
conservative value is free: it cannot degrade any frame that was already coded at a coarser QP.
Start at **16** (visually transparent for HEVC 8-bit; should already cut a lot), then walk up in
steps of 2 toward ~22, stopping at the first step where small text softens on a stationary read.
Text legibility is the acceptance criterion, not a PSNR number (§5).

**Expected saving:** unknown in magnitude, and this report deliberately does not guess a number —
§8 measures it. Directionally, on genuinely static content the bitrate should fall by a large
multiple, and under scroll/window-drag it should return to ~30 Mbit/s within a frame or two,
because the budget is untouched and only the *floor on quality* changed.

**Risks:** (a) the filler-data bit above; (b) over-tight floor visibly softening text — mitigated
by starting at 16; (c) `enableAQ = 1` interacts with `minQP` (AQ's per-block QP offsets are applied
around the base QP and are then clamped by the floor) — this slightly blunts AQ's ability to spend
extra bits on text blocks, which argues for the conservative end of the range; (d) issue #618 —
see §5.

### 4.2 Damage-gated frame skip

The idea the compositor work in §3 makes tempting. Two places it could live.

**B — in HypXRland, no WiVRn change at all (the interesting one).** The fork already knows, per
frame, that nothing changed: `OpenXRManager.cpp:1458` is exactly that predicate. Today it
`continue`s past the swapchain work but still submits the quad and calls `xrEndFrame`. If instead
the frame thread *throttled the whole wait/begin/end cycle* when
`(no layer released a swapchain image) && (head-pose delta below threshold)`, then `layer_commit`
is simply never called, nothing is encoded, and the Quest reprojects its last frame. **Zero
protocol change, zero WiVRn change, entirely inside our repo.**

The reason to take this seriously is that **`fps_divider` already proves the client tolerates it.**
Upstream PR #862 shipped a feature that halves the encode/transmit rate while the display stays at
its refresh rate and the client covers the gaps. A damage-adaptive divider is the natural
generalisation of a mechanism upstream has already validated — and, conveniently, `fps_divider = 2`
lets us test the *client-side* half of the hypothesis tonight with no code at all (§7 step 1).

Risks, honestly: (a) skipping `xrWaitFrame` is the app's own pacing primitive — throttling it may
perturb Monado's frame timing, predicted display times, and WiVRn's pacer
(`server/compositor/pacer.cpp`); (b) **issue #618** is a live report of latency spiking to
300-500 ms specifically when the head is held still, worse at lower bitrates — i.e. the exact
regime this option deliberately creates, and it is unexplained upstream; (c) the wake-up path on
first damage or first head motion must be immediate, or the user feels a hitch on every scroll;
(d) the server-side composited pose goes stale by up to N frames, which the client's timewarp
corrects for rotation but not for translation.

**C — in WiVRn's compositor.** Same predicate, evaluated server-side: "no swapchain image was
released for any submitted layer since the last frame" is observable in the runtime without any
app cooperation, and would benefit every WiVRn client, not just ours. But it means patching the
Monado fork inside WiVRn, it inherits all of B's risks plus a much harder review, and there is
**no upstream discussion of the idea whatsoever** to build on. Strictly worse than B as a first
move.

### 4.3 `fps_divider` — shipped, free, and the right first experiment

`common/wivrn_packets.h:244` `uint32_t fps_divider = 1;`, client default at
`client/configuration.h:93`, GUI at `client/scenes/gui_common.cpp:62-90`, applied server-side at
`server/compositor/compositor.cpp:543` (`*hz = frame_rate * settings->fps_divider`) and
`wivrn_session.cpp:463,491,836`. From upstream PR #862 "add option for half fps".

Halves the encode and transmit rate at the source. **Costs nothing to try and nothing to revert.**
Its diagnostic value is the point: it isolates "do I need 90 new frames per second?" from "do I
need 30 Mbit/s per frame-second?", and the answer determines whether §4.2 is worth building at all.

### 4.4 Foveation and stream scale — already on, and a trap for this use case

WiVRn already does dynamic foveated encoding (§3). The strength is implicit in the ratio of the
client's `stream_eye_width/height` to the render size; `configuration::set_stream_scale` is the
client-side knob.

**Turning either of these up is the wrong lever for a desktop.** Foveation trades peripheral
resolution for bits, and on a Quest 3 there is no eye tracking — the "fovea" tracks *head*
direction, not gaze. Reading a terminal in the corner of your view while facing forward is
precisely the case foveation degrades. For text-first XR, foveation strength and stream scale
should be treated as things to leave alone (or reduce), and the bits recovered elsewhere.

### 4.5 An adaptive bitrate controller

The thing issue #540 asks for: measure the link, move `bitrate_bps`. Plumbing is done (§1.5) and
the measurement primitive is half-built on `upstream/feat/bitrate` (§1.7).

**It is the wrong first move for this problem, and it is worth being blunt about why.** An ABR
controller answers *"how much can this link carry?"*. Our link carries 700 Mbit/s at 1.3 ms with a
0.0006 % retransmit rate (§2). It would measure that, conclude 30 Mbit/s is comfortable, and change
nothing. The complaint is not "the network can't keep up" — it is "we are spending 30 Mbit/s to
send a still image." That is a *rate-distortion* question, and §4.1 is its answer.

There is also a mechanical cost: NVENC's reconfigure path forces an IDR
(`video_encoder_nvenc.cpp:510`). A controller that adjusts frequently generates keyframes at that
frequency — on a link where keyframe bursts were already the historical failure mode
(`wivrn-tuning/README.md` §3).

It remains genuinely worth building **later**, and it has the best upstream story of anything here
(#540 open and unanswered; #626's *"if you have a working prototype I can consider it"*;
`feat/bitrate` stale and adoptable). Just not first.

### 4.6 Levers that don't apply

- **Intra-refresh** (`enableIntraRefresh`, `nvEncodeAPI.h:1845`) — the standard fix for periodic-IDR
  spikes. WiVRn has no periodic IDRs (§1.3). Nothing to fix.
- **`maxBitRate`** — ignored in CBR (`nvEncodeAPI.h:1533`). Only meaningful if we move to VBR.
- **Pure CQP / uncapped `targetQuality`** — see §6 option F. Unbounded peaks into a `tcp-only`
  transport is a latency bug waiting to happen (§5).
- **Removing `multiPass`** — tried upstream (PR #644) and withdrawn; net-neutral to negative on
  end-to-end latency (PR #279).
- **Multi-encoder / offloading an eye to the iGPU** — already considered and rejected in
  `wivrn-tuning/README.md` §5; the bottleneck is the serialized single-sender transmit, not encode.

---

## 5. Latency and quality trade-offs specific to desktop XR

**Text legibility is the acceptance criterion.** Not PSNR, not SSIM, not "does it look fine in a
game". The test is: small terminal text, held stationary, read comfortably. Every QP floor
candidate in §4.1 must be judged that way, and only that way.

**Rate dips must not smear on scroll.** The failure mode to watch for is a controller that has
settled into a low-bitrate steady state and then reacts sluggishly when a full-screen scroll
starts. §4.1 is structurally immune: `averageBitRate` never changes, so the budget is instantly
available the moment the residual grows; only the *floor* was added. This is a real advantage over
option D, where the controller has actively lowered the ceiling and must ramp it back — and ramping
it back costs an IDR.

**IDR behaviour on scene change.** With `NVENC_INFINITE_GOPLENGTH` there is no scene-change
keyframe at all (no lookahead, so no `disableIadapt` path). A hard cut is absorbed as an expensive
P-frame, which the VBV spreads over a couple of frames. That is the correct behaviour for this
transport and should not be disturbed.

**TCP-only changes which failure mode you get.** With `tcp-only: true` there is no shard loss and
therefore no IDR-drop mechanism (§1.3) — the historical right-eye artifact class is gone by
construction. What replaces it is **queueing**: bytes above the instantaneous link rate do not get
dropped, they get buffered, and buffered bytes are motion-to-photon latency. So on this transport,
**bitrate peaks are a latency problem, not an artifact problem.** Two consequences:

1. Any option that *lowers* the average (§4.1) also lowers mean queue occupancy — a latency win on
   top of the bandwidth win.
2. Any option that permits *unbounded* peaks (uncapped CQP) is strictly worse here than it would be
   on UDP. Hence option F's rating in §6.

**The #618 warning.** [Issue #618](https://github.com/WiVRn/WiVRn/issues/618) reports latency
spiking to 300-500 ms when the head is held still, cleared by moving, and **worse at lower
bitrates** (15 s to trigger at 5 Mbit/s vs 2 min at 65 Mbit/s). It is unexplained upstream and the
thread was diverted into Wi-Fi troubleshooting. Every option in this report drives the system
*toward* that regime — low bitrate, static content, possibly sparse frames. This is not a reason
to avoid them, but it is a specific thing to watch for during validation, and if it reproduces,
tracking it down is probably a more valuable contribution to WiVRn than any of the features here.

**Foveation cuts against text** — §4.4.

---

## 6. Options

Effort: XS ≈ minutes, S ≈ a day, M ≈ a few days, L ≈ a week+.

| # | Option | Where | Effort | Upstreamable | Risk | What it buys |
|---|---|---|---|---|---|---|
| **A0** | **Set `fps_divider = 2` on the Quest** | client GUI, **zero code** | **XS** | n/a (shipped, PR #862) | **low** | ~½ the stream immediately; and the answer to "do I need 90 new fps?", which gates B |
| **A** | **QP floor: `enableMinQP` + `minQP` (stay in CBR), + `enableFillerDataInsertion = 0`** | `video_encoder_nvenc.cpp:133-142`, ~5 lines | **S** | **high** — smallest possible diff, no behaviour change for anyone who doesn't hit the floor; a `min-qp` config key would make it opt-in | **low-med** — filler-data bit unknown (§4.1); floor too tight softens text | Bitrate tracks content. The actual fix. |
| **B** | Damage + pose-gated frame skip in HypXRland's XR frame loop | `OpenXRManager.cpp` around `:1458`/`:1892` | **M** | n/a (our fork) | **med** — pacer/frame-timing perturbation, #618, wake-up latency | Near-zero cost when truly idle; complements A rather than replacing it |
| **C** | Same predicate inside WiVRn's compositor | WiVRn's Monado fork + `compositor.cpp` | **L** | low-med — no upstream discussion at all | **high** | Same as B, for everyone. Strictly worse as a first move. |
| **D** | Adaptive bitrate controller on feedback / link capacity | WiVRn server; adopt `f56dc407` | **L** | **med-high** — #540 open & unanswered, #626 "prototype welcome" | med — reconfigure forces IDR (`:510`); tuning-heavy per #626 | Solves *congestion*. Our link isn't congested (§2). Right thing, wrong problem, later. |
| **E** | Just lower the client bitrate / raise foveation / lower stream scale | client GUI | XS | n/a | low | Linear trade; pays in quality on *every* frame including the ones that needed the bits. Foveation actively hurts text (§4.4). |
| **F** | Pure CQP / uncapped `targetQuality` | `video_encoder_nvenc.cpp` | S | low | **high** | Biggest theoretical saving, but unbounded peaks into `tcp-only` = bufferbloat = latency (§5). If ever pursued, only as VBR + `maxBitRate` cap, never uncapped. |

---

## 7. Recommendation

**Step 1 — tonight, no code.** Set `fps_divider = 2` in the WiVRn app on the Quest. Note the new
steady-state bitrate (§8's sampler makes this a one-liner) and, more importantly, whether 45 new
frames per second is subjectively acceptable for desktop work. Revert if not. **This is the
cheapest information available** and it determines whether option B is worth building.

**Step 2 — the patch.** In `server/encoder/video_encoder_nvenc.cpp`:

1. `get_rc_params()`: add `enableMinQP = 1` and `minQP = {20, 20, 20}` (start 16, walk up by 2).
   Change nothing else — `rateControlMode` stays `NV_ENC_PARAMS_RC_CBR`.
2. After `config = preset_config.presetCfg;` (`:214`), explicitly set
   `config.encodeCodecConfig.hevcConfig.enableFillerDataInsertion = 0` (and the h264 equivalent if
   that path is ever used). Non-negotiable — without it the whole change may be a no-op on the wire
   (§4.1).
3. For upstreaming, put the floor behind a config key (`"min-qp"`, or per-encoder in the existing
   `encoder.options`) defaulting to today's behaviour (floor disabled). A knob nobody has to opt
   into is a far easier review than a changed default, and it directly serves #540's constituency.

Rebuild the server, restart **out of session**, validate per §8.

**Step 3 — only if steps 1-2 leave you unsatisfied.** Build option B in our fork: extend the
existing `OpenXRManager.cpp:1458` predicate with a head-pose dead-band (the
`openxr:cursor_redraw_epsilon` precedent at `XRMath.hpp:1239-1252` is the pattern to copy) and
throttle the frame loop rather than submitting an unchanged quad. Instrument the wake-up path
first; measure `#618`-style stalls explicitly.

**Explicitly deferred:** option D. Revisit it if the user ever streams over congested Wi-Fi or off
the LAN, at which point adopting `f56dc407` and writing the controller on top is a genuinely
attractive upstream contribution — with #626's *"if you have a working prototype I can consider
it"* as the stated price of entry.

**Explicitly rejected:** option F uncapped, and turning up foveation/stream-scale to save bits.

**Free adjacent win, unrelated to any of the above:** the client is at **30 Mbit/s** only because
of the UDP-era IDR-drop mitigation in `wivrn-tuning/README.md` §5, and that mitigation was
superseded by `tcp-only: true`, which removes the loss mechanism entirely (§1.1). Once option A is
in — i.e. once bitrate is a *ceiling* rather than a *target* — raising the client back toward
50 Mbit/s costs nothing on static content and buys headroom for the frames that genuinely need it.
**That inversion is the real prize: today the number is what you always pay; after A it is only
what you're allowed to pay.**

---

## 8. Validation plan

Everything here is passive except the two server restarts, which must be done **out of session**.

**Bitrate on the wire (passive, works right now, no restart).** One socket carries everything under
`tcp-only`:

```sh
ss -tnp | grep wivrn-server            # find the ESTAB pair
# then, 1 Hz:
while :; do echo "$(date +%s.%N) $(ss -tni | grep -A1 ':9757' | tr '\n' ' ')"; sleep 1; done \
  | tee /tmp/wivrn-rate.log
# differentiate bytes_sent:
awk '{ts=$1; for(i=1;i<=NF;i++) if($i~/^bytes_sent:/){split($i,a,":");b=a[2]}
      if(pt) printf "%s %.2f Mbps\n", strftime("%H:%M:%S",int(ts)), (b-pb)*8/(ts-pt)/1e6;
      pt=ts; pb=b}' /tmp/wivrn-rate.log
```

**The A/B this report could not run.** With the headset **on**, alternate 60 s of a completely
static desktop against 60 s of continuous scrolling in a terminal, and diff the traces. Under
today's CBR the two should be indistinguishable near 30 Mbit/s — that is the falsifiable prediction
of §1.2. After option A they must diverge sharply. **This single experiment is the highest-value
thing to run**, before and after the patch.

**Per-frame sizes and actual QP.** `server/encoder/video_encoder.cpp:163` reads
**`WIVRN_DUMP_VIDEO`** and opens a per-stream bitstream dump (`:183`, written at `:306-307`).
Set it in the systemd override, restart out of session, capture 30 s each of static and scrolling,
then:

```sh
ffprobe -v error -select_streams v -show_entries frame=pkt_size,pict_type -of csv dump.stream0.h265
ffmpeg -debug qp -i dump.stream0.h265 -f null - 2>&1 | grep -c 'New frame'   # QP trace
```

This is what actually answers "is the QP floor binding, and is filler data eating the savings"
(§4.1). Unset the variable afterwards — it writes unbounded files.

**End-to-end timing.** `WIVRN_DUMP_TIMINGS` (`server/driver/wivrn_session.cpp:334`) dumps the
`feedback` timestamp chain — `encode_begin/end`, `send_begin/end`,
`received_first/last_packet`, `sent_to_decoder`, `received_from_decoder`, `blitted`, `displayed`,
`times_displayed` (`common/wivrn_packets.h:487-505`). Use it to confirm no latency regression, and
to watch for #618-shaped stalls. `times_displayed > 1` is the client reusing a frame — the direct
signal for option B's viability.

**Config sanity, any time, passive:**

```sh
busctl --user introspect io.github.wivrn.Server /io/github/wivrn/Server | grep -E 'Bitrate|Refresh'
journalctl --user -u wivrn.service | grep -A6 'Encoder configuration'   # print_encoders, encoder_settings.cpp:77-88
nvidia-smi --query-gpu=utilization.encoder,encoder.stats.sessionCount --format=csv
```

(Note: no `Encoder configuration:` block appeared in the current session's journal despite
`print_encoders` being `U_LOG_I` at `compositor.cpp:735`. Unexplained — possibly a log-level or
ordering effect in the child process. Worth a glance on the next restart, since it is the canonical
confirmation that a config edit took.)

**Acceptance criteria for option A:** (1) static-desktop bitrate falls substantially and scrolling
returns to ~30 Mbit/s within a frame or two; (2) small terminal text held stationary is
indistinguishable from today; (3) no regression in the `WIVRN_DUMP_TIMINGS` end-to-end latency
distribution; (4) no new `IDR frame dropped` lines.

---

## 9. What we verified vs. what we infer

**Verified in source (file:line given throughout):** NVENC runs `NV_ENC_PARAMS_RC_CBR` with no
`minQP`/`maxQP`/`targetQuality`/`maxBitRate`; `maxBitRate` is ignored in CBR and `minQP` is not
mode-restricted; infinite GOP with feedback-driven IDR only; the three-stream layout and the
weight-based bitrate split; the client-only `bitrate_bps` with a 50 Mbit/s default and the removal
of the server key in `0b526cef`; the fully-plumbed runtime `set_bitrate` path and the read-only
D-Bus property; `fps_divider`'s existence and wiring; dynamic head-driven foveation before encode;
that `upstream/master` changes nothing rate-related and that `upstream/feat/bitrate` `f56dc407`
measures link capacity without feeding it back; HypXRland's complete damage early-out and its
unconditional quad submission; Monado's layer squasher compositing per-eye views from the frame's
head pose.

**Verified by live passive measurement:** `Bitrate = 30,000,000` and `PreferredRefreshRate = 90`
off D-Bus; three live NVENC sessions; 28.1 Mbit/s sustained mean over a 399 s active window against
a 30.0 Mbit/s target (94 %), with the TCP connection busy 93.3 % of wall-clock; a 0.58-0.60 Mbit/s
non-video floor when the session goes non-visible; a healthy link (1.3 ms minrtt, 0.0006 %
retransmits).

**Inferred, not proven:**

- That the bits are going into *imperceptible* quality rather than genuinely hard content — i.e.
  that a head-pose-warped static desktop is cheap to code and only appears expensive because the
  controller has no stopping rule (§3). Strongly implied by the mechanism, **not measured**.
  §8's QP trace settles it.
- The *magnitude* of option A's saving. Deliberately not guessed.
- Whether the P4/ULTRA_LOW_LATENCY preset enables `enableFillerDataInsertion`. Unknown; hence the
  explicit guard in §7 step 2.

**Not obtained:** the instantaneous static-vs-scrolling bitrate correlation. The headset was doffed
for the entire observation window, so §2's figure is a cumulative average over a mixed-but-typical
workload, not a content-correlated time series. It establishes that the mean is pinned near target;
it does not directly bound the variance. §8 gives the recipe, and running it is the first thing to
do before touching any code.
