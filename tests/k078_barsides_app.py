#!/usr/bin/env python3
# Gridline - 0.7.8: parts of the control bar dragged from one side of the bar to the other in the bar list
# (right-click an empty bit of a bar), in the real app (hidden, an isolated config folder and HOME, SDL's
# dummy audio driver), with real SDL pointer events sent while the app runs (--dev-input-live):
#   Studio and Clips: a part of the left side dragged onto the right side, and back. Each drag loses the keys
#   to another window a third of the way through, with the button still down (on macOS every window shown
#   takes them; a drag that stopped there was the bug of before); it must land all the same. Mid-drag the
#   list reports the line and the side it lands on (a picture is taken). The bar's model records the side,
#   keeps it after a restart, and the part dragged back stands where it was (the bar as designed again);
#   Reset gives the defaults.
#   The bar's alignment, at window widths 1024 to 2560 (at the scale given): the right group ends at the
#   bar's right edge, the left group starts at its left edge, the parts of each side are evenly spaced
#   (Studio: 8 px between panels, 16 beside the menus; Clips: its designed 4 px inside a cluster or the
#   12 px group gap, always 12 beside a moved part), and no part overlaps another; before the move, with
#   the part on the other side, and after it came back.
#   Smooth and the Tracker: their bars have no free sides (the list offers no other side; nothing moves).
#   python3 k078_barsides_app.py <gridline-app> <song> <out dir> [scale] [WxH]
import json
import math
import os
import re
import shutil
import subprocess
import sys
import time

APP, SONG, OUT = sys.argv[1], sys.argv[2], sys.argv[3]
SCALE = float(sys.argv[4]) if len(sys.argv) > 4 else 1.0
W0, H0 = (int(v) for v in (sys.argv[5] if len(sys.argv) > 5 else "1440x900").split("x"))
failures = []


def fail(msg):
    failures.append(msg)
    print("FAIL: " + msg)
    sys.stdout.flush()


def ok(msg):
    print("ok - " + msg)
    sys.stdout.flush()


class App:
    """A hidden gridline-app driven while it runs, through --dev-input-live."""

    def __init__(self, tag, layout, cfg, fresh):
        self.work = os.path.join(OUT, tag)
        shutil.rmtree(self.work, ignore_errors=True)
        os.makedirs(self.work)
        if fresh:
            shutil.rmtree(cfg, ignore_errors=True)
            os.makedirs(os.path.join(cfg, "gridline"))
            os.makedirs(os.path.join(cfg, "home"))
            with open(os.path.join(cfg, "gridline", "settings.json"), "w") as f:
                json.dump({"format": "gridline-settings", "values": {"ui.layoutChosen": True, "ui.layout": layout, "keys.profile": layout,
                                                                     "ui.menuStyle": layout}}, f)
        self.live = os.path.join(self.work, "live.txt")
        start = os.path.join(self.work, "start.txt")
        with open(start, "w") as f:
            f.write("4 typing off\n5 toasts clear\n")
        env = dict(os.environ)
        env.update(SDL_AUDIODRIVER="dummy", GRIDLINE_NO_PLUGIN_SCAN="1", GRIDLINE_HIDE_PLUGINS="all", GRIDLINE_CONFIG_HOME=cfg,
                   HOME=os.path.join(cfg, "home"))
        args = [APP, SONG, "--hidden", "--no-ui-sounds", "--no-update-check", "--size", self.size(W0, H0), "--dev-input", start,
                "--dev-input-live", self.live]
        if SCALE != 1.0:
            args += ["--ui-scale", "%g" % SCALE]
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

    @staticmethod
    def size(w, h):  # logical px to the window's
        return "%dx%d" % (round(w * SCALE), round(h * SCALE))

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

    def pt(self, x, y):  # logical px to window px
        return "%d %d" % (round(x * SCALE), round(y * SCALE))

    def move(self, x, y, settle=3):
        self.send("sdlmove " + self.pt(x, y))
        self.frames(settle)

    def resize(self, w, h):
        self.send("sdlwin size %d %d" % (round(w * SCALE), round(h * SCALE)))
        self.frames(8)

    def shot(self, name):
        self.send('shot "%s"' % os.path.join(OUT, name + ".ppm"))
        self.frames(3)

    def dump(self, layout):
        t = self.ask("toolbar dump", r"TOOLBAR %s own [01]/[01] parts [^\n]* pins[^\n]*\n" % layout)
        m = re.search(r"TOOLBAR %s own [01]/[01] parts ([^\n]*) pins(?: \[[^\n]*)?\n" % layout, t)
        return m.group(1).split()

    def rows(self):
        t = self.ask("toolbar rows", r"BARLIST open \d")
        # (the answer is printed in one go: wait a frame and read it whole)
        heads, rows = {}, []
        for m in re.finditer(r"BARLIST head (\S+) (-?\d+) (-?\d+) (-?\d+) (-?\d+)", t):
            heads[m.group(1)] = tuple(int(v) for v in m.groups()[1:])
        for m in re.finditer(r"BARLIST row (\S+) (\S+) (-?\d+) (-?\d+) (-?\d+) (-?\d+)", t):
            rows.append((m.group(1), m.group(2)) + tuple(int(v) for v in m.groups()[2:]))
        line = re.search(r"BARLIST drag ([^\n]*)", t)
        return re.search(r"BARLIST open (\d)", t).group(1) == "1", heads, rows, line.group(1) if line else None

    def parts(self):
        t = self.ask("toolbar parts", r"BARPARTS \S+ frame -?\d+ edges [-\d.]+ [-\d.]+ n \d+\n")
        m = re.search(r"BARPARTS \S+ frame -?\d+ edges ([-\d.]+) ([-\d.]+) n (\d+)", t)
        n = int(m.group(3))
        got = re.findall(r"BARPART (\S+) (\S+) ([-\d.]+) ([-\d.]+)", t[m.end():])[:n]
        return float(m.group(1)), float(m.group(2)), [(s, i, float(a), float(b)) for s, i, a, b in got]

    def drag(self, x0, y0, x1, y1, mid_shot=None, secs=1.2, rate=120, aim=None):
        # aim: (section, id, dy): the target is that row's top + dy (bottom + dy when dy < 0), read again at the end
        """A person's drag: press, hold, motion over real time with the keys lost a third of the way, a
        stop at about two thirds (the list's line read, a picture), on to the end, release."""
        self.move(x0, y0, 3)
        self.send("sdldown 0")
        time.sleep(0.15)
        self.frames(1)
        n = max(6, int(secs * rate))
        t0 = time.time()
        pending = []
        line = None
        for i in range(1, n + 1):
            f = i / n
            e = 0.5 - 0.5 * math.cos(math.pi * f)
            pending.append("sdlmove " + self.pt(x0 + (x1 - x0) * e, y0 + (y1 - y0) * e))
            if i == n // 3:
                pending.append("focuslost")
            wait = t0 + secs * f - time.time()
            if wait > 0.004 or i == n or i == (2 * n) // 3:
                self.send(*pending)
                pending = []
                wait = t0 + secs * f - time.time()
                if wait > 0:
                    time.sleep(wait)
            if i == (2 * n) // 3:
                self.frames(2)
                line = self.rows()[3]
                if mid_shot:
                    self.shot(mid_shot)
        time.sleep(0.1)
        self.frames(2)
        if aim:  # a short list scrolls while the pointer is near its edge: aim again at the row where it is now
            for _ in range(4):
                rows = self.rows()[2]
                r = [q for q in rows if q[0] == aim[0] and q[1] == aim[1]]
                if not r:
                    break
                y = r[0][3] + aim[2] if aim[2] >= 0 else r[0][5] + aim[2]
                if abs(y - y1) < 1:
                    break
                y1 = y
                self.move(x1, y1, 3)
        end_line = self.rows()[3]
        self.send("sdlup 0")
        self.frames(8)
        return line, end_line

    def quit(self):
        try:
            self.send("quit discard", timeout=10)
            self.proc.wait(timeout=30)
        except Exception:
            self.proc.terminate()
            self.proc.wait(timeout=10)
        self.out.close()


def strip(parts):
    return [re.sub(r"[-!]+$", "", p) for p in parts]


# ---- alignment ----
WIDTHS = [1024, 1280, 1440, 1920, 2560]


def check_align(a, layout, what):
    bad = []
    for w in WIDTHS:
        a.resize(w, H0)
        L, R, got = a.parts()
        sides = {}
        for s, i, x0, x1 in got:
            sides.setdefault(s, []).append((x0, x1, i))
        for s in sides:
            sides[s].sort()
        left, right = sides.get("left", []), sides.get("right", [])
        if not left:
            bad.append("%d: the left side drew nothing (%s)" % (w, got))
            continue
        right = right or [(R, R, "")]  # (nothing on the right: the edge alone)
        if right and abs(right[-1][1] - R) > 0.6:  # (a narrow window may have sent the whole right side to More)
            bad.append("%d: the right group ends at %.1f, not the edge %.1f" % (w, right[-1][1], R))
        if layout == "studio":
            first = left[0]
            if abs(first[0] - L) > 0.6 and abs(first[0] - (L + 8)) > 0.6:
                bad.append("%d: the left group starts at %.1f (edge %.1f)" % (w, first[0], L))
            for grp in (left, right):
                for (a0, a1, ai), (b0, b1, bi) in zip(grp, grp[1:]):
                    want = 16 if "menus" in (ai, bi) else 8
                    if abs((b0 - a1) - want) > 0.6:
                        bad.append("%d: %s to %s: %.1f px (want %d)" % (w, ai, bi, b0 - a1, want))
        else:
            if abs(left[0][0] - L) > 0.6:
                bad.append("%d: the left group starts at %.1f (edge %.1f)" % (w, left[0][0], L))
            moved = set(MOVED_CLIPS)
            for s in sides:
                grp = sides[s]
                for (a0, a1, ai), (b0, b1, bi) in zip(grp, grp[1:]):
                    g = b0 - a1
                    if ((ai in moved) != (bi in moved)) and abs(g - 12) > 0.6:
                        bad.append("%d: %s beside the moved %s: %.1f px (want 12)" % (w, ai, bi, g))
                    elif min(abs(g - 4), abs(g - 12)) > 0.6:
                        bad.append("%d: %s to %s: %.1f px (want 4 or 12)" % (w, ai, bi, g))
        spans = sorted((x0, x1, i) for s in sides for x0, x1, i in sides[s])
        if spans and spans[-1][1] > R + 0.6:
            bad.append("%d: %s ends at %.1f, past the right edge %.1f" % (w, spans[-1][2], spans[-1][1], R))
        for (a0, a1, ai), (b0, b1, bi) in zip(spans, spans[1:]):
            if b0 < a1 - 0.5:
                bad.append("%d: %s overlaps %s" % (w, ai, bi))
    a.resize(W0, H0)
    if bad:
        fail("%s: alignment %s: %s" % (layout, what, "; ".join(bad[:6])))
    else:
        ok("%s: alignment %s at %s wide (%gx)" % (layout, what, ", ".join(str(w) for w in WIDTHS), SCALE))


MOVED_CLIPS = []
TAG = "%gx_%dx%d" % (SCALE, W0, H0)


def open_list(a):
    a.move(W0 // 2, 20, 2)
    a.send("toolbar list")
    a.frames(6)
    is_open, heads, rows, _ = a.rows()
    if not is_open:
        raise RuntimeError("the bar list did not open")
    return heads, rows


def into_view(a, section, pid):
    """A small window shows part of the list: wheel it until the row is well inside (as a person would)."""
    def settled():  # (a wheel may scroll smoothly over a few frames)
        rows = a.rows()[2]
        for _ in range(20):
            a.frames(3)
            again = a.rows()[2]
            if again == rows:
                break
            rows = again
        return rows
    for _ in range(12):
        rows = settled()
        r = [q for q in rows if q[0] == section and q[1] == pid]
        if not r or r[0][5] < H0 * 0.6:
            return rows
        a.move(r[0][2] + 120, H0 * 0.4, 2)
        a.send("wheel -1")
        a.frames(4)
    return settled()


def check_layout(layout, part, before_right):
    """Drags `part` (left side) onto the right side, before the right side's row `before_right`, then back."""
    cfg = os.path.join(OUT, "cfg_" + layout)
    a = App(layout + "_1", layout, cfg, True)
    try:
        defaults = a.dump(layout)
        if layout == "live":
            MOVED_CLIPS[:] = []
        check_align(a, layout, "as designed")
        a.shot("%s_before_%s" % (layout, TAG))
        heads, rows = open_list(a)
        if "right" not in heads or "left" not in heads:
            fail("%s: the list heads its sides (%s)" % (layout, sorted(heads)))
            return
        src = [r for r in rows if r[1] == part]
        dst = [r for r in rows if r[1] == before_right and r[0] == "right"]
        if not src or not dst:
            fail("%s: the rows of %s and %s are listed" % (layout, part, before_right))
            return
        s, d = src[0], dst[0]
        x = s[2] + 70
        mid, end = a.drag(x, (s[3] + s[5]) // 2, x, d[3] + 4, mid_shot="%s_middrag_%s" % (layout, TAG), aim=("right", before_right, 4))
        if not end or not re.match(r"%s \S+ \S+ -?\d+ right$" % part, end):
            fail("%s: while held over the right side the line says it lands there (%s)" % (layout, end))
        else:
            ok("%s: the insertion line shows the right side, through the lost keys (%s)" % (layout, end))
        got = a.dump(layout)
        want_tok = part + "@right"
        right_ids = [p for p in strip(got) if "@" not in p or p.endswith("@right")]
        if want_tok not in strip(got):
            fail("%s: %s dropped on the right side records the side (%s)" % (layout, part, " ".join(got)))
        else:
            order = [p.split("@")[0] for p in strip(got)]
            k = order.index(part)
            if order[k + 1] != before_right:
                fail("%s: %s lands right before %s (%s)" % (layout, part, before_right, " ".join(got)))
            else:
                ok("%s: %s landed on the right side before %s" % (layout, part, before_right))
        del right_ids
        a.send("sdlkey escape down", "sdlkey escape up")
        a.frames(6)
        a.move(W0 // 2, H0 // 2, 4)
        if layout == "live":
            MOVED_CLIPS[:] = [part]
        check_align(a, layout, "with %s on the right side" % part)
        a.shot("%s_after_%s" % (layout, TAG))
    finally:
        a.quit()
    # A restart keeps it; then dragged back to where it was: the bar as designed.
    a = App(layout + "_2", layout, cfg, False)
    try:
        got = a.dump(layout)
        if part + "@right" not in strip(got):
            fail("%s: the side is kept after a restart (%s)" % (layout, " ".join(got)))
        else:
            ok("%s: the side is kept after a restart" % layout)
        heads, rows = open_list(a)
        rows = into_view(a, "right", part)
        src = [r for r in rows if r[1] == part and r[0] == "right"]
        # Where it was: right after its old left neighbour (or before the first left row).
        k = strip(defaults).index(part)
        prev = [p for p in strip(defaults)[:k] if any(r[1] == p and r[0] == "left" for r in rows)]
        if not src:
            fail("%s: %s is listed on the right side after the restart" % (layout, part))
            return
        if prev:
            pr = [r for r in rows if r[1] == prev[-1]][0]
            ty = pr[5] - 4  # the lower half of the row before: after it
        else:
            pr = [r for r in rows if r[0] == "left"][0]
            ty = pr[3] + 4  # the upper half of the first row: before it
        s = src[0]
        x = s[2] + 70
        _, end = a.drag(x, (s[3] + s[5]) // 2, x, ty, aim=(pr[0], pr[1], -4 if prev else 4))
        if not end or not end.endswith(" left"):
            fail("%s: dragged back, the line shows the left side (%s)" % (layout, end))
        back = a.dump(layout)
        if back != defaults:
            fail("%s: dragged back to its place, the bar is as designed (%s | %s)" % (layout, " ".join(back), " ".join(defaults)))
        else:
            ok("%s: dragged back to the left side, the bar is as designed again" % layout)
        a.send("sdlkey escape down", "sdlkey escape up")
        a.frames(6)
        if layout == "live":
            MOVED_CLIPS[:] = []
        check_align(a, layout, "after it came back")
        # The other way: every part of the right side moved onto the left (the right side left empty).
        rights = [r[1] for r in rows if r[0] == "right" and r[1] != part]
        for rp in rights:
            a.send("toolbar into %s left" % rp)
            a.frames(2)
        if layout == "live":
            MOVED_CLIPS[:] = rights
        check_align(a, layout, "with the right side's parts on the left (%s)" % " ".join(rights))
        a.send("toolbar reset")
        a.frames(4)
        if layout == "live":
            MOVED_CLIPS[:] = []
        # Reset after a move: the defaults.
        a.send("toolbar into %s right" % part)
        a.frames(4)
        moved = a.dump(layout)
        a.send("toolbar reset")
        a.frames(4)
        reset = a.dump(layout)
        if part + "@right" not in strip(moved) or reset != defaults:
            fail("%s: Reset puts a moved part back (%s | %s)" % (layout, " ".join(moved), " ".join(reset)))
        else:
            ok("%s: Reset puts a moved part back on its side" % layout)
    finally:
        a.quit()


def check_fixed(layout):
    a = App(layout + "_fixed", layout, os.path.join(OUT, "cfg_" + layout), True)
    try:
        before = a.dump(layout)
        heads, rows = open_list(a)
        switches = [r for r in rows if r[0] == "switches"]
        if switches:  # Smooth: its switches only move among themselves; a drag far below them stays in their group
            s = switches[0]
            far = max(r[5] for r in rows) + 120
            _, end = a.drag(s[2] + 70, (s[3] + s[5]) // 2, s[2] + 70, far)
            if end and not end.endswith(" " + s[0]):
                fail("%s: a part stays in its group (%s)" % (layout, end))
        after = a.dump(layout)
        if any("@" in p for p in after):
            fail("%s: no part names another side (%s)" % (layout, " ".join(after)))
        else:
            ok("%s: no free sides; parts keep their groups (%s rows)" % (layout, len(rows)))
        del before
    finally:
        a.quit()


os.makedirs(OUT, exist_ok=True)
try:
    check_layout("studio", "pattern", "monitor")
    check_layout("live", "metronome", "draw")
    if SCALE == 1.0 and (W0, H0) == (1440, 900):
        check_fixed("logic")
        check_fixed("tracker")
except RuntimeError as e:
    fail(str(e))
if failures:
    print("%d failure(s)" % len(failures))
    sys.exit(1)
print("all passed")
