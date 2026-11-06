#!/usr/bin/env python3
"""Draws the PlayStation-style button prompts that replace the game's Xbox 360 ones.

Original drawings (geometric shapes and DejaVu Sans Bold lettering) in the style of the console's monochrome
prompts: solid buttons with the symbol or name cut out, soft-edged. The game draws its prompts from 64x64 cells of
its UI atlas (textures\\interface\\interfaceshared0.dds, 1024x1024 DXT5); these are written as DXT5 blocks for those
cells and their mipmaps, built into the executable (tools/embed_assets.py), and rt/glyphs.c writes them into the
atlas when the game uploads it. White on transparent: the menus tint them.

The Pip-Boy arm's printed trigger tags (LT/RT on the arm model's texture) are relabelled L2/R2: arm_label writes the
lettering as 8-bit coverage masks (a byte each for width and height, then the rows) that rt/glyphs.c paints over the
erased tags.

Usage: make_glyphs.py ASSETS_DIR
  (writes ASSETS_DIR/internal/atlas_glyphs/<size>/*.dxt5, ASSETS_DIR/internal/arm_labels/*.a8)
Needs Pillow and DejaVu Sans Bold (fonts-dejavu-core)."""
import os, struct, sys
from PIL import Image, ImageDraw, ImageFilter, ImageFont

SS = 4                      # supersampling
FONT = '/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf'
W = 6                       # stroke width at 64 px
SIZES = (50, 60, 70, 80, 100)  # each glyph set is drawn at these sizes (percent, about the cell's centre); the player
                               # picks one with iButtonPromptSize in FalloutPrefs.ini [fnv_recomp] (rt/glyphs.c)
LABELLED = {'lb', 'rb', 'lt', 'rt', 'ls', 'rs'}  # drawn 10% larger than the rest so their lettering stays readable


def canvas(n=64):
    return Image.new('L', (n * SS, n * SS), 0)


def s(v):
    return int(round(v * SS))


def ring(d, cx, cy, r, w=W, fill=255):
    d.ellipse([s(cx - r), s(cy - r), s(cx + r), s(cy + r)], outline=fill, width=s(w))


def disc(d, cx, cy, r, fill=255):
    d.ellipse([s(cx - r), s(cy - r), s(cx + r), s(cy + r)], fill=fill)


def text(d, cx, cy, t, size, fill=255):
    f = ImageFont.truetype(FONT, s(size))
    d.text((s(cx), s(cy)), t, font=f, fill=fill, anchor='mm')


# The look of the console's own monochrome prompts: each button a solid shape with its symbol or name cut out of
# it (the menus tint the shape), soft-edged so it stays smooth when the game draws it large on a 4K picture.
CX, CY = 32, 31.5


def face(symbol):
    """a face button: a solid disc with the symbol cut out"""
    im = canvas(); d = ImageDraw.Draw(im)
    disc(d, CX, CY, 19.5)
    if symbol == 'cross':
        k = 7.6
        d.line([s(CX - k), s(CY - k), s(CX + k), s(CY + k)], fill=0, width=s(4.6))
        d.line([s(CX - k), s(CY + k), s(CX + k), s(CY - k)], fill=0, width=s(4.6))
    elif symbol == 'circle':
        ring(d, CX, CY, 8.4, 4.4, fill=0)
    elif symbol == 'square':
        k = 7.6
        d.rectangle([s(CX - k), s(CY - k), s(CX + k), s(CY + k)], outline=0, width=s(4.1))
    elif symbol == 'triangle':
        d.polygon([(s(CX), s(CY - 10.5)), (s(CX + 10.2), s(CY + 7)), (s(CX - 10.2), s(CY + 7))], outline=0, width=s(4.1))
    return im


def shoulder(label):
    """L1/R1: a solid bumper (wide, low, rounded) with its name cut out"""
    im = canvas(); d = ImageDraw.Draw(im)
    d.rounded_rectangle([s(11), s(19.5), s(53), s(44.5)], radius=s(9), fill=255)
    text(d, CX, CY + 0.5, label, 17.5, fill=0)
    return im


def trigger(label):
    """L2/R2: a solid trigger (rounded top, squarer bottom) with its name cut out"""
    im = canvas(); d = ImageDraw.Draw(im)
    d.rounded_rectangle([s(15), s(12), s(49), s(51)], radius=s(14.5), fill=255, corners=(True, True, False, False))
    d.rounded_rectangle([s(15), s(30), s(49), s(51)], radius=s(5.5), fill=255)
    text(d, CX, CY + 3, label, 16.5, fill=0)
    return im


def stick(label):
    """L3/R3: a solid stick top with its name cut out"""
    im = canvas(); d = ImageDraw.Draw(im)
    disc(d, CX, CY, 19.5)
    text(d, CX, CY + 0.5, label, 14.5, fill=0)
    return im


def options():
    """Options: a solid upright pill with three lines cut out"""
    im = canvas(); d = ImageDraw.Draw(im)
    d.rounded_rectangle([s(19), s(11), s(45), s(52)], radius=s(13), fill=255)
    for y in (24, 31.5, 39):
        d.line([s(25.5), s(y), s(38.5), s(y)], fill=0, width=s(3.6))
    return im


def create():
    """Create: a solid upright pill with three short rays cut out"""
    im = canvas(); d = ImageDraw.Draw(im)
    d.rounded_rectangle([s(19), s(11), s(45), s(52)], radius=s(13), fill=255)
    for x0, y0, x1, y1 in ((32, 41, 32, 25), (26, 39, 24.5, 29), (38, 39, 39.5, 29)):
        d.line([s(x0), s(y0), s(x1), s(y1)], fill=0, width=s(3.4))
    return im


def up():
    """D-pad up: an arrow"""
    im = canvas(); d = ImageDraw.Draw(im)
    d.polygon([(s(32), s(12)), (s(47), s(30)), (s(37.5), s(30)), (s(37.5), s(51)), (s(26.5), s(51)), (s(26.5), s(30)),
               (s(17), s(30))], fill=255)
    return im


GLYPHS = {
    'a': lambda: face('cross'), 'b': lambda: face('circle'), 'x': lambda: face('square'), 'y': lambda: face('triangle'),
    'lb': lambda: shoulder('L1'), 'rb': lambda: shoulder('R1'), 'lt': lambda: trigger('L2'), 'rt': lambda: trigger('R2'),
    'ls': lambda: stick('L3'), 'rs': lambda: stick('R3'), 'start': options, 'back': create, 'up': up,
}


def silhouette(alpha):
    """the glyph's outline filled in (its cut-outs closed): flood the outside from a corner"""
    b = alpha.point(lambda v: 255 if v > 127 else 0)
    ImageDraw.floodfill(b, (0, 0), 128)
    return b.point(lambda v: 0 if v == 128 else 255)


def finish(alpha, size, glow, scale):
    """downsample the supersampled alpha, drawn at scale about the centre; white glyph, optionally over a soft glow
    (the selected look) that stays outside the button, so its cut-out symbol or name stays clear"""
    n = alpha.size[0]
    m = int(round(n * scale))

    def small(img):
        out = Image.new('L', (n, n), 0)
        out.paste(img.resize((m, m), Image.BOX), ((n - m) // 2, (n - m) // 2))
        return out.resize((size, size), Image.BOX)

    a = small(alpha)
    # soft (antialiased) edges: the game draws the 64-pixel cells larger on a 4K picture, where hard edges show steps
    a = a.point(lambda v: 0 if v < 12 else 255 if v > 243 else v)
    out = Image.new('RGBA', (size, size), (255, 255, 255, 0))
    if glow:
        sil = small(silhouette(alpha))
        g = sil.filter(ImageFilter.GaussianBlur(size / 64 * 2.6)).point(lambda v: min(255, int(v * 0.8)))
        g = Image.composite(Image.new('L', g.size, 0), g, sil.point(lambda v: 255 if v > 64 else 0))  # not inside
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
    alpha = {k: fn() for k, fn in GLYPHS.items()}
    n = 0
    for pct in SIZES:
        cells = os.path.join(root, 'internal', 'atlas_glyphs', str(pct))
        os.makedirs(cells, exist_ok=True)
        for glow, keys in ((False, ['a', 'b', 'x', 'y', 'lb', 'rb', 'lt', 'rt', 'ls', 'rs', 'up']),
                           (True, ['a', 'b', 'x', 'y', 'lb', 'rb', 'lt', 'rt', 'ls', 'rs', 'up', 'start', 'back'])):
            for k in keys:
                scale = min(1.0, pct / 100 * (1.10 if k in LABELLED else 1.0))
                name = ('glow_' if glow else '') + 'general_button_' + k
                write_atlas_cell(os.path.join(cells, name + '.dxt5'), finish(alpha[k], 64, glow, scale)); n += 1
    print('wrote', n, 'atlas cells in', len(SIZES), 'sizes')
    labels = os.path.join(root, 'internal', 'arm_labels')
    os.makedirs(labels, exist_ok=True)
    arm_label(os.path.join(labels, 'l2.a8'), 'L2', 20, 13)  # box sizes: rt/glyphs.c g_tags
    arm_label(os.path.join(labels, 'r2.a8'), 'R2', 25, 13)


if __name__ == '__main__':
    main()
