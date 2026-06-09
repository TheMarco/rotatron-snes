#!/usr/bin/env python3
"""Generate the Rotatron board tileset, palette, and C lookup tables.

Geometry: triangle edge S=32px, row height H=24px (a ~13% vertical squash of
the true sqrt(3)/2 so everything lands on the 8px tile grid). The 54-triangle
hex board renders as 192x168px = 24x21 tiles.

Approach: render the whole board once as an "owner map" (which triangle owns
each pixel) plus a line mask (1px inner outline wherever an 8-neighbor has a
different owner -> 2px bright seams between triangles, 1px on the hex rim).
Horizontal triangle edges sit on tile boundaries and diagonals never share a
tile, so every 8x8 tile is owned by at most TWO triangles (asserted).

Each distinct tile "structure" (per-pixel role: outside / fillA / lineA /
fillB / lineB) then gets one VRAM tile per color combination actually
expressible (6 single-owner, 36 ordered pairs dual-owner), deduplicated
across H/V flips via the tilemap flip bits. The C tables let the runtime
recolor a triangle by rewriting the tilemap words of its cells:

    entry = entryTable[structBase[s] + (dual ? cA*6+cB : cA)]

Outputs:
    res/board.pic        4bpp planar tiles (tile 0 = blank)
    res/board.pal        16-color BGR555 subpalette
    src/boardtab.c, include/boardtab.h   generated lookup tables
    res/preview.png      3x preview with random colors (geometry check)
"""

import random
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

SPX = 32          # triangle edge in px
HPX = 24          # triangle row height in px
BW, BH = 192, 168 # board px size
TW, TH = BW // 8, BH // 8  # 24 x 21 tiles
ROWS, COLS = 7, 12

# Phase-introduction order (must match include/core.h COLOR_*).
NEON = [
    ("magenta", (0xFF, 0x2B, 0xD6)),
    ("purple", (0x5C, 0x7C, 0xFF)),
    ("cyan", (0x00, 0xE5, 0xFF)),
    ("yellow", (0xF5, 0xFF, 0x00)),
    ("green", (0x00, 0xFF, 0x66)),
    ("orange", (0xFF, 0x7A, 0x00)),
]
DARK = {
    "magenta": (0x26, 0x06, 0x20),
    "purple": (0x0E, 0x13, 0x26),
    "cyan": (0x00, 0x22, 0x26),
    "yellow": (0x24, 0x26, 0x00),
    "green": (0x00, 0x26, 0x0F),
    "orange": (0x26, 0x12, 0x00),
}
FILL_BOOST = 2.0  # no bloom on SNES: lift the dark fills so they read vs black


def cell_in_hex(col, row):
    sj = 3 * row + (2 if (col + row) % 2 == 0 else 1)
    a = abs(sj - 9)
    b = abs(col - 5)
    return a <= 9 and 3 * b + a <= 18


def build_triangles():
    tris = []           # id -> (col, row)
    tri_of = {}         # (col, row) -> id
    for row in range(ROWS):
        for col in range(COLS):
            if cell_in_hex(col, row):
                tri_of[(col, row)] = len(tris)
                tris.append((col, row))
    return tris, tri_of


def owner_at(x, y, tri_of):
    """Triangle owning pixel center (x+.5, y+.5), or -1. Exact integer math:
    centers never land on a diagonal (parity argument), so ownership is
    unambiguous."""
    row = y // HPX
    if row < 0 or row >= ROWS:
        return -1
    xc6 = 6 * x + 3
    yc6 = 6 * (y - row * HPX) + 3
    for col in range(max(0, x // 16 - 1), min(COLS - 1, x // 16) + 1):
        if (col, row) not in tri_of:
            continue
        apex6 = (col + 1) * 16 * 6
        d = 3 * abs(xc6 - apex6)
        if (col + row) % 2 == 0:
            inside = d <= 2 * yc6              # up: apex at top
        else:
            inside = d <= 2 * (144 - yc6)      # down: apex at bottom
        if inside:
            return tri_of[(col, row)]
    return -1


def main():
    tris, tri_of = build_triangles()
    assert len(tris) == 54, len(tris)

    owner = [[owner_at(x, y, tri_of) for x in range(BW)] for y in range(BH)]

    def own(x, y):
        if 0 <= x < BW and 0 <= y < BH:
            return owner[y][x]
        return -1

    line = [[False] * BW for _ in range(BH)]
    for y in range(BH):
        for x in range(BW):
            o = owner[y][x]
            if o < 0:
                continue
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    if own(x + dx, y + dy) != o:
                        line[y][x] = True
                        break
                if line[y][x]:
                    break

    # ---- tile structures ----
    # role per pixel: 0 outside, 1 fillA, 2 lineA, 3 fillB, 4 lineB
    structs = {}        # key bytes -> struct id
    struct_owners = []  # struct id -> 1 or 2
    cell_struct = [[0xFF] * TW for _ in range(TH)]
    cell_tri = [[(0xFF, 0xFF)] * TW for _ in range(TH)]
    tri_cells = [[] for _ in tris]

    for ty in range(TH):
        for tx in range(TW):
            owners = []
            roles = bytearray(64)
            for py in range(8):
                for px in range(8):
                    o = owner[ty * 8 + py][tx * 8 + px]
                    if o < 0:
                        continue
                    if o not in owners:
                        owners.append(o)
                    ab = owners.index(o)  # 0 = A, 1 = B (first raster apparition)
                    roles[py * 8 + px] = 1 + 2 * ab + (1 if line[ty * 8 + py][tx * 8 + px] else 0)
            if not owners:
                continue
            assert len(owners) <= 2, f"tile {tx},{ty} has {len(owners)} owners"
            key = bytes(roles)
            if key not in structs:
                structs[key] = len(struct_owners)
                struct_owners.append(len(owners))
            sid = structs[key]
            cell_struct[ty][tx] = sid
            a = owners[0]
            b = owners[1] if len(owners) > 1 else 0xFF
            cell_tri[ty][tx] = (a, b)
            for o in owners:
                tri_cells[o].append((tx, ty))

    struct_keys = [None] * len(struct_owners)
    for k, sid in structs.items():
        struct_keys[sid] = k
    n_struct = len(struct_owners)

    # ---- palette ----
    # 0 transparent, 1..6 dark fills, 7..12 bright lines, 13 black, 14 white, 15 grey
    def boost(c):
        return tuple(min(255, int(v * FILL_BOOST)) for v in c)

    pal_rgb = [(0, 0, 0)]
    pal_rgb += [boost(DARK[name]) for name, _ in NEON]
    pal_rgb += [rgb for _, rgb in NEON]
    pal_rgb += [(0, 0, 0), (255, 255, 255), (96, 96, 96)]

    # ---- tiles: one graphic per (struct, color combo), dedup with flips ----
    tiles = [bytes(64)]                # tile 0 = blank
    canonical = {bytes(64): 0}         # stored tile pixels -> tile id
    tile_lookup = {bytes(64): (0, 0)}  # memo: any queried pixels -> (tile id, flips)

    def graphic(key, ca, cb):
        px = bytearray(64)
        for i, r in enumerate(key):
            if r == 0:
                px[i] = 0
            elif r == 1:
                px[i] = 1 + ca
            elif r == 2:
                px[i] = 7 + ca
            elif r == 3:
                px[i] = 1 + cb
            else:
                px[i] = 7 + cb
        return bytes(px)

    def hflip(p):
        return bytes(p[y * 8 + (7 - x)] for y in range(8) for x in range(8))

    def vflip(p):
        return bytes(p[(7 - y) * 8 + x] for y in range(8) for x in range(8))

    def tile_for(p):
        # Flip matching must run against CANONICAL stored tiles only: an
        # aliased entry's pixels are themselves a flip of some tile, so
        # composing its flip bits with ours would double-flip.
        if p in tile_lookup:
            return tile_lookup[p]
        h, v = hflip(p), vflip(p)
        for q, flips in ((p, 0), (h, 0x4000), (v, 0x8000), (vflip(h), 0xC000)):
            if q in canonical:
                tid = canonical[q]
                tile_lookup[p] = (tid, flips)
                return tid, flips
        tid = len(tiles)
        tiles.append(p)
        canonical[p] = tid
        tile_lookup[p] = (tid, 0)
        return tid, 0

    struct_base = []
    entry_table = []
    for sid in range(n_struct):
        struct_base.append(len(entry_table))
        key = struct_keys[sid]
        if struct_owners[sid] == 1:
            combos = [(ca, 0) for ca in range(6)]
        else:
            combos = [(ca, cb) for ca in range(6) for cb in range(6)]
        for ca, cb in combos:
            tid, flips = tile_for(graphic(key, ca, cb))
            assert tid < 1024, "BG tile index overflow"
            entry_table.append(tid | flips)  # palette 0, priority 0

    # ---- emit res/board.pic (4bpp planar) ----
    pic = bytearray()
    for t in tiles:
        planes = bytearray(32)
        for y in range(8):
            b0 = b1 = b2 = b3 = 0
            for x in range(8):
                c = t[y * 8 + x]
                bit = 0x80 >> x
                if c & 1:
                    b0 |= bit
                if c & 2:
                    b1 |= bit
                if c & 4:
                    b2 |= bit
                if c & 8:
                    b3 |= bit
            planes[y * 2] = b0
            planes[y * 2 + 1] = b1
            planes[16 + y * 2] = b2
            planes[16 + y * 2 + 1] = b3
        pic += planes
    (ROOT / "res/board.pic").write_bytes(pic)

    pal = bytearray()
    for r, g, b in pal_rgb:
        w = (b >> 3) << 10 | (g >> 3) << 5 | (r >> 3)
        pal += bytes((w & 0xFF, w >> 8))
    (ROOT / "res/board.pal").write_bytes(pal)

    # ---- emit C tables ----
    tri_cell_ofs = [0]
    tri_cell_xy = []
    for cells in tri_cells:
        for (tx, ty) in cells:
            tri_cell_xy += [tx, ty]
        tri_cell_ofs.append(len(tri_cell_xy) // 2)

    tri_of_cell = [[0xFF] * COLS for _ in range(ROWS)]
    for tid, (col, row) in enumerate(tris):
        tri_of_cell[row][col] = tid

    def carr(vals, per_line=24):
        out = []
        for i in range(0, len(vals), per_line):
            out.append("    " + ", ".join(str(v) for v in vals[i:i + per_line]) + ",")
        return "\n".join(out)

    h = f"""/* GENERATED by tools/build_board_gfx.py - do not edit. */
#ifndef BOARDTAB_H
#define BOARDTAB_H
#include "core_types.h"

#define BOARD_TILES_W {TW}
#define BOARD_TILES_H {TH}
#define N_STRUCTS {n_struct}
#define N_BOARD_VRAM_TILES {len(tiles)}
#define N_TRIANGLES 54

extern const u8 cellStruct[BOARD_TILES_H][BOARD_TILES_W];   /* 0xFF = blank */
extern const u8 cellTriA[BOARD_TILES_H][BOARD_TILES_W];
extern const u8 cellTriB[BOARD_TILES_H][BOARD_TILES_W];     /* 0xFF = single owner */
extern const u8 structOwners[N_STRUCTS];                    /* 1 or 2 */
extern const u16 structBase[N_STRUCTS];
extern const u16 entryTable[{len(entry_table)}];            /* tile | flip bits */
extern const u8 triCol[N_TRIANGLES];
extern const u8 triRow[N_TRIANGLES];
extern const u8 triOfCell[{ROWS}][{COLS}];                  /* 0xFF outside hex */
extern const u16 triCellOfs[N_TRIANGLES + 1];
extern const u8 triCellXY[{len(tri_cell_xy)}];              /* tx,ty pairs */
#endif
"""
    flat = lambda grid: [v for row in grid for v in row]
    c = f"""/* GENERATED by tools/build_board_gfx.py - do not edit. */
#include "boardtab.h"

const u8 cellStruct[BOARD_TILES_H][BOARD_TILES_W] = {{
{carr(flat(cell_struct))}
}};
const u8 cellTriA[BOARD_TILES_H][BOARD_TILES_W] = {{
{carr([t[0] for row in cell_tri for t in row])}
}};
const u8 cellTriB[BOARD_TILES_H][BOARD_TILES_W] = {{
{carr([t[1] for row in cell_tri for t in row])}
}};
const u8 structOwners[N_STRUCTS] = {{
{carr(struct_owners)}
}};
const u16 structBase[N_STRUCTS] = {{
{carr(struct_base)}
}};
const u16 entryTable[{len(entry_table)}] = {{
{carr(entry_table, 16)}
}};
const u8 triCol[N_TRIANGLES] = {{
{carr([c for c, r in tris])}
}};
const u8 triRow[N_TRIANGLES] = {{
{carr([r for c, r in tris])}
}};
const u8 triOfCell[{ROWS}][{COLS}] = {{
{carr(flat(tri_of_cell))}
}};
const u16 triCellOfs[N_TRIANGLES + 1] = {{
{carr(tri_cell_ofs)}
}};
const u8 triCellXY[{len(tri_cell_xy)}] = {{
{carr(tri_cell_xy, 32)}
}};
"""
    (ROOT / "include/boardtab.h").write_text(h)
    (ROOT / "src/boardtab.c").write_text(c)

    # ---- cursor sprite: 16x16 ring marker (OBJ 4bpp) ----
    # Emitted as 4 tiles [TL, TR, BL, BR]; the runtime DMAs TL/TR to the OBJ
    # base and BL/BR to base + 0x100 words so a 16x16 sprite at tile 0 works.
    cur = [[0] * 16 for _ in range(16)]
    for y in range(16):
        for x in range(16):
            d2 = (x - 7.5) ** 2 + (y - 7.5) ** 2
            if 27.0 <= d2 <= 56.0:
                cur[y][x] = 1  # white ring
            elif 18.0 <= d2 < 27.0 or 56.0 < d2 <= 68.0:
                cur[y][x] = 2  # grey halo
    cpix = bytearray()
    for ty2, tx2 in ((0, 0), (0, 1), (1, 0), (1, 1)):
        planes = bytearray(32)
        for y in range(8):
            b0 = b1 = 0
            for x in range(8):
                c = cur[ty2 * 8 + y][tx2 * 8 + x]
                bit = 0x80 >> x
                if c & 1:
                    b0 |= bit
                if c & 2:
                    b1 |= bit
            planes[y * 2] = b0
            planes[y * 2 + 1] = b1
        cpix += planes
    (ROOT / "res/cursor.pic").write_bytes(cpix)
    cpal = bytearray(32)
    for i, (r, g, b) in enumerate([(0, 0, 0), (255, 255, 255), (140, 140, 160)]):
        w = (b >> 3) << 10 | (g >> 3) << 5 | (r >> 3)
        cpal[i * 2] = w & 0xFF
        cpal[i * 2 + 1] = w >> 8
    (ROOT / "res/cursor.pal").write_bytes(cpal)

    # ---- preview PNG (random colors, 3x) ----
    from PIL import Image

    rng = random.Random(7)
    tcol = [rng.randrange(6) for _ in tris]
    img = Image.new("RGB", (BW, BH), (0, 0, 0))
    p = img.load()
    for y in range(BH):
        for x in range(BW):
            o = owner[y][x]
            if o < 0:
                continue
            c = tcol[o]
            p[x, y] = pal_rgb[7 + c] if line[y][x] else pal_rgb[1 + c]
    img.resize((BW * 3, BH * 3), Image.NEAREST).save(ROOT / "res/preview.png")

    dual = sum(1 for o in struct_owners if o == 2)
    print(f"structs: {n_struct} ({dual} dual-owner), entries: {len(entry_table)}, "
          f"unique VRAM tiles: {len(tiles)} ({len(tiles) * 32} bytes)")


if __name__ == "__main__":
    main()
