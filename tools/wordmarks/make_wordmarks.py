#!/usr/bin/env python3
"""Generate the Gridline plugin wordmarks (J-130, "Tile" direction).

Every built-in plugin gets a text logo in the Gridline lockup grammar: a rounded tile in the
plugin's family colour with the plugin's own pictogram cut out of it (motifs.py), then the
name in lower case, Inter Bold, in the logo guide's lockup proportions (mark 120 : type 104,
gap 28, tracking -3). The two suites get the real Tile mark with "gridline suite" and
"gridline suite mini".

Output: assets/wordmarks/<id>.svg for type gl.<id>, plus _suite.svg and _suite-mini.svg.
Each SVG is 24 units tall (logical px at 100 % UI scale). Shape ids carry the colour role the
app tints them with (src/app/icons/Wordmark.h): mark* (family colour), cut* (cut out of the
mark: transparent), word* (text), dim* (secondary text). The <title> is the display name.

The glyph outlines come from the bundled Inter Bold (SIL OFL 1.1, assets/fonts); the outlines
are drawn into the logos, the font software is not redistributed. Motifs are Gridline
originals.

Requires fontTools (pip install fonttools) and a built `gridline` CLI for the catalogue names.
The SVGs are committed; the build only embeds them (cmake/EmbedWordmarks.cmake).

Usage: python3 tools/wordmarks/make_wordmarks.py [--gridline build-cmake/gridline]
                                                 [--extra-names file.json] [--sheet out.svg]
"""
import argparse
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
sys.dont_write_bytecode = True  # keep tools/wordmarks free of __pycache__
from motifs import M as MOTIFS  # noqa: E402
from plugins import FAMILIES, P, MINI, MINI_FAMILIES  # noqa: E402

try:
    from fontTools.ttLib import TTFont
    from fontTools.pens.svgPathPen import SVGPathPen
    from fontTools.pens.transformPen import TransformPen
except ImportError:
    sys.exit('make_wordmarks.py needs fontTools: pip install fonttools')

TEXT, DIM, HOT, SURFACE = '#D5DBDF', '#A2ABB0', '#FFB547', '#353D42'  # default colours (dark theme) for viewing
H = 24.0
TILE = 20.0
FS = TILE * 104 / 120          # type size: lockup proportions
TRACK = -FS * 3 / 104
GAP = TILE * 28 / 120
BASE = 2 + TILE * 92 / 120     # baseline as in the lockup
CUT_W = 2.15                   # motif stroke, 24-unit grid


def num(v):
    s = ('%.2f' % v).rstrip('0').rstrip('.')
    return '0' if s in ('-0', '') else s


class Font:
    def __init__(self, path):
        self.t = TTFont(path)
        self.upm = self.t['head'].unitsPerEm
        self.gs = self.t.getGlyphSet()
        self.cmap = self.t.getBestCmap()
        self.hmtx = self.t['hmtx']
        self.pairs, self.classes = {}, []
        for lk in self.t['GPOS'].table.LookupList.Lookup:
            if lk.LookupType not in (2, 9):
                continue
            for st in lk.SubTable:
                if lk.LookupType == 9:
                    st = st.ExtSubTable
                if getattr(st, 'LookupType', 2) != 2:
                    continue
                if st.Format == 1:
                    for i, g1 in enumerate(st.Coverage.glyphs):
                        for pvr in st.PairSet[i].PairValueRecord:
                            v = pvr.Value1
                            if v is not None and getattr(v, 'XAdvance', 0):
                                self.pairs.setdefault((g1, pvr.SecondGlyph), v.XAdvance)
                else:
                    self.classes.append(st)

    def glyph(self, ch):
        g = self.cmap.get(ord(ch))
        if g is None:
            sys.exit('no glyph for %r' % ch)
        return g

    def kern(self, a, b):
        g1, g2 = self.glyph(a), self.glyph(b)
        if (g1, g2) in self.pairs:
            return self.pairs[(g1, g2)]
        for st in self.classes:
            if g1 not in st.Coverage.glyphs:
                continue
            rec = st.Class1Record[st.ClassDef1.classDefs.get(g1, 0)].Class2Record[st.ClassDef2.classDefs.get(g2, 0)]
            v = rec.Value1
            if v is not None and getattr(v, 'XAdvance', 0):
                return v.XAdvance
        return 0

    def set(self, text, x, base, size, track):
        """Outlines of `text` from x on baseline `base` (y down). Returns (path data, end x)."""
        s = size / self.upm
        out, prev = [], None
        for i, ch in enumerate(text):
            if prev:
                x += self.kern(prev, ch) * s
            if ch != ' ':
                pen = SVGPathPen(self.gs, ntos=num)
                self.gs[self.glyph(ch)].draw(TransformPen(pen, (s, 0, 0, -s, x, base)))
                out.append(pen.getCommands())
            x += self.hmtx[self.glyph(ch)][0] * s
            if i + 1 < len(text):
                x += track
            prev = ch
        return ' '.join(out), x


_tok = re.compile(r'[MmLlHhVvCcSsQqTtAaZz]|-?(?:\d+\.?\d*|\.\d+)(?:e-?\d+)?')
_nargs = {'M': 2, 'L': 2, 'H': 1, 'V': 1, 'C': 6, 'S': 4, 'Q': 4, 'T': 2, 'A': 7, 'Z': 0}


def place(d, ox, oy, s):
    """Scales path data by s and moves it by (ox, oy); relative commands and arcs included."""
    toks, out, i, cmd = _tok.findall(d), [], 0, None
    while i < len(toks):
        t = toks[i]
        if t.isalpha():
            cmd = t
            out.append(t)
            i += 1
            continue
        U, rel = cmd.upper(), cmd.islower()
        n = _nargs[U]
        v = [float(x) for x in toks[i:i + n]]
        i += n
        dx, dy = (0, 0) if rel else (ox, oy)
        if U == 'A':
            out.append('%s %s %s %d %d %s %s' % (num(v[0] * s), num(v[1] * s), num(v[2]), int(v[3]), int(v[4]),
                                                 num(v[5] * s + dx), num(v[6] * s + dy)))
        elif U == 'H':
            out.append(num(v[0] * s + dx))
        elif U == 'V':
            out.append(num(v[0] * s + dy))
        else:
            out.append(' '.join(num(v[k] * s + (dx if k % 2 == 0 else dy)) for k in range(n)))
    return ' '.join(out)


def rrect(x, y, w, h, r):
    return ('M%s %sH%sA%s %s 0 0 1 %s %sV%sA%s %s 0 0 1 %s %sH%sA%s %s 0 0 1 %s %sV%sA%s %s 0 0 1 %s %sZ' % (
        num(x + r), num(y), num(x + w - r), num(r), num(r), num(x + w), num(y + r), num(y + h - r), num(r), num(r),
        num(x + w - r), num(y + h), num(x + r), num(r), num(r), num(x), num(y + h - r), num(y + r), num(r), num(r),
        num(x + r), num(y)))


# The Tile mark (logo guide section 05, 120-unit grid): tile with the # cut out.
BRAND_MARK = ('M28 0H92A28 28 0 0 1 120 28V92A28 28 0 0 1 92 120H28A28 28 0 0 1 0 92V28A28 28 0 0 1 28 0Z'
              'M35 24h10v72h-10Z M75 24h10v72h-10Z M24 35h11v10h-11Z M45 35h30v10h-30Z M85 35h11v10h-11Z'
              'M24 75h11v10h-11Z M45 75h30v10h-30Z M85 75h11v10h-11Z')


class Svg:
    def __init__(self):
        self.items, self.n = [], 0

    def _id(self, role):
        self.n += 1
        return '%s-%d' % (role, self.n)

    def fill(self, role, d, colour, evenodd=False):
        self.items.append('<path id="%s" d="%s" fill="%s"%s/>' % (self._id(role), d, colour,
                                                                    ' fill-rule="evenodd"' if evenodd else ''))

    def stroke(self, role, d, colour, width):
        self.items.append('<path id="%s" d="%s" fill="none" stroke="%s" stroke-width="%s" stroke-linecap="round" '
                          'stroke-linejoin="round"/>' % (self._id(role), d, colour, num(width)))

    def dot(self, role, x, y, r, colour):
        self.items.append('<circle id="%s" cx="%s" cy="%s" r="%s" fill="%s"/>' % (self._id(role), num(x), num(y), num(r), colour))

    def doc(self, width, title):
        w = num(float(int(width + 0.999)))
        return ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %s %s" width="%s" height="%s"><title>%s</title>\n%s\n</svg>\n'
                % (w, num(H), w, num(H), title, '\n'.join(self.items)))


def tile_wordmark(font, name, colour, motif):
    svg = Svg()
    svg.fill('mark', rrect(0, 2, TILE, TILE, TILE * 28 / 120), colour)
    s = TILE / 24.0
    for it in MOTIFS[motif]:  # the motif is cut out of the tile (drawn in the surface colour for viewers)
        if it[0] == 'c':
            svg.dot('cut', it[1] * s, 2 + it[2] * s, it[3] * s, SURFACE)
        elif it[0] == 's':
            svg.stroke('cut', place(it[1], 0, 2, s), SURFACE, CUT_W * s)
        else:
            svg.fill('cut', place(it[1], 0, 2, s), SURFACE)
    d, x = font.set(name.lower(), TILE + GAP, BASE, FS, TRACK)
    svg.fill('word', d, TEXT)
    return svg.doc(x, name)


def suite_wordmark(font, mini):
    svg = Svg()
    svg.fill('word', place(BRAND_MARK, 0, 2, TILE / 120.0), TEXT, evenodd=True)
    d, x = font.set('gridline', TILE + GAP, BASE, FS, TRACK)
    svg.fill('word', d, TEXT)
    d, x = font.set('suite', x + FS * 0.26, BASE, FS, TRACK)
    svg.fill('dim', d, DIM)
    if mini:
        d, x = font.set('mini', x + FS * 0.26, BASE, FS, TRACK)
        svg.fill('mark', d, HOT)
    return svg.doc(x, 'Gridline Suite Mini' if mini else 'Gridline Suite')


def catalogue(cli):
    out = subprocess.run([cli, 'plugin-params'], check=True, capture_output=True, text=True).stdout
    return {p['type']: p['name'] for p in json.loads(out)}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--gridline', default=os.path.join(ROOT, 'build-cmake', 'gridline'), help='gridline CLI (catalogue names)')
    ap.add_argument('--out', default=os.path.join(ROOT, 'assets', 'wordmarks'))
    ap.add_argument('--sheet', help='also write a contact sheet SVG of every wordmark')
    args = ap.parse_args()

    font = Font(os.path.join(ROOT, 'assets', 'fonts', 'Inter-Bold.ttf'))
    names = catalogue(args.gridline)
    rows = []  # (type, name, colour, motif)
    for t, name in sorted(names.items()):
        if t in P:
            fam, motif, _ = P[t]
            rows.append((t, name, '#%06X' % FAMILIES[fam][1], motif))
        elif t in MINI:
            n, fam, motif, _ = MINI[t]
            rows.append((t, name, '#%06X' % MINI_FAMILIES[fam], motif))
        else:
            sys.exit('%s (%s) has no row in tools/wordmarks/plugins.py' % (t, name))
    for t, (n, fam, motif, _) in sorted(MINI.items()):  # the mini pack may not be in this build yet
        if t not in names:
            rows.append((t, n, '#%06X' % MINI_FAMILIES[fam], motif))
    for t, _, _, motif in rows:
        if motif not in MOTIFS:
            sys.exit('%s: unknown motif %s' % (t, motif))

    os.makedirs(args.out, exist_ok=True)
    keep = set()
    for t, name, colour, motif in rows:
        f = t[3:] + '.svg'
        keep.add(f)
        open(os.path.join(args.out, f), 'w').write(tile_wordmark(font, name, colour, motif))
    for mini in (False, True):
        f = '_suite-mini.svg' if mini else '_suite.svg'
        keep.add(f)
        open(os.path.join(args.out, f), 'w').write(suite_wordmark(font, mini))
    for f in os.listdir(args.out):
        if f.endswith('.svg') and f not in keep:
            os.remove(os.path.join(args.out, f))
    print('%d wordmarks + 2 suite logos in %s' % (len(rows), os.path.relpath(args.out, ROOT)))

    if args.sheet:
        cols, cw, ch = 3, 300, 36
        files = ['_suite.svg', '_suite-mini.svg'] + [t[3:] + '.svg' for t, _, _, _ in rows]
        nrows = (len(files) + cols - 1) // cols
        W, Hh = 24 + cols * cw, 24 + nrows * ch
        parts = ['<rect width="%d" height="%d" fill="#262C30"/>' % (W, Hh)]
        for i, f in enumerate(files):
            x, y = 12 + (i % cols) * cw, 12 + (i // cols) * ch
            body = open(os.path.join(args.out, f)).read()
            body = re.sub(r'<svg[^>]*>|</svg>|<title>.*?</title>', '', body)
            parts.append('<rect x="%d" y="%d" width="%d" height="%d" rx="4" fill="%s"/>' % (x, y, cw - 8, ch - 6, SURFACE))
            parts.append('<g transform="translate(%d %d)">%s</g>' % (x + 8, y + 3, body))
        open(args.sheet, 'w').write('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %d %d" width="%d" height="%d">%s</svg>\n'
                                    % (W, Hh, W, Hh, '\n'.join(parts)))


if __name__ == '__main__':
    main()
