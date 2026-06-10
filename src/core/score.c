#include "core.h"

/* Scoring math in integer form (rules.js):
 *   freshMult      = 1 + clamp(1000-100*(s-1), 100, 1000)/1000  -> tenths:
 *                    fm10 = clamp(21 - s, 11, 20)
 *   pointsPerHex   = 100 * fm * 2^(p-1) * 2^(c-1)
 *                  = (10 * fm10 * 2^(p-1)) << (c-1)   = waveUnit << (c-1)
 *   multiKillBonus = 100 * shape(m) * fm * 2^(p-1) * 2^(c-1)
 *                  = (shape10 * fm10 * 2^(p-1)) << (c-1)
 *   shape10(m)     = 60 (m=2) | 75 (m=3) | 80+10*(m-4) (m>=4)
 * All units fit u16 (max 80*20*8 = 12800); the runtime adds a unit to the
 * BCD score 2^min(c-1, SCORE_CASC_CAP) times, so no wide math ever runs.
 * Every product of the float formula is an exact integer, so these match
 * rules.js exactly (golden-tested) within the cascade cap. */

u8 fresh10(u8 spinsSinceLastClear) {
    u8 s = spinsSinceLastClear ? spinsSinceLastClear : 1;
    if (s >= 10) return 11;
    return (u8)(21 - s);
}

static u16 pow2p(u8 phase) {
    return (u16)1 << (phase - 1);
}

u16 scoreWaveUnit(u8 phase, u8 fm10) {
    return (u16)10 * fm10 * pow2p(phase);
}

u16 scoreBonusUnit(u8 m, u8 phase, u8 fm10) {
    u16 s10;
    if (m < 2) return 0;
    s10 = (m == 2) ? 60 : (m == 3) ? 75 : (u16)(80 + 10 * (m - 4));
    return s10 * fm10 * pow2p(phase);
}

/* score = SCORE_DIGITS base-10 digits, least significant first. */
void bcdClear(u8 *d) {
    u8 i;
    for (i = 0; i < SCORE_DIGITS; i++) d[i] = 0;
}

void bcdAdd(u8 *d, u16 amount) {
    u8 i, carry = 0;
    for (i = 0; i < SCORE_DIGITS; i++) {
        u8 v = d[i] + carry + (u8)(amount % 10);
        amount /= 10;
        carry = v >= 10;
        d[i] = carry ? v - 10 : v;
        if (!amount && !carry) return;
    }
}

/* Add unit 2^min(c-1, cap) times: the cascade doubling without wide math. */
void bcdAddWave(u8 *d, u16 unit, u8 cascade) {
    u8 e = cascade - 1;
    u16 n, i;
    if (e > SCORE_CASC_CAP) e = SCORE_CASC_CAP;
    n = (u16)1 << e;
    for (i = 0; i < n; i++) bcdAdd(d, unit);
}
