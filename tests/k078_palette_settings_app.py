#!/usr/bin/env python3
# Gridline - 0.7.8: the command palette shows the Settings key on one row in every layout. In the
# Tracker and in Clips it showed it twice (Settings, and Settings (MIDI page) or General settings...),
# in Studio on Settings and MIDI settings... The real app (hidden, isolated config and HOME, SDL's
# dummy audio) dumps the palette in each layout; the rows that open Settings or run its command
# show the key on exactly one row, and the command stays behind one row.
#   python3 k078_palette_settings_app.py <gridline-app> <project.gridline.json> <out dir>
import os
import shutil
import subprocess
import sys

app, song, out = sys.argv[1], sys.argv[2], sys.argv[3]
shutil.rmtree(out, ignore_errors=True)
os.makedirs(os.path.join(out, "home"))
env = dict(os.environ, SDL_AUDIODRIVER="dummy", GRIDLINE_NO_PLUGIN_SCAN="1", HOME=os.path.join(out, "home"), GRIDLINE_CONFIG_HOME=os.path.join(out, "home"))
script = os.path.join(out, "script.txt")
with open(script, "w") as f:
    f.write("20 palette dump\n24 quit discard\n")
failed = 0
n = 0


def check(ok, what):
    global failed, n
    n += 1
    print(("ok " if ok else "not ok ") + f"{n} - {what}")
    if not ok:
        failed += 1


names = {"studio": "Studio", "performance": "Clips", "tracks": "Smooth", "tracker": "Tracker"}
for layout, name in names.items():
    r = subprocess.run([app, song, "--hidden", "--no-ui-sounds", "--layout", layout, "--size", "1440x900", "--frames", "60", "--dev-input", script],
                       env=env, capture_output=True, text=True, timeout=120)
    check(r.returncode == 0, f"{name}: the app ran ({r.returncode})")
    rows = []
    for line in r.stdout.splitlines():
        f = line.split("\t")
        if f[0] == "ROW" and len(f) >= 9:
            rows.append({"kind": f[1], "id": f[2], "cmd": f[3], "label": f[6], "key": f[8]})
    check(len(rows) > 300, f"{name}: the palette was dumped ({len(rows)} rows)")
    settings = [x for x in rows if x["cmd"] == "view.settings" or x["id"] == "win:Settings"]
    keyed = [x for x in settings if x["key"]]
    shown = ", ".join(f"{x['label']} [{x['key']}]" for x in settings)
    check(len(keyed) == 1, f"{name}: the Settings key is on one row ({shown})")
    check(sum(1 for x in rows if x["cmd"] == "view.settings") == 1, f"{name}: the Settings command is behind one row")
    check(any(x["id"] == "win:Settings" for x in rows), f"{name}: the Settings window has its row")
    if keyed:
        same = [x["label"] for x in rows if x["key"] == keyed[0]["key"] and x not in settings and x["kind"] == "window"]
        check(not same, f"{name}: no other window row shows that key ({same})")
print(f"1..{n}")
sys.exit(1 if failed else 0)
