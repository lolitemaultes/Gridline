#!/usr/bin/env python3
# Gridline - the Stage page in the Tracker layout (its page strip's tab right after Mix), used as a
# person uses it in the real app (hidden, isolated config folder and HOME, SDL's dummy audio driver,
# clicks on the drawn controls, typed keys):
#   - the strip has a Stage tab right after Mix; a click on it shows the Stage (the Mix tab goes dark),
#     a click on Mix shows the strips again; Ctrl+Alt+G and Ctrl+Alt+5 do the same;
#   - the Stage page is drawn in the Tracker look (dark well, its console column at the right), and
#     only in the Tracker;
#   - its controls act as the Stage's do in Studio: the Mute button, the arrow keys, Spread out with
#     Apply give the same mixer values and one undo step each;
#   - at 1024x700 and 1440x900 its parts do not overlap, no button label is cut short, the column takes
#     a quarter of the width at most and its Groups list has room for six rows (the boxes fold first);
#   - a box's fold button folds it and the column remembers it;
#   - after a restart the mixer's page is the Stage again (the front page itself is kept with the
#     layout, which a scripted run does not save).
#   python3 k078_tracker_stage_app.py <gridline-app> <song.gridline.json> <out dir>
import json
import os
import re
import shutil
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


shutil.rmtree(OUT, ignore_errors=True)
os.makedirs(OUT)


def run(tag, layout, script, frames=120, size="1440x900", keep=None, extra=()):
    cfg = keep or os.path.join(OUT, "cfg_" + tag)
    if not keep:
        shutil.rmtree(cfg, ignore_errors=True)
        os.makedirs(os.path.join(cfg, "gridline"))
        os.makedirs(os.path.join(cfg, "home"))
        with open(os.path.join(cfg, "gridline", "settings.json"), "w") as f:
            values = {"ui.layoutChosen": True, "ui.layout": layout, "ui.stage.welcomed": True}
            if layout == "tracker":  # the Tracker's own keys and menus, as choosing the layout sets them
                values.update({"keys.profile": "tracker", "ui.menuStyle": "tracker"})
            json.dump({"format": "gridline-settings", "values": values}, f)
        shutil.copyfile(DEMO, os.path.join(cfg, "song.gridline.json"))
    song = os.path.join(cfg, "song.gridline.json")
    sp = os.path.join(OUT, tag + ".txt")
    with open(sp, "w") as f:
        f.write(script)
    env = dict(os.environ, SDL_AUDIODRIVER="dummy", GRIDLINE_NO_PLUGIN_SCAN="1", GRIDLINE_QUIET_WINDOWS="1", GRIDLINE_CONFIG_HOME=cfg,
               HOME=os.path.join(cfg, "home"))
    r = subprocess.run([APP, song, "--hidden", "--no-ui-sounds", "--no-update-check", "--fixed-dt", "16", "--size", size, "--dev-input", sp,
                        "--frames", str(frames)] + list(extra), env=env, capture_output=True, text=True, timeout=600)
    with open(os.path.join(OUT, tag + ".log"), "w") as f:
        f.write(r.stdout + "\n---- stderr ----\n" + r.stderr)
    if r.returncode != 0:
        fail("%s: gridline-app failed (%d)" % (tag, r.returncode))
    return r.stdout, cfg


def strip(out, frame):
    m = re.search(r"tracker pagestrip frame %d:(.*)" % frame, out)
    if not m:
        return None
    return [(t[0], float(t[1]), float(t[2]), float(t[3]), float(t[4]), t[5] == "1") for t in re.findall(r'"([^"]+)" (\S+) (\S+) (\S+) (\S+) ([01])', m.group(1))]


def lit(tabs, label):
    for t in tabs or []:
        if t[0] == label:
            return t[5]
    return None


def stage(out, frame):
    m = re.search(r"stage frame %d: open ([01]) undo (\d+) sel (\S+) [^\n]*\n((?:  [^\n]*\n)*)" % frame, out)
    if not m:
        return None
    d = {"open": m.group(1) == "1", "undo": int(m.group(2)), "sel": m.group(3), "tokens": {}}
    for t in re.finditer(r"  token (\d+) x \S+ y \S+ side \S+ pan (\S+) depth (\S+) volume (\S+) sep (\S+) focus \S+ muted ([01])", m.group(4)):
        d["tokens"][int(t.group(1))] = {"pan": float(t.group(2)), "depth": float(t.group(3)), "volume": float(t.group(4)), "sep": float(t.group(5)),
                                        "muted": t.group(6) == "1"}
    lk = re.search(r"  look tracker (\d) bg (\S+) (\S+) (\S+) floor \S+ \S+ \S+ body (.*?) side (.*?) folded (\d+) grouprows (\d+)", m.group(4))
    if lk:
        d["look"] = int(lk.group(1))
        d["bg"] = tuple(float(lk.group(i)) for i in (2, 3, 4))
        for k, g in (("body", 5), ("side", 6)):
            d[k] = tuple(float(x) for x in lk.group(g).split())
        d["folded"], d["grouprows"] = int(lk.group(7)), int(lk.group(8))
    return d


def click(frame, x, y):
    return "%d sdlmove %d %d\n%d sdldown 0\n%d sdlup 0\n" % (frame, x, y, frame + 1, frame + 2)


KEYUP = "%d sdlkey #224 up\n%d sdlkey #226 up\n"


def chord(frame, key):
    return "%d sdlkey %s down ctrl+alt\n%d sdlkey %s up ctrl+alt\n" % (frame, key, frame + 1, key) + KEYUP % (frame + 2, frame + 2)


# ---- 1. the tab: right after Mix; a click shows the Stage, Mix shows the strips ----
out, _ = run("tab", "tracker", "5 toasts clear\n20 tracker pagestrip\n22 atitem \"Stage\"\n24 sdldown 0\n25 sdlup 0\n40 tracker pagestrip\n42 stage\n"
             "44 atitem \"Mix\"\n46 sdldown 0\n47 sdlup 0\n60 tracker pagestrip\n62 stage\n70 quit discard\n", 90)
tabs = strip(out, 20)
labels = [t[0] for t in tabs or []]
ok("Mix" in labels and "Stage" in labels and labels.index("Stage") == labels.index("Mix") + 1, "the page strip has Stage right after Mix (%s)" % labels)
if tabs and "Stage" in labels:
    mix, stg = tabs[labels.index("Mix")], tabs[labels.index("Stage")]
    ok(stg[1] >= mix[2] and stg[3] == mix[3] and stg[4] == mix[4], "the Stage tab sits beside Mix, the same height")
t40 = strip(out, 40)
ok(lit(t40, "Stage") is True and lit(t40, "Mix") is False, "a click on Stage lights Stage, not Mix")
s42 = stage(out, 42)
ok(s42 is not None and s42["open"] and s42.get("look", 0) >= 1, "the Stage page shows the Stage in the Tracker look")
if s42 and "bg" in s42:
    lum = 0.2126 * s42["bg"][0] + 0.7152 * s42["bg"][1] + 0.0722 * s42["bg"][2]
    ok(lum < 0.2, "the stage's well is the Tracker's dark palette (luminance %.3f)" % lum)
t60 = strip(out, 60)
ok(lit(t60, "Mix") is True and lit(t60, "Stage") is False, "a click on Mix shows the strips again")
s62 = stage(out, 62)
ok(s62 is not None and not s62["open"], "the Mix page no longer draws the Stage")

# ---- 2. the keys: Ctrl+Alt+G the Stage page, Ctrl+Alt+5 the Mix page ----
out, _ = run("keys", "tracker", "5 toasts clear\n" + chord(20, "g") + "40 tracker pagestrip\n" + chord(50, "5") + "70 tracker pagestrip\n80 quit discard\n", 90)
ok(lit(strip(out, 40), "Stage") is True, "Ctrl+Alt+G shows the Stage page")
ok(lit(strip(out, 70), "Mix") is True and lit(strip(out, 70), "Stage") is False, "Ctrl+Alt+5 shows the Mix page")

# ---- 3. only the Tracker draws its look ----
out, _ = run("studio", "studio", "5 toasts clear\n30 stage\n40 quit discard\n", 50, extra=("--show", "mixer", "--show", "stage"))
s = stage(out, 30)
ok(s is not None and s["open"] and s.get("look", -1) == 0, "Studio's Stage keeps its own look")

# ---- 4. the controls act as in Studio (the same values, one undo step each) ----
# The same edits in both layouts: track 1 selected; Right twice, Up once (the keys); then Mute.
EDITS = "30 stage select 1\n32 stage\n" + "34 sdlkey right down\n35 sdlkey right up\n37 sdlkey right down\n38 sdlkey right up\n" \
        "40 sdlkey up down\n41 sdlkey up up\n50 stage\n"
studio, _ = run("edits_studio", "studio", "5 toasts clear\n10 layout focus Mixer\n" + EDITS + "52 sdlkey m down\n53 sdlkey m up\n60 stage\n70 quit discard\n", 80,
                extra=("--show", "mixer", "--show", "stage"))
tracker, _ = run("edits_tracker", "tracker", "5 toasts clear\n" + chord(10, "g") + "20 layout focus Mixer\n" + EDITS + "52 atitem \"Mute\"\n54 sdldown 0\n55 sdlup 0\n"
                 "60 stage\n70 quit discard\n", 80)
a0, a1, a2 = stage(studio, 32), stage(studio, 50), stage(studio, 60)
b0, b1, b2 = stage(tracker, 32), stage(tracker, 50), stage(tracker, 60)
if all((a0, a1, a2, b0, b1, b2)):
    ta, tb = a1["tokens"].get(1), b1["tokens"].get(1)
    ok(ta is not None and tb is not None and abs(ta["pan"] - tb["pan"]) < 1e-6 and abs(ta["depth"] - tb["depth"]) < 1e-6 and
       abs(ta["pan"] - a0["tokens"][1]["pan"]) > 0.01, "the arrow keys move a track the same in the Tracker as in Studio")
    ok(b1["undo"] - b0["undo"] == a1["undo"] - a0["undo"], "the moves take as many undo steps in both (%d, %d)" % (a1["undo"] - a0["undo"], b1["undo"] - b0["undo"]))
    ok(a2["tokens"][1]["muted"] and b2["tokens"][1]["muted"], "the Tracker's Mute button mutes the track, as Studio's M key")
    ok(b2["undo"] - b1["undo"] == 1, "Mute is one undo step")
else:
    fail("the edits' stage reports are missing")

# Spread out with its Apply, from the Tracker's buttons, as Studio's panel buttons place the tracks.
SPREAD = "30 stage none\n40 atitem \"Spread out\"\n42 sdldown 0\n43 sdlup 0\n50 atitem \"Apply\"\n52 sdldown 0\n53 sdlup 0\n60 stage\n70 quit discard\n"
studio, _ = run("spread_studio", "studio", "5 toasts clear\n10 layout focus Mixer\n30 stage none\n40 stage preview spread\n50 stage apply\n60 stage\n70 quit discard\n", 80,
                extra=("--show", "mixer", "--show", "stage"))
tracker, _ = run("spread_tracker", "tracker", "5 toasts clear\n" + chord(10, "g") + SPREAD, 80)
a, b = stage(studio, 60), stage(tracker, 60)
if a and b:
    same = a["tokens"].keys() == b["tokens"].keys() and all(abs(a["tokens"][k]["pan"] - b["tokens"][k]["pan"]) < 1e-6 for k in a["tokens"])
    spread = len({round(t["pan"], 3) for t in b["tokens"].values()}) > 1
    ok(same and spread, "Spread out and Apply on the Stage page place the tracks as in Studio")
else:
    fail("the spread reports are missing")

# ---- 5. nothing overlaps or is cut short at 1024x700 and 1440x900 ----


def inside(r, w, h):
    return r[0] >= 0 and r[1] >= 0 and r[2] <= w + 0.5 and r[3] <= h + 0.5


def overlap(r, q):
    return min(r[2], q[2]) - max(r[0], q[0]) > 0.5 and min(r[3], q[3]) - max(r[1], q[1]) > 0.5


for size in ("1024x700", "1440x900"):
    w, h = (int(x) for x in size.split("x"))
    for sel in ("none", "select 1", "select 1,2,3"):
        tag = "fit_%s_%s" % (size, sel.replace(" ", "").replace(",", ""))
        out, _ = run(tag, "tracker", "5 toasts clear\n" + chord(10, "g") + "16 tracker labels on\n20 stage compactdemo\n24 stage " + sel +
                     "\n40 stage\n42 tracker labels\n50 quit discard\n", 60, size=size)
        s = stage(out, 40)
        m = re.search(r"tracker labels frame 42: drawn (\d+) clipped (\d+) tight (\d+)(.*)", out)
        ok(m is not None and int(m.group(1)) > 0 and int(m.group(2)) == 0, "%s %s: no button label cut short%s" % (size, sel, m.group(4) if m else ""))
        if not s or "side" not in s:
            fail("%s %s: no stage report" % (size, sel))
            continue
        body, side = s["body"], s["side"]
        ok(inside(body, w, h) and inside(side, w, h) and side[2] > side[0], "%s %s: the stage and its column are inside the window" % (size, sel))
        ok(not overlap(body, side), "%s %s: the stage and the column do not overlap" % (size, sel))
        ok(side[2] - side[0] <= w * 0.25 + 1, "%s %s: the column takes a quarter of the width at most (%d px)" % (size, sel, side[2] - side[0]))
        ok(s["grouprows"] >= 6, "%s %s: the Groups list has room for six rows (%d; folded %d)" % (size, sel, s["grouprows"], s["folded"]))
        if size == "1440x900":
            ok(s["folded"] == 0, "%s %s: nothing is folded on a big window" % (size, sel))

# A box folded with its button stays folded (after a restart too); its button again opens it.
out, cfg = run("fold1", "tracker", "5 toasts clear\n" + chord(10, "g") + "20 stage select 1\n30 stage\n32 atitem \"-\"\n34 sdldown 0\n35 sdlup 0\n"
               "40 stage\n50 quit discard\n", 60)
a, b = stage(out, 30), stage(out, 40)
ok(a is not None and b is not None and not a["folded"] & 8 and b["folded"] & 8, "the Level box's fold button folds it")
out, _ = run("fold2", "tracker", "5 toasts clear\n10 tracker page Mixer\n30 stage\n50 quit discard\n", 60, keep=cfg)
c = stage(out, 30)
ok(c is not None and c["folded"] & 8, "the Level box stays folded after a restart")

# ---- 6. the tab comes back after a restart ----
out, cfg = run("persist1", "tracker", "5 toasts clear\n" + chord(10, "g") + "30 tracker pagestrip\n40 quit discard\n", 60)
out2, _ = run("persist2", "tracker", "5 toasts clear\n20 tracker page Mixer\n40 tracker pagestrip\n50 quit discard\n", 60, keep=cfg)
ok(lit(strip(out2, 40), "Stage") is True, "after a restart the mixer's page is the Stage again (Stage lit, not Mix)")

print("%d passed, %d failed" % (passed, len(failures)))
sys.exit(1 if failures else 0)
