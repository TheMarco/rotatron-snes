# Rotatron SNES - build via PVSnesLib (same pattern as deadfall-snes).
PVSNESLIB_HOME ?= $(shell cat .pvsneslib_home 2>/dev/null)

ifeq ($(strip $(PVSNESLIB_HOME)),)
$(error PVSNESLIB_HOME is not set. Export it or write the path into .pvsneslib_home)
endif

# snesmod soundbank: the SFX bank MUST be first (its samples are the global
# effects; order also fixes the MOD_ indices used in audio.c).
MUSICFILES := res/music_level1.it res/music_title.it res/music_gameover.it \
  res/music_level2.it res/music_level3.it res/music_level4.it
AUDIOFILES := res/sfx.it $(MUSICFILES)
export SOUNDBANK := res/soundbank

# Real FastROM, with a twist: FASTROM=1 makes tcc emit -F (our C code's
# labels land in the $80+ bank mirrors -> 3.58MHz once renderInit sets
# MEMSEL $420D), but this install's LoROM_FastROM lib objects are broken
# (duplicate section labels at link), so the link keeps the good SlowROM
# libs. Lib code (oamSet, dma helpers) stays at 2.68MHz; all of OUR game/
# render/core code and tables run fast. set_fastrom.py still patches the
# header mode byte post-link.
FASTROM := 1

include ${PVSNESLIB_HOME}/devkitsnes/snes_rules

LIBDIRSOBJS := $(PVSNESLIB_HOME)/pvsneslib/lib/LoROM_SlowROM
LIBDIRSOBJSW := $(LIBDIRSOBJS)

# smconv: soundbank mode, verbose, modules in ROM bank 5+, check sizes
SMCONVFLAGS := -s -o $(SOUNDBANK) -V -b 5 -f

# The soundbank's ROM-bank count varies with the music set; generate the
# matching spcSetBank list and compile audio.c only after it exists.
include/soundbank_banks.h: $(SOUNDBANK).asm
	@python3 tools/gen_soundbank_banks.py $< $@
src/audio.obj: include/soundbank_banks.h

.PHONY: all rom run gfx test clean

export ROMNAME := rotatron

CFLAGS += -I$(CURDIR)/include

# .incbin deps aren't auto-tracked: reassemble ROM data whenever converted assets change.
data.obj: data.asm hdr.asm $(wildcard res/*.pic) $(wildcard res/*.pal) $(wildcard res/*.map)

# render.c bakes in the generated ambient sprite table; recompile when it changes.
src/render.obj: include/ambtab.h

# Build with the project-local portable `sed` shim on PATH (snes_rules uses GNU
# `sed -i`, which macOS BSD sed rejects).
all:
	@PATH="$(CURDIR)/tools/bin:$$PATH" $(MAKE) --no-print-directory rom

# Post-link: WLA-DX ignores hdr.asm's FASTROM directive, so patch the speed bit
# ($7FD5 -> $30) and fix the checksum.
rom: $(ROMNAME).sfc
	@python3 tools/set_fastrom.py $(ROMNAME).sfc

run: all
	@bash tools/run_openemu.sh

# Generate board tileset/palette/tables + PNG preview from the geometry spec,
# and the BG2 backdrop from backdrops/level1.png.
gfx:
	python3 tools/build_board_gfx.py
	python3 tools/build_backdrop.py
	python3 tools/build_title8.py

# Rebuild music modules from music/*.mid (also writes res/*_preview.wav)
# and the SFX bank from music/s-*.mp3.
songs:
	python3 tools/mid2it.py music/level1.mid level1
	python3 tools/mid2it.py music/level2.mid level2
	python3 tools/mid2it.py music/level3.mid level3
	python3 tools/mid2it.py music/level4.mid level4
	python3 tools/mid2it.py music/title.mid title 145
	python3 tools/mid2it.py music/gameover.mid gameover 115
	python3 tools/build_audio.py

# Host-side golden tests: compile the core game logic with clang and compare
# against vectors generated from the web game's JS modules.
test:
	@bash tests/run_host_tests.sh

clean: cleanBuildRes cleanRom cleanGfx cleanAudio
	@rm -f src/*.ps src/*.asp src/*.asm src/*.obj src/core/*.ps src/core/*.asp src/core/*.asm src/core/*.obj
	@rm -f *.obj linkfile
	@rm -rf tests/build
