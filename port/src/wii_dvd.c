// Rayman Origins Wii U port - Wii DVD API on top of game files on the SD card.
//
// Disc file "files/<path>" is read from sd:/rayman/files/<path>.
//
// The game's file layer (ITF::hwFile) relies on real Wii semantics:
//  - DVDFileInfo / DVDCommandBlock keep the Wii layout; the game stores its wake-up data in
//    cb.userData and reads cb.state through DVDGetCommandBlockStatus.
//  - reads are asynchronous: after DVDReadAsyncPrio the caller suspends itself until the read
//    callback resumes it. The callback therefore runs on a separate worker thread, which sits
//    on the game's core so the caller's interrupt-disabled check-then-suspend stays atomic.
#include <coreinit/messagequeue.h>
#include <coreinit/thread.h>
#include <whb/log.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "port.h"

#define DISC_ROOT "fs:/vol/external01/rayman/files"

// Wii structures (RVL SDK layout)
typedef struct WiiDVDCommandBlock {
   struct WiiDVDCommandBlock *next, *prev; // 0x00
   uint32_t command;                       // 0x08
   volatile int32_t state;                 // 0x0C
   uint32_t offset;                        // 0x10
   uint32_t length;                        // 0x14
   void *addr;                             // 0x18
   uint32_t currTransferSize;              // 0x1C
   uint32_t transferredSize;               // 0x20
   void *id;                               // 0x24
   void *callback;                         // 0x28
   void *userData;                         // 0x2C
} WiiDVDCommandBlock;

typedef struct {
   WiiDVDCommandBlock cb;                  // 0x00
   uint32_t startAddr;                     // 0x30
   uint32_t length;                        // 0x34
   void *callback;                         // 0x38 void (*)(int32_t result, DVDFileInfo *)
} WiiDVDFileInfo;

_Static_assert(sizeof(WiiDVDCommandBlock) == 0x30, "DVDCommandBlock layout");
_Static_assert(sizeof(WiiDVDFileInfo) == 0x3C, "DVDFileInfo layout");

enum { DVD_STATE_END = 0, DVD_STATE_BUSY = 1, DVD_STATE_FATAL = -1, DVD_COMMAND_READ = 1 };
enum { DVD_RESULT_FATAL = -1 };

// ---------------------------------------------------------------------------------------
// Entry numbers: one per distinct path the game asks about
// ---------------------------------------------------------------------------------------
#define MAX_ENTRIES 1024
static char *sEntryPath[MAX_ENTRIES];
static uint32_t sEntrySize[MAX_ENTRIES];
static int32_t sEntryCount;

#define MODS_ROOT "fs:/vol/external01/rayman/mods"
static char *sEntryReal[MAX_ENTRIES]; // resolved SD path (a mod file replaces the disc file)

// sd:/rayman/mods/<path> replaces disc file <path> when it exists (e.g. edited text).
static void make_sd_path(char *out, size_t size, const char *discPath)
{
   while (*discPath == '/') discPath++;
   snprintf(out, size, "%s/%s", MODS_ROOT, discPath);
   FILE *m = fopen(out, "rb");
   if (m) { fclose(m); WHBLogPrintf("[dvd] using mod file %s", out); return; }
   snprintf(out, size, "%s/%s", DISC_ROOT, discPath);
}

int32_t wii_DVDConvertPathToEntrynum(const char *path)
{
   for (int32_t i = 0; i < sEntryCount; i++) {
      if (strcasecmp(sEntryPath[i], path) == 0) return i;
   }
   char sd[512];
   make_sd_path(sd, sizeof(sd), path);
   FILE *f = fopen(sd, "rb");
   if (!f) {
      WHBLogPrintf("[dvd] missing: %s", path);
      return -1;
   }
   fseek(f, 0, SEEK_END);
   long size = ftell(f);
   fclose(f);
   if (sEntryCount >= MAX_ENTRIES) return -1;
   int32_t e = sEntryCount++;
   sEntryPath[e] = strdup(path);
   sEntryReal[e] = strdup(sd);
   sEntrySize[e] = (uint32_t)size;
   WHBLogPrintf("[dvd] #%d %s (%u bytes)", (int)e, path, (unsigned)size);
   return e;
}

// ---------------------------------------------------------------------------------------
// Open files: DVDFileInfo -> FILE*
// ---------------------------------------------------------------------------------------
#define MAX_OPEN 64
static struct { WiiDVDFileInfo *fi; FILE *f; } sOpen[MAX_OPEN];

static FILE *file_of(WiiDVDFileInfo *fi)
{
   for (int i = 0; i < MAX_OPEN; i++) {
      if (sOpen[i].fi == fi) return sOpen[i].f;
   }
   return NULL;
}

int wii_DVDFastOpen(int32_t entry, WiiDVDFileInfo *fi)
{
   if (entry < 0 || entry >= sEntryCount) return 0;
   FILE *f = fopen(sEntryReal[entry], "rb");
   if (!f) return 0;
   int slot = -1;
   for (int i = 0; i < MAX_OPEN; i++) {
      if (sOpen[i].fi == fi) { fclose(sOpen[i].f); slot = i; break; }
      if (slot < 0 && !sOpen[i].fi) slot = i;
   }
   if (slot < 0) { fclose(f); return 0; }
   sOpen[slot].fi = fi;
   sOpen[slot].f = f;
   fi->startAddr = (uint32_t)entry;
   fi->length = sEntrySize[entry];
   fi->callback = NULL;
   fi->cb.state = DVD_STATE_END;
   return 1;
}

int wii_DVDClose(WiiDVDFileInfo *fi)
{
   for (int i = 0; i < MAX_OPEN; i++) {
      if (sOpen[i].fi == fi) {
         fclose(sOpen[i].f);
         sOpen[i].fi = NULL;
         sOpen[i].f = NULL;
         return 1;
      }
   }
   return 0;
}

// ---------------------------------------------------------------------------------------
// Asynchronous reads
// ---------------------------------------------------------------------------------------
static OSMessageQueue sQueue;
static OSMessage sQueueMsgs[64];
static OSThread sWorker __attribute__((aligned(16)));
static uint8_t sWorkerStack[0x8000] __attribute__((aligned(16)));

static int dvd_worker(int argc, const char **argv)
{
   for (;;) {
      OSMessage m;
      OSReceiveMessage(&sQueue, &m, OS_MESSAGE_FLAGS_BLOCKING);
      WiiDVDFileInfo *fi = (WiiDVDFileInfo *)m.message;
      FILE *f = file_of(fi);
      int32_t result = DVD_RESULT_FATAL;
      if (f && fseek(f, (long)fi->cb.offset, SEEK_SET) == 0) {
         size_t n = fread(fi->cb.addr, 1, fi->cb.length, f);
         result = (int32_t)n;
         fi->cb.transferredSize = (uint32_t)n;
         fi->cb.currTransferSize = (uint32_t)n;
      }
      fi->cb.state = (result >= 0) ? DVD_STATE_END : DVD_STATE_FATAL;
      if (result < 0) WHBLogPrintf("[dvd] read failed: entry %u", (unsigned)fi->startAddr);
      void *cb = fi->callback;
      if (cb) {
         game_call(cb, (uint32_t)result, (uint32_t)fi, 0);
      }
   }
   return 0;
}

void port_dvd_init(void)
{
   OSInitMessageQueue(&sQueue, sQueueMsgs, 64);
   // Same core as the game, above every game thread: behaves like the Wii's DVD interrupt.
   OSCreateThread(&sWorker, (OSThreadEntryPointFn)dvd_worker, 0, NULL, sWorkerStack + sizeof(sWorkerStack),
                  sizeof(sWorkerStack), 1, OS_THREAD_ATTRIB_AFFINITY_CPU1);
   OSSetThreadName(&sWorker, "Rayman DVD");
   OSResumeThread(&sWorker);
}

int wii_DVDReadAsyncPrio(WiiDVDFileInfo *fi, void *addr, int32_t length, int32_t offset, void *callback,
                         int32_t prio)
{
   if (!file_of(fi)) return 0;
   fi->cb.command = DVD_COMMAND_READ;
   fi->cb.addr = addr;
   fi->cb.length = (uint32_t)length;
   fi->cb.offset = (uint32_t)offset;
   fi->cb.transferredSize = 0;
   fi->cb.currTransferSize = 0;
   fi->callback = callback;
   fi->cb.state = DVD_STATE_BUSY;
   OSMessage m = { .message = fi };
   OSSendMessage(&sQueue, &m, OS_MESSAGE_FLAGS_BLOCKING);
   return 1;
}

int32_t wii_DVDGetCommandBlockStatus(const WiiDVDCommandBlock *cb)
{
   return cb->state;
}

int32_t wii_DVDCancel(WiiDVDCommandBlock *cb)
{
   // Reads complete quickly from SD; report the block as finished.
   return 0;
}

int32_t wii_DVDGetDriveStatus(void) { return DVD_STATE_END; }

uint32_t port_dvd_files(void) { return (uint32_t)sEntryCount; }
const char *port_dvd_file_name(uint32_t i) { return i < (uint32_t)sEntryCount ? sEntryPath[i] : ""; }
