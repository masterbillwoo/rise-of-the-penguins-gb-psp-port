#!/usr/bin/env python3
"""Generate the PSP XMB assets (ICON0.PNG and PIC1.PNG) from cover art.

The XMB shows ICON0 as the game's tile and PIC1 as the full-screen background
behind it when the tile is selected. Sizes are fixed by the PSP:

    ICON0.PNG   144x80    the tile
    PIC1.PNG    480x272   the background

Cover art is usually portrait, so the two need different treatment: the tile
contains the whole cover on a blurred fill of itself (nothing important is cropped),
while the background covers the screen and is darkened so the XMB's own white text
stays readable over it.

Usage:
    python build/make_xmb.py --source build/art/cover.png
"""

import argparse
import os

from PIL import Image, ImageEnhance, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CONFIG = os.path.join(ROOT, "build", "config")

ICON_W, ICON_H = 144, 80
PIC_W, PIC_H = 480, 272


def trim_bars(img, threshold=12):
    """Strip uniform black letterbox/pillarbox borders.

    The PortMaster covers are 640x480 canvases with the artwork pillarboxed inside,
    so fitting them directly would bake those bars into the XMB tile.
    """
    a = img.convert("RGB")
    px = a.load()
    w, h = a.size

    def bright_col(x):
        return max(sum(px[x, y]) for y in range(0, h, 2))

    def bright_row(y):
        return max(sum(px[x, y]) for x in range(0, w, 2))

    left = 0
    while left < w - 1 and bright_col(left) <= threshold:
        left += 1
    right = w - 1
    while right > left and bright_col(right) <= threshold:
        right -= 1
    top = 0
    while top < h - 1 and bright_row(top) <= threshold:
        top += 1
    bottom = h - 1
    while bottom > top and bright_row(bottom) <= threshold:
        bottom -= 1

    if (left, top, right, bottom) == (0, 0, w - 1, h - 1):
        return img, False
    return img.crop((left, top, right + 1, bottom + 1)), True


def cover_fit(img, w, h, anchor=0.5):
    """Scale to fill w x h, cropping the overflow.

    anchor picks where the vertical crop sits: 0 keeps the top, 1 the bottom. A 4:3
    cover squeezed into the 16:9 background loses a lot of height, and the title
    logo usually sits low, so the default leans downward.
    """
    sc = max(w / img.width, h / img.height)
    im = img.resize((max(1, round(img.width * sc)), max(1, round(img.height * sc))),
                    Image.LANCZOS)
    left = (im.width - w) // 2
    top = int(round((im.height - h) * min(max(anchor, 0.0), 1.0)))
    return im.crop((left, top, left + w, top + h))


def contain_fit(img, w, h):
    """Scale so the whole image fits inside w x h."""
    sc = min(w / img.width, h / img.height)
    return img.resize((max(1, round(img.width * sc)), max(1, round(img.height * sc))),
                      Image.LANCZOS)


def make_icon(src, out, fit="auto"):
    """Render the 144x80 tile.

    A source already close to the tile's 1.8 aspect (a banner, say) is simply
    cover-fitted so it fills the tile. A portrait cover would lose most of its
    height that way, so it is contained instead, on a blurred copy of itself.
    """
    target = ICON_W / float(ICON_H)
    ratio = src.width / float(src.height)
    if fit == "auto":
        fit = "cover" if 0.65 * target <= ratio <= 1.6 * target else "contain"

    if fit == "cover":
        icon = cover_fit(src, ICON_W, ICON_H)
    else:
        icon = cover_fit(src, ICON_W, ICON_H).filter(ImageFilter.GaussianBlur(6))
        icon = ImageEnhance.Brightness(icon).enhance(0.55)
        fore = contain_fit(src, ICON_W, ICON_H)
        icon.paste(fore, ((ICON_W - fore.width) // 2, (ICON_H - fore.height) // 2))
    icon.convert("RGB").save(out)
    return icon.size, fit


def make_pic1(src, out, darken, anchor):
    pic = cover_fit(src, PIC_W, PIC_H, anchor)
    pic = ImageEnhance.Brightness(pic).enhance(darken)
    pic.convert("RGB").save(out)
    return pic.size


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--source", default=os.path.join(ROOT, "build", "art", "gb_cover.png"),
                    help="artwork to derive both assets from")
    ap.add_argument("--icon-source", help="different artwork for the tile only")
    ap.add_argument("--icon-fit", default="auto", choices=["auto", "cover", "contain"],
                    help="how the tile art is fitted; auto picks by aspect ratio")
    ap.add_argument("--pic-source", help="different artwork for the background only")
    ap.add_argument("--darken", type=float, default=0.55,
                    help="brightness multiplier for PIC1 (lower = darker background)")
    ap.add_argument("--anchor", type=float, default=0.85,
                    help="PIC1 vertical crop position: 0 top, 0.5 centre, 1 bottom")
    ap.add_argument("--keep-bars", action="store_true",
                    help="do not strip black letterbox/pillarbox borders")
    ap.add_argument("--out", default=CONFIG)
    args = ap.parse_args()

    src = Image.open(args.source).convert("RGB")
    icon_src = Image.open(args.icon_source).convert("RGB") if args.icon_source else src
    pic_src = Image.open(args.pic_source).convert("RGB") if args.pic_source else src
    if not args.keep_bars:
        icon_src, t1 = trim_bars(icon_src)
        pic_src, t2 = trim_bars(pic_src)
        if t1 or t2:
            print("trimmed letterbox bars")

    os.makedirs(args.out, exist_ok=True)
    icon = os.path.join(args.out, "ICON0.PNG")
    pic = os.path.join(args.out, "PIC1.PNG")
    isize, ifit = make_icon(icon_src, icon, args.icon_fit)
    print("ICON0.PNG %s (%s fit) from %s"
          % (isize, ifit, os.path.basename(args.icon_source or args.source)))
    print("PIC1.PNG  %s darkened to %.0f%%, crop anchor %.2f, from %s"
          % (make_pic1(pic_src, pic, args.darken, args.anchor),
             args.darken * 100, args.anchor,
             os.path.basename(args.pic_source or args.source)))
    print("written to %s" % args.out)


if __name__ == "__main__":
    main()
