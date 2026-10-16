#!/usr/bin/env python3
"""hypxr-sound-render: render a declarative sound-theme recipe to WAV files.

Prototype of the "cheap theme authoring" path from docs/openxr/research/31-audio-design.md.
A theme is ONE data file (recipe.toml or recipe.json): a few notes per event (oscillator,
envelope, pitch sweep, filter, pan), optional shared reverb/echo, and pair-mirroring so that
"off" sounds are derived from "on" sounds. No code is executed from a recipe, every number is
clamped, and nothing outside the output directory is written, so a recipe from a stranger's
theme repo is as safe to render as its colors.toml.

Dependencies: Python >= 3.11 (tomllib) and numpy. Nothing else.

    hypxr-sound-render.py themes/glass/recipe.toml -o ~/.cache/hypxr-sounds/glass
    hypxr-sound-render.py themes/*/recipe.toml -o OUT --per-theme      # OUT/<theme>/<event>.wav
    hypxr-sound-render.py ... --check                                  # sanity report, non-zero on failure

Outputs per theme: <event>.wav (48 kHz, 16-bit, stereo), <event>.<k>.wav for variants, and
manifest.json (event -> files, xdg names, duration, peak, loudness).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import sys
import tomllib
import wave
from pathlib import Path

import numpy as np

SR = 48000
MAX_EVENT_S = 3.0  # hard cap, whatever a recipe says
MAX_NOTES = 48
MAX_VARIANTS = 8
NOTE_NAMES = {"c": 0, "d": 2, "e": 4, "f": 5, "g": 7, "a": 9, "b": 11}
SCALES = {
    "major": [0, 2, 4, 5, 7, 9, 11],
    "minor": [0, 2, 3, 5, 7, 8, 10],
    "dorian": [0, 2, 3, 5, 7, 9, 10],
    "lydian": [0, 2, 4, 6, 7, 9, 11],
    "major-pentatonic": [0, 2, 4, 7, 9],
    "minor-pentatonic": [0, 3, 5, 7, 10],
}


class RecipeError(Exception):
    pass


# ----------------------------------------------------------------------------------------- helpers
def clamp(v, lo, hi):
    return max(lo, min(hi, v))


def num(d: dict, key: str, default: float, lo: float, hi: float) -> float:
    v = d.get(key, default)
    if not isinstance(v, (int, float)) or isinstance(v, bool):
        raise RecipeError(f"'{key}' must be a number, got {v!r}")
    return float(clamp(float(v), lo, hi))


def note_to_hz(name: str) -> float:
    s = name.strip().lower()
    if not s or s[0] not in NOTE_NAMES:
        raise RecipeError(f"bad note name {name!r}")
    semi = NOTE_NAMES[s[0]]
    i = 1
    while i < len(s) and s[i] in "#b":
        semi += 1 if s[i] == "#" else -1
        i += 1
    try:
        octave = int(s[i:])
    except ValueError as e:
        raise RecipeError(f"bad note name {name!r}") from e
    midi = 12 * (octave + 1) + semi
    return 440.0 * 2 ** ((midi - 69) / 12)


class Pitch:
    """Resolves `freq` (Hz), `note` ("E5") or `deg` (scale degree, may be negative / > len) to Hz."""

    def __init__(self, theme: dict):
        sc = theme.get("scale", {})
        self.root = note_to_hz(sc.get("root", "A4"))
        mode = sc.get("mode", "major")
        if mode not in SCALES:
            raise RecipeError(f"unknown scale mode {mode!r} (have: {', '.join(SCALES)})")
        self.steps = SCALES[mode]

    def deg(self, d: float) -> float:
        di = int(math.floor(d))
        frac = d - di  # fractional degree = semitone offset (lets you nudge)
        octv, idx = divmod(di, len(self.steps))
        semis = 12 * octv + self.steps[idx] + frac
        return self.root * 2 ** (semis / 12)

    def resolve(self, n: dict, prefix: str = "") -> float | None:
        if prefix + "freq" in n:
            return num(n, prefix + "freq", 440, 20, 20000)
        if prefix + "note" in n:
            return note_to_hz(str(n[prefix + "note"]))
        if prefix + "deg" in n:
            return self.deg(num(n, prefix + "deg", 0, -28, 28))
        return None


# ----------------------------------------------------------------------------------------- DSP
def envelope(n: int, e: dict) -> np.ndarray:
    """a: attack (s, raised cosine) · h: hold · d: decay time constant (s) · s: sustain 0..1 · r: release (s, end)."""
    t = np.arange(n) / SR
    a = num(e, "a", 0.003, 0.0005, 1.0)
    h = num(e, "h", 0.0, 0.0, 2.0)
    d = num(e, "d", 0.15, 0.002, 5.0)
    s = num(e, "s", 0.0, 0.0, 1.0)
    r = num(e, "r", 0.02, 0.002, 1.0)
    env = np.ones(n)
    na = min(n, int(a * SR))
    if na > 1:
        env[:na] = 0.5 - 0.5 * np.cos(np.linspace(0, np.pi, na))
    after = t - a - h
    m = after > 0
    env[m] = s + (1 - s) * np.exp(-after[m] / d)
    nr = min(n, int(r * SR))
    if nr > 1:
        env[-nr:] *= 0.5 + 0.5 * np.cos(np.linspace(0, np.pi, nr))
    return env


def freq_curve(n: int, f0: float, f1: float | None, curve: str, glide: float) -> np.ndarray:
    if f1 is None or n < 2:
        return np.full(n, f0)
    x = np.linspace(0, 1, n)
    if glide > 0:  # reach the target in `glide` seconds, then hold
        x = np.clip(np.arange(n) / (glide * SR), 0, 1)
    if curve == "exp":
        return f0 * (f1 / f0) ** x
    if curve == "ease":
        x = 0.5 - 0.5 * np.cos(np.pi * x)
        return f0 * (f1 / f0) ** x
    return f0 + (f1 - f0) * x


def phase_of(f: np.ndarray) -> np.ndarray:
    return np.cumsum(f) / SR


def additive(phase: np.ndarray, f: np.ndarray, kind: str, duty: float) -> np.ndarray:
    """Band-limited classic waves by summing harmonics below Nyquist (no aliasing, no scipy)."""
    fmax = float(f.max())
    kmax = int(clamp((SR * 0.45) // max(fmax, 1), 1, 96))
    out = np.zeros_like(phase)
    for k in range(1, kmax + 1):
        if kind == "saw":
            out += np.sin(2 * np.pi * k * phase) / k
        elif kind == "square":
            if duty == 0.5:
                if k % 2:
                    out += np.sin(2 * np.pi * k * phase) / k
            else:  # pulse = difference of two saws
                out += (np.sin(2 * np.pi * k * phase) - np.sin(2 * np.pi * k * (phase + duty))) / k
        elif kind == "triangle":
            if k % 2:
                out += ((-1) ** ((k - 1) // 2)) * np.sin(2 * np.pi * k * phase) / (k * k)
    norm = {"saw": 2 / np.pi, "square": 4 / np.pi, "triangle": 8 / np.pi ** 2}[kind]
    return out * norm


def svf(x: np.ndarray, cutoff: np.ndarray, q: float, mode: str) -> np.ndarray:
    """Time-varying TPT state-variable filter (per-sample loop; sounds are short)."""
    y = np.empty_like(x)
    ic1 = ic2 = 0.0
    k = 1.0 / max(q, 0.05)
    g_all = np.tan(np.pi * np.clip(cutoff, 20, SR * 0.45) / SR)
    sel = {"lowpass": 0, "bandpass": 1, "highpass": 2}[mode]
    for i in range(len(x)):
        g = g_all[i]
        a1 = 1.0 / (1.0 + g * (g + k))
        a2 = g * a1
        a3 = g * a2
        v3 = x[i] - ic2
        v1 = a1 * ic1 + a2 * v3
        v2 = ic2 + a2 * ic1 + a3 * v3
        ic1 = 2.0 * v1 - ic1
        ic2 = 2.0 * v2 - ic2
        y[i] = v2 if sel == 0 else (k * v1 if sel == 1 else x[i] - k * v1 - v2)
    return y


def karplus(n: int, f0: float, damp: float, rng: np.random.Generator) -> np.ndarray:
    period = max(2, int(round(SR / f0)))
    buf = rng.uniform(-1, 1, period)
    out = np.empty(n)
    idx = 0
    for i in range(n):
        out[i] = buf[idx]
        nxt = (idx + 1) % period
        buf[idx] = damp * 0.5 * (buf[idx] + buf[nxt])
        idx = nxt
    return out


def render_note(nd: dict, pitch: Pitch, rng: np.random.Generator, tune_cents: float) -> tuple[np.ndarray, float]:
    """Returns (mono signal, pan)."""
    dur = num(nd, "dur", 0.15, 0.005, MAX_EVENT_S)
    n = int(dur * SR)
    wave_kind = nd.get("wave", "sine")
    f0 = pitch.resolve(nd)
    f1 = pitch.resolve(nd, "to_")
    cents = num(nd, "cents", 0, -1200, 1200) + tune_cents
    mul = 2 ** (cents / 1200)
    if f0 is None:
        f0 = 440.0
    f0 *= mul
    if f1 is not None:
        f1 *= mul
    f = freq_curve(n, f0, f1, nd.get("curve", "exp"), num(nd, "glide", 0, 0, MAX_EVENT_S))
    vib = nd.get("vibrato")
    if isinstance(vib, dict):
        rate = num(vib, "rate", 5, 0.1, 30)
        depth = num(vib, "cents", 15, 0, 200)
        f = f * 2 ** (depth / 1200 * np.sin(2 * np.pi * rate * np.arange(n) / SR))
    ph = phase_of(f) + rng.uniform(0, 1) * (wave_kind not in ("sine", "fm", "bell"))

    if wave_kind == "sine":
        x = np.sin(2 * np.pi * ph)
    elif wave_kind in ("triangle", "saw", "square"):
        x = additive(ph, f, wave_kind, num(nd, "duty", 0.5, 0.05, 0.5))
    elif wave_kind == "fm":
        fm = nd.get("fm", {})
        ratio = num(fm, "ratio", 2.0, 0.1, 16)
        index = num(fm, "index", 2.0, 0, 30)
        idx_decay = num(fm, "decay", 0.1, 0.002, 5)
        mod_index = index * np.exp(-np.arange(n) / SR / idx_decay)
        mod = np.sin(2 * np.pi * phase_of(f * ratio))
        x = np.sin(2 * np.pi * ph + mod_index * mod)
    elif wave_kind == "bell":  # modal: inharmonic partials, each with its own decay
        parts = nd.get("partials", [[1, 1, 0.4], [2.76, 0.5, 0.2], [5.4, 0.25, 0.08]])
        if not isinstance(parts, list) or len(parts) > 24:
            raise RecipeError("'partials' must be a list of [ratio, amp, decay] (max 24)")
        t = np.arange(n) / SR
        x = np.zeros(n)
        for p in parts:
            r, a, dk = (float(clamp(p[0], 0.1, 40)), float(clamp(p[1], 0, 4)), float(clamp(p[2], 0.001, 5)))
            if np.any(f * r > SR * 0.45):
                continue
            x += a * np.exp(-t / dk) * np.sin(2 * np.pi * phase_of(f * r) + rng.uniform(0, 2 * np.pi))
    elif wave_kind == "noise":
        x = rng.standard_normal(n) * 0.5
    elif wave_kind == "pluck":
        x = karplus(n, f0, num(nd, "damp", 0.996, 0.9, 0.9999), rng)
    else:
        raise RecipeError(f"unknown wave {wave_kind!r} (sine triangle saw square fm bell noise pluck)")

    flt = nd.get("filter")
    if isinstance(flt, dict):
        mode = flt.get("type", "lowpass")
        if mode not in ("lowpass", "bandpass", "highpass"):
            raise RecipeError(f"unknown filter type {mode!r}")
        c0 = num(flt, "cutoff", 2000, 20, 20000)
        c1 = flt.get("to")
        cut = freq_curve(n, c0, None if c1 is None else num(flt, "to", c0, 20, 20000), "exp", 0)
        if flt.get("track", False):  # cutoff follows pitch (ratio of f)
            cut = cut * f / f0
        x = svf(x, cut, num(flt, "q", 0.707, 0.1, 20), mode)

    x = x * envelope(n, nd.get("env", {})) * 10 ** (num(nd, "db", 0, -60, 12) / 20)
    return x, num(nd, "pan", 0, -1, 1)


def reverb(x: np.ndarray, rv: dict, rng: np.random.Generator) -> np.ndarray:
    """Small procedural room: exponentially decaying stereo-decorrelated noise IR, high-damped, FFT-convolved."""
    wet = num(rv, "wet", 0.15, 0, 1)
    if wet <= 0:
        return x
    rt60 = num(rv, "rt60", 0.4, 0.05, 3.0)
    bright = num(rv, "bright", 0.5, 0, 1)
    nir = int(rt60 * 1.1 * SR)
    t = np.arange(nir) / SR
    out = np.zeros((len(x) + nir - 1, 2))
    for ch in range(2):
        ir = rng.standard_normal(nir) * np.exp(-6.91 * t / rt60)
        # damping: highs decay faster -> lowpass the IR progressively by mixing a smoothed copy
        smooth = np.convolve(ir, np.ones(8) / 8, mode="same")
        mix = np.clip(t / rt60, 0, 1) * (1 - bright)
        ir = (1 - mix) * ir + mix * smooth
        ir[: int(0.004 * SR)] *= np.linspace(0, 1, int(0.004 * SR))  # pre-delay-ish soft onset
        ir /= np.sqrt(np.sum(ir ** 2)) + 1e-12
        L = len(x[:, ch]) + nir - 1
        nfft = 1 << (L - 1).bit_length()
        y = np.fft.irfft(np.fft.rfft(x[:, ch], nfft) * np.fft.rfft(ir, nfft), nfft)[:L]
        out[: len(x), ch] += x[:, ch] * (1 - wet * 0.5)
        out[:, ch] += wet * y
    return out


def echo(x: np.ndarray, ec: dict) -> np.ndarray:
    delay = num(ec, "delay", 0.12, 0.01, 1.0)
    fb = num(ec, "feedback", 0.35, 0, 0.9)
    mix = num(ec, "mix", 0.3, 0, 1)
    taps = int(clamp(ec.get("taps", 3), 1, 8))
    pingpong = bool(ec.get("pingpong", True))
    d = int(delay * SR)
    out = np.zeros((len(x) + d * taps, 2))
    out[: len(x)] += x
    g = mix
    for k in range(1, taps + 1):
        src = x[:, ::-1] if (pingpong and k % 2) else x
        out[k * d : k * d + len(x)] += g * src
        g *= fb
    return out


# ----------------------------------------------------------------------------------------- events
def mirror_notes(notes: list[dict]) -> list[dict]:
    """'Off' from 'on': time-reverse the sequence and invert every pitch sweep (rising -> falling)."""
    total = max((n.get("t", 0) + n.get("dur", 0.15)) for n in notes) if notes else 0
    out = []
    for n in notes:
        m = dict(n)
        m["t"] = max(0.0, total - n.get("t", 0) - n.get("dur", 0.15))
        for k in ("freq", "note", "deg"):
            if k in n and "to_" + k in n:
                m[k], m["to_" + k] = n["to_" + k], n[k]
        out.append(m)
    return sorted(out, key=lambda n: n["t"])


def transpose_notes(notes: list[dict], semis: float) -> list[dict]:
    out = []
    for n in notes:
        m = dict(n)
        m["cents"] = n.get("cents", 0) + 100 * semis
        out.append(m)
    return out


def resolve_event(name: str, events: dict, seen: tuple = ()) -> dict:
    """`like = "<event>"` copies another event (then overrides); `mirror = "<event>"` time-reverses it."""
    if name in seen:
        raise RecipeError(f"event inheritance loop: {' -> '.join(seen + (name,))}")
    ev = events.get(name)
    if not isinstance(ev, dict):
        raise RecipeError(f"no event {name!r}")
    base_name = ev.get("like") or ev.get("mirror")
    if not base_name:
        return dict(ev)
    base = resolve_event(base_name, events, seen + (name,))
    merged = {**base, **{k: v for k, v in ev.items() if k not in ("like", "mirror")}}
    notes = ev.get("notes", base.get("notes", []))
    if "mirror" in ev:
        notes = mirror_notes(notes)
    if "transpose" in ev:
        notes = transpose_notes(notes, float(clamp(ev["transpose"], -36, 36)))
    merged["notes"] = notes
    merged.pop("transpose", None)
    return merged


def render_event(ev: dict, theme: dict, pitch: Pitch, seed: int, variant: int, jitter: dict) -> np.ndarray:
    rng = np.random.default_rng(seed)
    notes = ev.get("notes", [])
    if not isinstance(notes, list) or not notes or len(notes) > MAX_NOTES:
        raise RecipeError(f"each event needs 1..{MAX_NOTES} notes")
    tune = 0.0
    gain_j = 0.0
    if variant > 0:
        tune = rng.uniform(-1, 1) * num(jitter, "cents", 0, 0, 200)
        gain_j = rng.uniform(-1, 1) * num(jitter, "db", 0, 0, 6)
    total = max(num(n, "t", 0, 0, MAX_EVENT_S) + num(n, "dur", 0.15, 0.005, MAX_EVENT_S) for n in notes)
    total = min(total, MAX_EVENT_S)
    mix = np.zeros((int(total * SR) + 1, 2))
    timbres = theme.get("timbres", {})
    for nd in notes:
        if "timbre" in nd:  # named preset from [theme.timbres.<name>]; the note's own keys win
            preset = timbres.get(nd["timbre"])
            if not isinstance(preset, dict):
                raise RecipeError(f"unknown timbre {nd['timbre']!r}")
            nd = {**preset, **nd}
        x, pan = render_note(nd, pitch, rng, tune)
        th = (pan + 1) * np.pi / 4  # constant-power pan
        i0 = int(num(nd, "t", 0, 0, MAX_EVENT_S) * SR)
        m = min(len(x), len(mix) - i0)
        if m <= 0:
            continue
        mix[i0 : i0 + m, 0] += x[:m] * np.cos(th) * np.sqrt(2)
        mix[i0 : i0 + m, 1] += x[:m] * np.sin(th) * np.sqrt(2)
    fx = {**theme.get("fx", {}), **ev.get("fx", {})}  # event fx override theme-wide fx
    if isinstance(fx.get("echo"), dict):
        mix = echo(mix, fx["echo"])
    if isinstance(fx.get("reverb"), dict):
        mix = reverb(mix, fx["reverb"], rng)
    return finish(mix, theme, ev, gain_j)


def dc_block(x: np.ndarray, fc: float = 15.0) -> np.ndarray:
    r = 1 - 2 * np.pi * fc / SR
    y = np.empty_like(x)
    for ch in range(x.shape[1]):
        px = py = 0.0
        col = x[:, ch]
        out = y[:, ch]
        for i in range(len(col)):
            v = col[i] - px + r * py
            px, py = col[i], v
            out[i] = v
    return y


def finish(x: np.ndarray, theme: dict, ev: dict, gain_j: float) -> np.ndarray:
    x = dc_block(x)
    # trim the tail below `tail_db` (relative to the peak), then cap at `max_ms`; a capped tail gets a
    # longer fade so a reverb is cut gracefully, not chopped
    mag = np.abs(x).max(axis=1)
    pk = mag.max() + 1e-12
    tail_db = num(ev, "tail_db", num(theme, "tail_db", -50, -80, -20), -80, -20)
    loud = np.nonzero(mag > pk * 10 ** (tail_db / 20))[0]
    end = min(len(x), (loud[-1] if len(loud) else len(x)) + int(0.01 * SR))
    cap = int(num(ev, "max_ms", 600, 40, MAX_EVENT_S * 1000) / 1000 * SR)
    capped = end > cap
    x = x[: min(end, cap)]
    # click-free edges
    fin = int(0.001 * SR)
    fout = min(len(x) // 2, int((0.06 if capped else 0.006) * SR))
    x[:fin] *= (0.5 - 0.5 * np.cos(np.linspace(0, np.pi, fin)))[:, None]
    x[-fout:] *= (0.5 + 0.5 * np.cos(np.linspace(0, np.pi, fout)))[:, None]
    # Level. `level_db` is the event's place in the theme's hierarchy (feedback < notification < alert).
    # With [theme] loudness_lufs set, the event is loudness-matched to (target + level_db) so themes and
    # events compare by how loud they SOUND; without it, it is peak-normalised to (ceiling + level_db).
    # Either way the peak never exceeds `peak_dbfs` (default -3 dBFS).
    ceiling = num(theme, "peak_dbfs", -3.0, -24, -1)
    level = min(0.0, num(ev, "level_db", 0, -30, 0) + gain_j)
    peak = np.abs(x).max() + 1e-12
    if "loudness_lufs" in theme:
        target = num(theme, "loudness_lufs", -18, -36, -8) + level
        gain_db = target - loudness_lufs(x)
        gain_db = min(gain_db, ceiling - 20 * math.log10(peak))
    else:
        gain_db = ceiling + level - 20 * math.log10(peak)
    return x * 10 ** (gain_db / 20)


# ----------------------------------------------------------------------------------------- measure
def biquad(x: np.ndarray, b: list[float], a: list[float]) -> np.ndarray:
    y = np.empty_like(x)
    x1 = x2 = y1 = y2 = 0.0
    b0, b1, b2 = b
    _, a1, a2 = a
    for i in range(len(x)):
        v = b0 * x[i] + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2
        x2, x1, y2, y1 = x1, x[i], y1, v
        y[i] = v
    return y


def loudness_lufs(x: np.ndarray) -> float:
    """BS.1770 K-weighted loudness over the whole (short) clip, ungated. Good enough to compare earcons."""
    hs_b, hs_a = [1.53512485958697, -2.69169618940638, 1.19839281085285], [1.0, -1.69065929318241, 0.73248077421585]
    hp_b, hp_a = [1.0, -2.0, 1.0], [1.0, -1.99004745483398, 0.99007225036621]
    pw = 0.0
    for ch in range(x.shape[1]):
        k = biquad(biquad(x[:, ch], hs_b, hs_a), hp_b, hp_a)
        pw += np.mean(k ** 2)
    return -0.691 + 10 * math.log10(pw + 1e-20)


def check(x: np.ndarray, name: str, lo_s: float, hi_s: float) -> list[str]:
    errs = []
    dur = len(x) / SR
    pk = 20 * math.log10(np.abs(x).max() + 1e-12)
    if not lo_s <= dur <= hi_s:
        errs.append(f"{name}: duration {dur*1000:.0f} ms outside {lo_s*1000:.0f}..{hi_s*1000:.0f} ms")
    if pk > -2.99:
        errs.append(f"{name}: peak {pk:.2f} dBFS above -3")
    if np.any(np.abs(x) >= 1.0):
        errs.append(f"{name}: clipping")
    dc = float(np.abs(x.mean(axis=0)).max())
    if dc > 1e-3:
        errs.append(f"{name}: DC offset {dc:.4f}")
    if np.abs(x[0]).max() > 1e-3 or np.abs(x[-1]).max() > 1e-3:
        errs.append(f"{name}: non-zero edge sample (click risk)")
    return errs


# ----------------------------------------------------------------------------------------- I/O
def write_wav(path: Path, x: np.ndarray) -> None:
    pcm = np.clip(np.round(x * 32767), -32768, 32767).astype("<i2")
    with wave.open(str(path), "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(pcm.tobytes())


def load_recipe(path: Path) -> dict:
    data = path.read_bytes()
    if len(data) > 256 * 1024:
        raise RecipeError("recipe larger than 256 KiB")
    return tomllib.loads(data.decode()) if path.suffix == ".toml" else json.loads(data)


def render_theme(path: Path, out: Path, do_check: bool) -> tuple[dict, list[str]]:
    recipe = load_recipe(path)
    theme = recipe.get("theme", {})
    events = recipe.get("events", {})
    pitch = Pitch(theme)
    name = theme.get("name", path.parent.name)
    out.mkdir(parents=True, exist_ok=True)
    manifest = {"theme": name, "description": theme.get("description", ""), "recipe_sha256": hashlib.sha256(path.read_bytes()).hexdigest(), "events": {}}
    errors: list[str] = []
    for ev_name in events:
        if not ev_name.replace("-", "").replace("_", "").isalnum():
            raise RecipeError(f"bad event name {ev_name!r}")
        ev = resolve_event(ev_name, events)
        long_ok = ev_name in ("boot", "shutdown", "login", "logout")
        variants = int(clamp(ev.get("variants", 1), 1, MAX_VARIANTS))
        seed = int.from_bytes(hashlib.sha256(f"{name}/{ev_name}".encode()).digest()[:4], "little")
        files = []
        for v in range(variants):
            x = render_event(ev, theme, pitch, seed + v, v, ev.get("jitter", {}))
            fn = f"{ev_name}.wav" if v == 0 else f"{ev_name}.{v + 1}.wav"
            write_wav(out / fn, x)
            info = {"file": fn, "ms": round(len(x) / SR * 1000), "peak_dbfs": round(20 * math.log10(np.abs(x).max()), 2), "lufs": round(loudness_lufs(x), 1)}
            files.append(info)
            if do_check:
                errors += check(x, f"{name}/{fn}", 0.04, 1.6 if long_ok else 0.6)
        manifest["events"][ev_name] = {"xdg": ev.get("xdg", []), "files": files}
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return manifest, errors


def write_index(out: Path, manifests: list[tuple[str, dict]]) -> None:
    """A static audition page: one row per event, one column per theme, relative <audio> paths."""
    from html import escape

    events: list[str] = []
    for _, m in manifests:
        events += [e for e in m["events"] if e not in events]
    head = "".join(f"<th>{escape(m['theme'])}<small>{escape(m['description'])}</small></th>" for _, m in manifests)
    rows = []
    for ev in events:
        cells = []
        for d, m in manifests:
            info = m["events"].get(ev)
            if not info:
                cells.append("<td class=missing>&mdash;</td>")
                continue
            f0 = info["files"][0]
            takes = "".join(f'<audio controls preload="none" src="{escape(d)}/{escape(f["file"])}"></audio>' for f in info["files"])
            label = f'{f0["ms"]} ms &middot; {f0["lufs"]} LUFS &middot; peak {f0["peak_dbfs"]} dBFS'
            if len(info["files"]) > 1:
                label += f' &middot; {len(info["files"])} takes'
            cells.append(f"<td>{takes}<small>{label}</small></td>")
        rows.append(f"<tr><th scope=row>{escape(ev)}</th>{''.join(cells)}</tr>")
    page = f"""<!doctype html>
<html lang=en><head><meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1">
<title>HypXR sound themes</title>
<style>
:root {{ color-scheme: light dark; --bg:#fafafa; --fg:#1d1d1f; --muted:#6b6b70; --line:#d9d9de; }}
@media (prefers-color-scheme: dark) {{ :root {{ --bg:#16161a; --fg:#ececf0; --muted:#9a9aa3; --line:#33333a; }} }}
body {{ background:var(--bg); color:var(--fg); font:15px/1.4 system-ui,sans-serif; margin:0; padding:16px; }}
h1 {{ font-size:1.3rem; margin:0 0 4px; }} p {{ color:var(--muted); margin:0 0 16px; max-width:70ch; }}
.wrap {{ overflow-x:auto; }} table {{ border-collapse:collapse; }}
th, td {{ border-bottom:1px solid var(--line); padding:8px 10px; text-align:left; vertical-align:top; }}
thead th {{ position:sticky; top:0; background:var(--bg); }}
th small, td small {{ display:block; color:var(--muted); font-weight:normal; font-size:.78rem; max-width:300px; }}
audio {{ display:block; height:32px; width:240px; margin-bottom:2px; }} .missing {{ color:var(--muted); }}
</style></head><body>
<h1>HypXR sound themes</h1>
<p>Rendered by <code>contrib/sounds/hypxr-sound-render.py</code> from one <code>recipe.toml</code> per theme
(docs/openxr/research/31-audio-design.md). 48 kHz / 16-bit stereo WAV. Events with several takes are cycled by the
player so repeats never sound identical. Start with your volume low.</p>
<div class=wrap><table><thead><tr><th>event</th>{head}</tr></thead><tbody>
{chr(10).join(rows)}
</tbody></table></div></body></html>
"""
    (out / "index.html").write_text(page)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("recipes", nargs="+", type=Path)
    ap.add_argument("-o", "--out", type=Path, required=True)
    ap.add_argument("--per-theme", action="store_true", help="write OUT/<theme>/ instead of OUT/")
    ap.add_argument("--check", action="store_true", help="validate duration/peak/DC/edges; exit 1 on failure")
    ap.add_argument("--index", action="store_true", help="with --per-theme: also write OUT/index.html to audition")
    args = ap.parse_args()
    if len(args.recipes) > 1 and not args.per_theme:
        ap.error("several recipes need --per-theme")
    all_errors = []
    manifests = []
    for r in args.recipes:
        try:
            out = args.out / r.parent.name if args.per_theme else args.out
            m, errs = render_theme(r, out, args.check)
        except (RecipeError, tomllib.TOMLDecodeError, json.JSONDecodeError, TypeError, ValueError, AttributeError, IndexError, KeyError) as e:
            print(f"{r}: error: {e}", file=sys.stderr)
            return 2
        all_errors += errs
        manifests.append((r.parent.name, m))
        for ev, info in m["events"].items():
            f = info["files"][0]
            more = f" (+{len(info['files']) - 1} variants)" if len(info["files"]) > 1 else ""
            print(f"{m['theme']:>8} {ev:<15} {f['ms']:>5} ms  peak {f['peak_dbfs']:>6} dBFS  {f['lufs']:>6} LUFS{more}")
    if args.index and args.per_theme:
        write_index(args.out, manifests)
    for e in all_errors:
        print("CHECK FAILED:", e, file=sys.stderr)
    return 1 if all_errors else 0


if __name__ == "__main__":
    sys.exit(main())
