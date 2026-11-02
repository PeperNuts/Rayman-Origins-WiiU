"""Rewrite every R_PPC_EMB_SDA21 relocation in a linked ELF into R_PPC_NONE.

The instructions were already patched by ld (offset + base register r2/r13). Their offsets
stay valid as long as each small-data area moves as one block with its anchor, which
rayman.ld guarantees. Leaving the relocations would let the loader recompute them against
its own idea of the small-data base.

usage: neutralize_sda.py in.elf out.elf
"""
import struct
import sys

R_PPC_NONE, R_PPC_EMB_SDA21 = 0, 109
SHT_RELA = 4

src, dst = sys.argv[1], sys.argv[2]
d = bytearray(open(src, 'rb').read())
shoff, = struct.unpack('>I', d[0x20:0x24])
shentsize, shnum = struct.unpack('>HH', d[0x2E:0x32])
count = 0
for i in range(shnum):
    h = struct.unpack('>10I', d[shoff + i * shentsize:shoff + i * shentsize + 40])
    if h[1] != SHT_RELA:
        continue
    off, size = h[4], h[5]
    for e in range(off, off + size, 12):
        info, = struct.unpack('>I', d[e + 4:e + 8])
        if info & 0xFF == R_PPC_EMB_SDA21:
            struct.pack_into('>I', d, e + 4, info & ~0xFF | R_PPC_NONE)
            count += 1
open(dst, 'wb').write(d)
print(f'neutralized {count} SDA21 relocations')
