/* Cursor navigation over the 37 valid vertices + CW/CCW spins.
 * D-pad: nearest valid vertex in the pressed direction (same row/column
 * preferred, falls back to diagonals so edge vertices stay reachable).
 * A or R = spin CW, B or L = spin CCW. */
#include <snes.h>
#include "core.h"
#include "boardtab.h"
#include "render.h"
#include "game.h"

u8 curK, curJ;

static u8 animTick; /* 0xFF = no spin in flight */
static u8 animCcw, animK, animJ;

void gameInit(void) {
    curK = 6;
    curJ = 3; /* board center vertex */
    animTick = 0xFF;
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
    /* A spin in flight owns the frame: advance the eased schedule, then bake
     * the carry into the board (the rotation IS the state change). */
    if (animTick != 0xFF) {
        spinAnimFrame(animK, animJ, animCcw, spinSched[animTick]);
        animTick++;
        if (animTick >= SPIN_TICKS) {
            spinAnimEnd();
            spinApply(animK, animJ, animCcw);
            boardRebuildMap();
            animTick = 0xFF;
        }
        return;
    }

    if (pressed & KEY_UP) moveCursor(0);
    if (pressed & KEY_DOWN) moveCursor(1);
    if (pressed & KEY_LEFT) moveCursor(2);
    if (pressed & KEY_RIGHT) moveCursor(3);

    if (pressed & (KEY_A | KEY_R | KEY_B | KEY_L)) {
        if (vertexValid[curJ][curK]) {
            animCcw = (pressed & (KEY_B | KEY_L)) ? 1 : 0;
            animK = curK;
            animJ = curJ;
            spinAnimBegin(animK, animJ, animCcw);
            spinAnimFrame(animK, animJ, animCcw, 0);
            animTick = 0;
        }
    }
}
