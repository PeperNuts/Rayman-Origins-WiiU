// Rayman Origins Wii U port - watchdog on a separate core.
//
// Runs on CPU0 at high priority so it keeps working whatever the game does on CPU1:
//  - draws the log console
//  - every 5 s samples where each game thread is (suspend, read saved PC/LR, resume)
//  - if the game makes no progress (no new platform functions, no files opened) for 30 s,
//    shows an OSFatal screen with every game thread's location and call stack.
#include <coreinit/debug.h>
#include <coreinit/thread.h>
#include <coreinit/mutex.h>
#include <coreinit/time.h>
#include <whb/log.h>
#include <whb/log_console.h>
#include <proc_ui/procui.h>

#include <stdio.h>
#include <string.h>

#include "port.h"

static OSThread sWatch __attribute__((aligned(16)));
static uint8_t sWatchStack[0x10000] __attribute__((aligned(16)));

static uint32_t game_off(uint32_t addr)
{
   uint32_t s = (uint32_t)__game_text_start, e = (uint32_t)__game_text_end;
   return (addr >= s && addr < e) ? addr - s : 0xFFFFFFFFu;
}

// Formats " g+XXXX" or " XXXXXXXX" for an address.
static int fmt_addr(char *buf, size_t size, uint32_t a)
{
   uint32_t o = game_off(a);
   return (o != 0xFFFFFFFFu) ? snprintf(buf, size, " g+%X", (unsigned)o) : snprintf(buf, size, " %08X", (unsigned)a);
}

extern volatile uint32_t port_cpu1_sample[5];
extern volatile uint32_t port_cpu1_sample_frame;
extern volatile int port_in_frame_callback;

// Last place the frame interrupt found CPU1 (no thread suspension: that can deadlock when
// CPU1 runs with interrupts disabled).
static int describe_cpu1(char *buf, size_t size)
{
   int n = snprintf(buf, size, "cpu1@frame%u:", (unsigned)port_cpu1_sample_frame);
   for (int i = 0; i < 5; i++) {
      if (i == 2) n += snprintf(buf + n, size - n, " |");
      if (port_cpu1_sample[i]) n += fmt_addr(buf + n, size - n, port_cpu1_sample[i]);
   }
   if (port_in_frame_callback) n += snprintf(buf + n, size - n, " [in %s-retrace cb]", port_in_frame_callback == 1 ? "pre" : "post");
   return n;
}

// Every game thread's scheduler state and saved PC/LR, read without suspending anything
// (the saved context is current for every thread that is not running).
static int describe_threads(char *buf, size_t size)
{
   OSThread *threads[12];
   int count = port_game_threads(threads, 12), n = 0;
   for (int i = 0; i < count && n < (int)size - 64; i++) {
      OSThread *t = threads[i];
      const char *st = t->state == OS_THREAD_STATE_RUNNING ? "run" : t->state == OS_THREAD_STATE_READY ? "ready"
                     : t->state == OS_THREAD_STATE_WAITING ? "wait" : t->state == OS_THREAD_STATE_MORIBUND ? "dead" : "?";
      n += snprintf(buf + n, size - n, "%s %s p%d", t->name ? t->name : "?", st, (int)t->priority);
      n += fmt_addr(buf + n, size - n, t->context.srr0);
      n += fmt_addr(buf + n, size - n, t->context.lr);
      n += snprintf(buf + n, size - n, (i % 2) ? "\n" : "  ");
   }
   if (n && buf[n - 1] != '\n') n += snprintf(buf + n, size - n, "\n");
   return n;
}

// One line: thread name, priority, state and the game return addresses up its saved stack.
static void log_thread_stack(const char *tag, OSThread *t)
{
   char tl[220];
   const char *st = t->state == OS_THREAD_STATE_RUNNING ? "run" : t->state == OS_THREAD_STATE_READY ? "ready"
                  : t->state == OS_THREAD_STATE_WAITING ? "wait" : "other";
   int n = snprintf(tl, sizeof(tl), "%s p%d %s:", t->name ? t->name : "?", (int)t->priority, st);
   if (t->state == OS_THREAD_STATE_RUNNING) {
      WHBLogPrintf("%s %s (running, no saved stack)", tag, tl);
      return;
   }
   uint32_t sp = t->context.gpr[1];
   for (int d = 0; d < 16 && n < (int)sizeof(tl) - 12; d++) {
      if (sp < 0x10000000 || sp >= 0x50000000 || (sp & 3)) break;
      uint32_t next = *(uint32_t *)sp;
      if (next < 0x10000000 || next >= 0x50000000 || (next & 3)) break;
      uint32_t ra = *(uint32_t *)(next + 4);
      if (game_off(ra) != 0xFFFFFFFFu) n += fmt_addr(tl + n, sizeof(tl) - n, ra);
      sp = next;
   }
   WHBLogPrintf("%s %s", tag, tl);
}

static void log_line(const char *l) { WHBLogPrintf("  %s", l); }

uint32_t port_prof_take(uint32_t *keys, uint32_t *hits, uint32_t *lrs, int max, int *n, OSThread **thr,
                        uint32_t *thrHits, int *nthr);

// Where CPU1 spent the last 10 seconds.
static void prof_report(void)
{
   static uint32_t keys[64], hits[64], lrs[64], thrHits[24];
   static OSThread *thr[24];
   int n, nt;
   uint32_t total = port_prof_take(keys, hits, lrs, 64, &n, thr, thrHits, &nt);
   if (!total) return;
   extern volatile uint64_t port_vi_wait_ticks, port_vi_frames, port_vi_late;
   static uint64_t lw, lf, ll;
   uint64_t w = port_vi_wait_ticks, f = port_vi_frames, la = port_vi_late;
   if (f > lf)
      WHBLogPrintf("[prof] game frames %u in 10 s (%u late), waiting for retrace %.1f ms per frame", (unsigned)(f - lf),
                   (unsigned)(la - ll), OSTicksToMicroseconds(w - lw) / 1000.0f / (float)(f - lf));
   lw = w; lf = f; ll = la;
   WHBLogPrintf("[prof] CPU1 %u samples in 10 s", (unsigned)total);
   for (int i = 0; i < n; i++) {
      char a[24], b[24];
      fmt_addr(a, sizeof(a), keys[i] << 6);
      fmt_addr(b, sizeof(b), lrs[i]);
      WHBLogPrintf("[prof] %5.1f%%%s (lr%s)", hits[i] * 100.0f / total, a, b);
   }
}

static int watch_main(int argc, const char **argv)
{
   uint32_t ticks = 0, seconds = 0, lastDistinct = 0, lastFiles = 0, quiet = 0;
   uint32_t lastFrames = 0, frozen = 0;
   static char buf[1400];
   for (;;) {
      OSSleepTicks(OSMillisecondsToTicks(100));
      if (++ticks % 10) continue;
      seconds++;

      uint32_t distinct = trace_distinct(), files = port_dvd_files(), frames = port_vi_retrace_count();
      quiet = (distinct == lastDistinct && files == lastFiles) ? quiet + 1 : 0;
      frozen = (frames == lastFrames) ? frozen + 1 : 0;
      lastDistinct = distinct;
      lastFiles = files;
      lastFrames = frames;

      if (seconds % 10 == 0) prof_report();
      if (seconds <= 60 || seconds % 5 == 0) {
         describe_cpu1(buf, sizeof(buf));
         WHBLogPrintf("[watch] %us st%d calls %u funcs %u files %u frames %u gpu %u %s", (unsigned)seconds, port_stage(),
                      (unsigned)trace_calls(), (unsigned)distinct, (unsigned)files, (unsigned)frames,
                      (unsigned)port_gx_breakpoints(), buf);
         char g[200];
         gx_describe(g, sizeof(g));
         WHBLogPrintf("[watch] %s", g);
      }

      // Steady state is fine while frames advance: print what the game is doing instead.
      if (seconds % 15 == 0) {
         WHBLogPrintf("==== status at %us: %u frames, %u files ====", (unsigned)seconds, (unsigned)frames, (unsigned)files);
         char fl[160];
         int fn = 0;
         for (uint32_t i = 0; i < files; i++) {
            fn += snprintf(fl + fn, sizeof(fl) - fn, "%s ", port_dvd_file_name(i));
            if (fn > 110 || i + 1 == files) { WHBLogPrintf("[files] %s", fl); fn = 0; }
         }
         WHBLogPrintf("[funcs] name:calls, in first-call order (~60 per second = every frame)");
         trace_report(log_line, 110);
         // Where each game thread waits: saved PC/LR plus game return addresses up its stack.
         OSThread *threads[24];
         int count = port_game_threads(threads, 16);
         OSThread *rad[8];
         int nrad = port_rad_threads(rad, 8);
         for (int k = 0; k < nrad && count < 24; k++) { // RAD threads with a shadow are listed already
            int dup = 0;
            for (int j = 0; j < count; j++) dup |= threads[j] == rad[k];
            if (!dup) threads[count++] = rad[k];
         }
         for (int i = 0; i < count; i++) {
            log_thread_stack("[stack]", threads[i]);
            OSMutex *m = threads[i]->mutex; // the Cafe mutex this thread is blocked on
            if (threads[i]->state == OS_THREAD_STATE_WAITING && m && m->owner) {
               WHBLogPrintf("[lock]  %s waits for a mutex owned by %s", threads[i]->name ? threads[i]->name : "?",
                            m->owner->name ? m->owner->name : "(unnamed)");
               log_thread_stack("[owner]", m->owner);
            }
         }
      }

      const char *why = NULL;
      if (frozen == 5) why = "CPU1 stopped taking interrupts for 5 s (frame clock frozen)";
      if (why) {
         int n = snprintf(buf, sizeof(buf), "Rayman port %s - %s\nstage %d, %u calls, %u funcs, %u files, %u frames\n",
                          port_version(), why, port_stage(), (unsigned)trace_calls(), (unsigned)distinct,
                          (unsigned)files, (unsigned)frames);
         n += describe_cpu1(buf + n, sizeof(buf) - n);
         n += snprintf(buf + n, sizeof(buf) - n, "\n");
         n += describe_threads(buf + n, sizeof(buf) - n);
         trace_summary(buf + n, sizeof(buf) - n, 0, 8);
         WHBLogPrintf("%s", buf);
         OSFatal(buf);
      }
   }
   return 0;
}

void port_watch_start(void)
{
   OSCreateThread(&sWatch, (OSThreadEntryPointFn)watch_main, 0, NULL, sWatchStack + sizeof(sWatchStack),
                  sizeof(sWatchStack), 0, OS_THREAD_ATTRIB_AFFINITY_CPU0);
   OSSetThreadName(&sWatch, "Rayman watchdog");
   OSResumeThread(&sWatch);
}
