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

static void paintRing(u8 k, u8 j, u8 ci) {
    u8 i;
    for (i = 0; i < 6; i++) {
        s8 c = (s8)k + RING_DC[i];
        s8 r = (s8)j + RING_DR[i];
        if (c >= 0 && c < BOARD_COLS && r >= 0 && r < BOARD_ROWS && boardColor[r][c] != NO_CELL)
            boardColor[r][c] = ci;
    }
}

static void dumpHexes(void) {
    u8 hk[19], hj[19], hc[19], hn, hi;
    hn = findCompletedHexes(hk, hj, hc);
    for (hi = 0; hi < hn; hi++) printf("H %d %d %d\n", hk[hi], hj[hi], hc[hi]);
}

int main(void) {
    int n;
    u8 si;
    rngSeed(0xbeef);
    boardInit(3);
    printf("INIT\n");
    dump();
    seamsInit();
    for (si = 0; si < seamCount; si++)
        printf("E %d %d %d %d\n", seamK0[si], seamJ0[si], seamK1[si], seamJ1[si]);

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
        dumpHexes();
    }

    printf("FORCE single\n");
    paintRing(6, 3, 0);
    dump();
    dumpHexes();
    printf("FORCE pair\n");
    paintRing(4, 2, 1);
    paintRing(5, 4, 1);
    dump();
    dumpHexes();
    printf("FORCE all\n");
    {
        u8 row, col;
        for (row = 0; row < BOARD_ROWS; row++)
            for (col = 0; col < BOARD_COLS; col++)
                if (boardColor[row][col] != NO_CELL) boardColor[row][col] = 2;
    }
    dump();
    dumpHexes();

    /* Scoring grid: replay the exact runtime path (units + BCD doubling)
     * and print the decimal results for diffing against rules.js. */
    {
        u8 p, c, s, m, fm, d[SCORE_DIGITS];
        s8 i;
        char buf[SCORE_DIGITS + 1];
        for (p = 1; p <= 4; p++)
            for (c = 1; c <= 6; c++)
                for (s = 1; s <= 12; s++) {
                    fm = fresh10(s);
                    for (m = 1; m <= 6; m++) {
                        u32 hex, bonus;
                        bcdClear(d);
                        bcdAddWave(d, scoreWaveUnit(p, fm), c);
                        hex = 0;
                        for (i = SCORE_DIGITS - 1; i >= 0; i--) hex = hex * 10 + d[i];
                        bcdClear(d);
                        bcdAddWave(d, scoreBonusUnit(m, p, fm), c);
                        bonus = 0;
                        for (i = SCORE_DIGITS - 1; i >= 0; i--) bonus = bonus * 10 + d[i];
                        (void)buf;
                        printf("S %d %d %d %d %u %u\n", p, c, s, m, hex, bonus);
                    }
                }
    }
    return 0;
}
