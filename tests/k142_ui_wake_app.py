#!/usr/bin/env python3
# Gridline - K-142 (0.7.8): work another thread hands the interface wakes it while nothing is drawn.
# A hidden Gridline on a private test endpoint (GRIDLINE_TEST_INSTANCE_ENDPOINT, isolated config, home
# and runtime folder) records the CPU report's "hidden" state, where its interface draws nothing and
# sleeps; a later launch hands it a request then, and the running one takes it at once (well under its
# next timed wake), not seconds later.
#   python3 k142_ui_wake_app.py <gridline-app> <project.gridline.json> <out dir>
import os
import shutil
import subprocess
import sys
import threading
import time

APP, DEMO, OUT = sys.argv[1], sys.argv[2], sys.argv[3]
WIN = os.name == "nt"
shutil.rmtree(OUT, ignore_errors=True)
for d in ("cfg", "home", "rt"):
    os.makedirs(os.path.join(OUT, d))
RT = os.path.join(OUT, "rt")
if not WIN:
    os.chmod(RT, 0o700)
    SOCKDIR = f"/tmp/glk142wake-{os.getpid()}"  # (a socket path must fit in about 100 bytes)
    shutil.rmtree(SOCKDIR, ignore_errors=True)
    os.makedirs(SOCKDIR, mode=0o700)
    EP = os.path.join(SOCKDIR, "ep.sock")
else:
    EP = f"gridline-test-k142wake-{os.getpid()}"
env = dict(os.environ)
env.update({"GRIDLINE_CONFIG_HOME": os.path.join(OUT, "cfg"), "HOME": os.path.join(OUT, "home"),
            "GRIDLINE_NO_PLUGIN_SCAN": "1", "GRIDLINE_HIDE_PLUGINS": "all", "XDG_RUNTIME_DIR": RT,
            "GRIDLINE_TEST_INSTANCE_ENDPOINT": EP, "SDL_AUDIODRIVER": "dummy", "GRIDLINE_QUIET_WINDOWS": "1"})
env.pop("GRIDLINE_SINGLE_INSTANCE", None)
wl = os.environ.get("WAYLAND_DISPLAY", "")
if wl and not os.path.isabs(wl) and os.environ.get("XDG_RUNTIME_DIR"):
    env["WAYLAND_DISPLAY"] = os.path.join(os.environ["XDG_RUNTIME_DIR"], wl)
COMMON = ["--hidden", "--no-ui-sounds", "--no-update-check", "--size", "1280x800", "--ui-scale", "1"]
failures = []


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what, flush=True)
    if not cond:
        failures.append(what)


report = os.path.join(OUT, "report.json")
first = subprocess.Popen([APP, DEMO] + COMMON + ["--cpu-report", "12", "--cpu-settle", "2", "--cpu-states", "hidden",
                                                  "--cpu-report-out", report],
                         env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace")
lines = []
stamps = {}


def reader():
    for line in first.stdout:
        lines.append(line.rstrip("\n"))
        for key in ("single instance: listening", "single instance: request with"):
            if key in line and key not in stamps:
                stamps[key] = time.time()


threading.Thread(target=reader, daemon=True).start()
end = time.time() + 120
while time.time() < end and "single instance: listening" not in stamps and first.poll() is None:
    time.sleep(0.05)
check("single instance: listening" in stamps, "the hidden Gridline listens")
# Into the recorded hidden state: settled (2 s) and a few seconds of drawing nothing.
time.sleep(7)
sent = time.time()
r = subprocess.run([APP] + COMMON, env=env, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=120)
check(r.returncode == 0 and "single instance: handed over 0 file(s)" in r.stdout, "a later launch hands over (rc %d)" % r.returncode)
end = time.time() + 10
while time.time() < end and "single instance: request with" not in stamps:
    time.sleep(0.02)
took = stamps.get("single instance: request with", 1e9) - sent
check(took < 2.0, "the hidden Gridline takes the request at once (%.2f s after the launch started)" % took)
try:
    first.wait(timeout=120)
except subprocess.TimeoutExpired:
    first.kill()
    first.wait()
check(first.returncode == 0, "the hidden Gridline ends its report by itself (rc %s)" % first.returncode)
if os.path.exists(report):
    import json
    runs = json.load(open(report)).get("runs", [])
    check(len(runs) == 1 and runs[0]["state"] == "hidden", "one hidden run recorded")
    if runs:
        # (the request brings no frame of its own: taking it is a turn of the loop, not a drawing)
        check(runs[0]["frames"] <= 2, "nothing drawn while hidden (%d frames)" % runs[0]["frames"])
else:
    check(False, "the report is written")
if not WIN:
    shutil.rmtree(SOCKDIR, ignore_errors=True)
with open(os.path.join(OUT, "first.log"), "w", encoding="utf-8") as f:
    f.write("\n".join(lines))
if failures:
    print("%d check(s) failed" % len(failures))
    print("\n".join(lines[-40:]))
    sys.exit(1)
print("k142 ui wake: complete")
