#!/usr/bin/env python3
"""Tests Windows plugins bridged with yabridge in Gridline through real X11 input.

Runs on a Linux machine with Wine, yabridge, a Wine prefix holding the plugins (setup_wine.sh) and
an X display. Every check ends as one line of <out>/results.jsonl: pass, fail or skip with a reason.
Nothing is retried: a case that fails sometimes shows up as unstable in summarize.py.

The probe plugin logs each event with client coordinates inside the Wine prefix, so a click is
compared with where it landed, to the pixel. Gridline is driven through --dev-input-live; the
mouse and keyboard go through XTEST (xdotool) to real mapped windows.
"""
import argparse
import json
import os
import re
import signal
import subprocess
import sys
import threading
import time
import wave
from pathlib import Path

CELL_SECONDS = 600  # a cell that takes longer is a hang: it is killed and reported
PROBE_ENTRIES = {"vst3": Path.home() / ".vst3/yabridge/GridlineProbe.vst3", "clap": Path.home() / ".clap/yabridge/GridlineProbe.clap", "vst2": Path.home() / ".vst/yabridge/GridlineProbeSynth.so"}


def sh(*cmd, timeout=30, check=False, env=None):
    """Runs a command; returns (exit code, stdout + stderr). A timeout is exit code 124."""
    try:
        r = subprocess.run([str(c) for c in cmd], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout, env=env, errors="replace", text=True)
        if check and r.returncode != 0:
            raise RuntimeError(f"{cmd[0]} failed ({r.returncode}): {r.stdout[-500:]}")
        return r.returncode, r.stdout
    except subprocess.TimeoutExpired as e:
        return 124, (e.stdout or b"").decode("utf-8", "replace") if isinstance(e.stdout, bytes) else (e.stdout or "")
    except FileNotFoundError as e:
        return 127, str(e)


class Results:
    def __init__(self, out, base):
        self.path = Path(out) / "results.jsonl"
        self.base = base
        self.cell = {}
        self.lock = threading.Lock()

    def add(self, check, status, detail=""):
        row = dict(self.base, **self.cell, check=check, status=status, detail=str(detail)[:600])
        with self.lock, self.path.open("a") as f:
            f.write(json.dumps(row) + "\n")
        print(f"[{status:4}] {self.cell.get('format', '')}/{self.cell.get('mode', '')}/{self.cell.get('plugin', '')} {check}: {str(detail)[:200]}", flush=True)

    def expect(self, check, ok, detail=""):
        self.add(check, "pass" if ok else "fail", detail)
        return ok


class Probe:
    """The log the probe plugin writes inside the Wine prefix."""

    def __init__(self, prefix, archive):
        self.path = Path(prefix) / "drive_c/gridline-probe/log.txt"
        self.archive = archive

    def clear(self):
        """Starts a new case; what was logged so far stays in the artifacts."""
        text = self.text()
        if text:
            with open(self.archive, "a") as f:
                f.write(text + "\n---\n")
        try:
            self.path.unlink()
        except OSError:
            pass

    def size(self):
        try:
            return self.path.stat().st_size
        except OSError:
            return 0

    def text(self, since=0):
        try:
            with self.path.open("rb") as f:
                f.seek(since)
                return f.read().decode("ascii", "replace")
        except OSError:
            return ""

    def wait(self, pattern, since=0, timeout=4.0):
        """The first match of `pattern` after offset `since`, waiting for it; None when it never comes."""
        end = time.time() + timeout
        rx = re.compile(pattern, re.M)
        while True:
            m = rx.search(self.text(since))
            if m:
                return m
            if time.time() > end:
                return None
            time.sleep(0.05)


class X:
    """The display, seen from outside: window tree, geometry, screenshots, XTEST input."""

    def __init__(self, sync=True):
        self.sync = sync

    def geom(self, win):
        rc, o = sh("xwininfo", "-id", win, timeout=10)
        if rc != 0:
            return None
        get = lambda k: int(re.search(k + r":\s+(-?\d+)", o).group(1))
        return {"x": get("Absolute upper-left X"), "y": get("Absolute upper-left Y"), "w": get("Width"), "h": get("Height"), "mapped": "IsViewable" in o}

    def children(self, win):
        rc, o = sh("xwininfo", "-tree", "-id", win, timeout=10)
        return re.findall(r"^\s+(0x[0-9a-f]+) ", o, re.M) if rc == 0 else []

    def dump(self, win, path):
        with open(path, "w") as f:
            for args in (["xwininfo", "-tree", "-id", win], ["xwininfo", "-events", "-id", win], ["xprop", "-id", win], ["xwininfo", "-root", "-tree"]):
                f.write("$ " + " ".join(args) + "\n" + sh(*args, timeout=10)[1] + "\n")
            # The windows of the interface inside: their event masks are what the classic faults show in.
            for child in self.children(win)[:12]:
                f.write(f"$ xwininfo -events -id {child}\n" + sh("xwininfo", "-events", "-id", child, timeout=10)[1] + "\n")
            f.write("$ xdotool getwindowfocus\n" + sh("xdotool", "getwindowfocus", timeout=5)[1] + "\n")

    def shot(self, win, path):
        """The window with its children as a PNG; returns (width, height, RGB bytes) or None."""
        rc, _ = sh("import", "-silent", "-window", win, path, timeout=30)
        if rc != 0:
            return None
        ppm = str(path) + ".ppm"
        rc, _ = sh("convert", path, "-depth", "8", ppm, timeout=30)
        if rc != 0:
            return None
        data = Path(ppm).read_bytes()
        os.unlink(ppm)
        m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+(\d+)\s", data)
        return (int(m.group(1)), int(m.group(2)), data[m.end():]) if m else None

    def move(self, x, y):
        sh("xdotool", "mousemove", *(["--sync"] if self.sync else []), x, y, timeout=6)
        time.sleep(0.12)

    def click(self, button=1, repeat=1):
        if repeat > 1:
            sh("xdotool", "click", "--repeat", repeat, "--delay", 60, button, timeout=6)
        else:
            sh("xdotool", "click", button, timeout=6)

    def down(self, button=1):
        sh("xdotool", "mousedown", button, timeout=6)

    def up(self, button=1):
        sh("xdotool", "mouseup", button, timeout=6)

    def key(self, *keys):
        sh("xdotool", "key", "--delay", 40, *keys, timeout=10)

    def type(self, text):
        sh("xdotool", "type", "--delay", 40, text, timeout=15)

    def window_move(self, win, x, y):
        sh("xdotool", "windowmove", win, x, y, timeout=6)
        time.sleep(0.5)

    def window_size(self, win, w, h):
        sh("xdotool", "windowsize", win, w, h, timeout=6)
        time.sleep(0.8)


def pixel(img, x, y):
    w, h, rgb = img
    if not (0 <= x < w and 0 <= y < h):
        return None
    i = (y * w + x) * 3
    return tuple(rgb[i : i + 3])


def near(a, b, tol=24):
    return a is not None and all(abs(p - q) <= tol for p, q in zip(a, b))


def changed_pixels(a, b):
    if not a or not b or a[:2] != b[:2]:
        return -1
    return sum(1 for i in range(0, len(a[2]), 3) if a[2][i : i + 3] != b[2][i : i + 3])


class App:
    """A running gridline-app with a real window, driven through --dev-input-live."""

    def __init__(self, ctx, tag, sandbox_types=()):
        self.ctx = ctx
        self.live = ctx.work / f"live-{os.getpid()}"
        self.log_path = ctx.cell_dir / f"app-{tag}.log"
        self.log = open(self.log_path, "ab")
        args = [ctx.app, "--no-ui-sounds", "--size", "1100x700", "--dev-input-live", self.live]
        for t in sandbox_types:
            args += ["--dev-sandbox", t, "1"]
        env = dict(ctx.env, SDL_AUDIODRIVER="dummy", GRIDLINE_NO_PLUGIN_SCAN="1", GRIDLINE_NO_UPDATE_CHECK="1")
        self.proc = subprocess.Popen([str(a) for a in args], stdout=self.log, stderr=subprocess.STDOUT, env=env, start_new_session=True)
        self.seq = 0

    def alive(self):
        return self.proc.poll() is None

    def output(self, since=0):
        self.log.flush()
        with open(self.log_path, "rb") as f:
            f.seek(since)
            return f.read().decode("utf-8", "replace")

    def mark(self):
        return self.log_path.stat().st_size

    def send(self, *lines, timeout=30):
        """Hands commands to the next frame; False when the app did not take them (hang or exit)."""
        tmp = Path(str(self.live) + ".tmp")
        tmp.write_text("".join(f"0 {l}\n" for l in lines))
        tmp.rename(self.live)
        end = time.time() + timeout
        while self.live.exists():
            if not self.alive() or time.time() > end:
                return False
            time.sleep(0.03)
        return True

    def ask(self, command, pattern, timeout=30):
        """Sends a command and waits for its output; the match, or None."""
        m0 = self.mark()
        if not self.send(command, timeout=timeout):
            return None
        end = time.time() + timeout
        rx = re.compile(pattern, re.M)
        while time.time() < end:
            m = rx.search(self.output(m0))
            if m:
                return m
            if not self.alive():
                return None
            time.sleep(0.05)
        return None

    def windows(self):
        m = self.ask("plugwin info", r"plugwin frame \d+: (\d+) windows\n((?:  plugwin .*\n)*)", timeout=20)
        out = []
        if m:
            for w in re.finditer(r'plugwin "(.*)" format (\S+) external (\d) window (0x[0-9a-f]+) container (0x[0-9a-f]+) at (-?\d+),(-?\d+) size (\d+)x(\d+) plugin (\d+),(\d+) (\d+)x(\d+) resizable (\d)', m.group(2)):
                g = w.groups()
                out.append({"name": g[0], "format": g[1], "external": g[2] == "1", "window": g[3], "container": g[4], "x": int(g[5]), "y": int(g[6]), "w": int(g[7]), "h": int(g[8]),
                            "px": int(g[9]), "py": int(g[10]), "pw": int(g[11]), "ph": int(g[12]), "resizable": g[13] == "1"})
        return out

    def params(self, target="chan 0", set_values=None):
        """hostedparam: instance facts and values; None when the app did not answer."""
        cmd = f"hostedparam {target}" + (f" {set_values[0]} {set_values[1]}" if set_values else "")
        m = self.ask(cmd, r"hostedparam frame \d+: instance (\d) sandboxed (\d) crashed (\d) latency (-?\d+) open (\d) params (\d+)\n((?:  param \d+ \S+\n)*)", timeout=30)
        if not m:
            return None
        g = m.groups()
        return {"instance": g[0] == "1", "sandboxed": g[1] == "1", "crashed": g[2] == "1", "latency": int(g[3]), "open": g[4] == "1",
                "values": [float(v) for v in re.findall(r"param \d+ (\S+)", g[6])]}

    def wait_instance(self, target="chan 0", timeout=120):
        end = time.time() + timeout
        while time.time() < end and self.alive():
            p = self.params(target)
            if p and p["instance"]:
                return p
            time.sleep(0.5)
        return None

    def main_window(self):
        """The biggest mapped Gridline window that is not a plugin window."""
        rc, o = sh("xwininfo", "-root", "-tree", timeout=10)
        ids = re.findall(r'^\s+(0x[0-9a-f]+) .*\("[^"]*" "[^"]*[Gg]ridline[^"]*"\)', o, re.M)
        plugin = {int(w["window"], 16) for w in self.windows()}
        best, area = None, 0
        for win in ids:
            g = self.ctx.x.geom(win)
            if int(win, 16) not in plugin and g and g["mapped"] and g["w"] * g["h"] > area:
                best, area = win, g["w"] * g["h"]
        return best

    def quit(self, timeout=45):
        """The normal quit path. Returns (exited in time, exit code)."""
        self.send("quit discard", timeout=10)
        try:
            return True, self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.dump_threads()
            return False, None

    def dump_threads(self):
        """Where a hung process waits, from /proc, into the app's log."""
        lines = [f"--- still running after quit, pid {self.proc.pid}"]
        base = Path(f"/proc/{self.proc.pid}/task")
        try:
            for t in sorted(base.iterdir(), key=lambda q: int(q.name)):
                read = lambda n: (t / n).read_text(errors="replace").strip() if (t / n).exists() else "?"
                lines.append(f"thread {t.name} {read('comm')} wchan {read('wchan')} syscall {read('syscall')[:40]}")
        except OSError as e:
            lines.append(repr(e))
        lines.append(sh("ps", "-eo", "pid,ppid,stat,wchan:20,etimes,cmd", "--forest", timeout=10)[1][-3000:])
        self.log.write(("\n".join(lines) + "\n").encode())
        self.log.flush()

    def kill(self):
        if self.alive():
            try:
                os.killpg(self.proc.pid, signal.SIGKILL)
            except OSError:
                pass
            try:
                self.proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                pass
        self.log.close()


def base_project(ctx):
    d = json.loads(ctx.demo.read_text())
    master = dict(d["mixer"][0], slots=[], sends=[])
    track = {"id": d["nextId"], "name": "Test", "color": 5263440, "muted": False, "pan": 0, "sends": [], "slots": []}
    return dict(d, automations=[], channels=[], patterns=[], playlist=[], markers=[], links=[], modLinks=[], tracks=d["tracks"][:3], mixer=[master, track], nextId=d["nextId"] + 1000, tempo=120.0,
                info=dict(d["info"], title="yabridge test"))


def make_project(ctx, path, plugin_type=None, fx_type=None, notes=None, level=None, automate=None):
    """A small project: one channel (the plugin under test, or a built-in source for an effect)."""
    d = base_project(ctx)
    mixer_id = d["mixer"][1]["id"]
    chan = {"id": d["nextId"] - 900, "name": "Test", "color": 5263440, "gain": 0.8, "pan": 0, "pitch": 0, "mixerTrack": mixer_id, "muted": False,
            "plugin": {"type": plugin_type or "gl.analog", "params": []}}
    if level is not None and plugin_type:
        chan["plugin"]["params"] = [level, 0.25, 0.0]
    d["channels"] = [chan]
    if fx_type:
        d["mixer"][1]["slots"] = [{"enabled": True, "mix": 1, "plugin": {"type": fx_type, "params": [level if level is not None else 0.5, 0.25, 0.0]}}]
    if notes:
        pid = d["nextId"] - 800
        d["patterns"] = [{"id": pid, "name": "Test", "bars": 4, "color": 5263440, "notes": {str(chan["id"]): notes}}]
        d["playlist"] = [{"id": d["nextId"] - 700, "length": 1536, "pattern": pid, "start": 0, "track": 0}]
    if automate:
        aid = d["nextId"] - 600
        d["automations"] = [{"id": aid, "name": "Level", "color": 5263440, "owner": chan["id"], "index": 100 + automate["param"], "flCurves": True,
                             "points": [[t, v, 0, 0] for t, v in automate["points"]]}]
        d["playlist"].append({"id": d["nextId"] - 500, "kind": "automation", "source": aid, "length": 1536, "start": 0, "track": 1})
    Path(path).write_text(json.dumps(d))
    return path


def read_wav(path):
    with wave.open(str(path)) as w:
        rate, ch, width, n = w.getframerate(), w.getnchannels(), w.getsampwidth(), w.getnframes()
        raw = w.readframes(n)
    if width == 2:
        import array

        a = array.array("h")
        a.frombytes(raw)
        scale = 32768.0
        s = [a[i] / scale for i in range(0, len(a), ch)]
    elif width == 3:
        s = [int.from_bytes(raw[i : i + 3], "little", signed=True) / 8388608.0 for i in range(0, len(raw), 3 * ch)]
    else:
        import array

        a = array.array("i")
        a.frombytes(raw)
        s = [a[i] / 2147483648.0 for i in range(0, len(a), ch)]
    return rate, s


def rms(s):
    return (sum(x * x for x in s) / max(1, len(s))) ** 0.5


def frequency(s, rate):
    """Frequency from rising zero crossings of the loudest part."""
    idx = [i for i in range(1, len(s)) if s[i - 1] < 0 <= s[i]]
    if len(idx) < 3:
        return 0.0
    return rate * (len(idx) - 1) / (idx[-1] - idx[0])


class Ctx:
    pass


def host_processes():
    rc, o = sh("pgrep", "-fc", "yabridge-host", timeout=5)
    try:
        return int(o.strip() or 0)
    except ValueError:
        return 0


def kill_hosts():
    sh("pkill", "-9", "-f", "yabridge-host", timeout=5)
    time.sleep(0.5)


# ------------------------------------------------------------------------ mouse checks


def check_points(ctx, r, name, window, points, button=1, label="L"):
    """Clicks each client point; every press must land exactly there. Returns True when all did."""
    probe, x = ctx.probe, ctx.x
    misses = []
    for px, py in points:
        g = x.geom(window["container"])
        if not g:
            misses.append("no container")
            break
        x.move(g["x"] + px, g["y"] + py)
        m0 = probe.size()
        x.click(button)
        m = probe.wait(rf"(?:down) {label} (-?\d+) (-?\d+)", m0)
        if not m:
            misses.append(f"({px},{py}): nothing reached the plugin")
        elif (int(m.group(1)), int(m.group(2))) != (px, py):
            misses.append(f"({px},{py}): landed at ({m.group(1)},{m.group(2)}), offset {int(m.group(1)) - px},{int(m.group(2)) - py}")
        time.sleep(0.05)
    return r.expect(name, not misses, "; ".join(misses) if misses else f"{len(points)} clicks exact")


def corner_points(w, h):
    return [(3, 3), (w - 4, 3), (3, h - 4), (w - 4, h - 4), (w // 2, h // 2), (400, 300) if w > 420 and h > 320 else (w // 3, h // 3)]


def check_editor_shown(ctx, r, window, tag):
    """Mapped, right size, and drawn: the corners of the probe's pattern are where the size says."""
    x = ctx.x
    x.dump(window["window"], ctx.cell_dir / f"x-{tag}.txt")
    g = x.geom(window["container"])
    r.expect(f"container mapped ({tag})", bool(g and g["mapped"]), g)
    r.expect(f"container has the plugin's window ({tag})", bool(x.children(window["container"])), x.children(window["container"])[:6])
    time.sleep(1.0)
    img = x.shot(window["window"], str(ctx.cell_dir / f"shot-{tag}.png"))
    if not img:
        r.add(f"editor drawn ({tag})", "fail", "no screenshot")
        return None
    w, h, ox, oy = window["pw"], window["ph"], window["px"], window["py"]
    want = {"top left": (3, 3, (255, 0, 0)), "top right": (w - 4, 3, (0, 255, 0)), "bottom left": (3, h - 4, (0, 0, 255)), "bottom right": (w - 4, h - 4, (255, 255, 0))}
    bad = {k: pixel(img, ox + v[0], oy + v[1]) for k, v in want.items() if not near(pixel(img, ox + v[0], oy + v[1]), v[2])}
    r.expect(f"editor drawn, corners in place ({tag})", not bad, bad or f"{w}x{h}")
    return img


def check_mouse(ctx, r, window):
    probe, x = ctx.probe, ctx.x
    w, h = window["pw"], window["ph"]
    check_points(ctx, r, "left click at corners and centre", window, corner_points(w, h))
    check_points(ctx, r, "right click", window, [(100, 300), (500, 60)], button=3, label="R")
    check_points(ctx, r, "middle click", window, [(120, 320)], button=2, label="M")
    g = x.geom(window["container"])
    # Hover: motion without a button, then the hover message, then leaving.
    m0 = probe.size()
    x.move(g["x"] + 450, g["y"] + 250)
    m = probe.wait(r"move (-?\d+) (-?\d+)\s*$", m0)
    r.expect("hover: motion reaches the plugin at the right place", bool(m and (int(m.group(1)), int(m.group(2))) == (450, 250)), m.group(0) if m else "no motion")
    r.expect("hover: the hover message arrives", bool(probe.wait(r"hover 450 250", m0, timeout=2.5)))
    # Double click.
    x.move(g["x"] + 420, g["y"] + 210)
    m0 = probe.size()
    x.click(1, repeat=2)
    r.expect("double click", bool(probe.wait(r"dbl L 420 210", m0)), probe.text(m0)[-200:])
    # Wheel, both ways, over the slider (parameter 0 follows).
    x.move(g["x"] + 120, g["y"] + 140)
    m0 = probe.size()
    x.click(4)
    up = probe.wait(r"wheel (-?\d+) (-?\d+) (-?\d+)", m0)
    m1 = probe.size()
    x.click(5)
    down = probe.wait(r"wheel (-?\d+) (-?\d+) (-?\d+)", m1)
    r.expect("wheel up", bool(up and int(up.group(1)) > 0 and (int(up.group(2)), int(up.group(3))) == (120, 140)), up.group(0) if up else "nothing")
    r.expect("wheel down", bool(down and int(down.group(1)) < 0 and (int(down.group(2)), int(down.group(3))) == (120, 140)), down.group(0) if down else "nothing")
    # Press, drag, release: each step at its own place, the release where the pointer ended.
    x.move(g["x"] + 120, g["y"] + 200)
    m0 = probe.size()
    x.down(1)
    time.sleep(0.15)
    for dy in (-20, -40, -60):
        x.move(g["x"] + 120, g["y"] + 200 + dy)
    x.up(1)
    t = probe.text(m0)
    ok = "down L 120 200" in t and "move 120 180" in t and "move 120 160" in t and "move 120 140" in t and bool(probe.wait(r"up L 120 140", m0))
    r.expect("press, drag, release", ok, t[-240:])
    # Dragging outside the window keeps the capture: the release still arrives.
    x.move(g["x"] + 120, g["y"] + 200)
    m0 = probe.size()
    x.down(1)
    time.sleep(0.1)
    x.move(g["x"] + window["pw"] + 60, g["y"] + 20)
    x.up(1)
    r.expect("drag out of the window, release arrives", bool(probe.wait(r"up L -?\d+ -?\d+", m0)), probe.text(m0)[-160:])


def check_moved(ctx, r, window, app):
    """The classic fault: Wine works out clicks from where it thinks the toplevel is."""
    x = ctx.x
    win = window["window"]
    pts = [(3, 3), (window["pw"] - 4, window["ph"] - 4), (300, 200)]
    for i, (mx, my) in enumerate([(300, 250), (700, 90), (40, 420)]):
        x.window_move(win, mx, my)
        check_points(ctx, r, f"clicks after moving the plugin window ({i + 1})", window, pts)
    main = app.main_window()
    if main:
        x.window_move(main, 120, 60)
        check_points(ctx, r, "clicks after moving the main window", window, pts)
    else:
        r.add("clicks after moving the main window", "skip", "main window not found in the tree")
    # A window moved by the user's own drag through the window manager comes last in a session: the
    # manager's frame moves, the client gets only a synthetic ConfigureNotify.
    if ctx.has_wm_tools:
        sh("wmctrl", "-i", "-r", win, "-e", "0,200,200,-1,-1", timeout=5)
        time.sleep(0.6)
        check_points(ctx, r, "clicks after a window manager move", window, pts)
        if not ctx.weston:
            sh("xdotool", "windowminimize", win, timeout=5)
            time.sleep(0.8)
            sh("wmctrl", "-i", "-a", win, timeout=5)
            time.sleep(1.0)
            check_points(ctx, r, "clicks after minimise and restore", window, pts)
            sh("wmctrl", "-s", "1", timeout=5)
            time.sleep(0.6)
            sh("wmctrl", "-s", "0", timeout=5)
            time.sleep(0.8)
            sh("wmctrl", "-i", "-a", win, timeout=5)
            time.sleep(0.6)
            check_points(ctx, r, "clicks after changing workspace and back", window, pts)
        else:
            r.add("clicks after minimise and restore", "skip", "weston's window manager offers neither from the X side")
            r.add("clicks after changing workspace and back", "skip", "weston's window manager offers neither from the X side")
    else:
        for n in ("clicks after a window manager move", "clicks after minimise and restore", "clicks after changing workspace and back"):
            r.add(n, "skip", "no window manager on this display")
    # Covered by another window, then raised.
    cover = subprocess.Popen(["xmessage", "-geometry", "900x700+0+0", "cover"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=ctx.env)
    time.sleep(1.2)
    g = x.geom(window["container"])
    x.move(g["x"] + 300, g["y"] + 200)
    m0 = ctx.probe.size()
    x.click(1)
    covered = ctx.probe.wait(r"down L", m0, timeout=1.0) is not None
    cover.kill()
    cover.wait()
    time.sleep(0.8)
    sh("xdotool", "windowraise", win, timeout=5)
    time.sleep(0.6)
    r.add("covered window takes no click", "fail" if covered else "pass", "the click went through the cover" if covered else "")
    check_points(ctx, r, "clicks after being covered and raised", window, pts)


# ------------------------------------------------------------------------ keyboard


def check_keys(ctx, r, window, app):
    probe, x = ctx.probe, ctx.x
    g = x.geom(window["container"])
    x.move(g["x"] + 450, g["y"] + 300)
    m0 = probe.size()
    x.click(1)
    r.expect("a click gives the editor the keyboard", bool(probe.wait(r"focus in", m0, timeout=2.0)), probe.text(m0)[-160:])
    time.sleep(0.3)
    m0 = probe.size()
    x.type("abc")
    time.sleep(0.5)
    chars = re.findall(r"char (\d+)", probe.text(m0))
    r.expect("typed text reaches the editor", chars[:3] == ["97", "98", "99"], chars)
    m0 = probe.size()
    x.key("shift+a")
    time.sleep(0.4)
    t = probe.text(m0)
    r.expect("Shift reaches the editor", "char 65" in t, t[-200:])
    m0 = probe.size()
    x.key("ctrl+k")
    time.sleep(0.4)
    t = probe.text(m0)
    r.expect("Ctrl chord reaches the editor", bool(re.search(r"keydown 75 shift 0 ctrl 1", t)), t[-200:])
    m0 = probe.size()
    x.key("alt+k")
    time.sleep(0.4)
    t = probe.text(m0)
    r.expect("Alt chord reaches the editor", bool(re.search(r"keydown 75 shift 0 ctrl 0 alt 1", t)), t[-200:])
    # Space is Gridline's: with the editor focused the transport still toggles (KeyForward).
    before = app.ask("playstate", r"playstate frame \d+: playing (\d)")
    m0 = probe.size()
    x.key("space")
    time.sleep(0.7)
    after = app.ask("playstate", r"playstate frame \d+: playing (\d)")
    toggled = bool(before and after and before.group(1) != after.group(1))
    r.expect("Space in the editor toggles Gridline's transport", toggled, f"playing {before and before.group(1)} -> {after and after.group(1)}")
    r.add("Space also reached the editor (for information)", "pass" if "keydown 32" in probe.text(m0) else "skip", "the editor saw it" if "keydown 32" in probe.text(m0) else "the editor did not see it")
    if after and after.group(1) == "1":
        app.send("stopvia button")
    # Back to Gridline: click its main window, Space goes to the transport and not to the plugin.
    main = app.main_window()
    if not main:
        r.add("focus back to Gridline", "skip", "main window not found in the tree")
        return
    mg = x.geom(main)
    sh("xdotool", "windowraise", main, timeout=5)
    time.sleep(0.4)
    x.move(mg["x"] + mg["w"] // 2, mg["y"] + mg["h"] - 40)
    m0 = probe.size()
    x.click(1)
    r.expect("the editor loses the keyboard on a click in Gridline", bool(probe.wait(r"focus out", m0, timeout=2.0)), probe.text(m0)[-120:])
    time.sleep(0.4)
    before = app.ask("playstate", r"playstate frame \d+: playing (\d)")
    m0 = probe.size()
    x.key("space")
    time.sleep(0.7)
    after = app.ask("playstate", r"playstate frame \d+: playing (\d)")
    r.expect("transport shortcut works with Gridline focused", bool(before and after and before.group(1) != after.group(1)), f"{before and before.group(1)} -> {after and after.group(1)}")
    r.expect("the key did not go to the plugin", "keydown 32" not in probe.text(m0), probe.text(m0)[-120:])
    if after and after.group(1) == "1":
        app.send("stopvia button")
    sh("xdotool", "windowraise", window["window"], timeout=5)
    time.sleep(0.4)


# ------------------------------------------------------------------------ resizing


def fresh_window(app):
    ws = app.windows()
    return ws[0] if ws else None


def check_resize(ctx, r, window, app):
    probe, x = ctx.probe, ctx.x
    g = x.geom(window["container"])
    # The plugin asks: bigger.
    x.move(g["x"] + 290, g["y"] + 65)
    m0 = probe.size()
    x.click(1)
    m = probe.wait(r"request 800 500: (\w+)", m0)
    r.expect("plugin asks for a bigger size: accepted", bool(m and m.group(1) == "accepted"), m.group(0) if m else "no request logged")
    time.sleep(1.2)
    w = fresh_window(app) or window
    r.expect("window follows: 800 x 500", (w["pw"], w["ph"]) == (800, 500), f"{w['pw']}x{w['ph']}")
    g2 = x.geom(w["container"])
    r.expect("container follows on the display", bool(g2 and (g2["w"], g2["h"]) == (800, 500)), g2)
    check_editor_shown(ctx, r, w, "bigger")
    check_points(ctx, r, "clicks exact after growing", w, corner_points(w["pw"], w["ph"]))
    # The plugin asks: smaller.
    g2 = x.geom(w["container"])
    x.move(g2["x"] + 290, g2["y"] + 125)
    m0 = probe.size()
    x.click(1)
    m = probe.wait(r"request 500 320: (\w+)", m0)
    r.expect("plugin asks for a smaller size: accepted", bool(m and m.group(1) == "accepted"), m.group(0) if m else "no request logged")
    time.sleep(1.2)
    w = fresh_window(app) or w
    r.expect("window follows: 500 x 320", (w["pw"], w["ph"]) == (500, 320), f"{w['pw']}x{w['ph']}")
    check_points(ctx, r, "clicks exact after shrinking", w, corner_points(w["pw"], w["ph"]))
    # The host resizes: the plugin must be told.
    if w["resizable"]:
        m0 = probe.size()
        x.window_size(w["window"], 700, 500)
        time.sleep(1.0)
        w2 = fresh_window(app) or w
        m = probe.wait(r"size (\d+) (\d+)", m0, timeout=2.0)
        r.expect("resizing the window tells the plugin", bool(m and (int(m.group(1)), int(m.group(2))) == (w2["pw"], w2["ph"])), f"{m.group(0) if m else 'nothing'} vs {w2['pw']}x{w2['ph']}")
        check_points(ctx, r, "clicks exact after resizing the window", w2, corner_points(w2["pw"], w2["ph"]))
    else:
        r.add("resizing the window tells the plugin", "fail", "the window is not resizable although the plugin says it can")
    x.window_size((fresh_window(app) or w)["window"], 640, 400 + 40)


# ------------------------------------------------------------------------ the cell


def wait_window(app, timeout=30):
    end = time.time() + timeout
    while time.time() < end and app.alive():
        ws = app.windows()
        if ws:
            return ws
        time.sleep(0.3)
    return []


def editor_cell(ctx, r, app, target):
    """Everything that needs the plugin's own interface on screen."""
    probe = ctx.probe
    app.send(f"plugwin {target}")
    ws = wait_window(app)
    if not ws:
        r.add("editor opens", "fail", "no plugin window: " + app.output()[-300:])
        return False
    w = ws[0]
    r.expect("editor opens", True, f"{w['format']} {w['w']}x{w['h']} plugin {w['pw']}x{w['ph']} resizable {w['resizable']}")
    r.expect("editor opens at the plugin's size", (w["pw"], w["ph"]) == (640, 400), f"{w['pw']}x{w['ph']}")
    r.expect("the plugin's window was created", bool(probe.wait(r"attached 640 400 hwnd ok", 0, timeout=15)), probe.text()[-200:])
    m = probe.wait(r"scale (\S+)", 0, timeout=1.0)
    r.add("content scale told to the plugin (for information)", "pass" if m else "skip", m.group(0) if m else "not told")
    time.sleep(1.0)
    check_editor_shown(ctx, r, w, "opened")
    if not ctx.has_input:
        r.add("mouse and keyboard", "skip", "no XTEST route to this display")
        return True
    check_mouse(ctx, r, w)
    check_sync_from_plugin(ctx, r, w, app, target)
    check_moved(ctx, r, w, app)
    w = fresh_window(app) or w
    check_keys(ctx, r, w, app)
    check_resize(ctx, r, w, app)
    return True


def check_sync_from_plugin(ctx, r, window, app, target):
    probe, x = ctx.probe, ctx.x
    g = x.geom(window["container"])
    x.move(g["x"] + 120, g["y"] + 220)
    before = app.params(target)
    m0 = probe.size()
    x.down(1)
    time.sleep(0.15)
    for dy in (-10, -30, -50, -70):
        x.move(g["x"] + 120, g["y"] + 220 + dy)
    x.up(1)
    edits = re.findall(r"edit 0 (\S+)", probe.text(m0))
    r.expect("dragging the plugin's slider edits its parameter", bool(edits), probe.text(m0)[-200:])
    time.sleep(0.8)
    after = app.params(target)
    if edits and after and after["values"]:
        r.expect("the edit reaches Gridline's parameter", abs(after["values"][0] - float(edits[-1])) < 0.01, f"plugin {edits[-1]}, Gridline {after['values'][0]} (before {before and before['values'][:1]})")
    else:
        r.add("the edit reaches Gridline's parameter", "fail", f"no values: {after}")
    # And the other way: a knob in Gridline moves the control the plugin shows.
    m0 = probe.size()
    app.params(target, (0, 0.31))
    m = probe.wait(r"param 0 0\.3[01]", m0, timeout=4.0)
    r.expect("a knob in Gridline moves the plugin's control", bool(m), probe.text(m0)[-200:])
    time.sleep(0.6)
    img = x.shot(window["window"], str(ctx.cell_dir / "shot-knob.png"))
    if img:
        ox, oy = window["px"], window["py"]
        # 0.31 fills the slider up to y = 240 - 62 = 178.
        r.expect("the plugin draws the new value", near(pixel(img, ox + 200, oy + 200), (230, 140, 40)) and not near(pixel(img, ox + 200, oy + 150), (230, 140, 40)),
                 f"{pixel(img, ox + 200, oy + 200)} {pixel(img, ox + 200, oy + 150)}")


def lifecycle_cell(ctx, r, app, target):
    probe, x = ctx.probe, ctx.x
    # Close and open again, many times.
    t0 = time.time()
    ok = True
    for i in range(10):
        app.send("plugwin close")
        time.sleep(0.5)
        m0 = probe.size()
        app.send(f"plugwin {target}")
        if not wait_window(app) or not probe.wait(r"attached \d+ \d+ hwnd ok", m0, timeout=15):
            ok = False
            r.add("close and reopen ten times", "fail", f"round {i + 1}: {probe.text(m0)[-200:]}")
            break
    if ok:
        r.add("close and reopen ten times", "pass", f"{time.time() - t0:.1f} s")
    time.sleep(1.0)
    w = fresh_window(app)
    if w:
        check_points(ctx, r, "clicks exact after reopening", w, corner_points(w["pw"], w["ph"]))
    return w


def two_editors_cell(ctx, r, types):
    """Two instances at once, each window gets its own clicks. Also: removing with the editor open."""
    probe, x = ctx.probe, ctx.x
    probe.clear()
    d = json.loads(Path(make_project(ctx, ctx.cell_dir / "two.json", types["synth"], level=0.5)).read_text())
    second = json.loads(json.dumps(d["channels"][0]))
    second["id"] = d["channels"][0]["id"] + 1
    second["name"] = "Second"
    d["channels"].append(second)
    (ctx.cell_dir / "two.json").write_text(json.dumps(d))
    app = App(ctx, "two")
    try:
        app.send(f'open "{ctx.cell_dir / "two.json"}"')
        if not app.wait_instance("chan 1"):
            r.add("two editors at once", "fail", "instances not created")
            return
        app.send("plugwin chan 0")
        wait_window(app)
        app.send("plugwin chan 1")
        end = time.time() + 30
        ws = []
        while time.time() < end and len(ws) < 2:
            ws = app.windows()
            time.sleep(0.3)
        if not r.expect("two editors at once", len(ws) == 2, f"{len(ws)} windows"):
            return
        x.window_move(ws[0]["window"], 10, 10)
        x.window_move(ws[1]["window"], 800, 300)
        insts = set()
        for w in ws:
            g = x.geom(w["container"])
            x.move(g["x"] + 300, g["y"] + 200)
            m0 = probe.size()
            x.click(1)
            m = probe.wait(r"^\d+\t(\d+)\tdown L 300 200", m0)
            r.expect(f"click lands exactly in editor {len(insts) + 1}", bool(m), probe.text(m0)[-160:])
            if m:
                insts.add(m.group(1))
        r.expect("each editor is its own instance", len(insts) == 2, insts)
        # Removing a plugin with its editor open.
        app.send("rmchan 1")
        time.sleep(2.0)
        left = app.windows()
        r.expect("removing the plugin closes its editor, Gridline lives on", app.alive() and len(left) == 1, f"alive {app.alive()}, {len(left)} windows")
        if left:
            check_points(ctx, r, "the other editor still takes clicks", left[0], [(3, 3), (300, 200)])
    finally:
        done, code = app.quit() if app.alive() else (True, None)
        r.expect("quit with an editor open: no hang", done, f"exit {code}")
        app.kill()
        kill_hosts()


def state_cell(ctx, r, types, sandbox):
    """Project save, close and reload."""
    probe = ctx.probe
    probe.clear()
    path = make_project(ctx, ctx.cell_dir / "state-start.json", types["synth"], level=0.5)
    saved = ctx.cell_dir / "state-saved.json"
    sb = [types["synth"]] if sandbox else []
    app = App(ctx, "state", sb)
    try:
        app.send(f'open "{path}"')
        if not app.wait_instance():
            r.add("state saved and restored", "fail", "no instance")
            return
        app.params("chan 0", (0, 0.73))
        time.sleep(0.5)
        m = app.ask(f'save "{saved}"', r"save frame \d+: (\w+)", timeout=60)
        r.expect("project saves", bool(m and m.group(1) == "saved"), m.group(0) if m else "no answer")
        r.expect("the plugin was asked for its state", bool(probe.wait(r"state save 0\.73", 0, timeout=5)), probe.text()[-200:])
        done, code = app.quit()
        r.expect("quit: no hang, clean exit", done and code == 0, f"exited {done}, code {code}")
    finally:
        app.kill()
    time.sleep(1.5)
    r.expect("no Wine host left behind after quitting", host_processes() == 0, f"{host_processes()} left")
    kill_hosts()
    probe.clear()
    app = App(ctx, "state2", sb)
    try:
        app.send(f'open "{saved}"')
        p = app.wait_instance()
        r.expect("reload: state restored in the plugin", bool(probe.wait(r"state load 0\.73", 0, timeout=10)), probe.text()[-240:])
        r.expect("reload: Gridline shows the value", bool(p and p["values"] and abs(p["values"][0] - 0.73) < 0.001), p and p["values"][:3])
        done, code = app.quit()
        r.expect("quit after reload: no hang, clean exit", done and code == 0, f"exited {done}, code {code}")
    finally:
        app.kill()
        kill_hosts()


def audio_cell(ctx, r, types, target_fmt, sandbox):
    """Notes reach the instrument, the signal is measured, automation moves the plugin."""
    probe, cli = ctx.probe, ctx.cli
    env = ctx.env
    # Real time, dummy driver.
    probe.clear()
    path = make_project(ctx, ctx.cell_dir / "rt.json", types["synth"], level=0.5)
    app = App(ctx, "rt", [types["synth"]] if sandbox else [])
    try:
        app.send(f'open "{path}"')
        p = app.wait_instance()
        if not p:
            r.add("real time: instance", "fail", app.output()[-300:])
            return
        r.expect("runs where it was asked to", p["sandboxed"] == sandbox, f"sandboxed {p['sandboxed']}")
        # A knob in Gridline reaches the sound: the same note at two levels, measured at the output.
        peaks = {}
        for level in (0.2, 0.8):
            app.params("chan 0", (0, level))
            time.sleep(0.4)
            app.send("sound")
            time.sleep(0.3)
            app.ask("sound", r"sound frame \d+: peak")
            app.send("noteon 0 69")
            time.sleep(1.2)
            s = app.ask("sound", r"sound frame \d+: peak (\S+)")
            peaks[level] = float(s.group(1)) if s else 0.0
            app.send("noteoff 0 69")
            time.sleep(0.8)
        peak = peaks[0.8]
        r.expect("real time: a note sounds", peak > 0.01, f"peak {peak}")
        ratio = peaks[0.8] / peaks[0.2] if peaks[0.2] > 0 else 0
        r.expect("real time: a knob in Gridline changes the plugin's level", 3.0 < ratio < 5.0, f"peaks {peaks}, ratio {ratio:.2f}, wanted 4")
        time.sleep(1.2)
        s = app.ask("sound", r"sound frame \d+: peak (\S+)")
        time.sleep(1.2)
        s = app.ask("sound", r"sound frame \d+: peak (\S+)")
        peak2 = float(s.group(1)) if s else 1.0
        r.expect("real time: silent after note off", peak2 < 0.001, f"peak {peak2}")
        done, code = app.quit()
        r.expect("real time: clean exit", done and code == 0, f"exited {done}, code {code}")
    finally:
        app.kill()
    t = probe.text()
    notes = re.findall(r"noteson (\d+) notesoff (\d+) lastkey (-?\d+)", t)
    r.expect("the note reached the plugin (key 69)", bool(notes and notes[-1][2] == "69"), notes[-1:] or t[-160:])
    kill_hosts()

    # Offline render: a held note at a known pitch.
    probe.clear()
    notes_a = [{"length": 384 * 2, "pitch": 69, "start": 0, "velocity": 100}]
    proj = make_project(ctx, ctx.cell_dir / "render.json", types["synth"], notes=notes_a, level=0.8)
    wav = ctx.cell_dir / "render.wav"
    rc, o = sh(cli, "render", proj, "-o", wav, "--sr", 48000, "--bits", 16, timeout=240, env=env)
    if rc != 0 or not wav.exists():
        r.add("offline render", "fail", f"exit {rc}: {o[-300:]}")
    else:
        rate, s = read_wav(wav)
        body = s[int(rate * 0.2) : int(rate * 0.8)]
        f = frequency(body, rate)
        level = rms(body)
        r.expect("offline render: the note at its pitch", abs(f - 440) < 2, f"{f:.1f} Hz")
        r.expect("offline render: not silent", level > 0.01, f"rms {level:.4f}")
        jumps = max((abs(body[i] - body[i - 1]) for i in range(1, len(body))), default=1)
        expected = 2 * 3.14159 * 440 / rate * max(abs(x) for x in body) if body else 0
        r.expect("offline render: no glitch in the sine", jumps <= expected * 1.3 + 1e-3, f"largest step {jumps:.4f}, sine allows {expected:.4f}")
    kill_hosts()

    # Automation from Gridline moves the plugin's level: half as loud half way down the ramp.
    probe.clear()
    long_note = [{"length": 1536, "pitch": 69, "start": 0, "velocity": 100}]
    proj = make_project(ctx, ctx.cell_dir / "auto.json", types["synth"], notes=long_note, level=0.5, automate={"param": 0, "points": [(0, 1.0), (384, 1.0), (768, 0.25), (1536, 0.25)]})
    wav = ctx.cell_dir / "auto.wav"
    rc, o = sh(cli, "render", proj, "-o", wav, "--sr", 48000, "--bits", 16, timeout=240, env=env)
    if rc != 0 or not wav.exists():
        r.add("automation in a render", "fail", f"exit {rc}: {o[-300:]}")
    else:
        rate, s = read_wav(wav)
        beat = rate // 2  # 120 BPM, 96 ticks a beat
        a, b = rms(s[beat : int(beat * 1.8)]), rms(s[beat * 9 : beat * 12])
        ratio = a / b if b > 0 else 0
        r.expect("automation moves the plugin's level in a render", 3.0 < ratio < 5.0, f"rms {a:.4f} then {b:.4f}, ratio {ratio:.2f}, wanted 4")
    kill_hosts()

    # Effect: a measured gain and the reported latency.
    probe.clear()
    source = [{"length": 768, "pitch": 57, "start": 0, "velocity": 100}]
    levels = {}
    for lv in (0.5, 0.25):
        proj = make_project(ctx, ctx.cell_dir / f"fx{lv}.json", None, fx_type=types["fx"], notes=source, level=lv)
        wav = ctx.cell_dir / f"fx{lv}.wav"
        rc, o = sh(cli, "render", proj, "-o", wav, "--sr", 48000, "--bits", 16, timeout=240, env=env)
        if rc == 0 and wav.exists():
            rate, s = read_wav(wav)
            levels[lv] = rms(s[int(rate * 0.2) : int(rate * 0.8)])
        kill_hosts()
    if len(levels) == 2 and levels[0.25] > 0:
        ratio = levels[0.5] / levels[0.25]
        r.expect("effect: the signal passes with the level set", 1.8 < ratio < 2.2, f"rms {levels[0.5]:.4f} and {levels[0.25]:.4f}, ratio {ratio:.2f}, wanted 2")
    else:
        r.add("effect: the signal passes with the level set", "fail", f"renders: {levels}")
    probe.clear()
    proj = make_project(ctx, ctx.cell_dir / "fx.json", None, fx_type=types["fx"], notes=source, level=0.5)
    app = App(ctx, "fx", [types["fx"]] if sandbox else [])
    try:
        app.send(f'open "{proj}"')
        p = app.wait_instance("fx 1 0")
        r.expect("effect: latency reported to Gridline", bool(p and p["latency"] >= 64), p and p["latency"])
        done, code = app.quit()
        r.expect("effect: clean exit", done and code == 0, f"exited {done}, code {code}")
    finally:
        app.kill()
        kill_hosts()


def kill_host_cell(ctx, r, types, sandbox):
    """The Wine host process dies. Sandbox mode: the instance is marked crashed. In-process: reported as is."""
    probe = ctx.probe
    probe.clear()
    path = make_project(ctx, ctx.cell_dir / "kill.json", types["synth"], level=0.5)
    app = App(ctx, "kill", [types["synth"]] if sandbox else [])
    try:
        app.send(f'open "{path}"')
        if not app.wait_instance():
            r.add("killing the Wine host", "fail", "no instance")
            return
        if not sandbox:
            app.send("plugwin chan 0")
            wait_window(app)
            time.sleep(1.5)
        kill_hosts()
        time.sleep(4.0)
        alive = app.alive()
        answers = alive and app.ask("epoch", r"epoch frame", timeout=20) is not None
        r.expect("killing the Wine host: Gridline stays alive and answers", bool(alive and answers), f"alive {alive}, answers {answers}, code {app.proc.poll()}")
        if answers:
            p = app.params()
            r.add("killing the Wine host: what Gridline shows (for information)", "pass", p and {k: p[k] for k in ("instance", "sandboxed", "crashed", "open")})
            done, code = app.quit()
            r.expect("killing the Wine host: quit still works", done, f"exited {done}, code {code}")
    finally:
        app.kill()
        kill_hosts()


def probe_cell(ctx, r, fmt, mode, types):
    sandbox = mode == "sandbox"
    ctx.format = fmt
    probe = ctx.probe
    r.cell = {"format": fmt, "mode": mode, "plugin": "probe", "host": "gridline"}
    ctx.cell_dir = ctx.out / f"{fmt}-{mode}"
    ctx.cell_dir.mkdir(parents=True, exist_ok=True)
    rc, o = sh(ctx.cli, "plugin-smoke", fmt, types["entry"], *(["--sandbox"] if sandbox else []), timeout=180, env=ctx.env)
    (ctx.cell_dir / "plugin-smoke.txt").write_text(o)
    r.expect("command line smoke test (load, play, save, restore)", rc == 0, o[-300:] if rc else f"exit {rc}")
    kill_hosts()
    probe.clear()
    path = make_project(ctx, ctx.cell_dir / "start.json", types["synth"], level=0.5)
    app = App(ctx, "main", [types["synth"]] if sandbox else [])
    try:
        app.send(f'open "{path}"')
        p = app.wait_instance()
        if not r.expect("the plugin loads", bool(p), app.output()[-300:] if not p else f"sandboxed {p['sandboxed']}"):
            return
        if sandbox:
            # Gridline's sandboxed plugin windows exist on macOS only (Sandbox.cpp hasEditor): the
            # plugin runs out of process and shows Gridline's generic parameter panel on Linux.
            app.send("plugwin chan 0")
            time.sleep(2.0)
            ws = app.windows()
            if ws:
                r.add("sandbox: no own interface expected on Linux", "fail", f"a window opened: {ws}")
            else:
                r.add("editor, mouse, keyboard, resize", "skip", "Run in sandbox shows no plugin interface on Linux: Gridline opens sandboxed interfaces on macOS only")
                img = ctx.x.shot(app.main_window() or "root", str(ctx.cell_dir / "shot-sandbox-panel.png"))
                r.add("sandbox: Gridline's own panel on screen", "pass" if img else "skip", "screenshot taken" if img else "no main window")
        else:
            editor_cell(ctx, r, app, "chan 0")
            lifecycle_cell(ctx, r, app, "chan 0")
    finally:
        if app.alive():
            done, code = app.quit()
            r.expect("quit with the editor open: no hang, clean exit", done and code == 0, f"exited {done}, code {code}")
        else:
            r.add("quit with the editor open: no hang, clean exit", "fail", f"the app had already exited, code {app.proc.poll()}")
        app.kill()
        kill_hosts()
    if not sandbox and ctx.has_input:
        two_editors_cell(ctx, r, types)
    state_cell(ctx, r, types, sandbox)
    audio_cell(ctx, r, types, fmt, sandbox)
    kill_host_cell(ctx, r, types, sandbox)


def real_cell(ctx, r, name, fmt, type_id, entry):
    """A real plugin: is it drawn, do clicks change pixels, do drags change parameters in Gridline."""
    r.cell = {"format": fmt, "mode": "inproc", "plugin": name, "host": "gridline"}
    ctx.cell_dir = ctx.out / f"real-{re.sub(r'[^A-Za-z0-9]+', '-', name)}-{fmt}"
    ctx.cell_dir.mkdir(parents=True, exist_ok=True)
    x = ctx.x
    path = make_project(ctx, ctx.cell_dir / "start.json", type_id)
    d = json.loads(Path(path).read_text())
    d["channels"][0]["plugin"]["params"] = []
    Path(path).write_text(json.dumps(d))
    app = App(ctx, "real")
    try:
        app.send(f'open "{path}"')
        p = app.wait_instance(timeout=180)
        if not r.expect("the plugin loads", bool(p), app.output()[-300:] if not p else f"{len(p['values'])} parameters"):
            return
        app.send("plugwin chan 0")
        ws = wait_window(app, timeout=90)
        if not r.expect("editor opens", bool(ws), app.output()[-300:]):
            return
        w = ws[0]
        time.sleep(8.0)  # toolkits draw late under software GL
        w = fresh_window(app) or w
        x.window_move(w["window"], 40, 40)
        time.sleep(1.5)
        x.dump(w["window"], ctx.cell_dir / "x.txt")
        first = x.shot(w["window"], str(ctx.cell_dir / "shot-opened.png"))
        if not first:
            r.add("editor drawn", "fail", "no screenshot")
            return
        fw, fh, rgb = first
        colours = len({rgb[i : i + 3] for i in range(0, min(len(rgb), fw * w["ph"] * 3), 3 * 7)})
        r.expect("editor drawn, not blank", colours > 24, f"{colours} colours in {w['pw']}x{w['ph']}")
        if not ctx.has_input:
            r.add("mouse", "skip", "no XTEST route to this display")
            return
        g = x.geom(w["container"])
        before = app.params()
        moved, base = 0, first
        total_changed = 0
        steps = [(cx, cy) for cy in (0.2, 0.4, 0.6, 0.8) for cx in (0.15, 0.35, 0.55, 0.75, 0.9)]
        for i, (cx, cy) in enumerate(steps):
            px, py = g["x"] + int(g["w"] * cx), g["y"] + int(g["h"] * cy)
            x.move(px, py)
            x.down(1)
            time.sleep(0.1)
            x.move(px, py - 30)
            x.move(px, py - 45)
            x.up(1)
            time.sleep(0.4)
            if i % 5 == 4:
                now = x.shot(w["window"], str(ctx.cell_dir / f"shot-step{i + 1}.png"))
                n = changed_pixels(base, now)
                if n > 0:
                    moved += 1
                    total_changed += n
                base = now or base
            if not app.alive():
                break
        r.expect("Gridline survives the mouse over the interface", app.alive(), f"exit {app.proc.poll()}")
        r.expect("clicks and drags change pixels", moved > 0, f"{moved} of 4 rounds changed, {total_changed} pixels")
        after = app.params()
        diffs = sum(1 for a, b in zip((before or {}).get("values", []), (after or {}).get("values", [])) if abs(a - b) > 1e-5) if before and after else -1
        r.expect("drags change parameters reported to Gridline", diffs > 0, f"{diffs} parameters changed")
        # The same after moving the window: the interface still reacts where the pointer is.
        x.window_move(w["window"], 500, 260)
        time.sleep(1.0)
        g = x.geom(w["container"])
        base = x.shot(w["window"], str(ctx.cell_dir / "shot-moved.png"))
        before = app.params()
        for cx, cy in steps[::2]:
            px, py = g["x"] + int(g["w"] * cx), g["y"] + int(g["h"] * cy)
            x.move(px, py)
            x.down(1)
            time.sleep(0.1)
            x.move(px, py + 40)
            x.up(1)
            time.sleep(0.3)
        now = x.shot(w["window"], str(ctx.cell_dir / "shot-moved-after.png"))
        r.expect("after moving the window: pixels still change", changed_pixels(base, now) > 0, f"{changed_pixels(base, now)} pixels")
        after = app.params()
        diffs = sum(1 for a, b in zip((before or {}).get("values", []), (after or {}).get("values", [])) if abs(a - b) > 1e-5) if before and after else -1
        r.expect("after moving the window: parameters still change", diffs > 0, f"{diffs} parameters changed")
    finally:
        if app.alive():
            done, code = app.quit(timeout=60)
            r.expect("quit: no hang, clean exit", done and code == 0, f"exited {done}, code {code}")
        app.kill()
        kill_hosts()


def control_cell(ctx, r, fmt, entry, carla_script):
    """The same probe in another host, so that a failure can be pinned on the right side."""
    r.cell = {"format": fmt, "mode": "inproc", "plugin": "probe", "host": "carla"}
    ctx.cell_dir = ctx.out / f"control-{fmt}"
    ctx.cell_dir.mkdir(parents=True, exist_ok=True)
    probe, x = ctx.probe, ctx.x
    probe.clear()
    log = open(ctx.cell_dir / "carla.log", "ab")
    cmdfile = ctx.work / "carla-cmd"
    proc = subprocess.Popen([sys.executable, str(carla_script), fmt, str(entry), str(cmdfile)], stdout=log, stderr=subprocess.STDOUT, env=ctx.env, start_new_session=True)
    try:
        end = time.time() + 120
        m = None
        while not m and time.time() < end and proc.poll() is None:
            m = probe.wait(r"attached \d+ \d+ hwnd ok", 0, timeout=1.0)
        said = (ctx.cell_dir / "carla.log").read_text(errors="replace")[-400:]
        if not m and "has no" in said and "support" in said:
            r.add("control host: editor opens", "skip", said.strip())
            return
        if not r.expect("control host: editor opens", bool(m), said):
            return
        time.sleep(2.0)
        rc, o = sh("xdotool", "search", "--name", "Gridline Probe", timeout=10)
        ids = o.split()
        if not ids:
            r.add("control host: window found", "fail", "no window named after the plugin")
            return
        win = hex(int(ids[-1]))
        x.dump(win, ctx.cell_dir / "x.txt")
        x.shot(win, str(ctx.cell_dir / "shot.png"))
        w = {"window": win, "container": win, "pw": 640, "ph": 400, "px": 0, "py": 0, "resizable": True}
        check_points(ctx, r, "left click at corners and centre", w, corner_points(640, 400))
        for i, (mx, my) in enumerate([(300, 250), (700, 90)]):
            x.window_move(win, mx, my)
            check_points(ctx, r, f"clicks after moving the plugin window ({i + 1})", w, [(3, 3), (636, 396), (300, 200)])
        g = x.geom(win)
        x.move(g["x"] + 450, g["y"] + 300)
        m0 = probe.size()
        x.click(1)
        r.expect("a click gives the editor the keyboard", bool(probe.wait(r"focus in", m0, timeout=2.0)), probe.text(m0)[-160:])
        time.sleep(0.3)
        m0 = probe.size()
        x.type("abc")
        time.sleep(0.5)
        chars = re.findall(r"char (\d+)", probe.text(m0))
        r.expect("typed text reaches the editor", chars[:3] == ["97", "98", "99"], chars)
    finally:
        try:
            os.killpg(proc.pid, signal.SIGTERM)
            proc.wait(timeout=15)
        except (OSError, subprocess.TimeoutExpired):
            try:
                os.killpg(proc.pid, signal.SIGKILL)
            except OSError:
                pass
        log.close()
        kill_hosts()


def control_real(ctx, r, entry, script):
    """Dexed in the control host: is a blank interface, or a hang on closing, Gridline's doing?"""
    r.cell = {"format": "vst3", "mode": "inproc", "plugin": "Dexed", "host": "carla"}
    ctx.cell_dir = ctx.out / "control-Dexed"
    ctx.cell_dir.mkdir(parents=True, exist_ok=True)
    x = ctx.x
    log = open(ctx.cell_dir / "carla.log", "ab")
    proc = subprocess.Popen([sys.executable, str(script), "vst3", str(entry), "-"], stdout=log, stderr=subprocess.STDOUT, env=ctx.env, start_new_session=True)
    try:
        ids = []
        end = time.time() + 120
        while not ids and time.time() < end:
            ids = sh("xdotool", "search", "--name", "Dexed", timeout=10)[1].split()
            time.sleep(1.0)
        if not r.expect("control host: editor opens", bool(ids), (ctx.cell_dir / "carla.log").read_text(errors="replace")[-300:]):
            return
        time.sleep(8.0)
        win = hex(int(ids[-1]))
        x.dump(win, ctx.cell_dir / "x.txt")
        img = x.shot(win, str(ctx.cell_dir / "shot.png"))
        colours = len({img[2][i : i + 3] for i in range(0, len(img[2]), 21)}) if img else 0
        r.add("control host: interface drawn (for comparison)", "pass" if colours > 24 else "skip", f"{colours} colours")
    finally:
        t0 = time.time()
        try:
            os.killpg(proc.pid, signal.SIGTERM)
            proc.wait(timeout=60)
            r.add("control host: closes in time (for comparison)", "pass", f"{time.time() - t0:.1f} s")
        except (OSError, subprocess.TimeoutExpired):
            r.add("control host: closes in time (for comparison)", "skip", "still running after 60 s")
            try:
                os.killpg(proc.pid, signal.SIGKILL)
            except OSError:
                pass
        log.close()
        kill_hosts()


def scan(ctx, r):
    """yabridge plugins through Gridline's own scanner, as a user's first start does."""
    r.cell = {"format": "all", "mode": "inproc", "plugin": "scan", "host": "gridline"}
    t0 = time.time()
    rc, o = sh(ctx.cli, "scan-plugins", "--force", timeout=900, env=ctx.env)
    (ctx.out / "scan.txt").write_text(o)
    r.expect("scan finishes", rc == 0, f"{time.time() - t0:.0f} s: {o[:300]}")
    diag = ctx.out / "scan-diagnosis"
    diag.mkdir(exist_ok=True)
    for path in re.findall(r"failed: (/.*?): ", o):
        fmt = "vst3" if ".vst3" in path else "clap" if ".clap" in path else "vst2"
        t1 = time.time()
        env = dict(ctx.env, YABRIDGE_DEBUG_LEVEL="2")
        rc2, o2 = sh(ctx.cli, "--scan-plugin", fmt, path, "--out", diag / "out.json", timeout=240, env=env)
        name = re.sub(r"[^A-Za-z0-9.]+", "-", Path(path).name)
        (diag / f"{fmt}-{name}.txt").write_text(f"exit {rc2} after {time.time() - t1:.1f} s\n" + o2[-200000:])
        kill_hosts()
        r.add(f"alone with a long limit: {Path(path).name}", "pass" if rc2 == 0 else "fail", f"exit {rc2} after {time.time() - t1:.1f} s: {o2[-160:]}")
    rc, o = sh(ctx.cli, "list-plugins", timeout=30, env=ctx.env)
    (ctx.out / "plugins.txt").write_text(o)
    rows = []
    for line in o.splitlines():
        m = re.match(r"(\S+)\s+(instrument|effect)\s+(.*?)\s{2,}.*?(\S+)$", line)
        if m:
            rows.append({"format": m.group(1).lower().replace("vst 3", "vst3"), "kind": m.group(2), "name": m.group(3), "type": m.group(4)})
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--app", required=True)
    ap.add_argument("--cli", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--wine", required=True, help="label of the Wine under test")
    ap.add_argument("--display", required=True, help="xvfb-openbox | xvfb-bare | weston-x11 | weston-wayland")
    ap.add_argument("--scale", required=True)
    ap.add_argument("--run", default="1", help="repeat number, to expose instability")
    ap.add_argument("--formats", default="vst3,vst2,clap")
    ap.add_argument("--modes", default="inproc,sandbox")
    ap.add_argument("--real", default="", help="comma separated substrings of real plugins to test")
    ap.add_argument("--control", action="store_true")
    a = ap.parse_args()

    out = Path(a.out).absolute()
    out.mkdir(parents=True, exist_ok=True)
    r = Results(out, {"wine": a.wine, "display": a.display, "scale": a.scale, "run": a.run})
    ctx = Ctx()
    ctx.app, ctx.cli = str(Path(a.app).absolute()), str(Path(a.cli).absolute())
    ctx.out, ctx.work = out, out / "work"
    ctx.work.mkdir(exist_ok=True)
    cfg = ctx.work / "config"
    cfg.mkdir(exist_ok=True)
    ctx.env = dict(os.environ, GRIDLINE_CONFIG_HOME=str(cfg), XDG_CONFIG_HOME=str(cfg), GRIDLINE_NO_UPDATE_CHECK="1", SDL_AUDIODRIVER="dummy", GRIDLINE_NO_GSETTINGS="1")
    os.environ.update(ctx.env)
    ctx.prefix = os.environ.get("WINEPREFIX", str(Path.home() / ".wine"))
    ctx.probe = Probe(ctx.prefix, out / "probe-log.txt")
    ctx.weston = a.display.startswith("weston")
    ctx.has_wm_tools = a.display in ("xvfb-openbox", "weston-x11")
    ctx.has_input = a.display != "weston-wayland"
    ctx.x = X(sync=not ctx.weston)
    ctx.format = ""
    ctx.demo = ctx.work / "demo.json"
    sh(ctx.cli, "demo", ctx.demo, timeout=60, check=True, env=ctx.env)

    # A cell that hangs takes the whole job with it otherwise.
    stop = threading.Event()

    def watchdog():
        if not stop.wait(CELL_SECONDS * 12):
            r.add("whole job watchdog", "fail", "still running after the limit: killed")
            os._exit(3)

    threading.Thread(target=watchdog, daemon=True).start()

    plugins = scan(ctx, r)
    probes = {}
    for fmt in a.formats.split(","):
        rows = [p for p in plugins if p["format"] == fmt and "Gridline Probe" in p["name"]]
        synth = next((p for p in rows if p["kind"] == "instrument"), None)
        fx = next((p for p in rows if p["kind"] == "effect"), None)
        r.cell = {"format": fmt, "mode": "inproc", "plugin": "scan", "host": "gridline"}
        if not r.expect("the scan finds the bridged probe (instrument and effect)", bool(synth and fx), rows or "nothing in this format"):
            continue
        probes[fmt] = {"synth": synth["type"], "fx": fx["type"], "entry": PROBE_ENTRIES[fmt]}

    if a.display == "weston-wayland":
        # SDL on its Wayland driver: there is no X window to embed a bridged interface in.
        for fmt, types in probes.items():
            r.cell = {"format": fmt, "mode": "inproc", "plugin": "probe", "host": "gridline"}
            ctx.cell_dir = out / f"{fmt}-wayland"
            ctx.cell_dir.mkdir(exist_ok=True)
            ctx.probe.clear()
            path = make_project(ctx, ctx.cell_dir / "start.json", types["synth"], level=0.5)
            app = App(ctx, "wayland")
            try:
                time.sleep(3)
                if not app.alive():
                    r.add("app on the Wayland driver", "skip", "the app does not start on SDL's Wayland driver here: " + app.output()[-200:])
                    continue
                app.send(f'open "{path}"')
                p = app.wait_instance()
                r.expect("the plugin loads", bool(p), app.output()[-200:])
                app.send("plugwin chan 0")
                time.sleep(4)
                ws = app.windows()
                out_text = app.output()
                r.add("interface on the Wayland driver", "skip", f"{len(ws)} windows; X11 is what plugin interfaces embed in, Gridline uses XWayland there. Log: {out_text[-160:]}")
                done, code = app.quit()
                r.expect("quit: no hang, clean exit", done and code == 0, f"exited {done}, code {code}")
            finally:
                app.kill()
                kill_hosts()
        stop.set()
        return 0

    for fmt, types in probes.items():
        for mode in a.modes.split(","):
            cell_stop = threading.Event()
            def cell_guard():
                if not cell_stop.wait(CELL_SECONDS):
                    r.add("cell watchdog", "fail", f"{fmt}/{mode}: still running after {CELL_SECONDS} s, everything killed")
                    sh("pkill", "-9", "-f", "gridline-app", timeout=5)
                    kill_hosts()
            threading.Thread(target=cell_guard, daemon=True).start()
            try:
                probe_cell(ctx, r, fmt, mode, types)
            except Exception as e:  # a harness fault must show as a failure, not vanish
                r.add("harness error", "fail", repr(e))
            cell_stop.set()

    wanted = [w for w in a.real.split(",") if w]
    for p in plugins:
        if "Gridline Probe" in p["name"] or not any(w.lower() in p["name"].lower() for w in wanted):
            continue
        try:
            real_cell(ctx, r, p["name"], p["format"], p["type"], None)
        except Exception as e:
            r.add("harness error", "fail", repr(e))

    if a.control:
        script = Path(__file__).parent / "carla_control.py"
        for fmt in a.formats.split(","):
            try:
                control_cell(ctx, r, fmt, PROBE_ENTRIES[fmt], script)
            except Exception as e:
                r.add("harness error", "fail", repr(e))
        if a.real:
            try:
                control_real(ctx, r, Path.home() / ".vst3/yabridge/Dexed.vst3", script)
            except Exception as e:
                r.add("harness error", "fail", repr(e))
    stop.set()
    return 0


if __name__ == "__main__":
    sys.exit(main())
