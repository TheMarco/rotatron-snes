#ifndef GAME_H
#define GAME_H

#include "core_types.h"

extern u8 curK, curJ;

void gameInit(void);
void gameFrame(u16 pressed);

#endif
