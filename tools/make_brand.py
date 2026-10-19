#!/usr/bin/env python3
"""Generate Gridline brand resources from the Tile mark construction
(logo guide v1.0, section 05):

  canvas 120 x 120u, corner radius 28u, four 10u cut-out bars centred at
  40u and 80u, inset 24u from every edge, solid 30 x 30u centre cell,
  true holes (evenodd), strictly monochrome Ink #000000 / Paper #FFFFFF.

Outputs (under assets/):
  brand/gridline-mark-{ink,paper}.svg
  brand/gridline-lockup-{ink,paper}.svg
  icons/gridline-{16,20,24,32,48,64,96,128,256,512}.png        Ink mark
  icons/gridline-paper-{16,...,512}.png                        Paper mark
  linux/gridline.desktop

Usage: python3 tools/make_brand.py [gridline root]
"""
import os
import struct
import sys
import zlib

U = 120.0
R = 28.0
BAR = 10.0
CENTRES = (40.0, 80.0)
INSET = 24.0
INK = (0, 0, 0)
PAPER = (255, 255, 255)
SIZES = (16, 20, 24, 32, 48, 64, 96, 128, 256, 512)


def mark_path(x=0.0, y=0.0, s=1.0):
    """SVG path: rounded tile + four bar rectangles (evenodd → holes)."""
    def f(v):
        return f"{v:.3f}".rstrip('0').rstrip('.')
    w = U * s
    r = R * s
    p = [f"M{f(x + r)} {f(y)}H{f(x + w - r)}A{f(r)} {f(r)} 0 0 1 {f(x + w)} {f(y + r)}"
         f"V{f(y + w - r)}A{f(r)} {f(r)} 0 0 1 {f(x + w - r)} {f(y + w)}"
         f"H{f(x + r)}A{f(r)} {f(r)} 0 0 1 {f(x)} {f(y + w - r)}V{f(y + r)}A{f(r)} {f(r)} 0 0 1 {f(x + r)} {f(y)}Z"]
    # The # is the union of two vertical and two horizontal bars. With evenodd
    # the crossings would flip back to filled, so emit the union outline as
    # separate non-overlapping pieces: full vertical bars + horizontal bar
    # segments between/outside them.
    a, b = INSET, U - INSET
    vx = [c - BAR / 2 for c in CENTRES]
    for x0 in vx:  # vertical bars
        p.append(_rect(x + x0 * s, y + a * s, BAR * s, (b - a) * s, f))
    for c in CENTRES:  # horizontal bars minus the vertical bar overlaps
        y0 = c - BAR / 2
        segs = [(a, vx[0]), (vx[0] + BAR, vx[1]), (vx[1] + BAR, b)]
        for s0, s1 in segs:
            p.append(_rect(x + s0 * s, y + y0 * s, (s1 - s0) * s, BAR * s, f))
    return ''.join(p)


def _rect(x, y, w, h, f):
    return f"M{f(x)} {f(y)}h{f(w)}v{f(h)}h{f(-w)}Z"


def svg_mark(color):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 120 120" width="120" height="120">'
            f'<title>Gridline</title><path fill="{color}" fill-rule="evenodd" d="{mark_path()}"/></svg>\n')


def svg_lockup(color):
    # Mark 120u tall, 24u gap (clear space), wordmark set in Inter Bold.
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 520 120" width="520" height="120">'
            f'<title>Gridline</title><path fill="{color}" fill-rule="evenodd" d="{mark_path()}"/>'
            f'<text x="148" y="92" fill="{color}" font-family="Inter, ui-sans-serif, system-ui, sans-serif" '
            f'font-weight="700" font-size="104" letter-spacing="-3">gridline</text></svg>\n')


def inside_mark(px, py):
    """True if the point (in u) is on the tile and not in a cut-out."""
    if px < 0 or py < 0 or px > U or py > U:
        return False
    # rounded corners
    cx = min(max(px, R), U - R)
    cy = min(max(py, R), U - R)
    if (px - cx) ** 2 + (py - cy) ** 2 > R * R:
        return False
    a, b = INSET, U - INSET
    for c in CENTRES:
        if c - BAR / 2 <= px <= c + BAR / 2 and a <= py <= b:
            return False
        if c - BAR / 2 <= py <= c + BAR / 2 and a <= px <= b:
            return False
    return True


def png(size, rgb):
    ss = 4 if size >= 64 else 8  # supersampling
    rows = []
    for y in range(size):
        row = bytearray([0])
        for x in range(size):
            hit = 0
            for sy in range(ss):
                for sx in range(ss):
                    u = (x + (sx + 0.5) / ss) * U / size
                    v = (y + (sy + 0.5) / ss) * U / size
                    hit += inside_mark(u, v)
            a = round(255 * hit / (ss * ss))
            row += bytes((*rgb, a))
        rows.append(bytes(row))
    raw = zlib.compress(b''.join(rows), 9)

    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 6, 0, 0, 0)) +
            chunk(b'IDAT', raw) + chunk(b'IEND', b''))


DESKTOP = """[Desktop Entry]
Type=Application
Name=Gridline
GenericName=Digital Audio Workstation
Comment=Pattern-based music production
Exec=gridline-app %F
Icon=gridline
Terminal=false
Categories=AudioVideo;Audio;Midi;Sequencer;Music;
MimeType=application/x-gridline-project;
Keywords=DAW;music;sequencer;beat;piano roll;
"""


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), '..')
    a = os.path.join(root, 'assets')
    for d in ('brand', 'icons', 'linux'):
        os.makedirs(os.path.join(a, d), exist_ok=True)
    for name, col in (('ink', '#000000'), ('paper', '#FFFFFF')):
        open(os.path.join(a, 'brand', f'gridline-mark-{name}.svg'), 'w').write(svg_mark(col))
        open(os.path.join(a, 'brand', f'gridline-lockup-{name}.svg'), 'w').write(svg_lockup(col))
    for s in SIZES:
        open(os.path.join(a, 'icons', f'gridline-{s}.png'), 'wb').write(png(s, INK))
        open(os.path.join(a, 'icons', f'gridline-paper-{s}.png'), 'wb').write(png(s, PAPER))
    open(os.path.join(a, 'linux', 'gridline.desktop'), 'w').write(DESKTOP)
    print('brand resources written to', a)


if __name__ == '__main__':
    main()
