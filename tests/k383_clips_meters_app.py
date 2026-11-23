#!/usr/bin/env python3
# Gridline - K-383 (0.7.8): the Clips layout's track meters during the demo. In the real app (hidden,
# isolated config folder and HOME, SDL's dummy audio driver) the demo song plays in Clips with the mixer
# band shown; the meters Clips draws (the mixer strips under the main view and the arrangement's track
# headers, dev input "clipsmeters") are read many times while it plays, then after it stops:
#   - while playing, tracks are metered (some strips above the floor) and the typical (median) lit track
#     sits in a sensible range of the height, not pinned near the top: under 0.85 of the meter, and lower
#     than the shared -60..+6 dB scale put it;
#   - no meter is at the top unless its level is over 0 dBFS (red only for overs);
#   - the headers and the strips read the same tracks alike;
#   - after the stop every meter falls to rest once the reverb tails end, and stays there (nothing keeps them lit).
#   python3 k383_clips_meters_app.py <gridline-app> <demo song> <out dir>
import json
import os
import re
import shutil
import statistics
import subprocess
import sys

APP, DEMO, OUT = sys.argv[1:4]
failures = []
passed = 0


def ok(cond, msg):
    global passed
    if cond:
        passed += 1
        print("ok - " + msg, flush=True)
    else:
        failures.append(msg)
        print("FAIL: " + msg, flush=True)
    return cond


shutil.rmtree(OUT, ignore_errors=True)
os.makedirs(OUT)
cfg = os.path.join(OUT, "cfg")
os.makedirs(os.path.join(cfg, "gridline"))
os.makedirs(os.path.join(cfg, "home"))
with open(os.path.join(cfg, "gridline", "settings.json"), "w") as f:
    json.dump({"format": "gridline-settings", "values": {"ui.layoutChosen": True, "ui.layout": "live"}}, f)
song = os.path.join(OUT, "demo.gridline.json")
shutil.copyfile(DEMO, song)

# Tab shows the arrangement (its headers have meters too; the strips stay under it). Play from bar 17 (the drums and bass are in) at frame 20; read the meters every 10 frames while playing (about 6 s at the fixed 16 ms step,
# the audio runs in real time beside it), stop, then read them as they fall.
PLAY_FROM, PLAY_TO, REST_TO = 20, 420, 1600
lines = ["5 toasts clear", "10 sdlkey tab down", "11 sdlkey tab up", "15 seekbeat 64 seek", "%d startsong" % PLAY_FROM, "%d waitfor transport-playing 120" % (PLAY_FROM + 2)]
for fr in range(PLAY_FROM + 40, PLAY_TO, 10):
    lines.append("%d clipsmeters" % fr)
lines.append("%d shot %s" % (PLAY_TO - 5, os.path.join(OUT, "playing.ppm")))
lines.append("%d stopvia button" % PLAY_TO)
lines.append("%d transport" % (PLAY_TO + 2))
for fr in range(PLAY_TO + 10, REST_TO, 10):
    lines.append("%d clipsmeters" % fr)
lines.append("%d quit discard" % (REST_TO + 5))
script = os.path.join(OUT, "input.txt")
with open(script, "w") as f:
    f.write("\n".join(lines) + "\n")

env = dict(os.environ, SDL_AUDIODRIVER="dummy", GRIDLINE_NO_PLUGIN_SCAN="1", GRIDLINE_CONFIG_HOME=cfg, HOME=os.path.join(cfg, "home"))
r = subprocess.run([APP, song, "--hidden", "--no-ui-sounds", "--no-update-check", "--fixed-dt", "16", "--size", "1440x900", "--layout", "live",
                    "--show", "mixer", "--dev-input", script, "--frames", str(REST_TO + 40)],
                   env=env, capture_output=True, text=True, timeout=600)
with open(os.path.join(OUT, "run.log"), "w") as f:
    f.write(r.stdout + "\n---- stderr ----\n" + r.stderr)
ok(r.returncode == 0, "gridline-app ran (%d)" % r.returncode)

HEAD = re.compile(r"clipsmeters frame (\d+): drawn (-?\d+) playing (\d) meters (\d+)")
ROW = re.compile(r"  meter (\S+) (\S+) levelDb (\S+) (\S+) holdDb (\S+) (\S+) pos (\S+) (\S+) oldpos (\S+) (\S+)")
reads = []  # (frame, playing, [(kind, id, levelDb, pos, oldpos)])
cur = None
for ln in r.stdout.splitlines():
    m = HEAD.match(ln)
    if m:
        cur = (int(m.group(1)), m.group(3) == "1", [])
        reads.append(cur)
        continue
    m = ROW.match(ln)
    if m and cur is not None:
        lv = max(float(m.group(3)), float(m.group(4)))
        ps = max(float(m.group(7)), float(m.group(8)))
        op = max(float(m.group(9)), float(m.group(10)))
        cur[2].append((m.group(1), m.group(2), lv, ps, op))

playing = [rd for rd in reads if rd[1] and rd[0] < PLAY_TO]
after = [rd for rd in reads if rd[0] > PLAY_TO]
ok(len(playing) >= 20, "meters read while playing (%d reads)" % len(playing))
ok(all(any(k == "strip" for k, *_ in rd[2]) for rd in playing), "the mixer strips are drawn while playing")
ok(all(any(k == "head" for k, *_ in rd[2]) for rd in playing), "the track headers' meters are drawn while playing")

# Typical heights: each lit strip's height in each read (Main left out: it sums every track).
lit, lit_old, dbs = [], [], []
tops = []
for _, _, rows in playing:
    for kind, ident, lv, ps, op in rows:
        if kind == "strip" and lv > -59.9:
            lit.append(ps), lit_old.append(op), dbs.append(lv)
            if ps >= 0.999 and lv <= 0.0:
                tops.append((ident, lv))
ok(len(lit) >= 20, "tracks are metered while the demo plays (%d lit strip readings)" % len(lit))
if lit:
    med, med_old, med_db = statistics.median(lit), statistics.median(lit_old), statistics.median(dbs)
    hi = sorted(lit)[int(len(lit) * 0.9)]
    print("info: median lit strip level %.1f dB, height %.3f (was %.3f on the old scale); 90th percentile height %.3f" % (med_db, med, med_old, hi), flush=True)
    ok(0.2 <= med <= 0.85, "the typical track meter sits in a sensible range (median height %.3f, 0.2..0.85)" % med)
    ok(med < med_old - 0.02, "lower than the old scale put it (%.3f < %.3f)" % (med, med_old))
    ok(hi < 0.97, "not pinned near the top (90th percentile %.3f)" % hi)
ok(not tops, "only overs reach the top of a meter (%s)" % tops[:3])

# Headers and strips of the same mixer track read alike (same ballistics, same reading per frame).
diff = []
for _, _, rows in playing:
    st = {i: ps for k, i, lv, ps, op in rows if k == "strip"}
    for k, i, lv, ps, op in rows:
        if k == "head" and i in st:
            diff.append(abs(st[i] - ps))
ok(diff and max(diff) < 0.02, "headers and strips of a track agree (%d pairs, largest gap %.3f)" % (len(diff), max(diff) if diff else -1))

# After the stop: every meter falls to rest (20 dB/s from at most 0 dB: 3 s, plus the 0.6 s hold).
rest = [rd for rd in after if rd[2] and all(lv <= -59.9 for _, _, lv, _, _ in rd[2])]
ok(after and any(not rd[1] for rd in after), "the transport stopped")
ok(bool(rest), "every meter at rest after the stop (first at frame %s)" % (rest[0][0] if rest else "-"))
if rest:
    # (the returns' reverb and echo tails ring on for a few seconds after the stop: the meters follow them)
    ok(rest[0][0] - PLAY_TO <= 1000, "at rest %d frames after the stop" % (rest[0][0] - PLAY_TO))
    later = [rd for rd in after if rd[0] >= rest[0][0]]
    ok(all(all(lv <= -59.9 for _, _, lv, _, _ in rd[2]) for rd in later), "and stays at rest (%d later reads)" % len(later))

print("%d passed, %d failed" % (passed, len(failures)), flush=True)
sys.exit(1 if failures else 0)
