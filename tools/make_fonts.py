"""Render anti-aliased VLW fonts (Processing / TFT_eSPI / LovyanGFX smooth-font
format) from DejaVu Sans for the board UI.

  python tools/make_fonts.py   (any env with Pillow + matplotlib for the TTFs)

VLW: big-endian int32 header [glyphs, version, size, 0, ascent, descent], then
per glyph [unicode, height, width, xAdvance, dY (top above baseline), dX, 0],
then each glyph's 8-bit alpha bitmap, width*height bytes, in the same order.
"""
import struct
from pathlib import Path

import matplotlib
from PIL import Image, ImageDraw, ImageFont

TTF = Path(matplotlib.get_data_path()) / "fonts" / "ttf"
OUT = Path(__file__).resolve().parents[1] / "data" / "fonts"
FONTS = {  # name: (ttf, pixel size)
    "sans16": ("DejaVuSans.ttf", 16),
    "bold16": ("DejaVuSans-Bold.ttf", 16),
    "sans21": ("DejaVuSans.ttf", 21),
    "bold21": ("DejaVuSans-Bold.ttf", 21),
}


def vlw(ttf, size):
    font = ImageFont.truetype(str(TTF / ttf), size)
    ascent, descent = font.getmetrics()
    recs, bitmaps = [], []
    for code in range(32, 127):
        ch = chr(code)
        x0, y0, x1, y1 = font.getbbox(ch)  # origin: left of the ascender line
        w, h = max(0, x1 - x0), max(0, y1 - y0)
        adv = round(font.getlength(ch))
        if w and h:
            img = Image.new("L", (w, h), 0)
            ImageDraw.Draw(img).text((-x0, -y0), ch, font=font, fill=255)
            bitmaps.append(img.tobytes())
        else:
            w = h = 0
            bitmaps.append(b"")
        recs.append((code, h, w, adv, ascent - y0 if h else 0, x0 if w else 0, 0))
    out = struct.pack(">6i", len(recs), 11, ascent + descent, 0, ascent, descent)
    out += b"".join(struct.pack(">7i", *r) for r in recs)
    return out + b"".join(bitmaps)


OUT.mkdir(parents=True, exist_ok=True)
for name, (ttf, size) in FONTS.items():
    data = vlw(ttf, size)
    (OUT / f"{name}.vlw").write_bytes(data)
    print(f"{name}.vlw: {len(data)} bytes")
