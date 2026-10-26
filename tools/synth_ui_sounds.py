#!/usr/bin/env python3
"""Synthesise Gridline's interface sounds from code (J-133). numpy only.

Every sound is built here from first principles: modal synthesis (sums of
decaying partials for struck glass, wood and metal tines), two-operator FM,
a tine-and-pickup model, shaped noise for contact and air, and a reverb whose
impulse response is itself synthesised (decaying, band-damped noise with
early reflections). Nothing is sampled or copied, so every file is original
and the same command always writes the same bytes.

Sonic identity (kept from J-48, which the user chose): key of D-flat major,
motif "Lattice" = D-flat, A-flat, E-flat rising in stacked fifths (the even
spacing of grid lines). Note onsets sit on a fixed grid step per family.
Each event quotes a fragment whose shape carries the meaning:

  startup       the full motif blooming into D-flat add9 over a low bed
  open          rising fifth D-flat to A-flat           (opens up: I to V)
  import        falling fifth A-flat to D-flat          (lands home: V to I)
  export        the full rising motif                    (sent out, complete)
  task_done     rising fourth A-flat to D-flat          (resolves home)
  save          one soft dyad, A-flat under D-flat      (quietly locked in)
  warning       A-flat twice                             (attention, open-ended)
  error         falling tritone G to D-flat              (outside the key: wrong)
  record_start  quick flam up, A-flat to E-flat
  record_stop   quick flam down, E-flat to A-flat

Error and the record pair are the only sounds the app plays while music
runs, so they sit in the upper register (where music is thinner) in every
family, and error adds a soft body an octave below for weight.

Mastering (every file): high-pass, gentle low-pass, DC removal, natural
decay with a raised-cosine end fade, one gain to the event's loudness target
(max momentary LUFS), a 4x oversampled look-ahead true-peak limiter that only
acts above the ceiling, 48 kHz / 24-bit stereo. Every file is then measured
(tools/uisound_measure.py) and checked against the targets below.

Usage:
  python3 tools/synth_ui_sounds.py --out DIR [--families prism,graphite] [--events save,open]
                                   [--report report.json] [--table table.md]
  python3 tools/synth_ui_sounds.py --install all          # every family -> assets/sounds/<family>/
  python3 tools/synth_ui_sounds.py --install graphite     # one family
  python3 tools/synth_ui_sounds.py --level assets/sounds/classic   # match existing files to the targets
  python3 tools/synth_ui_sounds.py --check assets/sounds/graphite  # verify existing files only

The app plays assets/sounds/<theme>/<event>.wav for the theme chosen in
Settings (setting "sound.theme", engine/UiSounds.h). The "classic" theme is the
J-48 Glass Soft set (tools/make_ui_sounds.py), brought to the same loudness
targets with --level (one gain per file, nothing else changes).
"""
import argparse
import json
import math
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import uisound_measure as M  # noqa: E402

SR = 48000
EVENTS = ["startup", "open", "save", "export", "import", "task_done", "warning", "error", "record_start", "record_stop"]

# ----------------------------------------------------------------- targets ---
# hit:  loudness of the loudest 100 ms (K-weighted, LUFS scale) of the file.
#       Hearing integrates over roughly 100 to 200 ms, so for sounds this short
#       the loudest 100 ms is what sets how loud they feel; the 400 ms momentary
#       window would average a 150 ms tap with silence and let short sounds
#       come out louder than intended. The app then applies its volume setting
#       (default 70 % = -3.1 dB). Music is -14 to -8 LUFS integrated.
# tp:   true-peak ceiling (dBTP). Sounds that can play over music (error,
#       record) keep extra headroom so they add little to the music's peaks.
# maxlen / minlen: seconds (tests allow 0.1 to 1.0, startup up to 3.0).
TARGET = {
    "startup":      dict(hit=-19.5, tp=-3.0, maxlen=2.70, minlen=1.6),
    "open":         dict(hit=-21.0, tp=-6.0, maxlen=0.85, minlen=0.4),
    "save":         dict(hit=-24.0, tp=-8.0, maxlen=0.32, minlen=0.12),
    "export":       dict(hit=-21.0, tp=-6.0, maxlen=0.95, minlen=0.5),
    "import":       dict(hit=-21.5, tp=-6.0, maxlen=0.80, minlen=0.4),
    "task_done":    dict(hit=-22.0, tp=-6.0, maxlen=0.70, minlen=0.35),
    "warning":      dict(hit=-22.0, tp=-6.0, maxlen=0.55, minlen=0.25),
    "error":        dict(hit=-20.0, tp=-6.0, maxlen=0.60, minlen=0.3),
    "record_start": dict(hit=-22.5, tp=-8.0, maxlen=0.30, minlen=0.12),
    "record_stop":  dict(hit=-22.5, tp=-8.0, maxlen=0.30, minlen=0.12),
}
# Checks applied to every file (see verify()).
LIMITS = dict(hit_tol=0.5, dc=-90.0, edge=-90.0, presence_pct=20.0, presence_peak=-6.0, sub_db=-40.0, hf8k=-20.0,
              corr=0.5, mono=-1.5, end_level=-40.0)

# ------------------------------------------------------------------ pitch ----
_NAMES = {"C": 0, "Db": 1, "D": 2, "Eb": 3, "E": 4, "F": 5, "Gb": 6, "G": 7, "Ab": 8, "A": 9, "Bb": 10, "B": 11}


def hz(note, shift=0):
    """'Db5' -> Hz (equal temperament, A4 = 440), shifted by `shift` semitones."""
    name, octave = note[:-1], int(note[-1])
    midi = 12 * (octave + 1) + _NAMES[name] + shift
    return 440.0 * 2 ** ((midi - 69) / 12.0)


def score(ev):
    """Note list [(onset in grid steps, note, velocity)] for the lead voice."""
    return {
        "startup": [(0, "Db5", 0.85), (1, "Ab5", 0.80), (2, "Eb6", 0.78)],
        "open": [(0, "Db5", 0.78), (1, "Ab5", 0.88)],
        "import": [(0, "Ab5", 0.80), (1, "Db5", 0.86)],
        "export": [(0, "Db5", 0.72), (1, "Ab5", 0.80), (2, "Eb6", 0.88)],
        "task_done": [(0, "Ab5", 0.74), (1, "Db6", 0.88)],
        "save": [(0, "Ab5", 0.62), (0.12, "Db6", 0.80)],
        "warning": [(0, "Ab5", 0.82), (1.5, "Ab5", 0.70)],
        "error": [(0, "G5", 0.86), (1.4, "Db5", 0.82)],
        "record_start": [(0, "Ab5", 0.50), (0.35, "Eb6", 0.88)],
        "record_stop": [(0, "Eb6", 0.50), (0.35, "Ab5", 0.88)],
    }[ev]


RAISED = ("error", "record_start", "record_stop")  # never shifted down: they must read over music
BLOOM = [("Ab5", 0.40), ("Db6", 0.34), ("Eb6", 0.44), ("F6", 0.52)]  # D-flat add9, on top of the motif
BED = [("Db3", 0.55), ("Ab3", 0.42), ("Db4", 0.30)]

# How long each event may ring (seconds, scales every decay in a family).
LIFE = {"startup": 1.0, "open": 0.62, "import": 0.58, "export": 0.66, "task_done": 0.52, "save": 0.22,
        "warning": 0.36, "error": 0.42, "record_start": 0.2, "record_stop": 0.2}


# -------------------------------------------------------------- primitives ---
def tvec(n):
    return np.arange(n) / SR


def attack_shape(n, attack):
    """Raised-cosine rise over `attack` seconds, then 1."""
    t = tvec(n)
    a = max(attack, 1.0 / SR)
    return np.where(t < a, 0.5 - 0.5 * np.cos(np.pi * np.minimum(t / a, 1.0)), 1.0)


def decay(n, t60, t60b=None, mix=0.0):
    """Exponential decay reaching -60 dB at t60; optional second, slower stage."""
    t = tvec(n)
    d = 10 ** (-3.0 * t / max(t60, 1e-4))
    if t60b:
        d = (1 - mix) * d + mix * 10 ** (-3.0 * t / t60b)
    return d


def pan2(sig, pan):
    """Constant-power pan, pan in -1..1 (mono-safe: no phase tricks)."""
    th = (pan + 1) * np.pi / 4
    return np.stack([sig * math.cos(th), sig * math.sin(th)], axis=1)


def place(buf, sig, t0):
    i = int(round(t0 * SR))
    if i >= len(buf):
        return
    m = min(len(sig), len(buf) - i)
    buf[i:i + m] += sig[:m]


def partial(n, f, amp, t60, attack, phase=0.0, t60b=None, mix=0.0):
    if f >= 0.45 * SR or amp == 0:
        return np.zeros(n)
    t = tvec(n)
    return amp * np.sin(2 * np.pi * f * t + phase) * attack_shape(n, attack) * decay(n, t60, t60b, mix)


def rng_for(*keys):
    """Deterministic noise per (family, event, voice): same bytes on every run."""
    h = 2166136261
    for ch in "/".join(str(k) for k in keys):
        h = ((h ^ ord(ch)) * 16777619) & 0xFFFFFFFF
    return np.random.default_rng(h)


def band_noise(n, lo, hi, rng, tilt=0.0):
    """White noise shaped in the frequency domain: smooth (raised-cosine in
    log-frequency) band from lo to hi Hz, optional tilt in dB/octave from lo."""
    x = rng.standard_normal(n + 2048)
    X = np.fft.rfft(x)
    f = np.fft.rfftfreq(len(x), 1 / SR)
    lf = np.log2(np.maximum(f, 1.0))
    a, b = math.log2(lo), math.log2(hi)
    edge = 0.5  # octaves of soft slope on each side
    g = np.clip((lf - (a - edge)) / edge, 0, 1) * np.clip(((b + edge) - lf) / edge, 0, 1)
    g = 0.5 - 0.5 * np.cos(np.pi * g)
    if tilt:
        g = g * 10 ** (tilt * np.maximum(lf - a, 0) / 20)
    y = np.fft.irfft(X * g, len(x))[1024:1024 + n]
    return y / (np.sqrt(np.mean(y * y)) + 1e-12)


def burst(n, lo, hi, attack, tau, rng, tilt=0.0):
    t = tvec(n)
    return band_noise(n, lo, hi, rng, tilt) * attack_shape(n, attack) * np.exp(-t / tau)


# ---------------------------------------------------------------- filters ---
def biquad(kind, f0, q=0.7071, gain_db=0.0):
    w0 = 2 * math.pi * f0 / SR
    al = math.sin(w0) / (2 * q)
    c = math.cos(w0)
    A = 10 ** (gain_db / 40)
    if kind == "hp":
        b, a = [(1 + c) / 2, -(1 + c), (1 + c) / 2], [1 + al, -2 * c, 1 - al]
    elif kind == "lp":
        b, a = [(1 - c) / 2, 1 - c, (1 - c) / 2], [1 + al, -2 * c, 1 - al]
    elif kind == "peak":
        b, a = [1 + al * A, -2 * c, 1 - al * A], [1 + al / A, -2 * c, 1 - al / A]
    elif kind == "hshelf":
        sq = 2 * math.sqrt(A) * al
        b = [A * ((A + 1) + (A - 1) * c + sq), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - sq)]
        a = [(A + 1) - (A - 1) * c + sq, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - sq]
    elif kind == "lshelf":
        sq = 2 * math.sqrt(A) * al
        b = [A * ((A + 1) - (A - 1) * c + sq), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - sq)]
        a = [(A + 1) + (A - 1) * c + sq, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - sq]
    else:
        raise ValueError(kind)
    return b, a


def filt(x, *stages):
    """Apply biquad stages (kind, f0, q, gain) to a stereo or mono signal."""
    y = np.array(x, np.float64)
    for st in stages:
        b, a = biquad(*st)
        if y.ndim == 1:
            y = M.lfilter(b, a, y)
        else:
            y = np.stack([M.lfilter(b, a, y[:, c]) for c in range(y.shape[1])], axis=1)
    return y


# ----------------------------------------------------------------- reverb ---
def make_ir(rt60, predelay=0.012, damp_hz=5000.0, low_cut=250.0, er=None, seed="ir", length=None, width=1.0):
    """Synthesised stereo room response. Late part: independent noise per side,
    decaying at rt60 below damp_hz and twice as fast above it (air and wall
    absorption), fading in over 25 ms after the pre-delay. Early part: a few
    discrete reflections (time s, gain, pan). Normalised to unit energy."""
    length = length or min(rt60 * 1.1 + predelay + 0.05, 3.5)
    n = int(length * SR)
    rng = rng_for(seed)
    t = tvec(n)
    ir = np.zeros((n, 2))
    late_on = predelay + 0.008
    env = np.clip((t - late_on) / 0.025, 0, 1)
    env = (0.5 - 0.5 * np.cos(np.pi * env))
    for c in range(2):
        noise = rng.standard_normal(n)
        X = np.fft.rfft(noise)
        f = np.fft.rfftfreq(n, 1 / SR)
        lowmask = 1 / (1 + (f / damp_hz) ** 4)
        lo = np.fft.irfft(X * lowmask, n)
        hi = np.fft.irfft(X * (1 - lowmask), n)
        tt = np.maximum(t - late_on, 0)
        late = lo * 10 ** (-3 * tt / rt60) + hi * 10 ** (-3 * tt / (rt60 * 0.45))
        ir[:, c] = late * env
    if width < 1.0:  # narrow the late field toward the middle
        mid = ir.mean(axis=1, keepdims=True)
        ir = mid + (ir - mid) * width
    for (dt, g, pan) in (er or []):
        i = int((predelay + dt) * SR)
        if i < n:
            th = (pan + 1) * np.pi / 4
            ir[i, 0] += g * math.cos(th) * 6
            ir[i, 1] += g * math.sin(th) * 6
    ir = filt(ir, ("hp", low_cut, 0.7071))
    return ir / math.sqrt(np.sum(ir * ir) / 2)


def convolve(x, ir):
    n = len(x) + len(ir) - 1
    nf = 1 << int(math.ceil(math.log2(n)))
    out = np.zeros((n, 2))
    for c in range(2):
        out[:, c] = np.fft.irfft(np.fft.rfft(x[:, c], nf) * np.fft.rfft(ir[:, c], nf), nf)[:n]
    return out


# ------------------------------------------------------------------ voices ---
def v_modal(n, f, vel, modes, hard=0.5, attack=0.0015, pan=0.0, spread=0.12, life=1.0):
    """Struck resonator: modes = [(ratio, amp, t60 in life units)]. Harder hits
    (velocity, `hard`) excite the upper modes more and shorten the attack."""
    out = np.zeros((n, 2))
    h = hard * (0.6 + 0.4 * vel)
    for k, (r, a, t60) in enumerate(modes):
        fk = f * r
        if fk > 16000:
            continue
        w = a * vel * (r ** (-(1.0 - h) * 0.9))
        att = attack * (1 + 0.6 * (1 - h)) / (1 + 0.15 * k)
        sig = partial(n, fk, w, max(t60 * life, 0.02), att, phase=0.0)
        p = max(-1.0, min(1.0, pan + spread * ((-1) ** k) * min(1.0, 0.35 * k)))
        out += pan2(sig, p)
    return out


def v_fm(n, f, vel, ratio=1.0, index=1.2, index_tau=0.02, glide=25.0, glide_tau=0.012, t60=0.5, attack=0.002,
         pan=0.0, fb_partial=0.0, detune=0.0):
    """Two-operator FM: the modulator index falls quickly, so the onset is bright
    and the tone settles to a near-pure sine; a small downward pitch settle
    (cents) gives a precise 'snap'."""
    t = tvec(n)
    cents = glide * np.exp(-t / glide_tau)
    fr = f * 2 ** ((cents + detune) / 1200)
    ph = 2 * np.pi * np.cumsum(fr) / SR
    idx = index * (0.5 + 0.5 * vel) * np.exp(-t / index_tau)
    s = np.sin(ph + idx * np.sin(ph * ratio))
    if fb_partial:
        s = s + fb_partial * np.sin(2 * ph) * np.exp(-t / (index_tau * 3))
    s *= vel * attack_shape(n, attack) * decay(n, t60)
    return pan2(s, pan)


def v_tine(n, f, vel, life=1.0, bark=0.55, pan=0.0):
    """Electric-piano style tine: a stiff rod (strong fundamental, inharmonic
    upper modes that die in a few ms) read by a magnetic pickup that sits a
    little off-centre. The pickup's curve adds the even 'bark' harmonics while
    the tine swings wide and fades them as it settles."""
    d = (partial(n, f, 1.0, 1.6 * life, 0.0012, t60b=4.0 * life, mix=0.25)
         + partial(n, f * 5.93, 0.10 * vel, 0.06, 0.0006)
         + partial(n, f * 13.1, 0.035 * vel, 0.025, 0.0004))
    g = 0.8 + 2.4 * bark * vel
    bias = 0.45
    y = np.tanh(g * (d + bias)) - math.tanh(g * bias)
    y /= g * (1 - math.tanh(g * bias) ** 2)  # unit small-signal gain
    y *= vel
    return pan2(y, pan)


def v_pad(n, f, amp, attack, t60, detune_c=4.0, harmonics=((1, 1.0), (2, 0.18), (3, 0.05)), pan_w=0.3, seed=0):
    """Soft sustained tone for beds and blooms: a few harmonics, three voices
    detuned by +-detune_c cents and spread left / centre / right."""
    out = np.zeros((n, 2))
    env = attack_shape(n, attack) * decay(n, t60)
    for j, dc in enumerate((-detune_c, 0.0, detune_c)):
        fr = f * 2 ** (dc / 1200)
        s = np.zeros(n)
        for (h, a) in harmonics:
            if fr * h < 15000:
                s += a * np.sin(2 * np.pi * fr * h * tvec(n) + 1.3 * j + 0.7 * h + seed)
        out += pan2(s * env * (0.8 if j != 1 else 1.0), (j - 1) * pan_w)
    return out / 2.6


# ---------------------------------------------------------------- families --
# A family = how to voice the shared scores. Each returns (dry stereo, wet
# send stereo, reverb spec, mastering spec).

def pitch_pan(note, shift=0):
    """Higher notes sit slightly right, lower slightly left (+-0.18)."""
    f = hz(note, shift)
    return max(-0.18, min(0.18, 0.18 * math.log2(f / hz("Ab5")) / 1.0))


class Family:
    key = ""
    label = ""
    desc = ""
    step = 0.1      # grid step (s)
    shift = 0       # semitones applied to the scores
    length_pad = 0.0

    def voice(self, ev, n, note, vel, shift):
        raise NotImplementedError

    def reverb(self, ev):
        return dict(rt60=0.9, predelay=0.012, damp_hz=5000, wet_db=-16)

    def master(self, ev):
        return dict(hp=150.0, lp=15000.0)

    def extra(self, ev, n):
        """Additional layers (startup bed, air...) as stereo array or None."""
        return None

    def render(self, ev):
        life = LIFE[ev]
        n = int((TARGET[ev]["maxlen"] + 0.6) * SR)
        dry = np.zeros((n, 2))
        notes = score(ev)
        shift = self.shift if ev not in RAISED else 0
        for (step, note, vel) in notes:
            v = self.voice(ev, n, note, vel, shift)
            place(dry, v, step * self.step)
        if ev == "startup":
            t_bloom = 3 * self.step + 0.02
            for (k, (note, vel)) in enumerate(BLOOM):
                v = self.bloom_voice(n, note, vel, shift)
                place(dry, v, t_bloom + 0.012 * k)
        ex = self.extra(ev, n)
        if ex is not None:
            dry += ex
        return dry

    def bloom_voice(self, n, note, vel, shift):
        return self.voice("startup", n, note, vel, shift)


def error_body(fam, n, level=0.5):
    """The error notes again an octave below, softer: weight under the raised pair."""
    out = np.zeros((n, 2))
    for (step, note, vel) in score("error"):
        place(out, fam.voice("error", n, note, vel * level, -12), step * fam.step)
    return out


class Prism(Family):
    key, label = "prism", "Prism"
    desc = ("Struck crystal: a clear, near-harmonic glass partial set with a soft mallet and a short inharmonic "
            "sparkle, in a small bright hall. The refined successor of Glass Soft.")
    step = 0.095
    # near-harmonic body (1, 2, 3) that rings, inharmonic sparkle (4.17, 5.43, 6.8) that is bright but gone
    # within about 60 ms: the 'ting' of glass without a lasting edge.
    MODES = [(1.0, 1.0, 1.0), (2.003, 0.40, 0.62), (3.008, 0.18, 0.40), (4.17, 0.16, 0.12), (5.43, 0.10, 0.07),
             (6.8, 0.06, 0.045)]
    # error: duller strike (fewer, softer upper modes) over a body an octave below
    DARK = [(1.0, 1.0, 1.0), (2.003, 0.30, 0.5), (3.008, 0.10, 0.3), (0.5, 0.45, 1.1)]

    def voice(self, ev, n, note, vel, shift):
        f = hz(note, shift)
        life = LIFE[ev] * (1.25 if ev == "startup" else 1.0)
        modes = self.DARK if ev == "error" else self.MODES
        hard = 0.45 if ev != "error" else 0.3
        out = v_modal(n, f, vel, modes, hard=hard, attack=0.0018, pan=pitch_pan(note, shift), spread=0.10, life=life)
        # mallet contact: a 3 ms breath of bright noise, 34 dB down
        out += pan2(burst(n, 2500, 9000, 0.0004, 0.0035, rng_for("prism", ev, note, vel)) * 0.02 * vel * vel, 0.0)
        return out

    def reverb(self, ev):
        big = ev == "startup"
        return dict(rt60=1.9 if big else 1.0, predelay=0.014, damp_hz=4200, wet_db=-13 if big else -17,
                    er=[(0.007, 0.25, -0.5), (0.011, 0.2, 0.55), (0.017, 0.14, -0.2)])

    def master(self, ev):
        return dict(hp=55.0 if ev == "startup" else 160.0, lp=14000.0, shelf=(6500, -2.5))

    def extra(self, ev, n):
        if ev != "startup":
            return None
        bed = np.zeros((n, 2))
        for (note, vel) in BED:
            place(bed, v_pad(n, hz(note), vel * 0.35, 0.32, 2.4, detune_c=3.0, harmonics=((1, 1.0), (2, 0.22), (3, 0.06))), 0.0)
        return bed


class Graphite(Family):
    key, label = "graphite", "Graphite"
    desc = ("Warm wood: a tuned bar struck with a felt mallet, an octave lower, with a resonator under the "
            "fundamental and a soft felt contact. Dry, close and tactile, like good studio hardware.")
    step = 0.1
    shift = -12
    # tuned bar: 1 : 3.99 : 9.2 (the bar is cut so the upper modes are close to harmonics)
    MODES = [(1.0, 1.0, 1.0), (3.99, 0.30, 0.30), (9.2, 0.10, 0.09), (2.0, 0.05, 0.35)]

    def voice(self, ev, n, note, vel, shift):
        f = hz(note, shift)
        life = LIFE[ev] * (1.3 if ev == "startup" else 1.0)
        out = v_modal(n, f, vel, self.MODES, hard=0.62, attack=0.0022, pan=pitch_pan(note), spread=0.08,
                      life=life * 0.9)
        # resonator tube: the fundamental again, slower attack, rings longer
        out += pan2(partial(n, f, 0.55 * vel, 1.4 * life, 0.006), pitch_pan(note))
        # felt contact: a short, low-mid thock with a faint woody click on top
        out += pan2(burst(n, 350, 2200, 0.0005, 0.004, rng_for("graphite", ev, note, vel), tilt=-3) * 0.05 * vel, 0.0)
        out += pan2(burst(n, 2000, 5000, 0.0003, 0.0015, rng_for("graphite", ev, note, vel, "tick")) * 0.012 * vel, 0.0)
        return out

    def reverb(self, ev):
        big = ev == "startup"
        return dict(rt60=1.3 if big else 0.42, predelay=0.006, damp_hz=3500, wet_db=-14 if big else -18,
                    er=[(0.004, 0.3, -0.6), (0.0065, 0.25, 0.6), (0.011, 0.15, 0.1)])

    def master(self, ev):
        return dict(hp=55.0 if ev == "startup" else 120.0, lp=12000.0, presence=(2800, -1.5, 1.0))

    def bloom_voice(self, n, note, vel, shift):
        return self.voice("startup", n, note, vel * 0.9, shift)

    def extra(self, ev, n):
        if ev == "error":
            return error_body(self, n)
        if ev != "startup":
            return None
        bed = np.zeros((n, 2))
        for (note, vel) in BED:
            place(bed, v_pad(n, hz(note), vel * 0.38, 0.28, 2.0, detune_c=2.0, harmonics=((1, 1.0), (2, 0.1), (4, 0.04))), 0.0)
        return bed


class Tine(Family):
    key, label = "tine", "Tine"
    desc = ("Electric-piano tine through a pickup: a warm bell with a gentle bark that softens as it rings, "
            "in a medium plate-like room. Musical and friendly, the most 'instrument' of the four.")
    step = 0.105
    shift = -12

    def voice(self, ev, n, note, vel, shift):
        f = hz(note, shift)
        life = LIFE[ev] * (1.2 if ev == "startup" else 1.0)
        bark = 0.7 if ev == "error" else 1.0
        out = v_tine(n, f, vel, life=life * 0.55, bark=bark, pan=pitch_pan(note))
        # the struck tip: a faint high ping an octave and a fifth up, gone in 40 ms
        out += pan2(partial(n, f * 3.0, 0.05 * vel, 0.04, 0.0008), pitch_pan(note))
        return out

    def reverb(self, ev):
        big = ev == "startup"
        return dict(rt60=1.7 if big else 0.8, predelay=0.010, damp_hz=4500, wet_db=-14 if big else -17,
                    er=[(0.005, 0.2, -0.4), (0.009, 0.2, 0.45), (0.014, 0.12, 0.0)], width=0.85)

    def master(self, ev):
        return dict(hp=55.0 if ev == "startup" else 140.0, lp=13000.0, shelf=(7000, -2.0))

    def extra(self, ev, n):
        if ev == "error":
            return error_body(self, n)
        if ev != "startup":
            return None
        bed = np.zeros((n, 2))
        for (note, vel) in BED:
            place(bed, v_pad(n, hz(note), vel * 0.3, 0.35, 2.2, detune_c=4.0, harmonics=((1, 1.0), (2, 0.25), (3, 0.08))), 0.0)
        return bed


class Signal(Family):
    key, label = "signal", "Signal"
    desc = ("Precise electronics: two-operator FM blips that snap into pure sines, a tiny switch-like tick, "
            "tight 80 ms timing and a short dry space. Minimal, exact, modern.")
    step = 0.08

    def voice(self, ev, n, note, vel, shift):
        f = hz(note, shift)
        life = LIFE[ev]
        p = pitch_pan(note, shift)
        if ev == "error":
            s = v_fm(n, f, vel, ratio=1.0, index=0.9, index_tau=0.03, glide=-30, glide_tau=0.03, t60=0.9 * life,
                     attack=0.003, pan=p)
            # body an octave below, a few cents off: a slow, soft roughness that reads as 'not right'
            s += v_fm(n, f * 0.5, vel * 0.5, ratio=1.0, index=0.4, index_tau=0.02, glide=-30, glide_tau=0.03,
                      t60=0.8 * life, attack=0.003, pan=-p, detune=9)
            return s
        s = v_fm(n, f, vel, ratio=2.0, index=1.6, index_tau=0.012, glide=22, glide_tau=0.010,
                 t60=(0.95 if ev == "startup" else 0.85) * life * (1.6 if ev == "startup" else 1.0), attack=0.0015, pan=p)
        s += v_fm(n, f * 2, vel * 0.12, ratio=1.0, index=0.0, index_tau=0.01, glide=0, t60=0.25 * life, attack=0.0015,
                  pan=-p)
        s += pan2(burst(n, 3000, 7000, 0.0003, 0.0012, rng_for("signal", ev, note, vel)) * 0.025 * vel, 0.0)
        return s

    def bloom_voice(self, n, note, vel, shift):
        f = hz(note, shift)
        return v_pad(n, f, vel * 0.7, 0.05, 1.6, detune_c=2.5, harmonics=((1, 1.0), (2, 0.06)), pan_w=0.35, seed=1)

    def reverb(self, ev):
        big = ev == "startup"
        return dict(rt60=1.4 if big else 0.45, predelay=0.008, damp_hz=6000, wet_db=-15 if big else -20,
                    er=[(0.003, 0.35, -0.7), (0.0055, 0.3, 0.7), (0.0095, 0.2, -0.3), (0.013, 0.15, 0.35)], width=0.9)

    def master(self, ev):
        return dict(hp=55.0 if ev == "startup" else 170.0, lp=15000.0, shelf=(8000, -1.5))

    def extra(self, ev, n):
        if ev != "startup":
            return None
        out = np.zeros((n, 2))
        for (note, vel) in BED:
            place(out, v_pad(n, hz(note), vel * 0.3, 0.25, 2.0, detune_c=1.5, harmonics=((1, 1.0), (2, 0.08))), 0.0)
        # air: a soft noise swell that opens upward into the bloom
        rng = rng_for("signal", "air")
        m = int(0.9 * SR)
        t = tvec(m)
        noise = band_noise(m, 1500, 9000, rng, tilt=-3)
        env = np.sin(np.pi * np.minimum(t / 0.9, 1.0)) ** 2
        air = noise * env * 0.010
        place(out, np.stack([air, np.roll(air, 97)], axis=1), 0.0)
        return out


FAMILIES = {f.key: f for f in (Prism(), Graphite(), Tine(), Signal())}


# --------------------------------------------------------------- mastering --
def limiter(x, ceiling_db, lookahead=0.0015, release=0.08, over=4):
    """Look-ahead peak limiter on the 4x oversampled true peak. Returns
    (signal, max gain reduction dB). Gain moves smoothly (raised-cosine attack
    across the look-ahead, exponential release), so there is no pumping on
    these single-hit sounds; it does nothing when the peak is under the ceiling."""
    ceil = 10 ** (ceiling_db / 20)
    n = len(x)
    # oversampled peak envelope folded back to the base rate
    pk = np.zeros(n)
    pad = 256
    for c in range(2):
        v = np.concatenate([np.zeros(pad), x[:, c], np.zeros(pad)])
        nf = 1 << int(math.ceil(math.log2(len(v))))
        up = np.abs(np.fft.irfft(np.fft.rfft(v, nf), nf * over) * over)[pad * over:(pad + n) * over]
        pk = np.maximum(pk, up.reshape(n, over).max(axis=1))
    need = np.minimum(1.0, ceil / np.maximum(pk, 1e-12))
    if need.min() >= 1.0:
        return x, 0.0
    la = max(1, int(lookahead * SR))
    # hold the minimum across the look-ahead window (moving min, forward-looking)
    padded = np.concatenate([need, np.ones(la)])
    g = np.min(np.lib.stride_tricks.sliding_window_view(padded, la + 1), axis=1)[:n]
    # smooth: release one-pole (loop), then a raised-cosine attack smoothing
    rel = math.exp(-1.0 / (release * SR))
    out = np.empty(n)
    s = 1.0
    gl = g.tolist()
    for i, v in enumerate(gl):
        s = v if v < s else v + (s - v) * rel
        out[i] = s
    k = np.hanning(la * 2 + 1)
    k /= k.sum()
    sm = np.convolve(np.concatenate([np.full(la, out[0]), out, np.full(la, out[-1])]), k, mode="valid")
    sm = np.minimum(sm, out)  # never above the needed gain
    return x * sm[:, None], 20 * math.log10(sm.min())


def finish(fam, ev, dry):
    """Reverb, filters, trim, fades, loudness, true-peak ceiling."""
    tg = TARGET[ev]
    rv = dict(fam.reverb(ev))
    if LIFE[ev] < 0.3:  # the shortest sounds get a shorter, quieter room so they stay crisp
        rv["rt60"] = min(rv["rt60"], 0.5)
        rv["wet_db"] -= 3
    ir = make_ir(rv["rt60"], rv["predelay"], rv["damp_hz"], er=rv.get("er"), seed=f"{fam.key}/{ev}",
                 width=rv.get("width", 1.0))
    wet = convolve(filt(dry, ("hp", 250.0, 0.7071), ("lp", 9000.0, 0.7071)), ir)[:len(dry)]
    x = dry + wet * 10 ** (rv["wet_db"] / 20)
    ms = fam.master(ev)
    st = [("hp", ms["hp"], 0.7071), ("hp", ms["hp"], 0.7071), ("lp", ms["lp"], 0.7071)]
    if "shelf" in ms:
        st.append(("hshelf", ms["shelf"][0], 0.7071, ms["shelf"][1]))
    if "presence" in ms:
        st.append(("peak", ms["presence"][0], ms["presence"][2], ms["presence"][1]))
    x = filt(x, *st)
    x -= x.mean(axis=0)
    # length: where the 10 ms envelope falls 60 dB under its peak, capped
    t, e = M.envelope_db(x, SR, 10.0)
    above = np.where(e > e.max() - 60)[0]
    end = min(int((t[above[-1]] + 0.03) * SR), int(tg["maxlen"] * SR), len(x))
    end = max(end, int(tg["minlen"] * SR))
    x = x[:end].copy()
    fin = int(0.0008 * SR)
    x[:fin] *= (0.5 - 0.5 * np.cos(np.pi * np.arange(fin) / fin))[:, None]
    fade = max(int(0.04 * SR), int(0.18 * len(x)))
    x[-fade:] *= (0.5 + 0.5 * np.cos(np.pi * (np.arange(fade) + 1) / fade))[:, None]
    # loudness, then the ceiling; repeat so both hold
    gr = 0.0
    for _ in range(4):
        mm = M.loudness_window(x, SR, 0.1)
        x = x * 10 ** ((tg["hit"] - mm) / 20)
        x, g = limiter(x, tg["tp"] - 0.2)
        gr = min(gr, g)
        if abs(M.loudness_window(x, SR, 0.1) - tg["hit"]) < 0.1:
            break
    x[0] = 0.0
    x[-1] = 0.0
    return x, gr


# ------------------------------------------------------------ verification --
def verify(ev, info):
    """Failed checks for one file (empty list = pass)."""
    tg, L = TARGET[ev], LIMITS
    bad = []
    if abs(info["lufs_100ms_max"] - tg["hit"]) > L["hit_tol"]:
        bad.append(f"loudness {info['lufs_100ms_max']} vs {tg['hit']}")
    if info["true_peak_dbtp"] > tg["tp"]:
        bad.append(f"true peak {info['true_peak_dbtp']} > {tg['tp']}")
    if info["true_peak_dbtp"] > -1.0:
        bad.append("true peak over -1 dBTP")
    if not (tg["minlen"] - 1e-3 <= info["length_s"] <= tg["maxlen"] + 1e-3):
        bad.append(f"length {info['length_s']}")
    if info["dc_dbfs"] > L["dc"]:
        bad.append(f"DC {info['dc_dbfs']}")
    if info["edge_start_dbfs"] > L["edge"] or info["edge_end_dbfs"] > L["edge"]:
        bad.append("edges not silent")
    if info["bands_pct"]["presence"] > L["presence_pct"]:
        bad.append(f"2-5 kHz share {info['bands_pct']['presence']} %")
    if info["presence_peak_rel_db"] > L["presence_peak"]:
        bad.append(f"2-5 kHz peak {info['presence_peak_rel_db']} dB")
    if info["sub_db"] > L["sub_db"]:
        bad.append(f"sub {info['sub_db']} dB")
    if info.get("hf8k_frame_db", -120) > L["hf8k"]:
        bad.append(f"click / HF frame {info['hf8k_frame_db']} dB")
    if info["corr"] < L["corr"]:
        bad.append(f"correlation {info['corr']}")
    if info["mono_change_db"] < L["mono"]:
        bad.append(f"mono fold {info['mono_change_db']} dB")
    return bad


def table(report):
    head = ("| family | event | len s | hit LUFS | target | M max LUFS | TP dBTP | ceiling | PLR | crest | 2-5k % | 2-5k pk | "
            "centroid Hz | corr | mono dB | rise ms | tail -60 s | limiter dB | checks |")
    rows = [head, "|" + "---|" * (head.count("|") - 1)]
    for fam, evs in report.items():
        for ev, i in evs.items():
            rows.append(f"| {fam} | {ev} | {i['length_s']:.2f} | {i['lufs_100ms_max']:.1f} | {TARGET[ev]['hit']:.1f} | "
                        f"{i['lufs_m_max']:.1f} | "
                        f"{i['true_peak_dbtp']:.1f} | {TARGET[ev]['tp']:.0f} | {i['plr_db']:.1f} | {i['crest_db']:.1f} | "
                        f"{i['bands_pct']['presence']:.1f} | {i['presence_peak_rel_db']:.0f} | {i['centroid_hz']} | "
                        f"{i['corr']:.2f} | {i['mono_change_db']:+.2f} | {i['rise_ms']:.1f} | {i['tail_60_s']:.2f} | "
                        f"{i.get('limiter_db', 0):.1f} | {'pass' if not i['fails'] else '; '.join(i['fails'])} |")
    return "\n".join(rows) + "\n"


# -------------------------------------------------------------------- main --
def main():
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", help="write <out>/<family>/<event>.wav")
    ap.add_argument("--families", default=",".join(FAMILIES))
    ap.add_argument("--events", default=",".join(EVENTS))
    ap.add_argument("--report")
    ap.add_argument("--table")
    ap.add_argument("--install", help="families to write into assets/sounds/<family>/ (comma list or 'all')")
    ap.add_argument("--level", help="set every <dir>/<event>.wav to its loudness target with one gain (in place)")
    ap.add_argument("--check", help="only measure and verify <dir>/<event>.wav")
    a = ap.parse_args()
    events = a.events.split(",")
    if a.level:
        bad = 0
        for ev in events:
            path = os.path.join(a.level, ev + ".wav")
            sr, _, _, x = M.read_wav(path)
            g = TARGET[ev]["hit"] - M.loudness_window(x, sr, 0.1)
            x = x * 10 ** (g / 20)
            M.write_wav24(path, x, sr)
            info = M.measure_file(path)
            fails = [f for f in verify(ev, info) if not f.startswith(("2-5 kHz", "correlation", "length"))]
            bad += bool(fails)
            print(f"{ev:13s} gain {g:+5.2f} dB  hit {info['lufs_100ms_max']:6.1f}  TP {info['true_peak_dbtp']:6.1f}  "
                  f"{'ok' if not fails else fails}")
        sys.exit(1 if bad else 0)
    if a.check:
        bad = 0
        for ev in events:
            info = M.measure_file(os.path.join(a.check, ev + ".wav"))
            fails = verify(ev, info)
            bad += bool(fails)
            print(f"{ev:13s} hit {info['lufs_100ms_max']:6.1f}  M {info['lufs_m_max']:6.1f}  TP {info['true_peak_dbtp']:6.1f}  {'pass' if not fails else fails}")
        sys.exit(1 if bad else 0)
    jobs = []  # (family, destination dir, events)
    if a.install:
        fams = list(FAMILIES) if a.install == "all" else a.install.split(",")
        jobs += [(fk, os.path.join(here, "assets", "sounds", fk), events) for fk in fams]
    if a.out:
        jobs += [(f, os.path.join(a.out, f), events) for f in a.families.split(",")]
    if not jobs:
        ap.error("nothing to do: give --out, --install or --check")
    report, failed = {}, 0
    for fam_key, dst, evs in jobs:
        fam = FAMILIES[fam_key]
        os.makedirs(dst, exist_ok=True)
        for ev in evs:
            x, gr = finish(fam, ev, fam.render(ev))
            path = os.path.join(dst, ev + ".wav")
            M.write_wav24(path, x, SR)
            info = M.measure_file(path)
            info["limiter_db"] = round(gr, 2)
            info["fails"] = verify(ev, info)
            failed += bool(info["fails"])
            report.setdefault(fam_key, {})[ev] = info
            print(f"{fam_key:9s} {ev:13s} {info['length_s']:5.2f}s  hit {info['lufs_100ms_max']:6.1f}  M {info['lufs_m_max']:6.1f}  TP {info['true_peak_dbtp']:6.1f}  "
                  f"PLR {info['plr_db']:4.1f}  cen {info['centroid_hz']:5d}  2-5k {info['bands_pct']['presence']:4.1f}%  "
                  f"corr {info['corr']:5.2f}  mono {info['mono_change_db']:+.2f}  lim {gr:5.1f}  "
                  f"{'ok' if not info['fails'] else 'FAIL ' + '; '.join(info['fails'])}")
    if a.report:
        meta = {k: {"label": f.label, "desc": f.desc} for k, f in FAMILIES.items()}
        with open(a.report, "w") as f:
            json.dump({"families": meta, "events": EVENTS, "targets": TARGET, "limits": LIMITS, "files": report}, f, indent=1)
    if a.table:
        with open(a.table, "w") as f:
            f.write(table(report))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
