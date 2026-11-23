#!/usr/bin/env python3
# Gridline - 0.7.8: in the Tracker layout a Sampler's sample and its audio clips open in the Tracker's
# Sample editor page, which holds every setting of the Audio clip window in the Tracker's look. In the
# real app (hidden, isolated config folder and HOME, SDL's dummy audio driver), on a copy of the test
# song with a Sampler playing an audio clip (Take, clip 70) and a Sampler no clip plays (Kick):
#   A. with the Advanced view picked, every way into the Sample editor opens the page in the lower
#      band, no window: a double-click on the clip, Edit clip..., the Audio page (its button and
#      Ctrl+Alt+8), Ctrl+Alt+E; Esc closes it. Channel settings of a Sampler and Open plugin window
#      open the floating Audio clip window instead (plugins float in the Tracker), nothing in the band;
#   B. every control of the Audio clip window is drawn there (its checklist has nothing missing),
#      for the clip and for the Sampler alone, and one control of each module changes the song in
#      one undo step (undo brings the song back byte for byte);
#   C. at 1440x900 and 1024x700, both themes, on the waveform page and every tab's page: every part
#      inside the band, none over another, the page list's rows inside it, the modules' controls
#      inside their module and none cut or over another, and no Tracker label cut short;
#   D. the view pair: the classic Sample editor is the default; "Advanced" and "Sample editor" switch
#      with one click each way, keep the Sampler and the selected range, and the choice is remembered
#      (an app setting): a double-click on the clip then opens the view picked last.
#   python3 k078_tracker_clipform_app.py <gridline-app> <test_song.gridline.json> <startup.wav> <out dir>
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
for n in ("Take.wav", "Kick.wav"):
    shutil.copyfile(WAV, os.path.join(OUT, n))
with open(DEMO) as f:
    demo = f.read()
chan = ('{"color": 5219296, "gain": 0.8, "id": 60, "mixerTrack": 0, "muted": false, "name": "Take", "pan": 0, "pitch": 0, '
        '"plugin": {"type": "gl.sampler", "data": %s, "params": []}}, '
        '{"color": 14251600, "gain": 0.8, "id": 61, "mixerTrack": 0, "muted": false, "name": "Kick", "pan": 0, "pitch": 0, '
        '"plugin": {"type": "gl.sampler", "data": %s, "params": []}}, ') % (json.dumps(os.path.join(OUT, "Take.wav")), json.dumps(os.path.join(OUT, "Kick.wav")))
clip = '{"id": 70, "kind": "audio", "source": 60, "track": 7, "start": 0, "length": 1536, "pattern": 0, "gain": 1.0}, '
demo = demo.replace('"channels": [', '"channels": [' + chan, 1).replace('"playlist": [', '"playlist": [' + clip, 1)
demo = re.sub(r'"nextId": *[0-9]+', '"nextId": 100', demo)
SONG = os.path.join(OUT, "song.gridline.json")
with open(SONG, "w") as f:
    f.write(demo)


def run(tag, script, frames, size="1440x900", light=False, view="advanced"):
    cfg = os.path.join(OUT, "cfg_" + tag)
    os.makedirs(os.path.join(cfg, "gridline"))
    os.makedirs(os.path.join(cfg, "home"))
    with open(os.path.join(cfg, "gridline", "settings.json"), "w") as f:
        json.dump({"format": "gridline-settings", "values": {"ui.layoutChosen": True, "ui.layoutDev": True, "ui.layout": "tracker", "keys.profile": "tracker",
                                                             "ui.menuStyle": "tracker", "tracker.sampleView": view}}, f)
    sp = os.path.join(OUT, tag + ".txt")
    with open(sp, "w") as f:
        f.write(script)
    env = dict(os.environ, SDL_AUDIODRIVER="dummy", GRIDLINE_NO_PLUGIN_SCAN="1", GRIDLINE_HIDE_PLUGINS="all", GRIDLINE_CONFIG_HOME=cfg,
               HOME=os.path.join(cfg, "home"))
    args = [APP, SONG, "--hidden", "--no-ui-sounds", "--no-update-check", "--fixed-dt", "16", "--size", size, "--dev-input", sp, "--frames", str(frames)]
    if light:
        args.append("--light")
    r = subprocess.run(args, env=env, capture_output=True, text=True, timeout=600)
    with open(os.path.join(OUT, tag + ".log"), "w") as f:
        f.write(r.stdout + "\n---- stderr ----\n" + r.stderr)
    ok(r.returncode == 0, "%s: the app ran (%d)" % (tag, r.returncode))
    return r.stdout


DUMP = re.compile(r"tracker editors clip frame (\d+): shown (\d) drawn (\d) wave (\d) tab (\d+) channel (\d+) band (\S+) (\S+) (\S+) (\S+) parts \d+([^\n]*)")


def dumps(out):
    r = {}
    for m in DUMP.finditer(out):
        parts = []
        for p in m.group(11).split(" | ")[1:]:
            w = p.strip().split(" ")
            parts.append((" ".join(w[:-4]), [float(v) for v in w[-4:]]))
        r[int(m.group(1))] = {"shown": m.group(2) == "1", "drawn": m.group(3) == "1", "wave": m.group(4) == "1", "tab": int(m.group(5)),
                              "channel": int(m.group(6)), "band": [float(m.group(i)) for i in range(7, 11)], "parts": parts}
    return r


def state(out, n=0):
    v = re.findall(r"clipeditor state frame \d+ host (\S+) shown (\d) clip (\d+) tab (\d+) [^\n]* window (\d) [^\n]* channel (\d+) mode", out)
    return v[n] if len(v) > n else None


E = "tracker editors"

# ---- A: the ways in ----
ways = [
    ("double-click on the clip", "6 clipeditor dbl 70\n", 60, "70"),
    ("Edit clip...", "6 clipeditor open 70 0\n", 60, "70"),
    ("the Audio page button", "4 %s open sample 1\n5 %s close\n8 %s menu at Page Audio\n10 sdldown 0\n11 sdlup 0\n" % (E, E, E), 61, "0"),
    ("Ctrl+Alt+8", "4 %s open sample 1\n5 %s close\n8 sdlkey 8 down ctrl+alt\n9 sdlkey 8 up ctrl+alt\n" % (E, E), 61, "0"),
    ("Ctrl+Alt+E", "4 %s open sample 1\n5 %s close\n8 sdlkey e down ctrl+alt\n9 sdlkey e up ctrl+alt\n" % (E, E), 61, "0"),
]
for i, (name, how, chanId, clipId) in enumerate(ways):
    close = "24 sdlkey escape down\n25 sdlkey escape up\n"
    out = run("way%d" % i, "3 toasts clear\n" + how + "20 %s clip\n21 clipeditor state\n22 %s windows\n" % (E, E) + close + "30 %s clip\n32 quit discard\n" % E, 40)
    d = dumps(out)
    st = state(out)
    ok(20 in d and d[20]["shown"] and d[20]["drawn"] and d[20]["channel"] == chanId,
       "%s opens the Sample editor page on its Sampler (%s)" % (name, d.get(20, {}).get("channel")))
    ok(st is not None and st[0] == "pane" and st[1] == "1" and st[2] == clipId and st[4] == "0",
       "%s: the Audio clip window's settings in the band with its clip, no window (%s)" % (name, st))
    ok(re.search(r"tracker windows frame 22: [^\n]* plugin closed band clip", out) is not None, "%s: no plugin window, the band shows the page" % name)
    ok(30 in d and not d[30]["shown"], "%s: Esc closes the page" % name)
# Channel settings of a Sampler and Open plugin window float its Audio clip window
for i, (name, how) in enumerate((("Channel settings of a Sampler", "6 plugwin chan 1\n"), ("Open plugin window", "6 %s plugin 1\n" % E))):
    out = run("float%d" % i, "3 toasts clear\n" + how + "20 %s clip\n21 clipeditor state\n22 %s windows\n32 quit discard\n" % (E, E), 40)
    d = dumps(out)
    st = state(out)
    ok(20 in d and not d[20]["shown"], "%s: nothing in the band" % name)
    ok(st is not None and st[0] == "window" and st[1] == "1" and st[5] == "61", "%s: the floating Audio clip window on its Sampler (%s)" % (name, st))
    ok(re.search(r"tracker windows frame 22: [^\n]* plugin open band closed", out) is not None, "%s: the plugin window floats, the band is closed" % name)

# ---- B: every control there, one undo step each ----
clipKeys = ("vol pan pitch colour gain reverse.clip fades semis loopclip tempo stretch engine startend removedc smpfades oneshot root volenv filter lfo "
            "fx.shape fx.tone fx.space tune").split()
chanKeys = ("vol pan pitch colour engine startend removedc smpfades oneshot reverse.notes loop.notes root volenv filter lfo fx.shape fx.tone fx.space "
            "tune").split()
for tag, how, keys in (("clip", "6 clipeditor dbl 70\n", clipKeys), ("sampler", "6 %s open sample 1\n" % E, chanKeys)):
    s = "3 toasts clear\n4 clipeditor reach on\n" + how
    f = 12
    s += "%d %s clip page wave\n" % (f, E)
    f += 4
    for t in range(12):
        s += "%d clipeditor tab %d\n" % (f, t)
        f += 4
    s += "%d clipeditor menu\n%d key escape\n%d clipeditor editmenu\n%d key escape\n" % (f, f + 4, f + 6, f + 10)
    f += 14
    s += "%d clipeditor checklist\n" % f
    f += 2
    s += "%d save \"%s\"\n" % (f, os.path.join(OUT, tag + "_before.gridline.json"))
    f += 2
    for k in keys:
        s += "%d clipeditor poke %s\n%d undo\n" % (f, k, f + 2)
        f += 4
    s += "%d save \"%s\"\n%d quit discard\n" % (f, os.path.join(OUT, tag + "_after.gridline.json"), f + 4)
    out = run("reach_" + tag, s, f + 10)
    m = re.search(r"clipeditor checklist reached (\d+) missing (\d+)([^\n]*)", out)
    ok(m is not None and m.group(2) == "0", "%s: every control of the Audio clip window drawn on the page (%s)" % (tag, m.group(0) if m else "no checklist"))
    for k in keys:
        ok(re.search(r"clipeditor poke %s done undo 1\n" % re.escape(k), out) is not None, "%s: %s is one undo step" % (tag, k))
    with open(os.path.join(OUT, tag + "_before.gridline.json")) as f1, open(os.path.join(OUT, tag + "_after.gridline.json")) as f2:
        ok(f1.read() == f2.read(), "%s: undo brought every change back" % tag)


# ---- C: nothing over another, outside or cut ----
def over(a, b):
    return min(a[2], b[2]) - max(a[0], b[0]) > 0.5 and min(a[3], b[3]) - max(a[1], b[1]) > 0.5


def inside(a, b):
    return a[0] >= b[0] - 0.5 and a[1] >= b[1] - 0.5 and a[2] <= b[2] + 0.5 and a[3] <= b[3] + 0.5


for size in ("1440x900", "1024x700"):
    for light in (False, True):
        for tag, how in (("clip", "6 clipeditor dbl 70\n"), ("sampler", "6 %s open sample 1\n" % E)):
            name = "%s_%s_%s" % (tag, size, "light" if light else "dark")
            s = "3 toasts clear\n4 tracker labels on\n" + how
            pages = ["wave"] + [str(t) for t in range(10)]
            f = 12
            probes = []
            for p in pages:
                s += "%d %s clip page %s\n%d %s clip\n%d clipeditor strip\n%d tracker labels\n" % (f, E, p, f + 4, E, f + 4, f + 4)
                probes.append((p, f + 4))
                f += 6
            s += "%d quit discard\n" % f
            out = run(name, s, f + 8, size, light)
            d = dumps(out)
            strips = re.findall(r"clipeditor strip frame \d+ tab \d+ modules (\d+) compact \d (outside \d+ overlaps \d+ clipped \d+ titles \d+ apart \d+ baselines \d+ wide \d+)"
                                r"([^\n]*)", out)
            good = "outside 0 overlaps 0 clipped 0 titles 0 apart 0 baselines 0 wide 0"
            ok(len(strips) == len(pages) and all(x[1] == good for x in strips[1:]),
               "%s: every module's controls inside it, none over another or cut, none past the page's side (%s)" % (name, [x for x in strips[1:] if x[1] != good]))
            labels = re.findall(r"tracker labels frame \d+: drawn (\d+) clipped (\d+)([^\n]*)", out)
            ok(len(labels) == len(pages) and all(x[1] == "0" for x in labels), "%s: no Tracker label cut short (%s)" % (name, [x for x in labels if x[1] != "0"]))
            seen = set()
            for p, fr in probes:
                r = d.get(fr)
                if r is None or not r["shown"] or not r["drawn"]:
                    ok(False, "%s page %s: the page drawn" % (name, p))
                    continue
                if not r["wave"]:
                    seen.add(r["tab"])
                band = r["band"]
                parts = dict(r["parts"])
                bad = [n for n, b in r["parts"] if not inside(b, band)]
                bad += [n for n in ("menu", "presets", "route") if n in parts and "title" in parts and not inside(parts[n], parts["title"])]
                bad += [n for n, b in r["parts"] if n.startswith("module ") and "modules" in parts and not inside(b, parts["modules"])]
                top = [(n, b) for n, b in r["parts"] if not n.startswith("page ") and n not in ("exit", "play", "edit audio", "menu", "presets", "route", "modules")]
                hits = ["%s/%s" % (top[i][0], top[j][0]) for i in range(len(top)) for j in range(i + 1, len(top)) if over(top[i][1], top[j][1])]
                rows = [(n, b) for n, b in r["parts"] if n.startswith("page ") or n in ("exit", "play", "edit audio")]
                bad += [n for n, b in rows if "pages" in parts and not inside(b, parts["pages"])]
                hits += ["%s/%s" % (rows[i][0], rows[j][0]) for i in range(len(rows)) for j in range(i + 1, len(rows)) if over(rows[i][1], rows[j][1])]
                ok(not bad and not hits and "exit" in parts and "menu" in parts and "presets" in parts,
                   "%s page %s: every part inside the band, none over another (outside %s, over %s)" % (name, p, bad, hits))
                ok(r["wave"] or "module output" in parts, "%s page %s: Output pinned at the right" % (name, p))
            ok(len(seen) >= (8 if tag == "clip" else 6), "%s: each tab with modules shows its page (%s)" % (name, sorted(seen)))

# ---- D: the classic and Advanced views (the dump's screen: 1 the classic Sample editor, 7 the Advanced page) ----
SDUMP = re.compile(r"tracker editors frame (\d+): shown (\d) screen (\d) [^\n]* range (-?\d+) (-?\d+) ")
script = ("3 toasts clear\n6 %s open sample 0\n10 %s range 20000 60000\n14 %s dump\n15 %s clip\n"
          "18 %s menu at View Advanced\n20 sdldown 0\n21 sdlup 0\n29 %s dump\n30 %s clip\n31 clipeditor state\n"
          "34 %s menu at View Sample editor\n36 sdldown 0\n37 sdlup 0\n46 %s dump\n47 %s clip\n"
          "50 %s menu at View Advanced\n52 sdldown 0\n53 sdlup 0\n58 %s close\n62 clipeditor dbl 70\n75 %s dump\n76 %s clip\n78 quit discard\n") % ((E,) * 14)
cfgView = os.path.join(OUT, "cfg_views", "gridline", "settings.json")
out = run("views", script, 84, view="classic")
d = dumps(out)
sd = {int(m.group(1)): (m.group(2), m.group(3), int(m.group(4)), int(m.group(5))) for m in SDUMP.finditer(out)}
ok(sd.get(14, ("0", "0"))[:2] == ("1", "1"), "the classic Sample editor is the default for a Sampler (%s)" % (sd.get(14),))
ok(sd.get(29, ("0", "0"))[:2] == ("1", "7") and d.get(30, {}).get("channel") == 60 and d[30]["parts"],
   "Advanced shows the Audio clip window's pages for the same Sampler (%s %s)" % (sd.get(29), d.get(30, {}).get("channel")))
st = state(out)
sel = re.search(r"clipeditor state frame \d+ host pane [^\n]* sel ([0-9.]+) ([0-9.]+) ", out)
ok(sel is not None and float(sel.group(2)) > float(sel.group(1)) > 0, "the range comes over as the page's selection (%s)" % (sel.group(0) if sel else None))
r46 = sd.get(46)
ok(r46 is not None and r46[1] == "1" and abs(r46[2] - 20000) <= 2 and abs(r46[3] - 60000) <= 2,
   "Sample editor switches back with the range kept (%s)" % (r46,))
ok(sd.get(75, ("0", "0"))[:2] == ("1", "7"), "a double-click on the clip opens the view picked last (Advanced) (%s)" % (sd.get(75),))
try:
    with open(cfgView) as f:
        saved = json.load(f)["values"].get("tracker.sampleView")
except (OSError, ValueError, KeyError):
    saved = None
ok(saved == "advanced", "the choice is remembered as an app setting (%s)" % saved)

print("%d passed, %d failed" % (passed, len(failures)))
if failures:
    for m in failures:
        print("FAIL: " + m)
    sys.exit(1)
