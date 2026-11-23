#!/usr/bin/env python3
# Gridline - 0.7.8: Seltmann's own screens in the Tracker layout, used as a person uses them (hidden
# app, an isolated config folder and HOME, SDL's dummy audio driver, real SDL mouse events and keys):
#   - with a Seltmann as the current instrument the header's Sample button opens its Chop screen in the
#     lower band (the waveform with its regions, the sequence, the band's buttons); Exit, Esc, the lit
#     button and Ctrl+Alt+E close and open it;
#   - the Chop screen's keys: Right picks the next region, Alt+Up raises its pitch (one undo step, undo
#     and redo), Alt+5 chops in eighths, Up / Down walk the steps; Split 2 splits the region;
#   - Record take opens the recorder in the well; Send to project links a pattern;
#   - the Instrument screen: a group, a layout applied with Enter (one undo step), the base note on
#     Alt+Up; another instrument and back keeps the chop;
#   - the Chop page: its tab shows only while the project has a Seltmann; the page draws its parts;
#   - every button label fits (the tracker labels watch) on the Chop screen, the Instrument screen and
#     the Chop page at three band heights, at 100 % and 200 %, in both themes; a picture of each.
#   python3 k078_seltmann_tracker_app.py <gridline-app> <gridline_test_seltmann> <song.gridline.json> <out dir>
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import zlib

APP, TESTBIN, DEMO, OUT = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
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


def ppm_to_png(src, dst):
    with open(src, "rb") as f:
        data = f.read()
    parts = data.split(maxsplit=4)
    w, h = int(parts[1]), int(parts[2])
    px = parts[4]
    raw = b"".join(b"\x00" + px[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

    with open(dst, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


shutil.rmtree(OUT, ignore_errors=True)
os.makedirs(OUT)
BREAK = os.path.join(OUT, "Break 96.wav")
r = subprocess.run([TESTBIN, "writeloop", BREAK], capture_output=True, text=True)
if r.returncode != 0 or not os.path.exists(BREAK):
    print("FAIL: could not write the synthetic break")
    sys.exit(1)
E = "tracker editors"
S = "tracker editors selt"


def run(tag, script, frames, size="1440x960", extra=()):
    cfg = os.path.join(OUT, "cfg_" + tag)
    shutil.rmtree(cfg, ignore_errors=True)
    os.makedirs(os.path.join(cfg, "gridline"))
    os.makedirs(os.path.join(cfg, "home"))
    with open(os.path.join(cfg, "gridline", "settings.json"), "w") as f:
        json.dump({"format": "gridline-settings", "values": {"ui.layoutChosen": True, "ui.layoutDev": True, "ui.layout": "tracker", "keys.profile": "tracker",
                                                              "ui.menuStyle": "tracker"}}, f)
    song = os.path.join(cfg, "song.gridline.json")
    shutil.copyfile(DEMO, song)
    sp = os.path.join(OUT, tag + ".txt")
    with open(sp, "w") as f:
        f.write(script.replace("@OUT@", OUT))
    env = dict(os.environ, SDL_AUDIODRIVER="dummy", GRIDLINE_NO_PLUGIN_SCAN="1", GRIDLINE_CONFIG_HOME=cfg, HOME=os.path.join(cfg, "home"),
               GRIDLINE_SELTMANN_DEMO=BREAK)
    r = subprocess.run([APP, song, "--hidden", "--no-ui-sounds", "--no-update-check", "--fixed-dt", "16", "--size", size, "--dev-input", sp,
                        "--frames", str(frames)] + list(extra), env=env, capture_output=True, text=True, timeout=600)
    with open(os.path.join(OUT, tag + ".log"), "w") as f:
        f.write(r.stdout + "\n---- stderr ----\n" + r.stderr)
    if r.returncode != 0:
        fail("%s: gridline-app failed (%d)" % (tag, r.returncode))
    return r.stdout


def selt(out, frame):
    m = re.search(r"tracker selt frame %d: screen (\S*) ready (\d) regions (\d+) steps (\d+) region (-?\d+) step (-?\d+) scope (\S+) tool (\d+) pitch (\S+) "
                  r"chop (-?\d+) preset \"([^\"]*)\" group (\d+) cursor \"([^\"]*)\" playing (\d) linked (\d+) recorder (\d) modal (\d) takes (\d+) base (\d+) "
                  r"undo (\d+) \"([^\"]*)\" bytes (\d+)" % frame, out)
    if not m:
        return None
    g = m.groups()
    d = {"screen": g[0], "ready": int(g[1]), "regions": int(g[2]), "steps": int(g[3]), "region": int(g[4]), "step": int(g[5]), "scope": g[6],
         "tool": int(g[7]), "pitch": float(g[8]), "chop": int(g[9]), "preset": g[10], "group": int(g[11]), "cursor": g[12], "playing": int(g[13]),
         "linked": int(g[14]), "recorder": int(g[15]), "modal": int(g[16]), "takes": int(g[17]), "base": int(g[18]), "undo": int(g[19]), "label": g[20],
         "bytes": int(g[21])}
    p = re.search(r"tracker selt frame %d: [^\n]*\ntracker selt order: [^\n]*\ntracker selt parts: ([^\n]*)" % frame, out)
    d["parts"] = {}
    if p:
        for part in p.group(1).split("; "):
            f = part.split()
            if len(f) == 5:
                d["parts"][f[0]] = tuple(float(x) for x in f[1:])
    return d


def band(out, frame):
    m = re.search(r"tracker windows frame %d: [^\n]* band (\S+)" % frame, out)
    return m.group(1) if m else None


START = "5 toasts clear\n6 %s open frame 0\n8 %s menu at Page Chop\n10 replace gl.seltmann\n12 %s close\n" % (E, E, E)

# ---- the Chop screen: open, keys, edits, undo, close ----
script = START + """16 {E} menu at Sample button
18 sdldown 0
19 sdlup 0
20 waitfor seltmann-ready 120
24 {E} windows
26 {S} dump
28 {E} open sample 0
30 sdlkey right down
31 sdlkey right up
34 {S} dump
36 sdlkey up down alt
37 sdlkey up up alt
38 sdlkey #226 up
42 {S} dump
44 undo
48 {S} dump
50 redo
54 {S} dump
56 sdlkey 5 down alt
57 sdlkey 5 up alt
58 sdlkey #226 up
62 {S} dump
64 sdlkey down down
65 sdlkey down up
66 sdlkey down down
67 sdlkey down up
70 {S} dump
72 {S} at ssplit2
74 sdldown 0
75 sdlup 0
78 {S} dump
80 {S} at sexit
82 sdldown 0
83 sdlup 0
86 {E} windows
88 tracker focus
90 sdlkey e down ctrl+alt
91 sdlkey e up ctrl+alt
92 sdlkey #224 up
92 sdlkey #226 up
96 {E} windows
98 {E} open sample 0
100 sdlkey escape down
101 sdlkey escape up
104 {E} windows
106 {E} menu at Sample button
108 sdldown 0
109 sdlup 0
112 {E} windows
114 {E} menu at Sample button
116 sdldown 0
117 sdlup 0
120 {E} windows
122 {E} menu at Sample button
124 sdldown 0
125 sdlup 0
128 {S} at ssend
130 sdldown 0
131 sdlup 0
136 {S} dump
138 {S} at stake
140 sdldown 0
141 sdlup 0
146 {S} dump
150 quit discard
""".format(E=E, S=S)
out = run("chop", script, 160)
ok("menu row \"Page Chop\" none" in out, "the Chop page's tab is not in the strip while the project has no Seltmann")
ok(band(out, 24) == "chop", "the header's button (Chop for a Seltmann) opens the Chop screen in the lower band (%s)" % band(out, 24))
d0 = selt(out, 26)
ok(d0 is not None and d0["screen"] == "chop" and d0["ready"] == 1 and d0["regions"] >= 4 and d0["steps"] >= 4,
   "the Chop screen shows the chopped break: regions and steps (%s)" % (d0 and {k: d0[k] for k in ("screen", "ready", "regions", "steps")}))
for part in ("title", "well", "sequence", "panel", "status"):
    ok(d0 is not None and part in d0["parts"], "the Chop screen draws its %s" % part)
d = selt(out, 34)
ok(d is not None and d["region"] == 1, "Right picks the next region (%s)" % (d and d["region"]))
d42 = selt(out, 42)
ok(d42 is not None and abs(d42["pitch"] - 1.0) < 0.01 and d42["undo"] == d0["undo"] + 1 and d42["label"] == "Region pitch",
   "Alt+Up raises the region's pitch a semitone: one undo step \"Region pitch\" (%s)" % (str(d42 and (d42["pitch"], d42["undo"], d42["label"])),))
d = selt(out, 48)
ok(d is not None and abs(d["pitch"]) < 0.01 and d["bytes"] == d0["bytes"], "undo takes the pitch back, the saved bytes as before (%s)" % (d and d["pitch"]))
d = selt(out, 54)
ok(d is not None and abs(d["pitch"] - 1.0) < 0.01 and d["bytes"] == d42["bytes"], "redo puts it back, the same bytes (%s)" % (d and d["pitch"]))
d62 = selt(out, 62)
ok(d62 is not None and d62["chop"] == 4, "Alt+5 chops in eighths (%s)" % (d62 and d62["chop"]))
d = selt(out, 70)
ok(d is not None and d["step"] == 1 and d["scope"] == "step", "Down, Down: the second step, the values edit it (%s)" % (str(d and (d["step"], d["scope"])),))
d78 = selt(out, 78)
ok(d78 is not None and d62 is not None and d78["regions"] == d62["regions"] + 1, "Split 2 splits the region (%s -> %s)" % (d62 and d62["regions"], d78 and d78["regions"]))
ok(band(out, 86) == "closed", "Exit closes the screen (%s)" % band(out, 86))
ok(band(out, 96) == "chop", "Ctrl+Alt+E opens it again (%s)" % band(out, 96))
ok(band(out, 104) == "closed", "Esc closes it (%s)" % band(out, 104))
ok(band(out, 112) == "chop" and band(out, 120) == "closed", "the lit button closes it (%s %s)" % (band(out, 112), band(out, 120)))
d = selt(out, 136)
ok(d is not None and d["linked"] >= 1, "Send to project: a pattern linked to the sequence (%s)" % (d and d["linked"]))
d = selt(out, 146)
ok(d is not None and d["recorder"] == 1, "Record take: the recorder in the well (%s)" % (d and d["recorder"]))

# ---- the Instrument screen: a layout applied by key, the base note, another instrument and back ----
script = START + """16 {E} open instrument 0
20 waitfor seltmann-ready 120
24 {E} windows
26 {S} group 2
28 {S} dump
30 {E} open instrument 0
32 sdlkey down down
33 sdlkey down up
36 sdlkey return down
37 sdlkey return up
42 {S} dump
44 sdlkey up down alt
45 sdlkey up up alt
46 sdlkey #226 up
50 {S} dump
52 undo
53 undo
56 {S} dump
58 {E} open instrument 1
62 {E} windows
64 {E} open sample 0
68 {E} windows
70 {S} dump
72 {E} menu at Page Chop
74 sdldown 0
75 sdlup 0
80 {S} dump
84 quit discard
""".format(E=E, S=S)
out = run("inst", script, 90)
ok(band(out, 24) == "seltinst", "Ctrl+Alt+I's screen for a Seltmann: its Instrument screen (%s)" % band(out, 24))
d28 = selt(out, 28)
for part in ("title", "sound", "layouts", "grid", "keys", "status"):
    ok(d28 is not None and part in d28["parts"], "the Instrument screen draws its %s" % part)
d42 = selt(out, 42)
ok(d42 is not None and d28 is not None and d42["preset"] == d42["cursor"] and d42["preset"] != "" and d42["undo"] == d28["undo"] + 1,
   "Down then Enter applies the layout under the cursor: one undo step (%s)" % (str(d42 and (d42["preset"], d42["cursor"], d42["undo"], d42["label"])),))
d50 = selt(out, 50)
ok(d50 is not None and d28 is not None and d50["base"] == d28["base"] + 1, "Alt+Up raises the base note (%s)" % (d50 and d50["base"]))
d = selt(out, 56)
ok(d is not None and d28 is not None and d["bytes"] == d28["bytes"] and d["base"] == d28["base"], "two undos: the layout and the base note back (%s)" % (d and d["preset"]))
# (another kind of instrument's editor floats in its window, nothing in the band; back to the Seltmann: its Chop screen)
ok(band(out, 62) == "closed" and re.search(r"tracker windows frame 62: [^\n]* plugin open", out) is not None and band(out, 68) == "chop",
   "another instrument (its window floats), then back: the Chop screen (%s %s)" % (band(out, 62), band(out, 68)))
d = selt(out, 70)
ok(d is not None and d28 is not None and d["regions"] == d28["regions"], "the chop is as it was (%s)" % (d and d["regions"]))
ok(re.search(r"menu row \"Page Chop\" at \d+ \d+", out) is not None, "with a Seltmann the strip shows the Chop page's tab")
d = selt(out, 80)
ok(d is not None and d["screen"] == "page" and all(p in d["parts"] for p in ("library", "wave", "region", "panel", "keys", "sequence", "takes", "loops")),
   "the Chop page: the library, the waveform, the region box, the panel, the key map, the sequence, takes and loops (%s)" % (d and sorted(d["parts"])))

# ---- a collaboration session: an edit on the Chop screen reaches a collaborator in Studio ----
D = os.path.join(OUT, "collab")
os.makedirs(D)
side_a = """5 collab profile Alex
10 collab new "{D}/Alex/Song"
20 collab invite "{D}/invite.txt"
21 waitfor collab-invite-ready 30
30 waitfor collab-connected 120
31 waitfor collab-synced 60
32 collab edit Lead
33 waitfor collab-synced 60
34 waitfor never 3
36 {E} open frame Lead
38 replace gl.seltmann
40 {E} open sample Lead
42 waitfor seltmann-ready 120
44 waitfor collab-synced 60
46 waitfor never 4
48 {E} open sample Lead
50 sdlkey right down
51 sdlkey right up
52 sdlkey up down alt
53 sdlkey up up alt
54 sdlkey #226 up
56 waitfor collab-synced 60
58 waitfor never 6
60 {S} dump
62 collab report "{D}/a.json"
64 waitfor "file:{D}/b.json" 120
70 quit discard
""".format(D=D, E=E, S=S)
side_b = """5 collab profile Sam
10 collab join "{D}/invite.txt" "{D}/Sam/Song"
11 waitfor collab-joined 120
12 waitfor collab-synced 60
13 waitfor collab-channel:Lead 90
14 waitfor "file:{D}/a.json" 240
16 waitfor collab-synced 60
18 waitfor never 6
20 collab report "{D}/b.json"
24 quit discard
""".format(D=D)
procs = []
for who, script, layout, extra_env in (("a", side_a, "tracker", {"GRIDLINE_SELTMANN_DEMO": BREAK}), ("b", side_b, "studio", {})):
    cfg = os.path.join(D, "cfg" + who)
    os.makedirs(os.path.join(cfg, "gridline"))
    os.makedirs(os.path.join(D, "home" + who))
    with open(os.path.join(cfg, "gridline", "settings.json"), "w") as f:
        json.dump({"format": "gridline-settings", "values": {"ui.layoutChosen": True, "ui.layoutDev": True}}, f)
    sp = os.path.join(D, who + ".txt")
    with open(sp, "w") as f:
        f.write(script)
    env = dict(os.environ, SDL_AUDIODRIVER="dummy", GRIDLINE_NO_PLUGIN_SCAN="1", GRIDLINE_CONFIG_HOME=cfg, HOME=os.path.join(D, "home" + who), **extra_env)
    log = open(os.path.join(D, who + ".log"), "w")
    procs.append((who, subprocess.Popen([APP, "--hidden", "--no-ui-sounds", "--no-update-check", "--layout", layout, "--dev-input", sp], env=env, stdout=log,
                                        stderr=subprocess.STDOUT, text=True), log))
for who, p, log in procs:
    try:
        rc = p.wait(timeout=600)
    except subprocess.TimeoutExpired:
        p.kill()
        rc = -1
    log.close()
    ok(rc == 0, "collab: side %s exits cleanly (%d)" % (who, rc))
ra = json.load(open(os.path.join(D, "a.json"))) if os.path.exists(os.path.join(D, "a.json")) else {}
rb = json.load(open(os.path.join(D, "b.json"))) if os.path.exists(os.path.join(D, "b.json")) else {}
outa = open(os.path.join(D, "a.log")).read()
m = re.search(r"tracker selt frame 60: [^\n]* pitch (\S+) ", outa)
ok(m is not None and abs(float(m.group(1)) - 1.0) < 0.01, "collab: Alt+Up on the Chop screen raised the region's pitch (%s)" % (m and m.group(1)))
ok(ra.get("song") and ra.get("song") == rb.get("song"), "collab: the collaborator in Studio has the same song, the Seltmann edit included")

# ---- labels, band heights and pictures: three heights, 100 % and 200 %, both themes ----
heights = {}
for size, scale in (("1024x640", 1), ("1440x960", 1), ("1600x1100", 1), ("2048x1280", 2), ("2880x1920", 2), ("3200x2200", 2)):
    for theme in ("dark", "light"):
        tag = "labels_%s_x%d_%s" % (size, scale, theme)
        script = START + """14 toasts clear
16 {E} open sample 0
20 waitfor seltmann-ready 120
22 tracker labels on
30 tracker labels
31 {S} dump
32 shot "@OUT@/{tag}_chop.ppm"
36 {E} open instrument 0
44 tracker labels
45 {S} dump
46 shot "@OUT@/{tag}_instrument.ppm"
50 {E} close
52 tracker page Chop
60 tracker labels
61 {S} dump
62 shot "@OUT@/{tag}_page.ppm"
66 quit discard
""".format(E=E, S=S, T=theme, tag=tag)
        out = run(tag, script, 70, size, (("--ui-scale", str(scale)) if scale != 1 else ()) + (("--light",) if theme == "light" else ()))
        for f, what in ((30, "Chop screen"), (44, "Instrument screen"), (60, "Chop page")):
            m = re.search(r"tracker labels frame %d: drawn (\d+) clipped (\d+) tight \d+([^\n]*)" % f, out)
            ok(m is not None and int(m.group(1)) >= 15 and int(m.group(2)) == 0, "%s %s: every button label fits (%s)" % (tag, what, m.group(0) if m else None))
        for f, what in ((31, "chop"), (45, "instrument")):
            d = selt(out, f)
            if d and "title" in d["parts"] and "status" in d["parts"]:
                h = d["parts"]["status"][3] - d["parts"]["title"][1]
                heights["%s %s" % (tag, what)] = h
                inside = all(p[3] <= d["parts"]["status"][3] + 0.5 and p[1] >= d["parts"]["title"][1] - 0.5 for p in d["parts"].values())
                ok(inside, "%s %s: every part inside the band (%.0f px)" % (tag, what, h))
            else:
                fail("%s %s: no parts dumped" % (tag, what))
        for k in ("chop", "instrument", "page"):
            p = os.path.join(OUT, "%s_%s.ppm" % (tag, k))
            if os.path.exists(p):
                ppm_to_png(p, p[:-4] + ".png")
                os.remove(p)
            else:
                fail("%s: no picture of the %s" % (tag, k))
for k in sorted(heights):
    print("band %s: %.0f px" % (k, heights[k]))

print("%d passed, %d failed" % (passed, len(failures)))
sys.exit(1 if failures else 0)
