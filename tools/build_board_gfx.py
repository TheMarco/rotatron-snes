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


def tri_corners(col, row):
    """Triangle corner points in image (margin-shifted) pixel coords."""
    x0 = col * 16 + MARGIN
    y0 = row * HPX + MARGIN
    if (col + row) % 2 == 0:  # up: apex on top
        return [(x0 + 16.0, y0), (x0, y0 + HPX), (x0 + 32.0, y0 + HPX)]
    return [(x0, y0), (x0 + 32.0, y0), (x0 + 16.0, y0 + HPX)]


def edge_depth(p, corners):
    """Distance from interior point p to the nearest triangle edge."""
    best = 1e9
    for i in range(3):
        ax, ay = corners[i]
        bx, by = corners[(i + 1) % 3]
        nx, ny = by - ay, ax - bx
        d = abs((p[0] - ax) * nx + (p[1] - ay) * ny) / math.hypot(nx, ny)
        best = min(best, d)
    return best


# Phase-1 "PCB chip" inner design: a chip block at the centroid, an altitude
# trace running toward the base, and two solder pads flanking it. Everything
# is axis-aligned and hugs the triangle's center column, away from the
# diagonal edges (detail is dropped on dual-owner tiles, so patterns that
# cross diagonals would fragment).
DETAIL_ENABLED = False  # the chip motif read as clutter in playtests


def detail_at(x, y, col, row):
    up = (col + row) % 2 == 0
    if not DETAIL_ENABLED:
        return False
    ax = col * 16 + MARGIN + 16.0          # altitude x
    yb = row * HPX + MARGIN + (HPX if up else 0)  # base y
    px, py = x + 0.5, y + 0.5
    h = (yb - py) if up else (py - yb)     # height above the base, 0..24
    u = abs(px - ax)
    if abs(h - 9.0) <= 2.0 and u <= 2.5:   # chip block (5x4)
        return True
    if 3.0 <= h <= 7.0 and u <= 0.8:       # trace from chip to the base pads
        return True
    return abs(h - 4.5) <= 1.5 and 5.0 <= u <= 7.5  # two pads


def compute_layers(tris, tri_of, verts):
    """owner / line / axis / detail pixel grids, shared with the verifier."""
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

    det = [[False] * IW for _ in range(IH)]
    for y in range(IH):
        for x in range(IW):
            o = owner[y][x]
            if o >= 0 and not line[y][x] and not axis[y][x]:
                det[y][x] = detail_at(x, y, tris[o][0], tris[o][1])
    return owner, line, axis, det


def tile_owners(owner, tx, ty):
    """Distinct triangle owners inside the 8x8 tile, in raster order."""
    out = []
    for py in range(8):
        for px in range(8):
            o = owner[ty * 8 + py][tx * 8 + px]
            if o >= 0 and o not in out:
                out.append(o)
    return out


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

    owner, line, axis, det = compute_layers(tris, tri_of, verts)

    # ---- tile structures ----
    # roles: 0 outside, 1 fillA, 2 lineA, 3 fillB, 4 lineB, 5 axis core,
    # 6 halo, 7 detail (dim design pixel; single-owner tiles only).
    # Single-owner tiles become COLOR-AGNOSTIC graphics rendered through
    # per-color sub-palettes 1..6 (slot 1 fill / 2 line / 3 dim / 4 halo /
    # 5 white / 6 glow); only dual tiles bake color pairs in sub-palette 0.
    structs = {}
    struct_owners = []
    cell_struct = [[0xFF] * TW for _ in range(TH)]
    cell_tri = [[(0xFF, 0xFF)] * TW for _ in range(TH)]
    tri_cells = [[] for _ in tris]

    for ty in range(TH):
        for tx in range(TW):
            owners = tile_owners(owner, tx, ty)
            single = len(owners) <= 1
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
                    ab = owners.index(o)
                    if single and det[iy][ix]:
                        roles[py * 8 + px] = 7
                    else:
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

    # Per-color sub-palettes 1..6 for single-owner tiles:
    # [1] fill, [2] neon line, [3] dim detail tint, [4] pin halo, [5] white
    # (axis core + flash), [6] glow (CGRAM-animated, mirrored by glowSet).
    DETAIL_MIX = 0.42
    def dim(name, neon_rgb):
        f = boost(DARK[name])
        return tuple(int(f[i] + (neon_rgb[i] - f[i]) * DETAIL_MIX) for i in range(3))

    color_pals = []
    for name, neon_rgb in NEON:
        cp = [(0, 0, 0), boost(DARK[name]), neon_rgb, dim(name, neon_rgb),
              GREY, WHITE, (0, 0, 0)] + [(0, 0, 0)] * 9
        color_pals.append(cp)
    S_FILL, S_LINE, S_DIM, S_HALO, S_WHITE, S_GLOW = 1, 2, 3, 4, 5, 6

    # ---- tiles: dedup with flips against canonical tiles only ----
    tiles = [bytes(64)]
    canonical = {bytes(64): 0}
    tile_lookup = {bytes(64): (0, 0)}

    # Display-color domain is 9: 0..5 real colors, 6 = solid white (refill
    # pop), 7 = hidden, 8 = glow (a CGRAM entry the clear animation ramps
    # every frame: white-hot -> neon -> black).
    GLOW_IDX = 15  # sub-palette 0 slot for dual tiles
    def cpx(c, is_line):
        if c == 6:
            return AXIS_CORE_IDX  # white
        if c == 7:
            return 0
        if c == 8:
            return GLOW_IDX
        return (7 + c) if is_line else (1 + c)

    # Dual tiles: sub-palette 0, colors baked per combo (as before).
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

    # Single-owner tiles: color-agnostic indices into a per-color sub-palette.
    # disp: 0 = normal (fill/line/dim), 1 = white, 2 = hidden, 3 = glow.
    def graphic_single(key, disp):
        body = {0: {1: S_FILL, 2: S_LINE, 7: S_DIM},
                1: {1: S_WHITE, 2: S_WHITE, 7: S_WHITE},
                2: {1: 0, 2: 0, 7: 0},
                3: {1: S_GLOW, 2: S_GLOW, 7: S_GLOW}}[disp]
        px = bytearray(64)
        for i, r in enumerate(key):
            if r == 5:
                px[i] = S_WHITE
            elif r == 6:
                px[i] = S_HALO
            elif r in body:
                px[i] = body[r]
        return bytes(px)

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

    # Entry layout per struct:
    #   owners 0: 1 entry  (axis pins, sub-pal 1 baked)
    #   owners 1: 4 entries [normal (runtime ORs (1+color)<<10), white,
    #                        hidden, glow] - variants bake sub-pal 1
    #   owners 2: 81 entries (9x9 display-color combos, sub-pal 0)
    PAL1_BITS = 0x0400
    struct_base = []
    entry_table = []
    for sid in range(n_struct):
        key = struct_keys[sid]
        struct_base.append(len(entry_table))
        nown = struct_owners[sid]
        PRIO = 0x2000  # board above OBJ prio 2 (ambient sky sprites)
        if nown == 0:
            tid, flips = tile_for(graphic_single(key, 2))
            entry_table.append(tid | flips | PAL1_BITS | PRIO)
        elif nown == 1:
            tid, flips = tile_for(graphic_single(key, 0))
            entry_table.append(tid | flips | PRIO)  # palette added at runtime
            for disp in (1, 2, 3):
                tid, flips = tile_for(graphic_single(key, disp))
                entry_table.append(tid | flips | PAL1_BITS | PRIO)
        else:
            for ca in range(9):
                for cb in range(9):
                    tid, flips = tile_for(graphic(key, ca, cb))
                    assert tid < 1024, "BG tile index overflow"
                    entry_table.append(tid | flips | PRIO)

    # Spin-time variants: axis-only blanking + dual half tiles (sub-pal 0).
    half_base = []
    half_table = []
    axis_entry = []
    for sid in range(n_struct):
        key = struct_keys[sid]
        if struct_owners[sid] == 2:
            tid, flips = tile_for(graphic(key, 0, 0, keep=()))
            axis_entry.append(tid | flips | 0x2000)
            half_base.append(len(half_table))
            for keep in ((1, 2), (3, 4)):  # A-only x6, then B-only x6
                for c in range(6):
                    tid, flips = tile_for(graphic(key, c, c, keep=keep))
                    half_table.append(tid | flips | 0x2000)
        else:
            tid, flips = tile_for(graphic_single(key, 2))
            axis_entry.append(tid | flips | PAL1_BITS | 0x2000)
            half_base.append(0xFFFF)

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
    all_pals = [pal_rgb] + color_pals  # sub-pal 0 + per-color 1..6, 224 bytes
    for cp in all_pals:
        for rgb in cp:
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

    # ---- seam spark sprite (2 frames, 16x16, cursor palette) ----
    # Frame A: white electric cross with grey halo; frame B: dim ember.
    # Emitted as [A-TL, A-TR, B-TL, B-TR, A-BL, A-BR, B-BL, B-BR] so the
    # runtime DMAs rows 30 and 31 of the OBJ name table in two strips.
    def spark_img(bright):
        img = [[0] * 16 for _ in range(16)]
        for y in range(16):
            for x in range(16):
                dx, dy = abs(x - 7), abs(y - 7)
                if bright:
                    if dx <= 1 and dy <= 1:
                        img[y][x] = 1
                    elif (dx <= 3 and dy == 0) or (dx == 0 and dy <= 3):
                        img[y][x] = 1
                    elif dx <= 1 and dy <= 2 or dx <= 2 and dy <= 1:
                        img[y][x] = 2
                else:
                    if dx <= 1 and dy <= 1:
                        img[y][x] = 2
                    if dx == 0 and dy == 0:
                        img[y][x] = 1
        return img

    simgs = [spark_img(True), spark_img(False)]
    spark = bytearray()
    for trow in (0, 1):
        for f in (0, 1):
            for tcol in (0, 1):
                img = simgs[f]
                spark += encode_tile_4bpp(
                    lambda x, y: img[trow * 8 + y][tcol * 8 + x])
    (ROOT / "res/spark.pic").write_bytes(spark)

    # ---- ambient sky sprites: ship + shooting star (16x16 each) ----
    ship = [[0] * 16 for _ in range(16)]
    hull = [(3, 13, 8)]
    for x in range(3, 14):
        ship[8][x] = 3
    for x in range(5, 12):
        ship[7][x] = 3
        ship[9][x] = 3
    for x in range(7, 10):
        ship[6][x] = 3
        ship[10][x] = 3
    ship[7][10] = ship[7][11] = 1     # canopy
    ship[8][13] = ship[8][14] = 2     # nose tip
    ship[8][1] = ship[8][2] = 4       # engine glow
    ship[7][2] = ship[9][2] = 4

    star = [[0] * 16 for _ in range(16)]
    for i in range(2, 12):
        star[i][i] = 2                # tail
    for dy in (0, 1):
        for dx in (0, 1):
            star[12 + dy][12 + dx] = 1  # bright head
    star[11][12] = star[12][11] = star[13][14] = star[14][13] = 2

    amb = bytearray()
    for trow in (0, 1):
        for img in (ship, star):
            for tcol in (0, 1):
                amb += encode_tile_4bpp(
                    lambda x, y, im=img: im[trow * 8 + y][tcol * 8 + x])
    (ROOT / "res/ambient.pic").write_bytes(amb)

    # ---- cursor sprite ----
    # Deliberately oval: 2px shorter than wide so the axle pin reads centered
    # inside it on screen (user-tuned; don't "fix" back to a circle).
    cur = [[0] * 16 for _ in range(16)]
    for y in range(16):
        for x in range(16):
            d2 = (x - 7.5) ** 2 + ((y - 7.5) * (7.5 / 6.0)) ** 2
            if 27.0 <= d2 <= 56.0:
                cur[y][x] = 1
            elif 18.0 <= d2 < 27.0 or 56.0 < d2 <= 68.0:
                cur[y][x] = 2
    cpix = bytearray()
    for ty2, tx2 in ((0, 0), (0, 1), (1, 0), (1, 1)):
        cpix += encode_tile_4bpp(lambda x, y: cur[ty2 * 8 + y][tx2 * 8 + x])
    (ROOT / "res/cursor.pic").write_bytes(cpix)
    cpal = bytearray(32)
    for i, rgb in enumerate([(0, 0, 0), WHITE, (140, 140, 160),
                             (96, 100, 116), (80, 220, 255)]):
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
#define DISP_GLOW 8    /* display-color: CGRAM-animated glow */
#define GLOW_CGRAM 15            /* glow slot in sub-palette 0 (dual tiles) */
#define GLOW_CGRAM_C(c) (16 * ((c) + 1) + 6) /* glow slot in per-color pals */

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
    single_grid = [[len(tile_owners(owner, tx, ty)) <= 1 for tx in range(TW)]
                   for ty in range(TH)]
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
            if line[y][x]:
                p[x, y] = pal_rgb[7 + c]
            elif det[y][x] and single_grid[y // 8][x // 8]:
                p[x, y] = color_pals[c][S_DIM]
            else:
                p[x, y] = pal_rgb[1 + c]
    img.resize((IW * 3, IH * 3), Image.NEAREST).save(ROOT / "res/preview.png")

    dual = sum(1 for o in struct_owners if o == 2)
    zero = sum(1 for o in struct_owners if o == 0)
    print(f"structs: {n_struct} ({dual} dual, {zero} axis-only), entries: {len(entry_table)}, "
          f"unique VRAM tiles: {len(tiles)} ({len(tiles) * 32} bytes)")
    assert len(tiles) <= 192, 'BG1 tiles would collide with the HUD font at word 0x1C00'


if __name__ == "__main__":
    main()
