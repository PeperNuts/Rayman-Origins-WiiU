// Rayman Origins Wii U port - log capture and CPU-drawn text surfaces for the screens.
// The log ring buffer collects every WHBLog line; gx_render.c draws it on the GamePad, using
// linear RGBA8 surfaces drawn by the CPU (8x16 font) and copied into GX2 colour buffers.
#include <coreinit/cache.h>
#include <coreinit/memdefaultheap.h>
#include <coreinit/spinlock.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <gx2/mem.h>
#include <gx2/surface.h>
#include <proc_ui/procui.h>
#include <whb/gfx.h>
#include <whb/log.h>

#include <stdio.h>
#include <string.h>

#include "font8x16.h"
#include "port.h"

#define LINES 64
#define COLS PORT_LOG_COLS

static char sLines[LINES][COLS];
static uint32_t sCount; // total lines ever logged
static OSSpinLock sLock;
static OSTime sStart;

static void on_log(const char *msg)
{
   OSUninterruptibleSpinLock_Acquire(&sLock);
   while (*msg) {
      const char *nl = strchr(msg, '\n');
      size_t len = nl ? (size_t)(nl - msg) : strlen(msg);
      char *dst = sLines[sCount % LINES];
      size_t n = len < COLS - 1 ? len : COLS - 1;
      memcpy(dst, msg, n);
      dst[n] = 0;
      sCount++;
      msg += len;
      if (*msg == '\n') msg++;
   }
   OSUninterruptibleSpinLock_Release(&sLock);
}

// Copies the last `rows` lines (oldest first) under the lock.
int port_log_snapshot(char out[][COLS], int rows)
{
   OSUninterruptibleSpinLock_Acquire(&sLock);
   uint32_t total = sCount;
   int n = total < (uint32_t)rows ? (int)total : rows;
   for (int i = 0; i < n; i++) memcpy(out[i], sLines[(total - n + i) % LINES], COLS);
   OSUninterruptibleSpinLock_Release(&sLock);
   return n;
}

void port_screen_init(void)
{
   OSInitSpinLock(&sLock);
   sStart = OSGetSystemTime();
   WHBAddLogHandler(on_log);
}

void port_screen_draw(void) {} // drawing happens on the render thread
void port_screen_free(void) { WHBRemoveLogHandler(on_log); }

// ---------------------------------------------------------------------------------------
// CPU-drawn linear RGBA8 surfaces
// ---------------------------------------------------------------------------------------
int port_make_surface(GX2Surface *s, uint32_t w, uint32_t h)
{
   memset(s, 0, sizeof(*s));
   s->dim = GX2_SURFACE_DIM_TEXTURE_2D;
   s->width = w;
   s->height = h;
   s->depth = 1;
   s->mipLevels = 1;
   s->format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
   s->aa = GX2_AA_MODE1X;
   s->use = GX2_SURFACE_USE_TEXTURE;
   s->tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
   GX2CalcSurfaceSizeAndAlignment(s);
   s->image = MEMAllocFromDefaultHeapEx(s->imageSize, s->alignment);
   if (s->image) memset(s->image, 0, s->imageSize);
   return s->image != NULL;
}

void port_fill(GX2Surface *s, uint32_t rgba)
{
   uint32_t *p = (uint32_t *)s->image;
   for (uint32_t y = 0; y < s->height; y++)
      for (uint32_t x = 0; x < s->width; x++) p[y * s->pitch + x] = rgba;
}

void port_text(GX2Surface *s, int x0, int y0, int scale, const char *str, uint32_t fg)
{
   uint32_t *p = (uint32_t *)s->image;
   for (; *str; str++, x0 += 8 * scale) {
      if (x0 + 8 * scale > (int)s->width || y0 + 16 * scale > (int)s->height) return;
      unsigned char c = (unsigned char)*str;
      if (c < 32 || c > 126) c = '?';
      const uint8_t *g = kFont8x16[c - 32];
      for (int y = 0; y < 16 * scale; y++) {
         uint32_t *line = p + (y0 + y) * s->pitch + x0;
         uint8_t bits = g[y / scale];
         for (int x = 0; x < 8 * scale; x++)
            if (bits & (0x80 >> (x / scale))) line[x] = fg;
      }
   }
}

void port_upload(GX2Surface *s)
{
   DCFlushRange(s->image, s->imageSize);
   GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, s->image, s->imageSize);
}

// Copies the whole of `src` into `dst` at (x, y).
void port_blit(GX2Surface *src, GX2Surface *dst, int x, int y)
{
   GX2Rect r = {0, 0, (int32_t)src->width, (int32_t)src->height};
   GX2Point pt = {x, y};
   GX2CopySurfaceEx(src, 0, 0, dst, 0, 0, 1, &r, &pt);
}

