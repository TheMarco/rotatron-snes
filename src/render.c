#include <snes.h>
#include "core.h"
#include "boardtab.h"
#include "bg2tab.h"
#include "ambtab.h"
#include "render.h"

/* HDMA channel 6 registers (per-scanline BG1 H-scroll shockwave ripple) */
#define REG_HDMAEN     (*(vuint8 *)0x420C)
#define HDMA6_CTRL     (*(vuint8 *)0x4360)
#define HDMA6_REG      (*(vuint8 *)0x4361)
#define HDMA6_ADDRL    (*(vuint8 *)0x4362)
#define HDMA6_ADDRH    (*(vuint8 *)0x4363)
#define HDMA6_ADDRB    (*(vuint8 *)0x4364)

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

/* White-pixel 16×16 OBJ tile for the logo landing burst and sparkles.
 * TL+TR (tile 392, VRAM 0x7880) blank; BL (tile 408, 0x7980) blank; BR
 * (tile 409) has one pixel at row 0, col 0 in color index 1 (= 0x7FFF white
 * from the cursor palette). oamSet at (px-8, py-8) places that pixel at (px,py). */
static const u8 logo_dot_tiles[128] = {
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* TL */
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* TR */
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* BL */
    /* BR: bp0 row 0 = 0x80 -> leftmost pixel = color 1 = white */
    0x80,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

static u16 mapBuf[32 * 32];
static u8 mapDirty;

/* Map updates mark the whole map dirty and the NMI hook DMAs it in one 2KB
 * burst at the START of vblank - the deadfall-proven pattern. (A clever
 * changed-word VRAM-port queue lived here briefly; its CPU pokes corrupted
 * VRAM on accurate emulators. One DMA at vblank start fits even the
 * overscan-shortened blank: ~2KB of a ~3.7KB budget.) */
static u8 mapRowLo, mapRowHi; /* dirty tile-row span; vblank sends only this */
static void mapWrite(u16 idx, u16 e) {
    u8 row;
    if (mapBuf[idx] == e) return;
    mapBuf[idx] = e;
    row = (u8)(idx >> 5);
    if (!mapDirty) {
        mapRowLo = mapRowHi = row;
        mapDirty = 1;
    } else {
        if (row < mapRowLo) mapRowLo = row;
        if (row > mapRowHi) mapRowHi = row;
    }
}
static u16 objPalBuf[16];
static u8 objPalDirty;

/* Per-triangle display override (DISP_WHITE/HIDDEN/GLOW), 0xFF = boardColor.
 * Drives the clear flash/blackout/pop without touching game state. */
u8 triDisp[N_TRIANGLES];
static u8 demoTris[18], demoN; /* how-to pages: rings drawn since last reset */

static u8 shakeT, shakeAmp;
static u16 glowColor;
static u8 glowDirty;
static u16 lineBuf[6]; /* breathing outline colors, staged for vblank */
static u8 lineDirty;
static u16 twBuf[TWINKLE_N]; /* backdrop twinkle colors, staged for vblank */
static u8 twDirty;
static u8 twSet; /* which backdrop's twinkle palette set (phase - 1) */
static u8 mosVal;      /* BG1 mosaic size, re-asserted every vblank */
static u8 mosImpactT;  /* decay steps remaining for hex-impact mosaic burst */
static u8 flashT;      /* frames remaining on white-flash color math */
static u8 flashStep;   /* per-frame brightness decrement (flashT * flashStep = level) */
static u8 bg1Shift;    /* extra BG1 down-shift in px (how-to illustrations) */
static u16 bg2X, bg2Y; /* backdrop drift accumulators (8.8) */
static s8 bg2Jolt;     /* short snap offset applied to BG2 Y, decays 2px/frame */
static u8 rippleT;    /* 10..1 = shockwave active, 0 = off */
static u8 rippleRad;  /* current ring radius in scanlines from center y=120 */
static u8 rippleBuf[718]; /* HDMA linear table: {1,lo,hi}×239 + {0} end-marker */
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
/* Ambient sprite palette (OBJ pal 0 indices 5-15): staged per spawn. */
static u8 ambPalBuf[22];
static u8 ambPalDirty;
static u8 ambTileStep;    /* spawn: strips left to DMA (ambTotalStrips..1) */
static u8 ambTotalStrips; /* ambRows * 2 */
static u8 ambRows;        /* 2 or 3 OBJ rows for the active sprite */
static u8 tmStage;        /* nonzero: REG_TM value to apply at next vblank start */
/* The flyer's tile strip destinations in VRAM (word addresses).
 * Strips 0-3: shared by all sprites (320 bytes each).
 * Strips 4-5: 3-row sprites only; 192 bytes each (6 tiles, cols 0-2). */
static const u16 ambRowDest[6] = {
    VRAM_OBJ_TILES + 0x1000 + 130 * 16, /* tiles 386+: art rows 0-7   */
    VRAM_OBJ_TILES + 0x1000 + 146 * 16, /* tiles 402+: art rows 8-15  */
    VRAM_OBJ_TILES + 0x1000 + 228 * 16, /* tiles 484+: art rows 16-23 */
    VRAM_OBJ_TILES + 0x1000 + 244 * 16, /* tiles 500+: art rows 24-31 */
    VRAM_OBJ_TILES + 0x1000 + 234 * 16, /* tiles 490+: art rows 32-39 (3-row) */
    VRAM_OBJ_TILES + 0x1000 + 250 * 16, /* tiles 506+: art rows 40-47 (3-row) */
};
static u8 ambOn, ambType, ambFlip, ambCols; /* ambType = index into the sprite table */
static u16 ambX, ambY;                       /* biased 9.7 fixed point */
static s16 ambDX, ambDY;
static u16 ambCool;

#if RENDER_VBLANK_DIAG
u16 renderVBlankLastBytes;
u16 renderVBlankWorstBytes;
u8 renderVBlankLastLargeTransfers;
u8 renderVBlankFlags;

#define VBD_BEGIN() do { \
    renderVBlankLastBytes = 0; \
    renderVBlankLastLargeTransfers = 0; \
} while (0)
#define VBD_ADD(bytes, large) do { \
    renderVBlankLastBytes = (u16)(renderVBlankLastBytes + (u16)(bytes)); \
    if (large) renderVBlankLastLargeTransfers++; \
} while (0)
#define VBD_END() do { \
    if (renderVBlankLastBytes > renderVBlankWorstBytes) \
        renderVBlankWorstBytes = renderVBlankLastBytes; \
    if (renderVBlankLastBytes > RENDER_VBLANK_BUDGET_BYTES) \
        renderVBlankFlags |= RENDER_VBLANK_FLAG_OVER_BUDGET; \
    if (renderVBlankLastLargeTransfers > 1) \
        renderVBlankFlags |= RENDER_VBLANK_FLAG_MULTI_LARGE; \
} while (0)
#else
#define VBD_BEGIN() do {} while (0)
#define VBD_ADD(bytes, large) do { (void)(bytes); (void)(large); } while (0)
#define VBD_END() do {} while (0)
#endif

static void renderZeroState(void); /* defined at file end, after all statics */
static void ambHideAll(void);      /* hide the ambient flyer's OAM slots 23-32 */
static void rippleInit(void);      /* pre-fill HDMA table with zero-offset entries */

void renderInit(void) {
    /* FastROM: tcc -F puts our code/tables in the $80+ bank mirrors; this
     * makes those mirrors actually run at 3.58MHz (without it they stay at
     * SlowROM speed and -F buys nothing). Lib code stays in slow banks. */
    REG_MEMSEL = 1;
    consoleInit();
    setBrightness(0);
    WaitForVBlank();
    oamInit();
    renderZeroState();

    /* Neutralize every compositing path: real hardware (and bsnes) boots
     * these as garbage - random windows masked sprites and random color
     * math darkened everything; snes9x's zeroed power-on state hid it. */
    REG_W12SEL = 0;
    REG_W34SEL = 0;
    REG_WOBJSEL = 0;
    REG_WBGLOG = 0;
    REG_WOBJLOG = 0;
    REG_TS = 0;    /* nothing on the sub-screen */
    REG_TMW = 0;   /* no window masking on main... */
    REG_TSW = 0;   /* ...or sub */
    REG_CGWSEL = 0;
    REG_CGADSUB = 0;
    REG_COLDATA = 0xE0; /* fixed color = black, all channels */
    REG_MOSAIC = 0;
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
    /* ambient sprite: ship & voyager share one 5-OBJ-col (80px) region; the
     * active sprite's tiles are DMA'd in at spawn (ambientFrame/renderVBlank).
     * Seed the region with the ship so it's valid before the first spawn.
     * 4 tile rows of 10 tiles -> OBJ tiles 386 / 402 / 484 / 500. */
    dmaCopyVram((u8 *)&ambient_pic,        (u16)(VRAM_OBJ_TILES + 0x1000 + 130 * 16), 320);
    dmaCopyVram((u8 *)&ambient_pic + 320,  (u16)(VRAM_OBJ_TILES + 0x1000 + 146 * 16), 320);
    dmaCopyVram((u8 *)&ambient_pic + 640,  (u16)(VRAM_OBJ_TILES + 0x1000 + 228 * 16), 320);
    dmaCopyVram((u8 *)&ambient_pic + 960,  (u16)(VRAM_OBJ_TILES + 0x1000 + 244 * 16), 320);
    setPalette((u8 *)&ambient_pic + AMB_PAL_OFF, 133, 22); /* seed: sprite 0 palette */
    /* HUD phase dot (always resident): 16x16 OBJ at tile 398 (cols 14-15). */
    dmaCopyVram((u8 *)&ambient_pic + AMB_DOT_OFF,      (u16)(VRAM_OBJ_TILES + 0x1000 + 142 * 16), 64);
    dmaCopyVram((u8 *)&ambient_pic + AMB_DOT_OFF + 64, (u16)(VRAM_OBJ_TILES + 0x1000 + 158 * 16), 64);
    dotPalDirty = 1; /* OBJ palettes 2..7 slot 1 = neons; staged to vblank */
    setPalette((u8 *)&cursor_pal, 128, 16 * 2);
    REG_OBSEL = OBJ_SIZE16_L32 | (VRAM_OBJ_TILES >> 13);
    /* Hide the flyer's slots AND every slot the logo scene used (particles +
     * sparkles, OBJ 23..127): the title left them parked, and the game never
     * overwrites 33+, so they would linger into play as floating debris. */
    { u16 dh; for (dh = 23; dh < 128; dh++) oamSetVisible((u16)(dh * 4), OBJ_HIDE); }

    for (i = 0; i < 32 * 32; i++) mapBuf[i] = 0;
    for (i = 0; i < N_TRIANGLES; i++) triDisp[i] = 0xFF;
    demoN = 0; /* WRAM is random at power-on; the demo list must start empty */
    mapRowLo = 0;
    mapRowHi = 31;
    mapDirty = 1;

    setMode(BG_MODE1, 0);
    REG_BGMODE = 0x09;  /* mode 1 + BG3 priority: HUD above everything */
    REG_SETINI = 0x04;  /* 239-line overscan: buys the board's lower position */
    mosVal = 0;
    bg1Shift = 0;       /* play uses the canonical board position */
    videoMode = 0x17;   /* BG1 + BG2 + BG3 + OBJ */
    REG_TM = 0x17;
    tmStage = 0;        /* drop any layer mask the how-to pages left staged */
    sceneMode = 0;
    rippleInit();       /* pre-fill HDMA table with count=1, offset=0 entries */
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
        videoMode = 0x01; /* BG1 only */
        REG_TM = 0x01;
    } else {
        dmaCopyVram((u8 *)&logo8_pic, 0x1000, (u16)(&logo8_picend - &logo8_pic));
        dmaCopyVram((u8 *)&logo8_map, 0x0000, 0x800);
        dmaCopyCGram((u8 *)&logo8_pal, 0, 512);
        /* dot tile: tile 392 (0x7880) TL+TR, tile 408 (0x7980) BL+BR */
        dmaCopyVram((u8 *)logo_dot_tiles,      (u16)(VRAM_OBJ_TILES + 0x1000 + 136 * 16), 64);
        dmaCopyVram((u8 *)logo_dot_tiles + 64, (u16)(VRAM_OBJ_TILES + 0x1000 + 152 * 16), 64);
        /* load cursor palette so particles use white (pal 0 slot 1 = 0x7FFF) */
        setPalette((u8 *)&cursor_pal, 128, 32);
        REG_OBSEL = OBJ_SIZE16_L32 | (VRAM_OBJ_TILES >> 13);
        videoMode = 0x11; /* BG1 + OBJ for logo particles */
        REG_TM = 0x11;
    }
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
    mapRowLo = 0;
    mapRowHi = 31; /* full map: rebuilds happen under blank/at init */
    mapDirty = 1;
}

/* Incremental recolor: a full rebuild costs several frames of 65816 time
 * (the visible 'pause' at the end of a spin); refreshing only the cells a
 * triangle owns keeps the staged dirty tile-row span small for the next DMA. */
void triRefresh(u8 t) {
    u16 o;
    for (o = triCellOfs[t]; o < triCellOfs[t + 1]; o++) {
        u8 tx = triCellXY[o * 2];
        u8 ty = triCellXY[o * 2 + 1];
        mapWrite((u16)ty * 32 + BOARD_TILE_X + tx, cellEntry(tx, ty));
    }
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

/* How-to-play illustrations: hexes drawn with the real board renderer.
 * Reset puts every triangle on DISP_HIDDEN and blanks whatever rings the
 * previous page drew; Demo colors the ring around hinge (k,j) and refreshes
 * just those cells - everything else stays tile 0 (backdrop shows through),
 * no pins, no neighbor fragments. boardColor is never read (all triDisp
 * set), so this works before any boardInit. Rings must not share triangles
 * (vertices >= 2 apart); tiles shared ACROSS two rings are fine (dual-owner
 * combos bake both colors). */
void howtoHexReset(void) {
    u8 i, t;
    for (t = 0; t < N_TRIANGLES; t++) triDisp[t] = DISP_HIDDEN;
    for (i = 0; i < demoN; i++) triRefresh(demoTris[i]);
    demoN = 0;
}

void howtoHexDemo(u8 k, u8 j, u8 color) {
    u8 i, t;
    for (i = 0; i < 6; i++) {
        u8 c = (u8)((s8)k + RING_DC[i]);
        u8 r = (u8)((s8)j + RING_DR[i]);
        t = triOfCell[r][c];
        triDisp[t] = color;
        triRefresh(t);
        if (demoN < sizeof(demoTris)) demoTris[demoN++] = t;
    }
}

/* All VRAM/CGRAM uploads + scroll asserts. Installed as the NMI hook
 * (nmiSet) so it runs at the VERY START of vblank: called after
 * WaitForVBlank it raced the vblank end, and on accurate hardware (bsnes)
 * the spilled writes were redirected to the PPU's live fetch address -
 * random tile corruption. Also still called directly during force-blank
 * boot/transition loads (safe: it only consumes staged state). */
void renderVBlank(void) {
    VBD_BEGIN();
    if (sceneMode) { /* logo / title: pinned scroll + the blink entry only */
        bgSetScroll(0, 0, sceneV);
        if (blinkDirty) {
            VBD_ADD(2, 0);
            dmaCopyCGram((u8 *)&blinkColor, 255, 2);
            blinkDirty = 0;
        }
        VBD_END();
        return;
    }
    if (tmStage) { /* staged main-screen layer switch (stage transitions) */
        videoMode = tmStage;
        REG_TM = tmStage;
        tmStage = 0;
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
        bgSetScroll(0, 0, (u16)(BOARD_VOFS - bg1Shift)); /* BOARD_PX_Y=76 */
    }
    if (mosImpactT) {
        mosVal = (u8)((u8)(mosVal * 2) / 3); /* geometric decay: ~×0.67/frame */
        if (--mosImpactT == 0) mosVal = 0;
    }
    REG_MOSAIC = (u8)((mosVal << 4) | (mosVal ? 0x01 : 0)); /* board dissolve */
    if (flashT) {
        REG_CGWSEL = 0x00;
        REG_CGADSUB = 0x03; /* add fixed color to BG1 + BG2 */
        REG_COLDATA = (u8)(0xE0 | (u8)((u8)flashT * flashStep));
        if (--flashT == 0) { REG_CGADSUB = 0x00; REG_COLDATA = 0xE0; }
    }
    /* Seamless backdrop drifts down-left: 1px steps every 8/16 frames -
     * frequent enough to read as motion (slower hops looked choppy). */
    bg2X += 0x0020; /* 7.5 px/s */
    bg2Y -= 0x0010;
    {
        s16 jy = (s16)(bg2Y >> 8) - 1 + bg2Jolt;
        bgSetScroll(1, (u16)(bg2X >> 8) & 0xFF, (u16)(jy & 0xFF));
        if (bg2Jolt > 0) bg2Jolt = (s8)(bg2Jolt > 2 ? bg2Jolt - 2 : 0);
        else if (bg2Jolt < 0) bg2Jolt = (s8)(bg2Jolt < -2 ? bg2Jolt + 2 : 0);
    }
    bgSetScroll(2, 0, 0x3F7);
    if (heatColorDirty) {
        VBD_ADD(2, 0);
        dmaCopyCGram((u8 *)&heatColor, 31, 2);
        heatColorDirty = 0;
    }
    if (ambPalDirty) {
        /* OBJ pal 0 indices 5-15 (CGRAM words 133-143): active flyer colors */
        VBD_ADD(22, 0);
        dmaCopyCGram((u8 *)ambPalBuf, 133, 22);
        ambPalDirty = 0;
    }
    if (dotPalDirty) {
        u8 c;
        VBD_ADD(12, 0);
        for (c = 0; c < 6; c++)
            dmaCopyCGram((u8 *)&lineBGR[c], (u16)(128 + (2 + c) * 16 + 1), 2);
        dotPalDirty = 0;
    }
    /* big transfers last (see above); ONE per vblank. The 2KB map wins the
     * slot (a delayed spin recolor is visible; a flyer entering from
     * off-screen a few frames late is not). A flyer spawn streams its 1280
     * tile bytes ONE 320-byte row per vblank: all four rows chained in one
     * vblank overran the overscan window (the NMI already carries the OAM
     * DMA + snesmod work), and the dropped TAIL rows left the previous
     * flyer's bottom half under every new ship - the "junk under the ship"
     * bug. ambientFrame holds the flyer off-screen until the last row lands. */
    if (mapDirty) {
        u16 mapBytes;
        /* only the dirty tile-row span: a spin/glow touches ~6-10 rows
         * (~640B) - the full 2KB goes only on rebuilds */
        mapBytes = (u16)((u16)(mapRowHi - mapRowLo + 1) << 6);
        VBD_ADD(mapBytes, 1);
        dmaCopyVram((u8 *)mapBuf + ((u16)mapRowLo << 6),
                    (u16)(VRAM_BG1_MAP + ((u16)mapRowLo << 5)),
                    mapBytes);
        mapDirty = 0;
    } else if (ambTileStep) {
        u8 strip = ambTotalStrips - ambTileStep;
        u16 nbytes = (strip < 4) ? 320 : 192;
        u8 *s = (u8 *)&ambient_pic + (u16)ambType * AMB_TILE_BYTES + (u16)strip * 320;
        VBD_ADD(nbytes, 1);
        dmaCopyVram(s, ambRowDest[strip], nbytes);
        ambTileStep--;
    } else if (hudDirty) {
        VBD_ADD(0x800, 1);
        dmaCopyVram((u8 *)hudMap, VRAM_BG3_MAP, 0x800);
        hudDirty = 0;
    }
    /* Small CGRAM stages fit beside the single big DMA chosen above. The board
     * map, ambient tile stream, and HUD map stay mutually exclusive so two 2KB
     * map-like transfers never ship in the same vblank. */
    if (objPalDirty) {
        VBD_ADD(32, 0);
        dmaCopyCGram((u8 *)objPalBuf, 144, 32); /* OBJ palette 1 */
        objPalDirty = 0;
    }
    if (glowDirty) {
        u8 c;
        VBD_ADD(14, 0);
        dmaCopyCGram((u8 *)&glowColor, GLOW_CGRAM, 2);
        for (c = 0; c < 6; c++) /* per-color sub-palette mirrors */
            dmaCopyCGram((u8 *)&glowColor, GLOW_CGRAM_C(c), 2);
        glowDirty = 0;
    }
    if (lineDirty) {
        u8 c;
        VBD_ADD(24, 0);
        dmaCopyCGram((u8 *)lineBuf, 7, 12); /* sub-pal 0 lines 7..12 */
        for (c = 0; c < 6; c++)             /* per-color line slot mirrors */
            dmaCopyCGram((u8 *)&lineBuf[c], (u16)(16 * (c + 1) + 2), 2);
        lineDirty = 0;
    }
    if (twDirty) {
        u8 i;
        VBD_ADD(TWINKLE_N * 2, 0);
        for (i = 0; i < TWINKLE_N; i++)
            dmaCopyCGram((u8 *)&twBuf[i], (u16)(112 + twSlot[twSet][i]), 2);
        twDirty = 0;
    }
    /* HDMA channel 6: per-scanline BG1 H-scroll ripple. Re-write A1T + re-enable
     * every vblank (hardware reloads A1T at active-display start from these regs).
     * rippleFrame() (called in game loop) filled rippleBuf before this vblank. */
    if (rippleT) {
        HDMA6_CTRL  = 0x02;  /* mode 2: write 2 bytes to same B-bus reg per H-blank */
        HDMA6_REG   = 0x0D;  /* B-bus reg $210D = BG1HOFS */
        HDMA6_ADDRL = (u8)((u16)&rippleBuf[0]);
        HDMA6_ADDRH = (u8)((u16)&rippleBuf[0] >> 8);
        HDMA6_ADDRB = 0x7E;  /* WRAM bank */
        REG_HDMAEN  = 0x40;  /* enable channel 6 */
    } else {
        REG_HDMAEN = 0x00;   /* no HDMA channels active */
    }
    VBD_END();
}

/* Breathing outlines: all six neon line colors ease toward white and back
 * on a slow sine - pure palette animation, the SNES stand-in for bloom. */
static const u8 breathTab[32] = {0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 4, 5, 5, 5, 5, 5,
                                 5, 5, 5, 5, 4, 4, 4, 3, 3, 2, 2, 1, 1, 0, 0, 0};

void linePulse(u8 frame) {
    u8 c, t;
    if (frame & 3) return; /* breathTab index moves every 4 frames: the other
                            * 3 recompute (6 lerps) + re-DMA identical colors */
    t = breathTab[(frame >> 2) & 31]; /* ~2.1s period, max 5/16 */
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
    u8 cols[6], real[6];
    u8 i, c;

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
}

/* Blank the ring's BG cells. Called a couple of ticks AFTER the sprites are
 * up: frame 0 is pixel-identical to the static board, so the swap happens
 * invisibly underneath the cluster - no ordering race can flash black
 * (bsnes showed one frame of bare blanked cells when both changed at once). */
void spinAnimBlank(u8 k, u8 j) {
    u8 ringTri[6];
    u8 i, q;
    u16 o;

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
            mapWrite((u16)ty * 32 + BOARD_TILE_X + tx, e);
        }
    }
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

void renderHexImpact(u8 cascN, u8 cascDepth) {
    u8 amp    = (u8)(cascDepth > 2 ? 3 : cascDepth > 1 ? 2 : 1);
    u8 frames = (u8)(cascDepth > 2 ? 14 : cascDepth > 1 ? 10 : 6);
    shakeStart(amp, frames);
    bg2Jolt = (s8)(cascN > 1 ? -10 : -6);
    /* Mosaic burst: big pixelation that geometrically decays to 0 */
    if (mosVal == 0) {
        mosVal    = (u8)(cascDepth > 2 ? 12 : cascDepth > 1 ? 10 : 8);
        mosImpactT = 5;
    }
    /* White flash: 3-frame bright-to-dim via color math add */
    flashStep = (u8)(cascN > 1 || cascDepth > 1 ? 9 : 7); /* ×3 = 27 or 21 max */
    flashT    = 3;
    /* HDMA shockwave ripple: expanding ring of per-scanline BG1 H-scroll offsets */
    rippleT   = 10;
    rippleRad = 0;
}

/* Pre-fill the HDMA table: all count=1 entries with 0-offset. rippleFrame()
 * only updates the lo/hi bytes, so the count bytes never change after init. */
static void rippleInit(void) {
    u8 *p = rippleBuf;
    u16 i;
    for (i = 0; i < 239; i++, p += 3) {
        p[0] = 1;  /* HDMA line count: apply to 1 scanline */
        p[1] = 0;  /* BG1HOFS lo = 0 (no scroll) */
        p[2] = 0;  /* BG1HOFS hi = 0 */
    }
    *p = 0; /* end-of-table marker */
}

/* Called once per game-loop frame (outside vblank). Fills rippleBuf with the
 * current ring frame's per-scanline H-scroll offsets, then advances the ring.
 * Rows above screen-center y=120 shift LEFT (positive scroll), rows below
 * shift RIGHT (negative 10-bit scroll = 1024-n), creating an outward push. */
void rippleFrame(void) {
    u8 *p;
    u8 amp, rad, y;
    if (!rippleT) return;
    amp = (u8)((rippleT + 1) >> 1); /* 5,5,4,4,3,3,2,2,1,1 over 10 frames */
    rad = rippleRad;
    p   = rippleBuf;
    for (y = 0; y < 239; y++, p += 3) {
        u8 dist = (y < 120) ? (u8)(120 - y) : (u8)(y - 120);
        u8 lo = 0, hi = 0;
        if (dist >= rad) {
            u8 rd = dist - rad;
            if (rd < 8) {
                u8 av = (rd < amp) ? (u8)(amp - rd) : 0; /* taper to 0 at ring edge */
                if (av) {
                    if (y < 120) {
                        lo = av;  /* positive scroll: content shifts left */
                    } else {
                        u16 sv = (u16)(1024 - (u16)av); /* negative 10-bit: shifts right */
                        lo = (u8)(sv & 0xFF);
                        hi = (u8)((sv >> 8) & 0x03);
                    }
                }
            }
        }
        p[1] = lo;
        p[2] = hi;
    }
    rippleT--;
    rippleRad = (u8)(rippleRad + 6);
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

/* Ambient sky: one wide sprite at a time crossing BEHIND the board (OBJ
 * priority 2, under BG1's priority-1 tiles, over the backdrop). Two kinds:
 * a long ship (5 OBJ-cols = 80px, horizontal LTR/RTL) and the voyager probe
 * (3 OBJ-cols = 48px, gentle downward diagonal). They never coexist, so they
 * SHARE one 5x2 OBJ-tile region (tiles 386/402/484/500); the active sprite's
 * 1280 bytes are DMA'd in at spawn (ambTileDirty -> renderVBlank), and its
 * 11-color palette swapped into OBJ pal0 indices 5-15 (CGRAM 133-143).
 *
 * Layout (OAM slots 23-32 = logo particles, unused during PLAY):
 *   top row  OBJs -> slots 23-27, tiles 386 388 390 392 394
 *   bottom   OBJs -> slots 28-32, tiles 484 486 488 490 492
 * Coordinates: 9.7 fixed point biased by AMB_BIAS so every value stays u16
 * (sprite up to 80px wide -> off-left spawn at screen -80 = bx 16 > 0). */
#define AMB_BIAS 96

/* The flyer redraws all 10 OBJs every frame, so it writes oamMemory (plain
 * WRAM, DMA'd to OAM each vblank by pvsneslib) directly from fast-bank code
 * instead of paying ~4 slow-bank lib jsl's per column (oamSet + oamSetEx +
 * the X8 fixup). Low table, 4 bytes per sprite at id = s*4: x low 8, y,
 * tile low 8, attr (vhoopppN = vflip<<7 | hflip<<6 | prio<<4 | pal<<1 |
 * tile bit 8). Every flyer tile is >=256, prio 2, pal 0 -> attr 0x21/0x61.
 * High table at oamMemory[512+s/4], 2 bits per sprite: bit (s%4)*2 = X8
 * (set -> the low x byte is a 9-bit negative; how partial-left columns
 * avoid wrapping to x&0xFF on the right), bit (s%4)*2+1 = size (0 = 16x16).
 * Tables instead of (1 << ((s&3)<<1)): tcc variable shifts are lib calls. */
static const u8 ambTopTileLow[5] = {130, 132, 134, 136, 138}; /* (386+2i)&0xFF */
static const u8 ambBotTileLow[5] = {228, 230, 232, 234, 236}; /* (484+2i)&0xFF */
static const u8 ambRow3TileLow[3] = {234, 236, 238};          /* (490+2i)&0xFF; 3-row sprites */
static const u8 oamHiClr[4] = {0xFC, 0xF3, 0xCF, 0x3F}; /* ~(3 << (s&3)*2) */
static const u8 oamHiX8[4] = {0x01, 0x04, 0x10, 0x40};  /*  1 << (s&3)*2  */

static void ambOamPut(u8 s, u8 x, u8 y, u8 tileLow, u8 attr, u8 x8) {
    u16 id = (u16)s << 2;
    u16 b = (u16)(512 + (s >> 2));
    oamMemory[id] = x;
    oamMemory[id + 1] = y;
    oamMemory[id + 2] = tileLow;
    oamMemory[id + 3] = attr;
    oamMemory[b] = (u8)((oamMemory[b] & oamHiClr[s & 3]) | (x8 ? oamHiX8[s & 3] : 0));
}

/* Same parked-off-screen state the lib's OBJ_HIDE uses: X8=1 with x=1 puts
 * the sprite at -255, y=240 is below even the overscan window. */
static void ambOamHide(u8 s) {
    u16 id = (u16)s << 2;
    u16 b = (u16)(512 + (s >> 2));
    oamMemory[id] = 1;
    oamMemory[id + 1] = 240;
    oamMemory[b] = (u8)((oamMemory[b] & oamHiClr[s & 3]) | oamHiX8[s & 3]);
}

static void ambHideAll(void) {
    u8 s;
    for (s = 23; s <= 32; s++) ambOamHide(s);
}

void ambientFrame(void) {
    u16 bx, by, sx, sy;
    u8 i, col;
    if (!ambOn) {
        if (ambCool) { ambCool--; return; }
        /* Pick a random sprite type (rejection-sampled - no signed modulo). */
        do { ambType = (u8)(rngNext() & AMB_PICK_MASK); } while (ambType >= AMB_SPRITE_N);
        ambFlip = rngNext() & 1;       /* RTL when set */
        ambCols = ambSpriteCols[ambType];
        /* Enter fully off-screen on the chosen side (sprite up to 80px wide).
         * Y stays in the board band (screen >=80) so flyers never ride up into
         * the HUD panels; the board top is at screen 76. */
        ambX = (u16)(ambFlip ? AMB_BIAS + 256 : AMB_BIAS - 80) << 7;
        if (ambSpriteMotion[ambType]) {
            /* 'D': slow gentle downward diagonal - bounded, never exits edges */
            ambY = (u16)(AMB_BIAS + 84 + (rngNext() & 31)) << 7;  /* screen 84..115 */
            ambDX = ambFlip ? -0x0040 : 0x0040; /* 0.5 px/f */
            ambDY = 0x0010;                      /* 0.125 px/f down */
        } else {
            /* 'H': straight horizontal pass at a random board height */
            ambY = (u16)(AMB_BIAS + 80 + (rngNext() & 95)) << 7; /* screen 80..175 */
            ambDX = ambFlip ? -0x0030 : 0x0030; /* ~0.375 px/f */
            ambDY = 0;
        }
        /* Stage the tile + palette swap (applied in renderVBlank pre-display). */
        {
            u8 k;
            u8 *src = (u8 *)&ambient_pic + AMB_PAL_OFF + (u16)ambType * AMB_PAL_BYTES;
            for (k = 0; k < 22; k++) ambPalBuf[k] = src[k];
            ambPalDirty = 1;
            ambRows = ambSpriteRows[ambType];
            ambTotalStrips = ambRows << 1;
            ambTileStep = ambTotalStrips;
        }
        ambOn = 1;
        return;
    }
    /* Hold position (still fully off-screen, columns hidden) until all four
     * tile rows are resident - moving sooner showed the previous flyer's
     * tiles on the leading columns. */
    if (ambTileStep) return;
    ambX += (u16)ambDX;
    ambY += (u16)ambDY;
    bx = ambX >> 7;
    by = ambY >> 7;
    /* exit off either horizontal edge (u16 wrap makes a left-exit a huge bx) */
    if (bx < AMB_BIAS - 80 || bx > AMB_BIAS + 256 || by > AMB_BIAS + 232) {
        ambOn = 0;
        ambCool = 240 + (rngNext() & 255); /* ~4-8s of empty sky between flyers */
        ambHideAll();
        return;
    }
    /* No per-scanline suppression: one flyer plus the spin cluster (~8),
     * a cascade's spread-out pulses (~8-12 on any line), cursor and sparks
     * all stay under the 34-tile cap (3-row sprite = 6 tiles/line). */
    sy = (u16)(by - AMB_BIAS);
    /* RTL (flip=1) reverses art columns. Show a column when its 16px span
     * touches the screen: cx in [-15..255]. cx>255 are hidden (the SNES
     * can't show them right; they'd wrap). Direct oamMemory writes. */
    {
        s16 baseX = (s16)bx - AMB_BIAS;
        u8 attr = (u8)(ambFlip ? 0x61 : 0x21); /* prio 2, pal 0, tile bit8 */
        if (ambRows == 3) {
            /* 3-row sprite: up to 3 cols x 3 rows, OBJ slots 23-31 */
            for (i = 0; i < 3; i++) {
                s16 cx = baseX + (s16)(i << 4);
                u8 s1 = (u8)(23 + i);
                u8 s2 = (u8)(26 + i);
                u8 s3 = (u8)(29 + i);
                if (i < ambCols && cx > -16 && cx <= 255) {
                    u8 neg = (cx < 0);
                    col = ambFlip ? (u8)(ambCols - 1 - i) : i;
                    ambOamPut(s1, (u8)cx, (u8)sy,        ambTopTileLow[col],  attr, neg);
                    ambOamPut(s2, (u8)cx, (u8)(sy + 16), ambBotTileLow[col],  attr, neg);
                    ambOamPut(s3, (u8)cx, (u8)(sy + 32), ambRow3TileLow[col], attr, neg);
                } else {
                    ambOamHide(s1); ambOamHide(s2); ambOamHide(s3);
                }
            }
            ambOamHide(32);
        } else {
            /* 2-row sprite: up to 5 cols x 2 rows, OBJ slots 23-32 */
            for (i = 0; i < 5; i++) {
                s16 cx = baseX + (s16)(i << 4);
                u8 sTop = (u8)(23 + i);
                u8 sBot = (u8)(28 + i);
                if (i < ambCols && cx > -16 && cx <= 255) {
                    u8 neg = (cx < 0);
                    col = ambFlip ? (u8)(ambCols - 1 - i) : i;
                    ambOamPut(sTop, (u8)cx, (u8)sy,        ambTopTileLow[col], attr, neg);
                    ambOamPut(sBot, (u8)cx, (u8)(sy + 16), ambBotTileLow[col], attr, neg);
                } else {
                    ambOamHide(sTop);
                    ambOamHide(sBot);
                }
            }
        }
    }
}

/* Backdrop star twinkle: the brightest backdrop palette slots breathe.
 * Staged here, DMA'd in renderVBlank. */
void twinkleFrame(u8 frame) {
    u8 i, t;
    if (frame & 1) return; /* indices move every 2 frames; skip the identical half */
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

void mosaicSet(u8 size) {
    mosVal = size;
}

void bg1ShiftSet(u8 px) {
    bg1Shift = px;
}

/* Stage a main-screen layer mask (applied at the next vblank start, so the
 * switch never tears mid-frame). Stage transitions drop BG1 (0x16) while the
 * stats panel sits over the new backdrop - the dissolved board parked at
 * mosaic 15 otherwise reads as a big pixel blob - and restore 0x17 for the
 * mosaic reveal. */
void layersSet(u8 tm) {
    tmStage = tm;
}

void hudDigits(u8 x, u8 y, const u8 *d, u8 n) {
    u8 i;
    u16 *p = &hudMap[(u16)y * 32 + x];
    for (i = 0; i < n; i++) {
        p[i] = (u16)(HUD_FONT_TILE + '0' - 32 + d[n - 1 - i]) | HUD_ATTR;
    }
    hudDirty = 1;
}

void hudClear(void) {
    u16 i;
    for (i = 0; i < 32 * 32; i++) hudMap[i] = 0;
    barPx = 0xFF;
    hudDirty = 1;
}

void hudText(u8 x, u8 y, const char *s) {
    u16 *p = &hudMap[(u16)y * 32 + x];
    u8 c;
    while (*s) {
        c = (u8)*s++;
        if (c >= 'a' && c <= 'z') c -= 32; /* font is ASCII 32..95: fold case */
        *p++ = (u16)(HUD_FONT_TILE + c - 32) | HUD_ATTR;
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
    u16 *row = &hudMap[32 + HUD_BAR_X]; /* row 1: bsnes crops overscan row 0 */
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

/* Demo heat bar for the how-to pages: the live bar's tiles at an arbitrary
 * map cell (hudBarSet is pinned to row 1 and caches barPx). */
void hudBarDemo(u8 x, u8 y, u8 w, u8 px) {
    u8 col, full, rem;
    u16 *row = &hudMap[(u16)y * 32 + x];
    full = px >> 3;
    rem = px & 7;
    for (col = 0; col < w; col++) {
        u8 lvl = (col < full) ? 8 : (col == full ? rem : 0);
        row[col] = (u16)(BAR_BASE + lvl) | HUD_ATTR;
    }
    hudDirty = 1;
}

void heatColorSet(u16 bgr) {
    heatColor = bgr;
    heatColorDirty = 1;
}

/* Phase-color dots: sprites 17..22 inside the PHASE panel value row.
 * 16x16 OBJ at tile 398 (high-bank cols 14-15), resident outside the shared
 * ambient region so the flyer's spawn-time tile DMA never disturbs it. */
#define DOT_TILE 398

void hudDots(u8 n) {
    u8 i;
    for (i = 0; i < 6; i++) {
        u16 id = (u16)(17 + i) * 4;
        if (i < n) {
            /* dot center tracks the 8px HUD scroll shift */
            oamSet(id, (u16)(22 + 8 * i), 46, 3, 0, 0, DOT_TILE, (u8)(2 + i));
            oamSetEx(id, OBJ_SMALL, OBJ_SHOW);
        } else {
            oamSetVisible(id, OBJ_HIDE);
        }
    }
}

/* Panel value-row writers: 3px-lowered glyphs across map rows 4 and 5. */
static void hudValGlyph(u8 x, u8 g) {
    hudMap[5 * 32 + x] = (u16)(HUD_SHIFT_TILE + g) | HUD_ATTR;
    hudMap[6 * 32 + x] = (u16)(HUD_SHIFT_TILE + 11 + g) | HUD_ATTR;
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

static u8 cursorHidden;

void renderCursorHide(u8 hide) {
    cursorHidden = hide;
    if (hide) oamSetVisible(OAM_CURSOR, OBJ_HIDE);
}

void cursorUpdate(u8 k, u8 j, u8 frame) {
    u16 x = VTX_PX_X(k) - 8;
    u16 y = VTX_PX_Y(j) - 8 - 1; /* OAM sprites display one line low */
    if (cursorHidden) return;
    oamSet(OAM_CURSOR, x, y, 3, 0, 0, CURSOR_TILE, 0);
    oamSetEx(OAM_CURSOR, OBJ_SMALL, OBJ_SHOW);
    /* gentle blink: ~0.7s on, ~0.2s off */
    if ((frame & 63) > 51) oamSetVisible(OAM_CURSOR, OBJ_HIDE);
}

/* Logo landing burst + settled sparkles (ported from deadfall-snes).
 * Particles: 24 pool, sprites 23..46 (OAM ids 92..184).
 * Sparkles: 28 pool, sprites 47..74 (OAM ids 188..296).
 * Both draw with logo_dot_tiles (tile 392, cursor palette 0). */
#define LOGO_PART_BASE  92   /* OAM byte offset for first particle sprite */
#define LOGO_SPARK_BASE 188  /* OAM byte offset for first sparkle sprite */
#define LOGO_DOT_TILE   392  /* 16x16: TL/TR at 392, BL/BR at 408 */
#define MAX_LPART  24
#define MAX_LSPARK 28

static u8 lp_rng;
static u8 lp_rand(void) { lp_rng = (u8)(lp_rng * 37 + 17); return lp_rng; }

/* 32 points on the logo perimeter ellipse, centre (128,112), ~76x30. */
static const s8 spark_ring[64] = {
     76,  0,  75,  6,  70, 11,  63, 17,  54, 21,  42, 25,  29, 28,  15, 29,
      0, 30, -15, 29, -29, 28, -42, 25, -54, 21, -63, 17, -70, 11, -75,  6,
    -76,  0, -75, -6, -70,-11, -63,-17, -54,-21, -42,-25, -29,-28, -15,-29,
      0,-30,  15,-29,  29,-28,  42,-25,  54,-21,  63,-17,  70,-11,  75, -6
};

static struct { s16 x, y, vx, vy; u8 life; }          lpart[MAX_LPART];
static struct { s16 x, y, vx, vy; u8 life, maxlife; } lspark[MAX_LSPARK];

void logoSpriteReset(void) {
    u8 i;
    lp_rng = 0x9D;
    for (i = 0; i < MAX_LPART; i++) {
        lpart[i].life = 0;
        oamSetVisible((u16)(LOGO_PART_BASE + i * 4), OBJ_HIDE);
    }
    for (i = 0; i < MAX_LSPARK; i++) {
        lspark[i].maxlife = 0;
        oamSetVisible((u16)(LOGO_SPARK_BASE + i * 4), OBJ_HIDE);
    }
}

void logoBurst(s16 cx, s16 cy, u8 n) {
    u8 i, k;
    for (k = 0; k < n; k++) {
        for (i = 0; i < MAX_LPART; i++) if (!lpart[i].life) break;
        if (i >= MAX_LPART) return;
        lpart[i].x  = (s16)((cx + (s16)(lp_rand() % 130) - 65) << 8);
        lpart[i].y  = (s16)(cy << 8);
        lpart[i].vx = (s16)((s16)(lp_rand() % 160) - 80);
        lpart[i].vy = (s16)(-(s16)((lp_rand() % 110) + 40));
        lpart[i].life = (u8)(22 + (lp_rand() % 18));
    }
}

void logoParticlesUpdate(void) {
    u8 i;
    for (i = 0; i < MAX_LPART; i++) {
        u16 slot = (u16)(LOGO_PART_BASE + i * 4);
        if (lpart[i].life) {
            lpart[i].vy = (s16)(lpart[i].vy + 13);
            lpart[i].x  = (s16)(lpart[i].x + lpart[i].vx);
            lpart[i].y  = (s16)(lpart[i].y + lpart[i].vy);
            lpart[i].life--;
            if (lpart[i].life > 6 || (lpart[i].life & 1)) {
                oamSet(slot, (u16)((lpart[i].x >> 8) - 8), (u16)((lpart[i].y >> 8) - 8),
                       2, 0, 0, (u16)LOGO_DOT_TILE, 0);
                oamSetEx(slot, OBJ_SMALL, OBJ_SHOW);
            } else oamSetVisible(slot, OBJ_HIDE);
        } else oamSetVisible(slot, OBJ_HIDE);
    }
}

void logoSparklesUpdate(u8 spawn) {
    u8 i, s;
    for (s = 0; s < spawn; s++) {
        for (i = 0; i < MAX_LSPARK; i++) if (!lspark[i].maxlife) break;
        if (i >= MAX_LSPARK) break;
        {
            s16 sx, sy;
            if ((lp_rand() % 5) < 2) {
                sx = (s16)(128 + (s16)(lp_rand() % 120) - 60);
                sy = (s16)(112 + (s16)(lp_rand() % 36) - 18);
            } else {
                u8 a = (u8)((lp_rand() & 31) * 2);
                sx = (s16)(128 + (s16)(s8)spark_ring[a]     + (s16)(lp_rand() % 9) - 4);
                sy = (s16)(112 + (s16)(s8)spark_ring[a + 1] + (s16)(lp_rand() % 9) - 4);
            }
            lspark[i].x = (s16)(sx << 8); lspark[i].y = (s16)(sy << 8);
            lspark[i].vx = (s16)((s16)(lp_rand() % 48) - 24);
            lspark[i].vy = (s16)((s16)(lp_rand() % 48) - 24 - 16);
            lspark[i].life = 0;
            lspark[i].maxlife = (u8)(8 + (lp_rand() % 14));
        }
    }
    for (i = 0; i < MAX_LSPARK; i++) {
        u16 slot = (u16)(LOGO_SPARK_BASE + i * 4);
        u16 ml = lspark[i].maxlife, lf;
        if (!ml) { oamSetVisible(slot, OBJ_HIDE); continue; }
        lspark[i].x = (s16)(lspark[i].x + lspark[i].vx);
        lspark[i].y = (s16)(lspark[i].y + lspark[i].vy);
        lf = ++lspark[i].life;
        if (lf >= ml) { lspark[i].maxlife = 0; oamSetVisible(slot, OBJ_HIDE); continue; }
        if ((u16)(lf * 20) >= (u16)(ml * 3) && (u16)(lf * 20) <= (u16)(ml * 13)) {
            oamSet(slot, (u16)((lspark[i].x >> 8) - 8), (u16)((lspark[i].y >> 8) - 8),
                   2, 0, 0, (u16)LOGO_DOT_TILE, 0);
            oamSetEx(slot, OBJ_SMALL, OBJ_SHOW);
        } else oamSetVisible(slot, OBJ_HIDE);
    }
}

/* WRAM is NOT zeroed at power-on and tcc doesn't clear BSS: every runtime
 * static boots as garbage. A garbage shakeT/shakeAmp made the board jitter
 * wildly for seconds on the FIRST cold boot only (warm restarts inherit
 * zeroes). Called once from renderInit. */
static void renderZeroState(void) {
    u8 i;
    mapDirty = 0;
    mapRowLo = 0;
    mapRowHi = 31;
    hudDirty = 0;
    objPalDirty = 0;
    glowDirty = 0;
    lineDirty = 0;
    twDirty = 0;
    twSet = 0;
    dotPalDirty = 0;
    heatColorDirty = 0;
    blinkDirty = 0;
    mosVal = 0;
    shakeT = 0;
    shakeAmp = 0;
    pulseN = 0;
    spkCool = 0;
    for (i = 0; i < SPARK_N; i++) spkOn[i] = 0;
    ambOn = 0;
    ambType = 0;
    ambRows = 0;
    ambTotalStrips = 0;
    ambPalDirty = 0;
    ambTileStep = 0;
    tmStage = 0;
    ambCool = 60;
    bg2X = 0;
    bg2Y = 0;
    bg2Jolt = 0;
    mosImpactT = 0;
    flashT     = 0;
    flashStep  = 0;
    rippleT    = 0;
    rippleRad  = 0;
    barPx = 0xFF;
    cursorHidden = 0;
    sceneMode = 0;
#if RENDER_VBLANK_DIAG
    renderVBlankLastBytes = 0;
    renderVBlankWorstBytes = 0;
    renderVBlankLastLargeTransfers = 0;
    renderVBlankFlags = 0;
#endif
}
