/* Rotatron SNES - entry point and scene machine:
 *   LOGO  - the studio logo (mode 3, high color) drops in under gravity,
 *           bounces twice, settles, fades (deadfall's LogoScene; START skips)
 *   TITLE - backdrops/title.png in mode 3 with baked texts; PRESS START
 *           blinks via its reserved palette entry
 *   PLAY  - the game (mode 1). START on the title seeds the RNG from press
 *           timing and reloads all game VRAM (the title owned it).
 */
#include <snes.h>
#include "core.h"
#include "render.h"
#include "game.h"
#include "audio.h"
#include "audio_sfx.h"
#include "res/soundbank.h"

#define SC_LOGO 0
#define SC_TITLE 1
#define SC_PLAY 2

/* Falling-logo physics (8.8 fixed; constants straight from deadfall). */
#define LOGO_START (-48)
#define LOGO_TARGET 92
#define LOGO_GRAV 105
#define LOGO_DAMP 102 /* bounce keeps 0.4 of the speed (102/256) */
#define LOGO_MAXBOUNCE 2
#define LOGO_HOLD 84
#define LOGO_FADE 18

int main(void) {
    u16 pad, padPrev = 0, pressed;
    u8 frame = 0;
    u16 bootFrames = 0; /* free-running: human START timing seeds the RNG */
    u8 scene = SC_LOGO;
    s16 logoY = (s16)LOGO_START << 8;
    s16 logoV = 0;
    u8 logoState = 0, logoBounce = 0;
    u16 logoTimer = 0;
    u8 blinkOn = 1;

    rngSeed(0x1d2b); /* provisional; reseeded at PRESS START */

    renderInit(); /* console + OAM boot; screen force-blanked */
    audioInit();
    logoSpriteReset(); /* zero particle/sparkle pools (WRAM not cleared at boot) */

    sceneShow(2); /* studio logo, parked above the screen */
    scenePinV((u16)(LOGO_TARGET - LOGO_START - 1));
    renderVBlank();
    /* From here every upload runs from the NMI at the START of vblank
     * (deadfall pattern); the loop below never calls renderVBlank again. */
    nmiSet(renderVBlank);
    WaitForVBlank();
    setScreenOn();

    while (1) {
        pad = padsCurrent(0);
        pressed = pad & ~padPrev;
        padPrev = pad;
        bootFrames++;

        switch (scene) {
            case SC_LOGO:
                logoParticlesUpdate();
                if (logoState == 0) { /* falling under gravity */
                    logoV += LOGO_GRAV;
                    logoY += logoV;
                    if (logoY >= ((s16)LOGO_TARGET << 8)) {
                        logoY = (s16)LOGO_TARGET << 8;
                        audioSfx(SFX_LOGOTHUD); /* impact thud */
                        logoBurst(128, (s16)(LOGO_TARGET + 40), 14);
                        if (logoBounce < LOGO_MAXBOUNCE) {
                            u16 m = (u16)logoV;
                            logoV = -(s16)(((m >> 8) * LOGO_DAMP) + (((m & 0xFF) * LOGO_DAMP) >> 8));
                            logoBounce++;
                        } else {
                            logoV = 0;
                            logoState = 1;
                            logoTimer = 0;
                        }
                    }
                    scenePinV((u16)((s16)LOGO_TARGET - (logoY >> 8) - 1));
                } else if (logoState == 1) { /* settled hold */
                    logoSparklesUpdate(2);
                    if (++logoTimer >= LOGO_HOLD) {
                        logoState = 2;
                        logoTimer = LOGO_FADE;
                    }
                } else { /* fade to the title */
                    logoSparklesUpdate(0);
                    if (logoTimer) {
                        logoTimer--;
                        setBrightness((u8)(15 * logoTimer / LOGO_FADE));
                    }
                    if (logoTimer == 0) {
                        setScreenOff();
                        audioPlayMusic(MOD_MUSIC_TITLE); /* theme starts WITH the title */
                        sceneShow(1); /* title art, texts baked in */
                        WaitForVBlank();
                        setScreenOn();
                        scene = SC_TITLE;
                    }
                }
                if ((pressed & KEY_START) && logoState < 2) {
                    logoState = 2;
                    logoTimer = LOGO_FADE;
                }
                break;

            case SC_TITLE:
                if (((bootFrames >> 5) & 1) != blinkOn) {
                    blinkOn = (u8)((bootFrames >> 5) & 1);
                    sceneBlink(blinkOn ? 0x7FFF : 0x0000);
                }
                if (pressed & KEY_START) {
                    rngSeed(bootFrames ^ (rngNext() << 1) ^ 0x5a5a);
                    setScreenOff();
                    audioPlayMusic(MOD_MUSIC_LEVEL1); /* blocking, masked by the blank */
                    renderGameLoad(); /* the title owned VRAM/CGRAM: reload all */
                    boardInit(3);
                    gameInit();
                    sparksInit();
                    boardRebuildMap();
                    renderVBlank(); /* board map (blank = free bandwidth) */
                    renderVBlank(); /* HUD map + staged palettes */
                    WaitForVBlank(); /* un-blank ON a vblank: no half-set frame */
                    setScreenOn();
                    scene = SC_PLAY;
                }
                break;

            default: /* SC_PLAY */
                gameFrame(pressed);
                cursorUpdate(curK, curJ, frame);
                linePulse(frame);
                sparksFrame(frame);
                ambientFrame();
                twinkleFrame(frame);
                frame++;
                break;
        }

        audioFrame();
        WaitForVBlank(); /* uploads happen inside the NMI (nmiSet hook) */
    }
    return 0;
}
