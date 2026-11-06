#!/usr/bin/env python3
"""Draws the PS5 app icon, ps5/sce_sys/icon0.png (512x512).

Original artwork made for this project: a Mojave highway at dusk with a distant city, lettered in DejaVu Sans Bold.
It uses no artwork, logo or lettering from the game.

    python3 tools/make_icon.py [out.png]

Needs Pillow and DejaVu Sans Bold (fonts-dejavu-core)."""
import sys
from PIL import Image, ImageDraw, ImageFilter, ImageFont

FONT = '/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf'
S = 2048              # drawn large, then scaled down for smooth edges
H = int(S * 0.64)     # horizon


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


def gradient(img, top, stops):
    d = ImageDraw.Draw(img)
    for y in range(top, top + len(stops)):
        d.line([(0, y), (S, y)], fill=stops[y - top])


def sky_colour(y):
    keys = [(0.0, (14, 10, 34)), (0.45, (62, 22, 64)), (0.75, (168, 52, 52)), (1.0, (246, 150, 56))]
    t = y / H
    for (t0, c0), (t1, c1) in zip(keys, keys[1:]):
        if t <= t1:
            return lerp(c0, c1, (t - t0) / (t1 - t0))
    return keys[-1][1]


def main(out):
    img = Image.new('RGB', (S, S))
    gradient(img, 0, [sky_colour(y) for y in range(H)])
    d = ImageDraw.Draw(img)

    # a few stars high up
    for i in range(70):
        x = (i * 7919) % S
        y = (i * 104729) % int(H * 0.45)
        r = 2 + i % 3
        d.ellipse([x - r, y - r, x + r, y + r], fill=(235, 225, 255))

    # the setting sun, with a soft glow
    glow = Image.new('L', (S, S), 0)
    gd = ImageDraw.Draw(glow)
    cx, cy, r = int(S * 0.5), int(H * 0.93), int(S * 0.2)
    gd.ellipse([cx - r * 1.6, cy - r * 1.6, cx + r * 1.6, cy + r * 1.6], fill=110)
    glow = glow.filter(ImageFilter.GaussianBlur(S // 18))
    img.paste((255, 170, 70), (0, 0), glow)
    d = ImageDraw.Draw(img)
    for y in range(cy - r, cy + r):
        t = (y - (cy - r)) / (2 * r)
        half = int((r * r - (y - cy) ** 2) ** 0.5)
        d.line([(cx - half, y), (cx + half, y)], fill=lerp((255, 236, 150), (250, 128, 44), t))

    # mesas on the horizon
    mesa = (34, 18, 26)
    d.polygon([(0, H), (0, H - 150), (120, H - 170), (300, H - 175), (360, H - 120), (520, H - 110), (600, H)], fill=mesa)
    d.polygon([(1250, H), (1380, H - 210), (1420, H - 230), (1700, H - 235), (1760, H - 190), (1900, H - 180),
               (2048, H - 140), (2048, H)], fill=mesa)

    # the distant city: plain towers with lit windows
    city = (24, 14, 30)
    towers = [(760, 90, 44), (812, 150, 40), (860, 120, 50), (918, 260, 46), (972, 190, 54), (1034, 330, 40),
              (1082, 220, 48), (1138, 160, 44), (1190, 110, 52), (1250, 80, 40)]
    for x, h, w in towers:
        d.rectangle([x, H - h, x + w, H], fill=city)
        for wy in range(H - h + 14, H - 10, 22):
            for wx in range(x + 8, x + w - 8, 14):
                if (wx * 31 + wy * 17) % 5 < 2:
                    d.rectangle([wx, wy, wx + 5, wy + 8], fill=(255, 196, 96))
    d.line([(1054, H - 330), (1054, H - 420)], fill=city, width=8)            # an antenna
    d.ellipse([1046, H - 432, 1062, H - 416], fill=(255, 90, 70))

    # desert floor
    gradient(img, H, [lerp((70, 36, 30), (22, 12, 14), (y - H) / (S - H)) for y in range(H, S)])
    d = ImageDraw.Draw(img)

    # the highway, running to the city
    vx = S // 2
    d.polygon([(vx - 14, H), (vx + 14, H), (int(S * 0.86), S), (int(S * 0.14), S)], fill=(36, 30, 34))
    for i in range(9):    # dashed centre line, longer and wider as it nears
        t0, t1 = (i + 0.15) / 9, (i + 0.55) / 9
        y0, y1 = H + (S - H) * t0 ** 1.7, H + (S - H) * t1 ** 1.7
        w0, w1 = 3 + 22 * t0 ** 1.7, 3 + 22 * t1 ** 1.7
        d.polygon([(vx - w0, y0), (vx + w0, y0), (vx + w1, y1), (vx - w1, y1)], fill=(242, 180, 74))

    # lettering, with a dark shadow so it reads on a busy home screen
    def text(s, size, y, fill, spacing):
        f = ImageFont.truetype(FONT, size)
        widths = [d.textlength(ch, font=f) for ch in s]
        total = sum(widths) + spacing * (len(s) - 1)
        x = (S - total) / 2
        for ch, w in zip(s, widths):
            for dx, dy in ((10, 12), (6, 6)):
                d.text((x + dx, y + dy), ch, font=f, fill=(10, 6, 12))
            d.text((x, y), ch, font=f, fill=fill)
            x += w + spacing

    text('NEW VEGAS', 250, int(S * 0.13), (248, 196, 92), 18)
    text('RECOMPILED', 118, int(S * 0.29), (250, 236, 214), 30)

    img.resize((512, 512), Image.LANCZOS).save(out)


if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else 'ps5/sce_sys/icon0.png')
