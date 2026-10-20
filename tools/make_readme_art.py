#!/usr/bin/env python3
"""Generate the README artwork (docs/images/) from Gridline's own brand assets.

  banner.png      header banner: the Tile mark lockup in Paper on a surface-950
                  card with a faint construction grid, and a tilted, toned-down
                  view of the workspace (docs/images/workspace.png) on the right.
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

from PIL import Image, ImageChops, ImageDraw, ImageEnhance, ImageFilter, ImageFont

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


def _panel(img, path, L, T, Wd, depth, rise, shadow, fade=(), color=0.18):
    """Paste a screenshot as a window in perspective (left edge L,T, width Wd,
    right edge depth x the left height, raised by rise); returns the quad."""
    W, H = img.size
    k = 2
    ws = Image.open(path).convert("RGB")
    ws = ImageEnhance.Color(ws).enhance(color)
    sw, sh = ws.size
    win = ws.convert("RGBA")
    r = round(sw * 0.012)
    wm = Image.new("L", (sw, sh), 0)
    ImageDraw.Draw(wm).rounded_rectangle([0, 0, sw - 1, sh - 1], radius=r, fill=255)
    win.putalpha(wm)
    edge = Image.new("RGBA", (sw, sh), (0, 0, 0, 0))
    ImageDraw.Draw(edge).rounded_rectangle([0, 0, sw - 1, sh - 1], radius=r, outline=(255, 255, 255, 80),
                                           width=max(2, sw // 600))
    win.alpha_composite(edge)
    Hd = Wd * sh / sw
    rh = Hd * depth
    quad = [(L, T), (L + Wd, T + (Hd - rh) * 0.5 - rise), (L + Wd, T + (Hd + rh) * 0.5 - rise), (L, T + Hd)]
    coeffs = perspective_coeffs(quad, [(0, 0), (sw, 0), (sw, sh), (0, sh)])
    tilted = win.transform((W, H), Image.PERSPECTIVE, coeffs, Image.BICUBIC)
    sm = ImageChops.offset(tilted.getchannel("A"), -12 * k, 22 * k).filter(ImageFilter.GaussianBlur(24 * k))
    img.alpha_composite(_tint(W, H, (0, 0, 0), sm, shadow))
    a = tilted.getchannel("A")
    for f in fade:
        a = ImageChops.multiply(a, f)
    tilted.putalpha(a)
    img.alpha_composite(tilted)
    return quad


def banner(root, out, w=1280, h=380, shot=None, card=None):
    """Brand header: lockup left, the product tilted into the card on the right."""
    k = 2  # rendered at 2x
    W, H = w * k, h * k
    card_r = 28 * k
    img = Image.new("RGBA", (W, H), S950 + (255,))

    # Depth: a slightly lifted centre-left, darker corners.
    lift = radial_mask(W, H, W * 0.30, H * 0.42, W * 0.55, H * 1.1, 34, 0, 1.4)
    img.alpha_composite(_tint(W, H, (255, 255, 255), lift, 0.30))

    # Construction grid: the 120u tile grid continued across the card (10u = 1 cell),
    # strongest around the lockup, gone towards the edges.
    cell = 20 * k
    grid = Image.new("L", (W, H), 0)
    gd = ImageDraw.Draw(grid)
    ox = 96 * k % cell
    for i, x in enumerate(range(ox, W, cell)):
        gd.line([(x, 0), (x, H)], fill=255 if (x - ox) // cell % 4 == 0 else 120, width=1)
    for y in range(H // 2 % cell, H, cell):
        gd.line([(0, y), (W, y)], fill=255 if (y - H // 2) // cell % 4 == 0 else 120, width=1)
    gmask = radial_mask(W, H, W * 0.26, H * 0.5, W * 0.42, H * 0.95, 255, 0, 1.2)
    grid = ImageChops.multiply(grid, gmask)
    img.alpha_composite(_tint(W, H, (255, 255, 255), grid, 0.075))

    # The product: workspace screenshot, toned to the brand palette, tilted away.
    if shot and os.path.exists(shot):
        # Workspace window: rotated about the vertical axis, receding to the right,
        # dissolving into the card at the left and bottom.
        L, T, Wd = W * 0.535, H * 0.14, W * 0.64
        quad = _panel(img, shot, L, T, Wd, 0.72, H * 0.035, shadow=0.55,
                      fade=(hfade(W, H, L - 10 * k, L + 200 * k, 40, 255), vfade(W, H, H * 0.58, H * 0.99, 255, 0)))
        # A soft rim of light on the top edge of the window.
        glow = Image.new("L", (W, H), 0)
        ImageDraw.Draw(glow).line([quad[0], quad[1]], fill=255, width=3 * k)
        glow = glow.filter(ImageFilter.GaussianBlur(14 * k))
        img.alpha_composite(_tint(W, H, (255, 255, 255), glow, 0.20))
        # In front: an editor card (automation curves), same rotation, nearer.
        if card and os.path.exists(card):
            cl, ct, cw = W * 0.585, H * 0.47, W * 0.20
            _panel(img, card, cl, ct, cw, 0.88, H * 0.01, shadow=0.9, color=0.55,
                   fade=(vfade(W, H, H * 0.84, H * 1.0, 255, 60),))

    # Lockup (logo guide proportions: gap 0.2 x mark, word cap height to mark).
    d = ImageDraw.Draw(img)
    mh = 112 * k
    lx = 88 * k
    ly = round(H * 0.5 - mh * 0.5 - 22 * k)
    # Soft halo behind the mark lifts it off the card.
    halo = Image.new("L", (W, H), 0)
    ImageDraw.Draw(halo).rounded_rectangle([lx, ly, lx + mh, ly + mh], radius=mh * R / U, fill=255)
    halo = halo.filter(ImageFilter.GaussianBlur(40 * k))
    img.alpha_composite(_tint(W, H, (255, 255, 255), halo, 0.10))
    mm = mark_mask(mh)
    img.alpha_composite(_tint(mh, mh, PAPER, mm, 1.0), (lx, ly))
    word = font(root, "Inter-Bold.ttf", mh * 0.86)
    bb = d.textbbox((0, 0), "gridline", font=word)
    tx = lx + mh + mh * 0.2
    ty = ly + (mh - (bb[3] - bb[1])) / 2 - bb[1]
    _tracked_word(img, (tx, ty), "gridline", word, PAPER, -0.025 * mh * 0.86)

    sub = font(root, "Inter-Regular.ttf", 23 * k)
    sy = ly + mh + 34 * k
    d.text((lx + 2 * k, sy), "The free, open source music workstation", font=sub, fill=(150, 150, 150))
    mono = font(root, "JetBrainsMono-Regular.ttf", 13 * k)
    tracked(d, (lx + 3 * k, sy + 44 * k), "PATTERNS   PIANO ROLL   PLAYLIST   MIXER", mono, (92, 92, 92), 2.2 * k)

    # Fine grain so the gradients do not band.
    rnd = random.Random(7)
    noise = Image.new("L", (W // 2, H // 2))
    noise.putdata([rnd.randrange(0, 255) for _ in range((W // 2) * (H // 2))])
    noise = noise.resize((W, H), Image.NEAREST)
    img.alpha_composite(_tint(W, H, (255, 255, 255), noise, 0.018))

    # Card shape: rounded corners and a hairline edge (reads on light and dark pages).
    cm = Image.new("L", (W, H), 0)
    ImageDraw.Draw(cm).rounded_rectangle([0, 0, W - 1, H - 1], radius=card_r, fill=255)
    edge = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    ImageDraw.Draw(edge).rounded_rectangle([0, 0, W - 1, H - 1], radius=card_r, outline=(255, 255, 255, 30), width=k)
    img.alpha_composite(edge)
    img.putalpha(ImageChops.multiply(img.getchannel("A"), cm))
    img.save(out, optimize=True)


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
    sheet.save(out, optimize=True)


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
            im.save(os.path.join(out, dst), optimize=True)
    banner(root, os.path.join(out, "banner.png"), shot=os.path.join(out, "workspace.png"),
           card=os.path.join(out, "automation.png"))


# Screenshot crops (tools/readme_shots.sh takes the captures).
# Plugin editors: instruments from 3200x2200 captures (the plugin part of the
# channel window), effects from 2880x1800 captures (the window below its title).
EDITORS = [
    ("ed_matrixfm.png", "Matrix FM", (400, 509, 2765, 1869)),
    ("ed_wavetable.png", "Wavetable", (461, 509, 2704, 1912)),
    ("ed_harmor.png", "Resynth", (480, 509, 2688, 1997)),
    ("ed_eq.png", "Parametric EQ", (507, 288, 2373, 1267)),
    ("ed_hall.png", "Hall reverb", (550, 302, 2333, 1250)),
    ("ed_amprack.png", "Amp rack", (412, 343, 2468, 1207)),
]
# Views from 3200x2000 captures (1600x1000 window at UI scale 2), saved 1600 px wide.
VIEWS = [
    ("main.png", "workspace.png", None),
    ("pr.png", "piano-roll.png", (491, 104, 2070, 1940)),
    ("edison.png", "audio-editor.png", (467, 104, 2728, 1712)),
    ("chords.png", "chord-editor.png", (123, 123, 2141, 1117)),
    ("autoedit.png", "automation.png", (978, 488, 2720, 1635)),
    ("picker.png", "plugin-picker.png", None),
]


if __name__ == "__main__":
    main()
