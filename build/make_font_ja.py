#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Build build/config/font_ja.oft and font_ja_map.json for the Japanese launcher.

Japanese cannot be done the way the other languages are: the menu font is a
256-glyph byte font, and there is no byte encoding that covers kana and kanji.

Rather than write a second text renderer, this builds a *custom code page*. It
reads build/lang/ja.json, collects every non-ASCII character the translation
actually uses, and assigns each one a spare byte (0x80 upward). The glyphs come
from the game's own Japanese fonts, so the menu is set in the same typeface the
game is. gen_menutext.py reads the map and encodes Japanese with it, so the C
tables stay one byte per character and the menu code needs no changes at all.

The ceiling is 128 characters (0x80-0xFF). If ja.json ever exceeds that this
script says so rather than producing a font that silently drops letters.

The game's kana and kanji are 8x8. The Latin font's capitals occupy rows 3-10 -
also 8 rows - so Japanese sits on the same cap height and the two mix cleanly.

    python build/make_font_ja.py
"""
import io, json, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..")
#Where the game project keeps its Japanese font sheets. Only needed when the font
#is rebuilt - build/config/font_ja.oft is committed, so an ordinary build of the
#launcher never looks at this.
GAME = os.environ.get("ROPGB_FONTS",
                      r"C:\Users\newjb\Desktop\rise-of-the-penguins\assets\fonts")
SRC  = os.path.join(ROOT, "_masterboy", "Res", "font.oft")
OUT  = os.path.join(HERE, "config", "font_ja.oft")
MAP  = os.path.join(HERE, "config", "font_ja_map.json")
LANG = os.path.join(HERE, "lang", "ja.json")

HDR, NGLYPH, HEIGHT, LINEW = 64, 256, 13, 3

#The sheets are 8x8 - the same height as this font's Latin capitals - and at that
#size the kana read as far smaller than the Latin beside them, because they are
#condensed and carry much more detail in the same box.
#
#So they are doubled. 2x exactly, never 1.5x: these are 1px strokes, and at 1.5x
#the rows that fall on half pixels come out either doubled or dropped, which
#thickens some strokes and erases others - it closes the gaps in characters that
#depend on them.
#
#16 rows do not fit a 13-row cell, so the Japanese font declares a taller one.
#Latin glyphs are copied in untouched at their original rows, which lands their
#cap centre within a pixel of the kana centre, so a mixed line sits straight.
KANA_PX = 16
OUT_HEIGHT = 16
INK = (0, 0, 0)          # glyphs are black on a light ground
SHEETS = ["japanese-condensed-kana",
          "japanese-condensed-kanji-01", "japanese-condensed-kanji-02",
          "japanese-condensed-kanji-03", "japanese-condensed-kanji-04"]


def load_sheets():
    """{character: 8x8 list of rows of 0/1} from the game's font sheets."""
    from PIL import Image
    glyphs = {}
    for name in SHEETS:
        png = Image.open(os.path.join(GAME, name + ".png")).convert("RGB")
        mapping = json.load(io.open(os.path.join(GAME, name + ".json"),
                                    encoding="utf-8"))["mapping"]
        cols = png.size[0] // 8
        #Two things about these sheets are easy to get wrong, and both of them
        #produce a menu full of sliced-up nonsense rather than an obvious error:
        #the mapping values are character codes offset by 32, not zero-based cell
        #indices; and the ink is black on a light ground, so "anything that is not
        #the magenta surround" fills the whole cell instead of drawing the letter.
        for ch, code in mapping.items():
            if ch in glyphs:
                continue
            idx = code - 32
            gx, gy = (idx % cols) * 8, (idx // cols) * 8
            if idx < 0 or gy + 8 > png.size[1]:
                continue
            rows = []
            for y in range(8):
                rows.append([1 if png.getpixel((gx + x, gy + y)) == INK else 0
                             for x in range(8)])
            glyphs[ch] = rows
    return glyphs


def main():
    text = json.load(io.open(LANG, encoding="utf-8"))
    wanted = sorted({c for s in text.values() for c in s if ord(c) > 0x7F})
    if len(wanted) > 128:
        sys.exit("ja.json uses %d distinct non-ASCII characters; only 128 fit. "
                 "Simplify the wording." % len(wanted))

    if not os.path.isdir(GAME):
        sys.exit("game font sheets not found at %s\n"
                 "Set ROPGB_FONTS to the game project's assets/fonts." % GAME)
    glyphs = load_sheets()
    missing = [c for c in wanted if c not in glyphs]
    if missing:
        sys.exit("not in the game's fonts: " +
                 " ".join("U+%04X" % ord(c) for c in missing))

    src = open(SRC, "rb").read()
    widths = bytearray(src[HDR:HDR + NGLYPH])
    srcdata = src[HDR + NGLYPH:]

    #Re-lay every glyph into the taller cell. The Latin ones keep their original
    #rows, so they sit exactly where they do in every other language's font.
    data = bytearray(NGLYPH * OUT_HEIGHT * LINEW)
    for cp in range(NGLYPH):
        s = cp * HEIGHT * LINEW
        dst = cp * OUT_HEIGHT * LINEW
        data[dst:dst + HEIGHT * LINEW] = srcdata[s:s + HEIGHT * LINEW]

    codes = {}
    for i, ch in enumerate(wanted):
        cp = 0x80 + i
        codes[ch] = cp
        off = cp * OUT_HEIGHT * LINEW
        for b in range(OUT_HEIGHT * LINEW):
            data[off + b] = 0
        #These are condensed faces - most glyphs use 5 or 6 of the 8 columns, so
        #measure the ink rather than reserving the full cell, or the text sprawls.
        used = [x for row in glyphs[ch] for x, on in enumerate(row) if on]
        ink_w = (max(used) + 2) if used else 4
        scale = KANA_PX // 8
        dst_w = min(ink_w * scale, LINEW * 8)

        for y in range(KANA_PX):
            for x in range(dst_w):
                if glyphs[ch][y // scale][x // scale]:
                    data[off + y * LINEW + (x >> 3)] |= 1 << (x & 7)
        widths[cp] = dst_w

    #charHeight is what the renderer uses for the glyph cell, so it has to agree
    header = bytearray(src[:HDR])
    header[20:24] = OUT_HEIGHT.to_bytes(4, "little")
    io.open(OUT, "wb").write(bytes(header) + bytes(widths) + bytes(data))
    io.open(MAP, "w", encoding="utf-8").write(
        json.dumps(codes, ensure_ascii=False, indent=1, sort_keys=True))
    sys.stderr.write("font_ja.oft: %d characters mapped to 0x80-0x%02X\n"
                     % (len(wanted), 0x80 + len(wanted) - 1))


main()
