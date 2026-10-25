#!/usr/bin/env python3
# The title's home-screen icon, sce_sys/icon0.png, made from the owner's logo
# ps5/title/art/rs.png (900x512: a round logo on white, see art/README.md).
# The circle is cut out, scaled to fill a 512x512 tile, and the tile's corners
# take the circle's own edge colour. Drawn at 4x and reduced so the edge is
# smooth.
#
#   python3 ps5/title/tools/make-title-icon.py     write the file
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

PS5 = Path(__file__).resolve().parent.parent
SOURCE = PS5 / "art" / "rs.png"
SIZE, SCALE = 512, 4


def main() -> int:
    source = Image.open(SOURCE).convert("RGB")
    pixels = np.asarray(source).astype(np.int32)
    # The logo's circle: everything not near-white
    ys, xs = np.nonzero(pixels.sum(-1) < 600)
    left, right, top, bottom = xs.min(), xs.max() + 1, ys.min(), ys.max() + 1
    circle = source.crop((left, top, right, bottom))
    side = circle.size[0]
    if abs(circle.size[1] - side) > 2:
        print(f"{SOURCE}: logo is not round ({circle.size})", file=sys.stderr)
        return 1
    # The corner colour: the median of the circle's outer ring
    rgb = np.asarray(circle).astype(np.float64)
    yy, xx = np.mgrid[0:circle.size[1], 0:side]
    distance = np.hypot(xx - (side - 1) / 2, yy - (circle.size[1] - 1) / 2)
    ring = rgb[(distance > side * 0.425) & (distance < side * 0.485)]
    corner = tuple(int(value) for value in np.median(ring, 0))
    big = SIZE * SCALE
    mask = Image.new("L", (big, big), 0)
    ImageDraw.Draw(mask).ellipse((3 * SCALE, 3 * SCALE, big - 3 * SCALE, big - 3 * SCALE), fill=255)
    tile = Image.new("RGB", (big, big), corner)
    tile.paste(circle.resize((big, big), Image.LANCZOS), (0, 0), mask)
    out = PS5 / "sce_sys" / "icon0.png"
    tile.resize((SIZE, SIZE), Image.LANCZOS).save(out, optimize=True)
    print(f"sce_sys/icon0.png: {out.stat().st_size} bytes, corner colour {corner}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
