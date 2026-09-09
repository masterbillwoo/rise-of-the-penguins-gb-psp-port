#!/usr/bin/env python3
"""Build build/config/icons/ICON0_<code>.PNG - one XMB icon per language.

Ten launchers sitting next to each other in the XMB were impossible to tell apart:
the title underneath says the language, but the icon is what you actually look at,
and every one of them was identical.

Each icon gets a small tag in the top-right corner. The letters are drawn with the
launcher's own menu font and the game's own colours, so the badge belongs to the
same design as everything else rather than looking like a sticker.

    python build/make_icons.py
"""
import os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..")
ICON = os.path.join(HERE, "config", "ICON0.PNG")
FONT = os.path.join(ROOT, "_masterboy", "Res", "font.oft")
OUTDIR = os.path.join(HERE, "config", "icons")

#Rise of the Penguins GB's own palette (project/settings.gbsres)
NAVY  = (32, 40, 80)
GREEN = (176, 240, 136)
MINT  = (232, 248, 224)

#Two letters each, per ROM-suffix language code. The Brazilian build is tagged BR
#rather than PT so it cannot be confused with the European one - the pair most
#likely to be picked up by mistake.
#
#There is deliberately no entry for English: that build is the plain one, and it
#keeps the untouched icon. Only the translations are marked.
TAGS = {
    "de": "DE", "es": "ES", "fr": "FR", "it": "IT",
    "ja": "JA", "nl": "NL", "pl": "PL", "pt": "PT", "pt_BR": "BR",
}

HDR, NGLYPH, HEIGHT, LINEW = 64, 256, 13, 3
SCALE = 2          # the font is ~8px tall; 2x reads clearly at icon size
PAD_X, PAD_Y = 5, 3
MARGIN = 5


def font_glyphs():
    d = open(FONT, "rb").read()
    widths = d[HDR:HDR + NGLYPH]
    data = d[HDR + NGLYPH:]

    def glyph(ch):
        cp = ord(ch)
        off = cp * HEIGHT * LINEW
        #Bits are least significant first - the other way round renders mirrored
        rows = [[(data[off + r * LINEW + (c >> 3)] >> (c & 7)) & 1
                 for c in range(LINEW * 8)] for r in range(HEIGHT)]
        return rows, widths[cp]
    return glyph


def main():
    from PIL import Image
    if not os.path.isfile(ICON):
        sys.exit("no base icon at %s" % ICON)
    glyph = font_glyphs()
    os.makedirs(OUTDIR, exist_ok=True)

    for code, tag in sorted(TAGS.items()):
        im = Image.open(ICON).convert("RGB")
        W, H = im.size

        #Cap rows of this font are 3..10, so the badge only needs those 8 rows
        top, bot = 3, 11
        text_w = sum(glyph(c)[1] for c in tag) * SCALE
        text_h = (bot - top) * SCALE
        bw, bh = text_w + PAD_X * 2, text_h + PAD_Y * 2
        bx, by = W - MARGIN - bw, MARGIN

        for x in range(bx, bx + bw):
            for y in range(by, by + bh):
                edge = (x < bx + 1 or x >= bx + bw - 1 or
                        y < by + 1 or y >= by + bh - 1)
                im.putpixel((x, y), GREEN if edge else NAVY)

        pen = bx + PAD_X
        for ch in tag:
            rows, gw = glyph(ch)
            for r in range(top, bot):
                for c in range(gw):
                    if rows[r][c]:
                        for dy in range(SCALE):
                            for dx in range(SCALE):
                                px = pen + c * SCALE + dx
                                py = by + PAD_Y + (r - top) * SCALE + dy
                                if bx < px < bx + bw - 1 and by < py < by + bh - 1:
                                    im.putpixel((px, py), MINT)
            pen += gw * SCALE

        im.save(os.path.join(OUTDIR, "ICON0_%s.PNG" % code))

    sys.stderr.write("%d language icons written to %s\n" % (len(TAGS), OUTDIR))


main()
