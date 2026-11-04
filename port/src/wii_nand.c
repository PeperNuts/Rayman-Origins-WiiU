// Rayman Origins Wii U port - Wii NAND file system (save games) on the SD card.
//
// Every Wii NAND path is kept under sd:/rayman/save/nand (so "/title/00010000/xxxx/data/f"
// becomes sd:/rayman/save/nand/title/00010000/xxxx/data/f). The game's safe-save sequence
// (create in /tmp, write, move to the home directory) maps onto ordinary files and rename.
// Asynchronous calls do the work at once and report through a short alarm on the calling
// core, like the Wii's IPC interrupt, so the game has set up its wait state by then.
#include <coreinit/alarm.h>
#include <coreinit/memdefaultheap.h>
#include <coreinit/time.h>
#include <whb/log.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "port.h"
#include <coreinit/interrupts.h>
#include <coreinit/thread.h>

// "nosave" in port.cfg: no NAND (the game runs without saving, as before first-boot 56).
static int no_save(void)
{
   static int v = -1;
   if (v < 0) v = port_cfg("nosave");
   return v;
}

// Every NAND call is logged with the calling thread (start-up crash investigation).
#define NLOG(fmt, ...) do { OSThread *t_ = OSGetCurrentThread(); \
   WHBLogPrintf("[nand] " fmt " (thread %s, irq %d)", __VA_ARGS__, t_ && t_->name ? t_->name : "?", OSIsInterruptEnabled()); } while (0)

#define SAVE_ROOT "fs:/vol/external01/rayman/save/nand"
#define HOME_DIR "/title/00010000/53584f50/data"

enum {
   NAND_OK = 0, NAND_ACCESS = -1, NAND_EXISTS = -6, NAND_INVALID = -8, NAND_MAXFD = -10, NAND_NOEXISTS = -12,
   NAND_UNKNOWN = -64,
};
enum { ACC_READ = 1, ACC_WRITE = 2, ACC_RW = 3 };

#define MAX_FILES 16
static FILE *sFiles[MAX_FILES];

static void sd_path(char *out, size_t size, const char *nand)
{
   if (nand[0] == '/') snprintf(out, size, "%s%s", SAVE_ROOT, nand);
   else snprintf(out, size, "%s%s/%s", SAVE_ROOT, HOME_DIR, nand); // relative to the home directory
}

// Creates every directory above `path`.
static void make_dirs(const char *path)
{
   char tmp[512];
   snprintf(tmp, sizeof(tmp), "%s", path);
   for (char *p = tmp + strlen("fs:/vol/external01/"); *p; p++) {
      if (*p != '/') continue;
      *p = 0;
      mkdir(tmp, 0777);
      *p = '/';
   }
}

static int exists(const char *sd)
{
   struct stat st;
   return stat(sd, &st) == 0;
}

// NANDFileInfo (0x8C bytes, opaque to the game): our handle in the first word.
static int32_t handle_of(const void *info) { int32_t h; memcpy(&h, info, 4); return h; }
static FILE *file_of(const void *info)
{
   int32_t h = handle_of(info);
   return h >= 1 && h <= MAX_FILES ? sFiles[h - 1] : NULL;
}

int32_t wii_NANDGetHomeDir(char *path)
{
   NLOG("get home dir -> %s", no_save() ? "none" : HOME_DIR);
   if (no_save()) return -1;
   strcpy(path, HOME_DIR);
   return NAND_OK;
}
int32_t wii_NANDSetUserData(void *a, void *b) { return NAND_OK; }
int32_t wii_NANDCheck(uint32_t blocks, uint32_t inodes, uint32_t *answer)
{
   NLOG("check %u blocks %u inodes", (unsigned)blocks, (unsigned)inodes);
   if (answer) *answer = 0;
   return NAND_OK;
}
void wii_NANDInitBanner(void *banner, uint32_t flag, const uint16_t *title, const uint16_t *comment) { NLOG("init banner %p", banner); }

int32_t wii_NANDCreate(const char *path, uint8_t perm, uint8_t attr)
{
   NLOG("create %s", path);
   if (no_save()) return -1;
   char sd[512];
   sd_path(sd, sizeof(sd), path);
   if (exists(sd)) return NAND_EXISTS;
   make_dirs(sd);
   FILE *f = fopen(sd, "wb");
   WHBLogPrintf("[nand] create %s -> %s", path, f ? "ok" : "failed");
   if (!f) return NAND_ACCESS;
   fclose(f);
   return NAND_OK;
}

int32_t wii_NANDOpen(const char *path, void *info, uint8_t acc)
{
   NLOG("open %s acc %u", path, (unsigned)acc);
   if (no_save()) return -1;
   char sd[512];
   sd_path(sd, sizeof(sd), path);
   if (!exists(sd)) { WHBLogPrintf("[nand] open %s: does not exist", path); return NAND_NOEXISTS; }
   int slot = -1;
   for (int i = 0; i < MAX_FILES; i++) if (!sFiles[i]) { slot = i; break; }
   if (slot < 0) return NAND_MAXFD;
   FILE *f = fopen(sd, acc == ACC_READ ? "rb" : "r+b");
   if (!f) return NAND_ACCESS;
   sFiles[slot] = f;
   int32_t h = slot + 1;
   memset(info, 0, 0x8C);
   memcpy(info, &h, 4);
   WHBLogPrintf("[nand] open %s (%s)", path, acc == ACC_READ ? "read" : "write");
   return NAND_OK;
}

int32_t wii_NANDSimpleSafeOpen(const char *path, void *info, uint8_t acc, void *buf, uint32_t len)
{
   int32_t r = wii_NANDOpen(path, info, acc);
   if (r == NAND_NOEXISTS && acc != ACC_READ) {
      wii_NANDCreate(path, 0x3F, 0);
      r = wii_NANDOpen(path, info, acc);
   }
   return r;
}

int32_t wii_NANDClose(void *info)
{
   int32_t h = handle_of(info);
   if (h < 1 || h > MAX_FILES || !sFiles[h - 1]) return NAND_INVALID;
   fclose(sFiles[h - 1]);
   sFiles[h - 1] = NULL;
   memset(info, 0, 4);
   return NAND_OK;
}
int32_t wii_NANDSimpleSafeClose(void *info) { return wii_NANDClose(info); }

int32_t wii_NANDRead(void *info, void *buf, uint32_t len)
{
   NLOG("%s %u bytes", "read", (unsigned)len);
   FILE *f = file_of(info);
   if (!f) return NAND_INVALID;
   return (int32_t)fread(buf, 1, len, f);
}

int32_t wii_NANDWrite(void *info, const void *buf, uint32_t len)
{
   NLOG("%s %u bytes", "write", (unsigned)len);
   FILE *f = file_of(info);
   if (!f) return NAND_INVALID;
   int32_t n = (int32_t)fwrite(buf, 1, len, f);
   fflush(f);
   return n;
}

int32_t wii_NANDSeek(void *info, int32_t off, int32_t whence)
{
   FILE *f = file_of(info);
   if (!f) return NAND_INVALID;
   if (fseek(f, off, whence == 1 ? SEEK_CUR : whence == 2 ? SEEK_END : SEEK_SET)) return NAND_INVALID;
   return (int32_t)ftell(f);
}

int32_t wii_NANDGetLength(void *info, uint32_t *len)
{
   FILE *f = file_of(info);
   if (!f) return NAND_INVALID;
   long at = ftell(f);
   fseek(f, 0, SEEK_END);
   *len = (uint32_t)ftell(f);
   fseek(f, at, SEEK_SET);
   return NAND_OK;
}

int32_t wii_NANDMove(const char *path, const char *destDir)
{
   NLOG("move %s -> %s", path, destDir);
   if (no_save()) return -1;
   char from[512], to[512], dir[512];
   sd_path(from, sizeof(from), path);
   sd_path(dir, sizeof(dir), destDir);
   const char *base = strrchr(path, '/');
   base = base ? base + 1 : path;
   snprintf(to, sizeof(to), "%s/%s", dir, base);
   make_dirs(to);
   remove(to);
   int r = rename(from, to);
   WHBLogPrintf("[nand] move %s -> %s: %s", path, destDir, r ? "failed" : "ok");
   return r ? NAND_ACCESS : NAND_OK;
}

// --- asynchronous calls: result through an alarm on this core -------------------------
typedef struct { OSAlarm alarm; void *cb, *block; int32_t result; int used; } Pending;
static Pending sPending[8];

static void deliver(OSAlarm *a, OSContext *ctx)
{
   Pending *p = (Pending *)a;
   void *cb = p->cb, *block = p->block;
   int32_t r = p->result;
   p->used = 0;
   if (cb) game_call(cb, (uint32_t)r, (uint32_t)block, 0);
}

static int32_t later(void *cb, void *block, int32_t result)
{
   Pending *p = NULL;
   for (int i = 0; i < 8; i++) if (!sPending[i].used) { p = &sPending[i]; break; }
   if (!p) { if (cb) game_call(cb, (uint32_t)result, (uint32_t)block, 0); return NAND_OK; }
   p->used = 1; p->cb = cb; p->block = block; p->result = result;
   OSCreateAlarm(&p->alarm);
   OSSetAlarm(&p->alarm, OSMillisecondsToTicks(2), deliver);
   return NAND_OK;
}

int32_t wii_NANDReadAsync(void *info, void *buf, uint32_t len, void *cb, void *block)
{
   return later(cb, block, wii_NANDRead(info, buf, len));
}

int32_t wii_NANDWriteAsync(void *info, const void *buf, uint32_t len, void *cb, void *block)
{
   return later(cb, block, wii_NANDWrite(info, buf, len));
}
