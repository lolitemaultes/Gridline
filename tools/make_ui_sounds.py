#!/usr/bin/env python3
"""Generate Gridline's interface sounds with Gridline itself.

Every sound is a small Gridline project made only of built-in instruments and
effects (FM Synth, Keys, Pluck, Additive, Hall, Reverb, Bucket Chorus, Filter,
Limiter). This script writes the projects, renders them with `gridline render`
at 48 kHz, then finishes each file: trims the lead-in, removes DC, applies
smooth fades, sets the loudness to the event's target with one gain, and
writes a 24-bit WAV. Nothing is sampled, so the sounds are original.

Sonic identity (see the J-48 plan): key of D-flat major, motif "Lattice" =
D-flat, A-flat, E-flat rising (two stacked fifths). The startup states it in
full and blooms into D-flat add9; the other events quote fragments.

Usage:
  python3 tools/make_ui_sounds.py --gridline build-cmake/gridline \\
      [--src assets/sounds/src] [--out DIR] [--sets glass,felt] [--events save,open]
      [--report report.json] [--images DIR]

  --src     where the source projects are written (one folder per set)
  --out     where finished WAVs go: <out>/<set>/<event>.wav
  --images  also write waveform + spectrogram PNGs (needs ffmpeg)
  --report  JSON with the measurements of every file
  --install the classic theme (assets/sounds/classic/<event>.wav) from the options:
            a set name for every event ("glass"), optionally with per-event picks
            ("glass,open=felt,save=precision"). Renders first unless --no-render.

The app plays assets/sounds/<theme>/<event>.wav (engine/UiSounds.h). J-133 adds
tools/synth_ui_sounds.py, which synthesises the J-133 option families purely in
code (no build needed) and verifies them; this script stays for the J-48 sets.

No third-party Python packages; ffmpeg is used only for the true-peak check and
the optional images.
"""
import argparse
import json
import math
import os
import re
import struct
import subprocess
import sys

SR = 48000
PPQ = 960
TEMPO = 120.0  # 1 beat = 0.5 s, 1 tick = 0.52 ms

# ---------------------------------------------------------------- notes ----
N = {"Db3": 49, "Ab3": 56, "Db4": 61, "F4": 65, "G4": 67, "Ab4": 68, "Db5": 73, "Eb5": 75, "F5": 77,
     "Ab5": 80, "Db6": 85, "Eb6": 87, "F6": 89, "Ab6": 92}

EVENTS = ["startup", "open", "save", "export", "import", "task_done", "warning", "error", "record_start", "record_stop"]

# Loudness targets: max momentary loudness (K-weighted, 400 ms window), LUFS.
TARGET = {"startup": -20.0, "open": -24.0, "export": -24.0, "import": -24.0, "error": -24.0, "warning": -26.0,
          "task_done": -26.0, "record_start": -27.0, "record_stop": -27.0, "save": -28.0}

# Maximum lengths (s) per event; the file ends earlier when the sound has died away.
MAXLEN = {"startup": 2.8, "open": 0.8, "export": 0.9, "import": 0.8, "task_done": 0.6, "warning": 0.45,
          "error": 0.45, "save": 0.3, "record_start": 0.25, "record_stop": 0.25}


def score(event, k=1.0):
    """Motif fragments: [(time ms, note, velocity 0..1, duration ms)] for the lead voice.
    k stretches the timing (precision is tighter, air is slower)."""
    t = lambda ms: ms * k
    if event == "startup":
        return [(t(0), "Db5", 0.80, t(150)), (t(120), "Ab5", 0.76, t(150)), (t(240), "Eb6", 0.74, t(170)),
                (t(430), "Ab5", 0.42, t(900)), (t(430), "Eb6", 0.46, t(900)), (t(430), "F6", 0.58, t(1000))]
    if event == "open":
        return [(t(0), "Db5", 0.78, t(110)), (t(100), "Ab5", 0.80, t(160))]
    if event == "save":
        return [(t(0), "Eb6", 0.40, t(40)), (t(42), "Ab5", 0.72, t(90))]
    if event == "export":
        return [(t(0), "Db5", 0.74, t(90)), (t(75), "Ab5", 0.76, t(90)), (t(150), "Eb6", 0.82, t(220))]
    if event == "import":
        return [(t(0), "Ab5", 0.76, t(110)), (t(100), "Eb6", 0.80, t(170))]
    if event == "task_done":
        return [(t(0), "Ab5", 0.62, t(160)), (t(0), "Db6", 0.66, t(160))]
    if event == "warning":
        return [(t(0), "Ab5", 0.74, t(70)), (t(135), "Ab5", 0.64, t(90))]
    if event == "error":
        return [(t(0), "G4", 0.80, t(90)), (t(125), "Db4", 0.66, t(120))]
    if event == "record_start":
        return [(t(0), "Ab5", 0.80, t(70))]
    if event == "record_stop":
        return [(t(0), "Db5", 0.80, t(70))]
    raise ValueError(event)


# Startup bed (low pad under the bloom): D-flat 3, A-flat 3, D-flat 4.
def bed(k=1.0):
    return [(0, "Db3", 0.55, 1500 * k), (0, "Ab3", 0.45, 1500 * k), (0, "Db4", 0.35, 1500 * k)]


# ------------------------------------------------------------ catalogue ----
class Catalog:
    def __init__(self, gridline):
        out = subprocess.run([gridline, "plugin-params"], check=True, capture_output=True, text=True).stdout
        self.types = {p["type"]: p for p in json.loads(out)}

    def state(self, type_, **over):
        """Full parameter vector from the defaults; overrides by parameter name
        (first match; spaces / punctuation as underscores) or as p<index>."""
        info = self.types[type_]
        params = [p["def"] for p in info["params"]]
        names = {}
        for i, p in enumerate(info["params"]):
            key = re.sub(r"[^a-z0-9]+", "_", p["name"].lower()).strip("_")
            names.setdefault(key, i)
        for k, v in over.items():
            if re.fullmatch(r"p\d+", k):
                i = int(k[1:])
            elif k in names:
                i = names[k]
            else:
                raise KeyError(f"{type_}: no parameter '{k}' (have: {', '.join(sorted(names))})")
            p = info["params"][i]
            if "choices" in p and isinstance(v, str):
                v = p["choices"].index(v)
            if not (p["min"] - 1e-9 <= v <= p["max"] + 1e-9):
                raise ValueError(f"{type_}.{p['name']} = {v} outside [{p['min']}, {p['max']}]")
            params[i] = v
        return {"type": type_, "params": params}


# -------------------------------------------------------------- the sets ---
# A set = how to voice the score: lead instrument, optional layers, bus effects.
# Each function returns {"layers": [...], "fx": [...], "k": timing}.
# A layer = {"plugin": state, "notes": score, "gain": channel gain, "pan": pan}.

def set_glass(C, ev, v=0):
    """Warm glassy FM bell: two FM pairs (sine carrier + 2:1 modulator that fades
    fast = bright strike, mellow body; a quiet 3:1 partial for the glass), Hall.

    v = 0 original, 1 "soft" (subtle refinement), 2 "velvet" (clearly smoother
    and darker). The refinements (J-48, user request "less harsh, smoothed
    edges"): rounder onset (slower carrier and strike attacks), a smaller and
    shorter 2:1 strike, less and less-detuned 3rd partial (no beating shimmer),
    a lower high shelf plus a low-pass on the 3-8 kHz region, a little warmth
    around 300 Hz, a darker hall with less modulation."""
    dark = ev == "error"
    short = ev in ("save", "record_start", "record_stop", "warning", "error")
    long_ = ev == "startup"
    pick = lambda a, b, c: (a, b, c)[v]
    bell = C.state("gl.fm",
                   algorithm=4,  # 4->3 + 2->1: two independent pairs
                   op_1_ratio=1, op_1_level=1.0, op_1_attack=pick(0.004, 0.007, 0.011),
                   op_1_decay=(0.9 if long_ else 0.6) if not short else 0.3, op_1_sustain=0,
                   op_1_release=(0.9 if long_ else 0.55) if not short else pick(0.25, 0.28, 0.3), op_1_velocity=0.4,
                   op_2_ratio=2,
                   op_2_level=pick(0.14, 0.10, 0.07) if dark else pick(0.24, 0.17, 0.11),
                   op_2_attack=pick(0.004, 0.009, 0.014), op_2_decay=pick(0.16, 0.12, 0.09),
                   op_2_sustain=pick(0.08, 0.05, 0.03),
                   op_2_release=pick(0.3, 0.2, 0.15), op_2_velocity=pick(0.6, 0.45, 0.35),
                   op_3_ratio=3, op_3_fine=pick(6, 3, 1),
                   op_3_level=pick(0.04, 0.025, 0.012) if dark else pick(0.08, 0.05, 0.025),
                   op_3_attack=pick(0.005, 0.009, 0.014), op_3_decay=pick(0.25, 0.2, 0.16),
                   op_3_sustain=0, op_3_release=pick(0.25, 0.2, 0.16),
                   op_4_ratio=1, op_4_level=pick(0.10, 0.06, 0.0), op_4_attack=pick(0.002, 0.006, 0.01), op_4_decay=0.2,
                   op_4_sustain=0, op_4_release=0.2,
                   feedback=0, volume=0.5)
    layers = [{"plugin": bell, "notes": score(ev), "gain": 0.8}]
    if ev == "startup":
        pad = C.state("gl.fm", algorithm=4,
                      op_1_ratio=1, op_1_level=1, op_1_attack=0.35, op_1_decay=1.2, op_1_sustain=0.6, op_1_release=1.0,
                      op_2_ratio=1, op_2_level=pick(0.12, 0.09, 0.06), op_2_attack=0.4, op_2_decay=1.0, op_2_sustain=0.5,
                      op_2_release=1.0,
                      op_3_ratio=2, op_3_fine=pick(4, 3, 2), op_3_level=pick(0.18, 0.15, 0.12), op_3_attack=0.4, op_3_decay=1.0,
                      op_3_sustain=0.5, op_3_release=1.0,
                      op_4_ratio=1, op_4_level=0.0, feedback=0, volume=0.5)
        layers.append({"plugin": pad, "notes": bed(), "gain": pick(0.25, 0.26, 0.28)})
    hall_decay = 1.6 if ev == "startup" else 0.9
    fx = [C.state("gl.filter", mode="High pass", p1=170 if ev != "startup" else 60, p2=0.0)]
    if v:  # warmth: a broad lift in the low mids
        fx.append(C.state("gl.filter", mode="Peak", p1=pick(0, 320, 300), p2=0.1, gain=pick(0, 1.5, 2.5)))
    hall = dict(input_low_cut=280, input_high_cut=7500, pre_delay=14, size=0.42, decay=hall_decay,
                mod_depth=0.12, wet_level=-13 if ev != "startup" else -10, dry_level=0, width=0.85, brightness=0.7)
    if v:
        hall.update(input_high_cut=pick(0, 6000, 4500), mod_depth=pick(0, 0.07, 0.04), brightness=pick(0, 0.58, 0.45),
                    high_decay=pick(0, 0.45, 0.35), high_crossover=pick(0, 4000, 3200), mod_rate=pick(0, 0.5, 0.4))
    fx.append(C.state("gl.hall", **hall))
    fx.append(C.state("gl.filter", mode="High shelf", p1=pick(4500, 3800, 3000), gain=pick(-6.0, -8.0, -10.0)))
    if v:
        fx.append(C.state("gl.filter", mode="Low pass", p1=pick(0, 9500, 6500), p2=0.0))
    return {"layers": layers, "fx": fx, "k": 1.0}


def set_felt(C, ev):
    """Soft felt mallet: Keys (soft model, low hardness, muffled) doubled very
    quietly by a damped Pluck for the wooden onset; Bucket Chorus (no hiss) for
    a slight tape wobble; Plate reverb."""
    short = ev in ("save", "record_start", "record_stop", "warning", "error")
    dark = ev == "error"
    keys = C.state("gl.keys", model="Soft", hardness=0.22 if not dark else 0.1, muffle=3200 if not dark else 1800,
                   tremolo_depth=0, detune=3, overdrive=0, decay=1.4 if not short else 0.6, release=0.35 if not short else 0.15,
                   stereo=0.0, velocity_sens=0.6, volume=0.7)
    pluck = C.state("gl.pluck", decay=0.5 if not short else 0.25, damping=0.8, brightness=0.2, pick_position=0.22,
                    stereo_width=0.0, release=0.12, volume=0.6)
    notes = score(ev, 1.05)
    layers = [{"plugin": keys, "notes": notes, "gain": 0.85},
              {"plugin": pluck, "notes": notes, "gain": 0.28}]
    if ev == "startup":
        pad = C.state("gl.keys", model="Soft", hardness=0.0, muffle=900, tremolo_depth=0.0, detune=6, overdrive=0,
                      decay=4.0, release=1.2, stereo=0.1, volume=0.7)
        layers.append({"plugin": pad, "notes": bed(), "gain": 0.22})
    fx = [C.state("gl.filter", mode="High pass", p1=160 if ev != "startup" else 55, p2=0.0),
          C.state("gl.bbdchorus", hiss=0, mix=0.16, speed=0.45, l_r_phase=0.15, noise_gate=1),
          C.state("gl.reverb", model="Plate", room_size=0.35, decay_time=1.1 if ev == "startup" else 0.7, wet=0.18 if ev != "startup" else 0.24,
                  dry=1.0, low_cut=300, high_cut=6000, high_damping=5000, width=0.35, pre_delay=0),
          C.state("gl.filter", mode="Low pass", p1=6500, p2=0.0),
          C.state("gl.stereo", separation=0.5)]  # keep the chorus / pluck image narrow (mono-safe)
    return {"layers": layers, "fx": fx, "k": 1.05}


def set_air(C, ev):
    """Airy: soft additive tone (rounded spectrum, 3-voice unison, gentle 18 ms
    swell), Hall with an octave shimmer; the startup bed swells in slowly."""
    short = ev in ("save", "record_start", "record_stop", "warning", "error")
    dark = ev == "error"
    tone = C.state("gl.additive", timbre_a="Soft", timbre_b="Sine", morph=0.55 if not dark else 0.85, partials=24,
                   filter="Low pass", cutoff=2600 if not dark else 1400, key_track=0.4,
                   unison=3, detune=7, spread=0.35,
                   attack=0.018 if not short else 0.008, decay=0.7 if not short else 0.25, sustain=0.0,
                   release=0.6 if not short else 0.18, volume=0.8)
    layers = [{"plugin": tone, "notes": score(ev, 1.15), "gain": 0.8}]
    if ev == "startup":
        pad = C.state("gl.additive", timbre_a="Soft", timbre_b="Sine", morph=0.7, partials=16, filter="Low pass", cutoff=1200,
                      unison=3, detune=9, spread=0.4, attack=0.6, decay=2.0, sustain=0.6, release=1.2, volume=0.8)
        layers.append({"plugin": pad, "notes": bed(1.1), "gain": 0.28})
    fx = [C.state("gl.filter", mode="High pass", p1=180 if ev != "startup" else 55, p2=0.0),
          C.state("gl.hall", input_low_cut=350, input_high_cut=9000, pre_delay=20, size=0.6,
                  decay=2.2 if ev == "startup" else 1.1, mod_depth=0.25, width=0.8, brightness=0.8,
                  shimmer_on=0 if (dark or short) else 1, shimmer_pitch=12, shimmer_gain=0.3, shimmer_low_cut=600,
                  shimmer_high_cut=8000,
                  wet_level=-9 if ev == "startup" else -12, dry_level=0)]
    return {"layers": layers, "fx": fx, "k": 1.15}


def set_precision(C, ev):
    """Minimal precision: near-pure sine (FM carrier with a 4:1 modulator that
    dies in ~10 ms = a soft tick inside the tone), very dry small room, tight timing."""
    dark = ev == "error"
    tone = C.state("gl.fm", algorithm=4,
                   op_1_ratio=1, op_1_level=1, op_1_attack=0.003, op_1_decay=0.22 if ev != "startup" else 0.5, op_1_sustain=0,
                   op_1_release=0.12 if ev != "startup" else 0.35, op_1_velocity=0.3,
                   op_2_ratio=4, op_2_level=0.07 if dark else 0.12, op_2_attack=0.003, op_2_decay=0.018, op_2_sustain=0,
                   op_2_release=0.01, op_2_velocity=0.5,
                   op_3_ratio=2, op_3_level=0.06, op_3_attack=0.003, op_3_decay=0.08, op_3_sustain=0, op_3_release=0.06,
                   op_4_ratio=1, op_4_level=0.0, feedback=0, volume=0.5)
    notes = score(ev, 0.8)
    layers = [{"plugin": tone, "notes": notes, "gain": 0.8}]
    if ev == "startup":
        pad = C.state("gl.fm", algorithm=7,  # 1+2+3+4: plain sines
                      op_1_ratio=1, op_1_level=0.8, op_1_attack=0.25, op_1_decay=1.2, op_1_sustain=0.4, op_1_release=0.6,
                      op_2_ratio=2, op_2_level=0.15, op_2_attack=0.25, op_2_decay=1.0, op_2_sustain=0.3, op_2_release=0.6,
                      op_3_ratio=1, op_3_level=0, op_4_ratio=1, op_4_level=0, feedback=0, volume=0.5)
        layers.append({"plugin": pad, "notes": bed(0.8), "gain": 0.22})
    fx = [C.state("gl.filter", mode="High pass", p1=200 if ev != "startup" else 60, p2=0.0),
          C.state("gl.reverb", model="Hall", room_size=0.18, decay_time=0.35 if ev != "startup" else 0.9, wet=0.10, dry=1.0,
                  low_cut=400, high_cut=7000, high_damping=6000, width=0.5, early_reflections=0.6, pre_delay=0)]
    return {"layers": layers, "fx": fx, "k": 0.8}


SETS = {
    "glass": (set_glass, "Glass", "Warm glassy FM bell in a soft hall: clear, polished, a little sparkle."),
    "glass-soft": (lambda C, ev: set_glass(C, ev, 1), "Glass soft",
                   "The Glass set with rounder onsets, a gentler strike and a darker top: subtle refinement."),
    "glass-velvet": (lambda C, ev: set_glass(C, ev, 2), "Glass velvet",
                     "The Glass set made clearly smoother and darker: slow round onsets, little strike, warm low mids."),
    "felt": (set_felt, "Felt", "Soft felt mallet with a wooden pluck, slight tape wobble and a plate: warm and intimate."),
    "air": (set_air, "Air", "Rounded additive tones with gentle swells and an octave shimmer: airy and spacious."),
    "precision": (set_precision, "Precision", "Near-pure sines with a tiny built-in tick, a small dry room, tight timing: minimal and exact."),
}


# ------------------------------------------------------------ projects -----
def ticks(ms):
    return int(round(ms / 1000.0 * TEMPO / 60.0 * PPQ))


def make_project(title, spec):
    master, bus = 2, 3
    empty = [{"enabled": True, "mix": 1} for _ in range(10)]
    slots = [{"enabled": True, "mix": 1, "plugin": fx} for fx in spec["fx"]] + empty[len(spec["fx"]):]
    limiter_slot = [{"enabled": True, "mix": 1, "plugin": spec["limiter"]}] + empty[1:]
    mixer = [
        {"id": master, "name": "Master", "volume": 1.0, "pan": 0, "muted": False, "solo": False, "sends": [], "slots": limiter_slot,
         "color": 5264990, "invertPolarity": False, "stereoSep": 0, "swapLR": False},
        {"id": bus, "name": "UI bus", "volume": 1.0, "pan": 0, "muted": False, "solo": False, "sends": [{"to": master, "level": 1}],
         "slots": slots, "color": 5224624, "invertPolarity": False, "stereoSep": 0, "swapLR": False},
    ]
    channels, notes = [], {}
    for i, layer in enumerate(spec["layers"]):
        cid = 20 + i
        channels.append({"id": cid, "name": f"Layer {i + 1}", "plugin": layer["plugin"], "mixerTrack": bus,
                         "gain": layer.get("gain", 0.8), "pan": layer.get("pan", 0), "pitch": 0, "muted": False, "color": 14186330})
        notes[str(cid)] = [{"start": ticks(t), "length": max(1, ticks(d)), "pitch": N[n], "velocity": int(round(v * 127))}
                           for (t, n, v, d) in layer["notes"]]
    return {
        "format": "gridline-project", "version": 4, "info": {"title": title, "author": "Gridline", "comments": "J-48 interface sound",
                                                              "genre": "", "url": ""},
        "tempo": TEMPO, "ppq": PPQ, "timeSigNum": 4, "timeSigDen": 4, "swing": 0, "masterPitch": 0, "links": [],
        "channels": channels, "mixer": mixer,
        "patterns": [{"id": 40, "name": "Sound", "color": 13214780, "stepsPerBar": 16, "bars": 1, "steps": {}, "notes": notes}],
        "playlist": [{"id": 60, "pattern": 40, "track": 0, "start": 0, "length": 0}],
        "tracks": [{"name": "Sound", "color": 14186330, "muted": False}],
        "nextId": 100,
    }


def dump_project(proj):
    """Readable JSON with each parameter vector on one line (smaller, diffable)."""
    text = json.dumps(proj, indent=1)
    def one_line(m):
        nums = re.findall(r"-?[\d.]+(?:e-?\d+)?", m.group(1))
        return '"params": [' + ", ".join(nums) + "]"
    return re.sub(r'"params": \[([^\]]*)\]', one_line, text) + "\n"


# ---------------------------------------------------------------- audio ----
def read_wav(path):
    with open(path, "rb") as f:
        data = f.read()
    assert data[:4] == b"RIFF" and data[8:12] == b"WAVE", path
    pos, fmt, raw = 12, None, None
    while pos + 8 <= len(data):
        cid, size = data[pos:pos + 4], struct.unpack("<I", data[pos + 4:pos + 8])[0]
        body = data[pos + 8:pos + 8 + size]
        if cid == b"fmt ":
            fmt = struct.unpack("<HHIIHH", body[:16])
        elif cid == b"data":
            raw = body
        pos += 8 + size + (size & 1)
    tag, ch, sr, _, align, bits = fmt
    if tag == 0xFFFE:
        tag = struct.unpack("<H", body[24:26])[0] if False else (3 if bits == 32 else 1)
    n = len(raw) // align
    if tag == 3 and bits == 32:
        flat = struct.unpack(f"<{n * ch}f", raw[:n * align])
    elif bits == 24:
        flat = [int.from_bytes(raw[i:i + 3], "little", signed=True) / 8388608.0 for i in range(0, n * align, 3)]
    elif bits == 16:
        flat = [v / 32768.0 for v in struct.unpack(f"<{n * ch}h", raw[:n * align])]
    else:
        raise ValueError(f"unsupported wav {fmt}")
    chans = [list(flat[c::ch]) for c in range(ch)]
    if ch == 1:
        chans.append(list(chans[0]))
    return sr, chans[:2]


def write_wav24(path, chans, sr=SR):
    n = len(chans[0])
    out = bytearray()
    for i in range(n):
        for c in range(2):
            v = max(-1.0, min(1.0, chans[c][i]))
            out += int(round(v * 8388607.0)).to_bytes(3, "little", signed=True)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(out)) + b"WAVE")
        f.write(b"fmt " + struct.pack("<IHHIIHH", 16, 1, 2, sr, sr * 6, 6, 24))
        f.write(b"data" + struct.pack("<I", len(out)) + bytes(out))


def biquad(x, b0, b1, b2, a1, a2):
    y, x1, x2, y1, y2 = [0.0] * len(x), 0.0, 0.0, 0.0, 0.0
    for i, v in enumerate(x):
        o = b0 * v + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2
        x2, x1, y2, y1 = x1, v, y1, o
        y[i] = o
    return y


def _highpass(f0, sr, q=0.7071):
    w0 = 2 * math.pi * f0 / sr
    al = math.sin(w0) / (2 * q)
    a0 = 1 + al
    return ((1 + math.cos(w0)) / 2 / a0, -(1 + math.cos(w0)) / a0, (1 + math.cos(w0)) / 2 / a0,
            -2 * math.cos(w0) / a0, (1 - al) / a0)


def k_weight(x):
    # ITU-R BS.1770 K-weighting coefficients at 48 kHz.
    x = biquad(x, 1.53512485958697, -2.69169618940638, 1.19839281085285, -1.69065929318241, 0.73248077421585)
    return biquad(x, 1.0, -2.0, 1.0, -1.99004745483398, 0.99007225036621)


def max_momentary(chans, sr=SR):
    """Max momentary loudness (400 ms window, 10 ms hop) in LUFS; shorter files
    are measured as if padded with silence to 400 ms."""
    win = int(0.4 * sr)
    kw = [k_weight(c + [0.0] * max(0, win - len(c))) for c in chans]
    sq = [a * a + b * b for a, b in zip(kw[0], kw[1])]
    acc = [0.0]
    for v in sq:
        acc.append(acc[-1] + v)
    best = 0.0
    for s in range(0, len(sq) - win + 1, int(0.01 * sr)):
        best = max(best, (acc[s + win] - acc[s]) / win)
    return -0.691 + 10 * math.log10(best) if best > 0 else -120.0


def finish(chans, event, maxlen):
    """Trim, DC, fades. Returns the finished stereo float lists (before gain)."""
    n = len(chans[0])
    peak = max(max(abs(v) for v in c) for c in chans) or 1.0
    onset = next(i for i in range(n) if abs(chans[0][i]) > peak * 1e-3 or abs(chans[1][i]) > peak * 1e-3)
    start = max(0, onset - int(0.001 * SR))
    # end: last point the 5 ms envelope is above -60 dB of the peak, capped at maxlen
    thr = peak * 10 ** (-60 / 20)
    last = start
    for i in range(n - 1, start, -1):
        if abs(chans[0][i]) > thr or abs(chans[1][i]) > thr:
            last = i
            break
    end = min(last + int(0.005 * SR), start + int(maxlen * SR), n)
    out = [c[start:end] for c in chans]
    m = len(out[0])
    for c in out:  # DC
        mean = sum(c) / m
        for i in range(m):
            c[i] -= mean
    fin = int(0.003 * SR)  # raised-cosine fade-in
    for i in range(min(fin, m)):
        g = 0.5 - 0.5 * math.cos(math.pi * i / fin)
        out[0][i] *= g
        out[1][i] *= g
    fade = max(int(0.03 * SR), int(m * (0.35 if event != "startup" else 0.45)))
    for i in range(fade):  # raised-cosine fade-out ending exactly at 0
        g = 0.5 + 0.5 * math.cos(math.pi * (i + 1) / fade)
        out[0][m - fade + i] *= g
        out[1][m - fade + i] *= g
    return out


def analyse(chans, sr=SR):
    m = len(chans[0])
    peak = max(max(abs(v) for v in c) for c in chans)
    L, R = chans
    sll = sum(v * v for v in L)
    srr = sum(v * v for v in R)
    slr = sum(a * b for a, b in zip(L, R))
    corr = slr / math.sqrt(sll * srr) if sll > 0 and srr > 0 else 1.0
    side = sum(((a - b) / 2) ** 2 for a, b in zip(L, R))
    mid = sum(((a + b) / 2) ** 2 for a, b in zip(L, R))
    # click score: energy above 5 kHz (2nd-order high-pass) in the loudest 5 ms
    # window, relative to the loudest 5 ms window of the full signal (dB). Smooth
    # onsets stay well under -30 dB; a click or a harsh strike shows up above it.
    w = sr // 200
    rbj = _highpass(5000.0, sr)
    hf = [biquad(c, *rbj) for c in chans]
    def wmax(sig):
        best = 0.0
        for s0 in range(0, m - w, w // 2):
            best = max(best, sum(sig[0][i] ** 2 + sig[1][i] ** 2 for i in range(s0, s0 + w)))
        return best
    full, hfe = wmax(chans), wmax(hf)
    worst = 10 * math.log10(hfe / full) if hfe > 0 and full > 0 else -120.0
    # attack: 10 % to 90 % rise of the first onset (0.5 ms RMS envelope, first 40 ms)
    hw = sr // 2000
    env = [math.sqrt(sum(chans[0][i] ** 2 + chans[1][i] ** 2 for i in range(k, min(m, k + hw))) / (2 * hw))
           for k in range(0, min(m, sr * 40 // 1000), hw)]
    top = max(env) or 1.0
    i10 = next(i for i, e in enumerate(env) if e >= 0.1 * top)
    i90 = next(i for i, e in enumerate(env) if e >= 0.9 * top)
    rise_ms = (i90 - i10) * 0.5
    dc = max(abs(sum(c) / m) for c in chans)
    return {"length_s": round(m / sr, 3), "peak_dbfs": round(20 * math.log10(peak), 2) if peak > 0 else -120,
            "lufs_m_max": round(max_momentary(chans, sr), 2), "corr": round(corr, 3),
            "side_db": round(10 * math.log10(side / mid), 1) if side > 0 and mid > 0 else -120.0,
            "hf5k_db": round(worst, 1), "rise_ms": rise_ms, "dc": dc, "edge_start": abs(L[0]) + abs(R[0]), "edge_end": abs(L[-1]) + abs(R[-1])}


def true_peak(path):
    r = subprocess.run(["ffmpeg", "-hide_banner", "-nostats", "-i", path, "-af", "ebur128=peak=true", "-f", "null", "-"],
                       capture_output=True, text=True)
    m = re.findall(r"Peak:\s+(-?[\d.]+|-inf) dBFS", r.stderr)
    return float(m[-1]) if m and m[-1] != "-inf" else None


def spectral(path, chans):
    """Energy-weighted spectral centroid and 85 % roll-off (Hz) of channel 1,
    from ffmpeg's aspectralstats (2048-point frames, hop 1024)."""
    r = subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-i", path, "-af",
                        "aspectralstats=win_size=2048:measure=centroid+rolloff,ametadata=print:file=-", "-f", "null", "-"],
                       capture_output=True, text=True)
    cen = [float(v) for v in re.findall(r"aspectralstats\.1\.centroid=([\d.]+)", r.stdout)]
    rol = [float(v) for v in re.findall(r"aspectralstats\.1\.rolloff=([\d.]+)", r.stdout)]
    x = chans[0]
    w = [sum(v * v for v in x[k * 1024:k * 1024 + 2048]) for k in range(len(cen))]
    tw = sum(w) or 1.0
    return (round(sum(a * b for a, b in zip(cen, w)) / tw), round(sum(a * b for a, b in zip(rol, w)) / tw))


def images(path, png_dir, name):
    os.makedirs(png_dir, exist_ok=True)
    subprocess.run(["ffmpeg", "-y", "-hide_banner", "-loglevel", "error", "-i", path, "-lavfi",
                    "showspectrumpic=s=800x300:legend=1:scale=log:fscale=log:start=60:stop=16000", os.path.join(png_dir, name + "_spec.png")], check=True)
    subprocess.run(["ffmpeg", "-y", "-hide_banner", "-loglevel", "error", "-i", path, "-lavfi",
                    "showwavespic=s=800x160:split_channels=1:scale=sqrt:colors=0x4f8cff|0xff8c4f", os.path.join(png_dir, name + "_wave.png")], check=True)


def waveform_svg_points(chans, cols=160):
    """Peak envelope (0..1, mono max) per column for the review page."""
    m = len(chans[0])
    peak = max(max(abs(v) for v in c) for c in chans) or 1.0
    pts = []
    for k in range(cols):
        a, b = k * m // cols, max(k * m // cols + 1, (k + 1) * m // cols)
        pts.append(round(max(max(abs(chans[0][i]), abs(chans[1][i])) for i in range(a, b)) / peak, 3))
    return pts


# ----------------------------------------------------------------- main ----
def main():
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--gridline", default=os.path.join(here, "build-cmake", "gridline"))
    ap.add_argument("--src", default=os.path.join(here, "assets", "sounds", "src"))
    ap.add_argument("--out", required=True)
    ap.add_argument("--sets", default=",".join(SETS))
    ap.add_argument("--events", default=",".join(EVENTS))
    ap.add_argument("--report")
    ap.add_argument("--images")
    ap.add_argument("--keep-raw", action="store_true", help="keep the raw renders next to the outputs")
    ap.add_argument("--install", help='set for all events plus optional event=set picks, e.g. "glass,save=felt"')
    ap.add_argument("--no-render", action="store_true", help="only --install from an existing --out folder")
    a = ap.parse_args()
    if not a.no_render:
        render_all(a)
    if a.install:
        base, picks = None, {}
        for item in a.install.split(","):
            if "=" in item:
                ev, st = item.split("=", 1)
                picks[ev] = st
            else:
                base = item
        dst_dir = os.path.join(here, "assets", "sounds", "classic")  # J-133: the classic theme
        for ev in EVENTS:
            st = picks.get(ev, base)
            if not st:
                continue
            src = os.path.join(a.out, st, f"{ev}.wav")
            with open(src, "rb") as fi, open(os.path.join(dst_dir, f"{ev}.wav"), "wb") as fo:
                fo.write(fi.read())
            print(f"installed {st}/{ev}.wav")


def render_all(a):
    C = Catalog(a.gridline)
    limiter = C.state("gl.limiter", ceiling=-3.0)
    report = {}
    for sname in a.sets.split(","):
        fn, label, desc = SETS[sname]
        for ev in a.events.split(","):
            spec = fn(C, ev)
            spec["limiter"] = limiter
            src = os.path.join(a.src, sname, f"{ev}.gridline.json")
            raw = os.path.join(a.out, sname, f"{ev}.raw.wav")
            os.makedirs(os.path.dirname(src), exist_ok=True)
            os.makedirs(os.path.dirname(raw), exist_ok=True)
            # Render; if the mix runs hot, scale the channel gains in the project
            # (Gridline sets the level) and render again, for a peak near -10 dBFS.
            for attempt in range(4):
                proj = make_project(f"Gridline sound - {label} - {ev}", spec)
                with open(src, "w") as f:
                    f.write(dump_project(proj))
                r = subprocess.run([a.gridline, "render", src, "-o", raw, "--sr", str(SR), "--bits", "24", "--dither", "none",
                                    "--tail", "leave"], capture_output=True, text=True)
                if r.returncode != 0:
                    sys.exit(f"render failed for {src}:\n{r.stdout}\n{r.stderr}")
                m = re.search(r"peak ([\d.]+)", r.stdout)
                render_peak = float(m.group(1)) if m else 1.0
                if 0.15 <= render_peak <= 0.6 or attempt == 3:
                    break
                f_ = math.sqrt(0.3 / max(render_peak, 1e-6))  # channel volume is a squared law
                for layer in spec["layers"]:
                    layer["gain"] = min(1.25, layer.get("gain", 0.8) * f_)
            sr, chans = read_wav(raw)
            assert sr == SR, sr
            raw_peak = max(max(abs(v) for v in c) for c in chans)
            out = finish(chans, ev, MAXLEN[ev])
            gain_db = TARGET[ev] - max_momentary(out)
            g = 10 ** (gain_db / 20)
            out = [[v * g for v in c] for c in out]
            peak = max(max(abs(v) for v in c) for c in out)
            if peak > 10 ** (-1.5 / 20):  # keep 1.5 dB under full scale (true peak <= -1 dBTP)
                h = 10 ** (-1.5 / 20) / peak
                out = [[v * h for v in c] for c in out]
            dst = os.path.join(a.out, sname, f"{ev}.wav")
            write_wav24(dst, out)
            if not a.keep_raw:
                os.remove(raw)
            info = analyse(out)
            info["true_peak_dbtp"] = true_peak(dst)
            info["centroid_hz"], info["rolloff_hz"] = spectral(dst, out)
            info["raw_peak"] = round(raw_peak, 4)
            info["gain_db"] = round(gain_db, 2)
            info["target_lufs"] = TARGET[ev]
            info["wave"] = waveform_svg_points(out)
            report.setdefault(sname, {})[ev] = info
            if a.images:
                images(dst, os.path.join(a.images, sname), ev)
            print(f"{sname:10s} {ev:13s} {info['length_s']:5.2f}s  M {info['lufs_m_max']:6.1f}  pk {info['peak_dbfs']:6.1f}  "
                  f"tp {info['true_peak_dbtp']}  cen {info['centroid_hz']:5d} ro {info['rolloff_hz']:5d}  corr {info['corr']:5.2f}  side {info['side_db']:6.1f}  hf5k {info['hf5k_db']:6.1f}  rise {info['rise_ms']:4.1f}ms  "
                  f"raw {raw_peak:.3f} gain {gain_db:+.1f}")
    if a.report:
        meta = {k: {"label": v[1], "desc": v[2]} for k, v in SETS.items()}
        with open(a.report, "w") as f:
            json.dump({"sets": meta, "events": EVENTS, "targets": TARGET, "files": report}, f, indent=1)


if __name__ == "__main__":
    main()
