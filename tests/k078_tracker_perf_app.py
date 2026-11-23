#!/usr/bin/env python3
# Gridline - the Performance panel in the Tracker layout, in the real app (hidden, isolated config
# folder and HOME, SDL's dummy audio driver):
#   - in the Tracker it is drawn in the Tracker's own look (its boxes laid out in the side panel) and
#     the side panel opens wide enough for it: no text cut short or wrapped, at 1440x900 and at 1024x700,
#     at 100 and 200 percent;
#   - opened after the Browser (whose panel is narrower) the panel still widens to the width it needs;
#   - closed and opened again it keeps its width; it never shrinks a wider panel;
#   - the other three layouts keep their own window (the Tracker's panel is not drawn there).
#   python3 k078_tracker_perf_app.py <gridline-app> <song.gridline.json> <out dir>
import json
import os
import re
import shutil
import subprocess
import sys

APP, DEMO, OUT = sys.argv[1], sys.argv[2], sys.argv[3]
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


def run(tag, layout, script, size="1440x900", scale=1, frames=120):
    cfg = os.path.join(OUT, "cfg_" + tag)
    os.makedirs(os.path.join(cfg, "gridline"))
    os.makedirs(os.path.join(cfg, "home"))
    with open(os.path.join(cfg, "gridline", "settings.json"), "w") as f:
        json.dump({"format": "gridline-settings", "values": {"ui.layoutChosen": True, "ui.layout": layout}}, f)
    song = os.path.join(cfg, "song.gridline.json")
    shutil.copyfile(DEMO, song)
    sp = os.path.join(OUT, tag + ".txt")
    with open(sp, "w") as f:
        f.write(script)
    env = dict(os.environ, SDL_AUDIODRIVER="dummy", GRIDLINE_NO_PLUGIN_SCAN="1", GRIDLINE_CONFIG_HOME=cfg, HOME=os.path.join(cfg, "home"))
    r = subprocess.run([APP, song, "--hidden", "--no-ui-sounds", "--no-update-check", "--fixed-dt", "16", "--size", size, "--ui-scale", str(scale),
                        "--dev-input", sp, "--frames", str(frames)], env=env, capture_output=True, text=True, timeout=600)
    with open(os.path.join(OUT, tag + ".log"), "w") as f:
        f.write(r.stdout + "\n---- stderr ----\n" + r.stderr)
    ok(r.returncode == 0, "%s: the app ran (%d)" % (tag, r.returncode))
    return r.stdout


PAT = re.compile(r"perfpanel frame (\d+): open (\d) tracker (\d) drawn (\d) win (\S+) (\S+) (\S+) (\S+) need (\S+) history (\d) rows (\d+) texts (\d+) "
                 r"clipped (\d+) wrapped (\d+)(.*)")


def report(out, frame):
    for m in PAT.finditer(out):
        if int(m.group(1)) == frame:
            return {"open": m.group(2) == "1", "tracker": m.group(3) == "1", "drawn": m.group(4) == "1", "w": float(m.group(7)),
                    "h": float(m.group(8)), "need": float(m.group(9)), "history": m.group(10) == "1", "texts": int(m.group(12)),
                    "clipped": int(m.group(13)), "wrapped": int(m.group(14)), "cut": m.group(15)}
    return None


# ---- the Tracker: its own look, wide enough, at two window sizes and two scales ----
for size in ("1440x900", "1024x700"):
    for scale in (1, 2):
        tag = "trk_%s_%dx" % (size, scale)
        out = run(tag, "tracker", "5 toasts clear\n8 layout open Performance\n10 key space\n60 perfpanel\n70 quit discard\n", size, scale, 80)
        r = report(out, 60)
        ok(r is not None and r["open"] and r["tracker"] and r["drawn"], "%s: the Tracker draws its own Performance panel" % tag)
        if r is None:
            continue
        ok(r["need"] > 200 and r["w"] + 1 >= r["need"], "%s: the panel is as wide as it needs (%.0f for %.0f)" % (tag, r["w"], r["need"]))
        ok(r["texts"] >= 8 and r["clipped"] == 0 and r["wrapped"] == 0, "%s: no text cut or wrapped (%d texts:%s)" % (tag, r["texts"], r["cut"]))
        ok(r["history"] or size == "1024x700", "%s: the load history shows" % tag)

# ---- after the Browser (a 280 px panel), then closed and opened again: still wide enough ----
out = run("trk_after_browser", "tracker",
          "5 toasts clear\n8 layout open Browser\n30 layout open Performance\n60 perfpanel\n62 layout close Performance\n70 layout open Performance\n"
          "100 perfpanel\n110 quit discard\n", frames=120)
r1, r2 = report(out, 60), report(out, 100)
ok(r1 is not None and r1["drawn"] and r1["w"] + 1 >= r1["need"] and r1["clipped"] == 0 and r1["wrapped"] == 0,
   "opened after the Browser the side panel widens for it")
ok(r2 is not None and r2["drawn"] and r1 is not None and abs(r2["w"] - r1["w"]) <= 1, "closed and opened again it keeps its width")

# ---- the other layouts keep their own window ----
for lay in ("studio", "live", "logic"):
    out = run("other_" + lay, lay, "5 toasts clear\n8 layout open Performance\n40 perfpanel\n50 quit discard\n", frames=60)
    r = report(out, 40)
    ok(r is not None and r["open"] and not r["tracker"] and not r["drawn"], "%s: its own Performance window (not the Tracker's)" % lay)

print("%d passed, %d failed" % (passed, len(failures)))
sys.exit(1 if failures else 0)
