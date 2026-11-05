#!/usr/bin/env python3
"""Do what FalloutNVLauncher.exe does on first run: copy Fallout_default.ini to My Games and
build FalloutPrefs.ini from a quality preset, naming the recompiled runtime's adapter so the
game does not hand off to the launcher.

Usage: first_run.py GAME_DIR USER_DIR [--preset medium] [--width 1280 --height 720]"""
import argparse, os, re, shutil

ap = argparse.ArgumentParser()
ap.add_argument('game'); ap.add_argument('user')
ap.add_argument('--preset', default='medium'); ap.add_argument('--width', default='1280'); ap.add_argument('--height', default='720')
a = ap.parse_args()
d = os.path.join(a.user, 'Documents', 'My Games', 'FalloutNV')
os.makedirs(d, exist_ok=True)
shutil.copyfile(os.path.join(a.game, 'Fallout_default.ini'), os.path.join(d, 'Fallout.ini'))
s = open(os.path.join(a.game, a.preset + '.ini'), encoding='latin-1').read()

def setv(s, sec, key, val):
    pat = re.compile(r'(\[%s\][^\[]*?^)%s=.*?$' % (re.escape(sec), re.escape(key)), re.M | re.S | re.I)
    if pat.search(s):
        return pat.sub(lambda m: m.group(1) + key + '=' + val, s, count=1)
    return re.sub(r'(\[%s\]\r?\n)' % re.escape(sec), lambda m: m.group(1) + key + '=' + val + '\r\n', s, count=1, flags=re.I)

for k, v in [('sD3DDevice', '"NVIDIA GeForce 8800 GT (fnv_recomp)"'), ('iSize W', a.width), ('iSize H', a.height),
             ('bFull Screen', '0')]:
    s = setv(s, 'Display', k, v)
open(os.path.join(d, 'FalloutPrefs.ini'), 'w', encoding='latin-1').write(s)
print('wrote', d)
