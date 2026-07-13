# Design: cascaded voice control for HypXRland (`hypxrvoice`)

Research memo (2026-07-12). **No implementation.** Proposes a standalone companion
daemon — working name **`hypxrvoice`** — that lets the user drive HypXRland monitor
manipulations and app launches by voice ("have YouTube follow me", "make a near-field
phone-sized monitor", "drop this monitor here", "open Apple TV"), with a **cascaded
activation architecture** whose whole point is to keep the microphone shut and the cloud
bill near zero until a real command is actually spoken.

> **Hard constraint (user directive):** the voice system lives as a **separate
> utility/daemon** — never inside Hyprland/HypXRland. There is no in-compositor deployment
> option in this proposal space; every proposal in §10 is a shape of a standalone
> `hypxrvoice` (its own repo, like `hypxrpaper`). Compositor-side involvement is limited to
> what the daemon consumes over the **existing** IPC surface, plus the short list of small,
> optional, read-only-or-event-only enablers in **§8a (compositor touchpoints)** — no daemon
> logic (audio, models, dialogue state, arming policy) ever enters the compositor.

Evidence base: the existing HypXRland IPC surface (`docs/openxr/05-configuration.md`,
`src/openxr/XRIpc.cpp`, `src/config/legacy/DispatcherTranslator.cpp`); the sibling
companion-tool precedents `hypxrpaper` and the `hypxrkeys` design memo
(`research/05-xr-screenkey.md`); this box's hardware (Ryzen AI 9 HX 370 "Strix Point" —
RDNA 3.5 iGPU + XDNA 2 NPU + discrete NVIDIA); and web research on the 2026 voice-model /
ASR / wake-word / local-LLM landscape (URLs cited in §12).

---

## TL;DR

1. **The actuation layer already exists.** Every example command maps onto a
   `hyprctl`/dispatcher call that ships today: `hyprctl openxr create|anchor|roam|undock|dock|move|distance|center|adaptive`, `hyprctl dispatch exec …`, and `hyprctl -j openxr|clients` for the read side. `hypxrvoice` is a **thin translator** from natural language to this JSON-RPC surface — no new compositor actuation is required for v1 (one small read-only IPC addition is *nice-to-have*, §1.4). The hard, interesting problem is **not** "how do I move a monitor," it's **cost/privacy gating** and **deixis** ("*this* monitor", "*here*").

2. **Cascade, don't stream.** Naive always-on realtime streaming to a cloud voice model
   costs **~$0.18–0.46/min uncached** (OpenAI Realtime) — roughly **$85–$220/day** for an
   8-hour session, and it means an open mic all day. A four-tier cascade (contextual gate →
   local wake word → local streaming ASR + intent gate → cloud reasoning *only for
   interpretable commands*) drops the cloud cost to **pennies/day at 50 commands/day** and
   keeps the mic **process-level closed** except in explicitly-armed states (§2, §6).

3. **Anthropic has no realtime voice API** (verified 2026-07; the consumer apps have
   push-to-talk voice, but there is **no developer Realtime/streaming speech API** — unlike
   OpenAI Realtime and Gemini Live). The Claude path is a **composition**: local STT →
   **Claude Messages API with tool-use** (the tool schema in §1.5) → local TTS. That
   composition is actually the *right shape* for this problem, because the command set is a
   small, well-specified tool surface and Claude's tool-calling + strict JSON is exactly what
   maps an utterance to `hyprctl openxr …`.

4. **A fully-local top tier is a serious contender**, not a fallback. The command domain is
   narrow (a dozen verbs, a handful of monitor/app nouns). A small local LLM with
   grammar-constrained JSON tool-calling (Qwen2.5-7B / SmolLM3-3B / Ministral-3B on the 890M
   iGPU via ROCm-llama.cpp, ~18–50 tok/s) can interpret these commands with **zero cloud, zero
   per-command cost, and no data egress**. Recommendation is a **hybrid** (Proposal B): local
   for the common cases, cloud reasoning as an opt-in escalation for genuinely ambiguous or
   novel phrasings.

5. **HypXRland already knows presence.** The contextual gate (Tier 0) can auto-arm listening
   *only when the user is in the headset and not at the keyboard*, because
   `hyprctl -j openxr` exposes `userPresence`/`visible`/`state` and the socket2 bus posts
   `openxractive>>1|0` on don/doff (`src/openxr/XRIpc.cpp:102`,
   `docs/openxr/05-configuration.md` §6). This is a privacy feature, not just ergonomics:
   "in-headset-only" is a hard, queryable invariant.

6. **Shape (hard constraint, per user directive):** a standalone BSD-3 daemon `hypxrvoice`
   (sibling to `hypxrpaper` / `hypxrkeys`), single-purpose, speaking PipeWire on one side and
   the Hyprland sockets on the other, launched as a systemd user service or `exec-once`.
   In-compositor deployment is out of the proposal space entirely; the only compositor-side
   items are the optional S-effort touchpoints in §8a. It learns the command surface from
   a **static schema** (versioned with the compositor), tracks a small **deixis/dialogue
   state** for "the other one" / "here", and gives visual + optional audio feedback. WPs
   V1–V12 in §11.

---

## Architecture sketch

```
                          in-headset presence + PTT               (Tier 0: contextual gate)
                          openxractive>>1  |  key/controller chord
                                     │  arms the mic ONLY here
                                     ▼
   PipeWire ──▶ [ AEC ] ──▶ ring buf ──▶ ┌─ Tier 1: wake word (openWakeWord / Porcupine)
   wivrn.source / desk mic   echo-cancel │     "hey hypr"  ~1MB RAM, <5% one core, local
                                         │
                                         ├─ Tier 2: local streaming ASR (whisper.cpp small
                                         │     + silero-VAD) → transcript
                                         │        └─ intent gate: is this a command at all?
                                         │           (keyword grammar / embedding / tiny LLM)
                                         │
                                         └─ Tier 3: REASONING — interpret the command
                                            ├─ LOCAL LLM tool-call (Qwen/SmolLM, GBNF JSON)   [Proposal C / B-default]
                                            └─ CLOUD  (Claude tool-use  |  OpenAI Realtime  |  Gemini Live)   [B-escalate / A]
                                                              │
                                                              ▼  emits a validated tool call
                                     ┌────────────────────────────────────────────────┐
                                     │  hyprctl / dispatcher IPC  (ALREADY EXISTS)      │
                                     │  openxr create|anchor|roam|move|distance|center  │
                                     │  dispatch exec <app>   ·   -j openxr|clients     │
                                     └────────────────────────────────────────────────┘
                                                              │
                          deixis resolver  ◀── hyprctl -j openxr (hovered/grabbed/selected)
                          dialogue state   ◀── "the other one", "no, the browser"
                                                              │
                          feedback: hyprctl notify  |  hypxrkeys overlay  |  piper TTS
```

Property inherited from the companion-tool family: **single-purpose, no compositor coupling
beyond the public sockets** — `hypxrvoice` is compositor-version-tolerant (it degrades if a
verb is missing) and can be developed, tested, and shipped independently, exactly like
`hypxrpaper`.

---

## 1. Command-surface mapping — the actuation layer already exists

Every example the user gave maps onto an IPC call that ships in HypXRland today. This section
is the proof, then a draft tool-call schema.

### 1.1 The three transports (one implementation)

Per `docs/openxr/05-configuration.md` §4–5, the `xrmonitor` verbs are reachable three ways,
all funnelling into `COpenXRManager` (`src/openxr/XRIpc.cpp:139-227`,
`src/config/legacy/DispatcherTranslator.cpp:800`):

- `hyprctl openxr <verb> …` — the command socket `.socket.sock`
- `hyprctl dispatch xrmonitor <verb> …` — same
- `bind = …, xrmonitor, <verb>` — keybind

`hypxrvoice` uses the **command socket** (`$XDG_RUNTIME_DIR/hypr/$HYPRLAND_INSTANCE_SIGNATURE/.socket.sock`,
`src/debug/HyprCtl.cpp:2323`) for actuation and the **socket2 event bus**
(`.socket2.sock`, `src/managers/EventManager.cpp:21`) for state. Both are plain `AF_UNIX`; no
new API is needed.

### 1.2 Each example → its call

| User utterance | Maps to | Verb / source |
|---|---|---|
| "Have YouTube follow me" | find the window/monitor showing YouTube, flip its anchor to `body` (or `head`) | `hyprctl -j clients` to locate it (`src/debug/HyprCtl.cpp:455`) → `hyprctl openxr anchor <name> body` (`XRIpc.cpp:181`) |
| "Create a new near-field phone-sized monitor" | make a small XR monitor close in | `hyprctl openxr create XR-phone 1080x2340 anchor:body offset:0,-0.1,-0.35, size:0.15` (grammar in `05-configuration.md` §3–4; `XRIpc.cpp:165`) |
| "Drop this monitor here" (body-leashed → world-locked) | re-anchor the *selected* monitor to `local` at its current pose | `hyprctl openxr anchor active local` (`XRIpc.cpp:181`) — `local` re-anchors *without moving the quad visually*, exactly "drop it where it is" |
| "Open Apple TV" / "Open the browser" | launch an app | `hyprctl dispatch exec <cmd>` (dispatcher list at `src/managers/KeybindManager.cpp:41`); resolve the spoken name → command via a user alias map + Omarchy's launcher inventory (walker/uwsm-app) |
| "Push it further away" / "bring it closer" | distance along the view ray | `hyprctl openxr distance +0.25` / `-0.25` (`XRIpc.cpp:197`) |
| "Make it bigger" / "shrink it" | scale the quad width | `hyprctl openxr scale 1.2` or `scale +0.2` (`XRIpc.cpp:193`) |
| "Center that in front of me" | re-place centered in view | `hyprctl openxr center` (`XRIpc.cpp:201`) |
| "Let it dock at my desk" / "pick it up and follow me" | adaptive decorator | `hyprctl openxr adaptive on` / `undock` / `dock [here]` (`XRIpc.cpp:207-218`) |
| "Close that monitor" (**destructive**) | destroy | `hyprctl openxr destroy active` (`XRIpc.cpp:169`) — **confirmation-gated**, §1.6 |

Nothing in this table needs new compositor actuation. The `size:`/`anchor:`/`offset:` grammar
("phone-sized", "near-field") is already the `xrmonitor` grammar (`05-configuration.md` §3);
"phone-sized" is just a **noun→params alias** `hypxrvoice` owns (a small YAML the user tunes:
`phone → 1080x2340 size:0.15`, `cinema → 3840x2160 size:3.0`, …).

### 1.3 The read side (state for deixis & queries)

- `hyprctl -j openxr` (`XRIpc.cpp:16`) — every live XR monitor with `name`, `anchor.mode`,
  live pose, **`hovered`**, **`grabbed`**, `adaptive.phase`, plus session `state`,
  `userPresence`, `visible`. This is the deixis substrate: **"this monitor" = the last
  ray-hovered or grabbed monitor**, which the status JSON already reports per-monitor.
- `hyprctl -j clients` (`HyprCtl.cpp:455`) — window titles/classes/monitor assignment, to map
  "YouTube" → the monitor it lives on.
- socket2 events (`05-configuration.md` §6): `openxractive`, `xrmonitorgrab`,
  `xrmonitoranchor`, `xrmonitoradded/removed`, `xrmonitorundocked/docked` — a live feed so the
  deixis resolver never has to poll hard.

### 1.4 Deixis: "this monitor" / "here"

The **selected-target resolution** already implemented for the dispatcher (`05-configuration.md`
§4: *explicit `select` > last ray-hovered > focused-monitor-if-XR*) is exactly the deixis rule
we want, and `hypxrvoice` gets it **for free** by passing `active` as the target — the
compositor resolves "this" the same way a controller grab does. For richer targeting:

- **"this monitor"** → `active` (compositor resolves to hovered/selected), *or* the voice daemon
  reads `hovered:true` from `hyprctl -j openxr` to name it explicitly for a confirmation
  message ("dropping XR-chat here — ok?").
- **"here"** (a world location) → for "drop this monitor here", `anchor active local` already
  means "freeze it at its current pose", which is the natural reading. A future "here = where
  I'm pointing" would want the **head-ray target** designed in the archived gaze memo
  (`research/archive/16-gaze-grab.md`) — **reference, do not redesign**; it already worked out
  head-ray → monitor selection and is the right substrate if pointing-deixis is wanted later.
- **NICE-TO-HAVE compositor touchpoint (read-only; WP-T1 in §8a):** a `hyprctl openxr gaze` that returns the
  monitor currently under the head-ray (or the ray hit-point in `LOCAL_FLOOR` coords) would let
  `hypxrvoice` resolve "that one over there" and "put it here" precisely without re-implementing
  ray math. It's a pure read, gated behind the same `HAVE_OPENXR` compile guard, and would ride
  the hypxrland branch. **Not required for v1** — `active` covers the common case.

### 1.5 Draft tool-call schema (function-calling JSON)

This is the schema a cloud (Claude/OpenAI/Gemini) *or* local LLM is handed. It is deliberately
**a small, closed set of strict-typed tools** — the whole reason a narrow domain is cheap and
reliable. Destructive tools carry a `confirm` contract (§1.6).

```jsonc
// tools handed to the reasoning tier. Each maps 1:1 to a hyprctl call.
[
  { "name": "create_monitor",
    "description": "Create a new XR monitor. Use preset for common sizes (phone/tablet/cinema/desk).",
    "input_schema": { "type": "object", "additionalProperties": false,
      "properties": {
        "name":   {"type":"string","description":"e.g. XR-phone; auto if omitted"},
        "preset": {"type":"string","enum":["phone","tablet","desk","cinema","hud"]},
        "anchor": {"type":"string","enum":["local","head","body","device:left","device:right"]},
        "distance_m": {"type":"number","minimum":0.15,"maximum":5.0},
        "size_m": {"type":"number","minimum":0.1,"maximum":4.0}
      }, "required": [] } },

  { "name": "set_anchor",
    "description": "Change a monitor's anchor mode. body/head = 'follow me'; local = 'drop it / world-lock it here'.",
    "input_schema": { "type":"object","additionalProperties":false,
      "properties": {
        "target": {"type":"string","description":"monitor name, or 'active' for this/selected/hovered"},
        "mode":   {"type":"string","enum":["local","head","body","device:left","device:right"]}
      }, "required":["target","mode"] } },

  { "name": "adaptive",
    "input_schema": { "type":"object","additionalProperties":false,
      "properties": { "target":{"type":"string"},
        "action":{"type":"string","enum":["on","off","undock","dock","dock_here","roam_head","roam_body"]} },
      "required":["target","action"] } },

  { "name": "move_monitor",   // distance/scale/center/rotate collapsed into one relative-motion tool
    "input_schema": { "type":"object","additionalProperties":false,
      "properties": { "target":{"type":"string"},
        "distance_delta_m":{"type":"number"}, "scale_factor":{"type":"number"},
        "yaw_delta_deg":{"type":"number"}, "recenter":{"type":"boolean"} },
      "required":["target"] } },

  { "name": "launch_app",
    "input_schema": { "type":"object","additionalProperties":false,
      "properties": { "app":{"type":"string","description":"spoken app name; resolved via alias map"} },
      "required":["app"] } },

  { "name": "focus_or_move_app",  // "have YouTube follow me": find window, then set_anchor on its monitor
    "input_schema": { "type":"object","additionalProperties":false,
      "properties": { "query":{"type":"string"},
        "anchor":{"type":"string","enum":["local","head","body"]} },
      "required":["query"] } },

  { "name": "destroy_monitor",   // DESTRUCTIVE — see §1.6
    "input_schema": { "type":"object","additionalProperties":false,
      "properties": { "target":{"type":"string"}, "confirmed":{"type":"boolean","default":false} },
      "required":["target"] } },

  { "name": "clarify",   // the model's escape hatch when a reference is ambiguous
    "input_schema": { "type":"object","additionalProperties":false,
      "properties": { "question":{"type":"string"},
        "candidates":{"type":"array","items":{"type":"string"}} },
      "required":["question"] } }
]
```

A **system prompt / context block** feeds the reasoner the *live* layout each turn (compact
JSON from `hyprctl -j openxr`: monitor names, anchor modes, which is `hovered`/`grabbed`), so
"the browser", "this one", "the other one" resolve against real state. This is small (a few
hundred tokens) — it is the `context` that makes deixis work and is the main variable input.

### 1.6 Confirmation policy for destructive / ambiguous ops

| Class | Ops | Policy |
|---|---|---|
| **Destructive** | `destroy_monitor`, "close everything", `exit`/quit compositor | **Two-step confirm required.** The reasoner emits the tool with `confirmed:false`; `hypxrvoice` speaks/shows "Close XR-chat? Say yes" and only actuates on an affirmative follow-up. Never actuate a destroy on first utterance. |
| **Ambiguous target** | any `target` that matches 0 or >1 monitors and no `hovered`/`selected` disambiguates | Reasoner is instructed to call `clarify` instead of guessing; `hypxrvoice` asks "the browser or the video?" and resolves on reply (dialogue state, §4). |
| **Reversible** | move/scale/distance/center/anchor/adaptive/create/launch | Actuate immediately; every one is trivially undoable (opposite verb), so no confirm. Optional "undo that" maps to an inverse-op stack `hypxrvoice` keeps. |

This mirrors the agent-design principle (destructive/irreversible actions gate behind
confirmation; reversible ones run free) and keeps the latency budget (§5) intact for the 95%
reversible case.

---

## 2. Cascading activation architecture — the cost/privacy core

Four tiers, each a cheap filter that only escalates when it must. The escalation ladder is the
whole product.

### Tier 0 — contextual gating (no audio processed at all)

The mic stream is **not even open** unless a gate says "the user might be about to talk to me."
Two complementary gates, both cheap and both **hard privacy invariants** (the OS-level fact that
no PCM is captured outside these states, §6):

| Gate | Mechanism | Cost |
|---|---|---|
| **Push-to-talk (physical)** | a Hyprland keybind or a controller button chord opens the mic for N seconds / while held. Keybind: `bind = …, exec, hypxrvoice ptt` writes a control socket; controller: a spare XR button routed the same way the ray-pointer trigger is (`openxr:pointer_*`). | ~0 |
| **Presence auto-arm (in-headset only)** | subscribe to socket2 `openxractive>>1` (don) / `openxractive>>0` (doff) and `hyprctl -j openxr` `userPresence`/`visible` (`XRIpc.cpp:102`). Arm the *wake-word* tier only while **in the headset AND not typing** (a keyboard-activity heuristic — recent key events suppress auto-arm, so desk work never triggers it). | ~0 (event-driven) |

"Away from keyboard" = no key event in the last *k* seconds (read from the same libinput/logind
path `hypxrkeys` uses, `research/05-xr-screenkey.md` Decision 2, or simply a
`hyprctl`-observable idle proxy). **In-headset-only** is the strongest privacy story HypXRland
can offer: presence is a first-class, queryable signal, so "the mic is closed whenever the
headset is off my face" is enforceable, not aspirational.

### Tier 1 — local wake word

Only runs while Tier 0 has armed it. A small always-on-when-armed keyword spotter turns a short
audio window into a binary "did they say the wake phrase."

| Engine | License | Footprint (this box) | Notes |
|---|---|---|---|
| **openWakeWord** | open (Apache) | a single core runs 15–20 models in real time on a *Raspberry Pi 3*; negligible on Strix Point | ONNX; custom phrases need a little ML work; recommended default for an open-source tool |
| **Porcupine** (Picovoice) | commercial (free tier) | ~1 MB RAM, <4% of one RPi3 core; 97%+ detection, <1 false-alarm/10h | best accuracy + easiest custom-wake training, but proprietary — offer as opt-in |
| Vosk keyword mode | Apache | heavier (full ASR) | only if we already run Vosk for Tier 2; otherwise overkill for wake |

Recommendation: **openWakeWord default, Porcupine opt-in.** CPU cost on this box is a rounding
error. A **push-to-talk-only mode** skips Tier 1 entirely (no always-listening at all) for the
maximally-private user.

### Tier 2 — local streaming ASR + intent gate

Wake fired → open the real ASR for a few seconds, transcribe, and **decide locally whether this
is even a command** before spending any cloud tokens.

**ASR:** `whisper.cpp` (`small`/`base` int8) or `faster-whisper` with **silero-VAD** for
endpointing. Measured envelope from the field: tiny <0.5 s latency (higher WER), small/medium
0.5–2 s; VAD-gated streaming stacks (whisper_streaming, WhisperX+silero) hit **380–800 ms**
end-of-utterance latency. On Strix Point the iGPU or even CPU handles `small` comfortably in
real time. silero-VAD does the endpointing so we transcribe only speech segments.

**Intent gate** (the token-saver): before any cloud call, decide "is this a HypXRland command?"
Three options, cheapest first:

1. **Keyword/grammar gate** — does the transcript contain a command trigger (monitor, follow,
   drop, open, bigger, closer, …)? Fastest, zero model, but brittle. Good as a first pass.
2. **Embedding classifier** — embed the transcript, cosine-match against a small set of command
   exemplars; a local MiniLM-class embedder is <10 ms. Robust to paraphrase, still no LLM.
3. **Tiny local LLM yes/no** — a 1–3B model answers "is this an actionable desktop command?".
   Heaviest of the three but most accurate; on this box it's ~tens of ms.

Recommendation: **(1) as a coarse pre-filter, (2) as the decision.** Only a transcript that
passes the intent gate is escalated to Tier 3. This is what turns "50 commands/day" into "~50
cloud calls/day" instead of "every stray sentence I say in the headset."

### Tier 3 — reasoning (interpret the command)

Only reached by transcripts the intent gate accepted. Turns the transcript + live-layout context
(§1.5) into a validated tool call. This is the tier where the local-vs-cloud choice lives (§7,
Proposals). The output is always the same: **a strict-validated tool call** that `hypxrvoice`
executes against the sockets (with the confirm gate for destructive ops, §1.6).

### Per-tier CPU / latency / where the cost lives

| Tier | Runs when | Local cost (Strix Point) | Added latency | Cloud $ |
|---|---|---|---|---|
| T0 gate | always (event-driven) | ~0 | 0 | 0 |
| T1 wake | armed by T0 | <1% core | ~50–150 ms detect | 0 |
| T2 ASR+intent | wake fired | `small` whisper in RT; VAD ms-level | 0.4–1.5 s (endpoint + decode) | 0 |
| T3 local LLM | intent passed | iGPU ~18–50 tok/s; a ~40-token tool call ≈ 1–2 s | ~1–2 s | 0 |
| T3 cloud | intent passed (or escalated) | ~0 local | network + model (§7) | **only tier that bills** |

---

## 3. Audio plumbing on this system

- **Capture — PipeWire.** The headset mic arrives as an ordinary PipeWire **source**
  (`wivrn.source`) because WiVRn duplex audio already works on this box (the container audio
  notes confirm host pipewire-0 is shared and `wivrn.sink`/`wivrn.source` behave as normal
  devices). At the desk a USB/analog source is used instead. `hypxrvoice` picks the source by
  policy: **in-headset → `wivrn.source`, at-desk → default source**, switched off the same
  presence signal that drives Tier 0.
- **Echo cancellation.** Media plays back through the *same* headset, so the mic hears the
  desktop's own audio. Load `pipewire-module-echo-cancel` (WebRTC AEC) between the raw source
  and `hypxrvoice`'s capture, so wake-word/ASR don't trigger on played-back audio. This is
  essential once any TTS or media is active — without AEC the assistant can hear itself.
- **Feedback — audio vs visual.** Two channels; the user picks:
  - **Visual (recommended default):** `hyprctl notify` for a quick toast, or — the natural fit —
    a **`hypxrkeys`-style head-locked overlay lane** showing the recognized command and its
    status (`research/05-xr-screenkey.md` is the companion-overlay design; a "voice lane" is a
    near-exact reuse of its IPC-activity lane). A "listening" indicator is a tiny layer-shell or
    XR quad, §5.
  - **Audio (opt-in):** **piper** for local low-latency TTS confirmations ("dropped it here"),
    real-time on far weaker hardware than this box; or an API voice if already paying for a cloud
    tier. Audio confirmation is often *unwanted* in a shared space — default to visual, offer
    voice as a flag.

---

## 4. Deployment shape — the `hypxrvoice` daemon

Standalone companion daemon, matching the project's offload-to-companion philosophy
(`hypxrpaper` shipped this way; `hypxrkeys` is designed this way).

- **Process:** one long-lived daemon speaking PipeWire ⟷ Hyprland sockets. A control socket
  (its own `AF_UNIX`) receives `ptt` / `arm` / `mute` commands from keybinds
  (`bind = …, exec, hypxrvoice ptt`).
- **Lifecycle:** a **systemd user service** (`hypxrvoice.service`, `WantedBy=graphical-session`)
  is cleanest — it restarts on crash, has its own log, and can be `systemctl --user stop` to
  hard-kill the mic. `exec-once = hypxrvoice` also works for the Omarchy/hyprland.conf crowd.
  Either way it inherits `HYPRLAND_INSTANCE_SIGNATURE` to find the sockets.
- **How it learns the IPC surface:** **static schema, versioned with the compositor** (the §1.5
  tool list, shipped in the repo), *not* live `hyprctl` introspection — the verb set is small
  and stable, static keeps the tool descriptions high-quality and the prompt cacheable, and it
  degrades gracefully (an older compositor missing a verb → that tool errors cleanly). It reads
  *live state* dynamically (`-j openxr`/`clients`) but the *tool definitions* are static.
- **State it must track (for deixis and follow-ups):**
  - **Last-referenced monitor** ("it", "that one") — updated on every command and from socket2
    `xrmonitorgrab`/`xrmonitoranchor`.
  - **Candidate set** for "the other one" — the monitors matched by the last ambiguous reference,
    so "no, the other one" flips selection without re-reasoning.
  - **Pending confirmation** — the destructive op awaiting "yes".
  - **Undo stack** — inverse ops for "undo that".
  - **App alias map** (user YAML: "Apple TV" → command, "YouTube" → window-title regex).
  This is a **small, bounded dialogue state**, not a general agent memory.

---

## 5. Interaction design

- **Barge-in:** while `hypxrvoice` is speaking a confirmation, keep the mic (post-AEC) open so
  "no, stop" interrupts. AEC makes this safe (it won't hear its own TTS).
- **Confirmation UX:** destructive/ambiguous → speak+show the question, resolve on the next
  utterance (§1.6, §4). Reversible → act, then a one-line "done" toast.
- **Error repair:** "I meant the browser" / "no, the other one" — handled by the candidate-set
  dialogue state (§4) without a fresh cloud round-trip when possible (local re-resolution).
- **Latency budget (target command→action < 2 s):** PTT bypasses T1 (0 ms) → T2 endpoint+decode
  0.4–1.5 s → **local** T3 ~1–2 s → actuate <50 ms (a socket write). **Local end-to-end lands
  ~1.5–3 s**; tight but achievable with PTT + `small` whisper + a 3B model. **Cloud** T3 adds
  network + model time; OpenAI Realtime is sub-second once streaming, but the *cascade* adds the
  T2 gate ahead of it. The realistic sub-2 s path is **PTT (skip wake) + local reasoning**; the
  cloud path trades a little latency for better hard-case understanding.
- **"Listening" indicator:** a persistent, cheap visual — options: a small **layer-shell client**
  on the flat desktop, a **head-locked XR quad** (a one-quad `hypxrkeys` sibling), or reuse the
  VISUALS.md chrome accent to tint something while armed. Requirement is minimal: a boolean
  "armed/listening/thinking" state rendered somewhere always-visible so the user *knows* when the
  mic is hot — which doubles as the privacy signal (§6).

---

## 6. Privacy & cost guarantees

**Hard "not listening" invariants (process-level, per tier):**

- The **PCM stream is only opened in armed states** (T0 gate). In PTT-only mode, the mic is
  closed except while the button is held — a kernel/PipeWire fact, observable in
  `pw-top`/`wpctl`, not a promise.
- **In-headset-only** mode: auto-arm keyed on `openxractive`/`userPresence`, so the mic is closed
  whenever the headset is off the face — enforceable because presence is queryable
  (`XRIpc.cpp:102`).
- **Local-only mode** (Proposal C / B with escalation disabled): **no audio, transcript, or
  derived text ever leaves the machine** — no network socket is opened by the reasoning tier at
  all. This is the maximal-privacy configuration and is a first-class supported mode, not a
  degraded one.
- **UI indicator** (§5) makes the armed state visible at all times.
- **No persistence:** transcripts and audio are in-memory, time-bounded; nothing written to disk
  (mirrors the `hypxrkeys` no-persistence rule).
- **API key handling** (cloud tiers only): key from env/secret file, never logged; masked in any
  overlay (the `hypxrkeys` `--ipc-mask` precedent).

**Cost table — naive realtime vs cascade.** Current pricing (2026-07, §12):

| Config | Per unit | 8 h/day always-on | 50 commands/day |
|---|---|---|---|
| **Naive OpenAI Realtime, always streaming** | $0.18–0.46/min uncached (mini ~⅓) | **~$86–$220/day** | n/a (it streams continuously) |
| **Naive Gemini Live, always streaming** | ~$0.005 in + $0.018 out /min | **~$11/day** (cheaper, still an open mic all day) | n/a |
| **Cascade → OpenAI Realtime, per command** | ~10–20 s of audio only when a command is spoken | — | **~$0.60–$2.30/day** (50 × ~$0.02–0.05) |
| **Cascade → Claude text pipeline** | ~600 tok in (ctx+transcript) + ~100 tok out per command | — | **< $0.10/day** on Sonnet 5 ($3/$15 MTok; Haiku 4.5 $1/$5 is ~⅓); with prompt caching of the tool schema, lower still |
| **Cascade → local LLM** | $0 | — | **$0.00** (electricity only) |

The cascade collapses the cloud bill by **1000–3000×** versus naive streaming (from hundreds of
dollars/day to cents or zero), *and* closes the mic outside armed states. That combination — cost
and privacy improving together — is the entire thesis.

---

## 7. Reasoning-tier options compared (the local↔cloud axis)

| Option | Latency | Per-command cost | Privacy | Fit for this domain |
|---|---|---|---|---|
| **Local LLM** (Qwen2.5-7B / SmolLM3-3B / Ministral-3B, GBNF-constrained JSON, ROCm-llama.cpp on 890M iGPU) | ~1–2 s | **$0** | **local-only** | **Excellent.** Narrow, closed tool set + grammar-constrained decoding → reliable JSON; 3B works, 7B safer on complex refs. Memory-bandwidth-bound (~90–120 GB/s) but a 40-token tool call is tiny. NPU (XDNA 2, 50 TOPS) is **not** the practical path — LLM decode is software-limited there; the **iGPU via ROCm** is. |
| **Claude Messages API + tool-use** (STT→Claude→TTS composition) | network + model | **< $0.002/command** (Sonnet 5; Haiku 4.5 cheaper) | text leaves box | **Best hard-case understanding.** No realtime voice API (compose STT/TTS locally), but tool-calling + strict JSON is *exactly* this problem; prompt-cache the static tool schema so only the transcript+context vary. Recommended cloud escalation target. |
| **OpenAI Realtime** (gpt-realtime-2 / mini) | sub-second streaming | $32/$64 per MTok audio ($10/$20 mini); ~$0.02–0.05/command via cascade | audio leaves box | Lowest *interaction* latency, but built for *continuous* conversation — overkill for discrete commands, and the priciest per-audio-minute. Use only if a chatty, interruptible voice UX is wanted. |
| **Gemini Live** | sub-second streaming | $3/$12 per MTok audio (~$0.005/$0.018/min); free tier | audio leaves box | Materially cheaper per audio token than OpenAI Realtime, adds a free tier — the best *cloud realtime* option on cost if streaming is desired. |

**Takeaway:** because the domain is narrow, **local reasoning is genuinely competitive** on
quality and wins outright on cost/privacy/latency-jitter. Cloud earns its keep only on
genuinely novel phrasing or multi-step reasoning ("tidy up my space" → several ops). Hence the
recommended **hybrid**.

---

## 8. Companion-tool precedent & reuse

`hypxrvoice` reuses patterns already proven in the family:

- **Socket discovery + degradation** exactly like `hypxrkeys`' socket2 subscriber
  (`research/05-xr-screenkey.md` §8.4): discover via `HYPRLAND_INSTANCE_SIGNATURE`, reconnect
  with backoff, degrade to a smaller feature set if a verb/event is missing.
- **libinput-on-logind** for the keyboard-activity heuristic (Tier 0 "away from keyboard") is the
  *same* capture path `hypxrkeys` uses for keystrokes (`research/05-xr-screenkey.md` Decision 2)
  — no new privilege model.
- **Single-purpose, `--gpu`-aware, vendored-deps** shape of `hypxrpaper`.
- **Visual feedback** can literally be a `hypxrkeys` overlay lane (a "voice" source class), so the
  two companion tools compose in one overlay session.

---

## 8a. Compositor touchpoints (the ONLY compositor-side items)

Per the hard constraint (§0 callout): no daemon logic enters the compositor. The daemon runs
entirely on the existing IPC surface today — **zero compositor changes are required for v1.**
The items below are small, optional enablers that would materially help the daemon; each is a
read-only query, a status field, or a socket2 event — never audio, model, dialogue, or arming
logic. All are S-effort, `HAVE_OPENXR`-guarded where applicable, and ride the hypxrland branch.

| WP | Touchpoint | What / why | Effort |
|---|---|---|---|
| **WP-T1** | `hyprctl openxr gaze` (read-only query) | Returns the monitor under the head-ray (and/or the ray hit-point in `LOCAL_FLOOR`), building on `research/archive/16-gaze-grab.md` ray math. Enables true point-and-place deixis ("put it here", "that one over there"). Same as WP-V12; deferred past v1 unless OQ #5 wants it. | S |
| **WP-T2** | `selected` field in `openxr status` JSON (`src/openxr/XRIpc.cpp`) | Expose the explicit selection target (what `active` will resolve to) so the daemon can name the target in confirmation prompts ("close **XR-chat**?") without replicating the dispatcher's resolution order. Pure status addition. | S |
| **WP-T3** | socket2 event `xrmonitorselect>><name>` | The `select` verb currently fires no event (`docs/openxr/05-configuration.md` §6 has no selection event), so the deixis resolver must poll after every select. An event keeps the daemon's dialogue state push-driven like the rest of its socket2 feed. | S |

**Considered and rejected — "arm listening" dispatcher.** The directive floated a dispatcher
so binds can trigger the daemon's armed state. Not needed: `bind = MODS, KEY, exec, hypxrvoice ptt`
already reaches the daemon's control socket with zero compositor change, and putting an
"arm voice" verb in the compositor would bake voice-awareness (arming policy) into it —
exactly what the constraint forbids. Binds stay on the `exec` path.

---

## 9. Open questions for the user

1. **Cloud posture:** is a cloud reasoning tier acceptable at all, or is **local-only** a hard
   requirement? (Drives Proposal C vs B.) If cloud is OK, which provider — Claude (compose, best
   understanding, cheapest text), Gemini Live (cheapest realtime), or OpenAI Realtime (lowest
   latency, priciest)?
2. **Activation default:** PTT-only (maximally private, no always-listening) or presence-armed
   wake word (hands-free in-headset)? Recommendation: ship **both**, default **PTT** with wake as
   opt-in.
3. **Wake engine:** openWakeWord (open) as default OK, with Porcupine (proprietary, better) as an
   opt-in?
4. **Feedback modality:** visual-only default (toast / `hypxrkeys` lane) vs audio TTS
   confirmations (piper)? Recommendation: visual default, `--voice` opt-in.
5. **"Here" precision:** is `anchor local` ("freeze it where it is") a good enough reading of
   "drop it here" for v1, or do you want true **point-and-place** — which needs the read-only
   `hyprctl openxr gaze` addition (§1.4) building on the archived gaze memo?
6. **App-name resolution:** provide a user alias YAML ("Apple TV" → command), auto-inventory
   Omarchy's launcher (walker/uwsm-app), or both?
7. **Repo:** standalone `hypxrvoice` repo (like `hypxrpaper`/`hypxrkeys`), confirmed?
8. **Local model size** if we go local: 3B (fits easily, occasional misfire on complex refs) vs
   7B (safer, more iGPU memory/bandwidth)? Recommendation: default 3B, offer 7B.

---

## 10. Three proposals

### Proposal A — "Minimal: PTT → cloud realtime"

Push-to-talk keybind opens the mic → stream straight to **OpenAI Realtime** (or Gemini Live)
with the §1.5 tool schema → actuate. No wake word, no local ASR, thin daemon.

- **Effort:** **S.** Mostly PipeWire capture + a realtime WebSocket + the tool→hyprctl glue.
- **Monthly cost:** ~**$1–5/mo** at 50 commands/day via cascade-per-command (mic only open on PTT).
- **Latency:** sub-2 s (realtime models are fast once streaming).
- **Privacy:** mic closed except on PTT; **audio leaves the box** each command; no always-listening.
- **Best when:** the user wants the fastest path to "it works," is fine with cloud, and PTT is an
  acceptable interaction.

### Proposal B — "Full cascade, hybrid reasoning" (RECOMMENDED)

The complete four-tier cascade (§2): T0 presence/PTT gate → T1 local wake → T2 local
whisper+silero + intent gate → T3 **local LLM by default, cloud (Claude tool-use) escalation for
hard cases**. Visual feedback via a `hypxrkeys`-style lane; piper TTS opt-in.

- **Effort:** **L.** Wake + streaming ASR + intent gate + local LLM tool-calling + cloud
  escalation + dialogue/deixis state + feedback overlay.
- **Monthly cost:** ~**$0–2/mo** (most commands local; only escalated ones bill, and Claude text
  is < $0.002 each).
- **Latency:** ~1.5–3 s local path; cloud escalation a bit more.
- **Privacy:** strongest — mic closed outside armed states, in-headset-only mode available, and a
  local-only switch that opens **no** network socket.
- **Best when:** the user wants the real product — cheap, private, hands-free-capable, with cloud
  as a quality backstop rather than a dependency. **This is the recommendation.**

### Proposal C — "Local-only"

Proposal B with the cloud tier removed entirely. Everything on-device: whisper.cpp + silero +
Qwen/SmolLM tool-calling.

- **Effort:** **M** (B minus the cloud-escalation plumbing).
- **Monthly cost:** **$0**.
- **Latency:** ~1.5–3 s.
- **Privacy:** **absolute** — nothing ever leaves the machine; no API key exists.
- **Best when:** cloud is a hard no. The narrow command domain makes this genuinely viable, not a
  compromise — it just loses some robustness on unusual phrasing.

**Recommendation: Proposal B**, built local-first so it *is* Proposal C until the day the user
opts into cloud escalation. Ship the local path (which is also the private path) first; add the
one-tool Claude escalation once the local reasoner's failure cases are characterized.

---

## 11. Work-package backlog (recommended: Proposal B)

Critical path: **V1 → V2 → V3 → V4 → V5 → V6**; V7–V12 layer on. Each WP is independently
reviewable with a crisp acceptance test, sized like the HypXRland/hypxrkeys WPs.

- **WP-V1 — Repo skeleton + IPC client + tool→hyprctl actuator.** New `hypxrvoice` repo (BSD-3,
  hypxrpaper shape). Command-socket + socket2 clients (discover via
  `HYPRLAND_INSTANCE_SIGNATURE`); the §1.5 tool set with each tool wired to its `hyprctl openxr`
  / `dispatch exec` call; live-state reader (`-j openxr`/`clients`). *Accept:* a scripted tool
  call (`set_anchor active body`, `create_monitor phone`) actuates against a running (nested/live)
  HypXRland and the change is visible in `-j openxr`; unknown verb degrades cleanly.

- **WP-V2 — PipeWire capture + AEC + source policy.** Open the mic via PipeWire, insert
  `module-echo-cancel`, select `wivrn.source` vs default by presence. Ring buffer + clean
  teardown. *Accept:* captures from the headset mic in-session and the desk mic at-desk; played-back
  audio doesn't leak into the captured stream (AEC verified).

- **WP-V3 — Tier 0 contextual gate (PTT + presence auto-arm).** Control socket for
  `ptt`/`arm`/`mute`; keybind + controller-button routing; presence auto-arm off socket2
  `openxractive` + keyboard-activity suppression. *Accept:* mic PCM is open **only** while PTT
  held or (armed-mode) in-headset-and-idle, observable in `pw-top`; doffing the headset closes it.

- **WP-V4 — Tier 2 ASR + endpointing + intent gate.** whisper.cpp/faster-whisper `small` + silero
  VAD; keyword pre-filter + embedding intent classifier. *Accept:* utterances transcribe with
  <1.5 s endpoint latency; non-command chatter is rejected by the intent gate (measured
  precision/recall on a scripted set); only accepted transcripts escalate.

- **WP-V5 — Tier 3 local reasoner (GBNF tool-calling).** ROCm-llama.cpp on the 890M iGPU running a
  3B model with a grammar constraining output to the §1.5 tool schema; live-layout context block.
  *Accept:* the ten §1.2 example utterances each produce the correct tool call with valid JSON,
  offline, in ~1–2 s; ambiguous refs emit `clarify`.

- **WP-V6 — Deixis + dialogue state + confirmation gate.** Last-referenced/candidate-set/undo/pending
  state (§4); `active`-target deixis; destructive two-step confirm (§1.6); "the other one" /
  "undo that" / "I meant X" repair. *Accept:* "drop this here" targets the hovered monitor;
  "close that" asks before destroying; "no, the other one" flips selection without re-reasoning.

- **WP-V7 — Tier 1 wake word.** openWakeWord default (Porcupine opt-in), gated by V3's armed state.
  *Accept:* the wake phrase opens Tier 2 hands-free in-headset; false-accept rate acceptable over a
  long idle session; PTT-only mode skips this tier entirely.

- **WP-V8 — Cloud escalation (Claude tool-use).** STT→Claude Messages API (tool-use, strict JSON,
  prompt-cached schema)→result, triggered when the local reasoner is low-confidence or the user
  opts a command to cloud. Key handling + masking. *Accept:* a phrasing the local model misfires on
  is handled correctly via Claude; local-only mode opens no network socket.

- **WP-V9 — Feedback (visual + optional TTS).** `hyprctl notify` toast + a `hypxrkeys`-style
  "voice" overlay lane showing recognized command/status; `--voice` piper confirmations. *Accept:*
  every actuation shows a confirmation; the "listening/thinking" state is always visible.

- **WP-V10 — App-name resolution.** User alias YAML + Omarchy launcher inventory for `launch_app` /
  `focus_or_move_app`. *Accept:* "open the browser" / "have YouTube follow me" resolve to the right
  command/window across a small alias set.

- **WP-V11 — Lifecycle + config.** systemd user service + `exec-once` support; a hyprlang/YAML config
  (tiers, engines, aliases, presets, provider keys); `systemctl --user stop` hard-kills the mic.
  *Accept:* survives reload, restarts on crash, has its own log; config changes apply on restart.

- **WP-V12 (= WP-T1, §8a) — (optional, compositor repo) read-only `hyprctl openxr gaze`.** Returns the monitor under
  the head-ray / the ray hit-point in `LOCAL_FLOOR`, building on `research/archive/16-gaze-grab.md`,
  behind `HAVE_OPENXR`. Enables true point-and-place "put it here / that one over there." *Accept:*
  the verb returns the correct monitor for a known head pose; `hypxrvoice` uses it to resolve
  pointing-deixis. **Deferred past v1** unless OQ #5 says otherwise.

---

## 12. Sources

**Voice models & pricing (web, 2026-07):**
- OpenAI Realtime pricing — https://developers.openai.com/api/docs/pricing ,
  https://callsphere.ai/blog/vw2c-openai-realtime-cost-per-minute-math-2026 ,
  https://hackernoon.com/openai-realtime-api-pricing-in-2026-real-world-data-from-4000-measured-sessions ,
  https://tokenmix.ai/blog/openai-realtime-voice-api-2026-cost-latency
  (gpt-realtime-2 $32/$64 per 1M audio tok, cached in $0.40; mini ~⅓; user 1 tok/100ms, assistant 1 tok/50ms; ~$0.18–0.46/min uncached).
- Gemini Live pricing — https://ai.google.dev/gemini-api/docs/live-api ,
  https://ai.google.dev/gemini-api/docs/pricing (~$3/$12 per 1M audio tok, ~$0.005/$0.018 per min, free tier).
- Anthropic voice status — https://www.datastudios.org/post/claude-voice-features-explained-current-status-and-upcoming-real-time-updates ,
  https://news.ycombinator.com/item?id=44116535 (consumer push-to-talk voice; **no developer Realtime/streaming speech API**).
  Claude Messages API tool-use / strict JSON / model pricing (Opus 4.8 $5/$25, Sonnet 5 $3/$15 intro $2/$10, Haiku 4.5 $1/$5 per MTok) per the `claude-api` skill.

**Wake word:**
- openWakeWord — https://github.com/dscripka/openWakeWord (15–20 models/core on RPi3, Apache).
- Porcupine — https://picovoice.ai/products/voice/wake-word/ ,
  https://github.com/Picovoice/wake-word-benchmark (~1MB, <4% RPi3 core, 97%+, easy custom training, proprietary).

**Local ASR / VAD:**
- whisper.cpp — https://github.com/ggml-org/whisper.cpp ; faster-whisper — https://github.com/SYSTRAN/faster-whisper ;
  whisper_streaming/WhisperX + silero-VAD latency — https://www.promptquorum.com/power-local-llm/local-whisper-stt-comparison-2026 ,
  https://medium.com/@aidenkoh/how-to-implement-high-speed-voice-recognition-in-chatbot-systems-with-whisperx-silero-vad-cdd45ea30904
  (tiny <0.5s, small/medium 0.5–2s, VAD-gated streaming ~380–800 ms).

**Local LLM on Strix Point (Ryzen AI 9 HX 370):**
- https://www.runlocalai.co/hardware/amd-ryzen-ai-9-hx-370 ,
  https://github.com/ggml-org/llama.cpp/issues/19396 (RDNA 3.5 + XDNA 2; ROCm llama.cpp on the iGPU works; Qwen3 30B Q8 ~18–23 tok/s; ~90–120 GB/s memory-bandwidth-bound; NPU LLM throughput software-limited → iGPU is the path).
- Small-model tool calling / grammar-constrained JSON — https://qwen.readthedocs.io/en/latest/framework/function_call.html ,
  https://www.bentoml.com/blog/the-best-open-source-small-language-models ,
  https://localaimaster.com/blog/best-ollama-models-tool-calling ,
  https://insiderllm.com/guides/home-assistant-local-llm-guide/ (Qwen2.5-7B / SmolLM3-3B / Ministral-3B; llama.cpp GBNF grammars make invalid JSON impossible; 3B works, 7B safer).

**Local TTS / audio:**
- piper — https://github.com/rhasspy/piper (fast local neural TTS, real-time on RPi5, GPL-3 since Oct 2025).
- `pipewire-module-echo-cancel` (WebRTC AEC) for the play-through-the-same-headset problem.

**HypXRland in-tree (integration points, verified 2026-07-12):**
`src/openxr/XRIpc.cpp:16` (`openxr status` JSON), `:102` (`userPresence`), `:139-227` (all
`openxr` subverbs: status/enable/disable/create/destroy/select/anchor/move/rotate/scale/distance/center/adaptive/dock/undock/roam/layout);
`src/config/legacy/DispatcherTranslator.cpp:800,927` (`xrmonitor` dispatcher);
`src/debug/HyprCtl.cpp:455` (`clients` JSON), `:1999-2001` (clients/activewindow), `:2045`
(`getReply` command choke-point), `:2323` (`.socket.sock` path);
`src/managers/EventManager.cpp:21` (`.socket2.sock` path);
`src/managers/KeybindManager.cpp:41` (`exec` dispatcher);
`docs/openxr/05-configuration.md` §3–6 (`xrmonitor` grammar, dispatcher verbs, `hyprctl openxr`,
socket2 events incl. `openxractive`);
`docs/openxr/research/05-xr-screenkey.md` (companion-tool + overlay-lane + libinput-capture
precedent); `docs/openxr/research/archive/16-gaze-grab.md` (head-ray targeting, for point-and-place deixis).
