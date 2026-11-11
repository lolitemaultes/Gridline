#!/usr/bin/env python3
"""Gridline's windows on a Wayland compositor, visible (not --hidden): run under tools/ci/with-compositor.sh.

    tools/ci/check-wayland-live.py <gridline-app> <work dir> <xwayland|native|wayland-setting> [--scale N] [--outputs N]
                                   [--shots DIR] [--plugin <gridline CLI> <folder with a native .vst3>]

xwayland: Gridline must be on X11 (XWayland), whatever SDL_VIDEODRIVER says. native: on Wayland itself (no X
server). wayland-setting: XWayland is there, but the Window system setting keeps Gridline on Wayland (plugin
interfaces then open in windows of their own on XWayland). Checks, through the app's own --dev-input-live commands and its log:

    the driver and its reason; the window is shown at the size asked for and at the compositor's density;
    the interface fills it; the command palette opens, and Play and Stop answer SDL pointer clicks;
    resize and fullscreen asked by the app are followed; a file dropped by another program lands where the
    pointer is; the clipboard round trip; a later launch hands its files and activation token over; detached
    panels (on X11) or the reason they are off (Wayland); a plugin's interface opens (embedded on X11, in a
    window of its own on XWayland in native mode, or a message without XWayland); screenshots of the main
    window and a floating panel at the compositor's scale through the app's own --screenshot path.

Prints a line per check and exits non-zero when one fails.
"""
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path

failures = []


def check(ok, name, detail=""):
    print(f"{'ok' if ok else 'FAIL'}: {name}" + (f" ({detail})" if detail and not ok else ""), flush=True)
    if not ok:
        failures.append(name)


def note(text):
    print(f"note: {text}", flush=True)


class App:
    def __init__(self, app, case_dir, args, env):
        self.dir = case_dir
        self.dir.mkdir(parents=True, exist_ok=True)
        self.live = case_dir / "live"
        self.log_path = case_dir / "app.log"
        self.sent = 0
        with open(self.log_path, "wb") as log:
            self.proc = subprocess.Popen([app, "--no-ui-sounds", "--no-update-check", "--dev-input-live", str(self.live)] + args,
                                         stdout=log, stderr=subprocess.STDOUT, env=env, start_new_session=True)

    def output(self):
        return self.log_path.read_text(errors="replace")

    def send(self, *lines, wait=60):
        tmp = Path(str(self.live) + ".tmp")
        tmp.write_text("".join(f"0 {l}\n" for l in lines))
        tmp.rename(self.live)
        end = time.time() + wait
        while self.live.exists() and time.time() < end and self.proc.poll() is None:
            time.sleep(0.05)

    def wait_for(self, pattern, seconds, since=0):
        end = time.time() + seconds
        while time.time() < end:
            m = re.search(pattern, self.output()[since:])
            if m or self.proc.poll() is not None:
                return m
            time.sleep(0.1)
        return re.search(pattern, self.output()[since:])

    def ask(self, cmd, pattern, seconds=20):
        mark = len(self.output())
        self.send(cmd)
        return self.wait_for(pattern, seconds, mark)

    def stop(self):
        try:
            os.killpg(self.proc.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        self.proc.wait(timeout=20)


def info(app):
    m = app.ask("sdlwin info", r"sdlwin frame \d+: driver (\S+) window (\d+)x(\d+) drawable (\d+)x(\d+) imgui (\d+)x(\d+) fb-scale (\S+) "
                               r"fullscreen (\d) maximized (\d) shown (\d) focus (\d) displays (\d+)")
    if not m:
        return None
    k = ["driver", "w", "h", "dw", "dh", "iw", "ih", "fb", "fullscreen", "maximized", "shown", "focus", "displays"]
    d = dict(zip(k, m.groups()))
    for key in k[1:]:
        d[key] = float(d[key]) if key == "fb" else int(d[key])
    return d


def wait_info(app, test, seconds=10):
    end = time.time() + seconds
    d = info(app)
    while d and not test(d) and time.time() < end:
        time.sleep(0.3)
        d = info(app)
    return d


def main():
    a = sys.argv[1:]
    if len(a) < 3:
        print(__doc__)
        return 2
    exe, work, mode = a[0], Path(a[1]).resolve(), a[2]
    scale, outputs, shots, plugin = 1, 1, None, None
    i = 3
    while i < len(a):
        if a[i] == "--scale":
            scale = float(a[i + 1]); i += 2
        elif a[i] == "--outputs":
            outputs = int(a[i + 1]); i += 2
        elif a[i] == "--shots":
            shots = Path(a[i + 1]).resolve(); i += 2
        elif a[i] == "--plugin":
            plugin = (a[i + 1], a[i + 2]); i += 3
        else:
            print(__doc__)
            return 2
    native = mode in ("native", "wayland-setting")
    want = "wayland" if native else "x11"
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)
    home = work / "home"
    home.mkdir()
    env = dict(os.environ, HOME=str(home), GRIDLINE_CONFIG_HOME=str(work / "config"), SDL_AUDIODRIVER="dummy",
               GRIDLINE_NO_PLUGIN_SCAN="1", GRIDLINE_NO_UPDATE_CHECK="1", GRIDLINE_DEV_DROP_TRACE="1",
               GRIDLINE_TEST_INSTANCE_ENDPOINT=f"gl-live-{os.getpid()}.sock", GRIDLINE_NO_GSETTINGS="1")
    env.pop("GRIDLINE_VIDEO_DRIVER", None)
    env.pop("XDG_ACTIVATION_TOKEN", None)
    env.pop("DESKTOP_STARTUP_ID", None)
    if mode == "wayland-setting":  # Settings > Interface > Window system: Native Wayland, as the dialog saves it
        (work / "config" / "gridline").mkdir(parents=True, exist_ok=True)
        (work / "config" / "gridline" / "settings.json").write_text(json.dumps({"version": 1, "values": {"ui.windowSystem": "wayland"}}))
    if plugin:
        env["VST3_PATH"] = plugin[1]
        env.pop("GRIDLINE_NO_PLUGIN_SCAN")
    sample = work / "Dropped tone.wav"
    shutil.copyfile(Path(__file__).resolve().parents[2] / "assets/sounds/classic/open.wav", sample)

    # ---- the main window
    app = App(exe, work / "main", ["--size", "1280x800"], env)
    try:
        started = app.wait_for(r"\[video\] video driver: (\S+) \(([^;]*);", 60)
        check(bool(started), "the app starts and names its window system", app.output()[-400:])
        if not started:
            return 1
        check(started.group(1) == want, f"{mode}: Gridline is on {want}", f"{started.group(1)} ({started.group(2)})")
        d = wait_info(app, lambda d: d["shown"] == 1 and d["iw"] > 0)
        check(d is not None and d["shown"] == 1, "the main window is shown", str(d))
        if d:
            check(d["driver"] == want, "SDL reports the same driver", d["driver"])
            check(d["w"] >= 960 and d["h"] >= 600, "the window has a usable size (at least its minimum)", f"{d['w']}x{d['h']}")
            density = d["dw"] / max(1, d["w"])
            expect = scale if native else 1.0  # (XWayland windows are X pixels; the compositor scales them)
            if native and scale != int(scale):
                ok = abs(density - scale) < 0.01 or abs(density - round(scale + 0.5)) < 0.01  # SDL 2: the next whole scale
            else:
                ok = abs(density - expect) < 0.01
            check(ok, f"the window's pixel density follows the compositor (scale {scale})", f"drawable {d['dw']}x{d['dh']} for {d['w']}x{d['h']}")
            check(d["iw"] == d["w"] and d["ih"] == d["h"], "the interface fills the window", f"imgui {d['iw']}x{d['ih']} window {d['w']}x{d['h']}")
            check(d["displays"] >= outputs, f"SDL sees the compositor's {outputs} output(s)", str(d["displays"]))

        # menus, lists and the palette through SDL's pointer events (the path a real click takes)
        app.send("toasts clear")
        app.send("palette open")
        time.sleep(1.0)
        m = app.ask("windows 1", r"window \"[^\"]*(Command|palette|Palette)[^\"]*\"")
        check(bool(m), "the command palette opens", app.output()[-300:])
        app.send("palette close")
        # the transport's play button through SDL pointer events (the path a real click takes)
        app.send("sdlat play", "sdldown 0")
        time.sleep(0.5)
        app.send("sdlup 0")
        time.sleep(1.0)
        play = app.ask("playstate", r"playstate frame \d+: playing (\d)")
        check(bool(play) and play.group(1) == "1", "a click on Play plays (SDL pointer input)", play.group(0) if play else app.output()[-300:])
        app.send("sdlat stop", "sdldown 0")
        time.sleep(0.5)
        app.send("sdlup 0")
        time.sleep(1.0)
        play = app.ask("playstate", r"playstate frame \d+: playing (\d)")
        check(bool(play) and play.group(1) == "0", "a click on Stop stops", play.group(0) if play else "")

        # resize, maximise, fullscreen asked by the application
        app.send("sdlwin size 1100 700")
        d = wait_info(app, lambda d: d["w"] == 1100 and d["h"] == 700)
        check(d is not None and d["w"] == 1100 and d["h"] == 700 and d["iw"] == 1100, "a resize is followed (window and interface)", str(d))
        app.send("sdlwin fullscreen 1")
        d = wait_info(app, lambda d: d["fullscreen"] == 1 and d["w"] > 1100)
        check(d is not None and d["fullscreen"] == 1 and d["iw"] == d["w"], "fullscreen: the interface fills the screen", str(d))
        app.send("sdlwin fullscreen 0")
        d = wait_info(app, lambda d: d["fullscreen"] == 0)
        check(d is not None and d["fullscreen"] == 0, "back from fullscreen", str(d))

        # a file dropped by another program (SDL's drop events): lands where the pointer comes back
        mark = len(app.output())
        app.send(f'sdlwin drop "{sample}"', "sdlmove 640 400")
        drop = app.wait_for(r"sdl drop at (-?\d+),(-?\d+): 1 item", 10, mark)
        check(bool(drop), "a dropped file arrives", app.output()[mark:][-300:])
        if drop and native:
            check((int(drop.group(1)), int(drop.group(2))) == (640, 400), "Wayland: it lands where the pointer is", drop.group(0))

        # clipboard (needs a keyboard seat on the compositor; headless ones may have none)
        mark = len(app.output())
        app.send("sdlwin clipset gridline-clip-check", "sdlwin clipget")
        clip = app.wait_for(r"sdlwin frame \d+: clip \"([^\"]*)\"", 10, mark)
        if clip and clip.group(1) == "gridline-clip-check":
            check(True, "clipboard round trip")
        else:
            note("clipboard round trip not available on this compositor: " + app.output()[mark:][-200:].strip())

        # detached panels: on X11 they pop out; on Wayland they stay in, with the reason
        vp = app.ask("viewports", r"viewports frame \d+: available (\d) enabled (\d)")
        check(bool(vp), "detachable windows report their state")
        if vp and native:
            check(vp.group(1) == "0", "Wayland: detaching is off", vp.group(0))
            check("Not available on Wayland" in app.output(), "and the reason is given")
        elif vp:
            check(vp.group(1) == "1", "X11 (XWayland): panels can be detached", vp.group(0))
            app.send("detach Mixer 300 200 700 360")
            time.sleep(1.5)
            vp2 = app.ask("viewports", r"viewports frame \d+: available \d enabled \d detached (\d+)")
            check(bool(vp2) and int(vp2.group(1)) >= 1, "X11 (XWayland): a panel pops out into a window of its own", vp2.group(0) if vp2 else "")

        # a later launch hands its file and its activation token to this one
        demo = work / "Second launch.gridline.json"
        subprocess.run([exe.replace("gridline-app", "gridline"), "demo", str(demo)], env=env, timeout=60, capture_output=True)
        if demo.exists():
            env2 = dict(env, XDG_ACTIVATION_TOKEN="gridline-live-check-token")
            mark = len(app.output())
            second = subprocess.run([exe, "--no-ui-sounds", str(demo)], env=env2, timeout=60, capture_output=True, text=True)
            check(second.returncode == 0, "a later launch hands over and exits", (second.stdout + second.stderr)[-300:])
            got = app.wait_for(r"single instance: request with 1 file\(s\), activation token received", 15, mark)
            check(bool(got), "the running Gridline gets the file and the activation token", app.output()[mark:][-300:])
            fwd = app.wait_for(r"single instance: brought forward \(([^)]*)\)", 5, mark)
            check(bool(fwd), "and comes forward", app.output()[mark:][-300:])
            if fwd:
                note("brought forward by: " + fwd.group(1))
        else:
            note("no demo project (CLI missing): the single-instance check is skipped")

        check(app.proc.poll() is None, "still running after all of the above", app.output()[-400:])
    finally:
        app.stop()

    # ---- screenshots through the app's own path, at the compositor's scale
    if shots:
        shots.mkdir(parents=True, exist_ok=True)
        # the panel shot opens the undo history, a floating panel over the main window
        for name, args in (("main", []), ("panel", ["--pane", "history"])):
            out = shots / f"{mode}-scale{scale:g}-{name}.ppm"
            r = subprocess.run([exe, "--hidden", "--no-ui-sounds", "--size", "1440x900", "--scale", f"{scale:g}", "--frames", "40",
                                "--screenshot", str(out)] + args, env=dict(env, GRIDLINE_TEST_INSTANCE_ENDPOINT=""), timeout=300,
                               capture_output=True, text=True)
            check(r.returncode == 0 and out.exists() and out.stat().st_size > 1000, f"screenshot {out.name}", (r.stdout + r.stderr)[-300:])

    # ---- a plugin's interface
    if plugin:
        cli, vst3 = plugin
        subprocess.run([cli, "scan-plugins", "--force"], env=env, check=True, timeout=600, capture_output=True)
        listed = subprocess.run([cli, "list-plugins"], env=env, check=True, timeout=60, capture_output=True, text=True).stdout
        ptype = None
        for line in listed.splitlines():
            m = re.match(r"(?i)vst\s?3\s+instrument\s+.*?(\S+)$", line)
            if m:
                ptype = m.group(1)
                break
        check(ptype is not None, "the scan finds a native VST3 instrument", listed[-300:])
        if ptype:
            demo = work / "plugin.json"
            subprocess.run([cli, "demo", str(demo)], env=env, check=True, timeout=60)
            doc = json.loads(demo.read_text())
            doc["channels"] = doc["channels"][:1]
            doc["channels"][0]["plugin"] = {"type": ptype, "params": []}
            demo.write_text(json.dumps(doc))
            app = App(exe, work / "plugin", ["--size", "1100x700"], dict(env, GRIDLINE_TEST_INSTANCE_ENDPOINT=""))
            try:
                app.wait_for(r"\[video\] video driver", 60)
                time.sleep(2)
                app.send(f'open "{demo}"')
                time.sleep(6)
                mark = len(app.output())
                app.send("plugwin chan 0")
                # a loaded machine draws slowly: ask until the window is there (or 60 s went by)
                n, text, until = -1, "", time.time() + 60
                while time.time() < until and app.proc.poll() is None:
                    time.sleep(3)
                    app.send("plugwin info")
                    time.sleep(1)
                    text = app.output()[mark:]
                    counts = re.findall(r"plugwin frame \d+: (\d+) windows", text)
                    n = int(counts[-1]) if counts else -1
                    if n >= 1 or (native and not os.environ.get("DISPLAY") and n == 0 and "need XWayland" in text):
                        break
                check(app.proc.poll() is None, "still running after opening the interface", text[-300:])
                has_x = bool(os.environ.get("DISPLAY"))
                if not native:
                    check(n == 1 and " external 0 " in text, "X11: the interface opens inside its plugin window", f"{n} windows")
                elif has_x:
                    check(n == 1 and " external 1 " in text, "Wayland with XWayland: the interface opens in a window of its own", text[-300:])
                else:
                    check(n == 0 and "need XWayland" in text, "Wayland without XWayland: no window, and the message says why", text[-300:])
            finally:
                app.stop()

    print("failed: " + ", ".join(failures) if failures else "all live Wayland checks passed", flush=True)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
