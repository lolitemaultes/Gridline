#!/usr/bin/env python3
# Gridline - K-388: the divots of looped regions in Smooth, in the real app (hidden, isolated config folder and
# HOME, SDL's dummy audio driver, a fixed frame time) on a copy of the test song whose arrangement is replaced by
# looped and unlooped regions: a one-bar pattern looped over eight bars, a two-bar pattern entered mid-loop (offset),
# a muted looped region, a cut loop of an eighth note (very short repeats), a cut loop of three beats starting off
# the bar, a looped audio region, and an unlooped pattern and audio region. At several zooms (the playlist's own
# geometry and the divots drawn, through the dev input's "plgeom"):
#   place     every divot drawn is on a loop point of its region (tick exact) and its seam column is that tick's
#             grid edge within 0.5 px: x0 + tick * pxPerTick, at every zoom (no drift along long regions);
#   all       zoomed in, every loop point in view has its divot (none skipped but those under the name);
#   thin      zoomed out, the short repeats' divots thin out (every 2nd, the first always; then none): two shown
#             divots of a region are never closer than 20 px, and the eighth-note loop shows fewer divots than
#             its loop points;
#   none      unlooped regions have none; Studio and Clips (their own notches) report none either;
#   pixels    at a divot's centre the region's top row shows the ground (dark), its neighbours the region.
#   python3 k388_smooth_divots_app.py <gridline-app> <test_song.gridline.json> <startup.wav> <out dir>
import json
import os
import re
import shutil
import subprocess
import sys

APP, DEMO, WAV, OUT = sys.argv[1:5]
failures = []
B = 384


def check(cond, what):
    print(("ok - " if cond else "FAIL - ") + what)
    if not cond:
        failures.append(what)


shutil.rmtree(OUT, ignore_errors=True)
os.makedirs(OUT)
shutil.copy(WAV, os.path.join(OUT, "Take.wav"))
song = json.load(open(DEMO))
song["channels"].insert(0, {"color": 5219296, "gain": 0.8, "id": 60, "mixerTrack": 0, "muted": False, "name": "Take", "pan": 0, "pitch": 0,
                            "plugin": {"type": "gl.sampler", "data": os.path.join(OUT, "Take.wav"), "params": []}})
# (id, pattern, track, start, length, offset, loop, loopLength, extra)
song["playlist"] = [
    {"id": 101, "pattern": 19, "track": 0, "start": 0, "length": 8 * B, "loop": True, "name": "Beat loop"},
    {"id": 102, "pattern": 29, "track": 1, "start": 0, "length": 6 * B + 192, "offset": 192, "loop": True, "name": "Bass from mid"},
    {"id": 104, "pattern": 19, "track": 2, "start": 0, "length": 6 * B, "loop": True, "muted": True},
    {"id": 105, "pattern": 19, "track": 3, "start": 0, "length": 8 * B, "loop": True, "loopStart": 0, "loopLength": 48, "name": "Eighth loop"},
    {"id": 106, "kind": "audio", "source": 60, "track": 4, "start": 0, "length": 8 * B, "pattern": 0, "loop": True},
    {"id": 107, "pattern": 19, "track": 5, "start": 0, "length": 0},
    {"id": 108, "kind": "audio", "source": 60, "track": 5, "start": 2 * B, "length": 192, "pattern": 0},
    {"id": 109, "pattern": 29, "track": 6, "start": 3 * B + 96, "length": 9 * B, "loop": True, "loopStart": 0, "loopLength": 288, "name": "Dotted"},
]
song["clipLoops"] = True
song["nextId"] = 300
path = os.path.join(OUT, "song.gridline.json")
json.dump(song, open(path, "w"))
# The loop of each pattern region and its first loop point (ticks from its start).
LOOPS = {101: (B, B), 102: (2 * B, 2 * B - 192), 104: (B, B), 105: (48, 48), 109: (288, 288)}
START = {c["id"]: c["start"] for c in song["playlist"]}


def run(tag, layout, zoom, script, extra=()):
    cfg = os.path.join(OUT, "cfg_" + tag)
    os.makedirs(os.path.join(cfg, "gridline"))
    with open(os.path.join(cfg, "gridline", "settings.json"), "w") as f:
        f.write('{"format": "gridline-settings", "values": {"ui.layoutChosen": true, "ui.layoutDev": true, "ui.layoutPreset": "%s"}}\n' % layout)
    sp = os.path.join(OUT, tag + ".txt")
    with open(sp, "w") as f:
        f.write(script)
    env = dict(os.environ, SDL_AUDIODRIVER="dummy", GRIDLINE_NO_PLUGIN_SCAN="1", GRIDLINE_HIDE_PLUGINS="all", GRIDLINE_CONFIG_HOME=cfg, HOME=cfg)
    r = subprocess.run([APP, path, "--hidden", "--no-ui-sounds", "--no-update-check", "--fixed-dt", "16", "--size", "1440x900", "--layout", layout,
                        "--show", "playlist", "--zoom", str(zoom), "--dev-input", sp, *extra], env=env, capture_output=True, text=True, timeout=300)
    with open(os.path.join(OUT, tag + ".log"), "w") as f:
        f.write(r.stdout + "\n" + r.stderr)
    if r.returncode != 0:
        failures.append("%s: gridline-app failed (%d)" % (tag, r.returncode))
    return r.stdout


def geom(out):
    m = re.search(r"plgeom frame \d+: valid 1 x0 (-?[0-9.]+) pxPerTick ([0-9.]+)", out)
    clips = {int(c[0]): tuple(float(v) for v in c[1:]) for c in re.findall(r"\n  clip (\d+) x ([-0-9.]+) ([-0-9.]+) y ([-0-9.]+) ([-0-9.]+) head", out)}
    divots = [(int(d[0]), int(d[1]), float(d[2]), float(d[3]), d[4] == "1") for d in re.findall(r"\n  divot (\d+) at (\d+) x ([-0-9.]+) cx ([-0-9.]+) top ([01])", out)]
    grid = re.search(r"\n  view [^\n]* grid ([-0-9.]+) ([-0-9.]+) ([-0-9.]+)", out)
    return (float(m.group(1)), float(m.group(2))) if m else (None, None), clips, divots, float(grid.group(1)) if grid else 0.0


def ppm(p):
    b = open(p, "rb").read()
    m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+255\s", b)
    w, h = int(m.group(1)), int(m.group(2))
    return w, h, b[m.end():]


def lum(img, x, y):
    w, _, px = img
    o = (y * w + x) * 3
    return px[o] + 2 * px[o + 1] + px[o + 2]


SCRIPT = "4 typing off\n6 pltool select\n50 plgeom\n54 shot \"%s\"\n58 quit discard\n"
counts = {}
for zoom in (0.25, 0.5, 1, 2.5):
    tag = "logic_z%s" % zoom
    shot = os.path.join(OUT, tag + ".ppm")
    out = run(tag, "logic", zoom, SCRIPT % shot)
    (x0, ppt), clips, divots, gx = geom(out)
    if x0 is None:
        failures.append(tag + ": no playlist geometry")
        continue
    gridX1 = 1440.0
    bad = []
    for cid, at, x, cx, top in divots:
        want = x0 + (START[cid] + at) * ppt
        if abs(x - want) > 0.5:
            bad.append("%d@%d x %.2f want %.2f" % (cid, at, x, want))
        if cid in LOOPS:
            loop, first = LOOPS[cid]
            if at < first or (at - first) % loop:
                bad.append("%d@%d not a loop point" % (cid, at))
    check(divots and not bad, "%s: %d divots, each on its loop point's grid edge within 0.5 px %s" % (tag, len(divots), " ".join(bad[:4])))
    check(not any(d[0] in (107, 108) for d in divots), tag + ": no divots on the unlooped regions")
    close = []
    for cid in set(d[0] for d in divots):
        xs = sorted(d[2] for d in divots if d[0] == cid)
        close += ["%d: %.1f" % (cid, b - a) for a, b in zip(xs, xs[1:]) if b - a < 19.5]
    check(not close, tag + ": shown divots of a region are at least 20 px apart " + " ".join(close[:4]))
    counts[zoom] = {cid: sum(1 for d in divots if d[0] == cid) for cid in LOOPS}
    # Every loop point of 101 (one-bar loop) in view and clear of the ends has its divot when a bar is wide.
    if 101 in clips and B * ppt >= 20:
        cx0, cx1 = clips[101][0], clips[101][1]
        want = [at for at in range(B, 8 * B, B) if cx0 + 12 < x0 + at * ppt < min(cx1, gridX1) - 12 and x0 + at * ppt > gx + 12]
        got = sorted(d[1] for d in divots if d[0] == 101)
        check(got == want, "%s: every loop point of the one-bar loop in view has its divot (%s / %s)" % (tag, got, want))
    # The eighth-note loop: when its repeats are under 20 px, divots thin out (the first loop point always shows).
    # The eighth-note loop: repeats under 24 px (3 divot widths) show every 2nd divot (the first always); under 12 px none.
    if 105 in clips:
        rep = 48 * ppt
        cx0, cx1 = clips[105][0], min(clips[105][1], gridX1)
        points = sum(1 for at in range(48, 8 * B, 48) if cx0 + 14 < x0 + at * ppt < cx1 - 14)
        shown = [d[1] for d in divots if d[0] == 105]
        if rep * 2 < 24:
            check(not shown, "%s: thin: eighth-note repeats of %.1f px show no divots" % (tag, rep))
        elif rep < 24:
            check(shown and len(shown) <= points // 2 + 2 and all((at - 48) % 96 == 0 for at in shown),
                  "%s: thin: the eighth-note loop (%.1f px) shows %d of %d loop points, every 2nd" % (tag, rep, len(shown), points))
    # 102 entered mid-loop: its first divot after the partial first pass.
    if 102 in clips and 2 * B * ppt >= 20:
        check(any(d[0] == 102 and d[1] == 2 * B - 192 for d in divots), tag + ": the region entered mid-loop has its first divot after the partial pass")
    if zoom == 1:
        img = ppm(shot)
        tops = [d for d in divots if d[0] == 101 and d[4]]
        if tops:
            _, at, x, cx, _ = tops[0]
            y0 = int(clips[101][2])
            xc = int(x)
            inside, beside = lum(img, xc, y0), lum(img, xc + 10, y0)
            check(inside + 120 < beside, "pixels: the top divot shows the ground at its centre (%d), the region beside it (%d)" % (inside, beside))
            y1 = int(clips[101][3]) - 1
            check(lum(img, xc, y1) + 120 < lum(img, xc + 10, y1), "pixels: ... and the bottom one too")
        else:
            failures.append("zoom 1: no top divot on the one-bar loop")
if 0.25 in counts and 1 in counts:
    # Zoomed out the one-bar loop thins out too (fewer divots for the same eight bars).
    check(0 < counts[0.25].get(101, 0) < counts[1].get(101, 0), "thin: the one-bar loop shows fewer divots zoomed out (%s, %s)" % (counts[0.25].get(101), counts[1].get(101)))
# None in Studio or Clips (their notches are their own).
for layout in ("studio", "live"):
    out = run(layout, layout, 1, SCRIPT % os.path.join(OUT, layout + ".ppm"))
    check("plgeom frame" in out and "\n  divot " not in out, layout + ": no Smooth divots")
if failures:
    print("FAILED:\n  " + "\n  ".join(failures))
    sys.exit(1)
print("k388_smooth_divots_app: all checks passed")
