# Write-gather-pipe capture (see tools/wgp_patch.py): trampoline area filled in after the
# link, and the stream pointer / scratch slot the trampolines address through r13.
    .section .text.wgp_tramp, "ax"
    .align 5
    .global port_wgp_tramp
port_wgp_tramp:
    .space 72 * 1024
    .global port_wgp_tramp_end
port_wgp_tramp_end:

    .section .sdata, "aw"
    .align 2
    .global port_wgp_ptr
port_wgp_ptr:
    .long 0
    .global port_wgp_save
port_wgp_save:
    .long 0
    .global port_wgp_save2
port_wgp_save2:
    .long 0
    .global port_wgp_tmp
port_wgp_tmp:
    .long 0
