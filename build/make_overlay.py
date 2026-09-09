#!/usr/bin/env python3
"""Generate a 480x272 overlay.png for the PSP launcher.

The overlay is one RGBA image composited over the finished frame by OverlayDraw()
in psp/VideoGu.c. It serves two purposes at once:

  * an LCD pixel grid over the play area, and
  * bezel art in the margins the game does not cover.

The grid is aligned to real Game Boy pixel boundaries by replaying the same integer
scaling maths the GU scaler uses (VideoGuUpdate_Core in psp/VideoGu.c), so the lines
land where the pixel edges actually are rather than on a guessed pitch.

A caveat worth knowing: at "Fit" scaling a Game Boy pixel is 1.887 screen pixels,
so a 1px grid line covers more than half of every cell. A crisp grid is simply not
possible at that ratio - the default alpha is therefore low, giving a soft LCD
texture rather than a hard grid. Raise --grid-alpha to taste, or use --mode 1x for
pixel-perfect output with a large bezel and no grid at all.

Usage:
    python build/make_overlay.py                      # subtle grid, no bezel
    python build/make_overlay.py --grid-alpha 70
    python build/make_overlay.py --bezel art.png      # bezel + grid
    python build/make_overlay.py --mode 1x --bezel art.png
"""

import argparse
import os

from PIL import Image, ImageDraw, ImageFilter

SCREEN_W, SCREEN_H = 480, 272
GB_W, GB_H = 160, 144

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_OUT = os.path.join(ROOT, "build", "config", "overlay.png")


def viewport(mode):
    """Replicate VideoGuUpdate_Core's integer maths for the chosen scaling mode."""
    if mode == "fit":
        scale = min((4096 * SCREEN_W) // GB_W, (4096 * SCREEN_H) // GB_H)
        w = (GB_W * scale) >> 12
        h = (GB_H * scale) >> 12
    elif mode == "1x":
        w, h = GB_W, GB_H
    elif mode == "1.5x":
        w, h = GB_W * 3 // 2, GB_H * 3 // 2
    else:
        raise ValueError("unknown mode %r" % mode)
    x = SCREEN_W // 2 - w // 2
    y = SCREEN_H // 2 - h // 2
    return x, y, w, h


def draw_grid(img, rect, colour, alpha, step):
    """Grid lines locked to Game Boy pixel boundaries.

    Faithful to where the pixels actually are, but at Fit scaling the cell pitch is
    1.887px so cells come out 2px/2px/1px and the rounding shows as banding.
    """
    x0, y0, w, h = rect
    if w <= GB_W and h <= GB_H:
        return 0  # 1:1 or smaller - no room for lines between pixels
    d = ImageDraw.Draw(img)
    line = colour + (alpha,)
    n = 0
    for i in range(step, GB_W, step):
        x = x0 + (i * w) // GB_W
        d.line([(x, y0), (x, y0 + h - 1)], fill=line)
        n += 1
    for j in range(step, GB_H, step):
        y = y0 + (j * h) // GB_H
        d.line([(x0, y), (x0 + w - 1, y)], fill=line)
        n += 1
    return n


def draw_texture(img, rect, colour, alpha, pitch, soft, scanlines_only):
    """Fixed-pitch LCD texture, not locked to source pixels.

    The reference packs (ourigen/perfect_overlays) simulate RGB subpixel stripes,
    which needs >=3 screen px per source px - they run at 3.17. The PSP gives 1.887,
    so that effect is out of reach here. What does survive is a regular fine texture:
    a fixed pitch avoids the banding that per-pixel alignment causes, and a soft
    (cosine) profile avoids the harsh screen-door look of hard 1px lines.
    """
    x0, y0, w, h = rect
    import math
    px = img.load()
    r, g, b = colour
    for yy in range(y0, min(y0 + h, SCREEN_H)):
        for xx in range(x0, min(x0 + w, SCREEN_W)):
            if soft:
                fy = 0.5 - 0.5 * math.cos(2 * math.pi * (yy - y0) / pitch)
                fx = 0.5 - 0.5 * math.cos(2 * math.pi * (xx - x0) / pitch)
            else:
                fy = 1.0 if (yy - y0) % pitch == 0 else 0.0
                fx = 1.0 if (xx - x0) % pitch == 0 else 0.0
            f = fy if scanlines_only else max(fx, fy)
            a = int(alpha * f)
            if a <= 0:
                continue
            old = px[xx, yy]
            na = min(255, old[3] + a)
            px[xx, yy] = (r, g, b, na)
    return 1


def draw_shadow(img, rect, width):
    """Soft inner shadow around the play area - reads as a recessed LCD, and is
    what actually sells the effect at this screen size."""
    if not width:
        return
    x0, y0, w, h = rect
    sh = Image.new("RGBA", (SCREEN_W, SCREEN_H), (0, 0, 0, 0))
    sd = ImageDraw.Draw(sh)
    for i in range(width):
        a = int(150 * (1.0 - i / float(width)) ** 2)
        sd.rectangle([x0 + i, y0 + i, x0 + w - 1 - i, y0 + h - 1 - i],
                     outline=(0, 0, 0, a))
    img.alpha_composite(sh.filter(ImageFilter.GaussianBlur(0.6)))


def bezel_window(bez):
    """Find the transparent rectangle a RetroArch bezel leaves for the game."""
    alpha = bez.getchannel("A")
    w, h = bez.size
    px = alpha.load()

    def clear_col(x):
        return any(px[x, y] < 8 for y in range(0, h, 2))

    def clear_row(y):
        return any(px[x, y] < 8 for x in range(0, w, 2))

    xs = [x for x in range(w) if clear_col(x)]
    ys = [y for y in range(h) if clear_row(y)]
    if not xs or not ys:
        return None
    return xs[0], ys[0], xs[-1] - xs[0] + 1, ys[-1] - ys[0] + 1


def draw_bezel(img, rect, bezel_path, shadow):
    """Composite a RetroArch bezel so its own window lands on our play area.

    These packs are built for 720p/1080p and carry their artwork - a power LED, the
    console's name down one side - around a transparent hole. Cover-fitting the
    image to 480x272 would put that hole in the wrong place, so instead the bezel is
    scaled and offset until its hole matches the viewport exactly. The two aspect
    ratios differ slightly, so the surround stretches a little; it is flat colour
    and lettering at the edges, which takes it well.
    """
    x0, y0, w, h = rect
    bez = Image.open(bezel_path).convert("RGBA")

    win = bezel_window(bez)
    if not win:
        # No hole to align to: fall back to covering the screen and punching one.
        sc = max(SCREEN_W / bez.width, SCREEN_H / bez.height)
        bez = bez.resize((max(1, round(bez.width * sc)), max(1, round(bez.height * sc))),
                         Image.LANCZOS)
        left = (bez.width - SCREEN_W) // 2
        top = (bez.height - SCREEN_H) // 2
        bez = bez.crop((left, top, left + SCREEN_W, top + SCREEN_H))
        hole = Image.new("L", (SCREEN_W, SCREEN_H), 255)
        ImageDraw.Draw(hole).rectangle([x0, y0, x0 + w - 1, y0 + h - 1], fill=0)
        bez.putalpha(Image.composite(bez.getchannel("A"),
                                     Image.new("L", bez.size, 0), hole))
        img.alpha_composite(bez)
        draw_shadow(img, rect, shadow)
        return "no window found, centre-cropped"

    wx, wy, ww, wh = win
    sx = w / float(ww)
    sy = h / float(wh)
    scaled = bez.resize((max(1, round(bez.width * sx)), max(1, round(bez.height * sy))),
                        Image.LANCZOS)

    canvas = Image.new("RGBA", (SCREEN_W, SCREEN_H), (0, 0, 0, 0))
    canvas.alpha_composite(scaled, (x0 - int(round(wx * sx)), y0 - int(round(wy * sy))))
    img.alpha_composite(canvas)

    draw_shadow(img, rect, shadow)
    return "window %dx%d at %d,%d -> %dx%d" % (ww, wh, wx, wy, w, h)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mode", default="fit", choices=["fit", "1x", "1.5x"],
                    help="scaling mode the launcher is configured for")
    ap.add_argument("--grid-alpha", type=int, default=40,
                    help="0-255; 0 disables the grid (default 40, deliberately soft)")
    ap.add_argument("--grid-colour", default="0,0,0",
                    help="R,G,B of the grid lines")
    ap.add_argument("--grid-step", type=int, default=1,
                    help="draw a line every N Game Boy pixels")
    ap.add_argument("--style", default="pixel",
                    choices=["pixel", "texture", "scanlines"],
                    help="pixel = lines on GB pixel boundaries (can band); "
                         "texture = fixed-pitch LCD texture; scanlines = rows only")
    ap.add_argument("--pitch", type=float, default=2.0,
                    help="texture/scanlines: pattern pitch in screen px")
    ap.add_argument("--hard", action="store_true",
                    help="texture/scanlines: hard 1px lines instead of a soft profile")
    ap.add_argument("--bezel", help="image to use as bezel art in the margins")
    ap.add_argument("--shadow", type=int, default=6,
                    help="inner shadow width in px around the play area; 0 disables")
    ap.add_argument("--out", default=DEFAULT_OUT)
    args = ap.parse_args()

    rect = viewport(args.mode)
    x0, y0, w, h = rect
    img = Image.new("RGBA", (SCREEN_W, SCREEN_H), (0, 0, 0, 0))

    bezel_note = None
    if args.bezel:
        bezel_note = draw_bezel(img, rect, args.bezel, args.shadow)
    else:
        draw_shadow(img, rect, args.shadow)

    lines = 0
    if args.grid_alpha > 0:
        colour = tuple(int(c) for c in args.grid_colour.split(","))
        if args.style == "pixel":
            lines = draw_grid(img, rect, colour, args.grid_alpha, args.grid_step)
        else:
            draw_texture(img, rect, colour, args.grid_alpha, args.pitch,
                         not args.hard, args.style == "scanlines")
            lines = -1

    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    img.save(args.out)

    print("mode %s: play area x=%d y=%d %dx%d (%.3f screen px per GB px)"
          % (args.mode, x0, y0, w, h, w / float(GB_W)))
    if lines >= 0:
        print("style pixel: %d grid lines at alpha %d, %d px inner shadow"
              % (lines, args.grid_alpha, args.shadow))
    else:
        print("style %s: pitch %.2f px, %s profile, alpha %d, %d px inner shadow"
              % (args.style, args.pitch, "hard" if args.hard else "soft",
                 args.grid_alpha, args.shadow))
    if bezel_note:
        print("bezel %s: %s" % (os.path.basename(args.bezel), bezel_note))
    print("written %s" % args.out)
    if args.mode == "fit" and lines and args.grid_alpha > 90:
        print("note: at 1.887 px per GB pixel this alpha will look heavy; "
              "try --grid-alpha 40-70")


if __name__ == "__main__":
    main()
