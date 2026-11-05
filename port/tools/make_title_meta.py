#!/usr/bin/env python3
"""Make the meta/ pictures and sounds of an installable Wii U title from your own files.

usage: make_title_meta.py OUTDIR --icon ICON --splash SPLASH [--logo LOGO] [--sound AUDIO] [--seconds N]

  ICON    square picture -> iconTex.tga (128x128 RGBA)
  SPLASH  wide picture   -> bootTvTex.tga (1280x720), bootDrcTex.tga (854x480), bootMovie.h264
  LOGO    optional picture for the top-left of the boot screen -> bootLogoTex.tga (170x42 RGBA;
          transparent without one)
  AUDIO   optional sound (any format ffmpeg reads) -> bootSound.btsnd: the first N seconds
          (default 8) with a fade-out, 48 kHz stereo 16-bit big-endian (silent without one)

TGAs are written the way the Wii U Menu expects them: uncompressed, bottom-up, BGR(A), with a TGA 2.0
footer. Needs Pillow, and ffmpeg for the boot movie and sound."""
import argparse, os, struct, subprocess, tempfile
from PIL import Image

FOOT = b'\0' * 8 + b'TRUEVISION-XFILE.\0'

def tga(img, path):
    w, h = img.size
    alpha = img.mode == 'RGBA'
    bpp = 4 if alpha else 3
    hdr = struct.pack('<BBBHHBHHHHBB', 0, 0, 2, 0, 0, 0, 0, 0, w, h, 32 if alpha else 24, 8 if alpha else 0)
    px, rows = img.tobytes(), []
    for y in range(h - 1, -1, -1):
        row = bytearray(px[y * w * bpp:(y + 1) * w * bpp])
        for x in range(0, len(row), bpp):
            row[x], row[x + 2] = row[x + 2], row[x]
        rows.append(bytes(row))
    open(path, 'wb').write(hdr + b''.join(rows) + FOOT)

def fit(img, w, h):  # crop to the target aspect around the centre, then scale
    sw, sh = img.size
    if sw * h > sh * w:
        cw = sh * w // h
        img = img.crop(((sw - cw) // 2, 0, (sw - cw) // 2 + cw, sh))
    elif sw * h < sh * w:
        ch = sw * h // w
        img = img.crop((0, (sh - ch) // 2, sw, (sh - ch) // 2 + ch))
    return img.resize((w, h), Image.LANCZOS)

ap = argparse.ArgumentParser()
ap.add_argument('out'); ap.add_argument('--icon', required=True); ap.add_argument('--splash', required=True)
ap.add_argument('--logo'); ap.add_argument('--sound'); ap.add_argument('--seconds', type=float, default=8)
a = ap.parse_args()
os.makedirs(a.out, exist_ok=True)
tga(fit(Image.open(a.icon).convert('RGBA'), 128, 128), f'{a.out}/iconTex.tga')
splash = Image.open(a.splash).convert('RGB')
tga(fit(splash, 1280, 720), f'{a.out}/bootTvTex.tga')
tga(fit(splash, 854, 480), f'{a.out}/bootDrcTex.tga')

W, H = 170, 42  # logo: kept whole, left-aligned, transparent around it
logo = Image.new('RGBA', (W, H), (0, 0, 0, 0))
if a.logo:
    im = Image.open(a.logo).convert('RGBA')
    im = im.crop(im.getbbox())
    h = H - 4; w = round(im.width * h / im.height)
    if w > W - 4: w = W - 4; h = round(im.height * w / im.width)
    logo.alpha_composite(im.resize((w, h), Image.LANCZOS), (2, (H - h) // 2))
tga(logo, f'{a.out}/bootLogoTex.tga')

with tempfile.TemporaryDirectory() as tmp:
    png = f'{tmp}/s.png'; fit(splash, 256, 128).save(png)
    subprocess.run(['ffmpeg', '-v', 'error', '-y', '-loop', '1', '-i', png, '-t', '2', '-vf', 'format=yuv420p',
                    '-r', '60000/1001', '-c:v', 'libx264', '-profile:v', 'high', '-level', '2.0', '-f', 'h264',
                    f'{a.out}/bootMovie.h264'], check=True)
    if a.sound:
        fade = max(0.0, a.seconds - 1.5)
        subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', a.sound, '-t', str(a.seconds), '-af',
                        f'afade=t=out:st={fade}:d=1.5', '-ac', '2', '-ar', '48000', '-f', 's16be', f'{tmp}/s.pcm'],
                       check=True)
        pcm = open(f'{tmp}/s.pcm', 'rb').read()
    else:
        pcm = b'\0' * (48000 * 4)
n = len(pcm) // 4
open(f'{a.out}/bootSound.btsnd', 'wb').write(struct.pack('>II', 2, n) + pcm)  # plays once
print('meta pictures and sounds written to', a.out)
