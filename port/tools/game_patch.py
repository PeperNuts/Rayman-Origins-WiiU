"""Patch individual instructions of the linked game code (post-link, like wgp_patch.py).

Each patch names a game function, an offset in it, the expected original word and the new one,
so a wrong match fails the build instead of corrupting code.

usage: game_patch.py <nm-bin> <elf>
"""
import struct
import subprocess
import sys

nm, elf = sys.argv[1:3]

# (function, offset, expected word, new word) - or a branch to a port function: new word given
# as the name of the target symbol.
PATCHES = [
    # LocalisationManager::getText tail-calls Localisation_Template::getText: go through
    # port_loc_gettext (loc_hook.c) for the controller-specific texts.
    ('getText__Q23ITF19LocalisationManagerCFRCQ23ITF14LocalisationId', 0x0C, None, 'port_loc_gettext'),
    # ...which calls the original at this distance from itself
    ('port_loc_template_gettext', 0, 0xDEAD0001, ('addr', 'getText__Q23ITF21Localisation_TemplateCFRCQ23ITF14LocalisationIdQ23ITF12ITF_LANGUAGE')),
    # raki ADPCMStreamSoundNative::postCallback compares the AX voice's current address
    # (nibbles, /2) with stream buffer addresses converted to Wii physical addresses
    # (addis -0x8000). Game memory is at 0x1xxxxxxx here, where that conversion no longer
    # matches the address encoding the voice reports: compare the plain addresses instead.
    ('postCallback__Q24raki22ADPCMStreamSoundNativeFv', 0x54, 0x3CA58000, 0x60000000),  # addis r5,r5,-32768 -> nop
    ('postCallback__Q24raki22ADPCMStreamSoundNativeFv', 0x64, 0x3C848000, 0x60000000),  # addis r4,r4,-32768 -> nop
    # Wii Health & Safety screen (SystemAdapter_SplashScreen::update, states 1 fade in,
    # 2 wait for a button / time, 3 fade out): every state ends at once.
    ('update__Q23ITF26SystemAdapter_SplashScreenFv', 0x094, 0x40800028, 0x60000000),  # fade in: bge (still fading) -> nop
    ('update__Q23ITF26SystemAdapter_SplashScreenFv', 0x0F0, 0x40800060, 0x60000000),  # fade out: bge (still fading) -> nop
    ('update__Q23ITF26SystemAdapter_SplashScreenFv', 0x1C8, 0x41800020, 0x48000020),  # wait: blt (time up) -> b
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
    if typ == 1 and flags & 2:  # PROGBITS, allocated (code and data)
        secs.append((addr, off, size))

for fn, delta, old, new in PATCHES:
    a = syms[fn] + delta
    if isinstance(new, tuple):  # ('addr', symbol): the symbol's distance from port_loc_gettext
        new = (syms[new[1]] - syms['port_loc_gettext']) & 0xFFFFFFFF
    elif isinstance(new, str):  # b <symbol>
        rel = syms[new] - a
        if not -0x2000000 <= rel < 0x2000000:
            sys.exit('game_patch: %s out of branch range' % new)
        new = 0x48000000 | (rel & 0x03FFFFFC)
    for addr, off, size in secs:
        if addr <= a < addr + size:
            fo = off + a - addr
            cur, = struct.unpack('>I', data[fo:fo + 4])
            if old is not None and cur != old:
                sys.exit('game_patch: %s+0x%X is %08X, expected %08X' % (fn, delta, cur, old))
            data[fo:fo + 4] = struct.pack('>I', new)
            break
    else:
        sys.exit('game_patch: %s not in code' % fn)
open(elf, 'wb').write(data)
print('game_patch: %d instructions patched' % len(PATCHES))
