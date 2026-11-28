#!/usr/bin/env python3
# Gridline - slide notes glide velocity (GitHub #87), in the real app (hidden, an isolated config folder and
# HOME, SDL's dummy audio driver, a fixed frame time), in every layout (Studio, Clips, Smooth, Tracker), at
# 1x and 2x:
#   - a 0% note and slide notes on it (fade in, fade out) and a softer slide on another note are written
#     through the app's own tools; the piano roll keeps the 0% note at 0;
#   - the piano roll draws the fades: its picture with "Slides glide velocity" on differs from the one with
#     it off, in the dark and the light theme, and the frames are saved for a person to look at;
#   - "Slides glide velocity" is in the piano roll's tools (Slide and portamento) in every layout: in the
#     system menu bar in Smooth and Tracker, where choosing it turns the song setting off (one undo step)
#     and Undo turns it on again; in Studio's Tools > Piano roll tools and the piano roll's own Tools menu
#     (Clips: the note editor's), read from the menu tree. Elsewhere the setting is switched as a
#     collaborator or the assistant would (patch), with the same undo.
#   python3 gh87_slide_velocity_app.py <gridline-app> <song> <out dir>
import json
import os
import re
import shutil
import subprocess
import sys

APP, SONG, OUT = sys.argv[1], sys.argv[2], sys.argv[3]
failures = []
passed = 0
ITEM = "Slides glide velocity"
OFF = "mcp call patch {'path':'song','set':{'slidesVelocity':false}}"
NOTES = ("{'pattern':1,'channel':6,'notes':["
         "{'start':0,'length':4,'pitch':48,'velocity':0},"
         "{'start':0,'length':1,'pitch':48,'velocity':127,'slide':true},"
         "{'start':2,'length':1.5,'pitch':48,'velocity':0,'slide':true},"
         "{'start':4,'length':4,'pitch':55,'velocity':110},"
         "{'start':5,'length':2,'pitch':60,'velocity':30,'slide':true}]}")


def ok(cond, msg):
    global passed
    if cond:
        passed += 1
        print("ok - " + msg)
    else:
        failures.append(msg)
        print("FAIL: " + msg)
    sys.stdout.flush()


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    parts = data.split(b"\n", 3)
    w, h = (int(x) for x in parts[1].split())
    return w, h, parts[3]


def changed(a, b, rect):
    """The pixels inside rect (x, y, w, h in frame pixels) that differ between two frames (None if either is missing)."""
    if not (os.path.exists(a) and os.path.exists(b)):
        return None
    wa, ha, pa = read_ppm(a)
    wb, hb, pb = read_ppm(b)
    if (wa, ha) != (wb, hb):
        return None
    x0, y0, w, h = rect
    out = set()
    for y in range(max(0, y0), min(ha, y0 + h)):
        for x in range(max(0, x0), min(wa, x0 + w)):
            i = (y * wa + x) * 3
            if pa[i:i + 3] != pb[i:i + 3]:
                out.add(i)
    return out


def menu_path(out):
    """The system menu bar's path to the item, from a sysmenu dump (two spaces a level)."""
    dump = out.split("SYSMENU BEGIN", 1)[-1].split("SYSMENU END", 1)[0].splitlines()[2:]
    stack = []
    for line in dump:
        depth = (len(line) - len(line.lstrip(" "))) // 2
        name = line.strip().split("|", 1)[0]
        stack = stack[:depth] + [name]
        if name == ITEM:
            return "/".join(stack)
    return None


def app_run(tag, layout, scale, lines, extra=(), theme="dark"):
    cfg = os.path.join(OUT, "cfg_" + tag)
    shutil.rmtree(cfg, ignore_errors=True)
    os.makedirs(os.path.join(cfg, "gridline"))
    os.makedirs(os.path.join(cfg, "home"))
    with open(os.path.join(cfg, "gridline", "settings.json"), "w") as f:
        json.dump({"format": "gridline-settings", "values": {"ui.layoutChosen": True, "ui.layout": layout, "theme.mode": theme,
                                                          "ui.darkTheme": theme == "dark"}}, f)
    sp = os.path.join(OUT, tag + ".txt")
    with open(sp, "w") as f:
        f.write("\n".join(lines) + "\n")
    env = dict(os.environ, SDL_AUDIODRIVER="dummy", GRIDLINE_NO_PLUGIN_SCAN="1", GRIDLINE_HIDE_PLUGINS="all", GRIDLINE_CONFIG_HOME=cfg,
               HOME=os.path.join(cfg, "home"))
    size = "1440x900" if scale == 1 else "2880x1800"
    args = [APP, SONG, "--hidden", "--no-ui-sounds", "--no-update-check", "--fixed-dt", "16", "--size", size] + list(extra)
    if scale != 1:
        args += ["--ui-scale", str(scale)]
    args += ["--dev-input", sp]
    r = subprocess.run(args, env=env, capture_output=True, text=True, timeout=600)
    with open(os.path.join(OUT, tag + ".log"), "w") as f:
        f.write(r.stdout + "\n---\n" + r.stderr)
    ok(r.returncode == 0, "%s: gridline-app ran (%d)" % (tag, r.returncode))
    shutil.rmtree(cfg, ignore_errors=True)
    return r.stdout


SETUP = ["4 typing off", "5 toasts clear", "8 mcp call set_notes " + NOTES, "9 mcp call select {'channels':[6],'pattern':1,'clips':[31]}",
         "10 layout open Piano roll", "11 layout close Channel rack", '12 focus "Piano roll"', "13 tracker page Roll", "30 toasts clear"]


def run(layout, scale):  # (shot paths quoted: the folder may hold spaces)
    tag = "%s_%dx" % (layout, scale)
    shot = lambda name: os.path.join(OUT, "%s_%s.ppm" % (tag, name))
    # Where the item is: the system menu bar (filled in every layout on macOS; Studio and Clips keep the
    # piano roll tools in the piano roll's own menu), else the piano roll's own Tools menu.
    probe = app_run(tag + "_menu", layout, scale, SETUP + ["60 sysmenu dump", "64 quit discard"], ("--system-menu", "--menu", "piano"))
    path = menu_path(probe)
    tree = probe.split("MENUTREE BEGIN piano", 1)[-1].split("MENUTREE END", 1)[0]
    ok("Tools > Slide and portamento > " + ITEM in tree, "%s: %s is in the piano roll's Tools menu, under Slide and portamento" % (tag, ITEM))
    if layout in ("logic", "tracker"):
        ok(path is not None and path.endswith("Slide and portamento/" + ITEM), "%s: %s is in the system menu bar (%s)" % (tag, ITEM, path))
    choose = ("sysmenu choose " + path) if path else OFF
    for theme in ("dark", "light"):
        t = "%s %s" % (tag, theme)
        lines = SETUP + ["40 notes", "60 toasts clear", '70 shot "%s"' % shot(theme + "_on"), "72 mcp call inspect {'path':'song'}", "73 layout where Piano roll",
                         '74 shot "%s"' % shot(theme + "_still"), "76 " + choose, "80 mcp call inspect {'path':'song'}",
                         '100 shot "%s"' % shot(theme + "_off"), "102 undo", "104 mcp call inspect {'path':'song'}", "106 notes", "110 quit discard"]
        out = app_run(tag + "_" + theme, layout, scale, lines, ("--system-menu",) if path else (), theme)
        notes = re.findall(r"notes frame \d+: (\d+) notes[^\n]*\n((?:  note [^\n]*\n)*)", out)
        ok(len(notes) == 2 and notes[0][0] == "5", "%s: the five notes are in the piano roll (%s)" % (t, [n[0] for n in notes]))
        if notes:
            vels = re.findall(r"vel (\d+)", notes[0][1])
            slides = re.findall(r"slide (\d)", notes[0][1])
            ok(vels[:1] == ["0"] and slides.count("1") == 3, "%s: the 0%% note stays at 0 and three notes are slides (%s, %s)" % (t, vels, slides))
            ok(len(notes) == 2 and notes[1][1] == notes[0][1], "%s: the notes are unchanged at the end" % t)
        flags = re.findall(r'MCP RESULT inspect [^\n]*?slidesVelocity\\?"?:\s*(true|false)', out)
        ok(flags == ["true", "false", "true"], "%s: %s turns slide velocity off, Undo on again (%s)" % (t, "the menu item" if path else "patch", flags))
        # Only the piano roll's own window counts (the undo label, meters and hints change elsewhere), and
        # what changes there on its own between two frames is left out.
        m = re.search(r"where Piano roll layout \S+ drawn 1 docked \d region \S+ rect (-?\d+) (-?\d+) (\d+) (\d+)", out)
        ok(m is not None, "%s: the piano roll is shown" % t)
        rect = tuple(int(int(v) * scale) for v in m.groups()) if m else (0, 0, 0, 0)
        still = changed(shot(theme + "_on"), shot(theme + "_still"), rect)
        off = changed(shot(theme + "_on"), shot(theme + "_off"), rect)
        d = len(off - still) if off is not None and still is not None else -1
        ok(d > 400 * scale * scale, "%s: the piano roll draws the fades (%d pixels differ with slide velocity off, %d move on their own)" %
           (t, d, len(still) if still is not None else -1))


for layout in ("studio", "live", "logic", "tracker"):
    for scale in (1, 2):
        run(layout, scale)
print("%d passed, %d failed" % (passed, len(failures)))
sys.exit(1 if failures else 0)
