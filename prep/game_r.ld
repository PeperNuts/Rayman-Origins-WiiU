/* Partial link of the Rayman Origins game objects for the Wii U port.
   Output section names are chosen so the final link can route them; small-data
   stays in sections literally named .sdata/.sbss/.sdata2/.sbss2 (GNU ld SDA21 rule). */
SECTIONS {
  .text : { __game_text_start = .; *(.init) *(.text) __game_text_end = .; }
  .rodata : { *(.rodata) }
  .game_extab : { __game_extab_start = .; *(extab) __game_extab_end = .; }
  .game_extabindex : { __game_extabindex_start = .; *(extabindex) __game_extabindex_end = .; }
  .game_ctors : { __game_ctors_start = .; KEEP(*(.ctors)) __game_ctors_end = .; }
  .sdata2 : { *(.sdata2) }
  .sbss2 : { *(.sbss2) }
  .data : { *(.data) }
  .sdata : { *(.sdata) }
  .sbss : { *(.sbss) }
  .bss : { *(.bss) *(COMMON) }
  /DISCARD/ : { *(.comment) *(.note.split) *(.ctors$10) *(.dtors$10) *(.dtors$15) *(.dtors) }
}
