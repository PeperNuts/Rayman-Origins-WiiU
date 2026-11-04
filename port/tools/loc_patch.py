"""Rewrite controller texts in Rayman Origins' localisation.loc for the Wii U port.

The port presents the Wii U GamePad and Pro Controller to the game as Classic Controllers
(same buttons in the same places). This adds texts 9049 (GamePad) and 9050 (Pro Controller)
next to text 4049 ("The controls are in Classic style."), which the port shows instead of
4049 for those controllers (src/loc_hook.c), makes the Wii Remote disconnect message
controller-neutral, for all six languages, and writes a new file for
sd:/rayman/mods/localisation/localisation.loc (the port reads files in sd:/rayman/mods/
instead of the disc files; the original stays).

File layout: u32 language count; per language: u32 language id, u32 string count, then per
string: u32 string id, u32 length (UTF-16 units), UTF-16BE text. Then a voice table, kept.

usage: loc_patch.py <original localisation.loc> <output localisation.loc>
"""
import struct
import sys

EN, FR, DE, ES, IT, NL = 0, 1, 3, 4, 5, 12
ADD = {
    9049: {  # Wii U GamePad (shown instead of 4049)
        EN: 'The controls are in Wii U GamePad style.',
        FR: 'Les commandes sont en mode Wii U GamePad.',
        DE: 'Steuerung im Wii U GamePad-Modus',
        ES: 'Los controles están en modo Wii U GamePad.',
        IT: 'I comandi sono in configurazione Wii U GamePad.',
        NL: 'Je gebruikt nu de Wii U GamePad-combinatie.',
    },
    9050: {  # Wii U Pro Controller (shown instead of 4049)
        EN: 'The controls are in Wii U Pro Controller style.',
        FR: 'Les commandes sont en mode manette Wii U Pro.',
        DE: 'Steuerung im Wii U Pro Controller-Modus',
        ES: 'Los controles están en modo Mando Pro de Wii U.',
        IT: 'I comandi sono in configurazione Wii U Pro Controller.',
        NL: 'Je gebruikt nu de Wii U Pro Controller-combinatie.',
    },
}
TEXT = {
    4046: {  # "Communications with the Wii Remote have been interrupted."
        EN: 'Communications with the controller have been interrupted.',
        FR: 'La connexion avec la manette a été interrompue.',
        DE: 'Die Verbindung zum Controller wurde unterbrochen.',
        ES: 'Se ha interrumpido la conexión con el mando.',
        IT: 'Il collegamento con il controller si è interrotto.',
        NL: 'De verbinding met de controller is verbroken.',
    },
}

src, dst = sys.argv[1:3]
d = open(src, 'rb').read()
o = 0


def u32():
    global o
    v, = struct.unpack('>I', d[o:o + 4])
    o += 4
    return v


out = bytearray()
nl = u32()
out += struct.pack('>I', nl)
changed = 0
for _ in range(nl):
    lid, cnt = u32(), u32()
    extra = [(sid, t[lid]) for sid, t in sorted(ADD.items()) if lid in t]
    out += struct.pack('>II', lid, cnt + len(extra))
    for _ in range(cnt):
        sid, n = u32(), u32()
        s = d[o:o + n * 2].decode('utf-16-be')
        o += n * 2
        new = TEXT.get(sid, {}).get(lid)
        if new is not None and new != s:
            s = new
            changed += 1
        b = s.encode('utf-16-be')
        out += struct.pack('>II', sid, len(b) // 2) + b
    for sid, t in extra:
        b = t.encode('utf-16-be')
        out += struct.pack('>II', sid, len(b) // 2) + b
        changed += 1
out += d[o:]  # voice table and anything after it, unchanged
open(dst, 'wb').write(out)
print('loc_patch: %d texts replaced or added, %d -> %d bytes' % (changed, len(d), len(out)))
