#!/usr/bin/env python3
"""Convert a General MIDI file into a chiptune .it module (for snesmod/smconv)
plus a WAV preview so the result can be auditioned before it goes in the ROM.

A MIDI file is pure composition: note pitches + timing, no audio. That is the
ideal SNES source -- we keep the real song and just give every MIDI channel a
tiny synthesised chiptune voice (a single-cycle waveform, ~64 bytes, looped),
and route channel 9 (GM percussion) to short drum one-shots. The whole thing
fits in snesmod's 8 channels and a few KB of ARAM.

  python3 mid2it.py test.mid          -> res/music_test.it + res/test_preview.wav
  python3 mid2it.py test.mid foo      -> res/music_foo.it  + res/foo_preview.wav

Tuning convention: tracker note 60 == middle C == MIDI note 60, so the IT note
is the MIDI note unchanged. WAVE_LEN is kept small so even high notes stay under
the SNES 32kHz playback ceiling (rate = freq * WAVE_LEN).
"""
import os
import sys
import struct
import numpy as np
from make_it import write_it

HERE = os.path.dirname(os.path.abspath(__file__))
RES = os.path.join(HERE, "..", "res")

WAVE_LEN = 32
C5_HZ = 261.63                        # tracker note 60 == middle C == MIDI 60
C5_SPEED = int(round(WAVE_LEN * C5_HZ))
DRATE = 16000                         # drum-sample rate
ROWS_PER_PAT = 64
R = 8                                 # rows per quarter note (32nd-note grid)
TARGET_BPM = 130                      # force every track to this tempo (the user's sweet
                                      # spot); set to None to keep each MIDI's own tempo
NOTE_OFF = 255                        # IT "===" -> release the instrument envelope

np.random.seed(1234)                  # deterministic drum noise


# ---------------------------------------------------------------- MIDI parse --
def parse_midi(path):
    d = open(path, "rb").read()
    assert d[:4] == b"MThd", "not a Standard MIDI File"
    _fmt, _ntrk, div = struct.unpack(">HHH", d[8:14])
    notes = {}            # midi-chan -> [(start_tick, end_tick, note, vel)]
    progs = {}
    tempo = 500000        # us per quarter (120bpm default)
    pos = 14

    def varlen(p):
        v = 0
        while True:
            b = d[p]; p += 1; v = (v << 7) | (b & 0x7F)
            if not b & 0x80:
                break
        return v, p

    maxtick = 0                       # song length (longest track end) for tight looping
    while pos < len(d) and d[pos:pos + 4] == b"MTrk":
        tlen = struct.unpack(">I", d[pos + 4:pos + 8])[0]
        p = pos + 8; tend = p + tlen
        t = 0; status = 0; active = {}
        while p < tend:
            dt, p = varlen(p); t += dt
            b = d[p]
            if b & 0x80:
                status = b; p += 1            # new status
            ev = status & 0xF0; ch = status & 0x0F
            if status == 0xFF:                # meta
                mt = d[p]; p += 1; ml, p = varlen(p)
                if mt == 0x51 and ml == 3:
                    tempo = struct.unpack(">I", b"\x00" + d[p:p + 3])[0]
                p += ml
            elif status in (0xF0, 0xF7):      # sysex
                ml, p = varlen(p); p += ml
            elif ev == 0x90:                  # note on
                note = d[p]; vel = d[p + 1]; p += 2
                if vel > 0:
                    active.setdefault((ch, note), []).append((t, vel))
                else:                          # vel 0 == note off
                    q = active.get((ch, note))
                    if q:
                        s, v = q.pop(0)
                        notes.setdefault(ch, []).append((s, t, note, v))
            elif ev == 0x80:                  # note off
                note = d[p]; p += 2
                q = active.get((ch, note))
                if q:
                    s, v = q.pop(0)
                    notes.setdefault(ch, []).append((s, t, note, v))
            elif ev == 0xC0:                  # program change
                progs[ch] = d[p]; p += 1
            elif ev in (0xA0, 0xB0, 0xE0):    # 2-byte channel msgs
                p += 2
            elif ev == 0xD0:                  # channel pressure
                p += 1
            else:
                p += 1
        pos = tend
        if t > maxtick:
            maxtick = t
    return div, tempo, progs, notes, maxtick


# ----------------------------------------------------------------- synthesis --
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
    noise = np.diff(np.concatenate([[0], noise]))      # crude high-pass -> bright
    return (np.clip(noise * np.exp(-t * 90), -1, 1) * 16000).astype(np.int16)


def _crash():
    n = int(0.45 * DRATE); t = np.arange(n) / DRATE
    noise = np.random.uniform(-1, 1, n)
    noise = np.diff(np.concatenate([[0], noise]))
    return (np.clip(noise * np.exp(-t * 7), -1, 1) * 14000).astype(np.int16)


# sample bank (1-based instrument indices, as smconv uses samples directly)
TRI, SQR, P25, P12, SAW, KICK, SNARE, HAT, CRASH = range(1, 10)


def instruments():
    # Re-seed so the drum noise is identical for EVERY module, even when several
    # are built in one process (build_songs imports and calls us repeatedly): the
    # combined level modules must share the exact same sample bank as each other.
    np.random.seed(1234)
    cyc = lambda k: {"name": k.upper(), "pcm": _cycle(k), "rate": C5_SPEED, "loop": (0, WAVE_LEN)}
    return [
        cyc("triangle"),                                             # 1 lead/flute
        cyc("square"),                                               # 2 bass
        cyc("pulse25"),                                              # 3 harp
        cyc("pulse12"),                                              # 4 bells
        cyc("saw"),                                                  # 5 strings
        {"name": "KICK",  "pcm": _kick(),  "rate": DRATE, "loop": None},
        {"name": "SNARE", "pcm": _snare(), "rate": DRATE, "loop": None},
        {"name": "HAT",   "pcm": _hat(),   "rate": DRATE, "loop": None},
        {"name": "CRASH", "pcm": _crash(), "rate": DRATE, "loop": None},
    ]


# --------------------------------------------------- channel/instrument map --
# midi channel -> (IT channel 0-7, instrument, volume gain)
MEL = {
    0: (0, TRI, 1.00),    # Flute (lead)
    1: (1, SQR, 1.00),    # Bass
    2: (2, P25, 0.85),    # Harp
    3: (3, P12, 0.70),    # Bells
    4: (4, SAW, 0.60),    # Strings L
    5: (5, SAW, 0.60),    # Strings R
}


def drum_voice(note):
    if note in (35, 36):                       return (6, KICK, 1.00)
    if note in (37, 38, 39, 40):               return (6, SNARE, 0.95)
    if note in (49, 51, 52, 53, 55, 57, 59):   return (6, CRASH, 0.80)
    if note in (42, 44, 46, 54, 69, 70):       return (7, HAT, 0.62)
    return (6, SNARE, 0.65)                     # toms / misc -> snare body


# --------------------------------------------------------------- conversion --
def build_events(div, notes):
    """Return (ticks_per_row, ev) where ev[itc] = [(start_row,end_row,note,ins,vol)]."""
    tpr = max(1, div // R)
    ev = {c: [] for c in range(8)}
    for ch, (itc, ins, g) in MEL.items():
        for (s, e, note, vel) in notes.get(ch, []):
            sr = int(round(s / tpr)); er = int(round(e / tpr))
            if er <= sr:
                er = sr + 1
            while note > 119:
                note -= 12
            while note < 0:
                note += 12
            vol = max(1, min(64, int(round(vel / 127 * 64 * g))))
            ev[itc].append((sr, er, note, ins, vol))
    for (s, e, note, vel) in notes.get(9, []):
        itc, ins, g = drum_voice(note)
        sr = int(round(s / tpr))
        vol = max(1, min(64, int(round(vel / 127 * 64 * g))))
        ev[itc].append((sr, sr + 1, 60, ins, vol))     # one-shot at native pitch
    return tpr, ev


def to_patterns(ev, loop_rows=None):
    """Flatten per-channel events into global rows -> IT patterns + order list.
    loop_rows = the song's true length (from the MIDI) so the module ends exactly
    there and loops seamlessly, instead of padding the last pattern out to a full
    64 rows (which plays as a silent gap at the loop seam)."""
    glob = {}

    def put(r, itc, note, ins, vol):
        glob.setdefault(r, []).append((itc, note, ins, vol, None, None))

    for itc in range(8):
        # one note per row per channel (monophonic voice); pick the strongest
        best = {}
        for rec in ev[itc]:
            sr = rec[0]
            if sr not in best:
                best[sr] = rec
            elif itc < 6:
                if rec[2] > best[sr][2]:        # higher pitch wins (melody)
                    best[sr] = rec
            elif rec[4] > best[sr][4]:          # louder hit wins (drums)
                best[sr] = rec
        seq = [best[k] for k in sorted(best)]
        for i, (sr, er, note, ins, vol) in enumerate(seq):
            put(sr, itc, note, ins, vol)
            if itc < 6:                          # sustained voice: cut on a gap
                nxt = seq[i + 1][0] if i + 1 < len(seq) else None
                if nxt is None or nxt > er:
                    put(er, itc, NOTE_OFF, None, None)
    if not glob:
        return [(ROWS_PER_PAT, {})], [0]
    maxrow = max(glob) + 1
    # loop length = the MIDI's own length (the composer's loop boundary). It's
    # normally ~1 row under maxrow because a note-off lands exactly on the boundary
    # -- use it anyway (the loop restart retriggers, so dropping that boundary
    # note-off is seamless). Fall back to maxrow only if loop_rows is clearly bad.
    # Never pad to a full final pattern -- the leftover rows are the silent gap.
    total = loop_rows if (loop_rows and loop_rows >= maxrow - 2) else maxrow
    npat = (total + ROWS_PER_PAT - 1) // ROWS_PER_PAT
    patterns = []
    for pi in range(npat):
        base = pi * ROWS_PER_PAT
        rows = min(ROWS_PER_PAT, total - base)   # last pattern trimmed to fit
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
        s = samples[ins - 1]; pcm = s["pcm"].astype(np.float32); loop = s.get("loop")
        s0 = int(start * out_rate)
        if loop:
            n = int(dur * out_rate)
            if n <= 0:
                return
            freq = C5_HZ * 2.0 ** ((note - 60) / 12.0)
            idx = np.arange(n) * (freq * WAVE_LEN / out_rate)
            ls, le = loop
            wave = pcm[(ls + np.mod(idx, le - ls)).astype(np.int32)]
            env = np.ones(n); a = min(64, n // 4)
            if a > 0:
                env[:a] = np.linspace(0, 1, a); env[-a:] = np.linspace(1, 0, a)
            wave = wave * env
        else:
            wave = pcm.copy(); n = len(wave)
        g = vol / 64.0
        seg = wave * g / 32768.0
        end = min(s0 + n, len(buf))
        if end > s0:
            buf[s0:end] += seg[:end - s0]

    for itc in range(8):
        for (sr, er, note, ins, vol) in ev[itc]:
            dur = (er - sr) * row_sec if itc < 6 else 2.0
            play(note, ins, vol, sr * row_sec, dur)

    mixed = np.tanh(buf * 0.5) / np.tanh(0.5) * 0.9
    pcm = np.clip(mixed * 32767, -32768, 32767).astype("<i2")
    data = pcm.tobytes()
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt ")
        f.write(struct.pack("<IHHIIHH", 16, 1, 1, out_rate, out_rate * 2, 2, 16))
        f.write(b"data" + struct.pack("<I", len(data)) + data)


# ----------------------------------------------------------- section looping --
IT_FX_POSJUMP = 2     # IT effect 'B' = jump to order (1=A, 2=B, 3=C, ...)


def prepare(src):
    """MIDI -> (patterns, orders, ev, it_tempo, it_speed, midi_bpm, bpm, tpr)."""
    div, tempo, progs, notes, maxtick = parse_midi(src)
    midi_bpm = round(60_000_000 / tempo)
    bpm = TARGET_BPM if TARGET_BPM else midi_bpm
    # speed=3 (not 6): with R=8 rows/quarter, IT tempo == BPM, so songs up to
    # 255bpm play at their real tempo. speed=6 made tempo=2*bpm, clamping anything
    # over 127bpm (e.g. a 170bpm track) down to 127 -- audibly too slow.
    it_speed = 3
    it_tempo = max(32, min(255, bpm))
    tpr, ev = build_events(div, notes)
    loop_rows = int(round(maxtick / tpr)) if maxtick else None   # seamless loop length
    patterns, orders = to_patterns(ev, loop_rows)
    return patterns, orders, ev, it_tempo, it_speed, midi_bpm, bpm, tpr


def set_loop_jump(patterns, target_order):
    """Make a self-contained section loop: put a Bxx (jump-to-order) on the last
    row of the section's last pattern so playback wraps back to `target_order`
    instead of falling through into whatever patterns follow (or wrapping to the
    module's order 0 at the 0xFF end marker). The position jump is global, so we
    can hang it on any channel -- reuse an existing event on that row, else add a
    bare effect on channel 0. snesmod supports Bxx (only SBx/Cxx are limited)."""
    rows, chrows = patterns[-1]
    chrows = dict(chrows)
    last = rows - 1
    evs = list(chrows.get(last, []))
    if evs:
        ch, note, ins, vol, _c, _v = evs[0]
        evs[0] = (ch, note, ins, vol, IT_FX_POSJUMP, target_order)
    else:
        evs = [(0, None, None, None, IT_FX_POSJUMP, target_order)]
    chrows[last] = evs
    patterns[-1] = (rows, chrows)


# --------------------------------------------------------------- module build --
def build_standalone(src, name):
    """One MIDI -> res/music_<name>.it (the module loops as a whole, the old way)."""
    patterns, orders, ev, it_tempo, it_speed, midi_bpm, bpm, tpr = prepare(src)
    smps = instruments()
    it_path = os.path.join(RES, f"music_{name}.it")
    size = write_it(it_path, f"DEADFALL {name.upper()}", smps,
                    patterns=patterns, orders=orders,
                    channels=8, speed=it_speed, tempo=it_tempo)
    wav_path = os.path.join(RES, f"{name}_preview.wav")
    render_wav(wav_path, smps, ev, it_tempo, speed=it_speed)
    counts = {c: len(ev[c]) for c in range(8) if ev[c]}
    print(f"src={src}  midi {midi_bpm}bpm -> {bpm}bpm / IT tempo {it_tempo}/speed{it_speed}  ({R} rows/quarter, {tpr} ticks/row)")
    print(f"IT channels (events): {counts}")
    print(f"{len(patterns)} patterns x {ROWS_PER_PAT} rows, {len(orders)} orders")
    print(f"wrote {it_path} ({size} bytes)")
    print(f"wrote {wav_path}  <-- listen to verify it's the song")
    return size


def build_combined(frantic_src, calm_src, name):
    """Two MIDIs -> one res/music_<name>.it holding both themes as independently
    looping sections, so the in-game swap (calm <-> frantic) is a cheap spcPlay
    position jump with NO blocking module reload. Layout: the FRANTIC section is
    first (orders 0..P-1), then the CALM theme (orders P..). Because the frantic
    MIDI is the same for every level, P (= the calm start order) is constant.
    Returns (size, calm_start). spcPlay(0)=frantic, spcPlay(calm_start)=calm."""
    fpat, _fo, _fev, ftempo, fspeed, *_ = prepare(frantic_src)
    cpat, _co, cev, ctempo, cspeed, *_ = prepare(calm_src)
    calm_start = len(fpat)                       # first calm order
    set_loop_jump(fpat, 0)                        # frantic loops within 0..P-1
    set_loop_jump(cpat, calm_start)               # calm loops within P..
    patterns = fpat + cpat
    orders = list(range(len(patterns)))
    smps = instruments()
    it_path = os.path.join(RES, f"music_{name}.it")
    # Both MIDIs are forced to TARGET_BPM, so one global tempo/speed fits both.
    size = write_it(it_path, f"DEADFALL {name.upper()}", smps,
                    patterns=patterns, orders=orders,
                    channels=8, speed=cspeed, tempo=ctempo)
    # Audition preview = the calm level theme (the frantic theme has its own
    # preview from the standalone portal build).
    wav_path = os.path.join(RES, f"{name}_preview.wav")
    render_wav(wav_path, smps, cev, ctempo, speed=cspeed)
    print(f"[combine] {name}: frantic {len(fpat)}pat + calm {len(cpat)}pat "
          f"-> calm_start={calm_start}, {len(patterns)} orders, {size}B -> {it_path}")
    return size, calm_start


# --------------------------------------------------------------------- main --
def main():
    if len(sys.argv) > 1 and sys.argv[1] == "--combine":
        # mid2it.py --combine FRANTIC.mid CALM.mid OUTNAME
        build_combined(sys.argv[2], sys.argv[3], sys.argv[4])
        return
    src = sys.argv[1] if len(sys.argv) > 1 else "test.mid"
    name = sys.argv[2] if len(sys.argv) > 2 else os.path.splitext(os.path.basename(src))[0]
    build_standalone(src, name)


if __name__ == "__main__":
    main()
