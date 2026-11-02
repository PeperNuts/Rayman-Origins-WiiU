"""Turn the game's write-gather-pipe stores into a real command stream.

On the Wii, GX vertex data and commands are written to the GPU FIFO port at 0xCC008000; the
CPU's write-gather pipe collects the stores into a stream. CodeWarrior emits them as
    lis  rX, 0xCC01            (patched to: lis rX, wii_gx_wgp_window@ha)
    stb/sth/stw/stfs rS, -0x8000(rX)
This post-link pass rewrites every such store into a branch to a per-site trampoline:
    stw   rA, port_wgp_save(r13)
    lwz   rA, port_wgp_ptr(r13)
    <st>  rS, 0(rA)
    addi  rA, rA, <size>
    stw   rA, port_wgp_ptr(r13)
    lwz   rA, port_wgp_save(r13)
    b     <site + 4>
r13 is the game's small-data base whenever game code runs, so the trampolines need no
relocations. All branches stay inside .text, so the loader's section relocation keeps them valid.

usage: wgp_patch.py <objdump-bin> <nm-bin> <elf>   (patches <elf> in place)
"""
import re
import struct
import subprocess
import sys

objdump, nm, elf = sys.argv[1:4]

syms = {}
for l in subprocess.run([nm, elf], capture_output=True, text=True, check=True).stdout.splitlines():
    p = l.split()
    if len(p) == 3:
        syms[p[2]] = int(p[0], 16)

window, sda = syms['wii_gx_wgp_window'], syms['_SDA_BASE_']
tramp, tramp_end = syms['port_wgp_tramp'], syms['port_wgp_tramp_end']
off_ptr, off_save = syms['port_wgp_ptr'] - sda, syms['port_wgp_save'] - sda
assert -0x8000 <= off_ptr < 0x8000 and -0x8000 <= off_save < 0x8000, 'wgp vars out of r13 range'
ha = ((window + 0x8000) >> 16) & 0xFFFF

# .text address and file offset
text_vma = text_off = None
for l in subprocess.run([objdump, '-h', elf], capture_output=True, text=True, check=True).stdout.splitlines():
    p = l.split()
    if len(p) >= 6 and p[1] == '.text':
        text_vma, text_off = int(p[3], 16), int(p[5], 16)
assert text_vma is not None

dis = subprocess.run([objdump, '-d', '--no-show-raw-insn', '--start-address=0x%x' % syms['__game_text_start'],
                      '--stop-address=0x%x' % syms['__game_text_end'], elf],
                     capture_output=True, text=True, check=True).stdout

SIZES = {'stb': (38, 1), 'sth': (44, 2), 'stw': (36, 4), 'stfs': (52, 4)}
sites = []
lisregs = set()
for l in dis.splitlines():
    if re.match(r'^[0-9a-f]+ <.*>:$', l):
        lisregs = set()
        continue
    m = re.match(r'^\s*([0-9a-f]+):\s+lis\s+(r\d+),(-?\d+)$', l)
    if m and (int(m.group(3)) & 0xFFFF) == ha:
        lisregs.add(m.group(2))
        continue
    m = re.match(r'^\s*([0-9a-f]+):\s+(stb|sth|stw|stfs)\s+(f?r?\d+),-32768\((r\d+)\)$', l)
    if m and m.group(4) in lisregs:
        sites.append((int(m.group(1), 16), m.group(2)))

TRAMP_WORDS = 7
need = len(sites) * TRAMP_WORDS * 4
assert need <= tramp_end - tramp, f'trampoline area too small: need {need}, have {tramp_end - tramp}'

data = bytearray(open(elf, 'rb').read())


def word(addr):
    o = text_off + addr - text_vma
    return struct.unpack('>I', data[o:o + 4])[0]


def put(addr, w):
    o = text_off + addr - text_vma
    data[o:o + 4] = struct.pack('>I', w & 0xFFFFFFFF)


def branch(src, dst):
    d = dst - src
    assert -0x2000000 <= d < 0x2000000
    return (18 << 26) | (d & 0x03FFFFFC)


t = tramp
for addr, op in sites:
    w = word(addr)
    prim, size = SIZES[op]
    assert w >> 26 == prim and (w & 0xFFFF) == 0x8000, hex(addr)
    rs = (w >> 21) & 31
    ra = 11 if (op != 'stfs' and rs == 12) else 12
    seq = [
        (36 << 26) | (ra << 21) | (13 << 16) | (off_save & 0xFFFF),   # stw  rA, save(r13)
        (32 << 26) | (ra << 21) | (13 << 16) | (off_ptr & 0xFFFF),    # lwz  rA, ptr(r13)
        (prim << 26) | (rs << 21) | (ra << 16),                       # st   rS, 0(rA)
        (14 << 26) | (ra << 21) | (ra << 16) | size,                  # addi rA, rA, size
        (36 << 26) | (ra << 21) | (13 << 16) | (off_ptr & 0xFFFF),    # stw  rA, ptr(r13)
        (32 << 26) | (ra << 21) | (13 << 16) | (off_save & 0xFFFF),   # lwz  rA, save(r13)
    ]
    for i, s in enumerate(seq):
        put(t + 4 * i, s)
    put(t + 24, branch(t + 24, addr + 4))
    put(addr, branch(addr, t))
    t += TRAMP_WORDS * 4

open(elf, 'wb').write(data)
print(f'wgp: patched {len(sites)} FIFO stores ({", ".join(f"{k} {sum(1 for _, o in sites if o == k)}" for k in SIZES)})')
