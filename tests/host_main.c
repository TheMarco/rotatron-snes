/* Host golden test driver: replays the exact scenario of gen_golden.mjs
 * through the C core and prints the same dump format. `diff` does the rest. */
#include <stdio.h>
#include "core.h"

static void dump(void) {
    u8 row, col, k, j, i;
    for (row = 0; row < BOARD_ROWS; row++) {
        printf("B ");
        for (col = 0; col < BOARD_COLS; col++) {
            u8 c = boardColor[row][col];
            if (c == NO_CELL) printf(".");
            else printf("%d", c);
        }
        printf("\n");
    }
    for (j = 0; j < VTX_ROWS; j++) {
        for (k = 0; k < VTX_COLS; k++) {
            u8 any = 0;
            for (i = 0; i < 6; i++)
                if (phantomColor[j][k][i] != NO_CELL) any = 1;
            if (!any) continue;
            printf("P %d %d ", k, j);
            for (i = 0; i < 6; i++) {
                u8 c = phantomColor[j][k][i];
                if (c == NO_CELL) printf(".");
                else printf("%d", c);
            }
            printf("\n");
        }
    }
}

int main(void) {
    int n;
    rngSeed(0xbeef);
    boardInit(3);
    printf("INIT\n");
    dump();

    for (n = 0; n < 400; n++) {
        u8 k, j, ccw;
        if (n == 150 || n == 250 || n == 325) {
            u8 len = (n == 150) ? 4 : (n == 250) ? 5 : 6;
            phantomReseed(len);
            printf("RESEED %d\n", len);
            dump();
        }
        do {
            k = rngNext() % 13;
            j = rngNext() % 8;
        } while (!(((k + j) & 1) == 1 && vertexValid[j][k]));
        ccw = rngNext() & 1;
        spinApply(k, j, ccw);
        printf("SPIN %d %d %d\n", k, j, ccw);
        dump();
    }
    return 0;
}
