# Rayman Origins — Wii U port

An experimental native Wii U port of **Rayman Origins** (Wii, Europe, `SOJP41`).

The original Wii game code runs natively on the Wii U's CPU (Espresso is a Broadway descendant, paired
singles included). This project replaces the layer underneath it — the Wii SDK (OS, GX, VI, AX, DVD,
NAND, WPAD/KPAD, …) — with an implementation on top of Cafe OS, and links the two into a Wii U `.rpx`.

> **No game code or data is included.** You need your own copy of the game. Everything derived from the
> disc (DOL, debug ELF, disassembly, the prepared game object, built `.rpx` files, game files) is
> produced locally and is excluded by `.gitignore`.

## Status

| Milestone | State |
|---|---|
| Matching decomp setup (dtk) — rebuilds `main.dol` byte-for-byte | ✅ |
| Symbol map from the unstripped `wii_ITF_Techno_f.elf` on the disc (40,059 symbols) | ✅ |
| Wii U hardware probes (clock, small data r2/r13, paired singles, GQRs, memory) | ✅ — see `docs/hardware_findings.md` |
| Game code linked into an RPX, static constructors and `main()` run on hardware | ✅ |
| Disc access from SD card (`DVD*`), threads, mutexes, alarms, heaps | ✅ first version |
| 60 Hz frame clock (`VIWaitForRetrace`, retrace callbacks) | ✅ |
| Wii blocking primitives safe on Cafe OS (no scheduler calls with interrupts off) | ✅ |
| Game main loop running (boot splash screen) | ✅ first-boot 7 |
| Direct GPU writes (`0xCC008000`) redirected to a RAM window | ✅ runs stably in the main loop |
| Wii Remote + Nunchuk via the Wii U's KPAD/WPAD (padscore) | ✅ connects (first-boot 10) |
| GX FIFO-breakpoint / draw-sync completion signals (render loop at 60 fps) | ✅ past Health & Safety to the splash screen |
| RAD/Bink threading (rrThread/rrMutex/rrSemaphore/atomics) for intro videos | ✅ |
| Wii Remote input drives the game: Health & Safety → main menu → prologue level | ✅ first-boot 13 |
| Main thread stall after the intro video starts (Bink IO suspended the main thread) | ✅ first-boot 15 — runs into the prologue level |
| Split screens: TV status page, GamePad log | ✅ first-boot 16 |
| GX2 renderer stage 1: GX2 on CPU2, TV shows the game's copy-clear colour, GamePad log | ✅ first-boot 17 |
| GX emulation: FIFO capture, vertex decoding, TEV ubershader (CafeGLSL), textures, EFB copies | 🔧 runs without hangs since first-boot 27; first visible output pending |
| Graphics (GX → GX2), audio (AX), input (WPAD/KPAD → Wii U), saves (NAND) | ⏳ |

With a Wii Remote, the game goes from Health & Safety through the main menu into the prologue level and starts the intro video (no video output yet).

## How it works

1. **Decomp project** (`decomp/`, decomp-toolkit) splits `main.dol` into 1,305 objects using the symbol
   map from the debug ELF. See `docs/decomp.md` and `docs/rayman_notes.md`.
2. **Game object**: the 1,059 portable objects (game, UbiArt engine, MSL runtime, zlib, Bink core) are
   partially linked into one object. Two Wii-only details are patched in the disassembly: the 10
   privileged `mtspr GQR` writes become `nop` (Cafe's per-thread defaults are identical), and the 25 reads
   of the Wii bus clock at `0x800000F8` read `wii_OSBusClock` instead. All game symbols are made local
   except the entry points; every reference into the Wii SDK is renamed `wii_<name>`.
3. **Port layer** (`port/src`) provides those `wii_*` functions:
   - `wii_os.c` — memory arenas and OS heaps, threads (Wii `OSThread` structs mapped to Cafe threads,
     all pinned to one core like the single-core Wii), mutexes, condition variables, alarms, time,
     interrupts, cache, system settings, logging
   - `wii_dvd.c` — `DVD*` on top of files in `sd:/rayman/files`, keeping Wii struct layouts and
     asynchronous callbacks (the game's file layer depends on both)
   - `wii_vi.c` — 60 Hz retrace clock
   - `wii_gx.c` — GX completion signals (FIFO breakpoint callback, draw-sync tokens)
   - `gx_core.c` — GX front end: the FIFO stream (see `tools/wgp_patch.py`), display lists, vertex
     formats/arrays/matrices, colour channels, TEV/blend/texture state → frame packets
   - `gx_render.c`, `gx_tex.c` — GX2 back end on CPU2: 2× EFB render target, a TEV ubershader compiled
     at start-up by [CafeGLSL](https://github.com/Exzap/CafeGLSL) (`sd:/wiiu/libs/glslcompiler.rpl`),
     Wii texture decoding and cache, EFB→texture copies, scaled output to the TV
   - `wii_rad.c` — RAD Game Tools threading used by Bink (threads, mutexes, semaphores, atomics)
   - `wii_kpad.c` — Wii Remotes through the Wii U's own KPAD/WPAD (same `KPADStatus` layout)
   - `port/tools/gen_stubs.py` — everything else: copies self-contained Wii functions verbatim, copies data
     tables, and generates tracing stubs for the rest
   - `trampoline.S` — sets the game's small-data registers (r13/r2) on every entry into game code
   - `watch.c`, `main.c` — boot, crash screen with game-relative addresses and call stack, watchdog
4. **Linking** (`port/rayman.ld`, `port/tools/build.sh`): wut's linker script with separate
   `.sdata`/`.sdata2` sections so GNU ld can resolve CodeWarrior's small-data relocations; those
   relocations are then neutralised (`neutralize_sda.py`) so the Cafe loader leaves them alone; `elf2rpl`
   produces the `.rpx`.

## Requirements

- Your own Rayman Origins (Europe) disc image (`SOJP41`)
- devkitPro with `wiiu-dev` (wut), Python 3, Ninja
- A Wii U with Aroma

## Running a build

1. Extract the disc's `files/` folder to `sd:/rayman/files/` on the Wii U SD card.
2. Copy the built `.rpx` to `sd:/wiiu/apps/` and launch it from the Wii U Menu.
3. Optional: `python udplog.py` on a PC on the same network shows the live log (UDP port 4405).

See `prep/README.md` for building the game object from your disc, and `port/tools/build.sh` for the RPX.

## Repository layout

```
decomp/        decomp-toolkit project: configure.py, config/SOJP41 (config, splits, symbols)
prep/          game-object preparation scripts (classification, patching, partial link)
port/src/      Wii SDK replacement layer + boot code
port/tools/    stub generator, SDA relocation fix-up, build script
port/rayman.ld linker script (wut.ld + small-data sections)
docs/          decomp notes, SDK boundary report (+ per-symbol list), hardware findings
```
