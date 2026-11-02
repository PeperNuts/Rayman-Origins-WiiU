# Wii U port — SDK boundary report

Approach: keep the game's compiled code (the split objects), drop the Wii platform layer, and write a
replacement for each platform function the game calls. This report measures that boundary.
Regenerate it after `ninja` with `python tools/port/boundary.py && python tools/port/boundary2.py && python tools/port/scan.py`
from the project root. The full per-symbol list is in `docs/port_boundary_symbols.md`.

## Units

- **1,059 kept** — the game, UbiArt engine, MSL C/C++ runtime, math, zlib, Bink decoder core, MEM heaps.
- **246 replaced** — Wii SDK (OS, GX, VI, AX/DSP, DVD, NAND/FS/IPC, SC, WPAD/KPAD, SI/EXI/PAD),
  Bluetooth stack (69), MetroTRK debugger (26), HOME Menu (8), NW4R layout/font (23), Bink's Wii I/O glue (5).

## Functions the replacement layer must provide (302)

| Library | Functions | Data | Wii U target |
|---|---|---|---|
| GX graphics | 95 | 6 | GX2 (shader-based) |
| OS / CPU | 60 | 2 | coreinit |
| AX / DSP audio | 27 | 0 | Wii U AX (sndcore2) |
| WPAD / KPAD Wiimote | 23 | 0 | padscore (KPAD/WPAD) + VPAD |
| Bink Wii glue (threads, mutexes) | 18 | 1 | coreinit threads |
| NAND / FS / IPC | 16 | 1 | FS / save dir |
| VI video | 14 | 0 | GX2 scan buffers |
| HOME Menu | 13 | 0 | no-op (system HOME Menu) |
| DVD disc | 9 | 0 | FS (content / SD) |
| SC settings | 7 | 0 | system config (coreinit UC*) |
| NW4R layout/font | 6 | 0 | small helpers, reimplement |
| MetroTRK console I/O | 3 | 0 | stub or logging |

Callers: GX is reached from just 14 units (the `GFXAdapter_WII*` family plus video/splash code), DVD from 3,
VI from 5, WPAD/KPAD from 9, AX from 10. OS functions are the exception (51 units: threads, time, mutexes),
which is fine because they map almost one-to-one onto Cafe OS coreinit.

## Linker-folded bodies

CodeWarrior merged identical functions, so some SDK-named symbols are really shared trivial bodies used by
hundreds of game units (e.g. `rrThreadSpinHyperYield`, 4 bytes, 758 units; `ARCClose`, 441 units;
`nw4hbm::ut::Color::Color()`, 386 units). These 21 bodies must be kept or re-provided under the same name,
even though their nominal owner is replaced.

## Code-level scan of the kept units

| Check | Result |
|---|---|
| Privileged instructions (mfmsr/mtmsr/rfi) | only `__ppc_eabi_init.cpp` (startup, replaced) |
| Hardware registers (0xCC00/0xCD00/0xCD80) | none |
| Locked cache (`dcbz_l`), cache invalidate (`dcbi`) | none |
| BAT/HID/DMA/SRR special registers | none |
| GQR writes (paired-single quantisation) | 10, in `itf_main_WII.cpp` and `MEM_manager_WII.cpp`; verify these are allowed in Wii U user mode, else route through a shim |
| Paired-single instructions | 60,343 in 602 units — native on Espresso |
| OS low-memory reads | 25, all `0x800000F8` (bus clock, for tick→ms conversion) in 10 units; must be patched to a variable (Wii U bus clock differs and the address is unmapped) |

## Still to verify on hardware (milestone 3)

- Small-data registers r2/r13: CodeWarrior code addresses `.sdata`/`.sdata2` through them. The RPX
  must give the game these bases, or the relocations must be rewritten to absolute addressing.
- CodeWarrior C++ runtime inside a GCC-linked RPX: static constructors (`.ctors$10`), `extab` exception tables.
- Memory: the game sizes heaps from the Wii's MEM1/MEM2 arenas; map them onto Wii U heaps.
