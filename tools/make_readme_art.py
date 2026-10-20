#!/usr/bin/env python3
"""Generate the README artwork (docs/images/) from Gridline's own brand assets.

  banner.png      header banner in the style of the splash screen: the Tile mark
                  lockup in Paper on surface-950, the tagline, the loading bar and
                  a faint step-sequencer grid with a playhead.
  editors.png     a contact sheet of built-in plugin editors, from screenshots
                  taken with gridline-app (see --shots).

Requires Pillow. Fonts come from assets/fonts (Inter, JetBrains Mono).

Usage: python3 tools/make_readme_art.py [--shots DIR] [gridline root]
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

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

SS = 2  # supersampling factor for smooth edges


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


def font(root, name, px):
    return ImageFont.truetype(os.path.join(root, "assets", "fonts", name), int(px))


def tracked(d, xy, text, f, fill, tracking):
    """Text with extra letter spacing (the splash tagline is set wide)."""
    x, y = xy
    for ch in text:
        d.text((x, y), ch, font=f, fill=fill)
        x += d.textlength(ch, font=f) + tracking
    return x


def text_width(d, text, f, tracking=0.0):
    return sum(d.textlength(ch, font=f) + tracking for ch in text) - tracking


def banner(root, out, w=1280, h=380):
    W, H = w * SS, h * SS
    img = Image.new("RGB", (W, H), S950)

    # Step-sequencer grid on the right: 4 rows x 32 steps in groups of four,
    # alternating group shades, a few lit pads, fading out towards the lockup.
    grid = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    g = ImageDraw.Draw(grid)
    cols, rows = 24, 5
    pad = 22 * SS
    gap = 6 * SS
    gx0 = W - 70 * SS - cols * (pad + gap) + gap
    gy0 = (H - (rows * pad + (rows - 1) * gap)) // 2
    pattern = [
        "x...x...x...x...x...x..x",
        "....x.......x.......x...",
        "..x...x...x...x.x.x...x.",
        "x..x..x...x.x...x..x..x.",
        "........x.......x...x...",
    ]
    play = 16  # playhead step
    for r in range(rows):
        for c in range(cols):
            x = gx0 + c * (pad + gap)
            y = gy0 + r * (pad + gap)
            lit = pattern[r][c] == "x"
            shade = (40, 40, 40) if (c // 4) % 2 == 0 else (29, 29, 29)
            col = (232, 232, 232) if lit else shade
            if lit and c == play:
                col = PAPER
            g.rounded_rectangle([x, y, x + pad, y + pad], radius=4 * SS, fill=col + (255,))
    # Playhead: a thin Paper line with a soft glow over the current step.
    px = gx0 + play * (pad + gap) + pad // 2
    glow = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    gd = ImageDraw.Draw(glow)
    gd.rectangle([px - 10 * SS, gy0 - 26 * SS, px + 10 * SS, gy0 + rows * (pad + gap) + 20 * SS], fill=(255, 255, 255, 46))
    glow = glow.filter(ImageFilter.GaussianBlur(18 * SS))
    grid = Image.alpha_composite(grid, glow)
    gl = ImageDraw.Draw(grid)
    gl.rectangle([px - 1 * SS, gy0 - 24 * SS, px + 1 * SS, gy0 + rows * (pad + gap) + 18 * SS], fill=(255, 255, 255, 210))
    gl.polygon([(px - 7 * SS, gy0 - 30 * SS), (px + 7 * SS, gy0 - 30 * SS), (px, gy0 - 22 * SS)], fill=(255, 255, 255, 230))

    # Horizontal fade: the grid dissolves to the left, full strength on the right.
    fade = Image.new("L", (W, H), 0)
    fd = ImageDraw.Draw(fade)
    for x in range(W):
        t = (x - gx0 + 40 * SS) / (W * 0.30)
        t = max(0.0, min(1.0, t))
        e = max(0.0, min(1.0, (W - x) / (60 * SS)))  # soft right edge
        fd.line([(x, 0), (x, H)], fill=int(255 * (t ** 1.4) * 0.9 * e))
    alpha = Image.composite(grid.getchannel("A"), Image.new("L", (W, H), 0), fade)
    grid.putalpha(alpha)
    img = Image.alpha_composite(img.convert("RGBA"), grid)

    # Faint ruler lines across the whole banner (bars), like the playlist grid.
    lines = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    ld = ImageDraw.Draw(lines)
    step = 64 * SS
    for i, x in enumerate(range(0, W, step)):
        a = 26 if i % 4 == 0 else 12
        ld.line([(x, 0), (x, H)], fill=(255, 255, 255, a), width=SS)
    img = Image.alpha_composite(img, lines)

    # Lockup on the left (splash proportions: gap 0.2 h, word 0.86 h).
    d = ImageDraw.Draw(img)
    mh = 92 * SS
    lx = 84 * SS
    ly = H // 2 - mh // 2 - 30 * SS
    draw_mark(img, lx, ly, mh, PAPER, S950)
    word = font(root, "Inter-Bold.ttf", mh * 0.86)
    bb = d.textbbox((0, 0), "gridline", font=word)
    tx = lx + mh + mh * 0.2
    ty = ly + (mh - (bb[3] - bb[1])) / 2 - bb[1]
    d.text((tx, ty), "gridline", font=word, fill=PAPER)

    # Tagline and loading bar, as on the splash.
    mono = font(root, "JetBrainsMono-Regular.ttf", 15 * SS)
    tag = "THE GRID, CUT CLEAN"
    tracked(d, (lx + 4 * SS, ly + mh + 30 * SS), tag, mono, FAINT, 3.2 * SS)
    by = ly + mh + 64 * SS
    bw = 300 * SS
    d.rounded_rectangle([lx + 4 * SS, by, lx + 4 * SS + bw, by + 4 * SS], radius=2 * SS, fill=(38, 38, 38))
    d.rounded_rectangle([lx + 4 * SS, by, lx + 4 * SS + int(bw * 0.68), by + 4 * SS], radius=2 * SS, fill=PAPER)
    sub = font(root, "Inter-Regular.ttf", 16 * SS)
    d.text((lx + 4 * SS, by + 18 * SS), "A free, open source music workstation", font=sub, fill=DIM)

    img = img.convert("RGB").resize((w * 2, h * 2), Image.LANCZOS) if SS != 2 else img.convert("RGB")
    img.save(out, optimize=True)


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
    banner(root, os.path.join(out, "banner.png"))
    if shots:
        # Editor windows cropped out of 2880x1800 (UI scale 2) captures.
        contact_sheet(root, shots, [
            ("ed_matrixfm.png", "Matrix FM", (233, 103, 2643, 1693)),
            ("ed_harmor.png", "Resynth", (296, 99, 2513, 1713)),
            ("ed_hall.png", "Hall reverb", (551, 294, 2298, 1245)),
            ("ed_eq.png", "Parametric EQ", (497, 228, 2380, 1245)),
            ("ed_delay.png", "Delay", (506, 440, 2343, 1106)),
            ("ed_comp.png", "Compressor", (681, 479, 2159, 1084)),
        ], os.path.join(out, "editors.png"), cols=2)
        # Full views from 3200x2000 captures, saved at 1600 px wide.
        for src, dst, box in [
            ("main.png", "workspace.png", None),
            ("pr.png", "piano-roll.png", (480, 100, 2440, 1960)),
            ("edison.png", "audio-editor.png", (472, 100, 2688, 1640)),
            ("chords.png", "chord-editor.png", (150, 120, 2140, 1130)),
            ("autoedit.png", "automation.png", None),
            ("picker.png", "plugin-picker.png", None),
        ]:
            p = os.path.join(shots, src)
            if not os.path.exists(p):
                continue
            im = Image.open(p).convert("RGB")
            if box:
                im = im.crop(box)
            if im.width > 1600:
                im = im.resize((1600, round(im.height * 1600 / im.width)), Image.LANCZOS)
            im.save(os.path.join(out, dst), optimize=True)


if __name__ == "__main__":
    main()
