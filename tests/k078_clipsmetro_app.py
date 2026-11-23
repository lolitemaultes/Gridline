#!/usr/bin/env python3
# Gridline - 0.7.8: the Clips control bar's metronome, in the real app (hidden, an isolated config folder
# and HOME, SDL's dummy audio driver), clicked with real SDL pointer events:
#   1. one box right after the meter box and before the launch grid box, as tall as they are, its arrow
#      part at its right edge;
#   2. a click on its main part switches the metronome on, another off; its hint names its key, and the
#      key switches it too; the box fills with the lit colour while on (pixels of the pictures), in the
#      dark and the light theme;
#   3. a click on the arrow opens its menu (the metronome stays as it was); Escape closes it;
#   4. while the song plays the filled circle swaps sides on every beat (an odd beat: the left one), at
#      the playhead's beat; stopped, the right one is filled again.
# Pictures in <out dir>/dark and <out dir>/light: off, on, hover, menu (and dark/playing) (.ppm).
#   python3 k078_clipsmetro_app.py <gridline-app> <song> <out dir>
import json
import math
import os
import re
import shutil
import subprocess
import sys

APP, SONG, OUT = sys.argv[1], sys.argv[2], sys.argv[3]
failures = []


def check(cond, msg):
    if cond:
        print("ok - " + msg)
    else:
        failures.append(msg)
        print("FAIL: " + msg)


NUM = r"(-?[0-9.]+)"
RECT = " ".join([NUM] * 4)
LINE = re.compile(r"clipsmetro frame (\d+): on ([01]) playing ([01]) left ([01]) menu ([01]) countin (-?\d+) beat " + NUM + " clipssig " + RECT + " metro " + RECT +
                  " clipsmetroarrow " + RECT + " clipsquant " + RECT)


def run(tag, light, script, frames):
    """One hidden run of the script (frame, command) in the Clips layout; its clipsmetro lines by frame, and its output."""
    work = os.path.join(OUT, tag)
    cfg = os.path.join(work, "cfg")
    os.makedirs(os.path.join(cfg, "gridline"))
    os.makedirs(os.path.join(work, "home"))
    values = {"ui.layoutChosen": True, "ui.layoutDev": True, "ui.layout": "live", "keys.profile": "live", "ui.menuStyle": "live"}
    if light:
        values.update({"theme.mode": "light", "ui.darkTheme": False})
    with open(os.path.join(cfg, "gridline", "settings.json"), "w") as f:
        json.dump({"format": "gridline-settings", "values": values}, f)
    song = os.path.join(work, "song.gridline.json")
    shutil.copyfile(SONG, song)
    inp = os.path.join(work, "in.txt")
    with open(inp, "w") as f:
        for fr, cmd in script:
            f.write("%d %s\n" % (fr, cmd.replace("@", work + os.sep)))
    env = dict(os.environ)
    env.update(SDL_AUDIODRIVER="dummy", GRIDLINE_NO_PLUGIN_SCAN="1", GRIDLINE_HIDE_PLUGINS="all", GRIDLINE_CONFIG_HOME=cfg, HOME=os.path.join(work, "home"))
    r = subprocess.run([APP, song, "--hidden", "--no-ui-sounds", "--no-update-check", "--fixed-dt", "16", "--size", "1440x900", "--frames", str(frames),
                        "--screenshot", os.path.join(work, "end.ppm"), "--dev-input", inp], env=env, capture_output=True, text=True, timeout=180)
    with open(os.path.join(work, "run.out"), "w") as f:
        f.write(r.stdout + "\n---- stderr ----\n" + r.stderr)
    if r.returncode != 0:
        print("FAIL: %s: gridline-app exited %d\n%s" % (tag, r.returncode, r.stderr[-2000:]))
        sys.exit(1)
    st = {}
    for m in LINE.finditer(r.stdout):
        g = m.groups()
        v = [float(x) for x in g[7:]]
        st[int(g[0])] = dict(on=int(g[1]), playing=int(g[2]), left=int(g[3]), menu=int(g[4]), countin=int(g[5]), beat=float(g[6]), sig=v[0:4],
                             metro=v[4:8], arrow=v[8:12], quant=v[12:16])
    missing = [fr for fr, c in script if c == "clipsmetro" and fr not in st]
    if missing:
        print("FAIL: %s: no full clipsmetro line at frames %s\n%s" % (tag, missing, r.stdout[-4000:]))
        sys.exit(1)
    return st, r.stdout


shutil.rmtree(OUT, ignore_errors=True)
AWAY = "sdlmove 700 600"
dark, out = run("dark", False, [
    (4, "typing off"), (5, "toasts clear"),
    (10, "clipsmetro"), (11, 'shot "@off.ppm"'),
    (12, "sdlat metro"), (15, "hint"), (16, "sdldown 0"), (17, "sdlup 0"),
    (19, "clipsmetro"), (20, 'shot "@hover.ppm"'), (21, AWAY), (24, 'shot "@on.ppm"'),
    (26, "sdlat metro"), (28, "sdldown 0"), (29, "sdlup 0"), (31, "clipsmetro"),
    (32, AWAY),
    (33, "sdlkey o down"), (34, "sdlkey o up"), (36, "clipsmetro"),
    (37, "sdlkey o down"), (38, "sdlkey o up"), (40, "clipsmetro"),
    (42, "sdlat clipsmetroarrow"), (44, "sdldown 0"), (45, "sdlup 0"), (48, "clipsmetro"), (49, 'shot "@menu.ppm"'),
    (50, "sdlkey escape down"), (51, "sdlkey escape up"), (54, "clipsmetro"),
    (56, "sdlat metro"), (58, "sdldown 0"), (59, "sdlup 0"), (60, AWAY),
    (62, "sdlat play"), (64, "sdldown 0"), (65, "sdlup 0"), (66, AWAY),
    (70, "seekbeat 4.3"), (73, "clipsmetro"),
    (75, "seekbeat 5.3"), (78, "clipsmetro"), (79, 'shot "@playing.ppm"'),
    (80, "seekbeat 6.3"), (83, "clipsmetro"),
    (85, "seekbeat 7.3"), (88, "clipsmetro"),
    (90, "sdlat stop"), (92, "sdldown 0"), (93, "sdlup 0"), (94, AWAY), (97, "clipsmetro"),
], 100)
light, _ = run("light", True, [
    (4, "typing off"), (5, "toasts clear"),
    (10, "clipsmetro"), (11, 'shot "@off.ppm"'),
    (12, "sdlat metro"), (16, "sdldown 0"), (17, "sdlup 0"), (19, "clipsmetro"), (20, 'shot "@hover.ppm"'), (21, AWAY), (24, 'shot "@on.ppm"'),
    (26, "sdlat clipsmetroarrow"), (28, "sdldown 0"), (29, "sdlup 0"), (32, "clipsmetro"), (33, 'shot "@menu.ppm"'),
    (34, "sdlkey escape down"), (35, "sdlkey escape up"),
    (38, "sdlat metro"), (40, "sdldown 0"), (41, "sdlup 0"), (42, AWAY), (45, "clipsmetro"),
], 48)

# 1. Where it sits.
f = dark[10]
sig, met, arr, q = f["sig"], f["metro"], f["arrow"], f["quant"]
check(f["on"] == 0 and f["left"] == 0 and f["menu"] == 0, "off at first, the right circle filled, no menu")
check(0 <= met[0] - sig[2] <= 6, "the box starts just after the meter box (gap %.1f px)" % (met[0] - sig[2]))
check(abs(arr[0] - met[2]) < 0.6, "the arrow part continues the box (one box: %.1f / %.1f)" % (met[2], arr[0]))
check(0 <= q[0] - arr[2] <= 6, "the launch grid box follows just after it (gap %.1f px)" % (q[0] - arr[2]))
check(all(abs(met[i] - sig[i]) < 0.6 and abs(arr[i] - sig[i]) < 0.6 and abs(q[i] - sig[i]) < 0.6 for i in (1, 3)), "as tall as its neighbours, on the same line")
w = arr[2] - met[0]
check(40 <= w <= 60 and arr[2] - arr[0] < w * 0.4, "one compact box (%.0f px wide, the arrow part %.0f)" % (w, arr[2] - arr[0]))
check(light[10]["metro"] == met and light[10]["arrow"] == arr, "the same place in the light theme")

# 2. Click, hint, key.
check(dark[19]["on"] == 1 and dark[19]["menu"] == 0, "a click on the circles switches it on")
check(dark[31]["on"] == 0, "another click switches it off")
hm = re.search(r"hint frame 15: (.*)", out)
check(hm is not None and re.match(r"Metronome \(O\)", hm.group(1)) is not None, "its hint names its key (%s)" % (hm.group(1) if hm else "-"))
check(dark[36]["on"] == 1 and dark[40]["on"] == 0, "its key switches it on and off")
check(light[19]["on"] == 1 and light[45]["on"] == 0, "light theme: a click switches it on, another off")

# 3. The arrow's menu.
check(dark[48]["menu"] == 1 and dark[48]["on"] == 0, "the arrow opens the menu and leaves the metronome as it was")
check(dark[54]["menu"] == 0, "Escape closes the menu")
check(light[32]["menu"] == 1 and light[32]["on"] == 1, "light theme: the arrow opens the menu")

# 4. The beat while playing.
for fr in (73, 78, 83, 88):
    s = dark[fr]
    want = int(math.floor(s["beat"])) % 2
    check(s["playing"] == 1 and s["on"] == 1 and s["left"] == want, "frame %d: playing at beat %.2f, the %s circle filled" % (fr, s["beat"], "left" if want else "right"))
check(dark[73]["left"] != dark[78]["left"] and dark[78]["left"] != dark[83]["left"] and dark[83]["left"] != dark[88]["left"], "the filled circle swaps on every beat")
check(dark[97]["playing"] == 0 and dark[97]["left"] == 0, "stopped: the right circle filled again")


# Pixels: the box's face is lit while on, the bar's field colour while off.
def face(path, dx=3):
    with open(path, "rb") as fh:
        data = fh.read()
    parts = data.split(maxsplit=4)
    wd = int(parts[1])
    sx = wd / 1440.0
    # a spot of the face away from the icon: just inside the left edge, mid height (dx 25.5: the filled circle's middle)
    x, y = int((met[0] + dx) * sx), int((met[1] + met[3]) * 0.5 * sx)
    i = (y * wd + x) * 3
    return tuple(parts[4][i:i + 3])


def sat(c):
    return max(c) - min(c)


for theme in ("dark", "light"):
    fo, fn = face(os.path.join(OUT, theme, "off.ppm")), face(os.path.join(OUT, theme, "on.ppm"))
    check(sat(fn) > 60 and sat(fo) < 30, "%s: the box fills with the lit colour when on %s, plain when off %s" % (theme, fn, fo))
    dot = face(os.path.join(OUT, theme, "on.ppm"), 25.5)
    check(max(dot) < 80, "%s: on the lit box the icon is dark ink %s" % (theme, dot))

print("%d failure(s)" % len(failures))
sys.exit(1 if failures else 0)
