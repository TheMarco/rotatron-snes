#include "core.h"

static u16 rngState = 1;

void rngSeed(u16 seed) {
    rngState = seed ? seed : 1;
}

u16 rngNext(void) {
    u16 x = rngState;
    x ^= x << 7;
    x ^= x >> 9;
    x ^= x << 8;
    rngState = x;
    return x;
}

u8 rngColor(u8 paletteLen) {
    return (u8)(((u32)rngNext() * paletteLen) >> 16);
}
