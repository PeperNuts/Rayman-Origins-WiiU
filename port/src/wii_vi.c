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
}

uint32_t port_vi_retrace_count(void) { return sRetraceCount; }

// Waits for the next retrace by polling the frame counter with short sleeps. Unlike the
// Wii's version it never disables interrupts around the wait.
void wii_VIWaitForRetrace(void)
{
   uint32_t count = sRetraceCount;
   int was = OSIsInterruptEnabled();
   if (!was) OSEnableInterrupts();
   while (count == sRetraceCount) {
      OSSleepTicks(OSMicrosecondsToTicks(500));
   }
   if (!was) OSDisableInterrupts();
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
