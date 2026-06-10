#!/usr/bin/env python3
"""Generate the Rotatron board tileset, palette, spin frames, and C tables.

Geometry: triangle edge S=32px, row height H=24px (a ~13% vertical squash of
the true sqrt(3)/2 so everything lands on the 8px tile grid). The 54-triangle
hex board renders as 192x168px, expanded by an 8px margin on every side
(208x184 = 26x23 tiles) so the white axis pins on rim vertices don't clip.

Layers, in priority order per pixel:
  axis pins (white core + grey halo) at every valid vertex
  1px triangle outlines (8-neighbor owner difference -> 2px neon seams)
  triangle fills
Every 8x8 tile reduces to a "structure" (per-pixel role: outside/fillA/lineA/
fillB/lineB/axis-core/axis-halo) with 0, 1, or 2 owner triangles (asserted).
Tiles are emitted per (structure x color combo), deduplicated across H/V
flips. Runtime recolor: entry = entryTable[structBase[s] + cA*6+cB | cA | 0].

Variants for the spin animation: "half" tiles (one owner side blanked, axis
kept) and "axis-only" tiles, so blanking the rotating ring never erases pins
or notches the neighbors.

Spin frames: 6 frames of the cluster rotating 0..50deg in true (unsquashed)
space, 64x64 per frame as 4x 32x32 sprites, sector i = palette index 1+i
(fill) / 9+i (line); index 15/8 = static axis pins. The runtime colorizes via
OBJ palette 1. CCW playback = H-flip + palette permutation.

Outputs: res/board.pic, res/board.pal, res/spin.pic, res/cursor.pic,
res/cursor.pal, src/boardtab.c, include/boardtab.h, res/preview.png.
"""

import math
import random
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

SPX = 32
HPX = 24
BW, BH = 192, 168          # board proper
MARGIN = 8
IW, IH = BW + 2 * MARGIN, BH + 2 * MARGIN  # generated region (208x184)
TW, TH = IW // 8, IH // 8  # 26 x 23 tiles
ROWS, COLS = 7, 12

AXIS_CORE_R2 = 5.0         # white pin core radius^2 (px)
AXIS_HALO_R2 = 10.5        # grey halo radius^2

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

WHITE = (255, 255, 255)
GREY = (150, 150, 165)

N_SPIN_FRAMES = 6
SPIN_TICKS = 20

RING_D = [(-1, -1), (0, -1), (0, 0), (-1, 0), (-2, 0), (-2, -1)]


def cell_in_hex(col, row):
    sj = 3 * row + (2 if (col + row) % 2 == 0 else 1)
    a = abs(sj - 9)
    b = abs(col - 5)
    return a <= 9 and 3 * b + a <= 18


def build_triangles():
    tris = []
    tri_of = {}
    for row in range(ROWS):
        for col in range(COLS):
            if cell_in_hex(col, row):
                tri_of[(col, row)] = len(tris)
                tris.append((col, row))
    return tris, tri_of


def owner_at(x, y, tri_of):
    """Triangle owning BOARD pixel center (x+.5, y+.5), or -1 (exact ints)."""
    row = y // HPX
    if y < 0 or row < 0 or row >= ROWS:
        return -1
    xc6 = 6 * x + 3
    yc6 = 6 * (y - row * HPX) + 3
    for col in range(max(0, x // 16 - 1), min(COLS - 1, x // 16) + 1):
        if (col, row) not in tri_of:
            continue
        apex6 = (col + 1) * 16 * 6
        d = 3 * abs(xc6 - apex6)
        if (col + row) % 2 == 0:
            inside = d <= 2 * yc6
        else:
            inside = d <= 2 * (144 - yc6)
        if inside:
            return tri_of[(col, row)]
    return -1


def valid_vertices(tri_of):
    """(k, j) of every spinnable vertex: real parity + >=1 on-board ring slot."""
    out = []
    for j in range(ROWS + 1):
        for k in range(COLS + 1):
            if (k + j) % 2 != 1:
                continue
            if any((k + dc, j + dr) in tri_of for dc, dr in RING_D):
                out.append((k, j))
    return out


def axis_at(ix, iy, verts):
    """0/none, 1/halo, 2/core for IMAGE pixel center vs vertex pins."""
    px, py = ix + 0.5 - MARGIN, iy + 0.5 - MARGIN
    best = 0
    for (k, j) in verts:
        d2 = (px - k * 16.0) ** 2 + (py - j * 24.0) ** 2
        if d2 <= AXIS_CORE_R2:
            return 2
        if d2 <= AXIS_HALO_R2:
            best = 1
    return best


def encode_tile_4bpp(getpx):
    planes = bytearray(32)
    for y in range(8):
        b0 = b1 = b2 = b3 = 0
        for x in range(8):
            c = getpx(x, y)
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
    return planes


def main():
    tris, tri_of = build_triangles()
    assert len(tris) == 54, len(tris)
    verts = valid_vertices(tri_of)
    assert len(verts) == 37, len(verts)

    owner = [[owner_at(x - MARGIN, y - MARGIN, tri_of) for x in range(IW)] for y in range(IH)]

    def own(x, y):
        if 0 <= x < IW and 0 <= y < IH:
            return owner[y][x]
        return -1

    line = [[False] * IW for _ in range(IH)]
    for y in range(IH):
        for x in range(IW):
            o = owner[y][x]
            if o < 0:
                continue
            line[y][x] = any(own(x + dx, y + dy) != o
                             for dy in (-1, 0, 1) for dx in (-1, 0, 1))

    axis = [[axis_at(x, y, verts) for x in range(IW)] for y in range(IH)]

    # ---- tile structures ----
    # roles: 0 outside, 1 fillA, 2 lineA, 3 fillB, 4 lineB, 5 axis core, 6 halo
    structs = {}
    struct_owners = []
    cell_struct = [[0xFF] * TW for _ in range(TH)]
    cell_tri = [[(0xFF, 0xFF)] * TW for _ in range(TH)]
    tri_cells = [[] for _ in tris]

    for ty in range(TH):
        for tx in range(TW):
            owners = []
            roles = bytearray(64)
            nonempty = False
            for py in range(8):
                for px in range(8):
                    ix, iy = tx * 8 + px, ty * 8 + py
                    a = axis[iy][ix]
                    o = owner[iy][ix]
                    if a:
                        roles[py * 8 + px] = 5 if a == 2 else 6  # core, halo
                        nonempty = True
                        continue
                    if o < 0:
                        continue
                    if o not in owners:
                        owners.append(o)
                    ab = owners.index(o)
                    roles[py * 8 + px] = 1 + 2 * ab + (1 if line[iy][ix] else 0)
                    nonempty = True
            if not nonempty:
                continue
            assert len(owners) <= 2, f"tile {tx},{ty} has {len(owners)} owners"
            key = bytes(roles)
            if key not in structs:
                structs[key] = len(struct_owners)
                struct_owners.append(len(owners))
            sid = structs[key]
            cell_struct[ty][tx] = sid
            a = owners[0] if owners else 0xFF
            b = owners[1] if len(owners) > 1 else 0xFF
            cell_tri[ty][tx] = (a, b)
            for o in owners:
                tri_cells[o].append((tx, ty))

    struct_keys = [None] * len(struct_owners)
    for k, sid in structs.items():
        struct_keys[sid] = k
    n_struct = len(struct_owners)

    # ---- palette ----
    # 0 transp, 1..6 dark fills, 7..12 neon lines, 13 grey halo, 14 white, 15 spare
    def boost(c):
        return tuple(min(255, int(v * FILL_BOOST)) for v in c)

    def bgr555(rgb):
        r, g, b = rgb
        return (b >> 3) << 10 | (g >> 3) << 5 | (r >> 3)

    pal_rgb = [(0, 0, 0)]
    pal_rgb += [boost(DARK[name]) for name, _ in NEON]
    pal_rgb += [rgb for _, rgb in NEON]
    pal_rgb += [GREY, WHITE, (0, 0, 0)]
    AXIS_CORE_IDX, AXIS_HALO_IDX = 14, 13

    # ---- tiles: dedup with flips against canonical tiles only ----
    tiles = [bytes(64)]
    canonical = {bytes(64): 0}
    tile_lookup = {bytes(64): (0, 0)}

    # Display-color domain is 9: 0..5 real colors, 6 = solid white (refill
    # pop), 7 = hidden, 8 = glow (solid palette index 15, whose CGRAM entry
    # the clear animation ramps every frame: white-hot -> neon -> black).
    GLOW_IDX = 15
    def cpx(c, is_line):
        if c == 6:
            return AXIS_CORE_IDX  # white
        if c == 7:
            return 0
        if c == 8:
            return GLOW_IDX
        return (7 + c) if is_line else (1 + c)

    def role_to_px(r, ca, cb, keep=(1, 2, 3, 4)):
        if r == 5:
            return AXIS_CORE_IDX
        if r == 6:
            return AXIS_HALO_IDX
        if r in keep:
            if r == 1:
                return cpx(ca, False)
            if r == 2:
                return cpx(ca, True)
            if r == 3:
                return cpx(cb, False)
            if r == 4:
                return cpx(cb, True)
        return 0

    def graphic(key, ca, cb, keep=(1, 2, 3, 4)):
        return bytes(role_to_px(r, ca, cb, keep) for r in key)

    def hflip(p):
        return bytes(p[y * 8 + (7 - x)] for y in range(8) for x in range(8))

    def vflip(p):
        return bytes(p[(7 - y) * 8 + x] for y in range(8) for x in range(8))

    def tile_for(p):
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
        nown = struct_owners[sid]
        if nown == 0:
            combos = [(0, 0)]
        elif nown == 1:
            combos = [(ca, 0) for ca in range(9)]
        else:
            combos = [(ca, cb) for ca in range(9) for cb in range(9)]
        for ca, cb in combos:
            tid, flips = tile_for(graphic(struct_keys[sid], ca, cb))
            assert tid < 1024, "BG tile index overflow"
            entry_table.append(tid | flips)

    # Spin-time variants: keep one owner side (+axis), or axis only.
    half_base = []
    half_table = []
    axis_entry = []
    for sid in range(n_struct):
        key = struct_keys[sid]
        tid, flips = tile_for(graphic(key, 0, 0, keep=()))
        axis_entry.append(tid | flips)
        if struct_owners[sid] != 2:
            half_base.append(0xFFFF)
            continue
        half_base.append(len(half_table))
        for keep in ((1, 2), (3, 4)):  # A-only x6, then B-only x6
            for c in range(6):
                tid, flips = tile_for(graphic(key, c, c, keep=keep))
                half_table.append(tid | flips)

    # ---- spin animation frames ----
    SQ = HPX / (16.0 * math.sqrt(3.0))

    def sector_at(x, y):
        for i, (dc, dr) in enumerate(RING_D):
            x0, y0 = dc * 16, dr * HPX
            if not (x0 <= x < x0 + 32 and y0 <= y < y0 + HPX):
                continue
            lx, ly = x - x0, y - y0
            if (dc + dr) % 2 == 0:
                hw = (HPX - ly) * (16.0 / HPX)
            else:
                hw = ly * (16.0 / HPX)
            if abs(lx - 16) <= hw:
                return i
        return -1

    def frame_sector(px, py, theta):
        x = px + 0.5 - 32
        y = py + 0.5 - 32
        c, s = math.cos(-theta), math.sin(-theta)
        yt = y / SQ
        return sector_at(x * c - yt * s, (x * s + yt * c) * SQ)

    # Static pins drawn on top: cluster center + its 6 outer corners.
    PIN_PTS = [(32.0, 32.0), (0.0, 32.0), (64.0, 32.0),
               (16.0, 8.0), (48.0, 8.0), (16.0, 56.0), (48.0, 56.0)]
    SPIN_CORE_IDX, SPIN_HALO_IDX = 15, 8

    def spin_axis(px, py):
        x, y = px + 0.5, py + 0.5
        best = 0
        for (vx, vy) in PIN_PTS:
            d2 = (x - vx) ** 2 + (y - vy) ** 2
            if d2 <= AXIS_CORE_R2:
                return 2
            if d2 <= AXIS_HALO_R2:
                best = 1
        return best

    frames = []
    for f in range(N_SPIN_FRAMES):
        theta = math.radians(60.0 * f / N_SPIN_FRAMES)
        sec = [[frame_sector(px, py, theta) for px in range(64)] for py in range(64)]
        img = [[0] * 64 for _ in range(64)]
        for py in range(64):
            for px in range(64):
                a = spin_axis(px, py)
                if a:
                    img[py][px] = SPIN_CORE_IDX if a == 2 else SPIN_HALO_IDX
                    continue
                o = sec[py][px]
                if o < 0:
                    continue
                edge = any(
                    not (0 <= px + dx < 64 and 0 <= py + dy < 64)
                    or sec[py + dy][px + dx] != o
                    for dy in (-1, 0, 1) for dx in (-1, 0, 1))
                img[py][px] = (9 + o) if edge else (1 + o)
        frames.append(img)

    spin_sheet = bytearray()
    for trow in range(24):
        for tcol in range(16):
            f = (trow // 8) * 2 + tcol // 8
            img = frames[f]
            tx, ty = tcol % 8, trow % 8
            spin_sheet += encode_tile_4bpp(lambda x, y: img[ty * 8 + y][tx * 8 + x])
    (ROOT / "res/spin.pic").write_bytes(spin_sheet)

    def ease(t):
        return 4 * t * t * t if t < 0.5 else 1 - ((-2 * t + 2) ** 3) / 2

    # Nearest-frame schedule, truncated the moment it would reach 60deg: the
    # ease-out tail would otherwise hold the 50deg frame for ~7 ticks before
    # the final swap (reads as a stall). The board recolor IS the last frame.
    sched = []
    for t in range(SPIN_TICKS):
        f = round(ease((t + 1) / SPIN_TICKS) * N_SPIN_FRAMES)
        if f >= N_SPIN_FRAMES:
            break
        sched.append(f)

    # ---- emit board.pic / board.pal ----
    pic = bytearray()
    for t in tiles:
        pic += encode_tile_4bpp(lambda x, y, t=t: t[y * 8 + x])
    (ROOT / "res/board.pic").write_bytes(pic)

    pal = bytearray()
    for rgb in pal_rgb:
        w = bgr555(rgb)
        pal += bytes((w & 0xFF, w >> 8))
    (ROOT / "res/board.pal").write_bytes(pal)

    # ---- hex-clear shockwave (OBJ 4bpp, 4 frames 32x32, cursor palette) ----
    # Expanding ring + six radial spark spokes aligned to the triangle seams,
    # rotating slightly per frame; white core dissipating to grey. Emitted in
    # the rows-26..29 band layout (frame f at tile cols f*4..f*4+3).
    PULSE_FRAMES = [(4.5, 2.6, 1), (8.5, 2.2, 1), (12.0, 1.8, 2), (15.0, 1.4, 2)]
    SEAM_ANGLES = [0.0, 56.3, 123.7, 180.0, 236.3, 303.7]  # E + the 4 diagonals + W
    pimgs = []
    for f, (rad, wid, idx) in enumerate(PULSE_FRAMES):
        img = [[0] * 32 for _ in range(32)]
        spoke_r0, spoke_r1 = rad - 1.0, min(15.4, rad + 3.5)
        rot = math.radians(8.0 * f)
        spokes = [math.radians(a) + rot for a in SEAM_ANGLES]
        for y in range(32):
            for x in range(32):
                dx, dy = x + 0.5 - 16, y + 0.5 - 16
                d = math.hypot(dx, dy)
                if abs(d - rad) <= wid / 2:
                    img[y][x] = idx
                elif abs(d - rad) <= wid / 2 + 1.0:
                    img[y][x] = 2
                elif spoke_r0 <= d <= spoke_r1:
                    ang = math.atan2(dy, dx)
                    for sa in spokes:
                        delta = abs((ang - sa + math.pi) % (2 * math.pi) - math.pi)
                        if delta * d <= 0.9:  # ~1.8px wide spark
                            img[y][x] = idx if d <= rad + 1.5 else 2
                            break
        pimgs.append(img)
    pulse = bytearray()
    for trow in range(4):
        for tcol in range(16):
            f, tx = tcol // 4, tcol % 4
            img = pimgs[f]
            pulse += encode_tile_4bpp(lambda x, y: img[trow * 8 + y][tx * 8 + x])
    (ROOT / "res/pulse.pic").write_bytes(pulse)

    # ---- cursor sprite ----
    cur = [[0] * 16 for _ in range(16)]
    for y in range(16):
        for x in range(16):
            d2 = (x - 7.5) ** 2 + (y - 7.5) ** 2
            if 27.0 <= d2 <= 56.0:
                cur[y][x] = 1
            elif 18.0 <= d2 < 27.0 or 56.0 < d2 <= 68.0:
                cur[y][x] = 2
    cpix = bytearray()
    for ty2, tx2 in ((0, 0), (0, 1), (1, 0), (1, 1)):
        cpix += encode_tile_4bpp(lambda x, y: cur[ty2 * 8 + y][tx2 * 8 + x])
    (ROOT / "res/cursor.pic").write_bytes(cpix)
    cpal = bytearray(32)
    for i, rgb in enumerate([(0, 0, 0), WHITE, (140, 140, 160)]):
        w = bgr555(rgb)
        cpal[i * 2] = w & 0xFF
        cpal[i * 2 + 1] = w >> 8
    (ROOT / "res/cursor.pal").write_bytes(cpal)

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
        return "\n".join("    " + ", ".join(str(v) for v in vals[i:i + per_line]) + ","
                         for i in range(0, len(vals), per_line))

    flat = lambda grid: [v for row in grid for v in row]

    h = f"""/* GENERATED by tools/build_board_gfx.py - do not edit. */
#ifndef BOARDTAB_H
#define BOARDTAB_H
#include "core_types.h"

#define BOARD_TILES_W {TW}
#define BOARD_TILES_H {TH}
#define N_STRUCTS {n_struct}
#define N_BOARD_VRAM_TILES {len(tiles)}
#define N_TRIANGLES 54
#define N_SPIN_FRAMES {N_SPIN_FRAMES}
#define SPIN_TICKS {len(sched)}
#define DISP_WHITE 6   /* display-color: solid white flash */
#define DISP_HIDDEN 7  /* display-color: blacked out (pins stay) */
#define DISP_GLOW 8    /* display-color: CGRAM-animated glow (BG pal idx 15) */
#define GLOW_CGRAM 15  /* the CGRAM entry the glow rides on */

extern const u8 cellStruct[BOARD_TILES_H][BOARD_TILES_W];   /* 0xFF = blank */
extern const u8 cellTriA[BOARD_TILES_H][BOARD_TILES_W];     /* 0xFF = no owner */
extern const u8 cellTriB[BOARD_TILES_H][BOARD_TILES_W];     /* 0xFF = <2 owners */
extern const u8 structOwners[N_STRUCTS];                    /* 0, 1 or 2 */
extern const u16 structBase[N_STRUCTS];
extern const u16 entryTable[{len(entry_table)}];
extern const u8 triCol[N_TRIANGLES];
extern const u8 triRow[N_TRIANGLES];
extern const u8 triOfCell[{ROWS}][{COLS}];
extern const u16 triCellOfs[N_TRIANGLES + 1];
extern const u8 triCellXY[{len(tri_cell_xy)}];

/* Spin-time blanking variants: axisEntry = pins only; halfBase -> 12 entries
 * (A-only x6 colors, then B-only) keeping axis pixels; 0xFFFF if not dual. */
extern const u16 axisEntry[N_STRUCTS];
extern const u16 halfBase[N_STRUCTS];
extern const u16 halfTable[{len(half_table)}];
extern const u8 spinSched[SPIN_TICKS];
extern const u16 fillBGR[6];
extern const u16 lineBGR[6];
#endif
"""
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
{carr([c0 for c0, r0 in tris])}
}};
const u8 triRow[N_TRIANGLES] = {{
{carr([r0 for c0, r0 in tris])}
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
const u16 axisEntry[N_STRUCTS] = {{
{carr(axis_entry, 16)}
}};
const u16 halfBase[N_STRUCTS] = {{
{carr(half_base)}
}};
const u16 halfTable[{len(half_table)}] = {{
{carr(half_table, 16)}
}};
const u8 spinSched[SPIN_TICKS] = {{
{carr(sched)}
}};
const u16 fillBGR[6] = {{
{carr([bgr555(boost(DARK[n])) for n, _ in NEON])}
}};
const u16 lineBGR[6] = {{
{carr([bgr555(rgb) for _, rgb in NEON])}
}};
"""
    (ROOT / "include/boardtab.h").write_text(h)
    (ROOT / "src/boardtab.c").write_text(c)

    # ---- preview ----
    from PIL import Image
    rng = random.Random(7)
    tcol = [rng.randrange(6) for _ in tris]
    img = Image.new("RGB", (IW, IH), (0, 0, 0))
    p = img.load()
    for y in range(IH):
        for x in range(IW):
            a = axis[y][x]
            if a:
                p[x, y] = WHITE if a == 2 else GREY
                continue
            o = owner[y][x]
            if o < 0:
                continue
            c = tcol[o]
            p[x, y] = pal_rgb[7 + c] if line[y][x] else pal_rgb[1 + c]
    img.resize((IW * 3, IH * 3), Image.NEAREST).save(ROOT / "res/preview.png")

    dual = sum(1 for o in struct_owners if o == 2)
    zero = sum(1 for o in struct_owners if o == 0)
    print(f"structs: {n_struct} ({dual} dual, {zero} axis-only), entries: {len(entry_table)}, "
          f"unique VRAM tiles: {len(tiles)} ({len(tiles) * 32} bytes)")
    assert len(tiles) <= 1024


if __name__ == "__main__":
    main()
