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

u8 curK, curJ;

static u8 animTick; /* 0xFF = no spin in flight */
static u8 animCcw, animK, animJ;

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
#define ACTIVE_COLORS 3 /* phase 1 palette until phases land */
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
        cascState = CASC_IDLE;
        cascDepth = 0;
        return 0;
    }
    cascDepth++;
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
                    if (affMask[t2]) boardColor[triRow[t2]][triCol[t2]] = rngColor(ACTIVE_COLORS);
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
            ringRefresh(animK, animJ);
            animTick = 0xFF;
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
