#!/bin/bash
# Rebuild game_final.o from the decomp build: patch asm, assemble patched kept units,
# partial link, isolate symbols. Paths: ~/proj (decomp), ~/port (work), ~/tools/binutils.
set -euo pipefail
B=~/tools/binutils; P=~/proj; W=~/port
cd $W
rm -rf patched_asm patched_obj && mkdir -p patched_obj
python3 scripts/patch_asm.py $P/build/SOJP41/asm patched_asm | tail -1
python3 - <<'PY'
import os
W=os.path.expanduser('~/port'); P=os.path.expanduser('~/proj'); OBJ=P+'/build/SOJP41/obj'
exec(open(os.path.expanduser('~/scripts/boundary.py')).read().split("gof={u:group(u)")[0])
kept=[u for u in units if not group(u) and u!='__ppc_eabi_init.cpp']
patched=set(f[:-2] for f in os.listdir(W+'/patched_asm'))
pk=[os.path.splitext(u)[0] for u in kept if os.path.splitext(u)[0] in patched]
open(W+'/patched_kept.txt','w').write('\n'.join(pk)+'\n')
out=[(W+'/patched_obj/'+os.path.splitext(u)[0]+'.o') if os.path.splitext(u)[0] in patched else OBJ+'/'+os.path.splitext(u)[0]+'.o' for u in kept]
out+=[OBJ+'/'+f for f in sorted(os.listdir(OBJ)) if f.startswith('auto_')]
open(W+'/objs_patched.txt','w').write('\n'.join(out)+'\n')
print(len(pk),'patched kept units;',len(out),'objects')
PY
for u in $(cat patched_kept.txt); do $B/powerpc-eabi-as -mgekko -I $P/build/SOJP41/include patched_asm/$u.s -o patched_obj/$u.o 2>&1 | grep -i error || true; done
$B/powerpc-eabi-ld -r -d -T game_r.ld -o game_r.o @objs_patched.txt
$B/powerpc-eabi-nm -u game_r.o | awk '{print $2}' | sort -u > undef.txt
$B/powerpc-eabi-nm game_r.o | awk '{print $NF}' | grep '@' | sort -u | awk '{n=$0; gsub("@","$",n); print $0" "n}' > at_map.txt
awk '$1 !~ /^wii_/ {print $1" wii_"$1}' undef.txt > undef_map.txt
cat at_map.txt undef_map.txt > rename1.txt; echo "main rayman_main" >> rename1.txt
$B/powerpc-eabi-objcopy --redefine-syms=rename1.txt game_r.o game_s1.o
printf 'rayman_main\n__game_text_start\n__game_text_end\n__game_extab_start\n__game_extab_end\n__game_extabindex_start\n__game_extabindex_end\n__game_ctors_start\n__game_ctors_end\n' > keep.txt
$B/powerpc-eabi-objcopy --keep-global-symbols=keep.txt game_s1.o game_final.o
echo "undefined: $($B/powerpc-eabi-nm -u game_final.o | wc -l); not wii_: $($B/powerpc-eabi-nm -u game_final.o | awk '{print $2}' | grep -vc '^wii_' || true)"
