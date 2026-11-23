#!/usr/bin/env python3
# Gridline - K-142 (0.7.8): the interface's idle pacing in the real app (hidden, quiet, scratch config
# and HOME, SDL's dummy audio driver), through the CPU report's window states: the demo song idle in
# Studio is drawn at the idle pace while visible, not at all while minimised or hidden, and drawn again
# as soon as the window comes back (after minimised and after hidden). The interface thread sleeps
# while nothing is drawn (its processor time a small part of the visible run's).
#   python3 k142_ui_pacing_app.py <gridline-app> <tools/cpu> <demo song> <out dir>
import os
import sys

APP, TOOLS, DEMO, OUT = sys.argv[1:5]
sys.path.insert(0, TOOLS)
import cpurun  # noqa: E402

failures = []


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what, flush=True)
    if not cond:
        failures.append(what)


STATES = ["visible", "minimised", "visible", "hidden", "visible", "unfocused"]
rep = cpurun.run_report(APP, DEMO, "studio", STATES, os.path.join(OUT, "run"), 4, 3)
runs = rep.get("runs", [])
check([r["state"] for r in runs] == STATES, "one run per state, in order: %s" % [r["state"] for r in runs])


def main_cpu(r):
    for t in r["threads_by_name"]:
        if t["name"] == "gl-main":
            return t["cpu_percent"]
    return None


for i, r in enumerate(runs):
    st, fps = r["state"], r["frames_per_s"]
    if st in ("visible", "unfocused"):
        check(r["frames"] > 0, "run %d %s: frames drawn (%d)" % (i, st, r["frames"]))
        if i > 0:  # (the first run follows the start: the song's sounds and meters may still be settling)
            check(fps <= 12.5, "run %d %s: at most the idle pace (%.1f frames a second)" % (i, st, fps))
    else:
        check(r["frames"] == 0, "run %d %s: nothing drawn (%d frames)" % (i, st, r["frames"]))
if len(runs) == len(STATES) and sys.platform in ("darwin", "linux"):
    vis, hid, mini = main_cpu(runs[0]), main_cpu(runs[3]), main_cpu(runs[1])
    if vis is not None and hid is not None and mini is not None:
        check(hid < max(0.5, vis * 0.25), "hidden: the interface thread sleeps (%.2f %% against %.2f %% visible)" % (hid, vis))
        check(mini < max(0.5, vis * 0.25), "minimised: the interface thread sleeps (%.2f %% against %.2f %% visible)" % (mini, vis))
if failures:
    print("%d check(s) failed" % len(failures))
    sys.exit(1)
print("k142 ui pacing: complete")
