# CLAUDE.md — Rotatron SNES

SNES port of the Rotatron web game (`../hex-spin`): neon arcade puzzle on a
hex-shaped triangle grid. Press A/R on a hinge and six triangles rotate 60°
CW (B/L = CCW), matching hexes shatter, a heat bar drains constantly and
clears refill it. Faithful port: the C core replays the JS logic bit-for-bit
(golden-tested), the presentation reinterprets the web game's bloom/particles
with SNES idioms (palette animation, mosaic, sprites).

The current code is the source of truth. If this file and the implementation
diverge, update this file to match the code.

---

## Tech stack

- **PVSnesLib** (`~/pvsneslib/pvsneslib`, path via `.pvsneslib_home`):
  816-tcc C compiler + WLA-DX assembler/linker + smconv (snesmod audio)
- **LoROM, 32 banks (1 MB), real FastROM**: `FASTROM := 1` makes tcc emit
  `-F` (our code/tables get $80+ mirror banks) and `renderInit` sets MEMSEL
  ($420D) — all project C runs at 3.58MHz. The link KEEPS the SlowROM libs
  (this install's LoROM_FastROM objects fail to link: duplicate section
  labels), so pvsneslib calls stay at 2.68MHz — works because $8X mirrors
  $0X. Header speed bit still patched post-link (`tools/set_fastrom.py` —
  this WLA-DX ignores the header directive)
- **Python 3 + PIL/numpy** asset pipeline (no gfx4snes)
- Patterns inherited from `../deadfall-snes` (Makefile shape, macOS `sed`
  shim in `tools/bin/`, NMI upload hook, soundbank layout)
- Host-side tests: clang + Node driving the REAL `../hex-spin` JS modules

Scripts:
- `make` — build `rotatron.sfc` (+ FastROM patch; generates missing gfx/audio)
- `make run` — build + deploy into OpenEmu's library
- `make gfx` — regenerate ALL graphics (board tiles, backdrops, title/logo)
- `make songs` — regenerate music modules from `music/*.mid` + SFX bank
- `make test` — golden tests (JS vs C, ~12.4K lines diffed verbatim) + host
  checks for the SNES-side game state machine

Always cold-boot after deploying: OpenEmu keeps the loaded ROM in memory and
auto-resumes save states (the deploy step deletes the auto-state, but an
OPEN game window keeps playing the old build).

**Test on bsnes too, not just snes9x/OpenEmu.** bsnes models real hardware:
random power-on RAM/PPU state, strict vblank write windows. Several bugs
shipped "working" on snes9x and broke only there (see Gotchas).

---

## Repo layout

```text
Makefile            PVSnesLib snes_rules + audio + asset targets
hdr.asm             LoROM header, 32 banks, vectors
data.asm            .incbin sections for all converted assets
backdrops/          source PNGs: level1-4.png (256x256 seamless), title.png
music/              source .mid files + the web game's s-*.mp3 SFX
src/
  main.c            scene machine: LOGO -> TITLE -> PLAY + main loop
  game.c            cursor, spin/cascade/stage state machines, heat, score,
                    phases, color elimination, game over/restart
  render.c          all PPU work: maps, palettes (staged->vblank), sprites,
                    spin animation, pulses, sparks, ambient, HUD, scenes
  audio.c           snesmod boot, music swap, SFX
  boardtab.c        GENERATED lookup tables (committed; `make gfx` refreshes)
  core/             pure game logic, host-testable (clang + 816-tcc):
    rng.c           xorshift16 (seed != 0)
    board.c         createBoard (re-roll loop), phantom slots, reseed
    spin.c          ring gather/apply (CW/CCW carry)
    rules.c         findCompletedHexes
    score.c         freshMult tenths, wave/bonus units, BCD score
    seams.c         interior seam list (spark paths)
include/            core.h, render.h, game.h, audio.h + GENERATED
                    boardtab.h, bg2tab.h, audio_sfx.h, soundbank_banks.h
tools/
  build_board_gfx.py   board tileset/tables/spin frames/cursor/pulse/spark/
                       ambient sprites + res/preview.png
  build_backdrop.py    level1-4.png -> bgl1-4 (BG2) + twinkle table
  build_title8.py      mode-3 8bpp title + studio logo (texts baked in; uses
                       local/fallback logo if the sibling source is absent)
  build_audio.py       s-*.mp3 -> res/sfx.it + audio_sfx.h
  mid2it.py            .mid -> snesmod .it (+preview wav); optional bpm arg
  verify_board_render.py  pixel-diffs the emitted tiles/tables vs ground truth
  gen_soundbank_banks.py  soundbank bank list -> header (handles 1-bank case)
  set_fastrom.py, run_openemu.sh, bin/sed
tests/
  gen_golden.mjs    drives ../hex-spin JS with seeded RNG -> expected.txt
  host_main.c       replays the same scenario through the C core
  host_game_main.c  white-box host checks for heat/cascade/phase glue
  run_host_tests.sh clang build + diff
```

---

## Core model (golden-tested against the JS)

Board: 7×12 cell scan, 54 triangles inside the flat-top hex (side 3).
`boardColor[row][col]` holds color index 0-5 or `NO_CELL`. A vertex `(k,j)`
is real when `(k+j)%2==1`; `RING_DC/DR` give its six ring slots CW from N.
Boundary vertices keep per-vertex `phantomColor[j][k][6]` buffers (hidden
colors that rotate in/out on rim spins) — independent per vertex, exactly
like the JS.

- `isInsideHex` reduced to exact integers:
  `|3*row + {2 up,1 down} - 9| <= 9 && 3*|col-5| + |..| <= 18`
- RNG: xorshift16; `rngColor(len) = (next()*len)>>16` matches the JS harness
  patching `Math.random = next()/65536` bit-for-bit
- Spin: slot i receives color from `(i+5)%6` (CW) / `(i+1)%6` (CCW)
- Colors in phase-introduction order: magenta, purple, cyan, yellow, green,
  orange (`COLOR_*` in core.h must match `NEON` order in build_board_gfx.py)
- Scoring in integers: `fm10 = clamp(21-spins, 11, 20)`;
  `waveUnit = 10*fm10*2^(p-1)` added to the **BCD score** `2^min(c-1,6)`
  times (cascade cap 2^6 — the web is unbounded); multi-kill shape
  60/75/80+10(m-4) in tenths; phase's new color (index phase+1) doubles.
  All 1728 grid cases golden-identical to rules.js.
- Color elimination requires the **present -> absent transition** (a
  newly-introduced color that never spawned must NOT count); suppressed
  colors leave the refill pool for 4 spins, pool falls back to full if empty.

The golden harness (`tests/`) covers: board generation incl. re-roll, 400
mixed CW/CCW spins, phantom reseeds, hex detection (incl. painted single/
pair/all-19 patterns), seam enumeration (vs an independent
triangleVertices-intersection derivation), and the scoring grid.

---

## Rendering

Geometry: triangle edge **S=32px, row height H=24px** — both LOCKED to the
8px tile grid (H must divide into tiles; S/2 must be tile-aligned). The
hexagon cannot be resized "a little". Board proper 192×168 at screen
(32, 68); the generated region is 208×184 (8px margins so rim axis pins
don't clip). Play uses **239-line overscan** (SETINI bit 2) to sit this low;
scenes are 224-line. bsnes shows the CENTER 224 lines of an overscan frame,
so the HUD starts at tile row 1, never row 0.

### BG1 — the board (mode 1, 4bpp)

`tools/build_board_gfx.py` renders the board once (owner map + 1px
neighbor-diff outlines + axis pins at all 37 hinges), slices it into ~30
tile *structures* (max 2 owner triangles per tile — asserted), and emits:

- **Single-owner tiles: color-agnostic.** One graphic; color comes from BG
  sub-palettes 1-6 via the tilemap palette bits (slots: 1 fill, 2 neon line,
  3 dim, 4 pin halo, 5 white, 6 glow). White/hidden/glow display variants
  at entry offsets +1..+3.
- **Dual-owner tiles (diagonal seams): baked color pairs** in sub-palette 0,
  81 combos per structure over the 9-value display domain
  (0-5 colors, 6 = solid white, 7 = hidden, 8 = glow).
- **Spin-time variants:** axis-only (pins survive blanking) and half tiles
  (outside neighbor's half kept) so spins never notch neighbors.
- Lookup: `entryTable[structBase[s] + (dual ? cA*9+cB : variant)]`; all
  entries carry the BG1 priority bit (ambient sprites at OBJ priority 2 fly
  BETWEEN backdrop and board).
- `triDisp[54]` overrides what a triangle displays (DISP_WHITE/HIDDEN/GLOW)
  without touching game state; `triRefresh`/`ringRefresh` recompute only the
  owned cells (a full `boardRebuildMap` costs multiple frames of 65816 time
  — init only).

`tools/verify_board_render.py` replays the entry lookup from the emitted
artifacts and pixel-diffs against ground truth in RGB (covers palette
routing). It has caught real bugs every time the pipeline changed — run it
after touching the generator.

### BG2 — backdrops; BG3 — HUD

Per-phase backdrops (`bgl1-4`, 256×256 seamless, ≤**1023** tiles each — the
converter auto-blurs to fit; 1024 would fill a ROM bank exactly and break
the end label). Slow drift (1px/8f), palette twinkle on the 3 brightest
slots (per-phase sets in generated `bg2tab.h`). Swapped per phase under
force-blank.

HUD on BG3 (2bpp, mode-1 BG3-priority = above everything): deadfall's font
(`res/hudfont.pic`, ASCII 32-95; `make gfx` creates a deterministic fallback
if it is absent) in BG1's tile-page tail (BG bases must be 4K-word aligned!),
procedural heat-bar/panel-border tiles, 3px-lowered digit glyphs split across
two tile rows. 2bpp sub-pal 7 = CGRAM 28-31 (unused tail of BG1 sub-pal 1).

### OBJ layout (OBSEL: base 0x6000, 16/32 sizes)

Tiles 0-383 spin frames (6 frames × 64×64, 2 frames per 8-row band),
384 cursor, 386-395/402-411 + 484-493/500-509 the shared ambient-flyer region
(5 OBJ-cols × 2 rows = 40 tiles), 398 HUD dot, 416-479 pulse rings, 480/482
sparks. Sprites: 0 cursor, 1-4 spin cluster (32×32 ×4), 5-12 pulse waves
A+echo, 13-15 seam sparks, 17-22 HUD dots, 23-32 ambient flyer (10 small OBJs).

**Ambient flyers** (`ambientFrame`): decorative sky sprites crossing BEHIND
the board (OBJ priority 2), ONE at a time. A generated table (`AMB_SPRITES` in
build_board_gfx.py → `include/ambtab.h`) holds N ≤80×32 PNG types (ship,
voyager, …); each is emitted as a 1280-byte 5-col block + 11-color palette.
At spawn a random type's tiles stream into the shared region ONE 320-byte
tile row per vblank (`ambTileStep`, map DMA wins the slot); the flyer holds
off-screen until the last row lands. All four rows chained in one vblank
overran the overscan window and the DROPPED TAIL rows left the previous
flyer's bottom half under every new ship. Motion 'H' (straight) or 'D' (gentle
downward diagonal); travels LTR/RTL (H-flip + reversed art columns). 9.7 fixed
point biased by AMB_BIAS (96) keeps X u16; off-screen columns are hidden by
true-signed-X clip. The 10 OBJs redraw every frame via DIRECT oamMemory
writes (`ambOamPut/Hide`, fast-bank, no slow-lib jsl's); partial-left
columns set the hi-table X8 bit (unset, they wrapped to x&0xFF — the old
"two ships" bug). NO per-scanline
suppression: one flyer + spin/pulse/cursor/sparks stays under the 34-tile cap.
TO ADD A TYPE: drop a PNG in sprites/ + one row in AMB_SPRITES, `make gfx`.
Rows whose PNG is missing are skipped with a generator warning, so clean
checkouts can still rebuild with the tracked sprite set.

**Spin animation:** pre-rendered sector-indexed frames (0..50°, true-space
rotation re-squashed); `spinAnimBegin` writes the ring's actual colors into
OBJ palette 1 per spin; CCW = H-flipped playback + sector permutation
(0)(15)(24)(3) — zero extra VRAM. The eased 14-tick schedule ends ON the
recolored board (the last 10° step IS the recolor).

### CGRAM map

BG: 0-15 dual-pairs pal, 16-111 per-color pals 1-6, 112-127 backdrop,
28-31 HUD (white/dark/heat). OBJ: 128 cursor/pulse/spark/ambient pal,
144 spin cluster (written per spin), 160+ dot palettes 2-7 (slot 1 = neon).
Animated entries (all staged in WRAM, flushed in the NMI hook): glow
(15 + per-color mirrors), breathing lines (7-12 + mirrors), twinkle, heat
color (31), title blink (255 in mode 3).

---

## The vblank/NMI contract (hard-won — do not regress)

- **ALL VRAM/CGRAM uploads run from the `nmiSet(renderVBlank)` hook**, i.e.
  at the START of vblank. Code after `WaitForVBlank()` races the vblank end;
  on accurate PPUs late VRAM writes are redirected to the live fetch address
  (= random tile corruption, bsnes showed it, snes9x doesn't).
- **DMA only — never CPU VRAM-port pokes** (a changed-word poke queue
  corrupted VRAM twice before being removed).
- **One map transfer per vblank** (board first, HUD next frame). The board
  map sends only its dirty tile-ROW span (`mapRowLo/Hi`, ~640B for a spin;
  full 2KB only on rebuilds). With overscan the budget is ~3.7KB total
  including pvsneslib's OAM DMA — and the flyer's spawn tiles stream one
  320B row per vblank because even 1280B chained late in the NMI overran it.
- Game code only STAGES (mapWrite/dirty flags/palette buffers); boot and
  scene transitions may call `renderVBlank()` directly under force-blank
  (unmetered bandwidth) and must `WaitForVBlank()` before `setScreenOn()`.
- **Overlap sprite/BG handoffs**: never blank BG cells the same frame their
  covering sprites appear (one-frame OAM-vs-map latency = black flash).
  The spin blanks cells at tick 2 under live sprites and lingers the
  sprites one frame over the result (`animTick == 0xFE`).
- Define `RENDER_VBLANK_DIAG=1` (or build with PVSnesLib debug) to track
  per-NMI DMA bytes, worst bytes, and whether the one-large-transfer/budget
  contract was violated.

---

## Game flow

Scenes (main.c): LOGO (mode-3, gravity drop + 2 bounces, silent except
impact SFX, START skips) → TITLE (mode-3 8bpp title.png, credit + blinking
PRESS START baked into the image, blink = CGRAM 255; title theme starts
here; START seeds the RNG from press-timing entropy) → PLAY (mode 1;
`renderGameLoad()` re-uploads ALL VRAM/CGRAM — the title owned everything).

Play state machines (game.c):
- **Spin**: input locked while `animTick != 0xFF`; apply at schedule end;
  `spinsSince++` + suppression tick per spin; then `cascadeCheck()`.
- **Cascade** (CASC_GLOW 14t / FADE 12t / IN 8t): glow rides one CGRAM entry
  white→neon→black; elimination check sits between fade-out and refill;
  refill via `pickRefill()` (suppression-aware); chains until stable
  (depth cap 50); freshness locks at the first wave, resets after clears.
- **Stage complete** (STG_FLASH/OUT/DOWN/UP/PANEL/IN): strobe+shake → BG1
  mosaic dissolve → fade to black → ALL heavy swaps at black (music module
  load ~0.5s blocking, backdrop DMA, stats panel; BG1 dropped from TM via
  `layersSet(0x16)` — parked at mosaic 15 it read as a big pixel blob) →
  fade up → stats hold → BG1 back (`layersSet(0x17)`) + mosaic reveal.
  Phase mechanics (+1 color, phantom reseed, +0.4 heat, +22% drain) apply
  mid-dissolve. START skips the panel hold. No shockwaves during the
  transition (user-removed).
- **Heat**: Q15 (32768 = full), drain accumulated in 1/256ths per frame,
  gain `0.18*n²` per wave (saturating table), pauses during stage
  transitions; empty (checked only while idle) → GAME OVER + gameover
  theme; START restarts with entropy-seeded RNG, phase-1 theme/backdrop.

---

## Audio

snesmod soundbank: `res/sfx.it` FIRST (its samples are the global effects;
order fixes `MOD_*` indices), then music_level1-4/title/gameover. ARAM is
64KB shared (driver + ONE music module + ~17KB SFX) — keep resident effects
≤0.6s (long stingers must become music modules). `audioPlayMusic` is
BLOCKING (~0.5s) and wipes ARAM (effects reload inside) — scene changes
only, masked by blanks/banner pauses. `spcProcess()` every frame, all
scenes. `mid2it.py src name [bpm]` keeps the MIDI's tempo unless overridden
(a stale global override once force-tempo'd a replaced song — overrides are
per-song in the Makefile `songs` target).

---

## Toolchain gotchas (each one cost a debugging round)

- **tcc-816 mangles signed 16-bit division/modulo** — never use runtime
  `/` or `%` on signed values; divide unsigned magnitudes + apply sign;
  rejection-sample instead of modulo. (Unsigned u16 `/10 %10` is fine.)
- **No 32-bit arithmetic** — tcc's long helpers produce garbage. Use u16
  fixed point; bias coordinates positive (e.g. 9.7 with +32px offset) when
  a range would go negative; BCD digit arrays for big numbers.
- **8.8 fixed-point screen coords overflow s16** (x≤228 → 58k): u16 for
  always-positive positions.
- **WRAM is random at power-on; tcc doesn't clear BSS** — every runtime
  static gets zeroed in `renderZeroState()`/`gameInit()`. Symptoms appear
  ONLY on the first cold boot.
- **PPU compositing registers are also random at power-on** (windows,
  sub-screen, color math) — renderInit neutralizes $2123-$2132. snes9x
  zeroes all of this and HIDES these bug classes; bsnes doesn't.
- **BG tile bases are 4K-word aligned** (BG34NBA truncates silently).
- **A section of exactly 32768 bytes** puts its end label out of 16-bit
  range (link error) — hence the 1023-tile backdrop cap.
- **pvsneslib name collisions**: it ships `scoreClear/scoreAdd` (scores.h)
  — ours are `bcd*`.
- tcc needs declarations above use within a file (no reordering passes);
  keep file-scope statics above the functions that touch them.
- `setMode()` resets BG scroll — scroll/mosaic are re-asserted every vblank.
- Generated `boardtab.c/h` are COMMITTED (the Makefile globs sources at
  parse time); regenerate with `make gfx` and commit the result.

---

## When making changes

- **Game rules / scoring / heat**: `src/core/` + extend the golden harness
  (both `tests/gen_golden.mjs` and `tests/host_main.c` — they must emit
  identical lines). Run `make test`.
- **Board look / tile pipeline**: `tools/build_board_gfx.py`, then
  `verify_board_render.py` MUST pass; check the tile count stays ≤192
  (HUD font collision) and eyeball `res/preview.png`.
- **New backdrops/music**: drop files in `backdrops/`/`music/`, run
  `make gfx` / `make songs`, add Makefile entries if new names.
- **Anything visual**: deploy and cold-boot in OpenEmu AND bsnes; bsnes is
  the hardware truth.
- Visual taste (user-established): motion and palette animation over static
  ornament; the board reads flat and bold (`DETAIL_ENABLED=False`).
