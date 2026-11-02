"""Turn the game's write-gather-pipe stores into a real command stream.

On the Wii, GX vertex data and commands are written to the GPU FIFO port at 0xCC008000; the
CPU's write-gather pipe collects the stores into a stream. CodeWarrior emits them as
    lis  rX, 0xCC01            (patched to: lis rX, wii_gx_wgp_window@ha)
    stb/sth/stw/stfs rS, -0x8000(rX)
This post-link pass rewrites every such store into a branch to a per-site trampoline:
    stw   rA, port_wgp_save(r13)        (stfs: via an aligned slot, stored byte by byte)
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
off_save2, off_tmp = syms['port_wgp_save2'] - sda, syms['port_wgp_tmp'] - sda
for o in (off_ptr, off_save, off_save2, off_tmp):
    assert -0x8000 <= o < 0x8000, 'wgp vars out of r13 range'
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

# Second form: the pipe address computed once (addi rP, rLis, -0x8000) and written with
# psq_st fS, 0(rP) - UbiArt's patch/atlas/particle/mesh vertex loops. Needs Gekko decoding
# and register tracking (a register stops being the pipe pointer when overwritten or across
# calls).
gdis = subprocess.run([objdump, '-d', '-M', 'gekko', '--no-show-raw-insn', '--start-address=0x%x' % syms['__game_text_start'],
                       '--stop-address=0x%x' % syms['__game_text_end'], elf], capture_output=True, text=True, check=True).stdout
NOKILL = ('st', 'psq_st', 'cmp', 'b', 'mt', 'tw', 'dcb', 'icb', 'sync', 'isync', 'eieio', 'cr', 'mcrf', 'sc', 'rfi', 'nop')
psq_sites = []
lis, der = set(), set()
for l in gdis.splitlines():
    if re.match(r'^[0-9a-f]+ <.*>:$', l):
        lis, der = set(), set()
        continue
    m = re.match(r'^\s*([0-9a-f]+):\s+(\S+)\s*(.*)$', l)
    if not m:
        continue
    addr, mn, ops = int(m.group(1), 16), m.group(2), m.group(3)
    o = [x.strip() for x in ops.split(',')]
    if mn == 'lis' and len(o) == 2 and (int(o[1]) & 0xFFFF) == ha:
        lis.add(o[0]); der.discard(o[0]); continue
    if mn == 'addi' and len(o) == 3 and o[1] in lis and o[2] == '-32768':
        der.add(o[0]); continue
    if mn == 'mr' and len(o) == 2 and o[1] in der:
        der.add(o[0]); continue
    if mn.startswith(('st', 'psq_st')):
        mm = re.match(r'(-?\d+)\((r\d+)\)', o[1]) if len(o) > 1 else None
        if mm and mm.group(2) in der:
            if mn == 'psq_st' and mm.group(1) == '0' and o[2] == '0' and o[3] == '0':
                psq_sites.append(addr)
            else:
                raise SystemExit(f'unhandled pipe store {mn} {ops} at {addr:#x}')
        continue
    if mn in ('bl', 'blrl', 'bctrl'):
        for r in ['r0'] + ['r%d' % i for i in range(3, 13)]:
            lis.discard(r); der.discard(r)
        continue
    if not mn.startswith(NOKILL) and o and re.match(r'r\d+$', o[0]):
        lis.discard(o[0]); der.discard(o[0])

TRAMP_WORDS, FLOAT_WORDS = 7, 18
need = sum((FLOAT_WORDS if op == 'stfs' else TRAMP_WORDS) * 4 for _, op in sites) + len(psq_sites) * 4 * 29
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


def D(o):
    return o & 0xFFFF


t = tramp
for addr, op in sites:
    w = word(addr)
    prim, size = SIZES[op]
    assert w >> 26 == prim and (w & 0xFFFF) == 0x8000, hex(addr)
    rs = (w >> 21) & 31
    if op == 'stfs':
        # Floating-point stores must be word-aligned on Espresso (an unaligned stfs raises an
        # alignment exception); the stream is byte-granular, so go through an aligned slot
        # and store the four bytes individually.
        rot = (21 << 26) | (11 << 21) | (11 << 16) | (8 << 11) | (0 << 6) | (31 << 1)  # rotlwi r11,r11,8
        seq = [
            (36 << 26) | (12 << 21) | (13 << 16) | D(off_save),    # stw  r12, save(r13)
            (36 << 26) | (11 << 21) | (13 << 16) | D(off_save2),   # stw  r11, save2(r13)
            (52 << 26) | (rs << 21) | (13 << 16) | D(off_tmp),     # stfs fS, tmp(r13)
            (32 << 26) | (11 << 21) | (13 << 16) | D(off_tmp),     # lwz  r11, tmp(r13)
            (32 << 26) | (12 << 21) | (13 << 16) | D(off_ptr),     # lwz  r12, ptr(r13)
        ]
        for i in range(4):
            seq += [rot, (38 << 26) | (11 << 21) | (12 << 16) | i]  # rotlwi; stb r11, i(r12)
        seq += [
            (14 << 26) | (12 << 21) | (12 << 16) | 4,              # addi r12, r12, 4
            (36 << 26) | (12 << 21) | (13 << 16) | D(off_ptr),     # stw  r12, ptr(r13)
            (32 << 26) | (11 << 21) | (13 << 16) | D(off_save2),   # lwz  r11, save2(r13)
            (32 << 26) | (12 << 21) | (13 << 16) | D(off_save),    # lwz  r12, save(r13)
        ]
    else:
        ra = 11 if rs == 12 else 12
        seq = [
            (36 << 26) | (ra << 21) | (13 << 16) | D(off_save),    # stw  rA, save(r13)
            (32 << 26) | (ra << 21) | (13 << 16) | D(off_ptr),     # lwz  rA, ptr(r13)
            (prim << 26) | (rs << 21) | (ra << 16),                # st   rS, 0(rA)
            (14 << 26) | (ra << 21) | (ra << 16) | size,           # addi rA, rA, size
            (36 << 26) | (ra << 21) | (13 << 16) | D(off_ptr),     # stw  rA, ptr(r13)
            (32 << 26) | (ra << 21) | (13 << 16) | D(off_save),    # lwz  rA, save(r13)
        ]
    for i, x in enumerate(seq):
        put(t + 4 * i, x)
    put(t + 4 * len(seq), branch(t + 4 * len(seq), addr + 4))
    put(addr, branch(addr, t))
    t += 4 * (len(seq) + 1)

rot = (21 << 26) | (11 << 21) | (11 << 16) | (8 << 11) | (0 << 6) | (31 << 1)  # rotlwi r11,r11,8
for addr in psq_sites:
    w = word(addr)
    assert w >> 26 == 60 and (w & 0xFFFF) == 0, hex(addr)       # psq_st fS, 0(rP), W=0, I=0
    fs = (w >> 21) & 31
    seq = [
        (36 << 26) | (12 << 21) | (13 << 16) | D(off_save),      # stw  r12, save(r13)
        (36 << 26) | (11 << 21) | (13 << 16) | D(off_save2),     # stw  r11, save2(r13)
        (14 << 26) | (12 << 21) | (13 << 16) | D(off_tmp),       # addi r12, r13, tmp
        (60 << 26) | (fs << 21) | (12 << 16),                    # psq_st fS, 0(r12), 0, 0
        (32 << 26) | (11 << 21) | (12 << 16) | 0,                # lwz  r11, 0(r12)
        (32 << 26) | (12 << 21) | (13 << 16) | D(off_ptr),       # lwz  r12, ptr(r13)
    ]
    for i in range(4):
        seq += [rot, (38 << 26) | (11 << 21) | (12 << 16) | i]
    seq += [(32 << 26) | (11 << 21) | (13 << 16) | D(off_tmp + 4)]  # lwz r11, tmp+4(r13)
    for i in range(4):
        seq += [rot, (38 << 26) | (11 << 21) | (12 << 16) | (4 + i)]
    seq += [
        (14 << 26) | (12 << 21) | (12 << 16) | 8,                # addi r12, r12, 8
        (36 << 26) | (12 << 21) | (13 << 16) | D(off_ptr),       # stw  r12, ptr(r13)
        (32 << 26) | (11 << 21) | (13 << 16) | D(off_save2),     # lwz  r11, save2(r13)
        (32 << 26) | (12 << 21) | (13 << 16) | D(off_save),      # lwz  r12, save(r13)
    ]
    for i, x in enumerate(seq):
        put(t + 4 * i, x)
    put(t + 4 * len(seq), branch(t + 4 * len(seq), addr + 4))
    put(addr, branch(addr, t))
    t += 4 * (len(seq) + 1)
assert t <= tramp_end, 'trampoline area overflow'

open(elf, 'wb').write(data)
print(f'wgp: patched {len(sites)} FIFO stores + {len(psq_sites)} paired-single pipe stores ({", ".join(f"{k} {sum(1 for _, o in sites if o == k)}" for k in SIZES)})')
