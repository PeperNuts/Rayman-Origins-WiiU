# Rayman Origins (SOJP41) — project notes

## Source of symbols

`orig/SOJP41/files/wii_ITF_Techno_f.elf` (from the disc's `files/` folder) is the unstripped linker output.
All loadable sections match `sys/main.dol` exactly; the only differences are the DOL's 32-byte section padding.
`config/SOJP41/{splits,symbols}.txt` came from `dtk elf config` on that ELF, then hand-fixed as below.

## Compiler

- The RVL SDK version strings say `0x4302_145`, which is **CodeWarrior Wii/1.3** (mwcceppc 4.3 build 145).
  The project defaults to `Wii/1.3` for compiling and linking.
- The game code may have been built with a different Wii compiler (1.3 – 1.7). If a function refuses to match
  despite correct logic, try another `mw_version` for that library in `configure.py`.
- Flags are the dtk-template defaults with `-enc SJIS`. Expect to tune them (e.g. `-O4,p`, `-inline`,
  `-Cpp_exceptions`, `-RTTI`) once the first UbiArt files start matching.

## Fixes applied to the generated config

1. **Empty ranges removed.** `dtk elf config` emitted 16 zero-length ranges, which the splitter rejects.
   Seven units became empty and were dropped (PhysBroadPhase.cpp, ProceduralPatch.cpp,
   Ray_AIMainLumFollowActorAction.cpp, ZSerializerObject.cpp, cwdemangle_WII.cpp, math_ppc.c, mem_list.c).
   They contribute no bytes to the DOL.
2. **Ranges clamped to section ends.** 13 ranges ran into alignment padding past their section.
3. **`zITF.cpp` removed.** Symbols without a FILE owner (linker tables and weak inline statics) were lumped
   into this pseudo-unit, which caused a link-order cycle. dtk now places that data in `auto_*` units.
4. **Windows paths.** MetroTRK's assembly units carried the original build machine's paths
   (`D:\Data\wiiProj\metrotrk\…`) and were renamed to `metrotrk/__exception.s` and `metrotrk/targsupp.s`.
5. **Name collisions.** `memory.c`, `locale.c` and `compress.c` collide with same-named `.cpp` files
   and were moved to `sdk/`.
6. **C++ runtime sections.** The Wii linker requires the ctor/dtor references in `__init_cpp_exceptions.cpp`
   under `.ctors$10`, `.dtors$10` and `.dtors$15` (via `rename:` in splits.txt).
7. **`__ArenaLo`.** The game reads `0x80749030`, which is `_db_stack_addr`, not rounded up to 32 bytes.
   `config/SOJP41/ldscript.tpl` is dtk's default linker script with `__ArenaLo = _db_stack_addr;`.

## Third-party code in the binary

- Nintendo RVL SDK (Aug 2010 builds).
- RAD Game Tools Bink video (`bink*.c`, `rad*.c`, `fft.c`, `dct.c`, …).
- Broadcom Bluetooth stack (`bta_*.c`, `btu_*.c`, `gki_*.c`).
- MetroTRK debugger, MSL C/C++ runtime.

## Toward the Wii U

A matching decomp gives you source that rebuilds the Wii executable. A native Wii U build is a separate
porting effort on top of it. CPU code mostly carries over (Espresso is a Broadway descendant), but the
platform layers must be rewritten against the Wii U SDK or a homebrew toolchain (devkitPPC + wut):

- Graphics: GX → GX2. Look for `GFXAdapter_WII` / the `*_WII.cpp` adapter units.
- Input: WPAD/KPAD → VPAD (GamePad) + KPAD.
- Audio: AX (similar API on Wii U), plus Bink and ADPCM streaming.
- File I/O: DVD → FS / content paths. The game data in `files/*.ipk` stays as-is.
- OS: threads, memory arenas and alarms map to Cafe OS (coreinit).

UbiArt isolates platform code behind `*Adapter_WII` classes (33 `_WII.cpp` units). Those are the files to
decompile first if the Wii U port is the goal.
