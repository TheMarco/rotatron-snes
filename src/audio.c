/* snesmod audio: three looping BGM modules (title / level1 / gameover, from
 * music/*.mid -> chiptune) + the web game's sampled SFX as resident effects.
 * spcLoad is BLOCKING and wipes ARAM (effects reload after every module
 * swap) - call audioPlayMusic only at scene changes. */
#include <snes.h>
#include "audio.h"
#include "audio_sfx.h"
#include "res/soundbank.h"
#include "soundbank_banks.h" /* SPC_SET_ALL_BANKS() (generated) */

#define MUSIC_VOLUME 96 /* 0..255; SFX play at full volume on top */

void audioPlayMusic(u8 mod) {
    u8 i;
    spcStop();
    spcLoad(mod);
    for (i = 0; i < SFX_COUNT; i++) spcLoadEffect(i);
    spcSetModuleVolume(MUSIC_VOLUME);
    spcPlay(0);
}

void audioInit(void) {
    u8 i;
    spcBoot();
    SPC_SET_ALL_BANKS();
    spcStop();
    spcLoad(MOD_SFX); /* effects bank: makes its samples the global effects */
    for (i = 0; i < SFX_COUNT; i++) spcLoadEffect(i);
    /* No music yet: the logo drops in silence (impact SFX only); the title
     * theme starts when the title appears. */
}

void audioSfx(u8 idx) {
    spcEffect(2, idx, 15 * 16 + 8); /* pitch=2 (8kHz), vol=15, pan=center */
}

void audioSfxPitch(u8 idx, u8 pitch) { /* 2 = natural; higher = up-shifted */
    spcEffect(pitch, idx, 15 * 16 + 8);
}

void audioFrame(void) {
    spcProcess(); /* feed the sound engine every frame */
}
