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


def decode_pal(data):
    out = []
    for i in range(0, len(data), 2):
        w = data[i] | (data[i + 1] << 8)
        out.append(((w & 31) << 3, ((w >> 5) & 31) << 3, ((w >> 10) & 31) << 3))
    return out


def main():
    tris, tri_of = G.build_triangles()
    verts = G.valid_vertices(tri_of)
    W, H = G.IW, G.IH
    owner, line, axis, det = G.compute_layers(tris, tri_of, verts)
    single = [[len(G.tile_owners(owner, tx, ty)) <= 1 for tx in range(G.TW)]
              for ty in range(G.TH)]
    pals_flat = decode_pal((ROOT / "res/board.pal").read_bytes())
    pals = [pals_flat[i * 16:(i + 1) * 16] for i in range(7)]
    pal0 = pals[0]

    arr = parse_c_arrays((ROOT / "src/boardtab.c").read_text())
    tiles = decode_pic((ROOT / "res/board.pic").read_bytes())
    TW, TH = G.TW, G.TH

    rng = random.Random(99)
    fails = 0
    for trial in range(5):
        tcol = [rng.randrange(6) for _ in tris]

        # ground truth in RGB (palette-agnostic: detail tiles use sub-pal 1)
        truth = [[pal0[0]] * W for _ in range(H)]
        for y in range(H):
            for x in range(W):
                if axis[y][x]:
                    truth[y][x] = pal0[14 if axis[y][x] == 2 else 13]
                    continue
                o = owner[y][x]
                if o < 0:
                    continue
                if line[y][x]:
                    truth[y][x] = pal0[7 + tcol[o]]
                elif det[y][x] and single[y // 8][x // 8]:
                    truth[y][x] = pals[1 + tcol[o]][3]  # dim detail tint
                else:
                    truth[y][x] = pal0[1 + tcol[o]]

        # render.c path: entry lookup -> tile pixels -> flips -> palette
        composed = [[pal0[0]] * W for _ in range(H)]
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
                        e = arr["entryTable"][arr["structBase"][sid]] | ((1 + ca) << 10)
                tid, hf, vf = e & 0x3FF, e & 0x4000, e & 0x8000
                epal = pals[(e >> 10) & 7]
                for py in range(8):
                    for px in range(8):
                        sx = 7 - px if hf else px
                        sy = 7 - py if vf else py
                        composed[ty * 8 + py][tx * 8 + px] = epal[tiles[tid][sy][sx]]

        bad = sum(1 for y in range(H) for x in range(W)
                  if truth[y][x] != composed[y][x])
        status = "OK" if bad == 0 else f"FAIL ({bad} px differ)"
        print(f"trial {trial}: {status}")
        fails += bad != 0

        if trial == 0:
            from PIL import Image
            img = Image.new("RGB", (W, H))
            p = img.load()
            for y in range(H):
                for x in range(W):
                    p[x, y] = composed[y][x]
            img.resize((W * 3, H * 3), Image.NEAREST).save(ROOT / "res/composed.png")

    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
