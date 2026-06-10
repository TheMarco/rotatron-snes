#include "core.h"

/* Interior seam segments: triangle edges whose BOTH triangles are on the
 * board (never the hex rim). Endpoints in vertex units (k,j). Deliberately
 * unsigned-only arithmetic and no helper calls - tcc-816 has burned us on
 * signed ops - and golden-tested against an independent JS derivation. */
u8 seamK0[MAX_SEAMS], seamJ0[MAX_SEAMS], seamK1[MAX_SEAMS], seamJ1[MAX_SEAMS];
u8 seamCount;

static void seamAdd(u8 k0, u8 j0, u8 k1, u8 j1) {
    if (seamCount >= MAX_SEAMS) return;
    seamK0[seamCount] = k0;
    seamJ0[seamCount] = j0;
    seamK1[seamCount] = k1;
    seamJ1[seamCount] = j1;
    seamCount++;
}

void seamsInit(void) {
    u8 c, r, j, k;
    seamCount = 0;
    /* horizontal: up(col=k,row=j-1) above the segment, down(col=k,row=j) below */
    for (j = 1; j < BOARD_ROWS; j++) {
        for (k = 0; k + 2 <= BOARD_COLS; k++) {
            if (((k + j) & 1) == 0) continue;
            if (boardColor[j - 1][k] != NO_CELL && boardColor[j][k] != NO_CELL)
                seamAdd(k, j, k + 2, j);
        }
    }
    /* diagonal: horizontally adjacent cells (c,r) | (c+1,r) */
    for (r = 0; r < BOARD_ROWS; r++) {
        for (c = 0; c + 1 < BOARD_COLS; c++) {
            if (boardColor[r][c] == NO_CELL || boardColor[r][c + 1] == NO_CELL) continue;
            if (((c + r) & 1) == 0) /* up | down: edge falls down-right */
                seamAdd(c + 1, r, c + 2, r + 1);
            else                    /* down | up: edge falls down-left */
                seamAdd(c + 2, r, c + 1, r + 1);
        }
    }
}
