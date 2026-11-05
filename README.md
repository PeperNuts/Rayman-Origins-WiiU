# Rayman Origins — Wii U port

A native Wii U port of **Rayman Origins** (Wii, Europe, `SOJP41`), running on a Wii U with Aroma.

The original Wii game code runs natively on the Wii U's CPU (Espresso is a Broadway descendant, paired
singles included). This project replaces the layer underneath it — the Wii SDK (OS, GX, VI, AX, DVD,
NAND, WPAD/KPAD, …) — with an implementation on top of Cafe OS, and links the two into a Wii U `.rpx`.

> **No game code or data is included, and none can be.** You need your own copy of the game. Everything
> derived from the disc (DOL, debug ELF, disassembly, the prepared game object, built `.rpx`/`.wuhb`
> files, installable titles, game files) is produced locally and is excluded by `.gitignore`. For the
> same reason the releases contain source code only.

## Features (release: first-boot 81)

- Full game: menus, levels, cutscenes, saves — rendered by the GPU (GX → GX2), at 60 fps in most areas
- **1.5× the Wii resolution** (960×792) by default; 2× or the original resolution via `port.cfg`
- **Controls:** Wii U GamePad, Pro Controller, Classic Controller and Wii Remote (+ Nunchuk). The
  GamePad and Pro Controller act as Classic Controllers with Rayman Legends-style buttons, rumble on the
  GamePad, and the game names and shows the controller in use (with the mods below)
- **Off-TV play:** the game is shown on the TV and on the GamePad
- **Sound:** game sound and music on TV and GamePad; video (Bink) sound since first-boot 81
- **Saves** like a Wii game (three slots), in the title's own save area when installed
- **HOME Menu** works (the game pauses and resumes)
- Wii Health & Safety screen skipped
- **Two ways to run it:** from the homebrew menu (`.rpx`/`.wuhb`, data on the SD card), or as an
  **installed title** on the Wii U Menu with its own icon, splash screen and boot sound and everything
  built in (no SD files needed)

## Requirements

- Your own Rayman Origins (Europe) disc image (`SOJP41`)
- A Wii U with [Aroma](https://aroma.foryour.cafe/)
- To build: devkitPro with `wiiu-dev` (wut), Python 3 with Pillow, Ninja, powerpc-eabi binutils
- `glslcompiler.rpl` from [CafeGLSL](https://github.com/Exzap/CafeGLSL) (compiles the shaders at start-up)
- Optional, for an installable title: Java, [NUSPacker](https://github.com/Maschell/nuspacker),
  [WUP Installer GX2](https://github.com/xhp-creations/WUP-Installer-GX2), ffmpeg, and your own Wii U
  common key (not included)

## Building

1. **Game object** — turn your disc into `game_final.o`: see [`prep/README.md`](prep/README.md)
   (decomp project in `decomp/`, then `prep/make_game_object.sh`).
2. **Disc files** — copy the disc's `files/` folder out of your image (`prep/extract_disc_files.sh`).
3. **RPX** — `port/tools/build.sh` builds `rayman.rpx` from `game_final.o` and `port/src`.
4. **Optional `.wuhb`** (homebrew menu with icon and splash screen) — `wuhbtool rayman.rpx rayman.wuhb
   --name="Rayman Origins" --icon=… --tv-image=… --drc-image=…`; `port/tools/make_wuhb_images.py`
   makes the three images from a square and a wide picture.

### Mods (controller texts and pictures)

The port reads files in `mods/` instead of the disc's files with the same path:

| File | Made with | What it does |
|---|---|---|
| `mods/localisation/localisation.loc` | `port/tools/loc_patch.py <disc>/localisation/localisation.loc <out>` | "Wii U GamePad" / "Pro Controller" texts in all six languages |
| `mods/icons/gamepad.bin`, `mods/icons/pro.bin` | `port/tools/make_icon_bin.py <picture> <out.bin>` | the controller pictures shown in the menus (any picture; `makeicon.py` / `makeicon_pro.py` draw simple ones) |

## Installing

### A. From the homebrew menu (SD card)

Copy to the SD card:

```
sd:/wiiu/apps/rayman.rpx            (or rayman.wuhb)
sd:/wiiu/libs/glslcompiler.rpl
sd:/rayman/files/...                the disc's files/ folder
sd:/rayman/mods/...                 optional, see Mods
```

Start it from the Wii U Menu (Aroma's homebrew launcher). Saves go to `sd:/rayman/save/`.

### B. As an installed title (Wii U Menu icon, no SD files needed)

1. Assemble the title folder with your own files and pictures:

   ```
   port/tools/make_title.sh title rayman.rpx glslcompiler.rpl <disc files/> <mods/> icon.png splash.png [logo.png] [sound.mp3]
   ```

   `icon.png` is square (→ 128×128 icon), `splash.png` wide (→ TV and GamePad boot screens),
   `logo.png` is shown in the top-left of the boot screen, and the first 8 seconds of `sound.mp3`
   play while it starts. The game files and mods are built in (`/vol/content`), and so is the shader
   compiler (`code/`).

2. Pack it with NUSPacker and your own common key:

   ```
   java -jar NUSPacker.jar -in title -out "Rayman Origins" -encryptKeyWith <your Wii U common key>
   ```

3. Copy the `Rayman Origins` folder to `sd:/install/` and install it with WUP Installer GX2 (USB or
   internal memory; about 3.5 GB). Installing a newer build over it replaces it (title ID
   `0005000010524F31`). Aroma must be running to start it, as for any homebrew title.

An installed title saves in its own save area, not on the SD card.

## Controls (GamePad / Pro Controller)

| Button | Action |
|---|---|
| B | jump / glide (hold) |
| Y | attack |
| ZR | sprint |
| Left stick / D-pad | move |
| + | pause |
| L3 + R3 | GamePad: switch between the game and the log |

## `port.cfg` options

An optional `sd:/rayman/port.cfg` (also read by an installed title when an SD card is in) holds words
separated by spaces or new lines:

| Option | Effect |
|---|---|
| `debug` | on-screen status bar on the TV and the log on the GamePad during start-up |
| `efb1x` / `efb2x` | Wii resolution / 2× the Wii resolution (default 1.5×) |
| `nosave` | no saving |
| `nohome` | HOME button does nothing |
| `noaudio`, `nobinksound` | no sound / no video sound |
| `padswap` | swaps A↔B and X↔Y (Xbox-style button positions) |
| `padnunchuk` | GamePad acts as a Wii Remote + Nunchuk instead of a Classic Controller |
| `tiledtex`, `bc1`, `nosmalltex` | texture storage experiments (`bc1` slows the GPU down on some screens) |
| `efbmem2`, `nosnapshot`, `oldhash`, `flipwait`, `whbwait`, `nopipeline` | renderer fallbacks for testing |

## Logs

`python port/tools/udplog.py` on a PC on the same network shows the live log (UDP port 4405). Crashes show a
report on screen and in the log; HOME Menu steps are also written to `sd:/rayman/home_log.txt`.

## Known issues

- Video sound (first-boot 81) and the installed-title mode are new and have had little testing.
- The busiest zoomed-out scenes can dip below 60 fps at 1.5× (`efb1x` if it bothers you).
- `bc1` (compressed textures) makes the GPU very slow on some screens, so it is off by default.

## How it works

1. **Decomp project** (`decomp/`, decomp-toolkit) splits `main.dol` into 1,305 objects using the symbol
   map from the debug ELF on the disc. See `docs/decomp.md` and `docs/rayman_notes.md`.
2. **Game object** (`prep/`): the 1,059 portable objects (game, UbiArt engine, MSL runtime, zlib, Bink
   core) are partially linked into one object. Privileged GQR writes become `nop`, reads of the Wii bus
   clock read `wii_OSBusClock`, writes to the GPU FIFO go to a RAM window; every reference into the Wii
   SDK is renamed `wii_<name>`.
3. **Port layer** (`port/src`) provides those `wii_*` functions:
   - `wii_os.c` — memory arenas and heaps, threads, mutexes, alarms, time, interrupts, cache
   - `wii_dvd.c` — `DVD*` on files from the SD card or the title's content, with `mods/` overrides
   - `wii_nand.c` — the Wii NAND file system (saves) on the SD card or the title's save area
   - `wii_vi.c` — 60 Hz retrace clock; `wii_gx.c` — GX completion signals
   - `gx_core.c` — GX front end on its own core: FIFO stream, display lists, vertex formats, TEV and
     texture state → frame packets
   - `gx_render.c`, `gx_tex.c` — GX2 back end: EFB render target in MEM1, TEV shaders compiled by
     CafeGLSL, indirect texturing, Wii texture decoding and cache, EFB→texture copies, TV and GamePad
   - `wii_ax.c` — software AX mixer (DSP-ADPCM/PCM voices) and Bink sound driver → sndcore2
   - `wii_kpad.c` — Wii Remotes, Classic Controllers, GamePad and Pro Controller
   - `wii_rad.c` — RAD Game Tools threading used by Bink
   - `loc_hook.c` — controller-specific texts
   - `main.c`, `watch.c`, `screen.c` — boot, HOME Menu, crash report, watchdog, network log
4. **Linking** (`port/rayman.ld`, `port/tools/build.sh`): wut's linker script with separate small-data
   sections; post-link patches (`wgp_patch.py`, `game_patch.py`, `neutralize_sda.py`); `elf2rpl` makes
   the `.rpx`.

## Repository layout

```
decomp/        decomp-toolkit project: configure.py, config/SOJP41 (config, splits, symbols)
prep/          game-object preparation scripts
port/src/      Wii SDK replacement layer + boot code
port/tools/    build script, post-link patchers, mod tools, title tools
port/title/    app.xml, cos.xml, meta.xml of the installable title
port/rayman.ld linker script (wut.ld + small-data sections)
docs/          decomp notes, SDK boundary report, hardware findings
```

The title's `cos.xml`/`app.xml`/`meta.xml` are based on WUP Installer GX2's channel files.
