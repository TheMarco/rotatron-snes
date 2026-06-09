#!/usr/bin/env python3
"""Set the FastROM speed bit in a LoROM SNES header and fix the checksum.

The WLA-DX bundled with this PVSnesLib accepts the FASTROM directive in
.SNESHEADER but still emits a SlowROM mode byte ($20) at $7FD5, so the
Makefile runs this on the linked ROM: $7FD5 |= $10 (-> $30, LoROM+FastROM),
then the checksum/complement at $7FDC-$7FDF are recomputed so accurate
emulators and flash carts don't flag the ROM as corrupted. Idempotent.
"""
import sys

MODE = 0x7FD5            # LoROM header: ROM makeup byte
CKS = 0x7FDC             # complement (2) + checksum (2)

path = sys.argv[1]
with open(path, "rb") as f:
    rom = bytearray(f.read())

rom[MODE] |= 0x10        # FastROM speed bit

rom[CKS:CKS + 4] = b"\xFF\xFF\x00\x00"   # canonical fill before summing
s = sum(rom) & 0xFFFF
rom[CKS + 2] = s & 0xFF
rom[CKS + 3] = s >> 8
c = s ^ 0xFFFF
rom[CKS] = c & 0xFF
rom[CKS + 1] = c >> 8

with open(path, "wb") as f:
    f.write(rom)
print(f"{path}: mode byte ${rom[MODE]:02X} (LoROM FastROM), checksum ${s:04X}")
