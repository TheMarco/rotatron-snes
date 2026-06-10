#include "core.h"

/* Port of rules.js findCompletedHexes: every real vertex whose 6 ring slots
 * are all on the board and all one color. Scan order matches the JS (j outer,
 * k inner) so the golden test can diff hex lists verbatim. Max 19 hexes (one
 * per interior vertex). */
u8 findCompletedHexes(u8 *outK, u8 *outJ, u8 *outColor) {
    u8 k, j, i, n = 0;
    for (j = 0; j < VTX_ROWS; j++) {
        for (k = 0; k < VTX_COLS; k++) {
            u8 first = NO_CELL;
            u8 ok = 1;
            if (((k + j) & 1) == 0) continue;
            for (i = 0; i < 6; i++) {
                s8 c = (s8)k + RING_DC[i];
                s8 r = (s8)j + RING_DR[i];
                u8 col;
                if (c < 0 || c >= BOARD_COLS || r < 0 || r >= BOARD_ROWS) {
                    ok = 0;
                    break;
                }
                col = boardColor[r][c];
                if (col == NO_CELL) {
                    ok = 0;
                    break;
                }
                if (first == NO_CELL) first = col;
                else if (col != first) {
                    ok = 0;
                    break;
                }
            }
            if (ok) {
                outK[n] = k;
                outJ[n] = j;
                outColor[n] = first;
                n++;
            }
        }
    }
    return n;
}
