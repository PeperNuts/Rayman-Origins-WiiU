#!/usr/bin/env python3
"""Turn two pictures into the TGA images a .wuhb package carries (meta/iconTex, bootTvTex,
bootDrcTex): icon 128x128 RGBA, TV 1280x720 RGB, GamePad 854x480 RGB. Wider pictures are
cropped to 16:9 around the centre. TGA as FreeImage writes it: uncompressed, bottom-up, BGR(A).
usage: make_wuhb_images.py icon.png splash.jpg outdir"""
import os, struct, sys
from PIL import Image

def tga(img, path):
    w, h = img.size
    alpha = img.mode == 'RGBA'
    hdr = struct.pack('<BBBHHBHHHHBB', 0, 0, 2, 0, 0, 0, 0, 0, w, h, 32 if alpha else 24, 8 if alpha else 0)
    rows = []
    px = img.tobytes()
    bpp = 4 if alpha else 3
    for y in range(h - 1, -1, -1):  # bottom-up
        row = bytearray(px[y * w * bpp:(y + 1) * w * bpp])
        for x in range(0, len(row), bpp):  # RGB(A) -> BGR(A)
            row[x], row[x + 2] = row[x + 2], row[x]
        rows.append(bytes(row))
    open(path, 'wb').write(hdr + b''.join(rows))

def fit(img, w, h):
    sw, sh = img.size
    if sw * h > sh * w:  # wider than the target: crop the sides
        cw = sh * w // h
        img = img.crop(((sw - cw) // 2, 0, (sw - cw) // 2 + cw, sh))
    elif sw * h < sh * w:  # taller: crop top and bottom
        ch = sw * h // w
        img = img.crop((0, (sh - ch) // 2, sw, (sh - ch) // 2 + ch))
    return img.resize((w, h), Image.LANCZOS)

icon, splash, out = sys.argv[1:4]
os.makedirs(out, exist_ok=True)
tga(fit(Image.open(icon).convert('RGBA'), 128, 128), os.path.join(out, 'icon.tga'))
s = Image.open(splash).convert('RGB')
tga(fit(s, 1280, 720), os.path.join(out, 'tv.tga'))
tga(fit(s, 854, 480), os.path.join(out, 'drc.tga'))
print('wrote', out)
