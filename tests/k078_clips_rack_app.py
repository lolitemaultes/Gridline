#!/usr/bin/env python3
# Gridline - clipsrack (0.7.8): the channel a playlist audio clip plays from is listed in the channel rack
# like any other sample channel: its name button, mute, pan and volume, and a row of steps that can be
# sequenced (it used to show the sound's outline instead of steps). In the real app (hidden, isolated
# config folder and HOME, SDL's dummy audio driver), in each layout (Studio, Clips, Smooth, Tracker):
#   - audio dropped on the playlist makes an audio clip channel; the rack draws it in view as a row of
#     steps with the row's step geometry;
#   - a click on its first step sets the step, as one undo step;
#   - the project saved after the click renders the pattern with sound (the step plays the sample),
#     the one saved before it renders the same pattern silent; the playlist audio clip is still there
#     and still plays from the same channel;
#   - the saved project loads back with the step.
# The Tracker shows the channels on its own Steps page (every channel a row of steps, audio clip channels
# too): there the audio is dropped, the Steps page shown, then the layout switched to Studio for the rack.
#   python3 k078_clips_rack_app.py <gridline-app> <gridline cli> <song.gridline.json> <wav> <out dir>
import json
import os
import re
import shutil
import subprocess
import sys
import wave

APP, CLI, DEMO, WAV, OUT = sys.argv[1:6]
failures = []
passed = 0


def fail(msg):
    failures.append(msg)
    print("FAIL: " + msg)


def ok(cond, msg):
    global passed
    if cond:
        passed += 1
        print("ok - " + msg)
    else:
        fail(msg)
    return cond


shutil.rmtree(OUT, ignore_errors=True)
os.makedirs(OUT)
media = os.path.join(OUT, "media")
os.makedirs(media)
tone = os.path.join(media, "tone loop.wav")
shutil.copyfile(WAV, tone)

# The test song with every pattern emptied (no steps, no notes): a pattern render is silent until a step is set.
with open(DEMO) as f:
    song = json.load(f)
for p in song["patterns"]:
    p["steps"] = {}
    p["notes"] = {}
quiet = os.path.join(OUT, "quiet.gridline.json")
with open(quiet, "w") as f:
    json.dump(song, f)


def run(tag, layout, script, frames, extra=()):
    cfg = os.path.join(OUT, "cfg_" + tag)
    shutil.rmtree(cfg, ignore_errors=True)
    os.makedirs(os.path.join(cfg, "gridline"))
    os.makedirs(os.path.join(cfg, "home"))
    with open(os.path.join(cfg, "gridline", "settings.json"), "w") as f:
        json.dump({"format": "gridline-settings", "values": {"ui.layoutChosen": True, "ui.layout": layout}}, f)
    sp = os.path.join(OUT, tag + ".txt")
    with open(sp, "w") as f:
        f.write(script)
    env = dict(os.environ, SDL_AUDIODRIVER="dummy", GRIDLINE_NO_PLUGIN_SCAN="1", GRIDLINE_HIDE_PLUGINS="all", GRIDLINE_CONFIG_HOME=cfg,
               HOME=os.path.join(cfg, "home"))
    r = subprocess.run([APP, quiet, "--hidden", "--no-ui-sounds", "--no-update-check", "--fixed-dt", "16", "--size", "1440x900", "--layout", layout,
                        "--dev-input", sp, "--frames", str(frames)] + list(extra), env=env, capture_output=True, text=True, timeout=600)
    with open(os.path.join(OUT, tag + ".log"), "w") as f:
        f.write(r.stdout + "\n---- stderr ----\n" + r.stderr)
    if r.returncode != 0:
        fail("%s: gridline-app failed (%d)" % (tag, r.returncode))
    return r.stdout


def block(out, marker):
    at = out.find(marker)
    if at < 0:
        return ""
    m = re.match(r"[^\n]*\n(?:  [^\n]*\n)*", out[at:])
    return m.group(0) if m else ""


def peak(path):
    with wave.open(path) as w:
        n, width = w.getnframes(), w.getsampwidth()
        raw = w.readframes(n)
    if width != 2:
        return -1.0
    best = 0
    for i in range(0, len(raw) - 1, 2):
        v = abs(int.from_bytes(raw[i:i + 2], "little", signed=True))
        best = max(best, v)
    return best / 32768.0


def render_pattern(project, index, tag):
    out = os.path.join(OUT, tag + ".wav")
    env = dict(os.environ, SDL_AUDIODRIVER="dummy", GRIDLINE_NO_PLUGIN_SCAN="1", GRIDLINE_CONFIG_HOME=os.path.join(OUT, "cfg_cli"),
               HOME=os.path.join(OUT, "cfg_cli"))
    os.makedirs(os.path.join(OUT, "cfg_cli"), exist_ok=True)
    r = subprocess.run([CLI, "render", project, "-o", out, "--scope", "pattern:%d" % index, "--tail", "cut"], env=env, capture_output=True, text=True,
                       timeout=600)
    if r.returncode != 0 or not os.path.exists(out):
        fail("%s: the render failed (%d): %s" % (tag, r.returncode, r.stderr.strip()))
        return -1.0
    return peak(out)


for layout, name in (("live", "Clips"), ("studio", "Studio"), ("logic", "Smooth"), ("tracker", "Tracker")):
    DROP = '20 audio "%s" 3 2\n40 layout open Channel rack\n' % tone
    extra = ()
    if layout == "tracker":
        DROP = '20 audio "%s" 3 2\n28 tracker page Steps\n32 shot "%s"\n50 layout open Channel rack\n' % (tone, os.path.join(OUT, "tracker_steps.ppm"))
        extra = ("--switch-layout", "studio@36")
    # 1. Where the audio clip channel's row and its first step are drawn.
    out = run(layout + "_where", layout, DROP + "60 channels\n62 rack\n64 quit discard\n", 70, extra)
    if layout == "tracker":
        ok("tracker page frame 28: Channel rack" in out, "Tracker: its Steps page shows the channels")
    chans, rack = block(out, "channels frame 60:"), block(out, "rack frame 62:")
    m = re.search(r"\n  channel (\d+) tone loop type gl\.sampler ", chans)
    if not ok(m is not None, "%s: audio dropped on the playlist makes its channel" % name):
        continue
    cid = m.group(1)
    m = re.search(r"\n  rackrow (\d+) id %s audioclip 1 steps 0 visible 1 shown 1 name tone loop\n" % cid, rack)
    if not ok(m is not None, "%s: the rack lists it in view as an audio clip channel" % name):
        print(rack)
        continue
    row = m.group(1)
    g = re.search(r"\n  rowgeom %s [^\n]* steps ([-0-9.]+) ([-0-9.]+) ([-0-9.]+) ([-0-9.]+) " % row, rack)
    lane = re.search(r"\n  lane %s [^\n]* step0 ([-0-9.]+) step1 ([-0-9.]+)\n" % row, rack)
    if not ok(g is not None and lane is not None and float(g.group(3)) > float(g.group(1)) + 20,
              "%s: its row is drawn with steps (a step area and a first step)" % name):
        print(rack)
        continue
    x0, x1 = float(lane.group(1)), float(lane.group(2))
    cx, cy = int((x0 + (x1 - x0) * 0.4) + 0.5), int((float(g.group(2)) + float(g.group(4))) * 0.5)

    # 2. A click on the first step sets it (one undo step); the project before and after is saved and rendered.
    before = os.path.join(OUT, layout + "_before.gridline")
    after = os.path.join(OUT, layout + "_after.gridline")
    script = (DROP + "60 channels\n62 save \"%s\"\n66 sdlmove %d %d\n68 sdldown 0\n69 sdlup 0\n76 channels\n78 rack\n80 shot \"%s\"\n"
              "82 save \"%s\"\n90 quit discard\n") % (before, cx, cy, os.path.join(OUT, layout + "_rack.ppm"), after)
    out = run(layout + "_click", layout, script, 96, extra)
    c60, c76, r78 = block(out, "channels frame 60:"), block(out, "channels frame 76:"), block(out, "rack frame 78:")
    u60 = re.search(r"undo (\d+)", c60)
    u76 = re.search(r"undo (\d+)", c76)
    st = re.search(r"\n  channel %s tone loop type gl\.sampler [^\n]* steps (\d+) " % cid, c76)
    ok(st is not None and st.group(1) == "1", "%s: a click on the audio clip channel's first step sets it" % name)
    ok(u60 and u76 and int(u76.group(1)) == int(u60.group(1)) + 1, "%s: the step is one undo step" % name)
    ok(re.search(r"\n  rackrow %s id %s audioclip 1 steps 1 visible 1 " % (row, cid), r78) is not None,
       "%s: the row stays a row of steps with its step on" % name)
    if not (os.path.exists(before) and os.path.exists(after)):
        fail("%s: the projects were not saved" % name)
        continue
    with open(after) as f:
        saved = json.load(f)
    pidx = -1
    for i, p in enumerate(saved.get("patterns", [])):
        if any(v for v in p.get("steps", {}).get(cid, [])):
            pidx = i + 1
    ok(pidx > 0, "%s: the saved project holds the step (and loads it back below)" % name)
    clips = [c for c in saved.get("playlist", []) if c.get("kind") == "audio" and str(c.get("source")) == cid]
    ok(len(clips) == 1, "%s: the playlist audio clip is still there, playing from the same channel" % name)
    if pidx > 0:
        pb, pa = render_pattern(before, pidx, layout + "_before"), render_pattern(after, pidx, layout + "_after")
        ok(0 <= pb < 1e-4, "%s: the pattern renders silent before the step (peak %.5f)" % (name, pb))
        ok(pa > 0.01, "%s: the step plays the audio clip channel's sample (peak %.5f)" % (name, pa))
        # 3. Loaded back, the step is there.
        cfg = os.path.join(OUT, "cfg_cli")
        env = dict(os.environ, SDL_AUDIODRIVER="dummy", GRIDLINE_NO_PLUGIN_SCAN="1", GRIDLINE_CONFIG_HOME=cfg, HOME=cfg)
        up = os.path.join(OUT, layout + "_again.gridline")
        r = subprocess.run([CLI, "upgrade", after, "-o", up], env=env, capture_output=True, text=True, timeout=120)
        again = {}
        if r.returncode == 0 and os.path.exists(up):
            with open(up) as f:
                again = json.load(f)
        ok(any(any(v for v in p.get("steps", {}).get(cid, [])) for p in again.get("patterns", [])), "%s: loaded back, the step is there" % name)

print("%d passed, %d failed" % (passed, len(failures)))
if failures:
    sys.exit(1)
