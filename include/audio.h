#ifndef AUDIO_H
#define AUDIO_H

#include "core_types.h"

void audioInit(void);  /* boot SPC700 + SFX bank + start the BGM loop */
void audioSfx(u8 idx); /* play a resident effect (SFX_* in audio_sfx.h) */
void audioFrame(void); /* call once per frame */

#endif
