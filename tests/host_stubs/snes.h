#ifndef HOST_STUB_SNES_H
#define HOST_STUB_SNES_H

#include "core_types.h"

#define KEY_A      0x0080
#define KEY_B      0x8000
#define KEY_SELECT 0x2000
#define KEY_START  0x1000
#define KEY_UP     0x0800
#define KEY_DOWN   0x0400
#define KEY_LEFT   0x0200
#define KEY_RIGHT  0x0100
#define KEY_L      0x0020
#define KEY_R      0x0010

void setBrightness(u8 brightness);
void setScreenOff(void);
void setScreenOn(void);
void spcStop(void);

#endif
