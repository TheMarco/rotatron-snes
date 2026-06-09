#include "core.h"

u8 boardColor[BOARD_ROWS][BOARD_COLS];
u8 phantomColor[VTX_ROWS][VTX_COLS][6];
u8 vertexValid[VTX_ROWS][VTX_COLS];

const s8 RING_DC[6] = {-1, 0, 0, -1, -2, -2};
const s8 RING_DR[6] = {-1, -1, 0, 0, 0, -1};

/* Integer form of math.js isInsideHex: the centroid test
 *   |y| <= R*sqrt(3)/2  and  |x| + |y|/sqrt(3) <= R   (R = HEX_SIDE = 3)
 * with x,y from the triangle's corner average reduces exactly to
 *   |sj - 9| <= 9  and  3*|col - 5| + |sj - 9| <= 18
 * where sj = 3*row+2 for up triangles, 3*row+1 for down (sum of corner j's).
 * The JS epsilon (0.02) never lands between two integer outcomes, so this is
 * bit-identical to the float version (proved by the golden test). */
u8 cellInHex(u8 col, u8 row) {
    s16 sj = (s16)(3 * row) + (((col + row) & 1) == 0 ? 2 : 1);
    s16 a = sj - 9;
    s16 b = (s16)col - 5;
    if (a < 0) a = -a;
    if (b < 0) b = -b;
    if (a > 9) return 0;
    return (s16)(3 * b) + a <= 18;
}

/* Slot i of vertex (k,j): on-board cell coords, or 0 if off-board/outside hex. */
static u8 slotCell(u8 k, u8 j, u8 i, u8 *outCol, u8 *outRow) {
    s8 c = (s8)k + RING_DC[i];
    s8 r = (s8)j + RING_DR[i];
    if (c < 0 || c >= BOARD_COLS || r < 0 || r >= BOARD_ROWS) return 0;
    if (boardColor[r][c] == NO_CELL) return 0;
    *outCol = (u8)c;
    *outRow = (u8)r;
    return 1;
}

u8 boardHasCompletedHex(void) {
    u8 k, j, i, c, r;
    for (j = 0; j < VTX_ROWS; j++) {
        for (k = 0; k < VTX_COLS; k++) {
            u8 first = NO_CELL;
            u8 ok = 1;
            if (((k + j) & 1) == 0) continue;
            for (i = 0; i < 6; i++) {
                if (!slotCell(k, j, i, &c, &r)) {
                    ok = 0;
                    break;
                }
                if (first == NO_CELL) first = boardColor[r][c];
                else if (boardColor[r][c] != first) {
                    ok = 0;
                    break;
                }
            }
            if (ok) return 1;
        }
    }
    return 0;
}

void boardInit(u8 paletteLen) {
    u8 col, row, k, j, i, c, r;

    /* createBoard: row-major fill, re-roll whole board while any ring is
     * already monochrome (RNG consumption order matches the JS exactly). */
    do {
        for (row = 0; row < BOARD_ROWS; row++) {
            for (col = 0; col < BOARD_COLS; col++) {
                boardColor[row][col] = cellInHex(col, row) ? rngColor(paletteLen) : NO_CELL;
            }
        }
    } while (boardHasCompletedHex());

    /* createPhantomSlots: j-outer/k-inner vertex scan, slot idx ascending. */
    for (j = 0; j < VTX_ROWS; j++) {
        for (k = 0; k < VTX_COLS; k++) {
            u8 realCount = 0;
            vertexValid[j][k] = 0;
            for (i = 0; i < 6; i++) phantomColor[j][k][i] = NO_CELL;
            if (((k + j) & 1) == 0) continue;
            for (i = 0; i < 6; i++) {
                if (slotCell(k, j, i, &c, &r)) realCount++;
            }
            if (realCount == 0) continue;
            vertexValid[j][k] = 1;
            if (realCount == 6) continue;
            for (i = 0; i < 6; i++) {
                if (!slotCell(k, j, i, &c, &r)) phantomColor[j][k][i] = rngColor(paletteLen);
            }
        }
    }
}

void phantomReseed(u8 paletteLen) {
    u8 k, j, i;
    for (j = 0; j < VTX_ROWS; j++) {
        for (k = 0; k < VTX_COLS; k++) {
            for (i = 0; i < 6; i++) {
                if (phantomColor[j][k][i] != NO_CELL) phantomColor[j][k][i] = rngColor(paletteLen);
            }
        }
    }
}
