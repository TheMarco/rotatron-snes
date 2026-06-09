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

/* Board placement: 192x168 px centered on 256x224. X = 32px = 4 tiles via
 * tilemap offset; Y = 28px via BG1 vertical scroll (board map rows start
 * at 0). Screen line n shows map line n + VOFS + 1 -> VOFS = -29. */
#define BOARD_TILE_X 4
#define BOARD_PX_X 32
#define BOARD_PX_Y 28
#define BOARD_VOFS ((u16)(0x400 - 29))

#define VTX_PX_X(k) (BOARD_PX_X + ((k) << 4))
#define VTX_PX_Y(j) (BOARD_PX_Y + (j) * 24)

#define OAM_CURSOR 0

void renderInit(void);
void boardRebuildMap(void);   /* boardColor -> mapBuf (full rebuild) */
void renderVBlank(void);      /* call right after WaitForVBlank: DMA dirty map */
void cursorUpdate(u8 k, u8 j, u8 frame);

#endif
