#!/usr/bin/env python3
"""Writes the test jobs as a GitHub Actions output line: plan.py <reduced|full> <repeats>.

yabridge 5.1.1 states that it works with Wine up to 9.21; 9.22 and later have a known input offset
(upstream issue 382). The matrix tests that version, the distribution's own and the current one on
every display, and the versions around the boundary on one display.
"""
import json
import sys

WINES = {"staging-9.21": "staging:9.21", "distro": "distro", "staging-11.21": "staging:11.21"}
BOUNDARY = {"staging-9.22": "staging:9.22", "staging-10.20": "staging:10.20"}
REAL = "Surge XT,Dexed,Nekobi"  # Kars has no interface of its own


def job(wine, display, scale, run, real="", control=0, modes="inproc,sandbox"):
    return {"wine": wine, "wine_install": {**WINES, **BOUNDARY}[wine], "display": display, "scale": scale, "run": run, "formats": "vst3,vst2,clap", "modes": modes, "real": real, "control": control}


def main():
    kind = sys.argv[1] if len(sys.argv) > 1 else "reduced"
    repeats = max(1, int(sys.argv[2])) if len(sys.argv) > 2 else 1
    jobs = []
    for run in range(1, repeats + 1):
        if kind != "full":
            jobs.append(job("staging-9.21", "xvfb-openbox", 1, run, real=REAL, control=1))
            continue
        for wine in WINES:
            for display in ("xvfb-openbox", "xvfb-bare", "weston-x11"):
                for scale in (1, 2):
                    first = display == "xvfb-openbox" and scale == 1
                    # The control host where a comparison can tell something: one per display.
                    control = 1 if scale == 1 else 0
                    jobs.append(job(wine, display, scale, run, real=REAL if first else "", control=control))
            jobs.append(job(wine, "weston-wayland", 1, run, modes="inproc"))
        for wine in BOUNDARY:
            jobs.append(job(wine, "xvfb-openbox", 1, run, real=REAL, control=1))
    print("matrix=" + json.dumps(jobs))


if __name__ == "__main__":
    main()
