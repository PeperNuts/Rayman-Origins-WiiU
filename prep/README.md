# Game object preparation

These scripts turn your own disc into `game_final.o`, the object the port links against.
They are committed as used during development and still assume this layout
(to be parameterised):

| Path | Contents |
|---|---|
| `~/proj` | the decomp project (`decomp/` here) after `python configure.py && ninja`, with `orig/SOJP41/sys/main.dol` from your disc |
| `~/port` | working directory for the steps below |
| `~/tools/binutils` | powerpc-eabi binutils (e.g. encounter/gc-wii-binutils 2.42) |

`make_game_object.sh` runs steps 3-5 in one go.

## Steps

1. **Build the decomp** (`decomp/`): `python configure.py && ninja` must end with `main.dol: OK`.
   This produces the split objects in `build/SOJP41/obj` and disassembly in `build/SOJP41/asm`.
2. **Classify units** - `boundary.py` (kept vs. replaced Wii SDK units), `boundary2.py` (boundary symbols,
   linker-folded bodies), `scan.py` (privileged instructions, hardware addresses in kept code).
3. **Patch assembly** - `patch_asm.py <asm dir> <out dir>`: GQR writes become `nop`, reads of the Wii bus
   clock at `0x800000F8` read `wii_OSBusClock`, and the 111 `lis rX, 0xCC01` that address the GPU
   write-gather pipe (stores to `0xCC008000`) point at `wii_gx_wgp_window` instead. Assemble the patched units with `powerpc-eabi-as -mgekko`.
4. **Partial link** - `powerpc-eabi-ld -r -d -T game_r.ld -o game_r.o @objs.txt` over the kept objects
   (patched ones substituted) plus dtk's `auto_*` objects; `__ppc_eabi_init.o` is excluded.
5. **Isolate symbols** with `powerpc-eabi-objcopy`:
   - `--redefine-syms`: `@` -> `$` in CodeWarrior local names; every undefined `X` -> `wii_X`
     (skip names that already start with `wii_`); `main` -> `rayman_main`
   - `--keep-global-symbols`: only `rayman_main` and the `__game_*` section bounds stay global
6. **Classify undefined symbols** - `classify.py` + `classify2.py` produce `undef_info.json`, which
   `port/tools/gen_stubs.py` uses to copy self-contained Wii functions instead of stubbing them.

`extract_disc_files.sh` copies the disc's `files/` folder out of an RVZ with `dtk vfs cp`, in time-boxed
batches; put the result in `sd:/rayman/files/`.
