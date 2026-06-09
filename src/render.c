#include <snes.h>
#include "core.h"
#include "boardtab.h"
#include "render.h"

extern char board_pic, board_picend, board_pal;
extern char cursor_pic, cursor_picend, cursor_pal;

static u16 mapBuf[32 * 32];
static u8 mapDirty;

void renderInit(void) {
    u16 i;

    consoleInit();
    setBrightness(0);
    WaitForVBlank();

    oamInit();

    /* BG1: the board. 4bpp tiles + 16-color subpalette 0. */
    dmaCopyVram((u8 *)&board_pic, VRAM_BG1_TILES, (u16)(&board_picend - &board_pic));
    setPalette((u8 *)&board_pal, 0, 16 * 2);
    bgSetGfxPtr(0, VRAM_BG1_TILES);
    bgSetMapPtr(0, VRAM_BG1_MAP, SC_32x32);
    bgSetScroll(0, 0, BOARD_VOFS);

    /* OBJ: cursor ring, 16x16 at tile 0 (TL/TR row 0, BL/BR row 1). */
    dmaCopyVram((u8 *)&cursor_pic, VRAM_OBJ_TILES, 64);
    dmaCopyVram((u8 *)&cursor_pic + 64, (u16)(VRAM_OBJ_TILES + 0x100), 64);
    setPalette((u8 *)&cursor_pal, 128, 16 * 2);
    REG_OBSEL = OBJ_SIZE16_L32 | (VRAM_OBJ_TILES >> 13);

    for (i = 0; i < 32 * 32; i++) mapBuf[i] = 0;
    mapDirty = 1;

    setMode(BG_MODE1, 0);
    videoMode = 0x11; /* BG1 + OBJ */
    REG_TM = 0x11;

    setScreenOn();
}

void boardRebuildMap(void) {
    u8 tx, ty, sid, a, b, ca, cb;
    u16 e;
    u16 *row;
    for (ty = 0; ty < BOARD_TILES_H; ty++) {
        row = &mapBuf[(u16)ty * 32 + BOARD_TILE_X];
        for (tx = 0; tx < BOARD_TILES_W; tx++) {
            sid = cellStruct[ty][tx];
            if (sid == 0xFF) {
                row[tx] = 0;
                continue;
            }
            a = cellTriA[ty][tx];
            ca = boardColor[triRow[a]][triCol[a]];
            if (structOwners[sid] == 2) {
                b = cellTriB[ty][tx];
                cb = boardColor[triRow[b]][triCol[b]];
                e = entryTable[structBase[sid] + (u16)ca * 6 + cb];
            } else {
                e = entryTable[structBase[sid] + ca];
            }
            row[tx] = e;
        }
    }
    mapDirty = 1;
}

void renderVBlank(void) {
    if (mapDirty) {
        dmaCopyVram((u8 *)mapBuf, VRAM_BG1_MAP, 0x800);
        mapDirty = 0;
    }
}

void cursorUpdate(u8 k, u8 j, u8 frame) {
    u16 x = VTX_PX_X(k) - 8;
    u16 y = VTX_PX_Y(j) - 8;
    oamSet(OAM_CURSOR, x, y, 3, 0, 0, 0, 0);
    oamSetEx(OAM_CURSOR, OBJ_SMALL, OBJ_SHOW);
    /* gentle blink: ~0.7s on, ~0.2s off */
    if ((frame & 63) > 51) oamSetVisible(OAM_CURSOR, OBJ_HIDE);
}
