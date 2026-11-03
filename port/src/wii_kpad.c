// Rayman Origins Wii U port - Wii Remote input (KPAD/WPAD) on top of the Wii U's padscore.
//
// The Wii U ships its own KPAD/WPAD libraries for Wii Remotes, descended from the Wii SDK:
// KPADStatus has the same 0xF0-byte layout (buttons, accelerometer, pointer, Nunchuk /
// Classic data, MotionPlus) and WPADInfo is identical. So most calls are forwarded, with
// callbacks routed back into game code through game_call (game r2/r13).
#include <padscore/kpad.h>
#include <padscore/wpad.h>
#include <vpad/input.h>
#include <coreinit/time.h>
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

// ---------------------------------------------------------------------------------------
// Wii U GamePad as player 1: presented to the game as a Wii Remote + Nunchuk on channel 0.
//   left stick -> Nunchuk stick     D-pad -> D-pad
//   A, X -> A        B, Y -> B       ZR, R -> Z      ZL, L -> C      + / - -> + / -
// A real Wii Remote on channel 0 still works; whichever was used last drives channel 0.
// ---------------------------------------------------------------------------------------
static uint32_t sPadPrev;       // previous synthesised hold
static OSTime sPadLastUse;      // last GamePad input
static OSTime sRemoteLastUse;   // last real remote input on channel 0
static int sPadReported;        // the game was told channel 0 is connected (GamePad)

static int remote_present(int chan)
{
   WPADExtensionType ext;
   return WPADProbe((WPADChan)chan, &ext) == 0;
}

static uint32_t pad_buttons(const VPADStatus *v)
{
   static const struct { uint32_t vpad, wpad; } map[] = {
      {VPAD_BUTTON_A, WPAD_BUTTON_A},       {VPAD_BUTTON_X, WPAD_BUTTON_A},       {VPAD_BUTTON_B, WPAD_BUTTON_B},
      {VPAD_BUTTON_Y, WPAD_BUTTON_B},       {VPAD_BUTTON_ZR, WPAD_BUTTON_Z},      {VPAD_BUTTON_R, WPAD_BUTTON_Z},
      {VPAD_BUTTON_ZL, WPAD_BUTTON_C},      {VPAD_BUTTON_L, WPAD_BUTTON_C},       {VPAD_BUTTON_PLUS, WPAD_BUTTON_PLUS},
      {VPAD_BUTTON_MINUS, WPAD_BUTTON_MINUS}, {VPAD_BUTTON_LEFT, WPAD_BUTTON_LEFT}, {VPAD_BUTTON_RIGHT, WPAD_BUTTON_RIGHT},
      {VPAD_BUTTON_UP, WPAD_BUTTON_UP},     {VPAD_BUTTON_DOWN, WPAD_BUTTON_DOWN},
   };
   uint32_t h = 0;
   for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++)
      if (v->hold & map[i].vpad) h |= map[i].wpad;
   return h;
}

// Reads the GamePad; returns 1 and fills `out` (Wii KPADStatus layout) when it has a sample.
static int pad_sample(KPADStatus *out)
{
   static VPADStatus v;
   static int have;
   VPADReadError e = VPAD_READ_SUCCESS;
   VPADStatus nv;
   if (VPADRead(VPAD_CHAN_0, &nv, 1, &e) == 1 && e == VPAD_READ_SUCCESS) { v = nv; have = 1; }
   if (!have) return 0;
   uint32_t hold = pad_buttons(&v);
   float sx = v.leftStick.x, sy = v.leftStick.y;
   if (hold || sx * sx + sy * sy > 0.04f) sPadLastUse = OSGetSystemTime();
   memset(out, 0, sizeof(*out));
   out->hold = hold;
   out->trigger = hold & ~sPadPrev;
   out->release = sPadPrev & ~hold;
   sPadPrev = hold;
   out->acc.y = -1.0f; // held still, pointing at the screen
   out->accMagnitude = 1.0f;
   out->down.y = 1.0f;
   out->angle.x = 1.0f;
   out->dist = 2.0f;
   out->extensionType = WPAD_EXT_NUNCHUK;
   out->error = 0;
   out->posValid = 0;
   out->nunchuk.stick.x = sx;
   out->nunchuk.stick.y = sy;
   out->nunchuk.acc.y = -1.0f;
   out->nunchuk.accMagnitude = 1.0f;
   out->nunchuk.hold = hold & NUNCHUK_BUTTONS;
   out->nunchuk.trigger = out->trigger & NUNCHUK_BUTTONS;
   out->nunchuk.release = out->release & NUNCHUK_BUTTONS;
   return 1;
}

static void connect_cb(WPADChan chan, int32_t reason)
{
   WHBLogPrintf("[pad] chan %d %s", (int)chan, reason == 0 ? "connected" : "disconnected");
   if (chan == 0 && reason != 0) return; // the GamePad keeps channel 0 connected
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
   if (cb && (WPADProbe((WPADChan)chan, &ext) == 0 || chan == 0)) {
      if (chan == 0) { sPadReported = 1; WHBLogPrintf("[pad] GamePad is player 1 (Wii Remote + Nunchuk on channel 0)"); }
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
   if (chan == 0) {
      for (int32_t i = 0; i < n; i++)
         if (buf[i].hold || buf[i].nunchuk.stick.x * buf[i].nunchuk.stick.x + buf[i].nunchuk.stick.y * buf[i].nunchuk.stick.y > 0.04f)
            sRemoteLastUse = OSGetSystemTime();
      KPADStatus ps;
      int havePad = pad_sample(&ps);
      // the GamePad drives channel 0 unless the real remote is present and was used last
      if (havePad && (n <= 0 || sPadLastUse >= sRemoteLastUse)) {
         buf[0] = ps;
         n = 1;
         e = KPAD_ERROR_OK;
      }
   }
   if (err) *err = (int32_t)e;
   static uint32_t sReads, sEvents;
   if (sReads++ == 0) WHBLogPrintf("[pad] game reads chan %d: %d samples, err %d", (int)chan, (int)n, (int)e);
   for (int32_t i = 0; i < n; i++) {
      KPADStatus *s = &buf[i];
      // Wii reports Nunchuk C/Z in the core button bits.
      if (s->extensionType == WPAD_EXT_NUNCHUK || s->extensionType == WPAD_EXT_MPLUS_NUNCHUK) {
         s->hold |= s->nunchuk.hold & NUNCHUK_BUTTONS;
         s->trigger |= s->nunchuk.trigger & NUNCHUK_BUTTONS;
         s->release |= s->nunchuk.release & NUNCHUK_BUTTONS;
      }
      memcpy(wiiBufs + i * WII_KPAD_STATUS_SIZE, s, WII_KPAD_STATUS_SIZE);
      if (s->trigger && sEvents < 40) {
         sEvents++;
         WHBLogPrintf("[pad] chan %d press %04X (held %04X, ext %d, stick %d,%d)", (int)chan, (unsigned)s->trigger,
                      (unsigned)s->hold, (int)s->extensionType, (int)(s->nunchuk.stick.x * 100), (int)(s->nunchuk.stick.y * 100));
      }
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
   if (chan == 0 && (r != 0 || sPadLastUse >= sRemoteLastUse)) { r = 0; ext = WPAD_EXT_NUNCHUK; } // GamePad
   if (type) *type = (uint32_t)ext;
   return r;
}

void *wii_WPADSetExtensionCallback(int32_t chan, void *cb)
{
   if (chan < 0 || chan >= NUM_CHAN) return NULL;
   void *old = sExtensionCb[chan];
   sExtensionCb[chan] = cb;
   WPADSetExtensionCallback((WPADChan)chan, cb ? extension_cb : NULL);
   if (chan == 0 && cb && !remote_present(0)) extension_cb(WPAD_CHAN_0, WPAD_EXT_NUNCHUK); // GamePad
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
   if (chan == 0 && !remote_present(0)) { // GamePad: a healthy remote with a Nunchuk
      if (info) { memset(info, 0, sizeof(*info)); info->extensionAttached = 1; info->batteryLevel = 4; info->led = 1; }
      if (cb) info_cb(WPAD_CHAN_0, WPAD_ERROR_NONE);
      return 0;
   }
   return WPADGetInfoAsync((WPADChan)chan, info, cb ? info_cb : NULL);
}
