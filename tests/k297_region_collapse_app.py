#!/usr/bin/env python3
# Gridline - gap-1118: every window that docks into a region of a shell layout (Clips, Smooth, Tracker) opened
# and closed in the real app (hidden, isolated config folder and HOME, SDL's dummy audio driver), with a trace of
# every frame ("layout splittrace": each shown region's node and the window drawn in it). For each window:
#   - opening it shows its region, with a window drawn in it, and no other region left empty;
#   - closing it (the last window there) collapses the region in the frame it closes: no node left for it, no
#     frame with an empty node, and every other region back exactly where it was before the window opened (the
#     neighbours take the room back);
#   - opening it again brings the region back at the size it had;
#   - a region holding two windows keeps its size when one of them closes and shows the other one;
#   - a region the person resized comes back at that size;
#   - a pane closed from its own title strip (Smooth's close button, drawn after the layout pass) goes in the
#     frame its windows do;
#   - the Tracker's header band (part of the layout's design) stays, its own window back in it;
#   - a window closed in the frame the layout is left: its region is gone when the person comes back.
#   python3 k297_region_collapse_app.py <gridline-app> <song.gridline.json> <out dir> [only-layout]
import os
import re
import shutil
import subprocess
import sys

APP, DEMO, OUT = sys.argv[1], sys.argv[2], sys.argv[3]
ONLY = sys.argv[4] if len(sys.argv) > 4 else ""
HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "src")
SIZE = "1440x900"
failures = []


def fail(msg):
    failures.append(msg)
    print("FAIL: " + msg)


# The windows with an open flag (app/LayoutShellUi.cpp windowFlags) and the new parts (the "N" rows).
FLAGGED = {"Browser", "Playlist", "Piano roll", "Channel rack", "Mixer", "Live pads", "Master chain", "Undo history", "Performance",
           "Settings", "Keyboard shortcuts", "Project info", "Plugin manager", "Collaboration", "Sync review", "Groove editor"}
LAYOUTS = [(1, "live", "live.main"), (2, "logic", "logic.tracks"), (3, "tracker", "tracker.pages")]
KEEP = {"tracker.header"}  # regions of the layout's own design (core/LayoutShell.cpp Region::keep)


def homes():
    rows = []
    with open(os.path.join(SRC, "core", "LayoutHomesTable.inc")) as f:
        for line in f:
            m = re.match(r'\{"(\w+)", "([^"]*)", "([^"]*)", "([^"]*)", "([^"]*)", \{(.*)\}\},?', line.strip())
            if not m:
                continue
            h = re.findall(r'\{HomeKind::(\w+), "([^"]*)", (true|false)\}', m.group(6))
            rows.append(dict(id=m.group(1), window=m.group(3), later=m.group(5), homes=h))
    return rows


def flagged(row):
    return row["window"] in FLAGGED or (row["id"].startswith("N") and row["window"] and not row["later"])


def run(tag, layout, script, frames, extra=()):
    cfg = os.path.join(OUT, "cfg_" + tag)
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
    env = dict(os.environ, SDL_AUDIODRIVER="dummy", GRIDLINE_NO_PLUGIN_SCAN="1", GRIDLINE_CONFIG_HOME=cfg, HOME=os.path.join(cfg, "home"))
    r = subprocess.run([APP, song, "--hidden", "--no-ui-sounds", "--no-update-check", "--fixed-dt", "16", "--size", SIZE, "--dev-input", sp,
                        "--frames", str(frames)] + list(extra), env=env, capture_output=True, text=True, timeout=600)
    with open(os.path.join(OUT, tag + ".log"), "w") as f:
        f.write(r.stdout + "\n---- stderr ----\n" + r.stderr)
    if r.returncode != 0:
        fail("%s: gridline-app failed (%d)" % (tag, r.returncode))
    return r.stdout


def parse(out):
    frames = {}
    for l in out.splitlines():
        m = re.match(r'splittrace frame (\d+) mouse ([-\d.]+) ([-\d.]+) held (\d) cursor (-?\d+)(.*)', l)
        if not m:
            continue
        regions = {}
        for part in m.group(6).split(' | ')[1:]:
            t = part.split()
            nums = [float(v) for v in t[2:6] + t[7:11] + t[12:16]]
            regions[t[0]] = dict(node=nums[0:4], inner=nums[4:8], win=nums[8:12])
        frames[int(m.group(1))] = regions
    return frames


def same(a, b, tol=1.0):
    return all(abs(x - y) <= tol for x, y in zip(a, b))


def empty_regions(regions):
    """Regions laid out with no window drawn in them (a gap)."""
    return [rid for rid, r in regions.items() if r['win'][2] <= 0 or r['inner'][2] <= 1 or r['inner'][3] <= 1]


STEP = 12  # frames between two commands


def toggle_suite(idx, layout, centre, rows):
    """Each window homed in a region of this layout: open, close, open again, close (one app run)."""
    cand = []
    for row in rows:
        kind, region, _ = row["homes"][idx]
        if kind not in ("Dock", "Part") or not region or region == centre or not row["window"] or not flagged(row):
            continue
        cand.append((row["window"], region))
    lines = ["5 layout splittrace on"]
    plan = []
    f = 30
    for w, region in cand:
        # Every window of the layout closed first (a clean start for this one), then open / close / open / close.
        # The default windows its region opens with it are closed again, so it is the region's last window.
        plan.append(dict(window=w, region=region, before=f + STEP - 1, open1=f + STEP, close1=f + 2 * STEP, open2=f + 3 * STEP, close2=f + 4 * STEP))
        for o, oreg in cand:
            if oreg not in KEEP:
                lines.append("%d layout close %s" % (f, o))
        for at in (f + STEP, f + 3 * STEP):
            lines.append("%d layout open %s" % (at, w))
            for o, oreg in cand:
                if o != w and oreg == region and oreg not in KEEP:
                    lines.append("%d layout close %s" % (at + 3, o))
            lines.append("%d layout close %s" % (at + STEP, w))
        f += 5 * STEP
    lines += ["%d layout splittrace off" % f, "%d quit discard" % (f + 1)]
    fr = parse(run("toggle_" + layout, layout, "\n".join(lines) + "\n", f + 6))
    if not fr:
        fail("%s: no trace" % layout)
        return
    checked = 0
    for p in plan:
        w, region = p["window"], p["region"]
        tag = "%s %s (%s)" % (layout, w, region)
        need = [p["before"], p["open1"] + 6, p["close1"], p["open2"] + 6, p["close2"]]
        if any(n not in fr for n in need):
            fail("%s: frames missing in the trace" % tag)
            continue
        before = fr[p["before"]]
        opened = fr[p["open1"] + 6]
        if region not in opened:
            # A window whose region another one holds in turn (an exclusive pane's member) or one that does not
            # open in this build: reported, not a collapse failure.
            print("note: %s: the region did not show (skipped)" % tag)
            continue
        checked += 1
        for k in range(p["open1"] + 2, p["close1"]):
            if k in fr and empty_regions(fr[k]):
                fail("%s: frame %d after opening: empty regions %s" % (tag, k, empty_regions(fr[k])))
                break
        for when in ("close1", "close2"):
            for k in range(p[when], p[when] + 4):  # the frame it closes, and the ones after it
                now = fr.get(k)
                if now is None:
                    continue
                if empty_regions(now):
                    fail("%s: frame %d (%s +%d): empty regions %s" % (tag, k, when, k - p[when], empty_regions(now)))
                    break
                if region in KEEP:
                    if region not in now:
                        fail("%s: frame %d: the layout's own region went" % (tag, k))
                    break
                if region not in before and region in now:
                    fail("%s: frame %d (%s +%d): the region stayed after its last window closed" % (tag, k, when, k - p[when]))
                    break
                # The neighbours take the room back: as before the window opened, when the same regions show
                # (opening a region inside another shows the one around it too, which stays for its own windows).
                if set(now) != set(before) - {region}:
                    continue
                for rid, r in before.items():
                    if rid == region:
                        continue
                    if rid in now and not same(now[rid]['node'], r['node']):
                        fail("%s: frame %d (%s +%d): %s at %s, was %s before the window opened" % (tag, k, when, k - p[when], rid, now[rid]['node'], r['node']))
                        break
        reopened = fr[p["open2"] + 6]
        if region not in reopened:
            fail("%s: the region did not come back when the window opened again" % tag)
        elif not same(reopened[region]['node'], opened[region]['node']):
            fail("%s: reopened at %s, was %s" % (tag, reopened[region]['node'], opened[region]['node']))
    print("%s: %d windows toggled" % (layout, checked))
    if checked < 3:
        fail("%s: only %d windows could be checked" % (layout, checked))


def two_windows():
    """Clips' side panel with two tools: one closes, the panel stays at its size and shows the other; the last
    closes and the panel goes; both open again: the panel is back at its size."""
    s = ("5 layout splittrace on\n30 layout open Performance\n31 layout open Undo history\n50 layout close Performance\n"
         "70 layout close Undo history\n90 layout open Undo history\n91 layout open Performance\n110 layout splittrace off\n111 quit discard\n")
    fr = parse(run("two_live", "live", s, 115))
    base, both = fr.get(29), fr.get(48)
    if not base or not both or "live.side" not in both:
        fail("two windows: the side panel did not show")
        return
    for k in range(50, 56):
        if "live.side" not in fr.get(k, {}):
            fail("two windows: frame %d: the side panel went with one tool still open" % k)
            break
        if not same(fr[k]["live.side"]["node"], both["live.side"]["node"]):
            fail("two windows: frame %d: the side panel at %s, was %s" % (k, fr[k]["live.side"]["node"], both["live.side"]["node"]))
            break
        if k >= 51 and empty_regions(fr[k]):
            fail("two windows: frame %d: empty regions %s (the other tool not shown)" % (k, empty_regions(fr[k])))
            break
    for k in range(70, 74):
        if "live.side" in fr.get(k, {}):
            fail("two windows: frame %d: the side panel stayed after its last tool closed" % k)
            break
        if not same(fr[k]["live.main"]["node"], base["live.main"]["node"]):
            fail("two windows: frame %d: the main view at %s, was %s" % (k, fr[k]["live.main"]["node"], base["live.main"]["node"]))
            break
    back = fr.get(100, {})
    if "live.side" not in back or not same(back["live.side"]["node"], both["live.side"]["node"]):
        fail("two windows: the side panel came back at %s, was %s" % (back.get("live.side", {}).get("node"), both["live.side"]["node"]))


def dragged_size():
    """The Clips side panel widened by its divider, closed with its last tool and opened again: as wide as left."""
    s = "5 layout splittrace on\n30 layout open Performance\n50 layout splittrace off\n51 quit discard\n"
    fr = parse(run("drag_probe", "live", s, 55))
    side = fr.get(48, {}).get("live.side")
    if not side:
        fail("dragged size: the side panel did not show")
        return
    x, y = side["node"][0] - 1.0, side["node"][1] + side["node"][3] * 0.5
    lines = ["5 layout splittrace on", "30 layout open Performance", "45 move %g %g" % (x, y), "48 down 0"]
    f = 49
    for _ in range(8):
        x -= 10
        lines.append("%d move %g %g" % (f, x, y))
        f += 1
    lines += ["%d up 0" % f, "%d layout close Performance" % (f + 15), "%d layout open Performance" % (f + 35),
              "%d layout splittrace off" % (f + 50), "%d quit discard" % (f + 51)]
    fr = parse(run("drag_live", "live", "\n".join(lines) + "\n", f + 55))
    left = fr.get(f + 12, {}).get("live.side")
    if not left or left["node"][2] < side["node"][2] + 40:
        fail("dragged size: the drag did not widen the side panel (%s)" % (left and left["node"]))
        return
    if "live.side" in fr.get(f + 15, {}):
        fail("dragged size: the panel stayed after its tool closed")
    back = fr.get(f + 40, {}).get("live.side")
    if not back or not same(back["node"], left["node"]):
        fail("dragged size: the panel came back at %s, was %s" % (back and back["node"], left["node"]))


def strip_close():
    """Smooth's right pane closed from its title strip's close button (drawn after the layout pass): gone in the
    frame its window is, nothing laid out empty."""
    s = "5 layout splittrace on\n30 layout open Browser\n50 layout splittrace off\n51 quit discard\n"
    fr = parse(run("strip_probe", "logic", s, 55))
    right = fr.get(48, {}).get("logic.right")
    if not right:
        fail("strip close: the right pane did not show")
        return
    base = parse(run("strip_base", "logic", "5 layout splittrace on\n30 layout splittrace off\n31 quit discard\n", 35)).get(28, {})
    x0, y0, w0, _ = right["node"]
    # The close button: the second of two 18 px buttons at the strip's right end, 4 px in (drawPaneStrips).
    bx = x0 + w0 - 4 - 18 + 9
    by = y0 + 13
    s = ("5 layout splittrace on\n30 layout open Browser\n50 move %g %g\n52 move %g %g\n55 down 0\n56 up 0\n70 layout splittrace off\n71 quit discard\n"
         % (bx - 30, by, bx, by))
    fr = parse(run("strip_close", "logic", s, 75))
    gone = None
    for k in range(55, 66):
        now = fr.get(k, {})
        if empty_regions(now):
            fail("strip close: frame %d: empty regions %s" % (k, empty_regions(now)))
            break
        if gone is None and "logic.right" not in now:
            gone = k
    if gone is None:
        fail("strip close: the right pane did not close")
    elif base and "logic.tracks" in base and not same(fr[gone]["logic.tracks"]["node"], base["logic.tracks"]["node"]):
        fail("strip close: the tracks at %s, %s without the pane" % (fr[gone]["logic.tracks"]["node"], base["logic.tracks"]["node"]))


def switch_case():
    """Clips' CPU panel closed in the frame the person switches to Smooth: back in Clips, no side panel (and
    nothing empty), the main view as wide as before the panel opened."""
    s = "5 layout splittrace on\n30 layout open Performance\n60 layout close Performance\n120 layout splittrace off\n121 quit discard\n"
    fr = parse(run("switch_live", "live", s, 125, ["--switch-layout", "logic@60", "--switch-layout", "live@90"]))
    base = fr.get(28, {})
    if "live.side" not in fr.get(50, {}):
        fail("switch: the side panel did not show")
        return
    back = [k for k in range(95, 118) if k in fr and "live.main" in fr[k]]
    if not back:
        fail("switch: no Clips frames after coming back")
        return
    for k in back:
        if "live.side" in fr[k] or empty_regions(fr[k]):
            fail("switch: frame %d back in Clips: side panel %s, empty regions %s" % (k, "live.side" in fr[k], empty_regions(fr[k])))
            break
    if base and not same(fr[back[-1]]["live.main"]["node"], base["live.main"]["node"]):
        fail("switch: the main view at %s, was %s" % (fr[back[-1]]["live.main"]["node"], base["live.main"]["node"]))


os.makedirs(OUT, exist_ok=True)
rows = homes()
for idx, layout, centre in LAYOUTS:
    if not ONLY or ONLY == layout:
        toggle_suite(idx, layout, centre, rows)
if not ONLY or ONLY == "live":
    two_windows()
    dragged_size()
    switch_case()
if not ONLY or ONLY == "logic":
    strip_close()
print("%d failures" % len(failures))
sys.exit(1 if failures else 0)
