#!/usr/bin/env python3
"""Build build/config/font_pl.oft - the menu font for the Polish launcher.

Polish needs l-stroke, a-ogonek, e-ogonek, s-acute, z-acute, z-dot, c-acute and
n-acute (plus capitals). None of them are in Latin-1, and the stock font has no
free glyph slots, so Polish gets a font of its own; package.py drops it into that
build's Res/ and nobody else pays for glyphs they will never draw.

The letters are not drawn from scratch - they are composed from the font's own
glyphs, so the weight and the shape of the accents match the rest of the typeface
exactly. They land on ISO-8859-2 code points, which is what gen_menutext.py
encodes Polish with, so the text is still one byte per letter.

    python build/make_font_pl.py

.oft layout (OSLib, "OSLFont v01"): a 64-byte header, then 256 width bytes, then
256 glyphs of charHeight rows x lineWidth bytes, 1bpp, least significant bit
leftmost. That last detail is easy to get backwards - it renders every asymmetric
glyph mirrored, which is how it was spotted.
"""
import io, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC  = os.path.join(HERE, "..", "_masterboy", "Res", "font.oft")
OUT  = os.path.join(HERE, "config", "font_pl.oft")

HDR, NGLYPH, HEIGHT, LINEW = 64, 256, 13, 3

#(target code point, base letter, how to mark it). ISO-8859-2 positions.
#o-acute and O-acute are already in Latin-1 at the same code points, so Polish
#needs nothing done to them.
PLAN = [
    (0xE6, "c", "acute"),  (0xF1, "n", "acute"),  (0xB6, "s", "acute"),
    (0xBC, "z", "acute"),  (0xBF, "z", "dot"),    (0xB1, "a", "ogonek"),
    (0xEA, "e", "ogonek"), (0xB3, "l", "stroke"),
    (0xC6, "C", "Acute"),  (0xD1, "N", "Acute"),  (0xA6, "S", "Acute"),
    (0xAC, "Z", "Acute"),  (0xAF, "Z", "Dot"),    (0xA1, "A", "ogonek"),
    (0xCA, "E", "ogonek"), (0xA3, "L", "stroke"),
]


def main():
    src = open(SRC, "rb").read()
    widths = bytearray(src[HDR:HDR + NGLYPH])
    data = bytearray(src[HDR + NGLYPH:])

    def get(cp):
        off = cp * HEIGHT * LINEW
        return [[(data[off + r * LINEW + (c >> 3)] >> (c & 7)) & 1
                 for c in range(LINEW * 8)] for r in range(HEIGHT)]

    def put(cp, g, w):
        off = cp * HEIGHT * LINEW
        for r in range(HEIGHT):
            for b in range(LINEW):
                data[off + r * LINEW + b] = 0
            for c in range(LINEW * 8):
                if g[r][c]:
                    data[off + r * LINEW + (c >> 3)] |= 1 << (c & 7)
        widths[cp] = w

    def centre(g, r0, r1):
        cols = [c for r in range(r0, r1 + 1) for c in range(LINEW * 8) if g[r][c]]
        return (min(cols) + max(cols)) // 2 if cols else 3

    def acute(g, top):
        #Two pixels leaning right, the same stroke the font uses on a-acute
        cx = centre(g, 5, 10)
        g[top][cx + 1] = 1
        g[top + 1][cx] = 1

    def dot(g, row):
        g[row][centre(g, 5, 10)] = 1

    def ogonek(g):
        #Hooks down and right from under the bowl, like the font's own cedilla
        cols = [c for r in (9, 10) for c in range(LINEW * 8) if g[r][c]]
        cx = (max(cols) - 1) if cols else 4
        g[11][cx] = 1
        g[12][cx] = 1
        g[12][cx + 1] = 1

    def stroke(g):
        #The stem already fills the middle, so two pixels complete the diagonal
        g[5][2] = 1
        g[7][0] = 1

    marks = {"acute": lambda g: acute(g, 2), "Acute": lambda g: acute(g, 0),
             "dot": lambda g: dot(g, 3),     "Dot":   lambda g: dot(g, 1),
             "ogonek": ogonek,               "stroke": stroke}

    for cp, base, kind in PLAN:
        b = ord(base)
        if not widths[b]:
            sys.exit("base glyph %r is empty - wrong source font?" % base)
        g = get(b)
        marks[kind](g)
        put(cp, g, widths[b])

    io.open(OUT, "wb").write(bytes(src[:HDR]) + bytes(widths) + bytes(data))
    sys.stderr.write("%s: %d glyphs added\n" % (os.path.basename(OUT), len(PLAN)))


main()
