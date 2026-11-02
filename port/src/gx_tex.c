// Rayman Origins Wii U port - Wii texture formats -> linear RGBA8 (level 0).
#include <stdint.h>
#include <string.h>

#include "gx.h"

static inline uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static inline uint32_t rgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a) { return r << 24 | g << 16 | b << 8 | a; }
static inline uint32_t x5(uint32_t v) { return (v << 3) | (v >> 2); }
static inline uint32_t x6(uint32_t v) { return (v << 2) | (v >> 4); }
static inline uint32_t x4(uint32_t v) { return (v << 4) | v; }
static inline uint32_t x3(uint32_t v) { return (v << 5) | (v << 2) | (v >> 1); }

// bits per pixel for texture and copy formats (0 = unknown)
static uint32_t bpp(uint32_t fmt)
{
   switch (fmt) {
   case 0x0: case 0x8: case 0xE: case 0x20: case 0x30: return 4;           // I4 CI4 CMPR R4 Z4
   case 0x1: case 0x2: case 0x9: case 0x11: case 0x22: case 0x27: case 0x28:
   case 0x29: case 0x2A: case 0x39: case 0x3A: return 8;                   // I8 IA4 CI8 Z8 RA4 A8 R8 G8 B8 Z8M Z8L
   case 0x3: case 0x4: case 0x5: case 0xA: case 0x13: case 0x23: case 0x2B:
   case 0x2C: case 0x3C: return 16;                                        // IA8 565 5A3 CI14 Z16 RA8 RG8 GB8 Z16L
   case 0x6: case 0x16: case 0x26: return 32;                              // RGBA8 Z24X8 YUVA8
   }
   return 0;
}

uint32_t gx_tex_buffer_size(uint32_t w, uint32_t h, uint32_t fmt)
{
   uint32_t b = bpp(fmt), bw, bh, bytes = 32;
   switch (b) {
   case 4: bw = 8; bh = 8; break;
   case 8: bw = 8; bh = 4; break;
   case 16: bw = 4; bh = 4; break;
   case 32: bw = 4; bh = 4; bytes = 64; break;
   default: return 0;
   }
   return ((w + bw - 1) / bw) * ((h + bh - 1) / bh) * bytes;
}

static inline void put(uint32_t *dst, uint32_t pitch, uint32_t w, uint32_t h, uint32_t x, uint32_t y, uint32_t c)
{
   if (x < w && y < h) dst[y * pitch + x] = c;
}

static void cmpr_block(const uint8_t *s, uint32_t *dst, uint32_t pitch, uint32_t w, uint32_t h, uint32_t bx, uint32_t by)
{
   uint16_t c0 = be16(s), c1 = be16(s + 2);
   uint32_t pal[4][4];
   uint32_t r0 = x5(c0 >> 11), g0 = x6((c0 >> 5) & 63), b0 = x5(c0 & 31);
   uint32_t r1 = x5(c1 >> 11), g1 = x6((c1 >> 5) & 63), b1 = x5(c1 & 31);
   pal[0][0] = r0; pal[0][1] = g0; pal[0][2] = b0; pal[0][3] = 255;
   pal[1][0] = r1; pal[1][1] = g1; pal[1][2] = b1; pal[1][3] = 255;
   if (c0 > c1) {
      pal[2][0] = (2 * r0 + r1) / 3; pal[2][1] = (2 * g0 + g1) / 3; pal[2][2] = (2 * b0 + b1) / 3; pal[2][3] = 255;
      pal[3][0] = (r0 + 2 * r1) / 3; pal[3][1] = (g0 + 2 * g1) / 3; pal[3][2] = (b0 + 2 * b1) / 3; pal[3][3] = 255;
   } else {
      pal[2][0] = (r0 + r1) / 2; pal[2][1] = (g0 + g1) / 2; pal[2][2] = (b0 + b1) / 2; pal[2][3] = 255;
      pal[3][0] = pal[3][1] = pal[3][2] = pal[3][3] = 0;
   }
   for (uint32_t y = 0; y < 4; y++) {
      uint8_t bits = s[4 + y];
      for (uint32_t x = 0; x < 4; x++) {
         uint32_t i = (bits >> (6 - 2 * x)) & 3;
         put(dst, pitch, w, h, bx + x, by + y, rgba(pal[i][0], pal[i][1], pal[i][2], pal[i][3]));
      }
   }
}

int gx_decode_texture(const GxTexKey *k, uint32_t *dst, uint32_t pitch)
{
   const uint8_t *s = (const uint8_t *)k->image;
   uint32_t w = k->width, h = k->height, fmt = k->fmt;
   if (!s || !w || !h) return 0;
   switch (fmt) {
   case 0x0: // I4: 8x8 tiles
      for (uint32_t ty = 0; ty < h; ty += 8)
         for (uint32_t tx = 0; tx < w; tx += 8)
            for (uint32_t y = 0; y < 8; y++)
               for (uint32_t x = 0; x < 8; x += 2, s++) {
                  uint32_t a = x4(*s >> 4), b = x4(*s & 15);
                  put(dst, pitch, w, h, tx + x, ty + y, rgba(a, a, a, a));
                  put(dst, pitch, w, h, tx + x + 1, ty + y, rgba(b, b, b, b));
               }
      return 1;
   case 0x1: // I8: 8x4
      for (uint32_t ty = 0; ty < h; ty += 4)
         for (uint32_t tx = 0; tx < w; tx += 8)
            for (uint32_t y = 0; y < 4; y++)
               for (uint32_t x = 0; x < 8; x++, s++) put(dst, pitch, w, h, tx + x, ty + y, rgba(*s, *s, *s, *s));
      return 1;
   case 0x2: // IA4: 8x4
      for (uint32_t ty = 0; ty < h; ty += 4)
         for (uint32_t tx = 0; tx < w; tx += 8)
            for (uint32_t y = 0; y < 4; y++)
               for (uint32_t x = 0; x < 8; x++, s++) {
                  uint32_t a = x4(*s >> 4), i = x4(*s & 15);
                  put(dst, pitch, w, h, tx + x, ty + y, rgba(i, i, i, a));
               }
      return 1;
   case 0x3: // IA8: 4x4, alpha then intensity
      for (uint32_t ty = 0; ty < h; ty += 4)
         for (uint32_t tx = 0; tx < w; tx += 4)
            for (uint32_t y = 0; y < 4; y++)
               for (uint32_t x = 0; x < 4; x++, s += 2) put(dst, pitch, w, h, tx + x, ty + y, rgba(s[1], s[1], s[1], s[0]));
      return 1;
   case 0x4: // RGB565: 4x4
      for (uint32_t ty = 0; ty < h; ty += 4)
         for (uint32_t tx = 0; tx < w; tx += 4)
            for (uint32_t y = 0; y < 4; y++)
               for (uint32_t x = 0; x < 4; x++, s += 2) {
                  uint16_t v = be16(s);
                  put(dst, pitch, w, h, tx + x, ty + y, rgba(x5(v >> 11), x6((v >> 5) & 63), x5(v & 31), 255));
               }
      return 1;
   case 0x5: // RGB5A3: 4x4
      for (uint32_t ty = 0; ty < h; ty += 4)
         for (uint32_t tx = 0; tx < w; tx += 4)
            for (uint32_t y = 0; y < 4; y++)
               for (uint32_t x = 0; x < 4; x++, s += 2) {
                  uint16_t v = be16(s);
                  uint32_t c = (v & 0x8000) ? rgba(x5((v >> 10) & 31), x5((v >> 5) & 31), x5(v & 31), 255)
                                            : rgba(x4((v >> 8) & 15), x4((v >> 4) & 15), x4(v & 15), x3((v >> 12) & 7));
                  put(dst, pitch, w, h, tx + x, ty + y, c);
               }
      return 1;
   case 0x6: // RGBA8: 4x4 tiles of 64 bytes, AR pairs then GB pairs
      for (uint32_t ty = 0; ty < h; ty += 4)
         for (uint32_t tx = 0; tx < w; tx += 4, s += 64)
            for (uint32_t i = 0; i < 16; i++) {
               const uint8_t *ar = s + 2 * i, *gb = s + 32 + 2 * i;
               put(dst, pitch, w, h, tx + (i & 3), ty + (i >> 2), rgba(ar[1], gb[0], gb[1], ar[0]));
            }
      return 1;
   case 0xE: // CMPR: 8x8 tiles of four 4x4 DXT1 blocks
      for (uint32_t ty = 0; ty < h; ty += 8)
         for (uint32_t tx = 0; tx < w; tx += 8)
            for (uint32_t b = 0; b < 4; b++, s += 8) cmpr_block(s, dst, pitch, w, h, tx + (b & 1) * 4, ty + (b >> 1) * 4);
      return 1;
   }
   // palette formats (no TLUTs used by this game) and unknown formats: mid grey
   for (uint32_t y = 0; y < h; y++)
      for (uint32_t x = 0; x < w; x++) dst[y * pitch + x] = 0x808080FF;
   return 1;
}
