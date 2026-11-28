#!/usr/bin/env python3
# Gridline - K-428: right-click pattern context menu & inline rename everywhere patterns appear:
#   1. Top Bar Pattern selector (Studio)
#   2. Top Bar Pattern dropdown row (nested popup inside ##patpop)
#   3. Browser -> Current project -> Patterns
#   4. Playlist header breadcrumb (Playlist > Arrangement > Pattern)
#   5. Tracker Song page -> Pattern Bank
# Each site opens patternContextMenu with Rename..., renames in one undo step, and Undo restores it.
#   python3 k428_pattern_rename_app.py <gridline-app> <song.gridline.json> <out dir>
import json
import os
import re
import shutil
import subprocess
import sys

APP, DEMO, OUT = sys.argv[1], sys.argv[2], sys.argv[3]
failures = []
passed = 0


def fail(msg):
    failures.append(msg)
    print("FAIL: " + msg)


def ok(cond, msg):
    global passed
    if cond:
        passed += 1
        print("ok " + msg)
    else:
        fail(msg)


shutil.rmtree(OUT, ignore_errors=True)
os.makedirs(OUT)


def run(tag, layout, script, frames, extra_env=None):
    cfg = os.path.join(OUT, "cfg_" + tag)
    shutil.rmtree(cfg, ignore_errors=True)
    os.makedirs(os.path.join(cfg, "gridline"))
    os.makedirs(os.path.join(cfg, "home"))
    with open(os.path.join(cfg, "gridline", "settings.json"), "w") as f:
        json.dump(
            {
                "format": "gridline-settings",
                "values": {
                    "ui.layoutChosen": True,
                    "ui.layoutDev": True,
                    "ui.layout": layout,
                    "keys.profile": layout,
                    "ui.menuStyle": layout,
                },
            },
            f,
        )
    song = os.path.join(cfg, "song.gridline.json")
    shutil.copyfile(DEMO, song)
    sp = os.path.join(OUT, tag + ".txt")
    with open(sp, "w") as f:
        f.write(script)
    env = dict(
        os.environ,
        SDL_AUDIODRIVER="dummy",
        GRIDLINE_QUIET_WINDOWS="1",
        GRIDLINE_NO_PLUGIN_SCAN="1",
        GRIDLINE_CONFIG_HOME=cfg,
        HOME=os.path.join(cfg, "home"),
    )
    if extra_env:
        env.update(extra_env)
    r = subprocess.run(
        [
            APP,
            song,
            "--hidden",
            "--no-ui-sounds",
            "--no-update-check",
            "--fixed-dt",
            "16",
            "--size",
            "1440x900",
            "--dev-input",
            sp,
            "--frames",
            str(frames),
        ],
        env=env,
        capture_output=True,
        text=True,
        timeout=500,
    )
    with open(os.path.join(OUT, tag + ".log"), "w") as f:
        f.write(r.stdout + "\n---- stderr ----\n" + r.stderr)
    if r.returncode != 0:
        fail("%s: gridline-app failed (%d)" % (tag, r.returncode))
    return r.stdout


def parse_patterns(out, frame):
    m = re.search(
        r'patterns frame %d: cur (\d+) undo (\d+) "([^"]*)"(.*)\n' % frame,
        out,
    )
    if not m:
        return None
    cur = int(m.group(1))
    undo = int(m.group(2))
    label = m.group(3)
    pats = {int(pm.group(1)): pm.group(2) for pm in re.finditer(r'\[(\d+):"([^"]*)"\]', m.group(4))}
    return cur, undo, label, pats


def build_steps(cases):
    """Build one dev-input script for a sequence of (tag, setup_lines, target, new_name, open_dropdown_first)."""
    lines = ["2 toasts clear", "4 patterns"]
    f = 6
    meta = []
    for tag, setup_lines, target, new_name, open_dropdown_first in cases:
        for s in setup_lines:
            lines.append("%d %s" % (f, s))
            f += 3
        if open_dropdown_first:
            lines.append("%d sdlat patbtn" % f)
            lines.append("%d sdldown 0" % (f + 1))
            lines.append("%d sdlup 0" % (f + 2))
            f += 5
        lines.append("%d sdlat %s" % (f, target))
        f_target = f
        lines.append("%d sdldown 1" % (f + 1))
        lines.append("%d sdlup 1" % (f + 2))
        f_menu = f + 5
        lines.append("%d sdlat patmenurename" % f_menu)
        # Wait >180ms at 16ms/frame for ImGui submenu hover-open so InputText auto-focuses.
        f_type = f_menu + 13
        lines.append("%d text %s" % (f_type, new_name))
        lines.append("%d key enter" % (f_type + 2))
        # Ensure any parent popup (e.g. ##patpop after nested rename) is dismissed before the next case.
        lines.append("%d key escape" % (f_type + 4))
        f_after = f_type + 6
        lines.append("%d patterns" % f_after)
        lines.append("%d undo" % (f_after + 1))
        f_undone = f_after + 3
        lines.append("%d patterns" % f_undone)
        meta.append((tag, target, new_name, f_target, f_menu, f_after, f_undone))
        f = f_undone + 2
    lines.append("%d quit discard" % (f + 1))
    return "\n".join(lines) + "\n", f + 6, meta


def check_batch(batch_tag, layout, cases, extra_env=None):
    script, total_frames, meta = build_steps(cases)
    out = run(batch_tag, layout, script, frames=total_frames, extra_env=extra_env)
    p0 = parse_patterns(out, 4)
    ok(p0 is not None and p0[0] in p0[3], "%s: initial pattern state captured" % batch_tag)
    if p0 is None:
        return
    cur0, undo0, _, pats0 = p0
    orig_name = pats0[cur0]

    for tag, target, new_name, f_target, f_menu, f_after, f_undone in meta:
        ok(
            re.search(r"sdlat frame %d: %s at " % (f_target, re.escape(target)), out) is not None,
            "%s: target %s is visible" % (tag, target),
        )
        ok(
            re.search(r"sdlat frame %d: patmenurename at " % f_menu, out) is not None,
            "%s: right-click opens patternContextMenu with Rename..." % tag,
        )
        p1 = parse_patterns(out, f_after)
        ok(
            p1 is not None and p1[3].get(cur0) == new_name,
            "%s: inline Rename... renames pattern to '%s' (got %s)" % (tag, new_name, p1[3].get(cur0) if p1 else None),
        )
        ok(
            p1 is not None and p1[1] == undo0 + 1 and p1[2] == "rename pattern",
            "%s: rename records one undo step (%s)" % (tag, (p1[1], p1[2]) if p1 else None),
        )
        p2 = parse_patterns(out, f_undone)
        ok(
            p2 is not None and p2[1] == undo0 and p2[3].get(cur0) == orig_name,
            "%s: undo restores original pattern name '%s' (got %s)" % (tag, orig_name, p2[3].get(cur0) if p2 else None),
        )


# Batch 1 (Studio): Top Bar selector, Top Bar dropdown row, Browser Current project > Patterns, Playlist breadcrumb
check_batch(
    "studio_sites",
    "studio",
    [
        ("topbar", [], "patbtn", "TopBarLead", False),
        ("topbar_dropdown", [], "patrow0", "DropdownBass", True),
        ("browser", ["browser tab 1"], "brpat0", "BrowserPad", False),
        ("playlist_crumb", [], "plsrc", "BreadcrumbArp", False),
    ],
    extra_env={"GRIDLINE_BROWSER_EXPAND": "Patterns"},
)

# Batch 2 (Tracker): Song page -> Pattern Bank
check_batch(
    "tracker_site",
    "tracker",
    [
        ("tracker_bank", ["tracker page Song"], "trpat0", "TrackerSeq", False),
    ],
)

print("%d passed, %d failed" % (passed, len(failures)))
sys.exit(1 if failures else 0)
