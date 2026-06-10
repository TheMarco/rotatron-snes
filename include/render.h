#ifndef RENDER_H
#define RENDER_H

#include "core_types.h"

/* VRAM layout (word addresses)
 *   0x0000  BG1 tilemap (32x32)
 *   0x0400  BG3 tilemap (HUD, later)
 *   0x1000  BG1 board tiles (4bpp, ~103 tiles)
 *   0x2000  BG2 starfield tiles (later)
 *   0x3000  BG3 font (later)
 *   0x6000  OBJ tiles (cursor; spin animation frames later)
 */
#define VRAM_BG1_MAP 0x0000
#define VRAM_BG2_MAP 0x0400
#define VRAM_BG3_MAP 0x0800
#define VRAM_BG1_TILES 0x1000
/* BG tile bases must be 4K-word aligned: BG3 shares the 0x1000 page with
 * BG1 and the 2bpp font lives in its unused tail (board uses <=384 of the
 * 4bpp slots = up to word 0x2800... capped at 0x1C00 by the generator). */
#define VRAM_BG3_TILES 0x1000
#define VRAM_HUDFONT 0x1C00   /* 64 glyphs x 8 words */
#define VRAM_HUDBAR 0x1E00    /* 9 heat-bar tiles + 8 panel borders */
#define VRAM_HUDSHIFT 0x1E88  /* 3px-lowered 0-9,X glyphs (11 upper + 11 lower) */
#define HUD_FONT_TILE 384     /* (0x1C00 - 0x1000) / 8 */
#define HUD_BAR_TILE 448
#define HUD_SHIFT_TILE 465
#define VRAM_BG2_TILES 0x2000 /* up to 1024 tiles, ends at OBJ base */
#define VRAM_OBJ_TILES 0x6000

/* Board placement: the generated region is 208x184 (8px margin around the
 * 192x168 board for rim axis pins). Region top-left lands at screen (24,44)
 * so the board proper sits at (32,52): pushed down to clear the HUD panels,
 * 4px bottom margin. X via tilemap col 3; Y via BG1 scroll: screen line n
 * shows map line n + VOFS + 1 -> VOFS = -45. */
#define BOARD_TILE_X 3
#define BOARD_PX_X 32
/* Board pushed 16px further down at the user's request: this needs the
 * 239-line OVERSCAN mode (SETINI bit 2, play scene only) - the bottom pin
 * halos end exactly on line 239. Real CRTs crop those lines; emulators show
 * them. Region top = 60 -> VOFS = -61. */
#define BOARD_PX_Y 68
#define BOARD_VOFS ((u16)(0x400 - 61))
#define REG_SETINI (*(vuint8 *)0x2133)
#define REG_MOSAIC (*(vuint8 *)0x2106)

#define VTX_PX_X(k) (BOARD_PX_X + ((k) << 4))
#define VTX_PX_Y(j) (BOARD_PX_Y + (j) * 24)

#define OAM_CURSOR 0
#define CURSOR_TILE 384 /* OBJ tile after the 6 spin frames (384 tiles) */

void renderInit(void);
void boardRebuildMap(void);   /* boardColor -> mapBuf (full rebuild, slow: init only) */
void triRefresh(u8 t);        /* recompute one triangle's cells */
void ringRefresh(u8 k, u8 j); /* recompute the 6 on-board ring triangles */
void renderVBlank(void);      /* call right after WaitForVBlank: DMA dirty map */
void cursorUpdate(u8 k, u8 j, u8 frame);
void renderCursorHide(u8 hide);

/* Spin animation: Begin stages OBJ palette 1 with the ring's colors (CCW =
 * permuted for the H-flipped frames) and blanks the ring's BG cells, keeping
 * outside-neighbor halves of shared tiles. Frame positions the 4 32x32
 * sprites for animation frame f. End hides them; caller then applies the
 * spin to the board and rebuilds the map. */
void spinAnimBegin(u8 k, u8 j, u8 ccw);
void spinAnimFrame(u8 k, u8 j, u8 ccw, u8 f);
void spinAnimEnd(void);

/* Clear-animation helpers. triDisp overrides what a triangle displays
 * (DISP_WHITE / DISP_HIDDEN / 0xFF = its board color); rebuild after edits. */
extern u8 triDisp[];
void shakeStart(u8 amp, u8 frames);
void pulseStart(u8 n, const u8 *ks, const u8 *js);
void pulseTick(u8 tick);
void pulseEnd(void);
u16 lerpBGR(u16 a, u16 b, u8 t); /* t 0..16 */
void glowSet(u16 bgr);           /* stage the DISP_GLOW CGRAM entry */
void linePulse(u8 frame);        /* breathing neon outlines (call per frame) */
void sparksInit(void);           /* enumerate interior seams (after boardInit) */
void sparksFrame(u8 frame);      /* spawn/advance seam sparks (call per frame) */
void ambientFrame(void);         /* ships / shooting stars behind the board */
void twinkleFrame(u8 frame);     /* backdrop star palette twinkle */

/* HUD (BG3, 2bpp, top priority). Text font = ASCII 32..95. The heat bar
 * fills cols 1..30 of row 0; its color is staged per frame. */
void hudText(u8 x, u8 y, const char *s);
void hudNum(u8 x, u8 y, u16 val, u8 digits);
void hudBarSet(u8 px);     /* 0..240 filled pixels */
void heatColorSet(u16 bgr);
void hudDots(u8 n);        /* phase color dots (active color count) */
void hudScore(const u8 *d); /* SCORE_DIGITS BCD digits, LSB first */
void hudValNum(u8 x, u16 val, u8 digits); /* lowered value-row number */
void hudValX(u8 x);                       /* lowered value-row 'X' */

/* Scene plumbing (logo -> title -> play). renderInit = console boot only;
 * renderGameLoad re-uploads ALL game VRAM/CGRAM (the mode-3 scenes own
 * everything). Both load paths require the screen force-blanked. */
void renderGameLoad(void);
void sceneShow(u8 which); /* mode-3: 1 = title, 2 = logo */
void scenePinV(u16 v);    /* BG1 vscroll during scenes (logo drop) */
void sceneBlink(u16 bgr); /* CGRAM 255 = the baked PRESS START text */
void hudClear(void);
void bg2LoadPhase(u8 phase); /* per-phase backdrop; screen must be blanked */
void twinkleSelect(u8 idx);  /* twinkle palette set (phase - 1) */
void mosaicSet(u8 size);     /* BG1 mosaic 0..15, applied in vblank */
void hudDigits(u8 x, u8 y, const u8 *d, u8 n); /* BCD digits, MSB first */
void hudBox(u8 x, u8 y, u8 w, u8 h); /* 1px-bordered panel ring */

#endif
