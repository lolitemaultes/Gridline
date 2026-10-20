#!/usr/bin/env python3
"""Build the README showcase project: Gridline's demo song (demo/demo.gridline.json)
grown into a fuller 12-bar arrangement with lead, pad and arp parts, an audio
clip track (the demo's own recording) and an automation clip.

Usage: python3 tools/readme_showcase.py OUT.gridline.json [gridline root]
Used by tools/readme_shots.sh; the result is not part of the repository.
"""
import json
import os
import sys


def main():
    out = sys.argv[1]
    root = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
    root = os.path.abspath(root)
    song = json.load(open(os.path.join(root, "demo", "demo.gridline.json")))
    ppq = song["ppq"]
    bar = ppq * 4
    nid = [max(song["nextId"], 1000)]

    def new_id():
        nid[0] += 1
        return nid[0]

    ch = {c["name"]: c for c in song["channels"]}
    pat = {p["name"]: p for p in song["patterns"]}
    mixer = song["mixer"]

    def add_channel(name, ptype, color, insert, data=None, audio=False):
        c = {"color": color, "gain": 0.78, "id": new_id(), "mixerTrack": insert, "muted": False, "name": name,
             "pan": 0, "pitch": 0, "plugin": {"type": ptype}}
        if data:
            c["plugin"]["data"] = data
        if audio:
            c["audioClip"] = True
        song["channels"].append(c)
        mixer[insert]["name"] = name
        mixer[insert]["color"] = color
        return c

    lead = add_channel("Lead", "gl.sawswarm", 0xE0A040, 9)
    pad = add_channel("Pad", "gl.twinstring", 0x6A8CD8, 10)
    arp = add_channel("Arp", "gl.pluck", 0x48C0A8, 11)
    wav = os.path.join(root, "demo", "Audio", "Edison")
    wav = os.path.join(wav, sorted(os.listdir(wav))[0])
    vox = add_channel("Texture", "gl.sampler", 0xC070C0, 12, data=wav, audio=True)

    def add_pattern(name, color, bars, notes):
        p = {"bars": bars, "color": color, "id": new_id(), "name": name, "notes": notes, "steps": {}, "stepsPerBar": 16}
        song["patterns"].append(p)
        return p

    def n(start16, len16, pitch, vel=96):
        return {"length": len16 * ppq // 4, "pitch": pitch, "start": start16 * ppq // 4, "velocity": vel}

    # Lead: a two-bar A minor hook.
    hook = [(0, 3, 76), (3, 3, 74), (6, 2, 72), (8, 4, 74), (12, 2, 72), (14, 2, 69),
            (16, 3, 72), (19, 3, 71), (22, 2, 67), (24, 6, 69), (30, 2, 64)]
    # ... over chord stabs (Am, F, C, G) in the same part.
    stabs = [(0, (57, 60, 64)), (8, (53, 57, 60)), (16, (52, 55, 60)), (24, (55, 59, 62))]
    lead_notes = [n(s, l, p, 100 + (s % 4 == 0) * 20) for s, l, p in hook]
    lead_notes += [n(s + o, 3, p, 72 - o * 6) for s, ch3 in stabs for o in (0, 4) for p in ch3]
    p_lead = add_pattern("Lead", 0xE0A040, 2, {str(lead["id"]): lead_notes})
    # Pad: four bars of slow chords (Am, F, C, G).
    chords = [(57, 60, 64), (53, 57, 60), (52, 55, 60), (55, 59, 62)]
    p_pad = add_pattern("Pad", 0x6A8CD8, 4, {str(pad["id"]): [n(i * 16, 16, p, 80) for i, c in enumerate(chords) for p in c]})
    # Arp: sixteenth notes over one bar.
    seq = [69, 72, 76, 72, 81, 76, 72, 76, 69, 72, 76, 79, 81, 79, 76, 72]
    p_arp = add_pattern("Arp", 0x48C0A8, 1, {str(arp["id"]): [n(i, 1, p, 70 + (i % 4 == 0) * 30) for i, p in enumerate(seq)]})

    # Automation: the bass filter opening over the first eight bars.
    auto_id = new_id()
    song["automations"] = [{
        "id": auto_id, "name": "Bass cutoff", "color": 0x9AA4B4, "owner": ch["Bass"]["id"], "index": 115,
        "points": [[0, 0.18, 0, 0], [bar * 4, 0.42, 0.4, 0], [bar * 8, 0.85, -0.3, 0], [bar * 10, 0.55, 0, 0],
                   [bar * 12, 0.9, 0, 0]],
    }]

    tracks = [("Drums", 0xD8775A), ("Bass", 0xA56CD8), ("Chords", 0x4FB0A0), ("Lead", 0xE0A040),
              ("Pad", 0x6A8CD8), ("Arp", 0x48C0A8), ("Texture", 0xC070C0), ("Automation", 0x9AA4B4)]
    song["tracks"] = [{"color": c, "muted": False, "name": t, "height": 1.7 if t == "Automation" else 2.1} for t, c in tracks]

    clips = []

    def clip(track, start_bar, **kw):
        c = {"id": new_id(), "length": kw.pop("length", 0), "start": start_bar * bar, "track": track}
        c.update(kw)
        clips.append(c)

    for b in range(12):
        clip(0, b, pattern=pat["Fill" if b % 4 == 3 else "Beat"]["id"])
    for b in range(0, 12, 2):
        clip(1, b, pattern=pat["Bassline"]["id"])
        clip(2, b, pattern=pat["Chords"]["id"])
    for b in range(4, 12, 2):
        clip(3, b, pattern=p_lead["id"])
    for b in range(0, 12, 4):
        clip(4, b, pattern=p_pad["id"])
    for b in range(2, 12):
        clip(5, b, pattern=p_arp["id"])
    # Audio: the recording three times, overlapping into crossfades.
    for i, b in enumerate((0, 4, 8)):
        clip(6, b, kind="audio", source=vox["id"], length=bar * 4 + (bar // 2 if i < 2 else 0),
             fadeIn=0.25 if i == 0 else 0.5, fadeOut=0.5)
    clip(7, 0, kind="automation", source=auto_id, length=bar * 12)
    song["playlist"] = clips
    song["markers"] = [{"pos": 0, "name": "Intro"}, {"pos": bar * 4, "name": "Verse"}, {"pos": bar * 8, "name": "Drop"}]
    song["nextId"] = nid[0] + 1
    song["info"]["title"] = "Gridline Showcase"
    with open(out, "w") as f:
        json.dump(song, f, indent=1)


if __name__ == "__main__":
    main()
