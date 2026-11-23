#!/usr/bin/env python3
# Gridline - K-142 (0.7.8 follow-up): a hidden Gridline with OSC and the AI assistant's server switched on
# draws nothing while idle, and what arrives wakes it at once: an OSC message (the OSC server's thread), an
# AI assistant request (its server's watcher) and a MIDI controller's message (from a thread of its own, as
# the MIDI input's thread brings it; a stand-in device, GRIDLINE_TEST_MIDI=fake). The CPU report records
# the hidden state meanwhile: a turn for each, no idle frames (they ran 4 a second with either server on).
#   python3 k142_wake_sources_app.py <gridline-app> <project.gridline.json> <out dir>
import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time
import urllib.request

APP, DEMO, OUT = sys.argv[1], sys.argv[2], sys.argv[3]
WIN = os.name == "nt"
shutil.rmtree(OUT, ignore_errors=True)
for d in ("cfg/gridline", "home", "rt"):
    os.makedirs(os.path.join(OUT, d))
RT = os.path.join(OUT, "rt")
if not WIN:
    os.chmod(RT, 0o700)
    SOCKDIR = f"/tmp/glk142src-{os.getpid()}"  # (a socket path must fit in about 100 bytes)
    shutil.rmtree(SOCKDIR, ignore_errors=True)
    os.makedirs(SOCKDIR, mode=0o700)
    EP = os.path.join(SOCKDIR, "ep.sock")
else:
    EP = f"gridline-test-k142src-{os.getpid()}"
CFG = os.path.join(OUT, "cfg")
with open(os.path.join(CFG, "gridline", "settings.json"), "w") as f:
    json.dump({"format": "gridline-settings", "values": {
        "ui.layoutChosen": True, "osc.enabled": True, "osc.port": 0, "mcp.enabled": True, "mcp.port": 0}}, f)
env = dict(os.environ)
env.update({"GRIDLINE_CONFIG_HOME": CFG, "HOME": os.path.join(OUT, "home"), "GRIDLINE_SECRET_STORE": "file",
            "GRIDLINE_NO_PLUGIN_SCAN": "1", "GRIDLINE_HIDE_PLUGINS": "all", "XDG_RUNTIME_DIR": RT,
            "GRIDLINE_TEST_INSTANCE_ENDPOINT": EP, "SDL_AUDIODRIVER": "dummy", "GRIDLINE_QUIET_WINDOWS": "1",
            "GRIDLINE_TEST_MIDI": "fake", "GRIDLINE_OSC_TRACE": "1", "GRIDLINE_MIDI_TRACE": "1"})
env.pop("GRIDLINE_SINGLE_INSTANCE", None)
wl = os.environ.get("WAYLAND_DISPLAY", "")
if wl and not os.path.isabs(wl) and os.environ.get("XDG_RUNTIME_DIR"):
    env["WAYLAND_DISPLAY"] = os.path.join(os.environ["XDG_RUNTIME_DIR"], wl)
failures = []


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what, flush=True)
    if not cond:
        failures.append(what)


# The MIDI message comes 11 s after the script's line (the report settles 2 s, then records 16 s hidden;
# the stand-in input opens meanwhile). One frame for every line: a hidden Gridline draws only its first.
script = os.path.join(OUT, "input.txt")
with open(script, "w") as f:
    f.write('2 midifake in add "Wake Knobs"\n'
            '2 midifake later 11000 "Wake Knobs" 0xB0 20 64\n')
report = os.path.join(OUT, "report.json")
app = subprocess.Popen([APP, DEMO, "--hidden", "--no-ui-sounds", "--no-update-check", "--size", "1280x800", "--ui-scale", "1",
                        "--dev-input", script, "--cpu-report", "16", "--cpu-settle", "2", "--cpu-states", "hidden",
                        "--cpu-report-out", report],
                       env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace")
lines = []
stamps = {}
oscPort = [0]


def reader():
    for line in app.stdout:
        line = line.rstrip("\n")
        lines.append(line)
        now = time.time()
        if line.startswith("osc: listening") and not oscPort[0]:
            try:
                oscPort[0] = int(line.split()[-1])
            except ValueError:
                pass
        for key in ("osc: /k142/probe", "midifake later: send", "midi: taken B0 20 64", "midifake frame 2: later"):
            if line.startswith(key) and key not in stamps:
                stamps[key] = now


threading.Thread(target=reader, daemon=True).start()
end = time.time() + 120
while time.time() < end and ("midifake frame 2: later" not in stamps or not oscPort[0]) and app.poll() is None:
    time.sleep(0.05)
check("midifake frame 2: later" in stamps, "the stand-in controller's message is on its way")
check(oscPort[0] > 0, "OSC listens (port %d)" % oscPort[0])
if app.poll() is not None or failures:
    print("\n".join(lines[-40:]))
    sys.exit(1)
t0 = stamps["midifake frame 2: later"]
# Well into the recorded hidden state: settled (2 s) and a few seconds of drawing nothing.
time.sleep(max(0.0, t0 + 6.0 - time.time()))

# 1. OSC: a message to the hidden Gridline.
addr = b"/k142/probe\0"
packet = addr + b"\0" * ((4 - len(addr) % 4) % 4) + b",i\0\0" + struct.pack(">i", 1)
sent = time.time()
with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
    s.sendto(packet, ("127.0.0.1", oscPort[0]))
end = time.time() + 5
while time.time() < end and "osc: /k142/probe" not in stamps:
    time.sleep(0.005)
took = stamps.get("osc: /k142/probe", sent + 1e9) - sent
check(took < 0.5, "an OSC message is taken at once while nothing is drawn (%.3f s)" % took)

# 2. The AI assistant's server: an initialize request.
time.sleep(1.0)
ep = {}
try:
    ep = json.load(open(os.path.join(CFG, "gridline", "mcp-endpoint.json")))
except (OSError, ValueError):
    pass
check(ep.get("port", 0) > 0 and len(ep.get("token", "")) >= 16, "the AI assistant's server wrote its endpoint")
body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "initialize",
                   "params": {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "k142-wake"}}}).encode()
req = urllib.request.Request("http://127.0.0.1:%d/mcp" % ep.get("port", 1), data=body, method="POST",
                             headers={"Content-Type": "application/json", "Accept": "application/json, text/event-stream",
                                      "Authorization": "Bearer " + ep.get("token", "")})
sent = time.time()
status, reply = 0, ""
try:
    with urllib.request.urlopen(req, timeout=10) as r:
        status, reply = r.status, r.read().decode("utf-8", "replace")
except Exception as e:  # noqa: BLE001 (any failure is the check's)
    reply = str(e)
took = time.time() - sent
check(status == 200 and '"result"' in reply, "the AI assistant's server answers while nothing is drawn (%d)" % status)
check(took < 0.5, "at once (%.3f s)" % took)

# 3. MIDI: the controller's message from its own thread.
end = time.time() + 15
while time.time() < end and "midi: taken B0 20 64" not in stamps:
    time.sleep(0.01)
check("midifake later: send" in stamps and "delivered" in next((l for l in lines if l.startswith("midifake later: send")), ""),
      "the controller's message arrives on its own thread")
took = stamps.get("midi: taken B0 20 64", 1e9) - stamps.get("midifake later: send", 0)
check(took < 0.5, "a MIDI controller's message is taken at once while nothing is drawn (%.3f s)" % took)

try:
    app.wait(timeout=120)
except subprocess.TimeoutExpired:
    app.kill()
    app.wait()
check(app.returncode == 0, "the hidden Gridline ends its report by itself (rc %s)" % app.returncode)
if os.path.exists(report):
    runs = json.load(open(report)).get("runs", [])
    check(len(runs) == 1 and runs[0]["state"] == "hidden", "one hidden run recorded")
    if runs:
        # a turn for each arrival (the request's connection, its reply), no idle frames
        check(runs[0]["frames"] <= 12, "nothing drawn while idle with OSC and the AI assistant's server on (%d turns in 16 s)" % runs[0]["frames"])
else:
    check(False, "the report is written")
if not WIN:
    shutil.rmtree(SOCKDIR, ignore_errors=True)
with open(os.path.join(OUT, "app.log"), "w", encoding="utf-8") as f:
    f.write("\n".join(lines))
if failures:
    print("%d check(s) failed" % len(failures))
    print("\n".join(lines[-40:]))
    sys.exit(1)
print("k142 wake sources: complete")
