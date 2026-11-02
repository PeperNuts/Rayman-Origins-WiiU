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
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <gx2/clear.h>
#include <gx2/context.h>
#include <gx2/draw.h>
#include <gx2/event.h>
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

#include "gx.h"
#include "port.h"

#define EFB_SCALE 2
#define EFB_W (640 * EFB_SCALE)
#define EFB_H (528 * EFB_SCALE)

// Switches read from sd:/rayman/port.cfg (to bisect GPU problems without new builds)
static int sCfgNoEfb, sCfgNoClear, sCfgNoCopy, sCfgNoDraw;
volatile const char *gx_render_step = "init"; // breadcrumb for the watchdog

// ---------------------------------------------------------------------------------------
// Shaders
// ---------------------------------------------------------------------------------------
static const char kTevVS[] =
   "#version 330 core\n"
   "layout(location = 0) in vec4 aPos;\n"
   "layout(location = 1) in vec4 aC0;\n"
   "layout(location = 2) in vec4 aC1;\n"
   "layout(location = 3) in vec4 aT0;\n"
   "layout(location = 4) in vec4 aT1;\n"
   "layout(location = 5) in vec4 aT2;\n"
   "layout(location = 6) in vec4 aT3;\n"
   "uniform vec4 uProj[4];\n"
   "out vec4 vC0; out vec4 vC1; out vec4 vT0; out vec4 vT1; out vec4 vT2; out vec4 vT3;\n"
   "void main() {\n"
   "  vec4 p = vec4(aPos.xyz, 1.0);\n"
   "  vec4 c = vec4(dot(uProj[0], p), dot(uProj[1], p), dot(uProj[2], p), dot(uProj[3], p));\n"
   "  c.z = 2.0 * c.z + c.w;\n" // GX clip z is [-w, 0]
   "  gl_Position = c;\n"
   "  vC0 = aC0; vC1 = aC1; vT0 = aT0; vT1 = aT1; vT2 = aT2; vT3 = aT3;\n"
   "}\n";

static const char kTevPS[] =
   "#version 330 core\n"
   "in vec4 vC0; in vec4 vC1; in vec4 vT0; in vec4 vT1; in vec4 vT2; in vec4 vT3;\n"
   "out vec4 fragColor;\n"
   "uniform sampler2D uTex0; uniform sampler2D uTex1; uniform sampler2D uTex2; uniform sampler2D uTex3;\n"
   "uniform sampler2D uTex4; uniform sampler2D uTex5; uniform sampler2D uTex6; uniform sampler2D uTex7;\n"
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
   "      vec2 uv = tc.xy / (abs(tc.z) < 1e-6 ? 1.0 : tc.z);\n"
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
   "#version 330 core\n"
   "layout(location = 0) in vec4 aPos;\n"
   "out vec2 vUV;\n"
   "void main() { gl_Position = vec4(aPos.xy, 0.0, 1.0); vUV = aPos.zw; }\n";

static const char kBlitPS[] =
   "#version 330 core\n"
   "in vec2 vUV;\n"
   "out vec4 fragColor;\n"
   "uniform sampler2D uSrc;\n"
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
static uint32_t sUboPos;
static int32_t uProj, uStage, uRegs, uKonst, uSwap, uMisc, uSampler[GX_MAX_SLOTS], uBlitSrc;
static float *sBlitVerts;

static void *alloc_gpu(uint32_t size, uint32_t align)
{
   void *p = MEMAllocFromDefaultHeapEx(size, align < 256 ? 256 : align);
   if (p) memset(p, 0, size);
   return p;
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
   s->image = alloc_gpu(s->imageSize, s->alignment);

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
   d->image = alloc_gpu(d->imageSize, d->alignment);

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

static int compile_shaders(void)
{
   return compile(&sTev, kTevVS, kTevPS, "TEV") && compile(&sBlit, kBlitVS, kBlitPS, "blit");
}

// Fetch shaders and uniform locations: GX2 work, done on the render thread.
static int finish_shaders(void)
{
   static const char *attr[7] = {"aPos", "aC0", "aC1", "aT0", "aT1", "aT2", "aT3"};
   for (int i = 0; i < 7; i++) WHBGfxInitShaderAttribute(&sTev, attr[i], 0, i * 16, GX2_ATTRIB_FORMAT_FLOAT_32_32_32_32);
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
      // Uniform-block mode. CafeGLSL v0.2 reports mode 1 without declaring a block: loose
      // uniforms live in an implicit block at location 0, at byte offsets.
      if (vs->uniformBlockCount) { sVsBlockLoc = vs->uniformBlocks[0].offset; sVsBlockSize = vs->uniformBlocks[0].size; }
      else { sVsBlockLoc = 0; sVsBlockSize = (uint32_t)(uProj >= 0 ? uProj : 0) + 64; }
      if (ps->uniformBlockCount) { sPsBlockLoc = ps->uniformBlocks[0].offset; sPsBlockSize = ps->uniformBlocks[0].size; }
      else {
         int32_t end = 0, offs[5] = {uStage, uRegs, uKonst, uSwap, uMisc};
         int32_t sizes[5] = {GX_MAX_STAGES * 7 * 16, 64, 64, 64, 32};
         for (int i = 0; i < 5; i++)
            if (offs[i] >= 0 && offs[i] + sizes[i] > end) end = offs[i] + sizes[i];
         sPsBlockLoc = 0;
         sPsBlockSize = (uint32_t)end;
      }
      sUboSwap = port_cfg("uboswap"); // GPU reads blocks 8-in-32 swapped, like vertex data
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
   GX2Texture tex;
} TexEntry;

#define MAX_TEX 1536
#define TEX_BUDGET (192u << 20)
static TexEntry sTex[MAX_TEX];
static uint32_t sNumTex, sTexBytes, sFrame;
static GX2Texture sWhite;
static uint32_t sTexStats[3]; // decoded, evicted, render targets

static uint32_t content_hash(const GxTexKey *k)
{
   uint32_t size = gx_tex_buffer_size(k->width, k->height, k->fmt);
   const uint32_t *p = (const uint32_t *)k->image;
   uint32_t n = size / 4, h = 2166136261u;
   if (!n) return 0;
   for (uint32_t i = 0; i < 16; i++) h = (h ^ p[(uint64_t)i * (n - 1) / 15]) * 16777619u;
   return h;
}

static int make_texture(GX2Texture *t, uint32_t w, uint32_t h, GX2TileMode tile, uint32_t use)
{
   memset(t, 0, sizeof(*t));
   GX2Surface *s = &t->surface;
   s->dim = GX2_SURFACE_DIM_TEXTURE_2D;
   s->width = w; s->height = h; s->depth = 1; s->mipLevels = 1;
   s->format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
   s->aa = GX2_AA_MODE1X;
   s->use = (GX2SurfaceUse)use;
   s->tileMode = tile;
   t->viewNumMips = 1;
   t->viewNumSlices = 1;
   t->compMap = 0x00010203;
   GX2CalcSurfaceSizeAndAlignment(s);
   GX2InitTextureRegs(t);
   s->image = MEMAllocFromDefaultHeapEx(s->imageSize, s->alignment);
   return s->image != NULL;
}

static void evict_one(void)
{
   uint32_t best = 0xFFFFFFFF, bi = 0;
   for (uint32_t i = 0; i < sNumTex; i++)
      if (!sTex[i].render && sTex[i].lastUsed < best && sTex[i].lastUsed + 2 < sFrame) { best = sTex[i].lastUsed; bi = i; }
   if (best == 0xFFFFFFFF) return;
   MEMFreeToDefaultHeap(sTex[bi].tex.surface.image);
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

static GX2Texture *get_texture(const GxTexKey *k)
{
   if (!k->image || !k->width || !k->height) return &sWhite;
   for (uint32_t i = 0; i < sNumTex; i++) {
      TexEntry *e = &sTex[i];
      if (e->key.image != k->image) continue;
      if (e->render) { e->lastUsed = sFrame; return &e->tex; }
      if (e->key.width == k->width && e->key.height == k->height && e->key.fmt == k->fmt) {
         if (e->hash == content_hash(k)) { e->lastUsed = sFrame; return &e->tex; }
         MEMFreeToDefaultHeap(e->tex.surface.image); // contents changed: decode again
         sTexBytes -= e->bytes;
         *e = sTex[--sNumTex];
         break;
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
   if (!make_texture(&e->tex, k->width, k->height, GX2_TILE_MODE_LINEAR_ALIGNED, GX2_SURFACE_USE_TEXTURE)) return &sWhite;
   gx_decode_texture(k, (uint32_t *)e->tex.surface.image, e->tex.surface.pitch);
   DCFlushRange(e->tex.surface.image, e->tex.surface.imageSize);
   GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, e->tex.surface.image, e->tex.surface.imageSize);
   e->key = *k;
   e->hash = content_hash(k);
   e->lastUsed = sFrame;
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

// Uniform-block memory: a ring the GPU reads at draw time (reset per packet, after the
// previous frame's GX2DrawDone). GX2 reads uniform blocks byte-swapped (little-endian words).
static uint8_t *ubo_alloc(uint32_t size)
{
   if (!sUbo || !size) return NULL;
   size = (size + 255) & ~255u;
   if (sUboPos + size > UBO_RING) { GX2DrawDone(); sUboPos = 0; }
   uint8_t *p = sUbo + sUboPos;
   sUboPos += size;
   memset(p, 0, size);
   return p;
}

static void ubo_put(uint8_t *block, uint32_t size, int32_t off, const void *src, uint32_t words)
{
   if (off < 0 || (uint32_t)off + words * 4 > size) return;
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
      if (pb && sPsBlockLoc >= 0) {
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

   // textures used by the active stages
   int n = (int)st->misc[0][0];
   uint32_t used = 0;
   for (int s = 0; s < n && s < GX_MAX_STAGES; s++) {
      int slot = (int)(st->stage[s * 7 + 5][0] + (st->stage[s * 7 + 5][0] < 0 ? -0.5f : 0.5f));
      if (slot >= 0 && slot < GX_MAX_SLOTS) used |= 1u << slot;
   }
   for (int i = 0; i < GX_MAX_SLOTS; i++) {
      if (uSampler[i] < 0) continue;
      const GxTexKey *k = &st->tex[i];
      GX2Texture *t = (used & (1u << i)) ? get_texture(k) : &sWhite;
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
   if (e) { MEMFreeToDefaultHeap(e->tex.surface.image); sTexBytes -= e->bytes; *e = sTex[--sNumTex]; }
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

static void copy_tex(const GxCmd *c)
{
   uint32_t l = c->a[0] >> 16, t = c->a[0] & 0xFFFF, w = c->a[1] >> 16, h = c->a[1] & 0xFFFF;
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

static uint32_t sDraws, sLastDraws, sLastVerts, sLastStates;

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
                   (unsigned)i, x->pos[0], x->pos[1], x->pos[2], cx, cy, cz, cw, x->c0[0], x->c0[1], x->c0[2], x->c0[3],
                   x->tc[0][0], x->tc[0][1], x->tc[0][2]);
   }
}

static void play(GxPacket *pk)
{
   if (!pk->numCmds) return;
   gx_render_step = "play";
   sUboPos = 0; // the previous frame finished (GX2DrawDone in WHBGfxFinishRender)
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
         if (c->state != cur) { STEP("apply state", apply_state(&pk->states[c->state])); cur = c->state; }
         dump_draw(pk, c);
         if (c->first + c->count > pk->numVerts) break;
         gx_render_step = "draw";
         if (!sCfgNoDraw) STEP("draw", GX2DrawEx(GX2_PRIMITIVE_MODE_TRIANGLES, c->count, c->first, 1));
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
#define SHADER_STACK (8u << 20)
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
   WHBLogPrintf("[gfx] switches: noefb %d noclear %d nocopy %d nodraw %d", sCfgNoEfb, sCfgNoClear, sCfgNoCopy, sCfgNoDraw);
   sGx2Up = 1;

   const int padRows = (int)(drc->surface.height / 16);
   static char rows[40][PORT_LOG_COLS];
   char l[200];
   uint32_t lastReport = 0;
   OSTime start = OSGetSystemTime();
   for (;;) {
      if (!ProcUIInForeground()) {
         OSSleepTicks(OSMillisecondsToTicks(50));
         continue;
      }
      if (sShaderDone && sCompiled && !sHaveShaders && !sCfgNoEfb) sHaveShaders = finish_shaders();
      static GxPacket idle = {.black = 1};
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
      sFrame++;

      if (sHaveShaders) play(pk); // the EFB clear of GXCopyDisp happens after the frame is shown (below)

      port_fill(&pad, 0x000000FF);
      int n = port_log_snapshot(rows, padRows < 40 ? padRows : 40);
      for (int i = 0; i < n; i++) port_text(&pad, 0, i * 16, 1, rows[i], 0xE0E0E0FF);
      port_upload(&pad);

      uint32_t secs = (uint32_t)OSTicksToSeconds(OSGetSystemTime() - start);
      snprintf(l, sizeof(l), " %s  %02u:%02u  frame %u  draws %u  verts %u  tex %u (%u MB)%s%s", port_version(),
               (unsigned)(secs / 60), (unsigned)(secs % 60), (unsigned)pk->frame, (unsigned)sLastDraws,
               (unsigned)sLastVerts, (unsigned)sNumTex, (unsigned)(sTexBytes >> 20), pk->black ? "  [VI black]" : "",
               sHaveShaders ? "" : sShaderDone ? "  [no shaders - see log]" : "  [compiling shaders]");
      port_fill(&strip, 0x000000FF);
      port_text(&strip, 4, 2 * big, big, l, 0xFFFFFFFF);
      port_upload(&strip);

      gx_render_step = "present";
      WHBGfxBeginRender();
      WHBGfxBeginRenderTV();
      uint32_t cc = pk->clearColor;
      if (pk->black) WHBGfxClearColor(0, 0, 0, 1);
      else if (sHaveShaders) STEP("present EFB to TV", present_tv(pk));
      else WHBGfxClearColor((cc >> 24) / 255.0f, ((cc >> 16) & 255) / 255.0f, ((cc >> 8) & 255) / 255.0f, 1.0f);
      port_blit(&strip, &tv->surface, 0, (int)tv->surface.height - (int)strip.height);
      WHBGfxFinishRenderTV();
      WHBGfxBeginRenderDRC();
      WHBGfxClearColor(0, 0, 0, 1);
      port_blit(&pad, &drc->surface, 0, 0);
      WHBGfxFinishRenderDRC();
      gx_render_step = "finish";
      if (sHaveShaders) STEP("finish frame", WHBGfxFinishRender()); // swaps, waits for the GPU and the flip
      else WHBGfxFinishRender();
      gx_render_step = "idle";

      if (sHaveShaders && pk->clearAfter) {
         GX2SetContextState(sCtx);
         clear_efb(pk->clearColor, pk->clearZ);
         gx_render_step = "clear-wait";
         GX2DrawDone();
         gx_render_step = "idle";
      }
      if (pk != &idle) gx_release_packet(pk);

      if (secs >= lastReport + 5) {
         lastReport = secs;
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
