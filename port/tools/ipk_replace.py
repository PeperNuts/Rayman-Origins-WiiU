"""Replace one file inside a Rayman Origins (UbiArt) .ipk bundle.

IPK layout (as found in the Wii files): header (u32 magic 0x50EC12BA, version, ?, base
offset of the data, file count, ...), then from 0x30 one entry per file:
  u32 parts (1), u32 size, u32 compressed size (0 = stored), u64 timestamp,
  u64 offset (relative to the base offset), u32 path length, UTF-16BE path.
The replacement is appended to the end of the bundle, stored uncompressed, and the entry
is pointed at it; every other byte of the bundle stays as it was.

usage: ipk_replace.py <in.ipk> <out.ipk> <path inside the ipk> <replacement file>
"""
import struct
import sys

src, dst, target, repl = sys.argv[1:5]
d = bytearray(open(src, 'rb').read())
if struct.unpack('>I', d[0:4])[0] != 0x50EC12BA:
    sys.exit('ipk_replace: not an IPK file')
base, count = struct.unpack('>II', d[12:20])
new = open(repl, 'rb').read()
o = 0x30
for _ in range(count):
    entry = o
    o += 12 + 16
    n, = struct.unpack('>I', d[o:o + 4])
    path = d[o + 4:o + 4 + n * 2].decode('utf-16-be')
    o += 4 + n * 2
    if path == target:
        while len(d) % 32:
            d.append(0)
        off = len(d) - base
        d += new
        struct.pack_into('>II', d, entry + 4, len(new), 0)  # size, stored
        struct.pack_into('>Q', d, entry + 20, off)
        open(dst, 'wb').write(d)
        print('ipk_replace: %s -> %d bytes at 0x%X' % (target, len(new), base + off))
        break
else:
    sys.exit('ipk_replace: %s not found' % target)
