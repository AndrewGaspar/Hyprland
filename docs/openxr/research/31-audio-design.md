# 31 — Audio design: themeable earcons for HypXRland and the Omarchy desktop

**Status: research plus a working renderer prototype.** Nothing ships in the compositor yet. The
prototype is `contrib/sounds/hypxr-sound-render.py` and three example recipes under
`contrib/sounds/themes/`. Rendered output from them (not committed) lives in
`~/.cache/hypxrland-research/audio-samples/`, and that directory's `index.html` page lets you
audition them.

**Audience:** the user, who decides direction (§11), and the agent who builds phase A0 (§10).
Compositor references are against `hypxrland` at `7f6363231`.

---

## 0. TL;DR

1. **Use a separate user daemon, `hypxrsound`, and keep audio out of the compositor.** It listens on
   socket2, maps events to sounds, and plays pre-rendered, cached PCM through PipeWire with
   `media.role=Notification`. The compositor's only job is to emit good *semantic* events. Several
   events we need do not exist yet: a settled don/doff, session lock, XR errors, and the cue
   geometry needed for spatial pan (§4.2).
2. **Themes are authored as one small declarative recipe** (`sounds/recipe.toml`). Each event gets a
   few notes, and a note is an oscillator, an envelope, a sweep, a filter and a pan. Pitches are
   written as scale degrees, so the whole theme stays in one key. Named timbres are shared. *Off*
   sounds are `mirror`s of *on* sounds. Recipes are rendered to WAV when a theme is installed or
   switched. In the prototype a whole theme of 19 events is 130–160 lines of TOML, comments
   included, and renders in about 1.5–2 s with numpy only. Authors can also override any event with a hand-made `.ogg`, so
   sample packs remain possible without being required.
3. **Level by loudness, not by peak.** Peak-normalizing alone left a 16 LU spread between the
   prototype themes. Retro squares came out 10 LU louder than glass bells at the same peak.
   Loudness-matching to a theme target (−16 LUFS for a primary event), with a −3 dBFS peak ceiling
   and a per-event `level_db` trim, makes theme switches level-neutral. It also encodes the hierarchy
   *feedback < state change < alert*.
4. **XR routing:** while a headset session is live, play to the headset's sink, `wivrn.sink` for the
   Quest or the XREAL's USB sink. Otherwise play to the default sink. **Spatial pan without an
   HRTF** uses the monitor's azimuth relative to the head. A constant-power pan, an interaural time
   difference of up to 0.66 ms, and a far-ear head-shadow low-pass come to about 40 lines of numpy
   and apply only to monitor-scoped cues (§7).
5. **Omarchy integration needs no change to Omarchy.** `omarchy-theme-set` already stages theme
   subdirectories, including for themes cloned from git. Non-Lua data files pass its safety filter.
   A `theme-set.d` hook re-renders and tells the daemon to reload. Stock themes without `sounds/`
   fall back to a default theme that ships with the daemon (§8).
6. **earshot** (Leonxlnx/earshot, MIT) is an agent *listening* toolkit built on CLAP and an AudioSet
   tagger, plus a physically-modelled procedural "forge". We reuse **ideas**: modal contact
   synthesis, the model-free measurements (LUFS, spectral centroid, tail), and its sound-design rules.
   We do not reuse its code or models: they need ~1.4 GB of models and scipy, torch and librosa. We
   take no assets from it, and it ships none (§2).

---

## 1. Goals and non-goals

**Goals.**
* Short, tasteful, *consistent* sounds for core XR interactions: don/doff, session start/stop,
  monitor create/destroy, grab/release, select, adaptive dock/undock, and errors. These later
  extend to desktop events: boot/login/logout/shutdown, lock/unlock, notifications and battery.
* Any Omarchy theme can ship its own audio design, and authoring one is **cheap**: no DAW, no
  licensing research, no binary assets needed.
* Sounds follow the active headset's output and, where useful, come from the monitor that caused
  them.
* The design never surprises, never fatigues and is easy to silence.

**Non-goals.**
* A general audio engine in the compositor.
* An HRTF/ambisonic renderer. If the headset runtime ever exposes spatial audio, it can take over
  later.
* Music, ambience beds, or per-application sounds. Those belong to apps or `hypxrpaper`.
* Replacing libcanberra for GTK apps. We *export* our themes in the freedesktop layout so those apps
  follow along (§3.1).

---

## 2. earshot (github.com/Leonxlnx/earshot): findings

Cloned read-only to `~/.cache/hypxrland-research/earshot` at `108763c` ("earshot 0.1.0"). Nothing in
it was installed or executed. Its install needs torch, transformers, librosa, scipy, soundfile, soxr,
scikit-learn, matplotlib and ~1.4 GB of HuggingFace models. We do not need any of that to reuse what
is valuable.

**What it is.** "Ears for coding agents": a CLI, Python API and MCP server that turns audio into
evidence an agent can reason about.
* `describe`, `match`, `rank` and `find` use CLAP (`laion/larger_clap_general`) text–audio embeddings
  with calibrated z-scores against a 322-phrase vocabulary, plus the AudioSet AST tagger.
* `realness` is a trained classifier that tells real recordings from synthesized ones.
* `timeline`, `sheet` and `critique` produce a listening transcript, an annotated spectrogram image,
  and a scorecard against 7 professional launch films.
* `measure` gives model-free meters: LUFS, true peak, LRA, envelope, spectrum, tempo and key.

**How it makes sounds: `earshot forge`.** Recipes are *Python modules*
(`earshot/forge/recipes/{click,keypress,pluck,pop,whoosh,impact}.py`). Each declares a parameter
`SPACE` (ranges, sometimes log-scaled) and a physical model. The model's pieces are a contact force
pulse (raised cosine plus grit), modal resonators (damped sinusoids for the shell, housing and
finger), filtered noise, a procedural room IR, and a "microphone" stage (low cut, soft top, pink
noise floor). An evolutionary search then mutates parameters and scores each candidate with the
critic: CLAP match against named confusions, realness, distance to reference recordings, and a
1/3-octave tonal-balance and tail check.

The repo is honest about how far this goes (`docs/LIMITS.md`, README "Goodhart"). The search
optimizes earshot's own models, so after a search those models stop telling made from real. The
model-free 1/3-octave and tail measurements, plus a human blind test, are the real judges.

**Parameter/theming model.** There is none for themes. A recipe is code, with ranges plus defaults.
The separate `skills/real-sound` path is a JSON *mix* format (samples, slices, pools, rules on cue
frames, buses, master) that places *real recordings* on a film's cue list. It says outright that
code-synthesized beeps are a "cheap tell" for **launch films**.

**License.** MIT, © 2026 Leon Lin.

**What we reuse.**

| Kind | Take | Leave |
|---|---|---|
| Ideas | (1) A sharp contact into a few *very* damped modes reads as a physical object, not a beep. Our `tactile` theme is built this way, with `noise→bandpass` as the contact and `bell` partials with 2–6 ms decays as the shell. (2) Vary *takes*, not pitch, for repeated actions. Cycle through 4+ takes with ±1 dB, and keep one stable voice for the click. (3) A ladder of level by importance. (4) Keep effect centroids at or below ~6.5 kHz, because 9–14 kHz "pings" fatigue. (5) Real sounds have a tail. Don't chop to silence, fade. | The launch-film rules (music drops, cue sync to picture). |
| Measurement | Model-free meters as an authoring **lint**: integrated LUFS, peak, spectral centroid, tail length. Our renderer reports LUFS and peak, and the verifier computes centroid. | CLAP/AST/realness models. They are overkill and need 1.4 GB, and an earcon is *supposed* to sound designed. |
| Code | Nothing verbatim. The prototype reimplements the few primitives it needs: TPT state-variable filter, modal partials, noise IR room, BS.1770 K-weighting. They are standard DSP and are written without scipy, so a theme renders on a stock Omarchy box. MIT would allow a copy with attribution if a later phase wants `dsp.py` wholesale. | |
| Assets | None. earshot ships no audio, and its `real-sound` skill pulls third-party recordings under per-item licences. | |

**Where earshot could still help later.** It could serve as an *offline* QA tool for theme authors
who want it: `earshot describe grab.wav` gives you "sounds like: a soft key release click". That is
an optional dev-time check, never a runtime dependency (WP A6).

---

## 3. Prior art

### 3.1 freedesktop Sound Theme and Sound Naming specs

* **Theme spec (0.2, 2008).** Themes live in `$XDG_DATA_DIRS/sounds/<theme>/` and contain an
  `index.theme` (`[Sound Theme] Name=, Inherits=, Directories=`). Each directory has an
  `OutputProfile` (`stereo`, `5.1`, …). Files are `<name>.oga|.ogg|.wav`, and a `<name>.disabled`
  file silences a name. Lookup walks the theme, then its `Inherits` chain, then `freedesktop`. If a
  name is not found, the lookup **strips the last `-suffix` and retries**: `dialog-error-serious` →
  `dialog-error` → `dialog`.
* **Naming spec.** The spec groups names into alerts, notifications, actions, input feedback and
  game. The only vendor convention is the **`x-<binary>-` prefix**, so our names are `x-hypxr-*`.
  Names already in the spec that we can map onto directly:
  * desktop: `system-bootup`, `system-ready`, `system-shutdown`, `desktop-login`,
    `desktop-logout`, `desktop-screen-lock`
  * services and devices: `service-login`, `service-logout`, `device-added`, `device-removed`
  * windows: `window-new`, `window-close`, `window-move-start`, `window-move-end`,
    `window-slide-in`, `window-slide-out`
  * feedback: `item-selected`, `drag-start`, `drag-accept`, `drag-fail`
  * alerts and power: `dialog-error`, `dialog-warning`, `message-new-instant`, `battery-low`,
    `power-plug`, `power-unplug`, `suspend-start`, `suspend-resume`

  There is no unlock name and no headset name.
* **What is installed here.** `sound-theme-freedesktop` 0.8 (35 `.oga` files in
  `/usr/share/sounds/freedesktop/stereo/`), and `libcanberra` 0.30 with `canberra-gtk-play`. The
  GNOME setting is `theme-name='freedesktop'`, `event-sounds=true`.

**Decision:** the event vocabulary *is* freedesktop names. We use spec names where they exist and
`x-hypxr-*` where they do not (§4 table). The daemon resolves each event through a fallback chain,
for example `x-hypxr-monitor-new → window-new`. A user can therefore point the daemon at *any* XDG
sound theme, such as Ocean or freedesktop, and get reasonable sounds, and our rendered themes can be
**exported** as a valid XDG theme (`index.theme` + `stereo/*.oga`) so GTK/KDE apps play them too.

### 3.2 libcanberra / gsound on modern PipeWire

libcanberra has been effectively unmaintained for over a decade (last release 0.30, 2012). GNOME
developers advise GTK4 ports to use GStreamer or `GtkMediaFile` directly instead. Under PipeWire it
goes through `pipewire-pulse`, which has had role-related bugs. The streams carry
`media.role=event`, and issues report they could be missed by pavucontrol volume control. gsound is
only a GObject wrapper over canberra and is not installed here. KDE's Plasma 6 work went the
*opposite* way: KNotifications passes XDG sound **names** to canberra, and Plasma ships the new
**Ocean** sound theme.

**Decision:** do not build on canberra. Its value is the theme *lookup*, which takes about 40 lines
to reimplement. We still *export* to the XDG layout so canberra-using apps benefit.

### 3.3 What the platforms do

| Platform | Model | Lesson for us |
|---|---|---|
| GNOME | One "alert sound" picker. UI event sounds are mostly off. `event-sounds` and `input-feedback-sounds` gsettings exist. | Default to *few* sounds. Input feedback sounds are opt-in. |
| KDE Plasma 6 | Per-event config in `notifyrc`. XDG sound names via KNotifications. Ocean theme. A sound-theme KCM. | Per-event enable/disable plus a theme choice is the expected UX. |
| macOS | Alert sound picker plus a separate **alert volume** slider. A "Play user interface sound effects" toggle. A separate startup-chime toggle. | A **separate volume** for UI sounds is essential. We get it from a dedicated PipeWire stream with `application.name`, which WirePlumber remembers. |
| Windows | **Sound Schemes**: a named mapping of event to `.wav`, with a "No Sounds" scheme and a startup-sound toggle. | "Theme = named mapping of events to files" is the proven abstraction, and a "silent" theme is a first-class option. |
| visionOS (WWDC23 "Explore immersive sound design") | Every interaction gets a subtle sound, because hand and eye input has no physical feedback. **UI sounds are placed at the element that caused them**: keyboard keys sound from each key. Frequent sounds slightly randomize pitch/amplitude, and long repetition calls for alternating takes. | The two XR-specific rules. (1) In XR, select and grab sounds replace missing haptics, so they matter *more* than on a desktop. (2) Spatialize to the source. |
| Meta Horizon OS (Quest) | Soft system UI sounds for pinch/select and panel open/close, plus a system-sounds toggle. Details not verified. | Our sounds play *on top of* Horizon's whenever the user is in Horizon UI, so they must be quieter than system alerts. |

### 3.4 Design guidelines we adopt (numbers)

These come from the sources above, earshot's `sound-design.md` and the classic earcon literature
(Brewster's Glasgow guidelines and Patterson's alarm work). The prototype follows them.

* **Duration tiers.** Feedback (grab, release, select) is **40–120 ms**. State changes (monitor,
  dock, lock) are **150–400 ms**. Lifecycle events (don, session, notify) are **300–600 ms**.
  Ceremony (boot, login, shutdown) is **≤ 1.5 s**. A longer tail is faded rather than chopped
  (`max_ms` with a 60 ms fade).
* **Loudness ladder.** Integrated loudness is relative to the theme target T, where T = −16 LUFS for
  a primary event:
  * feedback: T − 6 to T − 10
  * dock and undock: T − 4
  * lock and notify: T − 2 to T − 3
  * error: T − 2 to T − 3. It is deliberately *not* louder than the rest. An error here means "that
    did not work", not an alarm.
  * lifecycle: T

  The peak ceiling is −3 dBFS. In absolute terms that puts interaction sounds around −22 to
  −26 LUFS, a level that disappears under content, as it should.
* **Spectrum.** Keep energy in roughly 200 Hz–5 kHz and spectral centroids ≤ ~5 kHz. The
  prototype's medians are 1.3 kHz for glass, 2.9 kHz for tactile and 2.7 kHz for retro, and the
  maximum is 5.4 kHz. There is no sub-80 Hz content, since headset and glasses speakers cannot
  reproduce it and it only eats headroom. Avoid long energy at 2–6 kHz, which is the fatiguing band.
* **Families.** One timbre palette per theme, and *on* rises while *off* falls. A pair is the same
  motif mirrored, which `mirror` makes free. One stable voice is used for the click (grab), with 3–4
  takes that differ by ±1 dB and a few cents, cycled so a take never repeats twice in a row.
* **Repetition control.** Rate-limit per event and globally, coalesce bursts (§6.4), and never
  sound continuous motion (cursor, carry, scroll).

---

## 4. Events: what we play, and when

### 4.1 Event map

Each socket2 trigger below is either *existing* (posted by the compositor today, see the
`postEvent` sites in `src/openxr/OpenXRManager.cpp` and doc 05 §6) or **NEW** (proposed in §4.2).

| Sound key | Sound name (fallback chain) | Trigger | Scope | Level | Rate limit |
|---|---|---|---|---|---|
| `don` | `x-hypxr-headset-don` → `device-added` | P0: `openxractive>>1`, stable for 250 ms. P1: **NEW** `xrworn>>1` on DONNING→WORN (after settle) | global | T | 1 per 3 s |
| `doff` | `x-hypxr-headset-doff` → `device-removed` | P0: `openxractive>>0`, stable for 250 ms. P1: **NEW** `xrworn>>0` | global | T | 1 per 3 s |
| `session-start` | `x-hypxr-session-start` → `service-login` | `openxrsessionstate`: {disabled, unavailable, stopping} → {idle, visible, focused} | global | T | 1 per 5 s |
| `session-stop` | `x-hypxr-session-stop` → `service-logout` | `openxrsessionstate>>stopping`, or a running state → unavailable | global | T | 1 per 5 s |
| `monitor-new` | `x-hypxr-monitor-new` → `window-new` | `xrmonitoradded>>NAME` | monitor | T | burst → chord (§6.4) |
| `monitor-close` | `x-hypxr-monitor-close` → `window-close` | `xrmonitorremoved>>NAME` | monitor | T | burst → chord |
| `grab` | `x-hypxr-grab` → `window-move-start` | `xrmonitorgrab>>NAME,1` | monitor | T−6 | 60 ms |
| `release` | `x-hypxr-release` → `window-move-end` | `xrmonitorgrab>>NAME,0` | monitor | T−6 | 60 ms |
| `select` | `x-hypxr-select` → `item-selected` | `xrmonitorselect>>NAME` | monitor | T−9 | 80 ms |
| `undock` | `x-hypxr-undock` → `window-slide-out` | `xrmonitorundocked>>NAME` | monitor | T−4 | 1 per 2 s per monitor |
| `dock` | `x-hypxr-dock` → `window-slide-in` | `xrmonitordocked>>NAME` | monitor | T−4 | 1 per 2 s per monitor |
| `error` | `x-hypxr-error` → `dialog-error` | **NEW** `xrerror>>CODE,NAME` | monitor or global | T−2 | 1 per 1 s |
| `boot` | `system-bootup` | first session after boot: Omarchy `post-boot` hook | global | T | once |
| `login` | `desktop-login` | daemon start, once per `$XDG_RUNTIME_DIR` | global | T | once |
| `logout` / `shutdown` | `desktop-logout` / `system-shutdown` | the user unit's `ExecStop` (blocking, ≤ 1.5 s) | global | T | once |
| `lock` / `unlock` | `desktop-screen-lock` / `x-hypxr-screen-unlock` | **NEW** `sessionlock>>1/0`. Interim: logind `Lock`/`Unlock` signals | global | T−3 | 1 per 2 s |
| `notify` | `message-new-instant` | omarchy-shell notification hook (open Q) | global | T−2 | 1 per 1.5 s, off under DND |
| `battery-low` | `battery-low` | Omarchy `battery-low` hook | global | T | — |

Deliberately silent:
* `xrmonitoranchor` and `xrlayout2dsync`, which are side effects of other actions.
* `xrmonitorquad`, which is invisible to the user.
* The stock `monitoradded`/`monitorremoved` that the plug funnel fires on every don/doff. Unplugging
  does **not** post `xrmonitorremoved` (`OpenXRManager.cpp:3404` fires only on destroy), so the XR
  events are already the right ones.
* Hover, carry motion and cursor crossing.

### 4.2 Missing compositor events

The proposals below are small and additive. No existing payload changes.

1. **`xrworn>>1|0`.** The don/doff edge *as the FSM sees it* (research/30 §4). It is posted on
   DONNING→WORN (the head has settled and monitors are fading in) and on WORN→DOFFING. It is better
   than `openxractive` for three reasons:
   * The sound lands with the visual fade-in instead of 35 ms after the proximity edge, while the
     user is still adjusting the glasses.
   * WiVRn's session-create *visibility sprint* (doc 05 §7.1) cannot chirp a false don.
   * On XREAL it follows the hardware display-toggle presence signal through the same FSM, so both
     headsets produce one consistent edge. Whether `openxractive` alone tracks an XREAL doff in
     every `monitors_follow_session` mode was not verified.

   Post it where the manager applies an FSM transition into or out of `XR_DON_WORN`. The FSM is in
   `XRDonDoff.cpp`, and its input comes from `donDoffInput(...)` at `OpenXRManager.cpp:4629`. Size: XS.
2. **`xrerror>>CODE,NAME`.** At the moment an error is only a non-zero `hyprctl` reply and a log
   line. Useful codes:
   * `verb` (a refused `xrmonitor` verb)
   * `grab-refused`
   * `layer-cap` (no free quad)
   * `session-lost` (a session loss that was not requested, research/17 L4)
   * `create-failed`

   Size: S. Each site already has the reason string.
3. **`sessionlock>>1|0`.** This one is generic and upstreamable. It is posted from the
   ext-session-lock protocol on lock and unlock. `omarchy-hyprland-session-locked` currently infers
   lock state from `solitaryBlockedBy` and admits "Hyprland reports no lock state directly". Size:
   XS.
4. **`xrcue>>KIND,NAME,AZ,EL,DIST`** (P2, optional). Monitor-scoped events can carry the head-relative
   azimuth and elevation (degrees) and the distance (m) of the monitor at the moment of the
   interaction. The main thread computes them from the last head pose (`gazeSampleNow()`). This
   removes the daemon's follow-up `hyprctl -j openxr` and `gaze` queries and the race against a
   monitor that is already gone (`monitor-close`). It is a separate event family so that the
   existing events' payloads stay frozen. Without it, P1 spatial pan does one ~1–3 ms socket query
   per cue, which is acceptable.

Each new event needs a doc 05 §6 table row and an emission test. Event posting is testable with the
existing hyprtester socket2 helpers in `--xr` runs.

---

## 5. Authoring: recipes, not sample packs

### 5.1 The choice

| | Hand-made sample pack | **Declarative recipe → render** (recommended) |
|---|---|---|
| Cost to author a full theme | A DAW, sound design skill, hours to days, licence checks for any library sound | Edit ~150 lines of TOML. Change `scale.root` and every event retunes. Copy a theme and tweak 3 timbres for a variant. |
| Consistency (one key, one palette, mirrored pairs) | Manual discipline | Structural: `deg`, `timbre`, `mirror` |
| Size in a theme repo | 20–40 OGGs, ~0.3–1 MB | ~5 KB of text |
| Safety in an untrusted git theme | Binary blobs, but harmless audio | Pure data, clamped. Passes Omarchy's staging filter (§8). |
| Quality ceiling | Unlimited, using real recordings | High for abstract earcons (bells, chimes, chip, clicks). Lower for realistic foley. |
| New events added later | Every theme is missing them | Inherit via a fallback chain, or one `like = "…"` line |

**Recommendation:** use the recipe as the primary format, with **per-event sample override**. A file
`sounds/<event>.ogg|.wav` next to the recipe wins over the recipe for that event, so a theme can
mix procedural and hand-made sounds. Renders are cached by recipe hash, so the cost is paid once
per theme change.

### 5.2 Schema (as implemented in the prototype)

```toml
[theme]
name = "glass"
description = "Soft glassy bells…"
peak_dbfs = -3.0              # hard ceiling
loudness_lufs = -16           # loudness of a level_db = 0 event (omit → peak-normalise)
scale = { root = "E5", mode = "major-pentatonic" }   # major minor dorian lydian major/minor-pentatonic
fx = { reverb = { wet = 0.22, rt60 = 0.55, bright = 0.7 } }   # theme-wide; or echo = {delay, feedback, mix, taps}
tail_db = -50                 # trim the tail below this (rel. peak)

[theme.timbres]               # named presets; a note's own keys win
glass = { wave = "bell", partials = [[1, 1, 0.34], [2.32, 0.32, 0.16], [4.25, 0.1, 0.05]] }
pad   = { wave = "fm", fm = { ratio = 2.0, index = 1.2, decay = 0.3 }, env = { a = 0.12, d = 0.3 } }

[events.don]
level_db = 0                  # place in the loudness ladder (<= 0)
max_ms = 600                  # cap; a capped tail gets a 60 ms fade
notes = [
  { t = 0.00, dur = 0.42, timbre = "glass", deg = 0, db = -2, pan = -0.15 },
  { t = 0.07, dur = 0.40, timbre = "glass", deg = 2, db = -3 },
  { t = 0.14, dur = 0.42, timbre = "glass", deg = 4, db = -4, pan = 0.15 },
]

[events.doff]
mirror = "don"                # time-reverse the sequence, invert sweeps
transpose = -2                # semitones

[events.grab]
level_db = -6
variants = 3                  # render grab.wav, grab.2.wav, grab.3.wav; the player cycles them
jitter = { cents = 25, db = 1.0 }
fx = { reverb = { wet = 0.08, rt60 = 0.2 } }
notes = [{ t = 0, dur = 0.07, wave = "sine", deg = 4, to_deg = 5, glide = 0.03, env = { a = 0.002, d = 0.025 } }]
```

Note keys:

| Key | Values |
|---|---|
| `wave` | `sine`, `triangle`, `saw`, `square` (`duty`), `fm` (`fm = {ratio, index, decay}`), `bell` (`partials = [[ratio, amp, decay_s], …]`, modal), `noise`, `pluck` (Karplus-Strong, `damp`) |
| pitch | one of `freq` (Hz), `note` (`"E5"`) or `deg` (scale degree, may be negative) |
| sweep | `to_freq`, `to_note` or `to_deg`, with `curve` (`exp`, `lin`, `ease`) and `glide` (seconds to reach the target) |
| `vibrato` | `{rate, cents}` |
| `env` | `{a, h, d, s, r}`: raised-cosine attack, hold, exponential decay constant, sustain level, release |
| `filter` | `{type = lowpass\|bandpass\|highpass, cutoff, to, q, track}`. A time-varying state-variable filter; `track` makes the cutoff follow pitch. |
| level and placement | `db`, `pan`, `cents` |

Event keys are `notes`, `like`, `mirror`, `transpose`, `level_db`, `max_ms`, `tail_db`, `variants`,
`jitter` and `fx`.

**Untrusted-input rules** (implemented). A theme from a stranger's git repo must be as safe to
render as its `colors.toml`:
* There is no code, include or path anywhere in a recipe. Event names must be `[A-Za-z0-9_-]`.
* Every number is clamped. An event is at most 3 s and 48 notes, a theme at most 8 variants per
  event, and a recipe at most 256 KiB.
* Inheritance loops are detected.
* Malformed input exits with code 2 and writes nothing outside the output directory.

### 5.3 The prototype

`contrib/sounds/hypxr-sound-render.py` needs Python ≥ 3.11 (`tomllib`) and numpy (the Omarchy
image already has numpy 2.5 system-wide). Run it like this:

```sh
contrib/sounds/hypxr-sound-render.py contrib/sounds/themes/*/recipe.toml \
    -o ~/.cache/hypxrland-research/audio-samples --per-theme --check --index
```

* **Signal chain.** Notes are summed into a stereo bus with constant-power pan. Then come
  echo/reverb (FFT convolution with a decorrelated noise IR), a 15 Hz DC blocker, the tail trim and
  length cap, a 1 ms in-fade and 6 ms out-fade, and finally BS.1770 K-weighted loudness matching
  under the peak ceiling.
* **Output.** 48 kHz 16-bit stereo WAV, plus `manifest.json` per theme with milliseconds, peak and
  LUFS for every event and take.
* **Checks.** `--check` fails on any of:
  * duration outside 40–600 ms (1.6 s for boot, shutdown, login and logout)
  * peak above −3 dBFS or any clipping
  * DC above 1e-3
  * a non-zero first or last sample

**Results.** There are 3 themes × 19 events, 71 files including variants, rendered in 4.1 s. An
independent read-back verifier found 0 failures. Every primary event lands at −16.0 LUFS, and
peaks fall between −3.0 and −22 dBFS.

| Theme | Character | Lines of TOML | Median / max centroid |
|---|---|---|---|
| `glass` | E major pentatonic bells and FM pad, small bright room | 146 | 1.3 / 2.6 kHz |
| `tactile` | band-passed noise contacts, ms-decay shell modes, sine thunks, wood blocks; nearly dry | 161 | 2.9 / 5.4 kHz |
| `retro` | band-limited (additive) pulse and triangle, C major arpeggios, ping-pong echo | 134 | 2.7 / 4.7 kHz |

An authoring observation from building the prototype: with `mirror`, `timbres` and `deg`, the
second and third themes took about 20 minutes each. Most of that time went to choosing the
*vocabulary*, such as "dock = magnet clack". The syntax took little time.

---

## 6. Playback architecture

### 6.1 Daemon, not compositor

**Against in-compositor playback:**
* It would add a PipeWire client, its thread and its failure modes (a PipeWire restart, a sink that
  disappears) to the process that must never stall. See the xr-threading rules: the frame thread
  already carries strict constraints.
* It would add a dependency and a rebase surface to a fork that rebases onto upstream regularly.
* Half the events (boot, login, lock, notifications, battery) are not compositor events anyway.
* Theme switching, caching and rendering are user-space policy.

**For it:** exact geometry at the instant of the event, and no IPC hop. Both are recovered by the
`xrcue` event (§4.2.4). The socket2 hop itself costs well under 1 ms.

**Decision:** a user daemon, **`hypxrsound`**, in a sibling repository like `hypxrhud`, `hypxrvoice`
and `hypxrpaper`. It runs as a systemd user unit with `After=pipewire.service` and `PartOf=` the
graphical session. That ordering is what lets `ExecStop` play `logout` or `shutdown` before PipeWire
goes away.

### 6.2 Latency budget

| Path | Typical added latency | Notes |
|---|---|---|
| socket2 post → daemon read | < 1 ms | same box, unix socket |
| **spawn `pw-play file.wav`** per cue | ~20–80 ms (estimate, measure in A0) | fork/exec, PipeWire connect, format negotiation, then the default 100 ms `--latency` unless overridden |
| **persistent `pw_stream`**, cached PCM, mixed in-process | one quantum: 256/48k = 5.3 ms | plus device latency |
| Device: laptop speakers | ~10–20 ms | |
| Device: XREAL USB audio | ~10–30 ms | |
| Device: WiVRn → Quest | ~30–80 ms | Network and codec. Audio is already this late for everything, so 50 ms of spawn is noticeable here. |

Feedback sounds (grab, select) need to be under about 50 ms total to feel attached to the action.
Spawn-per-cue is fine for lifecycle sounds and marginal for feedback, which is why A0 uses spawn
and A3 moves to a persistent stream.

**Power.** A persistent stream that keeps outputting silence holds the sink awake: no suspend after
5 s, and the codec and amplifier stay powered on a laptop. The player must
`pw_stream_set_active(false)` when its mix queue is empty and re-activate on the next cue. That
costs one quantum, not a spawn.

### 6.3 Caching and stream properties

* **Cache.** Renders go to `~/.cache/hypxrsound/<theme>/<recipe-sha256>/<event>[.k].wav` together
  with `manifest.json`. The daemon mmaps or loads them all at start. One theme is 1.3–2.4 MB as
  int16 WAV in the prototype (19 events plus variants), and twice that as float32. A sample override (`sounds/<event>.ogg`) is decoded once at
  render time.
* **Stream properties.**
  * `media.role=Notification`, `media.category=Playback`, `application.name=HypXR Sounds`,
    `application.id=io.github.andrewgaspar.hypxrsound`, `node.name=hypxrsound`.
  * Users then get a separate, persistent volume slider in wiremix or pavucontrol, the equivalent
    of macOS's alert volume.
  * WirePlumber 0.5's role-based linking and ducking (`linking.role-based.*`, not enabled by
    default here) can route or duck by role later with no daemon change.
  * Do *not* use `media.role=event`. That is a pulse-ism and is the source of canberra's
    volume-control bugs.

### 6.4 Rate limiting, debounce and coalescing

* **Limits.** Per-event minimum intervals as in §4.1, plus a global token bucket of 6 cues/s with
  bursts of 3. Excess cues are dropped, not queued, because a late earcon is worse than none.
* **Burst becomes a chord.** `xrmonitoradded` arrives in bursts at config load, `hyprctl reload` and
  layout restore. Within a 350 ms window, the 2nd–4th occurrences are played as the same sound
  transposed up the scale: +2, +4 and +7 degrees, rendered as cheap extra variants. The 5th and
  later are dropped. N monitors appearing therefore sounds like one gesture, not a machine gun.
* **Suppressions.**
  * No `monitor-new` sounds for 2 s after `configreloaded`, or until the session first becomes
    visible.
  * Debounce `openxractive` (P0) and `xrworn` (P1) by 250 ms, so the visibility sprint and
    proximity bounce are silent.
  * Dock/undock hysteresis is already in the compositor (research/13 dwell). Apply the per-monitor
    2 s limit as a backstop.
* **Take cycling.** Variants play round-robin, and a take never plays twice in a row.

### 6.5 Mute, DND and "who is listening"

* **Mute.** A muted sink mutes us, with no work needed. The daemon also has its own
  `enabled=false` and per-event toggles (`[events] grab = false`), plus a `hypxrsound mute
  [toggle|on|off]` CLI over a control socket that a keybind can call.
* **DND.** When omarchy-shell's do-not-disturb is on (`omarchy-shell notifications toggleDnd`
  exists; a *query* is still needed, §11), notification-class sounds are silenced (`notify`,
  `battery-low`). Interaction feedback is kept by default, because it is not an interruption. This
  is configurable.
* **Headset state.** XR-only cues (grab, select, dock) play only while `xrworn`. A doffed headset
  makes no sound in an empty room.

---

## 7. XR routing and spatial pan

### 7.1 Routing: follow the active headset

| Active runtime (`hyprctl -j openxr` → `runtimeName`/`systemName`) | Target sink | Evidence |
|---|---|---|
| WiVRn (Quest 3) | `node.name = wivrn.sink` | WiVRn registers it while a client is connected: `server/driver/wivrn_session.cpp` creates sink `wivrn.sink` with description "WiVRn", `media.class=Audio/Sink`, a 5 ms quantum. |
| Monado + XREAL (`systemName` "Monado: Xreal Air Glasses") | the USB sink whose `node.name` matches `alsa_output.usb-XREAL_XREAL_*` | `pw-dump` here, read-only: `alsa_output.usb-XREAL_XREAL_Air_2_Ultra_*.analog-stereo` |
| no session, or the sink is missing | default sink (no `target.object`) | |

* **When to resolve.** The daemon resolves the target on every `openxrsessionstate` edge, and on
  PipeWire registry add and remove for that node. Each stream sets `target.object` per cue.
* **Behaviour.** This is *routing by intent*: XR cues go to the headset even if the user's default
  sink is still the laptop speakers.
* **Exceptions.** Desktop-class cues (lock, notify) follow the default sink. Only XR cues follow the
  headset. A config option (`route = auto | default | <node>`) covers odd setups. Whether WiVRn
  makes itself the default sink is an open question (§11). If it does, `auto` and `default`
  converge.

### 7.2 Spatial pan without an HRTF

Both target devices are *stereo, near-ear*: the Quest's integrated speakers or earbuds, and the
XREAL's temple speakers. Left/right placement is therefore strong and front/back is weak. That fits
the use: "which monitor did that come from" is mostly a left/right question across a horizontal
arc.

**Geometry.** For a cue on monitor M:
* `h` is the head position and `f` the head forward vector, from `hyprctl openxr gaze` or, later,
  the `xrcue` payload.
* `m` is the monitor centre, `anchor.pose.pos` in `hyprctl -j openxr`. For `anchor:local` this is
  in LOCAL_FLOOR. Head-anchored monitors are already head-relative, so use `atan2(x, -z)` on their
  pose directly.
* Project onto XZ: `v = m − h`, `right = normalize(f × up)`. Then
  `az = atan2(v·right, v·f)` and `d = |v|`.

**Rendering.** This happens per cue, in numpy, under 1 ms for a 0.6 s buffer. A daemon without
numpy can use 9 pre-rendered azimuth bins instead.

* **Constant-power pan.** `p = sin(az)`, `gL = cos((p+1)π/4)`, `gR = sin((p+1)π/4)`. Clamp `|p|` to
  ≤ 0.85 so nothing is ever only in one ear, which is uncomfortable on near-ear speakers.
* **ITD (Woodworth).** `τ = (a/c)(|θ| + sin|θ|)` with `a = 0.0875 m` and `c = 343 m/s`, so
  `τmax ≈ 0.66 ms`, about 31 samples at 48 kHz. Delay the far ear by `round(τ·48000)` samples.
  This one cue does much of the work for clicks and transients, which have strong onsets.
* **Head shadow (ILD).** A one-pole low-pass on the far ear, with `fc` sweeping from 18 kHz at 0° to
  about 3 kHz at 90° (`fc = 18000·(1/6)^|sin az|`).
* **Behind the head** (`|az| > 90°`). Use the mirrored front pan, −2 dB, and a gentle both-ear
  low-pass at about 6 kHz. This gives a weak but consistent "behind" colour.
* **Distance.** `g = clamp(d_ref/d, −6 dB, +3 dB)` with `d_ref = 1.2 m`, the default monitor
  distance. There is no reverb send.
* **Ignored.** Elevation, Doppler and room modelling. They add complexity and buy little on
  near-ear speakers.

Only monitor-scoped cues are panned. Global cues (don, doff, session, error without a monitor) stay
centred. That matches visionOS's rule and keeps lifecycle sounds "in the head" where they belong.

---

## 8. Omarchy theme integration

**How Omarchy themes work** (read-only from `/usr/share/omarchy/bin/omarchy-theme-set`, Omarchy
4.0.0.alpha):

* **Staging.** `omarchy-theme-set NAME` stages `/usr/share/omarchy/themes/NAME/*` and then the
  user's `~/.config/omarchy/themes/NAME/*` into `~/.local/state/omarchy/current/next-theme`. That
  directory is then swapped atomically into `~/.local/state/omarchy/current/theme`.
* **Themes cloned from git** (`.git` present) are untrusted. Omarchy refuses *any* `*.lua`,
  `alacritty.toml`, `foot.ini`, `ghostty.conf`, `kitty.conf` and `vscode.json`, and copies
  subdirectories without following symlinks (`stage_installed_dir`). "Everything else a theme ships
  is colour and is kept."
* **Hooks.** After the swap the script runs `omarchy-hook theme-set <slug>`, which executes
  `~/.config/omarchy/hooks/theme-set` and every non-`.sample` file in `theme-set.d/`. There are also
  `post-boot.d` (run from `autostart.lua`, `sleep 2 && omarchy-hook post-boot`) and `battery-low.d`
  hooks.

**Proposal: no Omarchy change is required.**

```
<theme>/sounds/
    recipe.toml          # the theme's audio design (data only; passes the staging filter)
    grab.ogg             # optional per-event override (hand-made)
```

1. `sounds/` is staged like `backgrounds/`, so it lands at
   `~/.local/state/omarchy/current/theme/sounds/`.
2. `hypxrsound` installs `~/.config/omarchy/hooks/theme-set.d/hypxrsound`, which runs
   `hypxrsound reload`. Reload works as follows:
   * It hashes `current/theme/sounds/recipe.toml`.
   * If no cache exists for that hash, it renders in the background, in about 1–2 s. The *old*
     theme's sounds stay active until the render finishes, so a switch never goes silent.
   * It then swaps atomically.
3. The fallback order for `current/theme/sounds/` missing or for an event missing from a recipe is
   the user's pinned theme (`~/.config/hypxrsound/config.toml: theme = "glass"`), then the daemon's
   bundled default theme, then the XDG lookup chain (§3.1).
   * Stock Omarchy themes ship no sounds, so by default your sound theme is *independent* of your
     colour theme until a theme provides one. Config `follow_omarchy_theme = true|false` controls
     this.
4. `post-boot.d/hypxrsound` plays `boot` the first time after a reboot. `battery-low.d/hypxrsound`
   plays `battery-low`.
5. An optional `hypxrsound export-xdg` writes `~/.local/share/sounds/hypxr-<theme>/index.theme`
   (`Inherits=freedesktop`) plus `stereo/*.oga` via `ffmpeg` (present here). It can then set
   `gsettings org.gnome.desktop.sound theme-name`, so GTK and canberra apps, and KDE apps via
   kdeglobals, use the same palette.

**Upstreaming to Omarchy** is optional and later. The useful asks would be:
* `sounds/` as a documented theme directory
* a DND *query* in omarchy-shell
* a notification hook

The same pattern works for `hypxrhud`. Theme colours already reach it through `colors.toml`.

---

## 9. Risks

* **Annoyance is the main failure mode.** Mitigations:
  * conservative defaults (§11 Q1)
  * levels well under content
  * take cycling and rate limits
  * a one-keybind mute

  The design must be judged *after a week of wear*, not on first listen.
* **WiVRn audio latency** (30–80 ms) makes feedback sounds feel detached no matter what the daemon
  does. If grab/select feel late on the Quest, keep them off there and use haptics instead. The
  compositor already does controller haptics (doc 05 "Haptics").
* **Procedural ceiling.** Abstract earcons render well, while realistic foley does not, and
  earshot's own rules call synth beeps a "cheap tell" in film. Theme authors who want realism use
  the per-event sample override.
* **The XREAL sink can disappear** (USB re-enumeration on mode flips; see the stereo-output
  findings). The daemon falls back to the default sink for that cue and re-resolves on the
  registry event.

---

## 10. Phased plan

| WP | What | Where | Size |
|---|---|---|---|
| **A0 (P0)** | `hypxrsound` MVP: a Python asyncio daemon, an XDG socket2 reader, and the §4.1 map over *existing* events (`openxractive` as a debounced don/doff proxy). It renders the active theme with the prototype renderer into the cache, plays by spawning `pw-play --media-role=Notification --target …`, and applies rate limits, burst chords, take cycling, the mute CLI and a systemd user unit. It ships **one** theme (the user's pick of the three) plus a `silent` theme. | new sibling repo | S (~400 lines) |
| A1 | Compositor events `xrworn`, `xrerror` and `sessionlock`, with doc 05 §6 rows and a hyprtester `--xr` emission test for `xrworn` and `xrerror` | Hyprland | S |
| A2 | Routing (§7.1) and spatial pan (§7.2), with geometry from `hyprctl -j openxr` + `gaze` | daemon | S |
| A3 | Low-latency player: a persistent `pw_stream`, in-process mixer, cached PCM, inactive when idle. Either C++ (shares hyprutils idioms) or Python via a tiny C helper. Target: cue → speaker under 15 ms plus device latency. | daemon | M |
| A4 | Omarchy integration: the `theme-set.d`, `post-boot.d` and `battery-low.d` hooks, `follow_omarchy_theme`, per-event sample override, `export-xdg` | daemon | S |
| A5 | Desktop events: login, logout and shutdown via `ExecStart`/`ExecStop`, lock/unlock via `sessionlock` (logind signals in the interim), notify via an omarchy-shell hook with DND | daemon (+ Omarchy ask) | S–M |
| A6 | Authoring kit: `hypxrsound render --watch` re-renders on save and refreshes the audition page (the prototype's `--index`), and a lint for loudness ladder, centroid and duration tiers. An optional dev-only earshot `describe` pass. | daemon | S |
| A7 (optional) | The `xrcue` geometry event (§4.2.4) | Hyprland | S |

**The smallest proof** is A0 with `glass`, a single keybind to mute, and a week of daily wear. Then
re-tune levels from the user's notes before building A3.

---

## 11. Open questions for the user

1. **Defaults.** Which events are on out of the box? The proposal: don, doff, session, monitor-new,
   monitor-close and error on. Grab, release and select on but quiet (T−6/−9). Dock and undock on.
   Desktop events off until A5. Or would you rather start with *everything off except
   don/doff/session/error*?
2. **Which direction?** glass, tactile, retro, or something else: soft felt/marimba, or
   "Omarchy-brand" minimal ticks? Listen at
   `~/.cache/hypxrland-research/audio-samples/index.html`.
3. **Don timing.** Should the don sound play at the *presence edge* (instant, before you can see
   anything) or at *settle/fade-in* (research/30 DONNING→WORN, 0.3–1.5 s later, synced to what you
   see)? The proposal is settle.
4. **Routing.** While you are in the headset, should *all* sounds, including notifications, go to
   the headset, or only XR cues? Does your WiVRn setup make `wivrn.sink` the default sink when it
   connects? That was not verified, since no session was live during this research.
5. **Spatial pan.** Do you want it in the first version, or after the basics have been worn for a
   while?
6. **Feedback on the Quest.** With 30–80 ms streamed-audio latency, do you want grab and select
   sounds there at all, or haptics only?
7. **Repository and language.** Is a new sibling repo `hypxrsound` OK? Is Python acceptable for A0
   (system numpy is present), with the A3 player in C++?
8. **Omarchy.** Should sound themes follow the colour theme automatically (`follow_omarchy_theme`)
   or be chosen independently? Should we upstream `sounds/` as a theme directory, and ask for an
   omarchy-shell DND query and notification hook?
9. **DND semantics.** Should DND silence *everything* we play, or only notification-class sounds?

---

## 12. Sources

* earshot: https://github.com/Leonxlnx/earshot (README; `docs/LIMITS.md`, `docs/DESIGN.md`;
  `earshot/forge/dsp.py`, `earshot/forge/recipes/click.py`;
  `skills/real-sound/references/{sound-design,design-format,licences}.md`). Read at `108763c`.
* freedesktop Sound Naming spec: https://specifications.freedesktop.org/sound-naming/latest/
* freedesktop Sound Theme spec: https://specifications.freedesktop.org/sound-theme/latest/
* GNOME Discourse, "What should we use instead of libcanberra?":
  https://discourse.gnome.org/t/what-should-we-use-instead-of-libcanberra/27185
* PipeWire issues on canberra and event-role streams:
  https://gitlab.freedesktop.org/pipewire/pipewire/-/issues/3933 and
  https://gitlab.freedesktop.org/pipewire/pipewire/-/issues/1704
* KDE Plasma 6 sound-theme proposal: https://invent.kde.org/plasma/plasma-workspace/-/issues/47
  and KNotifications MR 105: https://invent.kde.org/frameworks/knotifications/-/merge_requests/105
* WWDC23 "Explore immersive sound design" (session 10271): summary at
  https://nonstrict.eu/wwdcindex/wwdc2023/10271/
* Brewster et al., earcon guidelines: https://www.dcs.gla.ac.uk/~stephen/earcon_guidelines.shtml
* Local, read-only:
  * `/usr/share/omarchy/bin/{omarchy-theme-set,omarchy-hook,omarchy-hyprland-session-locked,omarchy-toggle-notification-silencing}`
  * `/usr/share/omarchy/default/agents/skills/omarchy/hooks.md`
  * `/usr/share/sounds/freedesktop/`
  * `~/code/wivrn/server/{driver/wivrn_session.cpp,audio/audio_pipewire.cpp}`
  * WirePlumber 0.5.17 `wireplumber.conf`
  * `pw-play --help`
* This tree: `src/openxr/OpenXRManager.cpp` (all `postEvent` sites), `src/openxr/XRIpc.cpp` (the
  `openxr` / `gaze` JSON), doc 05 §6–§7, research/30 §4, `XREAL-DONDOFF.md`.
