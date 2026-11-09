#!/usr/bin/env python3
# Gridline - K-93: the command palette in the real app (hidden, isolated config and HOME, SDL's
# dummy audio driver), once per layout (Studio, Live, Logic) and once in the pseudo language:
#   1. its rows: every menu item of the layout's menu bar becomes exactly one row (no item twice,
#      none lost), row ids are unique, every keymap command is behind exactly one row, and there
#      are windows, settings and plugins;
#   2. the shortcut opens it (Ctrl+K; Ctrl+Shift+P in the Logic layout, whose Ctrl/Cmd+K is the
#      typing keyboard), Esc closes it;
#   3. typing and Enter run a menu command (the metronome turns on), a keymap command without a
#      menu item (forward one bar), a setting (Settings opens on its page, the row lit) and a
#      plugin (a new channel), and Tab cycles the categories;
#   4. the recent list leads the empty query and is saved in the user's settings, never the project;
#   5. translated: rows are found by their English names.
#   python3 k93_palette_app.py <gridline-app> <project.gridline.json> <out dir>
import json
import os
import re
import shutil
import subprocess
import sys

APP, DEMO, OUT = sys.argv[1], sys.argv[2], sys.argv[3]
shutil.rmtree(OUT, ignore_errors=True)
os.makedirs(OUT)

failures = []


def check(ok, what):
    print(("ok - " if ok else "FAIL - ") + what)
    if not ok:
        failures.append(what)


def run(name, script, extra=()):
    cfg = os.path.join(OUT, name, "cfg")
    home = os.path.join(OUT, name, "home")
    os.makedirs(cfg)
    os.makedirs(home)
    song = os.path.join(OUT, name, "song.gridline.json")
    shutil.copyfile(DEMO, song)
    with open(os.path.join(OUT, name, "input.txt"), "w") as f:
        f.write(script)
    env = dict(os.environ, GRIDLINE_CONFIG_HOME=cfg, HOME=home, SDL_AUDIODRIVER="dummy", GRIDLINE_NO_PLUGIN_SCAN="1")
    cmd = [APP, song, "--hidden", "--no-ui-sounds", "--size", "1440x900", "--dev-input", os.path.join(OUT, name, "input.txt")] + list(extra)
    p = subprocess.run(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=300)
    out = p.stdout.decode("utf-8", "replace")
    with open(os.path.join(OUT, name, "out.log"), "w") as f:
        f.write(out + "\n---- stderr ----\n" + p.stderr.decode("utf-8", "replace"))
    check(p.returncode == 0, f"{name}: the app ran and quit cleanly (code {p.returncode})")
    settings = {}
    sp = os.path.join(cfg, "gridline", "settings.json")
    if os.path.exists(sp):
        with open(sp) as f:
            settings = json.load(f).get("values", {})
    return out, settings


def dump(out):
    rows, menus, keycmds = [], [], []
    inside = False
    for line in out.splitlines():
        if line.startswith("PALETTE BEGIN"):
            inside, rows, menus, keycmds = True, [], [], []
            continue
        if line.startswith("PALETTE END"):
            inside = False
            break
        if not inside:
            continue
        f = line.split("\t")
        if f[0] == "ROW" and len(f) >= 9:
            rows.append({"kind": f[1], "id": f[2], "cmd": f[3], "enabled": f[4] == "1", "label": f[5], "en": f[6], "path": f[7], "key": f[8]})
        elif f[0] == "MENU" and len(f) >= 6:
            menus.append({"row": f[1], "path": f[2], "label": f[3], "occ": f[4], "raw": f[5]})
        elif f[0] == "KEYCMD":
            keycmds.append(f[1])
    return rows, menus, keycmds


def states(out):
    s = {}
    # ids may hold spaces and commas: lists are joined with " ;; "
    for m in re.finditer(r"^palette frame (\d+): open (\d) cat (\d) query \[(.*?)\] sel (.*?) top (.*?) recent (.*?) \| metronome (\d) mixer (\d) "
                         r"settings (\d) highlight \[(.*?)\] position (-?\d+) channels (\d+)$", out, re.M):
        s[int(m.group(1))] = {"open": m.group(2) == "1", "cat": int(m.group(3)), "query": m.group(4), "sel": m.group(5), "top": m.group(6).split(" ;; "),
                              "recent": m.group(7), "metro": m.group(8) == "1", "mixer": m.group(9) == "1", "settings": m.group(10) == "1",
                              "highlight": m.group(11), "position": int(m.group(12)), "channels": int(m.group(13))}
    return s


def check_rows(name, rows, menus, keycmds):
    check(len(rows) > 300, f"{name}: the palette has rows ({len(rows)})")
    ids = [r["id"] for r in rows]
    check(len(ids) == len(set(ids)), f"{name}: every row id is unique")
    byid = {r["id"]: r for r in rows}
    check(len(menus) > 150, f"{name}: the menus were walked ({len(menus)} items)")
    # (two items may read the same, e.g. a template named "Empty project": their ImGui ids differ)
    keys = [(m["path"], m["raw"], m["occ"]) for m in menus]
    check(len(keys) == len(set(keys)), f"{name}: no menu item was found twice")
    cmdless = [m for m in menus if not m["row"].startswith(("cmd:", "win:"))]
    check(len({m["row"] for m in cmdless}) == len({(m["path"], m["raw"], m["occ"]) for m in cmdless}),
          f"{name}: items that read the same are rows of their own")
    lost = [m for m in menus if m["row"] not in byid]
    check(not lost, f"{name}: every menu item is a row" + ("" if not lost else f" (lost: {lost[:3]})"))
    cmds = {}
    for r in rows:
        if r["cmd"] != "-":
            cmds[r["cmd"]] = cmds.get(r["cmd"], 0) + 1
    twice = [c for c, n in cmds.items() if n != 1]
    missing = [c for c in keycmds if c not in cmds]
    check(not twice, f"{name}: every keymap command is behind one row only" + ("" if not twice else f" ({twice[:5]})"))
    check(not missing and len(keycmds) > 150, f"{name}: every keymap command has a row ({len(keycmds)})" + ("" if not missing else f" (missing {missing[:5]})"))
    kinds = {r["kind"] for r in rows}
    check(kinds == {"command", "window", "setting", "plugin"}, f"{name}: commands, windows, settings and plugins are listed ({sorted(kinds)})")
    check(sum(1 for r in rows if r["kind"] == "setting") > 60, f"{name}: the settings pages give their rows")
    # Where each lives, and shortcuts.
    save = byid.get("cmd:file.save")
    home = "File" if name != "pseudo" else "[F"  # pseudo: the path is shown translated ("[Fïlé]")
    check(save is not None and save["path"].startswith(home) and save["key"] != "", f"{name}: Save sits in File with its shortcut ({save and save['path']})")
    pal = byid.get("cmd:view.commandPalette")
    check(pal is not None and pal["key"] != "", f"{name}: the palette itself is a View menu row with its shortcut")
    nested = [m for m in menus if m["path"].count(" > ") >= 1]
    check(len(nested) > 20, f"{name}: submenu items carry their submenu in the path ({len(nested)})")
    check(any(not r["enabled"] for r in rows), f"{name}: greyed commands are listed (not hidden)")
    return byid


def ctrl(key, shift=False):
    s = "mod ctrl 1\n" + ("mod shift 1\n" if shift else "") + f"key {key}\n"
    return s


def script_for(layout):
    # frames: each line "<frame> <command>"
    lines = []
    f = [5]

    def at(cmd, step=2):
        lines.append(f"{f[0]} {cmd}")
        f[0] += step

    at("toasts clear", 1)
    at("palette dump")
    at("metro")
    # Logic: Ctrl+Shift+P; on macOS that set tells Cmd from Ctrl, so Cmd+Shift+P ("super").
    cmd = "super" if (layout == "logic" and sys.platform == "darwin") else "ctrl"
    opener = (f"mod {cmd} 1", "mod shift 1", "key p") if layout == "logic" else ("mod ctrl 1", "key k")
    marks = {}

    def open_palette():
        for c in opener:
            at(c, 1)
        at(f"mod {cmd} 0", 1)
        at("mod shift 0", 2)

    # 1. menu command by typing: the metronome
    open_palette()
    marks["opened"] = f[0]
    at("palette state", 2)
    at("text metronome", 3)
    marks["typed"] = f[0]
    at("palette state", 1)
    at("key enter", 4)
    marks["ran"] = f[0]
    at("palette state", 1)
    at("metro", 2)
    # 2. a keymap command with no menu item (studio): forward one bar
    open_palette()
    at("text forward one bar", 3)
    marks["fwdtyped"] = f[0]
    at("palette state", 1)
    at("key enter", 5)
    marks["fwdran"] = f[0]
    at("palette state", 2)
    # 3. a setting
    open_palette()
    at("text smooth zooming", 3)
    marks["settyped"] = f[0]
    at("palette state", 1)
    at("key enter", 4)
    marks["setran"] = f[0]
    at("palette state", 6)
    # 4. Tab cycles categories; a plugin is added
    open_palette()
    marks["empty"] = f[0]
    at("palette state", 1)
    for _ in range(4):  # (a press and its release each take a frame: one key every two frames)
        at("key tab", 2)
    marks["tabbed"] = f[0]
    at("palette state", 1)
    at("text sampler", 3)
    marks["plugtyped"] = f[0]
    at("palette state", 1)
    at("key enter", 4)
    marks["plugran"] = f[0]
    at("palette state", 2)
    # 5. Esc closes; the shortcut toggles
    open_palette()
    at("key escape", 3)
    marks["esc"] = f[0]
    at("palette state", 2)
    at("quit discard", 1)
    return "\n".join(lines) + "\n", marks


for layout in ("studio", "live", "logic"):
    script, marks = script_for(layout)
    out, settings = run(layout, script, ["--layout", layout])
    rows, menus, keycmds = dump(out)
    byid = check_rows(layout, rows, menus, keycmds)
    st = states(out)
    g = lambda k: st.get(marks[k], {})
    check(g("opened").get("open"), f"{layout}: the shortcut opens the palette")
    check(g("typed").get("top", ["-"])[0] == "cmd:options.metronome", f"{layout}: \"metronome\" ranks the Metronome command first ({g('typed').get('top')})")
    ran = g("ran")
    check(ran.get("open") is False and ran.get("metro") is True, f"{layout}: Enter ran it: the palette closed and the metronome is on")
    check(ran.get("recent", "").startswith("cmd:options.metronome"), f"{layout}: it leads the recent list")
    ft = g("fwdtyped")
    check(ft.get("top", ["-"])[0] == "cmd:transport.forwardBar", f"{layout}: \"forward one bar\" finds the command ({ft.get('top')})")
    check(g("fwdran").get("position", 0) > 0, f"{layout}: Enter ran it: the song position moved forward ({g('fwdran').get('position')})")
    stt = g("settyped")
    check(stt.get("top", ["-"])[0].startswith("set:Interface/Smooth zooming"), f"{layout}: \"smooth zooming\" finds the setting ({stt.get('top')})")
    sr = g("setran")
    check(sr.get("settings") is True and sr.get("highlight") == "Smooth zooming", f"{layout}: Enter opened Settings with the row lit")
    em = g("empty")
    check(em.get("query") == "" and em.get("top", [""])[0] == "set:Interface/Smooth zooming", f"{layout}: the empty query lists the latest recent row first")
    check(g("tabbed").get("cat") == 4, f"{layout}: Tab cycles the categories (Plugins after four)")
    pt = g("plugtyped")
    check(pt.get("top", ["-"])[0].startswith("plug:"), f"{layout}: the Plugins category finds a plugin ({pt.get('top')})")
    check(g("plugran").get("channels", 0) == pt.get("channels", 0) + 1, f"{layout}: Enter added it as a new channel")
    check(g("esc").get("open") is False, f"{layout}: Esc closes the palette")
    rec = settings.get("ui.paletteRecent", "")
    check(rec.split("\n")[:3] == [pt.get("top", ["-"])[0], "set:Interface/Smooth zooming", "cmd:transport.forwardBar"],
          f"{layout}: the recent list is saved in the user's settings ({rec!r})")
    with open(os.path.join(OUT, layout, "song.gridline.json")) as f:
        check("paletteRecent" not in f.read(), f"{layout}: nothing reaches the project")
    if layout == "logic":
        nav = [m for m in menus if m["label"] == "Forward one bar"]
        check(bool(nav) and byid.get(nav[0]["row"], {}).get("id") == "cmd:transport.forwardBar",
              "logic: its menu item and the keymap command are one row")

# Translated (pseudo language): rows are found by their English names.
script = "5 toasts clear\n7 palette dump\n9 palette open\n12 text metronome\n15 palette state\n17 quit discard\n"
out, _ = run("pseudo", script, ["--lang", "pseudo"])
rows, menus, keycmds = dump(out)
check_rows("pseudo", rows, menus, keycmds)
met = [r for r in rows if r["id"] == "cmd:options.metronome"]
check(bool(met) and met[0]["label"] != "Metronome" and met[0]["en"] == "Metronome", "pseudo: the row shows the translated label and keeps the English one")
st = states(out)
check(st.get(15, {}).get("top", ["-"])[0] == "cmd:options.metronome", "pseudo: the English name finds it")

print(f"{len(failures)} failure(s)")
sys.exit(1 if failures else 0)
