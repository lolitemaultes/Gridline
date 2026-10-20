#!/usr/bin/env python3
"""Generate the Gridline application icon from the Tile mark construction.

The icon is the Tile mark itself, built as an object: the tile becomes the
macOS app-icon body (824 of 1024 units, continuous-corner squircle, soft
shadow), made of dark graphite, and the # is cut into it as recessed
channels with a bright Paper floor. Strictly monochrome (logo guide v1.0).

Mark construction (logo guide section 05, 120u grid): four 10u bars centred
at 40u and 80u, inset 24u from every edge.

Small sizes (16 to 48 px) are drawn with pixel-snapped geometry so the #
stays crisp; large sizes get the material (gradients, rim light, inner
shadows).

Outputs (under assets/icons/):
  gridline-app-{16,24,32,48,64,96,128,256,512,1024}.png   app icon (Linux, docs)
  gridline-app-256.bmp                                     window icon (SDL_LoadBMP)
  Gridline.icns                                            macOS bundle icon

Options:
  --iconset DIR     also write a macOS .iconset folder (icon_16x16.png ...)
  --preview FILE    write a preview sheet of every size on light and dark

Requires Pillow. Usage: python3 tools/make_app_icon.py [options] [gridline root]
"""
import io
import math
import os
import struct
import sys

from PIL import Image, ImageChops, ImageDraw, ImageFilter

U = 120.0
R = 28.0
BAR = 10.0
CENTRES = (40.0, 80.0)
INSET = 24.0

SIZES = (16, 24, 32, 48, 64, 96, 128, 256, 512, 1024)
ICNS = (  # (type, pixel size)
    (b"icp4", 16), (b"icp5", 32), (b"icp6", 48), (b"ic07", 128), (b"ic08", 256),
    (b"ic09", 512), (b"ic10", 1024), (b"ic11", 32), (b"ic12", 64), (b"ic13", 256), (b"ic14", 512),
)


# ---------------------------------------------------------------- geometry

def tile_points(x0, y0, size, steps=96):
    """The tile outline: the mark's own 28u corner radius, with each corner
    drawn as a superellipse quadrant (continuous curvature, like the macOS
    icon shape) over a slightly longer run so the optical radius matches."""
    r = size * R / U * 1.28  # longer run, same optical radius
    n = 2.9
    pts = []
    corners = ((x0 + size - r, y0 + r, 0), (x0 + size - r, y0 + size - r, 1),
               (x0 + r, y0 + size - r, 2), (x0 + r, y0 + r, 3))
    for cx, cy, q in corners:
        for i in range(steps + 1):
            t = (q - 1) * math.pi / 2 + (math.pi / 2) * i / steps
            c, s = math.cos(t), math.sin(t)
            pts.append((cx + r * math.copysign(abs(c) ** (2 / n), c), cy + r * math.copysign(abs(s) ** (2 / n), s)))
    return pts


def hash_rects(x0, y0, size, snap=False):
    """The # as rectangles (x0, y0, x1, y1) in pixels, relative to the tile."""
    s = size / U

    def p(v):
        return float(round(v)) if snap else v
    a, b = p(x0 + INSET * s), p(y0 + INSET * s)
    a2, b2 = p(x0 + (U - INSET) * s), p(y0 + (U - INSET) * s)
    rects = []
    for c in CENTRES:
        lo = p(x0 + (c - BAR / 2) * s)
        hi = max(lo + 1, p(x0 + (c + BAR / 2) * s))
        rects.append((lo, b, hi, b2))  # vertical bar
        lo = p(y0 + (c - BAR / 2) * s)
        hi = max(lo + 1, p(y0 + (c + BAR / 2) * s))
        rects.append((a, lo, a2, hi))  # horizontal bar
    return rects


# ---------------------------------------------------------------- helpers

def mask_poly(W, pts, k):
    m = Image.new("L", (W * k, W * k), 0)
    ImageDraw.Draw(m).polygon([(x * k, y * k) for x, y in pts], fill=255)
    return m.resize((W, W), Image.LANCZOS) if k > 1 else m


def mask_rects(W, rects, k):
    m = Image.new("L", (W * k, W * k), 0)
    d = ImageDraw.Draw(m)
    for x0, y0, x1, y1 in rects:
        d.rectangle([x0 * k, y0 * k, x1 * k - 1, y1 * k - 1], fill=255)
    return m.resize((W, W), Image.LANCZOS) if k > 1 else m


def vgradient(W, top, bottom, y0=0, y1=None):
    """RGB vertical gradient from y0 (top colour) to y1 (bottom colour)."""
    y1 = W if y1 is None else y1
    col = Image.new("RGB", (1, W))
    for y in range(W):
        t = min(1.0, max(0.0, (y - y0) / max(1.0, y1 - y0)))
        t = t * t * (3 - 2 * t)
        col.putpixel((0, y), tuple(round(top[i] + (bottom[i] - top[i]) * t) for i in range(3)))
    return col.resize((W, W))


def radial(W, cx, cy, r, inner, outer=0):
    """L-mode radial falloff (inner at the centre, outer at radius r)."""
    small = 128
    g = Image.new("L", (small, small))
    for y in range(small):
        for x in range(small):
            dx = (x + 0.5) / small * W - cx
            dy = (y + 0.5) / small * W - cy
            t = min(1.0, math.hypot(dx, dy) / r)
            t = 1 - (1 - t) ** 2
            g.putpixel((x, y), round(inner + (outer - inner) * t))
    return g.resize((W, W), Image.BICUBIC)


def shift(m, dx, dy):
    return ImageChops.offset(m, dx, dy) if (dx or dy) else m


def layer(base, rgb, alpha):
    """Composite a flat colour (or RGB image) through an L mask onto base (RGBA)."""
    W = base.width
    src = Image.new("RGB", (W, W), rgb) if isinstance(rgb, tuple) else rgb
    top = src.convert("RGBA")
    top.putalpha(alpha)
    base.alpha_composite(top)


def mul(m, f):
    return m.point(lambda v: round(v * f))


# ---------------------------------------------------------------- the icon

GRAPHITE_TOP = (52, 52, 54)
GRAPHITE_BOTTOM = (14, 14, 15)
FLOOR_TOP = (255, 255, 255)
FLOOR_BOTTOM = (214, 214, 214)


def render_large(W):
    """Full material icon at W px (W >= 64), Big Sur grid: 824/1024 body."""
    k = 2 if W >= 256 else 4
    u = W / 1024.0
    bx = 100 * u
    bs = 824 * u
    img = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    body = mask_poly(W, tile_points(bx, bx, bs), k)

    # Shadows: a wide soft drop shadow and a tight contact shadow.
    sh = shift(body, 0, round(12 * u)).filter(ImageFilter.GaussianBlur(26 * u))
    layer(img, (0, 0, 0), mul(sh, 0.42))
    sh2 = shift(body, 0, max(1, round(3 * u))).filter(ImageFilter.GaussianBlur(max(0.6, 4 * u)))
    layer(img, (0, 0, 0), mul(sh2, 0.30))

    # Body: graphite with a top-left sheen.
    layer(img, vgradient(W, GRAPHITE_TOP, GRAPHITE_BOTTOM, bx, bx + bs), body)
    sheen = ImageChops.multiply(radial(W, bx + bs * 0.30, bx + bs * 0.05, bs * 0.95, 40, 0), body)
    layer(img, (255, 255, 255), sheen)

    # Fine engraved grid texture (very faint): 12 x 12 cells on the tile.
    tex = Image.new("L", (W, W), 0)
    if W >= 256:
        td = ImageDraw.Draw(tex)
        for i in range(1, 12):
            p = bx + bs * i / 12
            td.line([(p, bx), (p, bx + bs)], fill=255, width=max(1, round(1.2 * u)))
            td.line([(bx, p), (bx + bs, p)], fill=255, width=max(1, round(1.2 * u)))
        fade = radial(W, bx + bs * 0.5, bx + bs * 0.5, bs * 0.75, 255, 60)
        tex = ImageChops.multiply(ImageChops.multiply(tex, body), fade)
        layer(img, (255, 255, 255), mul(tex, 0.045))

    # The # channels.
    rects = hash_rects(bx, bx, bs)
    groove = ImageChops.multiply(mask_rects(W, rects, k), body)
    # Lip: a slightly lighter edge around the cut (chamfer catching light).
    lip = ImageChops.subtract(groove.filter(ImageFilter.MaxFilter(_odd(5 * u))), groove)
    lip_top = ImageChops.subtract(lip, shift(lip, 0, max(1, round(3 * u))))
    layer(img, (255, 255, 255), mul(lip, 0.06))
    layer(img, (255, 255, 255), mul(lip_top, 0.10))
    # Floor: bright Paper, slightly cooler at the bottom.
    layer(img, vgradient(W, FLOOR_TOP, FLOOR_BOTTOM, bx + bs * 0.2, bx + bs * 0.8), groove)
    # Inner shadow: the graphite walls shade the top and left of each channel.
    walls = ImageChops.invert(groove)
    inner = shift(walls, 0, round(8 * u)).filter(ImageFilter.GaussianBlur(5 * u))
    inner = ImageChops.multiply(inner, groove)
    layer(img, (40, 40, 44), mul(inner, 0.30))
    crisp = shift(walls, 0, max(1, round(2.5 * u))).filter(ImageFilter.GaussianBlur(max(0.4, 0.8 * u)))
    layer(img, (30, 30, 32), mul(ImageChops.multiply(crisp, groove), 0.45))

    # Rim light along the top edge of the body, dark rim at the bottom.
    rim_w = max(1, round(3 * u))
    top_rim = ImageChops.subtract(body, shift(body, 0, rim_w))
    layer(img, (255, 255, 255), ImageChops.multiply(top_rim, vgradient(W, (120,) * 3, (0,) * 3, bx, bx + bs * 0.6).convert("L")))
    edge = ImageChops.subtract(body, body.filter(ImageFilter.MinFilter(_odd(2.5 * u))))
    layer(img, (255, 255, 255), mul(edge, 0.10))
    bot_rim = ImageChops.subtract(body, shift(body, 0, -rim_w))
    layer(img, (0, 0, 0), mul(bot_rim, 0.5))
    return img


def _odd(v):
    n = max(3, int(round(v)))
    return n if n % 2 else n + 1


# Small sizes: (span start, bar 1 start, bar 1 end, bar 2 start, bar 2 end, span end)
# in pixels from the tile edge, keeping the 11 : 10 : 30 : 10 : 11 rhythm of the
# construction as closely as whole pixels allow.
HINTED = {
    16: (3, 4, 6, 8, 10, 11),   # tile 14 px
    24: (4, 6, 8, 12, 14, 16),  # tile 20 px
    32: (6, 8, 11, 17, 20, 22),  # tile 28 px
    48: (8, 12, 16, 26, 30, 34),  # tile 42 px
}


def render_small(W):
    """Pixel-snapped icon for 16..48 px: same body and #, hinted, flatter material."""
    k = 8
    m = {16: 1, 24: 2, 32: 2, 48: 3}.get(W, round(W * 0.0625))
    bs = W - 2 * m
    img = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    body = mask_poly(W, tile_points(m, m, bs), k)
    if W >= 24:
        sh = shift(body, 0, 1).filter(ImageFilter.GaussianBlur(W / 48))
        layer(img, (0, 0, 0), mul(sh, 0.35))
    layer(img, vgradient(W, (60, 60, 62), (16, 16, 17), m, m + bs), body)
    if W in HINTED:  # hand-hinted pixel columns of the #, relative to the tile
        lo, b0, b1, b2, b3, hi = HINTED[W]
        rects = [(m + b0, m + lo, m + b1, m + hi), (m + b2, m + lo, m + b3, m + hi),
                 (m + lo, m + b0, m + hi, m + b1), (m + lo, m + b2, m + hi, m + b3)]
    else:
        rects = hash_rects(m, m, bs, snap=True)
    groove = ImageChops.multiply(mask_rects(W, rects, 1), body)
    layer(img, vgradient(W, (255, 255, 255), (226, 226, 226), m, m + bs), groove)
    top_rim = ImageChops.subtract(body, shift(body, 0, 1))
    layer(img, (255, 255, 255), mul(top_rim, 0.12))
    return img


def render(W):
    if W >= 64:
        return render_large(W)
    return render_small(W)


# ---------------------------------------------------------------- writers

def png_bytes(im):
    b = io.BytesIO()
    im.save(b, "PNG", optimize=True)
    return b.getvalue()


def write_icns(path, images):
    chunks = b""
    for t, s in ICNS:
        data = png_bytes(images[s])
        chunks += t + struct.pack(">I", len(data) + 8) + data
    with open(path, "wb") as f:
        f.write(b"icns" + struct.pack(">I", len(chunks) + 8) + chunks)


def write_bmp(path, im):
    """32-bit BGRA BMP with a V5 header (alpha honoured by SDL_LoadBMP)."""
    W, H = im.size
    px = im.convert("RGBA").tobytes()
    rows = []
    for y in range(H - 1, -1, -1):  # bottom-up
        row = bytearray()
        for x in range(W):
            r, g, b, a = px[(y * W + x) * 4:(y * W + x) * 4 + 4]
            row += bytes((b, g, r, a))
        rows.append(bytes(row))
    data = b"".join(rows)
    hdr = struct.pack("<IiiHHIIiiII", 124, W, H, 1, 32, 3, len(data), 2835, 2835, 0, 0)
    hdr += struct.pack("<IIII", 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000)
    hdr += b"sRGB" + b"\0" * 36 + struct.pack("<III", 0, 0, 0)  # endpoints, gamma
    hdr += struct.pack("<IIII", 4, 0, 0, 0)  # intent, profile data/size, reserved
    off = 14 + len(hdr)
    with open(path, "wb") as f:
        f.write(b"BM" + struct.pack("<IHHI", off + len(data), 0, 0, off) + hdr + data)


def preview(path, images):
    """Every size at 1:1 on light and dark, plus the 1024 at half size."""
    sizes = [s for s in SIZES if s <= 256]
    row_w = sum(sizes) + 40 * (len(sizes) + 1)
    big = 512
    W = row_w + big + 40
    H = 2 * (256 + 80)
    sheet = Image.new("RGBA", (W, H), (0, 0, 0, 255))
    d = ImageDraw.Draw(sheet)
    for r, bg in enumerate([(236, 236, 238), (30, 30, 32)]):
        y0 = r * (H // 2)
        d.rectangle([0, y0, W, y0 + H // 2], fill=bg + (255,))
        x = 40
        for s in sizes:
            sheet.alpha_composite(images[s], (x, y0 + 40 + (256 - s)))
            x += s + 40
    # 1024 at half size spanning both rows' height, on a split background.
    sheet.alpha_composite(images[1024].resize((big, big), Image.LANCZOS), (row_w, (H - big) // 2))
    sheet.convert("RGB").save(path)


def main():
    args = sys.argv[1:]
    opts = {}
    for key in ("--iconset", "--preview"):
        if key in args:
            i = args.index(key)
            opts[key] = args[i + 1]
            del args[i:i + 2]
    root = args[0] if args else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
    out = os.path.join(root, "assets", "icons")
    os.makedirs(out, exist_ok=True)
    images = {s: render(s) for s in SIZES}
    for s in SIZES:
        images[s].save(os.path.join(out, f"gridline-app-{s}.png"), optimize=True)
    write_bmp(os.path.join(out, "gridline-app-256.bmp"), images[256])
    write_icns(os.path.join(out, "Gridline.icns"), images)
    if "--iconset" in opts:
        d = opts["--iconset"]
        os.makedirs(d, exist_ok=True)
        for s in (16, 32, 128, 256, 512):
            images[s].save(os.path.join(d, f"icon_{s}x{s}.png"))
            images[s * 2].save(os.path.join(d, f"icon_{s}x{s}@2x.png"))
    if "--preview" in opts:
        preview(opts["--preview"], images)
    print("app icon written to", out)


if __name__ == "__main__":
    main()
