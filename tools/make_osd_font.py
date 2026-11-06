#!/usr/bin/env python3
"""Draws the font of the performance overlay (rt/osd.c, drawn by rt/hostd3d9.cpp).

DejaVu Sans Mono Bold, characters 32-126 in a grid of 16 per row, then two of our own: 127 a solid block (panels and
graph bars use a texel from its middle) and 128 a degree sign. Written as 8-bit coverage:

  u8 cell width, u8 cell height, u8 first character, u8 count, u16 texture width, u16 texture height (little endian),
  then texture width x height bytes, row by row.

Usage: make_osd_font.py ASSETS_DIR   (writes ASSETS_DIR/internal/osd_font.a8)
Needs Pillow and DejaVu Sans Mono Bold (fonts-dejavu-core)."""
import os, struct, sys
from PIL import Image, ImageDraw, ImageFont

FONT = '/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf'
CW, CH = 32, 56           # cell size: drawn at about this size on a 4K picture, scaled down below that
SIZE = 46                 # font size in pixels
FIRST, COUNT = 32, 97     # 32..126, then 127 (block) and 128 (degree sign)
COLS = 16


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else 'assets'
    rows = (COUNT + COLS - 1) // COLS
    tw, th = 512, 512
    assert COLS * CW <= tw and rows * CH <= th
    SS = 4
    big = Image.new('L', (tw * SS, th * SS), 0)
    d = ImageDraw.Draw(big)
    f = ImageFont.truetype(FONT, SIZE * SS)
    asc, desc = f.getmetrics()
    for i in range(COUNT):
        ch = FIRST + i
        x0, y0 = (i % COLS) * CW * SS, (i // COLS) * CH * SS
        if ch == 127:
            d.rectangle([x0, y0, x0 + CW * SS - 1, y0 + CH * SS - 1], fill=255)
            continue
        t = '°' if ch == 128 else chr(ch)
        w = f.getlength(t)
        # baseline so the cell holds ascenders and descenders
        d.text((x0 + (CW * SS - w) / 2, y0 + (CH * SS - (asc + desc)) / 2), t, font=f, fill=255)
    img = big.resize((tw, th), Image.BOX)
    out = os.path.join(root, 'internal', 'osd_font.a8')
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, 'wb') as fo:
        fo.write(struct.pack('<BBBBHH', CW, CH, FIRST, COUNT, tw, th) + img.tobytes())
    print('wrote', out)


if __name__ == '__main__':
    main()
