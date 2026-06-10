#!/usr/bin/env python3
"""End-to-end check of the board tile pipeline.

Replays render.c's boardRebuildMap() on the host using the EMITTED artifacts
(res/board.pic + the arrays parsed out of src/boardtab.c), rasterizes the
resulting tilemap (tile pixels + H/V flip bits), and pixel-diffs against the
ground-truth owner/line render for several random colorings. Catches planar
encoding, flip-dedup, table emission, and role->palette mapping bugs without
an emulator.
"""

import random
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build_board_gfx as G

ROOT = Path(__file__).resolve().parent.parent


def parse_c_arrays(text):
    arrays = {}
    for m in re.finditer(r"const u(?:8|16) (\w+)(?:\[[^\]]*\])+ = \{(.*?)\};", text, re.S):
        arrays[m.group(1)] = [int(v) for v in re.findall(r"\d+", m.group(2))]
    return arrays


def decode_pic(data):
    tiles = []
    for off in range(0, len(data), 32):
        t = data[off:off + 32]
        px = [[0] * 8 for _ in range(8)]
        for y in range(8):
            b0, b1 = t[y * 2], t[y * 2 + 1]
            b2, b3 = t[16 + y * 2], t[16 + y * 2 + 1]
            for x in range(8):
                bit = 0x80 >> x
                px[y][x] = ((b0 & bit) and 1) | ((b1 & bit) and 2) | \
                           ((b2 & bit) and 4) | ((b3 & bit) and 8)
        tiles.append(px)
    return tiles


def main():
    tris, tri_of = G.build_triangles()
    verts = G.valid_vertices(tri_of)
    W, H = G.IW, G.IH
    owner = [[G.owner_at(x - G.MARGIN, y - G.MARGIN, tri_of) for x in range(W)]
             for y in range(H)]

    def own(x, y):
        return owner[y][x] if 0 <= x < W and 0 <= y < H else -1

    line = [[any(own(x + dx, y + dy) != owner[y][x]
                 for dy in (-1, 0, 1) for dx in (-1, 0, 1))
             and owner[y][x] >= 0 for x in range(W)] for y in range(H)]
    axis = [[G.axis_at(x, y, verts) for x in range(W)] for y in range(H)]

    arr = parse_c_arrays((ROOT / "src/boardtab.c").read_text())
    tiles = decode_pic((ROOT / "res/board.pic").read_bytes())
    TW, TH = G.TW, G.TH

    rng = random.Random(99)
    fails = 0
    for trial in range(5):
        tcol = [rng.randrange(6) for _ in tris]

        # ground truth palette-index image (axis pins on top)
        truth = [[0] * W for _ in range(H)]
        for y in range(H):
            for x in range(W):
                if axis[y][x]:
                    truth[y][x] = 14 if axis[y][x] == 2 else 13
                    continue
                o = owner[y][x]
                if o >= 0:
                    truth[y][x] = (7 if line[y][x] else 1) + tcol[o]

        # render.c path: entry lookup -> tile pixels -> flips
        composed = [[0] * W for _ in range(H)]
        for ty in range(TH):
            for tx in range(TW):
                sid = arr["cellStruct"][ty * TW + tx]
                if sid == 0xFF:
                    continue
                nown = arr["structOwners"][sid]
                if nown == 0:
                    e = arr["entryTable"][arr["structBase"][sid]]
                else:
                    a = arr["cellTriA"][ty * TW + tx]
                    ca = tcol[a]
                    if nown == 2:
                        b = arr["cellTriB"][ty * TW + tx]
                        e = arr["entryTable"][arr["structBase"][sid] + ca * 9 + tcol[b]]
                    else:
                        e = arr["entryTable"][arr["structBase"][sid] + ca]
                tid, hf, vf = e & 0x3FF, e & 0x4000, e & 0x8000
                for py in range(8):
                    for px in range(8):
                        sx = 7 - px if hf else px
                        sy = 7 - py if vf else py
                        composed[ty * 8 + py][tx * 8 + px] = tiles[tid][sy][sx]

        bad = sum(1 for y in range(H) for x in range(W)
                  if truth[y][x] != composed[y][x])
        status = "OK" if bad == 0 else f"FAIL ({bad} px differ)"
        print(f"trial {trial}: {status}")
        fails += bad != 0

        if trial == 0:
            from PIL import Image
            img = Image.new("RGB", (W, H))
            p = img.load()
            pal = [(0, 0, 0)] + [tuple(min(255, int(v * G.FILL_BOOST)) for v in G.DARK[n])
                                 for n, _ in G.NEON] + [rgb for _, rgb in G.NEON] + \
                  [G.GREY, G.WHITE, (0, 0, 0)]
            for y in range(H):
                for x in range(W):
                    p[x, y] = pal[composed[y][x]]
            img.resize((W * 3, H * 3), Image.NEAREST).save(ROOT / "res/composed.png")

    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
