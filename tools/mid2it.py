#!/usr/bin/env python3
"""Convert a General MIDI file into a chiptune .it module (for snesmod/smconv)
plus a WAV preview so the result can be auditioned before it goes in the ROM.

A MIDI file is pure composition: note pitches + timing, no audio. That is the
ideal SNES source -- we keep the real song and give every MIDI channel a
real BRR hardware sample from music/pscomp23/ (falling back to a synthesised
single-cycle waveform if a file is missing), and route channel 9 (GM percussion)
to electronic drum one-shots. The whole thing fits in snesmod's 8 channels.

  python3 mid2it.py test.mid          -> res/music_test.it + res/test_preview.wav
  python3 mid2it.py test.mid foo      -> res/music_foo.it  + res/foo_preview.wav

Tuning convention: tracker note 60 == middle C == MIDI note 60, so the IT note
is the MIDI note unchanged. For BRR samples the C5 speed is derived from the
loop's fundamental period so every sample plays in tune at the written note.
"""
import os
import sys
import struct
import numpy as np
from make_it import write_it

HERE = os.path.dirname(os.path.abspath(__file__))
RES  = os.path.join(HERE, "..", "res")
BRR_DIR = os.path.join(HERE, "..", "music", "pscomp23")

WAVE_LEN  = 32
C5_HZ     = 261.63      # tracker note 60 == middle C == MIDI 60
C5_SPEED  = int(round(WAVE_LEN * C5_HZ))
DRATE     = 16000
ROWS_PER_PAT = 64
R            = 8         # rows per quarter note (32nd-note grid)
TARGET_BPM   = None
NOTE_OFF     = 255       # IT "===" -> release the instrument envelope

np.random.seed(1234)


# ---------------------------------------------------------------- MIDI parse --
def parse_midi(path):
    d = open(path, "rb").read()
    assert d[:4] == b"MThd", "not a Standard MIDI File"
    _fmt, _ntrk, div = struct.unpack(">HHH", d[8:14])
    notes = {}
    progs = {}
    tempo = 500000
    pos = 14

    def varlen(p):
        v = 0
        while True:
            b = d[p]; p += 1; v = (v << 7) | (b & 0x7F)
            if not b & 0x80:
                break
        return v, p

    maxtick = 0
    while pos < len(d) and d[pos:pos + 4] == b"MTrk":
        tlen = struct.unpack(">I", d[pos + 4:pos + 8])[0]
        p = pos + 8; tend = p + tlen
        t = 0; status = 0; active = {}
        while p < tend:
            dt, p = varlen(p); t += dt
            b = d[p]
            if b & 0x80:
                status = b; p += 1
            ev = status & 0xF0; ch = status & 0x0F
            if status == 0xFF:
                mt = d[p]; p += 1; ml, p = varlen(p)
                if mt == 0x51 and ml == 3:
                    tempo = struct.unpack(">I", b"\x00" + d[p:p + 3])[0]
                p += ml
            elif status in (0xF0, 0xF7):
                ml, p = varlen(p); p += ml
            elif ev == 0x90:
                note = d[p]; vel = d[p + 1]; p += 2
                if vel > 0:
                    active.setdefault((ch, note), []).append((t, vel))
                else:
                    q = active.get((ch, note))
                    if q:
                        s, v = q.pop(0)
                        notes.setdefault(ch, []).append((s, t, note, v))
            elif ev == 0x80:
                note = d[p]; p += 2
                q = active.get((ch, note))
                if q:
                    s, v = q.pop(0)
                    notes.setdefault(ch, []).append((s, t, note, v))
            elif ev == 0xC0:
                progs[ch] = d[p]; p += 1
            elif ev in (0xA0, 0xB0, 0xE0):
                p += 2
            elif ev == 0xD0:
                p += 1
            else:
                p += 1
        pos = tend
        if t > maxtick:
            maxtick = t
    return div, tempo, progs, notes, maxtick


# ------------------------------------------------------- BRR sample loading --
def _decode_brr(data):
    """Decode raw BRR bytes (no 2-byte header) to int16 numpy array."""
    samples, p1, p2 = [], 0, 0
    for i in range(0, len(data), 9):
        blk = data[i:i + 9]
        if len(blk) < 9:
            break
        hdr   = blk[0]
        shift = (hdr >> 4) & 0xF
        filt  = (hdr >> 2) & 0x3
        end   = hdr & 1
        for j in range(16):
            n   = (blk[1 + j // 2] >> (4 * (1 - j % 2))) & 0xF
            raw = n - 16 if n >= 8 else n
            s   = (raw << shift) >> 1 if shift <= 12 else (raw >> 3) << 12
            if   filt == 1: s += p1 - (p1 >> 4)
            elif filt == 2: s += 2*p1 - ((3*p1) >> 5) - p2 + (p2 >> 4)
            elif filt == 3: s += 2*p1 - ((13*p1) >> 6) - p2 + ((3*p2) >> 4)
            s = max(-16384, min(16383, s))
            samples.append(s)
            p2, p1 = p1, s
        if end:
            break
    return np.array(samples, dtype=np.int16)


def _c5_from_loop(pcm, ls, le):
    """Find fundamental period P via autocorrelation; return (c5, adjusted_le).

    The IT loop is trimmed to (ls, ls+P_aligned) where P_aligned is P rounded
    up to the nearest 16-sample SNES BRR block boundary.  This is critical:
    the IT pitch formula is c5/loop_length at note 60, so loop_length MUST
    equal the sample period for correct tuning.  Using the full multi-cycle
    loop length would require an impossibly large c5 that overflows the SNES
    14-bit pitch register.
    """
    seg = pcm[ls:le].astype(np.float64)
    n   = len(seg)
    if n < 4:
        period = n
    else:
        ext = np.tile(seg, 4)
        nn  = 1 << (len(ext) - 1).bit_length()
        F   = np.fft.rfft(ext, n=nn)
        acf = np.fft.irfft(F * np.conj(F))[:n]
        if acf[0] <= 0:
            period = n
        else:
            acf /= acf[0]
            lo  = max(4, n // 64)
            sub = acf[lo:n]
            period = lo + int(np.argmax(sub)) if len(sub) else n

    # Round up to 16-sample (one BRR block) boundary
    period_aligned = max(16, ((period + 15) // 16) * 16)
    # Clamp: must not extend past original loop end
    max_period = ((le - ls) // 16) * 16
    if max_period < 16:
        max_period = le - ls
    period_aligned = min(period_aligned, max_period)
    new_le = ls + period_aligned
    c5 = max(1, int(round(C5_HZ * period_aligned)))
    return c5, new_le


def _load_brr(rel_path, name, drum=False):
    """Load one pscomp23 BRR file -> sample dict.  Returns None if file missing."""
    full = os.path.join(BRR_DIR, rel_path)
    if not os.path.exists(full):
        return None
    raw  = open(full, "rb").read()
    loop_off = struct.unpack_from("<H", raw, 0)[0]
    pcm      = _decode_brr(raw[2:])
    total    = (len(raw) - 2) // 9 * 16
    if drum or loop_off == 0 or (loop_off % 9) != 0:
        return {"name": name, "pcm": pcm, "rate": 32000, "loop": None}
    ls = (loop_off // 9) * 16
    le = min(total, len(pcm))
    if ls >= le:
        return {"name": name, "pcm": pcm, "rate": 32000, "loop": None}
    c5, new_le = _c5_from_loop(pcm, ls, le)
    return {"name": name, "pcm": pcm, "rate": c5, "loop": (ls, new_le)}


# Sample bank layout (1-based indices match IT instrument numbering):
#   1=PIANO(ch0 Lead)  2=PAD(ch1 Harmony)  3=BASS(ch2 Bass)
#   4=ARP(ch3 Motion/Arp)  5=BELL(ch4 Accents)
#   6=KICK  7=SNARE  8=HAT  9=CRASH
BRR_BANK = [
    # (relative path under pscomp23/,   display name,  is_drum)
    ("Keys/Acoustic Grand C4.brr",               "PIANO",  False),  # ch0: Lead melody
    ("Synth Pad/Fifth Ensemble.brr",             "FIFTHEN",False),  # ch1: Harmony chords (A#2-G4)
    ("Bass/Triangle Bass.brr",                   "BASS",   False),  # ch2: Bass line (smooth triangle wave, not buzzy)
    ("Mallet/Carillon.brr",                      "ARP",    False),  # ch3: Motion/arp (D4-C5)
    ("Ensemble/Viola Solo.brr",                  "VIOLA",  False),  # ch4: Accents (A5-C7) — warm string sweep
    ("Drum Kit/Electronic/Bass Drum Electric 1.brr", "KICK",  True),
    ("Drum Kit/Electronic/Snare Tight.brr",          "SNARE", True),
    ("Drum Kit/Electronic/Hi-Hat Closed 4.brr",      "HAT",   True),
    ("Drum Kit/Electronic/Crash Cymbal Shower.brr",  "CRASH", True),
]

PIANO, FIFTHEN, BASS, ARP, VIOLA, KICK, SNARE, HAT, CRASH = range(1, 10)


# ----------------------------------------------------------------- synthesis --
# Fallback waveforms used only when a BRR file is missing.
def _cycle(kind):
    t = np.arange(WAVE_LEN) / WAVE_LEN
    if kind == "square":      w = np.where(t < 0.5, 1.0, -1.0)
    elif kind == "pulse25":   w = np.where(t < 0.25, 1.0, -1.0)
    elif kind == "pulse12":   w = np.where(t < 0.125, 1.0, -1.0)
    elif kind == "saw":       w = 2.0 * t - 1.0
    elif kind == "triangle":  w = 2.0 * np.abs(2.0 * (t - np.floor(t + 0.5))) - 1.0
    else:                     w = np.zeros(WAVE_LEN)
    return (w * 20000).astype(np.int16)


def _kick():
    n = int(0.11 * DRATE); t = np.arange(n) / DRATE
    f = 150.0 * np.exp(-t * 32) + 48.0
    ph = np.cumsum(2 * np.pi * f / DRATE)
    body = np.sin(ph) * np.exp(-t * 26)
    click = np.zeros(n); click[:120] = np.random.uniform(-1, 1, 120) * np.exp(-np.arange(120) / 30)
    return (np.clip(body + 0.3 * click, -1, 1) * 30000).astype(np.int16)


def _snare():
    n = int(0.13 * DRATE); t = np.arange(n) / DRATE
    noise = np.random.uniform(-1, 1, n) * np.exp(-t * 22)
    tone = np.sin(2 * np.pi * 190 * t) * np.exp(-t * 30) * 0.5
    return (np.clip(0.8 * noise + tone, -1, 1) * 24000).astype(np.int16)


def _hat():
    n = int(0.035 * DRATE); t = np.arange(n) / DRATE
    noise = np.random.uniform(-1, 1, n)
    noise = np.diff(np.concatenate([[0], noise]))
    return (np.clip(noise * np.exp(-t * 90), -1, 1) * 16000).astype(np.int16)


def _crash():
    n = int(0.45 * DRATE); t = np.arange(n) / DRATE
    noise = np.random.uniform(-1, 1, n)
    noise = np.diff(np.concatenate([[0], noise]))
    return (np.clip(noise * np.exp(-t * 7), -1, 1) * 14000).astype(np.int16)


_FALLBACKS = [
    lambda: {"name": "PIANO",   "pcm": _cycle("triangle"), "rate": C5_SPEED, "loop": (0, WAVE_LEN)},
    lambda: {"name": "FIFTHEN", "pcm": _cycle("saw"),      "rate": C5_SPEED, "loop": (0, WAVE_LEN)},
    lambda: {"name": "BASS",    "pcm": _cycle("square"),   "rate": C5_SPEED, "loop": (0, WAVE_LEN)},
    lambda: {"name": "ARP",     "pcm": _cycle("pulse25"),  "rate": C5_SPEED, "loop": (0, WAVE_LEN)},
    lambda: {"name": "VIOLA",   "pcm": _cycle("triangle"), "rate": C5_SPEED, "loop": (0, WAVE_LEN)},
    lambda: {"name": "KICK",  "pcm": _kick(),  "rate": DRATE, "loop": None},
    lambda: {"name": "SNARE", "pcm": _snare(), "rate": DRATE, "loop": None},
    lambda: {"name": "HAT",   "pcm": _hat(),   "rate": DRATE, "loop": None},
    lambda: {"name": "CRASH", "pcm": _crash(), "rate": DRATE, "loop": None},
]


def instruments():
    # Re-seed so drum noise is identical across all modules built in one process.
    np.random.seed(1234)
    smps = []
    for idx, (rel, name, drum) in enumerate(BRR_BANK):
        s = _load_brr(rel, name, drum)
        if s is None:
            s = _FALLBACKS[idx]()
            print(f"  [warn] {rel} not found, using synthetic fallback")
        smps.append(s)
    # Decay envelopes: attack-to-silent, no sustain hold.
    # speed=6/tempo≈132: ~7ms/tick → 220 ticks ≈ 1.5s, 110 ticks ≈ 0.75s
    smps[0]['decay'] = 220   # PIANO:  natural fade after strike
    smps[3]['decay'] = 110   # ARP:    carillon bell ring fades out
    # smps[4] VIOLA (ch4 Accents) intentionally no decay — sustains/sweeps while held
    return smps


# --------------------------------------------------- channel/instrument map --
# l1-l4 MIDI layout (CHANNEL_MAP_README.txt standard):
#   ch0=Lead, ch1=Harmony, ch2=Bass, ch3=Motion/Arp, ch4=Accents, ch9=Drums
# MEL: midi_ch -> (it_channel, sample_index, gain, semitone_transpose)
MEL = {
    0: (0, PIANO,    1.00,  0),  # ch0: Lead (A4-C6)       -> Acoustic Grand
    1: (1, FIFTHEN,  0.68,  0),  # ch1: Harmony (A#2-G4)   -> Fifth Ensemble
    2: (2, BASS,     1.00, +24), # ch2: Bass (A#0-E2 +2oct) -> Triangle Bass, shift to A#2-E4
    3: (3, ARP,      0.88,  0),  # ch3: Arp (D4-C5)        -> Carillon
    4: (4, VIOLA,    0.80, -12), # ch4: Accents (A5-C7 -oct) -> Viola Solo A4-C6, warm string sweep
}


def drum_voice(note):
    if note in (35, 36):                       return (6, KICK,  1.00)
    if note in (37, 38, 39, 40):               return (6, SNARE, 0.95)
    if note in (49, 51, 52, 53, 55, 57, 59):   return (6, CRASH, 0.80)
    if note in (42, 44, 46, 54, 69, 70):       return (7, HAT,   0.62)
    return (6, SNARE, 0.65)


# --------------------------------------------------------------- conversion --
def build_events(div, notes):
    tpr = max(1, div // R)
    ev  = {c: [] for c in range(8)}
    for ch, (itc, ins, g, xpose) in MEL.items():
        for (s, e, note, vel) in notes.get(ch, []):
            sr = int(round(s / tpr)); er = int(round(e / tpr))
            if er <= sr:
                er = sr + 1
            note += xpose
            while note > 119: note -= 12
            while note < 0:   note += 12
            vol = max(1, min(64, int(round(vel / 127 * 64 * g))))
            ev[itc].append((sr, er, note, ins, vol))
    for (s, e, note, vel) in notes.get(9, []):
        itc, ins, g = drum_voice(note)
        sr  = int(round(s / tpr))
        vol = max(1, min(64, int(round(vel / 127 * 64 * g))))
        ev[itc].append((sr, sr + 1, 60, ins, vol))
    return tpr, ev


def to_patterns(ev, loop_rows=None):
    glob = {}

    def put(r, itc, note, ins, vol):
        glob.setdefault(r, []).append((itc, note, ins, vol, None, None))

    for itc in range(8):
        best = {}
        for rec in ev[itc]:
            sr = rec[0]
            if sr not in best:
                best[sr] = rec
            elif itc < 6:
                if rec[2] > best[sr][2]:
                    best[sr] = rec
            elif rec[4] > best[sr][4]:
                best[sr] = rec
        seq = [best[k] for k in sorted(best)]
        for i, (sr, er, note, ins, vol) in enumerate(seq):
            put(sr, itc, note, ins, vol)
            if itc < 6:
                nxt = seq[i + 1][0] if i + 1 < len(seq) else None
                if nxt is None or nxt > er:
                    put(er, itc, NOTE_OFF, None, None)
    if not glob:
        return [(ROWS_PER_PAT, {})], [0]
    maxrow = max(glob) + 1
    total  = loop_rows if (loop_rows and loop_rows >= maxrow - 2) else maxrow
    npat   = (total + ROWS_PER_PAT - 1) // ROWS_PER_PAT
    patterns = []
    for pi in range(npat):
        base = pi * ROWS_PER_PAT
        rows = min(ROWS_PER_PAT, total - base)
        if rows <= 0:
            break
        chrows = {}
        for r in range(rows):
            evs = glob.get(base + r)
            if evs:
                chrows[r] = evs
        patterns.append((rows, chrows))
    return patterns, list(range(len(patterns)))


# ------------------------------------------------------------- WAV preview ---
def render_wav(path, samples, ev, it_tempo, speed=6, out_rate=32000):
    row_sec = 2.5 * speed / it_tempo
    end_row = max((rec[1] for c in range(8) for rec in ev[c]), default=1)
    buf = np.zeros(int((end_row + 4) * row_sec * out_rate), np.float32)

    def play(note, ins, vol, start, dur):
        s   = samples[ins - 1]
        pcm = s["pcm"].astype(np.float32)
        loop = s.get("loop")
        c5   = s.get("rate", C5_SPEED)
        s0   = int(start * out_rate)
        if loop:
            n = int(dur * out_rate)
            if n <= 0:
                return
            spd = c5 * (2.0 ** ((note - 60) / 12.0)) / out_rate
            idx = np.arange(n, dtype=np.float64) * spd
            ls, le = loop
            wave = pcm[(ls + np.mod(idx, le - ls)).astype(np.intp)]
            env  = np.ones(n)
            a    = min(64, n // 4)
            if a > 0:
                env[:a]  = np.linspace(0, 1, a)
                env[-a:] = np.linspace(1, 0, a)
            wave = wave * env
        else:
            spd  = c5 / out_rate
            n    = max(1, int(len(pcm) / spd))
            idx  = np.minimum(
                (np.arange(n, dtype=np.float64) * spd).astype(np.intp),
                len(pcm) - 1)
            wave = pcm[idx]
        g   = vol / 64.0
        seg = wave * g / 32768.0
        end = min(s0 + n, len(buf))
        if end > s0:
            buf[s0:end] += seg[:end - s0]

    for itc in range(8):
        for (sr, er, note, ins, vol) in ev[itc]:
            dur = (er - sr) * row_sec if itc < 6 else 2.0
            play(note, ins, vol, sr * row_sec, dur)

    mixed = np.tanh(buf * 0.5) / np.tanh(0.5) * 0.9
    pcm   = np.clip(mixed * 32767, -32768, 32767).astype("<i2")
    data  = pcm.tobytes()
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt ")
        f.write(struct.pack("<IHHIIHH", 16, 1, 1, out_rate, out_rate * 2, 2, 16))
        f.write(b"data" + struct.pack("<I", len(data)) + data)


# ----------------------------------------------------------- section looping --
IT_FX_POSJUMP = 2


def prepare(src):
    div, tempo, progs, notes, maxtick = parse_midi(src)
    midi_bpm = round(60_000_000 / tempo)
    bpm      = TARGET_BPM if TARGET_BPM else midi_bpm
    it_speed = 5                               # speed=5 + tempo=bpm*2 → 20% faster than speed=6 baseline
    it_tempo = max(32, min(255, int(round(bpm * 2.3))))  # 2x baseline + 15% speedup
    tpr, ev  = build_events(div, notes)
    loop_rows = int(round(maxtick / tpr)) if maxtick else None
    patterns, orders = to_patterns(ev, loop_rows)
    return patterns, orders, ev, it_tempo, it_speed, midi_bpm, bpm, tpr


def set_loop_jump(patterns, target_order):
    rows, chrows = patterns[-1]
    chrows = dict(chrows)
    last   = rows - 1
    evs    = list(chrows.get(last, []))
    if evs:
        ch, note, ins, vol, _c, _v = evs[0]
        evs[0] = (ch, note, ins, vol, IT_FX_POSJUMP, target_order)
    else:
        evs = [(0, None, None, None, IT_FX_POSJUMP, target_order)]
    chrows[last] = evs
    patterns[-1] = (rows, chrows)


# --------------------------------------------------------------- module build --
def _print_samples(smps):
    for i, s in enumerate(smps):
        c5   = s.get("rate", 0)
        loop = s.get("loop")
        tag  = f"loop={loop[0]}-{loop[1]}" if loop else "one-shot"
        print(f"  [{i+1}] {s['name']:6s}  c5={c5:6d}  {tag}")


def build_standalone(src, name):
    patterns, orders, ev, it_tempo, it_speed, midi_bpm, bpm, tpr = prepare(src)
    smps    = instruments()
    it_path = os.path.join(RES, f"music_{name}.it")
    size    = write_it(it_path, f"ROTATRON {name.upper()}", smps,
                       patterns=patterns, orders=orders,
                       channels=8, speed=it_speed, tempo=it_tempo)
    wav_path = os.path.join(RES, f"{name}_preview.wav")
    render_wav(wav_path, smps, ev, it_tempo, speed=it_speed)
    counts = {c: len(ev[c]) for c in range(8) if ev[c]}
    print(f"src={src}  midi {midi_bpm}bpm -> {bpm}bpm / IT tempo {it_tempo}/speed{it_speed}  ({R} rows/quarter, {tpr} ticks/row)")
    print(f"IT channels (events): {counts}")
    _print_samples(smps)
    print(f"{len(patterns)} patterns x {ROWS_PER_PAT} rows, {len(orders)} orders")
    print(f"wrote {it_path} ({size} bytes)")
    print(f"wrote {wav_path}  <-- listen to verify it's the song")
    return size


def build_combined(frantic_src, calm_src, name):
    fpat, _fo, _fev, ftempo, fspeed, *_ = prepare(frantic_src)
    cpat, _co, cev, ctempo, cspeed, *_  = prepare(calm_src)
    calm_start = len(fpat)
    set_loop_jump(fpat, 0)
    set_loop_jump(cpat, calm_start)
    patterns = fpat + cpat
    orders   = list(range(len(patterns)))
    smps     = instruments()
    it_path  = os.path.join(RES, f"music_{name}.it")
    size     = write_it(it_path, f"ROTATRON {name.upper()}", smps,
                        patterns=patterns, orders=orders,
                        channels=8, speed=cspeed, tempo=ctempo)
    wav_path = os.path.join(RES, f"{name}_preview.wav")
    render_wav(wav_path, smps, cev, ctempo, speed=cspeed)
    print(f"[combine] {name}: frantic {len(fpat)}pat + calm {len(cpat)}pat "
          f"-> calm_start={calm_start}, {len(patterns)} orders, {size}B -> {it_path}")
    return size, calm_start


# --------------------------------------------------------------------- main --
def main():
    global TARGET_BPM
    if len(sys.argv) > 1 and sys.argv[1] == "--combine":
        build_combined(sys.argv[2], sys.argv[3], sys.argv[4])
        return
    src  = sys.argv[1] if len(sys.argv) > 1 else "test.mid"
    name = sys.argv[2] if len(sys.argv) > 2 else os.path.splitext(os.path.basename(src))[0]
    if len(sys.argv) > 3:
        TARGET_BPM = int(sys.argv[3])
    build_standalone(src, name)


if __name__ == "__main__":
    main()
