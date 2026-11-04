# Wii CMPR (DXT1 in 8x8 tiles, big-endian) encoder, plus the game's colour+alpha layout:
# [CMPR colour mip chain][CMPR alpha (grey) mip chain].
import struct
from PIL import Image
def to565(c): return ((c[0] * 31 + 127) // 255) << 11 | ((c[1] * 63 + 127) // 255) << 5 | ((c[2] * 31 + 127) // 255)
def from565(v): return ((v >> 11) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31)
def block(px):
    lum = [p[0] * 3 + p[1] * 6 + p[2] for p in px]
    lo = px[lum.index(min(lum))]; hi = px[lum.index(max(lum))]
    c0, c1 = to565(hi), to565(lo)
    if c0 == c1:
        return struct.pack('>HH', c0, c1) + b'\0\0\0\0'
    if c0 < c1: c0, c1 = c1, c0
    p0, p1 = from565(c0), from565(c1)
    pal = [p0, p1, tuple((2 * a + b) // 3 for a, b in zip(p0, p1)), tuple((a + 2 * b) // 3 for a, b in zip(p0, p1))]
    idx = bytearray(4)
    for i, p in enumerate(px):
        best = min(range(4), key=lambda k: sum((p[j] - pal[k][j]) ** 2 for j in range(3)))
        idx[i // 4] |= best << (6 - 2 * (i % 4))
    return struct.pack('>HH', c0, c1) + bytes(idx)
def cmpr(img):
    W, H = img.size; pix = img.load(); out = bytearray()
    for ty in range(0, H, 8):
        for tx in range(0, W, 8):
            for sb in range(4):
                bx = tx + (sb & 1) * 4; by = ty + (sb >> 1) * 4
                px = [pix[min(bx + x, W - 1), min(by + y, H - 1)][:3] for y in range(4) for x in range(4)]
                out += block(px)
    return out
def chain(img, levels):
    out = bytearray(); w = img.size[0]
    for l in range(levels):
        out += cmpr(img.resize((w >> l, w >> l), Image.LANCZOS) if l else img)
    return out
def encode(rgba, levels=6):
    rgb = rgba.convert('RGB'); a = rgba.split()[3]
    return chain(rgb, levels) + chain(Image.merge('RGB', (a, a, a)), levels)
