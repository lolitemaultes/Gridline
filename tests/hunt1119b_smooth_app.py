#!/usr/bin/env python3
# Gridline - hunt-1119 (second round): Smooth used as a person uses it, from a fresh start, in the real app (hidden,
# isolated config folder and HOME, SDL's dummy audio driver, a fixed frame time), driven only by the pointer and
# keys over many frames (the dev input script's mouse and key path), the layout chosen through the real settings
# keys. Each check is a whole short workflow:
#   samestart  two regions pencilled on a track (bars 1 and 3), the second dragged onto the first's start with
#              Drag: No overlap: the dropped one wins (the one it covers goes), one undo step brings both back.
#              Both were left on top of each other.
#   retarget   a second instrument track added, a region with notes on it dragged onto the first track: it plays
#              that track's instrument (the notes are its channel's), one undo step puts them back. It kept
#              playing the instrument of the track it came from. The open editor shows its notes all along.
#   emptytrack smoothinst-1119: a region with notes (and an Alt-copy of it) on the first track, the region dragged
#              onto the spare row below (an empty track with no instrument): that track becomes an instrument
#              track of its own (a copy of the sound on a channel strip of its own: the first track keeps its
#              instrument), its header and the Library name the instrument and the region's notes are its
#              channel's (what plays them); the copy dragged there next plays that instrument too (R2-3); one
#              undo step each, the last one leaving the track empty and the sound gone. The track gained the
#              sound only through its regions: header and Library said "No instrument".
#   emptymulti smoothinst-1119: two regions of a free track, one playing Synth and one Snare, dragged together
#              onto the empty track below: that track takes the earlier one's sound (Synth, which no other track
#              plays) and the later region plays it too; one undo step puts both back. The same drag with Alt
#              (copies): the first track keeps both its regions as they were, the empty track gets a copy of
#              Synth on a channel strip of its own and both copies play it.
#   fold       the demo song, a region dragged to another track: the rows shown stay as they were (the Music
#              Filter automation row folded away, as the bus it automates was heard on one more track).
#   cards      Settings > Interface: every layout card's description shows whole (the Tracker card's last line
#              was cut through by a two-line clip).
#   emptyhelp  the pointer over the empty space below the last track: the hint names no track (it named the
#              last one); over the track it names it.
#   pencil     the Pencil clicked a little past a beat's middle: the region starts at the beat under the pointer
#              (the snap value's cell), as other edits do. It started a beat later.
#   python3 hunt1119b_smooth_app.py <gridline-app> <demo song> <out dir> [check]
import json
import os
import re
import shutil
import subprocess
import sys

APP, DEMO, OUT = sys.argv[1], sys.argv[2], sys.argv[3]
ONLY = sys.argv[4] if len(sys.argv) > 4 else ""
failures = []

# At 1440 x 900 (logical px), Smooth's new project: the Library's rows, the tracks area's tool menu and
# grid (bar 1 at x 763, 0.17 px per tick, track 1's row from y 178, 36 px), the lower pane's piano roll.
LIB_ACID_LINE = (60, 297)
TOOL_MENU, TOOL_POINTER, TOOL_PENCIL = (818, 105), (846, 133), (846, 151)
PIANO_TOOL_MENU, PIANO_DRAW = (925, 539), (958, 567)
NEW_TRACKS, NEW_TRACKS_EDITOR, NEW_TRACKS_COUNT, NEW_TRACKS_OK = (449, 141), (426, 603), (770, 664), (1081, 664)
APP_MENU, APP_SETTINGS, SETTINGS_INTERFACE = (41, 12), (80, 98), (575, 270)
X0, PPT, BAR = 763.0, 0.17, 384
TRACK1_Y, TRACK2_Y = 196, 232


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

    def drag(self, x0, y0, x1, y1, frames=30, settle=4):
        self.at("move %.0f %.0f" % (x0, y0), 2)
        self.at("down 0")
        for i in range(1, frames + 1):
            self.at("move %.0f %.0f" % (x0 + (x1 - x0) * i / frames, y0 + (y1 - y0) * i / frames))
        self.at("up 0", settle)

    def cmd_key(self, k):
        self.at("mod super 1")
        self.at("key " + k)
        self.at("mod super 0", 3)

    def text(self):
        return "\n".join(self.lines + ["%d quit discard" % (self.f + 2)]) + "\n"


def run(tag, script, song=None):
    cfg = os.path.join(OUT, "cfg_" + tag)
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
    return r.stdout


def load_sound(sc):
    sc.dclick(*LIB_ACID_LINE, settle=10)


def pick_tool(sc, row):
    sc.click(*TOOL_MENU, settle=6)
    sc.click(*row, settle=4)


def clips(out, n):
    """The clips of the run's n-th clip dump (0 first)."""
    ms = list(re.finditer(r"dump frame \d+: (\d+) clips[^\n]*\n((?:  [^\n]*\n)*)", out))
    if n >= len(ms):
        return None
    return [dict(id=int(c.group(1)), track=int(c.group(2)), start=int(c.group(3)))
            for c in re.finditer(r"  clip (\d+) kind \d+ track (-?\d+) start (\d+) ", ms[n].group(2))]


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    parts = data.split(b"\n", 3)
    w, h = (int(x) for x in parts[1].split())
    return w, h, parts[3]


def check_samestart():
    sc = Script()
    load_sound(sc)
    pick_tool(sc, TOOL_PENCIL)
    sc.click(bar_x(1) + 4, TRACK1_Y, settle=8)
    sc.click(bar_x(3) + 4, TRACK1_Y, settle=8)
    pick_tool(sc, TOOL_POINTER)
    sc.click(1100, 500, settle=4)
    sc.at("dump", 2)
    sc.drag(bar_x(3) + 30, TRACK1_Y, bar_x(1) + 30, TRACK1_Y)
    sc.at("dump", 2)
    sc.cmd_key("z")
    sc.at("dump", 2)
    out = run("samestart", sc)
    before, after, undone = clips(out, 0), clips(out, 1), clips(out, 2)
    if not before or len(before) != 2 or after is None or undone is None:
        return fail("samestart: two regions pencilled and dumped: %s" % before)
    moved = [c for c in before if c["start"] == 768]
    if not moved:
        return fail("samestart: a region at bar 3: %s" % before)
    if len(after) != 1 or after[0]["id"] != moved[0]["id"] or after[0]["start"] != 0:
        fail("samestart: the dropped region wins its start, the one it covers goes (no overlap left): %s" % after)
    if sorted((c["id"], c["start"]) for c in undone) != sorted((c["id"], c["start"]) for c in before):
        fail("samestart: one undo step brings both back as they were: %s" % undone)
    if not [f for f in failures if f.startswith("samestart")]:
        print("ok - samestart: a region dropped on another's start wins it, one undo step")


def check_retarget():
    sc = Script()
    load_sound(sc)
    sc.click(*NEW_TRACKS, settle=10)  # New tracks: one more instrument track, its editor not opened
    sc.click(*NEW_TRACKS_EDITOR, settle=3)
    sc.click(*NEW_TRACKS_COUNT, settle=2)
    sc.cmd_key("a")
    sc.at("text 1", 2)
    sc.at("key tab", 2)
    sc.click(*NEW_TRACKS_OK, settle=12)
    pick_tool(sc, TOOL_PENCIL)
    sc.click(bar_x(1) + 4, TRACK2_Y, settle=8)
    pick_tool(sc, TOOL_POINTER)
    sc.dclick(bar_x(1) + 30, TRACK2_Y, settle=20)
    sc.click(*PIANO_TOOL_MENU, settle=6)
    sc.click(*PIANO_DRAW, settle=4)
    sc.click(700, 790)
    sc.click(840, 790)
    sc.click(1300, 450, settle=4)
    moved = os.path.join(OUT, "retarget_moved.gridline.json")
    undone = os.path.join(OUT, "retarget_undone.gridline.json")
    sc.drag(bar_x(1) + 30, TRACK2_Y, bar_x(3) + 30, TRACK1_Y)
    sc.at('save "%s"' % moved, 4)
    sc.at("notes", 2)
    sc.cmd_key("z")
    sc.at('save "%s"' % undone, 4)
    sc.at("notes", 2)
    out = run("retarget", sc)
    shown = re.findall(r"notes frame \d+: (\d+) notes", out)
    if shown != ["2", "2"]:
        fail("retarget: the open editor keeps showing the region's two notes, moved and undone: %s" % shown)

    def region(path, track, start):
        d = json.load(open(path))
        ch = [t.get("channel") for t in d.get("tracks", [])]
        for c in d.get("playlist", []):
            if c.get("track") == track and c.get("start") == start and "pattern" in c:
                for p in d.get("patterns", []):
                    if p.get("id") == c["pattern"]:
                        return ch, sorted(k for k, v in p.get("notes", {}).items() if v)
        return ch, None

    try:
        ch, keys = region(moved, 0, 768)
        ch2, keys2 = region(undone, 1, 0)
    except (OSError, ValueError) as e:
        return fail("retarget: the saved projects: %s" % e)
    if len(ch) < 2 or ch[0] is None or ch[1] is None or ch[0] == ch[1]:
        return fail("retarget: two instrument tracks with their own channels: %s" % ch)
    if keys != [str(ch[0])]:
        fail("retarget: the region moved onto track 1 plays track 1's instrument (channel %s): its notes are on %s" % (ch[0], keys))
    if keys2 != [str(ch[1])]:
        fail("retarget: one undo step puts the region back on track 2 with its own instrument (channel %s): %s" % (ch[1], keys2))
    if not [f for f in failures if f.startswith("retarget")]:
        print("ok - retarget: a region moved onto another instrument's track plays that instrument, one undo step")


def song_region(path, track, start):
    """(each track's linked channel, the channels with notes in the region at track / start, channel count)."""
    d = json.load(open(path))
    ch = [t.get("channel") for t in d.get("tracks", [])]
    for c in d.get("playlist", []):
        if c.get("track") == track and c.get("start") == start and "pattern" in c:
            for p in d.get("patterns", []):
                if p.get("id") == c["pattern"]:
                    return ch, sorted(k for k, v in p.get("notes", {}).items() if v), len(d.get("channels", []))
    return ch, None, len(d.get("channels", []))


def check_emptytrack():
    sc = Script()
    load_sound(sc)
    pick_tool(sc, TOOL_PENCIL)
    sc.click(bar_x(1) + 4, TRACK1_Y, settle=8)
    pick_tool(sc, TOOL_POINTER)
    sc.dclick(bar_x(1) + 30, TRACK1_Y, settle=20)
    sc.click(*PIANO_TOOL_MENU, settle=6)
    sc.click(*PIANO_DRAW, settle=4)
    sc.click(700, 790)
    sc.click(840, 790)
    sc.click(1300, 450, settle=4)
    sc.at("mod alt 1")  # an Alt-copy of the region at bar 5, on the same track
    sc.drag(bar_x(1) + 30, TRACK1_Y, bar_x(5) + 30, TRACK1_Y)
    sc.at("mod alt 0", 3)
    paths = {k: os.path.join(OUT, "emptytrack_%s.gridline.json" % k) for k in ("before", "moved", "second", "undo1", "undo2")}
    sc.at('save "%s"' % paths["before"], 4)
    sc.drag(bar_x(1) + 30, TRACK1_Y, bar_x(1) + 30, TRACK2_Y)  # onto the spare row below: track 2
    sc.at('save "%s"' % paths["moved"], 4)
    sc.at("parts lib tracks", 2)
    sc.click(bar_x(1) + 30, TRACK2_Y, settle=6)  # the moved region selected: its track is the Library's
    sc.at("parts lib state", 2)
    sc.drag(bar_x(5) + 30, TRACK1_Y, bar_x(5) + 30, TRACK2_Y)  # the copy onto that track next
    sc.at('save "%s"' % paths["second"], 4)
    sc.cmd_key("z")
    sc.at('save "%s"' % paths["undo1"], 4)
    sc.cmd_key("z")
    sc.at('save "%s"' % paths["undo2"], 4)
    sc.at("parts lib tracks", 2)
    out = run("emptytrack", sc)
    try:
        ch0, a0, n0 = song_region(paths["before"], 0, 0)
        _, b0, _ = song_region(paths["before"], 0, 1536)
        ch1, a1, n1 = song_region(paths["moved"], 1, 0)
        _, b1, _ = song_region(paths["moved"], 0, 1536)
        ch2, b2, _ = song_region(paths["second"], 1, 1536)
        ch3, b3, _ = song_region(paths["undo1"], 0, 1536)
        _, a3, _ = song_region(paths["undo1"], 1, 0)
        ch4, a4, n4 = song_region(paths["undo2"], 0, 0)
    except (OSError, ValueError) as e:
        return fail("emptytrack: the saved projects: %s" % e)
    c1 = ch0[0] if ch0 else None
    if c1 is None or a0 != [str(c1)] or b0 != [str(c1)] or (len(ch0) > 1 and ch0[1] is not None):
        return fail("emptytrack: a region with notes and its Alt-copy on the first track, the track below empty: %s %s %s" % (ch0, a0, b0))
    c2 = ch1[1] if len(ch1) > 1 else None
    if c2 is None or c2 == c1 or ch1[0] != c1 or n1 != n0 + 1:
        fail("emptytrack: the empty track becomes an instrument track of its own (a copy of the sound), the first "
             "keeps its instrument: tracks %s, channels %d then %d" % (ch1, n0, n1))
    elif a1 != [str(c2)] or b1 != [str(c1)]:
        fail("emptytrack: the moved region's notes are on the track's instrument (%s): %s; the copy left behind keeps "
             "the first track's (%s): %s" % (c2, a1, c1, b1))
    m = re.findall(r'smoothlib track (\d+) "([^"]*)" mode (\d+) sound "([^"]*)"', out)
    rows = {int(t): (nm, int(md), snd) for t, nm, md, snd in m[:2]} if len(m) >= 2 else {}
    if not rows.get(1) or rows[1][1] != 2 or not rows[1][2] or rows[1][2] != rows.get(0, ("", 0, ""))[2] or rows[1][0] != rows[1][2]:
        fail("emptytrack: the track is an instrument track and its header names the instrument: %s" % m[:2])
    st = re.search(r'smoothlib state: shown \d+ saved "[^"]*" nav \d+ track (\d+) sound "([^"]*)"', out)
    if not st or st.group(1) != "1" or not st.group(2) or st.group(2) == "No instrument" or (rows.get(1) and st.group(2) != rows[1][2]):
        fail("emptytrack: the Library names the selected track's instrument: %s" % (st.group(0) if st else out[-400:]))
    if ch2[1] != c2 or b2 != [str(c2)]:
        fail("emptytrack: a second region moved there plays the track's instrument (%s): %s, tracks %s" % (c2, b2, ch2))
    if b3 != [str(c1)] or a3 != [str(c2)] or ch3[1] != c2:
        fail("emptytrack: one undo step takes the second region back with its sound: %s %s %s" % (b3, a3, ch3))
    if a4 != [str(c1)] or (len(ch4) > 1 and ch4[1] is not None) or n4 != n0 or ch4[0] != c1:
        fail("emptytrack: one more undo step: the region back with its sound, the track empty, no extra channel: %s %s %d" % (a4, ch4, n4))
    tail = m[2:]
    if any(t == "1" for t, _, _, _ in tail):
        fail("emptytrack: after undo the track has no instrument: %s" % tail)
    if not [f for f in failures if f.startswith("emptytrack")]:
        print("ok - emptytrack: an empty track takes the instrument of a region moved onto it, one undo step")


def check_emptymulti():
    fresh = os.path.join(OUT, "emptymulti_fresh.gridline.json")
    sc = Script()
    sc.at('save "%s"' % fresh, 4)
    run("emptymulti_fresh", sc)
    d = json.load(open(fresh))
    ids = {c["name"]: c["id"] for c in d.get("channels", [])}
    if "Synth" not in ids or "Snare" not in ids or not d.get("patterns"):
        return fail("emptymulti: a new project's Synth and Snare channels and its pattern: %s" % sorted(ids))
    synth, snare = ids["Synth"], ids["Snare"]
    p0 = dict(d["patterns"][0])
    pa = dict(p0, id=150, name="A", notes={str(synth): [dict(length=24, pitch=60, start=0, velocity=100)]}, steps={})
    pb = dict(p0, id=151, name="B", notes={str(snare): [dict(length=24, pitch=60, start=0, velocity=100)]}, steps={})
    d["patterns"] = d["patterns"] + [pa, pb]
    d["playlist"] = [dict(id=91, length=384, pattern=150, start=0, track=0), dict(id=92, length=384, pattern=151, start=768, track=0)]
    d["nextId"] = 200
    song = os.path.join(OUT, "emptymulti.gridline.json")
    json.dump(d, open(song, "w"), indent=1)
    sc = Script()
    sc.wait(10)
    sc.at("plgeom", 2)
    sc.at("dump", 2)
    out = run("emptymulti_geo", sc, song=song)
    rect = {int(m.group(1)): [float(m.group(k)) for k in range(2, 6)]
            for m in re.finditer(r"\n  clip (\d+) x ([-0-9.]+) ([-0-9.]+) y ([-0-9.]+) ([-0-9.]+)", out)}
    if 91 not in rect or 92 not in rect:
        return fail("emptymulti: the two regions are drawn: %s" % rect)
    centre = lambda r: ((r[0] + r[1]) / 2, (r[2] + r[3]) / 2)
    (ax, ay), (bx, by) = centre(rect[91]), centre(rect[92])
    row = rect[91][3] - rect[91][2] + 2
    results = {}
    for tag, copy in (("move", False), ("copy", True)):
        after = os.path.join(OUT, "emptymulti_%s_after.gridline.json" % tag)
        undone = os.path.join(OUT, "emptymulti_%s_undone.gridline.json" % tag)
        sc = Script()
        sc.wait(10)
        sc.click(ax, ay, settle=4)
        sc.at("mod shift 1")
        sc.click(bx, by, settle=4)
        sc.at("mod shift 0", 3)
        if copy:
            sc.at("mod alt 1")
        sc.drag(ax, ay, ax, ay + row)
        if copy:
            sc.at("mod alt 0", 3)
        sc.at('save "%s"' % after, 4)
        sc.cmd_key("z")
        sc.at('save "%s"' % undone, 4)
        run("emptymulti_" + tag, sc, song=song)
        results[tag] = (after, undone)
    try:
        ch, a1, n1 = song_region(results["move"][0], 1, 0)
        _, b1, _ = song_region(results["move"][0], 1, 768)
        chu, au, nu = song_region(results["move"][1], 0, 0)
        _, bu, _ = song_region(results["move"][1], 0, 768)
        chc, ac, nc = song_region(results["copy"][0], 1, 0)
        _, bc, _ = song_region(results["copy"][0], 1, 768)
        _, ao, _ = song_region(results["copy"][0], 0, 0)
        _, bo, _ = song_region(results["copy"][0], 0, 768)
        chcu, _, ncu = song_region(results["copy"][1], 1, 0)
    except (OSError, ValueError) as e:
        return fail("emptymulti: the saved projects: %s" % e)
    n0 = len(d["channels"])
    if len(ch) < 2 or ch[1] != synth or a1 != [str(synth)] or b1 != [str(synth)] or n1 != n0:
        fail("emptymulti: the empty track takes the earlier region's sound (Synth %d) and both play it: tracks %s, %s %s, "
             "channels %d" % (synth, ch[:2], a1, b1, n1))
    if (len(chu) > 1 and chu[1] is not None) or au != [str(synth)] or bu != [str(snare)] or nu != n0:
        fail("emptymulti: one undo step: both back with their sounds, the track empty: %s %s %s" % (chu[:2], au, bu))
    cp = chc[1] if len(chc) > 1 else None
    if cp is None or cp in (synth, snare) or chc[0] is not None or nc != n0 + 1:
        fail("emptymulti: Alt-copies onto the empty track: a copy of Synth of its own, the first track as it was: %s, "
             "channels %d" % (chc[:2], nc))
    elif ac != [str(cp)] or bc != [str(cp)] or ao != [str(synth)] or bo != [str(snare)]:
        fail("emptymulti: both copies play the track's sound (%s), the originals theirs: %s %s / %s %s" % (cp, ac, bc, ao, bo))
    if (len(chcu) > 1 and chcu[1] is not None) or ncu != n0:
        fail("emptymulti: one undo step takes the copies and the track's sound back: %s, channels %d" % (chcu[:2], ncu))
    if not [f for f in failures if f.startswith("emptymulti")]:
        print("ok - emptymulti: several regions onto an empty track: the earliest one's sound for all, Alt-copies too")


def check_fold():
    sc = Script()
    sc.wait(10)
    sc.at("plrows", 2)
    sc.drag(1000, 448, 1000, 268, frames=40)
    sc.at("dump", 2)
    sc.at("plrows", 2)
    out = run("fold", sc, song=DEMO)
    blocks = out.split("plrows n")[1:]
    rows = [re.findall(r'plrow (\d+) "([^"]*)" [-0-9.]+ [-0-9.]+ top [-0-9.]+ h ([0-9.]+)', b) for b in blocks]
    m = re.search(r"dump frame \d+: \d+ clips, \d+ selected, undo (\d+)", out)
    if len(rows) < 2 or not rows[0] or not m or m.group(1) == "0":
        return fail("fold: the demo's rows before and after a region was moved (undo %s)" % (m.group(1) if m else "?"))
    shown = lambda rs: [(n, nm) for n, nm, h in rs if float(h) > 0]
    if shown(rows[0]) != shown(rows[1]):
        gone = set(shown(rows[0])) - set(shown(rows[1]))
        came = set(shown(rows[1])) - set(shown(rows[0]))
        fail("fold: the rows shown stay as they were when a region moves (folded away %s, shown %s)" % (sorted(gone), sorted(came)))
    else:
        print("ok - fold: moving a region leaves the automation rows shown as they were")


def check_cards():
    shot = os.path.join(OUT, "cards.ppm")
    sc = Script()
    sc.click(*APP_MENU, settle=8)
    sc.click(*APP_SETTINGS, settle=20)
    sc.click(*SETTINGS_INTERFACE, settle=30)
    sc.at("launcher", 2)
    sc.at('shot "%s"' % shot, 3)
    out = run("cards", sc)
    cards = re.findall(r"layoutcard (\d+) (\S+) x=(-?\d+) y=(-?\d+) w=(\d+) h=(\d+)", out)
    if len(cards) < 4:
        return fail("cards: the layout cards drawn: %s" % cards)
    w, h, px = read_ppm(shot)
    for _, name, cx, cy, cw, ch in cards:
        cx, cy, cw, ch = int(cx), int(cy), int(cw), int(ch)
        thumb = round((cw - 16) * 0.625)
        y0, y1 = cy + 8 + thumb + 12 + 20, cy + ch - 3  # the description, under the name
        lit = []
        for y in range(y0, min(y1, h)):
            n = 0
            for x in range(cx + 10, cx + cw - 10):
                i = (y * w + x) * 3
                if px[i] + px[i + 1] + px[i + 2] > 3 * 90:
                    n += 1
            lit.append(n > 0)
        bands, run_len = [], 0
        for on in lit + [False]:
            if on:
                run_len += 1
            elif run_len:
                bands.append(run_len)
                run_len = 0
        # Each line of text is a band of rows at least 7 px tall (lowercase with an ascender); one cut by the
        # card's clip is shorter.
        if not bands or bands[-1] < 7:
            fail("cards: the %s card's description shows whole (its last line %s px tall, lines %s)" % (name, bands[-1] if bands else 0, bands))
    if not [f for f in failures if f.startswith("cards")]:
        print("ok - cards: every layout card's description shows whole")


def check_emptyhelp():
    sc = Script()
    load_sound(sc)
    sc.at("move 1100 600", 4)
    sc.at("hint", 2)
    sc.at("move 1100 %d" % TRACK1_Y, 4)
    sc.at("hint", 2)
    out = run("emptyhelp", sc)
    hints = re.findall(r"hint frame \d+: ([^\n]*)", out)
    if len(hints) < 2:
        return fail("emptyhelp: no hint reports")
    if "Acid Line" in hints[0]:
        fail("emptyhelp: the empty space below the last track names no track: %r" % hints[0])
    if "Acid Line" not in hints[1]:
        fail("emptyhelp: over the track the hint names it: %r" % hints[1])
    if not [f for f in failures if f.startswith("emptyhelp")]:
        print("ok - emptyhelp: the space below the tracks names no track")


def check_pencil():
    sc = Script()
    load_sound(sc)
    pick_tool(sc, TOOL_PENCIL)
    sc.click(bar_x(6) + 13, TRACK1_Y, settle=8)  # 0.8 of a beat into bar 6 (a beat is 16 px here)
    sc.at("dump", 2)
    out = run("pencil", sc)
    cs = clips(out, 0)
    if not cs or len(cs) != 1:
        return fail("pencil: one region pencilled: %s" % cs)
    if cs[0]["start"] != 5 * BAR:
        fail("pencil: the region starts at the beat under the pointer (tick %d): tick %d" % (5 * BAR, cs[0]["start"]))
    else:
        print("ok - pencil: a pencilled region starts at the snap cell under the pointer")


if os.path.isdir(OUT):
    shutil.rmtree(OUT)
os.makedirs(OUT)
for name, fn in [("samestart", check_samestart), ("retarget", check_retarget), ("emptytrack", check_emptytrack), ("emptymulti", check_emptymulti), ("fold", check_fold), ("cards", check_cards),
                 ("emptyhelp", check_emptyhelp), ("pencil", check_pencil)]:
    if not ONLY or ONLY == name:
        try:
            fn()
        except RuntimeError as e:
            fail("%s: %s" % (name, e))

if failures:
    print("%d failure(s)" % len(failures))
    sys.exit(1)
print("all passed")
