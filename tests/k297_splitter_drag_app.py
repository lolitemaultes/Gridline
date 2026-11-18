#!/usr/bin/env python3
# Gridline - rsz-1118: every divider between the panels of Clips and Smooth, dragged in the real app (hidden,
# isolated config folder and HOME, SDL's dummy audio driver), with a trace of every frame ("layout
# splittrace": the pointer, the held divider, the cursor the frame ends with, each region's node and the
# window drawn in it). For each divider:
#   - the edge follows the pointer within 1 px on every frame of the drag (slow, fast, reversing);
#   - past a limit it stops there (the region's own min / max, or the neighbour's minimum) and stays put, and
#     follows again as soon as the pointer comes back past it (no jump, no oscillation);
#   - every window is drawn where its region is on every frame (no frame drawn with a stale layout);
#   - a band beside the main view keeps its size: the main view takes the change (Clips' mixer band under the
#     detail band's divider, Smooth's Inspector beside the Library's, the lower pane's title strip);
#   - the resize cursor shows through the whole drag;
#   - nothing moves when the button comes up, the size is kept in the layout's region state, and the next
#     run opens the region at that size.
#   python3 k297_splitter_drag_app.py <gridline-app> <song.gridline.json> <out dir> [WxH] [only-tag-prefix] [extra app args]
import os
import re
import shutil
import subprocess
import sys

APP, DEMO, OUT = sys.argv[1], sys.argv[2], sys.argv[3]
SIZE = sys.argv[4] if len(sys.argv) > 4 else "1440x900"
ONLY = sys.argv[5] if len(sys.argv) > 5 else ""
EXTRA = sys.argv[6].split() if len(sys.argv) > 6 else []  # e.g. "--ui-scale 2"
EDGE_AXIS = {'L': 0, 'R': 0, 'T': 1, 'B': 1}
GROW = {'L': 1, 'R': -1, 'T': 1, 'B': -1}  # the pointer direction that makes the region larger
CURSOR = {0: 4, 1: 3}  # ImGuiMouseCursor_ResizeEW, ImGuiMouseCursor_ResizeNS
failures = []


def fail(msg):
    failures.append(msg)
    print("FAIL: " + msg)


def run(tag, layout, script, frames, cfgtag=None, fresh=True):
    cfg = os.path.join(OUT, "cfg_" + (cfgtag or tag))
    if fresh:
        shutil.rmtree(cfg, ignore_errors=True)
        os.makedirs(os.path.join(cfg, "gridline"))
        os.makedirs(os.path.join(cfg, "home"))
        with open(os.path.join(cfg, "gridline", "settings.json"), "w") as f:
            f.write('{"format": "gridline-settings", "values": {"ui.layoutChosen": true, "ui.layout": "%s"}}\n' % layout)
    song = os.path.join(cfg, "song.gridline.json")
    shutil.copyfile(DEMO, song)
    sp = os.path.join(OUT, tag + ".txt")
    with open(sp, "w") as f:
        f.write(script)
    env = dict(os.environ, SDL_AUDIODRIVER="dummy", GRIDLINE_NO_PLUGIN_SCAN="1", GRIDLINE_CONFIG_HOME=cfg, HOME=os.path.join(cfg, "home"),
               GRIDLINE_SAVE_LAYOUT="1")
    r = subprocess.run([APP, song, "--hidden", "--no-ui-sounds", "--no-update-check", "--fixed-dt", "16", "--size", SIZE, "--dev-input", sp,
                        "--frames", str(frames)] + EXTRA, env=env, capture_output=True, text=True, timeout=240)
    with open(os.path.join(OUT, tag + ".log"), "w") as f:
        f.write(r.stdout + "\n---- stderr ----\n" + r.stderr)
    if r.returncode != 0:
        fail("%s: gridline-app failed (%d)" % (tag, r.returncode))
    return r.stdout


def parse(out):
    frames = []
    for l in out.splitlines():
        m = re.match(r'splittrace frame (\d+) mouse ([-\d.]+) ([-\d.]+) held (\d) cursor (-?\d+)(.*)', l)
        if not m:
            continue
        fr = dict(frame=int(m.group(1)), m=(float(m.group(2)), float(m.group(3))), held=int(m.group(4)), cursor=int(m.group(5)), regions={})
        for part in m.group(6).split(' | ')[1:]:
            t = part.split()
            nums = [float(v) for v in t[2:6] + t[7:11] + t[12:16]]
            fr['regions'][t[0]] = dict(node=nums[0:4], inner=nums[4:8], win=nums[8:12])
        frames.append(fr)
    return frames


def edge_of(node, edge):
    x, y, w, h = node
    return {'L': x + w, 'R': x, 'T': y + h, 'B': y}[edge]


def setup_lines(setup):
    return "".join("%d %s\n" % (5 + 4 * i, c) for i, c in enumerate(setup))


def probe(tag, layout, setup):
    s = setup_lines(setup) + "40 layout splittrace on\n41 layout splittrace off\n42 quit discard\n"
    fr = parse(run(tag + "_probe", layout, s, 50))
    if not fr:
        fail("%s: no trace" % tag)
        return None
    return fr[-1]


def drag(tag, layout, setup, region, edge, keep=()):
    """Drags the divider on the `edge` side of `region`; the regions in `keep` must keep their size."""
    if ONLY and not tag.startswith(ONLY):
        return
    base = probe(tag, layout, setup)
    if not base or region not in base['regions']:
        fail("%s: region %s not shown" % (tag, region))
        return
    ax = EDGE_AXIS[edge]
    node = base['regions'][region]['node']
    # Just outside the region's node, in the gap between the panels (1 to 6 px).
    e = edge_of(node, edge)
    out_side = 1 if edge in ('L', 'T') else -1
    if ax == 0:
        x, y = e + out_side * 1.0, node[1] + node[3] * 0.5
    else:
        x, y = node[0] + node[2] * 0.5, e + out_side * 1.0
    g = GROW[edge]
    # Slow, then fast, past the far limit, back past the near limit, and back into range.
    steps = [g * 1, g * 2, g * 3] + [g * 9] * 6 + [g * 70] * 12 + [-g * 13] * 5 + [-g * 90] * 14 + [g * 7] * 6 + [g * 31] * 4
    lines = setup_lines(setup) + "40 layout splittrace on\n40 move %g %g\n" % (x, y)
    f = 44
    lines += "%d down 0\n" % f
    f += 1
    for d in steps:
        if ax == 0:
            x += d
        else:
            y += d
        lines += "%d move %g %g\n" % (f, x, y)
        f += 1
    lines += "%d up 0\n%d layout splittrace off\n%d quit discard\n" % (f, f + 5, f + 6)
    out = run(tag, layout, lines, f + 10)
    fr = parse(out)
    held = [q for q in fr if q['held']]
    if len(held) < len(steps) - 2:
        fail("%s: the divider was not held through the drag (%d of %d frames)" % (tag, len(held), len(steps)))
        return
    pre = [q for q in fr if not q['held'] and q['frame'] < held[0]['frame']][-1]
    off = pre['m'][ax] - edge_of(pre['regions'][region]['node'], edge)
    edges = [edge_of(q['regions'][region]['node'], edge) for q in held]
    lo_e, hi_e = min(edges), max(edges)
    stops = 0
    for q, ed in zip(held, edges):
        want = q['m'][ax] - off
        if abs(ed - want) > 1.0:
            # Only at a limit: the edge at the furthest it went, the pointer beyond it.
            if (abs(ed - hi_e) < 0.5 and want > ed) or (abs(ed - lo_e) < 0.5 and want < ed):
                stops += 1
            else:
                fail("%s: frame %d the edge is at %.0f, the pointer asks for %.0f" % (tag, q['frame'], ed, want))
        for rid, r in q['regions'].items():
            if r['win'][2] >= 0 and max(abs(a - b) for a, b in zip(r['win'], r['inner'])) > 1.0:
                fail("%s: frame %d %s drawn at %s, its region is at %s" % (tag, q['frame'], rid, r['win'], r['inner']))
        if q['cursor'] != CURSOR[ax]:
            fail("%s: frame %d the cursor is %d, not the resize arrow" % (tag, q['frame'], q['cursor']))
        for k in keep:
            if k in q['regions'] and k in pre['regions'] and abs(q['regions'][k]['node'][2 + ax] - pre['regions'][k]['node'][2 + ax]) > 0.5:
                fail("%s: frame %d %s changed size (%.0f -> %.0f)" % (tag, q['frame'], k, pre['regions'][k]['node'][2 + ax], q['regions'][k]['node'][2 + ax]))
    if stops < 4:
        fail("%s: the drag never stopped at a limit (%d frames)" % (tag, stops))
    sizes = [q['regions'][region]['node'][2 + ax] for q in held]
    if any(abs(sv - round(sv)) > 0.01 for sv in sizes):
        fail("%s: a size off the pixel grid" % tag)
    print("%s: grab offset %+.1f, size %.0f..%.0f, %d frames, %d at a limit" % (tag, off, min(sizes), max(sizes), len(held), stops))
    post = [q for q in fr if not q['held'] and q['frame'] > held[-1]['frame']]
    for q in post:
        for rid, r in q['regions'].items():
            if rid in held[-1]['regions'] and max(abs(a - b) for a, b in zip(r['node'], held[-1]['regions'][rid]['node'])) > 0.5:
                fail("%s: frame %d after the release %s moved %s -> %s" % (tag, q['frame'], rid, held[-1]['regions'][rid]['node'], r['node']))
                break
    final = sizes[-1]
    # Kept in the layout's region state, and the next run opens the region at that size.
    with open(os.path.join(OUT, "cfg_" + tag, "gridline", "settings.json")) as fh:
        saved = fh.read()
    # (a region size: "<region>": n; an exclusive region's member size: "<region>|<window>": n)
    if abs(final - pre['regions'][region]['node'][2 + ax]) > 0.5 and not re.search(r'%s(\|[^"\\]+)?\\?":%d\b' % (re.escape(region), int(final)), saved):
        fail("%s: the region state does not keep %s at %d" % (tag, region, int(final)))
    again = parse(run(tag + "_again", layout, "40 layout splittrace on\n41 layout splittrace off\n42 quit discard\n", 50, cfgtag=tag, fresh=False))
    if not again or region not in again[-1]['regions']:
        fail("%s: %s not shown after a restart" % (tag, region))
    elif abs(again[-1]['regions'][region]['node'][2 + ax] - final) > 0.5:
        fail("%s: after a restart %s is %.0f, not %.0f" % (tag, region, again[-1]['regions'][region]['node'][2 + ax], final))


os.makedirs(OUT, exist_ok=True)
clips = ["toasts clear", "layout open Mixer"]
drag("clips_browser", "live", clips, "live.browser", "L")
drag("clips_info", "live", clips, "live.info", "B")
drag("clips_device", "live", clips, "live.device", "B", keep=("live.mixer",))
drag("clips_mixer", "live", clips, "live.mixer", "B", keep=("live.device",))
drag("clips_side", "live", clips + ["layout show live.side"], "live.side", "R", keep=("live.browser",))
smooth = ["toasts clear", "layout show logic.lower", "layout open Mixer", "layout show logic.library", "layout show logic.right"]
drag("smooth_library", "logic", smooth, "logic.library", "L", keep=("logic.inspector", "logic.right"))
drag("smooth_inspector", "logic", smooth, "logic.inspector", "L", keep=("logic.library", "logic.right"))
drag("smooth_right", "logic", smooth, "logic.right", "R", keep=("logic.library", "logic.inspector"))
drag("smooth_lower", "logic", smooth, "logic.lower", "B", keep=("logic.right", "logic.inspector"))

if failures:
    print("%d failures" % len(failures))
    sys.exit(1)
print("every divider follows the pointer")
