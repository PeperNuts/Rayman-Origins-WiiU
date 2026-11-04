// Rayman Origins Wii U port - GX2 back end (render thread on CPU2).
//
// Plays the frame packets built by gx_core.c:
//   - an "EFB" colour + depth target at 2x the Wii resolution
//   - one generic shader pair that evaluates the game's TEV configuration from uniforms
//     (compiled at start-up by CafeGLSL, sd:/wiiu/libs/glslcompiler.rpl)
//   - Wii textures decoded to RGBA8 on first use and cached
//   - GXCopyTex: EFB rectangle copied into a texture the game can sample later
//   - GXCopyDisp: the EFB is scaled onto the TV; the GamePad shows the log
#include <coreinit/cache.h>
#include <coreinit/dynload.h>
#include <coreinit/memdefaultheap.h>
#include <coreinit/semaphore.h>
#include <stdlib.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <gx2/clear.h>
#include <gx2/context.h>
#include <gx2/draw.h>
#include <gx2/event.h>
#include <gx2/display.h>
#include <gx2/swap.h>
#include <gx2/enum.h>
#include <gx2/mem.h>
#include <gx2/registers.h>
#include <gx2/sampler.h>
#include <gx2/shaders.h>
#include <gx2/state.h>
#include <gx2/surface.h>
#include <gx2/texture.h>
#include <proc_ui/procui.h>
#include <whb/gfx.h>
#include <whb/log.h>

#include <stdio.h>
#include <string.h>
#include <stddef.h>

#include "gx.h"
#include "port.h"

static int sEfbScale = 2; // 2 = twice the Wii resolution (default), 1 with "efb1x" in port.cfg
#define EFB_SCALE sEfbScale
#define SHADER_STACK (8u << 20)
#define EFB_W (640 * EFB_SCALE)
#define EFB_H (528 * EFB_SCALE)

// Switches read from sd:/rayman/port.cfg (to bisect GPU problems without new builds)
static int sCfgNoEfb, sCfgNoClear, sCfgNoCopy, sCfgNoDraw, sCfgNoAfterFx;
volatile const char *gx_render_step = "init"; // breadcrumb for the watchdog

// ---------------------------------------------------------------------------------------
// Shaders
// ---------------------------------------------------------------------------------------
static const char kTevVS[] =
   "#version 420 core\n"
   "layout(location = 0) in vec4 aPos;\n"
   "layout(location = 1) in vec4 aC0;\n"
   "layout(location = 2) in vec4 aC1;\n"
   "layout(location = 3) in vec4 aT0;\n"
   "layout(location = 4) in vec4 aT1;\n"
   "layout(location = 5) in vec4 aT2;\n"
   "layout(location = 6) in vec4 aT3;\n"
   "uniform vec4 uProj[4];\n"
   "layout(location = 0) out vec4 vC0; layout(location = 1) out vec4 vC1;\n"
   "layout(location = 2) out vec4 vT0; layout(location = 3) out vec4 vT1;\n"
   "layout(location = 4) out vec4 vT2; layout(location = 5) out vec4 vT3;\n"
   "void main() {\n"
   "  vec4 p = vec4(aPos.xyz, 1.0);\n"
   "  vec4 c = vec4(dot(uProj[0], p), dot(uProj[1], p), dot(uProj[2], p), dot(uProj[3], p));\n"
   "  c.z = 2.0 * c.z + c.w;\n" // GX clip z is [-w, 0]
   "  gl_Position = c;\n"
   "  vC0 = aC0; vC1 = aC1; vT0 = aT0; vT1 = aT1; vT2 = aT2; vT3 = aT3;\n"
   "}\n";

static const char kTevPS[] =
   "#version 420 core\n"
   "layout(location = 0) in vec4 vC0; layout(location = 1) in vec4 vC1;\n"
   "layout(location = 2) in vec4 vT0; layout(location = 3) in vec4 vT1;\n"
   "layout(location = 4) in vec4 vT2; layout(location = 5) in vec4 vT3;\n"
   "layout(location = 0) out vec4 fragColor;\n"
   "layout(binding = 0) uniform sampler2D uTex0; layout(binding = 1) uniform sampler2D uTex1;\n"
   "layout(binding = 2) uniform sampler2D uTex2; layout(binding = 3) uniform sampler2D uTex3;\n"
   "layout(binding = 4) uniform sampler2D uTex4; layout(binding = 5) uniform sampler2D uTex5;\n"
   "layout(binding = 6) uniform sampler2D uTex6; layout(binding = 7) uniform sampler2D uTex7;\n"
   "uniform vec4 uStage[56];\n"
   "uniform vec4 uRegs[4];\n"
   "uniform vec4 uKonst[4];\n"
   "uniform vec4 uSwap[4];\n"
   "uniform vec4 uMisc[2];\n"
   "int ri(float f) { return int(floor(f + 0.5)); }\n"
   "vec4 tex(int s, vec2 uv) {\n"
   "  if (s == 0) return texture(uTex0, uv); if (s == 1) return texture(uTex1, uv);\n"
   "  if (s == 2) return texture(uTex2, uv); if (s == 3) return texture(uTex3, uv);\n"
   "  if (s == 4) return texture(uTex4, uv); if (s == 5) return texture(uTex5, uv);\n"
   "  if (s == 6) return texture(uTex6, uv); return texture(uTex7, uv);\n"
   "}\n"
   "vec4 coord(int c) { if (c == 0) return vT0; if (c == 1) return vT1; if (c == 2) return vT2; return vT3; }\n"
   "vec4 swz(vec4 c, int t) { vec4 s = uSwap[t]; return vec4(c[ri(s.x)], c[ri(s.y)], c[ri(s.z)], c[ri(s.w)]); }\n"
   "float kfix(int s) { return float(8 - s) / 8.0; }\n"
   "vec3 kcol(int s) {\n"
   "  if (s < 8) return vec3(kfix(s));\n"
   "  if (s >= 12 && s < 16) return uKonst[s - 12].rgb;\n"
   "  if (s >= 16 && s < 32) return vec3(uKonst[s & 3][(s - 16) >> 2]);\n"
   "  return vec3(1.0);\n"
   "}\n"
   "float kalp(int s) {\n"
   "  if (s < 8) return kfix(s);\n"
   "  if (s >= 16 && s < 32) return uKonst[s & 3][(s - 16) >> 2];\n"
   "  return 1.0;\n"
   "}\n"
   "vec3 cin(int i, vec4 p, vec4 r0, vec4 r1, vec4 r2, vec4 t, vec4 r, vec3 k) {\n"
   "  if (i == 0) return p.rgb; if (i == 1) return vec3(p.a); if (i == 2) return r0.rgb; if (i == 3) return vec3(r0.a);\n"
   "  if (i == 4) return r1.rgb; if (i == 5) return vec3(r1.a); if (i == 6) return r2.rgb; if (i == 7) return vec3(r2.a);\n"
   "  if (i == 8) return t.rgb; if (i == 9) return vec3(t.a); if (i == 10) return r.rgb; if (i == 11) return vec3(r.a);\n"
   "  if (i == 12) return vec3(1.0); if (i == 13) return vec3(0.5); if (i == 14) return k; return vec3(0.0);\n"
   "}\n"
   "float ain(int i, vec4 p, vec4 r0, vec4 r1, vec4 r2, vec4 t, vec4 r, float k) {\n"
   "  if (i == 0) return p.a; if (i == 1) return r0.a; if (i == 2) return r1.a; if (i == 3) return r2.a;\n"
   "  if (i == 4) return t.a; if (i == 5) return r.a; if (i == 6) return k; return 0.0;\n"
   "}\n"
   "float bias(float b) { int i = ri(b); return i == 1 ? 0.5 : (i == 2 ? -0.5 : 0.0); }\n"
   "float scl(float s) { int i = ri(s); return i == 1 ? 2.0 : (i == 2 ? 4.0 : (i == 3 ? 0.5 : 1.0)); }\n"
   "bool cmp(int op, float a, float b) { return (op & 1) == 0 ? a > b + 0.5 / 255.0 : abs(a - b) < 0.5 / 255.0; }\n"
   "float pk(vec3 c, int n) { vec3 q = floor(c * 255.0 + 0.5); return n == 1 ? q.r : (n == 2 ? q.g * 256.0 + q.r : q.b * 65536.0 + q.g * 256.0 + q.r); }\n"
   "bool acmp(int f, float v, float r) {\n"
   "  if (f == 0) return false; if (f == 1) return v < r; if (f == 2) return abs(v - r) < 0.5 / 255.0;\n"
   "  if (f == 3) return v <= r + 0.5 / 255.0; if (f == 4) return v > r; if (f == 5) return abs(v - r) >= 0.5 / 255.0;\n"
   "  if (f == 6) return v >= r - 0.5 / 255.0; return true;\n"
   "}\n"
   "void main() {\n"
   "  vec4 p = uRegs[0], r0 = uRegs[1], r1 = uRegs[2], r2 = uRegs[3];\n"
   "  int n = ri(uMisc[0].x);\n"
   "  for (int s = 0; s < 8; s++) {\n"
   "    if (s >= n) break;\n"
   "    vec4 ci = uStage[s * 7], ai = uStage[s * 7 + 1], co = uStage[s * 7 + 2], ao = uStage[s * 7 + 3];\n"
   "    vec4 o = uStage[s * 7 + 4], to = uStage[s * 7 + 5], w = uStage[s * 7 + 6];\n"
   "    vec4 t = vec4(1.0);\n"
   "    int slot = ri(to.x);\n"
   "    if (slot >= 0) {\n"
   "      vec4 tc = coord(max(ri(to.y), 0));\n"
   "      vec2 uv = tc.xy / (abs(tc.z) < 1e-6 ? 1.0 : tc.z) * w.yz;\n"
   "      t = swz(tex(slot, uv), ri(w.x));\n"
   "    }\n"
   "    int ch = ri(to.z);\n"
   "    vec4 r = swz(ch == 0 ? vC0 : (ch == 1 ? vC1 : vec4(0.0)), ri(to.w));\n"
   "    vec3 kc = kcol(ri(o.z)); float ka = kalp(ri(o.w));\n"
   "    vec3 a = cin(ri(ci.x), p, r0, r1, r2, t, r, kc), b = cin(ri(ci.y), p, r0, r1, r2, t, r, kc);\n"
   "    vec3 c = cin(ri(ci.z), p, r0, r1, r2, t, r, kc), d = cin(ri(ci.w), p, r0, r1, r2, t, r, kc);\n"
   "    float aa = ain(ri(ai.x), p, r0, r1, r2, t, r, ka), ab = ain(ri(ai.y), p, r0, r1, r2, t, r, ka);\n"
   "    float ac = ain(ri(ai.z), p, r0, r1, r2, t, r, ka), ad = ain(ri(ai.w), p, r0, r1, r2, t, r, ka);\n"
   "    vec3 rc; float ra;\n"
   "    int cop = ri(co.x), aop = ri(ao.x);\n"
   "    if (cop < 2) { vec3 m = mix(a, b, c); rc = ((cop == 1 ? d - m : d + m) + bias(co.y)) * scl(co.z); }\n"
   "    else if (cop >= 14) { rc = d + vec3(cmp(cop, a.r, b.r) ? c.r : 0.0, cmp(cop, a.g, b.g) ? c.g : 0.0, cmp(cop, a.b, b.b) ? c.b : 0.0); }\n"
   "    else { int k = (cop - 8) / 2 + 1; rc = d + (cmp(cop, pk(a, k), pk(b, k)) ? c : vec3(0.0)); }\n"
   "    if (aop < 2) { float m = mix(aa, ab, ac); ra = ((aop == 1 ? ad - m : ad + m) + bias(ao.y)) * scl(ao.z); }\n"
   "    else { ra = ad + (cmp(aop, aa, ab) ? ac : 0.0); }\n"
   "    rc = co.w > 0.5 ? clamp(rc, 0.0, 1.0) : clamp(rc, -4.0, 4.0);\n"
   "    ra = ao.w > 0.5 ? clamp(ra, 0.0, 1.0) : clamp(ra, -4.0, 4.0);\n"
   "    int oc = ri(o.x), oa = ri(o.y);\n"
   "    if (oc == 0) p.rgb = rc; else if (oc == 1) r0.rgb = rc; else if (oc == 2) r1.rgb = rc; else r2.rgb = rc;\n"
   "    if (oa == 0) p.a = ra; else if (oa == 1) r0.a = ra; else if (oa == 2) r1.a = ra; else r2.a = ra;\n"
   "  }\n"
   "  bool t0 = acmp(ri(uMisc[0].y), p.a, uMisc[0].z), t1 = acmp(ri(uMisc[1].x), p.a, uMisc[1].y);\n"
   "  int op = ri(uMisc[0].w);\n"
   "  bool pass = op == 0 ? (t0 && t1) : (op == 1 ? (t0 || t1) : (op == 2 ? (t0 != t1) : (t0 == t1)));\n"
   "  if (!pass) discard;\n"
   "  fragColor = clamp(p, 0.0, 1.0);\n"
   "}\n";

static const char kBlitVS[] =
   "#version 420 core\n"
   "layout(location = 0) in vec4 aPos;\n"
   "layout(location = 0) out vec2 vUV;\n"
   "void main() { gl_Position = vec4(aPos.xy, 0.0, 1.0); vUV = aPos.zw; }\n";

static const char kBlitPS[] =
   "#version 420 core\n"
   "layout(location = 0) in vec2 vUV;\n"
   "layout(location = 0) out vec4 fragColor;\n"
   "layout(binding = 0) uniform sampler2D uSrc;\n"
   "void main() { fragColor = vec4(texture(uSrc, vUV).rgb, 1.0); }\n";

// CafeGLSL (v0.2.0) runtime library
static GX2VertexShader *(*glslVS)(const char *, char *, int, int);
static GX2PixelShader *(*glslPS)(const char *, char *, int, int);

static int glsl_init(void)
{
   OSDynLoad_Module m;
   OSTime t0 = OSGetSystemTime();
   WHBLogPrintf("[gfx] loading glslcompiler.rpl");
   OSDynLoad_Error e = OSDynLoad_Acquire("glslcompiler", &m);
   WHBLogPrintf("[gfx] OSDynLoad_Acquire(glslcompiler) = %d (%u ms)", (int)e, (unsigned)OSTicksToMilliseconds(OSGetSystemTime() - t0));
   if (e != OS_DYNLOAD_OK) {
      e = OSDynLoad_Acquire("~/wiiu/libs/glslcompiler.rpl", &m);
      WHBLogPrintf("[gfx] OSDynLoad_Acquire(~/wiiu/libs/glslcompiler.rpl) = %d (%u ms)", (int)e,
                   (unsigned)OSTicksToMilliseconds(OSGetSystemTime() - t0));
   }
   if (e != OS_DYNLOAD_OK) {
      WHBLogPrintf("[gfx] glslcompiler.rpl not found - copy it to sd:/wiiu/libs/");
      return 0;
   }
   void (*init)(void) = NULL;
   OSDynLoad_FindExport(m, OS_DYNLOAD_EXPORT_FUNC, "InitGLSLCompiler", (void **)&init);
   OSDynLoad_FindExport(m, OS_DYNLOAD_EXPORT_FUNC, "CompileVertexShader", (void **)&glslVS);
   OSDynLoad_FindExport(m, OS_DYNLOAD_EXPORT_FUNC, "CompilePixelShader", (void **)&glslPS);
   if (!init || !glslVS || !glslPS) {
      WHBLogPrintf("[gfx] glslcompiler.rpl: exports missing");
      return 0;
   }
   WHBLogPrintf("[gfx] glslcompiler.rpl loaded (%u ms), initialising", (unsigned)OSTicksToMilliseconds(OSGetSystemTime() - t0));
   init();
   WHBLogPrintf("[gfx] compiler ready (%u ms)", (unsigned)OSTicksToMilliseconds(OSGetSystemTime() - t0));
   return 1;
}

static int compile(WHBGfxShaderGroup *g, const char *vs, const char *ps, const char *what)
{
   static char log[1024];
   memset(g, 0, sizeof(*g));
   OSTime t0 = OSGetSystemTime();
   WHBLogPrintf("[gfx] compiling %s shaders", what);
   log[0] = 0;
   g->vertexShader = glslVS(vs, log, sizeof(log), 0);
   if (!g->vertexShader) { WHBLogPrintf("[gfx] %s vertex shader: %s", what, log); return 0; }
   log[0] = 0;
   g->pixelShader = glslPS(ps, log, sizeof(log), 0);
   if (!g->pixelShader) { WHBLogPrintf("[gfx] %s pixel shader: %s", what, log); return 0; }
   WHBLogPrintf("[gfx] %s shaders compiled (%u ms)", what, (unsigned)OSTicksToMilliseconds(OSGetSystemTime() - t0));
   return 1;
}

static int32_t uniform_offset(GX2UniformVar *vars, uint32_t n, const char *name)
{
   size_t len = strlen(name);
   for (uint32_t i = 0; i < n; i++)
      if (!strncmp(vars[i].name, name, len) && (vars[i].name[len] == 0 || vars[i].name[len] == '[')) return (int32_t)vars[i].offset;
   return -1;
}

static int32_t sampler_loc(GX2PixelShader *ps, const char *name)
{
   for (uint32_t i = 0; i < ps->samplerVarCount; i++)
      if (!strcmp(ps->samplerVars[i].name, name)) return (int32_t)ps->samplerVars[i].location;
   return -1;
}

// ---------------------------------------------------------------------------------------
// Targets
// ---------------------------------------------------------------------------------------
static GX2ColorBuffer sEfb;
static GX2DepthBuffer sEfbDepth;
static GX2Texture sEfbTex;
static GX2ContextState *sCtx;
static WHBGfxShaderGroup sTev, sBlit;
static volatile int sHaveShaders, sShaderDone;
static int sBlockMode, sUboSwap;
static int32_t sVsBlockLoc = -1, sPsBlockLoc = -1;
static uint32_t sVsBlockSize, sPsBlockSize;
#define UBO_RING (4u << 20)
static uint8_t *sUbo;
static uint32_t sUboPos, sUboBase, sUboEnd; // the current frame's half of the ring
static int32_t uProj, uStage, uRegs, uKonst, uSwap, uMisc, uSampler[GX_MAX_SLOTS], uBlitSrc;
static float *sBlitVerts;

static void *alloc_gpu(uint32_t size, uint32_t align)
{
   void *p = MEMAllocFromDefaultHeapEx(size, align < 256 ? 256 : align);
   if (p) memset(p, 0, size);
   return p;
}

// The EFB (colour + depth) lives in MEM1, the Wii U's fast embedded memory, next to WHBGfx's
// screen buffers: every blended pixel reads and writes it, which at twice the Wii resolution is
// more traffic than MEM2 can carry at 60 fps. WHBGfx discards its MEM1 heap while the HOME Menu
// is open, so the renderer allocates the EFB again afterwards (efb_alloc). "efbmem2" in
// port.cfg (or too little MEM1) keeps it in MEM2.
void *GfxHeapAllocMEM1(uint32_t size, uint32_t alignment);
static int sEfbMem1 = -1;
static int efb_alloc(void)
{
   GX2Surface *s = &sEfb.surface, *d = &sEfbDepth.surface;
   if (sEfbMem1 < 0) sEfbMem1 = !port_cfg("efbmem2");
   static int logged;
   int where = 2;
   s->image = d->image = NULL;
   if (sEfbMem1) {
      s->image = GfxHeapAllocMEM1(s->imageSize, s->alignment);
      d->image = s->image ? GfxHeapAllocMEM1(d->imageSize, d->alignment) : NULL;
      if (s->image && d->image) where = 1;
      else sEfbMem1 = 0; // does not fit: MEM2 from now on (the partial block goes with the heap)
   }
   if (where == 2) {
      s->image = alloc_gpu(s->imageSize, s->alignment);
      d->image = alloc_gpu(d->imageSize, d->alignment);
   }
   sEfbTex.surface.image = s->image;
   if (!logged++) WHBLogPrintf("[gfx] EFB %ux%u in MEM%d (%u KiB colour, %u KiB depth)", (unsigned)s->width,
                               (unsigned)s->height, where, (unsigned)(s->imageSize >> 10), (unsigned)(d->imageSize >> 10));
   return s->image && d->image;
}

static int init_targets(void)
{
   GX2Surface *s = &sEfb.surface;
   memset(&sEfb, 0, sizeof(sEfb));
   s->dim = GX2_SURFACE_DIM_TEXTURE_2D;
   s->width = EFB_W; s->height = EFB_H; s->depth = 1; s->mipLevels = 1;
   s->format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
   s->aa = GX2_AA_MODE1X;
   s->use = GX2_SURFACE_USE_TEXTURE | GX2_SURFACE_USE_COLOR_BUFFER;
   s->tileMode = GX2_TILE_MODE_DEFAULT;
   sEfb.viewNumSlices = 1;
   GX2CalcSurfaceSizeAndAlignment(s);
   GX2InitColorBufferRegs(&sEfb);

   GX2Surface *d = &sEfbDepth.surface;
   memset(&sEfbDepth, 0, sizeof(sEfbDepth));
   d->dim = GX2_SURFACE_DIM_TEXTURE_2D;
   d->width = EFB_W; d->height = EFB_H; d->depth = 1; d->mipLevels = 1;
   d->format = GX2_SURFACE_FORMAT_FLOAT_R32;
   d->aa = GX2_AA_MODE1X;
   d->use = GX2_SURFACE_USE_DEPTH_BUFFER;
   d->tileMode = GX2_TILE_MODE_DEFAULT;
   sEfbDepth.viewNumSlices = 1;
   sEfbDepth.depthClear = 1.0f;
   GX2CalcSurfaceSizeAndAlignment(d);
   GX2InitDepthBufferRegs(&sEfbDepth);
   efb_alloc();

   memset(&sEfbTex, 0, sizeof(sEfbTex));
   sEfbTex.surface = *s;
   sEfbTex.viewNumMips = 1;
   sEfbTex.viewNumSlices = 1;
   sEfbTex.compMap = 0x00010203;
   GX2InitTextureRegs(&sEfbTex);

   sCtx = alloc_gpu(sizeof(GX2ContextState), GX2_CONTEXT_STATE_ALIGNMENT);
   if (sCtx) GX2SetupContextStateEx(sCtx, TRUE);
   sBlitVerts = alloc_gpu(6 * 4 * sizeof(float), 256);
   return s->image && d->image && sCtx && sBlitVerts;
}

static const char kPlainVS[], kPlainPS[], kProbeVS[], kProbePS[];
static void probe_init(void);
static void probe_draw(void);
static WHBGfxShaderGroup sProbe;
static int sHaveProbe;
static WHBGfxShaderGroup sPlain;
static int sHavePlain;
static int compile_shaders(void)
{
   int ok = compile(&sTev, kTevVS, kTevPS, "TEV") && compile(&sBlit, kBlitVS, kBlitPS, "blit");
   if (ok) sHavePlain = compile(&sPlain, kPlainVS, kPlainPS, "self-test");
   if (ok) sHaveProbe = compile(&sProbe, kProbeVS, kProbePS, "probe");
   return ok;
}

// Fetch shaders and uniform locations: GX2 work, done on the render thread.
static int finish_shaders(void)
{
   WHBGfxInitShaderAttribute(&sTev, "aPos", 0, offsetof(GxVertex, pos), GX2_ATTRIB_FORMAT_FLOAT_32_32_32);
   WHBGfxInitShaderAttribute(&sTev, "aC0", 0, offsetof(GxVertex, c0), GX2_ATTRIB_FORMAT_UNORM_8_8_8_8);
   WHBGfxInitShaderAttribute(&sTev, "aC1", 0, offsetof(GxVertex, c1), GX2_ATTRIB_FORMAT_UNORM_8_8_8_8);
   static const char *tcs[4] = {"aT0", "aT1", "aT2", "aT3"};
   for (int i = 0; i < 4; i++)
      WHBGfxInitShaderAttribute(&sTev, tcs[i], 0, offsetof(GxVertex, tc) + i * 12, GX2_ATTRIB_FORMAT_FLOAT_32_32_32);
   // colours are bytes in memory order R, G, B, A (no swap); "clrswap" in port.cfg flips them
   for (uint32_t i = 0; i < sTev.numAttributes; i++)
      if (sTev.attributes[i].format == GX2_ATTRIB_FORMAT_UNORM_8_8_8_8)
         sTev.attributes[i].endianSwap = port_cfg("clrswap") ? GX2_ENDIAN_SWAP_8_IN_32 : GX2_ENDIAN_SWAP_NONE;
   WHBGfxInitFetchShader(&sTev);
   WHBGfxInitShaderAttribute(&sBlit, "aPos", 0, 0, GX2_ATTRIB_FORMAT_FLOAT_32_32_32_32);
   WHBGfxInitFetchShader(&sBlit);
   GX2VertexShader *vs = sTev.vertexShader;
   GX2PixelShader *ps = sTev.pixelShader;
   uProj = uniform_offset(vs->uniformVars, vs->uniformVarCount, "uProj");
   uStage = uniform_offset(ps->uniformVars, ps->uniformVarCount, "uStage");
   uRegs = uniform_offset(ps->uniformVars, ps->uniformVarCount, "uRegs");
   uKonst = uniform_offset(ps->uniformVars, ps->uniformVarCount, "uKonst");
   uSwap = uniform_offset(ps->uniformVars, ps->uniformVarCount, "uSwap");
   uMisc = uniform_offset(ps->uniformVars, ps->uniformVarCount, "uMisc");
   char n[8];
   for (int i = 0; i < GX_MAX_SLOTS; i++) {
      snprintf(n, sizeof(n), "uTex%d", i);
      uSampler[i] = sampler_loc(ps, n);
   }
   uBlitSrc = sampler_loc(sBlit.pixelShader, "uSrc");
   sBlockMode = vs->mode == GX2_SHADER_MODE_UNIFORM_BLOCK || ps->mode == GX2_SHADER_MODE_UNIFORM_BLOCK ||
                vs->uniformBlockCount || ps->uniformBlockCount;
   WHBLogPrintf("[gfx] shader modes: vs %d (%u blocks) ps %d (%u blocks)", (int)vs->mode, (unsigned)vs->uniformBlockCount,
                (int)ps->mode, (unsigned)ps->uniformBlockCount);
   for (uint32_t i = 0; i < vs->uniformBlockCount; i++)
      WHBLogPrintf("[gfx]   vs block '%s' location %u size %u", vs->uniformBlocks[i].name, (unsigned)vs->uniformBlocks[i].offset,
                   (unsigned)vs->uniformBlocks[i].size);
   for (uint32_t i = 0; i < ps->uniformBlockCount; i++)
      WHBLogPrintf("[gfx]   ps block '%s' location %u size %u", ps->uniformBlocks[i].name, (unsigned)ps->uniformBlocks[i].offset,
                   (unsigned)ps->uniformBlocks[i].size);
   if (sBlockMode) {
      // Uniform-block mode. CafeGLSL v0.2 has no uniform registers: loose uniforms are mapped to
      // a virtual uniform block bound to buffer 15, at byte offsets (CafeGLSL README).
      if (vs->uniformBlockCount) { sVsBlockLoc = vs->uniformBlocks[0].offset; sVsBlockSize = vs->uniformBlocks[0].size; }
      else { sVsBlockLoc = 15; sVsBlockSize = (uint32_t)(uProj >= 0 ? uProj : 0) + 64; }
      if (ps->uniformBlockCount) { sPsBlockLoc = ps->uniformBlocks[0].offset; sPsBlockSize = ps->uniformBlocks[0].size; }
      else {
         int32_t end = 0, offs[5] = {uStage, uRegs, uKonst, uSwap, uMisc};
         int32_t sizes[5] = {GX_MAX_STAGES * 7 * 16, 64, 64, 64, 32};
         for (int i = 0; i < 5; i++)
            if (offs[i] >= 0 && offs[i] + sizes[i] > end) end = offs[i] + sizes[i];
         sPsBlockLoc = 15;
         sPsBlockSize = (uint32_t)end;
      }
      sUboSwap = !port_cfg("ubonoswap"); // confirmed on hardware (first-boot 32): blocks need byte swapping
      sUbo = MEMAllocFromDefaultHeapEx(UBO_RING, 256);
      WHBLogPrintf("[gfx] uniform blocks: vs loc %d size %u, ps loc %d size %u, byte swap %d", (int)sVsBlockLoc,
                   (unsigned)sVsBlockSize, (int)sPsBlockLoc, (unsigned)sPsBlockSize, sUboSwap);
   } else if (uRegs - uStage == GX_MAX_STAGES * 7 * 16 || uMisc >= 1024) {
      uProj = uProj >= 0 ? uProj / 4 : -1;
      uStage = uStage >= 0 ? uStage / 4 : -1;
      uRegs = uRegs >= 0 ? uRegs / 4 : -1;
      uKonst = uKonst >= 0 ? uKonst / 4 : -1;
      uSwap = uSwap >= 0 ? uSwap / 4 : -1;
      uMisc = uMisc >= 0 ? uMisc / 4 : -1;
      WHBLogPrintf("[gfx] register offsets are in bytes - converted to words");
   }
   WHBLogPrintf("[gfx] shaders ready: proj@%d stage@%d regs@%d konst@%d swap@%d misc@%d tex0@%d blit@%d", (int)uProj,
                (int)uStage, (int)uRegs, (int)uKonst, (int)uSwap, (int)uMisc, (int)uSampler[0], (int)uBlitSrc);
   return 1;
}

// ---------------------------------------------------------------------------------------
// Texture cache
// ---------------------------------------------------------------------------------------
typedef struct {
   GxTexKey key;
   uint32_t hash;
   uint32_t lastUsed;
   uint32_t bytes;
   int render;      // EFB copy target
   int iconHalf;    // Classic Controller picture half (1 colour, 2 alpha), 0 otherwise
   int iconKind;    // controller kind the picture was decoded for
   uint32_t checked; // frame whose content hash was last compared (once per frame is enough)
   GX2Texture tex;
} TexEntry;

#define MAX_TEX 1536
#define TEX_BUDGET (192u << 20)
static TexEntry sTex[MAX_TEX];
static uint32_t sNumTex, sTexBytes, sFrame;
static GX2Texture sWhite;
static uint32_t sTexStats[3]; // decoded, evicted, render targets

// Textures whose contents changed before (video frames, animated UI) are compared in full:
// 16 samples miss most changes of a video frame (flat backgrounds), which left parts of old
// frames on screen. Everything else: 16 samples, checked once per frame.
#define DYN_MAX 64
static uint32_t sDynAddr[DYN_MAX], sDynCount[DYN_MAX];
static int dyn_find(uint32_t image)
{
   for (int i = 0; i < DYN_MAX; i++) if (sDynAddr[i] == image) return i;
   return -1;
}
static void dyn_changed(uint32_t image)
{
   int i = dyn_find(image);
   if (i < 0) {
      static uint32_t next;
      i = (int)(next++ % DYN_MAX);
      sDynAddr[i] = image;
      sDynCount[i] = 0;
   }
   sDynCount[i]++;
}

static uint32_t content_hash(const GxTexKey *k)
{
   uint32_t size = gx_tex_buffer_size(k->width, k->height, k->fmt);
   const uint32_t *p = (const uint32_t *)k->image;
   uint32_t n = size / 4, h = 2166136261u;
   if (!n) return 0;
   static int oldHash = -1;
   if (oldHash < 0) oldHash = port_cfg("oldhash"); // only the 16 samples, as before first-boot 65
   int d = oldHash ? -1 : dyn_find(k->image);
   if (d >= 0 && sDynCount[d] >= 2) {
      uint32_t a = h, b = 0x9E3779B9u;
      uint32_t i = 0;
      for (; i + 4 <= n; i += 4) {
         a = (a ^ p[i]) * 16777619u ^ p[i + 1];
         b = (b ^ p[i + 2]) * 2246822519u ^ p[i + 3];
      }
      for (; i < n; i++) a = (a ^ p[i]) * 16777619u;
      return a ^ (b * 3266489917u);
   }
   for (uint32_t i = 0; i < 16; i++) h = (h ^ p[(uint64_t)i * (n - 1) / 15]) * 16777619u;
   return h;
}

static int make_texture_ex(GX2Texture *t, uint32_t w, uint32_t h, GX2TileMode tile, uint32_t use, GX2SurfaceFormat fmt,
                           uint32_t compMap)
{
   memset(t, 0, sizeof(*t));
   GX2Surface *s = &t->surface;
   s->dim = GX2_SURFACE_DIM_TEXTURE_2D;
   s->width = w; s->height = h; s->depth = 1; s->mipLevels = 1;
   s->format = fmt;
   s->aa = GX2_AA_MODE1X;
   s->use = (GX2SurfaceUse)use;
   s->tileMode = tile;
   t->viewNumMips = 1;
   t->viewNumSlices = 1;
   t->compMap = compMap;
   GX2CalcSurfaceSizeAndAlignment(s);
   GX2InitTextureRegs(t);
   s->image = MEMAllocFromDefaultHeapEx(s->imageSize, s->alignment);
   return s->image != NULL;
}

static int make_texture(GX2Texture *t, uint32_t w, uint32_t h, GX2TileMode tile, uint32_t use)
{
   return make_texture_ex(t, w, h, tile, use, GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8, 0x00010203);
}

// Compact texture formats (gx_tex.c): BC1 for CMPR, R8 / R8G8 for intensity formats.
// "nocompact" in port.cfg: everything RGBA8 as before.
static int sCompact = -1;
static int make_game_texture(GX2Texture *t, const GxTexKey *k, int kind, GX2TileMode tile)
{
   int ok;
   switch (kind) {
   case 1: ok = make_texture_ex(t, k->width, k->height, tile, GX2_SURFACE_USE_TEXTURE, GX2_SURFACE_FORMAT_UNORM_BC1, 0x00010203); break;
   case 2: ok = make_texture_ex(t, k->width, k->height, tile, GX2_SURFACE_USE_TEXTURE, GX2_SURFACE_FORMAT_UNORM_R8, 0x00000000); break;
   case 3: ok = make_texture_ex(t, k->width, k->height, tile, GX2_SURFACE_USE_TEXTURE, GX2_SURFACE_FORMAT_UNORM_R8_G8, 0x00000001); break;
   default: ok = make_texture(t, k->width, k->height, tile, GX2_SURFACE_USE_TEXTURE); break;
   }
   static int logged[4];
   if (ok && kind && logged[kind]++ < 2)
      WHBLogPrintf("[tex] kind %d %ux%u: asked tile %d, got tile %d, pitch %u, %u bytes", kind, k->width, k->height, (int)tile,
                   (int)t->surface.tileMode, (unsigned)t->surface.pitch, (unsigned)t->surface.imageSize);
   return ok;
}
static int sSurfaceCopied; // a GPU copy changed the drawing state (apply_state restores it)

static void decode_game_texture(GX2Texture *t, const GxTexKey *k, int kind)
{
   GX2Surface *s = &t->surface;
   if (kind == 1) {
      // Is the pitch of a compressed surface counted in blocks or in pixels? Measured once on
      // a 1024-pixel-wide surface (256 = blocks).
      static int inPixels = -1;
      if (inPixels < 0) {
         GX2Surface probe;
         memset(&probe, 0, sizeof(probe));
         probe.dim = GX2_SURFACE_DIM_TEXTURE_2D;
         probe.width = 1024; probe.height = 4; probe.depth = 1; probe.mipLevels = 1;
         probe.format = GX2_SURFACE_FORMAT_UNORM_BC1;
         probe.aa = GX2_AA_MODE1X;
         probe.use = GX2_SURFACE_USE_TEXTURE;
         probe.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
         GX2CalcSurfaceSizeAndAlignment(&probe);
         inPixels = probe.pitch >= 1024;
         WHBLogPrintf("[tex] BC1 pitch of a 1024-wide surface: %u (%s)", (unsigned)probe.pitch, inPixels ? "pixels" : "blocks");
      }
      uint32_t pitch = inPixels ? s->pitch / 4 : s->pitch;
      gx_decode_bc1(k, s->image, pitch);
   } else if (kind) {
      gx_decode_small(k, s->image, s->pitch);
   } else {
      gx_decode_texture(k, (uint32_t *)s->image, s->pitch);
   }
}

// GPU memory still referenced by a frame in flight is freed once that frame is done:
// gpu_free_later() collects it for the frame being built, gpu_free_frame() runs after the
// renderer waited for that frame's timestamp (two lists: built / in flight).
#define FREE_MAX 1024
static void *sFreeList[2][FREE_MAX];
static uint32_t sFreeCount[2], sFreeCur;
static void gpu_free_later(void *p)
{
   if (!p) return;
   if (sFreeCount[sFreeCur] >= FREE_MAX) { GX2DrawDone(); MEMFreeToDefaultHeap(p); return; }
   sFreeList[sFreeCur][sFreeCount[sFreeCur]++] = p;
}
// Called once per frame after the previous frame finished on the GPU.
static void gpu_free_frame(void)
{
   uint32_t done = sFreeCur ^ 1; // the list of the frame that just finished
   for (uint32_t i = 0; i < sFreeCount[done]; i++) MEMFreeToDefaultHeap(sFreeList[done][i]);
   sFreeCount[done] = 0;
   sFreeCur = done; // the next frame collects here
}

// Tiled textures (much faster for the GPU to sample than linear ones): decoded into a
// linear staging surface, then copied (and tiled) by the GPU. Off by default ("tiledtex" in
// port.cfg): GX2CopySurface changes GPU state in the middle of a frame (graphical errors).
static int sLinearTex = -1;

static void evict_one(void)
{
   uint32_t best = 0xFFFFFFFF, bi = 0;
   for (uint32_t i = 0; i < sNumTex; i++)
      if (!sTex[i].render && sTex[i].lastUsed < best && sTex[i].lastUsed + 2 < sFrame) { best = sTex[i].lastUsed; bi = i; }
   if (best == 0xFFFFFFFF) return;
   gpu_free_later(sTex[bi].tex.surface.image);
   sTexBytes -= sTex[bi].bytes;
   sTex[bi] = sTex[--sNumTex];
   sTexStats[1]++;
}

static TexEntry *find_tex(uint32_t image)
{
   for (uint32_t i = 0; i < sNumTex; i++)
      if (sTex[i].key.image == image) return &sTex[i];
   return NULL;
}

// A texture whose contents keep changing at the same address is a video frame (Bink).
static uint32_t sVideoTex;
static void video_note(const GxTexKey *k)
{
   static uint32_t addr[16], cnt[16];
   if (sVideoTex) return;
   for (int i = 0; i < 16; i++) {
      if (addr[i] == k->image || !addr[i]) {
         addr[i] = k->image;
         if (++cnt[i] == 3) {
            sVideoTex = k->image;
            WHBLogPrintf("[video] texture %08X %ux%u fmt %u wrap %u/%u mip %u changes every frame", (unsigned)k->image, k->width,
                         k->height, k->fmt, k->wrapS, k->wrapT, k->mipmap);
         }
         return;
      }
   }
}

static uint16_t sTexSlot[4096]; // image address -> likely sTex index (verified on use)

// The game's Classic Controller picture (two 256x256 CMPR halves: colour, alpha) is swapped
// for the GamePad or Pro Controller picture (sd:/rayman/mods/icons/gamepad.bin, pro.bin,
// same layout) when that is the controller in use.
#define ICON_HALF 43680
static const uint32_t kIconHash[2] = {0x12536B97u, 0xA4C8E7A5u}; // colour, alpha (level 0)
static uint8_t *sIconData[3]; // by controller kind (1 GamePad, 2 Pro)
static int sIconLoaded;

static uint32_t fnv_full(const uint8_t *p, uint32_t n)
{
   uint32_t h = 2166136261u;
   for (uint32_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
   return h;
}

static void icons_load(void)
{
   if (sIconLoaded) return;
   sIconLoaded = 1;
   static const char *const names[3] = {NULL, "gamepad", "pro"};
   for (int k = 1; k < 3; k++) {
      char path[128];
      snprintf(path, sizeof(path), "fs:/vol/external01/rayman/mods/icons/%s.bin", names[k]);
      FILE *f = fopen(path, "rb");
      if (!f) continue;
      uint8_t *b = MEMAllocFromDefaultHeapEx(2 * ICON_HALF, 64);
      if (b && fread(b, 1, 2 * ICON_HALF, f) == 2 * ICON_HALF) sIconData[k] = b;
      fclose(f);
      WHBLogPrintf("[tex] controller picture %s: %s", names[k], sIconData[k] ? "loaded" : "unreadable");
   }
}

// 0, or 1/2 for the colour/alpha half of the Classic Controller picture.
static int icon_half(const GxTexKey *k)
{
   if (k->width != 256 || k->height != 256) return 0;
   uint32_t h = fnv_full((const uint8_t *)k->image, 32768);
   static int logged;
   if (logged < 40) {
      logged++;
      const uint8_t *b = (const uint8_t *)k->image;
      WHBLogPrintf("[tex] 256x256 fmt %u at %08X hash %08X first %02X%02X%02X%02X%02X%02X%02X%02X", k->fmt, (unsigned)k->image,
                   (unsigned)h, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
   }
   if (k->fmt != 14) return 0;
   return h == kIconHash[0] ? 1 : h == kIconHash[1] ? 2 : 0;
}

static GX2Texture *get_texture(const GxTexKey *k)
{
   if (!k->image || !k->width || !k->height) return &sWhite;
   uint32_t slot = (k->image >> 5) & 4095, cand = sTexSlot[slot];
   for (uint32_t pass = 0; pass < 2; pass++) {
      uint32_t i0 = pass ? 0 : cand, i1 = pass ? sNumTex : cand + 1;
      if (!pass && cand >= sNumTex) continue;
      for (uint32_t i = i0; i < i1; i++) {
         TexEntry *e = &sTex[i];
         if (e->key.image != k->image) continue;
         if (e->render) { e->lastUsed = sFrame; sTexSlot[slot] = (uint16_t)i; return &e->tex; }
         if (e->key.width == k->width && e->key.height == k->height && e->key.fmt == k->fmt) {
            extern volatile int port_pad_kind;
            if (e->iconHalf && e->iconKind != port_pad_kind) e->checked = 0, e->hash ^= 1; // other controller: decode again
            if (e->checked == sFrame || e->hash == content_hash(k)) {
               e->checked = sFrame;
               e->lastUsed = sFrame;
               sTexSlot[slot] = (uint16_t)i;
               return &e->tex;
            }
            video_note(k);
            dyn_changed(k->image);
            gpu_free_later(e->tex.surface.image); // contents changed: decode again
            sTexBytes -= e->bytes;
            *e = sTex[--sNumTex];
            pass = 2;
            break;
         }
      }
   }
   if (gx_is_render_texture(k->image)) return &sWhite; // copy target not written yet
   while ((sNumTex >= MAX_TEX || sTexBytes > TEX_BUDGET) && sNumTex) {
      uint32_t before = sNumTex;
      evict_one();
      if (sNumTex == before) break;
   }
   if (sNumTex >= MAX_TEX) return &sWhite;
   TexEntry *e = &sTex[sNumTex];
   memset(e, 0, sizeof(*e));
   if (sLinearTex < 0) sLinearTex = !port_cfg("tiledtex"); // tiled: opt-in until the graphics are right
   static int sBc1 = -1, sSmall = -1;
   if (sBc1 < 0) sBc1 = port_cfg("bc1");           // BC1: off by default (wrong graphics in first-boot 65/66)
   if (sSmall < 0) sSmall = port_cfg("smalltex");  // R8 / R8G8: off by default (freeze at the intro)
   int kind = gx_compact_kind(k->fmt);
   if ((kind == 1 && !sBc1) || (kind > 1 && !sSmall)) kind = 0;
   GX2Texture stage;
   int tiled = kind <= 1 && !sLinearTex && k->width >= 16 && k->height >= 16;
   if (!make_game_texture(&e->tex, k, kind, tiled ? GX2_TILE_MODE_DEFAULT : GX2_TILE_MODE_LINEAR_ALIGNED)) return &sWhite;
   if (tiled && e->tex.surface.tileMode == GX2_TILE_MODE_LINEAR_ALIGNED) tiled = 0; // nothing to tile
   if (tiled && !make_game_texture(&stage, k, kind, GX2_TILE_MODE_LINEAR_ALIGNED)) {
      MEMFreeToDefaultHeap(e->tex.surface.image);
      return &sWhite;
   }
   GX2Texture *dec = tiled ? &stage : &e->tex;
   {
      extern volatile int port_pad_kind;
      icons_load();
      int half = icon_half(k), kind = port_pad_kind;
      GxTexKey src = *k;
      if (half && kind > 0 && kind < 3 && sIconData[kind]) src.image = (uint32_t)(sIconData[kind] + (half - 1) * ICON_HALF);
      decode_game_texture(dec, &src, kind);
      e->iconHalf = half;
      e->iconKind = kind;
   }
   DCFlushRange(dec->surface.image, dec->surface.imageSize);
   GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, dec->surface.image, dec->surface.imageSize);
   if (tiled) {
      GX2CopySurface(&stage.surface, 0, 0, &e->tex.surface, 0, 0);
      sSurfaceCopied = 1;
      GX2Invalidate(GX2_INVALIDATE_MODE_TEXTURE, e->tex.surface.image, e->tex.surface.imageSize);
      gpu_free_later(stage.surface.image);
   }
   e->key = *k;
   e->hash = content_hash(k);
   e->lastUsed = sFrame;
   e->checked = sFrame;
   sTexSlot[slot] = (uint16_t)sNumTex;
   e->bytes = e->tex.surface.imageSize;
   sTexBytes += e->bytes;
   sNumTex++;
   sTexStats[0]++;
   return &e->tex;
}

// ---------------------------------------------------------------------------------------
// Packet playback
// ---------------------------------------------------------------------------------------
static const uint8_t kSrcBlend[8] = {GX2_BLEND_MODE_ZERO, GX2_BLEND_MODE_ONE, GX2_BLEND_MODE_DST_COLOR, GX2_BLEND_MODE_INV_DST_COLOR,
                                     GX2_BLEND_MODE_SRC_ALPHA, GX2_BLEND_MODE_INV_SRC_ALPHA, GX2_BLEND_MODE_DST_ALPHA,
                                     GX2_BLEND_MODE_INV_DST_ALPHA};
static const uint8_t kDstBlend[8] = {GX2_BLEND_MODE_ZERO, GX2_BLEND_MODE_ONE, GX2_BLEND_MODE_SRC_COLOR, GX2_BLEND_MODE_INV_SRC_COLOR,
                                     GX2_BLEND_MODE_SRC_ALPHA, GX2_BLEND_MODE_INV_SRC_ALPHA, GX2_BLEND_MODE_DST_ALPHA,
                                     GX2_BLEND_MODE_INV_DST_ALPHA};
static const uint8_t kRop[16] = {0x00, 0x88, 0x44, 0xCC, 0x22, 0xAA, 0x66, 0xEE, 0x11, 0x99, 0x55, 0xDD, 0x33, 0xBB, 0x77, 0xFF};

// GPU step tracing: the first few occurrences of each GPU operation are logged, given time
// to reach the PC, executed and waited for, so a GPU hang names its culprit in the log.
static int step_begin(const char *name, int *count)
{
   if (*count >= 3) return 0;
   (*count)++;
   WHBLogPrintf("[gpu] %s #%d ...", name, *count);
   OSSleepTicks(OSMillisecondsToTicks(60));
   return 1;
}

static void step_end(int traced, const char *name)
{
   if (!traced) return;
   GX2DrawDone();
   WHBLogPrintf("[gpu] %s ok", name);
}

#define STEP(name, code) do { static int _n; int _t = step_begin(name, &_n); code; step_end(_t, name); } while (0)

static void bind_efb(void)
{
   GX2SetContextState(sCtx);
   GX2SetColorBuffer(&sEfb, GX2_RENDER_TARGET_0);
   GX2SetDepthBuffer(&sEfbDepth);
   GX2SetShaderMode(sBlockMode ? GX2_SHADER_MODE_UNIFORM_BLOCK : GX2_SHADER_MODE_UNIFORM_REGISTER);
   GX2SetRasterizerClipControl(TRUE, FALSE); // GX depth ranges differ: never clip on z
   GX2SetFetchShader(&sTev.fetchShader);
   GX2SetVertexShader(sTev.vertexShader);
   GX2SetPixelShader(sTev.pixelShader);
   GX2SetCullOnlyControl(GX2_FRONT_FACE_CCW, FALSE, FALSE);
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

// Uploads uniform registers, refusing anything outside the 256-vec4 register file (an
// out-of-range write can hang the GPU).
static void set_uniform(int vertex, int32_t off, uint32_t count, const void *data)
{
   static int warned;
   if (off < 0) return;
   if ((uint32_t)off + count > 1024) {
      if (!warned++) WHBLogPrintf("[gfx] uniform at %d (+%u) outside the register file - skipped", (int)off, (unsigned)count);
      return;
   }
   if (vertex) GX2SetVertexUniformReg(off, count, data);
   else GX2SetPixelUniformReg(off, count, data);
}

// ---------------------------------------------------------------------------------------
// Specialised TEV shaders: one straight-line pixel shader per TEV configuration, generated
// from the state and compiled in the background (CPU0, big stack). Until a configuration's
// shader is ready, the ubershader draws it. Values that change per draw (registers, konst
// colours, swap tables, alpha references, texcoord scales) stay uniforms.
// ---------------------------------------------------------------------------------------
typedef struct {
   uint32_t key;
   volatile int state;      // 0 queued, 1 ready, 2 failed
   GX2PixelShader *ps;
   int32_t oStage, oRegs, oKonst, oSwap, oMisc;
   uint32_t size;
   char *src;
} SpecProg;

#define MAX_SPEC 384
static SpecProg sSpec[MAX_SPEC];
static volatile int sNumSpec;
static OSSemaphore sSpecSem;
static OSThread sSpecThread __attribute__((aligned(16)));
static int sSpecOn;
static uint32_t sSpecStats[3]; // compiled, failed, draws with specialised shader

static int rnd(float f) { return (int)(f + (f < 0 ? -0.5f : 0.5f)); }

static int stage_indirect(const GxState *st, int s)
{
   return st->indMtx[s] || st->indWrapS[s] || st->indWrapT[s] || st->indAddPrev[s];
}

static int state_indirect(const GxState *st)
{
   int ns = rnd(st->misc[0][0]);
   for (int s = 0; s < ns && s < GX_MAX_STAGES; s++)
      if (stage_indirect(st, s)) return 1;
   return 0;
}

// Texture maps read by the active stages, including indirect lookups.
static uint32_t state_slots(const GxState *st)
{
   int ns = rnd(st->misc[0][0]);
   uint32_t used = 0;
   for (int s = 0; s < ns && s < GX_MAX_STAGES; s++) {
      int slot = rnd(st->stage[s * 7 + 5][0]);
      if (slot >= 0 && slot < GX_MAX_SLOTS) used |= 1u << slot;
      if (stage_indirect(st, s) && st->indMtx[s]) used |= 1u << (st->indMap[st->indStage[s]] & 7);
   }
   return used;
}

static uint32_t spec_key(const GxState *st)
{
   int ns = rnd(st->misc[0][0]);
   if (ns > GX_MAX_STAGES) ns = GX_MAX_STAGES;
   uint32_t h = 2166136261u ^ (uint32_t)ns;
   const uint8_t *b = (const uint8_t *)st->stage;
   for (uint32_t i = 0; i < (uint32_t)ns * 7 * 16; i++) {
      if ((i % 112) >= 96 + 4) continue; // row 6: only .x (tex swap) is configuration
      h = (h ^ b[i]) * 16777619u;
   }
   float a[3] = {st->misc[0][1], st->misc[0][3], st->misc[1][0]};
   b = (const uint8_t *)a;
   for (int i = 0; i < 12; i++) h = (h ^ b[i]) * 16777619u;
   if (state_indirect(st)) {
      b = st->indStage;
      const uint8_t *e = (const uint8_t *)(st->indScale + 3);
      for (; b < e; b++) h = (h ^ *b) * 16777619u;
      for (int i = 0; i < GX_MAX_SLOTS; i++) h = (h ^ st->tex[i].wrapS ^ (st->tex[i].wrapT << 2)) * 16777619u;
   }
   return h ? h : 1;
}

static const char *const kCin[16] = {"p.rgb", "vec3(p.a)", "r0.rgb", "vec3(r0.a)", "r1.rgb", "vec3(r1.a)", "r2.rgb",
                                     "vec3(r2.a)", "t.rgb", "vec3(t.a)", "r.rgb", "vec3(r.a)", "vec3(1.0)", "vec3(0.5)",
                                     "kc", "vec3(0.0)"};
static const char *const kAin[8] = {"p.a", "r0.a", "r1.a", "r2.a", "t.a", "r.a", "ka", "0.0"};
static const float kBias[3] = {0.0f, 0.5f, -0.5f};
static const float kScale[4] = {1.0f, 2.0f, 4.0f, 0.5f};

#define EMIT(...) (n += snprintf(out + n, (size_t)(cap - n > 0 ? cap - n : 0), __VA_ARGS__))

static void emit_acmp(char *out, int *pn, int cap, const char *name, int f, const char *ref)
{
   int n = *pn;
   static const char *const ops[8] = {"false", "%s < %s", "abs(%s - %s) < 0.002", "%s <= %s + 0.002", "%s > %s",
                                      "abs(%s - %s) >= 0.002", "%s >= %s - 0.002", "true"};
   EMIT("  bool %s = ", name);
   if (f == 0 || f == 7) EMIT("%s;\n", ops[f]);
   else { EMIT(ops[f & 7], "p.a", ref); EMIT(";\n"); }
   *pn = n;
}

static int spec_source(const GxState *st, char *out, int cap)
{
   int n = 0, ns = rnd(st->misc[0][0]);
   if (ns > GX_MAX_STAGES) ns = GX_MAX_STAGES;
   if (ns < 1) ns = 1;
   uint32_t slots = state_slots(st);
   EMIT("#version 420 core\n"
        "layout(location = 0) in vec4 vC0; layout(location = 1) in vec4 vC1;\n"
        "layout(location = 2) in vec4 vT0; layout(location = 3) in vec4 vT1;\n"
        "layout(location = 4) in vec4 vT2; layout(location = 5) in vec4 vT3;\n"
        "layout(location = 0) out vec4 fragColor;\n");
   for (int i = 0; i < GX_MAX_SLOTS; i++)
      if (slots & (1u << i)) EMIT("layout(binding = %d) uniform sampler2D uTex%d;\n", i, i);
   EMIT("uniform vec4 uStage[56];\nuniform vec4 uRegs[4];\nuniform vec4 uKonst[4];\nuniform vec4 uSwap[4];\nuniform vec4 uMisc[2];\n"
        "int ri(float f) { return int(floor(f + 0.5)); }\n"
        "vec4 swz(vec4 c, int t) { vec4 s = uSwap[t]; return vec4(c[ri(s.x)], c[ri(s.y)], c[ri(s.z)], c[ri(s.w)]); }\n"
        "bool cmp(int op, float a, float b) { return (op & 1) == 0 ? a > b + 0.5 / 255.0 : abs(a - b) < 0.5 / 255.0; }\n"
        "float pk(vec3 c, int n) { vec3 q = floor(c * 255.0 + 0.5); return n == 1 ? q.r : (n == 2 ? q.g * 256.0 + q.r : q.b * 65536.0 + q.g * 256.0 + q.r); }\n"
        "void main() {\n"
        "  vec4 p = uRegs[0], r0 = uRegs[1], r1 = uRegs[2], r2 = uRegs[3];\n"
        "  vec4 t, r; vec3 kc, rc; float ka, ra;\n"
        "  vec2 icPrev = vec2(0.0);\n");
   // indirect lookups: (s, t, u) = alpha, blue, green of the indirect map, 0..255
   uint32_t indDone = 0;
   for (int s = 0; s < ns; s++) {
      if (!stage_indirect(st, s) || !st->indMtx[s]) continue;
      int i = st->indStage[s] & 3;
      if (indDone & (1u << i)) continue;
      indDone |= 1u << i;
      int c = st->indCoord[i] & 3, m = st->indMap[i] & 7;
      static const char *const wr[3] = {"clamp(c, ivec2(0), sz - 1)", "((c %% sz) + sz) %% sz", "((c %% (2 * sz)) + 2 * sz) %% (2 * sz)"};
      EMIT("  vec3 ind%d;\n  { ivec2 sz = textureSize(uTex%d, 0); ivec2 c = ivec2(floor(vT%d.xy / (abs(vT%d.z) < 1e-6 ? 1.0 : vT%d.z) * vec2(sz)));\n", i, m, c, c, c);
      int ws = st->tex[m].wrapS < 3 ? st->tex[m].wrapS : 0, wt = st->tex[m].wrapT < 3 ? st->tex[m].wrapT : 0;
      EMIT("    ivec2 cs = "); EMIT(wr[ws]); EMIT(", ct = "); EMIT(wr[wt]); EMIT(";\n");
      EMIT("    vec4 v = floor(texelFetch(uTex%d, ivec2(cs.x, ct.y), 0) * 255.0 + 0.5); ind%d = vec3(v.a, v.b, v.g); }\n", m, i);
   }
   for (int s = 0; s < ns; s++) {
      const float(*q)[4] = &st->stage[s * 7];
      int ci[4], ai[4];
      for (int k = 0; k < 4; k++) { ci[k] = rnd(q[0][k]) & 15; ai[k] = rnd(q[1][k]) & 7; }
      int cop = rnd(q[2][0]), aop = rnd(q[3][0]);
      int slot = rnd(q[5][0]), coord = rnd(q[5][1]), chan = rnd(q[5][2]), rsw = rnd(q[5][3]) & 3, tsw = rnd(q[6][0]) & 3;
      int kcs = rnd(q[4][2]), kas = rnd(q[4][3]), oc = rnd(q[4][0]) & 3, oa = rnd(q[4][1]) & 3;
      EMIT("  // stage %d\n", s);
      if (slot >= 0 && slot < GX_MAX_SLOTS && stage_indirect(st, s)) {
         if (coord < 0 || coord > 3) coord = 0;
         EMIT("  { vec2 sz = vec2(textureSize(uTex%d, 0));\n    vec2 tc = vT%d.xy / (abs(vT%d.z) < 1e-6 ? 1.0 : vT%d.z) * uStage[%d].yz * sz;\n",
              slot, coord, coord, coord, s * 7 + 6);
         static const float kWrap[7] = {0, 256, 128, 64, 32, 16, 0};
         for (int a = 0; a < 2; a++) {
            int w = a ? st->indWrapT[s] : st->indWrapS[s];
            char ax = a ? 'y' : 'x';
            if (w == 6) EMIT("    tc.%c = 0.0;\n", ax);
            else if (w >= 1 && w <= 5) EMIT("    tc.%c = mod(tc.%c, %.1f);\n", ax, ax, kWrap[w]);
         }
         int mtx = st->indMtx[s];
         if (mtx >= 1 && mtx <= 3) {
            int i = st->indStage[s] & 3, fmt = st->indFmt[s] & 3, bias = st->indBias[s];
            static const char *const mask[4] = {"", "mod(iv, 32.0)", "mod(iv, 16.0)", "mod(iv, 8.0)"};
            EMIT("    vec3 iv = ind%d;\n", i);
            if (fmt) EMIT("    iv = %s;\n", mask[fmt]);
            float bv = fmt == 0 ? -128.0f : 1.0f;
            if (bias) EMIT("    iv += vec3(%.1f, %.1f, %.1f);\n", (bias & 1) ? bv : 0.0f, (bias & 2) ? bv : 0.0f, (bias & 4) ? bv : 0.0f);
            const float *m = st->indM[mtx - 1];
            float sc = 1.0f;
            int e = st->indScale[mtx - 1];
            for (; e > 0; e--) sc *= 2.0f;
            for (; e < 0; e++) sc *= 0.5f;
            EMIT("    tc += vec2(dot(vec3(%.6f, %.6f, %.6f), iv), dot(vec3(%.6f, %.6f, %.6f), iv)) * %.6f;\n", m[0], m[1], m[2], m[3],
                 m[4], m[5], sc);
         }
         if (st->indAddPrev[s]) EMIT("    tc += icPrev;\n");
         EMIT("    icPrev = tc;\n    t = swz(texture(uTex%d, tc / sz), %d); }\n", slot, tsw);
      } else if (slot >= 0 && slot < GX_MAX_SLOTS) {
         if (coord < 0 || coord > 3) coord = 0;
         EMIT("  t = swz(texture(uTex%d, vT%d.xy / (abs(vT%d.z) < 1e-6 ? 1.0 : vT%d.z) * uStage[%d].yz), %d);\n", slot, coord, coord,
              coord, s * 7 + 6, tsw);
      } else EMIT("  t = vec4(1.0);\n");
      EMIT("  r = swz(%s, %d);\n", chan == 0 ? "vC0" : chan == 1 ? "vC1" : "vec4(0.0)", rsw);
      if (kcs < 8) EMIT("  kc = vec3(%.5f);\n", (8 - kcs) / 8.0f);
      else if (kcs >= 12 && kcs < 16) EMIT("  kc = uKonst[%d].rgb;\n", kcs - 12);
      else if (kcs >= 16 && kcs < 32) EMIT("  kc = vec3(uKonst[%d].%c);\n", kcs & 3, "rgba"[(kcs - 16) >> 2]);
      else EMIT("  kc = vec3(1.0);\n");
      if (kas < 8) EMIT("  ka = %.5f;\n", (8 - kas) / 8.0f);
      else if (kas >= 16 && kas < 32) EMIT("  ka = uKonst[%d].%c;\n", kas & 3, "rgba"[(kas - 16) >> 2]);
      else EMIT("  ka = 1.0;\n");
      if (cop < 2)
         EMIT("  rc = (%s %c mix(%s, %s, %s) + vec3(%.5f)) * %.5f;\n", kCin[ci[3]], cop == 1 ? '-' : '+', kCin[ci[0]], kCin[ci[1]],
              kCin[ci[2]], kBias[rnd(q[2][1]) % 3], kScale[rnd(q[2][2]) & 3]);
      else if (cop >= 14)
         EMIT("  { vec3 a = %s, b = %s, c = %s; rc = %s + vec3(cmp(%d, a.r, b.r) ? c.r : 0.0, cmp(%d, a.g, b.g) ? c.g : 0.0, cmp(%d, a.b, b.b) ? c.b : 0.0); }\n",
              kCin[ci[0]], kCin[ci[1]], kCin[ci[2]], kCin[ci[3]], cop, cop, cop);
      else
         EMIT("  rc = %s + (cmp(%d, pk(%s, %d), pk(%s, %d)) ? %s : vec3(0.0));\n", kCin[ci[3]], cop, kCin[ci[0]], (cop - 8) / 2 + 1,
              kCin[ci[1]], (cop - 8) / 2 + 1, kCin[ci[2]]);
      if (aop < 2)
         EMIT("  ra = (%s %c mix(%s, %s, %s) + %.5f) * %.5f;\n", kAin[ai[3]], aop == 1 ? '-' : '+', kAin[ai[0]], kAin[ai[1]],
              kAin[ai[2]], kBias[rnd(q[3][1]) % 3], kScale[rnd(q[3][2]) & 3]);
      else
         EMIT("  ra = %s + (cmp(%d, %s, %s) ? %s : 0.0);\n", kAin[ai[3]], aop, kAin[ai[0]], kAin[ai[1]], kAin[ai[2]]);
      EMIT(q[2][3] > 0.5f ? "  rc = clamp(rc, 0.0, 1.0);\n" : "  rc = clamp(rc, -4.0, 4.0);\n");
      EMIT(q[3][3] > 0.5f ? "  ra = clamp(ra, 0.0, 1.0);\n" : "  ra = clamp(ra, -4.0, 4.0);\n");
      static const char *const regs[4] = {"p", "r0", "r1", "r2"};
      EMIT("  %s.rgb = rc; %s.a = ra;\n", regs[oc], regs[oa]);
   }
   int c0 = rnd(st->misc[0][1]) & 7, c1 = rnd(st->misc[1][0]) & 7, aop = rnd(st->misc[0][3]) & 3;
   if (!((c0 == 7 && c1 == 7 && aop <= 1) || (aop == 1 && (c0 == 7 || c1 == 7)))) {
      emit_acmp(out, &n, cap, "t0", c0, "uMisc[0].z");
      emit_acmp(out, &n, cap, "t1", c1, "uMisc[1].y");
      static const char *const comb[4] = {"t0 && t1", "t0 || t1", "t0 != t1", "t0 == t1"};
      EMIT("  if (!(%s)) discard;\n", comb[aop]);
   }
   EMIT("  fragColor = clamp(p, 0.0, 1.0);\n}\n");
   return n < cap;
}

static int spec_main(int argc, const char **argv)
{
   static char log[512];
   for (;;) {
      OSWaitSemaphore(&sSpecSem);
      for (int i = 0; i < sNumSpec; i++) {
         SpecProg *p = &sSpec[i];
         if (p->state != 0 || !p->src) continue;
         log[0] = 0;
         GX2PixelShader *ps = glslPS(p->src, log, sizeof(log), 0);
         if (ps) {
            p->oStage = uniform_offset(ps->uniformVars, ps->uniformVarCount, "uStage");
            p->oRegs = uniform_offset(ps->uniformVars, ps->uniformVarCount, "uRegs");
            p->oKonst = uniform_offset(ps->uniformVars, ps->uniformVarCount, "uKonst");
            p->oSwap = uniform_offset(ps->uniformVars, ps->uniformVarCount, "uSwap");
            p->oMisc = uniform_offset(ps->uniformVars, ps->uniformVarCount, "uMisc");
            uint32_t end = 0;
            for (uint32_t k = 0; k < ps->uniformVarCount; k++) {
               uint32_t cnt = ps->uniformVars[k].count ? ps->uniformVars[k].count : 1;
               uint32_t e = ps->uniformVars[k].offset + cnt * 16;
               if (e > end) end = e;
            }
            p->size = end ? end : 16;
            p->ps = ps;
            __asm__ volatile("sync" ::: "memory");
            p->state = 1;
            sSpecStats[0]++;
         } else {
            p->state = 2;
            sSpecStats[1]++;
            WHBLogPrintf("[spec] compile failed (key %08X): %s", (unsigned)p->key, log);
         }
         free(p->src);
         p->src = NULL;
      }
   }
   return 0;
}

static void spec_start(void)
{
   sSpecOn = !port_cfg("nospec");
   if (!sSpecOn) return;
   OSInitSemaphore(&sSpecSem, 0);
   uint8_t *stack = MEMAllocFromDefaultHeapEx(SHADER_STACK, 16);
   if (!stack || !OSCreateThread(&sSpecThread, (OSThreadEntryPointFn)spec_main, 0, NULL, stack + SHADER_STACK, SHADER_STACK, 20,
                                 OS_THREAD_ATTRIB_AFFINITY_CPU0)) {
      sSpecOn = 0;
      return;
   }
   OSSetThreadName(&sSpecThread, "Rayman shader specialiser");
   OSResumeThread(&sSpecThread);
}

// The specialised program for this state, or NULL (queued for compilation / unavailable).
static SpecProg *spec_get(const GxState *st)
{
   if (!sSpecOn || !sBlockMode) return NULL;
   uint32_t key = spec_key(st);
   for (int i = 0; i < sNumSpec; i++)
      if (sSpec[i].key == key) return sSpec[i].state == 1 ? &sSpec[i] : NULL;
   if (sNumSpec >= MAX_SPEC) return NULL;
   SpecProg *p = &sSpec[sNumSpec];
   memset(p, 0, sizeof(*p));
   p->key = key;
   p->src = malloc(16384);
   if (!p->src || !spec_source(st, p->src, 16384)) { free(p->src); p->src = NULL; p->state = 2; sNumSpec++; return NULL; }
   __asm__ volatile("sync" ::: "memory");
   sNumSpec++;
   OSSignalSemaphore(&sSpecSem);
   return NULL;
}

// Uniform-block memory: a ring the GPU reads at draw time (reset per packet, after the
// previous frame's GX2DrawDone). GX2 reads uniform blocks byte-swapped (little-endian words).
static uint8_t *ubo_alloc(uint32_t size)
{
   if (!sUbo || !size) return NULL;
   size = (size + 255) & ~255u;
   if (!sUboEnd) sUboEnd = UBO_RING;
   if (sUboPos + size > sUboEnd) { GX2DrawDone(); sUboPos = sUboBase; }
   uint8_t *p = sUbo + sUboPos;
   sUboPos += size;
   memset(p, 0, size);
   return p;
}

static void ubo_put(uint8_t *block, uint32_t size, int32_t off, const void *src, uint32_t words)
{
   if (off < 0 || (uint32_t)off >= size) return;
   if ((uint32_t)off + words * 4 > size) words = (size - (uint32_t)off) / 4; // array trimmed by the compiler
   const uint32_t *in = src;
   uint32_t *out = (uint32_t *)(block + off);
   for (uint32_t i = 0; i < words; i++) out[i] = sUboSwap ? __builtin_bswap32(in[i]) : in[i];
}

static void ubo_done(uint8_t *block, uint32_t size)
{
   DCFlushRange(block, size);
   GX2Invalidate(GX2_INVALIDATE_MODE_CPU | GX2_INVALIDATE_MODE_UNIFORM_BLOCK, block, size);
}

static void apply_state(const GxState *st)
{
   // Textures first: creating one may run a GPU copy (tiling), which changes the drawing state,
   // so the EFB, shaders and the rest are (re)applied after it.
   uint32_t used = state_slots(st);
   GX2Texture *tx[GX_MAX_SLOTS];
   sSurfaceCopied = 0;
   for (int i = 0; i < GX_MAX_SLOTS; i++) tx[i] = uSampler[i] >= 0 && (used & (1u << i)) ? get_texture(&st->tex[i]) : &sWhite;
   if (sSurfaceCopied) bind_efb();
   const float *v = st->viewport;
   GX2SetViewport(v[0] * EFB_SCALE, v[1] * EFB_SCALE, v[2] * EFB_SCALE, v[3] * EFB_SCALE, v[4], v[5]);
   int x = clampi(st->scissor[0] * EFB_SCALE, 0, EFB_W), y = clampi(st->scissor[1] * EFB_SCALE, 0, EFB_H);
   int w = clampi(st->scissor[2] * EFB_SCALE, 0, EFB_W - x), h = clampi(st->scissor[3] * EFB_SCALE, 0, EFB_H - y);
   GX2SetScissor(x, y, w ? w : 1, h ? h : 1);

   if (sBlockMode) {
      uint8_t *vb = ubo_alloc(sVsBlockSize), *pb = ubo_alloc(sPsBlockSize);
      if (vb && sVsBlockLoc >= 0) {
         ubo_put(vb, sVsBlockSize, uProj, st->proj, 16);
         ubo_done(vb, sVsBlockSize);
         GX2SetVertexUniformBlock(sVsBlockLoc, sVsBlockSize, vb);
      }
      SpecProg *sp = spec_get(st);
      if (sp) {
         uint8_t *sb = ubo_alloc(sp->size);
         if (sb) {
            ubo_put(sb, sp->size, sp->oStage, st->stage, GX_MAX_STAGES * 7 * 4);
            ubo_put(sb, sp->size, sp->oRegs, st->regs, 16);
            ubo_put(sb, sp->size, sp->oKonst, st->konst, 16);
            ubo_put(sb, sp->size, sp->oSwap, st->swap, 16);
            ubo_put(sb, sp->size, sp->oMisc, st->misc, 8);
            ubo_done(sb, sp->size);
            GX2SetPixelShader(sp->ps);
            GX2SetPixelUniformBlock(15, sp->size, sb);
            sSpecStats[2]++;
         }
      } else if (pb && sPsBlockLoc >= 0) {
         GX2SetPixelShader(sTev.pixelShader);
         ubo_put(pb, sPsBlockSize, uStage, st->stage, GX_MAX_STAGES * 7 * 4);
         ubo_put(pb, sPsBlockSize, uRegs, st->regs, 16);
         ubo_put(pb, sPsBlockSize, uKonst, st->konst, 16);
         ubo_put(pb, sPsBlockSize, uSwap, st->swap, 16);
         ubo_put(pb, sPsBlockSize, uMisc, st->misc, 8);
         ubo_done(pb, sPsBlockSize);
         GX2SetPixelUniformBlock(sPsBlockLoc, sPsBlockSize, pb);
      }
   } else {
      set_uniform(1, uProj, 16, st->proj);
      set_uniform(0, uStage, GX_MAX_STAGES * 7 * 4, st->stage);
      set_uniform(0, uRegs, 16, st->regs);
      set_uniform(0, uKonst, 16, st->konst);
      set_uniform(0, uSwap, 16, st->swap);
      set_uniform(0, uMisc, 8, st->misc);
   }

   // textures used by the active stages (resolved above)
   for (int i = 0; i < GX_MAX_SLOTS; i++) {
      if (uSampler[i] < 0) continue;
      const GxTexKey *k = &st->tex[i];
      GX2Texture *t = tx[i];
      static const uint8_t wrap[3] = {GX2_TEX_CLAMP_MODE_CLAMP, GX2_TEX_CLAMP_MODE_WRAP, GX2_TEX_CLAMP_MODE_MIRROR};
      GX2Sampler smp;
      GX2InitSampler(&smp, GX2_TEX_CLAMP_MODE_CLAMP, GX2_TEX_XY_FILTER_MODE_LINEAR);
      GX2InitSamplerClamping(&smp, wrap[k->wrapS < 3 ? k->wrapS : 0], wrap[k->wrapT < 3 ? k->wrapT : 0], GX2_TEX_CLAMP_MODE_CLAMP);
      GX2InitSamplerXYFilter(&smp, k->magFilt ? GX2_TEX_XY_FILTER_MODE_LINEAR : GX2_TEX_XY_FILTER_MODE_POINT,
                             (k->minFilt & 1) || k->minFilt == 4 ? GX2_TEX_XY_FILTER_MODE_LINEAR : GX2_TEX_XY_FILTER_MODE_POINT,
                             GX2_TEX_ANISO_RATIO_NONE);
      GX2SetPixelTexture(t, uSampler[i]);
      GX2SetPixelSampler(&smp, uSampler[i]);
   }

   // pixel engine
   uint8_t mask = (st->colorUpdate ? GX2_CHANNEL_MASK_RGB : 0) | (st->alphaUpdate ? GX2_CHANNEL_MASK_A : 0);
   GX2SetTargetChannelMasks(mask, 0, 0, 0, 0, 0, 0, 0);
   if (st->blendType == 1 || st->blendType == 3) {
      GX2BlendMode src = st->blendType == 3 ? GX2_BLEND_MODE_ONE : kSrcBlend[st->blendSrc & 7];
      GX2BlendMode dst = st->blendType == 3 ? GX2_BLEND_MODE_ONE : kDstBlend[st->blendDst & 7];
      GX2BlendCombineMode op = st->blendType == 3 ? GX2_BLEND_COMBINE_MODE_REV_SUB : GX2_BLEND_COMBINE_MODE_ADD;
      GX2SetBlendControl(GX2_RENDER_TARGET_0, src, dst, op, FALSE, src, dst, op);
      GX2SetColorControl(GX2_LOGIC_OP_COPY, 1, FALSE, TRUE);
   } else if (st->blendType == 2) {
      GX2SetColorControl((GX2LogicOp)kRop[st->logicOp & 15], 0, FALSE, TRUE);
   } else {
      GX2SetColorControl(GX2_LOGIC_OP_COPY, 0, FALSE, TRUE);
   }
   GX2SetDepthOnlyControl(st->zEnable, st->zEnable && st->zUpdate, (GX2CompareFunction)(st->zFunc & 7));
}

static GX2Texture *render_texture(uint32_t dest, uint32_t w, uint32_t h)
{
   TexEntry *e = find_tex(dest);
   if (e && e->render && e->tex.surface.width == w && e->tex.surface.height == h) return &e->tex;
   if (e) { gpu_free_later(e->tex.surface.image); sTexBytes -= e->bytes; *e = sTex[--sNumTex]; }
   if (sNumTex >= MAX_TEX) return NULL;
   e = &sTex[sNumTex];
   memset(e, 0, sizeof(*e));
   if (!make_texture(&e->tex, w, h, GX2_TILE_MODE_DEFAULT, GX2_SURFACE_USE_TEXTURE | GX2_SURFACE_USE_COLOR_BUFFER)) return NULL;
   e->key.image = dest;
   e->render = 1;
   e->bytes = e->tex.surface.imageSize;
   sTexBytes += e->bytes;
   sNumTex++;
   sTexStats[2]++;
   return &e->tex;
}

static void clear_efb(uint32_t rgba, uint32_t z)
{
   if (sCfgNoClear) return;
   gx_render_step = "clear";
   STEP("clear colour", GX2ClearColor(&sEfb, (rgba >> 24) / 255.0f, ((rgba >> 16) & 255) / 255.0f, ((rgba >> 8) & 255) / 255.0f, (rgba & 255) / 255.0f));
   STEP("clear depth", GX2ClearDepthStencilEx(&sEfbDepth, (z & 0xFFFFFF) / 16777215.0f, 0, GX2_CLEAR_FLAGS_DEPTH));
}

// True if any active TEV stage samples a texture produced by GXCopyTex.
static int state_uses_rt(const GxState *st)
{
   int n = (int)st->misc[0][0];
   for (int s = 0; s < n && s < GX_MAX_STAGES; s++) {
      int slot = (int)(st->stage[s * 7 + 5][0] + (st->stage[s * 7 + 5][0] < 0 ? -0.5f : 0.5f));
      if (slot >= 0 && slot < GX_MAX_SLOTS && gx_is_render_texture(st->tex[slot].image)) return 1;
   }
   return 0;
}

static void copy_tex(const GxCmd *c)
{
   uint32_t l = c->a[0] >> 16, t = c->a[0] & 0xFFFF, w = c->a[1] >> 16, h = c->a[1] & 0xFFFF;
   {
      static uint32_t seen[24][3];
      static int n;
      int dup = 0;
      for (int i = 0; i < n; i++) dup |= seen[i][0] == c->a[0] && seen[i][1] == c->a[1] && seen[i][2] == c->a[2];
      if (!dup && n < 24) {
         seen[n][0] = c->a[0]; seen[n][1] = c->a[1]; seen[n][2] = c->a[2]; n++;
         WHBLogPrintf("[copy] #%d src %u,%u %ux%u -> %08X dst %ux%u fmt %02X clear %u (colour %08X)", n, (unsigned)l, (unsigned)t,
                      (unsigned)w, (unsigned)h, (unsigned)c->a[2], (unsigned)(c->a[3] >> 16), (unsigned)(c->a[3] & 0xFFFF),
                      (unsigned)c->a[7], (unsigned)c->a[4], (unsigned)c->a[5]);
      }
   }
   if (l >= EFB_W / EFB_SCALE || t >= EFB_H / EFB_SCALE) w = h = 0;
   if (l + w > EFB_W / EFB_SCALE) w = EFB_W / EFB_SCALE - l;
   if (t + h > EFB_H / EFB_SCALE) h = EFB_H / EFB_SCALE - t;
   if (sCfgNoCopy || !w || !h) { if (c->a[4]) clear_efb(c->a[5], c->a[6]); return; }
   gx_render_step = "copytex";
   GX2Texture *dst = render_texture(c->a[2], w * EFB_SCALE, h * EFB_SCALE);
   if (dst) {
      GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER, sEfb.surface.image, sEfb.surface.imageSize);
      GX2Rect r = {(int32_t)(l * EFB_SCALE), (int32_t)(t * EFB_SCALE), (int32_t)((l + w) * EFB_SCALE), (int32_t)((t + h) * EFB_SCALE)};
      GX2Point pt = {0, 0};
      STEP("copy EFB to texture", GX2CopySurfaceEx(&sEfb.surface, 0, 0, &dst->surface, 0, 0, 1, &r, &pt));
      GX2Invalidate(GX2_INVALIDATE_MODE_TEXTURE, dst->surface.image, dst->surface.imageSize);
   }
   if (c->a[4]) clear_efb(c->a[5], c->a[6]);
}

static uint32_t sDraws, sLastDraws, sLastVerts, sLastStates, sShown;
static OSTime sTimePlay, sTimeFinish, sTimeWait;

// ---------------------------------------------------------------------------------------
// Self-test squares drawn into the EFB's top-left corner every frame:
//   red   = plain shader without uniforms (vertex fetch, EFB, presentation)
//   green = TEV shader with a known state (uniform block path, TEV evaluation)
// ---------------------------------------------------------------------------------------
static const char kPlainVS[] =
   "#version 330 core\n"
   "layout(location = 0) in vec4 aPos;\n"
   "layout(location = 1) in vec4 aC0;\n"
   "out vec4 vC0;\n"
   "void main() { gl_Position = vec4(aPos.xy, 0.0, 1.0); vC0 = aC0; }\n";
static const char kPlainPS[] =
   "#version 330 core\n"
   "in vec4 vC0;\n"
   "out vec4 fragColor;\n"
   "void main() { fragColor = vC0; }\n";
typedef struct { float pos[4], c0[4], c1[4]; } PlainVertex; // self-test / probe
static PlainVertex *sTestVerts;
static GxState sTestState;

static void selftest_square(PlainVertex *v, float x0, float y0, float x1, float y1, float r, float g, float b)
{
   const float c[6][2] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y0}, {x1, y1}, {x0, y1}};
   for (int i = 0; i < 6; i++) {
      memset(&v[i], 0, sizeof(v[i]));
      v[i].pos[0] = c[i][0]; v[i].pos[1] = c[i][1]; v[i].pos[2] = 0; v[i].pos[3] = 1;
      v[i].c0[0] = r; v[i].c0[1] = g; v[i].c0[2] = b; v[i].c0[3] = 1;
      v[i].c1[3] = 1;
   }
}

static void selftest_init(void)
{
   if (sHavePlain) { // compiled with the other shaders (big-stack thread)
      WHBGfxInitShaderAttribute(&sPlain, "aPos", 0, 0, GX2_ATTRIB_FORMAT_FLOAT_32_32_32_32);
      WHBGfxInitShaderAttribute(&sPlain, "aC0", 0, 16, GX2_ATTRIB_FORMAT_FLOAT_32_32_32_32);
      WHBGfxInitFetchShader(&sPlain);
   }
   sTestVerts = MEMAllocFromDefaultHeapEx(12 * sizeof(PlainVertex), 256);
   if (!sTestVerts) return;
   selftest_square(sTestVerts, -0.95f, 0.95f, -0.80f, 0.75f, 1, 0, 0);
   selftest_square(sTestVerts + 6, -0.75f, 0.95f, -0.60f, 0.75f, 0, 1, 0);
   DCFlushRange(sTestVerts, 12 * sizeof(PlainVertex));
   // TEV state: identity projection, one stage passing the rasterised colour through.
   GxState *st = &sTestState;
   memset(st, 0, sizeof(*st));
   for (int i = 0; i < 4; i++) st->proj[i * 5] = 1;
   st->proj[10] = 0.5f; st->proj[11] = -0.5f;              // z 0 -> GX clip z -0.5 (inside [-1,0])
   st->viewport[2] = 640; st->viewport[3] = 528; st->viewport[5] = 1;
   st->scissor[2] = 640; st->scissor[3] = 528;
   float *s0 = st->stage[0];
   s0[0] = 15; s0[1] = 15; s0[2] = 15; s0[3] = 10;          // colour: d = RASC
   st->stage[1][0] = 7; st->stage[1][1] = 7; st->stage[1][2] = 7; st->stage[1][3] = 5; // alpha: d = RASA
   st->stage[3][3] = 1; st->stage[2][3] = 1;                // clamp
   st->stage[5][0] = -1; st->stage[5][1] = -1; st->stage[5][2] = 0; // no texture, COLOR0A0
   static const float swap[4][4] = {{0, 1, 2, 3}, {0, 0, 0, 3}, {1, 1, 1, 3}, {2, 2, 2, 3}};
   memcpy(st->swap, swap, sizeof(swap));
   st->misc[0][0] = 1; st->misc[0][1] = 7; st->misc[1][0] = 7; // 1 stage, alpha test ALWAYS
   st->colorUpdate = st->alphaUpdate = 1;
   WHBLogPrintf("[gfx] self-test ready (plain shader %d)", sHavePlain);
   probe_init();
}

static void selftest_draw(void)
{
   static int on = -1;
   if (on < 0) on = port_cfg("selftest");
   if (!on || !sTestVerts) return;
   bind_efb();
   GX2SetViewport(0, 0, EFB_W, EFB_H, 0, 1);
   GX2SetScissor(0, 0, EFB_W, EFB_H);
   GX2SetColorControl(GX2_LOGIC_OP_COPY, 0, FALSE, TRUE);
   GX2SetTargetChannelMasks(GX2_CHANNEL_MASK_RGBA, 0, 0, 0, 0, 0, 0, 0);
   GX2SetDepthOnlyControl(FALSE, FALSE, GX2_COMPARE_FUNC_ALWAYS);
   GX2Invalidate(GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER, sTestVerts, 12 * sizeof(PlainVertex));
   GX2SetAttribBuffer(0, 12 * sizeof(PlainVertex), sizeof(PlainVertex), sTestVerts);
   if (sHavePlain) {
      GX2SetFetchShader(&sPlain.fetchShader);
      GX2SetVertexShader(sPlain.vertexShader);
      GX2SetPixelShader(sPlain.pixelShader);
      STEP("self-test red", GX2DrawEx(GX2_PRIMITIVE_MODE_TRIANGLES, 6, 0, 1));
   }
   GX2SetFetchShader(&sTev.fetchShader);
   GX2SetVertexShader(sTev.vertexShader);
   GX2SetPixelShader(sTev.pixelShader);
   apply_state(&sTestState);
   GX2SetViewport(0, 0, EFB_W, EFB_H, 0, 1);
   STEP("self-test green", GX2DrawEx(GX2_PRIMITIVE_MODE_TRIANGLES, 6, 6, 1));
   probe_draw();
}

// ---------------------------------------------------------------------------------------
// Uniform delivery probe: a shader whose colour is a uniform, drawn six times with six
// delivery methods. Whichever squares show their colour use a method that works.
// ---------------------------------------------------------------------------------------
static const char kProbeVS[] =
   "#version 330 core\n"
   "layout(location = 0) in vec4 aPos;\n"
   "void main() { gl_Position = vec4(aPos.xy, 0.0, 1.0); }\n";
static const char kProbePS[] =
   "#version 330 core\n"
   "uniform vec4 uPad[3];\n"
   "uniform vec4 uColor;\n"
   "out vec4 fragColor;\n"
   "void main() { fragColor = uColor + uPad[1] * 0.0; }\n";
static int32_t sProbeOff = -1;
static PlainVertex *sProbeVerts;
static uint8_t *sProbeUbo;

static void probe_init(void)
{
   if (!sHaveProbe) return;
   WHBGfxInitShaderAttribute(&sProbe, "aPos", 0, 0, GX2_ATTRIB_FORMAT_FLOAT_32_32_32_32);
   WHBGfxInitFetchShader(&sProbe);
   GX2PixelShader *ps = sProbe.pixelShader;
   sProbeOff = uniform_offset(ps->uniformVars, ps->uniformVarCount, "uColor");
   WHBLogPrintf("[probe] ps mode %d, %u uniforms, %u blocks; uColor offset %d (uPad offset %d)", (int)ps->mode,
                (unsigned)ps->uniformVarCount, (unsigned)ps->uniformBlockCount, (int)sProbeOff,
                (int)uniform_offset(ps->uniformVars, ps->uniformVarCount, "uPad"));
   for (uint32_t i = 0; i < ps->uniformVarCount; i++)
      WHBLogPrintf("[probe]   var '%s' type %d count %u offset %u block %d", ps->uniformVars[i].name, (int)ps->uniformVars[i].type,
                   (unsigned)ps->uniformVars[i].count, (unsigned)ps->uniformVars[i].offset, (int)ps->uniformVars[i].block);
   sProbeVerts = MEMAllocFromDefaultHeapEx(36 * sizeof(PlainVertex), 256);
   sProbeUbo = MEMAllocFromDefaultHeapEx(6 * 256, 256);
   if (!sProbeVerts || !sProbeUbo) return;
   for (int i = 0; i < 6; i++) selftest_square(sProbeVerts + 6 * i, -0.95f + i * 0.2f, 0.70f, -0.80f + i * 0.2f, 0.50f, 1, 1, 1);
   DCFlushRange(sProbeVerts, 36 * sizeof(PlainVertex));
}

// 0 blue: buffer 15 plain | 1 yellow: buffer 15 byte-swapped | 2 magenta: register, offset/4
// 3 cyan: register, offset as is | 4 white: buffer 0 plain | 5 orange: buffer 0 swapped
static void probe_draw(void)
{
   if (!sProbeVerts || !sProbeUbo || sProbeOff < 0) return;
   static const float col[6][4] = {{0, 0, 1, 1}, {1, 1, 0, 1}, {1, 0, 1, 1}, {0, 1, 1, 1}, {1, 1, 1, 1}, {1, 0.5f, 0, 1}};
   GX2SetFetchShader(&sProbe.fetchShader);
   GX2SetVertexShader(sProbe.vertexShader);
   GX2SetPixelShader(sProbe.pixelShader);
   GX2Invalidate(GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER, sProbeVerts, 36 * sizeof(PlainVertex));
   GX2SetAttribBuffer(0, 36 * sizeof(PlainVertex), sizeof(PlainVertex), sProbeVerts);
   for (int m = 0; m < 6; m++) {
      uint32_t size = (uint32_t)sProbeOff + 16;
      uint8_t *ubo = sProbeUbo + m * 256;
      memset(ubo, 0, 256);
      const uint32_t *cw = (const uint32_t *)col[m];
      int swap = (m == 1 || m == 5);
      if (m == 2 || m == 3) {
         GX2SetShaderMode(GX2_SHADER_MODE_UNIFORM_REGISTER);
         GX2SetPixelUniformReg(m == 2 ? (uint32_t)sProbeOff / 4 : (uint32_t)sProbeOff, 4, col[m]);
      } else {
         if ((uint32_t)sProbeOff + 16 <= 256)
            for (int k = 0; k < 4; k++) ((uint32_t *)(ubo + sProbeOff))[k] = swap ? __builtin_bswap32(cw[k]) : cw[k];
         DCFlushRange(ubo, 256);
         GX2Invalidate(GX2_INVALIDATE_MODE_CPU | GX2_INVALIDATE_MODE_UNIFORM_BLOCK, ubo, 256);
         GX2SetShaderMode(GX2_SHADER_MODE_UNIFORM_BLOCK);
         GX2SetPixelUniformBlock(m >= 4 ? 0 : 15, size, ubo);
      }
      STEP("probe square", GX2DrawEx(GX2_PRIMITIVE_MODE_TRIANGLES, 6, 6 * m, 1));
   }
   GX2SetShaderMode(sBlockMode ? GX2_SHADER_MODE_UNIFORM_BLOCK : GX2_SHADER_MODE_UNIFORM_REGISTER);
}

// One-time dump of the first draws: state and where the vertices land in clip space.
static void dump_draw(const GxPacket *pk, const GxCmd *c)
{
   static int n;
   if (n >= 4) return;
   n++;
   const GxState *st = &pk->states[c->state];
   const float *m = st->proj, *v = st->viewport;
   WHBLogPrintf("[draw] #%d %u verts | vp %.0f %.0f %.0f %.0f z %.2f-%.2f | scissor %d %d %d %d | stages %.0f blend %u/%u/%u z %u/%u/%u cull %u",
                n, (unsigned)c->count, v[0], v[1], v[2], v[3], v[4], v[5], (int)st->scissor[0], (int)st->scissor[1],
                (int)st->scissor[2], (int)st->scissor[3], st->misc[0][0], st->blendType, st->blendSrc, st->blendDst, st->zEnable,
                st->zFunc, st->zUpdate, st->cull);
   WHBLogPrintf("[draw]   proj %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f", m[0], m[1],
                m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
   WHBLogPrintf("[draw]   stage0 c %.0f %.0f %.0f %.0f a %.0f %.0f %.0f %.0f tex %.0f coord %.0f chan %.0f | acmp %.0f %.2f %.0f %.0f %.2f | tex0 %08X %ux%u fmt %u",
                st->stage[0][0], st->stage[0][1], st->stage[0][2], st->stage[0][3], st->stage[1][0], st->stage[1][1],
                st->stage[1][2], st->stage[1][3], st->stage[5][0], st->stage[5][1], st->stage[5][2], st->misc[0][1],
                st->misc[0][2], st->misc[0][3], st->misc[1][0], st->misc[1][1], (unsigned)st->tex[0].image,
                st->tex[0].width, st->tex[0].height, st->tex[0].fmt);
   for (uint32_t i = 0; i < c->count && i < 3; i++) {
      const GxVertex *x = &pk->verts[c->first + i];
      float cx = m[0] * x->pos[0] + m[1] * x->pos[1] + m[2] * x->pos[2] + m[3];
      float cy = m[4] * x->pos[0] + m[5] * x->pos[1] + m[6] * x->pos[2] + m[7];
      float cz = m[8] * x->pos[0] + m[9] * x->pos[1] + m[10] * x->pos[2] + m[11];
      float cw = m[12] * x->pos[0] + m[13] * x->pos[1] + m[14] * x->pos[2] + m[15];
      WHBLogPrintf("[draw]   v%u pos %.1f %.1f %.1f -> clip %.2f %.2f %.2f w %.2f | c0 %.2f %.2f %.2f %.2f | t0 %.2f %.2f %.2f",
                   (unsigned)i, x->pos[0], x->pos[1], x->pos[2], cx, cy, cz, cw, x->c0[0] / 255.0f, x->c0[1] / 255.0f, x->c0[2] / 255.0f, x->c0[3] / 255.0f,
                   x->tc[0][0], x->tc[0][1], x->tc[0][2]);
   }
}

static void video_draw(const GxPacket *pk, const GxCmd *c)
{
   static int n;
   if (!sVideoTex || n >= 3) return;
   const GxState *st = &pk->states[c->state];
   int uses = 0;
   for (int i = 0; i < GX_MAX_SLOTS; i++) uses |= st->tex[i].image == sVideoTex;
   if (!uses) return;
   n++;
   int ns = (int)st->misc[0][0];
   WHBLogPrintf("[video] draw %d: %u verts, %d stages", n, (unsigned)c->count, ns);
   for (int s = 0; s < ns && s < 4; s++) {
      const float(*q)[4] = &st->stage[s * 7];
      int slot = rnd(q[5][0]);
      const GxTexKey *k = slot >= 0 && slot < GX_MAX_SLOTS ? &st->tex[slot] : NULL;
      WHBLogPrintf("[video]   st%d c %.0f %.0f %.0f %.0f a %.0f %.0f %.0f %.0f tex %d coord %.0f scale %.3f %.3f | %08X %ux%u f%u",
                   s, q[0][0], q[0][1], q[0][2], q[0][3], q[1][0], q[1][1], q[1][2], q[1][3], slot, q[5][1], q[6][1], q[6][2],
                   k ? (unsigned)k->image : 0, k ? k->width : 0, k ? k->height : 0, k ? k->fmt : 0);
   }
   for (uint32_t i = 0; i < c->count && i < 4; i++) {
      const GxVertex *v = &pk->verts[c->first + i];
      WHBLogPrintf("[video]   v%u pos %.1f %.1f | t0 %.3f %.3f %.3f t1 %.3f %.3f t2 %.3f %.3f t3 %.3f %.3f %.3f", (unsigned)i, v->pos[0],
                   v->pos[1], v->tc[0][0], v->tc[0][1], v->tc[0][2], v->tc[1][0], v->tc[1][1], v->tc[2][0], v->tc[2][1], v->tc[3][0],
                   v->tc[3][1], v->tc[3][2]);
   }
}

static void play_game(GxPacket *pk);
static void play(GxPacket *pk)
{
   if (pk->numCmds) play_game(pk);
   selftest_draw();
}

static void play_game(GxPacket *pk)
{
   gx_render_step = "play";
   // One frame may still be on the GPU: each frame uses its own half of the uniform ring
   // (the frame before the previous one has finished, see the end of the render loop).
   sUboBase = (sFrame & 1) ? UBO_RING / 2 : 0;
   sUboEnd = sUboBase + UBO_RING / 2;
   sUboPos = sUboBase;
   DCFlushRange(pk->verts, pk->numVerts * sizeof(GxVertex));
   GX2Invalidate(GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER, pk->verts, pk->numVerts * sizeof(GxVertex));
   STEP("bind EFB", bind_efb());
   GX2SetAttribBuffer(0, pk->numVerts * sizeof(GxVertex), sizeof(GxVertex), pk->verts);
   uint32_t cur = 0xFFFFFFFF, draws = 0;
   for (uint32_t i = 0; i < pk->numCmds; i++) {
      const GxCmd *c = &pk->cmds[i];
      switch (c->type) {
      case GXCMD_DRAW:
         if (!c->count) break;
         if (sCfgNoAfterFx && state_uses_rt(&pk->states[c->state])) break;
         if (c->state != cur) {
            STEP("apply state", apply_state(&pk->states[c->state]));
            cur = c->state;
            if (sSurfaceCopied) { // the context was re-applied: vertex buffer too
               GX2SetAttribBuffer(0, pk->numVerts * sizeof(GxVertex), sizeof(GxVertex), pk->verts);
               sSurfaceCopied = 0;
            }
         }
         dump_draw(pk, c);
         video_draw(pk, c);
         if (c->first + c->count > pk->numVerts) break;
         gx_render_step = "draw";
         if (!sCfgNoDraw) STEP("draw", GX2DrawEx(c->a[0] == 19 ? GX2_PRIMITIVE_MODE_QUADS : GX2_PRIMITIVE_MODE_TRIANGLES, c->count, c->first, 1));
         draws++;
         break;
      case GXCMD_COPYTEX:
         copy_tex(c);
         bind_efb();
         GX2SetAttribBuffer(0, pk->numVerts * sizeof(GxVertex), sizeof(GxVertex), pk->verts);
         cur = 0xFFFFFFFF;
         break;
      }
   }
   sDraws += draws;
   sLastDraws = draws;
   sLastVerts = pk->numVerts;
   sLastStates = pk->numStates;
}

// Scales the EFB rectangle copied by GXCopyDisp onto the TV colour buffer.
static int sPadGame = -1; // GamePad shows the game (1) or the log (0)

static void present_tv(const GxPacket *pk)
{
   float u0 = (float)(pk->dispSrc[0] * EFB_SCALE) / EFB_W, v0 = (float)(pk->dispSrc[1] * EFB_SCALE) / EFB_H;
   float u1 = (float)((pk->dispSrc[0] + pk->dispSrc[2]) * EFB_SCALE) / EFB_W;
   float v1 = (float)((pk->dispSrc[1] + pk->dispSrc[3]) * EFB_SCALE) / EFB_H;
   const float q[6][4] = {{-1, 1, u0, v0}, {1, 1, u1, v0}, {1, -1, u1, v1}, {-1, 1, u0, v0}, {1, -1, u1, v1}, {-1, -1, u0, v1}};
   memcpy(sBlitVerts, q, sizeof(q));
   DCFlushRange(sBlitVerts, sizeof(q));
   GX2Invalidate(GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER, sBlitVerts, sizeof(q));
   GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER | GX2_INVALIDATE_MODE_TEXTURE, sEfb.surface.image, sEfb.surface.imageSize);

   GX2SetShaderMode(sBlockMode ? GX2_SHADER_MODE_UNIFORM_BLOCK : GX2_SHADER_MODE_UNIFORM_REGISTER);
   GX2SetFetchShader(&sBlit.fetchShader);
   GX2SetVertexShader(sBlit.vertexShader);
   GX2SetPixelShader(sBlit.pixelShader);
   GX2Sampler smp;
   GX2InitSampler(&smp, GX2_TEX_CLAMP_MODE_CLAMP, GX2_TEX_XY_FILTER_MODE_LINEAR);
   GX2SetPixelTexture(&sEfbTex, uBlitSrc);
   GX2SetPixelSampler(&smp, uBlitSrc);
   GX2SetColorControl(GX2_LOGIC_OP_COPY, 0, FALSE, TRUE);
   GX2SetTargetChannelMasks(GX2_CHANNEL_MASK_RGBA, 0, 0, 0, 0, 0, 0, 0);
   GX2SetDepthOnlyControl(FALSE, FALSE, GX2_COMPARE_FUNC_ALWAYS);
   GX2SetCullOnlyControl(GX2_FRONT_FACE_CCW, FALSE, FALSE);
   GX2SetAttribBuffer(0, sizeof(q), 16, sBlitVerts);
   GX2DrawEx(GX2_PRIMITIVE_MODE_TRIANGLES, 6, 0, 1);
}

// ---------------------------------------------------------------------------------------
// Shader compile thread: CafeGLSL (Mesa) recurses deeply, so it gets a large stack of its
// own instead of the render thread's.
// ---------------------------------------------------------------------------------------
static OSThread sShaderThread __attribute__((aligned(16)));
static volatile int sGx2Up, sCompiled;

static int shader_main(int argc, const char **argv)
{
   sCompiled = compile_shaders();
   return 0;
}

// Called by main() before the game starts: loads CafeGLSL and compiles the shaders on the
// main core while nothing else runs (dynamic loading from a busy background thread hung).
void port_shaders_prepare(void)
{
   for (int i = 0; i < 500 && !sGx2Up; i++) OSSleepTicks(OSMillisecondsToTicks(10));
   if (!glsl_init()) { sShaderDone = 1; return; } // load on the main thread itself, like CafeGLSL's samples
   uint8_t *stack = MEMAllocFromDefaultHeapEx(SHADER_STACK, 16);
   if (stack && OSCreateThread(&sShaderThread, (OSThreadEntryPointFn)shader_main, 0, NULL, stack + SHADER_STACK,
                               SHADER_STACK, 16, OS_THREAD_ATTRIB_AFFINITY_CPU1)) {
      OSSetThreadName(&sShaderThread, "Rayman shaders");
      OSResumeThread(&sShaderThread);
      int ret;
      OSJoinThread(&sShaderThread, &ret);
   } else {
      WHBLogPrintf("[gfx] could not start the shader thread");
   }
   if (!sCompiled) WHBLogPrintf("[gfx] no shaders: the TV shows only the clear colour");
   sShaderDone = 1;
}

// ---------------------------------------------------------------------------------------
// Render thread
// ---------------------------------------------------------------------------------------
static OSThread sRender __attribute__((aligned(16)));
static uint8_t sRenderStack[0x20000] __attribute__((aligned(16)));

static int render_main(int argc, const char **argv)
{
   sEfbScale = port_cfg("efb1x") ? 1 : 2;
   WHBGfxInit();
   GX2ColorBuffer *tv = WHBGfxGetTVColourBuffer(), *drc = WHBGfxGetDRCColourBuffer();
   GX2Surface strip, pad;
   int big = tv->surface.width >= 1920 ? 2 : 1;
   if (!port_make_surface(&strip, tv->surface.width, 20 * big) ||
       !port_make_surface(&pad, drc->surface.width, drc->surface.height) || !init_targets() ||
       !make_texture(&sWhite, 8, 8, GX2_TILE_MODE_LINEAR_ALIGNED, GX2_SURFACE_USE_TEXTURE)) {
      WHBLogPrintf("[gfx] out of memory for screen surfaces");
      return 0;
   }
   for (uint32_t i = 0; i < sWhite.surface.imageSize / 4; i++) ((uint32_t *)sWhite.surface.image)[i] = 0xFFFFFFFF;
   DCFlushRange(sWhite.surface.image, sWhite.surface.imageSize);
   GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, sWhite.surface.image, sWhite.surface.imageSize);
   WHBLogPrintf("[gfx] GX2 up: TV %ux%u, GamePad %ux%u, EFB %ux%u", (unsigned)tv->surface.width,
                (unsigned)tv->surface.height, (unsigned)drc->surface.width, (unsigned)drc->surface.height, EFB_W, EFB_H);
   sCfgNoEfb = port_cfg("noefb");
   sCfgNoClear = port_cfg("noclear");
   sCfgNoCopy = port_cfg("nocopy");
   sCfgNoDraw = port_cfg("nodraw");
   sCfgNoAfterFx = port_cfg("noafterfx");
   WHBLogPrintf("[gfx] switches: noefb %d noclear %d nocopy %d nodraw %d noafterfx %d", sCfgNoEfb, sCfgNoClear, sCfgNoCopy,
                sCfgNoDraw, sCfgNoAfterFx);
   sGx2Up = 1;

   const int padRows = (int)(drc->surface.height / 16);
   static char rows[40][PORT_LOG_COLS];
   char l[200];
   uint32_t lastReport = 0;
   OSTime start = OSGetSystemTime();
   for (;;) {
      extern volatile int port_paused, port_render_parked;
      // Parked while the HOME Menu is open, and until WHBGfx has re-created the screen
      // buffers afterwards (they do not exist in between).
      if (port_paused || !ProcUIInForeground() || !tv->surface.image || !drc->surface.image) {
         if (!port_render_parked) {
            if (sHaveShaders) GX2DrawDone();
            port_render_parked = 1;
         }
         OSSleepTicks(OSMillisecondsToTicks(10));
         continue;
      }
      if (port_render_parked) {
         OSSleepTicks(OSMillisecondsToTicks(30)); // let the acquire callbacks finish
         if (port_paused || !tv->surface.image || !drc->surface.image) continue;
         GX2SetTVEnable(TRUE);
         GX2SetDRCEnable(TRUE);
         if (sEfbMem1 > 0) efb_alloc(); // WHBGfx rebuilt its MEM1 heap
      }
      port_render_parked = 0;
      if (sShaderDone && sCompiled && !sHaveShaders && !sCfgNoEfb) { sHaveShaders = finish_shaders(); if (sHaveShaders) { selftest_init(); spec_start(); } }
      static GxPacket idle = {.black = 1};
      OSTime tw0 = OSGetSystemTime();
      GxPacket *pk = gx_acquire_packet();
      static OSTime lastShown;
      if (!pk) {
         // No frame from the game: keep the screens (log) alive every 100 ms anyway.
         if (sShaderDone && OSGetSystemTime() - lastShown < OSMillisecondsToTicks(100)) {
            OSSleepTicks(OSMillisecondsToTicks(2));
            continue;
         }
         OSSleepTicks(OSMillisecondsToTicks(30));
         pk = &idle;
      }
      lastShown = OSGetSystemTime();
      sTimeWait += lastShown - tw0;
      sFrame++;

      OSTime tp0 = OSGetSystemTime();
      if (sHaveShaders) play(pk); // the EFB clear of GXCopyDisp happens after the frame is shown (below)
      sTimePlay += OSGetSystemTime() - tp0;

      // GamePad log: redraw only when new lines arrived, at most 5 times a second (CPU-drawn).
      static OSTime lastPad;
      static uint32_t lastLines = 0xFFFFFFFF;
      uint32_t lines = port_log_count();
      if (sPadGame < 0) sPadGame = !port_cfg("padlog");
      int padLog = !sPadGame || !sHaveShaders || pk->black; // the log until the game shows a picture
      if (padLog && lines != lastLines && OSGetSystemTime() - lastPad > OSMillisecondsToTicks(200)) {
         lastPad = OSGetSystemTime();
         lastLines = lines;
         port_fill(&pad, 0x000000FF);
         int n = port_log_snapshot(rows, padRows < 40 ? padRows : 40);
         for (int i = 0; i < n; i++) port_text(&pad, 0, i * 16, 1, rows[i], 0xE0E0E0FF);
         port_upload(&pad);
      }

      uint32_t secs = (uint32_t)OSTicksToSeconds(OSGetSystemTime() - start);
      snprintf(l, sizeof(l), " %s  %02u:%02u  frame %u  draws %u  verts %u  tex %u (%u MB)%s%s", port_version(),
               (unsigned)(secs / 60), (unsigned)(secs % 60), (unsigned)pk->frame, (unsigned)sLastDraws,
               (unsigned)sLastVerts, (unsigned)sNumTex, (unsigned)(sTexBytes >> 20), pk->black ? "  [VI black]" : "",
               sHaveShaders ? "" : sShaderDone ? "  [no shaders - see log]" : "  [compiling shaders]");
      static uint32_t lastStrip = 0xFFFFFFFF;
      if (secs != lastStrip) { // once a second
         lastStrip = secs;
         port_fill(&strip, 0x000000FF);
         port_text(&strip, 4, 2 * big, big, l, 0xFFFFFFFF);
         port_upload(&strip);
      }

      gx_render_step = "present";
      // WHBGfxBeginRender waits until every swap has flipped, which runs the CPU and GPU one
      // after the other (30 fps once both together take more than one TV frame). One flip may
      // stay queued instead (the wait at the end of the frame), so they work side by side.
      static int whbWait = -1;
      if (whbWait < 0) whbWait = port_cfg("whbwait");
      if (whbWait) WHBGfxBeginRender();
      WHBGfxBeginRenderTV();
      uint32_t cc = pk->clearColor;
      if (pk->black) WHBGfxClearColor(0, 0, 0, 1);
      else if (sHaveShaders) STEP("present EFB to TV", present_tv(pk));
      else WHBGfxClearColor((cc >> 24) / 255.0f, ((cc >> 16) & 255) / 255.0f, ((cc >> 8) & 255) / 255.0f, 1.0f);
      port_blit(&strip, &tv->surface, 0, (int)tv->surface.height - (int)strip.height);
      WHBGfxFinishRenderTV();
      // GamePad: the game (default) or the log; L stick + R stick clicked together switches.
      static uint32_t padPrev;
      extern volatile uint32_t port_pad_raw_hold;
      uint32_t ph = port_pad_raw_hold, both = 0x00060000; // VPAD_BUTTON_STICK_L | VPAD_BUTTON_STICK_R
      if ((ph & both) == both && (padPrev & both) != both) { sPadGame = !sPadGame; lastLines = 0xFFFFFFFF; }
      padPrev = ph;
      WHBGfxBeginRenderDRC();
      WHBGfxClearColor(0, 0, 0, 1);
      if (!padLog) present_tv(pk);
      else port_blit(&pad, &drc->surface, 0, 0);
      WHBGfxFinishRenderDRC();
      gx_render_step = "finish";
      OSTime tf0 = OSGetSystemTime();
      // Like WHBGfxFinishRender, but without its GX2DrawDone: the GPU works on this frame
      // while the CPU prepares the next one. Wait only for the previous frame here.
      static OSTime prevTs;
      static GxPacket *prevPk;
      static int tvOn;
      GX2SwapScanBuffers();
      GX2Flush();
      if (!tvOn) { GX2SetTVEnable(TRUE); GX2SetDRCEnable(TRUE); tvOn = 1; }
      OSTime ts = GX2GetLastSubmittedTimeStamp();
      static int noPipe = -1;
      if (noPipe < 0) noPipe = port_cfg("nopipeline");
      if (noPipe) GX2DrawDone();
      else if (prevTs) GX2WaitTimeStamp(prevTs);
      prevTs = ts;
      gpu_free_frame(); // memory the previous frame used
      if (prevPk) gx_release_packet(prevPk);
      prevPk = pk != &idle ? pk : NULL;
      // at most two flips queued (keeps pacing at the TV rate without serialising CPU and GPU)
      // Polled with a time limit: no flips happen while the HOME Menu takes the screens, and
      // a blocking GX2WaitForFlip would keep the renderer from parking.
      static int flipWait = -1;
      if (flipWait < 0) flipWait = port_cfg("flipwait"); // the blocking wait of first-boot 64
      if (flipWait) {
         for (int guard = 0; guard < 4; guard++) {
            uint32_t swaps, flips;
            OSTime lf, lv;
            GX2GetSwapStatus(&swaps, &flips, &lf, &lv);
            if (swaps - flips <= 1) break;
            GX2WaitForFlip();
         }
      } else {
         OSTime w0 = OSGetSystemTime();
         for (;;) {
            uint32_t swaps, flips;
            OSTime lf, lv;
            GX2GetSwapStatus(&swaps, &flips, &lf, &lv);
            if (swaps - flips <= 1 || port_paused) break;
            if (OSGetSystemTime() - w0 > OSMillisecondsToTicks(100)) break;
            OSSleepTicks(OSMicrosecondsToTicks(250));
         }
      }
      sTimeFinish += OSGetSystemTime() - tf0;
      sShown++;
      gx_render_step = "idle";

      if (sHaveShaders && pk->clearAfter) {
         GX2SetContextState(sCtx);
         clear_efb(pk->clearColor, pk->clearZ);
      }

      if (secs >= lastReport + 5) {
         uint32_t span = secs - lastReport;
         lastReport = secs;
         if (sShown) WHBLogPrintf("[perf] %.1f fps shown | per frame: play %.1f ms (CPU), finish %.1f ms (GPU+vsync), waiting for the game %.1f ms | spec shaders %u ready, %u failed, %u draws",
                                  sShown / (float)(span ? span : 1), OSTicksToMicroseconds(sTimePlay) / 1000.0f / sShown,
                                  OSTicksToMicroseconds(sTimeFinish) / 1000.0f / sShown,
                                  OSTicksToMicroseconds(sTimeWait) / 1000.0f / sShown, (unsigned)sSpecStats[0],
                                  (unsigned)sSpecStats[1], (unsigned)sSpecStats[2]);
         sShown = 0; sTimePlay = sTimeFinish = sTimeWait = 0; sSpecStats[2] = 0;
         WHBLogPrintf("[gfx] frame %u: %u draws, %u verts, %u states, %u dropped | tex %u cached (%u MB), %u decoded, %u evicted, %u rt | gx prims %u dl %u copies %u unk %u pkt drop %u",
                      (unsigned)pk->frame, (unsigned)sLastDraws, (unsigned)sLastVerts, (unsigned)sLastStates,
                      (unsigned)pk->dropped, (unsigned)sNumTex, (unsigned)(sTexBytes >> 20), (unsigned)sTexStats[0],
                      (unsigned)sTexStats[1], (unsigned)sTexStats[2], (unsigned)gx_stat(0), (unsigned)gx_stat(6),
                      (unsigned)gx_stat(7), (unsigned)gx_stat(3), (unsigned)gx_stat(5));
      }
   }
   return 0;
}

void port_render_start(void)
{
   gx_core_init();
   OSCreateThread(&sRender, (OSThreadEntryPointFn)render_main, 0, NULL, sRenderStack + sizeof(sRenderStack),
                  sizeof(sRenderStack), 16, OS_THREAD_ATTRIB_AFFINITY_CPU2);
   OSSetThreadName(&sRender, "Rayman render");
   OSResumeThread(&sRender);
}
