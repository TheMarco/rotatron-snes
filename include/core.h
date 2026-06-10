/* Rotatron core game model - a 1:1 port of the web game's pure logic
 * (hex-spin: src/utils/math.js, src/game/board.js, src/game/spin-model.js).
 * No floats, no allocation; verified against the JS modules by the host
 * golden tests in tests/.
 *
 * Board: 7x12 cell scan, 54 cells inside the flat-top hex (side 3).
 * Cell key (col,row); isUp = (col+row)%2==0. A vertex (k,j) is real when
 * (k+j)%2==1; its ring is 6 triangle slots clockwise from north.
 * Boundary vertices keep per-vertex phantom colors for off-board slots so the
 * ring behaves as one 6-position circular buffer (colors flow in from the
 * outside on edge spins) - phantom buffers are independent per vertex.
 */
#ifndef CORE_H
#define CORE_H

#include "core_types.h"

#define BOARD_ROWS 7
#define BOARD_COLS 12
#define VTX_ROWS 8  /* j = 0..7 */
#define VTX_COLS 13 /* k = 0..12 */
#define NO_CELL 0xFF

/* Color indices follow the phase introduction order of the web game
 * (colors.js): phase 1 plays 0..2, each later phase appends one. */
#define COLOR_MAGENTA 0
#define COLOR_PURPLE 1
#define COLOR_CYAN 2
#define COLOR_YELLOW 3
#define COLOR_GREEN 4
#define COLOR_ORANGE 5

extern u8 boardColor[BOARD_ROWS][BOARD_COLS];     /* NO_CELL outside the hex */
extern u8 phantomColor[VTX_ROWS][VTX_COLS][6];    /* NO_CELL where the slot is real / vertex has none */
extern u8 vertexValid[VTX_ROWS][VTX_COLS];        /* real vertex with >=1 ring triangle on board */

/* xorshift16; seed must be nonzero. rngColor mirrors the JS harness's
 * patched Math.random: floor(next()/65536 * len) == (next()*len)>>16. */
void rngSeed(u16 seed);
u16 rngNext(void);
u8 rngColor(u8 paletteLen);

u8 cellInHex(u8 col, u8 row);
/* Ring slot i (CW from north) of vertex (k,j): cell (k+RING_DC[i], j+RING_DR[i]). */
extern const s8 RING_DC[6];
extern const s8 RING_DR[6];

void boardInit(u8 paletteLen);     /* createBoard (re-roll until no completed hex) + createPhantomSlots */
void phantomReseed(u8 paletteLen); /* reseedPhantomSlots: re-roll hidden colors on phase change */
u8 boardHasCompletedHex(void);

/* Completed hexes: vertices whose 6 on-board ring slots share one color.
 * Out arrays must hold 19 entries (max = interior vertex count). */
u8 findCompletedHexes(u8 *outK, u8 *outJ, u8 *outColor);

/* Gather the 6 ring colors of (k,j); outReal[i]=1 for on-board slots.
 * Phantom slots read the vertex's hidden buffer. */
void spinGather(u8 k, u8 j, u8 outColors[6], u8 outReal[6]);
/* Rotate the ring one step (ccw=0 -> CW: slot i receives color from (i+5)%6).
 * Writes reals back to the board, phantoms to the vertex buffer.
 * Returns 0 if the vertex is invalid. */
u8 spinApply(u8 k, u8 j, u8 ccw);

#endif
