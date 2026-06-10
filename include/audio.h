#ifndef AUDIO_H
#define AUDIO_H

#include "core_types.h"

void audioInit(void);       /* boot SPC700 + SFX bank + title music */
void audioPlayMusic(u8 mod); /* swap BGM (MOD_MUSIC_*); BLOCKING, scene changes only */
void audioSfx(u8 idx);      /* play a resident effect (SFX_* in audio_sfx.h) */
void audioSfxPitch(u8 idx, u8 pitch);
void audioFrame(void);      /* call once per frame */

#endif
