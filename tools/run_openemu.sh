#!/usr/bin/env bash
# Deploy the freshly built ROM into OpenEmu's library (overwriting the existing
# 'rotatron' entry, so no duplicate imports) and bring OpenEmu to the front.
set -e
PROJ="$(cd "$(dirname "$0")/.." && pwd)"
ROM="$PROJ/rotatron.sfc"
LIBDIR="$HOME/Library/Application Support/OpenEmu/Game Library/roms/Super Nintendo (SNES)"
LIB="$LIBDIR/rotatron.sfc"

if [ ! -f "$ROM" ]; then echo "No $ROM — run 'make' first." >&2; exit 1; fi

# OpenEmu auto-resumes a save state on relaunch; after a rebuild the ROM layout
# shifts, so resuming an old state freezes. Clear the auto-save-state so each
# deploy cold-boots the new ROM.
AUTOSTATE="$HOME/Library/Application Support/OpenEmu/Save States/SuperNES/rotatron/Auto Save State.oesavestate"
if [ -e "$AUTOSTATE" ]; then
    rm -rf "$AUTOSTATE"
    echo "Cleared stale OpenEmu auto-save-state (forces a cold boot of the new ROM)."
fi

if [ -f "$LIB" ]; then
    cp -f "$ROM" "$LIB"
    echo "Updated OpenEmu copy: $LIB"
    open -a OpenEmu
    echo "OpenEmu is up — double-click 'rotatron' to (re)launch with this build."
else
    echo "No OpenEmu copy yet — importing $ROM into OpenEmu..."
    open -a OpenEmu "$ROM"
    echo "Imported. Double-click 'rotatron' in OpenEmu to play; future builds auto-update it."
fi
