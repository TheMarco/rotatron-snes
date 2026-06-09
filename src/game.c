/* Cursor navigation over the 37 valid vertices + CW/CCW spins.
 * D-pad: nearest valid vertex in the pressed direction (same row/column
 * preferred, falls back to diagonals so edge vertices stay reachable).
 * A or R = spin CW, B or L = spin CCW. */
#include <snes.h>
#include "core.h"
#include "render.h"
#include "game.h"

u8 curK, curJ;

void gameInit(void) {
    curK = 6;
    curJ = 3; /* board center vertex */
}

/* Score a candidate move: primary axis distance dominates, the off-axis
 * drift breaks ties so the cursor tracks straight lines when it can. */
static void moveCursor(u8 dir) {
    u8 k, j;
    s16 best = 0x7FFF;
    u8 bk = curK, bj = curJ;
    for (j = 0; j < VTX_ROWS; j++) {
        for (k = 0; k < VTX_COLS; k++) {
            s16 dx, dy, ax, ay, score;
            if (!vertexValid[j][k]) continue;
            dx = (s16)k - curK;
            dy = (s16)j - curJ;
            ax = dx < 0 ? -dx : dx;
            ay = dy < 0 ? -dy : dy;
            switch (dir) {
                case 0: if (dy >= 0) continue; score = ay * 8 + ax; break; /* up */
                case 1: if (dy <= 0) continue; score = ay * 8 + ax; break; /* down */
                case 2: if (dx >= 0) continue; score = ax + ay * 8; break; /* left */
                default: if (dx <= 0) continue; score = ax + ay * 8; break; /* right */
            }
            if (score < best) {
                best = score;
                bk = k;
                bj = j;
            }
        }
    }
    curK = bk;
    curJ = bj;
}

void gameFrame(u16 pressed) {
    if (pressed & KEY_UP) moveCursor(0);
    if (pressed & KEY_DOWN) moveCursor(1);
    if (pressed & KEY_LEFT) moveCursor(2);
    if (pressed & KEY_RIGHT) moveCursor(3);

    if (pressed & (KEY_A | KEY_R)) {
        if (spinApply(curK, curJ, 0)) boardRebuildMap();
    } else if (pressed & (KEY_B | KEY_L)) {
        if (spinApply(curK, curJ, 1)) boardRebuildMap();
    }
}
