#!/usr/bin/env python3
"""Draws the PlayStation-style button prompts that replace the game's Xbox 360 ones.

Original drawings (geometric shapes and DejaVu Sans Bold lettering), written as uncompressed 32-bit DDS files with
the same names, sizes and atlas layout as the game's textures\\interface\\shared\\button\\*.dds. The runtime serves
them from the app's assets folder as loose files over the game's archives (rt/overlay.c). White on transparent:
the menus tint them.

Usage: make_glyphs.py OUT_DIR   (writes OUT_DIR/Data/Textures/Interface/Shared/Button/*.dds)
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
    ring(d, 32, 32, 20, 4)
    text(d, 32, 33, label, 14)
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
    a = alpha.resize((size, size), Image.LANCZOS)
    out = Image.new('RGBA', (size, size), (255, 255, 255, 0))
    if glow:
        g = a.filter(ImageFilter.GaussianBlur(size / 64 * 3.2)).point(lambda v: min(255, int(v * 1.6)))
        halo = Image.new('RGBA', (size, size), (255, 255, 255, 0))
        halo.putalpha(g)
        out = Image.alpha_composite(out, halo)
    body = Image.new('RGBA', (size, size), (255, 255, 255, 0))
    body.putalpha(a)
    return Image.alpha_composite(out, body)


def write_dds(path, im):
    w, h = im.size
    hdr = struct.pack('<4sIIIIIII44x', b'DDS ', 124, 0x100F, h, w, w * 4, 0, 0)
    pf = struct.pack('<II4sIIIII', 32, 0x41, b'\0\0\0\0', 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000)
    caps = struct.pack('<IIIII', 0x1000, 0, 0, 0, 0)
    r, g, b, a = im.split()
    data = Image.merge('RGBA', (b, g, r, a)).tobytes()  # BGRA in memory = A8R8G8B8
    with open(path, 'wb') as f:
        f.write(hdr + pf + caps + data)


def main():
    out = os.path.join(sys.argv[1] if len(sys.argv) > 1 else 'assets', 'Data', 'Textures', 'Interface', 'Shared', 'Button')
    os.makedirs(out, exist_ok=True)
    alpha = {k: fn() for k, fn in GLYPHS.items()}
    plain = ['a', 'b', 'x', 'y', 'lb', 'rb', 'lt', 'rt', 'ls', 'rs', 'up']
    glow = plain + ['start', 'back']
    for k in plain:
        write_dds(os.path.join(out, 'general_button_%s.dds' % k), finish(alpha[k], 64, False))
    for k in glow:
        write_dds(os.path.join(out, 'glow_general_button_%s.dds' % k), finish(alpha[k], 64, True))
    # the 256x512 atlas: 4 x 8 cells of 64; glyphs drawn larger (the game's fill their cells)
    order = [('ls', 0), ('lb', 0), ('a', 0), ('b', 0), ('lt', 0), ('rb', 0), ('rs', 0), ('rt', 0),
             ('x', 0), ('y', 0), ('a', 1), ('b', 1), ('lb', 1), ('ls', 1), ('lt', 1), ('rb', 1),
             ('rs', 1), ('rt', 1), ('x', 1), ('y', 1)]
    atlas = Image.new('RGBA', (256, 512), (255, 255, 255, 0))
    for i, (k, gl) in enumerate(order):
        big = alpha[k].resize((64 * SS, 64 * SS)).crop((s(6), s(6), s(58), s(58))).resize((64 * SS, 64 * SS))
        atlas.paste(finish(big, 64, gl), ((i % 4) * 64, (i // 4) * 64))
    write_dds(os.path.join(out, 'buttons0.dds'), atlas)
    print('wrote', len(plain) + len(glow) + 1, 'textures to', out)


if __name__ == '__main__':
    main()
