#!/usr/bin/env python3
"""Generate the installer artwork (macOS disk image, Windows setup) from Gridline's brand.

Everything is drawn from the brand sources: the Tile mark construction (logo guide v1.0),
Ink / Paper and the surfaces 950 / 900 / 800 / 100, Inter and JetBrains Mono (assets/fonts),
the step plane of the README banner (tools/make_readme_art.py) and the app icon
(tools/make_app_icon.py). No screenshots, no third-party art.

Outputs (the default direction, committed; the packaging reads them):
  tools/release/dmg/background.png, background@2x.png
                     the disk image window (1x and 2x; make_dmg.py joins them into one
                     multi-resolution TIFF)
  tools/release/dmg/layout.json
                     window size, icon size, icon positions and the extras row: the single
                     source for make_dmg.py and the previews
  tools/windows/art/wizard-<scale>.png
                     Inno Setup WizardImageFile, one per DPI scale (100 ... 250), 164:314
  tools/windows/art/wizard-small-<scale>.png
                     Inno Setup WizardSmallImageFile (the app icon, transparent), square
  tools/windows/art/setup.ico
                     the setup program's icon (also the uninstaller's): the app icon with an
                     install badge

Options:
  --direction NAME   studio (default), graphite or paper
  --preview DIR      also write preview sheets of every direction (dmg window at 1x and 2x,
                     wizard pages, small images, icons) into DIR
  --check            exit 1 when a committed file differs from what this script makes

Finder draws icon labels in black on any window with a background picture (in light and dark
mode), so every label sits on a light area: plates under the main pair, a Paper shelf (or a light
window) for the extras.

Requires Pillow. Usage: python3 tools/make_installer_art.py [options] [gridline root]
"""
import io
import json
import math
import os
import random
import struct
import sys

from PIL import Image, ImageChops, ImageDraw, ImageFilter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import make_app_icon as appicon  # noqa: E402
import make_readme_art as art  # noqa: E402

PAPER, INK = (255, 255, 255), (0, 0, 0)
S950, S900, S800, S100 = (11, 11, 11), (20, 20, 20), (31, 31, 31), (242, 242, 242)
U, R = 120.0, 28.0  # Tile construction: 120u canvas, 28u corner radius

DIRECTIONS = ("graphite", "paper", "studio")
DEFAULT = "studio"

# Inno Setup 6.6 image areas per DPI scale (jrsoftware.org/ishelp: WizardImageFile,
# WizardSmallImageFile). Each large image keeps the 164:314 aspect ratio Inno maintains.
WIZARD_SCALES = (100, 125, 150, 175, 200, 225, 250)
WIZARD_AREA = {100: (202, 386), 125: (269, 515), 150: (336, 643), 175: (403, 772),
               200: (430, 824), 225: (498, 953), 250: (534, 1022)}
SMALL_AREA = {100: 58, 125: 77, 150: 97, 175: 116, 200: 124, 225: 143, 250: 159}
ICO_SIZES = (16, 20, 24, 32, 40, 48, 64, 96, 128, 256)


# ---------------------------------------------------------------- shared drawing

def tint(W, H, rgb, mask, opacity):
    return art._tint(W, H, rgb, mask, opacity)


def mark(size, rgb=PAPER):
    """The flat Tile mark (true holes) as an RGBA image."""
    return tint(size, size, rgb, art.mark_mask(size), 1.0)


def lockup(img, root, x, y, mh, rgb=PAPER, halo=0.0):
    """Mark + wordmark (logo guide: gap 0.2 x mark, wordmark at 0.86 x mark height).
    Returns the right edge."""
    W, H = img.size
    if halo:
        h = Image.new("L", (W, H), 0)
        ImageDraw.Draw(h).rounded_rectangle([x, y, x + mh, y + mh], radius=mh * R / U, fill=255)
        img.alpha_composite(tint(W, H, rgb, h.filter(ImageFilter.GaussianBlur(mh * 0.7)), halo))
    img.alpha_composite(mark(mh, rgb), (round(x), round(y)))
    d = ImageDraw.Draw(img)
    word = art.font(root, "Inter-Bold.ttf", mh * 0.86)
    bb = d.textbbox((0, 0), "gridline", font=word)
    tx = x + mh * 1.2
    ty = y + (mh - (bb[3] - bb[1])) / 2 - bb[1]
    tr = -0.025 * mh * 0.86
    art._tracked_word(img, (tx, ty), "gridline", word, rgb, tr)
    return tx + art.text_width(d, "gridline", word, tr)


def grain(img, amount, seed=7, rgb=PAPER):
    W, H = img.size
    rnd = random.Random(seed)
    n = Image.new("L", (W // 2 or 1, H // 2 or 1))
    n.putdata([rnd.randrange(0, 255) for _ in range(n.width * n.height)])
    img.alpha_composite(tint(W, H, rgb, n.resize((W, H), Image.NEAREST), amount))


def construction_grid(W, H, cell, ox, oy, major=4, strong=255, weak=110, width=1):
    """L mask of the 10u construction grid; every `major`th line brighter."""
    g = Image.new("L", (W, H), 0)
    d = ImageDraw.Draw(g)
    i = 0
    x = ox % cell
    while x < W:
        d.line([(x, 0), (x, H)], fill=strong if round((x - ox) / cell) % major == 0 else weak, width=width)
        x += cell
        i += 1
    y = oy % cell
    while y < H:
        d.line([(0, y), (W, y)], fill=strong if round((y - oy) / cell) % major == 0 else weak, width=width)
        y += cell
    return g


def step_arrow(img, x0, x1, cy, k, rgb, n=7, cell=9.0, alpha_lo=0.16, alpha_hi=1.0):
    """The install arrow: a row of step cells (the Tile's corner ratio) brightening towards
    the target, ending in a chevron built from two bars of the mark's stroke."""
    W, H = img.size
    lay = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(lay)
    s = cell * k
    chev = 11 * k
    span = (x1 - x0) * k - chev - 14 * k
    pitch = (span - s) / (n - 1)
    for i in range(n):
        t = i / (n - 1)
        a = alpha_lo + (alpha_hi - alpha_lo) * (t ** 1.4)
        x = x0 * k + i * pitch
        d.rounded_rectangle([x, cy * k - s / 2, x + s, cy * k + s / 2], radius=s * R / U, fill=rgb + (round(255 * a),))
    # Chevron: two strokes 10u wide on the mark's scale, meeting at the tip.
    tip = x1 * k
    bw = 2.4 * k
    pts = [(tip - chev, cy * k - chev), (tip, cy * k), (tip - chev, cy * k + chev)]
    d.line(pts, fill=rgb + (255,), width=round(bw), joint="curve")
    img.alpha_composite(lay)


def plate(img, cx, top, w, h, k, fill, alpha=255, edge=None):
    """A label plate: a rounded light field under a Finder label."""
    W, H = img.size
    lay = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(lay)
    box = [(cx - w / 2) * k, top * k, (cx + w / 2) * k, (top + h) * k]
    d.rounded_rectangle(box, radius=h * k / 2, fill=fill + (alpha,),
                        outline=(edge + (90,)) if edge else None, width=max(1, k // 2) if edge else 0)
    img.alpha_composite(lay)


# ---------------------------------------------------------------- disk image

def dmg_layout(direction):
    """Points (1x). Positions are icon centres, as Finder stores them (Iloc)."""
    if direction == "studio":
        return {
            "window": [720, 520], "icon_size": 96, "text_size": 12,
            "main": {"Gridline.app": [196, 272], "Applications": [524, 272]},
            "extras": {"y": 434, "pitch": 150, "order": ["Plugins", "gridline", "README.txt"]},
        }
    return {
        "window": [720, 480], "icon_size": 96, "text_size": 12,
        "main": {"Gridline.app": [196, 188], "Applications": [524, 188]},
        "extras": {"y": 400, "pitch": 150, "order": ["Plugins", "gridline", "README.txt"]},
    }


LABEL_GAP = 4      # points between the icon's bottom and its label's top (Finder, 12 pt text)
LABEL_H = 16       # label line height at 12 pt


def _label_plate(img, k, cx, cy, isz, fill, alpha, edge=None):
    plate(img, cx, cy + isz / 2 + LABEL_GAP - 4, 120, LABEL_H + 8, k, fill, alpha, edge)


def dmg_background(root, direction, k):
    L = dmg_layout(direction)
    w, h = L["window"]
    W, H = w * k, h * k
    isz = L["icon_size"]
    (ax, ay), (bx, by) = L["main"]["Gridline.app"], L["main"]["Applications"]
    ey = L["extras"]["y"]
    mono = art.font(root, "JetBrainsMono-Regular.ttf", 10 * k)
    mono_b = art.font(root, "JetBrainsMono-Bold.ttf", 10 * k)
    inter = art.font(root, "Inter-Regular.ttf", 11 * k)

    if direction == "paper":
        img = Image.new("RGBA", (W, H), S100 + (255,))
        img.alpha_composite(tint(W, H, PAPER, art.radial_mask(W, H, W * 0.5, H * 0.38, W * 0.62, H * 0.8, 255, 0, 1.6), 0.55))
        g = construction_grid(W, H, 20 * k, (w // 2) * k, ay * k, width=max(1, k // 2))
        g = ImageChops.multiply(g, art.radial_mask(W, H, W * 0.5, ay * k, W * 0.55, H * 0.75, 255, 0, 1.3))
        img.alpha_composite(tint(W, H, INK, g, 0.07))
        lockup(img, root, 40 * k, 34 * k, 24 * k, INK)
        d = ImageDraw.Draw(img)
        txt = "THE FREE, OPEN SOURCE MUSIC WORKSTATION"
        tw = art.text_width(d, txt, mono, 1.6 * k)
        art.tracked(d, (W - 40 * k - tw, 40 * k), txt, mono, (138, 138, 138), 1.6 * k)
        step_arrow(img, ax + isz / 2 + 26, bx - isz / 2 - 26, ay, k, INK)
        t = "DRAG TO APPLICATIONS"
        tw = art.text_width(d, t, mono_b, 2.2 * k)
        art.tracked(d, ((W - tw) / 2, (ay - 34) * k), t, mono_b, (64, 64, 64), 2.2 * k)
        # Extras: a hairline rule with a heading.
        ry = (ey - 74) * k
        d.line([(40 * k, ry), (W - 40 * k, ry)], fill=(208, 208, 208), width=max(1, k // 2 * 1 or 1))
        art.tracked(d, (40 * k, ry + 12 * k), "OPTIONAL", mono_b, (107, 107, 107), 2.2 * k)
        cap = "Plugins for other hosts and the command line tool"
        cw = d.textlength(cap, font=inter)
        d.text((W - 40 * k - cw, ry + 10 * k), cap, font=inter, fill=(138, 138, 138))
        grain(img, 0.012, rgb=INK)
        return img

    # Dark directions share the graphite field.
    img = Image.new("RGBA", (W, H), S950 + (255,))
    img.alpha_composite(tint(W, H, PAPER, art.radial_mask(W, H, W * 0.5, H * 0.30, W * 0.70, H * 0.85, 255, 0, 1.5), 0.075))

    if direction == "graphite":
        g = construction_grid(W, H, 20 * k, (w // 2) * k, ay * k, width=max(1, k // 2))
        g = ImageChops.multiply(g, art.radial_mask(W, H, W * 0.5, ay * k, W * 0.6, H * 0.7, 255, 0, 1.2))
        img.alpha_composite(tint(W, H, PAPER, g, 0.055))
        # Wells: a soft lift under the two main icons.
        for cx in (ax, bx):
            m = Image.new("L", (W, H), 0)
            ImageDraw.Draw(m).ellipse([(cx - 86) * k, (ay - 70) * k, (cx + 86) * k, (ay + 74) * k], fill=255)
            img.alpha_composite(tint(W, H, PAPER, m.filter(ImageFilter.GaussianBlur(36 * k)), 0.06))
        lockup(img, root, 40 * k, 34 * k, 24 * k, PAPER, halo=0.10)
        d = ImageDraw.Draw(img)
        txt = "THE FREE, OPEN SOURCE MUSIC WORKSTATION"
        tw = art.text_width(d, txt, mono, 1.6 * k)
        art.tracked(d, (W - 40 * k - tw, 40 * k), txt, mono, (96, 96, 96), 1.6 * k)
        step_arrow(img, ax + isz / 2 + 26, bx - isz / 2 - 26, ay, k, PAPER)
        t = "DRAG TO APPLICATIONS"
        tw = art.text_width(d, t, mono_b, 2.2 * k)
        art.tracked(d, ((W - tw) / 2, (ay - 34) * k), t, mono_b, (170, 170, 170), 2.2 * k)
        for cx in (ax, bx):
            _label_plate(img, k, cx, ay, isz, (226, 226, 226), 255)
        # The shelf: a Paper-100 band for the extras (labels stay legible for any number of items).
        top = (ey - 82) * k
        shelf = Image.new("RGBA", (W, H), (0, 0, 0, 0))
        sd = ImageDraw.Draw(shelf)
        sd.rectangle([0, top, W, H], fill=S100 + (255,))
        shadow = Image.new("L", (W, H), 0)
        ImageDraw.Draw(shadow).rectangle([0, top - 2 * k, W, top + 8 * k], fill=255)
        img.alpha_composite(tint(W, H, INK, shadow.filter(ImageFilter.GaussianBlur(6 * k)), 0.5))
        img.alpha_composite(shelf)
        d = ImageDraw.Draw(img)
        d.line([(0, top), (W, top)], fill=(255, 255, 255), width=max(1, k // 2))
        art.tracked(d, (40 * k, top + 14 * k), "OPTIONAL", mono_b, (107, 107, 107), 2.2 * k)
        cap = "Plugins for other hosts and the command line tool"
        cw = d.textlength(cap, font=inter)
        d.text((W - 40 * k - cw, top + 12 * k), cap, font=inter, fill=(138, 138, 138))
        grain(img, 0.014)
        return img

    # studio: a dark hero band in the README banner's family over a light stage.
    hero = 168
    HH = hero * k
    PW, PH = 1700 * k // 2, 1060 * k // 2
    cells, glow = art._plane(PW, PH, k)
    quad = [(W * 0.50, HH * 0.10), (W * 1.10, HH * -0.25), (W * 1.04, HH * 1.30), (W * 0.46, HH * 0.98)]
    coeffs = art.perspective_coeffs(quad, [(0, 0), (PW, 0), (PW, PH), (0, PH)])
    plane = cells.transform((W, HH), Image.PERSPECTIVE, coeffs, Image.BICUBIC)
    fade = art.hfade(W, HH, W * 0.46, W * 0.70, 0, 255)
    plane.putalpha(ImageChops.multiply(plane.getchannel("A"), fade))
    band = Image.new("RGBA", (W, HH), S950 + (255,))
    band.alpha_composite(tint(W, HH, PAPER, art.radial_mask(W, HH, W * 0.22, HH * 0.5, W * 0.5, HH * 1.2, 255, 0, 1.4), 0.07))
    g = construction_grid(W, HH, 20 * k, 40 * k, 40 * k, width=max(1, k // 2))
    band.alpha_composite(tint(W, HH, PAPER, g, 0.045))
    pl = Image.new("RGBA", (W, HH), (0, 0, 0, 0))
    pl.alpha_composite(plane)
    pl.putalpha(pl.getchannel("A").point(lambda v: round(v * 0.85)))
    band.alpha_composite(pl)
    vig = art.vfade(W, HH, HH * 0.55, HH, 0, 255)
    band.alpha_composite(tint(W, HH, S950, vig, 0.55))
    img = Image.new("RGBA", (W, H), S100 + (255,))
    img.alpha_composite(tint(W, H, PAPER, art.radial_mask(W, H, W * 0.5, ay * k, W * 0.6, H * 0.6, 255, 0, 1.6), 0.6))
    gs = construction_grid(W, H, 20 * k, (w // 2) * k, ay * k, width=max(1, k // 2))
    gs = ImageChops.multiply(gs, art.radial_mask(W, H, W * 0.5, ay * k, W * 0.55, H * 0.45, 255, 0, 1.3))
    img.alpha_composite(tint(W, H, INK, gs, 0.06))
    img.alpha_composite(band, (0, 0))
    lockup(img, root, 44 * k, 52 * k, 34 * k, PAPER, halo=0.10)
    d = ImageDraw.Draw(img)
    sub = art.font(root, "Inter-Regular.ttf", 13 * k)
    d.text((45 * k, 100 * k), "The free, open source music workstation", font=sub, fill=(150, 150, 150))
    step_arrow(img, ax + isz / 2 + 26, bx - isz / 2 - 26, ay, k, INK)
    t = "DRAG TO APPLICATIONS"
    tw = art.text_width(d, t, mono_b, 2.2 * k)
    art.tracked(d, ((W - tw) / 2, (ay - 34) * k), t, mono_b, (64, 64, 64), 2.2 * k)
    ry = (ey - 74) * k
    d.line([(40 * k, ry), (W - 40 * k, ry)], fill=(212, 212, 212), width=max(1, k // 2))
    art.tracked(d, (40 * k, ry + 12 * k), "OPTIONAL", mono_b, (107, 107, 107), 2.2 * k)
    cap = "Plugins for other hosts and the command line tool"
    cw = d.textlength(cap, font=inter)
    d.text((W - 40 * k - cw, ry + 10 * k), cap, font=inter, fill=(138, 138, 138))
    grain(img, 0.010, rgb=INK)
    return img


def extras_positions(layout, names):
    """Centres of the extras row for `names` (in order), centred in the window."""
    w = layout["window"][0]
    ex = layout["extras"]
    n = len(names)
    pitch = min(ex["pitch"], (w - 80) / max(1, n))  # more items: closer together, still inside
    x0 = w / 2 - pitch * (n - 1) / 2
    return {nm: [round(x0 + i * pitch), ex["y"]] for i, nm in enumerate(names)}


# ---------------------------------------------------------------- Windows

def wizard_image(root, direction, size):
    """Inno WizardImageFile at `size` (w, h); designed on the 164 x 314 base, rendered at 2x."""
    w, h = size
    k = 2
    W, H = w * k, h * k
    s = W / 164.0  # base unit -> pixels at 2x
    if direction == "paper":
        img = Image.new("RGBA", (W, H), S100 + (255,))
        img.alpha_composite(tint(W, H, PAPER, art.radial_mask(W, H, W * 0.5, H * 0.3, W * 0.9, H * 0.6, 255, 0, 1.5), 0.6))
        g = construction_grid(W, H, round(10 * s), round(82 * s), round(96 * s), width=max(1, round(s * 0.5)))
        g = ImageChops.multiply(g, art.radial_mask(W, H, W * 0.5, H * 0.32, W * 0.9, H * 0.55, 255, 0, 1.2))
        img.alpha_composite(tint(W, H, INK, g, 0.07))
        fg, word_rgb = INK, INK
    else:
        img = Image.new("RGBA", (W, H), S950 + (255,))
        img.alpha_composite(tint(W, H, PAPER, art.radial_mask(W, H, W * 0.5, H * 0.30, W * 0.95, H * 0.55, 255, 0, 1.5), 0.085))
        g = construction_grid(W, H, round(10 * s), round(82 * s), round(96 * s), width=max(1, round(s * 0.5)))
        g = ImageChops.multiply(g, art.radial_mask(W, H, W * 0.5, H * 0.32, W * 0.95, H * 0.6, 255, 0, 1.2))
        img.alpha_composite(tint(W, H, PAPER, g, 0.06))
        fg, word_rgb = PAPER, PAPER
    if direction in ("graphite", "paper"):
        # A column of step cells under the mark, one bar of a beat, fading down.
        steps = "2000100000102010"  # two rows of one bar, read left to right
        cs = 7.0 * s
        pitch = 10.0 * s
        x0 = W / 2 - (8 * pitch - (pitch - cs)) / 2
        y0 = 196 * s
        lay = Image.new("RGBA", (W, H), (0, 0, 0, 0))
        dl = ImageDraw.Draw(lay)
        for i, v in enumerate(steps):
            r, c = divmod(i, 8)
            x, y = x0 + c * pitch, y0 + r * pitch
            if v == "0":
                fill = fg + (40 if direction == "graphite" else 30,)
            else:
                fill = fg + (255 if v == "2" else 170,)
            dl.rounded_rectangle([x, y, x + cs, y + cs], radius=cs * R / U, fill=fill)
        fade = art.hfade(W, H, x0, x0 + 8 * pitch, 140, 255)
        lay.putalpha(ImageChops.multiply(lay.getchannel("A"), fade))
        img.alpha_composite(lay)
    else:  # studio: the step plane in perspective, rising from the bottom
        PW, PH = 1700 * k // 2, 1060 * k // 2
        cells, glow = art._plane(PW, PH, k)
        quad = [(W * -0.10, H * 0.56), (W * 1.55, H * 0.50), (W * 1.40, H * 1.05), (W * -0.25, H * 1.02)]
        coeffs = art.perspective_coeffs(quad, [(0, 0), (PW, 0), (PW, PH), (0, PH)])
        plane = cells.transform((W, H), Image.PERSPECTIVE, coeffs, Image.BICUBIC)
        fade = art.vfade(W, H, H * 0.55, H * 0.80, 0, 255)
        plane.putalpha(ImageChops.multiply(plane.getchannel("A"), fade))
        img.alpha_composite(plane)
    # The mark with its wordmark, centred in the upper third (clear space 24u kept).
    mh = round(46 * s)
    mx, my = round(W / 2 - mh / 2), round(70 * s)
    if direction != "paper":
        hm = Image.new("L", (W, H), 0)
        ImageDraw.Draw(hm).rounded_rectangle([mx, my, mx + mh, my + mh], radius=mh * R / U, fill=255)
        img.alpha_composite(tint(W, H, PAPER, hm.filter(ImageFilter.GaussianBlur(mh * 0.6)), 0.10))
    img.alpha_composite(mark(mh, fg), (mx, my))
    d = ImageDraw.Draw(img)
    word = art.font(root, "Inter-Bold.ttf", 21 * s)
    tr = -0.025 * 21 * s
    ww = art.text_width(d, "gridline", word, tr)
    bb = d.textbbox((0, 0), "gridline", font=word)
    art._tracked_word(img, (W / 2 - ww / 2, my + mh + 16 * s - bb[1]), "gridline", word, word_rgb, tr)
    grain(img, 0.014 if direction != "paper" else 0.010, rgb=PAPER if direction != "paper" else INK)
    out = img.convert("RGB").resize((w, h), Image.LANCZOS)
    return out


def small_image(n):
    """WizardSmallImageFile: the app icon with transparency, n x n."""
    return appicon.render(n) if n in (16, 24, 32, 48) or n >= 64 else appicon.render_large(n * 2).resize((n, n), Image.LANCZOS)


def setup_icon_image(n):
    """The setup program's icon: the app icon with an install badge (a Paper disc with an
    Ink arrow into a tray) at 32 px and larger; the plain app icon below that."""
    if n < 32:
        return appicon.render(n)
    big = 512
    base = appicon.render(big)
    k = 4
    B = big * k
    lay = Image.new("RGBA", (B, B), (0, 0, 0, 0))
    d = ImageDraw.Draw(lay)
    r = 0.165 * B
    cx, cy = 0.80 * B, 0.80 * B
    sh = Image.new("L", (B, B), 0)
    ImageDraw.Draw(sh).ellipse([cx - r, cy - r + 0.012 * B, cx + r, cy + r + 0.012 * B], fill=255)
    lay.alpha_composite(tint(B, B, INK, sh.filter(ImageFilter.GaussianBlur(0.02 * B)), 0.55))
    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=(250, 250, 250, 255))
    d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=(0, 0, 0, 60), width=round(0.006 * B))
    sw = 0.040 * B
    # Arrow: shaft and head, then the tray.
    top, tip = cy - 0.092 * B, cy + 0.028 * B
    d.line([(cx, top), (cx, tip - sw * 0.2)], fill=INK + (255,), width=round(sw))
    hw = 0.056 * B
    d.line([(cx - hw, tip - hw), (cx, tip), (cx + hw, tip - hw)], fill=INK + (255,), width=round(sw), joint="curve")
    ty = cy + 0.084 * B
    d.line([(cx - 0.084 * B, ty - 0.04 * B), (cx - 0.084 * B, ty), (cx + 0.084 * B, ty), (cx + 0.084 * B, ty - 0.04 * B)],
           fill=INK + (255,), width=round(sw * 0.8), joint="curve")
    badge = lay.resize((big, big), Image.LANCZOS)
    base.alpha_composite(badge)
    return base.resize((n, n), Image.LANCZOS)


def ico_bytes(images):
    """A .ico holding one PNG per size (Windows Vista and later)."""
    blobs = []
    for im in images:
        b = io.BytesIO()
        im.save(b, "PNG", optimize=True)
        blobs.append((im.width, b.getvalue()))
    out = struct.pack("<HHH", 0, 1, len(blobs))
    off = 6 + 16 * len(blobs)
    ent, data = b"", b""
    for s, blob in blobs:
        dim = 0 if s >= 256 else s
        ent += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(blob), off + len(data))
        data += blob
    return out + ent + data


# ---------------------------------------------------------------- previews (mock-ups)

def _folder(n, k, light=True):
    """A generic folder glyph for the mock-ups (not Apple's art: a plain blue folder)."""
    S = n * k
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    top = S * 0.22
    d.rounded_rectangle([S * 0.06, top, S * 0.46, top + S * 0.16], radius=S * 0.05, fill=(84, 160, 220, 255))
    d.rounded_rectangle([S * 0.06, top + S * 0.08, S * 0.94, S * 0.84], radius=S * 0.06, fill=(104, 178, 236, 255))
    d.rounded_rectangle([S * 0.06, top + S * 0.16, S * 0.94, S * 0.84], radius=S * 0.06, fill=(126, 194, 246, 255))
    return im


def _doc(n, k, dark=False, label=None, root=None):
    S = n * k
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    x0, x1, y0, y1 = S * 0.2, S * 0.8, S * 0.08, S * 0.92
    d.rectangle([x0, y0, x1, y1], fill=(24, 24, 24, 255) if dark else (252, 252, 252, 255), outline=(0, 0, 0, 70), width=max(1, k))
    if dark and root:
        f = art.font(root, "JetBrainsMono-Regular.ttf", S * 0.09)
        d.text((x0 + S * 0.05, y0 + S * 0.05), "exec", font=f, fill=(90, 220, 90, 255))
    elif not dark:
        for i in range(8):
            y = y0 + S * 0.12 + i * S * 0.075
            d.line([(x0 + S * 0.07, y), (x1 - S * (0.07 if i % 3 else 0.2), y)], fill=(180, 180, 180, 255), width=max(1, k))
    return im


def dmg_mock(root, direction, k, names=None):
    """The disk image window as Finder shows it: title bar, background, icons, black labels."""
    L = dmg_layout(direction)
    w, h = L["window"]
    names = names or L["extras"]["order"]
    bg = dmg_background(root, direction, k)
    bar = 28
    W, H = w * k, (h + bar) * k
    win = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    m = Image.new("L", (W, H), 0)
    ImageDraw.Draw(m).rounded_rectangle([0, 0, W - 1, H - 1], radius=10 * k, fill=255)
    body = Image.new("RGBA", (W, H), (236, 236, 236, 255))
    d = ImageDraw.Draw(body)
    d.rectangle([0, 0, W, bar * k], fill=(232, 232, 232, 255))
    d.line([(0, bar * k - 1), (W, bar * k - 1)], fill=(200, 200, 200, 255), width=max(1, k // 2))
    for i, c in enumerate([(255, 95, 87), (254, 188, 46), (40, 200, 64)]):
        cx = (20 + i * 20) * k
        d.ellipse([cx - 6 * k, 14 * k - 6 * k, cx + 6 * k, 14 * k + 6 * k], fill=c + (255,))
    tf = art.font(root, "Inter-SemiBold.ttf", 13 * k)
    title = "Gridline 0.7.0-beta.1"
    tw = d.textlength(title, font=tf)
    d.text(((W - tw) / 2, 6 * k), title, font=tf, fill=(60, 60, 60, 255))
    body.alpha_composite(bg, (0, bar * k))
    isz = L["icon_size"]
    lf = art.font(root, "Inter-Regular.ttf", L["text_size"] * k)
    items = dict(L["main"])
    items.update(extras_positions(L, names))
    for name, (cx, cy) in items.items():
        if name == "Gridline.app":
            ic = appicon.render(isz * k)
        elif name in ("Applications", "Plugins"):
            ic = _folder(isz, k)
        elif name == "gridline":
            ic = _doc(isz, k, dark=True, root=root)
        else:
            ic = _doc(isz, k)
        body.alpha_composite(ic, (round((cx - isz / 2) * k), round((cy - isz / 2 + bar) * k)))
        label = name[:-4] if name.endswith(".app") else name  # Finder hides ".app" by default
        lw = d.textlength(label, font=lf)
        d.text(((cx * k - lw / 2), (cy + isz / 2 + LABEL_GAP + bar) * k), label, font=lf, fill=(0, 0, 0, 255))
    win.alpha_composite(body)
    win.putalpha(ImageChops.multiply(win.getchannel("A"), m))
    # Drop shadow on a neutral desk.
    pad = 40 * k
    out = Image.new("RGBA", (W + 2 * pad, H + 2 * pad), (120, 124, 130, 255))
    sh = Image.new("L", out.size, 0)
    ImageDraw.Draw(sh).rounded_rectangle([pad, pad + 10 * k, pad + W, pad + H + 10 * k], radius=10 * k, fill=255)
    out.alpha_composite(tint(out.width, out.height, INK, sh.filter(ImageFilter.GaussianBlur(18 * k)), 0.45))
    out.alpha_composite(win, (pad, pad))
    return out.convert("RGB")


def wizard_mock(root, direction, page="welcome"):
    """An Inno Setup 6.6 'modern dark' wizard at 100% DPI (WizardSizePercent 120) with the art."""
    dark = direction != "paper"
    k = 2
    w, h = 596, 432 + 30
    W, H = w * k, h * k
    bgc = (32, 32, 32) if dark else (255, 255, 255)
    fgc = (240, 240, 240) if dark else (20, 20, 20)
    sub = (170, 170, 170) if dark else (90, 90, 90)
    img = Image.new("RGB", (W, H), bgc)
    d = ImageDraw.Draw(img)
    d.rectangle([0, 0, W, 30 * k], fill=(43, 43, 43) if dark else (243, 243, 243))
    tf = art.font(root, "Inter-Regular.ttf", 12 * k)
    ico = appicon.render(32).resize((16 * k, 16 * k), Image.LANCZOS)
    img.paste(ico, (8 * k, 7 * k), ico)
    d.text((30 * k, 8 * k), "Setup - Gridline 0.7.0-beta.1", font=tf, fill=fgc)
    foot = 48
    d.rectangle([0, (h - foot) * k, W, H], fill=(43, 43, 43) if dark else (240, 240, 240))
    bf = art.font(root, "Inter-Regular.ttf", 12 * k)
    for i, lab in enumerate(["Cancel", "Next", "Back"] if page != "finish" else ["Finish"]):
        bx1 = W - (12 + i * 88) * k
        bx0 = bx1 - 80 * k
        prim = lab in ("Next", "Finish")
        d.rounded_rectangle([bx0, (h - 36) * k, bx1, (h - 12) * k], radius=4 * k,
                            fill=(255, 255, 255) if prim and dark else ((20, 20, 20) if prim else ((58, 58, 58) if dark else (252, 252, 252))),
                            outline=None if prim else ((80, 80, 80) if dark else (200, 200, 200)))
        lw = d.textlength(lab, font=bf)
        d.text(((bx0 + bx1) / 2 - lw / 2, (h - 31) * k), lab, font=bf,
               fill=((10, 10, 10) if dark else (255, 255, 255)) if prim else fgc)
    hf = art.font(root, "Inter-SemiBold.ttf", 17 * k)
    bodyf = art.font(root, "Inter-Regular.ttf", 12 * k)
    if page in ("welcome", "finish"):
        aw, ah = WIZARD_AREA[200]
        im = wizard_image(root, direction, (aw, ah))
        th_ = (h - 30 - foot) * k
        im = im.resize((round(th_ * aw / ah), th_), Image.LANCZOS)
        img.paste(im, (0, 30 * k))
        tx = im.width + 24 * k
        if page == "welcome":
            head = "Welcome to Gridline"
            body = ["Setup installs Gridline 0.7.0-beta.1, the free,", "open source music workstation.", "",
                    "Patterns, piano roll, playlist and mixer, with", "built-in instruments and effects.", "",
                    "Close other programs before you continue."]
        else:
            head = "Gridline is ready"
            body = ["Gridline is installed on this computer.", "Open it from the Start menu or the desktop.", "",
                    "Make something good."]
        d.text((tx, 52 * k), head, font=hf, fill=fgc)
        for i, line in enumerate(body):
            d.text((tx, (92 + i * 18) * k), line, font=bodyf, fill=sub)
        if page == "finish":
            yy = (92 + len(body) * 18 + 16) * k
            for j, opt in enumerate(["Open Gridline now", "View the README"]):
                d.rounded_rectangle([tx, yy + j * 26 * k, tx + 14 * k, yy + j * 26 * k + 14 * k], radius=3 * k,
                                    fill=(255, 255, 255) if dark else (20, 20, 20))
                d.text((tx + 22 * k, yy + j * 26 * k - 1 * k), opt, font=bodyf, fill=fgc)
    else:
        hh = 58
        d.rectangle([0, 30 * k, W, (30 + hh) * k], fill=(25, 25, 25) if dark else (255, 255, 255))
        d.line([(0, (30 + hh) * k), (W, (30 + hh) * k)], fill=(55, 55, 55) if dark else (220, 220, 220), width=k)
        d.text((22 * k, 40 * k), "Choose what to install", font=art.font(root, "Inter-SemiBold.ttf", 13 * k), fill=fgc)
        d.text((36 * k, 60 * k), "Extras for Gridline", font=bodyf, fill=sub)
        sm = small_image(SMALL_AREA[200]).resize((48 * k, 48 * k), Image.LANCZOS)
        img.paste(sm, (W - 58 * k, 35 * k), sm)
        opts = ["Create a desktop shortcut", "Install the plugins for other hosts (VST3, CLAP)",
                "Open Gridline projects (.gridline.json) with Gridline", "Offer Gridline for FL Studio projects (.flp)"]
        for j, opt in enumerate(opts):
            yy = (112 + j * 30) * k
            d.rounded_rectangle([30 * k, yy, 44 * k, yy + 14 * k], radius=3 * k,
                                outline=(150, 150, 150) if dark else (120, 120, 120), width=k)
            d.text((54 * k, yy - 1 * k), opt, font=bodyf, fill=fgc)
    return img


def sheet(root, direction, out_dir):
    """One preview sheet per direction plus the individual mock-ups."""
    os.makedirs(out_dir, exist_ok=True)
    p = lambda n: os.path.join(out_dir, f"{direction}-{n}.png")  # noqa: E731
    dmg_mock(root, direction, 1).save(p("dmg-1x"))
    dmg_mock(root, direction, 2).save(p("dmg-2x"))
    dmg_mock(root, direction, 2, ["Plugins", "gridline", "README.txt", "Gridline Suite"]).save(p("dmg-2x-4extras"))
    for page in ("welcome", "tasks", "finish"):
        wizard_mock(root, direction, page).save(p(f"wizard-{page}"))
    # All wizard image scales side by side at 1:1.
    ims = [wizard_image(root, direction, WIZARD_AREA[s]) for s in WIZARD_SCALES]
    sw = sum(i.width for i in ims) + 20 * (len(ims) + 1)
    sh = max(i.height for i in ims) + 60
    s = Image.new("RGB", (sw, sh), (90, 90, 96))
    d = ImageDraw.Draw(s)
    f = art.font(root, "JetBrainsMono-Regular.ttf", 14)
    x = 20
    for sc, im in zip(WIZARD_SCALES, ims):
        s.paste(im, (x, 40))
        d.text((x, 12), f"{sc}% {im.width}x{im.height}", font=f, fill=(255, 255, 255))
        x += im.width + 20
    s.save(p("wizard-scales"))
    # Small images and setup icon on light and dark.
    smalls = [small_image(SMALL_AREA[sc]) for sc in WIZARD_SCALES]
    icons = [setup_icon_image(n) for n in ICO_SIZES]
    sw = max(sum(i.width for i in smalls), sum(i.width for i in icons)) + 30 * 12
    s = Image.new("RGBA", (sw, 2 * (170 + 280)), (0, 0, 0, 255))
    d = ImageDraw.Draw(s)
    for r, bgc in enumerate([(240, 240, 240), (32, 32, 32)]):
        y0 = r * 450
        d.rectangle([0, y0, sw, y0 + 450], fill=bgc + (255,))
        x = 20
        for im in smalls:
            s.alpha_composite(im, (x, y0 + 20 + 160 - im.height))
            x += im.width + 24
        x = 20
        for im in icons:
            s.alpha_composite(im, (x, y0 + 200 + 256 - im.height - 10))
            x += im.width + 24
    s.convert("RGB").save(p("small-and-icons"))


# ---------------------------------------------------------------- main

def outputs(root, direction):
    """{relative path: bytes} of every committed file."""
    files = {}
    L = dmg_layout(direction)

    def png(im):
        b = io.BytesIO()
        if im.mode == "RGBA" and im.getchannel("A").getextrema() == (255, 255):
            im = im.convert("RGB")
        im.save(b, "PNG", optimize=True)
        return b.getvalue()
    files["tools/release/dmg/background.png"] = png(dmg_background(root, direction, 1))
    files["tools/release/dmg/background@2x.png"] = png(dmg_background(root, direction, 2))
    lay = dict(L)
    lay["direction"] = direction
    files["tools/release/dmg/layout.json"] = (json.dumps(lay, indent=2) + "\n").encode()
    for sc in WIZARD_SCALES:
        files[f"tools/windows/art/wizard-{sc}.png"] = png(wizard_image(root, direction, WIZARD_AREA[sc]))
        n = SMALL_AREA[sc]
        files[f"tools/windows/art/wizard-small-{sc}.png"] = png(small_image(n))
    files["tools/windows/art/setup.ico"] = ico_bytes([setup_icon_image(n) for n in ICO_SIZES])
    return files


def main():
    args = sys.argv[1:]
    opts = {}
    for key in ("--direction", "--preview"):
        if key in args:
            i = args.index(key)
            opts[key] = args[i + 1]
            del args[i:i + 2]
    check = "--check" in args
    if check:
        args.remove("--check")
    root = os.path.abspath(args[0] if args else os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
    direction = opts.get("--direction", DEFAULT)
    if direction not in DIRECTIONS:
        raise SystemExit(f"unknown direction {direction} (one of {', '.join(DIRECTIONS)})")
    files = outputs(root, direction)
    stale = []
    for rel, data in files.items():
        path = os.path.join(root, rel)
        old = open(path, "rb").read() if os.path.exists(path) else None
        if old == data:
            continue
        if check:
            stale.append(rel)
            continue
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(data)
        print("wrote", rel)
    if "--preview" in opts:
        for dname in DIRECTIONS:
            sheet(root, dname, opts["--preview"])
        print("previews in", opts["--preview"])
    if stale:
        print("out of date (run tools/make_installer_art.py):", *stale, sep="\n  ")
        sys.exit(1)


if __name__ == "__main__":
    main()
