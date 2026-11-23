#!/usr/bin/env python3
# Gridline - the idle CPU sweep: the CPU report (`gridline-app --cpu-report`) over a matrix of
# layouts x projects x window states, each run quiet and hidden in a scratch folder of its own
# (tools/cpu/cpurun.py). Writes every run's JSON and a Markdown table per project.
#
#   python3 tools/cpu/sweep.py --app <gridline-app> --out <folder> [--scratch <folder>]
#       [--layouts studio,live,logic,tracker] [--projects demo,empty,large]
#       [--states visible,unfocused,minimised,hidden] [--seconds 20] [--settle 10]
#
# The runs are made one after another (never in parallel: they would measure each other).
import argparse
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cpurun  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
DEMO = os.path.normpath(os.path.join(HERE, "..", "..", "assets", "demo", "Gridline.gridline.json"))


def fmt_threads(run):
    return ", ".join("%s %.2f" % (t["name"], t["cpu_percent"]) for t in cpurun.top_threads(run))


def fmt_split(run, kind):
    return ", ".join("%s %.2f" % (t["name"], t["cpu_percent"]) for t in cpurun.top_split(run, kind)) or "none"


def fmt_nodes(run, limit=6):
    nodes = cpurun.running_nodes(run)
    if not nodes:
        return "none"
    names = ["%s (%s)" % (n["name"] or n["type"], n["type"]) for n in nodes[:limit]]
    more = len(nodes) - limit
    return "%d: %s%s" % (len(nodes), ", ".join(names), " +%d" % more if more > 0 else "")


def fmt_wakes(run):
    rows = [w for w in run.get("worker_wakeups", []) if w["per_s"] >= 0.5]
    return ", ".join("%s %.0f/s" % (w["name"], w["per_s"]) for w in rows) or "none"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--app", required=True)
    ap.add_argument("--out", required=True, help="folder for the tables and JSON")
    ap.add_argument("--scratch", help="folder for the runs' config and caches (default <out>/runs)")
    ap.add_argument("--layouts", default=",".join(k for k, _ in cpurun.LAYOUTS))
    ap.add_argument("--projects", default="demo,empty,large")
    ap.add_argument("--states", default=",".join(cpurun.STATES))
    ap.add_argument("--seconds", type=float, default=20)
    ap.add_argument("--settle", type=float, default=10)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    scratch = a.scratch or os.path.join(a.out, "runs")
    os.makedirs(scratch, exist_ok=True)
    names = dict(cpurun.LAYOUTS)
    states = a.states.split(",")
    projects = {}
    for p in a.projects.split(","):
        if p == "demo":
            projects[p] = DEMO
        elif p == "empty":
            projects[p] = cpurun.make_empty(DEMO, os.path.join(scratch, "empty.gridline.json"))
        elif p == "large":
            projects[p] = cpurun.make_large(DEMO, os.path.join(scratch, "large.gridline.json"))
        else:
            sys.exit("unknown project %s (demo, empty, large)" % p)
    results = []
    for pname, ppath in projects.items():
        for lay in a.layouts.split(","):
            t0 = time.time()
            folder = os.path.join(scratch, "%s-%s" % (pname, lay))
            try:
                rep = cpurun.run_report(a.app, ppath, lay, states, folder, a.seconds, a.settle)
            except Exception as e:  # noqa: BLE001 (reported, the sweep goes on)
                print("FAILED %s %s: %s" % (pname, lay, e), flush=True)
                results.append({"project": pname, "layout": lay, "error": str(e)})
                continue
            with open(os.path.join(a.out, "%s-%s.json" % (pname, lay)), "w") as f:
                json.dump(rep, f, indent=1)
            for run in rep["runs"]:
                results.append({"project": pname, "layout": lay, "run": run, "meta": {k: v for k, v in rep.items() if k != "runs"}})
                own, system, total = cpurun.split(run)
                print("%-6s %-8s %-9s cpu %6.2f%% (own %.2f, system %.2f)  fps %5.1f  dsp nodes %d  (%s)" % (
                    pname, names.get(lay, lay), run["state"], total, own, system, run["frames_per_s"],
                    run["nodes_running"], fmt_threads(run)), flush=True)
            print("  (%.0f s)" % (time.time() - t0), flush=True)
    # Tables, one per project.
    meta = next((r["meta"] for r in results if "meta" in r), {})
    lines = ["# Idle CPU sweep", "",
             "Machine: %s, %s cores; Gridline %s (%s build); audio driver %s; %g s recorded per state after %g s settling." % (
                 meta.get("machine", "?"), meta.get("cores", "?"), meta.get("version", "?"), meta.get("build", "?"),
                 meta.get("audio_driver", "?"), a.seconds, a.settle),
             "CPU is the whole process, percent of one core; Own is Gridline's own threads (gl-*, and of the audio device",
             "thread only Gridline's callback), System the rest (the audio driver's share, CoreAudio, the display link, SDL's",
             "and the system's threads). Threads: the busiest, percent of one core.", ""]
    for pname in projects:
        lines += ["## %s project" % pname.capitalize(), "",
                  "| Layout | State | CPU % | Own % | System % | Frames/s | Top own threads | Top system threads | Nodes running DSP | Worker wakeups | Syscalls/s |",
                  "|---|---|---|---|---|---|---|---|---|---|---|"]
        for r in results:
            if r["project"] != pname:
                continue
            if "error" in r:
                lines.append("| %s | (failed) | | | | | %s | | | | |" % (names.get(r["layout"], r["layout"]), r["error"].splitlines()[0]))
                continue
            run = r["run"]
            own, system, total = cpurun.split(run)
            lines.append("| %s | %s | %.2f | %.2f | %.2f | %.1f | %s | %s | %s | %s | %s |" % (
                names.get(r["layout"], r["layout"]), run["state"], total, own, system, run["frames_per_s"],
                fmt_split(run, "own"), fmt_split(run, "system"), fmt_nodes(run), fmt_wakes(run),
                "%.0f" % run["process"]["syscalls_unix_per_s"] if "syscalls_unix_per_s" in run["process"] else "n/a"))
        lines.append("")
    with open(os.path.join(a.out, "SWEEP.md"), "w") as f:
        f.write("\n".join(lines))
    print("tables: %s" % os.path.join(a.out, "SWEEP.md"))
    return 1 if any("error" in r for r in results) else 0


if __name__ == "__main__":
    sys.exit(main())
