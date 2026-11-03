// Rayman Origins Wii U port - GX completion signals.
//
// No GPU backend yet, so every command the game queues counts as finished immediately.
// What the game needs from GX at this stage is the "GPU is done" signalling:
//  - FIFO breakpoints: GFXAdapter_WII arms a breakpoint after each frame; the breakpoint
//    callback (SetNextBreakPt) marks the frame done, and VIPostCallback then frees the slot
//    in its 5-frame queue. Without it the render loop blocks after 4 frames.
//  - draw-sync tokens (GXSetDrawSync / GXReadDrawSync).
// The breakpoint fires from the 60 Hz retrace handler, ahead of the game's retrace
// callbacks, like a GPU that finishes each frame within one refresh.
#include <coreinit/alarm.h>
#include <coreinit/interrupts.h>
#include <coreinit/time.h>
#include <whb/log.h>

#include <stdint.h>

#include "port.h"

static void *volatile sBreakPtCallback;
static volatile int sBreakPtArmed;
static volatile uint32_t sBreakPtFired;
static volatile uint16_t sDrawSyncToken;

void *wii_GXSetBreakPtCallback(void *cb)
{
   int en = OSDisableInterrupts();
   void *old = sBreakPtCallback;
   sBreakPtCallback = cb;
   OSRestoreInterrupts(en);
   return old;
}

static volatile OSTime sBreakPtArmTime;
void port_gx_bp_enable(void) { sBreakPtArmTime = OSGetSystemTime(); sBreakPtArmed = 1; } // from the GX worker
void port_gx_bp_disable(void) { sBreakPtArmed = 0; }

void port_gx_set_drawsync(uint16_t token) { sDrawSyncToken = token; }
uint16_t wii_GXReadDrawSync(void) { return sDrawSyncToken; }

// Called from the retrace alarm (interrupt context), before the game's retrace callbacks.
void port_gx_retrace(void)
{
   if (sBreakPtArmed) {
      sBreakPtArmed = 0;
      void *cb = sBreakPtCallback;
      if (cb) {
         sBreakPtFired++;
         game_call(cb, 0, 0, 0); // may re-arm via GXEnableBreakPt
      }
   }
}

uint32_t port_gx_breakpoints(void) { return sBreakPtFired; }

// The commands before a breakpoint are already decoded when the game arms it, so a real GPU
// would reach it within a millisecond or so: fire it then (1 kHz alarm on the game core)
// instead of at the next retrace, which made a slightly late frame wait a whole refresh.
static OSAlarm sBpAlarm;
static void bp_tick(OSAlarm *a, OSContext *ctx)
{
   if (sBreakPtArmed && OSGetSystemTime() - sBreakPtArmTime >= (OSTime)OSMicrosecondsToTicks(500)) port_gx_retrace();
}

void port_gx_bp_init(void) // on the game core
{
   if (port_cfg("bpretrace")) return;
   OSCreateAlarm(&sBpAlarm);
   OSSetPeriodicAlarm(&sBpAlarm, OSGetTime(), OSMicrosecondsToTicks(1000), bp_tick);
}

// VISetBlack: the back end shows black while the game blanks the display.
static volatile int sViBlack = 1;
void wii_VISetBlack(int black) { sViBlack = black; }
int port_vi_black(void) { return sViBlack; }
