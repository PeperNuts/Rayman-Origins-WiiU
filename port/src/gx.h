// Rayman Origins Wii U port - GX emulation, shared between the GX front end (game core,
// gx_core.c) and the GX2 back end (render thread on CPU2, gx_render.c).
#pragma once
#include <stdint.h>

#define GX_MAX_STAGES 8   // TEV stages handled by the shader
#define GX_MAX_COORDS 4   // texture coordinates passed per vertex
#define GX_MAX_SLOTS 8    // texture maps

// One vertex as the shader sees it: position in view space, the two rasterised colour
// channels, and generated texture coordinates (s, t, q).
typedef struct {
   float pos[4];
   float c0[4], c1[4];
   float tc[GX_MAX_COORDS][4];
} GxVertex; // 112 bytes

typedef struct {
   uint32_t image;           // game address of the texture data (0 = none)
   uint16_t width, height;
   uint8_t fmt, wrapS, wrapT, minFilt, magFilt, mipmap;
   uint8_t pad[2];
} GxTexKey;

// Everything the back end needs to draw: copied when it changes, shared by later draws.
typedef struct {
   float proj[16];           // row-major 4x4 (GX projection)
   float viewport[6];        // left, top, width, height, near, far (EFB pixels, far/near 0..1)
   int32_t scissor[4];       // left, top, width, height
   // Pixel shader uniforms (vec4 each)
   float stage[GX_MAX_STAGES * 7][4];
   float regs[4][4];         // TEV registers PREV, REG0..2 (start values)
   float konst[4][4];
   float swap[4][4];
   float misc[2][4];         // numStages, cmp0, ref0, op | cmp1, ref1, 0, 0
   GxTexKey tex[GX_MAX_SLOTS];
   uint8_t blendType, blendSrc, blendDst, logicOp;
   uint8_t colorUpdate, alphaUpdate, zEnable, zFunc, zUpdate, cull;
   uint8_t pad[2];
} GxState;

enum { GXCMD_DRAW, GXCMD_CLEAR, GXCMD_COPYTEX };

typedef struct {
   uint32_t type;
   uint32_t state;           // index into packet states (DRAW)
   uint32_t first, count;    // vertices (DRAW, triangle list)
   // CLEAR: colour RGBA in a[0], z in a[1]; COPYTEX: src rect, dest address, width, height, clear
   uint32_t a[8];
} GxCmd;

typedef struct {
   GxVertex *verts;
   uint32_t numVerts, maxVerts;
   GxState *states;
   uint32_t numStates, maxStates;
   GxCmd *cmds;
   uint32_t numCmds, maxCmds;
   uint16_t dispSrc[4];      // EFB rectangle copied to the display
   uint32_t frame;
   int black;                // VISetBlack
   uint32_t dropped;         // primitives that did not fit
   int clearAfter;           // GXCopyDisp cleared the EFB
   uint32_t clearColor, clearZ;
} GxPacket;

// Front end -> back end hand-off (gx_core.c)
GxPacket *gx_acquire_packet(void);   // render thread: next finished packet or NULL
void gx_release_packet(GxPacket *p);
void gx_core_init(void);
uint32_t gx_stat(int which);

// Texture decoding (gx_tex.c): Wii texture -> linear RGBA8 (width x height, pitch in pixels)
int gx_decode_texture(const GxTexKey *k, uint32_t *dst, uint32_t pitch);
uint32_t gx_tex_buffer_size(uint32_t w, uint32_t h, uint32_t fmt);
int gx_is_render_texture(uint32_t addr);
