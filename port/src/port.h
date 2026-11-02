// Rayman Origins Wii U port - shared declarations.
#pragma once
#include <coreinit/thread.h>
#include <stddef.h>
#include <stdint.h>

// trampoline.S
void game_set_sda(void);
uint32_t game_call(void *fn, uint32_t a0, uint32_t a1, uint32_t a2);
uint32_t read_r2(void);
uint32_t read_r13(void);

// wii_os.c
void port_mem_init(void);
void port_register_boot_thread(OSThread *cafe);
int32_t stub_hit(const char *name, int32_t ret);
uint32_t trace_calls(void);
uint32_t trace_distinct(void);
void trace_report(void (*line)(const char *), uint32_t width);
void trace_summary(char *buf, size_t size, uint32_t nfirst, uint32_t nlast);

// wii_dvd.c
void port_dvd_init(void);
uint32_t port_dvd_files(void);
const char *port_dvd_file_name(uint32_t i);

// wii_vi.c
void port_vi_init(void);
uint32_t port_vi_retrace_count(void);

// watch.c
void port_watch_start(void);

// main.c
int port_stage(void);
const char *port_version(void);
OSThread *port_boot_thread(void);

// wii_os.c (threads)
int port_game_threads(OSThread **out, int max);

// game_final.o
int rayman_main(int argc, char **argv);
extern char __game_text_start[], __game_text_end[];
extern void (*__game_ctors_start[])(void);
extern void (*__game_ctors_end[])(void);

// linker script
extern char _SDA_BASE_[], _SDA2_BASE_[];
