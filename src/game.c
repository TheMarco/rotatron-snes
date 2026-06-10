/* Cursor navigation over the 37 valid vertices + CW/CCW spins.
 * D-pad: nearest valid vertex in the pressed direction (same row/column
 * preferred, falls back to diagonals so edge vertices stay reachable).
 * A or R = spin CW, B or L = spin CCW. */
#include <snes.h>
#include "core.h"
#include "boardtab.h"
#include "render.h"
#include "game.h"
#include "audio.h"
#include "audio_sfx.h"
#include "res/soundbank.h"

u8 curK, curJ;

static u8 animTick; /* 0xFF = no spin in flight */
static u8 animCcw, animK, animJ;

/* Heat = the run timer (port of state.js, Q15: 32768 = full).
 * Drain: 0.00886/s (phase-1 multiplier = 1) -> 4.84/frame, accumulated in
 * 1/256ths so no fraction is lost. Gain: 0.18*n^2 per cleared wave. */
#define HEAT_FULL 32768U
#define HEAT_DRAIN_256 1239 /* 32768 * 0.00886 / 60 * 256 */
#define HEAT_DANGER 9830    /* 0.3 */
static u16 heat, heatAcc;
static u8 gameOver;
static u16 hexCount;
static u16 entropy; /* free-running; seeds the RNG on restart */

static void heatGain(u8 n) {
    /* 0.18*32768 = 5898; n>=3 saturates anyway */
    u16 g = (n == 1) ? 5898 : (n == 2) ? 23592 : 32767;
    heat = (heat > HEAT_FULL - g) ? HEAT_FULL : heat + g;
}

/* Phases (state.js): 1..4, +1 active color each; the phase's NEW color is
 * color index phase+1 and scores double. Banner pauses heat + input. */
static u8 phase, activeColors;
static u8 bannerTick; /* >0: PHASE N announcement in progress */
static const u16 drain256ByPhase[5] = {0, 1239, 1512, 1784, 2057}; /* +22%/phase */

/* Score (BCD) + freshness. fm10 locks at the first wave of a cascade. */
static u8 score[SCORE_DIGITS];
static u8 spinsSince, fmLocked;
static u16 longestCasc;

/* HEAT: bar on row 0; three bordered panels 8px below (rows 2..5) with a
 * 1-col margin each side (web HUD layout):
 *   PHASE: digit + color dots | SCORE: 8 digits | HEXES: count + X## */
static void hudRefresh(void) {
    hudText(1, 0, "HEAT:");
    hudBox(1, 2, 9, 4);
    hudBox(11, 2, 10, 4);
    hudBox(21, 2, 10, 4);
    hudText(3, 3, "PHASE");
    hudText(13, 3, "SCORE");
    hudText(23, 3, "HEXES");
    hudValNum(2, phase, 1);
    hudScore(score);
    hudValNum(22, hexCount, 4);
    hudValX(27);
    hudValNum(28, (u16)longestCasc, 2);
    hudDots(activeColors);
}

/* Cascade clear animation (port of resolveCompletions' visual beat):
 *   GLOW  14 ticks: white-hot flash easing into the hex's neon (one CGRAM
 *                   entry animated per frame), shockwave ring + spokes
 *   FADE  12 ticks: glow ramps to black (pins stay); echo ring + shake;
 *                   refill colors at the end
 *   IN     8 ticks: 3-tick white pop, then the new colors land
 * then re-check: chained completions loop with cascade depth++ (max 50). */
#define CASC_IDLE 0
#define CASC_GLOW 1
#define CASC_FADE 2
#define CASC_IN 3
#define GLOW_TICKS 14
#define FADE_TICKS 12
#define WHITE_BGR 0x7FFF

static u8 cascState, cascTick, cascDepth;
static u8 cascN;
static u8 cascHexK[19], cascHexJ[19];
static u8 affMask[N_TRIANGLES];
static u16 cascNeon; /* glow target: the cleared hex's line color */

static void setAffDisp(u8 disp) {
    u8 t;
    for (t = 0; t < N_TRIANGLES; t++) {
        if (affMask[t]) {
            triDisp[t] = disp;
            triRefresh(t);
        }
    }
}

/* Detect completions; start a wave if any. Returns wave size. */
static u8 cascadeCheck(void) {
    u8 hc[19], i, q, t;
    cascN = findCompletedHexes(cascHexK, cascHexJ, hc);
    if (!cascN || cascDepth >= 50) {
        if (cascDepth) spinsSince = 0; /* a clearing cascade resets freshness */
        cascState = CASC_IDLE;
        cascDepth = 0;
        /* phase advance is only checked after the whole cascade settles */
        {
            u8 target = (hexCount >= PHASE_T4) ? 4 : (hexCount >= PHASE_T3) ? 3
                        : (hexCount >= PHASE_T2) ? 2 : 1;
            if (target > phase && phase < 4) {
                static const u8 phaseMod[5] = {0, MOD_MUSIC_LEVEL1, MOD_MUSIC_LEVEL2,
                                               MOD_MUSIC_LEVEL3, MOD_MUSIC_LEVEL4};
                phase = target;
                activeColors = 2 + phase;
                phantomReseed(activeColors); /* new color flows in from the rim */
                heat = (heat > HEAT_FULL - 13107) ? HEAT_FULL : heat + 13107; /* +0.4 */
                audioSfx(SFX_LEVELUP);
                hudText(12, 12, "PHASE");
                hudNum(18, 12, phase, 1);
                bannerTick = 96; /* ~1.6s: input + heat paused, like the web */
                hudRefresh();
                /* theme swap: music load freezes a visible frame (fine), the
                 * backdrop needs one blanked frame for its 32KB DMA */
                audioPlayMusic(phaseMod[phase]);
                setScreenOff();
                bg2LoadPhase(phase);
                twinkleSelect(phase - 1);
                setScreenOn();
            }
        }
        return 0;
    }
    if (cascDepth == 0) fmLocked = fresh10(spinsSince); /* lock per tap */
    cascDepth++;
    if ((u16)cascDepth > longestCasc) longestCasc = cascDepth;

    /* Score the wave: per hex (new phase color = double), + multi-kill. */
    {
        u16 unit = scoreWaveUnit(phase, fmLocked);
        u8 newColor = (phase >= 2) ? phase + 1 : 0xFF;
        u8 sawNew = 0;
        for (i = 0; i < cascN; i++) {
            bcdAddWave(score, unit, cascDepth);
            if (hc[i] == newColor) {
                bcdAddWave(score, unit, cascDepth); /* NEW_COLOR_BONUS x2 */
                sawNew = 1;
            }
        }
        if (cascN >= 2) bcdAddWave(score, scoreBonusUnit(cascN, phase, fmLocked), cascDepth);
        if (sawNew) audioSfx(SFX_HEXAGONNEW);
    }
    for (t = 0; t < N_TRIANGLES; t++) affMask[t] = 0;
    for (i = 0; i < cascN; i++) {
        for (q = 0; q < 6; q++) {
            s8 c = (s8)cascHexK[i] + RING_DC[q];
            s8 r = (s8)cascHexJ[i] + RING_DR[q];
            affMask[triOfCell[r][c]] = 1; /* all 6 on-board by definition */
        }
    }
    cascNeon = lineBGR[hc[0]]; /* multi-color waves throb with the first hex */
    cascState = CASC_GLOW;
    cascTick = 0;
    glowSet(WHITE_BGR);
    setAffDisp(DISP_GLOW);
    pulseStart(cascN, cascHexK, cascHexJ);
    audioSfx(SFX_HEXAGON);
    if (cascDepth >= 2) audioSfx(SFX_EXTRABONUS); /* cascade escalation layer */
    heatGain(cascN);
    hexCount += cascN;
    hudRefresh();
    return cascN;
}

static void cascadeFrame(void) {
    pulseTick(cascState == CASC_GLOW ? cascTick : GLOW_TICKS + cascTick);
    switch (cascState) {
        case CASC_GLOW: {
            /* white impact easing into the hex's own neon */
            u8 t = (u8)(((u16)cascTick * 16) / (GLOW_TICKS - 1));
            glowSet(lerpBGR(WHITE_BGR, cascNeon, t));
            if (++cascTick >= GLOW_TICKS) {
                cascState = CASC_FADE;
                cascTick = 0;
                shakeStart(cascN > 1 ? 2 : 1, cascN > 1 ? 12 : 8);
            }
            break;
        }
        case CASC_FADE: {
            /* neon dims to black; the echo shockwave rides this phase */
            u8 t = (u8)(((u16)cascTick * 16) / (FADE_TICKS - 1));
            glowSet(lerpBGR(cascNeon, 0, t));
            if (++cascTick >= FADE_TICKS) {
                u8 t2;
                for (t2 = 0; t2 < N_TRIANGLES; t2++) {
                    if (affMask[t2]) boardColor[triRow[t2]][triCol[t2]] = rngColor(activeColors);
                }
                setAffDisp(DISP_WHITE); /* pop-in flash over the new colors */
                cascState = CASC_IN;
                cascTick = 0;
            }
            break;
        }
        case CASC_IN:
            if (cascTick == 3) setAffDisp(0xFF); /* reveal the new colors */
            if (++cascTick >= 8) {
                pulseEnd();
                cascadeCheck(); /* chain: next wave or back to idle */
            }
            break;
    }
}

void gameInit(void) {
    curK = 6;
    curJ = 3; /* board center vertex */
    animTick = 0xFF;
    cascState = CASC_IDLE;
    cascDepth = 0;
    heat = HEAT_FULL;
    heatAcc = 0;
    gameOver = 0;
    hexCount = 0;
    entropy = 0;
    phase = 1;
    activeColors = 3;
    bannerTick = 0;
    bcdClear(score);
    spinsSince = 0;
    fmLocked = 20;
    longestCasc = 0;
    hudRefresh();
}

/* Heat drain + bar/color update; runs every frame while alive (the web game
 * also drains during spins and cascades). */
static void heatFrame(void) {
    u16 t;
    heatAcc += drain256ByPhase[phase];
    t = heatAcc >> 8;
    heatAcc &= 0xFF;
    heat = (heat > t) ? heat - t : 0;
    hudBarSet((u8)((((heat >> 7) * 192) >> 8))); /* 24 tiles = 192 px */
    /* green when hot, through amber, to red in the danger zone */
    t = heat >> 11; /* 0..16 */
    heatColorSet(lerpBGR(0x001F, 0x03E0, (u8)t));
}

static void enterGameOver(void) {
    gameOver = 1;
    spinAnimEnd();
    pulseEnd();
    hudText(11, 12, "GAME OVER");
    hudText(9, 14, "PRESS  START");
    audioPlayMusic(MOD_MUSIC_GAMEOVER); /* blocking ~0.5s: a beat of stillness */
}

static void restartRun(void) {
    u8 t;
    rngSeed(entropy ^ (rngNext() << 1) ^ 0x1d2b); /* human-timed entropy */
    audioPlayMusic(MOD_MUSIC_LEVEL1);
    if (phase > 1) { /* back to the phase-1 theme */
        setScreenOff();
        bg2LoadPhase(1);
        twinkleSelect(0);
        setScreenOn();
    }
    boardInit(3);
    seamsInit();
    for (t = 0; t < N_TRIANGLES; t++) triDisp[t] = 0xFF;
    hudText(11, 12, "         ");
    hudText(9, 14, "            ");
    gameInit();
    boardRebuildMap();
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
    entropy += 1 + (pressed & 15); /* press timing feeds restart seeds */

    if (gameOver) {
        if (pressed & KEY_START) restartRun();
        return;
    }
    if (bannerTick) { /* PHASE N announcement: heat + input on hold */
        if (--bannerTick == 0) {
            hudText(12, 12, "       ");
        }
        return;
    }
    heatFrame();
    if (heat == 0 && animTick == 0xFF && cascState == CASC_IDLE) {
        enterGameOver();
        return;
    }

    /* A spin in flight owns the frame: advance the eased schedule, then bake
     * the carry into the board (the rotation IS the state change). */
    if (animTick != 0xFF) {
        spinAnimFrame(animK, animJ, animCcw, spinSched[animTick]);
        animTick++;
        if (animTick >= SPIN_TICKS) {
            spinAnimEnd();
            spinApply(animK, animJ, animCcw);
            ringRefresh(animK, animJ);
            animTick = 0xFF;
            if (spinsSince < 255) spinsSince++;
            cascadeCheck(); /* the spin may have completed hexes */
        }
        return;
    }

    if (cascState != CASC_IDLE) {
        cascadeFrame();
        return; /* input locked while clears resolve */
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
            audioSfx(SFX_TURNWHEEL);
        }
    }
}
