#!/usr/bin/env python3
# Gridline - K-121: a later launch hands its files to the Gridline that is running.
# Two hidden instances on a private test endpoint (GRIDLINE_TEST_INSTANCE_ENDPOINT), isolated
# config, home and runtime folder; never the user's own Gridline:
#   1. the first one listens (its socket is 0600), started with a .gridline project;
#   2. a launch with an older .gridline.json hands it over and exits; the running one opens it;
#   3. with unsaved changes a handed-over project asks first and the open one stays;
#   4. with GRIDLINE_SINGLE_INSTANCE=0 a launch runs on its own;
#   5. a launch without files hands over (the window would come forward) and exits;
#   6. after a clean exit the socket is gone; a stale socket left by a crash does not block the
#      next launch;
#   7. a hidden run without the test endpoint never makes or uses one.
#   python3 k121_single_instance.py <gridline-app> <project.gridline.json> <out dir>
import os
import shutil
import socket
import stat
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
    # (a socket path must fit in about 100 bytes: a short private folder of its own)
    SOCKDIR = f"/tmp/glk121app-{os.getpid()}"
    shutil.rmtree(SOCKDIR, ignore_errors=True)
    os.makedirs(SOCKDIR, mode=0o700)
    EP = os.path.join(SOCKDIR, "ep.sock")
else:
    EP = f"gridline-test-k121app-{os.getpid()}"

A = os.path.join(OUT, "Song A.gridline")
B = os.path.join(OUT, "Été mix 2.gridline")
C = os.path.join(OUT, "Old song.gridline.json")
for p in (A, B, C):
    shutil.copyfile(DEMO, p)

env = dict(os.environ)
env.update({"GRIDLINE_CONFIG_HOME": os.path.join(OUT, "cfg"), "HOME": os.path.join(OUT, "home"),
            "GRIDLINE_NO_PLUGIN_SCAN": "1", "GRIDLINE_HIDE_PLUGINS": "all", "XDG_RUNTIME_DIR": RT,
            "GRIDLINE_TEST_INSTANCE_ENDPOINT": EP})
env.pop("GRIDLINE_SINGLE_INSTANCE", None)
# The private runtime folder hides the session's Wayland socket, which is named relative to the real one:
# name it by its full path, so the instances still reach the compositor (a Wayland session).
wl = os.environ.get("WAYLAND_DISPLAY", "")
if wl and not os.path.isabs(wl) and os.environ.get("XDG_RUNTIME_DIR"):
    env["WAYLAND_DISPLAY"] = os.path.join(os.environ["XDG_RUNTIME_DIR"], wl)
COMMON = ["--hidden", "--no-ui-sounds", "--no-update-check", "--size", "1280x800", "--ui-scale", "1"]


def fail(msg):
    print(f"FAIL: {msg}")
    sys.exit(1)


def ok(msg):
    print(f"ok - {msg}")


def run(args, extra_env=None, timeout=240, cwd=None):
    e = dict(env)
    e.update(extra_env or {})
    t = time.time()
    r = subprocess.run([APP] + args, env=e, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=timeout, cwd=cwd)
    return r, time.time() - t


def touch(name):
    open(os.path.join(OUT, name), "w").close()


# 1. The first Gridline.
script = os.path.join(OUT, "a.txt")
with open(script, "w", encoding="utf-8") as f:
    f.write("8 launcher\n"
            f"10 waitfor \"file:{os.path.join(OUT, 'b1.done')}\" 120\n"
            "14 mknotes 4 96 48\n"
            "16 launcher\n"
            f"18 waitfor \"file:{os.path.join(OUT, 'b2.done')}\" 120\n"
            "24 launcher\n"
            f"26 waitfor \"file:{os.path.join(OUT, 'b3.done')}\" 120\n"
            "30 quit discard\n")
first = subprocess.Popen([APP, A] + COMMON + ["--dev-input", script], env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                         text=True, encoding="utf-8", errors="replace")
lines, errs = [], []
listening = threading.Event()


def reader():
    for line in first.stdout:
        lines.append(line.rstrip("\n"))
        if "single instance: listening" in line:
            listening.set()


threading.Thread(target=reader, daemon=True).start()
threading.Thread(target=lambda: errs.extend(first.stderr), daemon=True).start()
if not listening.wait(120):
    first.kill()
    fail("the first Gridline does not listen:\n" + "\n".join(lines) + "".join(errs))
ok("1: the first Gridline listens")
if not WIN:
    st = os.lstat(EP)
    if not stat.S_ISSOCK(st.st_mode) or (st.st_mode & 0o777) != 0o600 or st.st_uid != os.getuid():
        first.kill()
        fail(f"1: the socket is not private: mode {oct(st.st_mode)}")
    ok("1: its socket is the user's own, mode 0600")


def wait_for(text, secs=120):
    end = time.time() + secs
    while time.time() < end:
        if any(text in l for l in lines):
            return True
        time.sleep(0.05)
    return False


def count(text):
    return sum(text in l for l in lines)


def wait_count(text, n, secs=120):
    end = time.time() + secs
    while time.time() < end:
        if count(text) >= n:
            return True
        time.sleep(0.05)
    return False


def hand_over(tag, args, want_files, done, cwd=None):
    r, took = run(args + ["--hidden"], cwd=cwd)
    out = r.stdout + r.stderr
    with open(os.path.join(OUT, tag + ".log"), "w", encoding="utf-8") as f:
        f.write(out)
    if r.returncode != 0 or f"single instance: handed over {want_files} file(s)" not in r.stdout:
        first.kill()
        fail(f"{tag}: the launch did not hand over (rc {r.returncode}):\n{out}")
    if took > 30:
        first.kill()
        fail(f"{tag}: handing over took {took:.1f} s")
    ok(f"{tag}: handed over {want_files} file(s) and exited in {took:.1f} s")
    if not wait_for(f"single instance: request with {want_files} file(s)"):
        first.kill()
        fail(f"{tag}: the running Gridline got no request:\n" + "\n".join(lines))
    touch(done)


# 2. A launch with an older-name project, named relative to the folder it starts in (after the
# first one has reported what it started with).
if not wait_count("launcher visible=", 1):
    first.kill()
    fail("1: no first report:\n" + "\n".join(lines))
hand_over("2", [os.path.basename(C)], 1, "b1.done", cwd=OUT)
got_open = wait_for("os open: ")
opened = [l[len("os open: "):-len(" (running)")] for l in lines if l.startswith("os open: ") and l.endswith(" (running)")]
if not got_open or not opened or not os.path.isabs(opened[0]) or os.path.basename(opened[0]) != os.path.basename(C):
    first.kill()
    fail("2: the handed-over project was not opened by its full path:\n" + "\n".join(lines))
ok("2: the running Gridline opens it (by its full path) as a double click would")
if not wait_count("launcher visible=", 2):
    first.kill()
    fail("2: no report after the open:\n" + "\n".join(lines))

# 4. Opt-out: this launch runs on its own (and quits), the running one gets nothing.
q = os.path.join(OUT, "q.txt")
with open(q, "w", encoding="utf-8") as f:
    f.write("4 quit discard\n")
before = sum("single instance: request" in l for l in lines)
r, _ = run([B] + COMMON + ["--dev-input", q], {"GRIDLINE_SINGLE_INSTANCE": "0"})
if r.returncode != 0 or "handed over" in r.stdout or "single instance: listening" in r.stdout:
    first.kill()
    fail(f"4: GRIDLINE_SINGLE_INSTANCE=0 still met the running Gridline (rc {r.returncode}):\n{r.stdout}{r.stderr}")
time.sleep(0.5)
if sum("single instance: request" in l for l in lines) != before:
    first.kill()
    fail("4: the running Gridline got a request from an opted-out launch")
ok("4: GRIDLINE_SINGLE_INSTANCE=0 runs on its own")

# 3. With unsaved changes: the prompt first, the open project stays.
hand_over("3", [B], 1, "b2.done")

# 5. Without files.
hand_over("5", [], 0, "b3.done")

try:
    first.wait(240)
except subprocess.TimeoutExpired:
    first.kill()
    fail("the first Gridline did not quit:\n" + "\n".join(lines))
time.sleep(0.2)
with open(os.path.join(OUT, "first.log"), "w", encoding="utf-8") as f:
    f.write("\n".join(lines) + "\n" + "".join(errs))
if first.returncode != 0:
    fail(f"the first Gridline exited with {first.returncode}:\n" + "\n".join(lines) + "".join(errs))
reps = [l for l in lines if l.startswith("launcher visible=")]
if len(reps) != 3:
    fail(f"{len(reps)} launcher reports, expected 3:\n" + "\n".join(lines))
if "project=Song A.gridline " not in reps[0] + " ":
    fail("1: not started with Song A.gridline: " + reps[0])
ok("1: a .gridline project opens from the command line")
if "prompt=0" not in reps[1] or "modified=1" not in reps[1] or "project=Old song.gridline.json" not in reps[1]:
    fail("2: the older-name project is not the open one: " + reps[1])
ok("2: the older-name project is open")
if "prompt=1" not in reps[2] or "modified=1" not in reps[2] or "project=Old song.gridline.json" not in reps[2]:
    fail("3: with unsaved changes it should ask first and keep the project: " + reps[2])
ok("3: with unsaved changes it asks first, the open project stays")

# 6. The socket after a clean exit, and a stale one after a crash.
if not WIN:
    if os.path.exists(EP):
        fail("6: the socket is still there after a clean exit")
    ok("6: the socket is gone after a clean exit")
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.bind(EP)
    s.listen(1)
    s.close()  # (a crash leaves the file: nothing listens on it)
    if not os.path.exists(EP):
        fail("6: could not make a stale socket")
    r, _ = run([A] + COMMON + ["--dev-input", q])
    if r.returncode != 0 or "single instance: listening" not in r.stdout:
        fail(f"6: a stale socket blocked the next launch (rc {r.returncode}):\n{r.stdout}{r.stderr}")
    ok("6: a stale socket does not block the next launch")

# 7. A hidden run without the test endpoint: no endpoint at all.
e7 = {"GRIDLINE_TEST_INSTANCE_ENDPOINT": ""}
r, _ = run([A] + COMMON + ["--dev-input", q], e7)
made = [f for f in os.listdir(RT) if f.startswith("gridline")]
if r.returncode != 0 or "single instance" in r.stdout or made:
    fail(f"7: a hidden run met an endpoint (rc {r.returncode}, {made}):\n{r.stdout}")
ok("7: hidden runs never make or use an endpoint")
if not WIN:
    shutil.rmtree(SOCKDIR, ignore_errors=True)
print("ok - k121 single instance")
