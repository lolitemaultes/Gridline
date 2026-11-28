#!/usr/bin/env python3
# Gridline - k078: notes, regions, clips and the loop range dragged in Smooth and Clips the way a person drags,
# in the real app (hidden, isolated config folder and HOME, SDL's dummy audio driver), on the demo song. The
# pointer is real SDL mouse events sent while the app runs in real time (--dev-input-live, no fixed frame
# time): a press, a hold, then many small motions over real time (several to a frame when the app is slower
# than the stream), released after it. Each drag is made with several streams: a hand's drag (1.5 s, 120
# events a second), a slow one (3 s), a flick (0.25 s), a trackpad's (2 s of 250 tiny steps a second) and one
# that leaves the editor and comes back; at 1x and at 2x (window pixels twice the interface's, --ui-scale 2
# on a 2880 x 1800 window: the same layout in logical pixels). One run also has a fixed frame time. One more
# stream loses the keys to another window a third of the way through, with the button still down (the real
# app on macOS: every window shown took them; the drag stopped after a few steps); it must reach the end.
#   smooth  the piano roll in the lower pane with Smooth's own tool (the Pointer): a note dragged 7 rows and
#           3 bars in one drag, two selected notes together, a note's right end resized by 2 bars; a region
#           dragged 3 tracks and 4 bars; each one undo step. With the Pointer a press on a note drew a
#           selection box from it, so a note never followed the pointer (only the arrow keys moved it, a step
#           at a time). The Draw tool's drag, which worked, is checked too.
#   clips   the notes editor in the clip band (taller, zoomed out as a person would) with Clips' own tool
#           (draw mode off): the same note cases as Smooth; an arrangement clip dragged by its title bar and by
#           its body 3 tracks and 4 bars; each one undo step. A press on a note drew a box there too.
#   audio   Clips: an audio clip dragged 3 audio tracks (over a MIDI track) and 4 bars; a clip of the clip grid
#           dragged 3 slots down and 3 tracks on.
#   loop    the loop range, the one setting every layout shares: Smooth's cycle area dragged out in the ruler
#           over 7 bars, each edge and the whole area dragged, a click switching it off and on; Clips' loop
#           brace switched on, moved 4 bars, each end dragged over bars, a click switching it; playback
#           loops over the range in both (song mode), and while recording in Smooth.
#   python3 k078_drag_app.py <gridline-app> <demo song> <out dir> <smooth|clips|audio|loop> [1|2] [fixed]
import json
import math
import os
import re
import shutil
import subprocess
import sys
import time

APP, DEMO, OUT, GROUP = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
# The demo song is opened from a copy in this test's own folder: an edit of its audio saves an
# "Edited audio" folder beside the project, which must never land in the source tree.
os.makedirs(OUT, exist_ok=True)
_demo_copy = os.path.join(OUT, "demo.gridline.json")
if os.path.abspath(DEMO) != os.path.abspath(_demo_copy):
    shutil.copyfile(DEMO, _demo_copy)
DEMO = _demo_copy
SCALE = float(sys.argv[5]) if len(sys.argv) > 5 else 1.0
FIXED = len(sys.argv) > 6 and sys.argv[6] == "fixed"
# 0.7.8 (the user's own drag log from the real app, rc5): the menu bar's walk of every menu (macOS fills
# the system menu bar from the menus' code, again whenever the project or the selection changes, so on
# every step of a drag) drew a plugin picker's search field, which asks for the keyboard as it appears:
# it took the active item from the note or the clip being dragged, which stopped after its first step
# with the button still down. "menubar" runs (group menubar) fill the system menu bar as the real app
# does (--system-menu), with the idle pacing of the real app on, each drag starting from the long idle.
MENUBAR = GROUP == "menubar"
failures = []
BAR = 384


def fail(msg):
    failures.append(msg)
    print("FAIL: " + msg)
    sys.stdout.flush()


def ok(msg):
    print("ok - " + msg)
    sys.stdout.flush()


STREAMS = [  # name, seconds, motion events a second, path
    ("hand", 1.5, 120, None),
    ("slow", 3.0, 60, None),
    ("flick", 0.25, 120, None),
    ("trackpad", 2.0, 250, None),
    ("leave", 2.0, 120, "leave"),
]
# 0.7.8 (user report from the real app: a note moved a few steps and stopped, a region one step, while the
# button was still down): a third of the way through, the main window loses the keys (another window came to
# the front: on macOS every window shown took them). The drag must go on to the end all the same.
FOCUS = ("focus lost", 1.5, 120, "focus")


class App:
    """A hidden gridline-app driven while it runs, through --dev-input-live."""

    def __init__(self, tag, layout, song):
        self.work = os.path.join(OUT, tag)
        shutil.rmtree(self.work, ignore_errors=True)
        cfg = os.path.join(self.work, "cfg")
        os.makedirs(os.path.join(cfg, "gridline"))
        os.makedirs(os.path.join(cfg, "home"))
        with open(os.path.join(cfg, "gridline", "settings.json"), "w") as f:
            json.dump({"format": "gridline-settings", "values": {"ui.layoutChosen": True, "ui.layout": layout, "keys.profile": layout,
                                                                 "ui.menuStyle": layout}}, f)
        self.live = os.path.join(self.work, "live.txt")
        start = os.path.join(self.work, "start.txt")
        with open(start, "w") as f:
            f.write("4 typing off\n")
        env = dict(os.environ)
        env.update(SDL_AUDIODRIVER="dummy", GRIDLINE_NO_PLUGIN_SCAN="1", GRIDLINE_HIDE_PLUGINS="all", GRIDLINE_CONFIG_HOME=cfg,
                   HOME=os.path.join(cfg, "home"))
        size = "%dx%d" % (1440 * SCALE, 900 * SCALE)
        args = [APP] + ([song] if song else []) + ["--hidden", "--no-ui-sounds", "--no-update-check", "--size", size, "--dev-input", start,
                                                   "--dev-input-live", self.live]
        if SCALE != 1.0:
            args += ["--ui-scale", "%g" % SCALE]
        if FIXED:
            args += ["--fixed-dt", "16"]
        if MENUBAR:
            args += ["--system-menu"]
            env["GRIDLINE_IDLE_PACING"] = "1"
            env["GRIDLINE_DRAGLOG"] = os.path.join(self.work, "drag.log")
        self.outp = os.path.join(self.work, "out.log")
        self.out = open(self.outp, "w")
        self.proc = subprocess.Popen(args, env=env, stdout=self.out, stderr=subprocess.STDOUT)
        end = time.time() + 120
        while time.time() < end and "frame" not in self.text():
            if self.proc.poll() is not None:
                raise RuntimeError("%s: gridline-app exited (%s)" % (tag, self.proc.returncode))
            self.send("hint")
            time.sleep(0.05)
        self.frames(20)
        # With the system menu bar filled, the layout's own menu row is gone: everything sits that much higher.
        self.yoff = 0
        if MENUBAR:
            pat = r'window "##topbar" pos [^\n]* size [0-9.]+x([0-9.]+)'
            self.yoff = float(re.search(pat, self.ask("windows", pat)).group(1)) - {"logic": 88, "live": 70}[layout]

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

    def at(self, name):
        """The centre of a noted control (an "sdlat" target), in logical px: where the app draws it now."""
        m = re.search(r"sdlat frame \d+: %s at (-?\d+),(-?\d+)" % re.escape(name), self.ask("sdlat " + name, r"sdlat frame \d+: %s (at|not)" % re.escape(name)))
        if not m:
            raise RuntimeError("%s is not shown" % name)
        return int(m.group(1)) / SCALE, int(m.group(2)) / SCALE - self.yoff

    def pt(self, x, y):  # logical px to window px
        return "%d %d" % (round(x * SCALE), round((y + self.yoff) * SCALE))

    def move(self, x, y, settle=3):
        self.send("sdlmove " + self.pt(x, y))
        self.frames(settle)

    def click(self, x, y, settle=8):
        self.move(x, y, 2)
        self.send("sdldown 0")
        self.frames(2)
        self.send("sdlup 0")
        self.frames(settle)

    def dclick(self, x, y, settle=20):
        self.move(x, y, 2)
        if MENUBAR:  # idle frames are up to a quarter second apart: the four in one go, as a hand's double click comes
            self.send("sdldown 0", "sdlup 0", "sdldown 0", "sdlup 0")
            self.frames(20)
            return
        self.send("sdldown 0")
        self.send("sdlup 0")
        self.send("sdldown 0")
        self.send("sdlup 0")
        self.frames(settle)

    def key(self, k, mods=()):
        for m in mods:
            self.send("mod %s 1" % m)
        self.send("key " + k)
        for m in mods:
            self.send("mod %s 0" % m)
        self.frames(4)

    def drag(self, x0, y0, dx, dy, stream=STREAMS[0], hold=0.15):
        """A person's drag: press, hold, then motion events over real time, release."""
        _, secs, rate, path = stream
        self.move(x0, y0, 3)
        self.send("sdldown 0")
        time.sleep(hold)
        self.frames(1)
        n = max(2, int(secs * rate))
        t0 = time.time()
        pending = []
        for i in range(1, n + 1):
            f = i / n
            if path == "leave":  # half-way out 300 px above the start (out of the editor), then on to the end
                if f < 0.5:
                    g = 0.5 - 0.5 * math.cos(math.pi * f / 0.5)
                    x, y = x0 + dx * 0.5 * g, y0 - 300 * g
                else:
                    g = 0.5 - 0.5 * math.cos(math.pi * (f - 0.5) / 0.5)
                    x, y = x0 + dx * 0.5 + dx * 0.5 * g, y0 - 300 + (dy + 300) * g
            else:
                e = 0.5 - 0.5 * math.cos(math.pi * f)
                x, y = x0 + dx * e, y0 + dy * e
            pending.append("sdlmove " + self.pt(x, y))
            if path == "focus" and i == n // 3:
                pending.append("focuslost")
            wait = t0 + secs * f - time.time()
            if wait > 0.004 or i == n:
                self.send(*pending)
                pending = []
                wait = t0 + secs * f - time.time()
                if wait > 0:
                    time.sleep(wait)
        time.sleep(0.15)
        self.frames(2)
        self.send("sdlup 0")
        self.frames(8)

    def notes(self):
        t = self.ask("notes", r"notes frame \d+")
        return [(int(m.group(1)), int(m.group(2)), int(m.group(3))) for m in
                re.finditer(r"  note \d+ start (\d+) len (\d+) pitch (\d+)", t)]

    def dump(self):
        t = self.ask("dump", r"dump frame \d+: \d+ clips[^\n]*undo \d+[^\n]*\n(?:  [^\n]*\n)*  cycle on")
        clips = {int(m.group(1)): (int(m.group(2)), int(m.group(3)), int(m.group(4))) for m in
                 re.finditer(r"  clip (\d+) kind \d+ track (-?\d+) start (\d+) len (\d+) ", t)}
        undo = int(re.search(r"undo (\d+)", t).group(1))
        cy = re.search(r"  cycle on (\d) start (\d+) end (\d+)", t)
        return clips, undo, (int(cy.group(1)), int(cy.group(2)), int(cy.group(3)))

    def undo_pos(self):
        return self.dump()[1]

    def transport(self):
        t = self.ask("transport", r"transport frame \d+: pos")
        m = re.search(r"transport frame \d+: pos (\d+) playing (\d)", t)
        return int(m.group(1)), m.group(2) == "1"

    def quit(self):
        try:
            self.send("quit discard", timeout=10)
            self.proc.wait(timeout=30)
        except Exception:
            self.proc.terminate()
            self.proc.wait(timeout=10)
        self.out.close()


def label(what, stream):
    return "%s, %s drag, %gx%s" % (what, stream[0], SCALE, ", fixed frame time" if FIXED else "")


def note_drag(a, what, x0, y0, dx, dy, moved, stream, prep=None):
    """Drags from (x0, y0) by (dx, dy); `moved` maps (start, pitch) before -> after for the notes that must move.
    One undo step, then undone (the notes back as they were). Nothing is selected before, unless `prep` selects."""
    a.send("selnotes none")
    a.frames(2)
    if prep:
        prep()
    before = a.notes()
    u0 = a.undo_pos()
    a.drag(x0, y0, dx, dy, stream)
    after = a.notes()
    u1 = a.undo_pos()
    have = set((s, p) for s, _, p in after)
    gone = set((s, p) for s, _, p in before) - have
    want_gone = set(moved.keys())
    want_new = set(moved.values())
    name = label(what, stream)
    if gone == want_gone and want_new <= have and u1 == u0 + 1:
        ok("%s: %s" % (name, ", ".join("%s -> %s" % (k, v) for k, v in sorted(moved.items()))))
    else:
        new = sorted(have - set((s, p) for s, _, p in before))
        fail("%s: want %s, got %s -> %s, undo +%d" % (name, sorted(moved.items()), sorted(gone), new, u1 - u0))
    if u1 > u0:
        a.send("undo")
        a.frames(6)
    if sorted(a.notes()) != sorted(before):
        fail("%s: undo did not put the notes back" % name)


def note_resize(a, what, x0, y0, dx, note, min_len, stream, prep=None):
    a.send("selnotes none")
    a.frames(2)
    if prep:
        prep()
    u0 = a.undo_pos()
    a.drag(x0, y0, dx, 0, stream)
    got = [ln for s, ln, p in a.notes() if (s, p) == note]
    u1 = a.undo_pos()
    name = label(what, stream)
    if got and got[0] >= min_len and u1 == u0 + 1:
        ok("%s: %s is %d ticks long" % (name, note, got[0]))
    else:
        fail("%s: want %s at least %d ticks long in one undo step, got %s, undo +%d" % (name, note, min_len, got, u1 - u0))
    if u1 > u0:
        a.send("undo")
        a.frames(6)


def clip_drag(a, what, x0, y0, dx, dy, clip, want, stream, prep=None):
    """`want`: (track, start) of `clip` after the drag; one undo step, then undone."""
    if prep:
        prep()
    c0, u0, _ = a.dump()
    a.drag(x0, y0, dx, dy, stream)
    c1, u1, _ = a.dump()
    got = c1.get(clip, (None, None, None))[:2]
    name = label(what, stream)
    if got == want and u1 == u0 + 1:
        ok("%s: clip %d from track %d at %d to track %d at %d" % (name, clip, c0[clip][0], c0[clip][1], want[0], want[1]))
    else:
        fail("%s: clip %d want track/start %s, got %s (from %s), undo +%d" % (name, clip, want, got, c0.get(clip), u1 - u0))
    if u1 > u0:
        a.send("undo")
        a.frames(6)
    if a.dump()[0] != c0:
        fail("%s: undo did not put the clips back" % name)


# ---------------------------------------------------------------- Smooth
# At 1440 x 900 (logical px), the demo song in Smooth: Graphite Mallet's region "Motif Opening" (clip 161, track
# 8, bar 1) at y 447; its piano roll in the lower pane: bar 1 at x 686, 94 px a bar, 14 px a row, D#5 (75) at
# y 674, G#4 (68) at y 772. Note (432, 68) is 528 ticks long, its body x 787..910.
def check_smooth():
    a = App("smooth_%g%s" % (SCALE, "f" if FIXED else ""), "logic", DEMO)
    try:
        for st in STREAMS[:2] + STREAMS[3:4] + [FOCUS]:
            clip_drag(a, "Smooth tracks area: a region 3 tracks down and 4 bars on", 850, 447, 264, 108, 161, (10, 4 * BAR), st)
        a.dclick(850, 447)  # the region's piano roll in the lower pane
        hint = a.ask("hint", r"hint frame")
        for st in STREAMS + [FOCUS]:
            note_drag(a, "Smooth piano roll, Pointer: a note 7 rows up and 3 bars on", 845, 772, 282, -98, {(432, 68): (432 + 3 * BAR, 75)}, st)

        def two():
            a.click(845, 772)  # the note, then Shift+click the one at (336, 75): both selected
            a.send("mod shift 1")
            a.click(775, 674)
            a.send("mod shift 0")
            a.frames(3)
        for st in STREAMS[:2] + STREAMS[3:4]:
            note_drag(a, "Smooth piano roll, Pointer: two selected notes 7 rows up and 3 bars on", 845, 772, 282, -98,
                      {(432, 68): (432 + 3 * BAR, 75), (336, 75): (336 + 3 * BAR, 82)}, st, prep=two)
        for st in STREAMS[:1] + STREAMS[3:4]:
            note_resize(a, "Smooth piano roll, Pointer: a note's right end 2 bars on", 906, 772, 188, (432, 68), 528 + 2 * BAR - 96, st)
        # The Draw tool, which moved notes already (its drag is the one Studio's piano roll has)
        a.click(925, 539)
        a.click(958, 567)
        for st in STREAMS[:1] + STREAMS[3:4]:
            note_drag(a, "Smooth piano roll, Draw: a note 7 rows up and 3 bars on", 845, 772, 282, -98, {(432, 68): (432 + 3 * BAR, 75)}, st)
        if "Motif Opening" not in hint and "Bar" not in hint:
            print("note: hint after opening: " + hint.strip()[-200:])
    finally:
        a.quit()


# ---------------------------------------------------------------- Clips
# The demo song in Clips' Arrangement: "Motif Opening" (clip 161, track 8) title bar at y 465, body at y 488,
# bar 1 at x 309, 64.75 px a bar, 46 px a track. Double-clicked, its notes in the clip band; the band made
# taller (its top edge from y 637 to 380) and the editor's time zoomed out (the overview's right end from x 712
# to 1100): bar 1 at x 570, 87.5 px a bar, 14 px a row, G#4 (68) at y 525, D#5 (75) at y 427 is above the grid.
def check_clips():
    a = App("clips_%g%s" % (SCALE, "f" if FIXED else ""), "live", DEMO_ARR)
    try:
        for st in STREAMS[:1] + STREAMS[3:4] + [FOCUS]:
            # in Clips a MIDI clip moved over another one on a track replaces its part there (clip 474: what is left)
            clip_drag(a, "Clips Arrangement: a MIDI clip by its title bar 3 tracks up and 4 bars on", 400, 465, 260, -138, 161, (4, 4 * BAR), st)
            clip_drag(a, "Clips Arrangement: a MIDI clip by its body 3 tracks up and 4 bars on", 400, 488, 260, -138, 161, (4, 4 * BAR), st)
        a.dclick(400, 480)  # its notes in the clip band
        a.drag(870, 637, 0, -257, STREAMS[0])  # the band taller
        a.drag(712, 412, 388, 0, STREAMS[0])  # the editor's time zoomed out
        for st in STREAMS + [FOCUS]:
            note_drag(a, "Clips notes editor: a note 7 rows down and 3 bars on", 720, 525, 262, 98, {(432, 68): (432 + 3 * BAR, 61)}, st)

        def two_b():
            a.click(720, 525)
            a.send("mod shift 1")
            a.click(637, 525)  # the note at (264, 68), x 630..646
            a.send("mod shift 0")
            a.frames(3)
        for st in STREAMS[:2] + STREAMS[3:4]:
            note_drag(a, "Clips notes editor: two selected notes 7 rows down and 3 bars on", 720, 525, 262, 98,
                      {(432, 68): (432 + 3 * BAR, 61), (264, 68): (264 + 3 * BAR, 61)}, st, prep=two_b)
        for st in STREAMS[:1] + STREAMS[3:4]:
            note_resize(a, "Clips notes editor: a note's right end 2 bars on", 782, 525, 175, (432, 68), 528 + 2 * BAR - 96, st)
    finally:
        a.quit()


def check_audio():
    tone = os.path.join(OUT, "tone.wav")
    if not os.path.exists(tone):
        import struct
        import wave
        w = wave.open(tone, "wb")
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(44100)
        w.writeframes(b"".join(struct.pack("<h", int(8000 * math.sin(i * 0.05))) for i in range(44100 * 2)))
        w.close()
    # Clips' new set (1 MIDI, 2 and 3 Audio) with three audio tracks added (Cmd+T: 4 Audio, 5 MIDI (the new set's
    # own), 6 and 7 Audio), the Arrangement shown, a sound at bar 2 of track 3: x 372..436 (0.17 px a tick from
    # x 306), y 227..271.
    a = App("audio_%g%s" % (SCALE, "f" if FIXED else ""), "live", None)
    try:
        a.key("t", ("super",))
        a.key("t", ("super",))
        a.key("t", ("super",))
        a.key("tab")
        a.send("audio \"%s\" 2 1" % tone)  # (quoted: the folder may hold spaces)
        a.frames(20)
        clips, _, _ = a.dump()
        audio = [k for k, v in clips.items()]
        if len(audio) != 1:
            fail("the audio clip was not added: %s" % clips)
            return
        cid = audio[0]
        for st in STREAMS[:2] + STREAMS[3:4]:
            clip_drag(a, "Clips Arrangement: an audio clip 3 audio tracks down (over a MIDI track) and 4 bars on", 404, 235, 261, 138, cid,
                      (5, BAR + 4 * BAR), st)
    finally:
        a.quit()
    # The demo song's clip grid: a MIDI clip made in Drums' second slot (double-click), dragged 3 slots down and
    # 3 tracks on (96 px a track, 20 px a slot).
    a = App("grid_%g%s" % (SCALE, "f" if FIXED else ""), "live", DEMO)
    try:
        a.dclick(360, 137)
        clips, _, _ = a.dump()
        made = max(clips)
        start0 = clips[made][1]
        for st in STREAMS[:2] + STREAMS[3:4]:
            clip_drag(a, "Clips clip grid: a clip 3 slots down and 3 tracks on", 360, 137, 290, 60, made, (3, start0 + 3 * 4 * BAR), st)
    finally:
        a.quit()


def cycle(a):
    return a.dump()[2]


def want_cycle(a, what, want):
    got = cycle(a)
    if got == want:
        ok("%s: loop %s" % (what, want))
    else:
        fail("%s: want the loop (on, start, end) %s, got %s" % (what, want, got))


def loops_over(a, what, lo, hi, record=False):
    """Plays (Space, or Record) and samples the position until it has wrapped back twice (up to 60 s: the dummy
    audio driver's clock runs slower on a busy machine): once inside the range it stays in [lo, hi]."""
    if record:
        a.click(547, 56, settle=20)  # Smooth's Record button
    else:
        a.key("space")
    t0 = time.time()
    pos = []
    wraps = 0
    while time.time() - t0 < 60 and wraps < 2:
        pos.append(a.transport()[0])
        if len(pos) > 1 and pos[-1] < pos[-2]:
            wraps += 1
        time.sleep(0.2)
    a.key("space")
    inside = [i for i, p in enumerate(pos) if lo <= p <= hi]  # (the end itself: the moment it wraps)
    after = pos[inside[0]:] if inside else []
    if after and all(lo <= p <= hi for p in after) and wraps >= 2:
        ok("%s: playback loops over %d..%d (%d samples, it wrapped %d times)" % (what, lo, hi, len(pos), wraps))
    else:
        fail("%s: playback does not loop over %d..%d: %s" % (what, lo, hi, pos))


def check_loop():
    # Smooth: the ruler of the tracks area at y 140, bar 1 at x 764, 65.75 px a bar.
    bx = lambda b: 764 + (b - 1) * 65.75  # noqa: E731
    a = App("loop_smooth_%g%s" % (SCALE, "f" if FIXED else ""), "logic", DEMO)
    try:
        st = STREAMS[0]
        a.drag(bx(2) + 3, 140, bx(9) - bx(2), 0, st)
        want_cycle(a, label("Smooth: a cycle area dragged out in the ruler from bar 2 to bar 9", st), (1, BAR, 8 * BAR))
        a.drag(bx(9), 140, bx(10) - bx(9), 0, STREAMS[3])
        want_cycle(a, label("Smooth: its right edge dragged a bar on", STREAMS[3]), (1, BAR, 9 * BAR))
        a.drag(bx(2), 140, -(bx(2) - bx(1)), 0, STREAMS[1])
        want_cycle(a, label("Smooth: its left edge dragged a bar back", STREAMS[1]), (1, 0, 9 * BAR))
        a.drag(bx(5) + 20, 140, bx(7) - bx(5), 0, st)
        want_cycle(a, label("Smooth: the area dragged 2 bars on", st), (1, 2 * BAR, 11 * BAR))
        a.click(bx(5) + 20, 140)
        want_cycle(a, "Smooth: a click on the area turns Cycle off (the range kept)", (0, 2 * BAR, 11 * BAR))
        a.click(bx(5) + 20, 140)
        want_cycle(a, "Smooth: a click on it again turns Cycle on", (1, 2 * BAR, 11 * BAR))
    finally:
        a.quit()
    a = App("loop_smooth_play_%g%s" % (SCALE, "f" if FIXED else ""), "logic", DEMO)
    try:
        a.drag(bx(2) + 3, 140, bx(4) - bx(2), 0, STREAMS[0])
        want_cycle(a, "Smooth: a cycle area of bars 2 and 3", (1, BAR, 3 * BAR))
        loops_over(a, "Smooth: song playback", BAR, 3 * BAR)
        loops_over(a, "Smooth: recording", BAR, 3 * BAR, record=True)
    finally:
        a.quit()
    # Clips: the Arrangement's ruler, the loop brace row at y 112, bar 1 at x 309, 64.75 px a bar.
    cx = lambda b: 309 + (b - 1) * 64.75  # noqa: E731
    a = App("loop_clips_%g%s" % (SCALE, "f" if FIXED else ""), "live", DEMO_ARR)
    try:
        # Loop: no brace yet, four bars from the play position. The switch is found where the bar draws it
        # (its place moves as the control bar's items change: the metronome box, pinned buttons).
        a.click(*a.at("loop"))
        want_cycle(a, "Clips: the Loop switch makes a brace of 4 bars", (1, 0, 4 * BAR))
        a.drag(cx(3), 112, cx(7) - cx(3), 0, STREAMS[0])
        want_cycle(a, label("Clips: the brace dragged 4 bars on", STREAMS[0]), (1, 4 * BAR, 8 * BAR))
        a.drag(cx(9), 112, cx(13) - cx(9), 0, STREAMS[3])
        want_cycle(a, label("Clips: its right end dragged 4 bars on", STREAMS[3]), (1, 4 * BAR, 12 * BAR))
        a.drag(cx(5), 112, cx(2) - cx(5), 0, STREAMS[1])
        want_cycle(a, label("Clips: its left end dragged 3 bars back", STREAMS[1]), (1, BAR, 12 * BAR))
        a.click(cx(7), 112)
        want_cycle(a, "Clips: a click on the brace turns Loop off (the brace kept)", (0, BAR, 12 * BAR))
        a.click(cx(7), 112)
        want_cycle(a, "Clips: a click on it again turns Loop on", (1, BAR, 12 * BAR))
        a.drag(cx(13), 112, cx(4) - cx(13), 0, STREAMS[0])
        want_cycle(a, "Clips: its right end dragged back to bar 4", (1, BAR, 3 * BAR))
        loops_over(a, "Clips: song playback", BAR, 3 * BAR)
    finally:
        a.quit()


def idle():
    """The long idle first (frames a quarter second apart after 5 still seconds), as a person's drag starts."""
    time.sleep(6.0)


def sub_step(a, what, x0, y0, dx, note, lo, hi, stream):
    """Snap off: a note dragged `dx` px (under a step) starts between lo and hi ticks, same pitch, one undo step."""
    a.send("selnotes none")
    a.send("pianosnap None")
    a.frames(2)
    u0 = a.undo_pos()
    idle()
    a.drag(x0, y0, dx, 0, stream)
    after = a.notes()
    u1 = a.undo_pos()
    got = [s for s, _, p in after if p == note[1] and lo <= s <= hi]
    name = label(what, stream)
    if got and (note[0], note[1]) not in set((s, p) for s, _, p in after) and u1 == u0 + 1:
        ok("%s: start %d -> %d" % (name, note[0], got[0]))
    else:
        fail("%s: want the note at %s between %d and %d, got %s, undo +%d" % (name, note, lo, hi, sorted(after), u1 - u0))
    if u1 > u0:
        a.send("undo")
        a.frames(6)
    a.send("pianosnap Line")
    a.frames(2)


def menubar_status(a, tag):
    t = a.ask("sysmenu status", r"sysmenu status [^\n]*\n")
    m = re.search(r"sysmenu status installed (\d) [^\n]* walks (\d+) deferred (\d+) focus kept (\d+)", t)
    if not m:
        fail("%s: no system menu status: %s" % (tag, t.strip()[-200:]))
        return
    installed, walks, deferred, kept = (int(v) for v in m.groups())
    print("%s: system menu installed %d, walks %d, deferred %d, focus put back %d" % (tag, installed, walks, deferred, kept))
    if not installed and sys.platform != "darwin":
        ok("%s: no system menu bar here (no global menu host), the drags ran with the idle pacing alone" % tag)
    elif not installed or walks < 2:
        fail("%s: the system menu bar was not filled (installed %d, walks %d)" % (tag, installed, walks))
    elif WALK_IN_DRAG and kept < 1:
        fail("%s: the menus' walk during the drags never asked for the focus (the case this run checks)" % tag)
    elif not WALK_IN_DRAG and deferred < 1:
        fail("%s: no walk was put off during a drag" % tag)
    else:
        ok("%s: system menu bar walked %d times (%d put off during drags, focus put back %d times)" % (tag, walks, deferred, kept))


WALK_IN_DRAG = MENUBAR and len(sys.argv) > 6 and sys.argv[6] == "walk"


def note_at(a, start, pitch, into):
    """Where the piano roll shows a note (`into` ticks from its start), in the script's coordinates."""
    t = a.ask("lane", r"lane frame \d+: [^\n]*\n")
    m = re.search(r"lane frame \d+: x0 ([-0-9.]+) pxPerTick ([0-9.]+) [^\n]* pitchY0 ([-0-9.]+) rowH ([0-9.]+)", t)
    x0, ppt, py0, row = (float(v) for v in m.groups())
    return x0 + (start + into) * ppt, py0 + (127 - pitch) * row + row / 2 - a.yoff, ppt, row


def check_menubar():
    if WALK_IN_DRAG:
        os.environ["GRIDLINE_SYSMENU_WALK_IN_DRAG"] = "1"
    streams = [STREAMS[0], STREAMS[3]]
    a = App("menubar_smooth%s" % ("_walk" if WALK_IN_DRAG else ""), "logic", DEMO)
    try:
        for st in streams:
            idle()
            clip_drag(a, "Smooth menu bar, from the long idle: a region 3 tracks down and 4 bars on", 850, 447, 264, 108, 161, (10, 4 * BAR), st)
        a.dclick(850, 447)
        nx, ny, ppt, row = note_at(a, 432, 68, 240)
        for st in streams:
            idle()
            note_drag(a, "Smooth menu bar, from the long idle: a note 7 rows up and 3 bars on", nx, ny, 3 * BAR * ppt, -7 * row,
                      {(432, 68): (432 + 3 * BAR, 75)}, st)
        # half a step's pixels (48 ticks): the note goes 48 ticks on, give or take a pixel
        sub_step(a, "Smooth menu bar, snap off: a note half a step on", nx, ny, 48 * ppt, (432, 68), 432 + 40, 432 + 56, STREAMS[0])
        menubar_status(a, "Smooth")
    finally:
        a.quit()
    a = App("menubar_clips%s" % ("_walk" if WALK_IN_DRAG else ""), "live", DEMO_ARR)
    try:
        for st in streams:
            idle()
            clip_drag(a, "Clips menu bar, from the long idle: a MIDI clip by its title bar 3 tracks up and 4 bars on", 400, 465, 260, -138, 161, (4, 4 * BAR), st)
        # its notes in the clip band as it opens (the band's own size and zoom: G#4 and the bar after it in view)
        a.dclick(400, 465)
        nx, ny, ppt, row = note_at(a, 432, 68, 48)
        for st in streams:
            idle()
            note_drag(a, "Clips menu bar, from the long idle: a note 2 rows down and a bar on", nx, ny, BAR * ppt, 2 * row,
                      {(432, 68): (432 + BAR, 66)}, st)
        sub_step(a, "Clips menu bar, snap off: a note half a step on", nx, ny, 48 * ppt, (432, 68), 432 + 40, 432 + 56, STREAMS[0])
        menubar_status(a, "Clips")
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
    {"smooth": check_smooth, "clips": check_clips, "audio": check_audio, "loop": check_loop, "menubar": check_menubar}[GROUP]()
except RuntimeError as e:
    fail(str(e))
if failures:
    print("%d failure(s)" % len(failures))
    sys.exit(1)
print("all passed")
