// Golden-vector generator: drives the REAL web-game modules (hex-spin) with a
// deterministic RNG patched over Math.random, and dumps board/phantom state in
// the same format tests/host_main.c prints. Any divergence = port bug.
//
// RNG: xorshift16, identical to src/core/rng.c. The patched Math.random
// returns next()/65536, so the JS `Math.floor(random()*len)` is bit-identical
// to the C `(next()*len)>>16` (exact in doubles for len<=6).
import { ring, key, ROWS, COLS } from '../../hex-spin/src/utils/math.js';
import {
  createBoard,
  createPhantomSlots,
  reseedPhantomSlots,
  isValidVertex,
} from '../../hex-spin/src/game/board.js';
import { buildSpinSlots, planSpin } from '../../hex-spin/src/game/spin-model.js';
import { findCompletedHexes } from '../../hex-spin/src/game/rules.js';

const SEED = 0xbeef;
let st = SEED;
function rngNext() {
  let x = st;
  x = (x ^ ((x << 7) & 0xffff)) & 0xffff;
  x = (x ^ (x >>> 9)) & 0xffff;
  x = (x ^ ((x << 8) & 0xffff)) & 0xffff;
  st = x;
  return x;
}
Math.random = () => rngNext() / 65536;

// Color name -> index, in phase introduction order (matches core.h).
const COLOR_IDX = { magenta: 0, purple: 1, cyan: 2, yellow: 3, green: 4, orange: 5 };
const NAMES = Object.keys(COLOR_IDX);
const paletteOf = (len) => NAMES.slice(0, len);

const lines = [];
function dump(board, phantoms) {
  for (let row = 0; row < ROWS; row++) {
    let s = 'B ';
    for (let col = 0; col < COLS; col++) {
      const t = board[key(col, row)];
      s += t ? String(COLOR_IDX[t.color]) : '.';
    }
    lines.push(s);
  }
  for (let j = 0; j <= ROWS; j++) {
    for (let k = 0; k <= COLS; k++) {
      const m = phantoms[key(k, j)];
      if (!m) continue;
      let s = `P ${k} ${j} `;
      for (let i = 0; i < 6; i++) s += m[i] !== undefined ? String(COLOR_IDX[m[i]]) : '.';
      lines.push(s);
    }
  }
}

// Apply one spin exactly the way spin.js does: planSpin -> remapRealTargets
// to the board, phantomAssignments to the spun vertex's own buffer.
function applySpin(board, phantoms, k, j, dir) {
  const slots = buildSpinSlots(ring(k, j), board, phantoms[key(k, j)]);
  const plan = planSpin(slots, dir);
  for (const t of plan.remapRealTargets) {
    if (t.color !== undefined) board[t.targetKey].color = t.color;
  }
  const pm = phantoms[key(k, j)];
  if (pm) {
    for (const a of plan.phantomAssignments) pm[a.targetIdx] = a.color;
  }
}

let board = createBoard(paletteOf(3));
let phantoms = createPhantomSlots(board, paletteOf(3));
lines.push('INIT');
dump(board, phantoms);

const RESEEDS = { 150: 4, 250: 5, 325: 6 }; // spin index -> new palette length
for (let n = 0; n < 400; n++) {
  if (RESEEDS[n]) {
    phantoms = reseedPhantomSlots(phantoms, paletteOf(RESEEDS[n]));
    lines.push(`RESEED ${RESEEDS[n]}`);
    dump(board, phantoms);
  }
  let k, j;
  do {
    k = rngNext() % 13;
    j = rngNext() % 8;
  } while (!isValidVertex(k, j, board));
  const ccw = rngNext() & 1;
  applySpin(board, phantoms, k, j, ccw ? 'ccw' : 'cw');
  lines.push(`SPIN ${k} ${j} ${ccw}`);
  dump(board, phantoms);
  for (const h of findCompletedHexes(board)) {
    lines.push(`H ${h.k} ${h.j} ${COLOR_IDX[h.color]}`);
  }
}

// Directed completion patterns: random spins essentially never complete a
// hex, so paint rings mono to exercise detection (incl. overlap and all-19).
function paintRing(k, j, ci) {
  for (const [c, r] of ring(k, j)) {
    const t = board[key(c, r)];
    if (t) t.color = NAMES[ci];
  }
}
function dumpHexes() {
  for (const h of findCompletedHexes(board)) lines.push(`H ${h.k} ${h.j} ${COLOR_IDX[h.color]}`);
}
lines.push('FORCE single');
paintRing(6, 3, 0);
dump(board, phantoms);
dumpHexes();
lines.push('FORCE pair');
paintRing(4, 2, 1);
paintRing(5, 4, 1);
dump(board, phantoms);
dumpHexes();
lines.push('FORCE all');
for (const tk of Object.keys(board)) board[tk].color = NAMES[2];
dump(board, phantoms);
dumpHexes();

console.log(lines.join('\n'));
