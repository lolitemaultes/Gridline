#!/usr/bin/env python3
# Gridline - hunt-1119: Smooth used as a person uses it, from a fresh start and a new project, in the real app
# (hidden, isolated config folder and HOME, SDL's dummy audio driver, a fixed frame time), driven only by the
# pointer and keys over many frames (the dev input script's mouse and key path), the layout chosen through the
# real settings keys. Each check is a whole short workflow:
#   playmode   a sound double-clicked in the Library, a region pencilled at bar 5, notes drawn in its piano
#              roll, Space: the song plays from bar 1 (silent until bar 5). It played the region's pattern
#              alone, looping from the top, the moment Space was pressed (the app started in pattern mode,
#              which Smooth has no way to see or leave).
#   noregion   a new project, a sound loaded, the editor opened from the toolbar and notes drawn there: the
#              notes get a region on the track (one undo step with the first note) and are heard. They went
#              into a pattern no region played (silent, nowhere in the arrangement).
#   reopen     a project with a region and notes saved and opened again: the piano roll (open, as saved)
#              shows that region's notes. It showed the new project's empty first pattern ("Pattern 1").
#   ruler      a click on the piano roll's ruler of a region at bar 3: the song's playhead goes there in the region, and
#              Space plays the song from it. It switched to pattern mode (Space played the pattern alone).
#   misc       the track's instrument opened from the inspector, its Misc tab (Fine in cents, Bend range, the
#              echo row): nothing is cut off at the window's right edge. The Layer and Display group lists
#              ran on under it (and the echo row's long names ran into each other).
#   edge       a region dragged down the window and held past its bottom edge: the tracks scroll with it and
#              the dropped region is in view. It went onto spare rows far below the window, out of sight.
#   menukey    the app menu (macOS glyph shortcuts): Settings... shows its key as glyphs, as the other rows do.
#              It showed "Cmd+," (the comma read as two shortcuts). Elsewhere (text shortcuts): its key shows.
#   velocity   the piano roll Velocity slider, nothing selected, shows what a drawn note gets (it read 100,
#              notes came out at 80).
#   python3 hunt1119_smooth_app.py <gridline-app> <out dir> [check]
import os
import re
import shutil
import subprocess
import sys

APP, OUT = sys.argv[1], sys.argv[2]
ONLY = sys.argv[3] if len(sys.argv) > 3 else ""
failures = []

# At 1440 x 900 (logical px), Smooth's new project: the Library's rows, the tracks area's tool menu and
# grid (bar 1 at x 763, 0.17 px per tick, track 1's row from y 178, 36 px), the lower pane's piano roll.
LIB_ACID_LINE = (60, 297)
TOOL_MENU, TOOL_POINTER, TOOL_PENCIL = (818, 105), (846, 133), (846, 151)
EDITORS_BUTTON = (190, 56)
PIANO_TOOL_MENU, PIANO_DRAW = (925, 539), (958, 567)
PIANO_RULER_Y = 583
INSPECTOR_INSTRUMENT = (261, 216)
MISC_TAB_DX, MISC_TAB_DY = 189, 95
APP_MENU = (41, 12)
VEL_SLIDER = (443, 613, 812)  # the piano roll side panel Velocity slider: track ends, handle row
SETTINGS_KEY_ROW = (120, 222, 92, 106)  # the app menu's Settings... row, right of its name
X0, PPT, BAR = 763.0, 0.17, 384
TRACK1_Y = 196


def bar_x(bar):
    return X0 + (bar - 1) * BAR * PPT


def fail(msg):
    failures.append(msg)
    print("FAIL: " + msg)


class Script:
    def __init__(self):
        self.lines = ["4 typing off"]
        self.f = 20

    def at(self, cmd, n=1):
        self.lines.append("%d %s" % (self.f, cmd))
        self.f += n

    def wait(self, n):
        self.f += n

    def click(self, x, y, settle=3):
        self.at("move %.0f %.0f" % (x, y), 2)
        self.at("down 0")
        self.at("up 0", settle)

    def dclick(self, x, y, settle=4):
        self.at("move %.0f %.0f" % (x, y), 2)
        self.at("down 0")
        self.at("up 0", 2)
        self.at("down 0")
        self.at("up 0", settle)

    def cmd_key(self, k):
        self.at("mod super 1")
        self.at("key " + k)
        self.at("mod super 0", 3)

    def text(self):
        return "\n".join(self.lines + ["%d quit discard" % (self.f + 2)]) + "\n"


def run(tag, script, song=None, cfg=None):
    cfg = cfg or os.path.join(OUT, "cfg_" + tag)
    if not os.path.isdir(cfg):
        os.makedirs(os.path.join(cfg, "gridline"))
        os.makedirs(os.path.join(cfg, "home"))
        with open(os.path.join(cfg, "gridline", "settings.json"), "w") as f:
            f.write('{"format": "gridline-settings", "values": {"ui.layoutChosen": true, "ui.layout": "logic", '
                    '"keys.profile": "logic", "ui.menuStyle": "logic"}}\n')
    path = os.path.join(OUT, tag + ".txt")
    with open(path, "w") as f:
        f.write(script.text())
    env = dict(os.environ)
    env.update(SDL_AUDIODRIVER="dummy", GRIDLINE_NO_PLUGIN_SCAN="1", GRIDLINE_HIDE_PLUGINS="all", GRIDLINE_CONFIG_HOME=cfg,
               HOME=os.path.join(cfg, "home"))
    cmd = [APP] + ([song] if song else []) + ["--hidden", "--no-ui-sounds", "--no-update-check", "--fixed-dt", "16",
                                             "--size", "1440x900", "--dev-input", path]
    r = subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=300)
    with open(os.path.join(OUT, tag + ".log"), "w") as f:
        f.write(r.stdout + "\n---\n" + r.stderr)
    if r.returncode != 0:
        raise RuntimeError("%s: gridline-app failed (%d): %s" % (tag, r.returncode, r.stderr[-2000:]))
    return r.stdout, cfg


def load_sound(sc):
    sc.dclick(*LIB_ACID_LINE, settle=10)


def pick_tool(sc, row):
    sc.click(*TOOL_MENU, settle=6)
    sc.click(*row, settle=4)


def draw_notes(sc, xs, y=790):
    sc.click(*PIANO_TOOL_MENU, settle=6)
    sc.click(*PIANO_DRAW, settle=4)
    for x in xs:
        sc.click(x, y)


def clips(out, n):
    """The clips of the run's n-th clip dump (0 first)."""
    ms = list(re.finditer(r"dump frame \d+: (\d+) clips[^\n]*\n((?:  [^\n]*\n)*)", out))
    if n >= len(ms):
        return None
    m = ms[n]
    return [dict(id=int(c.group(1)), kind=int(c.group(2)), track=int(c.group(3)), start=int(c.group(4)), pattern=int(c.group(5)))
            for c in re.finditer(r"  clip (\d+) kind (\d+) track (-?\d+) start (\d+) len \d+ .*? pattern (\d+)", m.group(2))]


def check_playmode():
    sc = Script()
    load_sound(sc)
    pick_tool(sc, TOOL_PENCIL)
    sc.click(bar_x(5) + 4, TRACK1_Y, settle=8)  # a region at bar 5
    pick_tool(sc, TOOL_POINTER)
    sc.dclick(bar_x(5) + 30, TRACK1_Y, settle=20)  # its piano roll
    draw_notes(sc, (700, 840, 990, 1140))
    sc.click(1100, 300, settle=4)  # the empty tracks area
    sc.at("forget output-silent", 2)
    sc.at("waitfor output-silent 10", 2)  # the notes' previews die away (a long release tail)
    sc.at("sound", 2)
    sc.at("key space", 30)  # half a second: the song is still in bar 1
    sc.at("transport")
    sc.at("sound", 2)
    sc.at("key space", 4)
    out, _ = run("playmode", sc)
    m = re.search(r"transport frame \d+: pos (\d+) playing (\d) seek \d+ mode (\d)", out)
    if not m:
        return fail("playmode: no transport report")
    if m.group(2) != "1":
        fail("playmode: Space did not start playback")
    if m.group(3) != "1":
        fail("playmode: Space plays the song (mode 1), not the region's pattern alone: mode %s" % m.group(3))
    peaks = [float(p) for p in re.findall(r"sound frame \d+: peak (\S+)", out)]
    if len(peaks) < 2:
        return fail("playmode: no sound reports")
    if peaks[1] > 1e-4:
        fail("playmode: nothing sounds before the region at bar 5 (output peak %.4f in bar 1)" % peaks[1])
    else:
        print("ok - playmode: Space plays the song from bar 1, silent before the region at bar 5")


def check_noregion():
    sc = Script()
    load_sound(sc)
    sc.click(*EDITORS_BUTTON, settle=15)  # the editor, on the new project's empty track
    draw_notes(sc, (700, 840))
    sc.at("dump", 2)
    sc.at("notes", 2)
    sc.click(1100, 300, settle=20)
    sc.at("sound", 2)
    sc.at("key space", 20)
    sc.at("sound", 2)
    sc.at("key space", 4)
    sc.cmd_key("z")
    sc.cmd_key("z")
    sc.at("dump", 2)
    out, _ = run("noregion", sc)
    cs = clips(out, 0)
    if cs is None:
        return fail("noregion: no dump")
    if len(cs) != 1 or cs[0]["track"] != 0 or cs[0]["start"] != 0:
        fail("noregion: the drawn notes get one region on track 1 at bar 1: %s" % cs)
    nm = re.search(r"notes frame \d+: (\d+) notes", out)
    if not nm or nm.group(1) != "2":
        fail("noregion: two notes drawn: %s" % (nm.group(0) if nm else "no notes report"))
    peaks = [float(p) for p in re.findall(r"sound frame \d+: peak (\S+)", out)]
    if len(peaks) < 2 or peaks[1] < 1e-3:
        fail("noregion: the notes are heard when the song plays (output peak %s)" % (peaks[1:] or "none"))
    cu = clips(out, 1)
    if cu is None or len(cu) != 0:
        fail("noregion: undoing both notes takes the region with the first (one undo step): %s" % cu)
    if not [f for f in failures if f.startswith("noregion")]:
        print("ok - noregion: notes drawn with no region make one, heard, undone with the first note")


def check_reopen():
    song = os.path.join(OUT, "reopen.gridline.json")
    if os.path.exists(song):
        os.remove(song)
    sc = Script()
    load_sound(sc)
    pick_tool(sc, TOOL_PENCIL)
    sc.click(bar_x(1) + 4, TRACK1_Y, settle=8)
    pick_tool(sc, TOOL_POINTER)
    sc.dclick(bar_x(1) + 30, TRACK1_Y, settle=20)
    draw_notes(sc, (700, 840, 990))
    sc.at("dump", 2)
    sc.at('save "%s"' % song, 10)
    out, cfg = run("reopen_save", sc)
    cs = clips(out, 0) or []
    m = re.search(r"dump frame \d+: \d+ clips[^\n]*\n  clip \d+ kind 0 track 0 start 0 len \d+ .*? pattern (\d+)", out)
    if not m or not os.path.exists(song):
        return fail("reopen: the project was not built and saved: %s" % cs)
    pat = int(m.group(1))
    sc2 = Script()
    sc2.wait(10)
    sc2.at("notes", 2)
    sc2.at("plgeom", 2)
    sc2.at("layout dump", 2)
    out2, _ = run("reopen_open", sc2, song=song, cfg=cfg)
    lower = re.search(r"region logic.lower visible (\d)", out2)
    if not lower or lower.group(1) != "1":
        fail("reopen: the piano roll comes back open, as saved")
    cur = re.search(r" pattern (\d+) channel \d+ start", out2)
    nm = re.search(r"notes frame \d+: (\d+) notes", out2)
    if not cur or int(cur.group(1)) != pat:
        fail("reopen: the editor shows the region's pattern %d, not %s" % (pat, cur.group(1) if cur else "?"))
    if not nm or nm.group(1) != "3":
        fail("reopen: the piano roll shows the region's three notes: %s" % (nm.group(0) if nm else "no notes report"))
    if not [f for f in failures if f.startswith("reopen")]:
        print("ok - reopen: the reopened project's piano roll shows the region's notes")


def check_ruler():
    sc = Script()
    load_sound(sc)
    pick_tool(sc, TOOL_PENCIL)
    sc.click(bar_x(3) + 4, TRACK1_Y, settle=8)  # a region at bar 3
    pick_tool(sc, TOOL_POINTER)
    sc.dclick(bar_x(3) + 30, TRACK1_Y, settle=20)  # its piano roll: the region's bar from x 689 to 1289
    sc.click(689 + 600 * 0.5, PIANO_RULER_Y, settle=6)  # the ruler, half way through the region
    sc.at("transport", 2)
    sc.at("key space", 10)
    sc.at("transport", 2)
    sc.at("key space", 4)
    out, _ = run("ruler", sc)
    ts = re.findall(r"transport frame \d+: pos (\d+) playing (\d) seek \d+ mode (\d)", out)
    if len(ts) < 2:
        return fail("ruler: no transport reports")
    pos, _, mode = (int(v) for v in ts[0])
    if mode != 1 or not (2 * BAR + BAR // 2 - 24 <= pos <= 2 * BAR + BAR // 2 + 24):
        fail("ruler: the click puts the song's playhead half way through the region at bar 3 (tick %d, song mode): "
             "tick %d mode %d" % (2 * BAR + BAR // 2, pos, mode))
    if int(ts[1][2]) != 1 or int(ts[1][1]) != 1:
        fail("ruler: Space then plays the song from there: %s" % (ts[1],))
    if not [f for f in failures if f.startswith("ruler")]:
        print("ok - ruler: the piano roll's ruler moves the song's playhead into the region")


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    parts = data.split(b"\n", 3)  # P6\n<w> <h>\n255\n<rgb>
    w, h = (int(x) for x in parts[1].split())
    return w, h, parts[3]


def check_misc():
    shot = os.path.join(OUT, "misc.ppm")
    sc = Script()
    load_sound(sc)
    pick_tool(sc, TOOL_PENCIL)
    sc.click(bar_x(1) + 4, TRACK1_Y, settle=8)
    pick_tool(sc, TOOL_POINTER)
    sc.click(*INSPECTOR_INSTRUMENT, settle=20)  # the track's instrument, from the inspector's channel strip
    sc.at("windows", 3)
    out, _ = run("misc_find", sc)
    m = re.search(r'window "channelsettings" pos ([-0-9.]+),([-0-9.]+) size ([0-9.]+)x([0-9.]+)', out)
    if not m:
        return fail("misc: the instrument window did not open from the inspector")
    wx, wy, ww, wh = (float(v) for v in m.groups())
    sc.click(wx + MISC_TAB_DX, wy + MISC_TAB_DY, settle=12)  # its Misc tab (the extra settings)
    sc.at('shot "%s"' % shot, 3)
    run("misc", sc)
    w, h, px = read_ppm(shot)
    # Nothing is cut off at the window's right edge: no text in its last few columns (the Layer list's
    # "No children" ran on under the edge).
    x1 = int(wx + ww) - 3
    cut = 0
    for y in range(int(wy) + 60, min(h, int(wy + wh) - 6)):
        for x in range(x1 - 8, x1):
            i = (y * w + x) * 3
            if px[i] + px[i + 1] + px[i + 2] > 3 * 165:
                cut += 1
    if cut > 4:
        fail("misc: the extra settings fit the instrument window: %d text pixels at its right edge" % cut)
    else:
        print("ok - misc: the instrument's extra settings fit the window (nothing cut at its right edge)")


def check_edge():
    sc = Script()
    load_sound(sc)
    pick_tool(sc, TOOL_PENCIL)
    sc.click(bar_x(1) + 4, TRACK1_Y, settle=8)
    pick_tool(sc, TOOL_POINTER)
    sc.click(1100, 600, settle=4)
    x, y = bar_x(1) + 30, TRACK1_Y
    sc.at("move %.0f %.0f" % (x, y), 2)
    sc.at("down 0")
    for i in range(1, 41):  # down the window to its bottom edge, then a little past it, and held there
        sc.at("move %.0f %.0f" % (x + i, y + (890 - y) * i / 40))
    for i in range(1, 11):
        sc.at("move %.0f %.0f" % (x + 40, 890 + i * 2))
    sc.wait(40)
    sc.at("plgeom", 2)
    sc.at("up 0", 4)
    sc.at("dump", 2)
    sc.at("plgeom", 2)
    out, _ = run("edge", sc)
    geoms = out.split("plgeom frame")[1:]
    cs = clips(out, 0)
    if len(geoms) < 2 or not cs:
        return fail("edge: no geometry or dump")
    cid = cs[0]["id"]
    g = geoms[1]
    sy = re.search(r"view scrollY ([0-9.]+) trackHeight [0-9.]+ grid [0-9.]+ ([0-9.]+) ([0-9.]+)", g)
    cm = re.search(r"\n  clip %d x [-0-9.]+ [-0-9.]+ y ([-0-9.]+) ([-0-9.]+)" % cid, g)
    if not sy or float(sy.group(1)) <= 0:
        fail("edge: a region held past the bottom edge scrolls the tracks down (scrollY %s)" % (sy.group(1) if sy else "?"))
    if cs[0]["track"] < 20:
        fail("edge: the region went on down the spare rows as the view followed: track %d" % cs[0]["track"])
    if not cm or not sy or not (float(sy.group(2)) <= float(cm.group(1)) and float(cm.group(2)) <= float(sy.group(3)) + 1):
        fail("edge: the dropped region is in view (its rows %s, the grid's %s)" % (cm.groups() if cm else "not drawn", sy.groups()[1:] if sy else "?"))
    if not [f for f in failures if f.startswith("edge")]:
        print("ok - edge: a region dragged past the bottom edge scrolls the tracks and stays in view")


def check_menukey():
    shot = os.path.join(OUT, "menukey.ppm")
    sc = Script()
    sc.click(*APP_MENU, settle=8)
    sc.at('shot "%s"' % shot, 3)
    run("menukey", sc)
    w, h, px = read_ppm(shot)
    # The app menu's Settings... row: its shortcut is written as the other rows' are, glyphs ("⌘,", a dozen px
    # wide at the right), never "Cmd+," (text over 30 px wide).
    x0, x1, y0, y1 = SETTINGS_KEY_ROW
    xs = [x for y in range(y0, y1) for x in range(x0, x1) if sum(px[(y * w + x) * 3:(y * w + x) * 3 + 3]) > 3 * 75]
    span = (max(xs) - min(xs) + 1) if xs else 0
    if sys.platform != "darwin":
        # Glyph shortcuts are macOS only: elsewhere every row writes its key as text ("Ctrl+,").
        if not xs:
            fail("menukey: Settings... shows its shortcut")
        else:
            print("ok - menukey: Settings... shows its shortcut as text, as the other rows do (%d px wide)" % span)
    elif not xs or span > 20:
        fail("menukey: Settings... shows its shortcut as glyphs, as the other rows do (%d px wide)" % span)
    else:
        print("ok - menukey: Settings... shows its shortcut as glyphs")


def check_velocity():
    shot = os.path.join(OUT, "velocity.ppm")
    sc = Script()
    load_sound(sc)
    sc.click(*EDITORS_BUTTON, settle=15)
    sc.at('shot "%s"' % shot, 3)
    draw_notes(sc, (700,))
    sc.at("notes", 2)
    out, _ = run("velocity", sc)
    m = re.search(r"\n  note 0 .* vel (\d+) ", out)
    if not m:
        return fail("velocity: no note drawn")
    vel = int(m.group(1))
    w, h, px = read_ppm(shot)
    x0, x1, y = VEL_SLIDER
    xs = [x for x in range(x0 - 8, x1 + 8) if min(px[(y * w + x) * 3:(y * w + x) * 3 + 3]) > 170]
    if not xs:
        return fail("velocity: no slider handle found")
    hx = (min(xs) + max(xs)) / 2.0
    want = x0 + (vel - 1) / 126.0 * (x1 - x0)
    # With nothing selected the editor's Velocity slider is the velocity new notes get: a drawn note
    # came out at 80 while it read 100.
    if abs(hx - want) > 6:
        fail("velocity: the slider (handle at x %.0f) shows the drawn note's velocity %d (x %.0f)" % (hx, vel, want))
    else:
        print("ok - velocity: the Velocity slider shows the velocity a drawn note gets (%d)" % vel)


if os.path.isdir(OUT):
    shutil.rmtree(OUT)
os.makedirs(OUT)
for name, fn in [("playmode", check_playmode), ("noregion", check_noregion), ("reopen", check_reopen), ("ruler", check_ruler), ("misc", check_misc), ("edge", check_edge), ("menukey", check_menukey), ("velocity", check_velocity)]:
    if not ONLY or ONLY == name:
        fn()
if failures:
    print("%d failure(s)" % len(failures))
    sys.exit(1)
print("all passed")
