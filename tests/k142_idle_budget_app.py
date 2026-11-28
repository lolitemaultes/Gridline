#!/usr/bin/env python3
# Gridline - K-142 (0.7.8): the demo song left idle (transport stopped, no input) in each layout, in the
# real app (hidden, quiet, scratch config and HOME, SDL's dummy audio driver, no plugin scan), measured
# by its own CPU report (--cpu-report, src/app/CpuReport.h), window visible and then hidden:
#  * frames: at most the idle pace (IdlePace: 10 a second) while visible, none while hidden;
#  * DSP: no instrument or effect runs its process() (everything is silent, past its tail);
#  * workers: the audio worker pool never wakes, and no other worker wakes more than 4 times a
#    second (no timed wait shorter than 250 ms without work);
#  * processor time of Gridline's own threads (the report's "cpu_split", K-142z): every thread
#    Gridline makes (gl-*, the interface thread whole) and, of the audio device thread the driver
#    makes, the time inside Gridline's callback: at most 2 % of one core visible and 0.5 % hidden
#    (the 0.7.8 budget, measured on an i7-10700K Release build), times CPU_SCALE (default 1.5) for
#    the margin. GRIDLINE_K142_CPU_SCALE sets another; 0 reports the time without judging it.
#    Hidden, the interface thread alone at most 0.1 % (times CPU_SCALE): it sleeps until it has work.
#  * the system's threads (the audio driver's own share of its thread, CoreAudio, the display link,
#    SDL's and the system's threads) are printed, not budgeted: Gridline cannot make them cheaper.
#    They fail the test only past a generous sanity bound that means Gridline drives them
#    abnormally, and a display link running while hidden fails at once (nothing is drawn then).
# The structural checks count and never time, so they hold on any machine.
# A software OpenGL renderer (the report's gl_software: Mesa's llvmpipe on the Linux CI, which has
# no graphics card) draws every frame on the processor: in the interface thread's OpenGL calls
# and in the renderer's own threads (llvmpipe-N), the work a graphics card does elsewhere. There,
# visible, the interface thread's time inside the calls that draw and present the frames
# (main_gl_percent) is the renderer's, not Gridline's, and the renderer's threads are left out of
# the system's sanity bound; both are printed. Everything else is judged as on a graphics card,
# and hidden draws nothing, so it is judged whole.
#
# Label k142_expected_fail stays until the coordinator has seen it pass on main; it lists every
# check that failed and the numbers, so it shows what is left.
#   python3 k142_idle_budget_app.py <gridline-app> <tools/cpu> <demo song> <out dir> [layout,...]
import os
import sys

APP, TOOLS, DEMO, OUT = sys.argv[1:5]
ONLY = sys.argv[5].split(",") if len(sys.argv) > 5 and sys.argv[5] else []
sys.path.insert(0, TOOLS)
import cpurun  # noqa: E402

SETTLE = float(os.environ.get("GRIDLINE_K142_SETTLE", "10"))
SECONDS = float(os.environ.get("GRIDLINE_K142_SECONDS", "15"))
SCALE = float(os.environ.get("GRIDLINE_K142_CPU_SCALE", "1.5"))
IDLE_FPS = 10.0
BUDGET = {"visible": 2.0, "hidden": 0.5}  # Gridline's own threads, percent of one core (times SCALE)
# K-142e (0.7.9): the audio graph sleeps in O(1) without touching its nodes once quiescent, so hidden
# is judged at the original strict 0.5 % x SCALE budget on every platform (including macOS).
# The interface thread alone, hidden (percent of one core, times SCALE): it draws nothing and sleeps until
# input, a wake from another thread or a duty (a changed project's backups, the report's steps), waking
# only for the freeze watchdog every few seconds. Measured near 0.01 %; a loop that looks for events on a
# timer again lands far past it.
MAIN_HIDDEN = 0.1
# The system's threads, percent of one core (not scaled). Measured on this Mac (K-142z, 2026-11-22, five
# runs of every layout): hidden 0.25 to 0.6 (nearly all the dummy audio driver's waking between
# callbacks), visible 0.4 to 0.8 (that, and the window system's unnamed threads). About three times the
# most: an audio driver woken far more often than its buffer needs, or a display link or the window
# system kept busy by Gridline, lands well past it, while a busy machine does not.
SYSTEM_SANITY = {"visible": 2.5, "hidden": 1.5}
MAX_WAKES_PER_S = 4.0
RENDERER_THREADS = ("llvmpipe", "SwiftShader")  # a software renderer's drawing threads

failures = []


def check(cond, what):
    if not cond:
        failures.append(what)
        print("FAIL " + what, flush=True)


def fmt(rows):
    return ", ".join("%s %.2f%%" % (t["name"], t["cpu_percent"]) for t in rows) or "none"


os.makedirs(OUT, exist_ok=True)
for lay, name in cpurun.LAYOUTS:
    if ONLY and lay not in ONLY and name.lower() not in ONLY:
        continue
    try:
        rep = cpurun.run_report(APP, DEMO, lay, ["visible", "hidden"], os.path.join(OUT, lay), SECONDS, SETTLE)
    except Exception as e:  # noqa: BLE001
        check(False, "%s: no report (%s)" % (name, str(e).splitlines()[0]))
        continue
    soft = bool(rep.get("gl_software"))
    for run in rep["runs"]:
        st = run["state"]
        tag = "%s %s" % (name, st)
        own, system, total = cpurun.split(run)
        sp = run.get("cpu_split") or {}
        own_judged, system_judged, scoped = own, system, ""
        if soft and st == "visible":
            gl = sp.get("main_gl_percent", 0.0)
            drawn = sum(t["cpu_percent"] for t in sp.get("system_threads", []) if t["name"].startswith(RENDERER_THREADS))
            own_judged, system_judged = own - gl, system - drawn
            scoped = " (software OpenGL %s: its calls on the interface thread %.2f%%, its threads %.2f%%, not Gridline's)" % (
                rep.get("gl_renderer", "?"), gl, drawn)
        fps = run["frames_per_s"]
        print("%s: own %.2f%% of one core, system %.2f%%, total %.2f%%; %.1f frames/s, %d nodes ran DSP (%d calls), pool wakeups %d" % (
            tag, own, system, total, fps, run["nodes_running"], run["dsp_calls_total"], run["audio"]["pool_wakeups"]) + scoped, flush=True)
        print("    own: %s" % fmt(cpurun.top_split(run, "own", 6)), flush=True)
        print("    system: %s" % fmt(cpurun.top_split(run, "system", 6)), flush=True)
        if run.get("main_loop"):
            ml = run["main_loop"]
            print("    interface loop: %.1f sleeps/s (%.1f timed out, %.1f events, %.1f wakes, %.1f changes), %.1f turns/s, waiting %.3f%%" % (
                ml["sleeps_per_s"], ml["timeouts_per_s"], ml["events_per_s"], ml["pokes_per_s"], ml["busy_per_s"], ml["turns_per_s"],
                ml["wait_cpu_percent"]), flush=True)
        check(bool(sp), "%s: the report has no own and system split" % tag)
        limit = IDLE_FPS * 1.05 if st == "visible" else 0.5
        check(fps <= limit, "%s: %.1f frames/s (at most %.1f)" % (tag, fps, limit))
        nodes = cpurun.running_nodes(run)
        check(not nodes, "%s: %d instruments and effects ran DSP while silent: %s" % (
            tag, len(nodes), ", ".join("%s %s (%d blocks)" % (n["name"], n["type"], n["dsp_calls"]) for n in nodes[:12])))
        check(run["audio"]["pool_wakeups"] == 0, "%s: the audio worker pool woke %d times" % (tag, run["audio"]["pool_wakeups"]))
        busy = [w for w in run.get("worker_wakeups", []) if w["per_s"] > MAX_WAKES_PER_S]
        check(not busy, "%s: workers woke without work: %s" % (tag, ", ".join("%s %.0f/s" % (w["name"], w["per_s"]) for w in busy)))
        if st == "hidden" and sys.platform == "darwin":
            link = sum(t["cpu_percent"] for t in run["threads_by_name"] if t["name"] == "CVDisplayLink")
            check(link == 0, "%s: a display link runs while hidden (%.3f%%)" % (tag, link))
        if SCALE > 0:
            cap = BUDGET[st] * SCALE
            check(own_judged <= cap, "%s: Gridline's own threads %.2f%% of one core (budget %.2f%%, %.1f x %.2f%%): %s%s" % (
                tag, own_judged, cap, SCALE, BUDGET[st], fmt(cpurun.top_split(run, "own", 6)), scoped))
            if st == "hidden":
                main = sum(t["cpu_percent"] for t in sp.get("own_threads", []) if t["name"] == "gl-main")
                ml = run.get("main_loop") or {}
                check(main <= MAIN_HIDDEN * SCALE, "%s: the interface thread %.3f%% of one core (budget %.2f%%, %.1f x %.2f%%; it slept %.1f times a second)" % (
                    tag, main, MAIN_HIDDEN * SCALE, SCALE, MAIN_HIDDEN, ml.get("sleeps_per_s", -1)))
            scap = SYSTEM_SANITY[st]
            check(system_judged <= scap, "%s: the system's threads %.2f%% of one core (sanity bound %.2f%%): Gridline drives them abnormally? %s%s" % (
                tag, system_judged, scap, fmt(cpurun.top_split(run, "system", 6)), scoped))

if failures:
    print("\n%d check(s) failed:" % len(failures))
    for f in failures:
        print("  " + f)
    sys.exit(1)
print("k142 idle budget: every layout within the budget (Gridline's own threads)")
