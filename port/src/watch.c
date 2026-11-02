// Rayman Origins Wii U port - watchdog on a separate core.
//
// Runs on CPU0 at high priority so it keeps working whatever the game does on CPU1:
//  - draws the log console
//  - every 5 s samples where each game thread is (suspend, read saved PC/LR, resume)
//  - if the game makes no progress (no new platform functions, no files opened) for 30 s,
//    shows an OSFatal screen with every game thread's location and call stack.
#include <coreinit/debug.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <whb/log.h>
#include <whb/log_console.h>

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

static int watch_main(int argc, const char **argv)
{
   uint32_t ticks = 0, seconds = 0, lastDistinct = 0, lastFiles = 0, quiet = 0;
   uint32_t lastFrames = 0, frozen = 0;
   static char buf[1400];
   for (;;) {
      WHBLogConsoleDraw();
      OSSleepTicks(OSMillisecondsToTicks(100));
      if (++ticks % 10) continue;
      seconds++;

      uint32_t distinct = trace_distinct(), files = port_dvd_files(), frames = port_vi_retrace_count();
      quiet = (distinct == lastDistinct && files == lastFiles) ? quiet + 1 : 0;
      frozen = (frames == lastFrames) ? frozen + 1 : 0;
      lastDistinct = distinct;
      lastFiles = files;
      lastFrames = frames;

      if (seconds <= 10 || seconds % 5 == 0) {
         describe_cpu1(buf, sizeof(buf));
         WHBLogPrintf("[watch] %us st%d calls %u funcs %u files %u frames %u %s", (unsigned)seconds, port_stage(),
                      (unsigned)trace_calls(), (unsigned)distinct, (unsigned)files, (unsigned)frames, buf);
      }

      const char *why = NULL;
      if (frozen == 5) why = "CPU1 stopped taking interrupts for 5 s (frame clock frozen)";
      else if (quiet == 30) why = "no progress for 30 s";
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
