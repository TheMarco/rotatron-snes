/* White-box host checks for game.c's runtime state machines.
 * The pure board/rules/scoring core has golden coverage; these tests cover the
 * SNES-side glue that owns heat, cascades, color suppression, and phase gates. */
#include <stdio.h>
#include "core.h"
#include "boardtab.h"

u8 triDisp[N_TRIANGLES];

#include "../src/game.c"

static u8 lastMusic;
static u8 stoppedSpc;

void setBrightness(u8 brightness) { (void)brightness; }
void setScreenOff(void) {}
void setScreenOn(void) {}
void spcStop(void) { stoppedSpc = 1; }

void audioPlayMusic(u8 mod) { lastMusic = mod; }
void audioSfx(u8 idx) { (void)idx; }
void audioSfxPitch(u8 idx, u8 pitch) { (void)idx; (void)pitch; }

void boardRebuildMap(void) {}
void triRefresh(u8 t) { (void)t; }
void ringRefresh(u8 k, u8 j) { (void)k; (void)j; }
void renderCursorHide(u8 hide) { (void)hide; }
void spinAnimBegin(u8 k, u8 j, u8 ccw) { (void)k; (void)j; (void)ccw; }
void spinAnimBlank(u8 k, u8 j) { (void)k; (void)j; }
void spinAnimFrame(u8 k, u8 j, u8 ccw, u8 f) { (void)k; (void)j; (void)ccw; (void)f; }
void spinAnimEnd(void) {}
void pulseStart(u8 n, const u8 *ks, const u8 *js) { (void)n; (void)ks; (void)js; }
void pulseTick(u8 tick) { (void)tick; }
void pulseEnd(void) {}
void glowSet(u16 bgr) { (void)bgr; }
u16 lerpBGR(u16 a, u16 b, u8 t) { (void)b; (void)t; return a; }
void shakeStart(u8 amp, u8 frames) { (void)amp; (void)frames; }
void renderHexImpact(u8 n, u8 depth) { (void)n; (void)depth; }
void mosaicSet(u8 size) { (void)size; }
void layersSet(u8 tm) { (void)tm; }
void bg2LoadPhase(u8 p) { (void)p; }
void twinkleSelect(u8 idx) { (void)idx; }
void hudClear(void) {}
void hudText(u8 x, u8 y, const char *s) { (void)x; (void)y; (void)s; }
void hudNum(u8 x, u8 y, u16 val, u8 digits) { (void)x; (void)y; (void)val; (void)digits; }
void hudDigits(u8 x, u8 y, const u8 *d, u8 n) { (void)x; (void)y; (void)d; (void)n; }
void hudBox(u8 x, u8 y, u8 w, u8 h) { (void)x; (void)y; (void)w; (void)h; }
void hudBarSet(u8 px) { (void)px; }
void heatColorSet(u16 bgr) { (void)bgr; }
void hudDots(u8 n) { (void)n; }
void hudScore(const u8 *d) { (void)d; }
void hudValNum(u8 x, u16 val, u8 digits) { (void)x; (void)val; (void)digits; }
void hudValX(u8 x) { (void)x; }

static int failures;

static void check(int cond, const char *msg) {
    if (!cond) {
        printf("FAIL: %s\n", msg);
        failures++;
    }
}

static void ringMask(u8 k, u8 j, u8 mask[BOARD_ROWS][BOARD_COLS]) {
    u8 i;
    for (i = 0; i < 6; i++) {
        s8 c = (s8)k + RING_DC[i];
        s8 r = (s8)j + RING_DR[i];
        if (c >= 0 && c < BOARD_COLS && r >= 0 && r < BOARD_ROWS && boardColor[r][c] != NO_CELL)
            mask[r][c] = 1;
    }
}

static u8 hexCountNow(void) {
    u8 hk[19], hj[19], hc[19];
    return findCompletedHexes(hk, hj, hc);
}

static void makeOneCompletedHex(u8 k, u8 j, u8 color) {
    u8 row, col, attempt, mask[BOARD_ROWS][BOARD_COLS];
    for (row = 0; row < BOARD_ROWS; row++)
        for (col = 0; col < BOARD_COLS; col++)
            mask[row][col] = 0;
    ringMask(k, j, mask);
    for (attempt = 0; attempt < 64; attempt++) {
        for (row = 0; row < BOARD_ROWS; row++) {
            for (col = 0; col < BOARD_COLS; col++) {
                if (boardColor[row][col] == NO_CELL) continue;
                boardColor[row][col] = mask[row][col]
                    ? color
                    : (u8)(1 + ((row * 5 + col * 3 + attempt) & 1));
            }
        }
        if (hexCountNow() == 1) return;
    }
    check(0, "could not construct single completed hex fixture");
}

static void testEliminationTransition(void) {
    u8 c;
    rngSeed(0x1234);
    boardInit(3);
    gameInit();
    phase = 2;
    activeColors = 4; /* yellow has been introduced but is absent in fixture */
    for (c = 0; c < 6; c++) suppress[c] = 0;
    makeOneCompletedHex(6, 3, 0);

    check(cascadeCheck() == 1, "cascade starts one directed wave");
    cascState = CASC_FADE;
    cascTick = FADE_TICKS - 1;
    cascadeFrame();

    check(suppress[0] == 4, "present color wiped by the wave is suppressed");
    check(suppress[3] == 0, "absent newly introduced color is not suppressed");
    check(elimBanner == 80, "color clear banner is shown");
}

static void testPhaseGateAfterStableCascade(void) {
    rngSeed(0x2234);
    boardInit(3);
    gameInit();
    hexCount = PHASE_T2;
    cascDepth = 1; /* means a cascade just finished and is settling */
    stageState = STG_NONE;
    pendingPhase = 0;

    check(cascadeCheck() == 0, "stable cascade returns idle");
    check(stageState == STG_FLASH, "phase threshold starts stage transition");
    check(pendingPhase == 2, "phase 2 is queued at the first threshold");
}

static void testHeatGameOverAndRestart(void) {
    rngSeed(0x3234);
    boardInit(3);
    gameInit();
    lastMusic = 0;
    stoppedSpc = 0;
    heat = 1;
    heatAcc = 0;
    animTick = 0xFF;
    cascState = CASC_IDLE;
    stageState = STG_NONE;

    gameFrame(0);
    check(gameOver == 1, "heat drain enters game over while idle");
    check(lastMusic == MOD_MUSIC_GAMEOVER, "game-over music starts");

    gameFrame(KEY_START);
    check(gameOver == 0, "START restarts from game over");
    check(phase == 1 && activeColors == 3, "restart resets phase and palette length");
    check(heat == HEAT_FULL, "restart restores full heat");
    check(stoppedSpc == 0, "restart happened before delayed SPC stop");
}

int main(void) {
    testEliminationTransition();
    testPhaseGateAfterStableCascade();
    testHeatGameOverAndRestart();
    if (failures) return 1;
    puts("GAME STATE OK");
    return 0;
}
