#!/usr/bin/env python3
# Gridline - a Tracker Sampler's sounds kept and taken out of a module, and effects over a sample, in
# the real app (hidden, an isolated config folder, HOME and cache, SDL's dummy audio driver):
#   - a .xm and a .mod made here open in the Tracker; an instrument saved as a preset carries its
#     samples; every instrument of the song goes into the library in a folder named after it; the
#     instrument's samples are written as WAV files;
#   - in a fresh config folder (the first run's cache gone), the preset loads into a new Tracker
#     Sampler and plays the original sample (one undo step "load preset");
#   - the Sample editor's Effects: a delay with a tail over the whole sample makes it longer, one undo
#     step "sample effects", undone in one; the box drawn in Dark and Light, at 100 and 200 percent.
#   python3 k435_tracker_samples_app.py <gridline-app> <song.gridline.json> <out dir>
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


def sine(n, period, amp):
    return [int(amp * math.sin(2 * math.pi * i / period)) for i in range(n)]


def make_mod(path):
    b = bytearray(1084)
    b[0:8] = b"k435 mod"
    smps = [("square", [100 if (i // 16) % 2 else -100 for i in range(800)]), ("sine", sine(1200, 40, 100))]
    for k, (name, d) in enumerate(smps):
        sh = 20 + k * 30
        b[sh:sh + len(name)] = name.encode()
        words = len(d) // 2
        b[sh + 22], b[sh + 23], b[sh + 25] = words >> 8, words & 255, 64
    b[950], b[951], b[952] = 1, 127, 0
    b[1080:1084] = b"M.K."
    pat = bytearray(64 * 4 * 4)
    for row, ch, smp in ((0, 0, 1), (8, 1, 2)):
        o = (row * 4 + ch) * 4
        pat[o], pat[o + 1], pat[o + 2] = (smp & 0xF0) | (428 >> 8), 428 & 255, (smp & 15) << 4
    b += pat
    for _, d in smps:
        b += bytes((v & 255) for v in d)
    open(path, "wb").write(b)


def make_xm(path):
    w = bytearray()
    u8 = lambda v: w.append(v & 255)
    u16 = lambda v: w.extend(struct.pack("<H", v & 0xFFFF))
    u32 = lambda v: w.extend(struct.pack("<I", v & 0xFFFFFFFF))
    text = lambda s, n: w.extend(s.encode()[:n].ljust(n, b"\0"))
    text("Extended Module: ", 17), text("k435 xm", 20), u8(0x1A), text("Gridline test", 20), u16(0x0104), u32(276)
    for v in (1, 0, 2, 1, 2, 1, 6, 125):
        u16(v)
    for i in range(256):
        u8(0)
    rows, d = 32, bytearray()
    for r in range(rows):
        for c in range(2):
            d += bytes([49, 1, 0, 0, 0]) if (r, c) == (0, 0) else bytes([52, 2, 0, 0, 0]) if (r, c) == (8, 1) else bytes(5)
    u32(9), u8(0), u16(rows), u16(len(d))
    w.extend(d)
    for name, data in (("Pluck", sine(3000, 50, 20000)), ("Buzz", [12000 if (i // 25) % 2 else -12000 for i in range(2500)])):
        start = len(w)
        u32(263), text(name, 22), u8(0), u16(1), u32(40)
        for i in range(96):
            u8(0)
        env = [(0, 64), (20, 40), (60, 0)]
        for i in range(12):
            x, y = env[i] if i < len(env) else (0, 0)
            u16(x), u16(y)
        for i in range(24):
            u16(0)
        for v in (len(env), 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0):
            u8(v)
        u16(256)
        while len(w) < start + 263:
            u8(0)
        u32(len(data) * 2), u32(0), u32(0), u8(64), u8(0), u8(16), u8(128), u8(0), u8(0), text(name.lower(), 22)
        prev = 0
        for v in data:
            u16(v - prev)
            prev = v
    open(path, "wb").write(w)


shutil.rmtree(OUT, ignore_errors=True)
MODS = os.path.join(OUT, "modules")
os.makedirs(MODS)
XM, MOD = os.path.join(MODS, "k435 test.xm"), os.path.join(MODS, "k435 test.mod")
make_xm(XM)
make_mod(MOD)
E = "tracker editors"


def run(tag, script, frames, cfg=None, size="1440x900", extra=()):
    cfg = cfg or os.path.join(OUT, "cfg_" + tag)
    if not os.path.isdir(cfg):
        os.makedirs(os.path.join(cfg, "gridline"))
        os.makedirs(os.path.join(cfg, "home"))
        with open(os.path.join(cfg, "gridline", "settings.json"), "w") as f:
            json.dump({"format": "gridline-settings", "values": {"ui.layoutChosen": True, "ui.layoutDev": True, "ui.layout": "tracker", "keys.profile": "tracker",
                                                                  "ui.menuStyle": "tracker"}}, f)
    song = os.path.join(cfg, "song.gridline.json")
    shutil.copyfile(DEMO, song)
    sp = os.path.join(OUT, tag + ".txt")
    with open(sp, "w") as f:
        f.write(script)
    env = dict(os.environ, SDL_AUDIODRIVER="dummy", GRIDLINE_NO_PLUGIN_SCAN="1", GRIDLINE_CONFIG_HOME=cfg, HOME=os.path.join(cfg, "home"),
               GRIDLINE_CACHE_DIR=os.path.join(cfg, "cache"))
    r = subprocess.run([APP, song, "--hidden", "--no-ui-sounds", "--no-update-check", "--fixed-dt", "16", "--size", size, "--dev-input", sp,
                        "--frames", str(frames)] + list(extra), env=env, capture_output=True, text=True, timeout=600)
    with open(os.path.join(OUT, tag + ".log"), "w") as f:
        f.write(r.stdout + "\n---- stderr ----\n" + r.stderr)
    if r.returncode != 0:
        fail("%s: gridline-app failed (%d)" % (tag, r.returncode))
    return r.stdout, cfg


def dump(out, frame):
    m = re.search(r"tracker files frame %d: dump\n((?:  channel [^\n]*\n)*)" % frame, out)
    if not m:
        return None
    rows = []
    for line in m.group(1).splitlines():
        mm = re.match(r"  channel (\d+) \"([^\"]*)\" playable (\d+) files(.*)", line)
        if mm:
            rows.append((int(mm.group(1)), mm.group(2), int(mm.group(3)), [f for f in mm.group(4).split(" |") if f.strip()]))
    return rows


EXPORT = os.path.join(OUT, "export", "pluck.wav")
# ---- the module's sounds into the library, and as files ----
script = """2 loadhold on
10 open "{xm}"
40 {E} files dump
42 {E} files save 0 Pluck lead
44 {E} files all
46 {E} files export 0 {exp}
48 {E} files dump
60 quit discard
""".format(xm=XM, E=E, exp=EXPORT)
out, cfgA = run("save", script, 70)
d = dump(out, 40)
ok(d is not None and len(d) == 2 and all(r[2] == 1 for r in d), "the .xm opens as two Tracker Samplers that play their samples (%s)" % (d,))
cache = os.path.join(cfgA, "cache")
ok(d is not None and d and d[0][3] and d[0][3][0].startswith(cache), "their samples are in the cache (%s)" % (d and d[0][3]))
ok("tracker files frame 42: save ok" in out, "Save as preset")
presets = os.path.join(cfgA, "gridline", "presets", "gl.tracker")
pj = os.path.join(presets, "Pluck lead.json")
data = json.load(open(pj)).get("data", "") if os.path.isfile(pj) else ""
ok(data.split("\n")[0] == "Pluck lead samples/instrument001-sample00.wav", "the preset names its sample beside it (%r)" % data.split("\n")[0])
ok(os.path.isfile(os.path.join(presets, "Pluck lead samples", "instrument001-sample00.wav")), "the preset's sample is copied beside it")
ok("tracker files frame 44: all 2" in out, "Save every instrument as a preset: both")
lib = os.path.join(presets, "k435 xm")
ok(os.path.isfile(os.path.join(lib, "01 Pluck.json")) and os.path.isfile(os.path.join(lib, "02 Buzz.json")), "in a folder named after the song (%s)" %
   (os.listdir(lib) if os.path.isdir(lib) else None))
ok("tracker files frame 46: export 1" in out and os.path.getsize(EXPORT) > 3000 * 2, "Export samples as WAV: the sample's file")
# The .mod too.
out, cfgM = run("mod", """2 loadhold on
10 open "{mod}"
40 {E} files dump
42 {E} files all
60 quit discard
""".format(mod=MOD, E=E), 70)
d = dump(out, 40)
ok(d is not None and len(d) == 2 and all(r[2] == 1 for r in d), "the .mod opens as two Tracker Samplers that play (%s)" % (d,))
ok("tracker files frame 42: all 2" in out and os.path.isdir(os.path.join(cfgM, "gridline", "presets", "gl.tracker", "k435 mod")), "its instruments into the library")

# ---- a fresh config folder: only the presets, no cache ----
cfgB = os.path.join(OUT, "cfg_fresh")
os.makedirs(os.path.join(cfgB, "gridline"))
os.makedirs(os.path.join(cfgB, "home"))
shutil.copyfile(os.path.join(cfgA, "gridline", "settings.json"), os.path.join(cfgB, "gridline", "settings.json"))
shutil.copytree(os.path.join(cfgA, "gridline", "presets"), os.path.join(cfgB, "gridline", "presets"))
shutil.rmtree(cache, ignore_errors=True)
script = """5 toasts clear
6 {E} open frame 0
8 replace gl.tracker
20 {E} files dump
22 {E} files load 0 Pluck lead
30 {E} files dump
32 {E} open sample 0
40 {E} dump
42 {E} files load 0 k435 xm/02 Buzz
50 {E} files dump
60 quit discard
""".format(E=E)
out, _ = run("fresh", script, 70, cfg=cfgB)
d0, d1, d2 = dump(out, 20), dump(out, 30), dump(out, 50)
ok(d0 is not None and d0 and d0[0][2] == 0, "a new Tracker Sampler has no sample (%s)" % (d0,))
ok(d1 is not None and d1 and d1[0][2] == 1 and d1[0][3][0].startswith(os.path.join(cfgB, "gridline", "presets")),
   "the preset loads in a fresh config folder and plays its own copy of the sample (%s)" % (d1,))
m = re.search(r"tracker editors frame 40: .* frames (\d+) .* undo \"([^\"]*)\"", out)
ok(m is not None and int(m.group(1)) == 3000 and m.group(2) == "load preset", "the Sample editor shows the original 3000 frames; one undo step (%s)" % (m and m.groups(),))
ok(d2 is not None and d2 and d2[0][2] == 1 and "02 Buzz samples" in d2[0][3][0], "a preset from the song's folder loads too (%s)" % (d2,))

# ---- effects over the sample ----
script = """2 loadhold on
10 open "{xm}"
30 {E} open sample 0
34 {E} dump
36 {E} fx dump
38 {E} fx add gl.delay
39 {E} fx tail 20
40 {E} fx apply
44 {E} dump
46 undo
50 {E} dump
54 quit discard
""".format(xm=XM, E=E)
out, _ = run("fx", script, 60)
m0 = re.search(r"tracker editors frame 34: .* frames (\d+) ", out)
m1 = re.search(r"tracker editors frame 44: .* frames (\d+) .* undo \"([^\"]*)\"", out)
m2 = re.search(r"tracker editors frame 50: .* frames (\d+) ", out)
mf = re.search(r"tracker editors frame 36: fx rows 0 sel 0 tail 0 effects (\d+)", out)
ok(mf is not None and int(mf.group(1)) >= 10, "the Effects box offers Gridline's effects (%s)" % (mf and mf.group(1)))
ok("fx apply ok" in out, "Apply runs the delay")
ok(m0 and m1 and int(m1.group(1)) > int(m0.group(1)) and m1.group(2) == "sample effects", "the delay's tail makes the sample longer; one undo step \"sample effects\" (%s %s)" %
   (m0 and m0.group(1), m1 and m1.groups()))
ok(m0 and m2 and m2.group(1) == m0.group(1), "one undo brings the sample back (%s)" % (m2 and m2.group(1)))

# ---- the box, both themes, both scales: every effect's parameter names fit in full ----
out, _ = run("fit", """2 loadhold on
10 open "{xm}"
30 {E} open sample 0
34 {E} fx fit
38 quit discard
""".format(xm=XM, E=E), 42)
m = re.search(r"tracker editors frame 34: fx fit effects (\d+) two (\d+) one (\d+) short (\d+) widest (\S+)", out)
ok(m is not None and int(m.group(4)) == 0, "every offered effect's names and values fit in its settings grid (%s)" % (m and m.group(0)))
WIDE = m.group(5) if m else "gl.reverb"
for tag, size, extra in (("box_dark", "1440x900", ()), ("box_light2x", "2880x1800", ("--ui-scale", "2", "--light"))):
    shot, shotw = os.path.join(OUT, tag + ".ppm"), os.path.join(OUT, tag + "_widest.ppm")
    script = """2 loadhold on
10 open "{xm}"
30 {E} open sample 0
34 {E} range 400 2200
36 {E} fx add gl.delay
37 {E} fx add gl.reverb
38 {E} fx add gl.chorus
39 {E} fx add {wide}
40 {E} fx sel 1
41 {E} fx tail 15
42 {E} fx show
43 {E} fx fit
44 tracker labels on
56 tracker labels
58 shot "{shot}"
60 {E} fx sel 3
70 shot "{shotw}"
74 quit discard
""".format(xm=XM, E=E, shot=shot, shotw=shotw, wide=WIDE)
    out, _ = run(tag, script, 78, size=size, extra=extra)
    m = re.search(r"tracker labels frame 56: drawn (\d+) clipped (\d+) tight \d+([^\n]*)", out)
    ok(m is not None and int(m.group(1)) >= 8 and int(m.group(2)) == 0, "%s: every button label in the Effects box fits (%s)" % (tag, m.group(0) if m else None))
    m = re.search(r"tracker editors frame 43: fx fit .* short (\d+)", out)
    ok(m is not None and m.group(1) == "0", "%s: at this size every effect's names fit in full (%s)" % (tag, m and m.group(0)))
    ok(os.path.isfile(shot) and os.path.isfile(shotw), "%s: the box drawn, with Reverb and with %s (%s)" % (tag, WIDE, shot))

print("%d passed, %d failed" % (passed, len(failures)))
sys.exit(1 if failures else 0)
