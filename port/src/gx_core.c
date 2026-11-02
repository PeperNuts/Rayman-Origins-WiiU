// Rayman Origins Wii U port - GX front end (runs on the game's core).
//
// The game's FIFO stores are redirected into a byte stream (tools/wgp_patch.py), and GXBegin
// writes real GX primitive headers into it, so the stream - and every display list the game
// records - has the Wii's own command format. Before any GX state changes, the pending stream
// is parsed with the state that was current when it was written: each primitive is decoded
// (vertex formats, indexed arrays, position/texture matrices, colour channels) into
// float vertices in a frame packet, with a snapshot of the TEV/blend/texture state.
// GXCopyDisp hands the packet to the GX2 back end (gx_render.c).
#include <coreinit/debug.h>
#include <coreinit/memdefaultheap.h>
#include <coreinit/spinlock.h>
#include <whb/log.h>

#include <math.h>
#include <stdarg.h>
#include <coreinit/time.h>
#include <stdio.h>
#include <string.h>

#include "gx.h"
#include "port.h"

// ---------------------------------------------------------------------------------------
// Enumerations (Wii SDK values = hardware values)
// ---------------------------------------------------------------------------------------
enum { VA_PNMTXIDX = 0, VA_TEX0MTXIDX = 1, VA_POS = 9, VA_NRM = 10, VA_CLR0 = 11, VA_CLR1 = 12, VA_TEX0 = 13,
       VA_NBT = 25, VA_MAX = 26, VA_NULL = 0xFF };
enum { AT_NONE, AT_DIRECT, AT_INDEX8, AT_INDEX16 };
enum { CT_U8, CT_S8, CT_U16, CT_S16, CT_F32 };
enum { CLR_RGB565, CLR_RGB8, CLR_RGBX8, CLR_RGBA4, CLR_RGBA6, CLR_RGBA8 };
enum { CC_CPREV, CC_APREV, CC_C0, CC_A0, CC_C1, CC_A1, CC_C2, CC_A2, CC_TEXC, CC_TEXA, CC_RASC, CC_RASA, CC_ONE,
       CC_HALF, CC_KONST, CC_ZERO };
enum { CA_APREV, CA_A0, CA_A1, CA_A2, CA_TEXA, CA_RASA, CA_KONST, CA_ZERO };

extern uint8_t *volatile port_wgp_ptr; // wgp.S, advanced by the FIFO trampolines
extern char wii_gx_wgp_window[];
extern char port_wgp_tramp[];

// ---------------------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------------------
typedef struct { uint8_t cnt, type, frac; } Vat;
typedef struct { uint8_t enable, ambSrc, matSrc; } ChanCtrl;
typedef struct { uint8_t func, src, mtx; } TexGen;

static struct {
   uint8_t vcd[VA_MAX];
   uint8_t nbt;
   Vat vat[8][VA_MAX];
   const uint8_t *array[VA_MAX];
   uint8_t stride[VA_MAX];
   float posMtx[10][12];
   float texMtx[10][12];
   uint8_t texMtxType[10];
   uint32_t curPosMtx;
   TexGen texGen[8];
   uint32_t numTexGens, numChans;
   ChanCtrl chan[4];        // COLOR0, ALPHA0, COLOR1, ALPHA1
   float mat[2][4], amb[2][4];
   uint8_t tevOrderMap[GX_MAX_STAGES];
   float projv[7];
   uint16_t dispSrc[4], texSrc[4], texDst[2];
   uint8_t texDstFmt;
   uint32_t copyClear, copyClearZ;
} G;

static struct { uint8_t on; uint16_t ss, ts; } sManual[8]; // GXSetTexCoordScaleManually
static GxState sCur;       // back end state being built
static int sDirty = 1;

enum { MODE_FIFO, MODE_DL, MODE_REDIRECT };
static int sMode;
static uint8_t *sFifo, *sParse, *sSaved, *sDlStart;
#define FIFO_SIZE (8u << 20)

// packets
#define NPACKETS 3
enum { PK_FREE, PK_FILLING, PK_READY, PK_RENDERING };
static GxPacket sPk[NPACKETS];
static int sPkState[NPACKETS];
static GxPacket *sFill;
static OSSpinLock sPkLock;

static uint32_t sStats[8]; // prims, verts, dropped, unknown ops, packets, packets dropped, DL calls, copies
#define RT_SLOTS 4096
static uint32_t sRenderTex[RT_SLOTS]; // GXCopyTex destinations (open-addressing set; never decoded)
static uint32_t sNumRenderTex;

uint32_t gx_stat(int i) { return (i >= 0 && i < 8) ? sStats[i] : 0; }


static void flush(void);
// Breadcrumbs for the watchdog: last GX entry point, call count, parser activity.
volatile const char *gx_last_api = "-";
volatile uint32_t gx_api_calls, gx_in_flush, gx_flushes;
// Start-up tracing: logged, then a 30 ms busy wait so the line reaches the PC before a hang.
static void gx_trace(const char *fmt, ...)
{
   char b[200];
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(b, sizeof(b), fmt, ap);
   va_end(ap);
   WHBLogPrintf("%s", b);
   OSTime t = OSGetSystemTime();
   while (OSGetSystemTime() - t < OSMillisecondsToTicks(30)) {}
}
#define TRACE_N(n, ...) do { static int _c; if (_c < (n)) { _c++; gx_trace(__VA_ARGS__); } } while (0)
#define FLUSH() do { gx_last_api = __func__; gx_api_calls++; \
   TRACE_N(1, "[gxcall] %s (r13 %08X, stream %p, mode %d)", __func__, (unsigned)read_r13(), port_wgp_ptr, sMode); \
   if (sMode == MODE_FIFO && port_wgp_ptr != sParse) flush(); } while (0)
#define DIRTY() (sDirty = 1)

static inline uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static inline uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static inline float bef(const uint8_t *p)
{
   uint32_t u = be32(p);
   float f;
   memcpy(&f, &u, 4);
   return f;
}
static void color_from(float out[4], const uint8_t *rgba)
{
   for (int i = 0; i < 4; i++) out[i] = rgba[i] / 255.0f;
}

// ---------------------------------------------------------------------------------------
// Packets
// ---------------------------------------------------------------------------------------
static void packet_alloc(GxPacket *p)
{
   p->maxVerts = 96 * 1024;
   p->maxStates = 4096;
   p->maxCmds = 16384;
   p->verts = MEMAllocFromDefaultHeapEx(p->maxVerts * sizeof(GxVertex), 256);
   p->states = MEMAllocFromDefaultHeapEx(p->maxStates * sizeof(GxState), 64);
   p->cmds = MEMAllocFromDefaultHeapEx(p->maxCmds * sizeof(GxCmd), 64);
   if (!p->verts || !p->states || !p->cmds) OSFatal("Rayman port: out of memory for GX packets");
}

static void packet_begin(GxPacket *p)
{
   p->numVerts = p->numStates = p->numCmds = 0;
   p->dropped = 0;
   p->clearAfter = 0;
   sDirty = 1;
}

static void submit_packet(void)
{
   OSUninterruptibleSpinLock_Acquire(&sPkLock);
   int cur = (int)(sFill - sPk), next = -1;
   for (int i = 0; i < NPACKETS; i++) {
      if (sPkState[i] == PK_READY) { sPkState[i] = PK_FREE; sStats[5]++; } // render thread fell behind
   }
   sPkState[cur] = PK_READY;
   for (int i = 0; i < NPACKETS; i++) if (sPkState[i] == PK_FREE) { next = i; break; }
   sPkState[next] = PK_FILLING;
   OSUninterruptibleSpinLock_Release(&sPkLock);
   sStats[4]++;
   sFill = &sPk[next];
   packet_begin(sFill);
}

GxPacket *gx_acquire_packet(void)
{
   GxPacket *r = NULL;
   OSUninterruptibleSpinLock_Acquire(&sPkLock);
   for (int i = 0; i < NPACKETS; i++) {
      if (sPkState[i] == PK_READY) { sPkState[i] = PK_RENDERING; r = &sPk[i]; break; }
   }
   OSUninterruptibleSpinLock_Release(&sPkLock);
   return r;
}

void gx_release_packet(GxPacket *p)
{
   OSUninterruptibleSpinLock_Acquire(&sPkLock);
   sPkState[p - sPk] = PK_FREE;
   OSUninterruptibleSpinLock_Release(&sPkLock);
}

static GxCmd *new_cmd(uint32_t type)
{
   GxPacket *p = sFill;
   if (p->numCmds >= p->maxCmds) return NULL;
   GxCmd *c = &p->cmds[p->numCmds++];
   memset(c, 0, sizeof(*c));
   c->type = type;
   return c;
}

// ---------------------------------------------------------------------------------------
// Defaults (what GXInit sets up)
// ---------------------------------------------------------------------------------------
static void stage_inputs(int s, int a, int b, int c, int d, int aa, int ab, int ac, int ad)
{
   float *ci = sCur.stage[s * 7 + 0], *ai = sCur.stage[s * 7 + 1];
   ci[0] = a; ci[1] = b; ci[2] = c; ci[3] = d;
   ai[0] = aa; ai[1] = ab; ai[2] = ac; ai[3] = ad;
}

static void stage_ops_default(int s)
{
   float *co = sCur.stage[s * 7 + 2], *ao = sCur.stage[s * 7 + 3], *o = sCur.stage[s * 7 + 4];
   co[0] = ao[0] = 0; co[1] = ao[1] = 0; co[2] = ao[2] = 0; co[3] = ao[3] = 1;
   o[0] = o[1] = 0;
}

static void tev_op(int s, int mode)
{
   switch (mode) {
   case 0: stage_inputs(s, CC_ZERO, CC_TEXC, CC_RASC, CC_ZERO, CA_ZERO, CA_TEXA, CA_RASA, CA_ZERO); break; // MODULATE
   case 1: stage_inputs(s, CC_RASC, CC_TEXC, CC_TEXA, CC_ZERO, CA_ZERO, CA_ZERO, CA_ZERO, CA_RASA); break; // DECAL
   case 2: stage_inputs(s, CC_RASC, CC_ONE, CC_TEXC, CC_ZERO, CA_ZERO, CA_TEXA, CA_RASA, CA_ZERO); break;  // BLEND
   case 3: stage_inputs(s, CC_ZERO, CC_ZERO, CC_ZERO, CC_TEXC, CA_ZERO, CA_ZERO, CA_ZERO, CA_TEXA); break; // REPLACE
   default: // PASSCLR
      stage_inputs(s, CC_ZERO, CC_ZERO, CC_ZERO, s ? CC_CPREV : CC_RASC, CA_ZERO, CA_ZERO, CA_ZERO, s ? CA_APREV : CA_RASA);
      break;
   }
   stage_ops_default(s);
}

static void defaults(void)
{
   memset(&G, 0, sizeof(G));
   memset(&sCur, 0, sizeof(sCur));
   for (int i = 0; i < 10; i++) {
      G.posMtx[i][0] = G.posMtx[i][5] = G.posMtx[i][10] = 1;
      G.texMtx[i][0] = G.texMtx[i][5] = G.texMtx[i][10] = 1;
   }
   for (int i = 0; i < 8; i++) G.texGen[i] = (TexGen){1, 4 + i, 60};
   G.numChans = 1;
   G.numTexGens = 1;
   for (int i = 0; i < 2; i++)
      for (int k = 0; k < 4; k++) G.mat[i][k] = G.amb[i][k] = 1.0f;
   G.chan[0].matSrc = G.chan[1].matSrc = 1; // vertex colour
   for (int s = 0; s < GX_MAX_STAGES; s++) {
      tev_op(s, s ? 4 : 0);
      float *o = sCur.stage[s * 7 + 4], *t = sCur.stage[s * 7 + 5], *w = sCur.stage[s * 7 + 6];
      o[2] = 0x0C; o[3] = 0x1C;                                  // KCSEL_K0 / KASEL_K0_A
      t[0] = s == 0 ? 0 : -1; t[1] = s == 0 ? 0 : -1; t[2] = 0; t[3] = 0;
      w[0] = 0;
   }
   static const float swap[4][4] = {{0, 1, 2, 3}, {0, 0, 0, 3}, {1, 1, 1, 3}, {2, 2, 2, 3}};
   memcpy(sCur.swap, swap, sizeof(swap));
   sCur.misc[0][0] = 1;                           // one stage
   sCur.misc[0][1] = 7; sCur.misc[0][3] = 0;      // ALWAYS, AND, ALWAYS
   sCur.misc[1][0] = 7;
   for (int i = 0; i < 4; i++) sCur.konst[i][3] = 1.0f;
   sCur.colorUpdate = sCur.alphaUpdate = 1;
   sCur.zEnable = 1; sCur.zFunc = 3; sCur.zUpdate = 1;
   sCur.viewport[2] = 640; sCur.viewport[3] = 456; sCur.viewport[5] = 1;
   sCur.scissor[2] = 640; sCur.scissor[3] = 456;
   for (int i = 0; i < 4; i++) sCur.proj[i * 5] = 1;
   G.dispSrc[2] = 640; G.dispSrc[3] = 456;
   G.copyClear = 0x000000FF;
   G.copyClearZ = 0xFFFFFF;
}

void gx_core_init(void)
{
   OSInitSpinLock(&sPkLock);
   defaults();
   sFifo = MEMAllocFromDefaultHeapEx(FIFO_SIZE + 0x10000, 64);
   if (!sFifo) OSFatal("Rayman port: out of memory for the GX FIFO");
   for (int i = 0; i < NPACKETS; i++) packet_alloc(&sPk[i]);
   sFill = &sPk[0];
   sPkState[0] = PK_FILLING;
   packet_begin(sFill);
   sParse = sFifo;
   port_wgp_ptr = sFifo;
   sMode = MODE_FIFO;
   WHBLogPrintf("[gx] FIFO capture at %p (trampolines %p), packets %u KiB each", sFifo, port_wgp_tramp,
                (unsigned)((sPk[0].maxVerts * sizeof(GxVertex)) >> 10));
}

// ---------------------------------------------------------------------------------------
// Vertex decoding
// ---------------------------------------------------------------------------------------
static const uint8_t kCompSize[5] = {1, 1, 2, 2, 4};
static const uint8_t kClrSize[6] = {2, 3, 4, 2, 3, 4};

static uint32_t attr_direct_size(int fmt, int a)
{
   const Vat *v = &G.vat[fmt][a];
   if (a == VA_POS) return (v->cnt ? 3 : 2) * kCompSize[v->type > 4 ? 4 : v->type];
   if (a == VA_NRM) return (G.nbt || v->cnt) ? 9 * kCompSize[v->type > 4 ? 4 : v->type] : 3 * kCompSize[v->type > 4 ? 4 : v->type];
   if (a == VA_CLR0 || a == VA_CLR1) return kClrSize[v->type > 5 ? 5 : v->type];
   return (v->cnt ? 2 : 1) * kCompSize[v->type > 4 ? 4 : v->type]; // texcoords
}

static uint32_t vertex_size(int fmt)
{
   uint32_t n = 0;
   for (int a = 0; a <= 20; a++) {
      uint8_t t = G.vcd[a];
      if (t == AT_NONE) continue;
      if (a < VA_POS) { n += 1; continue; } // matrix indices are always direct u8
      if (t == AT_INDEX8) n += 1;
      else if (t == AT_INDEX16) n += 2;
      else n += attr_direct_size(fmt, a);
   }
   return n;
}

static float comp(const uint8_t *p, int type, float scale)
{
   switch (type) {
   case CT_U8: return p[0] * scale;
   case CT_S8: return (int8_t)p[0] * scale;
   case CT_U16: return be16(p) * scale;
   case CT_S16: return (int16_t)be16(p) * scale;
   default: return bef(p);
   }
}

// Reads attribute `a` (direct or via its array) and returns a pointer to its components.
static const uint8_t *attr_src(int a, const uint8_t **pp)
{
   const uint8_t *p = *pp;
   switch (G.vcd[a]) {
   case AT_DIRECT: return p; // caller advances
   case AT_INDEX8: *pp = p + 1; return G.array[a] ? G.array[a] + (uint32_t)p[0] * G.stride[a] : NULL;
   case AT_INDEX16: *pp = p + 2; return G.array[a] ? G.array[a] + (uint32_t)be16(p) * G.stride[a] : NULL;
   }
   return NULL;
}

static void read_color(const uint8_t *s, int type, float out[4])
{
   uint32_t v;
   switch (type) {
   case CLR_RGB565: v = be16(s); out[0] = ((v >> 11) & 31) / 31.0f; out[1] = ((v >> 5) & 63) / 63.0f; out[2] = (v & 31) / 31.0f; out[3] = 1; break;
   case CLR_RGB8: case CLR_RGBX8: out[0] = s[0] / 255.0f; out[1] = s[1] / 255.0f; out[2] = s[2] / 255.0f; out[3] = 1; break;
   case CLR_RGBA4: v = be16(s); out[0] = (v >> 12) / 15.0f; out[1] = ((v >> 8) & 15) / 15.0f; out[2] = ((v >> 4) & 15) / 15.0f; out[3] = (v & 15) / 15.0f; break;
   case CLR_RGBA6: v = (uint32_t)s[0] << 16 | s[1] << 8 | s[2];
      out[0] = (v >> 18) / 63.0f; out[1] = ((v >> 12) & 63) / 63.0f; out[2] = ((v >> 6) & 63) / 63.0f; out[3] = (v & 63) / 63.0f; break;
   default: out[0] = s[0] / 255.0f; out[1] = s[1] / 255.0f; out[2] = s[2] / 255.0f; out[3] = s[3] / 255.0f; break;
   }
}

static void mtx_apply(const float *m, const float v[4], float out[3], int rows)
{
   for (int r = 0; r < rows; r++) out[r] = m[r * 4] * v[0] + m[r * 4 + 1] * v[1] + m[r * 4 + 2] * v[2] + m[r * 4 + 3] * v[3];
}

static void decode_vertex(int fmt, const uint8_t **pp, GxVertex *o)
{
   const uint8_t *p = *pp;
   uint32_t pm = G.curPosMtx;
   int tmIdx[8];
   for (int i = 0; i < 8; i++) tmIdx[i] = -1;
   if (G.vcd[VA_PNMTXIDX]) pm = *p++ / 3;
   for (int i = 0; i < 8; i++)
      if (G.vcd[VA_TEX0MTXIDX + i]) tmIdx[i] = *p++;

   float pos[4] = {0, 0, 0, 1}, clr[2][4] = {{1, 1, 1, 1}, {1, 1, 1, 1}}, tex[8][2] = {{0}};
   for (int a = VA_POS; a <= 20; a++) {
      if (G.vcd[a] == AT_NONE) continue;
      const Vat *v = &G.vat[fmt][a];
      const uint8_t *s = attr_src(a, &p);
      uint32_t dsz = attr_direct_size(fmt, a);
      if (G.vcd[a] == AT_DIRECT) p += dsz;
      if (!s) continue;
      float scale = 1.0f / (float)(1u << v->frac);
      int cs = kCompSize[v->type > 4 ? 4 : v->type];
      if (a == VA_POS) {
         pos[0] = comp(s, v->type, scale);
         pos[1] = comp(s + cs, v->type, scale);
         if (v->cnt) pos[2] = comp(s + 2 * cs, v->type, scale);
      } else if (a == VA_CLR0 || a == VA_CLR1) {
         read_color(s, v->type, clr[a - VA_CLR0]);
      } else if (a >= VA_TEX0) {
         int t = a - VA_TEX0;
         tex[t][0] = comp(s, v->type, scale);
         if (v->cnt) tex[t][1] = comp(s + cs, v->type, scale);
      }
   }
   *pp = p;

   if (pm > 9) pm = 0;
   mtx_apply(G.posMtx[pm], pos, o->pos, 3);
   o->pos[3] = 1.0f;

   // colour channels (lighting approximated as material x ambient)
   for (int c = 0; c < 2; c++) {
      float *out = c ? o->c1 : o->c0;
      if ((uint32_t)c >= G.numChans) { out[0] = out[1] = out[2] = out[3] = 0; continue; }
      const ChanCtrl *cc = &G.chan[c * 2], *ac = &G.chan[c * 2 + 1];
      for (int k = 0; k < 4; k++) {
         const ChanCtrl *ctl = k < 3 ? cc : ac;
         float m = ctl->matSrc ? clr[c][k] : G.mat[c][k];
         if (ctl->enable) m *= ctl->ambSrc ? clr[c][k] : G.amb[c][k];
         out[k] = m;
      }
   }

   // texture coordinate generation
   for (uint32_t i = 0; i < GX_MAX_COORDS; i++) {
      float *tc = o->tc[i];
      tc[0] = tc[1] = 0; tc[2] = 1; tc[3] = 0;
      if (i >= G.numTexGens) continue;
      const TexGen *tg = &G.texGen[i];
      float in[4] = {0, 0, 1, 1};
      if (tg->src >= 4 && tg->src <= 11) { in[0] = tex[tg->src - 4][0]; in[1] = tex[tg->src - 4][1]; }
      else if (tg->src == 0) { in[0] = pos[0]; in[1] = pos[1]; in[2] = pos[2]; }
      else if (tg->src == 19 || tg->src == 20) { in[0] = o->c0[0]; in[1] = o->c0[1]; }
      else continue;
      int mtx = tmIdx[i] >= 0 ? tmIdx[i] : tg->mtx;
      if (tg->func > 1 || mtx >= 60 || mtx < 30) { tc[0] = in[0]; tc[1] = in[1]; continue; }
      int mi = (mtx - 30) / 3;
      float r[3];
      mtx_apply(G.texMtx[mi], in, r, tg->func == 0 ? 3 : 2);
      tc[0] = r[0]; tc[1] = r[1]; tc[2] = tg->func == 0 ? r[2] : 1.0f;
   }
}

// ---------------------------------------------------------------------------------------
// Primitives -> triangle lists in the packet
// ---------------------------------------------------------------------------------------
static void push_state(void)
{
   GxPacket *pk = sFill;
   if (!sDirty && pk->numStates) return;
   // per-stage texture coordinate scale (manual scaling), applied in the shader
   for (int st = 0; st < GX_MAX_STAGES; st++) {
      float *t = sCur.stage[st * 7 + 5], *w = sCur.stage[st * 7 + 6];
      int slot = (int)(t[0] + (t[0] < 0 ? -0.5f : 0.5f)), tc = (int)(t[1] + (t[1] < 0 ? -0.5f : 0.5f));
      w[1] = w[2] = 1.0f;
      if (slot >= 0 && slot < GX_MAX_SLOTS && tc >= 0 && tc < 8 && sManual[tc].on && sCur.tex[slot].width && sCur.tex[slot].height) {
         w[1] = (float)sManual[tc].ss / sCur.tex[slot].width;
         w[2] = (float)sManual[tc].ts / sCur.tex[slot].height;
      }
   }
   if (pk->numStates >= pk->maxStates) return;
   pk->states[pk->numStates++] = sCur;
   sDirty = 0;
}

// ---------------------------------------------------------------------------------------
// Draw census: the first draw of every distinct state configuration is logged once, with
// its first decoded vertex, so invisible objects can be traced to their state.
// ---------------------------------------------------------------------------------------
static int sParseFromDL;
volatile uint32_t gx_nan_verts;

static uint32_t fnv(uint32_t h, const void *p, uint32_t n)
{
   const uint8_t *b = p;
   for (uint32_t i = 0; i < n; i++) h = (h ^ b[i]) * 16777619u;
   return h;
}

static void census(int fmt, const GxVertex *v)
{
   static uint32_t seen[96];
   static int nseen;
   if (nseen >= 96) return;
   int ns = (int)sCur.misc[0][0];
   if (ns > GX_MAX_STAGES) ns = GX_MAX_STAGES;
   uint32_t h = 2166136261u;
   h = fnv(h, sCur.stage, (uint32_t)ns * 7 * 16);
   h = fnv(h, sCur.misc, sizeof(sCur.misc));
   h = fnv(h, &sCur.blendType, 10);
   h = fnv(h, G.chan, sizeof(G.chan));
   h = fnv(h, &G.numChans, 4);
   h = fnv(h, &G.numTexGens, 4);
   h = fnv(h, G.vcd, 21);
   h = fnv(h, &fmt, 4);
   h = fnv(h, &sParseFromDL, 4);
   for (int i = 0; i < nseen; i++) if (seen[i] == h) return;
   seen[nseen++] = h;
   const float *m = sCur.proj;
   float cx = m[0] * v->pos[0] + m[1] * v->pos[1] + m[2] * v->pos[2] + m[3];
   float cy = m[4] * v->pos[0] + m[5] * v->pos[1] + m[6] * v->pos[2] + m[7];
   float cz = m[8] * v->pos[0] + m[9] * v->pos[1] + m[10] * v->pos[2] + m[11];
   float cw = m[12] * v->pos[0] + m[13] * v->pos[1] + m[14] * v->pos[2] + m[15];
   char vcd[32];
   for (int a = 0; a < 21; a++) vcd[a] = (char)('0' + G.vcd[a]);
   vcd[21] = 0;
   WHBLogPrintf("[census] #%d %s fmt %d vcd %s | stages %d blend %u/%u/%u z %u/%u/%u upd %u/%u | acmp %.0f %.2f %.0f %.0f %.2f",
                nseen, sParseFromDL ? "DL" : "fifo", fmt, vcd, ns, sCur.blendType, sCur.blendSrc, sCur.blendDst, sCur.zEnable,
                sCur.zFunc, sCur.zUpdate, sCur.colorUpdate, sCur.alphaUpdate, sCur.misc[0][1], sCur.misc[0][2], sCur.misc[0][3],
                sCur.misc[1][0], sCur.misc[1][1]);
   WHBLogPrintf("[census]   chans %u c0 %u/%u/%u a0 %u/%u/%u mat0 %.2f %.2f %.2f %.2f amb0 %.2f %.2f %.2f %.2f | texgens %u tg0 %u/%u/%u tg1 %u/%u/%u",
                (unsigned)G.numChans, G.chan[0].enable, G.chan[0].ambSrc, G.chan[0].matSrc, G.chan[1].enable, G.chan[1].ambSrc,
                G.chan[1].matSrc, G.mat[0][0], G.mat[0][1], G.mat[0][2], G.mat[0][3], G.amb[0][0], G.amb[0][1], G.amb[0][2],
                G.amb[0][3], (unsigned)G.numTexGens, G.texGen[0].func, G.texGen[0].src, G.texGen[0].mtx, G.texGen[1].func,
                G.texGen[1].src, G.texGen[1].mtx);
   for (int s = 0; s < ns && s < 3; s++) {
      const float (*q)[4] = &sCur.stage[s * 7];
      WHBLogPrintf("[census]   st%d c %.0f %.0f %.0f %.0f op %.0f/%.0f/%.0f/%.0f a %.0f %.0f %.0f %.0f op %.0f/%.0f/%.0f/%.0f out %.0f/%.0f k %.0f/%.0f tex %.0f coord %.0f chan %.0f swap %.0f/%.0f",
                   s, q[0][0], q[0][1], q[0][2], q[0][3], q[2][0], q[2][1], q[2][2], q[2][3], q[1][0], q[1][1], q[1][2], q[1][3],
                   q[3][0], q[3][1], q[3][2], q[3][3], q[4][0], q[4][1], q[4][2], q[4][3], q[5][0], q[5][1], q[5][2], q[5][3],
                   q[6][0]);
   }
   const GxTexKey *t = &sCur.tex[0];
   WHBLogPrintf("[census]   v0 clip %.2f %.2f %.2f w %.2f | c0 %.2f %.2f %.2f %.2f c1 %.2f %.2f %.2f %.2f | tc0 %.2f %.2f %.2f | tex0 %08X %ux%u f%u | regs0 %.2f %.2f %.2f %.2f",
                cx, cy, cz, cw, v->c0[0], v->c0[1], v->c0[2], v->c0[3], v->c1[0], v->c1[1], v->c1[2], v->c1[3], v->tc[0][0],
                v->tc[0][1], v->tc[0][2], (unsigned)t->image, t->width, t->height, t->fmt, sCur.regs[0][0], sCur.regs[0][1],
                sCur.regs[0][2], sCur.regs[0][3]);
}

static void draw_prim(int prim, int fmt, const uint8_t *data, uint32_t n)
{
   TRACE_N(8, "[gx] prim %02X fmt %d n %u vsize %u data %p", prim, fmt, (unsigned)n, (unsigned)vertex_size(fmt), data);
   GxPacket *pk = sFill;
   uint32_t need;
   switch (prim) {
   case 0x80: case 0x88: need = n / 4 * 6; break;
   case 0x90: need = n / 3 * 3; break;
   case 0x98: case 0xA0: need = n >= 3 ? (n - 2) * 3 : 0; break;
   default: return; // lines and points: not drawn yet
   }
   sStats[0]++;
   if (!need) return;
   push_state();
   if (pk->numVerts + need > pk->maxVerts || pk->numStates == 0) { pk->dropped++; sStats[2]++; return; }

   GxCmd *last = pk->numCmds ? &pk->cmds[pk->numCmds - 1] : NULL;
   uint32_t st = pk->numStates - 1;
   if (!last || last->type != GXCMD_DRAW || last->state != st || last->first + last->count != pk->numVerts) {
      last = new_cmd(GXCMD_DRAW);
      if (!last) { pk->dropped++; sStats[2]++; return; }
      last->state = st;
      last->first = pk->numVerts;
   }

   GxVertex *out = pk->verts + pk->numVerts;
   GxVertex v0, v1, v2;
   const uint8_t *p = data;
   uint32_t emitted = 0;
   for (uint32_t i = 0; i < n; i++) {
      GxVertex v;
      decode_vertex(fmt, &p, &v);
      if (v.pos[0] != v.pos[0] || v.pos[1] != v.pos[1] || v.pos[2] != v.pos[2]) { // NaN: collapse (invisible)
         v.pos[0] = v.pos[1] = v.pos[2] = 0;
         gx_nan_verts++;
      }
      if (i == 0) census(fmt, &v);
      switch (prim) {
      case 0x80: case 0x88: // quads
         switch (i & 3) {
         case 0: v0 = v; break;
         case 1: v1 = v; break;
         case 2: v2 = v; break;
         case 3: out[0] = v0; out[1] = v1; out[2] = v2; out[3] = v0; out[4] = v2; out[5] = v; out += 6; emitted += 6; break;
         }
         break;
      case 0x90:
         *out++ = v; emitted++;
         break;
      case 0x98: // strip
         if (i >= 2) {
            if (i & 1) { out[0] = v1; out[1] = v0; } else { out[0] = v0; out[1] = v1; }
            out[2] = v; out += 3; emitted += 3;
         }
         v0 = v1; v1 = v;
         break;
      case 0xA0: // fan
         if (i == 0) v0 = v;
         else if (i >= 2) { out[0] = v0; out[1] = v1; out[2] = v; out += 3; emitted += 3; }
         v1 = v;
         break;
      }
   }
   if (prim == 0x90) emitted -= emitted % 3;
   pk->numVerts += emitted;
   last->count += emitted;
   sStats[1] += n;
}

// Parses GX commands in [p, end). Returns where it stopped (an incomplete primitive).
static const uint8_t *parse(const uint8_t *p, const uint8_t *end)
{
   while (p < end) {
      uint8_t op = *p;
      if (op & 0x80) {
         if (end - p < 3) return p;
         uint32_t n = be16(p + 1), vs = vertex_size(op & 7);
         if ((uint32_t)(end - p - 3) < n * vs) return p;
         draw_prim(op & 0xF8, op & 7, p + 3, n);
         p += 3 + n * vs;
         continue;
      }
      switch (op) {
      case 0x00: case 0x48: p += 1; break;          // NOP, invalidate vertex cache
      case 0x08: p += 6; break;                     // CP register
      case 0x10: if (end - p < 5) return p; p += 5 + 4 * ((be32(p + 1) >> 16) + 1); break; // XF registers
      case 0x20: case 0x28: case 0x30: case 0x38: p += 5; break; // indexed XF loads
      case 0x40: p += 9; break;                     // call display list (game uses GXCallDisplayList)
      case 0x61: p += 5; break;                     // BP register
      default:
         if (!sStats[3]++) WHBLogPrintf("[gx] unknown FIFO command %02X - rest of the batch skipped", op);
         return end;
      }
   }
   return end;
}

static void flush(void)
{
   TRACE_N(8, "[gx] flush %d bytes at %p", (int)(port_wgp_ptr - sParse), sParse);
   gx_in_flush = 1;
   gx_flushes++;
   uint8_t *end = port_wgp_ptr;
   if (end < sParse || end > sFifo + FIFO_SIZE) { // should not happen: resynchronise
      sParse = port_wgp_ptr = sFifo;
      gx_in_flush = 0;
      return;
   }
   static int noDecode = -1;
   if (noDecode < 0) noDecode = port_cfg("nodecode");
   const uint8_t *stop = noDecode ? end : parse(sParse, end);
   if (stop == end) {
      sParse = port_wgp_ptr = sFifo;
   } else {
      uint32_t rest = (uint32_t)(end - stop);
      memmove(sFifo, stop, rest);
      sParse = sFifo;
      port_wgp_ptr = sFifo + rest;
   }
   gx_in_flush = 0;
}

// ---------------------------------------------------------------------------------------
// GX API - FIFO, display lists, synchronisation
// ---------------------------------------------------------------------------------------
void wii_GXBegin(uint32_t prim, uint32_t fmt, uint16_t n)
{
   FLUSH();
   TRACE_N(8, "[gx] GXBegin %02X fmt %u n %u at %p (mode %d)", (unsigned)prim, (unsigned)fmt, (unsigned)n, port_wgp_ptr, sMode);
   uint8_t *p = port_wgp_ptr;
   while (((uint32_t)p + 3) & 3) *p++ = 0; // GX NOPs so the vertex data starts word-aligned
   p[0] = (uint8_t)(prim | (fmt & 7));
   p[1] = (uint8_t)(n >> 8);
   p[2] = (uint8_t)n;
   port_wgp_ptr = p + 3;
}

void wii_GXBeginDisplayList(void *list, uint32_t size)
{
   FLUSH();
   TRACE_N(8, "[gx] GXBeginDisplayList %p size %u", list, (unsigned)size);
   sSaved = port_wgp_ptr;
   sDlStart = list;
   port_wgp_ptr = list;
   sMode = MODE_DL;
}

uint32_t wii_GXEndDisplayList(void)
{
   TRACE_N(8, "[gx] GXEndDisplayList at %p (start %p)", port_wgp_ptr, sDlStart);
   uint8_t *p = port_wgp_ptr;
   while ((uint32_t)(p - sDlStart) & 31) *p++ = 0;
   uint32_t n = (uint32_t)(p - sDlStart);
   port_wgp_ptr = sSaved;
   sMode = MODE_FIFO;
   return n;
}

void wii_GXCallDisplayList(const void *list, uint32_t nbytes)
{
   FLUSH();
   TRACE_N(8, "[gx] GXCallDisplayList %p %u bytes", list, (unsigned)nbytes);
   sStats[6]++;
   sParseFromDL = 1;
   parse(list, (const uint8_t *)list + nbytes);
   sParseFromDL = 0;
}

void *wii_GXRedirectWriteGatherPipe(void *ptr)
{
   FLUSH();
   TRACE_N(8, "[gx] GXRedirectWriteGatherPipe %p (stream was %p)", ptr, port_wgp_ptr);
   sSaved = port_wgp_ptr;
   port_wgp_ptr = ptr;
   sMode = MODE_REDIRECT;
   return wii_gx_wgp_window;
}

void wii_GXRestoreWriteGatherPipe(void)
{
   TRACE_N(8, "[gx] GXRestoreWriteGatherPipe: wrote %p, back to %p", port_wgp_ptr, sSaved);
   port_wgp_ptr = sSaved;
   sMode = MODE_FIFO;
}

void wii_GXFlush(void) { FLUSH(); }
void wii_GXDrawDone(void) { FLUSH(); }
void wii_GXSetDrawDone(void) { FLUSH(); }
void wii_GXPixModeSync(void) { FLUSH(); }
void wii_GXInvalidateVtxCache(void) {}
void wii_GXInvalidateTexAll(void) {}

// ---------------------------------------------------------------------------------------
// Vertex formats and arrays
// ---------------------------------------------------------------------------------------
static void set_vcd(uint32_t attr, uint32_t type)
{
   if (attr == VA_NBT) { G.vcd[VA_NRM] = (uint8_t)type; G.nbt = type != AT_NONE; }
   else if (attr < VA_MAX) { G.vcd[attr] = (uint8_t)type; if (attr == VA_NRM) G.nbt = 0; }
}

void wii_GXSetVtxDesc(uint32_t attr, uint32_t type) { FLUSH(); set_vcd(attr, type); }
void wii_GXClearVtxDesc(void) { FLUSH(); memset(G.vcd, 0, sizeof(G.vcd)); G.nbt = 0; }

typedef struct { uint32_t attr, type; } VtxDesc;
void wii_GXSetVtxDescv(const VtxDesc *l)
{
   FLUSH();
   for (; l->attr != VA_NULL; l++) set_vcd(l->attr, l->type);
}

void wii_GXGetVtxDescv(VtxDesc *l)
{
   for (uint32_t a = 0; a < VA_MAX; a++) {
      l[a].attr = a;
      l[a].type = a == VA_NBT ? (G.nbt ? G.vcd[VA_NRM] : 0) : (a == VA_NRM && G.nbt) ? 0 : (a < VA_MAX ? G.vcd[a] : 0);
   }
   l[VA_MAX].attr = VA_NULL;
   l[VA_MAX].type = 0;
}

void wii_GXSetVtxAttrFmt(uint32_t fmt, uint32_t attr, uint32_t cnt, uint32_t type, uint8_t frac)
{
   FLUSH();
   if (fmt > 7) return;
   if (attr == VA_NBT) { attr = VA_NRM; cnt = 1; }
   if (attr >= VA_MAX) return;
   G.vat[fmt][attr] = (Vat){(uint8_t)cnt, (uint8_t)type, frac};
}

void wii_GXSetArray(uint32_t attr, const void *base, uint8_t stride)
{
   FLUSH();
   if (attr == VA_NBT) attr = VA_NRM;
   if (attr < VA_MAX) { G.array[attr] = base; G.stride[attr] = stride; }
}

// ---------------------------------------------------------------------------------------
// Transforms
// ---------------------------------------------------------------------------------------
void wii_GXLoadPosMtxImm(const float *m, uint32_t id)
{
   FLUSH();
   if (id / 3 < 10) memcpy(G.posMtx[id / 3], m, 12 * sizeof(float));
}

void wii_GXSetCurrentMtx(uint32_t id) { FLUSH(); G.curPosMtx = id / 3; }

void wii_GXLoadTexMtxImm(const float *m, uint32_t id, uint32_t type)
{
   FLUSH();
   if (id < 30 || id >= 60) return; // post-transform matrices: dual texture transform not used
   int i = (id - 30) / 3;
   memset(G.texMtx[i], 0, sizeof(G.texMtx[i]));
   memcpy(G.texMtx[i], m, (type == 1 ? 8 : 12) * sizeof(float));
   if (type == 1) G.texMtx[i][10] = 1;
   G.texMtxType[i] = (uint8_t)type;
}

void wii_GXSetTexCoordGen2(uint32_t dst, uint32_t func, uint32_t src, uint32_t mtx, int normalize, uint32_t post)
{
   FLUSH();
   if (dst < 8) G.texGen[dst] = (TexGen){(uint8_t)func, (uint8_t)src, (uint8_t)mtx};
}

void wii_GXSetNumTexGens(uint8_t n) { FLUSH(); G.numTexGens = n; }
// Manual scaling: the hardware uses s * ss texels instead of normalising by the texture size
// (Bink's YUV planes share coordinates across differently sized textures).
void wii_GXSetTexCoordScaleManually(uint32_t coord, int enable, uint16_t ss, uint16_t ts)
{
   FLUSH();
   if (coord < 8) { sManual[coord].on = enable != 0; sManual[coord].ss = ss; sManual[coord].ts = ts; DIRTY(); }
}
void wii_GXSetTexCoordCylWrap(uint32_t coord, int s, int t) { FLUSH(); }

static void proj_from_params(void)
{
   float *m = sCur.proj, *v = G.projv;
   memset(m, 0, 16 * sizeof(float));
   m[0] = v[1]; m[5] = v[3]; m[10] = v[5]; m[11] = v[6];
   if (v[0] == 0) { m[2] = v[2]; m[6] = v[4]; m[14] = -1; } // perspective
   else { m[3] = v[2]; m[7] = v[4]; m[15] = 1; }             // orthographic
}

void wii_GXSetProjection(const float *mtx, uint32_t type)
{
   FLUSH();
   float *v = G.projv;
   v[0] = (float)type;
   v[1] = mtx[0]; v[3] = mtx[5]; v[5] = mtx[10]; v[6] = mtx[11];
   if (type == 0) { v[2] = mtx[2]; v[4] = mtx[6]; } else { v[2] = mtx[3]; v[4] = mtx[7]; }
   proj_from_params();
   DIRTY();
}

void wii_GXSetProjectionv(const float *p) { FLUSH(); memcpy(G.projv, p, 7 * sizeof(float)); proj_from_params(); DIRTY(); }
void wii_GXGetProjectionv(float *p) { memcpy(p, G.projv, 7 * sizeof(float)); }

void wii_GXSetViewport(float l, float t, float w, float h, float n, float f)
{
   FLUSH();
   float *v = sCur.viewport;
   v[0] = l; v[1] = t; v[2] = w; v[3] = h; v[4] = n; v[5] = f;
   DIRTY();
}
void wii_GXSetViewportJitter(float l, float t, float w, float h, float n, float f, uint32_t field) { wii_GXSetViewport(l, t, w, h, n, f); }
void wii_GXGetViewportv(float *v) { memcpy(v, sCur.viewport, 6 * sizeof(float)); }

void wii_GXSetScissor(uint32_t l, uint32_t t, uint32_t w, uint32_t h)
{
   FLUSH();
   sCur.scissor[0] = (int32_t)l; sCur.scissor[1] = (int32_t)t; sCur.scissor[2] = (int32_t)w; sCur.scissor[3] = (int32_t)h;
   DIRTY();
}

// ---------------------------------------------------------------------------------------
// Colour channels
// ---------------------------------------------------------------------------------------
void wii_GXSetNumChans(uint8_t n) { FLUSH(); G.numChans = n; }

void wii_GXSetChanCtrl(uint32_t chan, int enable, uint32_t ambSrc, uint32_t matSrc, uint32_t lights, uint32_t diff, uint32_t attn)
{
   FLUSH();
   ChanCtrl c = {(uint8_t)(enable != 0), (uint8_t)ambSrc, (uint8_t)matSrc};
   if (chan < 4) G.chan[chan] = c;
   else if (chan == 4 || chan == 5) G.chan[(chan - 4) * 2] = G.chan[(chan - 4) * 2 + 1] = c;
}

static void chan_color(float dst[2][4], uint32_t chan, const uint8_t *c)
{
   float f[4];
   color_from(f, c);
   switch (chan) {
   case 0: memcpy(dst[0], f, 12); break;
   case 1: dst[0][3] = f[3]; break;
   case 2: memcpy(dst[1], f, 12); break;
   case 3: dst[1][3] = f[3]; break;
   case 4: memcpy(dst[0], f, 16); break;
   case 5: memcpy(dst[1], f, 16); break;
   }
}

void wii_GXSetChanMatColor(uint32_t chan, const uint8_t *c) { FLUSH(); chan_color(G.mat, chan, c); }
void wii_GXSetChanAmbColor(uint32_t chan, const uint8_t *c) { FLUSH(); chan_color(G.amb, chan, c); }

// ---------------------------------------------------------------------------------------
// TEV
// ---------------------------------------------------------------------------------------
#define STAGE_OK(s) ((s) < GX_MAX_STAGES)
void wii_GXSetNumTevStages(uint8_t n) { FLUSH(); sCur.misc[0][0] = n; DIRTY(); }
void wii_GXSetTevOp(uint32_t s, uint32_t mode) { FLUSH(); if (STAGE_OK(s)) { tev_op(s, mode); DIRTY(); } }

void wii_GXSetTevColorIn(uint32_t s, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
   FLUSH();
   if (!STAGE_OK(s)) return;
   float *ci = sCur.stage[s * 7 + 0];
   ci[0] = a; ci[1] = b; ci[2] = c; ci[3] = d;
   DIRTY();
}

void wii_GXSetTevAlphaIn(uint32_t s, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
   FLUSH();
   if (!STAGE_OK(s)) return;
   float *ai = sCur.stage[s * 7 + 1];
   ai[0] = a; ai[1] = b; ai[2] = c; ai[3] = d;
   DIRTY();
}

void wii_GXSetTevColorOp(uint32_t s, uint32_t op, uint32_t bias, uint32_t scale, int clamp, uint32_t out)
{
   FLUSH();
   if (!STAGE_OK(s)) return;
   float *o = sCur.stage[s * 7 + 2];
   o[0] = op; o[1] = bias; o[2] = scale; o[3] = clamp ? 1 : 0;
   sCur.stage[s * 7 + 4][0] = out;
   DIRTY();
}

void wii_GXSetTevAlphaOp(uint32_t s, uint32_t op, uint32_t bias, uint32_t scale, int clamp, uint32_t out)
{
   FLUSH();
   if (!STAGE_OK(s)) return;
   float *o = sCur.stage[s * 7 + 3];
   o[0] = op; o[1] = bias; o[2] = scale; o[3] = clamp ? 1 : 0;
   sCur.stage[s * 7 + 4][1] = out;
   DIRTY();
}

void wii_GXSetTevColor(uint32_t id, const uint8_t *c) { FLUSH(); if (id < 4) { color_from(sCur.regs[id], c); DIRTY(); } }
void wii_GXSetTevColorS10(uint32_t id, const int16_t *c)
{
   FLUSH();
   if (id >= 4) return;
   for (int i = 0; i < 4; i++) sCur.regs[id][i] = c[i] / 255.0f;
   DIRTY();
}
void wii_GXSetTevKColor(uint32_t id, const uint8_t *c) { FLUSH(); if (id < 4) { color_from(sCur.konst[id], c); DIRTY(); } }
void wii_GXSetTevKColorSel(uint32_t s, uint32_t sel) { FLUSH(); if (STAGE_OK(s)) { sCur.stage[s * 7 + 4][2] = sel; DIRTY(); } }
void wii_GXSetTevKAlphaSel(uint32_t s, uint32_t sel) { FLUSH(); if (STAGE_OK(s)) { sCur.stage[s * 7 + 4][3] = sel; DIRTY(); } }

void wii_GXSetTevOrder(uint32_t s, uint32_t coord, uint32_t map, uint32_t chan)
{
   FLUSH();
   if (!STAGE_OK(s)) return;
   float *t = sCur.stage[s * 7 + 5];
   int slot = (map == 0xFF || (map & 0x100)) ? -1 : (int)(map & 7);
   int tc = coord == 0xFF ? -1 : (int)coord;
   if (tc >= GX_MAX_COORDS) tc = 0;
   t[0] = (float)slot;
   t[1] = (float)tc;
   t[2] = chan == 4 ? 0 : chan == 5 ? 1 : 2; // COLOR0A0, COLOR1A1, otherwise zero
   DIRTY();
}

void wii_GXSetTevSwapMode(uint32_t s, uint32_t ras, uint32_t tex)
{
   FLUSH();
   if (!STAGE_OK(s)) return;
   sCur.stage[s * 7 + 5][3] = ras & 3;
   sCur.stage[s * 7 + 6][0] = tex & 3;
   DIRTY();
}

void wii_GXSetTevSwapModeTable(uint32_t table, uint32_t r, uint32_t g, uint32_t b, uint32_t a)
{
   FLUSH();
   if (table >= 4) return;
   float *t = sCur.swap[table];
   t[0] = r; t[1] = g; t[2] = b; t[3] = a;
   DIRTY();
}

void wii_GXSetTevDirect(uint32_t s) { FLUSH(); }
void wii_GXSetTevIndirect(uint32_t s, uint32_t ind, uint32_t fmt, uint32_t bias, uint32_t mtx, uint32_t ws, uint32_t wt,
                          int addPrev, int utcLod, uint32_t alphaSel) { FLUSH(); }
void wii_GXSetNumIndStages(uint8_t n) { FLUSH(); }
void wii_GXSetIndTexOrder(uint32_t s, uint32_t coord, uint32_t map) { FLUSH(); }
void wii_GXSetIndTexMtx(uint32_t id, const float *m, int8_t scale) { FLUSH(); }

void wii_GXSetAlphaCompare(uint32_t c0, uint8_t r0, uint32_t op, uint32_t c1, uint8_t r1)
{
   FLUSH();
   sCur.misc[0][1] = c0; sCur.misc[0][2] = r0 / 255.0f; sCur.misc[0][3] = op;
   sCur.misc[1][0] = c1; sCur.misc[1][1] = r1 / 255.0f;
   DIRTY();
}

// ---------------------------------------------------------------------------------------
// Pixel engine
// ---------------------------------------------------------------------------------------
void wii_GXSetBlendMode(uint32_t type, uint32_t src, uint32_t dst, uint32_t op)
{
   FLUSH();
   sCur.blendType = (uint8_t)type; sCur.blendSrc = (uint8_t)src; sCur.blendDst = (uint8_t)dst; sCur.logicOp = (uint8_t)op;
   DIRTY();
}
void wii_GXSetColorUpdate(int on) { FLUSH(); sCur.colorUpdate = on != 0; DIRTY(); }
void wii_GXSetAlphaUpdate(int on) { FLUSH(); sCur.alphaUpdate = on != 0; DIRTY(); }
void wii_GXSetZMode(int enable, uint32_t func, int update)
{
   FLUSH();
   sCur.zEnable = enable != 0; sCur.zFunc = (uint8_t)func; sCur.zUpdate = update != 0;
   DIRTY();
}
void wii_GXSetCullMode(uint32_t mode) { FLUSH(); sCur.cull = (uint8_t)mode; DIRTY(); }
void wii_GXGetCullMode(uint32_t *mode) { *mode = sCur.cull; }
void wii_GXSetZCompLoc(int before) { FLUSH(); }
void wii_GXSetZTexture(uint32_t op, uint32_t fmt, uint32_t bias) { FLUSH(); }
void wii_GXSetFog(uint32_t type, float a, float b, float n, float f, const uint8_t *color) { FLUSH(); }
void wii_GXPokeBlendMode(uint32_t type, uint32_t src, uint32_t dst, uint32_t op) {}

// ---------------------------------------------------------------------------------------
// Textures
// ---------------------------------------------------------------------------------------
// GXTexObj (0x20 bytes, opaque to the game): GxTexKey + user data.
typedef struct {
   GxTexKey key;
   void *userData;
   uint32_t pad[3];
} TexObj;

void wii_GXInitTexObj(TexObj *o, void *image, uint16_t w, uint16_t h, uint32_t fmt, uint32_t ws, uint32_t wt, int mip)
{
   memset(o, 0, sizeof(*o));
   o->key.image = (uint32_t)image;
   o->key.width = w;
   o->key.height = h;
   o->key.fmt = (uint8_t)fmt;
   o->key.wrapS = (uint8_t)ws;
   o->key.wrapT = (uint8_t)wt;
   o->key.mipmap = mip != 0;
   o->key.minFilt = o->key.magFilt = 1; // linear
}

void wii_GXInitTexObjLOD(TexObj *o, uint32_t minF, uint32_t magF, float minLod, float maxLod, float bias, int biasClamp,
                         int edgeLod, uint32_t aniso)
{
   o->key.minFilt = (uint8_t)minF;
   o->key.magFilt = (uint8_t)magF;
}

void wii_GXInitTexObjWrapMode(TexObj *o, uint32_t s, uint32_t t) { o->key.wrapS = (uint8_t)s; o->key.wrapT = (uint8_t)t; }
void wii_GXInitTexObjUserData(TexObj *o, void *ud) { o->userData = ud; }
void *wii_GXGetTexObjData(const TexObj *o) { return (void *)o->key.image; }
uint32_t wii_GXGetTexObjFmt(const TexObj *o) { return o->key.fmt; }

void wii_GXLoadTexObj(const TexObj *o, uint32_t map)
{
   FLUSH();
   if (map < GX_MAX_SLOTS) { sCur.tex[map] = o->key; DIRTY(); }
}

uint32_t wii_GXGetTexBufferSize(uint16_t w, uint16_t h, uint32_t fmt, int mip, uint8_t maxLod)
{
   if (!mip) return gx_tex_buffer_size(w, h, fmt);
   uint32_t n = 0;
   for (uint32_t l = 0; l < maxLod && (w || h); l++) {
      n += gx_tex_buffer_size(w ? w : 1, h ? h : 1, fmt);
      w >>= 1; h >>= 1;
   }
   return n;
}

int gx_is_render_texture(uint32_t addr)
{
   if (!addr) return 0;
   for (uint32_t i = (addr >> 5) & (RT_SLOTS - 1), n = 0; n < RT_SLOTS; i = (i + 1) & (RT_SLOTS - 1), n++) {
      if (sRenderTex[i] == addr) return 1;
      if (!sRenderTex[i]) return 0;
   }
   return 0;
}

// ---------------------------------------------------------------------------------------
// EFB copies
// ---------------------------------------------------------------------------------------
void wii_GXSetCopyClear(const uint8_t *rgba, uint32_t z)
{
   G.copyClear = (uint32_t)rgba[0] << 24 | rgba[1] << 16 | rgba[2] << 8 | rgba[3];
   G.copyClearZ = z;
}

void wii_GXSetDispCopySrc(uint16_t l, uint16_t t, uint16_t w, uint16_t h)
{
   G.dispSrc[0] = l; G.dispSrc[1] = t; G.dispSrc[2] = w; G.dispSrc[3] = h;
}

void wii_GXSetTexCopySrc(uint16_t l, uint16_t t, uint16_t w, uint16_t h)
{
   G.texSrc[0] = l; G.texSrc[1] = t; G.texSrc[2] = w; G.texSrc[3] = h;
}

void wii_GXSetTexCopyDst(uint16_t w, uint16_t h, uint32_t fmt, int mip)
{
   G.texDst[0] = w; G.texDst[1] = h; G.texDstFmt = (uint8_t)fmt;
}

void wii_GXCopyTex(void *dest, int clear)
{
   FLUSH();
   sStats[7]++;
   if (dest && !gx_is_render_texture((uint32_t)dest) && sNumRenderTex < RT_SLOTS / 2) {
      uint32_t i = ((uint32_t)dest >> 5) & (RT_SLOTS - 1);
      while (sRenderTex[i]) i = (i + 1) & (RT_SLOTS - 1);
      sRenderTex[i] = (uint32_t)dest;
      sNumRenderTex++;
   }
   GxCmd *c = new_cmd(GXCMD_COPYTEX);
   if (!c) return;
   c->a[0] = (uint32_t)G.texSrc[0] << 16 | G.texSrc[1];
   c->a[1] = (uint32_t)G.texSrc[2] << 16 | G.texSrc[3];
   c->a[2] = (uint32_t)dest;
   c->a[3] = (uint32_t)G.texDst[0] << 16 | G.texDst[1];
   c->a[4] = clear != 0;
   c->a[5] = G.copyClear;
   c->a[6] = G.copyClearZ;
   c->a[7] = G.texDstFmt;
}

static volatile uint32_t sCopyDisp;
uint32_t port_gx_copy_disp(void) { return sCopyDisp; }
uint32_t port_gx_copy_clear(void) { return G.copyClear; }

void wii_GXCopyDisp(void *dest, int clear)
{
   FLUSH();
   sCopyDisp++;
   GxPacket *p = sFill;
   memcpy(p->dispSrc, G.dispSrc, sizeof(p->dispSrc));
   p->black = port_vi_black();
   p->frame = sCopyDisp;
   p->clearAfter = clear != 0;
   p->clearColor = G.copyClear;
   p->clearZ = G.copyClearZ;
   submit_packet();
}

// One-line GX status for the watchdog.
int gx_describe(char *buf, int size)
{
   extern uint8_t *volatile port_wgp_ptr;
   static const char *modes[3] = {"fifo", "dl", "redirect"};
   extern volatile const char *gx_render_step;
   extern volatile uint32_t gx_nan_verts;
   return snprintf(buf, size, "gx: nan verts %u, render %s, last %s, %u calls, %u flushes%s, mode %s, stream %d bytes, prims %u, pkts %u",
                   (unsigned)gx_nan_verts, (const char *)gx_render_step, (const char *)gx_last_api, (unsigned)gx_api_calls, (unsigned)gx_flushes, gx_in_flush ? " (IN FLUSH)" : "",
                   modes[sMode < 3 ? sMode : 0], sMode == MODE_FIFO ? (int)(port_wgp_ptr - sParse) : -1,
                   (unsigned)sStats[0], (unsigned)sStats[4]);
}
