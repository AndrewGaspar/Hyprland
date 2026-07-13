# WiVRn right-eye IDR-drop diagnosis + proposed encoder config

**Box:** Quest 3 via WiVRn **v26.6.1** (`/usr/bin/wivrn-server`, `wivrn.service`
user unit). Host RTX 5070 Laptop (NVENC) + AMD 890M iGPU. WiFi excellent
(−56 dBm, 700 Mbit/s). **Symptom:** persistent compression artifacts / lag
localized to the **RIGHT EYE**; server journal shows continuous
`WARN IDR frame dropped, stream 1` and `stream 2` — **never stream 0**.
No user `config.json` exists → stock defaults.

All source refs below are the user's read-only checkout at `~/code/wivrn`
(v26.6.1). Line numbers are from that tree.

---

## 1. Stream → region → encoder mapping

WiVRn's default layout is **exactly three encoders**, one per stream, in a
fixed order:

```
server/driver/configuration.h:54
    std::array<encoder, 3> encoders; // left, right, alpha
```

`get_encoder_settings()` builds those three from the per-eye stream size and
assigns each a role by index:

```
server/encoder/encoder_settings.cpp:296-305
    // Ensure we don't try to encode too large images (only for left/right, ignore alpha)
    for (size_t i = 0; i < 2; ++i)
        check_video_size(res[i].encoder_name, res[i].codec, width, height);
    ...
    if (i == 2) // alpha channel
        dst.height /= 2;
```

The compositor then creates them with `stream_idx == i`:

```
server/compositor/compositor.cpp:744-745
    for (auto [i, settings]: std::ranges::enumerate_view(settings))
        encoders[i] = video_encoder::create(vk, settings, i);
```

So the mapping is unambiguous:

| stream_index | region                    | notes                                   |
|:------------:|---------------------------|-----------------------------------------|
| **0**        | **LEFT eye**              | full per-eye size                        |
| **1**        | **RIGHT eye**             | full per-eye size                        |
| **2**        | **ALPHA** (passthrough)   | half height; ~5 % of the bitrate budget |

**The user's "right eye" symptom is `stream 1` by construction.** `stream 2`
is the passthrough alpha channel (active because the user runs head-anchored
monitors over passthrough / boundaryless).

### Encoder backend picked on this box
`prober::select_encoder` (`encoder_settings.cpp:221-265`): the compositor's
Vulkan device is the **NVIDIA** GPU (the XR session runs on it), so
`is_nvidia()` is true and, with no user config, **all three streams use
`nvenc`** (`encoder_settings.cpp:227-234`). Codec is the first entry of the
headset's `supported_codecs` list that nvenc can do
(`docs/configuration.md:36` — "best supported by both of av1, h264, h265").
Bit depth defaults to 8 for h264/raw, else 10 (`encoder_settings.cpp:307-316`).

### What "stream N" is on the wire
Each encoder is an independent video stream with its own frame sequence and its
own **independent decoder + reassembly window on the Quest**
(`client/decoder/shard_accumulator.*`). Frames are fragmented into
MTU-sized "shards" and sent over **UDP** (`common/wivrn_sockets.cpp:71`,
`SOCK_DGRAM`, AES-128-CTR encrypted).

---

## 2. What "IDR frame dropped" means (precise, with code)

It is **NOT** an encoder-queue, pacing, or server-send failure. It is the
**client reporting that it never had a complete frame to decode**, fed back to
the server.

**Client side** — `sent_to_decoder` is stamped **only when every data shard of
the frame arrived**:

```
client/decoder/shard_accumulator.cpp:188-195
    bool frame_complete = last_idx == data_shards.size() and data_shards.back()->timing_info;
    decoder_->push_data(payload, data_shards[shard_idx]->frame_idx, not frame_complete);
    if (not frame_complete)
        return;                                  // <-- sent_to_decoder stays 0
    current.feedback.received_last_packet = instance.now();
    current.feedback.sent_to_decoder    = current.feedback.received_last_packet;
```

When a later frame arrives before the current one completes, the incomplete
frame's feedback is shipped back **with `sent_to_decoder == 0`**
(`shard_accumulator.cpp:130-159`, the `frame_diff >= 1` paths call
`send_feedback(current.feedback)` without ever completing it).

**Server side** — the IDR handler reads that feedback:

```
server/encoder/idr_handler.cpp:34-46
    [this, &f](wait_idr_feedback s) {
        if (f.frame_index == s.idr_id) {
            if (f.sent_to_decoder) { ... "IDR frame received" ... }
            else {
                U_LOG_W("IDR frame dropped, stream %d", f.stream_index);
                state = need_idr{};
            }
        }
    }
```

**So `IDR frame dropped, stream N` = the Quest received an incomplete IDR
(keyframe) for stream N — at least one UDP shard was lost — so it discarded the
frame and the server must resend a fresh keyframe.** Until that keyframe lands
intact, that eye keeps showing stale / corrupt reference data → the persistent
compression artifacts. The drop is **packet loss on the keyframe**, reported
via the feedback channel; it is not CPU/GPU/bandwidth saturation (a prior
investigation already ruled those out: enc ~45 %, sm ~25 %, load 0.55).

IDR frames are **intra-coded and large**, so they fragment into far more shards
than P-frames — losing any one of them dooms the whole keyframe. That is why
the log spams *IDR* drops specifically.

---

## 3. Why streams 1 & 2, and never 0

The whole transmit path is **serialized in stream order through a single shared
sender thread on a single UDP socket**:

- All three encoders run in **one** `encoder_thread`, encoded sequentially in
  index order:
  ```
  server/compositor/compositor.cpp:639-643
      for (auto & encoder: encoders)
          if (encoder->stream_idx < 2 or image.view_info.alpha)
              encoder->encode(session, image.view_info, image.frame_index);
  ```
- `encode()` pushes the finished bitstream into a **process-wide singleton
  `sender`** and blocks on `wait_idle(this)` so streams cannot interleave:
  ```
  server/encoder/video_encoder.cpp:269-293   (shared_sender->wait_idle / push)
  server/encoder/video_encoder.h:62-75, 110  (single std::jthread, one deque)
  ```

Consequence: inside every frame interval the socket transmits **stream 0's
shards first, then stream 1, then stream 2**, as one back-to-back burst.

- **Stream 0 (left eye)** always transmits into the fresh inter-frame gap and
  finishes first → its shards win the race, land with slack, complete.
- **Stream 1 (right eye)** transmits into the **tail** of that burst.
- **Stream 2 (alpha)** transmits last, deepest in the tail.

Under a bursty bulk transfer, the loss that shows up is classic **tail-drop**:
the head of the burst gets through, the tail packets are the ones dropped when a
queue (Wi-Fi driver / AP / client socket buffer) momentarily fills. Excellent
RF signal (−56 dBm) does **not** prevent tail-drop — it's a queue/pacing effect,
not a link-quality effect. The deterministic ordering (0 first, 2 last) is
exactly why the asymmetry is deterministic: **0 never drops, 1 and 2 always do.**
Keyframes make it worse because the burst is 2–3× larger the instant all streams
emit an IDR together, pushing even more bytes into the vulnerable tail.

(The alpha stream carries only ~5 % of the bitrate —
`encoder_settings.cpp:45,54-55` `passthrough_bitrate_factor = 0.05` — so its
drops are logged but barely visible; the user-visible damage is **stream 1 =
right eye**.)

---

## 4. Is this a known upstream issue?

There is **no open upstream bug with a pending magic fix** for this exact
symptom, and the relevant hardening already shipped **before** v26.6.1, so the
user is *not* missing a fix by staying on 26.6.1:

- **v25.8** "fixed performance issues with multiple nvenc encoders" (multi-NVENC
  session contention) — already in 26.6.1.
- **PR #644 → split into #652 / #653** "Optimize NVENC configuration & Optimize
  the IDR handler": adds **non-reference P-frame** tracking so a lost *non-ref P*
  no longer forces a full IDR reset. That code is present in the user's tree
  (`idr_handler.cpp:88-97` `set_non_ref` / `is_non_ref_frame`,
  `idr_handler.cpp:49-55`). It reduces *spurious* IDR requests but does **not**
  help a *keyframe* that is itself losing shards — which is this case.
  https://github.com/WiVRn/WiVRn/pull/644
- **Issue #865** "Recent compositor changes … heavy graphical glitches in the
  **right eye** on NVENC / broke Vulkan encoding" (RTX 4080S, Quest 3, **NVENC
  AV1**, 200 Mbps) — closed, no config fix given; notably it was on **AV1** at a
  very high bitrate. https://github.com/WiVRn/WiVRn/issues/865
- **ALVR #3209** "AV1 Dropping IDR frames" — a sibling project's AV1-specific
  IDR-drop report. https://github.com/alvr-org/ALVR/issues/3209
- **Issue #618** "Stream lags when head is held still" — same feedback/pacing
  subsystem, latency spikes. https://github.com/WiVRn/WiVRn/issues/618

Takeaways for tuning: **(a)** AV1 correlates with IDR-drop reports on Quest 3 →
prefer **H.265**; **(b)** very high bitrate makes the burst/tail-drop worse →
**lower the bitrate**; **(c)** the drop mechanism is UDP shard loss → **TCP
eliminates it outright**.

---

## 5. Proposed config

Server config path (create it — it does not exist yet):
`~/.config/wivrn/config.json`. Server config **cannot set bitrate** — bitrate is
a **client-side** setting sent from the Quest (`client/configuration.h:65`
`bitrate_bps = 50'000'000` default; server just consumes
`from_headset::settings_changed`, `wivrn_session.cpp:477-478`). So the fix is
two-pronged: a small server config **plus** a client bitrate change.

### `config.json.proposed` (primary — try this first)
```json
{
	"encoder": {
		"encoder": "nvenc",
		"codec": "h265"
	},
	"bit-depth": 8
}
```
Rationale, per key:
- **`"encoder": "nvenc"`** — pins all three streams to NVENC on the RTX 5070
  explicitly (this is already the auto-pick, but pinning removes any ambiguity
  and stops a probe failure from silently falling back to vaapi/x264).
- **`"codec": "h265"`** — H.265/HEVC over AV1: smaller keyframes than H.264
  (fewer shards → smaller vulnerable tail) **and** the most reliable, lowest-
  overhead decode path on Quest 3. Avoids the AV1 IDR-drop pattern seen in #865
  / ALVR #3209. Pinning it also makes both eyes use the *same* codec
  deterministically.
- **`"bit-depth": 8`** — smaller frames and lighter Quest decode load than
  10-bit HEVC → fewer shards, faster reassembly. Reliability-first; bump to
  `10` for color quality only after the drops are gone.

### Client change to pair with it (do this too)
In the **WiVRn app on the Quest**, lower **Bitrate from 50 → ~30 Mbit/s**
(Settings while connected, or the streaming panel). This directly shrinks every
frame — including the IDR burst tail that is being lost. This is the single
biggest lever and needs no server restart. Raise it back up in ~5 Mbit steps
once the journal is clean; the point where `stream 1` drops reappear is your
ceiling for this environment.

### `config.json.tcp-fallback` (escalation — guaranteed fix)
If codec + bitrate still leave residual `stream 1` drops:
```json
{
	"encoder": {
		"encoder": "nvenc",
		"codec": "h265"
	},
	"bit-depth": 8,
	"tcp-only": true
}
```
- **`"tcp-only": true`** — forces video shards over the **TCP** control channel
  instead of UDP (`wivrn_connection.h:99-105`: `send_stream` uses `stream`
  (UDP) when present, else `control` (TCP)). TCP retransmits lost segments, so
  the client **always** reassembles a complete frame → `sent_to_decoder` is
  always set → the `IDR frame dropped` path in `idr_handler.cpp:44` can no
  longer fire. This **eliminates the mechanism**, not just reduces it. Cost:
  slightly higher/less-even latency (`docs/configuration.md:105-109`). On this
  LAN (−56 dBm, 700 Mbit/s headroom) that cost is small and worth trying if the
  UDP tuning isn't enough.

### Considered and rejected
- **Offloading the right eye to the AMD 890M via `"encoder": ["nvenc","vaapi",
  "nvenc"]` + `"device": "/dev/dri/renderD128"`** (`docs/configuration.md:81-84`).
  This parallelizes *encode*, but the bottleneck is the *serialized single-socket
  transmit* and *network loss*, not encode time (GPU was ~25–45 % busy). It would
  not fix tail-drop and adds a second codec path to go wrong. Skip.

---

## 6. Validation plan (for the user)

All steps are safe; **do them while OUT of the headset session** (session
create reads the encoder layout once at connect).

1. **Take the headset off / disconnect** the WiVRn session first. Do not run any
   state-changing `hyprctl`; do not `pkill` anything.
2. **Install the config** (only the user should do this — this agent did not
   touch `~/.config/wivrn/`):
   ```
   cp docs/openxr/research/wivrn-tuning/config.json.proposed ~/.config/wivrn/config.json
   ```
3. **Restart the server** while disconnected:
   ```
   systemctl --user restart wivrn.service
   ```
4. **Lower the Quest bitrate** to ~30 Mbit/s in the WiVRn app.
5. **Watch the journal** in one terminal:
   ```
   journalctl --user -u wivrn.service -f
   ```
   On connect you should see one `Encoder configuration:` block listing three
   `nvenc (h265 8-bit)` streams (from `print_encoders`,
   `encoder_settings.cpp:77-88`) — confirms the config took.
6. **Reconnect the headset** and use it normally for a few minutes with
   passthrough + head-anchored monitors (the real workload).
7. **Success criteria:**
   - `WARN IDR frame dropped, stream 1` / `stream 2` lines **gone or rare**
     (an occasional one right at connect is fine).
   - Right eye visually clean — no persistent blockiness / smear that the left
     eye lacks.
8. **If drops persist:** swap in the TCP fallback and restart (out of session):
   ```
   cp docs/openxr/research/wivrn-tuning/config.json.tcp-fallback ~/.config/wivrn/config.json
   systemctl --user restart wivrn.service
   ```
   With `tcp-only` the `IDR frame dropped` lines must disappear entirely; if any
   lag remains it will be latency-shaped, not artifact-shaped — then walk the
   Quest bitrate back up until latency is acceptable.
9. **Tuning back up:** once clean, raise Quest bitrate in ~5 Mbit steps and/or
   set `"bit-depth": 10` for quality; the reappearance of `stream 1` drops marks
   this environment's ceiling.

---

### File manifest
- `config.json.proposed` — primary (nvenc + h265 + 8-bit), pair with Quest
  bitrate ~30 Mbit/s.
- `config.json.tcp-fallback` — escalation adding `tcp-only: true` (guaranteed
  elimination of the drop mechanism, small latency cost).
