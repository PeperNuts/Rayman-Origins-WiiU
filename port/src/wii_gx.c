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
#include <coreinit/interrupts.h>
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

void wii_GXEnableBreakPt(void *fifoAddr) { sBreakPtArmed = 1; }
void wii_GXDisableBreakPt(void) { sBreakPtArmed = 0; }

void wii_GXSetDrawSync(uint16_t token) { sDrawSyncToken = token; }
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

// Display copy state for the TV (render.c): stage 1 of the GX2 backend shows the game's
// copy-clear colour each time it copies a frame to the display (GXCopyDisp).
static volatile uint32_t sCopyClear = 0x000000FFu; // RGBA
static volatile uint32_t sCopyDisp;
static volatile int sViBlack = 1;

void wii_GXSetCopyClear(const uint8_t *rgba, uint32_t z)
{
   sCopyClear = ((uint32_t)rgba[0] << 24) | ((uint32_t)rgba[1] << 16) | ((uint32_t)rgba[2] << 8) | rgba[3];
}

void wii_GXCopyDisp(void *dest, int clear) { sCopyDisp++; }
void wii_VISetBlack(int black) { sViBlack = black; }

uint32_t port_gx_copy_clear(void) { return sCopyClear; }
uint32_t port_gx_copy_disp(void) { return sCopyDisp; }
int port_vi_black(void) { return sViBlack; }
