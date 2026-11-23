#!/usr/bin/env python3
# Gridline - Smooth's pane buttons on the control bar (the editors, Smart controls and mixer of the lower pane;
# the lists, notes and browser of the right pane) and their keys, in the real app (hidden, an isolated config
# folder and HOME, SDL's dummy audio driver, a fixed frame time), clicked with real SDL pointer events:
#   - a click on a view that is not shown opens it in its pane (the pane coming back with it alone);
#   - a click on the view shown closes its pane (the tracks take the room back);
#   - a third click opens that view again: it showed the pane's default window instead (the mixer gave the
#     piano roll, Smart controls the piano roll, notes and lists the browser);
#   - a click on another view of an open pane switches the pane to it (the pane stays open);
#   - the lit buttons always name what the panes show (one per pane at most), the audio editor included;
#   - the keys (E, Y, X, D, Alt+Cmd+P, Cmd+2) do the same, and a key and the button agree.
#   python3 k078_smooth_view_buttons_app.py <gridline-app> <song> <out dir>
import json
import os
import re
import shutil
import subprocess
import sys

APP, SONG, OUT = sys.argv[1], sys.argv[2], sys.argv[3]
failures = []
passed = 0
BUTTONS = ["lb_ed", "lb_sm", "lb_mx", "lb_lists", "lb_notes", "lb_browser"]
PANE = {"lb_ed": "lower", "lb_sm": "lower", "lb_mx": "lower", "lb_lists": "right", "lb_notes": "right", "lb_browser": "right"}
WINDOW = {"lb_sm": "Smart controls", "lb_mx": "Mixer", "lb_lists": "Lists", "lb_notes": "Notes", "lb_browser": "Browser"}
KEYS = {"e": "lb_ed", "y": "lb_sm", "x": "lb_mx", "d": "lb_lists", "notes": "lb_notes", "cmd2": "lb_mx"}


def ok(cond, msg):
    global passed
    if cond:
        passed += 1
        print("ok - " + msg)
    else:
        failures.append(msg)
        print("FAIL: " + msg)


def run(tag, steps):
    """Runs the steps (a button id, key:<e|y|x|d|w|notes|cmd2>); returns the state before and after each."""
    cfg = os.path.join(OUT, "cfg_" + tag)
    shutil.rmtree(cfg, ignore_errors=True)
    os.makedirs(os.path.join(cfg, "gridline"))
    os.makedirs(os.path.join(cfg, "home"))
    with open(os.path.join(cfg, "gridline", "settings.json"), "w") as f:
        json.dump({"format": "gridline-settings", "values": {"ui.layoutChosen": True, "ui.layout": "logic", "keys.profile": "logic",
                                                              "ui.menuStyle": "logic"}}, f)
    lines = ["4 typing off", "5 toasts clear"]
    fr = 40

    def report():
        lines.append("%d barlit" % fr)
        lines.append("%d layout dump" % fr)

    report()
    fr += 4
    for s in steps:
        if s.startswith("key:"):
            k = s[4:]
            if k == "notes":  # Alt+Cmd+P
                lines += ["%d mod super 1" % fr, "%d mod alt 1" % fr, "%d key p" % (fr + 1), "%d mod alt 0" % (fr + 2), "%d mod super 0" % (fr + 2)]
            elif k == "cmd2":
                lines += ["%d mod super 1" % fr, "%d key 2" % (fr + 1), "%d mod super 0" % (fr + 2)]
            else:
                lines.append("%d key %s" % (fr, k))
        else:
            lines += ["%d sdlat %s" % (fr, s), "%d sdldown 0" % (fr + 2), "%d sdlup 0" % (fr + 3)]
        fr += 14
        report()
        fr += 4
    lines.append("%d quit discard" % (fr + 2))
    sp = os.path.join(OUT, tag + ".txt")
    with open(sp, "w") as f:
        f.write("\n".join(lines) + "\n")
    env = dict(os.environ, SDL_AUDIODRIVER="dummy", GRIDLINE_NO_PLUGIN_SCAN="1", GRIDLINE_HIDE_PLUGINS="all", GRIDLINE_CONFIG_HOME=cfg,
               HOME=os.path.join(cfg, "home"))
    r = subprocess.run([APP, SONG, "--hidden", "--no-ui-sounds", "--no-update-check", "--fixed-dt", "16", "--size", "1440x900", "--dev-input", sp],
                       env=env, capture_output=True, text=True, timeout=600)
    with open(os.path.join(OUT, tag + ".log"), "w") as f:
        f.write(r.stdout + "\n---\n" + r.stderr)
    if r.returncode != 0:
        ok(False, "%s: gridline-app ran (%d)" % (tag, r.returncode))
        return []
    lits = re.findall(r"barlit frame \d+: ([^\n]*)", r.stdout)
    dumps = re.findall(r"LAYOUT BEGIN frame \d+\n(.*?)LAYOUT END", r.stdout, re.S)
    states = []
    for lit, d in zip(lits, dumps):
        st = {"lit": {kv.split("=")[0] for kv in lit.split() if kv.endswith("=1")}}
        for pane in ("lower", "right"):
            m = re.search(r"region logic\.%s visible (\d) [^\n]*windows \[([^\]]*)\] front ([^\n]*)" % pane, d)
            st[pane] = (m.group(1) == "1", m.group(3).strip()) if m else (False, "-")
        states.append(st)
    ok(len(states) == len(steps) + 1, "%s: a state after every step (%d of %d)" % (tag, len(states), len(steps) + 1))
    return states


def lit_in(st, pane):
    return sorted(b for b in st["lit"] if PANE.get(b) == pane)


def check_shown(tag, st, btn):
    """The view of `btn` is what its pane shows, and its button alone is lit there."""
    pane = PANE[btn]
    vis, front = st[pane]
    want = WINDOW.get(btn)
    good = vis and (front == want if want else front in ("Piano roll", "Channel rack", "Loupe")) and lit_in(st, pane) == [btn]
    ok(good, "%s: %s shown in the %s pane, its button alone lit (pane %s, front %s, lit %s)" % (tag, btn, pane, vis, front, lit_in(st, pane)))


def check_closed(tag, st, pane):
    ok(not st[pane][0] and not lit_in(st, pane), "%s: the %s pane closed, nothing lit for it (pane %s, lit %s)" % (tag, pane, st[pane][0], lit_in(st, pane)))


def each_button():
    # Open, close, open again: every button on its own, from the start, three clicks each.
    steps = []
    for b in BUTTONS:
        steps += [b, b, b, b]
    states = run("buttons", steps)
    if len(states) != len(steps) + 1:
        return
    for i, b in enumerate(BUTTONS):
        s1, s2, s3, s4 = states[i * 4 + 1:i * 4 + 5]
        check_shown("buttons %s first click" % b, s1, b)
        check_closed("buttons %s second click" % b, s2, PANE[b])
        check_shown("buttons %s third click (opens it again, not the pane's default)" % b, s3, b)
        check_closed("buttons %s fourth click" % b, s4, PANE[b])


def switching():
    # An open pane switches to the view of another of its buttons (it never closes between), and back.
    steps = ["lb_lists", "lb_notes", "lb_browser", "lb_lists", "lb_lists", "lb_ed", "lb_mx", "lb_sm", "lb_ed", "lb_sm", "lb_mx", "lb_mx",
             "lb_mx", "lb_notes", "lb_ed"]
    states = run("switch", steps)
    if len(states) != len(steps) + 1:
        return
    shown = {1: "lb_lists", 2: "lb_notes", 3: "lb_browser", 4: "lb_lists", 6: "lb_ed", 7: "lb_mx", 8: "lb_sm", 9: "lb_ed", 10: "lb_sm",
             11: "lb_mx", 13: "lb_mx", 15: "lb_ed"}
    for i, b in shown.items():
        check_shown("switch step %d (%s)" % (i, steps[i - 1]), states[i], b)
    check_closed("switch step 5 (lists again)", states[5], "right")
    check_closed("switch step 12 (mixer again)", states[12], "lower")
    # The panes are independent: the notes opened beside the mixer, the editors beside the notes.
    check_shown("switch step 14 (notes beside the mixer)", states[14], "lb_notes")
    ok("lb_mx" in states[14]["lit"] and states[14]["lower"][0], "switch: the mixer stays when the notes open")


def keys():
    # The keys follow the same rule, and a key and its button agree.
    steps = ["key:e", "key:e", "key:e", "key:x", "key:x", "key:x", "key:y", "key:y", "key:y", "key:e", "key:x", "key:y", "key:e", "key:e",
             "key:d", "key:d", "key:d", "key:notes", "key:notes", "key:notes", "key:d", "key:d",
             "lb_mx", "key:x", "key:x", "lb_mx", "key:cmd2", "lb_sm", "key:y"]
    states = run("keys", steps)
    if len(states) != len(steps) + 1:
        return
    for i, s in enumerate(steps, 1):
        btn = KEYS.get(s[4:]) if s.startswith("key:") else s
        prev = states[i - 1]
        if btn in prev["lit"]:
            check_closed("keys step %d (%s on the view shown)" % (i, s), states[i], PANE[btn])
        else:
            check_shown("keys step %d (%s)" % (i, s), states[i], btn)


def audio():
    # The audio editor holding the lower pane (W) lights the editors' button (not the mixer's left set behind it);
    # the mixer's button then shows the mixer, the editors' button closes the pane.
    steps = ["lb_mx", "key:w", "lb_mx", "key:w", "lb_ed", "lb_ed"]
    states = run("audio", steps)
    if len(states) != len(steps) + 1:
        return
    check_shown("audio: the mixer", states[1], "lb_mx")
    ok(states[2]["lower"][0] and states[2]["lower"][1] == "Loupe" and lit_in(states[2], "lower") == ["lb_ed"],
       "audio: the audio editor in the pane lights the editors' button alone (front %s, lit %s)" % (states[2]["lower"][1], lit_in(states[2], "lower")))
    check_shown("audio: the mixer's button switches to the mixer", states[3], "lb_mx")
    ok(lit_in(states[4], "lower") == ["lb_ed"] and states[4]["lower"][1] == "Loupe", "audio: the audio editor again (lit %s)" % lit_in(states[4], "lower"))
    check_closed("audio: the editors' button closes the pane", states[5], "lower")
    check_shown("audio: the editors' button opens it again", states[6], "lb_ed")


shutil.rmtree(OUT, ignore_errors=True)
os.makedirs(OUT)
each_button()
switching()
keys()
audio()
print("%d passed, %d failed" % (passed, len(failures)))
sys.exit(1 if failures else 0)
