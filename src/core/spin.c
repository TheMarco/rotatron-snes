#include "core.h"

static u8 slotOnBoard(u8 k, u8 j, u8 i, u8 *outCol, u8 *outRow) {
    s8 c = (s8)k + RING_DC[i];
    s8 r = (s8)j + RING_DR[i];
    if (c < 0 || c >= BOARD_COLS || r < 0 || r >= BOARD_ROWS) return 0;
    if (boardColor[r][c] == NO_CELL) return 0;
    *outCol = (u8)c;
    *outRow = (u8)r;
    return 1;
}

void spinGather(u8 k, u8 j, u8 outColors[6], u8 outReal[6]) {
    u8 i, c, r;
    for (i = 0; i < 6; i++) {
        if (slotOnBoard(k, j, i, &c, &r)) {
            outColors[i] = boardColor[r][c];
            outReal[i] = 1;
        } else {
            outColors[i] = phantomColor[j][k][i];
            outReal[i] = 0;
        }
    }
}

u8 spinApply(u8 k, u8 j, u8 ccw) {
    u8 cols[6], real[6], nc[6];
    u8 i, c, r;
    if (k >= VTX_COLS || j >= VTX_ROWS || !vertexValid[j][k]) return 0;
    spinGather(k, j, cols, real);
    for (i = 0; i < 6; i++) {
        nc[i] = cols[ccw ? (i + 1) % 6 : (i + 5) % 6];
    }
    for (i = 0; i < 6; i++) {
        if (real[i]) {
            slotOnBoard(k, j, i, &c, &r);
            boardColor[r][c] = nc[i];
        } else if (phantomColor[j][k][i] != NO_CELL) {
            phantomColor[j][k][i] = nc[i];
        }
    }
    return 1;
}
