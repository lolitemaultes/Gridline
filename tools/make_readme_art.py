#!/usr/bin/env python3
"""Generate the README artwork (docs/images/) from Gridline's own brand assets.

  banner.png      header banner: the Tile mark lockup and tagline in Paper on a
                  surface-950 card with a faint construction grid, and on the
                  right a step plane (sequencer cells, waveform, level meters)
                  drawn as vectors and laid back in perspective. Brand
                  elements only, no screenshots.
  release-<version>.png
                  the release banner in the same family: the version, a
                  headline and a subline.
  release-<version>/
                  release screenshots (--shots): playlist, channel rack,
                  stretch modes and a sheet of effect editors.
  workspace.png, piano-roll.png, chord-editor.png, audio-editor.png,
  automation.png, plugin-picker.png
                  crops of screenshots taken with gridline-app (see --shots and
                  tools/readme_shots.sh).
  editors.png     a contact sheet of built-in plugin editors (--shots).

Requires Pillow. Fonts come from assets/fonts (Inter, JetBrains Mono).

Usage: python3 tools/make_readme_art.py [--shots DIR] [gridline root]
"""
import math
import os
import random
import sys

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

# Brand (logo guide v1.0): Ink / Paper, surfaces 950/900/800, Tile mark construction.
INK = (0, 0, 0)
PAPER = (255, 255, 255)
S950 = (11, 11, 11)
S900 = (20, 20, 20)
S800 = (31, 31, 31)
FAINT = (138, 138, 138)
DIM = (107, 107, 107)
U, R, BAR, INSET = 120.0, 28.0, 10.0, 24.0
CENTRES = (40.0, 80.0)


def draw_mark(img, x, y, size, fg, bg):
    """The Tile mark: rounded tile with the # cut out (true holes)."""
    s = size / U
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([x, y, x + size, y + size], radius=R * s, fill=fg)
    a, b = INSET * s, (U - INSET) * s
    for c in CENTRES:
        c *= s
        half = BAR * s / 2
        d.rectangle([x + c - half, y + a, x + c + half, y + b], fill=bg)  # vertical bar
        d.rectangle([x + a, y + c - half, x + b, y + c + half], fill=bg)  # horizontal bar


def mark_mask(size, k=4):
    """Anti-aliased L mask of the mark (holes transparent)."""
    big = Image.new("L", (size * k, size * k), 0)
    draw_mark(big, 0, 0, size * k - 1, 255, 0)
    return big.resize((size, size), Image.LANCZOS)


def font(root, name, px):
    return ImageFont.truetype(os.path.join(root, "assets", "fonts", name), int(px))


def tracked(d, xy, text, f, fill, tracking):
    """Text with extra letter spacing."""
    x, y = xy
    for ch in text:
        d.text((x, y), ch, font=f, fill=fill)
        x += d.textlength(ch, font=f) + tracking
    return x


def text_width(d, text, f, tracking=0.0):
    return sum(d.textlength(ch, font=f) + tracking for ch in text) - tracking


def _solve(a, b):
    """Gaussian elimination for the 8 x 8 perspective system."""
    n = len(b)
    m = [row[:] + [b[i]] for i, row in enumerate(a)]
    for c in range(n):
        p = max(range(c, n), key=lambda r: abs(m[r][c]))
        m[c], m[p] = m[p], m[c]
        for r in range(n):
            if r != c:
                f = m[r][c] / m[c][c]
                m[r] = [x - f * y for x, y in zip(m[r], m[c])]
    return [m[i][n] / m[i][i] for i in range(n)]


def perspective_coeffs(dst, src):
    """Coefficients mapping output points dst[i] to input points src[i] (PIL PERSPECTIVE)."""
    a, b = [], []
    for (x, y), (u, v) in zip(dst, src):
        a.append([x, y, 1, 0, 0, 0, -u * x, -u * y])
        b.append(u)
        a.append([0, 0, 0, x, y, 1, -v * x, -v * y])
        b.append(v)
    return _solve(a, b)


def radial_mask(W, H, cx, cy, rx, ry, inner=255, outer=0, power=1.6):
    small = Image.new("L", (160, max(2, round(160 * H / W))))
    sw, sh = small.size
    for y in range(sh):
        for x in range(sw):
            dx = ((x + 0.5) / sw * W - cx) / rx
            dy = ((y + 0.5) / sh * H - cy) / ry
            t = min(1.0, math.hypot(dx, dy))
            small.putpixel((x, y), round(inner + (outer - inner) * (t ** power)))
    return small.resize((W, H), Image.BICUBIC)


def hfade(W, H, x0, x1, a0=0, a1=255):
    """Horizontal ramp from a0 at x0 to a1 at x1 (smoothstep)."""
    row = Image.new("L", (W, 1))
    for x in range(W):
        t = min(1.0, max(0.0, (x - x0) / max(1, x1 - x0)))
        t = t * t * (3 - 2 * t)
        row.putpixel((x, 0), round(a0 + (a1 - a0) * t))
    return row.resize((W, H))


def vfade(W, H, y0, y1, a0=0, a1=255):
    col = Image.new("L", (1, H))
    for y in range(H):
        t = min(1.0, max(0.0, (y - y0) / max(1, y1 - y0)))
        t = t * t * (3 - 2 * t)
        col.putpixel((0, y), round(a0 + (a1 - a0) * t))
    return col.resize((W, H))


# The rhythm on the step plane: one bar of 16 steps per row (1 = lit, 2 = accent).
STEPS = [
    "2000100020001000",
    "0000200000002000",
    "1010101110101011",
    "0000000100000100",
    "2001000020010010",
    "0000001000000010",
]


def _plane(PW, PH, k):
    """The flat step plane: construction grid, rows of rounded step cells in the
    Tile mark's corner ratio, a waveform and level meters, all as vectors.
    Returns (cells RGBA, glow L) at PW x PH."""
    cells = Image.new("RGBA", (PW, PH), (0, 0, 0, 0))
    glow = Image.new("L", (PW, PH), 0)
    d = ImageDraw.Draw(cells)
    g = ImageDraw.Draw(glow)
    cols, rows = len(STEPS[0]), len(STEPS)
    pitch = PW / (cols + 6)  # room on the right for the meters
    size = pitch * 0.84
    rad = size * R / U
    top = PH * 0.08
    # Construction grid under the cells: hairlines on every cell edge, brighter each bar.
    for c in range(cols + 7):
        x = c * pitch - (pitch - size) / 2
        d.line([(x, 0), (x, PH)], fill=(255, 255, 255, 60 if c % 4 == 0 else 22), width=max(1, k))
    for r in range(rows + 6):
        y = top + r * pitch - (pitch - size) / 2
        d.line([(0, y), (PW, y)], fill=(255, 255, 255, 22), width=max(1, k))
    for r, row in enumerate(STEPS):
        for c, v in enumerate(row):
            x, y = c * pitch, top + r * pitch
            box = [x, y, x + size, y + size]
            if v == "0":
                shade = 34 if (c // 4) % 2 == 0 else 26
                d.rounded_rectangle(box, radius=rad, fill=(shade + 6, shade + 6, shade + 6, 255),
                                    outline=(255, 255, 255, 26), width=max(1, k))
            else:
                a = 255 if v == "2" else 200
                d.rounded_rectangle(box, radius=rad, fill=PAPER + (a,))
                g.rounded_rectangle(box, radius=rad, fill=255 if v == "2" else 150)
    # Waveform across the plane below the steps: a decaying, beat-shaped signal.
    wy = top + rows * pitch + pitch * 1.6
    amp = pitch * 1.9
    pts_hi, pts_lo = [], []
    n = 1400
    span = cols * pitch - (pitch - size)
    for i in range(n + 1):
        t = i / n
        beat = (t * 4) % 1.0
        env = math.exp(-beat * 3.2) * 0.8 + 0.2 * (0.6 + 0.4 * math.sin(t * 2 * math.pi * 2.0))
        s = (abs(math.sin(t * 2 * math.pi * 13)) * 0.55 + abs(math.sin(t * 2 * math.pi * 29 + 1.3)) * 0.3 +
             abs(math.sin(t * 2 * math.pi * 11 + 0.4)) * 0.25)
        a = min(1.0, s * env)
        x = t * span
        pts_hi.append((x, wy - a * amp))
        pts_lo.append((x, wy + a * amp * 0.92))
    d.polygon(pts_hi + pts_lo[::-1], fill=(255, 255, 255, 46))
    d.line(pts_hi, fill=(255, 255, 255, 210), width=max(2, 2 * k), joint="curve")
    d.line(pts_lo, fill=(255, 255, 255, 120), width=max(1, k), joint="curve")
    d.line([(0, wy), (span, wy)], fill=(255, 255, 255, 50), width=max(1, k))
    g.line(pts_hi, fill=110, width=4 * k)
    # Level meters: segmented columns at the end of the rows.
    mx0 = (cols + 0.9) * pitch
    seg = pitch * 0.5
    levels = [0.66, 0.92, 0.78, 0.58]
    mbot = wy + amp
    for j, lv in enumerate(levels):
        x = mx0 + j * pitch * 0.82
        wseg = pitch * 0.58
        nseg = int((mbot - top) / seg)
        lit = int(nseg * lv)
        for s in range(nseg):
            y1 = mbot - s * seg
            y0 = y1 - seg * 0.68
            on = s < lit
            hot = on and s >= nseg * 0.82
            fill = PAPER + (240 if hot else 170,) if on else (255, 255, 255, 18)
            d.rounded_rectangle([x, y0, x + wseg, y1], radius=seg * 0.18, fill=fill)
            if on:
                g.rectangle([x, y0, x + wseg, y1], fill=200 if hot else 70)
    return cells, glow


def _scene(W, H, k, quad, fade_x0, fade_x1):
    """The abstract motif (step plane in perspective) composited onto a brand card."""
    img = Image.new("RGBA", (W, H), S950 + (255,))
    # Depth: lifted light towards the lockup and a cooler pool under the plane.
    img.alpha_composite(_tint(W, H, (255, 255, 255), radial_mask(W, H, W * 0.24, H * 0.40, W * 0.50, H * 1.1, 38, 0, 1.4), 0.30))
    img.alpha_composite(_tint(W, H, (255, 255, 255), radial_mask(W, H, W * 0.78, H * 0.55, W * 0.36, H * 0.9, 30, 0, 1.8), 0.35))

    # Construction grid across the card (10u cells), stronger near the lockup.
    cell = 20 * k
    grid = Image.new("L", (W, H), 0)
    gd = ImageDraw.Draw(grid)
    ox = 96 * k % cell
    for x in range(ox, W, cell):
        gd.line([(x, 0), (x, H)], fill=255 if (x - ox) // cell % 4 == 0 else 110, width=1)
    for y in range(H // 2 % cell, H, cell):
        gd.line([(0, y), (W, y)], fill=255 if (y - H // 2) // cell % 4 == 0 else 110, width=1)
    grid = ImageChops.multiply(grid, radial_mask(W, H, W * 0.24, H * 0.5, W * 0.45, H * 0.95, 255, 0, 1.2))
    img.alpha_composite(_tint(W, H, (255, 255, 255), grid, 0.06))

    # The step plane, laid back in perspective, rendered large then mapped.
    PW, PH = 1700 * k, 1060 * k
    cells, glow = _plane(PW, PH, k)
    coeffs = perspective_coeffs(quad, [(0, 0), (PW, 0), (PW, PH), (0, PH)])
    plane = cells.transform((W, H), Image.PERSPECTIVE, coeffs, Image.BICUBIC)
    gl = glow.transform((W, H), Image.PERSPECTIVE, coeffs, Image.BICUBIC)
    fade = ImageChops.multiply(hfade(W, H, fade_x0, fade_x1, 0, 255), vfade(W, H, H * 0.70, H * 1.02, 255, 70))
    # Soft shadow of the plane, then the glow of lit cells, then the plane.
    sh = plane.getchannel("A").filter(ImageFilter.GaussianBlur(26 * k))
    img.alpha_composite(_tint(W, H, (0, 0, 0), ImageChops.multiply(sh, fade), 0.55))
    bloom = ImageChops.multiply(gl.filter(ImageFilter.GaussianBlur(18 * k)), fade)
    img.alpha_composite(_tint(W, H, (255, 255, 255), bloom, 0.28))
    plane.putalpha(ImageChops.multiply(plane.getchannel("A"), fade))
    img.alpha_composite(plane)
    tight = ImageChops.multiply(gl.filter(ImageFilter.GaussianBlur(4 * k)), fade)
    img.alpha_composite(_tint(W, H, (255, 255, 255), tight, 0.16))
    # Vignette keeps the edges calm.
    vig = radial_mask(W, H, W * 0.5, H * 0.5, W * 0.62, H * 0.9, 0, 255, 2.2)
    img.alpha_composite(_tint(W, H, (0, 0, 0), vig, 0.40))
    return img


def _finish(img, W, H, k, card_r):
    """Grain, rounded card corners and a hairline edge (reads on light and dark pages)."""
    rnd = random.Random(7)
    noise = Image.new("L", (W // 2, H // 2))
    noise.putdata([rnd.randrange(0, 255) for _ in range((W // 2) * (H // 2))])
    noise = noise.resize((W, H), Image.NEAREST)
    img.alpha_composite(_tint(W, H, (255, 255, 255), noise, 0.018))
    cm = Image.new("L", (W, H), 0)
    ImageDraw.Draw(cm).rounded_rectangle([0, 0, W - 1, H - 1], radius=card_r, fill=255)
    edge = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    ImageDraw.Draw(edge).rounded_rectangle([0, 0, W - 1, H - 1], radius=card_r, outline=(255, 255, 255, 34), width=k)
    img.alpha_composite(edge)
    img.putalpha(ImageChops.multiply(img.getchannel("A"), cm))
    return img


def _lockup(img, root, lx, ly, mh, halo=0.10):
    """The Tile mark and the wordmark (logo guide: gap 0.2 x mark, cap height to mark)."""
    W, H = img.size
    k = 2
    h = Image.new("L", (W, H), 0)
    ImageDraw.Draw(h).rounded_rectangle([lx, ly, lx + mh, ly + mh], radius=mh * R / U, fill=255)
    img.alpha_composite(_tint(W, H, (255, 255, 255), h.filter(ImageFilter.GaussianBlur(40 * k)), halo))
    img.alpha_composite(_tint(mh, mh, PAPER, mark_mask(mh), 1.0), (lx, ly))
    d = ImageDraw.Draw(img)
    word = font(root, "Inter-Bold.ttf", mh * 0.86)
    bb = d.textbbox((0, 0), "gridline", font=word)
    tx = lx + mh + mh * 0.2
    ty = ly + (mh - (bb[3] - bb[1])) / 2 - bb[1]
    _tracked_word(img, (tx, ty), "gridline", word, PAPER, -0.025 * mh * 0.86)
    return tx + text_width(d, "gridline", word, -0.025 * mh * 0.86)


def banner(root, out, w=1280, h=380):
    """README header: lockup and tagline on the left, the step plane (cells,
    waveform, meters) laid back in perspective on the right. Brand elements only."""
    k = 2  # rendered at 2x
    W, H = w * k, h * k
    quad = [(W * 0.53, H * 0.10), (W * 1.12, H * -0.10), (W * 1.04, H * 1.20), (W * 0.47, H * 0.86)]
    img = _scene(W, H, k, quad, W * 0.47, W * 0.66)
    d = ImageDraw.Draw(img)
    mh = 112 * k
    lx = 88 * k
    ly = round(H * 0.5 - mh * 0.5 - 30 * k)
    _lockup(img, root, lx, ly, mh)
    sub = font(root, "Inter-Regular.ttf", 23 * k)
    sy = ly + mh + 34 * k
    d.text((lx + 2 * k, sy), "The free, open source music workstation", font=sub, fill=(158, 158, 158))
    mono = font(root, "JetBrainsMono-Regular.ttf", 13 * k)
    tracked(d, (lx + 3 * k, sy + 44 * k), "PATTERNS   PIANO ROLL   PLAYLIST   MIXER", mono, (96, 96, 96), 2.2 * k)
    save_png(_finish(img, W, H, k, 28 * k), out)


def release_banner(root, out, version, headline, subline, w=1280, h=380):
    """Release header in the same family: small lockup, the version large, a headline."""
    k = 2
    W, H = w * k, h * k
    quad = [(W * 0.57, H * 0.12), (W * 1.13, H * -0.08), (W * 1.05, H * 1.20), (W * 0.51, H * 0.88)]
    img = _scene(W, H, k, quad, W * 0.52, W * 0.70)
    d = ImageDraw.Draw(img)
    lx = 88 * k
    mh = 40 * k
    ly = 62 * k
    _lockup(img, root, lx, ly, mh, halo=0.06)
    mono = font(root, "JetBrainsMono-Regular.ttf", 13 * k)
    tracked(d, (lx + 2 * k, ly + mh + 30 * k), "RELEASE", mono, (110, 110, 110), 3.0 * k)
    big = font(root, "Inter-Bold.ttf", 132 * k)
    vb = d.textbbox((0, 0), version, font=big)
    vy = ly + mh + 52 * k - vb[1]
    _tracked_word(img, (lx - 4 * k, vy), version, big, PAPER, -0.03 * 132 * k)
    hy = vy + vb[3] + 18 * k
    head = font(root, "Inter-SemiBold.ttf", 30 * k)
    d.text((lx, hy), headline, font=head, fill=(235, 235, 235))
    sub = font(root, "Inter-Regular.ttf", 19 * k)
    d.text((lx + 1 * k, hy + 44 * k), subline, font=sub, fill=(150, 150, 150))
    save_png(_finish(img, W, H, k, 28 * k), out)


def save_png(im, path, limit=600_000):
    """Optimised PNG; neutral images as grey (+ alpha), and a 256-colour palette
    when the file would still be over `limit` bytes (README pages load fast)."""
    if im.mode in ("RGB", "RGBA"):
        r, g, b = im.split()[:3]
        if not ImageChops.difference(r, g).getbbox() and not ImageChops.difference(g, b).getbbox():
            im = im.convert("LA" if im.mode == "RGBA" else "L")
    im.save(path, optimize=True)
    if os.path.getsize(path) > limit and im.mode == "RGB":
        im.quantize(256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.FLOYDSTEINBERG).save(path, optimize=True)


def _tint(W, H, rgb, mask, opacity):
    layer = Image.new("RGBA", (W, H), rgb + (0,))
    layer.putalpha(mask.point(lambda v: round(v * opacity)))
    return layer


def _tracked_word(img, xy, text, f, fill, tracking):
    d = ImageDraw.Draw(img)
    x, y = xy
    for ch in text:
        d.text((x, y), ch, font=f, fill=fill)
        x += d.textlength(ch, font=f) + tracking


def contact_sheet(root, shots, names, out, cols=3, cell_w=600, label_h=34, margin=16):
    """Plugin editors on surface-950 tiles with a mono caption, 2x resolution."""
    imgs = []
    for file, caption, box in names:
        p = os.path.join(shots, file)
        if os.path.exists(p):
            im = Image.open(p).convert("RGB")
            imgs.append((im.crop(box) if box else im, caption))
    if not imgs:
        return
    k = 2
    cw = cell_w * k
    scaled = []
    for im, cap in imgs:
        s = cw / im.width
        scaled.append((im.resize((cw, int(im.height * s)), Image.LANCZOS), cap))
    rows = (len(scaled) + cols - 1) // cols
    m = margin * k
    row_h = [max(im.height for im, _ in scaled[r * cols:(r + 1) * cols]) for r in range(rows)]
    W = cols * cw + (cols + 1) * m
    H = sum(row_h) + rows * label_h * k + (rows + 1) * m
    sheet = Image.new("RGB", (W, H), S950)
    d = ImageDraw.Draw(sheet)
    mono = font(root, "JetBrainsMono-Regular.ttf", 13 * k)
    y = m
    for r in range(rows):
        for c, (im, cap) in enumerate(scaled[r * cols:(r + 1) * cols]):
            x = m + c * (cw + m)
            d.rounded_rectangle([x - 1, y - 1, x + cw, y + row_h[r]], radius=6 * k, fill=S900)
            sheet.paste(im, (x, y + (row_h[r] - im.height) // 2))
            tracked(d, (x + 2 * k, y + row_h[r] + 10 * k), cap.upper(), mono, FAINT, 1.5 * k)
        y += row_h[r] + label_h * k + m
    if sheet.width > 1760:  # 2x of GitHub's README column
        sheet = sheet.resize((1760, round(sheet.height * 1760 / sheet.width)), Image.LANCZOS)
    save_png(sheet, out)


def main():
    args = sys.argv[1:]
    shots = None
    if "--shots" in args:
        i = args.index("--shots")
        shots = args[i + 1]
        del args[i:i + 2]
    root = args[0] if args else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
    out = os.path.join(root, "docs", "images")
    os.makedirs(out, exist_ok=True)
    if shots:
        # Editor windows cropped out of 2880x1800 (UI scale 2) captures.
        contact_sheet(root, shots, EDITORS, os.path.join(out, "editors.png"), cols=3)
        # Full views from 3200x2000 captures, saved at 1600 px wide.
        for src, dst, box in VIEWS:
            p = os.path.join(shots, src)
            if not os.path.exists(p):
                continue
            im = Image.open(p).convert("RGB")
            if box:
                im = im.crop(box)
            if im.width > 1600:
                im = im.resize((1600, round(im.height * 1600 / im.width)), Image.LANCZOS)
            save_png(im, os.path.join(out, dst))
        # Release screenshots (docs/images/release-<version>/).
        rel = os.path.join(out, "release-" + RELEASE)
        os.makedirs(rel, exist_ok=True)
        contact_sheet(root, shots, RELEASE_EDITORS, os.path.join(rel, "effects.png"), cols=1, cell_w=800)
        for src, dst, box in RELEASE_VIEWS:
            p = os.path.join(shots, src)
            if not os.path.exists(p):
                continue
            im = Image.open(p).convert("RGB").crop(box)
            if im.width > 1600:
                im = im.resize((1600, round(im.height * 1600 / im.width)), Image.LANCZOS)
            save_png(im, os.path.join(rel, dst))
    banner(root, os.path.join(out, "banner.png"))
    release_banner(root, os.path.join(out, "release-" + RELEASE + ".png"), RELEASE, "Every knob, checked",
                   "Tideline, new plugin names, sharper editors and Linux display fixes")


# Screenshot crops (tools/readme_shots.sh takes the captures).
# Plugin editors: instruments from 3200x2200 captures (the plugin part of the
# channel window), effects from 2880x1800 captures (the window below its title).
EDITORS = [
    ("ed_matrixfm.png", "Lattice", (400, 509, 2765, 1869)),
    ("ed_wavetable.png", "Isoline", (461, 509, 2704, 1912)),
    ("ed_harmor.png", "Halftone", (480, 509, 2688, 1997)),
    ("ed_eq.png", "Parametric EQ", (507, 288, 2373, 1267)),
    ("ed_hall.png", "Cloister", (550, 302, 2333, 1250)),
    ("ed_amprack.png", "Pedal Rack", (412, 343, 2468, 1207)),
]
# Views from 3200x2000 captures (1600x1000 window at UI scale 2), saved 1600 px wide.
VIEWS = [
    ("main.png", "workspace.png", None),
    ("pr.png", "piano-roll.png", (490, 104, 2440, 1940)),
    ("edison.png", "audio-editor.png", (467, 104, 2728, 1712)),
    ("chords.png", "chord-editor.png", (592, 525, 2595, 1520)),
    ("autoedit.png", "automation.png", (978, 488, 2720, 1635)),
    ("picker.png", "plugin-picker.png", None),
]

# The release banner and screenshots of this version.
RELEASE = "0.6.1"
# Effect editors with live graphics while the song plays (2880x1800 captures).
RELEASE_EDITORS = [
    ("rel_reverb.png", "Reverb", (530, 449, 2354, 1109)),
    ("rel_delay.png", "Tracer", (508, 452, 2373, 1106)),
    ("rel_limiter.png", "Limiter", (629, 340, 2254, 1214)),
]
# Views from 3200x2000 captures.
RELEASE_VIEWS = [
    ("rel_playlist.png", "playlist.png", (352, 230, 2290, 1835)),
    ("rel_rack.png", "channel-rack.png", (680, 397, 2080, 1488)),
    ("rel_stretch.png", "stretch-modes.png", (0, 130, 1560, 700)),
]


if __name__ == "__main__":
    main()
