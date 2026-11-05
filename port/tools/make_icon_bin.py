#!/usr/bin/env python3
"""Turn a controller picture into the .bin the port shows instead of the game's Classic
Controller picture (sd:/rayman/mods/icons/gamepad.bin and pro.bin, or content/mods/icons in an
installed title).

The picture is fitted into 256x256 (centred, aspect kept). Without transparency of its own, the
white background connected to the border is made transparent. The result is the game's icon
layout: a CMPR colour mip chain followed by a CMPR alpha mip chain (6 levels, 87360 bytes).

usage: make_icon_bin.py <picture> <out.bin>"""
import sys
from collections import deque
from PIL import Image
sys.path.insert(0, __import__('os').path.dirname(__file__))
from cmpr_enc import encode

src, dst = sys.argv[1:3]
im = Image.open(src).convert('RGBA')
if im.getchannel('A').getextrema()[0] == 255:  # opaque: flood-fill the white background from the border
    w, h = im.size
    px = im.load()
    seen = bytearray(w * h)
    q = deque((x, y) for x in range(w) for y in (0, h - 1))
    q.extend((x, y) for y in range(h) for x in (0, w - 1))
    while q:
        x, y = q.popleft()
        if not (0 <= x < w and 0 <= y < h) or seen[y * w + x]:
            continue
        seen[y * w + x] = 1
        r, g, b, _ = px[x, y]
        if min(r, g, b) < 225:
            continue
        px[x, y] = (r, g, b, 0)
        q.extend(((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)))
im = im.crop(im.getbbox() or (0, 0, im.width, im.height))
s = 240 / max(im.size)
im = im.resize((max(1, round(im.width * s)), max(1, round(im.height * s))), Image.LANCZOS)
out = Image.new('RGBA', (256, 256), (0, 0, 0, 0))
out.alpha_composite(im, ((256 - im.width) // 2, (256 - im.height) // 2))
data = encode(out)
open(dst, 'wb').write(data)
print(dst, len(data), 'bytes')
