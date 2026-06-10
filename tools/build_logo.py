#!/usr/bin/env python3
"""Decode deadfall-snes's studio logo (res/logo.pic/.map/.pal) and re-encode
it as a Rotatron BG2 asset for the falling-logo pre-intro.

The deadfall asset is 4bpp tiles + a 32x64 map; we rasterize it, find the
logo's bounding box, and compose a 256x224 canvas with the logo's top edge at
y = LOGO_TARGET (92) - the landing position the physics constants expect.
Output: res/logo2.pic/.map/.pal via the shared BG2 encoder."""

from pathlib import Path
from PIL import Image
import build_backdrop as BB

ROOT = Path(__file__).resolve().parent.parent
DF = Path("/Users/marcovhv/projects/GIT/deadfall-snes/res")
LOGO_TARGET = 92


def main():
    pic = (DF / "logo.pic").read_bytes()
    mp = (DF / "logo.map").read_bytes()
    pal_raw = (DF / "logo.pal").read_bytes()
    pals = []
    for i in range(0, len(pal_raw), 2):
        w = pal_raw[i] | (pal_raw[i + 1] << 8)
        pals.append(((w & 31) << 3, ((w >> 5) & 31) << 3, ((w >> 10) & 31) << 3))

    tiles = []
    for off in range(0, len(pic), 32):
        t = pic[off:off + 32]
        px = [[0] * 8 for _ in range(8)]
        for y in range(8):
            b0, b1 = t[y * 2], t[y * 2 + 1]
            b2, b3 = t[16 + y * 2], t[16 + y * 2 + 1]
            for x in range(8):
                bit = 0x80 >> x
                px[y][x] = (1 if b0 & bit else 0) | (2 if b1 & bit else 0) | \
                           (4 if b2 & bit else 0) | (8 if b3 & bit else 0)
        tiles.append(px)

    rows = len(mp) // 64  # 32 entries per row, 2 bytes each
    img = Image.new("RGB", (256, rows * 8), (0, 0, 0))
    p = img.load()
    for ty in range(rows):
        for tx in range(32):
            e = mp[(ty * 32 + tx) * 2] | (mp[(ty * 32 + tx) * 2 + 1] << 8)
            tid, paln = e & 0x3FF, (e >> 10) & 7
            hf, vf = e & 0x4000, e & 0x8000
            if tid >= len(tiles):
                continue
            for y in range(8):
                for x in range(8):
                    c = tiles[tid][7 - y if vf else y][7 - x if hf else x]
                    if c:
                        ci = paln * 16 + c
                        if ci < len(pals):
                            p[tx * 8 + x, ty * 8 + y] = pals[ci]

    bbox = img.getbbox()
    logo = img.crop(bbox)
    canvas = Image.new("RGB", (256, 224), (0, 0, 0))
    canvas.paste(logo, ((256 - logo.width) // 2, LOGO_TARGET))
    BB.convert(canvas, "logo2")
    print(f"logo: {logo.width}x{logo.height} placed at y={LOGO_TARGET}")


if __name__ == "__main__":
    main()
