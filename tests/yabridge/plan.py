#!/usr/bin/env python3
"""Writes the test jobs as a GitHub Actions output line: plan.py <reduced|full> <repeats>.

yabridge 5.1.1 states that it works with Wine up to 9.21; 9.22 and later have a known input offset
(upstream issue 382). The matrix tests that version, the distribution's own and the current one.
"""
import json
import sys

WINES = {"staging-9.21": "staging:9.21", "distro": "distro", "staging-11.21": "staging:11.21"}
REAL = "Surge XT,Dexed,Kars,Nekobi"


def job(wine, display, scale, run, real="", control=0, modes="inproc,sandbox"):
    return {"wine": wine, "wine_install": WINES[wine], "display": display, "scale": scale, "run": run, "formats": "vst3,vst2,clap", "modes": modes, "real": real, "control": control}


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
                    jobs.append(job(wine, display, scale, run, real=REAL if first else "", control=1 if first else 0))
            jobs.append(job(wine, "weston-wayland", 1, run, modes="inproc"))
    print("matrix=" + json.dumps(jobs))


if __name__ == "__main__":
    main()
