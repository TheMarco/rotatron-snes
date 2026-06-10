#include <snes.h>
#include "core.h"
#include "boardtab.h"
#include "bg2tab.h"
#include "render.h"

extern char board_pic, board_picend, board_pal;
extern char cursor_pic, cursor_picend, cursor_pal;
extern char spin_pic, pulse_pic, spark_pic, ambient_pic;
extern char bg2_pic, bg2_picend, bg2_map, bg2_pal;

static u16 mapBuf[32 * 32];
static u8 mapDirty;
static u16 objPalBuf[16];
static u8 objPalDirty;

/* Per-triangle display override (DISP_WHITE/HIDDEN/GLOW), 0xFF = boardColor.
 * Drives the clear flash/blackout/pop without touching game state. */
u8 triDisp[N_TRIANGLES];

static u8 shakeT, shakeAmp;
static u16 glowColor;
static u8 glowDirty;
static u16 lineBuf[6]; /* breathing outline colors, staged for vblank */
static u8 lineDirty;
static u16 twBuf[TWINKLE_N]; /* backdrop twinkle colors, staged for vblank */
static u8 twDirty;

void renderInit(void) {
    u16 i;

    consoleInit();
    setBrightness(0);
    WaitForVBlank();

    oamInit();

    /* BG1: the board. 4bpp tiles + 16-color subpalette 0. */
    dmaCopyVram((u8 *)&board_pic, VRAM_BG1_TILES, (u16)(&board_picend - &board_pic));
    setPalette((u8 *)&board_pal, 0, 112 * 2); /* sub-pal 0 + per-color 1..6 */
    bgSetGfxPtr(0, VRAM_BG1_TILES);
    bgSetMapPtr(0, VRAM_BG1_MAP, SC_32x32);

    /* BG2: full-screen backdrop art behind the board (sub-palette 7). */
    dmaCopyVram((u8 *)&bg2_pic, VRAM_BG2_TILES, (u16)(&bg2_picend - &bg2_pic));
    dmaCopyVram((u8 *)&bg2_map, VRAM_BG2_MAP, 0x800);
    setPalette((u8 *)&bg2_pal, 112, 16 * 2);
    bgSetGfxPtr(1, VRAM_BG2_TILES);
    bgSetMapPtr(1, VRAM_BG2_MAP, SC_32x32);

    /* OBJ tiles. Spin frames fill tiles 0..383: tiles 0..255 live in the
     * first name table (word 0x6000), 256+ in the second (base + 8KB ->
     * word 0x7000). Cursor 16x16 at tile 384 (TL/TR) + 400 (BL/BR). */
    dmaCopyVram((u8 *)&spin_pic, VRAM_OBJ_TILES, 8192);
    dmaCopyVram((u8 *)&spin_pic + 8192, (u16)(VRAM_OBJ_TILES + 0x1000), 4096);
    dmaCopyVram((u8 *)&cursor_pic, (u16)(VRAM_OBJ_TILES + 0x1000 + 128 * 16), 64);
    dmaCopyVram((u8 *)&cursor_pic + 64, (u16)(VRAM_OBJ_TILES + 0x1000 + 144 * 16), 64);
    /* pulse rings: 4-row band right after the cursor rows (tiles 416..479) */
    dmaCopyVram((u8 *)&pulse_pic, (u16)(VRAM_OBJ_TILES + 0x1000 + 160 * 16), 2048);
    /* spark frames: rows 30/31 (tiles 480/482 + their BL/BR row) */
    dmaCopyVram((u8 *)&spark_pic, (u16)(VRAM_OBJ_TILES + 0x1000 + 224 * 16), 128);
    dmaCopyVram((u8 *)&spark_pic + 128, (u16)(VRAM_OBJ_TILES + 0x1000 + 240 * 16), 128);
    /* ambient ship/star: rows 24/25 cols 2..5 (tiles 386 ship, 388 star) */
    dmaCopyVram((u8 *)&ambient_pic, (u16)(VRAM_OBJ_TILES + 0x1000 + 130 * 16), 128);
    dmaCopyVram((u8 *)&ambient_pic + 128, (u16)(VRAM_OBJ_TILES + 0x1000 + 146 * 16), 128);
    setPalette((u8 *)&cursor_pal, 128, 16 * 2);
    REG_OBSEL = OBJ_SIZE16_L32 | (VRAM_OBJ_TILES >> 13);

    for (i = 0; i < 32 * 32; i++) mapBuf[i] = 0;
    for (i = 0; i < N_TRIANGLES; i++) triDisp[i] = 0xFF;
    mapDirty = 1;

    setMode(BG_MODE1, 0);
    videoMode = 0x13; /* BG1 + BG2 + OBJ */
    REG_TM = 0x13;

    setScreenOn();
}

static u16 cellEntry(u8 tx, u8 ty) {
    u8 sid = cellStruct[ty][tx];
    u8 a, b, ca, cb;
    if (sid == 0xFF) return 0;
    if (structOwners[sid] == 0) /* axis pins over blank space */
        return entryTable[structBase[sid]];
    a = cellTriA[ty][tx];
    ca = (triDisp[a] != 0xFF) ? triDisp[a] : boardColor[triRow[a]][triCol[a]];
    if (structOwners[sid] == 2) {
        b = cellTriB[ty][tx];
        cb = (triDisp[b] != 0xFF) ? triDisp[b] : boardColor[triRow[b]][triCol[b]];
        return entryTable[structBase[sid] + (u16)ca * 9 + cb];
    }
    /* single-owner: color-agnostic tile + per-color sub-palette 1..6;
     * white/hidden/glow variants follow at +1..+3 */
    if (ca < 6) return entryTable[structBase[sid]] | ((u16)(1 + ca) << 10);
    return entryTable[structBase[sid] + 1 + (ca - 6)];
}

void boardRebuildMap(void) {
    u8 tx, ty;
    u16 *row;
    for (ty = 0; ty < BOARD_TILES_H; ty++) {
        row = &mapBuf[(u16)ty * 32 + BOARD_TILE_X];
        for (tx = 0; tx < BOARD_TILES_W; tx++) row[tx] = cellEntry(tx, ty);
    }
    mapDirty = 1;
}

/* Incremental recolor: a full rebuild costs several frames of 65816 time
 * (the visible 'pause' at the end of a spin); refreshing only the cells a
 * triangle owns keeps every transition within one frame. */
void triRefresh(u8 t) {
    u16 o;
    for (o = triCellOfs[t]; o < triCellOfs[t + 1]; o++) {
        u8 tx = triCellXY[o * 2];
        u8 ty = triCellXY[o * 2 + 1];
        mapBuf[(u16)ty * 32 + BOARD_TILE_X + tx] = cellEntry(tx, ty);
    }
    mapDirty = 1;
}

void ringRefresh(u8 k, u8 j) {
    u8 i;
    for (i = 0; i < 6; i++) {
        s8 c = (s8)k + RING_DC[i];
        s8 r = (s8)j + RING_DR[i];
        if (c >= 0 && c < BOARD_COLS && r >= 0 && r < BOARD_ROWS && boardColor[r][c] != NO_CELL)
            triRefresh(triOfCell[r][c]);
    }
}

void renderVBlank(void) {
    /* Re-assert scroll every frame: setMode() resets BG offsets, and this
     * also survives any future mode/screen transitions. Shake rides on top. */
    if (shakeT) {
        s16 sx = (shakeT & 1) ? shakeAmp : -(s16)shakeAmp;
        s16 sy = (shakeT & 2) ? shakeAmp : -(s16)shakeAmp;
        if (shakeT < 4) sy = 0; /* settle horizontally first */
        bgSetScroll(0, (u16)sx, (u16)(BOARD_VOFS + sy));
        shakeT--;
    } else {
        bgSetScroll(0, 0, BOARD_VOFS);
    }
    bgSetScroll(1, 0, 0x3FF); /* backdrop pinned at screen origin (-1 quirk) */
    if (mapDirty) {
        dmaCopyVram((u8 *)mapBuf, VRAM_BG1_MAP, 0x800);
        mapDirty = 0;
    }
    if (objPalDirty) {
        dmaCopyCGram((u8 *)objPalBuf, 144, 32); /* OBJ palette 1 */
        objPalDirty = 0;
    }
    if (glowDirty) {
        u8 c;
        dmaCopyCGram((u8 *)&glowColor, GLOW_CGRAM, 2);
        for (c = 0; c < 6; c++) /* per-color sub-palette mirrors */
            dmaCopyCGram((u8 *)&glowColor, GLOW_CGRAM_C(c), 2);
        glowDirty = 0;
    }
    if (lineDirty) {
        u8 c;
        dmaCopyCGram((u8 *)lineBuf, 7, 12); /* sub-pal 0 lines 7..12 */
        for (c = 0; c < 6; c++)             /* per-color line slot mirrors */
            dmaCopyCGram((u8 *)&lineBuf[c], (u16)(16 * (c + 1) + 2), 2);
        lineDirty = 0;
    }
    if (twDirty) {
        u8 i;
        for (i = 0; i < TWINKLE_N; i++)
            dmaCopyCGram((u8 *)&twBuf[i], (u16)(112 + twSlot[i]), 2);
        twDirty = 0;
    }
}

/* Breathing outlines: all six neon line colors ease toward white and back
 * on a slow sine - pure palette animation, the SNES stand-in for bloom. */
static const u8 breathTab[32] = {0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 4, 5, 5, 5, 5, 5,
                                 5, 5, 5, 5, 4, 4, 4, 3, 3, 2, 2, 1, 1, 0, 0, 0};

void linePulse(u8 frame) {
    u8 c, t = breathTab[(frame >> 2) & 31]; /* ~2.1s period, max 5/16 */
    for (c = 0; c < 6; c++) lineBuf[c] = lerpBGR(lineBGR[c], 0x7FFF, t);
    lineDirty = 1;
}

/* The glow display-color rides on one CGRAM entry; the clear animation
 * ramps it every frame (white-hot -> neon -> black). t is 0..16. */
u16 lerpBGR(u16 a, u16 b, u8 t) {
    u16 r = ((a & 31) * (16 - t) + (b & 31) * t) >> 4;
    u16 g = (((a >> 5) & 31) * (16 - t) + ((b >> 5) & 31) * t) >> 4;
    u16 bl = (((a >> 10) & 31) * (16 - t) + ((b >> 10) & 31) * t) >> 4;
    return (bl << 10) | (g << 5) | r;
}

void glowSet(u16 bgr) {
    glowColor = bgr;
    glowDirty = 1;
}

/* H-flip sends static sector i to sector ccwPerm[i]; the flipped frame's
 * palette index i must therefore carry the color of slot ccwPerm[i]. */
static const u8 ccwPerm[6] = {0, 5, 4, 3, 2, 1};

void spinAnimBegin(u8 k, u8 j, u8 ccw) {
    u8 cols[6], real[6], ringTri[6];
    u8 i, q, c;
    u16 o;

    spinGather(k, j, cols, real);
    for (i = 0; i < 16; i++) objPalBuf[i] = 0;
    for (i = 0; i < 6; i++) {
        c = cols[ccw ? ccwPerm[i] : i];
        objPalBuf[1 + i] = fillBGR[c];
        objPalBuf[9 + i] = lineBGR[c];
    }
    objPalBuf[15] = 0x7FFF; /* axis pin core (white) */
    objPalBuf[8] = 0x5252;  /* axis pin halo (grey 150,150,165) */
    objPalDirty = 1;

    /* Ring triangle ids (0xFF = phantom slot, nothing to blank). */
    for (i = 0; i < 6; i++) {
        s8 cc = (s8)k + RING_DC[i];
        s8 rr = (s8)j + RING_DR[i];
        ringTri[i] = 0xFF;
        if (cc >= 0 && cc < BOARD_COLS && rr >= 0 && rr < BOARD_ROWS && boardColor[rr][cc] != NO_CELL)
            ringTri[i] = triOfCell[rr][cc];
    }

    /* Blank the spinning triangles' cells. Tiles shared with a non-ring
     * neighbor keep that neighbor's half so no notch opens at the cluster
     * boundary; everything else under the sprites goes transparent. */
    for (i = 0; i < 6; i++) {
        u8 t = ringTri[i];
        if (t == 0xFF) continue;
        for (o = triCellOfs[t]; o < triCellOfs[t + 1]; o++) {
            u8 tx = triCellXY[o * 2];
            u8 ty = triCellXY[o * 2 + 1];
            u8 sid = cellStruct[ty][tx];
            u16 e = axisEntry[sid]; /* default: keep the pins, drop the rest */
            if (structOwners[sid] == 2) {
                u8 a = cellTriA[ty][tx];
                u8 b = cellTriB[ty][tx];
                u8 other = (a == t) ? b : a;
                u8 inRing = 0;
                for (q = 0; q < 6; q++)
                    if (ringTri[q] == other) inRing = 1;
                if (!inRing) {
                    u8 oc = boardColor[triRow[other]][triCol[other]];
                    /* keep the OTHER side: halfTable has A-only first, B-only at +6 */
                    e = halfTable[halfBase[sid] + ((a == t) ? 6 : 0) + oc];
                }
            }
            mapBuf[(u16)ty * 32 + BOARD_TILE_X + tx] = e;
        }
    }
    mapDirty = 1;
}

void spinAnimFrame(u8 k, u8 j, u8 ccw, u8 f) {
    u16 t0 = ((u16)(f >> 1)) * 128 + (u16)(f & 1) * 8;
    u16 x0 = VTX_PX_X(k) - 32;
    u16 y0 = VTX_PX_Y(j) - 32 - 1;
    if (!ccw) {
        oamSet(4, x0, y0, 3, 0, 0, t0, 1);
        oamSet(8, x0 + 32, y0, 3, 0, 0, t0 + 4, 1);
        oamSet(12, x0, y0 + 32, 3, 0, 0, t0 + 64, 1);
        oamSet(16, x0 + 32, y0 + 32, 3, 0, 0, t0 + 68, 1);
    } else {
        /* H-flipped playback: quadrant art swaps sides too. */
        oamSet(4, x0, y0, 3, 1, 0, t0 + 4, 1);
        oamSet(8, x0 + 32, y0, 3, 1, 0, t0, 1);
        oamSet(12, x0, y0 + 32, 3, 1, 0, t0 + 68, 1);
        oamSet(16, x0 + 32, y0 + 32, 3, 1, 0, t0 + 64, 1);
    }
    oamSetEx(4, OBJ_LARGE, OBJ_SHOW);
    oamSetEx(8, OBJ_LARGE, OBJ_SHOW);
    oamSetEx(12, OBJ_LARGE, OBJ_SHOW);
    oamSetEx(16, OBJ_LARGE, OBJ_SHOW);
}

void spinAnimEnd(void) {
    oamSetVisible(4, OBJ_HIDE);
    oamSetVisible(8, OBJ_HIDE);
    oamSetVisible(12, OBJ_HIDE);
    oamSetVisible(16, OBJ_HIDE);
}

void shakeStart(u8 amp, u8 frames) {
    shakeAmp = amp;
    shakeT = frames;
}

/* Shockwave pulses at up to 4 cleared hex centers: wave A fires at tick 0
 * (sprites 5..8) and an echo wave at tick PULSE_ECHO_AT (sprites 9..12).
 * 32x32, cursor palette; frame f's top-left tile is 416 + f*4. */
#define PULSE_BASE_TILE 416
#define PULSE_SPRITES 4
#define PULSE_ECHO_AT 14

static u8 pulseN;
static u8 pulseKs[PULSE_SPRITES], pulseJs[PULSE_SPRITES];

static void pulseWaveShow(u8 wave) {
    u8 i;
    for (i = 0; i < pulseN; i++) {
        u16 id = (u16)(5 + wave * PULSE_SPRITES + i) * 4;
        u16 x = VTX_PX_X(pulseKs[i]) - 16;
        u16 y = VTX_PX_Y(pulseJs[i]) - 16 - 1;
        oamSet(id, x, y, 3, 0, 0, PULSE_BASE_TILE, 0);
        oamSetEx(id, OBJ_LARGE, OBJ_SHOW);
    }
}

static void pulseWaveTick(u8 wave, u8 t) {
    u8 i, f;
    if (t == 0) pulseWaveShow(wave);
    f = t >> 2; /* 4 ticks per frame */
    for (i = 0; i < pulseN; i++) {
        u16 id = (u16)(5 + wave * PULSE_SPRITES + i) * 4;
        if (f >= 4) oamSetVisible(id, OBJ_HIDE);
        else oamSetGfxOffset(id, (u16)(PULSE_BASE_TILE + f * 4));
    }
}

void pulseStart(u8 n, const u8 *ks, const u8 *js) {
    u8 i;
    pulseN = (n > PULSE_SPRITES) ? PULSE_SPRITES : n;
    for (i = 0; i < pulseN; i++) {
        pulseKs[i] = ks[i];
        pulseJs[i] = js[i];
    }
    pulseWaveShow(0);
}

void pulseTick(u8 tick) {
    pulseWaveTick(0, tick);
    if (tick >= PULSE_ECHO_AT) pulseWaveTick(1, tick - PULSE_ECHO_AT);
}

void pulseEnd(void) {
    u8 i;
    for (i = 0; i < PULSE_SPRITES * 2; i++) oamSetVisible((u16)(5 + i) * 4, OBJ_HIDE);
    pulseN = 0;
}

/* Seam sparks (port of the web game's seam-current layer): white currents
 * crawling the interior triangle seams - edges whose BOTH triangles are on
 * the board, never the hex rim. Up to 3 concurrent, sprites 13..15,
 * bright/dim flicker, 8.8 fixed-point motion. */
#define SPARK_N 3
#define SPARK_TILE 480
#define SPARK_DUR 36
#define MAX_EDGES 96

static u8 edgeX0[MAX_EDGES], edgeY0[MAX_EDGES], edgeX1[MAX_EDGES], edgeY1[MAX_EDGES];
static u8 edgeCount;
static u8 spkOn[SPARK_N], spkT[SPARK_N];
static s16 spkX[SPARK_N], spkY[SPARK_N], spkDX[SPARK_N], spkDY[SPARK_N];
static u8 spkCool;

static void addEdge(u8 x0, u8 y0, u8 x1, u8 y1) {
    if (edgeCount >= MAX_EDGES) return;
    edgeX0[edgeCount] = x0;
    edgeY0[edgeCount] = y0;
    edgeX1[edgeCount] = x1;
    edgeY1[edgeCount] = y1;
    edgeCount++;
}

static u8 onBoard(s8 c, s8 r) {
    return c >= 0 && c < BOARD_COLS && r >= 0 && r < BOARD_ROWS && boardColor[r][c] != NO_CELL;
}

void sparksInit(void) {
    u8 c, r, j, k, i;
    edgeCount = 0;
    /* horizontal seams: up(col=k,row=j-1) above, down(col=k,row=j) below */
    for (j = 1; j < BOARD_ROWS; j++) {
        for (k = 0; k + 2 <= BOARD_COLS; k++) {
            if (((k + j) & 1) == 0) continue;
            if (onBoard(k, j - 1) && onBoard(k, j))
                addEdge(VTX_PX_X(k), VTX_PX_Y(j), VTX_PX_X(k + 2), VTX_PX_Y(j));
        }
    }
    /* diagonal seams between horizontally adjacent triangles */
    for (r = 0; r < BOARD_ROWS; r++) {
        for (c = 0; c + 1 < BOARD_COLS; c++) {
            if (!onBoard(c, r) || !onBoard(c + 1, r)) continue;
            if (((c + r) & 1) == 0) /* up | down: edge falls down-right */
                addEdge(VTX_PX_X(c + 1), VTX_PX_Y(r), VTX_PX_X(c + 2), VTX_PX_Y(r + 1));
            else                    /* down | up: edge falls down-left */
                addEdge(VTX_PX_X(c + 2), VTX_PX_Y(r), VTX_PX_X(c + 1), VTX_PX_Y(r + 1));
        }
    }
    for (i = 0; i < SPARK_N; i++) spkOn[i] = 0;
    spkCool = 30;
}

/* Ambient sky: one object at a time crossing BEHIND the board (OBJ priority
 * 2, under BG1's priority-1 tiles, over the backdrop) - a slow ship
 * (left/right) or a fast shooting star. Sprite 16. */
#define AMB_SHIP_TILE 386
#define AMB_STAR_TILE 388
#define AMB_ID (16 * 4)

static u8 ambOn, ambStar, ambFlip;
static s16 ambX, ambY, ambDX, ambDY; /* 8.8 */
static u16 ambCool;

void ambientFrame(void) {
    s16 sx, sy;
    if (!ambOn) {
        if (ambCool) {
            ambCool--;
            return;
        }
        ambStar = (rngNext() & 3) == 0; /* 1 in 4 spawns is a shooting star */
        if (ambStar) {
            ambFlip = rngNext() & 1;
            ambX = (s16)(40 + (rngNext() % 150)) << 8;
            ambY = (s16)(-12) << 8;
            ambDX = ambFlip ? -0x0280 : 0x0280; /* 2.5 px/f diagonal */
            ambDY = 0x0200;
        } else {
            ambFlip = rngNext() & 1; /* RTL when set */
            ambX = ambFlip ? ((s16)256 << 8) : ((s16)(-16) << 8);
            ambY = (s16)(24 + (rngNext() % 170)) << 8;
            ambDX = ambFlip ? -0x0060 : 0x0060; /* ~0.4 px/f drift */
            ambDY = 0;
        }
        ambOn = 1;
        return;
    }
    ambX += ambDX;
    ambY += ambDY;
    sx = ambX >> 8;
    sy = ambY >> 8;
    if (sx < -16 || sx > 256 || sy > 224) {
        ambOn = 0;
        ambCool = 500 + (rngNext() & 511); /* ~8-17s of empty sky */
        oamSetVisible(AMB_ID, OBJ_HIDE);
        return;
    }
    oamSet(AMB_ID, (u16)sx, (u16)(sy - 1), 2, ambFlip, 0,
           ambStar ? AMB_STAR_TILE : AMB_SHIP_TILE, 0);
    oamSetEx(AMB_ID, OBJ_SMALL, OBJ_SHOW);
}

/* Backdrop star twinkle: the brightest backdrop palette slots breathe.
 * Staged here, DMA'd in renderVBlank. */
void twinkleFrame(u8 frame) {
    u8 i, t;
    for (i = 0; i < TWINKLE_N; i++) {
        t = breathTab[((frame >> 1) + i * 11) & 31];
        twBuf[i] = lerpBGR(twColor[i], 0x294A, t); /* dim toward dark grey */
    }
    twDirty = 1;
}

void sparksFrame(u8 frame) {
    u8 i;
    if (spkCool) {
        spkCool--;
    } else {
        for (i = 0; i < SPARK_N; i++) {
            if (!spkOn[i]) break;
        }
        if (i < SPARK_N && edgeCount) {
            u8 e = rngNext() % edgeCount;
            u8 rev = rngNext() & 1;
            u8 ax = rev ? edgeX1[e] : edgeX0[e];
            u8 ay = rev ? edgeY1[e] : edgeY0[e];
            u8 bx = rev ? edgeX0[e] : edgeX1[e];
            u8 by = rev ? edgeY0[e] : edgeY1[e];
            u16 m;
            spkX[i] = (s16)ax << 8;
            spkY[i] = (s16)ay << 8;
            /* unsigned divide + explicit sign: tcc-816's signed 16-bit
             * division mangles negative deltas (sparks flew off-board on
             * right-to-left / bottom-to-top runs) */
            m = ((u16)(bx > ax ? bx - ax : ax - bx) << 8) / SPARK_DUR;
            spkDX[i] = (bx >= ax) ? (s16)m : -(s16)m;
            m = ((u16)(by > ay ? by - ay : ay - by) << 8) / SPARK_DUR;
            spkDY[i] = (by >= ay) ? (s16)m : -(s16)m;
            spkT[i] = 0;
            spkOn[i] = 1;
        }
        spkCool = 50 + (rngNext() & 63);
    }
    for (i = 0; i < SPARK_N; i++) {
        u16 id = (u16)(13 + i) * 4;
        u16 tile;
        if (!spkOn[i]) continue;
        spkX[i] += spkDX[i];
        spkY[i] += spkDY[i];
        spkT[i]++;
        if (spkT[i] >= SPARK_DUR) {
            spkOn[i] = 0;
            oamSetVisible(id, OBJ_HIDE);
            continue;
        }
        /* electric flicker: bright/dim every other pair of frames */
        tile = ((frame >> 1) & 1) ? SPARK_TILE : SPARK_TILE + 2;
        oamSet(id, (u16)(spkX[i] >> 8) - 8, (u16)(spkY[i] >> 8) - 9, 3, 0, 0, tile, 0);
        oamSetEx(id, OBJ_SMALL, OBJ_SHOW);
    }
}

void cursorUpdate(u8 k, u8 j, u8 frame) {
    u16 x = VTX_PX_X(k) - 8;
    u16 y = VTX_PX_Y(j) - 8 - 1; /* OAM sprites display one line low */
    oamSet(OAM_CURSOR, x, y, 3, 0, 0, CURSOR_TILE, 0);
    oamSetEx(OAM_CURSOR, OBJ_SMALL, OBJ_SHOW);
    /* gentle blink: ~0.7s on, ~0.2s off */
    if ((frame & 63) > 51) oamSetVisible(OAM_CURSOR, OBJ_HIDE);
}
