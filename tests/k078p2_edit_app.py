#!/usr/bin/env python3
# Gridline - Smooth and Clips editing picks, in the real app (hidden, isolated config folder and HOME, SDL's
# dummy audio driver, fixed frame time), driven while it runs (--dev-input-live) with real SDL pointer events
# and the modifier keys held as a person holds them:
#   pointer  the piano roll's Pointer (Smooth: the lower pane; Clips: the clip band): Cmd+drag (Ctrl+drag on
#            Windows and Linux) on a note copies it: the original stays, the copy lands where the drag ends,
#            one undo step that takes the copy away; Cmd+Alt+drag copies too (Alt is the snap bypass, as
#            before); a plain drag still moves the note; Cmd+drag on an empty spot still selects with a box.
#            Studio's Select tool is unchanged: Ctrl+drag (Cmd on macOS) on a note draws a box, no copy.
#   menus    the track header menu's "Copy to a new pattern" (picked by its label) in Clips and in Smooth
#            (either Drag mode) puts a new pattern's clip on the track, one undo step, and the original
#            clip stays where it was (user 2026-11-21): in Smooth the new clip lies on the original, in
#            Clips a clip of the clip grid gets the slot under it; Smooth's Track menu in the menu
#            bar has "Duplicate without notes" and "Copy to a new pattern", which do the same.
#   chips    Clips' clip band (the clip's own view) on a clip of the clip grid: the Launch and Follow action
#            cards come first after the head card and the sample display; the Follow action card turns the
#            follow action on, the Launch card sets Legato, in the band (nothing pops over the clip grid);
#            each one undo step, saved with the clip.
#   python3 k078p2_edit_app.py <gridline-app> <demo song> <test song> <sound.wav> <out dir> <pointer|menus|chips>
import json
import os
import re
import shutil
import subprocess
import sys
import time

APP, DEMO, TEST_SONG, SOUND, OUT, GROUP = sys.argv[1:7]
SHOTS = os.environ.get("GL_SHOTS", "")  # a folder: screenshots of each state (for a person to look at)
CMD = "super" if sys.platform == "darwin" else "ctrl"  # Cmd on macOS (the app reads it as its Ctrl), else Ctrl
BAR = 384
failures = []


def fail(msg):
    failures.append(msg)
    print("FAIL: " + msg)
    sys.stdout.flush()


def ok(msg):
    print("ok - " + msg)
    sys.stdout.flush()


def want(what, got, expect):
    if got == expect:
        ok("%s (%s)" % (what, got))
    else:
        fail("%s: want %s, got %s" % (what, expect, got))


class App:
    """A hidden gridline-app driven while it runs, through --dev-input-live, with a fixed frame time."""

    def __init__(self, tag, layout, song, extra=None, args=()):
        self.work = os.path.join(OUT, tag)
        shutil.rmtree(self.work, ignore_errors=True)
        cfg = os.path.join(self.work, "cfg")
        os.makedirs(os.path.join(cfg, "gridline"))
        os.makedirs(os.path.join(cfg, "home"))
        values = {"ui.layoutChosen": True, "ui.layoutDev": True, "ui.layout": layout, "keys.profile": layout, "ui.menuStyle": layout}
        values.update(extra or {})
        with open(os.path.join(cfg, "gridline", "settings.json"), "w") as f:
            json.dump({"format": "gridline-settings", "values": values}, f)
        self.live = os.path.join(self.work, "live.txt")
        start = os.path.join(self.work, "start.txt")
        with open(start, "w") as f:
            f.write("2 toasts clear\n4 typing off\n")
        env = dict(os.environ)
        env.update(SDL_AUDIODRIVER="dummy", GRIDLINE_NO_PLUGIN_SCAN="1", GRIDLINE_HIDE_PLUGINS="all", GRIDLINE_CONFIG_HOME=cfg,
                   HOME=os.path.join(cfg, "home"))
        cmd = [APP, song, "--hidden", "--no-ui-sounds", "--no-update-check", "--size", "1440x900", "--fixed-dt", "16", "--layout", layout,
               "--layout-dev", "--dev-input", start, "--dev-input-live", self.live] + list(args)
        self.outp = os.path.join(self.work, "out.log")
        self.out = open(self.outp, "w")
        self.proc = subprocess.Popen(cmd, env=env, stdout=self.out, stderr=subprocess.STDOUT)
        end = time.time() + 120
        while time.time() < end and "frame" not in self.text():
            if self.proc.poll() is not None:
                raise RuntimeError("%s: gridline-app exited (%s)" % (tag, self.proc.returncode))
            self.send("hint")
            time.sleep(0.05)
        self.frames(20)

    def text(self):
        with open(self.outp, errors="replace") as f:
            return f.read()

    def send(self, *lines, timeout=60):
        tmp = self.live + ".tmp"
        with open(tmp, "w") as f:
            f.write("".join("0 %s\n" % ln for ln in lines))
        os.rename(tmp, self.live)
        end = time.time() + timeout
        while os.path.exists(self.live):
            if self.proc.poll() is not None:
                raise RuntimeError("gridline-app exited (%s)" % self.proc.returncode)
            if time.time() > end:
                raise RuntimeError("the app stopped reading its input")
            time.sleep(0.001)

    def ask(self, cmd, pat):
        n = len(self.text())
        self.send(cmd)
        end = time.time() + 20
        while time.time() < end:
            t = self.text()[n:]
            if re.search(pat, t):
                self.frames(2)
                return self.text()[n:]
            time.sleep(0.01)
        raise RuntimeError("no answer to " + cmd)

    def frames(self, n):
        for _ in range(n):
            self.send("hint")

    def move(self, x, y, settle=3):
        self.send("sdlmove %d %d" % (round(x), round(y)))
        self.frames(settle)

    def click(self, x, y, settle=8, button=0):
        self.move(x, y, 2)
        self.send("sdldown %d" % button)
        self.frames(2)
        self.send("sdlup %d" % button)
        self.frames(settle)

    def dclick(self, x, y, settle=20):
        self.move(x, y, 2)
        self.send("sdldown 0", "sdlup 0", "sdldown 0", "sdlup 0")
        self.frames(settle)

    def item(self, label, settle=8):
        """Clicks the item so labelled (menu rows first), as a person would."""
        t = self.ask('atitem "%s"' % label, r"frame")
        if re.search(r'atitem frame \d+: "%s" not shown' % re.escape(label), t):
            raise RuntimeError('no "%s" item drawn' % label)
        self.frames(1)
        self.send("down 0")
        self.frames(2)
        self.send("up 0")
        self.frames(settle)

    def drag(self, x0, y0, dx, dy, mods=(), steps=24):
        """Press, hold, many small motions (a frame each), release; `mods` held throughout."""
        self.move(x0, y0, 3)
        for m in mods:
            self.send("mod %s 1" % m)
        self.frames(2)
        self.send("sdldown 0")
        self.frames(3)
        for i in range(1, steps + 1):
            f = i / steps
            self.send("sdlmove %d %d" % (round(x0 + dx * f), round(y0 + dy * f)))
            self.send("hint")
        self.frames(3)
        self.send("sdlup 0")
        self.frames(3)
        for m in mods:
            self.send("mod %s 0" % m)
        self.frames(6)

    def notes(self):
        t = self.ask("notes", r"notes frame \d+[^\n]*\n")
        return sorted((int(m.group(1)), int(m.group(2)), int(m.group(3))) for m in
                      re.finditer(r"  note \d+ start (\d+) len (\d+) pitch (\d+)", t))

    def undo_pos(self):
        t = self.ask("dump", r"dump frame \d+: [^\n]*undo \d+")
        return int(re.search(r"dump frame \d+: [^\n]*undo (\d+)", t).group(1))

    def lane(self):
        t = self.ask("lane", r"lane frame \d+: [^\n]*\n")
        m = re.search(r"lane frame \d+: x0 ([-0-9.]+) pxPerTick ([0-9.]+) [^\n]* pitchY0 ([-0-9.]+) rowH ([0-9.]+)[^\n]* grid ([-0-9.]+) "
                      r"([-0-9.]+) ([-0-9.]+) ([-0-9.]+)", t)
        if not m:
            raise RuntimeError("no piano roll geometry: " + t[-300:])
        return [float(v) for v in m.groups()]

    def save(self, path):
        self.ask('save "%s"' % path, r"save frame \d+: ")
        with open(path) as f:
            return json.load(f)

    def shot(self, name):
        if SHOTS:
            os.makedirs(SHOTS, exist_ok=True)
            self.send('shot "%s"' % os.path.join(SHOTS, name + ".ppm"))
            self.frames(3)

    def quit(self):
        try:
            self.send("quit discard", timeout=10)
            self.proc.wait(timeout=30)
        except Exception:
            self.proc.terminate()
            self.proc.wait(timeout=10)
        self.out.close()


# ---------------------------------------------------------------- the Pointer's copy-drag

def pick_note(a):
    """A note well inside the piano roll's grid, with room for a drag one bar on and two rows up: its press
    point (a little after its start: never its right end, which resizes) and the drag's pixels."""
    x0, ppt, py0, rowH, gx0, gy0, gx1, gy1 = a.lane()
    for s, ln, p in a.notes():
        x = x0 + (s + min(ln * 0.3, 24)) * ppt
        y = py0 + (127 - p) * rowH + rowH / 2
        if gx0 + 10 < x and x + BAR * ppt < gx1 - 20 and gy0 + 3 * rowH < y < gy1 - 10 and ln * ppt > 12:
            return (s, ln, p), x, y, BAR * ppt, -2 * rowH
    raise RuntimeError("no note to drag in view")


def copy_drag(a, where, mods, snapped=True):
    note, x, y, dx, dy = pick_note(a)
    s, ln, p = note
    before, u0 = a.notes(), a.undo_pos()
    a.send("selnotes none")
    a.frames(2)
    a.drag(x, y, dx, dy, mods)
    after, u1 = a.notes(), a.undo_pos()
    name = "%s: %s+drag on a note" % (where, "+".join(m.replace("super", "Cmd").capitalize() for m in mods))
    added = list(after)
    for n in before:
        if n in added:
            added.remove(n)
    want(name + " keeps every note where it was", all(n in after for n in before), True)
    want(name + ": one note more", len(after), len(before) + 1)
    if len(added) == 1:
        cs, cl, cp = added[0]
        want(name + ": the copy is two rows up, of the same length", (cp, cl), (p + 2, ln))
        if snapped:
            want(name + ": a bar on", cs, s + BAR)
        else:
            want(name + ": about a bar on, off the grid (Alt)", abs(cs - (s + BAR)) <= 12 and cs != s, True)
    want(name + ": one undo step", u1 - u0, 1)
    if u1 > u0:
        a.send("undo")
        a.frames(6)
    want(name + ": undo takes the copy away", a.notes() == before, True)
    return x, y, dx, dy


def plain_drag(a, where):
    note, x, y, dx, dy = pick_note(a)
    s, ln, p = note
    before, u0 = a.notes(), a.undo_pos()
    a.send("selnotes none")
    a.frames(2)
    a.drag(x, y, dx, dy)
    after, u1 = a.notes(), a.undo_pos()
    want("%s: a plain drag still moves the note (no copy)" % where,
         (len(after), (s, ln, p) in after, (s + BAR, ln, p + 2) in after), (len(before), False, True))
    if u1 > u0:
        a.send("undo")
        a.frames(6)


def box_on_empty(a, where):
    x0, ppt, py0, rowH, gx0, gy0, gx1, gy1 = a.lane()
    notes = a.notes()
    # an empty spot: a row with no note, well inside the grid, before the notes' bar
    for p in range(127, -1, -1):
        y = py0 + (127 - p) * rowH + rowH / 2
        if gy0 + 3 * rowH < y < gy1 - 3 * rowH and not any(q in (p, p - 1, p + 1) for _, _, q in notes):
            break
    else:
        return
    u0 = a.undo_pos()
    a.drag(gx0 + 30, y, 60, 2 * rowH, (CMD,))
    want("%s: Cmd+drag on an empty spot changes no note" % where, (a.notes() == notes, a.undo_pos() - u0), (True, 0))


def check_pointer():
    # Smooth: the demo song's "Motif Opening" region (its piano roll in the lower pane, Smooth's Pointer)
    a = App("smooth", "logic", DEMO)
    try:
        a.dclick(850, 447)
        a.send("pianotool select")
        a.frames(4)
        hint_shown = False
        note, x, y, _, _ = pick_note(a)
        a.move(x, y, 4)
        h = a.ask("hint", r"hint frame \d+: [^\n]*\n")
        hint_shown = ("Cmd+drag: copy" if CMD == "super" else "Ctrl+drag: copy") in h
        want("Smooth: the Pointer's hint over a note names the copy", hint_shown, True)
        a.shot("smooth_pointer_before")
        copy_drag(a, "Smooth piano roll, Pointer", (CMD,))
        copy_drag(a, "Smooth piano roll, Pointer", (CMD, "alt"), snapped=False)
        plain_drag(a, "Smooth piano roll, Pointer")
        box_on_empty(a, "Smooth piano roll, Pointer")
        # the copy left in place for a picture
        note, x, y, dx, dy = pick_note(a)
        a.drag(x, y, dx, dy, (CMD,))
        a.shot("smooth_pointer_copied")
    finally:
        a.quit()
    # Clips: the same region's notes in the clip band (Clips' own tool, the Pointer)
    a = App("clips", "live", DEMO_ARR)
    try:
        a.dclick(400, 480)
        a.drag(870, 637, 0, -257)  # the band taller (as a person makes it to edit notes)
        a.send("pianotool select")
        a.frames(4)
        copy_drag(a, "Clips notes editor, Pointer", (CMD,))
        copy_drag(a, "Clips notes editor, Pointer", (CMD, "alt"), snapped=False)
        plain_drag(a, "Clips notes editor, Pointer")
        note, x, y, dx, dy = pick_note(a)
        a.drag(x, y, dx, dy, (CMD,))
        a.shot("clips_pointer_copied")
    finally:
        a.quit()
    # Studio: unchanged (its Select tool: Ctrl+drag draws a box with any tool, no copy)
    a = App("studio", "studio", TEST_SONG, args=("--edit", "0", "6", "--hide", "rack"))
    try:
        a.send("mknotes 4 384 192", "pianotool select")
        a.frames(4)
        note, x, y, dx, dy = pick_note(a)
        before, u0 = a.notes(), a.undo_pos()
        a.drag(x, y, dx, dy, (CMD,))
        want("Studio: Select tool, Cmd/Ctrl+drag on a note copies nothing and moves nothing", (a.notes() == before, a.undo_pos() - u0), (True, 0))
        a.drag(x, y, dx, dy)
        want("Studio: Select tool, a plain drag on a note moves nothing (a box)", a.notes() == before, True)
    finally:
        a.quit()


# ---------------------------------------------------------------- Copy to a new pattern places its clip

def song_counts(j, old_ids=()):
    """Patterns, the Bass track's clips, and the ids of patterns that are new (not in old_ids)."""
    clips = [c for c in j["playlist"] if c.get("track") == 1]
    return len(j["patterns"]), clips, set(p["id"] for p in j["patterns"]) - set(old_ids)


def check_menus():
    src = json.load(open(TEST_SONG))
    src["tracks"][1]["mode"] = 2  # the Bass track: an instrument track of the Bass channel (id 25)
    src["tracks"][1]["channel"] = 25
    base = os.path.join(OUT, "menus_song.gridline.json")
    os.makedirs(OUT, exist_ok=True)
    with open(base, "w") as f:
        json.dump(src, f)
    pats0, clips0, _ = song_counts(src)
    ids0 = [p["id"] for p in src["patterns"]]
    first = min(clips0, key=lambda c: c["start"])
    for layout, tag, extra in (("live", "clips", {"clips.arrangementSets": base}), ("logic", "smooth", {"ui.logicDragMode": 1}),
                               ("logic", "smooth_overlap", {"ui.logicDragMode": 0})):
        a = App("menus_" + tag, layout, base, extra)
        try:
            t = a.ask("plrows", r'plrow \d+ "Bass" ')
            m = re.search(r'plrow \d+ "Bass" ([-0-9]+) ([-0-9]+)', t)
            hx, hy = int(m.group(1)), int(m.group(2))
            u0 = a.undo_pos()
            a.move(hx, hy, 3)
            a.send("down 1")
            a.frames(2)
            a.send("up 1")
            a.frames(6)
            a.shot("menu_%s_track" % tag)
            a.item("Copy to a new pattern", settle=12)
            u1 = a.undo_pos()
            j = a.save(os.path.join(a.work, "after.gridline.json"))
            pats, clips, newp = song_counts(j, ids0)
            want("%s: track menu Copy to a new pattern adds a pattern" % tag, pats, pats0 + 1)
            # (Clips: the first clip may sit in the clip grid, where the new one takes the slot under it)
            new = [c for c in clips if c["pattern"] in newp and (layout == "live" or c["start"] == first["start"])]
            want("%s: a clip of the new pattern on the track where the first clip was" % tag, len(new), 1)
            kept = [c for c in clips if c["id"] == first["id"]]
            want("%s: the original clip stays where it was" % tag,
                 [(c["pattern"], c["start"], c.get("length", 0)) for c in kept], [(first["pattern"], first["start"], first.get("length", 0))])
            want("%s: the track's clips (the original and the new one)" % tag, len(clips), len(clips0) + 1)
            if new:
                want("%s: with the original clip's length" % tag, new[0].get("length", 0), first.get("length", 0))
            want("%s: one undo step" % tag, u1 - u0, 1)
            a.send("undo")
            a.frames(6)
            j = a.save(os.path.join(a.work, "undone.gridline.json"))
            pats, clips, _ = song_counts(j)
            want("%s: undo takes the pattern and the clip back" % tag, (pats, len(clips), any(c["id"] == first["id"] for c in clips)),
                 (pats0, len(clips0), True))
            if layout == "logic":  # the menu bar's Track menu: the same two rows, for the selected track
                a.send("clipsel %d" % first["id"])
                a.frames(3)
                a.send("menu open Track")
                a.frames(6)
                a.shot("menubar_%s_track" % tag)
                u0 = a.undo_pos()
                a.item("Copy to a new pattern", settle=12)
                j = a.save(os.path.join(a.work, "menubar.gridline.json"))
                pats, clips, newp = song_counts(j, ids0)
                want("%s: Track menu (menu bar) Copy to a new pattern: a pattern and its clip, one undo step" % tag,
                     (pats, sum(1 for c in clips if c["pattern"] in newp), a.undo_pos() - u0), (pats0 + 1, 1, 1))
                a.send("undo")
                a.frames(6)
                a.send("menu open Track")
                a.frames(6)
                u0 = a.undo_pos()
                a.item("Duplicate without notes", settle=12)
                j = a.save(os.path.join(a.work, "menubar_dup.gridline.json"))
                want("%s: Track menu (menu bar) Duplicate without notes: a track with no clips after Bass, one undo step" % tag,
                     (len([c for c in j["playlist"] if c.get("track") == 2]), j["tracks"][2].get("mode"), a.undo_pos() - u0), (0, 2, 1))
        finally:
            a.quit()


# ---------------------------------------------------------------- Launch and Follow action cards

def cards(a):
    t = a.ask("clipeditor geom", r"clipeditor audible frame")
    return [m.group(1) for m in re.finditer(r'clipeditor card "([^"]+)" ([-0-9.]+) ([-0-9.]+) ([-0-9.]+) ([-0-9.]+)', t)]


def check_chips():
    src = json.load(open(TEST_SONG))
    shutil.copy(SOUND, os.path.join(OUT, "Take.wav"))
    src["channels"].insert(0, {"color": 5219296, "gain": 0.8, "id": 60, "mixerTrack": 0, "muted": False, "name": "Take", "pan": 0, "pitch": 0,
                               "plugin": {"type": "gl.sampler", "data": os.path.join(OUT, "Take.wav"), "params": []}})
    # a clip of the clip grid (the live area: clips that start before its end)
    src["playlist"].insert(0, {"id": 70, "kind": "audio", "source": 60, "track": 7, "start": 0, "length": 384, "pattern": 0, "gain": 1.0})
    src["live"] = dict(src.get("live") or {}, areaEnd=384)
    src["nextId"] = 100
    song = os.path.join(OUT, "chips_song.gridline.json")
    with open(song, "w") as f:
        json.dump(src, f)
    a = App("chips", "live", song)
    try:
        a.send("clipeditor dbl 70")
        a.frames(10)
        a.send("clipeditor tab 0")  # the clip's cards in view
        a.frames(6)
        ids = cards(a)
        want("Clips band: the head card, the sample display, then the Launch and Follow action cards", ids[:4], ["head", "wave", "launch", "follow"])
        a.shot("chips_band")
        # Follow action: its switch, in its card in the band
        u0 = a.undo_pos()
        a.item("Follow action")
        u1 = a.undo_pos()
        want("Follow action card: the follow action on, one undo step", u1 - u0, 1)
        j = a.save(os.path.join(a.work, "follow.gridline.json"))
        clip = next(k for k in j["playlist"] if k["id"] == 70)
        want("saved with the clip", clip.get("launchSet", {}).get("follow", {}).get("on"), True)
        a.shot("chips_follow")
        # Launch: Legato
        u0 = a.undo_pos()
        a.item("Legato")
        u1 = a.undo_pos()
        j = a.save(os.path.join(a.work, "legato.gridline.json"))
        clip = next(k for k in j["playlist"] if k["id"] == 70)
        want("Launch card: Legato on, one undo step, saved", (u1 - u0, clip.get("launchSet", {}).get("legato")), (1, True))
        a.send("undo")
        a.frames(6)
        j = a.save(os.path.join(a.work, "undone.gridline.json"))
        clip = next(k for k in j["playlist"] if k["id"] == 70)
        want("undo: Legato off again, the follow action kept", (clip.get("launchSet", {}).get("legato", False),
                                                                clip.get("launchSet", {}).get("follow", {}).get("on")), (False, True))
    finally:
        a.quit()
    # an arrangement clip has no launch cards
    src["live"]["areaEnd"] = 0
    song2 = os.path.join(OUT, "chips_song2.gridline.json")
    with open(song2, "w") as f:
        json.dump(src, f)
    a = App("chips_arr", "live", song2)
    try:
        a.send("clipeditor dbl 70")
        a.frames(10)
        ids = cards(a)
        want("an arrangement clip: no Launch or Follow action card, the Clip card first", (ids + ["-", "-", "-"])[2], "clip")
    finally:
        a.quit()


DEMO_ARR = os.path.join(OUT, "demo_arrangement.gridline.json")
os.makedirs(OUT, exist_ok=True)
with open(DEMO) as f:
    d = json.load(f)
d["live"] = dict(d.get("live") or {}, view="arrangement")
with open(DEMO_ARR, "w") as f:
    json.dump(d, f)

try:
    {"pointer": check_pointer, "menus": check_menus, "chips": check_chips}[GROUP]()
except RuntimeError as e:
    fail(str(e))
if failures:
    print("%d failure(s)" % len(failures))
    sys.exit(1)
print("all passed")
