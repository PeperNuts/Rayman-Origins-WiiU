#!/bin/bash
# Rayman Origins Wii U port - build rayman.rpx from game_final.o + port sources.
# Toolchain: clang (C/asm, powerpc eabi), GNU binutils 2.42 (powerpc-eabi), elf2rpl (wut-tools),
# libraries and headers copied from the user's devkitPro install (dkp/).
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD
DKP=$ROOT/dkp
BIN=$ROOT/binutils
GCCLIB=$DKP/devkitPPC/lib/gcc/powerpc-eabi/16.2.0
OUT=$ROOT/build
mkdir -p "$OUT" gen

python3 -I tools/gen_stubs.py game/undef.txt game/undef_info.json game/config/SOJP41/symbols.txt \
    game/orig/SOJP41/sys/main.dol src gen/stubs.S

CFLAGS=(--target=powerpc-unknown-eabi -mcpu=750 -mhard-float -O2 -g -fno-pic -fno-common
        -ffunction-sections -fno-builtin-printf -Wall -Werror=implicit-function-declaration
        -nostdinc -isystem "$(clang -print-resource-dir)/include"
        -isystem "$DKP/wut/include" -isystem "$DKP/devkitPPC/powerpc-eabi/include"
        -D__WIIU__ -D__WUT__ -DESPRESSO -Isrc)

objs=()
for f in src/*.c src/*.S gen/stubs.S; do
    o=$OUT/$(basename "${f%.*}").o
    clang "${CFLAGS[@]}" -c "$f" -o "$o"
    objs+=("$o")
done

"$BIN/powerpc-eabi-ld" -T rayman.ld --gc-sections --emit-relocs --no-eh-frame-hdr -z nocopyreloc \
    -Map "$OUT/rayman.map" -o "$OUT/rayman.elf" \
    "$GCCLIB/ecrti.o" "$GCCLIB/crtbegin.o" \
    "${objs[@]}" game/game_final.o \
    "$DKP/wut/lib/libwut.a" "$GCCLIB/libgcc.a" \
    --start-group "$DKP/devkitPPC/powerpc-eabi/lib/libsysbase.a" "$DKP/devkitPPC/powerpc-eabi/lib/libc.a" "$DKP/devkitPPC/powerpc-eabi/lib/libm.a" --end-group \
    "$GCCLIB/libgcc.a" "$GCCLIB/crtend.o" "$GCCLIB/ecrtn.o"

# GX FIFO stores -> stream trampolines (gx_core.c parses the stream)
python3 -I tools/wgp_patch.py "$BIN/powerpc-eabi-objdump" "$BIN/powerpc-eabi-nm" "$OUT/rayman.elf"

# Small-data instructions were resolved by ld against anchors that move together with the
# data, so the loader must not touch them: turn their relocations into R_PPC_NONE.
"$BIN/powerpc-eabi-objcopy" --strip-debug -R .note.GNU-stack "$OUT/rayman.elf" "$OUT/rayman_stripped.elf"
python3 -I tools/neutralize_sda.py "$OUT/rayman_stripped.elf" "$OUT/rayman_rpl.elf"

./elf2rpl "$OUT/rayman_rpl.elf" "$OUT/rayman.rpx"
ls -la "$OUT/rayman.rpx"
