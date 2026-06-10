/* Rotatron SNES - entry point.
 * Milestone 1: playfield + d-pad cursor over valid vertices + CW/CCW spin
 * (A/R clockwise, B/L counter-clockwise). Instant recolor; the pre-rendered
 * rotation animation comes later.
 */
#include <snes.h>
#include "core.h"
#include "render.h"
#include "game.h"

int main(void) {
    u16 pad, padPrev = 0, pressed;
    u8 frame = 0;

    /* Fixed seed until the title screen exists to harvest entropy from
     * "press start" timing. Phase 1 palette: 3 colors. */
    rngSeed(0x1d2b);
    boardInit(3);

    renderInit();
    gameInit();
    sparksInit();
    boardRebuildMap();

    while (1) {
        pad = padsCurrent(0);
        pressed = pad & ~padPrev;
        padPrev = pad;

        gameFrame(pressed);
        cursorUpdate(curK, curJ, frame);
        linePulse(frame);
        sparksFrame(frame);
        ambientFrame();
        twinkleFrame(frame);
        frame++;

        WaitForVBlank();
        renderVBlank();
    }
    return 0;
}
