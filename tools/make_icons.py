#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Builds the 16bit Forge icons from the logo (Python 3 + Pillow).

  python tools/make_icons.py                      # regenerate assets/icons/ from the 512 logo
  python tools/make_icons.py --logo original.png  # first refresh the 512 logo from a full-size one

Outputs (all committed):
  assets/branding/16bitForge_logo_512.png  logo, transparent outside its rounded frame
  assets/icons/16bitForge.ico              256/128/64/48 from the logo + hand-built 32/24/16
  assets/icons/16bitForge_64.png           runtime window icon (embedded by CMake)

The 32/24/16 frames are a plain anvil silhouette drawn pixel by pixel below: the logo's text
and texture don't survive at those sizes.
"""
import argparse
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFilter

ROOT = Path(__file__).resolve().parent.parent
LOGO = ROOT / "assets/branding/16bitForge_logo_512.png"
ICONS = ROOT / "assets/icons"

LOGO_SIZES = (256, 128, 64, 48)

# Hand-built frames. '.' background, 'o' frame, '#' steel, '+' lit top face, '_' transparent.
PALETTE = {
    ".": (24, 26, 30, 255),
    "o": (104, 112, 122, 255),
    "#": (196, 203, 212, 255),
    "+": (240, 244, 248, 255),
    "_": (0, 0, 0, 0),
}
FRAMES = {
    16: """
_oooooooooooooo_
o..............o
o..............o
o..............o
o....+++++++++.o
o.++++#######..o
o...########...o
o.....#####....o
o.......##.....o
o.......##.....o
o......####....o
o....########..o
o....########..o
o..............o
o..............o
_oooooooooooooo_
""",
    24: """
__oooooooooooooooooooo__
_o....................o_
o......................o
o......................o
o......................o
o.......++++++++++++...o
o..+++++############...o
o...################...o
o.....##############...o
o.......###########....o
o.........#######......o
o..........#####.......o
o..........#####.......o
o..........#####.......o
o.........#######......o
o.......###########....o
o......#############...o
o......#############...o
o......................o
o......................o
o......................o
o......................o
_o....................o_
__oooooooooooooooooooo__
""",
    32: """
___oooooooooooooooooooooooooo___
__o..........................o__
_o............................o_
o..............................o
o..............................o
o..............................o
o..............................o
o..........++++++++++++++++....o
o...+++++++++################..o
o....########################..o
o......######################..o
o........####################..o
o..........#################...o
o.............############.....o
o..............#########.......o
o...............#######........o
o...............#######........o
o...............#######........o
o...............#######........o
o..............#########.......o
o.............###########......o
o...........###############....o
o..........#################...o
o..........#################...o
o..........#################...o
o..............................o
o..............................o
o..............................o
o..............................o
_o............................o_
__o..........................o__
___oooooooooooooooooooooooooo___
""",
}


def frame_image(size):
    rows = [r for r in FRAMES[size].split("\n") if r]
    assert len(rows) == size and all(len(r) == size for r in rows), size
    img = Image.new("RGBA", (size, size))
    img.putdata([PALETTE[c] for r in rows for c in r])
    return img


def logo_from_original(path):
    """Square crop, transparent outside the rounded frame (flood fill of the black corners)."""
    src = Image.open(path).convert("RGB")
    side = min(src.size)
    src = src.crop(((src.width - side) // 2, (src.height - side) // 2,
                    (src.width + side) // 2, (src.height + side) // 2))
    dark = src.convert("L").point(lambda v: 255 if v < 24 else 0)
    outside = Image.new("L", src.size, 0)
    for corner in ((0, 0), (side - 1, 0), (0, side - 1), (side - 1, side - 1)):
        probe = dark.copy()
        ImageDraw.floodfill(probe, corner, 128)
        outside = ImageChops.lighter(outside, probe.point(lambda v: 255 if v == 128 else 0))
    alpha = ImageChops.invert(outside).filter(ImageFilter.GaussianBlur(1.5))
    logo = src.convert("RGBA")
    logo.putalpha(alpha)
    return logo.resize((512, 512), Image.LANCZOS)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--logo", help="full-size logo PNG to refresh assets/branding from")
    ap.add_argument("--preview", help="also write 8x previews of the 16/24/32 frames to this folder")
    args = ap.parse_args()

    if args.logo:
        LOGO.parent.mkdir(parents=True, exist_ok=True)
        logo_from_original(args.logo).save(LOGO, optimize=True)
    logo = Image.open(LOGO).convert("RGBA")

    big = [logo.resize((s, s), Image.LANCZOS) for s in LOGO_SIZES]
    small = [frame_image(s) for s in (32, 24, 16)]
    frames = big + small
    ICONS.mkdir(parents=True, exist_ok=True)
    frames[0].save(ICONS / "16bitForge.ico", sizes=[f.size for f in frames], append_images=frames[1:])
    big[LOGO_SIZES.index(64)].save(ICONS / "16bitForge_64.png", optimize=True)

    if args.preview:
        out = Path(args.preview)
        out.mkdir(parents=True, exist_ok=True)
        for img in small:
            s = img.width
            bg = Image.new("RGBA", img.size, (255, 255, 255, 255))
            Image.alpha_composite(bg, img).resize((s * 8, s * 8), Image.NEAREST).save(out / f"icon_{s}_x8.png")


if __name__ == "__main__":
    main()
