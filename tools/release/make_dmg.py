#!/usr/bin/env python3
"""Builds the Gridline disk image with its window design (package-macos.sh calls it).

    make_dmg.py <dmgroot> <out.dmg> <volume name>
    make_dmg.py --check <mount point>    checks a mounted image has the window design

The window (tools/release/dmg/): background.png + background@2x.png (joined into one
multi-resolution TIFF), the window size, icon size and icon positions from layout.json
(written by tools/make_installer_art.py), the volume icon (assets/icons/Gridline.icns), an
Applications link, no toolbar / sidebar / status bar. Gridline.app and Applications take their
places in the main row; every other item of <dmgroot> goes into the extras row, in the order
layout.json lists, then by name.

The Finder view settings (.DS_Store) and the background alias are written directly with
dmgbuild (the ds_store / mac_alias modules): no Finder, no AppleScript, so it runs headless on CI
machines. Needs dmgbuild (pip install dmgbuild); hdiutil and SetFile come with macOS / Xcode's
command line tools.
"""
import json
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.dirname(os.path.dirname(HERE))
TITLE_BAR = 28  # Finder stores the window frame: the background fills the content below the title bar


def settings_for(root):
    with open(os.path.join(HERE, "dmg", "layout.json")) as f:
        L = json.load(f)
    w, h = L["window"]
    names = sorted(n for n in os.listdir(root) if not n.startswith("."))
    if "Applications" in names:
        names.remove("Applications")  # the link is made by dmgbuild
    main = L["main"]
    order = L["extras"]["order"]
    extras = [n for n in order if n in names] + sorted(n for n in names if n not in main and n not in order)
    pitch = min(L["extras"]["pitch"], (w - 80) / max(1, len(extras)))
    x0 = w / 2 - pitch * (len(extras) - 1) / 2
    locs = {n: tuple(p) for n, p in main.items()}
    for i, n in enumerate(extras):
        locs[n] = (round(x0 + i * pitch), L["extras"]["y"])
    return {
        "format": "UDZO",
        "compression_level": 9,
        "filesystem": "HFS+",
        "files": [os.path.join(root, n) for n in names],
        "symlinks": {"Applications": "/Applications"},
        "icon": os.path.join(SRC, "assets", "icons", "Gridline.icns"),
        "background": os.path.join(HERE, "dmg", "background.png"),  # + background@2x.png
        "window_rect": ((200, 140), (w, h + TITLE_BAR)),
        "default_view": "icon-view",
        "show_status_bar": False,
        "show_tab_view": False,
        "show_toolbar": False,
        "show_pathbar": False,
        "show_sidebar": False,
        "show_icon_preview": False,
        "include_icon_view_settings": True,
        "include_list_view_settings": False,
        "arrange_by": None,
        "label_pos": "bottom",
        "icon_size": L["icon_size"],
        "text_size": L["text_size"],
        "icon_locations": locs,
        # No hide_extensions: its Finder flag on Gridline.app would break the code signature
        # (codesign: "Finder information ... not allowed"); Finder hides ".app" by default anyway.
    }


def check(mp):
    """The mounted image has the window design: view settings with the background picture,
    both resolutions of the background, the volume icon and the main row in place."""
    import plistlib
    import subprocess

    from ds_store import DSStore
    with open(os.path.join(HERE, "dmg", "layout.json")) as f:
        L = json.load(f)
    errors = []
    for name in (".DS_Store", ".background.tiff", ".VolumeIcon.icns"):
        if not os.path.isfile(os.path.join(mp, name)):
            errors.append(f"missing {name}")
    if os.readlink(os.path.join(mp, "Applications")) != "/Applications":
        errors.append("Applications is not a link to /Applications")
    if not errors:
        locs, icvp, bwsp = {}, {}, {}
        with DSStore.open(os.path.join(mp, ".DS_Store"), "r") as d:
            for e in d:
                v = e.value
                if isinstance(v, (bytes, bytearray)) and e.code in (b"icvp", b"bwsp"):
                    v = plistlib.loads(v)
                if e.code == b"Iloc":
                    locs[e.filename] = tuple(v)
                elif e.filename == "." and e.code == b"icvp":
                    icvp = v
                elif e.filename == "." and e.code == b"bwsp":
                    bwsp = v
        if icvp.get("backgroundType") != 2 or "backgroundImageAlias" not in icvp:
            errors.append("no background picture in the view settings")
        if icvp.get("iconSize") != L["icon_size"]:
            errors.append(f"icon size {icvp.get('iconSize')}")
        w, h = L["window"]
        if bwsp.get("WindowBounds", "").replace(" ", "").split("},{")[-1] != f"{w},{h + TITLE_BAR}}}}}":
            errors.append(f"window bounds {bwsp.get('WindowBounds')}")
        for n, p in L["main"].items():
            if locs.get(n) != tuple(p):
                errors.append(f"{n} at {locs.get(n)}, expected {tuple(p)}")
        for n in os.listdir(mp):
            if not n.startswith(".") and n not in locs:
                errors.append(f"{n} has no position")
        info = subprocess.run(["/usr/bin/tiffutil", "-info", os.path.join(mp, ".background.tiff")],
                              capture_output=True, text=True).stdout
        sizes = [ln.split() for ln in info.splitlines() if "Image Width" in ln]
        want = [["Image", "Width:", str(w), "Image", "Length:", str(h)], ["Image", "Width:", str(2 * w), "Image", "Length:", str(2 * h)]]
        if sizes != want:
            errors.append(f"background resolutions {sizes}")
    for e in errors:
        print("dmg check:", e, file=sys.stderr)
    if errors:
        sys.exit(1)
    print("dmg window design: ok")


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "--check":
        check(sys.argv[2])
        return
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    root, out, volname = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2]), sys.argv[3]
    import dmgbuild  # noqa: E402 (a clear error when it is missing: package-macos.sh checks first)
    settings = settings_for(root)
    for attempt in range(5):  # hdiutil is flaky on CI machines ("resource busy")
        try:
            if os.path.exists(out):
                os.remove(out)
            dmgbuild.build_dmg(out, volname, settings=settings)
            break
        except Exception as e:  # noqa: BLE001
            if attempt == 4:
                raise
            print(f"dmgbuild failed ({e}); retrying", file=sys.stderr)
            time.sleep(5)
    print(out)


if __name__ == "__main__":
    main()
