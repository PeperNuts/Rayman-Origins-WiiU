# Wii U hardware findings (probes v1 + v2, run on a retail Wii U under Aroma)

Source: `C:\dev\wiiu-port\hello` (probe app). Results from 2026-11-01.

| Item | Result | Consequence for the port |
|---|---|---|
| Bus clock | 248,625,000 Hz (timer 62,156,250 ticks/s); core 1,243,125,000 Hz | Wii is 243 MHz. Replace the 25 reads of `0x800000F8` (10 units) with a variable holding the Wii U value. |
| Small data | Loader sets r2 = `_SDA2_BASE_`, r13 = `_SDA_BASE_` (e.g. r2 10523000, r13 1050FDA4) | Same convention as CodeWarrior. Game `.sdata/.sdata2` can live in the RPX's small-data area. |
| Paired singles | `psq_l` + `ps_sum0` correct | The game's 60k paired-single instructions run natively. |
| GQR access | `mfspr/mtspr GQR` from an app **freezes the console** (privileged) | Must not execute the game's GQR writes. |
| Default GQRs (per thread) | GQR0..7 = 0, 0, 0x00040004, 0x00050005, 0x00060006, 0x00070007, 0, 0 | Identical to what the game sets (u8, u16, s8, s16 in GQR2..5). |
| Game GQR usage | psq ops use only qr0 (45,221), qr2 (55), qr3 (8), qr5 (1). GQR6 is written (0x0F070F07) but never used. | **Fix: NOP the 10 `mtspr GQR` instructions** in `itf_main_WII.cpp` and `MEM_manager_WII.cpp`. No context tricks needed. |
| Memory | largest malloc ~921 MiB; MEM1 frame heap 21.7 MiB; FG frame heap 40 MiB | Wii game used ~88 MB. Back its MEM1/MEM2 arenas with plain allocations. |

Notes:
- Probe v2 also confirmed `OSThread.context.gqr[]` is readable before a thread starts. Writing it is untested
  (the test value equalled the default) and no longer needed.
- Under Aroma, an `.rpx` that fails to load falls back to the real Health & Safety app. Seen during testing,
  caused by SD card corruption, not the app.
