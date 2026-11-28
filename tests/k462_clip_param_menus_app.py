#!/usr/bin/env python3
# Gridline - 0.7.9: every parameter control of the Audio clip window (the Sampler's own interface) has the
# right-click parameter menu every plugin control has: reset to default, type a value, the automation items,
# Modulate and the controller link. In the real app (hidden, isolated config folder and HOME, SDL's dummy
# audio driver), on a copy of the test song with a Sampler (Take, channel 60) playing an audio clip (clip 70),
# in each place the window's controls show: Studio's window (Main view, then the Advanced view's tabs), the
# Clips layout's clip band, Smooth's audio editor and the Tracker's Sample editor page:
#   A. a first run finds where each of the Sampler's parameter controls is drawn (tab by tab); every switch,
#      segmented choice and value field (Loop, Ping-pong, Reverse, One shot, the stretch engine, the fit
#      tempo, the processing switches, the filter type, the LFO's target and shape, the FX switches and the
#      room) is among them;
#   B. a second run right-clicks each one found (knobs too) and reads the menu that opens: its rows hold
#      reset to default, type a value, the layout's automation rows, Modulate and the controller link;
#   C. (Studio) Loop switched on, then right-click > Reset to default turns it off in one undo step; undo
#      brings it back on.
#   python3 k462_clip_param_menus_app.py <gridline-app> <test_song.gridline.json> <startup.wav> <out dir>
import json
import os
import re
import shutil
import subprocess
import sys

APP, DEMO, WAV, OUT = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
failures = []
passed = 0


def ok(cond, msg):
    global passed
    if cond:
        passed += 1
        print("ok " + msg)
    else:
        failures.append(msg)
        print("FAIL: " + msg)


shutil.rmtree(OUT, ignore_errors=True)
os.makedirs(OUT)
shutil.copyfile(WAV, os.path.join(OUT, "Take.wav"))
with open(DEMO) as f:
    demo = f.read()
chan = ('{"color": 5219296, "gain": 0.8, "id": 60, "mixerTrack": 0, "muted": false, "name": "Take", "pan": 0, "pitch": 0, '
        '"plugin": {"type": "gl.sampler", "data": %s, "params": []}}, ') % json.dumps(os.path.join(OUT, "Take.wav"))
clip = '{"id": 70, "kind": "audio", "source": 60, "track": 7, "start": 0, "length": 1536, "pattern": 0, "gain": 1.0}, '
demo = demo.replace('"channels": [', '"channels": [' + chan, 1).replace('"playlist": [', '"playlist": [' + clip, 1)
demo = re.sub(r'"nextId": *[0-9]+', '"nextId": 100', demo)
SONG = os.path.join(OUT, "song.gridline.json")
with open(SONG, "w") as f:
    f.write(demo)

# The Sampler's parameters (core/PluginParams.h, enum Sampler) whose controls in the window are switches,
# segmented choices or value fields: none of them had the menu before.
SWITCHES = {7: "Reverse", 8: "Loop", 9: "One shot", 14: "Loop mode", 16: "Invert polarity", 17: "Remove DC", 18: "Swap stereo",
            23: "Stretch engine", 28: "Filter type", 38: "LFO target", 39: "LFO shape", 50: "Boost clips", 59: "Room",
            60: "Stereo fade", 61: "Fit tempo"}
FIELDS = {6: "Start", 11: "End"}  # (the Advanced view's sample start and end fields; the Main view has the handles)
COUNT = 62  # SmpCount


def run(tag, layout, script, frames, view="main", size="1440x900"):
    cfg = os.path.join(OUT, "cfg_" + tag)
    os.makedirs(os.path.join(cfg, "gridline"))
    os.makedirs(os.path.join(cfg, "home"))
    with open(os.path.join(cfg, "gridline", "settings.json"), "w") as f:
        json.dump({"format": "gridline-settings", "values": {"ui.layoutChosen": True, "ui.layoutDev": True, "ui.layout": layout, "keys.profile": layout,
                                                             "ui.menuStyle": layout, "ui.clipWindowView": view, "tracker.sampleView": "advanced"}}, f)
    sp = os.path.join(OUT, tag + ".txt")
    with open(sp, "w") as f:
        f.write(script)
    env = dict(os.environ, SDL_AUDIODRIVER="dummy", GRIDLINE_NO_PLUGIN_SCAN="1", GRIDLINE_HIDE_PLUGINS="all", GRIDLINE_CONFIG_HOME=cfg,
               HOME=os.path.join(cfg, "home"))
    args = [APP, SONG, "--hidden", "--no-ui-sounds", "--no-update-check", "--fixed-dt", "16", "--size", size, "--dev-input", sp, "--frames", str(frames)]
    r = subprocess.run(args, env=env, capture_output=True, text=True, timeout=900)
    with open(os.path.join(OUT, tag + ".log"), "w") as f:
        f.write(r.stdout + "\n---- stderr ----\n" + r.stderr)
    ok(r.returncode == 0, "%s: the app ran (%d)" % (tag, r.returncode))
    return r.stdout


# Each place: (tag, layout setting, view, how it opens, tabs to visit (None: the page as it opens), window size,
# scrolling (None, or the places the pointer wheels at each step, how much and how many steps)). The Clips band is given
# room for every card side by side and the Tracker's band its modules at full height (a short band shows the
# modules' knobs alone); in Smooth the audio editor's own inspector (the sample's and the channel's sections)
# and the Region card's stretch engine are scrolled down step by step.
PLACES = [
    ("studio_main", "studio", "main", "6 clipeditor dbl 70\n", [None], (1440, 900), None),
    ("studio_advanced", "studio", "advanced", "6 clipeditor dbl 70\n", list(range(12)), (1440, 900), None),
    ("clips", "live", "advanced", "6 clipeditor open 70\n14 clipeditor host clips\n", list(range(12)), (1920, 1000), None),
    ("smooth", "logic", "advanced", "6 clipeditor dbl 70\n10 clipeditor unfold\n18 clipeditor host pane\n", [0], (1440, 900),
     ([(560, 780), (300, 700)], -3, 16)),
    ("tracker", "tracker", "advanced", "6 clipeditor dbl 70\n", list(range(12)), (1440, 1300), None),
]
START = 30

AT = re.compile(r"atpid frame (\d+): 60 p(\d+) at (\S+) (\S+)")
MENU = re.compile(r'popuplabels frame \d+ (\S+):([^\n]*)')


def expected_rows(layout):
    # (a description, a test on the rows' labels)
    clips = layout == "live"
    rows = [("reset to default", lambda r: any(x.startswith("Reset to default") for x in r)),
            ("type a value", lambda r: "Type a value..." in r),
            ("Modulate", lambda r: "Modulate" in r)]
    if clips:  # Clips' own terms: show and delete the control's automation, map it to MIDI
        rows += [("show automation", lambda r: "Show automation" in r), ("MIDI map", lambda r: "Edit MIDI map\u2026" in r)]
    else:
        rows += [("create automation clip", lambda r: "Create automation clip" in r), ("edit events", lambda r: "Edit events" in r),
                 ("record automation", lambda r: "Record automation" in r),
                 ("link to controller", lambda r: "Link to controller\u2026" in r or "Edit controller link\u2026" in r)]
    return rows


for tag, layout, view, opener, tabs, (W, H), scroll in PLACES:
    size = "%dx%d" % (W, H)
    steps = [(t, n) for t in tabs for n in range(scroll[2] if scroll else 1)]

    def step_lines(f, t, n):
        # The tab and the scrolling of step (t, n), from frame f: its lines and the frame after them.
        out = ""
        if n == 0 and t is not None:
            out += "%d clipeditor tab %d\n" % (f, t)
            f += 6
        if scroll and n > 0:
            out += "%d closepopups\n" % f
            for (x, y) in scroll[0]:
                out += "%d move %d %d\n%d wheel %d\n" % (f + 1, x, y, f + 2, scroll[1])
                f += 3
            f += 6
        return out, f

    # ---- A: where each parameter control is (and, scrolled, at which step it is in view)
    s = opener
    f = START
    marks = {}  # frame of the atpid lines -> step
    for st in steps:
        lines, f = step_lines(f, *st)
        s += lines + "".join("%d atpid 60 p%d\n" % (f, k) for k in range(COUNT))
        marks[f] = st
        f += 2
    s += "%d quit discard\n" % (f + 2)
    out = run(tag + "_find", layout, s, f + 20, view, size)
    found = set()
    visible = {}  # step -> parameters in view there
    for m in AT.finditer(out):
        fr, k, x, y = int(m.group(1)), int(m.group(2)), float(m.group(3)), float(m.group(4))
        found.add(k)
        if 4 <= x <= W - 4 and 40 <= y <= H - 30:
            visible.setdefault(marks[fr], []).append(k)
    missing = [SWITCHES[k] for k in sorted(SWITCHES) if k not in found]
    ok(not missing, "%s: every switch, segmented choice and value field is drawn as a parameter control (missing: %s)" % (tag, ", ".join(missing) or "none"))
    unreached = sorted(found - {k for v in visible.values() for k in v})
    ok(not unreached, "%s: each of the %d controls comes into view (out of view: %s)" % (tag, len(found), unreached or "none"))

    # ---- B: right-click each one in view and read its menu
    s = opener
    f = START
    probes = set()
    for st in steps:
        lines, f = step_lines(f, *st)
        s += lines
        for k in visible.get(st, []):
            s += "%d atpid 60 p%d\n%d down 1\n%d up 1\n%d popuplabels p%d\n%d closepopups\n" % (f, k, f + 1, f + 2, f + 5, k, f + 6)
            probes.add(k)
            f += 9
    s += "%d quit discard\n" % (f + 2)
    out = run(tag + "_menus", layout, s, f + 20, view, size)
    rows = expected_rows(layout)
    good = set()
    seen = {}
    for m in MENU.finditer(out):
        k = int(m.group(1)[1:])
        r = re.findall(r'"([^"]*)"', m.group(2))
        seen[k] = r
        if all(test(r) for _, test in rows):
            good.add(k)
    bad = ["p%d (%s) lacks %s" % (k, SWITCHES.get(k, FIELDS.get(k, "knob")), "/".join(d for d, test in rows if not test(seen.get(k, []))))
           for k in sorted(probes - good)]
    ok(probes and not bad, "%s: right-click on each of the %d controls opens the parameter menu with every row (%s)" % (tag, len(probes), "; ".join(bad[:12]) or "all"))
    sw = sorted(k for k in good if k in SWITCHES)
    ok(len(sw) == len([k for k in SWITCHES if k in found]), "%s: the switches, choices and fields have it (%d of %d)" % (tag, len(sw), len(SWITCHES)))
    print("%s: menus checked on %s" % (tag, " ".join("p%d" % k for k in sorted(good))))

# ---- C: reset from the menu is one undo step; undo brings the value back (Studio, Main view)
saveA, saveB, saveC = (os.path.join(OUT, n + ".gridline.json") for n in ("on", "reset", "undone"))
s = ("6 clipeditor dbl 70\n30 atpid 60 p8\n31 down 0\n32 up 0\n36 save \"%s\"\n38 atpid 60 p8\n39 down 1\n40 up 1\n43 popuplabels loopmenu\n44 atitem \"Reset to default*\"\n"
     "46 down 0\n47 up 0\n52 save \"%s\"\n54 undo\n58 save \"%s\"\n60 quit discard\n") % (saveA, saveB, saveC)
out = run("undo", "studio", s, 80)


def loop(path):
    try:
        with open(path) as fh:
            song = json.load(fh)
        ch = next(c for c in song["channels"] if c["id"] == 60)
        p = ch["plugin"]["params"]
        return p[8] if len(p) > 8 else 0.0
    except (OSError, ValueError, StopIteration, KeyError):
        return None


m = MENU.search(out)
ok(m is not None and '"Reset to default' in m.group(2) and "not shown" not in out, "the right-click on Loop opened its menu with Reset to default")
ok(loop(saveA) == 1, "a click switches Loop on (%s)" % loop(saveA))
ok(loop(saveB) == 0, "right-click > Reset to default switches it off (%s)" % loop(saveB))
ok(loop(saveC) == 1, "one undo brings it back on (%s)" % loop(saveC))

print("%d passed, %d failed" % (passed, len(failures)))
for m in failures:
    print("FAIL: " + m)
sys.exit(1 if failures else 0)
