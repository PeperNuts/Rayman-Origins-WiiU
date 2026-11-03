// Rayman Origins Wii U port - Wii video interface (VI) timing.
//
// No real video output yet. What the game needs from VI at this stage is the frame clock:
// VIWaitForRetrace must block until the next 60 Hz retrace, and the pre/post-retrace
// callbacks must fire once per frame (on the Wii they run from the VI interrupt).
#include <coreinit/alarm.h>
#include <coreinit/interrupts.h>
#include <coreinit/thread.h>
#include <coreinit/threadqueue.h>
#include <coreinit/time.h>
#include <whb/log.h>

#include <stdint.h>

#include "port.h"

static OSAlarm sRetraceAlarm;
static OSThreadQueue sRetraceQueue;
static volatile uint32_t sRetraceCount;
static void *volatile sPreCallback;
static void *volatile sPostCallback;

// Where CPU1 was when the frame interrupt hit: PC, LR and three return addresses.
volatile uint32_t port_cpu1_sample[5];
volatile uint32_t port_cpu1_sample_frame;
volatile int port_in_frame_callback; // 1 = pre-retrace callback, 2 = post-retrace callback

static uint32_t chain_ra(uint32_t *sp)
{
   uint32_t s = *sp;
   if (s < 0x10000000 || s >= 0x50000000 || (s & 3)) { *sp = 0; return 0; }
   *sp = *(uint32_t *)s;
   return *(uint32_t *)(s + 4);
}

// ---------------------------------------------------------------------------------------
// Sampling profiler for CPU1: a ~2 kHz alarm on the game core records where it interrupted
// (64-byte PC buckets plus the interrupted thread). The watchdog prints the top entries.
// ---------------------------------------------------------------------------------------
#define PROF_SLOTS 8192
typedef struct { uint32_t key, hits, lr; } ProfSlot;
static ProfSlot sProf[PROF_SLOTS];
static uint32_t sProfTotal, sProfLost;
#define PROF_THREADS 24
static OSThread *sProfThr[PROF_THREADS];
static uint32_t sProfThrHits[PROF_THREADS];
static OSAlarm sProfAlarm;

static void prof_tick(OSAlarm *alarm, OSContext *ctx)
{
   if (!ctx) return;
   sProfTotal++;
   uint32_t key = ctx->srr0 >> 6;
   uint32_t h = (key * 2654435761u) >> 20;
   for (int i = 0; i < 16; i++) {
      ProfSlot *s = &sProf[(h + i) & (PROF_SLOTS - 1)];
      if (s->key == key || !s->hits) { s->key = key; s->hits++; s->lr = ctx->lr; goto thr; }
   }
   sProfLost++;
thr:;
   OSThread *t = OSGetCurrentThread();
   for (int i = 0; i < PROF_THREADS; i++) {
      if (sProfThr[i] == t || !sProfThr[i]) { sProfThr[i] = t; sProfThrHits[i]++; return; }
   }
}

// Copies out and resets the profile; returns the number of samples.
uint32_t port_prof_take(uint32_t *keys, uint32_t *hits, uint32_t *lrs, int max, int *n, OSThread **thr,
                        uint32_t *thrHits, int *nthr)
{
   int en = OSDisableInterrupts();
   uint32_t total = sProfTotal;
   int k = 0;
   for (int i = 0; i < PROF_SLOTS; i++) {
      if (!sProf[i].hits) continue;
      // keep the `max` biggest (insertion into a small sorted list)
      uint32_t hv = sProf[i].hits;
      int j;
      if (k < max) j = k++;
      else if (hits[max - 1] < hv) j = max - 1;
      else continue;
      while (j > 0 && hits[j - 1] < hv) { keys[j] = keys[j - 1]; hits[j] = hits[j - 1]; lrs[j] = lrs[j - 1]; j--; }
      keys[j] = sProf[i].key; hits[j] = sProf[i].hits; lrs[j] = sProf[i].lr;
   }
   *n = k;
   int m = 0;
   for (int i = 0; i < PROF_THREADS && sProfThr[i]; i++) { thr[m] = sProfThr[i]; thrHits[m] = sProfThrHits[i]; m++; }
   *nthr = m;
   for (int i = 0; i < PROF_SLOTS; i++) sProf[i].hits = 0, sProf[i].key = 0;
   for (int i = 0; i < PROF_THREADS; i++) sProfThr[i] = NULL, sProfThrHits[i] = 0;
   sProfTotal = sProfLost = 0;
   OSRestoreInterrupts(en);
   return total;
}

static void retrace(OSAlarm *alarm, OSContext *ctx)
{
   uint32_t count = ++sRetraceCount;
   if (ctx) {
      port_cpu1_sample[0] = ctx->srr0;
      port_cpu1_sample[1] = ctx->lr;
      uint32_t sp = ctx->gpr[1];
      for (int i = 2; i < 5; i++) port_cpu1_sample[i] = sp ? chain_ra(&sp) : 0;
      port_cpu1_sample_frame = count;
   }
   port_gx_retrace(); // "GPU" finishes the queued frame: breakpoint callback first
   void *pre = sPreCallback;
   if (pre) { port_in_frame_callback = 1; game_call(pre, count, 0, 0); }
   OSWakeupThread(&sRetraceQueue);
   void *post = sPostCallback;
   if (post) { port_in_frame_callback = 2; game_call(post, count, 0, 0); }
   port_in_frame_callback = 0;
}

// Must be called on the game's core: Cafe runs alarm callbacks on the core that set them.
void port_vi_init(void)
{
   OSInitThreadQueue(&sRetraceQueue);
   OSCreateAlarm(&sRetraceAlarm);
   OSSetPeriodicAlarm(&sRetraceAlarm, OSGetTime(), OSNanosecondsToTicks(16683333), retrace); // 59.94 Hz
   extern void port_gx_bp_init(void);
   port_gx_bp_init();
   OSCreateAlarm(&sProfAlarm);
   OSSetPeriodicAlarm(&sProfAlarm, OSGetTime(), OSNanosecondsToTicks(487000), prof_tick); // ~2 kHz, not frame-locked
}

uint32_t port_vi_retrace_count(void) { return sRetraceCount; }

// Waits for the next retrace by polling the frame counter with short sleeps. Unlike the
// Wii's version it never disables interrupts around the wait.
// Frame pacing statistics for the profiler: time the game spends working vs waiting.
volatile uint64_t port_vi_wait_ticks, port_vi_frames, port_vi_late;

void wii_VIWaitForRetrace(void)
{
   static uint32_t lastReturn;
   static int strict = -1;
   if (strict < 0) strict = port_cfg("strictvsync");
   uint32_t count = sRetraceCount;
   port_vi_frames++;
   // Adaptive: a frame that already missed its retrace continues at once instead of
   // waiting for the following one (which would halve the frame rate to 30).
   if (!strict && count != lastReturn) {
      port_vi_late++;
      lastReturn = count;
      return;
   }
   OSTime t0 = OSGetSystemTime();
   int was = OSIsInterruptEnabled();
   if (!was) OSEnableInterrupts();
   while (count == sRetraceCount) {
      OSSleepTicks(OSMicrosecondsToTicks(500));
   }
   if (!was) OSDisableInterrupts();
   lastReturn = sRetraceCount;
   port_vi_wait_ticks += OSGetSystemTime() - t0;
}

void *wii_VISetPreRetraceCallback(void *cb)
{
   int en = OSDisableInterrupts();
   void *old = sPreCallback;
   sPreCallback = cb;
   OSRestoreInterrupts(en);
   return old;
}

void *wii_VISetPostRetraceCallback(void *cb)
{
   int en = OSDisableInterrupts();
   void *old = sPostCallback;
   sPostCallback = cb;
   OSRestoreInterrupts(en);
   return old;
}

uint32_t wii_VIGetNextField(void) { return 1; } // progressive: always "field 1"
