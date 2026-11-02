// Rayman Origins Wii U port - stand-in for the Wii GPU write-gather pipe.
//
// Game code streams GX commands and vertex data by storing to 0xCC008000 (set up with
// "lis rX, 0xCC01" and stores at -0x8000(rX)). The asm patcher replaces each of those lis
// instructions with "lis rX, wii_gx_wgp_window@ha", so the stores land in this RAM window
// instead of faulting. @ha keeps only the upper half of the address, so the window is
// padded by 128 KiB on each side. Nothing reads the data yet (no graphics backend).
    .section .bss
    .align 6
wii_gx_wgp_low:
    .space 0x20000
    .global wii_gx_wgp_window
wii_gx_wgp_window:
    .space 0x20000
