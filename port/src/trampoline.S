// Rayman Origins Wii U port - entry/exit trampolines between Wii U code and game code.
//
// CodeWarrior game code addresses its small data through r13 (.sdata/.sbss) and r2
// (.sdata2/.sbss2). Cafe OS does not set these to the game's anchors, so every path
// that enters game code goes through one of these trampolines.
//
// _SDA_BASE_ / _SDA2_BASE_ are defined in rayman.ld inside the .sdata / .sdata2 output
// sections, so the loader relocates them together with the data they anchor.

    .section .text
    .align 2

// void game_set_sda(void)
// Sets r2/r13 to the game's anchors for the rest of the current thread. Used at the start
// of threads that run nothing but game code.
    .global game_set_sda
    .type game_set_sda, @function
game_set_sda:
    lis     13, _SDA_BASE_@ha
    addi    13, 13, _SDA_BASE_@l
    lis     2, _SDA2_BASE_@ha
    addi    2, 2, _SDA2_BASE_@l
    blr
    .size game_set_sda, .-game_set_sda

// uint32_t game_call(void *fn, uint32_t a0, uint32_t a1, uint32_t a2)
// Calls fn(a0, a1, a2) with the game's r2/r13, then restores the caller's r2/r13.
// Safe to use from any context (OS callbacks, alarm handlers, our own threads).
    .global game_call
    .type game_call, @function
game_call:
    stwu    1, -32(1)
    mflr    0
    stw     0, 36(1)
    stw     2, 8(1)
    stw     13, 12(1)
    mtctr   3
    mr      3, 4
    mr      4, 5
    mr      5, 6
    lis     13, _SDA_BASE_@ha
    addi    13, 13, _SDA_BASE_@l
    lis     2, _SDA2_BASE_@ha
    addi    2, 2, _SDA2_BASE_@l
    bctrl
    lwz     2, 8(1)
    lwz     13, 12(1)
    lwz     0, 36(1)
    mtlr    0
    addi    1, 1, 32
    blr
    .size game_call, .-game_call

// uint32_t read_r2(void), read_r13(void)  - diagnostics
    .global read_r2
    .type read_r2, @function
read_r2:
    mr      3, 2
    blr
    .size read_r2, .-read_r2

    .global read_r13
    .type read_r13, @function
read_r13:
    mr      3, 13
    blr
    .size read_r13, .-read_r13
