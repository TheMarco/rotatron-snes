#!/usr/bin/env python3
"""Mode-3 (8bpp, high color) assets for the boot scenes.

  res/title8a.pic + title8b.pic  - backdrops/title.png, quantized to 253
      colors, with the credit + PRESS START texts baked in using reserved
      palette indices (254 = static white, 255 = CGRAM-blinked at runtime).
      The pic is split into two files so each ROM section fits a 32KB bank.
  res/title8.map / title8.pal    - 32x32 map + 256-color palette
  res/logo8.pic / .map / .pal    - the studio logo (original RGBA source from
      ../cubed), centered on black with its top edge at y=92 (LOGO_TARGET).

8bpp tiles dedupe across H/V flips (palette is global, so flips are safe).
"""

from pathlib import Path
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
LOGO_SRC = Path("/Users/marcovhv/projects/GIT/cubed/public/sprites/gamestudios.png")
W, H = 256, 224
LOGO_TARGET = 92

CREDIT = "BY MARCO VAN HYLCKAMA VLIEG"
PRESS = "PRESS START"
CREDIT_ROW, PRESS_ROW = 22, 25
IDX_TEXT, IDX_BLINK = 254, 255


def font_glyphs():
    """Decode res/hudfont.pic (2bpp, ASCII 32..95) -> {char: 8x8 bitmask}."""
    data = (ROOT / "res/hudfont.pic").read_bytes()
    out = {}
    for g in range(64):
        rows = []
        for y in range(8):
            rows.append(data[g * 16 + y * 2])  # plane 0 = white pixels
        out[chr(32 + g)] = rows
    return out


def bake_text(idx_img, glyphs, text, row, color_index):
    x0 = (32 - len(text)) // 2 * 8
    for i, ch in enumerate(text):
        rows = glyphs.get(ch)
        if rows is None:
            continue
        for y in range(8):
            for x in range(8):
                if rows[y] & (0x80 >> x):
                    idx_img[(row * 8 + y) * W + x0 + i * 8 + x] = color_index
    return idx_img


def encode_tile_8bpp(px):
    out = bytearray(64)
    for block in range(4):
        for y in range(8):
            b0 = b1 = 0
            for x in range(8):
                v = px[y][x] >> (block * 2)
                bit = 0x80 >> x
                if v & 1:
                    b0 |= bit
                if v & 2:
                    b1 |= bit
            out[block * 16 + y * 2] = b0
            out[block * 16 + y * 2 + 1] = b1
    return bytes(out)


def convert8(im, name, texts=False):
    q = im.quantize(colors=253, dither=Image.Dither.NONE)
    qpal = q.getpalette()[: 253 * 3]
    qpal += [0] * (253 * 3 - len(qpal))  # PIL trims trailing unused entries
    qpx = q.load()
    idx = [0] * (W * H)
    for y in range(H):
        for x in range(W):
            idx[y * W + x] = qpx[x, y] + 1  # 0 stays transparent/black

    if texts:
        glyphs = font_glyphs()
        bake_text(idx, glyphs, CREDIT, CREDIT_ROW, IDX_TEXT)
        bake_text(idx, glyphs, PRESS, PRESS_ROW, IDX_BLINK)

    def hflip(t):
        return tuple(tuple(row[::-1]) for row in t)

    def vflip(t):
        return tuple(t[7 - y] for y in range(8))

    tiles = [bytes(64)]
    canonical = {tuple(tuple([0] * 8) for _ in range(8)): 0}
    entries = []
    for ty in range(H // 8):
        for tx in range(W // 8):
            t = tuple(tuple(idx[(ty * 8 + y) * W + tx * 8 + x] for x in range(8))
                      for y in range(8))
            ent = None
            for cand, flips in ((t, 0), (hflip(t), 0x4000), (vflip(t), 0x8000),
                                (vflip(hflip(t)), 0xC000)):
                if cand in canonical:
                    ent = canonical[cand] | flips
                    break
            if ent is None:
                tid = len(tiles)
                tiles.append(encode_tile_8bpp([list(r) for r in t]))
                canonical[t] = tid
                ent = tid
            entries.append(ent)
    assert len(tiles) <= 896, f"{len(tiles)} mode-3 tiles ({name})"

    pic = b"".join(tiles)
    if name == "title8":  # split so each ROM section fits a 32KB bank
        (ROOT / "res/title8a.pic").write_bytes(pic[:0x7000])
        (ROOT / "res/title8b.pic").write_bytes(pic[0x7000:] if len(pic) > 0x7000 else bytes(64))
    else:
        (ROOT / f"res/{name}.pic").write_bytes(pic)

    mp = bytearray()
    for ty in range(32):
        for tx in range(32):
            e = entries[ty * 32 + tx] if ty < H // 8 else 0
            mp += bytes((e & 0xFF, e >> 8))
    (ROOT / f"res/{name}.map").write_bytes(mp)

    pal = bytearray(2)  # index 0: transparent (backdrop = black)
    for i in range(253):
        r, g, b = qpal[i * 3], qpal[i * 3 + 1], qpal[i * 3 + 2]
        w = (b >> 3) << 10 | (g >> 3) << 5 | (r >> 3)
        pal += bytes((w & 0xFF, w >> 8))
    pal += bytes((0xFF, 0x7F))  # 254: static text white
    pal += bytes((0xFF, 0x7F))  # 255: PRESS START (blinked at runtime)
    (ROOT / f"res/{name}.pal").write_bytes(pal)
    print(f"{name}: {len(tiles)} tiles ({len(pic)} bytes)")


def main():
    title = Image.open(ROOT / "backdrops/title.png").convert("RGB")
    if title.size != (W, H):
        title = title.resize((W, H), Image.LANCZOS)
    convert8(title, "title8", texts=True)

    logo = Image.open(LOGO_SRC).convert("RGBA")
    canvas = Image.new("RGBA", (W, H), (0, 0, 0, 255))
    canvas.paste(logo, ((W - logo.width) // 2, LOGO_TARGET), logo)
    convert8(canvas.convert("RGB"), "logo8")


if __name__ == "__main__":
    main()
