#!/usr/bin/env python3
"""Write Gridline's demo song (J-135) as a project file. Standard library only.

The song is composed here as data: harmony, motif, drum patterns, sound
design (parameters of Gridline's own instruments and effects), the mixer
(buses, sends, sidechain) and the automation. Nothing is sampled: every
sound comes from the built-in gl.* plugins when Gridline plays the file.
The same command always writes the same bytes.

Usage:
  python3 tools/make_demo_song.py --gridline build/gridline --out assets/demo/Gridline.gridline.json

Then rebuild: the file is compiled into the programs (cmake/EmbedDemo.cmake), and the test
j135_demo_song checks that the compiled copy matches it.

`--gridline` is used once to read the plugin catalogue (`gridline plugin-params`),
so parameters are set by name and the file follows the current parameter lists.
"""
import argparse
import json
import random
import subprocess
import sys

PPQ = 96
BAR = PPQ * 4
STEP = PPQ // 4


# ------------------------------------------------------------------ catalogue
class Catalog:
    def __init__(self, exe):
        out = subprocess.run([exe, "plugin-params"], capture_output=True, check=True).stdout
        self.by_type = {p["type"]: p for p in json.loads(out)}

    def defaults(self, ty):
        return [float(p["def"]) for p in self.by_type[ty]["params"]]

    def index(self, ty, name):
        if isinstance(name, int):
            return name
        params = self.by_type[ty]["params"]
        for i, p in enumerate(params):
            if p["name"] == name:
                return i
        raise KeyError(f"{ty}: no parameter '{name}'")

    def desc(self, ty, name):
        return self.by_type[ty]["params"][self.index(ty, name)]

    def state(self, ty, values):
        params = self.defaults(ty)
        for k, v in values.items():
            i = self.index(ty, k)
            d = self.by_type[ty]["params"][i]
            if "choices" in d and isinstance(v, str):
                v = d["choices"].index(v)
            lo, hi = float(d["min"]), float(d["max"])
            if not lo <= float(v) <= hi:
                raise ValueError(f"{ty} '{d['name']}' = {v} outside {lo}..{hi}")
            params[i] = float(v)
        return {"type": ty, "params": params}


# ------------------------------------------------------------------- builder
class SongBuilder:
    def __init__(self, cat, tempo, title):
        self.cat = cat
        self.next_id = 2
        self.tempo = tempo
        self.info = {"title": title, "author": "Gridline", "genre": "Drum & Bass", "url": "",
                     "comments": "The Gridline demo song, made entirely with the built-in instruments and effects."}
        self.channels, self.patterns, self.playlist, self.automations, self.markers = [], [], [], [], []
        self.tracks = []
        self.mixer = []
        master = self._mixer_track("Master", 0x50565E)
        master["sends"] = []
        self.master = master

    def nid(self):
        i = self.next_id
        self.next_id += 1
        return i

    # mixer --------------------------------------------------------------------
    def _mixer_track(self, name, color):
        t = {"id": self.nid(), "name": name, "color": color, "volume": 0.8, "pan": 0, "stereoSep": 0, "muted": False,
             "solo": False, "invertPolarity": False, "swapLR": False,
             "slots": [{"enabled": True, "mix": 1} for _ in range(10)], "sends": []}
        self.mixer.append(t)
        return t

    def insert(self, name, color, icon=0, to=None, volume_db=0.0, pan=0.0):
        t = self._mixer_track(name, color)
        t["sends"] = [{"to": (to or self.master)["id"], "level": 1}]
        t["volume"] = fader(volume_db, 0.8)
        t["pan"] = pan
        if icon:
            t["icon"] = icon
        return t

    def send(self, src, dst, level_db=0.0, sidechain=False, pre=False):
        s = {"to": dst["id"], "level": round(10 ** (level_db / 20), 6) if not sidechain else 1}
        if sidechain:
            s["sidechain"] = True
        if pre:
            s["preFader"] = True
        src["sends"].append(s)

    def fx(self, track, slot, ty, mix=1.0, **values):
        track["slots"][slot] = {"enabled": True, "mix": mix, "plugin": self.cat.state(ty, values)}
        return (track, slot, ty)

    def strip_eq(self, track, bands):
        """bands: [(gain_db, freq, q)] x3 (low shelf, peak, high shelf)."""
        track["eq"] = [{"gain": g, "freq": f, "q": q} for (g, f, q) in bands]

    # channels -----------------------------------------------------------------
    def channel(self, name, ty, color, track, volume_db=0.0, pan=0.0, **values):
        c = {"id": self.nid(), "name": name, "color": color, "gain": fader(volume_db, 0.78), "pan": pan, "muted": False,
             "pitch": 0, "mixerTrack": track["id"], "plugin": self.cat.state(ty, values)}
        self.channels.append(c)
        return c

    # patterns -----------------------------------------------------------------
    def pattern(self, name, bars, color):
        p = {"id": self.nid(), "name": name, "color": color, "bars": bars, "stepsPerBar": 16, "notes": {}, "steps": {}}
        self.patterns.append(p)
        return p

    @staticmethod
    def notes(p, ch, notes):
        """notes: [(tick, length, pitch, velocity[, extra dict])]"""
        lst = p["notes"].setdefault(str(ch["id"]), [])
        for n in notes:
            e = {"start": int(n[0]), "length": int(n[1]), "pitch": int(n[2]), "velocity": int(n[3])}
            if len(n) > 4:
                e.update(n[4])
            lst.append(e)
        lst.sort(key=lambda e: (e["start"], e["pitch"]))

    def pattern_auto(self, p, owner, index, points):
        p.setdefault("automation", []).append({"owner": owner, "index": index, "points": [list(q) for q in points]})

    # playlist -----------------------------------------------------------------
    def track(self, name, color):
        self.tracks.append({"name": name, "color": color, "muted": False})
        return len(self.tracks) - 1

    def clip(self, p, track, bar, bars=None):
        c = {"id": self.nid(), "pattern": p["id"], "track": track, "start": int(bar * BAR),
             "length": int((bars if bars is not None else p["bars"]) * BAR)}
        self.playlist.append(c)

    def automation(self, name, color, track, owner, index, points, start_bar, end_bar, lo=0.0, hi=1.0):
        """points: [(bar (absolute), value 0..1 within lo..hi, tension, shape)] -> one automation clip."""
        a = {"id": self.nid(), "name": name, "color": color, "owner": owner, "index": index, "flCurves": True,
             "points": [[int(round((b - start_bar) * BAR)), round(v, 5), t, s] for (b, v, t, s) in points]}
        if lo != 0.0:
            a["min"] = lo
        if hi != 1.0:
            a["max"] = hi
        self.automations.append(a)
        self.playlist.append({"id": self.nid(), "pattern": 0, "track": track, "start": int(start_bar * BAR),
                              "length": int((end_bar - start_bar) * BAR), "kind": "automation", "source": a["id"]})
        return a

    def marker(self, bar, name):
        self.markers.append({"pos": int(bar * BAR), "name": name})

    def to_json(self):
        while len(self.tracks) < 32:
            self.tracks.append({"name": "", "color": 0x50565E, "muted": False})
        return {"format": "gridline-project", "version": 8, "info": self.info, "ppq": PPQ, "tempo": self.tempo,
                "timeSigNum": 4, "timeSigDen": 4, "swing": 0, "masterPitch": 0, "nextId": self.next_id,
                "channels": self.channels, "patterns": self.patterns, "playlist": self.playlist, "tracks": self.tracks,
                "mixer": self.mixer, "links": [], "modLinks": [], "automations": self.automations, "markers": self.markers}


TRIM = -10.0
CLIP_GAIN = 13.4  # master clipper input gain (dB)
LIMIT_GAIN = 2.5  # master limiter input gain (dB)  # channel trim (dB) for gain staging


def fader(db, unity):
    """Fader position for a gain in dB (Gridline faders: gain = (v / unity)^2)."""
    return round(unity * 10 ** (db / 40.0), 6)


def norm(cat, ty, name, value):
    """Normalized 0..1 value of a parameter (log scale for frequencies in Hz)."""
    import math
    d = cat.desc(ty, name)
    lo, hi = float(d["min"]), float(d["max"])
    if d.get("unit") == "Hz" and lo > 0 and LOG_PARAMS.get((ty, d["name"]), True):
        return math.log(value / lo) / math.log(hi / lo)
    return (value - lo) / (hi - lo)


LOG_PARAMS = {}


# ================================================================== the song
TITLE = "Gridline"
TEMPO = 174

NOTE = {n: i for i, n in enumerate(["C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B"])}


def m(name):
    """'Db5' -> MIDI number (C4 = 60)."""
    return 12 * (int(name[-1]) + 1) + NOTE[name[:-1]]


# Chords: rootless voicings (the bass carries the root) and the bass root.
CHORDS = {
    "Gbmaj9": (["Bb3", "Db4", "F4", "Ab4"], "Gb1"),
    "Absus": (["Ab3", "Db4", "Eb4", "Bb4"], "Ab1"),   # Ab sus4 add9: holds the whole motif (Db Ab Eb)
    "Ab": (["Ab3", "C4", "Eb4", "Bb4"], "Ab1"),
    "Fm9": (["Ab3", "C4", "Eb4", "G4"], "F1"),
    "Bbm9": (["Db4", "F4", "Ab4", "C5"], "Bb1"),
    "Ebm11": (["Gb3", "Bb3", "Db4", "Ab4"], "Eb2"),
    "Dbadd9": (["Db4", "F4", "Ab4", "Eb5"], "Db2"),
    "Db/F": (["Db4", "Ab4", "Eb5", "F5"], "F1"),
    "Ab7sus": (["Gb3", "Db4", "Eb4", "Bb4"], "Ab1"),
}

# Progressions: [(chord, bars)]
PROG = {
    "introA": [("Dbadd9", 4), ("Gbmaj9", 4)] * 2,
    "chorus": [("Gbmaj9", 2), ("Absus", 1), ("Ab", 1), ("Fm9", 2), ("Bbm9", 2),
               ("Gbmaj9", 2), ("Absus", 1), ("Ab", 1), ("Fm9", 1), ("Bbm9", 1), ("Ebm11", 1), ("Absus", 1)],
    "verse": [("Bbm9", 4), ("Gbmaj9", 4), ("Ebm11", 4), ("Fm9", 2), ("Absus", 2)],
    "bridge": [("Ebm11", 2), ("Fm9", 2), ("Gbmaj9", 2), ("Ab", 2), ("Ebm11", 2), ("Fm9", 2), ("Gbmaj9", 2), ("Ab7sus", 2)],
    "breakdown": [("Bbm9", 4), ("Gbmaj9", 4), ("Db/F", 4), ("Absus", 2), ("Ab", 2)],
    "outro": [("Gbmaj9", 2), ("Absus", 1), ("Ab", 1), ("Fm9", 2), ("Bbm9", 2), ("Gbmaj9", 4), ("Dbadd9", 4)],
}

# Arrangement (bars): name, start, length
SECTIONS = [("Intro", 0, 32), ("Verse", 32, 32), ("Chorus", 64, 32), ("Verse 2", 96, 16), ("Bridge", 112, 16),
            ("Breakdown", 128, 16), ("Chorus 2", 144, 32), ("Outro", 176, 16)]
SONG_BARS = 192

# Colours (warm graphite family)
C_DRUM, C_DRUM2, C_PERC = 0xC8875A, 0xB97852, 0xC9A65A
C_BASS, C_SUB = 0x8E6CC8, 0x6F58A8
C_PAD, C_KEYS, C_MALLET, C_PLUCK, C_AIR = 0x5A8FB8, 0x6FA89A, 0xD0A060, 0xC0905A, 0x7E8C9C
C_FX, C_RET, C_BUS = 0x9AA0A8, 0x70808E, 0x5E6670


def expand(prog):
    """[(chord, bars)] -> [(chord, start bar, bars)]"""
    out, b = [], 0
    for c, n in prog:
        out.append((c, b, n))
        b += n
    return out


class Song:
    """Instruments, mixer and playlist tracks of the song; the parts are added by the functions below."""

    def __init__(self, cat):
        S = self.S = SongBuilder(cat, TEMPO, TITLE)
        self.cat = cat
        self.rnd = random.Random(135)
        # ------------------------------------------------------------ mixer
        mk = S.insert
        self.t = t = {}
        t["drum_bus"] = mk("Drum Bus", C_DRUM2, icon=14)
        t["bass_bus"] = mk("Bass Bus", C_BASS, icon=14)
        t["music_bus"] = mk("Music Bus", C_PAD, icon=14)
        t["kick"] = mk("Kick", C_DRUM, icon=1, to=t["drum_bus"])
        t["snare"] = mk("Snare", C_DRUM, icon=2, to=t["drum_bus"])
        t["clap"] = mk("Clap", C_DRUM, icon=2, to=t["drum_bus"])
        t["hats"] = mk("Hats", C_PERC, icon=3, to=t["drum_bus"])
        t["perc"] = mk("Shaker + Ride", C_PERC, icon=3, to=t["drum_bus"])
        t["wood"] = mk("Wood", C_MALLET, icon=4, to=t["drum_bus"])
        t["sub"] = mk("Sub", C_SUB, icon=5, to=t["bass_bus"])
        t["reese"] = mk("Reese", C_BASS, icon=5, to=t["bass_bus"])
        t["pad"] = mk("Haze Pad", C_PAD, icon=8, to=t["music_bus"])
        t["airpad"] = mk("Air Pad", C_AIR, icon=8, to=t["music_bus"])
        t["keys"] = mk("Felt Keys", C_KEYS, icon=6, to=t["music_bus"])
        t["mallet"] = mk("Graphite Mallet", C_MALLET, icon=7, to=t["music_bus"])
        t["pluck"] = mk("Felt Pluck", C_PLUCK, icon=7, to=t["music_bus"])
        t["atmos_bus"] = mk("Weather Bus", C_AIR, icon=14)
        t["rain"] = mk("Rain", C_AIR, icon=11, to=t["atmos_bus"])
        t["drops"] = mk("Droplets", C_AIR, icon=11, to=t["atmos_bus"])
        t["dust"] = mk("Dust + Hiss", C_AIR, icon=11, to=t["atmos_bus"])
        t["fx"] = mk("Risers", C_FX, icon=11)
        t["impact"] = mk("Impacts", C_FX, icon=11)
        t["hall"] = mk("Hall", C_RET, icon=12)
        t["echo"] = mk("Echo", C_RET, icon=13)
        t["space"] = mk("Space", C_RET, icon=12)
        # Strip order: sources, then their bus; returns last.
        S.mixer = [S.master] + [t[k] for k in ["kick", "snare", "clap", "hats", "perc", "wood", "drum_bus", "sub", "reese",
                                               "bass_bus", "pad", "airpad", "keys", "mallet", "pluck", "music_bus", "rain",
                                               "drops", "dust", "atmos_bus", "fx", "impact", "hall", "echo", "space"]]
        # ---------------------------------------------------------- channels
        self.c = c = {}
        ch = S.channel
        # Drums: punchy but soft-edged; no raw top end (the snare noise sits at 3 kHz, tilted down).
        c["kick"] = ch("Kick", "gl.kick", C_DRUM, t["kick"], **{
            "Start pitch": 260, "End pitch": 52, "Sweep": 0.035, "Decay": 0.30, "Click": 0.22, "Drive": 0.12,
            "Volume": 0.9, "Key tracking": 0, "Click tone": 3500, "Click length": 0.003, "Tone": 9000})
        c["snare"] = ch("Snare", "gl.snare", C_DRUM, t["snare"], **{
            "Tone": 0.55, "Snappy": 0.58, "Body decay": 0.15, "Noise decay": 0.16, "Tune": 205, "Volume": 1.0,
            "Pitch sweep": 4, "Sweep time": 0.015, "Noise band": 3000, "Noise width": 0.8, "Tone tilt": -5, "Click": 0.1})
        c["clap"] = ch("Clap", "gl.clap", C_DRUM, t["clap"], **{
            "Decay": 0.2, "Spread": 0.01, "Tone": 1400, "Volume": 0.7, "Hits": 3, "Width": 0.7, "Stereo spread": 0.5,
            "Humanize": 0.3})
        c["hat"] = ch("Hat", "gl.hat", C_PERC, t["hats"], pan=0.12, **{
            "Decay": 0.035, "Tone": 9000, "Volume": 0.6, "Metal": 0.1, "Width": 0.6, "Choke": 1, "Attack": 0.0008})
        c["ohat"] = ch("Open Hat", "gl.hat", C_PERC, t["hats"], pan=-0.12, **{
            "Decay": 0.22, "Tone": 8200, "Open": 1, "Volume": 0.5, "Metal": 0.12, "Width": 0.7, "Choke": 1})
        c["shaker"] = ch("Shaker", "gl.hat", C_PERC, t["perc"], pan=0.3, **{
            "Decay": 0.05, "Tone": 7000, "Volume": 0.45, "Metal": 0.0, "Width": 0.9, "Attack": 0.006, "Choke": 0})
        c["ride"] = ch("Ride", "gl.hat", C_PERC, t["perc"], pan=-0.25, **{
            "Decay": 0.3, "Tone": 7400, "Open": 1, "Volume": 0.34, "Metal": 0.1, "Base freq": 420, "Width": 0.55,
            "Choke": 0})
        # Wood: a small Graphite bar for percussion accents (the bar's modes, very short).
        c["wood"] = ch("Wood", "gl.fm", C_MALLET, t["wood"], **{
            "Algorithm": 7, "Op 1 ratio": 1, "Op 1 level": 1, "Op 1 attack": 0.001, "Op 1 decay": 0.09, "Op 1 sustain": 0,
            "Op 1 release": 0.09, "Op 2 ratio": 3.99, "Op 2 level": 0.35, "Op 2 attack": 0.001, "Op 2 decay": 0.035,
            "Op 2 sustain": 0, "Op 2 release": 0.035, "Op 3 ratio": 9.2, "Op 3 level": 0.08, "Op 3 attack": 0.001,
            "Op 3 decay": 0.012, "Op 3 sustain": 0, "Op 3 release": 0.012, "Op 4 level": 0, "Feedback": 0.05,
            "Voices": 4, "Volume": 0.6})
        # Bass: a pure mono sine sub, and a reese above it (three detuned saws in unison, filtered, moving).
        c["sub"] = ch("Sub", "gl.analog", C_SUB, t["sub"], **{
            "Volume": 0.8, "Voice mode": "Legato", "Glide": 0.045, "Glide mode": "Overlap", "Velocity to volume": 0.1,
            "Velocity to cutoff": 0, "Osc 1 wave": "Sine", "Osc 1 level": 1, "Osc 2 on": 0, "Filter mode": "LP 12",
            "Cutoff": 240, "Resonance": 0, "Key tracking": 0, "Filter env amount": 0, "Amp attack": 0.004,
            "Amp decay": 0.3, "Amp sustain": 1, "Amp release": 0.09})
        c["reese"] = ch("Reese", "gl.analog", C_BASS, t["reese"], **{
            "Volume": 0.5, "Voice mode": "Legato", "Glide": 0.05, "Glide mode": "Overlap", "Velocity to volume": 0.1,
            "Velocity to cutoff": 0, "Unison voices": 3, "Unison detune": 0.14, "Unison spread": 0.7,
            "Osc 1 wave": "Saw", "Osc 1 fine": -9, "Osc 1 level": 0.8, "Osc 2 wave": "Saw", "Osc 2 fine": 11,
            "Osc 2 level": 0.75, "Osc 3 on": 1, "Osc 3 wave": "Pulse", "Osc 3 coarse": 12, "Osc 3 fine": 4,
            "Osc 3 level": 0.18, "Osc 3 pulse width": 0.3, "Filter mode": "LP 24", "Cutoff": 650, "Resonance": 0.2,
            "Filter drive": 0.25, "Key tracking": 0.25, "Filter env amount": 0.0, "Amp attack": 0.006, "Amp sustain": 1,
            "Amp release": 0.12, "LFO 1 shape": "Smooth random", "LFO 1 rate": 0.3, "LFO 1 retrigger": 0,
            "LFO 1 target": "Cutoff", "LFO 1 amount": 0.12, "LFO 2 shape": "Sine", "LFO 2 rate": 0.09,
            "LFO 2 retrigger": 0, "LFO 2 target": "Unison detune", "LFO 2 amount": 0.35, "Mod 1 source": "LFO 2",
            "Mod 1 target": "Pulse width", "Mod 1 amount": 0.3, "Drive": 0.18})
        # Pads: a warm haze in the middle, and a high, wide air pad that opens the quiet sections.
        c["pad"] = ch("Haze Pad", "gl.analog", C_PAD, t["pad"], **{
            "Volume": 0.3, "Polyphony": 16, "Velocity to volume": 0.3, "Velocity to cutoff": 0.1, "Unison voices": 3,
            "Unison detune": 0.16, "Unison spread": 0.9, "Osc 1 wave": "Saw", "Osc 1 level": 0.7, "Osc 2 wave": "Pulse",
            "Osc 2 fine": 7, "Osc 2 level": 0.5, "Osc 2 pulse width": 0.32, "Osc 3 on": 1, "Osc 3 wave": "Triangle",
            "Osc 3 coarse": 12, "Osc 3 level": 0.22, "Filter mode": "LP 12", "Cutoff": 1500, "Resonance": 0.08,
            "Key tracking": 0.3, "Filter env amount": 0.12, "Filter attack": 1.4, "Filter decay": 2.5,
            "Filter sustain": 0.5, "Amp attack": 0.9, "Amp decay": 2.0, "Amp sustain": 0.85, "Amp release": 2.4,
            "LFO 1 shape": "Sine", "LFO 1 rate": 0.13, "LFO 1 retrigger": 0, "LFO 1 target": "Cutoff",
            "LFO 1 amount": 0.1, "LFO 2 shape": "Smooth random", "LFO 2 rate": 0.2, "LFO 2 retrigger": 0,
            "LFO 2 target": "Pulse width", "LFO 2 amount": 0.25, "Chorus": 1, "Chorus rate": 0.3,
            "Chorus depth": 0.5, "Chorus mix": 0.45})
        c["airpad"] = ch("Air Pad", "gl.analog", C_AIR, t["airpad"], **{
            "Volume": 0.3, "Polyphony": 16, "Velocity to volume": 0.4, "Velocity to cutoff": 0, "Unison voices": 4,
            "Unison detune": 0.1, "Unison spread": 0.8, "Osc 1 wave": "Triangle", "Osc 1 level": 0.8,
            "Osc 2 wave": "Saw", "Osc 2 fine": -6, "Osc 2 level": 0.22, "Osc 3 on": 1, "Osc 3 wave": "Sine",
            "Osc 3 coarse": 12, "Osc 3 level": 0.3, "Noise": 0.04, "Filter mode": "LP 12", "Cutoff": 4500,
            "Resonance": 0.05, "Key tracking": 0.2, "Filter env amount": 0, "Amp attack": 2.6, "Amp decay": 3.0,
            "Amp sustain": 0.9, "Amp release": 4.5, "LFO 1 shape": "Sine", "LFO 1 rate": 0.07, "LFO 1 retrigger": 0,
            "LFO 1 target": "Volume", "LFO 1 amount": 0.12, "Chorus": 1, "Chorus rate": 0.2, "Chorus depth": 0.5,
            "Chorus mix": 0.35})
        c["keys"] = ch("Felt Keys", "gl.keys", C_KEYS, t["keys"], **{
            "Decay": 4.0, "Release": 0.6, "Hardness": 0.3, "Treble": -1.5, "Muffle": 6800, "Tremolo rate": 3.2,
            "Tremolo depth": 0.18, "Stereo": 0.6, "Detune": 6, "Overdrive": 0.12, "Velocity sens.": 0.6,
            "Volume": 0.36, "Model": "Soft", "Velocity curve": "Soft", "Modulation": "Auto-pan"})
        # The Graphite bar, felted: the fundamental and the bar's 3.99 mode ring as carriers; a third carrier
        # at the fundamental is struck by a short, bright 9.2 modulator burst: the felt contact.
        c["mallet"] = ch("Graphite Mallet", "gl.fm", C_MALLET, t["mallet"], **{
            "Algorithm": 6, "Op 1 ratio": 1, "Op 1 level": 1, "Op 1 attack": 0.002, "Op 1 decay": 1.7,
            "Op 1 sustain": 0, "Op 1 release": 1.3, "Op 1 velocity": 0.5, "Op 2 ratio": 3.99, "Op 2 level": 0.38,
            "Op 2 attack": 0.001, "Op 2 decay": 0.38, "Op 2 sustain": 0, "Op 2 release": 0.3, "Op 2 velocity": 0.8,
            "Op 3 ratio": 1, "Op 3 level": 0.45, "Op 3 attack": 0.001, "Op 3 decay": 0.09, "Op 3 sustain": 0,
            "Op 3 release": 0.08, "Op 3 velocity": 0.9, "Op 4 ratio": 9.2, "Op 4 level": 0.32, "Op 4 attack": 0.001,
            "Op 4 decay": 0.014, "Op 4 sustain": 0, "Op 4 release": 0.012, "Op 4 velocity": 1, "Feedback": 0.0,
            "Voices": 12, "Volume": 0.9})
        c["pluck"] = ch("Felt Pluck", "gl.fm", C_PLUCK, t["pluck"], **{
            "Algorithm": 6, "Op 1 ratio": 1, "Op 1 level": 1, "Op 1 attack": 0.002, "Op 1 decay": 0.55,
            "Op 1 sustain": 0, "Op 1 release": 0.4, "Op 1 velocity": 0.6, "Op 2 ratio": 3.99, "Op 2 level": 0.22,
            "Op 2 attack": 0.001, "Op 2 decay": 0.16, "Op 2 sustain": 0, "Op 2 release": 0.15, "Op 3 ratio": 2,
            "Op 3 level": 0.35, "Op 3 attack": 0.002, "Op 3 decay": 0.3, "Op 3 sustain": 0, "Op 3 release": 0.25,
            "Op 4 ratio": 1, "Op 4 level": 0.25, "Op 4 attack": 0.001, "Op 4 decay": 0.08, "Op 4 sustain": 0,
            "Op 4 release": 0.08, "Feedback": 0.0, "Voices": 8, "Volume": 0.55})
        # Weather: a drizzle bed that breathes, droplets falling across the stereo field, vinyl dust and tape hiss.
        c["rain"] = ch("Rain", "gl.analog", C_AIR, t["rain"], **{
            "Volume": 0.5, "Unison voices": 2, "Unison spread": 1.0, "Osc 1 wave": "Noise", "Osc 1 level": 0.7,
            "Osc 2 on": 0, "Filter mode": "BP 12", "Cutoff": 3800, "Resonance": 0.1, "Key tracking": 0,
            "Filter env amount": 0, "Amp attack": 1.5, "Amp sustain": 1, "Amp release": 6.0,
            "LFO 1 shape": "Smooth random", "LFO 1 rate": 0.06, "LFO 1 retrigger": 0, "LFO 1 target": "Cutoff",
            "LFO 1 amount": 0.22, "LFO 2 shape": "Smooth random", "LFO 2 rate": 0.11, "LFO 2 retrigger": 0,
            "LFO 2 target": "Volume", "LFO 2 amount": 0.2})
        c["drops"] = ch("Droplets", "gl.fm", C_AIR, t["drops"], **{
            "Algorithm": 7, "Op 1 ratio": 1, "Op 1 level": 1, "Op 1 attack": 0.001, "Op 1 decay": 0.045,
            "Op 1 sustain": 0, "Op 1 release": 0.04, "Op 1 velocity": 0.8, "Op 2 ratio": 2.76, "Op 2 level": 0.3,
            "Op 2 attack": 0.001, "Op 2 decay": 0.02, "Op 2 sustain": 0, "Op 2 release": 0.02, "Op 3 ratio": 5.4,
            "Op 3 level": 0.1, "Op 3 attack": 0.001, "Op 3 decay": 0.008, "Op 3 sustain": 0, "Op 3 release": 0.008,
            "Op 4 level": 0, "Feedback": 0.0, "Voices": 8, "Volume": 0.5})
        c["dust"] = ch("Dust", "gl.hat", C_AIR, t["dust"], **{
            "Decay": 0.012, "Tone": 11000, "Volume": 0.25, "Metal": 0.0, "Width": 1.0, "Click": 0.6, "Choke": 0})
        c["hiss"] = ch("Tape Hiss", "gl.analog", C_AIR, t["dust"], **{
            "Volume": 0.2, "Unison voices": 2, "Unison spread": 1.0, "Osc 1 wave": "Noise", "Osc 1 level": 0.5,
            "Osc 2 on": 0, "Filter mode": "HP 12", "Cutoff": 6000, "Resonance": 0, "Key tracking": 0,
            "Filter env amount": 0, "Amp attack": 2.0, "Amp sustain": 1, "Amp release": 3.0})
        # Transitions: a noise and saw riser (its filter opens inside each riser clip) and a sub impact.
        c["riser"] = ch("Riser", "gl.analog", C_FX, t["fx"], **{
            "Volume": 0.4, "Osc 1 wave": "Noise", "Osc 1 level": 0.7, "Osc 2 wave": "Saw", "Osc 2 level": 0.12,
            "Osc 2 fine": 0, "Unison voices": 3, "Unison detune": 0.3, "Unison spread": 1.0, "Filter mode": "BP 12",
            "Cutoff": 400, "Resonance": 0.35, "Key tracking": 0, "Filter env amount": 0, "Amp attack": 2.5,
            "Amp sustain": 1, "Amp release": 0.25, "Mod env target": "Pitch", "Mod env amount": 0.0})
        # Reverse swell: a soft triangle + noise chord that rises out of nothing and cuts off at the drop.
        c["swell"] = ch("Swell", "gl.analog", C_FX, t["fx"], **{
            "Volume": 0.35, "Polyphony": 8, "Unison voices": 3, "Unison detune": 0.12, "Unison spread": 0.6,
            "Osc 1 wave": "Triangle", "Osc 1 level": 0.7, "Osc 2 wave": "Saw", "Osc 2 fine": 5, "Osc 2 level": 0.2,
            "Osc 3 on": 0, "Noise": 0.18, "Filter mode": "LP 12", "Cutoff": 900, "Resonance": 0.1, "Key tracking": 0.2,
            "Filter env amount": 0, "Amp attack": 2.6, "Amp decay": 1, "Amp sustain": 1, "Amp release": 0.04})
        c["impact"] = ch("Impact", "gl.kick", C_FX, t["impact"], **{
            "Start pitch": 140, "End pitch": 32, "Sweep": 0.22, "Decay": 1.4, "Click": 0.05, "Drive": 0.1,
            "Volume": 0.6, "Key tracking": 0, "Sub": 0.3, "Noise": 0.25, "Noise decay": 0.6, "Tone": 3000})
        # Thunder: far off, a filtered noise rumble that rolls in and out (two smooth random LFOs).
        c["thunder"] = ch("Thunder", "gl.analog", C_FX, t["impact"], **{
            "Volume": 1.0, "Osc 1 wave": "Noise", "Osc 1 level": 1.0, "Osc 2 on": 0, "Filter mode": "LP 24",
            "Cutoff": 240, "Resonance": 0.05, "Key tracking": 0, "Filter env amount": 0, "Amp attack": 1.2,
            "Amp decay": 2.0, "Amp sustain": 0.7, "Amp release": 3.5, "LFO 1 shape": "Smooth random",
            "LFO 1 rate": 0.9, "LFO 1 retrigger": 1, "LFO 1 target": "Volume", "LFO 1 amount": 0.55,
            "LFO 2 shape": "Smooth random", "LFO 2 rate": 0.35, "LFO 2 retrigger": 1, "LFO 2 target": "Cutoff",
            "LFO 2 amount": 0.08})
        # ---------------------------------------------------- playlist tracks
        self.tr = {}
        for name, col in [("Drums", C_DRUM), ("Fills", C_DRUM2), ("Shaker + Ride", C_PERC), ("Sub + Reese", C_BASS),
                          ("Haze Pad", C_PAD), ("Air Pad", C_AIR), ("Felt Keys", C_KEYS), ("Graphite Mallet", C_MALLET),
                          ("Felt Pluck", C_PLUCK), ("Rain", C_AIR), ("Droplets", C_AIR), ("Dust + Hiss", C_AIR),
                          ("Risers + Impacts", C_FX), ("Swells", C_FX)]:
            self.tr[name] = S.track(name, col)
        for name, start, n in SECTIONS:
            S.marker(start, name)

    # ----------------------------------------------------------- helpers
    def hum(self, v, spread=4):
        return max(1, min(127, v + self.rnd.randint(-spread, spread)))

    def hits(self, p, ch, seq, pitch=60, length=1):
        """seq: [(step, velocity)] -> notes on channel `ch` (16th steps from the pattern start)."""
        self.S.notes(p, self.c[ch], [(s * STEP, length * STEP, pitch, v) for s, v in seq])

    def lead(self, p, ch, seq, shift=0, vel=1.0):
        """seq: [(bar, step, length in steps, note name, velocity)]"""
        self.S.notes(p, self.c[ch], [((b * 16 + s) * STEP, n * STEP, m(nm) + shift, max(1, int(v * vel)))
                                     for (b, s, n, nm, v) in seq])

    # ------------------------------------------------------------ drums
    def drums(self):
        S, P = self.S, {}
        hats8 = lambda bars, on=64, off=90: [(s, self.hum(off if s % 4 == 2 else on)) for s in range(0, 16 * bars, 2)]

        def kit(name, bars, kick, snare, ghosts, hats, ohats=(), wood=(), clap=()):
            p = S.pattern(name, bars, C_DRUM)
            if clap:
                self.hits(p, "clap", clap)
            # humanised downwards only, so no hit gets louder than written
            self.hits(p, "kick", [(s, v - self.rnd.randint(0, 5)) for s, v in kick])
            self.hits(p, "snare", [(s, v - self.rnd.randint(0, 5)) for s, v in snare] + [(s, self.hum(v, 3)) for s, v in ghosts])
            self.hits(p, "hat", hats)
            if ohats:
                self.hits(p, "ohat", ohats, length=2)
            for s, v, nm in wood:
                self.S.notes(p, self.c["wood"], [(s * STEP, STEP, m(nm), v)])
            return p

        K = [(0, 118), (10, 108), (16, 116), (26, 108)]
        SN = [(4, 112), (12, 114), (20, 112), (28, 116)]
        G = [(7, 30), (15, 34), (23, 28), (25, 30), (31, 40)]
        ghost16 = [(s, 40) for s in (3, 11, 19, 27, 29)]
        P["A"] = kit("Break A", 2, K, SN, G, hats8(2) + ghost16)
        P["chorus"] = kit("Break Chorus", 2, K, SN, G, [h for h in hats8(2) if h[0] not in (14, 30)] + ghost16,
                          ohats=[(14, 72), (30, 76)], clap=[(4, 84), (12, 88), (20, 84), (28, 92)])
        roll = [(s, self.hum([72, 40, 92, 44][s % 4])) for s in range(32)]
        P["B"] = kit("Break Rolling", 2, [(0, 118), (10, 104), (13, 66), (16, 116), (22, 92), (26, 104)], SN,
                     [(3, 26), (7, 32), (9, 26), (15, 36), (19, 28), (23, 32), (25, 26), (30, 30), (31, 42)], roll)
        P["lift"] = kit("Break Lift", 2, K + [(29, 74)], SN, G + [(9, 28)],
                        [h for h in roll if h[0] not in (14, 30)], ohats=[(14, 74), (30, 80)],
                        clap=[(4, 88), (12, 92), (20, 88), (28, 96)])
        P["half"] = kit("Break Half-time", 2, [(0, 116), (16, 112)], [(8, 104), (24, 108)], [(14, 30), (30, 36)],
                        hats8(2, 56, 80))
        P["outro"] = kit("Break Outro", 2, K, SN, [], hats8(2))
        P["kickhats"] = kit("Kick + Hats", 2, K, [], [], hats8(2))
        # fills (1 bar)
        P["fill_roll"] = kit("Fill Roll", 1, [(0, 116)], [(4, 110)] + [(s, 50 + (s - 8) * 8) for s in range(8, 16)], [],
                             [(0, 64), (2, 90), (4, 64), (6, 90)])
        P["fill_wood"] = kit("Fill Wood", 1, [(0, 116), (10, 104)], [(4, 110), (12, 112)], [(7, 30)], hats8(1)[:6],
                             wood=[(13, 92, "Eb6"), (14, 86, "Db6"), (15, 96, "Ab5")])
        P["fill_gap"] = kit("Fill Gap", 1, [(0, 116)], [(4, 110), (12, 100)], [], [(0, 64), (2, 90), (4, 64), (6, 90)])
        P["fill_flam"] = kit("Fill Flam", 1, [(0, 116), (10, 100)], [(4, 110), (12, 108), (14, 84), (15, 104)],
                             [(7, 28)], hats8(1)[:6], wood=[(11, 80, "Ab5")])
        P["fill_drop"] = kit("Fill Drop", 1, [(0, 112)], [(14, 72), (15, 98)], [], [(0, 60)])
        # perc layers
        p = S.pattern("Shaker", 1, C_PERC)
        self.hits(p, "shaker", [(s, self.hum([30, 20, 56, 24][s % 4], 3)) for s in range(16)])
        P["shaker"] = p
        p = S.pattern("Ride", 1, C_PERC)
        self.hits(p, "ride", [(0, 64), (4, 50), (6, 40), (8, 68), (12, 50), (14, 44)])
        P["ride"] = p
        return P

    # ------------------------------------------------------------- bass
    def bassline(self, name, prog, style):
        """Sub and reese play the same line. style: verse | chorus | stabs | long | home | intro | breakdown"""
        S = self.S
        bars = sum(n for _, n in prog)
        p = S.pattern(name, bars, C_BASS)
        notes = []
        segs = expand(prog)
        for k, (ch, b0, n) in enumerate(segs):
            r = m(CHORDS[ch][1])
            nxt = m(CHORDS[segs[(k + 1) % len(segs)][0]][1])
            t0 = b0 * 16
            if style == "intro":
                notes += [(t0, 8, r, 84)]
                continue
            if style == "breakdown":  # the sub returns only for the second half, softly
                if b0 >= 8:
                    notes += [(t0, n * 16 - 1, r, 50)]
                continue
            if style == "long" or (style == "home" and k < len(segs) - 1):
                notes += [(t0, n * 16 - 4, r, 100), (t0 + n * 16 - 4, 4, r + 12, 86)]
                continue
            if style == "home":  # the last note holds to the end, softer, with no pickup into nothing
                notes += [(t0, n * 16 - 1, r, 88)]
                continue
            for bb in range(n):
                s = t0 + bb * 16
                last = bb == n - 1
                if style == "stabs":
                    notes += [(s, 3, r, 104), (s + 3, 3, r, 84), (s + 10, 4, r, 98)]
                    if last:
                        notes += [(s + 14, 2, r + 12, 86)]
                elif style == "chorus":
                    if last:
                        notes += [(s, 10, r, 104), (s + 10, 4, r, 92), (s + 14, 2, r + 12 if nxt <= r else r + 7, 88)]
                    else:
                        notes += [(s, 10, r, 106), (s + 10, 6, r, 94)]
                else:  # verse: rolls with the kick (steps 0 and 10)
                    if last and n > 1:
                        notes += [(s, 10, r, 104), (s + 10, 2, r, 90), (s + 12, 4, r + 7, 88)]
                    else:
                        notes += [(s, 10, r, 104), (s + 10, 6, r, 92)]
        lst = [(st * STEP, ln * STEP, pi, v) for st, ln, pi, v in notes]
        S.notes(p, self.c["sub"], lst)
        if style not in ("intro", "breakdown"):
            S.notes(p, self.c["reese"], lst)
        return p

    # -------------------------------------------------------- harmony
    def pads(self, name, prog, vel=84, top=False, skip=0):
        p = self.S.pattern(name, sum(n for _, n in prog), C_PAD)
        ns = []
        for ch, b0, n in expand(prog):
            if b0 < skip:
                continue
            voicing = [m(x) for x in CHORDS[ch][0]]
            if top:
                voicing.append(voicing[1] + 12)
            for k, pi in enumerate(voicing):
                ns.append((b0 * BAR + k * 2, n * BAR - STEP - k * 2, pi, vel - 4 * k))
        self.S.notes(p, self.c["pad"], ns)
        return p

    def airpads(self, name, prog, vel=80):
        """High, open voicings: the chord an octave up without its lowest note, plus a ninth on top."""
        p = self.S.pattern(name, sum(n for _, n in prog), C_AIR)
        ns = []
        for ch, b0, n in expand(prog):
            v = [m(x) for x in CHORDS[ch][0]]
            tones = [x + 12 for x in v[1:]] + [v[0] + 24]
            for k, pi in enumerate(tones):
                ns.append((b0 * BAR + k * 3, n * BAR - STEP, pi, vel - 5 * k))
        self.S.notes(p, self.c["airpad"], ns)
        return p

    def keys(self, name, prog, style, shift=0):
        p = self.S.pattern(name, sum(n for _, n in prog), C_KEYS)
        ns = []
        for ch, b0, n in expand(prog):
            voicing = [m(x) + shift for x in CHORDS[ch][0]]
            for bb in range(n):
                s = (b0 + bb) * 16
                if style == "comp":      # syncopated comping, answers the kick
                    hitsl = [(s, 6, 80), (s + 6, 3, 58), (s + 10, 6, 72)] if bb % 2 == 0 else [(s + 4, 4, 60), (s + 10, 6, 70)]
                elif style == "halves":
                    hitsl = [(s, 7, 72), (s + 8, 7, 62)]
                else:  # "whole": one soft chord per chord change
                    hitsl = [(s, n * 16 - 1, 66)] if bb == 0 else []
                for st, ln, v in hitsl:
                    for k, pi in enumerate(voicing):
                        ns.append((st * STEP + k * 3, ln * STEP - k * 3, pi, v - 3 * k))
        self.S.notes(p, self.c["keys"], ns)
        return p

    def counter(self, name, prog, style):
        """Felt pluck lines built from chord tones moving in fifths."""
        p = self.S.pattern(name, sum(n for _, n in prog), C_PLUCK)
        ns = []
        for ch, b0, n in expand(prog):
            tones = [m(x) + 12 for x in CHORDS[ch][0]]
            for bb in range(n):
                s = (b0 + bb) * 16
                if style == "fifths":   # verse 2: dotted-eighth falling fifths
                    order, rhythm, vels = [3, 1, 2, 0, 2, 1], [0, 3, 6, 9, 12, 14], [92, 62, 78, 66, 82, 60]
                else:                   # chorus 2: eighths under the hook, an octave lower
                    order, rhythm, vels = [0, 2, 1, 3, 0, 2, 1, 2], [0, 2, 4, 6, 8, 10, 12, 14], [70, 50, 62, 52, 66, 48, 60, 50]
                for i, st in enumerate(rhythm):
                    pi = tones[order[(i + bb) % len(order)]] - (12 if style == "eighths" else 0)
                    ln = (rhythm[i + 1] if i + 1 < len(rhythm) else 16) - st
                    # a wide pair: successive notes alternate left and right
                    ns.append(((s + st) * STEP, max(1, ln - 1) * STEP, pi, self.hum(vels[i], 3),
                               {"pan": -22 if i % 2 == 0 else 22}))
        self.S.notes(p, self.c["pluck"], ns)
        return p

    # ------------------------------------------------------------ motif
    BLOOM = ["Ab4", "Db5", "Eb5", "F5"]   # D-flat add9 above the motif (the startup sound's bloom)
    # Chorus 2: the felt keys answer the hook, a quiet top line that moves only where the hook holds
    # (bars 1, 3, 5, 11, 13, 15 of the 16), off the comping hits on steps 0 and 8.
    ANSWER = [(1, 10, 3, "Bb4", 70), (1, 13, 3, "Db5", 76), (3, 10, 3, "Eb5", 72), (3, 13, 3, "C5", 66),
              (5, 3, 4, "C5", 68), (5, 13, 3, "Ab4", 60), (11, 10, 3, "Eb5", 72), (11, 13, 3, "C5", 66),
              (13, 10, 3, "F5", 72), (13, 13, 3, "Ab5", 76), (15, 3, 4, "Eb5", 68)]

    def mallet_parts(self):
        S, P = self.S, {}
        # Intro: the startup gesture at Graphite pitch, blooming into D-flat add9; an echo answers.
        p = S.pattern("Motif Intro", 8, C_MALLET)
        seq = [(0, 0, 3, "Db4", 92), (0, 3, 3, "Ab4", 88), (0, 6, 4, "Eb5", 90)]
        seq += [(0, 10, 22, nm, 66 - 3 * k) for k, nm in enumerate(self.BLOOM)]
        seq += [(2, 4, 4, "Eb5", 58), (2, 8, 4, "Db5", 54), (2, 12, 8, "Ab4", 52)]
        seq += [(4, 0, 3, "Db4", 92), (4, 3, 3, "Ab4", 88), (4, 6, 4, "Eb5", 92), (4, 10, 6, "F5", 80),
                (5, 0, 12, "Db5", 70), (6, 8, 4, "Ab4", 56), (6, 12, 20, "Db5", 60)]
        self.lead(p, "mallet", seq)
        P["intro"] = p
        # the very first statement enters half a bar in, after the rain and the pad have faded up
        p = S.pattern("Motif Opening", 8, C_MALLET)
        self.lead(p, "mallet", [(b + (st + 8) // 16, (st + 8) % 16, min(ln, 128 - (b * 16 + st + 8)), nm, v)
                                for b, st, ln, nm, v in seq])
        P["opening"] = p
        # Verse: only the "open" fifth as a pickup at the phrase end.
        p = S.pattern("Open Fifth", 8, C_MALLET)
        self.lead(p, "mallet", [(7, 8, 4, "Db5", 70), (7, 12, 4, "Ab5", 84)])
        P["pickup"] = p
        # Chorus hook (16 bars): the motif rising, a stepwise answer down; "task_done" fourth as the turnaround.
        hook = [(0, 0, 3, "Db5", 100), (0, 3, 3, "Ab5", 96), (0, 6, 8, "Eb6", 102), (0, 14, 2, "Db6", 84),
                (1, 0, 4, "Bb5", 90), (1, 4, 10, "Ab5", 86),
                (2, 0, 3, "Db5", 96), (2, 3, 3, "Ab5", 94), (2, 6, 6, "Eb6", 100), (2, 12, 4, "Db6", 86),
                (3, 0, 4, "C6", 92), (3, 4, 4, "Bb5", 84), (3, 8, 8, "Ab5", 86),
                (4, 0, 3, "Ab5", 94), (4, 3, 3, "G5", 84), (4, 6, 6, "F5", 90), (4, 12, 4, "Eb5", 80),
                (5, 0, 8, "F5", 88), (5, 10, 2, "Eb5", 72), (5, 12, 4, "Db5", 78),
                (6, 0, 3, "Db5", 92), (6, 3, 3, "F5", 88), (6, 6, 10, "C6", 90),
                (7, 0, 6, "Bb5", 88), (7, 6, 4, "Ab5", 80), (7, 10, 6, "F5", 78),
                (8, 0, 3, "Db5", 100), (8, 3, 3, "Ab5", 96), (8, 6, 8, "Eb6", 104), (8, 14, 2, "F6", 86),
                (9, 0, 6, "Eb6", 92), (9, 6, 4, "Db6", 86), (9, 10, 6, "Bb5", 84),
                (10, 0, 3, "Db5", 96), (10, 3, 3, "Ab5", 94), (10, 6, 6, "Eb6", 100), (10, 12, 4, "Db6", 86),
                (11, 0, 4, "C6", 90), (11, 4, 12, "Bb5", 84),
                (12, 0, 3, "Ab5", 92), (12, 3, 3, "G5", 82), (12, 6, 10, "F5", 88),
                (13, 0, 3, "F5", 88), (13, 3, 3, "Db5", 80), (13, 6, 10, "C5", 84),
                (14, 0, 4, "Bb4", 82), (14, 4, 4, "Db5", 80), (14, 8, 8, "Gb5", 86),
                (15, 8, 3, "Ab4", 78), (15, 11, 5, "Db5", 90)]
        p = S.pattern("Hook", 16, C_MALLET)
        self.lead(p, "mallet", hook)
        P["hook"] = p
        p = S.pattern("Hook Lift", 16, C_MALLET)
        self.lead(p, "mallet", hook)
        P["hook_lift"] = p
        # Bridge: the motif climbs, the motif shape stacked on each chord root.
        p = S.pattern("Motif Climb", 16, C_MALLET)
        seq = []
        for k, (ch, b0, n) in enumerate(expand(PROG["bridge"])):
            root = {"Ebm11": "Eb4", "Fm9": "F4", "Gbmaj9": "Gb4", "Ab": "Ab4", "Ab7sus": "Ab4"}[ch]
            r = m(root)
            v = 80 if k < 4 else 92
            seq += [(b0 * 16, 3, r, v), (b0 * 16 + 3, 3, r + 7, v - 4), (b0 * 16 + 6, 10, r + 14, v + 2)]
            if k >= 4:
                seq += [(b0 * 16 + 16, 4, r + 21, v - 10), (b0 * 16 + 20, 8, r + 19, v - 14)]
        S.notes(p, self.c["mallet"], [(s * STEP, n * STEP, pi, v) for s, n, pi, v in seq])
        P["climb"] = p
        # Breakdown: the motif at half speed, solo, with long tails.
        p = S.pattern("Motif Slow", 16, C_MALLET)
        self.lead(p, "mallet", [
            (0, 0, 6, "Db5", 86), (0, 6, 6, "Ab5", 82), (0, 12, 20, "Eb6", 88),
            (2, 0, 6, "C6", 72), (2, 6, 6, "Ab5", 68), (2, 12, 20, "F5", 70),
            (4, 0, 6, "Db5", 86), (4, 6, 6, "Ab5", 84), (4, 12, 20, "Eb6", 90),
            (6, 0, 6, "Db6", 74), (6, 6, 6, "Bb5", 70), (6, 12, 20, "F5", 72),
            (8, 0, 6, "Db5", 88), (8, 6, 6, "Ab5", 86), (8, 12, 6, "Eb6", 92), (9, 2, 14, "F6", 80),
            (10, 0, 30, "Ab5", 58), (10, 0, 30, "Db6", 56), (10, 0, 30, "Eb6", 54),
            (12, 0, 16, "Eb6", 80), (13, 0, 8, "Db6", 74), (13, 8, 8, "C6", 70), (14, 0, 16, "Bb5", 72),
            (15, 8, 4, "Db5", 76), (15, 12, 4, "Ab5", 88)])
        P["slow"] = p
        # Outro: "task_done" fourth, then the full motif and bloom; the "save" dyad is the last colour.
        p = S.pattern("Motif Home", 16, C_MALLET)
        self.lead(p, "mallet", [h for h in hook if h[0] < 4] + [
            (8, 0, 3, "Ab4", 80), (8, 3, 13, "Db5", 88),
            (12, 0, 3, "Db4", 92), (12, 3, 3, "Ab4", 88), (12, 6, 4, "Eb5", 92)] +
            [(12, 10, 22, nm, 70 - 3 * k) for k, nm in enumerate(self.BLOOM)] +
            [(14, 8, 24, "Ab4", 58), (14, 8, 24, "Db5", 62)])
        P["home"] = p
        return P

    # ------------------------------------------------------------- fx
    def fx_parts(self):
        S, P = self.S, {}
        cat = self.cat
        ci = lambda name: 100 + cat.index("gl.analog", name)
        for bars in (4, 2):
            p = S.pattern(f"Riser {bars}", bars, C_FX)
            S.notes(p, self.c["riser"], [(0, bars * BAR - STEP, 60, 100)])
            L = bars * BAR
            S.pattern_auto(p, self.c["riser"]["id"], ci("Cutoff"),
                           [(0, norm(cat, "gl.analog", "Cutoff", 300), 0, 0),
                            (L - STEP, norm(cat, "gl.analog", "Cutoff", 9000), 0, 0)])
            S.pattern_auto(p, self.c["riser"]["id"], ci("Unison detune"), [(0, 0.08, 0, 0), (L - STEP, 0.6, 0, 0)])
            P[f"riser{bars}"] = p
        # reverse swells: the next section's first chord (Gbmaj9 for the choruses), two bars long
        p = S.pattern("Swell", 2, C_FX)
        S.notes(p, self.c["swell"], [(0, 2 * BAR - STEP // 2, m(x) + 12, 80) for x in CHORDS["Gbmaj9"][0]])
        S.pattern_auto(p, self.c["swell"]["id"], ci("Cutoff"),
                       [(0, norm(cat, "gl.analog", "Cutoff", 500), 0, 0),
                        (2 * BAR - STEP, norm(cat, "gl.analog", "Cutoff", 6500), 0, 0)])
        P["swell"] = p
        p = S.pattern("Impact", 1, C_FX)
        S.notes(p, self.c["impact"], [(0, STEP, 60, 110)])
        P["impact"] = p
        # Weather
        p = S.pattern("Drizzle", 16, C_AIR)
        S.notes(p, self.c["rain"], [(0, 16 * BAR - 2 * STEP, 60, 96)])
        P["rain"] = p
        p = S.pattern("Dust + Hiss", 16, C_AIR)
        S.notes(p, self.c["hiss"], [(0, 16 * BAR - 2 * STEP, 60, 90)])
        P["hiss"] = p
        pent = [m(x) for x in ("Db6", "Eb6", "F6", "Ab6", "Bb6", "Db7", "Eb7")]
        for name, dens in (("Droplets", 7), ("Droplets Light", 3)):
            p = S.pattern(name, 8, C_AIR)
            ns = []
            for bar in range(8):
                for _ in range(dens + self.rnd.randint(-1, 2)):
                    ns.append((bar * BAR + self.rnd.randrange(0, BAR - STEP // 2), STEP // 2, self.rnd.choice(pent),
                               self.rnd.randint(14, 46), {"pan": self.rnd.randint(-48, 48)}))
            if dens > 4:
                run = ["Eb7", "Db7", "Bb6", "Ab6", "F6"]
                ns += [(5 * BAR + 8 * STEP + k * (STEP + 9), STEP // 2, m(nm), 44 - 5 * k, {"pan": -60 + 30 * k})
                       for k, nm in enumerate(run)]
            S.notes(p, self.c["drops"], sorted(ns, key=lambda n: n[0]))
            P["drops" if dens > 4 else "drops_light"] = p
        dust = sorted([(self.rnd.randrange(0, 4 * BAR), STEP // 2, 60, self.rnd.randint(18, 64)) for _ in range(46)])
        S.notes(P["hiss"], self.c["dust"], [(t + k * 4 * BAR, ln, pi, v) for k in range(4) for t, ln, pi, v in dust])
        p = S.pattern("Distant Thunder", 8, C_FX)
        S.notes(p, self.c["thunder"], [(0, 5 * BAR, 60, 120), (5 * BAR + 6 * STEP, 2 * BAR, 60, 90)])
        S.pattern_auto(p, self.c["thunder"]["id"], ci("Cutoff"),
                       [(0, norm(cat, "gl.analog", "Cutoff", 200), 0, 0),
                        (2 * BAR, norm(cat, "gl.analog", "Cutoff", 520), 0, 0),
                        (8 * BAR - STEP, norm(cat, "gl.analog", "Cutoff", 140), 0, 0)])
        P["thunder"] = p
        p = S.pattern("Last Kick", 1, C_DRUM)
        self.hits(p, "kick", [(0, 112)])
        P["lastkick"] = p
        p = S.pattern("Shaker + Ride", 1, C_PERC)
        self.hits(p, "shaker", [(s, self.hum([30, 20, 56, 24][s % 4], 3)) for s in range(16)])
        self.hits(p, "ride", [(0, 64), (4, 50), (6, 40), (8, 68), (12, 50), (14, 44)])
        P["shride"] = p
        return P

    # ------------------------------------------------------ arrangement
    def place(self, p, track, bar, bars=None):
        self.S.clip(p, self.tr[track], bar, bars)

    def run(self, p, track, start, end, fills=None, fill_track="Fills"):
        """Repeats pattern p from bar `start` to `end`; bars listed in `fills` play that fill instead."""
        fills = fills or {}
        b = start
        while b < end:
            n = min(p["bars"], end - b)
            for k in range(n):
                if b + k in fills:
                    n = k
                    break
            if n > 0:
                self.place(p, track, b, n)
                b += n
            if b in fills:
                self.place(fills[b], fill_track, b, 1)
                b += 1

    def arrange(self):
        D = self.drums()
        L = self.mallet_parts()
        F = self.fx_parts()
        pr = PROG
        bass = {
            "intro": self.bassline("Sub Intro", pr["chorus"], "intro"),
            "verse": self.bassline("Bass Verse", pr["verse"], "verse"),
            "chorus": self.bassline("Bass Chorus", pr["chorus"], "chorus"),
            "stabs": self.bassline("Bass Stabs", pr["verse"], "stabs"),
            "bridge": self.bassline("Bass Bridge", pr["bridge"], "long"),
            "breakdown": self.bassline("Sub Breakdown", pr["breakdown"], "breakdown"),
            "outro1": self.bassline("Bass Outro", pr["outro"][:4], "chorus"),
            "outro2": self.bassline("Bass Home", pr["outro"][4:], "home"),
        }
        pads = {
            "intro": self.pads("Pad Intro", pr["introA"], vel=78),
            "chorus": self.pads("Pad Chorus", pr["chorus"]),
            "chorus_top": self.pads("Pad Chorus Lift", pr["chorus"], top=True),
            "verse": self.pads("Pad Verse", pr["verse"], vel=76),
            "verse2": self.pads("Pad Verse 2", pr["verse"], vel=74, skip=4),
            "bridge": self.pads("Pad Bridge", pr["bridge"], vel=80),
            "breakdown": self.pads("Pad Breakdown", pr["breakdown"], vel=74, top=True),
            "outro": self.pads("Pad Outro", pr["outro"], vel=78),
        }
        keys = {
            "intro": self.keys("Keys Intro", pr["introA"], "whole"),
            "introB": self.keys("Keys Intro B", pr["chorus"], "halves"),
            "verse": self.keys("Keys Verse", pr["verse"], "comp", shift=12),
            "chorus": self.keys("Keys Chorus", pr["chorus"], "halves"),
            "chorus2": self.keys("Keys Chorus Answer", pr["chorus"], "halves"),
            "chorus_hint": self.keys("Keys Chorus Hint", pr["chorus"], "halves"),
            "bridge": self.keys("Keys Bridge", pr["bridge"], "halves"),
            "breakdown": self.keys("Keys Breakdown", pr["breakdown"], "whole", shift=12),
            "verse2": self.keys("Keys Verse 2", pr["verse"][2:], "whole", shift=12),
            "outro": self.keys("Keys Outro", pr["outro"], "whole"),
        }
        self.lead(keys["chorus2"], "keys", self.ANSWER)
        self.lead(keys["chorus_hint"], "keys", [a for a in self.ANSWER if a[0] >= 8], vel=0.8)
        plucks = {"verse2": self.counter("Pluck Fifths", pr["verse"], "fifths"),
                  "chorus2": self.counter("Pluck Eighths", pr["chorus"], "eighths")}

        airpads = {
            "intro": self.airpads("Air Pad Intro", pr["introA"]),
            "chorus": self.airpads("Air Pad Chorus", pr["chorus"], vel=70),
            "breakdown": self.airpads("Air Pad Breakdown", pr["breakdown"]),
            "outro": self.airpads("Air Pad Outro", pr["outro"]),
        }

        # Weather: drizzle, droplets, dust and hiss run through the whole song (the Weather Bus
        # automation lifts them in the quiet sections and tucks them under the choruses).
        self.run(F["rain"], "Rain", 0, SONG_BARS)
        self.run(F["hiss"], "Dust + Hiss", 0, SONG_BARS)
        for a, b in ((0, 32), (128, 144), (176, SONG_BARS)):
            self.run(F["drops"], "Droplets", a, b)
        for a, b in ((32, 64), (96, 128)):
            self.run(F["drops_light"], "Droplets", a, b)
        # Air pad: the airy sections (intro, breakdown, outro) open up above everything.
        self.place(airpads["intro"], "Air Pad", 0)
        self.place(airpads["chorus"], "Air Pad", 16)
        self.place(airpads["breakdown"], "Air Pad", 128)
        self.place(airpads["outro"], "Air Pad", 176)
        # --- Intro (0-31)
        self.place(pads["intro"], "Haze Pad", 0)
        self.place(L["opening"], "Graphite Mallet", 0)
        self.place(L["intro"], "Graphite Mallet", 8)
        self.place(keys["intro"], "Felt Keys", 8, 8)
        self.place(pads["chorus"], "Haze Pad", 16)
        self.place(keys["introB"], "Felt Keys", 16)
        self.place(L["intro"], "Graphite Mallet", 16)
        self.place(L["intro"], "Graphite Mallet", 24)
        self.place(bass["intro"], "Sub + Reese", 16)
        self.run(D["A"], "Drums", 16, 32, fills={31: D["fill_roll"]})
        self.place(F["riser4"], "Risers + Impacts", 28)
        # --- Verse (32-63)
        self.place(F["impact"], "Risers + Impacts", 32)
        self.run(D["A"], "Drums", 32, 64, fills={47: D["fill_wood"], 63: D["fill_gap"]})
        self.run(D["shaker"], "Shaker + Ride", 48, 64)
        for b in (32, 48):
            self.place(bass["verse"], "Sub + Reese", b)
            self.place(pads["verse"], "Haze Pad", b)
            self.place(keys["verse"], "Felt Keys", b)
        for b in (32, 40, 48, 56):
            self.place(L["pickup"], "Graphite Mallet", b)
        self.place(F["riser4"], "Risers + Impacts", 60)
        self.place(F["swell"], "Swells", 62)
        # --- Chorus (64-95)
        self.place(F["impact"], "Risers + Impacts", 64)
        self.run(D["chorus"], "Drums", 64, 96, fills={79: D["fill_wood"], 95: D["fill_gap"]})
        self.run(D["shaker"], "Shaker + Ride", 64, 96)
        for b in (64, 80):
            self.place(bass["chorus"], "Sub + Reese", b)
            self.place(pads["chorus"], "Haze Pad", b)
            self.place(keys["chorus"] if b == 64 else keys["chorus_hint"], "Felt Keys", b)
            self.place(L["hook"], "Graphite Mallet", b)
        # --- Verse 2 (96-111)
        self.run(D["B"], "Drums", 96, 112, fills={111: D["fill_flam"]})
        self.run(D["shaker"], "Shaker + Ride", 96, 112)
        self.place(bass["stabs"], "Sub + Reese", 96)
        self.place(pads["verse2"], "Haze Pad", 96)
        self.place(plucks["verse2"], "Felt Pluck", 96)
        self.place(keys["verse2"], "Felt Keys", 104)
        # --- Bridge (112-127)
        self.place(F["impact"], "Risers + Impacts", 112)
        self.run(D["half"], "Drums", 112, 120)
        self.run(D["B"], "Drums", 120, 128, fills={127: D["fill_gap"]})
        self.run(F["shride"], "Shaker + Ride", 120, 127)
        self.place(bass["bridge"], "Sub + Reese", 112)
        self.place(pads["bridge"], "Haze Pad", 112)
        self.place(keys["bridge"], "Felt Keys", 112)
        self.place(L["climb"], "Graphite Mallet", 112)
        self.place(F["riser4"], "Risers + Impacts", 124)
        # --- Breakdown (128-143)
        self.place(F["impact"], "Risers + Impacts", 128)
        self.place(bass["breakdown"], "Sub + Reese", 128)
        self.place(pads["breakdown"], "Haze Pad", 128)
        self.place(keys["breakdown"], "Felt Keys", 128)
        self.place(L["slow"], "Graphite Mallet", 128)
        self.place(F["thunder"], "Risers + Impacts", 130)
        self.run(D["A"], "Drums", 140, 144, fills={143: D["fill_drop"]})
        self.place(F["riser4"], "Risers + Impacts", 140)
        self.place(F["swell"], "Swells", 142)
        # --- Chorus 2 (144-175)
        self.place(F["impact"], "Risers + Impacts", 144)
        self.run(D["lift"], "Drums", 144, 176, fills={159: D["fill_wood"], 175: D["fill_flam"]})
        self.run(F["shride"], "Shaker + Ride", 144, 176)
        for b in (144, 160):
            self.place(bass["chorus"], "Sub + Reese", b)
            self.place(pads["chorus_top"], "Haze Pad", b)
            self.place(keys["chorus2"], "Felt Keys", b)
            self.place(L["hook_lift"], "Graphite Mallet", b)
            self.place(plucks["chorus2"], "Felt Pluck", b)
        self.place(F["riser2"], "Risers + Impacts", 174)
        # --- Outro (176-191)
        self.place(F["impact"], "Risers + Impacts", 176)
        self.run(D["outro"], "Drums", 176, 184)
        self.run(D["kickhats"], "Drums", 184, 190)
        self.place(F["lastkick"], "Drums", 190)
        self.run(D["shaker"], "Shaker + Ride", 176, 184)
        self.place(bass["outro1"], "Sub + Reese", 176)
        self.place(bass["outro2"], "Sub + Reese", 182)
        self.place(pads["outro"], "Haze Pad", 176)
        self.place(keys["outro"], "Felt Keys", 176)
        self.place(L["home"], "Graphite Mallet", 176)

    # ------------------------------------------------------------- mix
    def eq(self, track, slot, bands, mix=1.0):
        """bands: [(band 1..7, type, freq, gain[, order])]; order e.g. "Steep 4" for a steeper cut."""
        vals = {}
        for k in range(1, 8):
            vals[f"Band {k} gain"] = 0
        for b in bands:
            k, ty, f, g = b[:4]
            vals[f"Band {k} type"] = ty
            vals[f"Band {k} freq"] = f
            vals[f"Band {k} gain"] = g
            if len(b) > 4:
                vals[f"Band {k} order"] = b[4]
        return self.S.fx(track, slot, "gl.eq", mix=mix, **vals)

    def mix(self):
        S, t = self.S, self.t
        # Gain staging: every channel is trimmed so its mixer track peaks around -18 to -12 dBFS before the
        # fader; the faders then set the balance (kick and snare lead, bass just under, the lead clear but not
        # on top, pads and weather well back) and the master chain brings the level up.
        for ch in self.c.values():
            ch["gain"] = ch["gain"] * 10 ** (TRIM / 40)
        db = {"kick": 0, "snare": 7.7, "clap": -1.5, "hats": 6.5, "perc": 1.5, "wood": -2, "drum_bus": 0, "sub": -1.5,
              "reese": -0.5, "bass_bus": 0, "pad": -3, "airpad": -4, "keys": -3, "mallet": -3.5, "pluck": -0.5,
              "music_bus": 0, "atmos_bus": 0, "rain": 0, "drops": -4, "dust": -6, "fx": -2, "impact": -5, "hall": 0,
              "echo": -2, "space": -1.5}
        for k, v in db.items():
            t[k]["volume"] = fader(v, 0.8)
        # --- drums
        self.eq(t["kick"], 0, [(1, "Low cut", 28, 0), (2, "Peak", 200, -4.5), (3, "Peak", 380, -2.5),
                               (6, "Peak", 3800, 1.5)])
        self.eq(t["snare"], 0, [(1, "Low cut", 120, 0), (2, "Peak", 200, 0.5), (3, "Peak", 310, -2.5), (4, "Peak", 650, -2.0),
                                (5, "Peak", 3200, 2.0), (6, "High shelf", 8000, -3.0), (7, "High cut", 12000, 0)])
        S.fx(t["kick"], 1, "gl.limiter", **{"Gain": 0, "Ceiling": -14.0, "Release": 0.04})
        S.fx(t["snare"], 1, "gl.limiter", **{"Gain": 0, "Ceiling": -19.0, "Release": 0.03,
                                             "Saturation threshold": -2.0})
        self.eq(t["clap"], 0, [(1, "Low cut", 300, 0), (7, "High shelf", 8000, -5.0)])
        self.eq(t["hats"], 0, [(1, "Low cut", 450, 0), (5, "Peak", 5200, -1.5), (6, "Peak", 9500, -2.0),
                               (7, "High shelf", 12000, -2.0)])
        self.eq(t["perc"], 0, [(1, "Low cut", 500, 0), (7, "High shelf", 11000, -3.0)])
        self.eq(t["wood"], 0, [(1, "Low cut", 300, 0)])
        S.fx(t["drum_bus"], 0, "gl.filter", **{"Mode": "Low pass", "Cutoff": 20000, "Resonance": 0.12,
                                              "Slope": "24 dB/oct"})
        self.eq(t["drum_bus"], 1, [(1, "Low cut", 30, 0), (3, "Peak", 380, -1.5), (6, "Peak", 7000, -1.0)])
        S.fx(t["drum_bus"], 2, "gl.comp", **{"Threshold": -26, "Ratio": 2.5, "Attack": 0.012, "Release": 0.12, "Knee": 6,
                                            "Detector": "RMS", "Mix": 0.8, "Character": "Vintage", "Output limiter": 0})
        S.fx(t["drum_bus"], 3, "gl.limiter", **{"Gain": 0, "Ceiling": -15.5, "Release": 0.05,
                                                "Saturation threshold": -2.5})
        # --- bass: the sub stays mono below 140 Hz, the reese lives above it
        S.fx(t["sub"], 0, "gl.stereo", **{"Width": 0})
        self.eq(t["sub"], 1, [(7, "High cut", 140, 0)])
        self.eq(t["reese"], 0, [(1, "Low cut", 130, 0), (2, "Peak", 200, -2.0), (3, "Peak", 310, -4.0), (4, "Peak", 900, -3.0), (6, "Peak", 2400, 1.0),
                                (7, "High cut", 9000, 0)])
        S.fx(t["reese"], 1, "gl.dist", mix=0.35, **{"Mode": "Tube", "Drive": 16, "Tone": 5000, "Output": -8})
        S.fx(t["reese"], 2, "gl.chorus", **{"Mix": 0.3, "Depth": 0.4, "LFO 1 rate": 0.25})
        S.fx(t["reese"], 3, "gl.stereo", **{"Mono below": 180, "Width": 1.15})
        # the unison saws line up at every note start: round those spikes off so the reese sits at a steady level
        S.fx(t["reese"], 4, "gl.limiter", **{"Gain": 0, "Ceiling": -14.5, "Release": 0.06,
                                             "Saturation threshold": -3.0})
        S.fx(t["bass_bus"], 0, "gl.scduck", **{"Threshold": -40, "Ratio": 6, "Attack": 0.002, "Release": 0.14,
                                              "Range": 7, "Key": "Sidechain input"})
        self.eq(t["bass_bus"], 2, [(3, "Peak", 260, -2.0)])
        S.fx(t["bass_bus"], 1, "gl.comp", **{"Threshold": -20, "Ratio": 2, "Attack": 0.02, "Release": 0.15, "Knee": 6,
                                            "Output limiter": 0})
        # --- music: damp and wide, low-mids kept clean
        self.eq(t["pad"], 0, [(1, "Low cut", 240, 0, "Steep 4"), (3, "Peak", 400, -2.5), (7, "High shelf", 9000, -2.0)])
        S.fx(t["pad"], 1, "gl.stereo", **{"Width": 1.2})
        self.eq(t["airpad"], 0, [(1, "Low cut", 380, 0), (7, "High shelf", 12000, -1.5)])
        S.fx(t["airpad"], 1, "gl.hall", **{"Dry level": 0, "Wet level": -7, "Decay": 6.0, "Size": 0.9, "Pre-delay": 40,
                                          "Input low cut": 500, "Input high cut": 9000, "Shimmer on": 1,
                                          "Shimmer pitch": 12, "Shimmer gain": 0.32, "Mod depth": 0.35, "Width": 1.0})
        self.eq(t["keys"], 0, [(1, "Low cut", 260, 0, "Steep 4"), (3, "Peak", 380, -4.0)])
        S.fx(t["keys"], 1, "gl.chorus", **{"Mix": 0.18, "Depth": 0.35, "LFO 1 rate": 0.3})
        self.eq(t["mallet"], 0, [(1, "Low cut", 180, 0), (4, "Peak", 700, -1.5), (6, "Peak", 2500, 2.0)])
        S.fx(t["mallet"], 2, "gl.comp", **{"Threshold": -24, "Ratio": 3, "Attack": 0.0015, "Release": 0.09, "Knee": 6,
                                          "Output limiter": 0})
        S.fx(t["mallet"], 1, "gl.hall", **{"Dry level": 0, "Wet level": -15, "Decay": 0.6, "Size": 0.25,
                                          "Pre-delay": 4, "Input low cut": 200, "Input high cut": 7000, "Width": 1.0})
        self.eq(t["pluck"], 0, [(1, "Low cut", 300, 0, "Steep 4"), (7, "High shelf", 7000, -3.0)])
        S.fx(t["music_bus"], 0, "gl.scduck", **{"Threshold": -40, "Ratio": 3, "Attack": 0.004, "Release": 0.18,
                                               "Range": 3, "Key": "Sidechain input"})
        S.fx(t["music_bus"], 1, "gl.filter", **{"Mode": "Low pass", "Cutoff": 20000, "Resonance": 0.1,
                                               "Slope": "12 dB/oct"})
        self.eq(t["music_bus"], 2, [(3, "Peak", 300, -2.0), (4, "Peak", 950, -2.5), (6, "Peak", 2800, 2.0),
                                    (7, "High shelf", 4500, 1.5)])
        S.fx(t["music_bus"], 3, "gl.dist", mix=0.15, **{"Mode": "Tube", "Drive": 14, "Tone": 9000, "Output": -6})
        S.fx(t["music_bus"], 4, "gl.comp", **{"Threshold": -28, "Ratio": 1.5, "Attack": 0.03, "Release": 0.25,
                                             "Knee": 8, "Detector": "RMS", "Output limiter": 0})
        S.fx(t["music_bus"], 5, "gl.stereo", **{"Mono below": 120})
        # --- weather: band-limited so it never muddies the low-mids
        self.eq(t["rain"], 0, [(1, "Low cut", 650, 0), (7, "High cut", 9500, 0)])
        S.fx(t["rain"], 1, "gl.stereo", **{"Width": 1.1})
        self.eq(t["drops"], 0, [(1, "Low cut", 1000, 0), (7, "High shelf", 9000, -3.0)])
        self.eq(t["dust"], 0, [(1, "Low cut", 1500, 0), (7, "High cut", 12000, 0)])
        self.eq(t["atmos_bus"], 0, [(1, "Low cut", 400, 0), (7, "High shelf", 10000, -2.0)])
        self.eq(t["fx"], 0, [(1, "Low cut", 150, 0)])
        self.eq(t["impact"], 0, [(1, "Low cut", 30, 0), (7, "High shelf", 6000, -4.0)])
        # --- returns: long, dark and diffuse; high-passed so the tails stay out of the low-mids
        S.fx(t["hall"], 0, "gl.hall", **{"Dry level": -60, "Wet level": 0, "Decay": 3.2, "Size": 0.75, "Pre-delay": 22,
                                         "Input low cut": 280, "Input high cut": 7500, "Mod depth": 0.3, "Width": 1.05,
                                         "High decay": 0.4})
        self.eq(t["hall"], 1, [(1, "Low cut", 300, 0, "Steep 4"), (7, "High cut", 7000, 0)])
        S.fx(t["echo"], 0, "gl.delay", **{"Delay model": "Legacy", "Tempo sync": 1, "Time": "1/8.", "Feedback": 0.38,
                                          "Feedback mode": "Normal", "Feedback cutoff": 2600, "Feedback low cut": 350,
                                          "Dry": 0, "Wet": 1, "Mod depth": 0.15, "Mod rate": 0.4})
        self.eq(t["echo"], 1, [(1, "Low cut", 320, 0, "Steep 4"), (7, "High cut", 6500, 0)])
        S.fx(t["space"], 0, "gl.hall", **{"Dry level": -60, "Wet level": 0, "Decay": 8.0, "Size": 0.95, "Pre-delay": 60,
                                          "Input low cut": 350, "Input high cut": 6500, "Mod depth": 0.4, "Width": 1.1,
                                          "High decay": 0.35})
        self.eq(t["space"], 1, [(1, "Low cut", 350, 0, "Steep 4"), (7, "High cut", 6000, 0)])
        S.send(t["echo"], t["hall"], -10)
        # sends
        S.send(t["snare"], t["hall"], -14)
        S.send(t["snare"], t["space"], -60)   # automated: throws at the ends of sections
        S.send(t["clap"], t["hall"], -10)
        S.send(t["wood"], t["hall"], -10)
        S.send(t["wood"], t["echo"], -12)
        S.send(t["keys"], t["hall"], -11)
        S.send(t["keys"], t["space"], -40)    # automated: the keys drift into the long space in the quiet parts
        S.send(t["mallet"], t["hall"], -10)
        S.send(t["mallet"], t["echo"], -20)   # automated throws
        S.send(t["mallet"], t["space"], -14)  # automated per section
        S.send(t["pluck"], t["echo"], -12)
        S.send(t["pluck"], t["hall"], -14)
        S.send(t["pad"], t["hall"], -14)
        S.send(t["drops"], t["space"], -10)
        S.send(t["drops"], t["hall"], -12)
        S.send(t["fx"], t["space"], -8)
        S.send(t["impact"], t["space"], -14)
        S.send(t["kick"], t["bass_bus"], sidechain=True)
        S.send(t["kick"], t["music_bus"], sidechain=True)
        # --- master: gentle
        self.eq(S.master, 0, [(1, "Low cut", 24, 0), (2, "Low shelf", 80, -0.8), (3, "Peak", 260, -1.0), (6, "Peak", 2500, 1.5),
                              (7, "High shelf", 9000, 0.8)])
        S.fx(S.master, 1, "gl.comp", **{"Threshold": -18, "Ratio": 1.5, "Attack": 0.03, "Release": 0.25, "Knee": 8,
                                       "Detector": "RMS", "Output limiter": 0})
        # loudness in stages: an oversampled soft clipper shaves only the tallest transient tips, then the
        # limiter takes the rest with a few dB at most
        S.fx(S.master, 2, "gl.clipper", **{"Input gain": CLIP_GAIN, "Threshold": -1.8, "Ceiling": "At threshold",
                                          "Oversampling": "4x"})
        S.fx(S.master, 3, "gl.limiter", **{"Gain": LIMIT_GAIN, "Ceiling": -1.2, "Release": 0.08})
        # the limiter has no oversampling: a steep 17 kHz high cut and a last 4x oversampled safety clipper keep
        # the true peak under -1 dBTP (it only touches the inter-sample overs the limiter leaves)
        self.eq(S.master, 4, [(7, "High cut", 17000, 0, "Steep 4")])
        S.fx(S.master, 5, "gl.clipper", **{"Input gain": 0, "Threshold": -0.3, "Ceiling": "At threshold",
                                          "Oversampling": "4x"})

    # ------------------------------------------------------- automation
    def automate(self):
        """Every lane is one automation clip spanning the song; points are (bar, value, shape)."""
        S, cat, t = self.S, self.cat, self.t
        H, SM, LIN = 2, 4, 0  # point shapes: hold (jump), smooth, curve with tension 0 (linear)
        self.lanes = []

        def lane(name, color, owner, idx, pts):
            S.automation(name, color, S.track(name, color), owner, idx,
                         [(b, min(1.0, max(0.0, v)), 0, sh) for (b, v, sh) in pts], 0, SONG_BARS)
            self.lanes.append(name)

        def chp(ch, name):
            ty = self.c[ch]["plugin"]["type"]
            return self.c[ch]["id"], 100 + cat.index(ty, name), (lambda v, ty=ty, name=name: self.nv(ty, name, v))

        def slp(track, slot, name):
            ty = t[track]["slots"][slot]["plugin"]["type"]
            return t[track]["id"], 1000 + slot * 1000 + cat.index(ty, name), (lambda v, ty=ty, name=name: self.nv(ty, name, v))

        def fdr(track):  # fader in dB -> normalized
            return t[track]["id"], 0, (lambda d: fader(d, 0.8) / 1.25)

        def snd(src, dst):  # send level in dB -> normalized
            return t[src]["id"], 200 + S.mixer.index(t[dst]), (lambda d: (10 ** (d / 20)) / 1.25)

        def put(name, color, target, pts):
            owner, idx, f = target
            lane(name, color, owner, idx, [(b, f(v), sh) for (b, v, sh) in pts])

        # Drum bus low-pass: the intro break opens up; the breakdown drums return filtered; the outro closes.
        put("Drum Filter", C_DRUM2, slp("drum_bus", 0, "Cutoff"), [
            (0, 500, LIN), (16, 500, LIN), (31.75, 20000, SM), (94, 20000, LIN), (95.5, 3500, SM), (96, 20000, H),
            (139.99, 20000, LIN), (140, 380, H), (143.75, 20000, SM), (186, 20000, LIN), (192, 900, SM)])
        # Music bus low-pass: blurred top in the quiet parts, a dip into every drop, open in the choruses.
        put("Music Filter", C_PAD, slp("music_bus", 1, "Cutoff"), [
            (0, 5500, LIN), (16, 9000, SM), (31, 12000, SM), (31.9, 2200, SM), (32, 14000, H), (63, 14000, LIN),
            (63.9, 1800, SM), (64, 20000, H), (95, 20000, LIN), (96, 11000, H), (112, 11000, LIN), (127.5, 16000, SM),
            (128, 5500, H), (140, 9000, SM), (143.9, 2000, SM), (144, 20000, H), (176, 14000, H), (192, 3200, SM)])
        # Reese: cutoff, unison detune and drive move with the sections (and its LFOs keep it alive inside them).
        put("Reese Cutoff", C_BASS, chp("reese", "Cutoff"), [
            (0, 700, LIN), (32, 800, SM), (40, 980, SM), (48, 860, SM), (56, 1050, SM), (62, 1000, SM), (63, 1300, SM),
            (64, 1800, H), (79, 2100, SM), (80, 1900, H), (95, 2300, SM),
            (96, 950, SM), (104, 1150, SM), (111, 1000, SM), (112, 900, H), (127.5, 2800, SM), (144, 2000, H), (159, 2400, SM), (160, 2200, H),
            (175, 2600, SM), (176, 1500, H), (191, 300, SM), (192, 300, LIN)])
        put("Reese Detune", C_BASS, chp("reese", "Unison detune"), [
            (0, 0.12, LIN), (32, 0.12, SM), (44, 0.17, SM), (52, 0.13, SM), (63, 0.19, SM), (64, 0.2, H),
            (96, 0.1, SM), (104, 0.15, SM), (111.5, 0.1, SM), (112, 0.1, H), (127.5, 0.3, SM), (144, 0.24, H),
            (175, 0.3, SM), (176, 0.26, H), (192, 0.08, SM)])
        put("Reese Drive", C_BASS, chp("reese", "Filter drive"), [
            (0, 0.2, LIN), (64, 0.35, H), (96, 0.15, H), (112, 0.2, H), (127.5, 0.45, SM), (144, 0.4, H),
            (176, 0.3, H), (192, 0.1, SM)])
        # Reese width: focused in the verses, narrowed into each drop, thrown open on the impact.
        put("Reese Width", C_BASS, slp("reese", 3, "Width"), [
            (0, 0.9, LIN), (32, 0.9, H), (62, 0.9, LIN), (63.9, 0.55, SM), (64, 1.3, H), (66, 1.15, SM),
            (96, 0.85, H), (112, 0.95, H), (127, 1.1, SM), (127.9, 0.6, SM), (144, 1.3, H), (146, 1.2, SM),
            (176, 1.0, H), (192, 0.8, SM)])
        # Sidechain depth: deeper pump in the choruses, none in the breakdown.
        put("Bass Duck Depth", C_BASS, slp("bass_bus", 0, "Range"), [
            (0, 2, H), (32, 5, H), (64, 8, H), (96, 5, H), (112, 4, H), (127, 6, SM), (128, 0, H), (144, 8.5, H),
            (176, 5, H)])
        put("Music Duck Depth", C_PAD, slp("music_bus", 0, "Range"), [
            (0, 1, H), (32, 1.5, H), (64, 3, H), (96, 1.5, H), (112, 2, H), (128, 0, H), (144, 3, H), (176, 1.5, H)])
        # Pads: brightness and swells into the drops.
        put("Pad Cutoff", C_PAD, chp("pad", "Cutoff"), [
            (0, 600, LIN), (16, 1400, SM), (32, 1600, SM), (64, 2300, H), (80, 2300, SM), (95.5, 2900, SM),
            (96, 1400, H), (112, 1500, H),
            (127.5, 2800, SM), (128, 1100, H), (143.75, 3200, SM), (144, 2800, H), (160, 2800, SM), (175.5, 3400, SM), (176, 2000, H),
            (192, 600, SM)])
        put("Pad Swell", C_PAD, fdr("pad"), [
            (0, -3, LIN), (60, -4, H), (63.9, -0.5, SM), (64, -3, H), (124, -4, H), (127.9, -0.5, SM), (128, -3.5, H),
            (140, -4, H), (143.9, -1, SM), (144, -3, H)])
        put("Pad Width", C_PAD, slp("pad", 1, "Width"), [
            (0, 1.1, LIN), (32, 1.0, SM), (63, 1.0, LIN), (64, 1.3, H), (96, 1.0, H), (112, 1.1, H), (127, 1.2, SM),
            (128, 1.45, H), (144, 1.3, H), (176, 1.2, H), (192, 1.45, SM)])
        put("Air Pad Swell", C_AIR, fdr("airpad"), [
            (0, -16, LIN), (8, -4, SM), (28, -4, LIN), (32, -10, SM), (128, -14, H), (136, -4, SM), (144, -8, SM),
            (176, -10, H), (184, -3, SM), (192, -6, LIN)])
        # Music bus level: verses leaner than the choruses, the breakdown held back until its build.
        put("Music Level", C_PAD, fdr("music_bus"), [
            (0, -1, H), (32, -2.4, SM), (63.5, -2.4, LIN), (64, 0, H), (96, -1.9, H), (112, -1, H), (128, -3, H),
            (140, -3, LIN), (143.9, -1, SM), (144, -0.3, H), (176, -1, H), (184, -1, LIN), (192, -7, SM)])
        put("Drum Level", C_DRUM2, fdr("drum_bus"), [
            (0, 0, H), (32, -1.3, H), (63.5, -1.3, LIN), (64, 0, H), (96, -1.1, H), (112, -0.3, H), (144, -0.5, H),
            (176, -0.5, H)])
        put("Bass Level", C_BASS, fdr("bass_bus"), [
            (0, 0, H), (32, -1.3, H), (63.5, -1.3, LIN), (64, 0, H), (96, -1.2, H), (112, -0.6, H), (128, -2, H),
            (144, -0.4, H), (176, -1, H), (186, -1, LIN), (192, -12, SM)])
        # Weather: up in the airy sections, tucked under the choruses.
        put("Weather Level", C_AIR, fdr("atmos_bus"), [
            (0, 0, LIN), (16, -1, SM), (32, -5, SM), (64, -9, H), (96, -5, H), (112, -6, H), (128, 0, SM),
            (144, -9, H), (176, -4, H), (184, 0, SM), (192, 0, LIN)])
        # Mallet: section depth in the long Space reverb, and echo throws at the ends of its phrases.
        put("Mallet Space Send", C_MALLET, snd("mallet", "space"), [
            (0, -7, LIN), (32, -13, H), (64, -18, H), (96, -13, H), (112, -11, H), (128, -5, H), (144, -17, H),
            (176, -12, H), (184, -6, SM)])
        put("Keys Space Send", C_KEYS, snd("keys", "space"), [
            (0, -12, LIN), (16, -18, SM), (32, -30, SM), (128, -10, SM), (140, -16, LIN), (144, -30, H),
            (176, -22, H), (184, -10, SM), (192, -10, LIN)])
        throws = [1, 9, 17, 25, 39, 47, 55, 63, 79, 95, 119, 127, 143, 159, 175, 190]
        pts = [(0, -20, LIN)]
        for b in throws:
            pts += [(b + 0.5, -20, H), (b + 0.5, -6, H), (b + 1.25, -6, LIN), (b + 1.5, -20, SM)]
        put("Mallet Echo Throws", C_MALLET, snd("mallet", "echo"), pts)
        # Snare throws into Space on the last backbeat of a section.
        pts = [(0, -60, LIN)]
        for b in (31, 63, 95, 111, 127, 143, 175):
            pts += [(b + 0.7, -60, H), (b + 0.7, -5, H), (b + 0.95, -60, H)]
        put("Snare Space Throws", C_DRUM, snd("snare", "space"), pts)

    def nv(self, ty, name, v):
        return norm(self.cat, ty, name, v)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gridline", required=True, help="path to the gridline command-line program")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    song = Song(Catalog(a.gridline))
    song.arrange()
    song.mix()
    song.automate()
    data = song.S.to_json()
    with open(a.out, "w", encoding="utf-8") as f:
        json.dump(data, f, sort_keys=True, separators=(",", ":"))  # compact: it is compiled into the programs
        f.write("\n")
    print(f"wrote {a.out}: {len(data['channels'])} channels, {len(data['patterns'])} patterns, "
          f"{len(data['playlist'])} clips, {len(data['automations'])} automation clips")


if __name__ == "__main__":
    main()
