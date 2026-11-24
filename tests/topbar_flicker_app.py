#!/usr/bin/env python3
# Gridline - flick-1119: no part of any layout's top bar alternates between frames. Each layout (Studio, Clips,
# Smooth, Tracker) runs in the real app (hidden, isolated config folder and HOME, SDL's dummy audio driver, the
# system menu bar as on macOS) with a 2048-frame audio buffer, so most frames finish no audio buffer (as a large
# buffer or a 120 Hz display does): every frame of the bar is saved, first stopped, then playing.
#   - stopped, the bar stays as it is from frame to frame (no window drawn on alternate frames, no text or layout
#     that toggles): only the processing load's figures may change (Studio's and Clips' "CPU n %"), and nothing
#     flips back;
#   - playing, nothing flips back: a part that changes in one frame and is back as it was the next (a meter drawn
#     empty on the frames no audio arrived in, a pill between two widths, a hint) fails. Meters still move: their
#     level end may step back over a few pixels (rise on a buffer, fall in between).
# Before flick-1119 Smooth's master volume pill drew the raw peak since the last frame: empty on every frame no
# audio buffer finished in, so it flashed on and off while the song played.
# The readouts that count on their own while playing (the song position, the metronome's beat, the processing load;
# the app lists where it drew them: "barcounters") are left out while playing: on a slow machine (a software renderer
# on a loaded runner, frames a fifth of a second or more apart) a counter's digits or a beat light can show again
# what they showed two frames before, which is no flash.
#   python3 topbar_flicker_app.py <gridline-app> <song.gridline.json> <out dir> [only-layout]
import os
import re
import shutil
import subprocess
import sys

APP, DEMO, OUT = sys.argv[1], sys.argv[2], sys.argv[3]
ONLY = sys.argv[4] if len(sys.argv) > 4 else ""
W, H = 1440, 900
BAR = 64  # the rows checked: every layout's top bar (Smooth's is the tallest, 64 px without its menu row)
IDLE = range(40, 70)
PLAY = range(100, 140)
# A flip back larger than this many pixels in one frame is a part flashing (the pill drawn empty was ~580 px);
# a meter's level end stepping back is a few pixels per bar (rise on a buffer, fall in between).
FLIP_MAX = 120
# Stopped: only the load figures may change (a digit of Clips' "CPU n %" is ~100 px); anything bigger fails.
IDLE_MAX = 200
# The load figures' box: one line of text a few digits wide (Clips' "CPU n %" is 46 x 9 px).
LOAD_W, LOAD_H = 90, 16
failures = []


def fail(msg):
    failures.append(msg)
    print("FAIL: " + msg)


def read_bar(path):
    with open(path, "rb") as f:
        data = f.read()
    # P6\n<w> <h>\n255\n<rgb>
    parts = data.split(b"\n", 3)
    w, h = (int(x) for x in parts[1].split())
    if w != W or h < BAR:
        raise RuntimeError("%s: unexpected size %dx%d" % (path, w, h))
    return parts[3][: w * BAR * 3]


def changed(a, b):
    # the pixels (index) that differ by more than a little (anti-aliasing noise aside)
    out = set()
    for i in range(0, len(a), 3):
        if abs(a[i] - b[i]) + abs(a[i + 1] - b[i + 1]) + abs(a[i + 2] - b[i + 2]) > 24:
            out.add(i // 3)
    return out


def box(px):
    xs = [p % W for p in px]
    ys = [p // W for p in px]
    return "x %d-%d y %d-%d" % (min(xs), max(xs), min(ys), max(ys))


def run(layout):
    out = os.path.join(OUT, layout)
    shutil.rmtree(out, ignore_errors=True)
    cfg = os.path.join(out, "cfg")
    os.makedirs(os.path.join(cfg, "gridline"))
    os.makedirs(os.path.join(out, "home"))
    os.makedirs(os.path.join(out, "f"))
    with open(os.path.join(cfg, "gridline", "settings.json"), "w") as f:
        f.write('{"format": "gridline-settings", "values": {"ui.layoutChosen": true, "ui.layout": "%s", "keys.profile": "%s", '
                '"ui.menuStyle": "%s", "audio.bufferFrames": 2048}}\n' % (layout, layout, layout))
    shutil.copy(DEMO, os.path.join(out, "song.gridline.json"))
    lines = ["5 toasts clear", "6 move 700 600"]
    lines += ['%d shot "%s"' % (k, os.path.join(out, "f", "i%d.ppm" % k)) for k in IDLE]
    lines.append("80 key space")
    lines += ['%d shot "%s"' % (k, os.path.join(out, "f", "p%d.ppm" % k)) for k in PLAY]
    lines += ["%d barcounters" % k for k in (PLAY[0], PLAY[-1] + 1)]
    lines.append("150 quit discard")
    with open(os.path.join(out, "input.txt"), "w") as f:
        f.write("\n".join(lines) + "\n")
    env = dict(os.environ, SDL_AUDIODRIVER="dummy", GRIDLINE_NO_PLUGIN_SCAN="1", GRIDLINE_CONFIG_HOME=cfg, HOME=os.path.join(out, "home"))
    cmd = [APP, os.path.join(out, "song.gridline.json"), "--hidden", "--no-ui-sounds", "--system-menu", "--size", "%dx%d" % (W, H),
           "--layout", layout, "--dev-input", os.path.join(out, "input.txt"), "--frames", "170"]
    p = subprocess.run(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=300)
    with open(os.path.join(out, "run.log"), "wb") as f:
        f.write(p.stdout)
    if p.returncode != 0:
        fail("%s: gridline-app failed (%d), see %s" % (layout, p.returncode, os.path.join(out, "run.log")))
        return
    counters = set()
    named = set()
    for m in re.finditer(r"^barcounters frame \d+: (.*)$", p.stdout.decode("utf-8", "replace"), re.M):
        for part in filter(None, (t.strip() for t in m.group(1).split(";"))):
            what, x0, y0, x1, y1 = part.split()
            named.add(what)
            x0, y0, x1, y1 = (int(float(v)) for v in (x0, y0, x1, y1))
            counters |= {y * W + x for y in range(max(0, y0 - 2), min(BAR, y1 + 3)) for x in range(max(0, x0 - 2), min(W, x1 + 3))}
    if counters:
        print("%s: playing, the readouts that count are left out (%s)" % (layout, ", ".join(sorted(named))))
    frames = {}
    for k in list(IDLE) + list(PLAY):
        path = os.path.join(out, "f", ("i%d.ppm" if k in IDLE else "p%d.ppm") % k)
        if not os.path.exists(path):
            fail("%s: frame %d was not saved" % (layout, k))
            return
        frames[k] = read_bar(path)

    worst = 0
    idle = list(IDLE)
    # hunt-1119: the load figures ("CPU n %") change with the machine's load, by a digit or two (two digits are
    # over IDLE_MAX px, so a busy machine failed the test): the one small text-sized box they change in while
    # stopped is left out of every comparison. A part that changes anywhere else, or over a larger box (a
    # window, a pill, a meter), still counts.
    every = set()
    for a, b in zip(idle, idle[1:]):
        every |= changed(frames[a], frames[b])
    load = set()
    if every:
        xs = [p % W for p in every]
        ys = [p // W for p in every]
        if max(xs) - min(xs) <= LOAD_W and max(ys) - min(ys) <= LOAD_H:
            load = {y * W + x for y in range(max(0, min(ys) - 2), min(BAR, max(ys) + 3)) for x in range(max(0, min(xs) - 4), min(W, max(xs) + 5))}
            print("%s: the load figures change in %s (left out)" % (layout, box(load)))
    for a, b in zip(idle, idle[1:]):
        d = changed(frames[a], frames[b]) - load
        worst = max(worst, len(d))
        if len(d) > IDLE_MAX:
            fail("%s: stopped, the bar changed between frames %d and %d (%d px, %s)" % (layout, a, b, len(d), box(d)))
    for a, b, c in zip(idle, idle[1:], idle[2:]):
        back = changed(frames[b], frames[c]) - changed(frames[a], frames[c]) - load
        if len(back) > FLIP_MAX:
            fail("%s: stopped, a part of the bar flips back on frame %d (%d px, %s)" % (layout, c, len(back), box(back)))
    flips, moving = 0, 0
    play = list(PLAY)
    for a, b, c in zip(play, play[1:], play[2:]):
        d = changed(frames[b], frames[c]) - load
        if d:
            moving += 1  # (the counters too: the position moving shows the transport runs)
        back = d - counters - changed(frames[a], frames[c])  # changed in c and back as it was in a (a counter aside)
        flips = max(flips, len(back))
        if len(back) > FLIP_MAX:
            fail("%s: playing, a part of the bar flips back on frame %d (%d px, %s)" % (layout, c, len(back), box(back)))
    if moving == 0:
        fail("%s: playing, the bar never changed (the transport did not run?)" % layout)
    print("%s: stopped max change %d px, playing max flip back %d px, %d of %d frames moved" % (layout, worst, flips, moving, len(play) - 2))


for lay in ["studio", "live", "logic", "tracker"]:
    if not ONLY or ONLY == lay:
        run(lay)

if failures:
    print("%d failure(s)" % len(failures))
    sys.exit(1)
print("topbar_flicker_app: ok")
