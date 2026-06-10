/* snesmod audio: looping BGM (music/level1.mid -> chiptune module) + the
 * web game's sampled SFX as resident effects (8kHz, music/s-*.mp3).
 * spcLoad re-inits ARAM, so effects load after the music module. Everything
 * here is blocking boot-time work. */
#include <snes.h>
#include "audio.h"
#include "audio_sfx.h"
#include "res/soundbank.h"
#include "soundbank_banks.h" /* SPC_SET_ALL_BANKS() (generated) */

#define MUSIC_VOLUME 96 /* 0..255; SFX play at full volume on top */

void audioInit(void) {
    u8 i;
    spcBoot();
    SPC_SET_ALL_BANKS();
    spcStop();
    spcLoad(MOD_SFX); /* effects bank: makes its samples the global effects */
    for (i = 0; i < SFX_COUNT; i++) spcLoadEffect(i);
    spcLoad(MOD_MUSIC_LEVEL1); /* wipes ARAM -> effects reload below */
    for (i = 0; i < SFX_COUNT; i++) spcLoadEffect(i);
    spcSetModuleVolume(MUSIC_VOLUME);
    spcPlay(0);
}

void audioSfx(u8 idx) {
    spcEffect(2, idx, 15 * 16 + 8); /* pitch=2 (8kHz), vol=15, pan=center */
}

void audioFrame(void) {
    spcProcess(); /* feed the sound engine every frame */
}
