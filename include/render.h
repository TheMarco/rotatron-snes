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
#define VRAM_BG1_TILES 0x1000
#define VRAM_OBJ_TILES 0x6000

/* Board placement: the generated region is 208x184 (8px margin around the
 * 192x168 board for rim axis pins). Region top-left lands at screen (24,20)
 * so the board proper sits at (32,28), centered. X via tilemap col 3; Y via
 * BG1 scroll: screen line n shows map line n + VOFS + 1 -> VOFS = -21. */
#define BOARD_TILE_X 3
#define BOARD_PX_X 32
#define BOARD_PX_Y 28
#define BOARD_VOFS ((u16)(0x400 - 21))

#define VTX_PX_X(k) (BOARD_PX_X + ((k) << 4))
#define VTX_PX_Y(j) (BOARD_PX_Y + (j) * 24)

#define OAM_CURSOR 0
#define CURSOR_TILE 384 /* OBJ tile after the 6 spin frames (384 tiles) */

void renderInit(void);
void boardRebuildMap(void);   /* boardColor -> mapBuf (full rebuild) */
void renderVBlank(void);      /* call right after WaitForVBlank: DMA dirty map */
void cursorUpdate(u8 k, u8 j, u8 frame);

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

#endif
