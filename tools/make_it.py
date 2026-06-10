#!/usr/bin/env python3
"""Minimal Impulse Tracker (.it) writer for PVSnesLib's smconv.

Two uses:
  - SFX bank: an .it whose SAMPLES are the sound effects (smconv -> BRR, played
    via spcLoadEffect/spcEffect by sample index 1..N).
  - Music: notes/instruments in patterns (composed separately).

Only the subset of the IT format smconv needs is emitted: header, orders,
sample headers + 16-bit signed PCM sample data, and pattern data.

write_it(path, songname, samples, patterns=None, orders=None, channels=8,
         speed=6, tempo=125): samples = list of dicts
  { 'name':str, 'pcm':int16 np.array (mono), 'rate':int, 'loop':(start,end)|None }
"""
import struct
import numpy as np


def _it_pattern(rows, channel_rows):
    """Pack one IT pattern. channel_rows[row] = list of (chan, note, ins, vol, cmd, val).
    note: 0..119 (C5=60), 255=note off blank. Minimal mask packing."""
    out = bytearray()
    last = {}
    for r in range(rows):
        evs = channel_rows.get(r, [])
        for (chan, note, ins, vol, cmd, val) in evs:
            cv = chan & 63
            mask = 0
            maskvar = 0
            if note is not None:
                mask |= 1
            if ins is not None:
                mask |= 2
            if vol is not None:
                mask |= 4
            if cmd is not None:
                mask |= 8
            # always write channel + mask variable (no caching, simpler/robust)
            out.append((cv + 1) | 0x80)
            out.append(mask)
            if note is not None:
                out.append(note & 0xFF)
            if ins is not None:
                out.append(ins & 0xFF)
            if vol is not None:
                out.append(vol & 0xFF)
            if cmd is not None:
                out.append(cmd & 0xFF)
                out.append(val & 0xFF)
        out.append(0)  # end of row
    return bytes(out)


# Volume envelope (same mechanism as PVSnesLib's working overworld.it, which
# snesmod plays): instant attack, hold at full on a single-node sustain loop,
# then a short ~1-row release to 0 on note-off so rests stay clean for chiptune.
_VOL_ENV = [(64, 0), (64, 2), (0, 8)]
_VOL_SUS = 1                          # sustain-loop node (hold here while keyed)


def _it_instrument(name, sample_num):
    """One 554-byte IT instrument mapping every key to `sample_num`.

    snesmod's module player needs instruments (sample-mode .it files are
    silent). Settings mirror overworld.it (NNA=note-off, fadeout, sustaining
    volume envelope) so playback behaves like a known-good module."""
    h = bytearray(554)
    h[0:4] = b'IMPI'
    nm = name.encode('ascii', 'ignore')[:12].ljust(12, b'\x00')
    h[4:16] = nm
    h[0x10] = 0
    h[0x11] = 2          # NNA = note off
    h[0x12] = 0          # DCT off (no duplicate-note killing)
    h[0x13] = 0          # DCA
    struct.pack_into('<H', h, 0x14, 32)   # fadeout
    h[0x16] = 0          # PPS
    h[0x17] = 60         # PPC
    h[0x18] = 128        # global volume
    h[0x19] = 0xA0       # default pan disabled (bit7)
    h[0x1A] = 0          # random vol
    h[0x1B] = 0          # random pan
    struct.pack_into('<H', h, 0x1C, 0x0214)  # TrkVers
    h[0x1E] = 1          # number of samples linked
    h[0x1F] = 0
    h[0x20:0x20+26] = name.encode('ascii', 'ignore')[:26].ljust(26, b'\x00')
    # note -> sample keyboard table (120 * [note, sample])
    for k in range(120):
        h[0x40 + k * 2] = k
        h[0x41 + k * 2] = sample_num & 0xFF
    # volume envelope at 0x130
    env = bytearray(82)
    env[0] = 0x05                          # on + sustain loop
    env[1] = len(_VOL_ENV)                 # node count
    env[2] = 0; env[3] = 0                 # loop begin/end
    env[4] = _VOL_SUS; env[5] = _VOL_SUS   # sustain loop begin/end (hold node)
    for k, (y, tick) in enumerate(_VOL_ENV):
        env[6 + k * 3] = y & 0xFF
        struct.pack_into('<H', env, 7 + k * 3, tick)
    h[0x130:0x130+82] = env
    # pan envelope (0x182) and pitch envelope (0x1D4) left disabled (flags=0)
    return bytes(h)


def write_it(path, songname, samples, patterns=None, orders=None,
             channels=8, speed=6, tempo=125, flags=0x0009):
    nsmp = len(samples)
    ninst = nsmp                       # one instrument per sample (ins i -> smp i)
    flags |= 0x0004                    # use instruments (required by snesmod)
    # default: one empty 64-row pattern, single order
    if patterns is None:
        patterns = [(64, {})]
    if orders is None:
        orders = [0]
    npat = len(patterns)
    order_bytes = bytes([o & 0xFF for o in orders]) + b'\xff'  # 255 = end
    ordnum = len(order_bytes)

    header = bytearray(0xC0)
    header[0:4] = b'IMPM'
    header[4:4+26] = songname.encode('ascii', 'ignore')[:26].ljust(26, b'\x00')
    struct.pack_into('<H', header, 0x1E, 0x0000)       # highlight
    struct.pack_into('<H', header, 0x20, ordnum)
    struct.pack_into('<H', header, 0x22, ninst)        # InsNum (instrument per sample)
    struct.pack_into('<H', header, 0x24, nsmp)
    struct.pack_into('<H', header, 0x26, npat)
    struct.pack_into('<H', header, 0x28, 0x0214)       # Cwt
    struct.pack_into('<H', header, 0x2A, 0x0200)       # Cmwt
    struct.pack_into('<H', header, 0x2C, flags)
    struct.pack_into('<H', header, 0x2E, 0)            # Special
    header[0x30] = 128                                  # GV
    header[0x31] = 48                                   # MV
    header[0x32] = speed
    header[0x33] = tempo
    header[0x34] = 128                                  # pan separation
    header[0x35] = 0
    struct.pack_into('<I', header, 0x38, 0)            # msg offset
    for c in range(64):
        header[0x40 + c] = 32 if c < channels else (32 | 0x80)  # pan center / disabled
    for c in range(64):
        header[0x80 + c] = 64                           # channel volume

    # layout offsets: parapointers are instruments, then samples, then patterns
    pos = 0xC0 + ordnum + ninst * 4 + nsmp * 4 + npat * 4
    ins_off = []
    for _ in range(ninst):
        ins_off.append(pos); pos += 554
    smp_hdr_off = []
    for _ in samples:
        smp_hdr_off.append(pos); pos += 80
    pat_off = []
    pat_blobs = []
    for (rows, chrows) in patterns:
        data = _it_pattern(rows, chrows)
        pat_off.append(pos)
        blob = struct.pack('<HH', len(data), rows) + b'\x00\x00\x00\x00' + data
        pat_blobs.append(blob)
        pos += len(blob)
    # sample data after patterns
    smp_data_off = []
    smp_data = []
    for s in samples:
        pcm = np.asarray(s['pcm'], dtype=np.int16)
        smp_data_off.append(pos)
        raw = pcm.tobytes()
        smp_data.append(raw)
        pos += len(raw)

    buf = bytearray()
    buf += header
    buf += order_bytes
    for o in ins_off:
        buf += struct.pack('<I', o)
    for o in smp_hdr_off:
        buf += struct.pack('<I', o)
    for o in pat_off:
        buf += struct.pack('<I', o)
    # instrument blocks (instrument i maps the whole keyboard to sample i+1)
    for i, s in enumerate(samples):
        buf += _it_instrument(s['name'], i + 1)
    # sample headers
    for i, s in enumerate(samples):
        pcm = np.asarray(s['pcm'], dtype=np.int16)
        length = len(pcm)
        loop = s.get('loop')
        flg = 0x01 | 0x02   # sample present + 16-bit
        ls = le = 0
        if loop:
            flg |= 0x10
            ls, le = loop
        h = bytearray(80)
        h[0:4] = b'IMPS'
        h[4:16] = s['name'].encode('ascii', 'ignore')[:12].ljust(12, b'\x00')
        h[0x10] = 0
        h[0x11] = 64                              # global vol
        h[0x12] = flg
        h[0x13] = 64                              # default vol
        h[0x14:0x14+26] = s['name'].encode('ascii', 'ignore')[:26].ljust(26, b'\x00')
        h[0x2E] = 0x01                            # Cvt: signed
        h[0x2F] = 32
        struct.pack_into('<I', h, 0x30, length)
        struct.pack_into('<I', h, 0x34, ls)
        struct.pack_into('<I', h, 0x38, le)
        struct.pack_into('<I', h, 0x3C, int(s['rate']))  # C5 speed
        struct.pack_into('<I', h, 0x48, smp_data_off[i])
        buf += h
    for blob in pat_blobs:
        buf += blob
    for raw in smp_data:
        buf += raw

    with open(path, 'wb') as f:
        f.write(buf)
    return len(buf)
