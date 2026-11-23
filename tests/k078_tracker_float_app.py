#!/usr/bin/env python3
# Gridline 0.7.8 - in the Tracker every plugin's editor and settings window floats, as in Studio
# (the real app: hidden, an isolated config folder and HOME, SDL's dummy audio driver):
#   * Channel settings of every built-in instrument of the fixture (Tercet, the drums, the keys, the
#     pads), Seltmann, a Tracker Sampler and a third-party plugin's stand-in (not installed) open the
#     floating channel settings window; a Sampler's open the floating Audio clip window;
#   * a double-click in the instrument list, "Open plugin window" and the Instrument button for an
#     instrument without a Tracker screen do the same;
#   * a mixer effect opens its floating effect window, the Master chain its own;
#   * never once does the lower band (tracker.lower) draw a plugin, and the Tracker's own screens
#     (a Tracker Sampler's Instrument editor, a Sampler's Sample editor) still open there.
#   python3 k078_tracker_float_app.py <gridline-app> <test_song.gridline.json> <wav> <out dir>
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
# channels 0..3: a Sampler, Seltmann, a third-party plugin's stand-in, a Tracker Sampler; then the fixture's
chan = ('{"color": 5219296, "gain": 0.8, "id": 60, "mixerTrack": 0, "muted": false, "name": "Take", "pan": 0, "pitch": 0, '
        '"plugin": {"type": "gl.sampler", "data": %s, "params": []}}, '
        '{"color": 14251600, "gain": 0.8, "id": 61, "mixerTrack": 0, "muted": false, "name": "Chopper", "pan": 0, "pitch": 0, '
        '"plugin": {"type": "gl.seltmann", "data": "", "params": []}}, '
        '{"color": 11251600, "gain": 0.8, "id": 62, "mixerTrack": 0, "muted": false, "name": "Outer Synth", "pan": 0, "pitch": 0, '
        '"plugin": {"type": "vst3:Outer Synth", "data": "", "params": []}}, '
        '{"color": 9251600, "gain": 0.8, "id": 63, "mixerTrack": 0, "muted": false, "name": "Tracked", "pan": 0, "pitch": 0, '
        '"plugin": {"type": "gl.tracker", "data": "", "params": []}}, ') % json.dumps(os.path.join(OUT, "Take.wav"))
demo = demo.replace('"channels": [', '"channels": [' + chan, 1)
demo = re.sub(r'"nextId": *[0-9]+', '"nextId": 100', demo)
SONG = os.path.join(OUT, "song.gridline.json")
with open(SONG, "w") as f:
    f.write(demo)
song = json.loads(demo)
song = song.get("song", song)
channels = song["channels"]
fxSlots = [(mi, k) for mi, m in enumerate(song["mixer"]) for k, sl in enumerate(m.get("slots", [])) if sl and sl.get("plugin", {}).get("type")]


def run(tag, script, frames, extra=()):
    cfg = os.path.join(OUT, "cfg_" + tag)
    os.makedirs(os.path.join(cfg, "gridline"))
    os.makedirs(os.path.join(cfg, "home"))
    with open(os.path.join(cfg, "gridline", "settings.json"), "w") as f:
        json.dump({"format": "gridline-settings", "values": {"ui.layoutChosen": True, "ui.layoutDev": True, "ui.layout": "tracker", "keys.profile": "tracker",
                                                             "ui.menuStyle": "tracker"}}, f)
    sp = os.path.join(OUT, tag + ".txt")
    with open(sp, "w") as f:
        f.write(script)
    env = dict(os.environ, SDL_AUDIODRIVER="dummy", GRIDLINE_NO_PLUGIN_SCAN="1", GRIDLINE_HIDE_PLUGINS="all", GRIDLINE_CONFIG_HOME=cfg,
               HOME=os.path.join(cfg, "home"))
    args = [APP, SONG, "--hidden", "--no-ui-sounds", "--no-update-check", "--fixed-dt", "16", "--size", "1440x900", "--dev-input", sp, "--frames", str(frames)]
    args += list(extra)
    r = subprocess.run(args, env=env, capture_output=True, text=True, timeout=600)
    with open(os.path.join(OUT, tag + ".log"), "w") as f:
        f.write(r.stdout + "\n---- stderr ----\n" + r.stderr)
    ok(r.returncode == 0, "%s: the app ran (%d)" % (tag, r.returncode))
    return r.stdout


FLOAT = re.compile(r"tracker float frame (\d+): settings (\S+) fx (\S+) clip (\S+) master (\S+) hosted (\d+) band (\S+) foreign (\d+)")
E = "tracker editors"


def floats(out):
    return {int(m.group(1)): {"settings": m.group(2), "fx": m.group(3), "clip": m.group(4), "master": m.group(5), "band": m.group(7),
                              "foreign": int(m.group(8))} for m in FLOAT.finditer(out)}


# ---- each way in, one after another in one run: open, report, close ----
cases = []  # (name, how, the window that must float)
for i, c in enumerate(channels):
    t = c["plugin"]["type"]
    cases.append(("Channel settings of %s (%s)" % (c["name"], t), "plugwin chan %d" % i, "clip" if t == "gl.sampler" else "settings"))
cases.append(("Open plugin window (Tercet)", "%s plugin %d" % (E, next(i for i, c in enumerate(channels) if c["plugin"]["type"] == "gl.synth")), "settings"))
cases.append(("Open plugin window (the stand-in)", "%s plugin 2" % E, "settings"))
cases.append(("the Instrument button (a synth)", "%s open instrument %d" % (E, next(i for i, c in enumerate(channels) if c["plugin"]["type"] == "gl.synth")),
              "settings"))
for mi, k in fxSlots:
    cases.append(("the mixer effect %d/%d" % (mi, k), "plugwin fx %d %d" % (mi, k), "fx"))
s = "3 toasts clear\n"
f = 8
at = []
for name, how, win in cases:
    s += "%d %s\n%d %s float\n" % (f, how, f + 14, E)
    at.append(f + 14)
    s += "%d plugwin close\n%d %s close\n" % (f + 16, f + 17, E)
    f += 22
# a double-click in the instrument list: Seltmann (row 2) and the Kick (row 5): their windows float
for row, name in ((1, "Seltmann"), (4, "a drum")):
    y = 41 + 16 * row
    s += "%d move 930 %d\n%d down 0\n%d up 0\n%d down 0\n%d up 0\n%d %s float\n%d plugwin close\n%d %s close\n" % (f, y, f + 2, f + 3, f + 4, f + 5, f + 20, E, f + 22,
                                                                                                         f + 23, E)
    cases.append(("a double-click in the instrument list (%s)" % name, "", "settings"))
    at.append(f + 20)
    f += 28
# the Tracker's own screens still open in the band
s += "%d %s open instrument 3\n%d %s float\n%d %s close\n" % (f, E, f + 12, E, f + 14, E)
ownAt = [f + 12]
f += 18
s += "%d %s open sample 0\n%d %s float\n%d %s close\n" % (f, E, f + 12, E, f + 14, E)
ownAt.append(f + 12)
f += 18
s += "%d %s windows\n%d quit discard\n" % (f, E, f + 2)
out = run("ways", s, f + 6)
d = floats(out)
for (name, how, win), fr in zip(cases, at):
    r = d.get(fr)
    ok(r is not None and r[win] == "floating", "%s: its window floats (%s)" % (name, r))
    ok(r is not None and r["band"] == "closed", "%s: nothing in the lower band (%s)" % (name, r and r["band"]))
    others = [w for w in ("settings", "fx", "clip") if w != win and r and r[w] == "docked"]
    ok(r is not None and not others, "%s: no window docked (%s)" % (name, others))
for fr, what in zip(ownAt, ("a Tracker Sampler's Instrument editor", "a Sampler's Sample editor")):
    r = d.get(fr)
    ok(r is not None and r["band"] == "ours" and r["settings"] == "down" and r["clip"] == "down", "%s stays in the band (%s)" % (what, r))
last = max(d) if d else None
ok(last is not None and d[last]["foreign"] == 0, "the lower band never drew a plugin (%s frames)" % (d[last]["foreign"] if last else "?"))

# ---- the Master chain ----
out = run("master", "3 toasts clear\n20 %s float\n24 quit discard\n" % E, 30, ("--show", "master"))
d = floats(out)
ok(d.get(20, {}).get("master") == "floating" and d.get(20, {}).get("band") == "closed", "the Master chain floats (%s)" % d.get(20))

print("%d passed, %d failed" % (passed, len(failures)))
if failures:
    for m in failures:
        print("FAIL: " + m)
    sys.exit(1)
print("k078_tracker_float_app: ok")
