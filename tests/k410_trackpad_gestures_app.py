#!/usr/bin/env python3
"""Gridline — K-410 quiet app test for trackpad gestures (pinch zoom, 2D pan,
smart-zoom, and input.trackpadGestures toggle) across Playlist, Piano Roll,
Automation Editor, and Loupe.

Usage:
  python3 k410_trackpad_gestures_app.py <gridline-app> <demo song> <out dir>
"""
import json
import math
import os
import re
import shutil
import subprocess
import sys

APP, DEMO, OUT = sys.argv[1], sys.argv[2], sys.argv[3]
# The demo song is opened from a copy in this test's own folder: an edit of its audio saves an
# "Edited audio" folder beside the project, which must never land in the source tree.
os.makedirs(OUT, exist_ok=True)
_demo_copy = os.path.join(OUT, "demo.gridline.json")
if os.path.abspath(DEMO) != os.path.abspath(_demo_copy):
    shutil.copyfile(DEMO, _demo_copy)
DEMO = _demo_copy
failures = []


def fail(msg):
    failures.append(msg)
    print("FAIL: " + msg)
    sys.stdout.flush()


def ok(msg):
    print("ok - " + msg)
    sys.stdout.flush()


def run_script(tag, lines, extra_args=None, extra_env=None, layout="studio"):
    work = os.path.join(OUT, tag)
    shutil.rmtree(work, ignore_errors=True)
    cfg = os.path.join(work, "cfg")
    os.makedirs(os.path.join(cfg, "gridline"))
    os.makedirs(os.path.join(cfg, "home"))
    with open(os.path.join(cfg, "gridline", "settings.json"), "w") as f:
        json.dump(
            {
                "format": "gridline-settings",
                "values": {
                    "ui.layoutChosen": True,
                    "general.launcher": False,
                    "ui.layout": layout,
                },
            },
            f,
        )
    script_path = os.path.join(work, "script.txt")
    with open(script_path, "w") as f:
        f.write("\n".join(lines) + "\n")
    env = dict(os.environ)
    env.update(
        GRIDLINE_QUIET_WINDOWS="1",
        SDL_AUDIODRIVER="dummy",
        GRIDLINE_NO_PLUGIN_SCAN="1",
        GRIDLINE_HIDE_PLUGINS="all",
        GRIDLINE_CONFIG_HOME=cfg,
        HOME=os.path.join(cfg, "home"),
    )
    if extra_env:
        env.update(extra_env)
    args = [
        APP,
        DEMO,
        "--hidden",
        "--no-draw",
        "--no-ui-sounds",
        "--no-update-check",
        "--size",
        "1440x900",
        "--hide",
        "rack",
        "--dev-input",
        script_path,
    ]
    if extra_args:
        args.extend(extra_args)
    res = subprocess.run(args, env=env, capture_output=True, text=True, timeout=180)
    out = res.stdout + "\n" + res.stderr
    with open(os.path.join(work, "out.log"), "w") as f:
        f.write(out)
    if res.returncode != 0:
        fail(f"{tag}: gridline-app exited with code {res.returncode}")
    return out


def parse_gestures(out):
    rows = {}
    pat = re.compile(
        r"gestures frame (\d+): enabled (\d+) "
        r"playlist px ([0-9.eE+-]+) sx ([0-9.eE+-]+) sy ([0-9.eE+-]+) th ([0-9.eE+-]+) "
        r"piano px ([0-9.eE+-]+) sx ([0-9.eE+-]+) sy ([0-9.eE+-]+) rh ([0-9.eE+-]+) "
        r"auto sx ([0-9.eE+-]+) px ([0-9.eE+-]+)"
    )
    for m in pat.finditer(out):
        f = int(m.group(1))
        rows[f] = {
            "enabled": int(m.group(2)),
            "pl_px": float(m.group(3)),
            "pl_sx": float(m.group(4)),
            "pl_sy": float(m.group(5)),
            "pl_th": float(m.group(6)),
            "pr_px": float(m.group(7)),
            "pr_sx": float(m.group(8)),
            "pr_sy": float(m.group(9)),
            "pr_rh": float(m.group(10)),
            "ae_sx": float(m.group(11)),
            "ae_px": float(m.group(12)),
        }
    return rows


def test_playlist_and_pianoroll():
    # Probe Playlist geometry (center dock is shared by Playlist and Piano roll tabs).
    probe = run_script("pl_probe", ["12 plgeom", "15 quit discard"])
    m = re.search(r"view scrollY [-0-9.]+ trackHeight [-0-9.]+ grid ([-0-9.]+) ([-0-9.]+) ([-0-9.]+)", probe)
    if not m:
        fail("playlist probe: could not parse plgeom")
        return
    gx0, gy0, gy1 = float(m.group(1)), float(m.group(2)), float(m.group(3))
    anchor_off = 280.0
    ax = gx0 + anchor_off
    ay = gy0 + min(180.0, (gy1 - gy0) * 0.45)

    lines = [
        f"10 move {ax:.1f} {ay:.1f}",
        # Pan right & down first so scrollX > 0 (anchor not clamped at left edge).
        f"12 pan 180 90 {ax:.1f} {ay:.1f}",
        "14 gestures",
    ]
    # 4 pinch-in steps (+0.18 each) then 4 symmetric pinch-out steps (-0.18 each).
    f = 16
    for _ in range(4):
        lines.append(f"{f} pinch 0.18 {ax:.1f} {ay:.1f}")
        lines.append(f"{f + 1} gestures")
        f += 2
    for _ in range(4):
        lines.append(f"{f} pinch -0.18 {ax:.1f} {ay:.1f}")
        lines.append(f"{f + 1} gestures")
        f += 2

    # 2D pan test: simultaneous X and Y.
    f_pan_before = f
    lines.append(f"{f} gestures")
    lines.append(f"{f + 1} pan 120 64 {ax:.1f} {ay:.1f}")
    f_pan_after = f + 3
    lines.append(f"{f_pan_after} gestures")
    f += 5

    # Smart-zoom toggle test (smooth glide: settle 24 frames each).
    f_sz_before = f
    lines.append(f"{f} gestures")
    lines.append(f"{f + 1} smartzoom {ax:.1f} {ay:.1f}")
    f_sz_in = f + 26
    lines.append(f"{f_sz_in} gestures")
    lines.append(f"{f_sz_in + 1} smartzoom {ax:.1f} {ay:.1f}")
    f_sz_back = f_sz_in + 26
    lines.append(f"{f_sz_back} gestures")
    f = f_sz_back + 2

    # Disabled toggle test: turning input.trackpadGestures off blocks pinch, smartzoom, pan.
    lines.append(f"{f} setting input.trackpadGestures off")
    f_off_before = f + 2
    lines.append(f"{f_off_before} gestures")
    lines.append(f"{f + 3} pinch 0.5 {ax:.1f} {ay:.1f}")
    lines.append(f"{f + 4} smartzoom {ax:.1f} {ay:.1f}")
    lines.append(f"{f + 5} pan 150 90 {ax:.1f} {ay:.1f}")
    f_off_after = f + 10
    lines.append(f"{f_off_after} gestures")
    f = f_off_after + 2

    # Re-enable and switch to Piano Roll (F7) in the same center dock area!
    lines.append(f"{f} setting input.trackpadGestures on")
    lines.append(f"{f + 1} key f7")
    f += 8
    lines.append(f"{f} move {ax:.1f} {ay:.1f}")
    lines.append(f"{f + 2} pan 160 40 {ax:.1f} {ay:.1f}")
    f_pr_base = f + 4
    lines.append(f"{f_pr_base} gestures")
    lines.append(f"{f_pr_base + 2} pinch 0.25 {ax:.1f} {ay:.1f}")
    f_pr_in = f_pr_base + 4
    lines.append(f"{f_pr_in} gestures")
    lines.append(f"{f_pr_in + 2} pinch -0.25 {ax:.1f} {ay:.1f}")
    f_pr_out = f_pr_in + 4
    lines.append(f"{f_pr_out} gestures")
    lines.append(f"{f_pr_out + 2} pan 90 50 {ax:.1f} {ay:.1f}")
    f_pr_pan = f_pr_out + 4
    lines.append(f"{f_pr_pan} gestures")
    lines.append(f"{f_pr_pan + 2} smartzoom {ax:.1f} {ay:.1f}")
    f_pr_sz1 = f_pr_pan + 28
    lines.append(f"{f_pr_sz1} gestures")
    lines.append(f"{f_pr_sz1 + 2} smartzoom {ax:.1f} {ay:.1f}")
    f_pr_sz2 = f_pr_sz1 + 28
    lines.append(f"{f_pr_sz2} gestures")
    lines.append(f"{f_pr_sz2 + 2} quit discard")

    out = run_script("pl_pr_gestures", lines)
    g = parse_gestures(out)
    if 14 not in g:
        fail("playlist: missing initial gestures frame 14")
        return

    base = g[14]
    tick0 = (anchor_off + base["pl_sx"]) / base["pl_px"]
    worst_drift = 0.0
    prev_px = base["pl_px"]
    for idx in range(8):
        fr = 17 + idx * 2
        row = g.get(fr)
        if not row:
            fail(f"playlist pinch step {idx}: missing frame {fr}")
            continue
        if idx < 4 and not (row["pl_px"] > prev_px * 1.05):
            fail(f"playlist pinch-in step {idx}: zoom did not increase ({prev_px} -> {row['pl_px']})")
        if idx >= 4 and not (row["pl_px"] < prev_px * 0.95):
            fail(f"playlist pinch-out step {idx}: zoom did not decrease ({prev_px} -> {row['pl_px']})")
        drawn_x = tick0 * row["pl_px"] - row["pl_sx"]
        drift = abs(drawn_x - anchor_off)
        worst_drift = max(worst_drift, drift)
        prev_px = row["pl_px"]

    if worst_drift >= 1.0:
        fail(f"playlist pinch anchor drifted {worst_drift:.3f} px (>= 1.0 px)")
    else:
        ok(f"playlist anchored pinch: 8 steps, worst anchor drift {worst_drift:.4f} px (< 1.0 px)")

    final_pinch = g.get(31)
    if final_pinch and abs(math.log(final_pinch["pl_px"] / base["pl_px"])) < 0.01:
        ok(f"playlist symmetric pinch in/out returned cleanly ({base['pl_px']:.5f} -> {final_pinch['pl_px']:.5f})")
    else:
        fail("playlist symmetric pinch did not return to initial zoom")

    # 2D pan check.
    pb, pa = g.get(f_pan_before), g.get(f_pan_after)
    if pb and pa:
        ddx = pa["pl_sx"] - pb["pl_sx"]
        ddy = pa["pl_sy"] - pb["pl_sy"]
        if abs(ddx - 120.0) < 2.0 and abs(ddy - 64.0) < 2.0:
            ok(f"playlist 2D pan moved both axes cleanly (dx={ddx:.1f}, dy={ddy:.1f})")
        else:
            fail(f"playlist 2D pan mismatch: got dx={ddx:.2f}, dy={ddy:.2f} expected 120, 64")
    else:
        fail("playlist 2D pan: missing frames")

    # Smart-zoom check.
    sb, si, sr = g.get(f_sz_before), g.get(f_sz_in), g.get(f_sz_back)
    if sb and si and sr:
        if abs(math.log(si["pl_px"] / sb["pl_px"])) > 0.35 and abs(math.log(sr["pl_px"] / sb["pl_px"])) < 0.05:
            ok(f"playlist smart-zoom toggled cleanly ({sb['pl_px']:.5f} -> {si['pl_px']:.5f} -> {sr['pl_px']:.5f})")
        else:
            fail(f"playlist smart-zoom did not toggle cleanly ({sb['pl_px']:.5f} -> {si['pl_px']:.5f} -> {sr['pl_px']:.5f})")
    else:
        fail("playlist smart-zoom: missing frames")

    # Disabled setting check.
    ob, oa = g.get(f_off_before), g.get(f_off_after)
    if ob and oa:
        if (
            ob["enabled"] == 0
            and abs(oa["pl_px"] - ob["pl_px"]) < 1e-6
            and abs(oa["pl_sx"] - ob["pl_sx"]) < 1e-3
            and abs(oa["pl_sy"] - ob["pl_sy"]) < 1e-3
        ):
            ok("input.trackpadGestures=false disables pinch, smart-zoom, and pan cleanly")
        else:
            fail(f"disabled gestures still moved playlist: before={ob} after={oa}")
    else:
        fail("disabled gestures: missing frames")

    # Piano Roll checks.
    if not all(k in g for k in (f_pr_base, f_pr_in, f_pr_out, f_pr_pan, f_pr_sz1, f_pr_sz2)):
        fail("piano roll: missing gesture frames")
        return
    prb, pri, pro, prp, prs1, prs2 = g[f_pr_base], g[f_pr_in], g[f_pr_out], g[f_pr_pan], g[f_pr_sz1], g[f_pr_sz2]
    if pri["pr_px"] > prb["pr_px"] * 1.15 and abs(math.log(pro["pr_px"] / prb["pr_px"])) < 0.01:
        ok(f"piano roll pinch zoom in/out symmetric ({prb['pr_px']:.5f} -> {pri['pr_px']:.5f} -> {pro['pr_px']:.5f})")
    else:
        fail(f"piano roll pinch failed ({prb['pr_px']:.5f} -> {pri['pr_px']:.5f} -> {pro['pr_px']:.5f})")

    ddx = prp["pr_sx"] - pro["pr_sx"]
    ddy = prp["pr_sy"] - pro["pr_sy"]
    if abs(ddx - 90.0) < 2.0 and abs(ddy - 50.0) < 2.0:
        ok(f"piano roll 2D pan moved both axes cleanly (dx={ddx:.1f}, dy={ddy:.1f})")
    else:
        fail(f"piano roll 2D pan mismatch: dx={ddx:.2f}, dy={ddy:.2f}")

    if abs(math.log(prs1["pr_px"] / prp["pr_px"])) > 0.35 and abs(math.log(prs2["pr_px"] / prp["pr_px"])) < 0.05:
        ok(f"piano roll smart-zoom toggled cleanly ({prp['pr_px']:.5f} -> {prs1['pr_px']:.5f} -> {prs2['pr_px']:.5f})")
    else:
        fail(f"piano roll smart-zoom failed ({prp['pr_px']:.5f} -> {prs1['pr_px']:.5f} -> {prs2['pr_px']:.5f})")


def test_automation():
    probe = run_script("ae_probe", ["14 autoedit", "16 aepoints", "18 quit discard"], extra_args=["--show", "autoedit"])
    m = re.search(r"aepoints frame \d+: chip [-0-9. ]+ gridy ([-0-9.]+) ([-0-9.]+) gridx ([-0-9.]+) ([-0-9.]+)", probe)
    if not m:
        fail("automation probe: could not parse aepoints")
        return
    gy0, gy1, gx0, gx1 = float(m.group(1)), float(m.group(2)), float(m.group(3)), float(m.group(4))
    anchor_off = (gx1 - gx0) * 0.45
    ax = gx0 + anchor_off
    ay = (gy0 + gy1) * 0.5

    lines = [
        f"14 move {ax:.1f} {ay:.1f}",
        f"16 pan 120 0 {ax:.1f} {ay:.1f}",
        "18 autoedit",
        f"20 pinch 0.30 {ax:.1f} {ay:.1f}",
        "22 autoedit",
        f"24 pinch -0.30 {ax:.1f} {ay:.1f}",
        "26 autoedit",
        f"28 pan 80 0 {ax:.1f} {ay:.1f}",
        "30 autoedit",
        "32 quit discard",
    ]
    out = run_script("ae_gestures", lines, extra_args=["--show", "autoedit"])
    vals = {}
    for m in re.finditer(r"autoedit frame (\d+):[^\n]*view sx ([-0-9.]+) pxPerTick ([0-9.eE+-]+)", out):
        vals[int(m.group(1))] = (float(m.group(2)), float(m.group(3)))
    if not all(k in vals for k in (18, 22, 26, 30)):
        fail("automation editor: missing autoedit frames")
        return
    sx0, px0 = vals[18]
    sx1, px1 = vals[22]
    sx2, px2 = vals[26]
    sx3, _ = vals[30]
    tick0 = (anchor_off + sx0) / px0
    drift = abs((tick0 * px1 - sx1) - anchor_off)
    if px1 > px0 * 1.2 and drift < 1.0 and abs(math.log(px2 / px0)) < 0.01:
        ok(f"automation editor anchored pinch passed (drift {drift:.4f} px, symmetric return)")
    else:
        fail(f"automation editor pinch failed (px {px0}->{px1}->{px2}, drift {drift:.4f})")
    if abs((sx3 - sx2) - 80.0) < 2.0:
        ok(f"automation editor 2D pan moved cleanly (dx={sx3 - sx2:.1f})")
    else:
        fail(f"automation editor pan failed (dx={sx3 - sx2:.2f})")


def test_loupe():
    probe = run_script(
        "loupe_probe",
        ["14 loupe state", "16 quit discard"],
        extra_args=["--plugin", "gl.edison"],
        extra_env={"GRIDLINE_EDISON_DEMO": "1"},
    )
    # edison_ui.cpp prints: lanes lx0 lx1 ly0 ly1 ... view start len
    m = re.search(
        r"loupe state:[^\n]*lanes ([-0-9.]+) ([-0-9.]+) ([-0-9.]+) ([-0-9.]+)[^\n]*view ([-0-9.]+) ([-0-9.]+)",
        probe,
    )
    if not m:
        fail("loupe probe: could not parse loupe state")
        return
    lx0, lx1, ly0, ly1 = float(m.group(1)), float(m.group(2)), float(m.group(3)), float(m.group(4))
    w = lx1 - lx0
    ax = lx0 + w * 0.4
    ay = (ly0 + ly1) * 0.5
    lines = [
        f"14 move {ax:.1f} {ay:.1f}",
        "16 loupe state",
        f"18 pinch 0.40 {ax:.1f} {ay:.1f}",
        "20 loupe state",
        f"22 pinch -0.40 {ax:.1f} {ay:.1f}",
        "24 loupe state",
        f"26 smartzoom {ax:.1f} {ay:.1f}",
        "28 loupe state",
        f"30 pan 60 0 {ax:.1f} {ay:.1f}",
        "32 loupe state",
        "34 quit discard",
    ]
    out = run_script(
        "loupe_gestures",
        lines,
        extra_args=["--plugin", "gl.edison"],
        extra_env={"GRIDLINE_EDISON_DEMO": "1"},
    )
    views = [
        (float(m.group(1)), float(m.group(2)))
        for m in re.finditer(r"loupe state:[^\n]*view ([-0-9.]+) ([-0-9.]+)", out)
    ]
    if len(views) < 5:
        fail(f"loupe: expected 5 state lines, got {len(views)}")
        return
    (s0, l0), (s1, l1), (s2, l2), (s3, l3), (s4, l4) = views[:5]
    anchor_frame = s0 + 0.4 * l0
    anchor_px_after = ((anchor_frame - s1) / l1) * w
    drift = abs(anchor_px_after - 0.4 * w)
    if l1 < l0 * 0.75 and drift < 1.0 and abs(math.log(l2 / l0)) < 0.02:
        ok(f"loupe anchored pinch passed (len {l0:.0f}->{l1:.0f}->{l2:.0f}, drift {drift:.4f} px)")
    else:
        fail(f"loupe pinch failed (len {l0:.0f}->{l1:.0f}->{l2:.0f}, drift {drift:.4f})")
    if l3 < l2 * 0.5:
        ok(f"loupe smart-zoom zoomed in cleanly ({l2:.0f} -> {l3:.0f})")
    else:
        fail(f"loupe smart-zoom did not zoom in ({l2:.0f} -> {l3:.0f})")
    if s4 > s3 + 10.0:
        ok(f"loupe 2-finger pan moved horizontally ({s3:.0f} -> {s4:.0f})")
    else:
        fail(f"loupe 2-finger pan did not move ({s3:.0f} -> {s4:.0f})")


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    test_playlist_and_pianoroll()
    test_automation()
    test_loupe()
    if failures:
        print(f"{len(failures)} failure(s)")
        sys.exit(1)
    print("all passed")
