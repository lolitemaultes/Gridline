#!/usr/bin/env python3
# Gridline - 0.7.8: the Tracker Sampler's sample entries in the Tracker layout, used as a person uses
# them in the real app (hidden, an isolated config folder and HOME, SDL's dummy audio driver, real SDL
# mouse events and keys):
#   - an instrument with no sample lists its 16 entries; a click on entry 02 picks it, and the Sample
#     editor shows entry 02, empty;
#   - a double-click on a file in the Files page loads it into entry 02 (every key plays it: the
#     instrument's first sample);
#   - Ctrl+Alt+Down picks entry 03; a double-click on a sample in the Browser loads it there; the
#     Sample editor shows it; its menu's "Map all keys to this entry" gives it the keys, and a note key
#     then plays entry 03 (the play tap's line is drawn for it);
#   - a range dragged on the waveform, To entry > Next free entry: entry 01 (the first empty one) plays that range of the
#     same file; undo (Ctrl+Z) takes it back in one step;
#   - with a ping-pong loop, the play line walks back on the backward passes;
#   - every button label of the lower band's screens fits its button (no label cut short) at 1440x900
#     and at 1024x640 at twice the size (the Browser beside it: the range and clipboard buttons fold into
#     Zoom... and Edit..., whose Crop crops), and the To entry menu opens above its button.
#   python3 k078_tracker_entries_app.py <gridline-app> <song.gridline.json> <out dir>
import json
import math
import os
import re
import shutil
import struct
import subprocess
import sys

APP, DEMO, OUT = sys.argv[1], sys.argv[2], sys.argv[3]
failures = []
passed = 0


def fail(msg):
    failures.append(msg)
    print("FAIL: " + msg)


def ok(cond, msg):
    global passed
    if cond:
        passed += 1
        print("ok " + msg)
    else:
        fail(msg)


def wav(path, freq, secs):
    rate = 22050
    n = int(rate * secs)
    data = b"".join(struct.pack("<h", int(9000 * math.sin(2 * math.pi * freq * i / rate))) for i in range(n))
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16))
        f.write(b"data" + struct.pack("<I", len(data)) + data)


shutil.rmtree(OUT, ignore_errors=True)
LIB = os.path.join(OUT, "sample folder")  # (a path with a space)
os.makedirs(LIB)
PAD, BELL = os.path.join(LIB, "pad one.wav"), os.path.join(LIB, "bell two.wav")
wav(PAD, 110, 2.0)
wav(BELL, 440, 2.0)
E = "tracker editors"


def run(tag, script, frames, size="1440x900", extra=()):
    cfg = os.path.join(OUT, "cfg_" + tag)
    shutil.rmtree(cfg, ignore_errors=True)
    os.makedirs(os.path.join(cfg, "gridline"))
    os.makedirs(os.path.join(cfg, "home"))
    with open(os.path.join(cfg, "gridline", "browser-folders.txt"), "w") as f:
        f.write(LIB + "\n")
    with open(os.path.join(cfg, "gridline", "settings.json"), "w") as f:
        json.dump({"format": "gridline-settings", "values": {"ui.layoutChosen": True, "ui.layoutDev": True, "ui.layout": "tracker", "keys.profile": "tracker",
                                                              "ui.menuStyle": "tracker", "tracker.diskFolder": LIB}}, f)
    song = os.path.join(cfg, "song.gridline.json")
    shutil.copyfile(DEMO, song)
    sp = os.path.join(OUT, tag + ".txt")
    with open(sp, "w") as f:
        f.write(script)
    env = dict(os.environ, SDL_AUDIODRIVER="dummy", GRIDLINE_NO_PLUGIN_SCAN="1", GRIDLINE_CONFIG_HOME=cfg, HOME=os.path.join(cfg, "home"))
    r = subprocess.run([APP, song, "--hidden", "--no-ui-sounds", "--no-update-check", "--fixed-dt", "16", "--size", size, "--dev-input", sp,
                        "--frames", str(frames)] + list(extra), env=env, capture_output=True, text=True, timeout=600)
    with open(os.path.join(OUT, tag + ".log"), "w") as f:
        f.write(r.stdout + "\n---- stderr ----\n" + r.stderr)
    if r.returncode != 0:
        fail("%s: gridline-app failed (%d)" % (tag, r.returncode))
    return r.stdout


def entry(out, frame):
    m = re.search(r"tracker editors frame %d: entry (-?\d+) well (\S+) (\S+) (\S+) (\S+) used([^\n]*?) lines([^\n]*)\n" % frame, out)
    if not m:
        return None
    used = {}
    for u in m.group(6).split():
        k, rest = u.split(":")
        cut, keys = rest.split("/")
        a, b = cut.split("-")
        used[int(k, 16) - 1] = (int(a), int(b), int(keys))
    lines = [(int(x.split("@")[0]), int(x.split("@")[1])) for x in m.group(7).split()]
    return {"entry": int(m.group(1)), "well": tuple(float(m.group(i)) for i in range(2, 6)), "used": used, "lines": lines}


def undo_of(out, frame):
    m = re.search(r"tracker editors frame %d: .* pos (\d+) undo \"([^\"]*)\"" % frame, out)
    return (int(m.group(1)), m.group(2)) if m else (-1, "")


# Browser rows: where the bell's row is (the Browser in the side panel, the folder tab shown).
START = "5 toasts clear\n6 %s open frame 0\n8 replace gl.tracker\n10 layout open Browser\n12 browser addtab %s\n" % (E, LIB)
out = run("rows", START + "14 browser rows on\n20 %s open sample 0\n40 browser rows\n44 quit discard\n" % E, 60)
m = re.search(r"  row \"[^\"]*\" x ([0-9.]+) ([0-9.]+) y ([0-9.]+) ([0-9.]+) path %s\n" % re.escape(BELL), out)
ok(m is not None, "the Browser lists the bell")
if m is None:
    print("%d passed, %d failed" % (passed, len(failures)))
    sys.exit(1)
bx, by = int(float(m.group(1)) + 60), int((float(m.group(3)) + float(m.group(4))) / 2)

# The well's place (an instrument with a sample, the Sample editor open).
out = run("well", START + "20 %s open sample 0\n24 %s load \"%s\"\n30 %s dump\n34 quit discard\n" % (E, E, PAD, E), 40)
w = entry(out, 30)
ok(w is not None and w["well"][2] > w["well"][0] + 200, "the Sample editor's waveform is drawn")
if w is None:
    print("%d passed, %d failed" % (passed, len(failures)))
    sys.exit(1)
wx0, wy0, wx1, wy1 = w["well"]
wy = int((wy0 + wy1) / 2)
ra, rb = int(wx0 + (wx1 - wx0) * 0.25), int(wx0 + (wx1 - wx0) * 0.6)

script = START + """20 {E} open sample 0
24 tracker samplelist
26 {E} menu at entry 02
28 sdldown 0
29 sdlup 0
34 {E} dump
36 tracker swap files
38 {E} menu at files Samples
40 sdldown 0
41 sdlup 0
44 {E} menu at file pad one.wav
46 sdldown 0
47 sdlup 0
48 sdldown 0
49 sdlup 0
56 {E} dump
58 tracker swap none
60 {E} focus
62 sdlkey down down ctrl+alt
63 sdlkey down up ctrl+alt
64 sdlkey #224 up
64 sdlkey #226 up
66 {E} dump
70 sdlmove {bx} {by}
72 sdldown 0
73 sdlup 0
74 sdldown 0
75 sdlup 0
82 {E} dump
84 {E} menu at entry 03
86 sdldown 1
87 sdlup 1
92 {E} menu at Map all keys to this entry
94 sdldown 0
95 sdlup 0
100 {E} dump
102 {E} focus
104 sdlkey z down
112 {E} dump
114 sdlkey z up
130 sdlmove {ra} {wy}
132 sdldown 0
134 sdlmove {rm} {wy}
136 sdlmove {rb} {wy}
138 sdlup 0
142 {E} dump
144 {E} menu at To entry button
146 sdldown 0
147 sdlup 0
152 {E} menu at Next free entry
154 sdldown 0
155 sdlup 0
160 {E} dump
162 {E} focus
164 sdlkey z down ctrl
165 sdlkey z up ctrl
166 sdlkey #224 up
170 {E} dump
174 quit discard
""".format(E=E, bx=bx, by=by, ra=ra, rb=rb, rm=(ra + rb) // 2, wy=wy)
out = run("use", script, 180)

d = entry(out, 34)
ok(re.search(r"tracker samplelist frame 24: 01 [|]02 [|]03 [|]04 [|]05 [|]06 [|]07 [|]08 [|]09 [|]0A [|]0B ", out) is not None,
   "an instrument with no sample lists its entries, empty (the rows that fit: 01 to 0B; the wheel scrolls to 10)")
ok(d is not None and d["entry"] == 1 and not d["used"], "a click on entry 02 picks it; the Sample editor shows it, empty (%s)" % (d,))
d = entry(out, 56)
ok(d is not None and d["entry"] == 1 and list(d["used"]) == [1] and d["used"][1][2] == 96,
   "Files page: a double-click loads the file into entry 02, on every key (the first sample) (%s)" % (d,))
d = entry(out, 66)
ok(d is not None and d["entry"] == 2, "Ctrl+Alt+Down picks entry 03 (%s)" % (d,))
d = entry(out, 82)
ok(d is not None and d["entry"] == 2 and sorted(d["used"]) == [1, 2] and d["used"][2][2] == 0 and d["used"][1][2] == 96,
   "Browser: a double-click loads the sample into entry 03; the keys stay with entry 02 (%s)" % (d,))
ok(re.search(r"tracker editors frame 82: shown 1 .* slot 2 frames 44100 ", out) is not None, "the Sample editor shows entry 03's sample")
d = entry(out, 100)
ok(d is not None and d["used"].get(2, (0, 0, 0))[2] == 96 and d["used"].get(1, (0, 0, -1))[2] == 0,
   "the entry's menu: Map all keys to this entry (%s)" % (d,))
d = entry(out, 112)
ok(d is not None and len(d["lines"]) >= 1, "a note key mapped to entry 03 plays it: its play line in the editor (%s)" % (d,))
m = re.search(r"tracker editors frame 142: .* range (\d+) (\d+) ", out)
ok(m is not None and 0 < int(m.group(1)) < int(m.group(2)), "a range dragged on the waveform")
rng = (int(m.group(1)), int(m.group(2))) if m else (0, 0)
d = entry(out, 160)
ok(d is not None and d["entry"] == 0 and d["used"].get(0, (0, 0, 0))[:2] == rng and d["used"][0][2] == 0,
   "To entry > Next free entry: entry 01, the first empty one, plays the range of the same file, no keys, picked (%s, range %s)" % (d, rng))
u160, u170 = undo_of(out, 160), undo_of(out, 170)
ok(u160[1] == "range to entry", "one undo step, \"range to entry\" (%s)" % (u160,))
d = entry(out, 170)
ok(d is not None and 0 not in d["used"] and u170[0] == u160[0] - 1, "Ctrl+Z takes the entry back in one step (%s %s)" % (d, u170))

# ---- the ping-pong loop: the line walks back on the backward pass ----
dumps = "".join("%d %s dump\n" % (f, E) for f in range(60, 200))
script = START + """20 {E} open sample 0
24 {E} load "{pad}"
28 {E} loop pingpong
30 {E} pin 2 2000
32 {E} pin 3 9000
40 {E} focus
50 sdlkey z down
{dumps}202 sdlkey z up
206 quit discard
""".format(E=E, pad=PAD, dumps=dumps)
out = run("pingpong", script, 210)
xs = []
for f in range(60, 200):
    d = entry(out, f)
    if d and d["lines"]:
        xs.append((f, d["lines"][0][0], d["lines"][0][1]))
back = sum(1 for i in range(1, len(xs)) if xs[i][2] < xs[i - 1][2] and xs[i - 1][2] - xs[i][2] < 4000)
fwd = sum(1 for i in range(1, len(xs)) if xs[i][2] > xs[i - 1][2])
leftward = sum(1 for i in range(1, len(xs)) if xs[i][1] < xs[i - 1][1])
ok(len(xs) > 40, "ping-pong: the play line is drawn while the note holds (%d frames)" % len(xs))
ok(back >= 5 and fwd >= 5 and leftward >= 5, "ping-pong: the line moves left on the backward passes (%d back, %d forward, %d leftward)" % (back, fwd, leftward))
lo = min((x[2] for x in xs), default=0)
ok(all(x[2] <= 9001 for x in xs[5:]) and lo >= 0, "ping-pong: the line stays inside the sample")

# ---- every button label fits (the Sample editor's grid with To entry, Duplicate and All keys; the
# Instrument editor and its Extra page; the Channel frame), at two window sizes and scales, both themes;
# the To entry menu opens above its button and leaves the button column in view ----
script = START + """20 {E} open sample 0
24 {E} load "{pad}"
28 tracker labels on
30 {E} range 9000 30000
40 tracker labels
44 {E} menu at To entry button
46 sdldown 0
47 sdlup 0
52 {E} menu at Next free entry
53 {E} menu at Entry 10
54 key escape
56 {E} menu at Edit button
57 sdldown 0
58 sdlup 0
62 {E} menu at Edit Crop
64 sdldown 0
65 sdlup 0
68 {E} dump
69 {E} open instrument 0
70 tracker labels
74 {E} ins extra on
90 tracker labels
100 {E} open frame 1
110 tracker labels
114 quit discard
""".format(E=E, pad=PAD)
for tag, size, extra in (("labels1440", "1440x900", ()), ("labels1024x2", "2048x1280", ("--ui-scale", "2", "--light"))):
    out = run(tag, script, 120, size, extra)
    for f, what in ((40, "the Sample editor"), (70, "the Instrument editor"), (90, "the Instrument editor's Extra page"), (110, "the Channel frame")):
        m = re.search(r"tracker labels frame %d: drawn (\d+) clipped (\d+) tight \d+([^\n]*)" % f, out)
        ok(m is not None and int(m.group(1)) >= 20 and int(m.group(2)) == 0, "%s %s: every button label fits (%s)" % (tag, what, m.group(0) if m else None))
    b = re.search(r"menu row \"To entry button\" at (\d+) (\d+)", out)
    rows = [re.search(r"menu row \"%s\" at (\d+) (\d+)" % n, out) for n in ("Next free entry", "Entry 10")]
    ok(b is not None and all(rows) and all(int(r.group(2)) < int(b.group(2)) - 8 for r in rows),
       "%s: the To entry menu opens above its button (%s %s)" % (tag, b and b.group(0), [r and r.group(0) for r in rows]))
    if tag == "labels1024x2":  # (the Browser beside it: the range and clipboard columns fold, Edit... holds Crop)
        m = re.search(r"tracker editors frame 68: .* frames (\d+) ", out)
        ok(m is not None and int(m.group(1)) == 21000, "%s: short of width, Edit... > Crop crops the sample to the range (%s)" % (tag, m and m.group(1)))

print("%d passed, %d failed" % (passed, len(failures)))
sys.exit(1 if failures else 0)
