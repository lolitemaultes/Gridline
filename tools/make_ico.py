#!/usr/bin/env python3
"""Builds assets/icons/Gridline.ico (Windows application icon) from the app icon PNGs that
tools/make_app_icon.py writes (assets/icons/gridline-app-<size>.png).

The .ico holds one PNG-compressed image per size (16 ... 256 px), which Windows Vista and later
read directly. No third-party modules.

    python3 tools/make_ico.py            # writes assets/icons/Gridline.ico
    python3 tools/make_ico.py --check    # exits 1 when the committed file is out of date
"""
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ICONS = os.path.join(ROOT, 'assets', 'icons')
SIZES = [16, 24, 32, 48, 64, 128, 256]


def png_size(data):
    if data[:8] != b'\x89PNG\r\n\x1a\n' or data[12:16] != b'IHDR':
        raise ValueError('not a PNG')
    return struct.unpack('>II', data[16:24])


def build():
    images = []
    for s in SIZES:
        path = os.path.join(ICONS, 'gridline-app-%d.png' % s)
        with open(path, 'rb') as f:
            data = f.read()
        w, h = png_size(data)
        if (w, h) != (s, s):
            raise SystemExit('%s is %dx%d, expected %dx%d' % (path, w, h, s, s))
        images.append((s, data))
    out = struct.pack('<HHH', 0, 1, len(images))  # ICONDIR: reserved, type 1 (icon), count
    offset = 6 + 16 * len(images)
    entries, blobs = b'', b''
    for s, data in images:
        dim = 0 if s >= 256 else s  # 0 means 256
        entries += struct.pack('<BBBBHHII', dim, dim, 0, 0, 1, 32, len(data), offset)
        blobs += data
        offset += len(data)
    return out + entries + blobs


def main():
    data = build()
    dest = os.path.join(ICONS, 'Gridline.ico')
    if '--check' in sys.argv:
        with open(dest, 'rb') as f:
            same = f.read() == data
        print('Gridline.ico is up to date' if same else 'Gridline.ico is out of date: run tools/make_ico.py')
        return 0 if same else 1
    with open(dest, 'wb') as f:
        f.write(data)
    print('wrote', os.path.relpath(dest, ROOT), len(data), 'bytes')
    return 0


if __name__ == '__main__':
    sys.exit(main())
