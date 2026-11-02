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

// wii_gx.c
void port_gx_retrace(void);
uint32_t port_gx_breakpoints(void);
uint32_t port_gx_copy_clear(void);
uint32_t port_gx_copy_disp(void);
int port_vi_black(void);

// screen.c
void port_screen_init(void);
void port_screen_draw(void);
void port_screen_free(void);
void port_render_start(void);
void port_shaders_prepare(void);
int gx_describe(char *buf, int size);
int port_cfg(const char *word);
#define PORT_LOG_COLS 240
int port_log_snapshot(char out[][PORT_LOG_COLS], int rows);
void port_log_udp_start(void);
uint32_t port_log_count(void);
struct GX2Surface;
int port_make_surface(struct GX2Surface *s, uint32_t w, uint32_t h);
void port_fill(struct GX2Surface *s, uint32_t rgba);
void port_text(struct GX2Surface *s, int x, int y, int scale, const char *str, uint32_t fg);
void port_upload(struct GX2Surface *s);
void port_blit(struct GX2Surface *src, struct GX2Surface *dst, int x, int y);

// watch.c
void port_watch_start(void);

// main.c
int port_stage(void);
const char *port_version(void);
OSThread *port_boot_thread(void);

// wii_os.c (shared helpers)
void *port_map_get(const void *key, uint32_t kind, uint32_t size, int *created);
int port_irq_enable_for_block(void);
void port_irq_restore_after_block(int was);
enum { PORT_MAP_RR_MUTEX = 16, PORT_MAP_RR_SEM, PORT_MAP_RR_THREAD };

// wii_rad.c
int port_rad_threads(OSThread **out, int max);

// wii_os.c (threads)
int port_game_threads(OSThread **out, int max);

// game_final.o
int rayman_main(int argc, char **argv);
extern char __game_text_start[], __game_text_end[];
extern void (*__game_ctors_start[])(void);
extern void (*__game_ctors_end[])(void);

// linker script
extern char _SDA_BASE_[], _SDA2_BASE_[];
