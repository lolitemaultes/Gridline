#!/usr/bin/env python3
# Gridline - dev-input path lint: every path a test script hands the app's dev-input "save", "shot" or "open"
# command is quoted. The dev-input parser splits a line at spaces unless the words are in double quotes, so an
# unquoted path under a folder with a space in its name ("untitled folder 3") is cut at the space: the save
# lands somewhere else (once, a file outside the build folder) and the test fails or, worse, passes on a
# stale file.
#   python3 devinput_paths_lint.py <tests dir>
# Prints each unquoted use with its file and line, and exits 1 when there is any.
import os
import re
import sys

CMDS = r"(?:save|shot|open)"
# A dev-input line in a CMake string: "<frame> save ${OUT}/x" (the frame may be "${f}"), at("save ${OUT}/x"),
# or a bare absolute path.
CMAKE = [
    re.compile(r"(?:[0-9]+|\}) " + CMDS + r" +(?:\$\{|/)"),
    re.compile(r"at\(\s*\"" + CMDS + r" +(?:\$\{|/)"),
    # "dragenter <x> <y> <file>...": a file dragged in from another program (plugin:/clipsrc: items are not paths)
    re.compile(r"dragenter +\S+ +\S+ +(?:\$\{|/)"),
]
# Python: "save " + path, f"save {path}", "save %s" % path, "shot @SHOT@" placeholders.
PY = [
    re.compile(r"[\"'](?:[0-9]+ )?" + CMDS + r" [\"']\s*\+"),
    re.compile(r"[0-9}] " + CMDS + r" +(?:\{|%s|@\w+@|/)"),
    re.compile(r"[\"']" + CMDS + r" +(?:\{|%s|@\w+@|/)"),
]


def scan(root):
    bad = []
    for dirpath, _dirs, files in os.walk(root):
        for name in sorted(files):
            if name.endswith(".cmake"):
                pats = CMAKE
            elif name.endswith(".py"):
                pats = PY
            else:
                continue
            path = os.path.join(dirpath, name)
            if os.path.abspath(path) == os.path.abspath(__file__):
                continue
            with open(path, encoding="utf-8", errors="replace") as f:
                for n, line in enumerate(f, 1):
                    if line.lstrip().startswith("#"):
                        continue
                    if any(p.search(line) for p in pats):
                        bad.append("%s:%d: %s" % (os.path.relpath(path, root), n, line.strip()[:160]))
    return bad


def selftest():
    # The patterns catch the forms that broke, and leave the quoted ones alone.
    cm = lambda s: any(p.search(s) for p in CMAKE)
    py = lambda s: any(p.search(s) for p in PY)
    cases = [
        (cm, r'run(a "4 typing off\n20 save ${OUT}/fresh.gridline.json\n")', True),
        (cm, r'${clicks}${f} save ${OUT}/p.json\n', True),
        (cm, 'at("shot ${OUT}/${name}.ppm")', True),
        (cm, '60 save ${OUT}/moved.gridline.json', True),
        (cm, r'run(a "20 save \"${OUT}/fresh.gridline.json\"\n")', False),
        (cm, r'at("shot \"${OUT}/${name}.ppm\"")', False),
        (cm, 'message(FATAL_ERROR "FAIL (shot ${scale}x)")', False),
        (cm, r'"15 layout open Browser\n"', False),
        (cm, 'at("dragenter ${sx2} ${ry1} ${WAV}")', True),
        (cm, r'at("dragenter ${sx2} ${ry1} \"${WAV}\"")', False),
        (cm, r'20 dragenter ${x} ${y} plugin:gl.synth\n', False),
        (py, 'sc.at("save " + song, 10)', True),
        (py, '"60 shot @SHOT@\\n"', True),
        (py, 'f"20 save {out}\\n"', True),
        (py, "sc.at('save \"%s\"' % song, 10)", False),
        (py, '"60 shot \\"@SHOT@\\"\\n"', False),
    ]
    ok = True
    for fn, s, want in cases:
        if fn(s) != want:
            print("lint self-test: %r should %sbe flagged" % (s, "" if want else "not "))
            ok = False
    return ok


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.abspath(__file__))
    if not selftest():
        return 1
    bad = scan(root)
    for b in bad:
        print("unquoted dev-input path: " + b)
    if bad:
        print("FAIL: %d dev-input save/shot/open/dragenter paths are not in double quotes" % len(bad))
        return 1
    print("ok - every dev-input save/shot/open/dragenter path in %s is quoted" % root)
    return 0


if __name__ == "__main__":
    sys.exit(main())
