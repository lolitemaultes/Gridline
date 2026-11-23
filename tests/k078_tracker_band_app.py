#!/usr/bin/env python3
# Gridline - trackerband (0.7.8): the Tracker's header band (the order list, tempo and rows, scopes, instruments,
# the buttons and the page tabs) is never covered or lost, in the real app (hidden, isolated config folder and
# HOME, SDL's dummy audio driver). 0.7.7 docked the Browser in the band as a tab in front of it, and a layout
# saved with the Browser open but the band's own window closed came back with the Browser alone there: no band,
# no page tabs, nothing to switch back with.
#   - every way the Browser opens in the Tracker (its window flag, the Tracker and Studio keys, the system menu
#     in both menu styles where there is one (macOS, or Linux with a global menu host), the command palette, a
#     folder dropped on the window, a switch from Studio with the Browser shown) opens it in the side panel,
#     with the band drawn every frame and holding its window alone;
#   - the band's Files page shows the files in the band (no Browser);
#   - Performance (also homed in the band before) opens in the side panel too;
#   - a window docked in the band anyway (a drag, a saved dock file) goes back to its home;
#   - a layout saved by 0.7.7 with the Browser alone in the band (its panes without the band's window, the
#     Browser in front), a band hidden or a tab moved into it in the region state, and a broken dock file, all
#     load with the band whole;
#   - the Browser's key closes it again (the way back), the band staying;
#   - Studio, Clips and Smooth still open the Browser where they did.
#   python3 k078_tracker_band_app.py <gridline-app> <song.gridline.json> <out dir>
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


def run(tag, layout, script, frames, values=None, ini=None, extra=()):
    cfg = os.path.join(OUT, "cfg_" + tag)
    shutil.rmtree(cfg, ignore_errors=True)
    os.makedirs(os.path.join(cfg, "gridline"))
    os.makedirs(os.path.join(cfg, "home"))
    v = {"ui.layoutChosen": True, "ui.layout": layout}
    v.update(values or {})
    with open(os.path.join(cfg, "gridline", "settings.json"), "w") as f:
        json.dump({"format": "gridline-settings", "values": v}, f)
    if ini is not None:
        with open(os.path.join(cfg, "gridline", "layout-%s.ini" % layout), "w") as f:
            f.write(ini)
    song = os.path.join(cfg, "song.gridline.json")
    shutil.copyfile(DEMO, song)
    sp = os.path.join(OUT, tag + ".txt")
    with open(sp, "w") as f:
        f.write(script.replace("@SHOT@", os.path.join(OUT, tag + ".ppm")))
    env = dict(os.environ, SDL_AUDIODRIVER="dummy", GRIDLINE_NO_PLUGIN_SCAN="1", GRIDLINE_CONFIG_HOME=cfg, HOME=os.path.join(cfg, "home"))
    r = subprocess.run([APP, song, "--hidden", "--no-ui-sounds", "--no-update-check", "--fixed-dt", "16", "--size", "1440x900", "--dev-input", sp,
                        "--frames", str(frames)] + list(extra), env=env, capture_output=True, text=True, timeout=600)
    with open(os.path.join(OUT, tag + ".log"), "w") as f:
        f.write(r.stdout + "\n---- stderr ----\n" + r.stderr)
    if r.returncode != 0:
        fail("%s: gridline-app failed (%d)" % (tag, r.returncode))
    return r.stdout


def regions(out, frame):
    m = re.search(r"LAYOUT BEGIN frame %d\n(.*?)LAYOUT END" % frame, out, re.S)
    if not m:
        return None
    res = {}
    for l in m.group(1).splitlines():
        r = re.match(r"region (\S+) visible ([01]) .* windows \[([^\]]*)\] front (.*)$", l)
        if r:
            res[r.group(1)] = (r.group(2) == "1", [w for w in r.group(3).split(",") if w], r.group(4))
    return res


def band_frame(out, frame):
    m = re.search(r"tracker band frame %d: drawn (\w+) height (\d+) strip \d+ frame (-?\d+)" % frame, out)
    return (m.group(1), int(m.group(2)), int(m.group(3))) if m else None


def where(out, window, frame):
    m = re.search(r"frame %d where %s layout \S+ drawn (\d) docked (\d) region (\S+)" % (frame, re.escape(window)), out)
    return (m.group(1) == "1", m.group(3)) if m else None


# The checks at <frame> (a script line "<frame> layout dump", "<frame> tracker band" and "<frame + 10> tracker band"
# from check_lines): the band shown, its window alone in it and in front, drawn every frame at its height; the
# pattern pages shown; `browser` the region the Browser is drawn in ("" for not drawn).
def check_lines(frame, browser=True):
    s = "%d layout dump\n%d tracker band\n%d tracker band\n" % (frame, frame, frame + 10)
    if browser:
        s += "%d layout where Browser\n" % frame
    return s


def check_band(tag, out, frame, browser="tracker.side"):
    global passed
    regs = regions(out, frame)
    if regs is None:
        return fail("%s: no layout dump at frame %d" % (tag, frame))
    head = regs.get("tracker.header")
    if not head or not head[0] or head[1] != ["Order list"] or head[2] != "Order list":
        fail("%s: the header band does not hold its own window alone, in front: %s" % (tag, head))
    pages = regs.get("tracker.pages")
    if not pages or not pages[0]:
        fail("%s: the pattern pages are not shown: %s" % (tag, pages))
    a, b = band_frame(out, frame), band_frame(out, frame + 10)
    if not a or not b:
        return fail("%s: no band report at frames %d and %d" % (tag, frame, frame + 10))
    if a[0] != "header" or a[1] < 150:
        fail("%s: the band is not drawn whole (with its page tabs): %s" % (tag, a))
    if b[2] - a[2] < 8:
        fail("%s: the band was not drawn every frame (%d to %d over 10 frames)" % (tag, a[2], b[2]))
    if browser is not None:
        w = where(out, "Browser", frame)
        got = w[1] if w and w[0] else ""
        if got != browser:
            fail("%s: the Browser is drawn in '%s', expected '%s'" % (tag, got, browser))
    if not any(f.startswith(tag + ":") for f in failures):
        passed += 1
        print("ok %s" % tag)


KEYUP = "%d sdlkey #224 up\n%d sdlkey #226 up\n"
TRACKER_KEYS = {"keys.profile": "tracker", "ui.menuStyle": "tracker"}


def key_b(f):
    return "%d sdlkey b down ctrl+alt\n%d sdlkey b up ctrl+alt\n" % (f, f + 1) + KEYUP % (f + 2, f + 2)


def key_f8(f):
    return "%d sdlkey f8 down alt\n%d sdlkey f8 up alt\n%d sdlkey #226 up\n" % (f, f + 1, f + 2)


START = "5 toasts clear\n"
END = "90 quit discard\n"

# ---- 1. each way in ----
ways = [
    ("flag", {}, "20 layout open Browser\n", ()),
    ("trackerkey", TRACKER_KEYS, key_b(20), ()),
    ("studiokey", {}, key_f8(20), ()),
    ("sysmenu_tracker", TRACKER_KEYS, "19 sysmenu status\n20 sysmenu choose View/Windows and panels/Browser\n", ("--system-menu",)),
    ("sysmenu_studio", {}, "19 sysmenu status\n20 sysmenu choose View/Browser\n", ("--system-menu",)),
    ("palette", {}, "18 palette open\n20 palette run win:Browser\n", ()),
]
folder = os.path.join(OUT, "dropped folder")
os.makedirs(folder, exist_ok=True)
ways.append(("drop", {}, '20 sdlwin drop "%s"\n' % folder, ()))
for tag, values, lines, extra in ways:
    out = run(tag, "tracker", START + lines + check_lines(50) + "60 shot \"@SHOT@\"\n" + END, 100, values, extra=extra)
    if tag.startswith("sysmenu"):
        # The system menu bar is there on macOS always, on Linux only while a global menu host runs on the
        # session bus (none under the CI's xvfb or Weston): with no host, Gridline keeps its in-window menu
        # row, nothing is installed and there is no system menu to choose from. Then the run still checks
        # the band stays whole with --system-menu asked for, and the Browser stays closed.
        st = re.search(r"frame 19 sysmenu status installed (\d)", out)
        if st and st.group(1) == "0" and sys.platform != "darwin":
            print("note %s: no system menu bar here (no global menu host on the session bus); the in-window menu row is used" % tag)
            check_band(tag, out, 50, browser="")
            continue
        if "sysmenu choose" in out and " ok" not in out.split("sysmenu choose", 1)[1].splitlines()[0]:
            fail("%s: the menu item was not found" % tag)
    check_band(tag, out, 50)

# A switch from Studio with the Browser shown (Studio opens it by default).
out = run("fromstudio", "studio", START + "15 layout open Browser\n" + check_lines(60) + "70 shot \"@SHOT@\"\n" + END, 100, extra=("--switch-layout", "tracker@30"))
check_band("fromstudio", out, 60, browser=None)

# The band's own Files page: the files show in the band, the Browser stays closed.
out = run("filespage", "tracker", START + "20 tracker swap files\n" + check_lines(50) + END, 100)
check_band("filespage", out, 50, browser="")

# Performance, the other window homed in the band before.
out = run("performance", "tracker", START + "20 layout open Performance\n" + check_lines(50) + "50 layout where Performance\n" + END, 100)
check_band("performance", out, 50, browser=None)
w = where(out, "Performance", 50)
if not w or not w[0] or w[1] != "tracker.side":
    fail("performance: Performance is not drawn in the side panel: %s" % (w,))

# A window docked in the band anyway (a drag, a saved dock file): it goes back to its home.
out = run("dockinto", "tracker", START + "20 layout open Browser\n30 layout dockinto Browser|tracker.header\n" + check_lines(50) + END, 100)
if not re.search(r"frame 30 dockinto Browser\|tracker.header ok", out):
    fail("dockinto: the scripted dock did not run")
check_band("dockinto", out, 50)

# ---- 2. the way back: the Browser's key closes it, the band stays ----
out = run("back", "tracker", START + key_b(20) + key_b(40) + check_lines(60, browser=True) + END, 100, TRACKER_KEYS)
check_band("back", out, 60, browser="")
regs = regions(out, 60) or {}
if regs.get("tracker.side", (False,))[0]:
    fail("back: the side panel stays after the Browser closed: %s" % (regs.get("tracker.side"),))

# ---- 3. layouts saved broken (0.7.7's state of the user's report), repaired on load with no action ----
saved = {"ui.layoutVersion.tracker": 1, "layout.tracker.panes": "1\nBrowser\nPattern editor\nPlaylist\nfront:Browser"}
INI_OK = "[Docking][Data]\n"
# A dock file that put the Browser in a node of its own over the band, and lines that are not a dock file at all.
INI_BROKEN = ("[Window][Browser]\nPos=0,24\nSize=1440,248\nCollapsed=0\nDockId=0x00000BAD,1\n\n[Window][Order list]\nPos=0,24\nSize=1440,248\n"
              "Collapsed=0\nDockId=0x00000BAD,0\n\n[Docking][Data]\nDockSpace ID=0x12345678 Window=0x0 Pos=0,24 Size=1440,876 Split=Y\n"
              "  DockNode ID=0x00000BAD Parent=0x12345678 SizeRef=1440,248 Selected=0xDEADBEEF\n  garbage line\n\x00\xff")
loads = [
    ("saved077", dict(saved), INI_OK),
    ("savedtracker077", dict(saved, **TRACKER_KEYS), INI_OK),
    ("brokenini", dict(saved), INI_BROKEN),
    ("hiddenband", dict(saved, **{"layout.tracker.state": json.dumps({"hidden": ["tracker.header"], "moved": {"Browser": "tracker.header"}})}), INI_OK),
]
for tag, values, ini in loads:
    out = run(tag, "tracker", START + check_lines(40) + "50 shot \"@SHOT@\"\n" + END, 100, values, ini)
    check_band(tag, out, 40)

# ---- 4. the other layouts open the Browser where they did ----
for layout, region in (("live", "live.browser"), ("logic", "logic.right")):
    out = run("other_" + layout, layout, START + "20 layout open Browser\n40 layout where Browser\n" + END, 100)
    w = where(out, "Browser", 40)
    if not w or not w[0] or w[1] != region:
        fail("%s: the Browser is not in %s: %s" % (layout, region, w))
    else:
        passed += 1
        print("ok other_" + layout)
out = run("other_studio", "studio", START + "40 layout where Browser\n" + END, 100)
w = where(out, "Browser", 40)
if not w or not w[0]:
    fail("studio: the Browser is not drawn: %s" % (w,))
else:
    passed += 1
    print("ok other_studio")

print("%d checks passed, %d failures" % (passed, len(failures)))
sys.exit(1 if failures else 0)
