// Rayman Origins Wii U port - Wii AX (the DSP voice mixer) emulated in software.
//
// The game's sound engine ("raki") drives the Wii's AX library: it acquires voices,
// points them at DSP-ADPCM (or PCM) data in memory, sets volumes and pitch from the AX
// frame callback, and reads back each voice's play position and state from its AXVPB.
// Here every voice is decoded and mixed on the CPU, one 3 ms AX frame (96 stereo samples
// at 32 kHz) at a time, and the mix is streamed to the Wii U's own AX (sndcore2, 32 kHz
// renderer) through two looping PCM16 voices (left, right) on the TV and the GamePad.
//
// Frames are produced on the game core (CPU1) from a 1 ms alarm, a few frames ahead of the
// hardware's play position, so the game's AX callback runs in interrupt context on the game
// core as on the Wii. The hardware position comes from a Wii U AX frame callback that only
// counts frames.
#include <coreinit/alarm.h>
#include <coreinit/cache.h>
#include <coreinit/interrupts.h>
#include <coreinit/memdefaultheap.h>
#include <coreinit/time.h>
#include <sndcore2/core.h>
#include <sndcore2/device.h>
#include <sndcore2/voice.h>
#include <whb/log.h>

#include <stdint.h>
#include <string.h>

#include "port.h"

#define FRAME 96                   // samples per AX frame (3 ms at 32 kHz)
#define RING_FRAMES 64             // output ring: 192 ms
#define RING (FRAME * RING_FRAMES)
#define LEAD 5                     // frames mixed ahead of the hardware (15 ms)
#define MAX_VOICES 96

// --- Wii AXVPB / AXPB layout (only what the game reads or what we keep there) --------------
#define VPB_SIZE 0x200
#define VPB_PRIO 0x0C
#define VPB_CB 0x10
#define VPB_CTX 0x14
#define VPB_INDEX 0x18
#define PB 0x28
#define PB_STATE (PB + 0x10)
#define PB_TYPE (PB + 0x12)
#define PB_MIX (PB + 0x14)
#define PB_ADDR (PB + 0x6E)        // loopFlag, format, loop, end, current (hi/lo halves)
enum { FMT_ADPCM = 0, FMT_PCM16 = 0x0A, FMT_PCM8 = 0x19 };

typedef struct {
   uint8_t *vpb;                  // game-visible block
   int used, running, stream;
   uint16_t format, loop;
   uint32_t loopAddr, endAddr, cur;  // in the game's units (nibbles / samples / bytes)
   int16_t coef[16];
   uint16_t gain, ps;
   int16_t yn1, yn2;
   uint16_t lps;
   int16_t lyn1, lyn2;
   uint32_t ratio;                // 16.16 source samples per output sample
   uint32_t frac;
   int srcType;
   int32_t s0, s1;                // previous and current source sample
   int primed;
   uint16_t ve;                   // envelope volume (0x8000 = 1.0)
   int16_t veDelta;
   uint16_t vL, vR;
   int16_t dL, dR;
} Voice;

static Voice sV[MAX_VOICES];
static void *sAxCallback, *sAuxCb[3];
static int sInit;

static int16_t *sRingL, *sRingR;
static AXVoice *sOutL, *sOutR;
static volatile uint32_t sHwFrames;     // frames the hardware has played
static uint32_t sProduced;              // frames written to the ring
static OSAlarm sAlarm;
static uint32_t sStats[4];              // frames, max voices, underruns, callbacks

static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static inline void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static inline uint32_t rd32hl(const uint8_t *p) { return (uint32_t)rd16(p) << 16 | rd16(p + 2); }

// The game encodes voice addresses from physical Wii addresses (virtual - 0x80000000) in
// nibbles / samples; with game memory at 0x1xxxxxxx part of that wraps. Pick the reading
// that lands in game memory.
static inline const uint8_t *mem_at(uint32_t byteAddr)
{
   static const uint8_t zero[4];
   uint32_t a = byteAddr;
   if (a < 0x10000000u || a >= 0x60000000u) a ^= 0x80000000u;
   if (a < 0x10000000u || a >= 0x60000000u) return zero; // not game memory: silence, never a crash
   return (const uint8_t *)a;
}

static void publish(Voice *v)
{
   uint8_t *b = v->vpb;
   wr16(b + PB_STATE, (uint16_t)v->running);
   wr16(b + PB_ADDR + 0x0C, (uint16_t)(v->cur >> 16));
   wr16(b + PB_ADDR + 0x0E, (uint16_t)v->cur);
}

// Next source sample (advances the address; handles ADPCM frame headers, loops, the end).
static int32_t next_sample(Voice *v)
{
   if (!v->running) return 0;
   int32_t s;
   switch (v->format) {
   case FMT_ADPCM: {
      if ((v->cur & 15) == 0) { // frame header: predictor/scale byte
         v->ps = *mem_at(v->cur >> 1);
         v->cur += 2;
      }
      uint8_t byte = *mem_at(v->cur >> 1);
      int nib = (v->cur & 1) ? (byte & 15) : (byte >> 4);
      if (nib >= 8) nib -= 16;
      int scale = 1 << (v->ps & 15), ci = (v->ps >> 4) & 7;
      int32_t val = ((nib * scale) << 11) + 1024 + v->coef[ci * 2] * v->yn1 + v->coef[ci * 2 + 1] * v->yn2;
      val >>= 11;
      if (val > 32767) val = 32767;
      if (val < -32768) val = -32768;
      v->yn2 = v->yn1;
      v->yn1 = (int16_t)val;
      s = val;
      break;
   }
   case FMT_PCM16: {
      const uint8_t *p = mem_at(v->cur << 1);
      s = (int16_t)rd16(p);
      break;
   }
   default: // PCM8
      s = (int8_t)*mem_at(v->cur) << 8;
      break;
   }
   if (v->cur == v->endAddr) {
      if (v->loop) {
         v->cur = v->loopAddr;
         if (!v->stream && v->format == FMT_ADPCM) { v->ps = v->lps; v->yn1 = v->lyn1; v->yn2 = v->lyn2; }
      } else {
         v->running = 0;
      }
   } else {
      v->cur++;
   }
   return s;
}

// Mixes one frame of every running voice into acc (stereo, 32-bit).
static void mix_frame(int32_t *accL, int32_t *accR)
{
   memset(accL, 0, FRAME * 4);
   memset(accR, 0, FRAME * 4);
   uint32_t active = 0;
   for (int i = 0; i < MAX_VOICES; i++) {
      Voice *v = &sV[i];
      if (!v->used || !v->running) continue;
      active++;
      if (!v->primed) { v->s0 = 0; v->s1 = next_sample(v); v->primed = 1; }
      int32_t ve = v->ve, vL = v->vL, vR = v->vR;
      for (int n = 0; n < FRAME; n++) {
         int32_t s;
         if (v->srcType == 0) s = v->s1; // AX_SRC_TYPE_NONE: no interpolation
         else s = v->s0 + (int32_t)(((int64_t)(v->s1 - v->s0) * (int32_t)(v->frac >> 1)) >> 15);
         s = (s * ve) >> 15;
         accL[n] += (s * vL) >> 15;
         accR[n] += (s * vR) >> 15;
         ve += v->veDelta;
         if (ve < 0) ve = 0;
         if (ve > 0xFFFF) ve = 0xFFFF;
         vL += v->dL; vR += v->dR;
         if (vL < 0) vL = 0;
         if (vR < 0) vR = 0;
         if (vL > 0xFFFF) vL = 0xFFFF;
         if (vR > 0xFFFF) vR = 0xFFFF;
         v->frac += v->ratio;
         while (v->frac >= 0x10000) {
            v->frac -= 0x10000;
            v->s0 = v->s1;
            v->s1 = next_sample(v);
         }
         if (!v->running) break;
      }
      v->ve = (uint16_t)ve;
      v->vL = (uint16_t)vL; v->vR = (uint16_t)vR;
      v->dL = v->dR = 0; // deltas apply over one frame
      v->veDelta = 0;
   }
   if (active > sStats[1]) sStats[1] = active;
   for (int i = 0; i < MAX_VOICES; i++)
      if (sV[i].used) publish(&sV[i]);
}

// ---------------------------------------------------------------------------------------
// Bink video sound: BinkOpenAX hands Bink a driver (BINKSND callbacks). Bink copies decoded
// interleaved s16 PCM into a ring here; the mixer resamples it to 32 kHz.
// ---------------------------------------------------------------------------------------
#define MAX_BINK 4
#define BINK_FRAMES 16384 // ring per track, in sample frames (~0.35 s at 48 kHz)
typedef struct {
   uint8_t *bs;
   int used, paused, on;
   uint32_t freq, chans, step, frac;
   int16_t *buf;
   volatile uint32_t rd, wr; // frames, monotonic
   int32_t vol;              // 0..32768
   int32_t l0, r0, l1, r1;
   uint8_t *bink; // the BINK the track belongs to (diagnostics)
} BinkTrack;
static BinkTrack sBink[MAX_BINK];
// Diagnostics (video sound): calls and data per second, logged by the watch thread.
static volatile uint32_t sBkReady, sBkReadyYes, sBkLock, sBkUnlockBytes, sBkMixed, sBkStarved;
void port_bink_stats(void)
{
   for (int i = 0; i < MAX_BINK; i++) {
      BinkTrack *t = &sBink[i];
      if (!t->used) continue;
      uint8_t *bs = t->bs;
      uint32_t w2c, w54, w0, w18;
      memcpy(&w2c, bs + 0x2C, 4); memcpy(&w54, bs + 0x54, 4); memcpy(&w0, bs, 4); memcpy(&w18, bs + 0x18, 4);
      // Bink's sound pump (pump_sound / checksound) only reaches the driver when these allow it
      extern char _SDA_BASE_[];
      uint32_t g9c, g7c, g80, g20, b124 = 0, b12c = 0, b460 = 0, b154 = 0, s4c;
      memcpy(&g9c, _SDA_BASE_ - 28004, 4); memcpy(&g7c, _SDA_BASE_ - 28036, 4);
      memcpy(&g80, _SDA_BASE_ - 28032, 4); memcpy(&g20, _SDA_BASE_ - 32736, 4);
      if (t->bink) { memcpy(&b124, t->bink + 292, 4); memcpy(&b12c, t->bink + 300, 4); memcpy(&b460, t->bink + 1120, 4); memcpy(&b154, t->bink + 340, 4); }
      memcpy(&s4c, bs + 0x4C, 4);
      WHBLogPrintf("[bink] gates: shutdown %u, active %u, list %08X, wait %u ms | bink %p: tracks %u, +300 %u, +1120 %u, "
                   "snd %08X (bs %p), bs+0x4C %d", (unsigned)g9c, (unsigned)g7c, (unsigned)g80, (unsigned)g20, t->bink,
                   (unsigned)b124, (unsigned)b12c, (unsigned)b460, (unsigned)b154, bs, (int)s4c);
      WHBLogPrintf("[bink] track %d: ready %u (yes %u), lock %u, %u bytes in, %u frames mixed, starved %u, "
                   "buffered %u, vol %d, on %d, paused %d | bs 0x00 %08X 0x18 %08X 0x2C %08X 0x54 %08X",
                   i, (unsigned)sBkReady, (unsigned)sBkReadyYes, (unsigned)sBkLock, (unsigned)sBkUnlockBytes,
                   (unsigned)sBkMixed, (unsigned)sBkStarved, (unsigned)(t->wr - t->rd), (int)t->vol, t->on, t->paused,
                   (unsigned)w0, (unsigned)w18, (unsigned)w2c, (unsigned)w54);
   }
   sBkReady = sBkReadyYes = sBkLock = sBkUnlockBytes = sBkMixed = sBkStarved = 0;
}

static BinkTrack *bink_of(void *bs)
{
   for (int i = 0; i < MAX_BINK; i++)
      if (sBink[i].used && sBink[i].bs == bs) return &sBink[i];
   return NULL;
}

static int bink_next(BinkTrack *t)
{
   if (t->wr - t->rd < 1) return 0;
   const int16_t *f = t->buf + (t->rd % BINK_FRAMES) * t->chans;
   t->l0 = t->l1; t->r0 = t->r1;
   t->l1 = f[0];
   t->r1 = t->chans > 1 ? f[1] : f[0];
   t->rd++;
   return 1;
}

static void mix_bink(int32_t *accL, int32_t *accR)
{
   for (int i = 0; i < MAX_BINK; i++) {
      BinkTrack *t = &sBink[i];
      if (!t->used || t->paused || !t->on) continue;
      sBkMixed += FRAME;
      for (int n = 0; n < FRAME; n++) {
         int32_t f = (int32_t)(t->frac >> 1);
         int32_t l = t->l0 + (((t->l1 - t->l0) * f) >> 15), r = t->r0 + (((t->r1 - t->r0) * f) >> 15);
         accL[n] += (l * t->vol) >> 15;
         accR[n] += (r * t->vol) >> 15;
         t->frac += t->step;
         while (t->frac >= 0x10000) {
            t->frac -= 0x10000;
            if (!bink_next(t)) { t->frac = 0; sBkStarved++; break; } // starved: hold
         }
      }
   }
}

static int32_t bink_ready(uint8_t *bs)
{
   BinkTrack *t = bink_of(bs);
   sBkReady++;
   int r = t && BINK_FRAMES - (t->wr - t->rd) >= 256;
   sBkReadyYes += r;
   return r;
}

static int32_t bink_lock(uint8_t *bs, uint8_t **addr, uint32_t *len)
{
   BinkTrack *t = bink_of(bs);
   sBkLock++;
   if (!t) return 0;
   uint32_t used = t->wr - t->rd, free = BINK_FRAMES - used;
   uint32_t at = t->wr % BINK_FRAMES, run = BINK_FRAMES - at;
   if (run > free) run = free;
   if (!run) return 0;
   *addr = (uint8_t *)(t->buf + at * t->chans);
   *len = run * t->chans * 2;
   return 1;
}

static int32_t bink_unlock(uint8_t *bs, uint32_t filled)
{
   BinkTrack *t = bink_of(bs);
   if (!t) return 0;
   __asm__ volatile("sync" ::: "memory");
   sBkUnlockBytes += filled;
   t->wr += filled / (t->chans * 2);
   return 1;
}

static void bink_volume(uint8_t *bs, int32_t vol)
{
   BinkTrack *t = bink_of(bs);
   if (t) t->vol = vol < 0 ? 0 : vol > 65536 ? 65536 : vol;
}
static void bink_pan(uint8_t *bs, int32_t pan) {}
static int32_t bink_pause(uint8_t *bs, int32_t status)
{
   BinkTrack *t = bink_of(bs);
   if (t && status >= 0) t->paused = status != 0;
   return t ? t->paused : 0;
}
static int32_t bink_onoff(uint8_t *bs, int32_t on)
{
   BinkTrack *t = bink_of(bs);
   if (t && on >= 0) t->on = on != 0;
   return t ? t->on : 0;
}
static void bink_close(uint8_t *bs)
{
   BinkTrack *t = bink_of(bs);
   if (!t) return;
   int en = OSDisableInterrupts();
   t->used = 0;
   OSRestoreInterrupts(en);
}

// BINKSND layout (Bink 1.9x, Wii): Bink's own ring and sizes up to 0x5C, the driver
// callbacks from 0x15C. Latency (ms) at 0x38.
static int32_t bink_open(uint8_t *bs, uint32_t freq, int32_t bits, int32_t chans, uint32_t flags, void *bink)
{
   if (!sRingL || bits != 16 || chans < 1 || chans > 2 || !freq) return 0;
   BinkTrack *t = NULL;
   for (int i = 0; i < MAX_BINK; i++) if (!sBink[i].used) { t = &sBink[i]; break; }
   if (!t) return 0;
   if (!t->buf) t->buf = MEMAllocFromDefaultHeapEx(BINK_FRAMES * 2 * 2, 64);
   if (!t->buf) return 0;
   int en = OSDisableInterrupts();
   t->bs = bs;
   t->freq = freq; t->chans = (uint32_t)chans;
   t->step = (uint32_t)(((uint64_t)freq << 16) / 32000);
   t->frac = 0; t->rd = t->wr = 0;
   t->vol = 32768; t->paused = 0; t->on = 1;
   t->l0 = t->r0 = t->l1 = t->r1 = 0;
   t->bink = bink;
   t->used = 1;
   OSRestoreInterrupts(en);
   static void *const fns[9] = {(void *)bink_ready, (void *)bink_lock, (void *)bink_unlock, (void *)bink_volume,
                                (void *)bink_pan, (void *)bink_pause, (void *)bink_onoff, (void *)bink_close, NULL};
   memcpy(bs + 0x15C, fns, sizeof(fns));
   uint32_t latency = 30, best = 0;
   memcpy(bs + 0x38, &latency, 4);
   memcpy(bs + 0x2C, &best, 4);
   WHBLogPrintf("[ax] Bink sound track: %u Hz, %d channels", (unsigned)freq, (int)chans);
   return 1;
}

void *wii_BinkOpenAX(uint32_t param) { return port_cfg("nobinksound") ? NULL : (void *)bink_open; }

static inline int16_t clamp16(int32_t x) { return (int16_t)(x > 32767 ? 32767 : x < -32768 ? -32768 : x); }

// One AX frame: mix into the ring, then the game's AX callback prepares the next one.
static void produce(void)
{
   static int32_t accL[FRAME], accR[FRAME];
   extern volatile int port_paused;
   if (port_paused) { // HOME Menu: silence, the game's sound engine is not run
      uint32_t at = (sProduced % RING_FRAMES) * FRAME;
      memset(sRingL + at, 0, FRAME * 2); memset(sRingR + at, 0, FRAME * 2);
      DCFlushRange(sRingL + at, FRAME * 2); DCFlushRange(sRingR + at, FRAME * 2);
      sProduced++;
      return;
   }
   mix_frame(accL, accR);
   mix_bink(accL, accR);
   uint32_t at = (sProduced % RING_FRAMES) * FRAME;
   for (int n = 0; n < FRAME; n++) { sRingL[at + n] = clamp16(accL[n]); sRingR[at + n] = clamp16(accR[n]); }
   DCFlushRange(sRingL + at, FRAME * 2);
   DCFlushRange(sRingR + at, FRAME * 2);
   sProduced++;
   sStats[0]++;
   void *cb = sAxCallback;
   if (cb) { sStats[3]++; game_call(cb, 0, 0, 0); }
}

static void tick(OSAlarm *a, OSContext *ctx)
{
   uint32_t hw = sHwFrames;
   if ((int32_t)(sProduced - hw) < 0) { sProduced = hw; sStats[2]++; } // fell behind: skip ahead
   int n = 0;
   while ((int32_t)(sProduced - hw) < LEAD && n++ < LEAD) produce();
}

static void hw_frame(void) { sHwFrames++; }

static AXVoice *out_voice(int16_t *ring, int channel)
{
   AXVoice *v = AXAcquireVoice(31, NULL, NULL);
   if (!v) return NULL;
   AXVoiceBegin(v);
   AXSetVoiceType(v, 0);
   AXVoiceVeData ve = {0x8000, 0};
   AXSetVoiceVe(v, &ve);
   AXVoiceDeviceMixData mix[6];
   memset(mix, 0, sizeof(mix));
   mix[channel].bus[0].volume = 0x8000;
   AXSetVoiceDeviceMix(v, AX_DEVICE_TYPE_TV, 0, mix);
   AXSetVoiceDeviceMix(v, AX_DEVICE_TYPE_DRC, 0, mix);
   AXVoiceOffsets o = {0};
   o.dataType = AX_VOICE_FORMAT_LPCM16;
   o.loopingEnabled = 1;
   o.loopOffset = 0;
   o.endOffset = RING - 1;
   o.currentOffset = 0;
   o.data = ring;
   AXSetVoiceOffsets(v, &o);
   AXSetVoiceSrcType(v, AX_VOICE_SRC_TYPE_NONE);
   AXSetVoiceSrcRatio(v, 1.0f);
   AXVoiceEnd(v);
   return v;
}

// Starts the output (called on the game core: the frame alarm must run there).
static void ax_start(void)
{
   if (sInit) return;
   sInit = 1;
   if (port_cfg("noaudio")) { WHBLogPrintf("[ax] audio disabled (noaudio)"); return; }
   AXInitParams p;
   memset(&p, 0, sizeof(p));
   p.renderer = AX_INIT_RENDERER_32KHZ;
   p.pipeline = AX_INIT_PIPELINE_SINGLE;
   AXInitWithParams(&p);
   sRingL = MEMAllocFromDefaultHeapEx(RING * 2, 64);
   sRingR = MEMAllocFromDefaultHeapEx(RING * 2, 64);
   if (!sRingL || !sRingR) { WHBLogPrintf("[ax] out of memory"); return; }
   memset(sRingL, 0, RING * 2);
   memset(sRingR, 0, RING * 2);
   DCFlushRange(sRingL, RING * 2);
   DCFlushRange(sRingR, RING * 2);
   sOutL = out_voice(sRingL, 0);
   sOutR = out_voice(sRingR, 1);
   if (!sOutL || !sOutR) { WHBLogPrintf("[ax] could not get output voices"); return; }
   sProduced = LEAD;
   AXRegisterAppFrameCallback(hw_frame);
   AXVoiceBegin(sOutL); AXVoiceBegin(sOutR);
   AXSetVoiceState(sOutL, AX_VOICE_STATE_PLAYING);
   AXSetVoiceState(sOutR, AX_VOICE_STATE_PLAYING);
   AXVoiceEnd(sOutL); AXVoiceEnd(sOutR);
   OSCreateAlarm(&sAlarm);
   OSSetPeriodicAlarm(&sAlarm, OSGetTime(), OSMicrosecondsToTicks(1000), tick);
   WHBLogPrintf("[ax] software AX mixer running (32 kHz, %u-sample frames, %u voices)", FRAME, MAX_VOICES);
}

uint32_t port_ax_stat(int i) { return i >= 0 && i < 4 ? sStats[i] : 0; }

// ---------------------------------------------------------------------------------------
// Wii AX API
// ---------------------------------------------------------------------------------------
static Voice *voice_of(void *vpb)
{
   for (int i = 0; i < MAX_VOICES; i++)
      if (sV[i].vpb == vpb) return &sV[i];
   return NULL;
}

void wii_AXInitSpecifyMem(uint32_t num, void *mem)
{
   for (int i = 0; i < MAX_VOICES; i++) {
      if (!sV[i].vpb) {
         sV[i].vpb = MEMAllocFromDefaultHeapEx(VPB_SIZE, 32);
         memset(sV[i].vpb, 0, VPB_SIZE);
         uint32_t idx = (uint32_t)i;
         memcpy(sV[i].vpb + VPB_INDEX, &idx, 4);
      }
   }
   ax_start();
}
void wii_AXInit(void) { wii_AXInitSpecifyMem(MAX_VOICES, NULL); }
int wii_AXIsInit(void) { return sInit; }
void wii_AXQuit(void) {}

void *wii_AXRegisterCallback(void *cb)
{
   int en = OSDisableInterrupts();
   void *old = sAxCallback;
   sAxCallback = cb;
   OSRestoreInterrupts(en);
   WHBLogPrintf("[ax] game frame callback %p", cb);
   return old;
}

static void *aux_swap(int i, void *cb) { void *old = sAuxCb[i]; sAuxCb[i] = cb; return old; }
void *wii_AXRegisterAuxACallback(void *cb, void *ctx) { return aux_swap(0, cb); } // effects: dry mix only
void *wii_AXRegisterAuxBCallback(void *cb, void *ctx) { return aux_swap(1, cb); }
void *wii_AXRegisterAuxCCallback(void *cb, void *ctx) { return aux_swap(2, cb); }
void wii_AXSetCompressor(uint32_t on) {}

void *wii_AXAcquireVoice(uint32_t prio, void *cb, uint32_t ctx)
{
   int en = OSDisableInterrupts();
   void *r = NULL;
   for (int i = 0; i < MAX_VOICES; i++) {
      Voice *v = &sV[i];
      if (v->vpb && !v->used) {
         uint8_t *b = v->vpb;
         memset(b + 0x0C, 0, VPB_SIZE - 0x0C);
         uint32_t idx = (uint32_t)i;
         memcpy(b + VPB_INDEX, &idx, 4);
         memcpy(b + VPB_PRIO, &prio, 4);
         memcpy(b + VPB_CB, &cb, 4);
         memcpy(b + VPB_CTX, &ctx, 4);
         memset((uint8_t *)v + sizeof(v->vpb), 0, sizeof(*v) - sizeof(v->vpb));
         v->used = 1;
         v->ratio = 0x10000;
         v->ve = 0x8000;
         r = b;
         break;
      }
   }
   OSRestoreInterrupts(en);
   static int logged;
   if (!r && !logged++) WHBLogPrintf("[ax] out of voices");
   return r;
}

void wii_AXFreeVoice(void *vpb)
{
   Voice *v = voice_of(vpb);
   if (!v) return;
   int en = OSDisableInterrupts();
   v->used = v->running = 0;
   wr16(v->vpb + PB_STATE, 0);
   OSRestoreInterrupts(en);
}

void wii_AXSetVoiceAddr(void *vpb, const uint8_t *a)
{
   Voice *v = voice_of(vpb);
   if (!v) return;
   memcpy(v->vpb + PB_ADDR, a, 16);
   v->loop = rd16(a);
   v->format = rd16(a + 2);
   v->loopAddr = rd32hl(a + 4);
   v->endAddr = rd32hl(a + 8);
   v->cur = rd32hl(a + 12);
   v->primed = 0;
   v->frac = 0;
}

void wii_AXSetVoiceAdpcm(void *vpb, const uint8_t *a)
{
   Voice *v = voice_of(vpb);
   if (!v) return;
   for (int i = 0; i < 16; i++) v->coef[i] = (int16_t)rd16(a + i * 2);
   v->gain = rd16(a + 0x20);
   v->ps = rd16(a + 0x22);
   v->yn1 = (int16_t)rd16(a + 0x24);
   v->yn2 = (int16_t)rd16(a + 0x26);
}

void wii_AXSetVoiceAdpcmLoop(void *vpb, const uint8_t *a)
{
   Voice *v = voice_of(vpb);
   if (!v) return;
   v->lps = rd16(a);
   v->lyn1 = (int16_t)rd16(a + 2);
   v->lyn2 = (int16_t)rd16(a + 4);
}

void wii_AXSetVoiceEndAddr(void *vpb, uint32_t addr)
{
   Voice *v = voice_of(vpb);
   if (!v) return;
   v->endAddr = addr;
   wr16(v->vpb + PB_ADDR + 8, (uint16_t)(addr >> 16));
   wr16(v->vpb + PB_ADDR + 10, (uint16_t)addr);
}

void wii_AXSetVoiceLoopAddr(void *vpb, uint32_t addr)
{
   Voice *v = voice_of(vpb);
   if (!v) return;
   v->loopAddr = addr;
   wr16(v->vpb + PB_ADDR + 4, (uint16_t)(addr >> 16));
   wr16(v->vpb + PB_ADDR + 6, (uint16_t)addr);
}

void wii_AXSetVoiceLoop(void *vpb, uint16_t loop)
{
   Voice *v = voice_of(vpb);
   if (!v) return;
   v->loop = loop;
   wr16(v->vpb + PB_ADDR, loop);
}

void wii_AXSetVoiceMix(void *vpb, const uint8_t *m)
{
   Voice *v = voice_of(vpb);
   if (!v) return;
   v->vL = rd16(m);
   v->dL = (int16_t)rd16(m + 2);
   v->vR = rd16(m + 4);
   v->dR = (int16_t)rd16(m + 6);
   memcpy(v->vpb + PB_MIX, m, 8);
}

void wii_AXSetVoiceSrc(void *vpb, const uint8_t *s)
{
   Voice *v = voice_of(vpb);
   if (!v) return;
   v->ratio = rd32hl(s);
   v->frac = rd16(s + 4);
}

void wii_AXSetVoiceSrcRatio(void *vpb, float ratio)
{
   Voice *v = voice_of(vpb);
   if (!v) return;
   if (ratio < 0) ratio = 0;
   if (ratio > 64.0f) ratio = 64.0f;
   v->ratio = (uint32_t)(ratio * 65536.0f);
}

void wii_AXSetVoiceSrcType(void *vpb, uint32_t type)
{
   Voice *v = voice_of(vpb);
   if (v) v->srcType = (int)type;
}

void wii_AXSetVoiceState(void *vpb, uint16_t state)
{
   Voice *v = voice_of(vpb);
   if (!v) return;
   v->running = state != 0;
   wr16(v->vpb + PB_STATE, state);
}

void wii_AXSetVoiceType(void *vpb, uint16_t type)
{
   Voice *v = voice_of(vpb);
   if (!v) return;
   v->stream = type != 0;
   wr16(v->vpb + PB_TYPE, type);
}

void wii_AXSetVoiceVe(void *vpb, const uint8_t *ve)
{
   Voice *v = voice_of(vpb);
   if (!v) return;
   v->ve = rd16(ve);
   v->veDelta = (int16_t)rd16(ve + 2);
}

// Audio interface and the Wii Remote speaker: nothing to drive.
void wii_AIInit(void *stack) {}
void *wii_AIRegisterDMACallback(void *cb) { return NULL; }
