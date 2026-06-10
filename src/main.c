/* Rotatron SNES - entry point and scene machine:
 *   LOGO  - the studio logo drops in under gravity, bounces twice, settles,
 *           fades (port of deadfall's LogoScene; START skips ahead)
 *   TITLE - backdrops/title.png + credit + blinking PRESS START
 *   PLAY  - the game. START on the title seeds the RNG from press timing.
 */
#include <snes.h>
#include "core.h"
#include "render.h"
#include "game.h"
#include "audio.h"
#include "audio_sfx.h"

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

static void titleTexts(u8 blinkOn) {
    hudText(2, 22, "BY MARCO VAN HYLCKAMA VLIEG");
    hudText(10, 25, blinkOn ? "PRESS START" : "           ");
}

int main(void) {
    u16 pad, padPrev = 0, pressed;
    u8 frame = 0;
    u16 bootFrames = 0; /* free-running: human START timing seeds the RNG */
    u8 scene = SC_LOGO;
    s16 logoY = (s16)LOGO_START << 8;
    s16 logoV = 0;
    u8 logoState = 0, logoBounce = 0;
    u16 logoTimer = 0;

    rngSeed(0x1d2b); /* provisional; reseeded at PRESS START */

    renderInit(); /* leaves the screen force-blanked */
    audioInit();

    bg2Load(2); /* studio logo art, top edge lands at y=LOGO_TARGET */
    bg2Pin(1, (u16)(LOGO_TARGET - LOGO_START - 1));
    renderLayers(0x02); /* BG2 only */
    renderVBlank();
    setScreenOn();

    while (1) {
        pad = padsCurrent(0);
        pressed = pad & ~padPrev;
        padPrev = pad;
        bootFrames++;

        switch (scene) {
            case SC_LOGO:
                if (logoState == 0) { /* falling under gravity */
                    logoV += LOGO_GRAV;
                    logoY += logoV;
                    if (logoY >= ((s16)LOGO_TARGET << 8)) {
                        logoY = (s16)LOGO_TARGET << 8;
                        audioSfx(SFX_TURNWHEEL); /* impact thud */
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
                    bg2Pin(1, (u16)((s16)LOGO_TARGET - (logoY >> 8) - 1));
                } else if (logoState == 1) { /* settled hold */
                    if (++logoTimer >= LOGO_HOLD) {
                        logoState = 2;
                        logoTimer = LOGO_FADE;
                    }
                } else { /* fade to the title */
                    if (logoTimer) {
                        logoTimer--;
                        setBrightness((u8)(15 * logoTimer / LOGO_FADE));
                    }
                    if (logoTimer == 0) {
                        setScreenOff();
                        bg2Load(1); /* title art */
                        bg2Pin(1, 0x3FF);
                        hudClear();
                        titleTexts(1);
                        renderLayers(0x06); /* BG2 + BG3 */
                        renderVBlank();
                        setBrightness(15);
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
                titleTexts((u8)(((bootFrames >> 5) & 1) == 0));
                if (pressed & KEY_START) {
                    rngSeed(bootFrames ^ (rngNext() << 1) ^ 0x5a5a);
                    setScreenOff();
                    bg2Load(0); /* game backdrop */
                    bg2Pin(0, 0);
                    hudClear();
                    boardInit(3);
                    gameInit();
                    sparksInit();
                    boardRebuildMap();
                    renderVBlank(); /* board map (blank = free bandwidth) */
                    renderVBlank(); /* HUD map + staged palettes */
                    renderLayers(0x17);
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
        WaitForVBlank();
        renderVBlank();
    }
    return 0;
}
