#!/usr/bin/env python3
"""Draws the PlayStation-style button prompts that replace the game's Xbox 360 ones.

Original drawings (geometric shapes and DejaVu Sans Bold lettering). The game draws its prompts from 64x64 cells of
its UI atlas (textures\\interface\\interfaceshared0.dds, 1024x1024 DXT5); these are written as DXT5 blocks for those
cells and their mipmaps, built into the executable (tools/embed_assets.py), and rt/glyphs.c writes them into the
atlas when the game uploads it. White on transparent: the menus tint them.

The Pip-Boy arm's printed trigger tags (LT/RT on the arm model's texture) are relabelled L2/R2: arm_label writes the
lettering as 8-bit coverage masks (a byte each for width and height, then the rows) that rt/glyphs.c paints over the
erased tags.

Usage: make_glyphs.py ASSETS_DIR   (writes ASSETS_DIR/internal/atlas_glyphs/*.dxt5, ASSETS_DIR/internal/arm_labels/*.a8)
Needs Pillow and DejaVu Sans Bold (fonts-dejavu-core)."""
import os, struct, sys
from PIL import Image, ImageDraw, ImageFilter, ImageFont

SS = 4                      # supersampling
FONT = '/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf'
W = 6                       # stroke width at 64 px


def canvas(n=64):
    return Image.new('L', (n * SS, n * SS), 0)


def s(v):
    return int(round(v * SS))


def ring(d, cx, cy, r, w=W):
    d.ellipse([s(cx - r), s(cy - r), s(cx + r), s(cy + r)], outline=255, width=s(w))


def text(d, cx, cy, t, size):
    f = ImageFont.truetype(FONT, s(size))
    d.text((s(cx), s(cy)), t, font=f, fill=255, anchor='mm')


def face(symbol):
    """a face button: the symbol inside a thin ring"""
    im = canvas(); d = ImageDraw.Draw(im)
    ring(d, 32, 32, 21, 3.5)
    if symbol == 'cross':
        k = 9.5
        d.line([s(32 - k), s(32 - k), s(32 + k), s(32 + k)], fill=255, width=s(5))
        d.line([s(32 - k), s(32 + k), s(32 + k), s(32 - k)], fill=255, width=s(5))
    elif symbol == 'circle':
        ring(d, 32, 32, 10, 4.5)
    elif symbol == 'square':
        k = 9
        d.rectangle([s(32 - k), s(32 - k), s(32 + k), s(32 + k)], outline=255, width=s(4.5))
    elif symbol == 'triangle':
        d.polygon([(s(32), s(18)), (s(44.5), s(40)), (s(19.5), s(40))], outline=255, width=s(4.5))
    return im


def shoulder(label):
    """L1/R1: a bumper outline with its name"""
    im = canvas(); d = ImageDraw.Draw(im)
    d.rounded_rectangle([s(12), s(20), s(52), s(44)], radius=s(8), outline=255, width=s(4))
    text(d, 32, 32.5, label, 15)
    return im


def trigger(label):
    """L2/R2: a trigger outline (taller, rounded top) with its name"""
    im = canvas(); d = ImageDraw.Draw(im)
    d.rounded_rectangle([s(17), s(13), s(47), s(51)], radius=s(12), outline=255, width=s(4))
    text(d, 32, 34, label, 14)
    return im


def stick(label):
    """L3/R3: a stick (ring) with its name"""
    im = canvas(); d = ImageDraw.Draw(im)
    ring(d, 32, 32, 19, 3.5)
    text(d, 32, 33, label, 13)
    return im


def options():
    """Options: three lines in a small pill"""
    im = canvas(); d = ImageDraw.Draw(im)
    d.rounded_rectangle([s(17), s(15), s(47), s(49)], radius=s(12), outline=255, width=s(4))
    for y in (25, 32, 39):
        d.line([s(25), s(y), s(39), s(y)], fill=255, width=s(3.5))
    return im


def create():
    """Create: a small pill with three short rays"""
    im = canvas(); d = ImageDraw.Draw(im)
    d.rounded_rectangle([s(17), s(15), s(47), s(49)], radius=s(12), outline=255, width=s(4))
    for x0, y0, x1, y1 in ((32, 40, 32, 26), (26, 38, 24, 29), (38, 38, 40, 29)):
        d.line([s(x0), s(y0), s(x1), s(y1)], fill=255, width=s(3.5))
    return im


def up():
    """D-pad up: an arrow"""
    im = canvas(); d = ImageDraw.Draw(im)
    d.polygon([(s(32), s(14)), (s(46), s(31)), (s(37), s(31)), (s(37), s(49)), (s(27), s(49)), (s(27), s(31)),
               (s(18), s(31))], fill=255)
    return im


GLYPHS = {
    'a': lambda: face('cross'), 'b': lambda: face('circle'), 'x': lambda: face('square'), 'y': lambda: face('triangle'),
    'lb': lambda: shoulder('L1'), 'rb': lambda: shoulder('R1'), 'lt': lambda: trigger('L2'), 'rt': lambda: trigger('R2'),
    'ls': lambda: stick('L3'), 'rs': lambda: stick('R3'), 'start': options, 'back': create, 'up': up,
}


def finish(alpha, size, glow):
    """downsample the supersampled alpha; white glyph, optionally over a soft glow (the selected look)"""
    a = alpha.resize((size, size), Image.BOX)
    # hard edges for the plain glyphs (they show at about 20 pixels and must compress small); the glow keeps soft ones
    a = a.point(lambda v: 255 if v >= 110 else 0) if not glow else a.point(lambda v: 0 if v < 40 else 255 if v > 215 else v)
    out = Image.new('RGBA', (size, size), (255, 255, 255, 0))
    if glow:
        g = a.filter(ImageFilter.GaussianBlur(size / 64 * 2.4)).point(lambda v: min(255, int(v * 0.85)))
        halo = Image.new('RGBA', (size, size), (255, 255, 255, 0))
        halo.putalpha(g)
        out = Image.alpha_composite(out, halo)
    body = Image.new('RGBA', (size, size), (255, 255, 255, 0))
    body.putalpha(a)
    return Image.alpha_composite(out, body)


def _alpha_dxt5(block):
    a0, a1 = max(block), min(block)
    if a0 == a1:
        return bytes([a0, a1]) + bytes(6)
    a0, a1 = 255, 0  # fixed end points for edge blocks: the glyphs are nearly two-level, and this keeps edges crisp
    pal = [a0, a1] + [((7 - i) * a0 + i * a1) // 7 for i in range(1, 7)]
    bits = 0
    for k, v in enumerate(block):
        idx = min(range(8), key=lambda j: abs(pal[j] - v))
        bits |= idx << (3 * k)
    return bytes([a0, a1]) + bits.to_bytes(6, 'little')


WHITE = struct.pack('<HHI', 0xFFFF, 0x0000, 0)  # every texel colour 0: white (the menus tint it)


def dxt5_blocks(im):
    """raw DXT5 blocks of an RGBA image (white, alpha from the image), row by row"""
    w, h = im.size
    a = im.getchannel('A').load()
    data = bytearray()
    for by in range(0, h, 4):
        for bx in range(0, w, 4):
            data += _alpha_dxt5([a[bx + x, by + y] for y in range(4) for x in range(4)]) + WHITE
    return bytes(data)


def write_atlas_cell(path, im):
    """the game draws its prompts from cells of a 1024x1024 DXT5 atlas (InterfaceShared0.dds): our glyph as DXT5
    blocks for the 64x64 cell and its 32, 16, 8 and 4 mipmap levels, one after another (rt/glyphs.c)"""
    data = bytearray()
    for size in (64, 32, 16, 8, 4):
        level = im if size == 64 else im.resize((size, size), Image.BOX)
        data += dxt5_blocks(level)
    with open(path, 'wb') as f:
        f.write(bytes(data))


def arm_label(path, label, w, h):
    """the lettering for one arm tag, squeezed to the box the old letters took (bold, like the arm's own stencil)"""
    f = ImageFont.truetype(FONT, 100)
    c = Image.new('L', (400, 200), 0)
    ImageDraw.Draw(c).text((10, 10), label, font=f, fill=255)
    c = c.crop(c.getbbox()).resize((w, h), Image.BOX)
    with open(path, 'wb') as out:
        out.write(bytes([w, h]) + c.tobytes())


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else 'assets'
    cells = os.path.join(root, 'internal', 'atlas_glyphs')
    os.makedirs(cells, exist_ok=True)
    alpha = {k: fn() for k, fn in GLYPHS.items()}
    n = 0
    for k in ['a', 'b', 'x', 'y', 'lb', 'rb', 'lt', 'rt', 'ls', 'rs', 'up']:
        write_atlas_cell(os.path.join(cells, 'general_button_%s.dxt5' % k), finish(alpha[k], 64, False)); n += 1
    for k in ['a', 'b', 'x', 'y', 'lb', 'rb', 'lt', 'rt', 'ls', 'rs', 'up', 'start', 'back']:
        write_atlas_cell(os.path.join(cells, 'glow_general_button_%s.dxt5' % k), finish(alpha[k], 64, True)); n += 1
    print('wrote', n, 'atlas cells to', cells)
    labels = os.path.join(root, 'internal', 'arm_labels')
    os.makedirs(labels, exist_ok=True)
    arm_label(os.path.join(labels, 'l2.a8'), 'L2', 20, 13)  # box sizes: rt/glyphs.c g_tags
    arm_label(os.path.join(labels, 'r2.a8'), 'R2', 25, 13)


if __name__ == '__main__':
    main()
