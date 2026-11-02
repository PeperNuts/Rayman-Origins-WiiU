// Rayman Origins Wii U port - display output through GX2 (stage 1 of the renderer).
//
//   TV:      the game's display copies. Stage 1 shows the copy-clear colour the game sets
//            (GXSetCopyClear) for every frame, plus a one-line status strip at the bottom.
//            Geometry, textures and TEV come in the next stages.
//   GamePad: scrolling log (every WHBLog line), drawn by the CPU in an 8x16 font.
//
// GX2 is bound to the core that initialised it, so one render thread on CPU2 (unused by the
// game, which runs on CPU1) owns all GX2 work. Text is drawn by the CPU into linear surfaces
// and copied into WHBGfx's colour buffers with GX2CopySurfaceEx (the copy retiles).
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
#define COLS 110

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
static int snapshot(char out[][COLS], int rows)
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
static int make_surface(GX2Surface *s, uint32_t w, uint32_t h)
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

static void fill(GX2Surface *s, uint32_t rgba)
{
   uint32_t *p = (uint32_t *)s->image;
   for (uint32_t y = 0; y < s->height; y++)
      for (uint32_t x = 0; x < s->width; x++) p[y * s->pitch + x] = rgba;
}

static void text(GX2Surface *s, int col, int row, const char *str, uint32_t fg)
{
   uint32_t *p = (uint32_t *)s->image;
   int x0 = col * 8, y0 = row * 16;
   for (; *str; str++, x0 += 8) {
      if (x0 + 8 > (int)s->width || y0 + 16 > (int)s->height) return;
      unsigned char c = (unsigned char)*str;
      if (c < 32 || c > 126) c = '?';
      const uint8_t *g = kFont8x16[c - 32];
      for (int y = 0; y < 16; y++) {
         uint32_t *line = p + (y0 + y) * s->pitch + x0;
         for (int x = 0; x < 8; x++)
            if (g[y] & (0x80 >> x)) line[x] = fg;
      }
   }
}

static void upload(GX2Surface *s)
{
   DCFlushRange(s->image, s->imageSize);
   GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, s->image, s->imageSize);
}

// Copies the whole of `src` into `dst` at (x, y).
static void blit(GX2Surface *src, GX2Surface *dst, int x, int y)
{
   GX2Rect r = {0, 0, (int32_t)src->width, (int32_t)src->height};
   GX2Point pt = {x, y};
   GX2CopySurfaceEx(src, 0, 0, dst, 0, 0, 1, &r, &pt);
}

// ---------------------------------------------------------------------------------------
// Render thread
// ---------------------------------------------------------------------------------------
static OSThread sRender __attribute__((aligned(16)));
static uint8_t sRenderStack[0x10000] __attribute__((aligned(16)));

static int render_main(int argc, const char **argv)
{
   WHBGfxInit();
   GX2ColorBuffer *tv = WHBGfxGetTVColourBuffer(), *drc = WHBGfxGetDRCColourBuffer();
   GX2Surface strip, pad;
   if (!make_surface(&strip, tv->surface.width, 32) || !make_surface(&pad, drc->surface.width, drc->surface.height)) {
      WHBLogPrintf("[gfx] out of memory for screen surfaces");
      return 0;
   }
   WHBLogPrintf("[gfx] GX2 up: TV %ux%u, GamePad %ux%u", (unsigned)tv->surface.width, (unsigned)tv->surface.height,
                (unsigned)drc->surface.width, (unsigned)drc->surface.height);

   const int padRows = (int)(drc->surface.height / 16);
   static char rows[40][COLS];
   char l[160];
   for (;;) {
      if (!ProcUIInForeground()) { // never touch the GPU in the background
         OSSleepTicks(OSMillisecondsToTicks(50));
         continue;
      }
      // GamePad: log.
      fill(&pad, 0x000000FF);
      int n = snapshot(rows, padRows < 40 ? padRows : 40);
      for (int i = 0; i < n; i++) text(&pad, 0, i, rows[i], 0xE0E0E0FF);
      upload(&pad);

      // TV status strip.
      uint32_t secs = (uint32_t)OSTicksToSeconds(OSGetSystemTime() - sStart);
      uint32_t cc = port_gx_copy_clear();
      snprintf(l, sizeof(l), " Rayman Origins Wii U - %s   %02u:%02u   frames %u   copies %u   clear %08X%s",
               port_version(), (unsigned)(secs / 60), (unsigned)(secs % 60), (unsigned)port_vi_retrace_count(),
               (unsigned)port_gx_copy_disp(), (unsigned)cc, port_vi_black() ? "   (VI black)" : "");
      fill(&strip, 0x000000C0);
      text(&strip, 0, 1, l, 0xFFFFFFFF);
      upload(&strip);

      float r = ((cc >> 24) & 0xFF) / 255.0f, g = ((cc >> 16) & 0xFF) / 255.0f, b = ((cc >> 8) & 0xFF) / 255.0f;
      if (port_vi_black()) r = g = b = 0.0f;

      WHBGfxBeginRender();
      WHBGfxBeginRenderTV();
      WHBGfxClearColor(r, g, b, 1.0f);
      blit(&strip, &tv->surface, 0, (int)tv->surface.height - 32);
      WHBGfxFinishRenderTV();
      WHBGfxBeginRenderDRC();
      WHBGfxClearColor(0.0f, 0.0f, 0.0f, 1.0f);
      blit(&pad, &drc->surface, 0, 0);
      WHBGfxFinishRenderDRC();
      WHBGfxFinishRender(); // swaps and waits for the flip: paced by the display
   }
   return 0;
}

void port_render_start(void)
{
   OSCreateThread(&sRender, (OSThreadEntryPointFn)render_main, 0, NULL, sRenderStack + sizeof(sRenderStack),
                  sizeof(sRenderStack), 16, OS_THREAD_ATTRIB_AFFINITY_CPU2);
   OSSetThreadName(&sRender, "Rayman render");
   OSResumeThread(&sRender);
}
