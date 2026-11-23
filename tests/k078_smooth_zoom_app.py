#!/usr/bin/env python3
# Gridline - 0.7.8: Smooth's time zoom sliders (the tracks area's, at the right end of its bar, and the piano
# roll's in the lower pane) dragged the way a person drags, in the real app (hidden, isolated config folder and
# HOME, SDL's dummy audio driver, the system menu bar filled and the real app's idle pacing on, each drag from
# the long idle). The user (rc6, macOS): dragging the slider's line made the zoom jump around. The slider asked
# for its zoom as a ratio of the zoom drawn that frame, which still glided towards the last one, and the ratio
# went onto the glide's target: every frame of the drag added the glide's lag again, so the zoom ran past the
# pointer and swung back. Then (the user: "the zoom slider shouldn't be longer for finer control, just make its
# control finer"): a drag moves the zoom relative to the press, the whole range over 250 px of pointer travel
# (Shift: a quarter of that), with no glide behind it; a click on the line still jumps there.
# Each slider: the knob dragged in (right, zoom in) by 60 px, released, then pressed again where it sits and
# dragged back out by 60 px, then the same with Shift held by 60 px (in first: zoomed out from where it opens, a view meets its start or its end and
# holds that edge instead of the point); with a hand's stream (1.5 s, 120 motions a second) and a trackpad's (2 s, 250 a
# second). From the drag log (GRIDLINE_DRAGLOG) of every frame:
#   the zoom changes by ln(range) / 250 per px of pointer travel (a quarter with Shift), within 25%;
#   the drawn zoom moves one way only through each drag and after it (in: never out, back: never in);
#   no frame changes it by more than kMaxStep (natural log), so it never jumps;
#   the point held (the song position in the tracks area, the middle of the notes in the piano roll) stays
#   within 1 px of where it was;
#   after the release it settles on the zoom the slider showed at the release and stays there;
#   pressing the knob where it sits does not change the zoom; in and back comes back to the zoom it began at.
# Then a click on the line away from the knob jumps the zoom to that place, and a wheel notch over the slider
# steps it by 4 px worth of travel.
#   python3 k078_smooth_zoom_app.py <gridline-app> <demo song> <out dir>
import json
import math
import os
import re
import shutil
import subprocess
import sys
import time

APP, DEMO, OUT = sys.argv[1], sys.argv[2], sys.argv[3]
failures = []
kMaxStep = 0.15  # the largest change of ln(zoom) one frame may draw
kTravel = 250.0  # px of pointer travel over the whole zoom range
STREAMS = [("hand", 1.5, 120), ("trackpad", 2.0, 250)]


def fail(msg):
    failures.append(msg)
    print("FAIL: " + msg)
    sys.stdout.flush()


def ok(msg):
    print("ok - " + msg)
    sys.stdout.flush()


class App:
    def __init__(self, tag):
        self.work = os.path.join(OUT, tag)
        shutil.rmtree(self.work, ignore_errors=True)
        cfg = os.path.join(self.work, "cfg")
        os.makedirs(os.path.join(cfg, "gridline"))
        os.makedirs(os.path.join(cfg, "home"))
        with open(os.path.join(cfg, "gridline", "settings.json"), "w") as f:
            json.dump({"format": "gridline-settings", "values": {"ui.layoutChosen": True, "ui.layout": "logic", "keys.profile": "logic",
                                                                 "ui.menuStyle": "logic"}}, f)
        self.live = os.path.join(self.work, "live.txt")
        start = os.path.join(self.work, "start.txt")
        with open(start, "w") as f:
            f.write("4 typing off\n")
        self.dlog = os.path.join(self.work, "drag.log")
        env = dict(os.environ)
        env.update(SDL_AUDIODRIVER="dummy", GRIDLINE_NO_PLUGIN_SCAN="1", GRIDLINE_HIDE_PLUGINS="all", GRIDLINE_CONFIG_HOME=cfg,
                   HOME=os.path.join(cfg, "home"), GRIDLINE_IDLE_PACING="1", GRIDLINE_DRAGLOG=self.dlog)
        args = [APP, DEMO, "--hidden", "--no-ui-sounds", "--no-update-check", "--size", "1440x900", "--dev-input", start,
                "--dev-input-live", self.live, "--system-menu"]
        self.outp = os.path.join(self.work, "out.log")
        self.out = open(self.outp, "w")
        self.proc = subprocess.Popen(args, env=env, stdout=self.out, stderr=subprocess.STDOUT)
        end = time.time() + 120
        while time.time() < end and "frame" not in self.text():
            if self.proc.poll() is not None:
                raise RuntimeError("gridline-app exited (%s)" % self.proc.returncode)
            self.send("hint")
            time.sleep(0.05)
        self.frames(20)
        pat = r'window "##topbar" pos [^\n]* size [0-9.]+x([0-9.]+)'
        self.yoff = float(re.search(pat, self.ask("windows", pat)).group(1)) - 88

    def text(self):
        with open(self.outp, errors="replace") as f:
            return f.read()

    def log(self):
        try:
            with open(self.dlog, errors="replace") as f:
                return f.read()
        except OSError:
            return ""

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

    def slider(self, which):
        t = self.ask("zoomsl", r"zoomsl frame \d+: piano [^\n]*\n")
        m = re.search(r"zoomsl frame (\d+): %s x0 ([-0-9.]+) x1 ([-0-9.]+) y ([-0-9.]+) drawn (-?\d+) px ([0-9.e-]+)" % which, t)
        frame, x0, x1, y, drawn, px = int(m.group(1)), float(m.group(2)), float(m.group(3)), float(m.group(4)), int(m.group(5)), float(m.group(6))
        return x0, x1, y, frame - drawn < 4, px

    def drag(self, x0, y, dx, stream, shift=False):
        """From the long idle: press, hold, many small motions over real time, release, settle."""
        _, secs, rate = stream
        time.sleep(6.0)
        self.move(x0, y, 3)
        if shift:
            self.send("sdlkey #225 down shift")
            self.frames(3)
        self.send("sdldown 0")
        time.sleep(0.15)
        self.frames(1)
        n = max(2, int(secs * rate))
        t0 = time.time()
        pending = []
        for i in range(1, n + 1):
            f = i / n
            e = 0.5 - 0.5 * math.cos(math.pi * f)
            pending.append("sdlmove %d %d" % (round(x0 + dx * e), round(y)))
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
        if shift:
            self.frames(2)
            self.send("sdlkey #225 up")
        time.sleep(0.4)
        self.frames(30)

    def quit(self):
        try:
            self.send("quit discard", timeout=10)
            self.proc.wait(timeout=30)
        except Exception:
            self.proc.terminate()
            self.proc.wait(timeout=10)
        self.out.close()


def zoom_frames(chunk, tag):
    """[(frame, mouse down, px, scroll, anchor px, target)] of the drag log's frames that noted the view's zoom."""
    rows = []
    cur = None
    for ln in chunk.split("\n"):
        m = re.match(r"frame (\d+) t [0-9.]+ dt [0-9.]+ mouse ([-0-9.e+]+) [-0-9.e+]+ down (\d)", ln)
        if m:
            cur = (int(m.group(1)), m.group(3) == "1")
            continue
        m = re.match(r"  %s px ([0-9.e-]+) scroll ([-0-9.e]+) keyanchor ([-0-9.e]+) tick [-0-9.e]+ target ([0-9.e-]+)" % tag, ln)
        if m and cur:
            rows.append((cur[0], cur[1], float(m.group(1)), float(m.group(2)), float(m.group(3)), float(m.group(4))))
    return rows


def check_drag(a, what, which, tag, dx, stream, shift=False):
    """One drag of the knob by dx px; returns the zoom before and after (None when the slider is not shown)."""
    x0, x1, y, shown, px0 = a.slider(which)
    if not shown:
        fail("%s: the slider is not drawn" % what)
        return None
    knob = x0 + (x1 - x0) * math.log(px0 / ZMIN[which]) / math.log(ZMAX[which] / ZMIN[which])
    n = len(a.log())
    a.drag(round(knob), y, dx, stream, shift)
    rows = zoom_frames(a.log()[n:], tag)
    name = "%s, %s drag by %d px%s" % (what, stream[0], dx, " with Shift" if shift else "")
    if len(rows) < 10:
        fail("%s: only %d frames of the zoom in the drag log" % (name, len(rows)))
        return None
    sign = 1 if dx > 0 else -1
    worst_back, worst_step, worst_anchor = 0.0, 0.0, 0.0
    first = rows[0]
    tick0 = (first[4] + first[3]) / first[2]
    for p, q in zip(rows, rows[1:]):
        d = math.log(q[2] / p[2])
        worst_step = max(worst_step, abs(d))
        worst_back = max(worst_back, -sign * d)
    for r in rows:
        if r[3] > 0.5:  # (a view scrolled to its start cannot hold a point left of it)
            worst_anchor = max(worst_anchor, abs(tick0 * r[2] - r[3] - first[4]))
    # the press on the knob changes nothing
    press = [r for r in rows if r[1]][:3]
    pressed_jump = max((abs(math.log(r[2] / px0)) for r in press), default=0.0)
    release_target = [r for r in rows if r[1]][-1][5]
    settled = rows[-1][2]
    _, _, _, _, px1 = a.slider(which)
    a.frames(20)
    _, _, _, _, px2 = a.slider(which)
    good = True
    if worst_back > 1e-5:
        fail("%s: the zoom went back against the pointer by %.4f (ln)" % (name, worst_back))
        good = False
    if worst_step > kMaxStep:
        fail("%s: one frame jumped the zoom by %.3f (ln, at most %.2f)" % (name, worst_step, kMaxStep))
        good = False
    if worst_anchor > 1.0:
        fail("%s: the held point moved %.2f px" % (name, worst_anchor))
        good = False
    if pressed_jump > 1e-4:
        fail("%s: the press on the knob changed the zoom (%.4f ln)" % (name, pressed_jump))
        good = False
    if abs(math.log(px1 / release_target)) > 1e-3 or px2 != px1:
        fail("%s: after the release the zoom is %.7f then %.7f, the slider showed %.7f (settled %.7f)" % (name, px1, px2, release_target, settled))
        good = False
    want = math.log(ZMAX[which] / ZMIN[which]) / kTravel * (0.25 if shift else 1.0)
    gain = math.log(px1 / px0) / dx
    at_end = min(abs(math.log(px1 / ZMIN[which])), abs(math.log(px1 / ZMAX[which]))) < 1e-3
    if not at_end and abs(gain / want - 1) > 0.25:
        fail("%s: the zoom changed %.4f (ln) a px, expected %.4f (%.7f -> %.7f)" % (name, gain, want, px0, px1))
        good = False
    if good:
        ok("%s: %.6f -> %.6f px a tick over %d frames, %.4f (ln) a px (expected %.4f), one way, largest step %.3f, held point within %.2f px, kept after release" %
           (name, px0, px1, len(rows), gain, want, worst_step, worst_anchor))
    return px0, px1


ZMIN, ZMAX = {}, {}


def click_and_wheel(a, what, which):
    """A click on the line away from the knob jumps there; a wheel notch over the slider steps finely."""
    x0, x1, y, shown, px0 = a.slider(which)
    lr = math.log(ZMAX[which] / ZMIN[which])
    f0 = math.log(px0 / ZMIN[which]) / lr
    f = 0.25 if f0 > 0.5 else 0.75
    time.sleep(1.0)
    a.move(x0 + (x1 - x0) * f, y, 3)
    a.send("sdldown 0")
    a.frames(3)
    a.send("sdlup 0")
    a.frames(10)
    _, _, _, _, px1 = a.slider(which)
    got = math.log(px1 / ZMIN[which]) / lr
    if abs(got - f) * (x1 - x0) > 1.0:
        fail("%s: a click on the line at %.2f set the zoom at %.3f of the line" % (what, f, got))
    else:
        ok("%s: a click on the line at %.2f jumps the zoom there (%.6f -> %.6f)" % (what, f, px0, px1))
    step = 4.0 / kTravel * lr
    for notch in (1, -1):
        _, _, _, _, pa = a.slider(which)
        a.send("wheel %d" % notch)
        a.frames(10)
        _, _, _, _, pb = a.slider(which)
        d = math.log(pb / pa)
        if abs(d - notch * step) > 0.25 * step:
            fail("%s: a wheel notch (%d) over the slider changed the zoom by %.4f (ln), expected %.4f" % (what, notch, d, notch * step))
        else:
            ok("%s: a wheel notch (%d) over the slider steps the zoom by %.4f (ln)" % (what, notch, d))


def out_and_back(a, what, which, tag):
    for st in STREAMS:
        r1 = check_drag(a, what, which, tag, 60, st)
        r2 = check_drag(a, what, which, tag, -60, st)
        if r1 and r2:
            if abs(math.log(r2[1] / r1[0])) > 1e-3:
                fail("%s, %s: in and back ends at %.7f, began at %.7f" % (what, st[0], r2[1], r1[0]))
            else:
                ok("%s, %s: in and back returns to %.7f" % (what, st[0], r1[0]))
    check_drag(a, what, which, tag, 60, STREAMS[0], shift=True)
    check_drag(a, what, which, tag, -60, STREAMS[0], shift=True)
    click_and_wheel(a, what, which)


a = App("smooth")
try:
    # the demo song's beat is 96 ticks: zoom from 1.5 to 16 * 96 px a beat
    for w in ("tracks", "piano"):
        ZMIN[w], ZMAX[w] = 1.5 / 96, 16.0
    a.send("seekbeat 32 seek")  # the song position at bar 9, in view: the point the tracks area's slider holds
    a.frames(10)
    out_and_back(a, "Smooth tracks area zoom slider", "tracks", "plzoom")
    a.send("sdlmove %d %d" % (850, 447 + a.yoff), "sdldown 0", "sdlup 0", "sdldown 0", "sdlup 0")  # the region's piano roll below
    a.frames(30)
    time.sleep(1.0)
    a.frames(10)
    out_and_back(a, "Smooth piano roll zoom slider", "piano", "przoom")
except RuntimeError as e:
    fail(str(e))
finally:
    a.quit()
if failures:
    print("%d failure(s)" % len(failures))
    sys.exit(1)
print("all passed")
