/* Rotatron SNES - entry point.
 * Milestone 1: playfield + d-pad cursor over valid vertices + CW/CCW spin.
 * For now: minimal boot to prove the toolchain (backdrop color + vblank loop).
 */
#include <snes.h>

int main(void) {
    consoleInit();

    setMode(BG_MODE1, 0);
    bgSetDisable(0);
    bgSetDisable(1);
    bgSetDisable(2);

    /* Dark violet backdrop so a successful boot is visually obvious. */
    setPaletteColor(0, RGB8(24, 8, 48));

    setScreenOn();

    while (1) {
        WaitForVBlank();
    }
    return 0;
}
