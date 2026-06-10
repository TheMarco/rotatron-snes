/* snesmod audio: one looping BGM module (music/level1.mid -> snesmod
 * chiptune). spcLoad is blocking and slow (uploads the module to SPC700
 * ARAM) - boot-time only. */
#include <snes.h>
#include "audio.h"
#include "res/soundbank.h"
#include "soundbank_banks.h" /* SPC_SET_ALL_BANKS() (generated) */

#define MUSIC_VOLUME 96 /* 0..255; headroom for future SFX on top */

void audioInit(void) {
    spcBoot();
    SPC_SET_ALL_BANKS();
    spcStop();
    spcLoad(MOD_MUSIC_LEVEL1);
    spcSetModuleVolume(MUSIC_VOLUME);
    spcPlay(0);
}

void audioFrame(void) {
    spcProcess(); /* feed the sound engine every frame */
}
