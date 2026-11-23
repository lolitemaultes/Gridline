#!/usr/bin/env python3
# Gridline - 0.7.8 (user report: "i cant drag the markers and ping pong / areas in tracker sample editor"):
# the Tracker layout's Sample editor, in the real app (hidden, an isolated config folder and HOME, SDL's
# dummy audio driver, real SDL mouse events), a ping-pong loop on a 2 s sample:
#   - the loop start dragged by its pin at the top, the loop end by its pin at the bottom;
#   - each loop line dragged from the middle of the well (it drew a range there: only the pins took it);
#   - the whole loop dragged by its strip at the top of the well, between its lines: it moves, its length kept;
#   - a drag that loses the keys to another window half-way, the button still down, goes on to the end;
#   - each drag one undo step, which Ctrl+Z takes back.
#   python3 k078_sample_markers_app.py <gridline-app> <song.gridline.json> <out dir>
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


def fail(msg):
    failures.append(msg)
    print("FAIL: " + msg)


def ok(cond, msg):
    if cond:
        print("ok - " + msg)
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
PAD = os.path.join(LIB, "pad one.wav")
wav(PAD, 110, 2.0)
FRAMES = 44100
E = "tracker editors"


def run(tag, script, frames):
    cfg = os.path.join(OUT, "cfg_" + tag)
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
    r = subprocess.run([APP, song, "--hidden", "--no-ui-sounds", "--no-update-check", "--fixed-dt", "16", "--size", "1440x900", "--dev-input", sp,
                        "--frames", str(frames)], env=env, capture_output=True, text=True, timeout=600)
    with open(os.path.join(OUT, tag + ".log"), "w") as f:
        f.write(r.stdout + "\n---- stderr ----\n" + r.stderr)
    if r.returncode != 0:
        fail("%s: gridline-app failed (%d)" % (tag, r.returncode))
    return r.stdout


def state(out, frame):
    """(loop start, loop length, undo position, undo name) at a frame's dump."""
    m = re.search(r"tracker editors frame %d: .*? loop \d+ (\d+) (\d+) .* pos (\d+) undo \"([^\"]*)\"" % frame, out)
    return (int(m.group(1)), int(m.group(2)), int(m.group(3)), m.group(4)) if m else None


def well(out, frame):
    m = re.search(r"tracker editors frame %d: entry -?\d+ well (\S+) (\S+) (\S+) (\S+)" % frame, out)
    return tuple(float(v) for v in m.groups()) if m else None


START = """5 toasts clear
6 {E} open frame 0
8 replace gl.tracker
20 {E} open sample 0
24 {E} load "{pad}"
28 {E} loop pingpong
30 {E} pin 2 10000
32 {E} pin 3 30000
36 {E} dump
40 focusmove main
""".format(E=E, pad=PAD)

# The well's place first (x0 y0 x1 y1, logical px; the whole sample in view).
out = run("place", START + "40 quit discard\n", 44)
w = well(out, 36)
if not w:
    fail("no well reported")
    sys.exit(1)
x0, y0, x1, y1 = w


def x_of(f):
    return round(x0 + (x1 - x0) * f / FRAMES)


def drag(f0, fr, y, to_frame, focus=False):
    """Frame-numbered lines: press at frame f0 at (x of frame `fr`, y), five moves to `to_frame`, release."""
    xa, xb = x_of(fr), x_of(to_frame)
    lines = ["%d sdlmove %d %d" % (f0, xa, y), "%d sdldown 0" % (f0 + 2)]
    for i in range(1, 6):
        lines.append("%d sdlmove %d %d" % (f0 + 2 + 2 * i, round(xa + (xb - xa) * i / 5), y))
        if focus and i == 2:
            lines.append("%d focuslost main" % (f0 + 3 + 2 * i))
    lines.append("%d sdlup 0" % (f0 + 15))
    lines.append("%d %s dump" % (f0 + 19, E))
    return lines, f0 + 19


top, bottom, middle = round(y0 + 3), round(y1 - 3), round((y0 + y1) / 2 + (y1 - y0) / 4)
cases = [  # name, press frame of the sample, y, to frame, focus loss, want (loop start, loop length)
    ("the loop start by its pin at the top", 10000, top, 14000, False, (14000, 16000)),
    ("the loop end by its pin at the bottom", 30000, bottom, 34000, False, (10000, 24000)),
    ("the loop start's line from the middle of the well", 10000, middle, 6000, False, (6000, 24000)),
    ("the loop end's line from the middle of the well", 30000, middle, 26000, False, (10000, 16000)),
    ("the whole ping-pong loop by its strip at the top", 20000, top, 26000, False, (16000, 20000)),
    ("the loop start by its pin, the keys lost half-way", 10000, top, 14000, True, (14000, 16000)),
]
base = None
for n, (name, fr, y, to, focus, want) in enumerate(cases):  # each in a run of its own (an undo of nothing would undo the setup)
    lines, at = drag(50, fr, y, to, focus)
    script = START.splitlines() + lines + ["%d sdlkey z down ctrl" % (at + 3), "%d sdlkey z up ctrl" % (at + 4), "%d sdlkey #224 up" % (at + 5),
                                           "%d %s dump" % (at + 9, E), "%d quit discard" % (at + 12)]
    out = run("drag%d" % n, "\n".join(script) + "\n", at + 16)
    b, s1, u = state(out, 36), state(out, at), state(out, at + 9)
    if n == 0:
        ok(b is not None and b[:2] == (10000, 20000), "the loop to start with: 10000, 20000 frames long (%s)" % (b,))
    if not b or not s1 or not u:
        fail("%s: no report" % name)
        continue
    ok(abs(s1[0] - want[0]) <= 120 and abs(s1[1] - want[1]) <= 240 and s1[2] == b[2] + 1,
       "%s: loop %d, %d frames long, undo +%d \"%s\" (want about %s in one step)" % (name, s1[0], s1[1], s1[2] - b[2], s1[3], want))
    ok(u[:3] == b[:3], "%s: Ctrl+Z puts the loop back (%s)" % (name, u[:2]))
if failures:
    print("%d failure(s)" % len(failures))
    sys.exit(1)
print("all passed")
