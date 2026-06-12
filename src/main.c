/* Rotatron SNES - entry point and scene machine:
 *   LOGO  - the studio logo (mode 3, high color) drops in under gravity,
 *           bounces twice, settles, fades (deadfall's LogoScene; START skips)
 *   TITLE - backdrops/title.png in mode 3 with baked texts; PRESS START
 *           blinks via its reserved palette entry
 *   HOW   - 6-page "how to play" over the phase-1 backdrop (mode 1);
 *           entered by SELECT on the title or after 10s idle (attract mode)
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

#define SC_LOGO  0
#define SC_TITLE 1
#define SC_PLAY  2
#define SC_HOW   3

/* Falling-logo physics (8.8 fixed; constants straight from deadfall). */
#define LOGO_START (-48)
#define LOGO_TARGET 92
#define LOGO_GRAV 105
#define LOGO_DAMP 102 /* bounce keeps 0.4 of the speed (102/256) */
#define LOGO_MAXBOUNCE 2
#define LOGO_HOLD 84
#define LOGO_FADE 18

/* How-to-play: 6 pages of text on BG3 over the phase-1 backdrop.
 * Mode 1, BG2+BG3 only (layersSet 0x06), no board, no sprites. */
#define HOWTO_PAGES 6
#define HOWTO_AUTO_FRAMES 720 /* attract mode: 12s per page */
static u8 howtoPage;
static u8 howtoAuto;    /* 1 = entered via title idle: pages auto-advance */
static u16 howtoTimer;

/* All text sits at x=1 with strings <=30 chars: a 1-tile margin each side.
 * Pages 1 and 3 draw hex illustrations on BG1 (howtoHexReset/Demo); the
 * staged layer mask flips BG1 in/out per page. */
static void howtoDrawPage(u8 p) {
    layersSet((p == 1 || p == 3) ? 0x07 : 0x06); /* BG1 under hex pages only */
    bg1ShiftSet(p == 3 ? 8 : 0); /* cascade pair sits 8px lower */
    hudClear();
    switch (p) {
        case 0:
            hudText(1, 1,  "HOW TO PLAY (1 OF 6)");
            hudText(1, 2,  "==============================");
            hudText(1, 4,  "CONTROLS");
            hudText(1, 6,  "DPAD   MOVE CURSOR TO A HINGE");
            hudText(1, 7,  "A / R  SPIN 60 DEGREES CW");
            hudText(1, 8,  "B / L  SPIN 60 DEGREES CCW");
            hudText(1, 10, "Six triangles around the hinge");
            hudText(1, 11, "rotate, carrying colors along.");
            hudText(1, 13, "Rim hinges pull hidden PHANTOM");
            hudText(1, 14, "colors in from beyond the hex.");
            break;
        case 1:
            hudText(1, 1,  "HOW TO PLAY (2 OF 6)");
            hudText(1, 2,  "==============================");
            hudText(1, 4,  "THE GOAL");
            hudText(1, 6,  "When 6 triangles of the SAME");
            hudText(1, 7,  "COLOR meet at a vertex, that");
            hudText(1, 8,  "hex SHATTERS for points.");
            hudText(1, 10, "Shattered cells refill; new");
            hudText(1, 11, "matches CHAIN automatically.");
            hudText(1, 13, "Clears refill HEAT.");
            howtoHexReset();
            howtoHexDemo(6, 3, 2); /* cyan hex at center, y 124..172 */
            hudText(8, 22, "A COMPLETED HEX");
            break;
        case 2:
            hudText(1, 1,  "HOW TO PLAY (3 OF 6)");
            hudText(1, 2,  "==============================");
            hudText(1, 4,  "HEAT");
            hudText(1, 6,  "The bar at the top drains");
            hudText(1, 7,  "constantly. Zero = GAME OVER.");
            hudText(1, 9,  "Heat refill per cleared wave:");
            hudText(1, 10, "  1 hex   = +18 percent");
            hudText(1, 11, "  2 hexes = +72 percent");
            hudText(1, 12, "  3 hexes = +100 percent");
            hudText(1, 14, "Multi-clears pay quadratically");
            hudText(1, 15, "Each new phase drains faster.");
            hudText(1, 18, "HEAT:");
            hudBarDemo(7, 18, 24, 132);
            hudText(7, 20, "EMPTY");
            hudText(27, 20, "FULL");
            break;
        case 3:
            hudText(1, 1,  "HOW TO PLAY (4 OF 6)");
            hudText(1, 2,  "==============================");
            hudText(1, 4,  "CASCADES");
            hudText(1, 6,  "Refilled cells can match again");
            hudText(1, 7,  "for an automatic CHAIN.");
            hudText(1, 8,  "Score doubles at each depth:");
            howtoHexReset();
            /* Stacked, sharing their horizontal edge at y=124 (a hex's flat
             * bottom IS the next one's top; rings stay disjoint: rows 0-1
             * vs 2-3). x 96..160, y 76..172. */
            howtoHexDemo(6, 1, 2); /* cyan, center (128,108) with the shift */
            howtoHexDemo(6, 3, 2); /* cyan too: a chain is same-color matches */
            hudText(9, 13, "X1");  /* beside hex 1's center row */
            hudText(9, 19, "X2");  /* beside hex 2's center row */
            hudText(2, 23, "EACH DEPTH DOUBLES: X4, X8..");
            break;
        case 4:
            hudText(1, 1,  "HOW TO PLAY (5 OF 6)");
            hudText(1, 2,  "==============================");
            hudText(1, 4,  "SCORING");
            hudText(1, 6,  "100 x PHASE x CASCADE");
            hudText(1, 7,  "    x FRESHNESS x MULTI-HEX");
            hudText(1, 9,  "PHASE: doubles each phase");
            hudText(1, 10, "CASCADE: doubles per depth");
            hudText(1, 11, "FRESHNESS: x2 just after clear");
            hudText(1, 12, "  (fades over 10 spins)");
            hudText(1, 13, "MULTI-HEX: N at once = Nx each");
            hudText(1, 15, "Newest phase color = x2 extra.");
            hudText(1, 16, "Wipe a color = COLOR CLEARED.");
            break;
        default: /* page 5 */
            hudText(1, 1,  "HOW TO PLAY (6 OF 6)");
            hudText(1, 2,  "==============================");
            hudText(1, 4,  "PHASES");
            hudText(1, 6,  "Clear hexes to advance phases.");
            hudText(1, 7,  "Each phase adds a new color");
            hudText(1, 8,  "and speeds up heat drain.");
            hudText(1, 10, "  P1  3 colors  (start)");
            hudText(1, 11, "  P2  + YELLOW  (11 hexes)");
            hudText(1, 12, "  P3  + GREEN   (26 hexes)");
            hudText(1, 13, "  P4  + ORANGE  (51 hexes)");
            hudText(1, 15, "The board carries over between");
            hudText(1, 16, "phases. Hunt newest color: x2.");
            break;
    }
    if (p < HOWTO_PAGES - 1)
        hudText(1, 26, "A:NEXT   B:BACK   SEL:EXIT");
    else
        hudText(1, 26, "A:EXIT   B:BACK   SEL:EXIT");
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
    u8 blinkOn = 1;
    u16 titleIdleFrames = 0; /* attract-mode timer: auto-enter HOW after 10s */

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
                        titleIdleFrames = 0;
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
                /* Attract mode: auto-enter HOW after 10s of no input */
                if (pad) {
                    titleIdleFrames = 0;
                } else if (titleIdleFrames < 600) {
                    titleIdleFrames++;
                }
                if ((pressed & KEY_SELECT) || titleIdleFrames >= 600) {
                    howtoAuto = (pressed & KEY_SELECT) ? 0 : 1;
                    howtoTimer = 0;
                    titleIdleFrames = 0;
                    setScreenOff();
                    renderGameLoad();
                    bg2LoadPhase(1);
                    twinkleSelect(0);
                    howtoPage = 0;
                    howtoDrawPage(0); /* stages layers + draws any hex demo */
                    renderVBlank();
                    renderVBlank();
                    WaitForVBlank();
                    setScreenOn();
                    scene = SC_HOW;
                } else if (pressed & KEY_START) {
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

            case SC_HOW:
                twinkleFrame(frame);
                frame++;
                if (howtoAuto) {
                    if (pressed) {
                        howtoAuto = 0; /* viewer takes over: manual paging */
                    } else if (++howtoTimer >= HOWTO_AUTO_FRAMES) {
                        howtoTimer = 0;
                        if (howtoPage < HOWTO_PAGES - 1) {
                            howtoPage++;
                            howtoDrawPage(howtoPage);
                        } else { /* cycled through: back to the title */
                            setScreenOff();
                            sceneShow(1);
                            blinkOn = 1;
                            titleIdleFrames = 0;
                            WaitForVBlank();
                            setScreenOn();
                            scene = SC_TITLE;
                        }
                        break;
                    }
                }
                if ((pressed & KEY_SELECT) ||
                    ((pressed & KEY_B) && howtoPage == 0)) {
                    /* exit back to title */
                    setScreenOff();
                    sceneShow(1);
                    blinkOn = 1;
                    titleIdleFrames = 0;
                    WaitForVBlank();
                    setScreenOn();
                    scene = SC_TITLE;
                } else if (pressed & KEY_B) {
                    howtoPage--;
                    howtoDrawPage(howtoPage);
                } else if (pressed & (KEY_A | KEY_START)) {
                    if (howtoPage < HOWTO_PAGES - 1) {
                        howtoPage++;
                        howtoDrawPage(howtoPage);
                    } else {
                        /* last page: exit to title */
                        setScreenOff();
                        sceneShow(1);
                        blinkOn = 1;
                        titleIdleFrames = 0;
                        WaitForVBlank();
                        setScreenOn();
                        scene = SC_TITLE;
                    }
                }
                break;

            default: /* SC_PLAY */
                gameFrame(pressed);
                cursorUpdate(curK, curJ, frame);
                linePulse(frame);
                sparksFrame(frame);
                ambientFrame();
                rippleFrame();
                twinkleFrame(frame);
                frame++;
                break;
        }

        audioFrame();
        WaitForVBlank(); /* uploads happen inside the NMI (nmiSet hook) */
    }
    return 0;
}
