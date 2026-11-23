#!/usr/bin/env python3
# Gridline - the Browser in the Tracker layout, drawn in the Tracker's look (its header band's bevel
# boxes, buttons and scroll bar), used as a person uses it in the real app (hidden, isolated config
# folder and HOME, SDL's dummy audio driver, real SDL mouse events and typed text):
#   - the Tracker look is active in the Tracker and nowhere else;
#   - typing in the search field finds a file; its clear button empties the field;
#   - clicking a tab shows it (a built-in tab, then a folder tab);
#   - clicking a sample previews it; the Down key moves to the next one;
#   - a sample dragged with the mouse carries the file and drops on the pattern; dropped on an instrument it loads there;
#   - a double-click loads a sample;
#   - a right-click opens the file's menu, and its items work;
#   - the + menu opens.
#   python3 k078_tracker_browser_app.py <gridline-app> <song.gridline.json> <out dir>
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


def ok(cond, msg, out=""):
    global passed
    if cond:
        passed += 1
        print("ok " + msg)
    else:
        fail(msg)


def wav(path, freq, secs=0.4):
    rate = 22050
    n = int(rate * secs)
    data = b"".join(struct.pack("<h", int(10000 * math.sin(2 * math.pi * freq * i / rate))) for i in range(n))
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16))
        f.write(b"data" + struct.pack("<I", len(data)) + data)


shutil.rmtree(OUT, ignore_errors=True)
LIB = os.path.join(OUT, "sample folder")  # (a path with a space)
os.makedirs(LIB)
KICK, SNARE = os.path.join(LIB, "kick one.wav"), os.path.join(LIB, "snare two.wav")
wav(KICK, 60)
wav(SNARE, 240)


def run(tag, layout, script, frames=200):
    cfg = os.path.join(OUT, "cfg_" + tag)
    shutil.rmtree(cfg, ignore_errors=True)
    os.makedirs(os.path.join(cfg, "gridline"))
    os.makedirs(os.path.join(cfg, "home"))
    with open(os.path.join(cfg, "gridline", "browser-folders.txt"), "w") as f:  # the folder is indexed for search
        f.write(LIB + "\n")
    with open(os.path.join(cfg, "gridline", "settings.json"), "w") as f:
        json.dump({"format": "gridline-settings", "values": {"ui.layoutChosen": True, "ui.layout": layout}}, f)
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


def look(out, frame):
    m = re.search(r"browserlook frame %d: tracker (\d) drag (\S+) \"([^\"]*)\"" % frame, out)
    return (m.group(1) == "1", m.group(2), m.group(3)) if m else None


def row(out, path):
    m = re.search(r"  row \"[^\"]*\" x ([0-9.]+) ([0-9.]+) y ([0-9.]+) ([0-9.]+) path %s\n" % re.escape(path), out)
    if not m:
        return None
    x0, x1, y0, y1 = (float(m.group(i)) for i in range(1, 5))
    return int(x0 + 60), int((y0 + y1) / 2)


START = '5 toasts clear\n8 layout open Browser\n10 browser addtab %s\n' % LIB

# ---- the look is the Tracker's in the Tracker only ----
for lay in ("studio", "live", "logic", "tracker"):
    out = run("look_" + lay, lay, START + "30 browser look\n40 quit discard\n", 60)
    lk = look(out, 30)
    ok(lk is not None and lk[0] == (lay == "tracker"), "%s: the Tracker look is %s" % (lay, "on" if lay == "tracker" else "off"))

# ---- where the rows are (the folder tab shown, as it is once added) ----
out = run("rows", "tracker", START + "12 browser rows on\n30 browser rows\n40 quit discard\n", 60)
kick, snare = row(out, KICK), row(out, SNARE)
ok(kick is not None and snare is not None, "the folder tab lists both samples")
if kick is None or snare is None:
    print("%d passed, %d failed" % (passed, len(failures)))
    sys.exit(1)
kx, ky = kick
sx, sy = snare

script = START + """12 browser look
14 sdlat brsearch
16 sdldown 0
17 sdlup 0
20 text two
26 browser tree
28 sdlat brclear
30 sdldown 0
31 sdlup 0
36 browser tree
38 sdlat brtab1
40 sdldown 0
41 sdlup 0
46 browser tabs
48 sdlat brtab5
50 sdldown 0
51 sdlup 0
56 browser tabs
60 sdlmove {kx} {ky}
62 sdldown 0
63 sdlup 0
68 browser nav
70 key down
74 browser nav
76 browser state
78 sdlmove {kx} {ky}
80 sdldown 0
82 sdlmove {kx} {ky2}
84 sdlmove {kx} {ky3}
86 sdlmove 500 600
88 browser look
90 sdlup 0
94 browser look
96 browser state
98 browser drag "{kick}" 0
100 browser state
104 sdlmove {sx} {sy}
106 sdldown 0
107 sdlup 0
108 sdldown 0
109 sdlup 0
114 browser state
118 sdlmove {kx} {ky}
120 sdldown 1
121 sdlup 1
126 sdlat brfilemenu
128 shot "{shot}"
130 sdldown 0
131 sdlup 0
136 browser state
140 sdlat braddtab
142 sdldown 0
143 sdlup 0
148 sdlat braddfolder
150 key escape
160 quit discard
""".format(kx=kx, ky=ky, ky2=ky + 12, ky3=ky + 40, sx=sx, sy=sy, kick=KICK, shot=os.path.join(OUT, "menu.ppm"))
# Every step from frame 12 on runs 60 frames later, so the added folder is indexed before the search.
WAIT = 60
script = re.sub(r"^(\d+) ", lambda m: "%d " % (int(m.group(1)) + (WAIT if int(m.group(1)) >= 12 else 0)), script, flags=re.M)
out = run("use", "tracker", script, 200 + WAIT)
# (the checks below name the frames as written above)
out = re.sub(r"frame (\d+)", lambda m: "frame %d" % (int(m.group(1)) - (WAIT if int(m.group(1)) >= 12 + WAIT else 0)), out)

lk = look(out, 12)
ok(lk is not None and lk[0], "the Browser in the Tracker draws in the Tracker look")
ok(re.search(r"browsertree frame 26: .*search \"two\"", out) is not None and
   re.search(r"browsertree frame 26:.*\n(  treerow [^\n]*\n)*  treerow [^\n]*path %s\n" % re.escape(SNARE), out) is not None,
   "typing in the search field finds the sample")
ok(re.search(r"browsertree frame 36: .*search \"\"", out) is not None, "the clear button empties the search field")
ok(re.search(r"browsertabs frame 46: count 6 shown 1 ", out) is not None, "clicking the Project tab shows it")
ok(re.search(r"browsertabs frame 56: count 6 shown 5 ", out) is not None, "clicking the folder tab shows it")
m = re.search(r"browsernav frame 68: .* sel \"([^\"]*)\" previewed \"[^\"]*\" previewing (\d)", out)
ok(m is not None and m.group(1) == KICK and m.group(2) == "1", "a click on a sample selects and previews it")
m = re.search(r"browsernav frame 74: .* sel \"([^\"]*)\"", out)
ok(m is not None and m.group(1) == SNARE, "the Down key moves to the next sample")
lk = look(out, 88)
ok(lk is not None and lk[1] == "GL_FILE" and lk[2] == KICK, "a mouse drag from a sample row carries the file")
lk = look(out, 94)
ok(lk is not None and lk[1] == "-", "the drag ends when the button is let go")


def undo_at(frame):
    m = re.search(r"browserstate frame %d: .* undo (\d+) \"([^\"]*)\"" % frame, out)
    return (int(m.group(1)), m.group(2)) if m else (-1, "")


u76, u96, u100, u114, u136 = undo_at(76), undo_at(96), undo_at(100), undo_at(114), undo_at(136)
ok(u96[0] == u76[0] + 1 and u96[1] == "drop from the browser", "the sample dragged onto the pattern drops there (one undo step: %s)" % (u96,))
ok(re.search(r"browserdrag frame 98: .*kick one\.wav -> \S", out) is not None and u100[0] == u96[0] + 1,
   "a sample dropped on an instrument loads there (one undo step: %s)" % (u100,))
ok(u114[0] == u100[0] + 1, "a double-click on a sample loads it (%s)" % (u114,))
ok(re.search(r"sdlat frame 126: brfilemenu at ", out) is not None, "a right-click on a sample opens its menu")
ok(u136[0] == u114[0] + 1, "the menu's Open / add loads the sample (%s)" % (u136,))
ok(re.search(r"sdlat frame 148: braddfolder at ", out) is not None, "the + button opens its menu")

print("%d passed, %d failed" % (passed, len(failures)))
sys.exit(1 if failures else 0)
