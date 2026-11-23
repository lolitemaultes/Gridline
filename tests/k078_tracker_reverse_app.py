#!/usr/bin/env python3
# Gridline - 0.7.8: the Tracker Sampler's Backwards option in the Tracker layout, used as a person uses
# it in the real app (hidden, an isolated config folder and HOME, SDL's dummy audio driver, real SDL
# mouse events and keys):
#   - a click on the Sample editor's Backwards box (beside the loop type) is one undo step, "sample play
#     direction";
#   - a note then plays the sample from its end: the play line starts near the end and moves left;
#   - Ctrl+Z takes it back in one step, and the next note's line moves right again;
#   - with a forward loop, played backwards: the line runs down through the loop again and again;
#   - every button label of the Sample editor fits (the label watch) with the Backwards box shown, at
#     1440x900 and at 1024x640 at twice the size; photos of the editor and of the plugin window.
#   - the header's sample list marks a backwards sample (B beside its loop letter), and the mark goes
#     when Backwards is switched off;
#   - Cut after loop and Minimise on a backwards sample take what precedes the loop (the sample then
#     starts with its loop), each one undo step that Ctrl+Z takes back; on a forward sample they take
#     what follows it, as before; photos of the list, the plugin window's list and the convert box.
#   python3 k078_tracker_reverse_app.py <gridline-app> <song.gridline.json> <out dir> [photo dir]
import json
import math
import os
import re
import shutil
import struct
import subprocess
import sys

APP, DEMO, OUT = sys.argv[1], sys.argv[2], sys.argv[3]
PHOTOS = sys.argv[4] if len(sys.argv) > 4 else OUT
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
os.makedirs(PHOTOS, exist_ok=True)
LIB = os.path.join(OUT, "sample folder")  # (a path with a space)
os.makedirs(LIB)
PAD = os.path.join(LIB, "pad one.wav")
wav(PAD, 110, 2.0)
E = "tracker editors"


def run(tag, script, frames, size="1440x900", extra=()):
    cfg = os.path.join(OUT, "cfg_" + tag)
    shutil.rmtree(cfg, ignore_errors=True)
    os.makedirs(os.path.join(cfg, "gridline"))
    os.makedirs(os.path.join(cfg, "home"))
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


def lines_of(out, frame):
    m = re.search(r"tracker editors frame %d: entry -?\d+ well \S+ \S+ \S+ \S+ used[^\n]*? lines([^\n]*)\n" % frame, out)
    if not m:
        return None
    return [(int(x.split("@")[0]), int(x.split("@")[1])) for x in m.group(1).split()]


def undo_of(out, frame):
    m = re.search(r"tracker editors frame %d: .* pos (\d+) undo \"([^\"]*)\"" % frame, out)
    return (int(m.group(1)), m.group(2)) if m else (-1, "")


def walk(out, frames):
    xs = []
    for f in frames:
        ls = lines_of(out, f)
        if ls:
            xs.append((f, ls[0][0], ls[0][1]))
    left = sum(1 for i in range(1, len(xs)) if xs[i][1] < xs[i - 1][1])
    right = sum(1 for i in range(1, len(xs)) if xs[i][1] > xs[i - 1][1])
    return xs, left, right


START = "5 toasts clear\n6 %s open frame 0\n8 replace gl.tracker\n" % E
FRAMES = 44100  # (the pad: 2 s at 22050 Hz)

# ---- the click, the note, undo, the next note ----
d1 = "".join("%d %s dump\n" % (f, E) for f in range(60, 150))
d2 = "".join("%d %s dump\n" % (f, E) for f in range(200, 290))
script = START + """20 {E} open sample 0
24 {E} load "{pad}"
30 {E} menu at Backwards box
32 sdldown 0
33 sdlup 0
38 {E} dump
40 {E} focus
50 sdlkey z down
{d1}152 sdlkey z up
160 {E} focus
164 sdlkey z down ctrl
165 sdlkey z up ctrl
166 sdlkey #224 up
172 {E} dump
180 {E} focus
190 sdlkey z down
{d2}292 sdlkey z up
296 quit discard
""".format(E=E, pad=PAD, d1=d1, d2=d2)
out = run("click", script, 300)
u38, u172 = undo_of(out, 38), undo_of(out, 172)
ok(u38[1] == "sample play direction", "a click on Backwards: one undo step, \"sample play direction\" (%s)" % (u38,))
xs, left, right = walk(out, range(60, 150))
ok(len(xs) > 30, "backwards: the play line is drawn while the note holds (%d frames)" % len(xs))
ok(xs and xs[0][2] > FRAMES * 0.8, "backwards: the line starts near the sample's end (%s)" % (xs[:1],))
ok(left >= 20 and right == 0, "backwards: the line moves left, never right (%d left, %d right)" % (left, right))
ok(u172[0] == u38[0] - 1 and u172[1] != "sample play direction", "Ctrl+Z takes it back in one step (%s %s)" % (u38, u172))
xs, left, right = walk(out, range(200, 290))
ok(len(xs) > 30 and xs[0][2] < FRAMES * 0.2 and right >= 20 and left == 0,
   "after undo the next note plays forwards: the line moves right (%d right, %d left, %s)" % (right, left, xs[:1]))

# ---- a forward loop, played backwards: down through the loop, round again ----
d1 = "".join("%d %s dump\n" % (f, E) for f in range(60, 200))
script = START + """20 {E} open sample 0
24 {E} load "{pad}"
26 {E} backwards on
28 {E} loop forward
30 {E} pin 2 36000
32 {E} pin 3 40000
40 {E} focus
50 sdlkey z down
{d1}202 sdlkey z up
206 quit discard
""".format(E=E, pad=PAD, d1=d1)
out = run("loop", script, 210)
xs, left, right = walk(out, range(60, 200))
inside = [x for x in xs if x[2] < 40000]
ok(len(xs) > 40 and left >= 5 * max(1, right) and right >= 1,
   "forward loop, backwards: the line runs left, jumping right at each round (%d left, %d right)" % (left, right))
ok(inside and all(35990 <= x[2] <= 40001 for x in inside[2:]), "forward loop, backwards: it stays in the loop once there")

# ---- labels fit with the Backwards box; photos of the editor and the plugin window ----
for tag, size, extra in (("labels1440", "1440x900", ()), ("labels1024x2", "2048x1280", ("--ui-scale", "2", "--light"))):
    shot = os.path.join(PHOTOS, "sample-editor-backwards-%s.ppm" % tag)
    pshot = os.path.join(PHOTOS, "plugin-window-backwards-%s.ppm" % tag)
    script = START + """20 {E} open sample 0
24 {E} load "{pad}"
26 {E} backwards on
28 tracker labels on
30 {E} loop pingpong
34 {E} focus
36 sdlkey z down
50 tracker labels
52 shot "{shot}"
54 sdlkey z up
60 {E} plugin 0
70 shot "{pshot}"
74 quit discard
""".format(E=E, pad=PAD, shot=shot, pshot=pshot)
    out = run(tag, script, 80, size, extra)
    m = re.search(r"tracker labels frame 50: drawn (\d+) clipped (\d+) tight \d+([^\n]*)", out)
    ok(m is not None and int(m.group(1)) >= 20 and int(m.group(2)) == 0, "%s: every label of the Sample editor fits (%s)" % (tag, m.group(0) if m else None))
    ok(re.search(r"\"Backwards\"", m.group(3) if m else "") is None, "%s: the Backwards box is neither cut short nor tight" % tag)
    ok(os.path.exists(shot), "%s: photo of the Sample editor" % tag)

# ---- short of width (1024x640 at twice the size, the Browser beside it): the Backwards box moves into
# Process..., whose row switches it as one undo step; every label still fits ----
script = START + """10 layout open Browser
20 {E} open sample 0
24 {E} load "{pad}"
28 tracker labels on
40 tracker labels
42 {E} menu at Backwards box
44 {E} menu at Process button
46 sdldown 0
47 sdlup 0
50 shot "{shot}"
52 {E} menu at Process Backwards
54 sdldown 0
55 sdlup 0
60 {E} dump
64 quit discard
""".format(E=E, pad=PAD, shot=os.path.join(PHOTOS, "sample-editor-tight-process-menu.ppm"))
out = run("tight", script, 70, "2048x1280", ("--ui-scale", "2"))
m = re.search(r"tracker labels frame 40: drawn (\d+) clipped (\d+) tight \d+([^\n]*)", out)
ok(m is not None and int(m.group(1)) >= 20 and int(m.group(2)) == 0, "tight: every label of the Sample editor fits (%s)" % (m.group(0) if m else None))
ok(re.search(r"menu row \"Backwards box\" none", out) is not None, "tight: no Backwards column")
u60 = undo_of(out, 60)
ok(u60[1] == "sample play direction", "tight: Process... > Backwards, one undo step (%s)" % (u60,))

# ---- the list's mark; Cut after loop and Minimise follow the direction ----
def state(out, frame):
    m = re.search(r"tracker editors frame %d: .* frames (\d+) start (\d+) end (\d+) .* loop (-?\d+) (\d+) (\d+) .* pos (\d+) undo \"([^\"]*)\"" % frame, out)
    return (int(m.group(1)), int(m.group(4)), int(m.group(5)), int(m.group(6)), int(m.group(7)), m.group(8)) if m else None


def marks(out, frame):
    m = re.search(r"tracker samplelist frame %d: ([^\n]*)" % frame, out)
    if not m:
        return None
    mm = re.search(r"  marks (.*)$", m.group(1))
    return mm.group(1).strip() if mm else ""


for tag, back in (("trimback", True), ("trimfwd", False)):
    script = START + """20 {E} open sample 0
24 {E} load "{pad}"
26 {E} backwards {onoff}
28 {E} loop forward
30 {E} pin 2 36000
32 {E} pin 3 40000
34 tracker labels on
36 tracker samplelist
38 {E} dump
39 shot "{shot}"
40 {E} menu at Cut after loop button
42 sdldown 0
43 sdlup 0
48 {E} dump
50 tracker labels
52 {E} focus
54 sdlkey z down ctrl
55 sdlkey z up ctrl
56 sdlkey #224 up
60 {E} dump
62 {E} menu at Minimise button
64 sdldown 0
65 sdlup 0
70 {E} dump
72 {E} backwards off
74 tracker samplelist
76 {E} plugin 0
82 shot "{pshot}"
86 quit discard
""".format(E=E, pad=PAD, onoff="on" if back else "off", shot=os.path.join(PHOTOS, "sample-list-mark-%s.ppm" % tag),
           pshot=os.path.join(PHOTOS, "plugin-window-list-%s.ppm" % tag))
    out = run(tag, script, 90)
    s38, s48, s60, s70 = state(out, 38), state(out, 48), state(out, 60), state(out, 70)
    m36, m74 = marks(out, 36), marks(out, 74)
    ok(s38 is not None and s38[0:4] == (FRAMES, 1, 36000, 4000), "%s: a looped sample, 36000 + 4000 (%s)" % (tag, s38))
    if back:
        ok(m36 is not None and re.search(r"(^| )01:LB( |$)", m36) is not None, "the list marks the backwards sample: 01:LB (%s)" % m36)
        ok(m74 is not None and re.search(r"(^| )01:L( |$)", m74) is not None, "Backwards off: the mark goes, the loop's stays (%s)" % m74)
        want = (FRAMES - 36000, 1, 0, 4000)
        what = "the part before the loop goes, the loop starts the sample"
    else:
        ok(m36 is not None and re.search(r"(^| )01:L( |$)", m36) is not None, "a forward sample: no backwards mark (%s)" % m36)
        want = (40000, 1, 36000, 4000)
        what = "the part after the loop goes, as before"
    ok(s48 is not None and s48[0:4] == want and s48[5] == "trim sample after loop" and s48[4] == s38[4] + 1,
       "%s: Cut after loop: %s, one undo step (%s)" % (tag, what, s48))
    ok(s60 is not None and s60[0:4] == s38[0:4] and s60[4] == s38[4], "%s: Ctrl+Z brings the whole sample back (%s)" % (tag, s60))
    ok(s70 is not None and s70[0:4] == want and s70[4] == s38[4] + 1, "%s: Minimise does the same, one undo step (%s)" % (tag, s70))
    lm = re.search(r"tracker labels frame 50: drawn (\d+) clipped (\d+)", out)
    ok(lm is not None and int(lm.group(2)) == 0, "%s: every label fits (%s)" % (tag, lm.group(0) if lm else None))

# ---- the list's mark in the light theme (ping-pong, backwards); the Convert box's text at twice the
# size (reverse is carried over) ----
script = START + """20 {E} open sample 0
24 {E} load "{pad}"
26 {E} backwards on
28 {E} loop pingpong
30 tracker samplelist
32 shot "{shot}"
36 quit discard
""".format(E=E, pad=PAD, shot=os.path.join(PHOTOS, "sample-list-mark-light.ppm"))
out = run("marklight", script, 40, "1440x900", ("--light",))
m30 = marks(out, 30)
ok(m30 is not None and re.search(r"(^| )01:PB( |$)", m30) is not None, "light: ping-pong and backwards marked: 01:PB (%s)" % m30)
script = START + """12 addvia filerack "{pad}"
16 {E} close
20 {E} open frame 8
24 {E} convert
30 shot "{cshot}"
32 key escape
36 quit discard
""".format(E=E, pad=PAD, cshot=os.path.join(PHOTOS, "convert-box-x2.ppm"))
run("convertx2", script, 40, "2048x1280", ("--ui-scale", "2", "--light"))

print("%d passed, %d failed" % (passed, len(failures)))
sys.exit(1 if failures else 0)
