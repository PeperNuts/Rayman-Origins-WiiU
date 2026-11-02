"""Wii U port: patch dtk disassembly of the few game units that touch Wii-only CPU/OS state.
 - GQR writes (privileged on Cafe OS; values equal Cafe defaults)  -> nop
 - bus clock reads from 0x800000F8 (unmapped on Wii U)              -> load wii_OSBusClock
Usage: patch_asm.py <dtk asm dir> <out dir>
"""
import re, sys, os
src, out = sys.argv[1], sys.argv[2]
os.makedirs(out, exist_ok=True)
INS = re.compile(r'^(/\*[^*]*\*/\s*)(\S+)\s*(.*)$')
report = []
patched_units = []
for fn in sorted(os.listdir(src)):
    if not fn.endswith('.s'): continue
    text = open(os.path.join(src, fn)).read()
    if 'GQR' not in text and not re.search(r'lis r\d+, 0x(8000|cc01)$', text, re.M | re.I):
        continue
    L = text.split('\n'); changed = 0
    for i, line in enumerate(L):
        m = INS.match(line)
        if not m: continue
        pre, op, args = m.groups()
        if op == 'mtspr' and args.startswith('GQR'):
            L[i] = pre + 'nop  # wiiu: was mtspr ' + args; changed += 1; continue
        if op == 'lis' and re.fullmatch(r'r\d+, 0xcc01', args, re.I):
            # Wii GPU write-gather pipe (stores to 0xCC008000): point at a RAM window instead.
            rx = args.split(',')[0]
            L[i] = pre + f'lis {rx}, wii_gx_wgp_window@ha  # wiiu: was lis {rx}, 0xcc01'
            changed += 1
            continue
        if op == 'lis' and re.fullmatch(r'r\d+, 0x8000', args):
            rx = args.split(',')[0]
            # find the 0xF8 load from rx within the next 6 instructions
            for j in range(i + 1, min(i + 8, len(L))):
                mj = INS.match(L[j])
                if not mj: continue
                if mj.group(2) == 'lwz' and re.fullmatch(r'(r\d+), 0xf8\(' + rx + r'\)', mj.group(3), re.I):
                    rd = mj.group(3).split(',')[0]
                    # rx must not be read again after the lwz unless rewritten first
                    safe, k = True, j + 1
                    while k < len(L):
                        mk = INS.match(L[k])
                        if mk:
                            o, a = mk.group(2), mk.group(3)
                            ops = [t.strip() for t in re.split(r'[,()]', a) if t.strip()]
                            if o in ('blr', 'b', 'bctr') or o.startswith('.'): break
                            if rx in ops[1:] or (o.startswith('st') and rx in ops): safe = False; break
                            if ops and ops[0] == rx: break   # rx overwritten
                        elif L[k].startswith('.L_') or L[k].startswith('.endfn'): break
                        k += 1
                    if rd == rx: safe = True
                    report.append(f"{fn}:{i+1} {rx}->{rd} {'OK' if safe else 'REUSED'}")
                    if safe:
                        L[i] = pre + f'lis {rx}, wii_OSBusClock@ha  # wiiu: was lis {rx}, 0x8000'
                        L[j] = INS.match(L[j]).group(1) + f'lwz {rd}, wii_OSBusClock@l({rx})  # wiiu: was 0xf8'
                        changed += 1
                    break
                if mj.group(3).split(',')[0].strip() == rx: break
    if changed:
        open(os.path.join(out, fn), 'w').write('\n'.join(L))
        patched_units.append((fn, changed))
print('\n'.join(report))
print('patched:', patched_units)
