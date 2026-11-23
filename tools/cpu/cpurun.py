#!/usr/bin/env python3
# Gridline - the idle CPU report (`gridline-app --cpu-report`, src/app/CpuReport.h) run from scripts:
# one quiet hidden run of the app in a scratch folder of its own (config, HOME, caches, temp files,
# SDL's dummy audio driver, no plugin scan, no plugin folders), and the test projects it is run on
# (the demo song, an empty project, a large project). Used by tools/cpu/sweep.py and the
# k142_idle_budget_app test.
import copy
import json
import os
import shutil
import subprocess

# Layout ids as the app reads them (--layout), with the names users see.
LAYOUTS = [("studio", "Studio"), ("live", "Clips"), ("logic", "Smooth"), ("tracker", "Tracker")]
STATES = ["visible", "unfocused", "minimised", "hidden"]


def scratch_env(folder):
    """The environment of a quiet, isolated run whose files all stay in `folder`."""
    for sub in ("cfg/gridline", "cache", "xdgcache", "tmp"):
        os.makedirs(os.path.join(folder, sub), exist_ok=True)
    env = dict(os.environ)
    env.update({
        "GRIDLINE_CONFIG_HOME": os.path.join(folder, "cfg"),
        "HOME": os.path.join(folder, "cfg"),
        "XDG_CACHE_HOME": os.path.join(folder, "xdgcache"),
        "GRIDLINE_CACHE_DIR": os.path.join(folder, "cache"),
        "TMPDIR": os.path.join(folder, "tmp"),
        "SDL_AUDIODRIVER": "dummy",
        "GRIDLINE_QUIET_WINDOWS": "1",
        "GRIDLINE_NO_PLUGIN_SCAN": "1",
        "VST_PATH": "", "VST3_PATH": "", "CLAP_PATH": "", "LV2_PATH": "",
    })
    for k in ("GRIDLINE_NO_IDLE_PACING", "GRIDLINE_IDLE_PACING"):
        env.pop(k, None)
    # No plugin folders at all (not even the default ones under the scratch HOME).
    prefs = {"version": 1, "extraFolders": [], "noDefaultFolders": ["vst3", "vst2", "clap", "lv2"], "disabledFormats": ["lv2", "au"]}
    with open(os.path.join(folder, "cfg", "gridline", "plugin-manager.json"), "w") as f:
        json.dump(prefs, f)
    return env


def run_report(app, project, layout, states, folder, seconds, settle, size="1440x900", timeout=None):
    """Runs the app once on `project` in `layout`, recording each state in turn. Returns the report
    (a dict) or raises RuntimeError with the run's output."""
    if os.path.isdir(folder):
        shutil.rmtree(folder)
    env = scratch_env(folder)
    out = os.path.join(folder, "report.json")
    cmd = [app]
    if project:
        cmd.append(project)
    cmd += ["--hidden", "--no-ui-sounds", "--no-update-check", "--size", size, "--layout", layout,
            "--cpu-report", str(seconds), "--cpu-settle", str(settle), "--cpu-states", ",".join(states),
            "--cpu-report-out", out]
    if timeout is None:
        timeout = 120 + len(states) * (seconds + settle) * 2
    p = subprocess.run(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout)
    log = p.stdout.decode("utf-8", "replace")
    with open(os.path.join(folder, "run.log"), "w") as f:
        f.write(log)
    if p.returncode != 0 or not os.path.exists(out):
        raise RuntimeError("gridline-app exited %d without a report:\n%s" % (p.returncode, log[-4000:]))
    with open(out) as f:
        return json.load(f)


# ---- test projects ----

def make_empty(demo_path, out_path):
    """The demo song with nothing in it: no channels, patterns, clips or effects (its mixer tracks stay)."""
    with open(demo_path) as f:
        d = json.load(f)
    for k in ("channels", "patterns", "playlist", "automations", "markers", "links", "modLinks"):
        if k in d:
            d[k] = []
    for m in d.get("mixer", []):
        m["slots"] = []
        m["sends"] = []
    d.setdefault("info", {})["title"] = "Empty"
    with open(out_path, "w") as f:
        json.dump(d, f)
    return out_path


def make_large(demo_path, out_path, channels=200, clips=2000):
    """The demo song grown to `channels` channels and `clips` playlist clips: its channels copied
    (with their notes in every pattern) and its arrangement repeated along the timeline."""
    with open(demo_path) as f:
        d = json.load(f)
    next_id = int(d["nextId"])
    base = list(d["channels"])
    copies = []
    while len(base) + len(copies) < channels:
        src = base[len(copies) % len(base)]
        c = copy.deepcopy(src)
        c["id"] = next_id
        c["name"] = "%s %d" % (src.get("name", "Channel"), len(copies) // len(base) + 2)
        next_id += 1
        copies.append((src["id"], c))
    d["channels"] = base + [c for _, c in copies]
    for p in d.get("patterns", []):
        notes = p.get("notes", {})
        for src_id, c in copies:
            if str(src_id) in notes:
                notes[str(c["id"])] = copy.deepcopy(notes[str(src_id)])
    pl = list(d.get("playlist", []))
    if pl:
        span = max(int(c["start"]) + int(c["length"]) for c in pl)
        rep = 1
        out = list(pl)
        while len(out) < clips:
            for c in pl:
                if len(out) >= clips:
                    break
                n = dict(c)
                n["id"] = next_id
                next_id += 1
                n["start"] = int(c["start"]) + rep * span
                out.append(n)
            rep += 1
        d["playlist"] = out
    d["nextId"] = next_id
    d.setdefault("info", {})["title"] = "Large"
    with open(out_path, "w") as f:
        json.dump(d, f)
    return out_path


# ---- reading a report ----

def top_threads(run, n=4, floor=0.05):
    rows = [t for t in run.get("threads_by_name", []) if t.get("cpu_percent", 0) >= floor]
    return rows[:n]


def running_nodes(run):
    return sorted((x for x in run.get("nodes", []) if x.get("dsp_calls", 0) > 0), key=lambda x: -x["dsp_calls"])


def split(run):
    """K-142z: (own %, system %, total %) of one core: Gridline's own threads (gl-*, the audio device
    thread's callback only) and the system's (the audio driver's share, CoreAudio, the display link,
    SDL's and the system's threads). A report without the split counts the whole process as own."""
    s = run.get("cpu_split")
    total = run["process"]["cpu_percent_of_one_core"]
    if not s:
        return total, 0.0, total
    return s["own_percent"], s["system_percent"], total


def top_split(run, kind, n=4, floor=0.02):
    """The busiest of Gridline's own threads (kind "own") or the system's ("system")."""
    rows = (run.get("cpu_split") or {}).get(kind + "_threads", [])
    return [t for t in rows if t.get("cpu_percent", 0) >= floor][:n]
