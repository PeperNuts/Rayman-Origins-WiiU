// Rayman Origins Wii U port - Wii Remote input (KPAD/WPAD) on top of the Wii U's padscore.
//
// The Wii U ships its own KPAD/WPAD libraries for Wii Remotes, descended from the Wii SDK:
// KPADStatus has the same 0xF0-byte layout (buttons, accelerometer, pointer, Nunchuk /
// Classic data, MotionPlus) and WPADInfo is identical. So most calls are forwarded, with
// callbacks routed back into game code through game_call (game r2/r13).
#include <padscore/kpad.h>
#include <padscore/wpad.h>
#include <whb/log.h>

#include <stdint.h>
#include <string.h>

#include "port.h"

#define NUM_CHAN 4
#define WII_KPAD_STATUS_SIZE 0xF0
#define NUNCHUK_BUTTONS (WPAD_BUTTON_Z | WPAD_BUTTON_C)

_Static_assert(sizeof(KPADStatus) == WII_KPAD_STATUS_SIZE, "KPADStatus layout differs from the Wii's");
_Static_assert(sizeof(WPADInfo) == 0x18, "WPADInfo layout differs from the Wii's");

static void *sConnectCb[NUM_CHAN];
static void *sSamplingCb[NUM_CHAN];
static void *sExtensionCb[NUM_CHAN];
static void *sInfoCb[NUM_CHAN];
static int sDpdEnabled[NUM_CHAN];
static int sInitialized;

static void connect_cb(WPADChan chan, int32_t reason)
{
   WHBLogPrintf("[pad] chan %d %s", (int)chan, reason == 0 ? "connected" : "disconnected");
   if (chan < NUM_CHAN && sConnectCb[chan]) game_call(sConnectCb[chan], (uint32_t)chan, (uint32_t)reason, 0);
}

static void sampling_cb(WPADChan chan)
{
   if (chan < NUM_CHAN && sSamplingCb[chan]) game_call(sSamplingCb[chan], (uint32_t)chan, 0, 0);
}

static void extension_cb(WPADChan chan, WPADExtensionType ext)
{
   WHBLogPrintf("[pad] chan %d extension %d", (int)chan, (int)ext);
   if (chan < NUM_CHAN && sExtensionCb[chan]) game_call(sExtensionCb[chan], (uint32_t)chan, (uint32_t)ext, 0);
}

static void info_cb(WPADChan chan, WPADError status)
{
   if (chan < NUM_CHAN && sInfoCb[chan]) game_call(sInfoCb[chan], (uint32_t)chan, (uint32_t)status, 0);
}

// ---------------------------------------------------------------------------------------
// KPAD
// ---------------------------------------------------------------------------------------
void wii_KPADInitEx(void *uniRingBufs, uint32_t length)
{
   if (!sInitialized) {
      KPADInit();
      sInitialized = 1;
      WHBLogPrintf("[pad] KPAD initialised");
   }
}

void wii_KPADReset(void) {}

void wii_KPADShutdown(void)
{
   if (sInitialized) {
      KPADShutdown();
      sInitialized = 0;
   }
}

void *wii_KPADSetConnectCallback(int32_t chan, void *cb)
{
   if (chan < 0 || chan >= NUM_CHAN) return NULL;
   void *old = sConnectCb[chan];
   sConnectCb[chan] = cb;
   KPADSetConnectCallback((KPADChan)chan, cb ? (KPADConnectCallback)connect_cb : NULL);
   // A remote that was connected before the game registered its callback: report it now,
   // as the Wii would have done when it connected.
   WPADExtensionType ext;
   if (cb && WPADProbe((WPADChan)chan, &ext) == 0) {
      connect_cb((WPADChan)chan, 0);
   }
   return old;
}

void *wii_KPADSetSamplingCallback(int32_t chan, void *cb)
{
   if (chan < 0 || chan >= NUM_CHAN) return NULL;
   void *old = sSamplingCb[chan];
   sSamplingCb[chan] = cb;
   KPADSetSamplingCallback((KPADChan)chan, cb ? (KPADSamplingCallback)sampling_cb : NULL);
   return old;
}

int32_t wii_KPADReadEx(int32_t chan, uint8_t *wiiBufs, uint32_t length, int32_t *err)
{
   static KPADStatus buf[32];
   if (length > 32) length = 32;
   KPADError e = KPAD_ERROR_OK;
   int32_t n = KPADReadEx((KPADChan)chan, buf, length, &e);
   if (err) *err = (int32_t)e;
   for (int32_t i = 0; i < n; i++) {
      KPADStatus *s = &buf[i];
      // Wii reports Nunchuk C/Z in the core button bits.
      if (s->extensionType == WPAD_EXT_NUNCHUK || s->extensionType == WPAD_EXT_MPLUS_NUNCHUK) {
         s->hold |= s->nunchuk.hold & NUNCHUK_BUTTONS;
         s->trigger |= s->nunchuk.trigger & NUNCHUK_BUTTONS;
         s->release |= s->nunchuk.release & NUNCHUK_BUTTONS;
      }
      memcpy(wiiBufs + i * WII_KPAD_STATUS_SIZE, s, WII_KPAD_STATUS_SIZE);
   }
   return n;
}

void wii_KPADEnableDPD(int32_t chan)
{
   if (chan >= 0 && chan < NUM_CHAN) sDpdEnabled[chan] = 1;
   KPADEnableDPD((KPADChan)chan);
}

void wii_KPADDisableDPD(int32_t chan)
{
   if (chan >= 0 && chan < NUM_CHAN) sDpdEnabled[chan] = 0;
   KPADDisableDPD((KPADChan)chan);
}

void wii_KPADDisableMpls(int32_t chan) { KPADDisableMpls((KPADChan)chan); }
uint8_t wii_KPADGetMplsStatus(int32_t chan) { return KPADGetMplsStatus((KPADChan)chan); }

// ---------------------------------------------------------------------------------------
// WPAD
// ---------------------------------------------------------------------------------------
void wii_WPADRegisterAllocator(void *alloc, void *free) {}

int32_t wii_WPADProbe(int32_t chan, uint32_t *type)
{
   WPADExtensionType ext = WPAD_EXT_DEV_NOT_FOUND;
   int32_t r = WPADProbe((WPADChan)chan, &ext);
   if (type) *type = (uint32_t)ext;
   return r;
}

void *wii_WPADSetExtensionCallback(int32_t chan, void *cb)
{
   if (chan < 0 || chan >= NUM_CHAN) return NULL;
   void *old = sExtensionCb[chan];
   sExtensionCb[chan] = cb;
   WPADSetExtensionCallback((WPADChan)chan, cb ? extension_cb : NULL);
   return old;
}

void wii_WPADDisconnect(int32_t chan) { WPADDisconnect((WPADChan)chan); }
void wii_WPADControlMotor(int32_t chan, uint32_t cmd) { WPADControlMotor((WPADChan)chan, cmd != 0); }
int wii_WPADIsMotorEnabled(void) { return WPADIsMotorEnabled(); }
uint8_t wii_WPADGetSensorBarPosition(void) { return 0; } // sensor bar below the TV
int wii_WPADIsDpdEnabled(int32_t chan) { return (chan >= 0 && chan < NUM_CHAN) ? sDpdEnabled[chan] : 0; }

int32_t wii_WPADGetInfoAsync(int32_t chan, WPADInfo *info, void *cb)
{
   if (chan < 0 || chan >= NUM_CHAN) return -1;
   sInfoCb[chan] = cb;
   return WPADGetInfoAsync((WPADChan)chan, info, cb ? info_cb : NULL);
}
