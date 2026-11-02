# Rayman Origins (Wii) — Decompilation

Matching decompilation project for **Rayman Origins (Europe), game ID `SOJP41`**, built on
[decomp-toolkit](https://github.com/encounter/decomp-toolkit) / [dtk-template](https://github.com/encounter/dtk-template).

**Status:** the project splits `main.dol` into 1,305 objects and links it back **byte-for-byte identical**
to the original. Progress: 0% — every object is still assembly. Your job from here is to replace them with C/C++.

| | Units | Functions | Code |
|---|---|---|---|
| Game / UbiArt engine (`.cpp`) | 992 | 25,826 | 5.8 MB |
| SDK / middleware (`.c`) | 313 | 2,526 | 0.76 MB |

## The big advantage: a full symbol map

The disc contains `files/wii_ITF_Techno_f.elf`, an **unstripped build of `main.dol`** (verified byte-identical).
It provides:

- **40,059 symbols**, with full CodeWarrior-mangled C++ names (classes, methods, templates, vtables).
- **Source file names** for every compilation unit (`AABB.cpp`, `Ray_PlayerControllerComponent.cpp`, …).
- The original relocation tables.

`config/SOJP41/splits.txt` and `symbols.txt` were generated from it with `dtk elf config`. Most decomp projects
have to guess all of this by hand.

## Requirements

- Python 3.8+
- Ninja (`pip install ninja`, or put `ninja.exe` on PATH)
- Windows: nothing else. Linux/macOS: wibo is downloaded automatically (see notes below).

`configure.py` downloads dtk, objdiff, binutils and the CodeWarrior compilers into `build/` on first run.

## Building

```sh
cd decomp
python configure.py
ninja
```

A successful build ends with `build/SOJP41/main.dol: OK` and a progress report. The original files live in
`orig/SOJP41/` (git-ignored; never commit them).

## Decompiling a file

1. Open **objdiff** (`build/tools/objdiff` or the [GUI release](https://github.com/encounter/objdiff/releases)),
   point it at this folder, and pick a unit.
2. Create the source file under `src/` (e.g. `src/AABB.cpp`).
3. In `configure.py`, find the unit's `Object(NonMatching, "AABB.cpp")` line. Leave it NonMatching while working.
4. Write C++ until objdiff shows 100% for every function, then switch it to `Object(Matching, ...)` and run
   `ninja`. The DOL must still pass the SHA-1 check.

Good first targets: small math/container units in the ITF core (`AABB.cpp`, `Vec2d`, `Color`, string helpers).
The SDK units (`OS.c`, `GX*.c`, …) are mostly already matched in other public Wii decomps and can often be borrowed.

## Project-specific notes

See `docs/rayman_notes.md` for the fixes that were needed to get a matching link, and the compiler findings.
