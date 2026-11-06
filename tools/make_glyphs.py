#!/usr/bin/env python3
"""Draws the PlayStation button symbols that replace the letters of the game's Xbox 360 prompts.

The PS3 version drew its prompts in the same art as the Xbox ones the PC files carry (a thin symbol, and for the face
buttons the arc of a raised button beside it: "✕)" where the Xbox has "A)"), only with the PlayStation symbols. So
the port keeps the game's own icons and replaces only their letters, when the game uploads its UI atlas
(textures\\interface\\interfaceshared0.dds, 1024x1024 DXT5, cells laid out by interfaceshared.tai): rt/glyphs.c
splits each 64x64 cell into its pieces, keeps the main shape (the arc, the bumper or trigger outline, the stick, the
circle), erases the letters and the glow around them, and lays in what this tool draws: ✕ ○ □ △ for the face
buttons, L1/R1, L2/R2, L3/R3 for the shoulders, triggers and sticks, and the DualSense's Options and Create marks for
Start and Back. Nothing of the game's art is stored here: only the letter boxes (where the letters sit in each cell)
and our own drawings.

The face buttons keep the game's arc and get the PlayStation symbol in place of the letter. The shoulders, triggers,
sticks and Start/Back are drawn whole, the way PS3-style icon sets for these games draw them (as the PS3 version
did): thin outlines at the game's line weight - a flat rectangle for L1/R1, a taller rounded box for L2/R2, a stick
with its letter in the cap for L3/R3, the button's name in a rectangle for Options and Create.

Each cell file (internal/ps3_cells/<name>.cell): u8 flags (1: a highlighted, glowing cell; 2: replace the whole cell,
not just its letters), then the body coverage (64x64 bytes) and, for highlighted cells, the glow coverage.

The Pip-Boy arm's printed trigger tags (LT/RT on the arm model's texture) are relabelled L2/R2: arm_label writes the
lettering as 8-bit coverage masks (a byte each for width and height, then the rows) that rt/glyphs.c paints over the
erased tags.

Usage: make_glyphs.py ASSETS_DIR   (writes ASSETS_DIR/internal/ps3_cells/*.cell, ASSETS_DIR/internal/arm_labels/*.a8)
Needs Pillow and DejaVu Sans Bold (fonts-dejavu-core)."""
import os, sys
from PIL import Image, ImageDraw, ImageFilter, ImageFont

FONT = '/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf'
SS = 8      # supersampling
SW = 3.1    # stroke width: the game's own lettering

# where the letters sit in each cell of the vanilla atlas (x0, y0, x1, y1, inclusive), as rt/glyphs.c finds them
BOXES = {
    'general_button_a': (21, 23, 31, 40), 'general_button_b': (23, 22, 30, 39), 'general_button_x': (22, 22, 30, 39),
    'general_button_y': (21, 23, 30, 40), 'general_button_lb': (25, 26, 41, 37), 'general_button_rb': (21, 26, 39, 37),
    'general_button_lt': (28, 25, 40, 38), 'general_button_rt': (23, 25, 37, 38), 'general_button_ls': (21, 16, 42, 28),
    'general_button_rs': (20, 16, 43, 28),
    'glow_general_button_a': (20, 23, 30, 40), 'glow_general_button_b': (21, 23, 30, 41),
    'glow_general_button_x': (20, 22, 30, 40), 'glow_general_button_y': (20, 22, 30, 40),
    'glow_general_button_lb': (25, 25, 42, 37), 'glow_general_button_rb': (21, 25, 39, 37),
    'glow_general_button_lt': (27, 25, 40, 39), 'glow_general_button_rt': (23, 25, 37, 38),
    'glow_general_button_ls': (20, 16, 42, 28), 'glow_general_button_rs': (19, 17, 44, 29),
    'glow_general_button_start': (28, 26, 38, 38), 'glow_general_button_back': (25, 26, 35, 38),
}
KIND = {'a': 'cross', 'b': 'circle', 'x': 'square', 'y': 'triangle', 'lb': 'L1', 'rb': 'R1', 'lt': 'L2', 'rt': 'R2',
        'ls': 'L3', 'rs': 'R3', 'start': 'options', 'back': 'create'}


def S(v):
    return v * SS


def draw(kind, box):
    """our symbol or lettering in the letters' box: body coverage, 64x64, 0-255"""
    x0, y0, x1, y1 = box
    cx, cy, h, w = (x0 + x1 + 1) / 2, (y0 + y1 + 1) / 2, y1 - y0 + 1, x1 - x0 + 1
    im = Image.new('L', (64 * SS, 64 * SS), 0)
    d = ImageDraw.Draw(im)
    r = h / 2 * 0.98
    W = int(S(SW))
    if kind == 'cross':
        k = r * 0.90
        d.line([S(cx - k), S(cy - k), S(cx + k), S(cy + k)], fill=255, width=W)
        d.line([S(cx - k), S(cy + k), S(cx + k), S(cy - k)], fill=255, width=W)
    elif kind == 'circle':
        rr = r * 0.98
        d.ellipse([S(cx - rr), S(cy - rr), S(cx + rr), S(cy + rr)], outline=255, width=W)
    elif kind == 'square':
        k = r * 0.86
        d.rectangle([S(cx - k), S(cy - k), S(cx + k), S(cy + k)], outline=255, width=W)
    elif kind == 'triangle':
        t = r * 1.06
        d.polygon([(S(cx), S(cy - t * 1.05)), (S(cx + t), S(cy + t * 0.8)), (S(cx - t), S(cy + t * 0.8))], outline=255, width=W)
    elif kind == 'options':  # three lines
        for dy in (-4.2, 0, 4.2):
            d.line([S(cx - 5.5), S(cy + dy), S(cx + 5.5), S(cy + dy)], fill=255, width=int(S(2.6)))
    elif kind == 'create':  # three rays
        for ax, ay, bx, by in ((0, 5, 0, -6), (-4.5, 4, -6, -4), (4.5, 4, 6, -4)):
            d.line([S(cx + ax), S(cy + ay), S(cx + bx), S(cy + by)], fill=255, width=int(S(2.6)))
    else:  # lettering: cap height = the old letters', squeezed to their width, thickened to their strokes
        f = ImageFont.truetype(FONT, 400)
        t = Image.new('L', (1400, 700), 0)
        ImageDraw.Draw(t).text((40, 40), kind, font=f, fill=255, stroke_width=14, stroke_fill=255)
        t = t.crop(t.getbbox())
        th = S(h)
        tw = min(S(w + 2), t.size[0] * th / t.size[1])
        t = t.resize((int(tw), int(th)), Image.LANCZOS)
        im.paste(255, (int(S(cx) - t.size[0] / 2), int(S(y0))), t)
    return im.resize((64, 64), Image.BOX)


LW = 2.7  # outline width: the game's own icons' lines


def text_in(d, txt, cx, cy, cap, maxw):
    """bold lettering of cap height `cap`, squeezed to at most maxw, centred on cx, cy"""
    f = ImageFont.truetype(FONT, 400)
    t = Image.new('L', (2600, 700), 0)
    ImageDraw.Draw(t).text((40, 40), txt, font=f, fill=255, stroke_width=10, stroke_fill=255)
    t = t.crop(t.getbbox())
    th = S(cap)
    tw = min(S(maxw), t.size[0] * th / t.size[1])
    t = t.resize((int(tw), int(th)), Image.LANCZOS)
    return t, (int(S(cx) - t.size[0] / 2), int(S(cy) - t.size[1] / 2))


def outline(shapes, inset):
    """the outline of the union of rounded rectangles [(x0, y0, x1, y1, radius)]: filled, then filled again smaller"""
    im = Image.new('L', (64 * SS, 64 * SS), 0)
    d = ImageDraw.Draw(im)
    for x0, y0, x1, y1, r in shapes:
        d.rounded_rectangle([S(x0), S(y0), S(x1), S(y1)], radius=S(r), fill=255)
    for x0, y0, x1, y1, r in shapes:
        d.rounded_rectangle([S(x0 + inset), S(y0 + inset), S(x1 - inset), S(y1 - inset)], radius=S(max(0.5, r - inset)), fill=0)
    return im


def draw_whole(kind):
    """a whole icon, centred in the cell"""
    cy = 31.5
    if kind in ('L1', 'R1'):  # a flat rectangle
        im = outline([(15, 20.5, 49, 42.5, 3.5)], LW)
        t, at = text_in(None, kind, 32, cy, 11, 24)
    elif kind in ('L2', 'R2'):  # a taller rounded box
        im = outline([(19, 15, 45, 48, 7)], LW)
        t, at = text_in(None, kind, 32, cy + 0.5, 10, 18)
    elif kind in ('L3', 'R3'):  # a stick: a wide cap on a stem, its letter in the cap
        im = outline([(12, 17, 52, 35, 8), (25, 31, 39, 46, 2)], LW)
        t, at = text_in(None, kind[0], 32, 26, 10, 12)
    else:  # Options, Create: the name in a rectangle
        im = outline([(7, 21.5, 57, 41.5, 3)], LW)
        t, at = text_in(None, 'OPTIONS' if kind == 'options' else 'CREATE', 32, cy, 8, 36 if kind == 'options' else 33)
    im.paste(255, at, t)
    return im.resize((64, 64), Image.BOX)


def glow_of(body):
    """the highlighted cells' halo, like the game's own: soft, about 0.6 at its brightest"""
    g = body.filter(ImageFilter.GaussianBlur(2.4))
    return g.point(lambda v: int(min(255, v * 1.6) * 0.62))


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
    out = os.path.join(root, 'internal', 'ps3_cells')
    os.makedirs(out, exist_ok=True)
    for name, box in BOXES.items():
        glow = name.startswith('glow_')
        kind = KIND[name.split('_')[-1]]
        whole = kind not in ('cross', 'circle', 'square', 'triangle')
        body = draw_whole(kind) if whole else draw(kind, box)
        data = bytes([(1 if glow else 0) | (2 if whole else 0)]) + body.tobytes() + (glow_of(body).tobytes() if glow else b'')
        with open(os.path.join(out, name + '.cell'), 'wb') as f:
            f.write(data)
    print('wrote', len(BOXES), 'cells to', out)
    labels = os.path.join(root, 'internal', 'arm_labels')
    os.makedirs(labels, exist_ok=True)
    arm_label(os.path.join(labels, 'l2.a8'), 'L2', 20, 13)  # box sizes: rt/glyphs.c g_tags
    arm_label(os.path.join(labels, 'r2.a8'), 'R2', 25, 13)


if __name__ == '__main__':
    main()
