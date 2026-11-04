"""Patch individual instructions of the linked game code (post-link, like wgp_patch.py).

Each patch names a game function, an offset in it, the expected original word and the new one,
so a wrong match fails the build instead of corrupting code.

usage: game_patch.py <nm-bin> <elf>
"""
import struct
import subprocess
import sys

nm, elf = sys.argv[1:3]

PATCHES = [
    # raki ADPCMStreamSoundNative::postCallback compares the AX voice's current address
    # (nibbles, /2) with stream buffer addresses converted to Wii physical addresses
    # (addis -0x8000). Game memory is at 0x1xxxxxxx here, where that conversion no longer
    # matches the address encoding the voice reports: compare the plain addresses instead.
    ('postCallback__Q24raki22ADPCMStreamSoundNativeFv', 0x54, 0x3CA58000, 0x60000000),  # addis r5,r5,-32768 -> nop
    ('postCallback__Q24raki22ADPCMStreamSoundNativeFv', 0x64, 0x3C848000, 0x60000000),  # addis r4,r4,-32768 -> nop
]

syms = {}
for l in subprocess.run([nm, elf], capture_output=True, text=True, check=True).stdout.splitlines():
    p = l.split()
    if len(p) == 3:
        syms[p[2]] = int(p[0], 16)

data = bytearray(open(elf, 'rb').read())
shoff, = struct.unpack('>I', data[0x20:0x24])
shentsize, shnum = struct.unpack('>HH', data[0x2E:0x32])
secs = []
for i in range(shnum):
    sh = data[shoff + i * shentsize:shoff + (i + 1) * shentsize]
    name, typ, flags, addr, off, size = struct.unpack('>IIIIII', sh[:24])
    if typ == 1 and flags & 4:
        secs.append((addr, off, size))

for fn, delta, old, new in PATCHES:
    a = syms[fn] + delta
    for addr, off, size in secs:
        if addr <= a < addr + size:
            fo = off + a - addr
            cur, = struct.unpack('>I', data[fo:fo + 4])
            if cur != old:
                sys.exit('game_patch: %s+0x%X is %08X, expected %08X' % (fn, delta, cur, old))
            data[fo:fo + 4] = struct.pack('>I', new)
            break
    else:
        sys.exit('game_patch: %s not in code' % fn)
open(elf, 'wb').write(data)
print('game_patch: %d instructions patched' % len(PATCHES))
