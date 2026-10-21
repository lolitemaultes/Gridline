#!/usr/bin/env python3
"""Gridline J-67 - scroll regression check (smooth scrolling, elastic edges).

    python3 tools/scroll_check.py build-cmake/gridline-app [--keep <dir>]

Runs the app offscreen with a throwaway config (XDG_CONFIG_HOME in a temp
dir) and the dev-input scripts in tests/scroll/: trackpad flings (with OS-like
gaps between events) and wheel notches driven into the top / left edges of the
playlist, the piano roll and the browser list, with `trace on` printing the
drawn scroll positions every frame (--fixed-dt 16.667: deterministic).

A "jump" is the view reversing on a frame where the input does not ask for
that direction (drawn past its limit one frame, snapped back the next). The
check fails when any jump is larger than 2 px or the view is drawn more than
60 px past an edge. Before J-67 the playlist jumped 46 px per wheel notch at
its top, the piano roll 60 px at its left edge, and scrolling the piano roll
past its top re-centred it (a 2520 px jump).
"""
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
COLS = ["playlist.x", "playlist.y", "piano.x", "piano.y", "window.x", "window.y"]
PAT = re.compile(r"trace frame (\d+): wheel (\S+) (\S+) playlist (\S+) (\S+) piano (\S+) (\S+) window (.*) (\S+) (\S+)$")
RUNS = [("playlist", "playlist_edges.txt", [0, 1]), ("piano", "piano_edges.txt", [2, 3, 5])]
MAX_JUMP, MAX_OVER = 2.0, 60.0


def trace(app, view, script, cfg, shot):
    env = dict(os.environ, XDG_CONFIG_HOME=cfg, GRIDLINE_REDUCED_MOTION="0", GRIDLINE_NO_UI_SOUNDS="1")
    out = subprocess.run([app, "--show", view, "--size", "1280x800", "--fixed-dt", "16.667", "--dev-input", script,
                          "--screenshot", shot, "--frames", "220"], env=env, capture_output=True, text=True, timeout=300).stdout
    rows = []
    for line in out.splitlines():
        m = PAT.match(line)
        if m:
            g = m.groups()
            rows.append((int(g[0]), (float(g[1]), float(g[2])), [float(g[3]), float(g[4]), float(g[5]), float(g[6]), float(g[8]), float(g[9])]))
    return rows


def jumps(rows, c):
    n, worst, prev = 0, 0.0, 0
    for i in range(1, len(rows)):
        d = rows[i][2][c] - rows[i - 1][2][c]
        if abs(d) < 1e-6:
            continue
        s = 1 if d > 0 else -1
        if prev and s != prev:
            w = rows[i][1][0] if c % 2 == 0 else rows[i][1][1]
            asked = (-1 if w > 0 else 1) if w != 0 else 0  # wheel up / left scrolls towards 0
            if asked != s:
                n += 1
                worst = max(worst, abs(d))
        prev = s
    return n, worst


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    app = os.path.abspath(sys.argv[1])
    keep = sys.argv[sys.argv.index("--keep") + 1] if "--keep" in sys.argv else None
    ok = True
    with tempfile.TemporaryDirectory() as tmp:
        for view, script, cols in RUNS:
            rows = trace(app, view, os.path.join(ROOT, "tests", "scroll", script), os.path.join(tmp, "cfg"), os.path.join(tmp, "shot.ppm"))
            if keep:
                os.makedirs(keep, exist_ok=True)
                with open(os.path.join(keep, view + ".trace.txt"), "w") as f:
                    for r in rows:
                        f.write("%d %s %s\n" % (r[0], r[1], r[2]))
            if len(rows) < 150:
                print("FAIL %s: only %d trace frames" % (view, len(rows)))
                ok = False
                continue
            for c in cols:
                n, worst = jumps(rows, c)
                over = -min(r[2][c] for r in rows)
                good = worst <= MAX_JUMP and over <= MAX_OVER
                ok = ok and good
                print("%s %-11s unasked reversals %2d, largest %6.2f px, drawn past the edge %5.2f px" % ("ok  " if good else "FAIL", COLS[c], n, worst, max(0.0, over)))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
