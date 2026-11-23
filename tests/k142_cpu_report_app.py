#!/usr/bin/env python3
# Gridline - K-142 (0.7.8): the CPU report (`gridline-app --cpu-report`, src/app/CpuReport.h) in the real
# app (hidden, quiet, scratch config and HOME, SDL's dummy audio driver): a short run of the demo song in
# Studio through the four window states writes a full report and the run ends by itself, its changes
# discarded: one run per state in order, the process's processor time, the interface and audio threads
# under their names, frames drawn, audio device callbacks and engine blocks, every instrument and effect
# of the song listed with its type and DSP calls, the file system and wakeup counts.
#   python3 k142_cpu_report_app.py <gridline-app> <tools/cpu> <demo song> <out dir>
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


rep = cpurun.run_report(APP, DEMO, "studio", cpurun.STATES, os.path.join(OUT, "run"), 3, 2)
check(rep.get("format") == "gridline-cpu-report-1", "report format")
check(rep.get("layout") == "studio", "layout recorded")
check(bool(rep.get("machine")) and rep.get("cores", 0) > 0, "machine and cores recorded")
check(bool(rep.get("gl_renderer")) and isinstance(rep.get("gl_software"), bool),
      "OpenGL renderer recorded (%s, %s)" % (rep.get("gl_renderer"), "software" if rep.get("gl_software") else "hardware"))
runs = rep.get("runs", [])
check([r["state"] for r in runs] == cpurun.STATES, "one run per state, in order: %s" % [r["state"] for r in runs])
for r in runs:
    st = r["state"]
    p = r["process"]
    check(p["cpu_seconds"] > 0 and p["cpu_percent_of_one_core"] > 0, "%s: process processor time" % st)
    check(2.5 <= r["seconds"] <= 6, "%s: recorded about 3 s (%.2f)" % (st, r["seconds"]))
    names = {t["name"] for t in r["threads_by_name"]}
    if sys.platform in ("darwin", "linux"):
        check("gl-main" in names, "%s: the interface thread is named" % st)
        check("gl-audio" in names, "%s: the audio thread is named" % st)
    if sys.platform == "darwin" and st in ("minimised", "hidden"):
        # 0.7.8: nothing shown, no display link runs (ours nor the GL context's own)
        link = sum(t["cpu_percent"] for t in r["threads_by_name"] if t["name"] == "CVDisplayLink")
        check(link == 0, "%s: no display link thread runs (%.3f %%)" % (st, link))
    # K-142z: Gridline's own threads and the system's, adding up to the whole process.
    sp = r.get("cpu_split") or {}
    check(all(k in sp for k in ("own_percent", "system_percent", "total_percent", "audio_callback_percent", "audio_driver_percent",
                                "main_frames_percent", "main_gl_percent", "own_threads", "system_threads")), "%s: own and system split" % st)
    if sp:
        check(abs(sp["own_percent"] + sp["system_percent"] - sp["total_percent"]) <= max(0.05, 0.02 * sp["total_percent"]),
              "%s: own %.3f + system %.3f = total %.3f" % (st, sp["own_percent"], sp["system_percent"], sp["total_percent"]))
        check(0 <= sp["main_gl_percent"] <= sp["main_frames_percent"] + 1e-9,
              "%s: the frames' OpenGL calls (%.3f %%) are timed, within the frames (%.3f %%)" % (st, sp["main_gl_percent"], sp["main_frames_percent"]))
        if sys.platform in ("darwin", "linux"):
            check(0 < sp["audio_callback_percent"] <= sp["audio_thread_percent"] + 1e-9,
                  "%s: the audio callback (%.3f %%) is timed, within its thread (%.3f %%)" % (st, sp["audio_callback_percent"], sp["audio_thread_percent"]))
            own_names = {t["name"] for t in sp["own_threads"]}
            sys_names = {t["name"] for t in sp["system_threads"]}
            check(all(n.startswith("gl-") for n in own_names) and not any(n.startswith("gl-") and n != "gl-audio (driver)" for n in sys_names),
                  "%s: own threads are Gridline's (%s)" % (st, ", ".join(sorted(own_names))))
    check(r["audio"]["device_callbacks"] > 0 and r["audio"]["engine_blocks"] > 0, "%s: audio blocks counted" % st)
    check(r["audio"]["sample_rate"] > 0, "%s: sample rate" % st)
    check(len(r["nodes"]) >= 20 and all(n["type"] for n in r["nodes"]), "%s: every instrument and effect listed (%d)" % (st, len(r["nodes"])))
    check(sum(n["dsp_calls"] for n in r["nodes"]) == r["dsp_calls_total"], "%s: DSP calls add up" % st)
    check(set(r["fs_calls"]) == {"exists", "is_file", "is_dir"}, "%s: file system counts" % st)
    check(isinstance(r["worker_wakeups"], list), "%s: worker wakeups" % st)
check(runs and runs[0]["frames"] > 0, "frames drawn while visible")
log = open(os.path.join(OUT, "run", "run.log")).read()
check("cpu-report: written" in log, "the report says where it is written")
if failures:
    print("%d check(s) failed" % len(failures))
    sys.exit(1)
print("k142 cpu report: complete")
