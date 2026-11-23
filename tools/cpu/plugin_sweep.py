#!/usr/bin/env python3
# Gridline - the idle CPU sweep of every built-in plugin in the app (K-142, 0.7.8): for each type, an
# empty project with one instance (`--plugin <type>`: instruments on a new channel, effects in the
# last slot of insert 1, note effects on a small synth channel), hidden and quiet, two CPU reports
# (`--cpu-report`, tools/cpu/cpurun.py's scratch setup):
#   closed: one note played (instruments and note effects; frames 20 to 50), then the editor closed,
#           then settle and record: the plugin's DSP calls, process CPU, frames per second;
#   open:   the editor left open and untouched: process CPU, frames per second (an animation that
#           never stops shows as frames above the idle pace and main thread time).
# The engine side (tails, resting, offline sound) is tests/test_k142_plugins.cpp; this is the app side.
#
#   python3 tools/cpu/plugin_sweep.py --app <gridline-app> --types <file of types> --out <folder>
#       [--seconds 6] [--settle 6] [--only <type,...>] [--jobs 1]
import argparse
import concurrent.futures
import queue
import json
import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cpurun  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
DEMO = os.path.normpath(os.path.join(HERE, "..", "..", "assets", "demo", "Gridline.gridline.json"))


def run_one(app, project, ptype, kind, mode, folder, seconds, settle):
    if os.path.isdir(folder):
        shutil.rmtree(folder)
    env = cpurun.scratch_env(folder)
    out = os.path.join(folder, "report.json")
    script = os.path.join(folder, "input.txt")
    lines = []
    if mode == "closed":
        if kind != "effect":
            lines += ["20 noteon 0 60", "50 noteoff 0 60"]
        lines += ["55 closeplugins"]
    lines += ["60 #"]
    with open(script, "w") as f:
        f.write("\n".join(lines) + "\n")
    # The open run has nothing to script: without --dev-input it runs as a user's session does (a dev
    # script turns on development records, such as the editor fit measuring, that cost time per frame).
    scripted = ["--dev-input", script] if mode == "closed" else []
    cmd = [app, project, "--hidden", "--no-ui-sounds", "--no-update-check", "--size", "1440x900", "--layout", "studio",
           "--plugin", ptype] + scripted + [
           "--cpu-report", str(seconds), "--cpu-settle", str(settle), "--cpu-states", "visible", "--cpu-report-out", out]
    try:
        p = subprocess.run(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120 + 3 * (seconds + settle))
        log = p.stdout.decode("utf-8", "replace")
        rc = p.returncode
    except subprocess.TimeoutExpired:
        log, rc = "timeout", -9
    with open(os.path.join(folder, "run.log"), "w") as f:
        f.write(log)
    if rc != 0 or not os.path.exists(out):
        return {"error": "exit %d" % rc, "log": log[-1500:]}
    with open(out) as f:
        rep = json.load(f)
    run = rep["runs"][0] if "runs" in rep else rep
    proc = run.get("process", {})
    nodes = run.get("nodes", [])
    mine = [n for n in nodes if n.get("type") == ptype]
    main = sum(t.get("cpu_percent", 0) for t in run.get("threads_by_name", []) if t.get("name") == "gl-main")
    audio = sum(t.get("cpu_percent", 0) for t in run.get("threads_by_name", []) if t.get("name") == "gl-audio")
    blocks = run.get("audio", {}).get("engine_blocks", 0)
    return {
        "cpu": proc.get("cpu_percent_of_one_core", 0),
        "fps": run.get("frames_per_s", 0),
        "main": main,
        "audio": audio,
        "dsp": sum(n.get("dsp_calls", 0) for n in mine),
        "blocks": blocks,
        "others_dsp": sum(n.get("dsp_calls", 0) for n in nodes if n.get("type") != ptype),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--app", required=True)
    ap.add_argument("--types", required=True, help="TSV from gridline_tests_k142_plugins --tsv (type, name, kind columns)")
    ap.add_argument("--out", required=True)
    ap.add_argument("--scratch")
    ap.add_argument("--seconds", type=float, default=6)
    ap.add_argument("--settle", type=float, default=6)
    ap.add_argument("--only", default="")
    ap.add_argument("--modes", default="closed,open")
    ap.add_argument("--jobs", type=int, default=1)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    scratch = a.scratch or os.path.join(a.out, "scratch")
    os.makedirs(scratch, exist_ok=True)
    project = cpurun.make_empty(DEMO, os.path.join(a.out, "empty.gridline.json"))
    rows = []
    with open(a.types) as f:
        head = f.readline().rstrip("\n").split("\t")
        for line in f:
            v = dict(zip(head, line.rstrip("\n").split("\t")))
            rows.append((v["type"], v["name"], v["kind"]))
    only = set(x for x in a.only.split(",") if x)
    if only:
        rows = [r for r in rows if r[0] in only]
    modes = a.modes.split(",")
    results = {}
    res_path = os.path.join(a.out, "plugins-app.json")
    if os.path.exists(res_path):
        with open(res_path) as f:
            results = json.load(f)

    def job(r, mode, k):
        t, name, kind = r
        return t, mode, run_one(a.app, project, t, kind, mode, os.path.join(scratch, "r%d" % k), a.seconds, a.settle)

    todo = [(r, m) for r in rows for m in modes if not (r[0] in results and m in results[r[0]] and "error" not in results[r[0]][m])]
    slots = queue.Queue()
    for s in range(max(1, a.jobs)):
        slots.put(s)

    def jobq(r, m):
        s = slots.get()
        try:
            return job(r, m, s)
        finally:
            slots.put(s)

    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, a.jobs)) as ex:
        futs = [ex.submit(jobq, r, m) for (r, m) in todo]
        for fu in concurrent.futures.as_completed(futs):
            t, mode, res = fu.result()
            results.setdefault(t, {})[mode] = res
            print(t, mode, {k: (round(v, 2) if isinstance(v, float) else v) for k, v in res.items() if k != "log"}, flush=True)
            with open(res_path, "w") as f:
                json.dump(results, f, indent=1, sort_keys=True)
    # table
    with open(os.path.join(a.out, "plugins-app.md"), "w") as f:
        f.write("| Plugin | Type | Kind | Closed: CPU % | DSP blocks / engine blocks | Open: CPU % | Open: frames/s | Open: gl-main % |\n|---|---|---|---|---|---|---|---|\n")
        for t, name, kind in rows:
            c = results.get(t, {}).get("closed", {})
            o = results.get(t, {}).get("open", {})
            def g(d, k, fmtx="%.2f"):
                return fmtx % d[k] if k in d else ("err" if "error" in d else "-")
            f.write("| %s | %s | %s | %s | %s | %s | %s | %s |\n" % (name, t, kind, g(c, "cpu"), ("%d / %d" % (c["dsp"], c["blocks"])) if "dsp" in c else g(c, "dsp"),
                                                            g(o, "cpu"), g(o, "fps", "%.1f"), g(o, "main")))


if __name__ == "__main__":
    main()
