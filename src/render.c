#include <snes.h>
#include "core.h"
#include "boardtab.h"
#include "bg2tab.h"
#include "render.h"

extern char board_pic, board_picend, board_pal;
extern char cursor_pic, cursor_picend, cursor_pal;
extern char spin_pic, pulse_pic, spark_pic, ambient_pic;
extern char bgl1_pic, bgl1_picend, bgl1_map, bgl1_pal;
extern char bgl2_pic, bgl2_picend, bgl2_map, bgl2_pal;
extern char bgl3_pic, bgl3_picend, bgl3_map, bgl3_pal;
extern char bgl4_pic, bgl4_picend, bgl4_map, bgl4_pal;
extern char title8a_pic, title8a_picend, title8b_pic, title8b_picend;
extern char title8_map, title8_pal;
extern char logo8_pic, logo8_picend, logo8_map, logo8_pal;
extern char hudfont_pic;

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
static u8 twSet; /* which backdrop's twinkle palette set (phase - 1) */
static u16 bg2X, bg2Y; /* backdrop drift accumulators (8.8) */
static u8 sceneMode;   /* mode-3 boot scene active: minimal vblank path */
static u16 sceneV;     /* pinned BG1 vscroll during scenes */
static u16 blinkColor; /* CGRAM 255: the baked PRESS START text */
static u8 blinkDirty;
static u16 hudMap[32 * 32];
static u8 hudDirty;
static u16 heatColor;
static u8 heatColorDirty;
static u8 barPx = 0xFF; /* current bar fill, 0..240 */
static u8 dotPalDirty;

void renderInit(void) {
    consoleInit();
    setBrightness(0);
    WaitForVBlank();
    oamInit();
}

/* Load EVERYTHING the game needs into VRAM/CGRAM (call with the screen
 * force-blanked). Runs at every title -> play transition: the mode-3 title
 * owns nearly all of VRAM and the full CGRAM, so nothing survives it. */
void renderGameLoad(void) {
    u16 i;

    /* BG1: the board. 4bpp tiles + 16-color subpalette 0. */
    dmaCopyVram((u8 *)&board_pic, VRAM_BG1_TILES, (u16)(&board_picend - &board_pic));
    setPalette((u8 *)&board_pal, 0, 112 * 2); /* sub-pal 0 + per-color 1..6 */
    bgSetGfxPtr(0, VRAM_BG1_TILES);
    bgSetMapPtr(0, VRAM_BG1_MAP, SC_32x32);

    /* BG2: phase-1 backdrop behind the board (sub-palette 7). */
    bg2LoadPhase(1);
    twinkleSelect(0);
    bgSetGfxPtr(1, VRAM_BG2_TILES);
    bgSetMapPtr(1, VRAM_BG2_MAP, SC_32x32);

    /* BG3: HUD. Deadfall's transparent 2bpp font (white on transparent,
     * ASCII 32..95) + 9 procedurally built heat-bar fill tiles at 64..72.
     * 2bpp sub-palette 7 = CGRAM 28..31: those sit in BG1 sub-pal 1's
     * unused tail, so nothing collides. 29=white, 30=dark, 31=heat color. */
    dmaCopyVram((u8 *)&hudfont_pic, VRAM_HUDFONT, 1024);
    {
        static u8 bar[9 * 16];
        u16 o = 0;
        u8 f, r2, mask;
        for (f = 0; f < 9; f++) {
            mask = (f == 0) ? 0 : (u8)(0xFF << (8 - f));
            for (r2 = 0; r2 < 8; r2++) {
                if (r2 == 0 || r2 == 7) {
                    bar[o++] = 0xFF; /* white border line (idx 1) */
                    bar[o++] = 0x00;
                } else {
                    bar[o++] = mask; /* fill = idx 3, rest = idx 2 (dark) */
                    bar[o++] = 0xFF;
                }
            }
        }
        dmaCopyVram(bar, VRAM_HUDBAR, sizeof(bar));
    }
    /* Panel border tiles at HUD_BAR_TILE+9..16: 1px white edges (T, B, L,
     * R, TL, TR, BL, BR). 2bpp: plane0 = white (idx 1). */
    {
        static u8 box[8 * 16];
        u16 o = 0;
        u8 t2, r2;
        for (t2 = 0; t2 < 8; t2++) {
            u8 top = (t2 == 0 || t2 == 4 || t2 == 5);
            u8 bot = (t2 == 1 || t2 == 6 || t2 == 7);
            u8 lef = (t2 == 2 || t2 == 4 || t2 == 6);
            u8 rig = (t2 == 3 || t2 == 5 || t2 == 7);
            for (r2 = 0; r2 < 8; r2++) {
                u8 p0 = 0;
                if ((top && r2 == 0) || (bot && r2 == 7)) p0 = 0xFF;
                if (lef) p0 |= 0x80;
                if (rig) p0 |= 0x01;
                box[o++] = p0;
                box[o++] = 0;
            }
        }
        dmaCopyVram(box, (u16)(VRAM_HUDBAR + 9 * 8), sizeof(box));
    }
    /* 3px-lowered digit/X glyphs for the panel value rows: each glyph
     * splits across two tile rows; the lower half bakes in the panel's
     * bottom border line so row 5 stays seamless. */
    {
        static u8 up[11 * 16], lo[11 * 16];
        const u8 *src;
        u8 g, r2;
        for (g = 0; g < 11; g++) {
            u8 glyph = (g < 10) ? (u8)('0' - 32 + g) : (u8)('X' - 32);
            src = (u8 *)&hudfont_pic + (u16)glyph * 16;
            for (r2 = 0; r2 < 8; r2++) {
                u8 p0 = (r2 >= 3) ? src[(u8)(r2 - 3) * 2] : 0;
                up[g * 16 + r2 * 2] = p0;
                up[g * 16 + r2 * 2 + 1] = 0;
                p0 = (r2 < 3) ? src[(u8)(r2 + 5) * 2] : 0;
                if (r2 == 7) p0 |= 0xFF; /* panel bottom border line */
                lo[g * 16 + r2 * 2] = p0;
                lo[g * 16 + r2 * 2 + 1] = 0;
            }
        }
        dmaCopyVram(up, VRAM_HUDSHIFT, sizeof(up));
        dmaCopyVram(lo, (u16)(VRAM_HUDSHIFT + 11 * 8), sizeof(lo));
    }
    setPaletteColor(29, 0x7FFF); /* HUD white */
    setPaletteColor(30, 0x0C63); /* HUD dark backing */
    setPaletteColor(31, 0x03E0); /* heat color (staged per frame after) */
    bgSetGfxPtr(2, VRAM_BG3_TILES);
    bgSetMapPtr(2, VRAM_BG3_MAP, SC_32x32);
    for (i = 0; i < 32 * 32; i++) hudMap[i] = 0;
    hudDirty = 1;

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
    /* ambient ship/star/dot: rows 24/25 cols 2..7 (tiles 386/388/390) */
    dmaCopyVram((u8 *)&ambient_pic, (u16)(VRAM_OBJ_TILES + 0x1000 + 130 * 16), 192);
    dmaCopyVram((u8 *)&ambient_pic + 192, (u16)(VRAM_OBJ_TILES + 0x1000 + 146 * 16), 192);
    dotPalDirty = 1; /* OBJ palettes 2..7 slot 1 = neons; staged to vblank */
    setPalette((u8 *)&cursor_pal, 128, 16 * 2);
    REG_OBSEL = OBJ_SIZE16_L32 | (VRAM_OBJ_TILES >> 13);

    for (i = 0; i < 32 * 32; i++) mapBuf[i] = 0;
    for (i = 0; i < N_TRIANGLES; i++) triDisp[i] = 0xFF;
    mapDirty = 1;

    setMode(BG_MODE1, 0);
    REG_BGMODE = 0x09;  /* mode 1 + BG3 priority: HUD above everything */
    REG_SETINI = 0x04;  /* 239-line overscan: buys the board's lower position */
    videoMode = 0x17;   /* BG1 + BG2 + BG3 + OBJ */
    REG_TM = 0x17;
    sceneMode = 0;
    /* Screen stays force-blanked: the caller flushes the initial maps (free
     * DMA bandwidth while blanked), then calls setScreenOn(). */
}

/* Mode-3 boot scenes: 8bpp BG1, full-CGRAM palette. which: 1 title, 2 logo.
 * Call with the screen force-blanked. */
void sceneShow(u8 which) {
    setMode(BG_MODE3, 0);
    REG_SETINI = 0x00; /* scenes are 224-line compositions */
    bgSetGfxPtr(0, 0x1000);
    bgSetMapPtr(0, 0x0000, SC_32x32);
    if (which == 1) {
        dmaCopyVram((u8 *)&title8a_pic, 0x1000, (u16)(&title8a_picend - &title8a_pic));
        dmaCopyVram((u8 *)&title8b_pic, 0x4800, (u16)(&title8b_picend - &title8b_pic));
        dmaCopyVram((u8 *)&title8_map, 0x0000, 0x800);
        dmaCopyCGram((u8 *)&title8_pal, 0, 512);
    } else {
        dmaCopyVram((u8 *)&logo8_pic, 0x1000, (u16)(&logo8_picend - &logo8_pic));
        dmaCopyVram((u8 *)&logo8_map, 0x0000, 0x800);
        dmaCopyCGram((u8 *)&logo8_pal, 0, 512);
    }
    videoMode = 0x01; /* BG1 only */
    REG_TM = 0x01;
    sceneMode = 1;
    sceneV = 0x3FF;
    blinkDirty = 0;
}

void scenePinV(u16 v) {
    sceneV = v;
}

void sceneBlink(u16 bgr) {
    blinkColor = bgr;
    blinkDirty = 1;
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
    if (sceneMode) { /* logo / title: pinned scroll + the blink entry only */
        bgSetScroll(0, 0, sceneV);
        if (blinkDirty) {
            dmaCopyCGram((u8 *)&blinkColor, 255, 2);
            blinkDirty = 0;
        }
        return;
    }
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
    /* Seamless backdrop drifts down-left: 1px steps every 8/16 frames -
     * frequent enough to read as motion (slower hops looked choppy). */
    bg2X += 0x0020; /* 7.5 px/s */
    bg2Y -= 0x0010;
    bgSetScroll(1, (u16)(bg2X >> 8) & 0xFF, (u16)(((bg2Y >> 8) - 1) & 0xFF));
    bgSetScroll(2, 0, 0x3FF);
    if (heatColorDirty) {
        dmaCopyCGram((u8 *)&heatColor, 31, 2);
        heatColorDirty = 0;
    }
    if (dotPalDirty) {
        u8 c;
        for (c = 0; c < 6; c++)
            dmaCopyCGram((u8 *)&lineBGR[c], (u16)(128 + (2 + c) * 16 + 1), 2);
        dotPalDirty = 0;
    }
    /* The two 2KB maps NEVER ship in the same vblank: together with OAM +
     * palettes they exceed vblank DMA bandwidth and the tail gets dropped
     * mid-VRAM (the 'board cut off after row 0' bug). Board first. */
    if (mapDirty) {
        dmaCopyVram((u8 *)mapBuf, VRAM_BG1_MAP, 0x800);
        mapDirty = 0;
    } else if (hudDirty) {
        dmaCopyVram((u8 *)hudMap, VRAM_BG3_MAP, 0x800);
        hudDirty = 0;
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
            dmaCopyCGram((u8 *)&twBuf[i], (u16)(112 + twSlot[twSet][i]), 2);
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

/* Positions are UNSIGNED 8.8: screen x up to 228 -> 58k, which overflows
 * s16 (that bug put sparks 'outside in space' on the right half of the
 * board). Deltas stay s16; u16 += s16 is exact modulo arithmetic. */
static u8 spkOn[SPARK_N], spkT[SPARK_N];
static u16 spkX[SPARK_N], spkY[SPARK_N];
static s16 spkDX[SPARK_N], spkDY[SPARK_N];
static u8 spkCool;

void sparksInit(void) {
    u8 i;
    seamsInit(); /* golden-tested seam list in core (vertex units) */
    for (i = 0; i < SPARK_N; i++) spkOn[i] = 0;
    spkCool = 30;
}

/* Ambient sky: one object at a time crossing BEHIND the board (OBJ priority
 * 2, under BG1's priority-1 tiles, over the backdrop) - a slow ship
 * (left/right) or a fast shooting star. Sprite 16. */
#define AMB_SHIP_TILE 386
#define AMB_STAR_TILE 388
#define AMB_ID (16 * 4)

/* Pure 16-bit, all-positive coordinates: 9.7 fixed point with a +32px bias
 * (tcc-816's 32-bit helpers proved as unreliable as its signed division -
 * the s32 version made flyers pop in and out at the screen edges).
 * Biased x spans 16..288 (-16..256 on screen): 288<<7 = 36864, fits u16. */
#define AMB_BIAS 32
static u8 ambOn, ambStar, ambFlip;
static u16 ambX, ambY; /* biased 9.7 */
static s16 ambDX, ambDY;
static u16 ambCool;

void ambientFrame(void) {
    u16 bx, by;
    if (!ambOn) {
        if (ambCool) {
            ambCool--;
            return;
        }
        ambStar = (rngNext() & 3) == 0; /* 1 in 4 spawns is a shooting star */
        if (ambStar) {
            ambFlip = rngNext() & 1;
            ambX = (u16)(AMB_BIAS + 40 + (rngNext() & 127)) << 7;
            ambY = (u16)(AMB_BIAS - 12) << 7;
            ambDX = ambFlip ? -0x0140 : 0x0140; /* 2.5 px/f diagonal */
            ambDY = 0x0100;
        } else {
            ambFlip = rngNext() & 1; /* RTL when set */
            ambX = (u16)(ambFlip ? AMB_BIAS + 256 : AMB_BIAS - 16) << 7;
            ambY = (u16)(AMB_BIAS + 20 + (rngNext() & 127) + (rngNext() & 31)) << 7;
            ambDX = ambFlip ? -0x0030 : 0x0030; /* ~0.4 px/f drift */
            ambDY = 0;
        }
        ambOn = 1;
        return;
    }
    ambX += (u16)ambDX;
    ambY += (u16)ambDY;
    bx = ambX >> 7; /* biased pixels, 0..511 */
    by = ambY >> 7;
    if (bx < AMB_BIAS - 16 || bx > AMB_BIAS + 256 || by > AMB_BIAS + 224) {
        ambOn = 0;
        ambCool = 500 + (rngNext() & 511); /* ~8-17s of empty sky */
        oamSetVisible(AMB_ID, OBJ_HIDE);
        return;
    }
    oamSet(AMB_ID, (u16)(bx - AMB_BIAS), (u16)(by - AMB_BIAS - 1), 2, ambFlip, 0,
           ambStar ? AMB_STAR_TILE : AMB_SHIP_TILE, 0);
    oamSetEx(AMB_ID, OBJ_SMALL, OBJ_SHOW);
}

/* Backdrop star twinkle: the brightest backdrop palette slots breathe.
 * Staged here, DMA'd in renderVBlank. */
void twinkleFrame(u8 frame) {
    u8 i, t;
    for (i = 0; i < TWINKLE_N; i++) {
        t = breathTab[((frame >> 1) + i * 11) & 31];
        twBuf[i] = lerpBGR(twColor[twSet][i], 0x294A, t); /* dim toward grey */
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
        if (i < SPARK_N && seamCount) {
            u8 e, rev, ax, ay, bx, by;
            u16 m;
            do {
                e = rngNext() & 127; /* rejection sample: tcc division/modulo is unsafe */
            } while (e >= seamCount);
            rev = rngNext() & 1;
            ax = VTX_PX_X(rev ? seamK1[e] : seamK0[e]);
            ay = VTX_PX_Y(rev ? seamJ1[e] : seamJ0[e]);
            bx = VTX_PX_X(rev ? seamK0[e] : seamK1[e]);
            by = VTX_PX_Y(rev ? seamJ0[e] : seamJ1[e]);
            spkX[i] = (u16)ax << 8;
            spkY[i] = (u16)ay << 8;
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
        spkX[i] += (u16)spkDX[i];
        spkY[i] += (u16)spkDY[i];
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

/* BG3 entries: glyph | sub-pal 7 | tile-priority (with BGMODE bit 3 that
 * lifts the HUD above every layer and sprite). */
#define HUD_ATTR ((u16)(7 << 10) | 0x2000)
#define BAR_BASE HUD_BAR_TILE

/* Per-phase backdrop swap (call with the screen force-blanked: ~32KB DMA). */
void bg2LoadPhase(u8 phase) {
    static char *const pics[4] = {&bgl1_pic, &bgl2_pic, &bgl3_pic, &bgl4_pic};
    static char *const ends[4] = {&bgl1_picend, &bgl2_picend, &bgl3_picend, &bgl4_picend};
    static char *const maps[4] = {&bgl1_map, &bgl2_map, &bgl3_map, &bgl4_map};
    static char *const pals[4] = {&bgl1_pal, &bgl2_pal, &bgl3_pal, &bgl4_pal};
    u8 i = phase - 1;
    dmaCopyVram((u8 *)pics[i], VRAM_BG2_TILES, (u16)(ends[i] - pics[i]));
    dmaCopyVram((u8 *)maps[i], VRAM_BG2_MAP, 0x800);
    dmaCopyCGram((u8 *)pals[i], 112, 32);
}

void twinkleSelect(u8 idx) {
    twSet = idx;
}

void hudClear(void) {
    u16 i;
    for (i = 0; i < 32 * 32; i++) hudMap[i] = 0;
    barPx = 0xFF;
    hudDirty = 1;
}

void hudText(u8 x, u8 y, const char *s) {
    u16 *p = &hudMap[(u16)y * 32 + x];
    while (*s) {
        *p++ = (u16)(HUD_FONT_TILE + *s - 32) | HUD_ATTR;
        s++;
    }
    hudDirty = 1;
}

void hudNum(u8 x, u8 y, u16 val, u8 digits) {
    u16 *p = &hudMap[(u16)y * 32 + x + digits];
    u8 i;
    for (i = 0; i < digits; i++) {
        *--p = (u16)(HUD_FONT_TILE + '0' - 32 + (val % 10)) | HUD_ATTR;
        val /= 10;
    }
    hudDirty = 1;
}

/* Bar fills cols HUD_BAR_X..+HUD_BAR_W-1 of row 0; "HEAT:" label left. */
#define HUD_BAR_X 7
#define HUD_BAR_W 24

void hudBarSet(u8 px) {
    u8 col, full, rem;
    u16 *row = &hudMap[HUD_BAR_X];
    if (px == barPx) return;
    barPx = px;
    full = px >> 3;
    rem = px & 7;
    for (col = 0; col < HUD_BAR_W; col++) {
        u8 lvl = (col < full) ? 8 : (col == full ? rem : 0);
        row[col] = (u16)(BAR_BASE + lvl) | HUD_ATTR;
    }
    hudDirty = 1;
}

void heatColorSet(u16 bgr) {
    heatColor = bgr;
    heatColorDirty = 1;
}

/* Phase-color dots: sprites 17..22 inside the PHASE panel value row. */
#define DOT_TILE 390

void hudDots(u8 n) {
    u8 i;
    for (i = 0; i < 6; i++) {
        u16 id = (u16)(17 + i) * 4;
        if (i < n) {
            /* dot center at (30 + 8i, 39): lowered value row, right of digit */
            oamSet(id, (u16)(22 + 8 * i), 30, 3, 0, 0, DOT_TILE, (u8)(2 + i));
            oamSetEx(id, OBJ_SMALL, OBJ_SHOW);
        } else {
            oamSetVisible(id, OBJ_HIDE);
        }
    }
}

/* Panel value-row writers: 3px-lowered glyphs across map rows 4 and 5. */
static void hudValGlyph(u8 x, u8 g) {
    hudMap[4 * 32 + x] = (u16)(HUD_SHIFT_TILE + g) | HUD_ATTR;
    hudMap[5 * 32 + x] = (u16)(HUD_SHIFT_TILE + 11 + g) | HUD_ATTR;
}

void hudValNum(u8 x, u16 val, u8 digits) {
    u8 i;
    for (i = 0; i < digits; i++) {
        hudValGlyph((u8)(x + digits - 1 - i), (u8)(val % 10));
        val /= 10;
    }
    hudDirty = 1;
}

void hudValX(u8 x) {
    hudValGlyph(x, 10);
    hudDirty = 1;
}

void hudScore(const u8 *d) {
    u8 i;
    for (i = 0; i < SCORE_DIGITS; i++) {
        hudValGlyph((u8)(12 + i), d[SCORE_DIGITS - 1 - i]);
    }
    hudDirty = 1;
}

/* 1px-bordered panel ring: tiles HUD_BAR_TILE+9.. = T B L R TL TR BL BR. */
void hudBox(u8 x, u8 y, u8 w, u8 h) {
    u16 base = HUD_BAR_TILE + 9;
    u8 i;
    u16 *m = &hudMap[(u16)y * 32 + x];
    m[0] = (u16)(base + 4) | HUD_ATTR;
    m[w - 1] = (u16)(base + 5) | HUD_ATTR;
    for (i = 1; i < w - 1; i++) m[i] = (u16)(base + 0) | HUD_ATTR;
    for (i = 1; i < h - 1; i++) {
        m[(u16)i * 32] = (u16)(base + 2) | HUD_ATTR;
        m[(u16)i * 32 + w - 1] = (u16)(base + 3) | HUD_ATTR;
    }
    m += (u16)(h - 1) * 32;
    m[0] = (u16)(base + 6) | HUD_ATTR;
    m[w - 1] = (u16)(base + 7) | HUD_ATTR;
    for (i = 1; i < w - 1; i++) m[i] = (u16)(base + 1) | HUD_ATTR;
    hudDirty = 1;
}

void cursorUpdate(u8 k, u8 j, u8 frame) {
    u16 x = VTX_PX_X(k) - 8;
    u16 y = VTX_PX_Y(j) - 8 - 1; /* OAM sprites display one line low */
    oamSet(OAM_CURSOR, x, y, 3, 0, 0, CURSOR_TILE, 0);
    oamSetEx(OAM_CURSOR, OBJ_SMALL, OBJ_SHOW);
    /* gentle blink: ~0.7s on, ~0.2s off */
    if ((frame & 63) > 51) oamSetVisible(OAM_CURSOR, OBJ_HIDE);
}
