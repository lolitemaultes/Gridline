#!/usr/bin/env python3
"""Plugin interfaces on Wayland desktops: gridline-app on a real compositor (Weston, headless).

    tools/ci/check-wayland.py <gridline-app> <gridline CLI> <folder with a native .vst3> <work dir>

Each case starts its own Weston and opens a native VST3's interface:

    desktop           Weston with XWayland, SDL left to itself (on sdl2-compat with SDL3 this is
                      where SDL may pick Wayland)
    session-variable  the same with SDL_VIDEODRIVER=wayland, as desktops set it for games
    own-setting       GRIDLINE_VIDEO_DRIVER=wayland: stays on Wayland and says why it cannot open
    no-xwayland       Weston without XWayland and no DISPLAY: stays on Wayland and says why

Prints a line per check and exits non-zero when one fails. The driver in use and the reason come
from the app's own log line ("[video] video driver: ...").
"""
import json
import os
import re
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

failures = []


def check(ok, name, detail=""):
    print(f"{'ok' if ok else 'FAIL'}: {name}" + (f" ({detail})" if detail and not ok else ""), flush=True)
    if not ok:
        failures.append(name)


class Weston:
    def __init__(self, work, xwayland):
        self.socket = f"wl-check-{os.getpid()}-{int(xwayland)}"
        self.log_path = work / f"weston-{int(xwayland)}.log"
        self.env = dict(os.environ, WAYLAND_DISPLAY=self.socket)
        self.env.pop("DISPLAY", None)
        args = ["weston", "--backend=headless", "--width=1600", "--height=1000", f"--socket={self.socket}", "--idle-time=0"]
        if xwayland:
            args.append("--xwayland")
        self.proc = subprocess.Popen(args, stdout=open(self.log_path, "wb"), stderr=subprocess.STDOUT, start_new_session=True)
        run_dir = Path(os.environ["XDG_RUNTIME_DIR"])
        for _ in range(100):
            if (run_dir / self.socket).exists():
                break
            time.sleep(0.1)
        if xwayland:
            for _ in range(100):
                m = re.search(r"xserver listening on display (:\d+)", self.log_path.read_text(errors="replace"))
                if m:
                    self.env["DISPLAY"] = m.group(1)
                    break
                time.sleep(0.1)
            if "DISPLAY" not in self.env:
                raise RuntimeError("Weston did not start XWayland: " + self.log_path.read_text(errors="replace")[-400:])

    def stop(self):
        try:
            os.killpg(self.proc.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        self.proc.wait(timeout=20)


def run_case(name, app, project, work, weston, extra_env):
    case_dir = work / name
    case_dir.mkdir(parents=True, exist_ok=True)
    live = case_dir / "live"
    log_path = case_dir / "app.log"
    env = dict(weston.env, **extra_env, SDL_AUDIODRIVER="dummy", GRIDLINE_NO_PLUGIN_SCAN="1", GRIDLINE_NO_UPDATE_CHECK="1")
    with open(log_path, "wb") as log:
        proc = subprocess.Popen([app, "--no-ui-sounds", "--size", "1100x700", "--dev-input-live", str(live)], stdout=log, stderr=subprocess.STDOUT, env=env, start_new_session=True)

    def send(*lines):
        tmp = Path(str(live) + ".tmp")
        tmp.write_text("".join(f"0 {l}\n" for l in lines))
        tmp.rename(live)
        end = time.time() + 60
        while live.exists() and time.time() < end and proc.poll() is None:
            time.sleep(0.05)

    def output():
        return log_path.read_text(errors="replace")

    def wait_for(pattern, seconds):
        end = time.time() + seconds
        while time.time() < end:
            m = re.search(pattern, output())
            if m or proc.poll() is not None:
                return m
            time.sleep(0.1)
        return re.search(pattern, output())

    try:
        started = wait_for(r"\[video\] video driver: (\S+) \(([^;]*);", 60)
        check(bool(started), f"{name}: the app starts and names its driver", output()[-400:])
        time.sleep(3)
        send(f'open "{project}"')
        time.sleep(8)
        mark = len(output())
        send("plugwin chan 0")
        time.sleep(6)
        send("plugwin info")
        time.sleep(1)
        check(proc.poll() is None, f"{name}: still running after asking for the interface", output()[-400:])
        info = re.search(r"plugwin frame \d+: (\d+) windows", output()[mark:])
        windows = int(info.group(1)) if info else -1
        return (started.group(1) if started else ""), windows, output()[mark:]
    finally:
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        proc.wait(timeout=20)


def main():
    app, cli, vst3_dir, work = sys.argv[1], sys.argv[2], sys.argv[3], Path(sys.argv[4]).resolve()
    work.mkdir(parents=True, exist_ok=True)
    os.environ.setdefault("XDG_RUNTIME_DIR", tempfile.mkdtemp(prefix="xdg-", dir=work))
    os.chmod(os.environ["XDG_RUNTIME_DIR"], 0o700)
    os.environ["HOME"] = str(work / "home")
    os.environ["GRIDLINE_CONFIG_HOME"] = str(work / "config")
    os.environ["VST3_PATH"] = vst3_dir
    Path(os.environ["HOME"]).mkdir(parents=True, exist_ok=True)

    subprocess.run([cli, "scan-plugins", "--force"], check=True, timeout=600)
    listed = subprocess.run([cli, "list-plugins"], check=True, timeout=60, capture_output=True, text=True).stdout
    plugin = None
    for line in listed.splitlines():
        m = re.match(r"(?i)vst\s?3\s+instrument\s+.*?(\S+)$", line)
        if m:
            plugin = m.group(1)
            break
    check(plugin is not None, "the scan finds a native VST3 instrument", listed[-400:])
    if plugin is None:
        return 1

    demo = work / "demo.json"
    subprocess.run([cli, "demo", str(demo)], check=True, timeout=60)
    d = json.loads(demo.read_text())
    d["channels"] = d["channels"][:1]
    d["channels"][0]["plugin"] = {"type": plugin, "params": []}
    project = work / "project.json"
    project.write_text(json.dumps(d))

    weston = Weston(work, xwayland=True)
    try:
        driver, windows, _ = run_case("desktop", app, project, work, weston, {})
        check(driver == "x11", "desktop: Gridline is on X11 (XWayland)", driver)
        check(windows == 1, "desktop: the plugin's interface opened", f"{windows} windows")

        driver, windows, _ = run_case("session-variable", app, project, work, weston, {"SDL_VIDEODRIVER": "wayland"})
        check(driver == "x11", "SDL_VIDEODRIVER=wayland: Gridline is on X11 (XWayland)", driver)
        check(windows == 1, "SDL_VIDEODRIVER=wayland: the plugin's interface opened", f"{windows} windows")

        driver, windows, text = run_case("own-setting", app, project, work, weston, {"GRIDLINE_VIDEO_DRIVER": "wayland"})
        check(driver == "wayland", "GRIDLINE_VIDEO_DRIVER=wayland: Gridline is on Wayland", driver)
        check(windows == 0, "GRIDLINE_VIDEO_DRIVER=wayland: no window", f"{windows} windows")
        check("GRIDLINE_VIDEO_DRIVER keeps Gridline on Wayland" in text, "GRIDLINE_VIDEO_DRIVER=wayland: the message says why", text[-300:])
    finally:
        weston.stop()

    weston = Weston(work, xwayland=False)
    try:
        driver, windows, text = run_case("no-xwayland", app, project, work, weston, {})
        check(driver == "wayland", "no XWayland: Gridline is on Wayland", driver)
        check(windows == 0, "no XWayland: no window", f"{windows} windows")
        check("need XWayland" in text, "no XWayland: the message says why", text[-300:])
    finally:
        weston.stop()

    print("failed: " + ", ".join(failures) if failures else "all Wayland checks passed", flush=True)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
