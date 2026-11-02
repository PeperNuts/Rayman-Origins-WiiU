// Rayman Origins Wii U port - split screens until the GX2 renderer exists:
//   GamePad: scrolling log (every WHBLog line)
//   TV:      status page (version, uptime, frame/GPU counters, files, last log lines)
// Replaces WHBLogConsole, which mirrors the same log on both screens.
#include <coreinit/cache.h>
#include <coreinit/memdefaultheap.h>
#include <coreinit/screen.h>
#include <coreinit/spinlock.h>
#include <coreinit/time.h>
#include <whb/log.h>

#include <stdio.h>
#include <string.h>

#include "port.h"

#define LINES 64
#define COLS 100
#define DRC_ROWS 18
#define TV_LOG_ROWS 8

static char sLines[LINES][COLS];
static uint32_t sCount; // total lines ever logged
static OSSpinLock sLock;
static void *sTv, *sDrc;
static uint32_t sTvSize, sDrcSize;
static OSTime sStart;

static void on_log(const char *msg)
{
   // Split multi-line messages; keep each line within the screen width.
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

void port_screen_init(void)
{
   OSInitSpinLock(&sLock);
   sStart = OSGetSystemTime();
   OSScreenInit();
   sTvSize = OSScreenGetBufferSizeEx(SCREEN_TV);
   sDrcSize = OSScreenGetBufferSizeEx(SCREEN_DRC);
   sTv = MEMAllocFromDefaultHeapEx(sTvSize, 0x100);
   sDrc = MEMAllocFromDefaultHeapEx(sDrcSize, 0x100);
   if (sTv && sDrc) {
      OSScreenSetBufferEx(SCREEN_TV, sTv);
      OSScreenSetBufferEx(SCREEN_DRC, sDrc);
      OSScreenEnableEx(SCREEN_TV, TRUE);
      OSScreenEnableEx(SCREEN_DRC, TRUE);
   }
   WHBAddLogHandler(on_log);
}

void port_screen_free(void)
{
   WHBRemoveLogHandler(on_log);
   if (sTv) MEMFreeToDefaultHeap(sTv);
   if (sDrc) MEMFreeToDefaultHeap(sDrc);
   sTv = sDrc = NULL;
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

void port_screen_draw(void)
{
   if (!sTv || !sDrc) return;
   static char rows[DRC_ROWS][COLS];
   char l[128];

   // GamePad: the log.
   OSScreenClearBufferEx(SCREEN_DRC, 0x00000000);
   int n = snapshot(rows, DRC_ROWS);
   for (int i = 0; i < n; i++) OSScreenPutFontEx(SCREEN_DRC, 0, i, rows[i]);

   // TV: status page.
   OSScreenClearBufferEx(SCREEN_TV, 0x10183000);
   uint32_t secs = (uint32_t)(OSTicksToSeconds(OSGetSystemTime() - sStart));
   int y = 1;
   OSScreenPutFontEx(SCREEN_TV, 2, y++, "Rayman Origins - Wii U port (experimental)");
   snprintf(l, sizeof(l), "build %s", port_version());
   OSScreenPutFontEx(SCREEN_TV, 2, y++, l);
   y++;
   snprintf(l, sizeof(l), "uptime %02u:%02u   stage %d", (unsigned)(secs / 60), (unsigned)(secs % 60), port_stage());
   OSScreenPutFontEx(SCREEN_TV, 2, y++, l);
   snprintf(l, sizeof(l), "frames %u   gpu frames %u   files opened %u", (unsigned)port_vi_retrace_count(),
            (unsigned)port_gx_breakpoints(), (unsigned)port_dvd_files());
   OSScreenPutFontEx(SCREEN_TV, 2, y++, l);
   uint32_t f = port_dvd_files();
   if (f) {
      snprintf(l, sizeof(l), "last file: %s", port_dvd_file_name(f - 1));
      OSScreenPutFontEx(SCREEN_TV, 2, y++, l);
   }
   y++;
   OSScreenPutFontEx(SCREEN_TV, 2, y++, "The game is running, but its graphics are not drawn yet");
   OSScreenPutFontEx(SCREEN_TV, 2, y++, "(GX -> GX2 renderer still to be written). Log is on the GamePad.");
   y++;
   static char last[TV_LOG_ROWS][COLS];
   n = snapshot(last, TV_LOG_ROWS);
   for (int i = 0; i < n; i++) OSScreenPutFontEx(SCREEN_TV, 2, y + i, last[i]);

   DCFlushRange(sTv, sTvSize);
   DCFlushRange(sDrc, sDrcSize);
   OSScreenFlipBuffersEx(SCREEN_TV);
   OSScreenFlipBuffersEx(SCREEN_DRC);
}
