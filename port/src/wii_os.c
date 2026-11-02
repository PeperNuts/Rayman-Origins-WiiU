// Rayman Origins Wii U port - Wii OS (RVL SDK) replacement layer, first-boot version.
//
// Every function here is called by the original Wii game code under its Wii name with a
// "wii_" prefix (see the symbol isolation step). Wii structures (OSThread, OSMutex, ...)
// live in game memory with Wii layouts, so each one is mapped to a Cafe OS object.
#include <coreinit/alarm.h>
#include <coreinit/cache.h>
#include <coreinit/condition.h>
#include <coreinit/debug.h>
#include <coreinit/interrupts.h>
#include <coreinit/memdefaultheap.h>
#include <coreinit/memexpheap.h>
#include <coreinit/mutex.h>
#include <coreinit/thread.h>
#include <coreinit/threadqueue.h>
#include <coreinit/time.h>
#include <whb/log.h>

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "port.h"

// ---------------------------------------------------------------------------------------
// Stub tracing (generated stubs call stub_hit)
// ---------------------------------------------------------------------------------------
#define TRACE_RING 16
#define TRACE_DISTINCT 256

static const char *sRing[TRACE_RING];
static uint32_t sRingPos;
static const char *sDistinct[TRACE_DISTINCT];
static uint32_t sDistinctCount;
static volatile uint32_t sStubCalls;

int32_t stub_hit(const char *name, int32_t ret)
{
   sStubCalls++;
   sRing[sRingPos++ % TRACE_RING] = name;
   uint32_t i;
   for (i = 0; i < sDistinctCount; i++) {
      if (sDistinct[i] == name) {
         return ret;
      }
   }
   if (sDistinctCount < TRACE_DISTINCT) {
      sDistinct[sDistinctCount++] = name;
      WHBLogPrintf("[stub] #%u %s -> %d", (unsigned)sDistinctCount, name, (int)ret);
   }
   return ret;
}

uint32_t trace_calls(void) { return sStubCalls; }
uint32_t trace_distinct(void) { return sDistinctCount; }

// Writes "first: a,b,c | last: x,y,z" into buf.
void trace_summary(char *buf, size_t size, uint32_t nfirst, uint32_t nlast)
{
   size_t n = 0;
   n += snprintf(buf + n, size - n, "first:");
   for (uint32_t i = 0; i < sDistinctCount && i < nfirst && n < size; i++) {
      n += snprintf(buf + n, size - n, " %s", sDistinct[i]);
   }
   if (n < size) n += snprintf(buf + n, size - n, "\nlast:");
   uint32_t count = sRingPos < TRACE_RING ? sRingPos : TRACE_RING;
   if (nlast > count) nlast = count;
   for (uint32_t k = nlast; k > 0 && n < size; k--) {
      n += snprintf(buf + n, size - n, " %s", sRing[(sRingPos - k) % TRACE_RING]);
   }
}

// ---------------------------------------------------------------------------------------
// Wii pointer -> Cafe object map
// ---------------------------------------------------------------------------------------
enum { MAP_MUTEX = 1, MAP_COND, MAP_QUEUE, MAP_ALARM };

typedef struct {
   const void *key;
   void *obj;
   uint32_t kind;
} MapEntry;

#define MAP_SIZE 8192 // power of two
static MapEntry sMap[MAP_SIZE];
static OSMutex sMapLock;

static void *alloc_zero(uint32_t size, int32_t align)
{
   void *p = MEMAllocFromDefaultHeapEx(size, align);
   if (!p) {
      OSFatal("Rayman port: out of memory (Wii U default heap)");
   }
   memset(p, 0, size);
   return p;
}

// Returns the Cafe object for key; creates it with `size` bytes when missing.
static void *map_get(const void *key, uint32_t kind, uint32_t size, int *created)
{
   uint32_t h = ((uint32_t)key >> 2) * 2654435761u;
   if (created) *created = 0;
   OSLockMutex(&sMapLock);
   for (uint32_t i = 0; i < MAP_SIZE; i++) {
      MapEntry *e = &sMap[(h + i) & (MAP_SIZE - 1)];
      if (e->key == key && e->kind == kind) {
         OSUnlockMutex(&sMapLock);
         return e->obj;
      }
      if (e->key == NULL) {
         e->key = key;
         e->kind = kind;
         e->obj = alloc_zero(size, 16);
         if (created) *created = 1;
         OSUnlockMutex(&sMapLock);
         return e->obj;
      }
   }
   OSUnlockMutex(&sMapLock);
   OSFatal("Rayman port: Wii object map full");
   return NULL;
}

// ---------------------------------------------------------------------------------------
// Memory: arenas and OS heaps
// ---------------------------------------------------------------------------------------
#define MEM1_ARENA_SIZE (24u << 20)
#define MEM2_ARENA_SIZE (64u << 20)
#define ROUND_UP(x, a) (((uint32_t)(x) + ((a) - 1)) & ~((uint32_t)(a) - 1))

static uint8_t *sMem1Lo, *sMem1Hi, *sMem2Lo, *sMem2Hi;

// Small-data globals referenced by game code through r13: must live in .sdata.
__attribute__((section(".sdata"))) volatile int32_t wii___OSCurrHeap = -1;
__attribute__((section(".sdata"))) volatile uint32_t wii___OSFpscrEnableBits = 0;

#define MAX_OS_HEAPS 16
static MEMHeapHandle sOSHeaps[MAX_OS_HEAPS];
static int32_t sOSHeapCount;

void port_mem_init(void)
{
   sMem1Lo = alloc_zero(MEM1_ARENA_SIZE, 64);
   sMem1Hi = sMem1Lo + MEM1_ARENA_SIZE;
   sMem2Lo = alloc_zero(MEM2_ARENA_SIZE, 64);
   sMem2Hi = sMem2Lo + MEM2_ARENA_SIZE;
   OSInitMutex(&sMapLock);
   WHBLogPrintf("[mem] MEM1 arena %p-%p, MEM2 arena %p-%p", sMem1Lo, sMem1Hi, sMem2Lo, sMem2Hi);
}

void *wii_OSGetMEM1ArenaLo(void) { return sMem1Lo; }
void *wii_OSGetMEM1ArenaHi(void) { return sMem1Hi; }
void *wii_OSGetMEM2ArenaLo(void) { return sMem2Lo; }
void *wii_OSGetMEM2ArenaHi(void) { return sMem2Hi; }
void wii_OSSetMEM1ArenaLo(void *lo) { sMem1Lo = lo; }

static void *arena_alloc(uint8_t **lo, uint8_t *hi, uint32_t size, uint32_t align)
{
   if (align < 4) align = 4;
   uint8_t *p = (uint8_t *)ROUND_UP(*lo, align);
   uint8_t *end = (uint8_t *)ROUND_UP(p + size, align);
   if (end > hi) {
      OSFatal("Rayman port: Wii arena exhausted");
   }
   *lo = end;
   return p;
}

void *wii_OSAllocFromMEM1ArenaLo(uint32_t size, uint32_t align) { return arena_alloc(&sMem1Lo, sMem1Hi, size, align); }
void *wii_OSAllocFromMEM2ArenaLo(uint32_t size, uint32_t align) { return arena_alloc(&sMem2Lo, sMem2Hi, size, align); }

void *wii_OSInitAlloc(void *arenaStart, void *arenaEnd, int32_t maxHeaps)
{
   WHBLogPrintf("[mem] OSInitAlloc %p-%p max %d", arenaStart, arenaEnd, (int)maxHeaps);
   sOSHeapCount = 0;
   wii___OSCurrHeap = -1;
   return (void *)ROUND_UP(arenaStart, 32);
}

int32_t wii_OSCreateHeap(void *start, void *end)
{
   if (sOSHeapCount >= MAX_OS_HEAPS) {
      OSFatal("Rayman port: too many OS heaps");
   }
   uint8_t *s = (uint8_t *)ROUND_UP(start, 32);
   uint32_t size = (uint32_t)((uint8_t *)end - s) & ~31u;
   MEMHeapHandle h = MEMCreateExpHeapEx(s, size, MEM_HEAP_FLAG_USE_LOCK);
   if (!h) {
      OSFatal("Rayman port: MEMCreateExpHeapEx failed");
   }
   sOSHeaps[sOSHeapCount] = h;
   WHBLogPrintf("[mem] OSCreateHeap #%d %p (%u KiB)", (int)sOSHeapCount, s, (unsigned)(size >> 10));
   return sOSHeapCount++;
}

int32_t wii_OSSetCurrentHeap(int32_t heap)
{
   int32_t prev = wii___OSCurrHeap;
   wii___OSCurrHeap = heap;
   return prev;
}

void *wii_OSAllocFromHeap(int32_t heap, uint32_t size)
{
   if (heap < 0 || heap >= sOSHeapCount) {
      return NULL;
   }
   return MEMAllocFromExpHeapEx(sOSHeaps[heap], size, 32);
}

void wii_OSFreeToHeap(int32_t heap, void *ptr)
{
   if (heap >= 0 && heap < sOSHeapCount && ptr) {
      MEMFreeToExpHeap(sOSHeaps[heap], ptr);
   }
}

// ---------------------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------------------
void wii_OSReport(const char *fmt, ...)
{
   char buf[512];
   va_list va;
   va_start(va, fmt);
   vsnprintf(buf, sizeof(buf), fmt, va);
   va_end(va);
   size_t n = strlen(buf);
   if (n && buf[n - 1] == '\n') buf[n - 1] = 0;
   WHBLogPrintf("[game] %s", buf);
}

void wii_OSPanic(const char *file, int line, const char *fmt, ...)
{
   char buf[512];
   int n = snprintf(buf, sizeof(buf), "Rayman port: game OSPanic\n%s:%d\n", file ? file : "?", line);
   va_list va;
   va_start(va, fmt);
   vsnprintf(buf + n, sizeof(buf) - n, fmt, va);
   va_end(va);
   WHBLogPrintf("%s", buf);
   OSFatal(buf);
}

void wii_exit(int code)
{
   char buf[96];
   snprintf(buf, sizeof(buf), "Rayman port: game called exit(%d)", code);
   OSFatal(buf);
}

// MSL console I/O (printf/puts from game code)
int wii___write_console(uint32_t handle, const char *buf, uint32_t *count, void *idle)
{
   char line[256];
   uint32_t n = *count < sizeof(line) - 1 ? *count : sizeof(line) - 1;
   memcpy(line, buf, n);
   line[n] = 0;
   if (n && line[n - 1] == '\n') line[n - 1] = 0;
   WHBLogPrintf("[game:stdout] %s", line);
   return 0;
}

int wii___read_console(uint32_t handle, char *buf, uint32_t *count, void *idle)
{
   *count = 0;
   return 1;
}

int wii___close_console(uint32_t handle) { return 0; }
uint32_t wii___get_time(void) { return 0; }

// ---------------------------------------------------------------------------------------
// Time, interrupts, cache
// ---------------------------------------------------------------------------------------
int64_t wii_OSGetTime(void) { return OSGetTime(); }

void wii_OSTicksToCalendarTime(int64_t ticks, OSCalendarTime *cal)
{
   OSTicksToCalendarTime(ticks, cal); // identical layout on Wii and Wii U
}

uint32_t wii_OSDisableInterrupts(void) { return OSDisableInterrupts(); }
uint32_t wii_OSEnableInterrupts(void) { return OSEnableInterrupts(); }
uint32_t wii_OSRestoreInterrupts(uint32_t level) { return OSRestoreInterrupts(level); }

void wii_DCFlushRange(void *addr, uint32_t len) { DCFlushRange(addr, len); }
void wii_DCInvalidateRange(void *addr, uint32_t len) { DCInvalidateRange(addr, len); }
void wii_DCStoreRange(void *addr, uint32_t len) { DCStoreRange(addr, len); }
void wii_PPCSync(void) { __asm__ volatile("sync" ::: "memory"); }

// ---------------------------------------------------------------------------------------
// Threads
// ---------------------------------------------------------------------------------------
#define MAX_THREADS 64
#define WII_THREAD_ATTR_DETACH 1

typedef struct {
   void *wii;          // game's Wii OSThread struct (key)
   OSThread *cafe;     // our Cafe thread
   void *func;         // void *(*)(void *)
   void *param;
   void *result;
} ThreadRec;

static ThreadRec sThreads[MAX_THREADS];
static uint8_t sBootWiiThread[0x320] __attribute__((aligned(8)));

static ThreadRec *thread_by_wii(const void *wii)
{
   for (int i = 0; i < MAX_THREADS; i++) {
      if (sThreads[i].wii == wii) return &sThreads[i];
   }
   return NULL;
}

static ThreadRec *thread_by_cafe(const OSThread *cafe)
{
   for (int i = 0; i < MAX_THREADS; i++) {
      if (sThreads[i].cafe == cafe && sThreads[i].wii) return &sThreads[i];
   }
   return NULL;
}

static ThreadRec *thread_new(void *wii)
{
   ThreadRec *r = thread_by_wii(wii);
   if (r) return r;
   int en = OSDisableInterrupts();
   for (int i = 0; i < MAX_THREADS; i++) {
      if (!sThreads[i].wii) {
         memset(&sThreads[i], 0, sizeof(ThreadRec));
         sThreads[i].wii = wii;
         OSRestoreInterrupts(en);
         return &sThreads[i];
      }
   }
   OSRestoreInterrupts(en);
   OSFatal("Rayman port: too many game threads");
   return NULL;
}

// Registers the Cafe thread that runs rayman_main as the Wii boot thread.
void port_register_boot_thread(OSThread *cafe)
{
   ThreadRec *r = thread_new(sBootWiiThread);
   r->cafe = cafe;
}

static int game_thread_entry(int argc, const char **argv)
{
   ThreadRec *r = (ThreadRec *)argv;
   game_set_sda();
   r->result = (void *)game_call(r->func, (uint32_t)r->param, 0, 0);
   return (int)r->result;
}

// Replacement for game threads whose body is a bare spin loop ("b ."). On the single-core
// Wii such an idle thread is harmless; on Wii U it keeps CPU1 busy, so sleep instead.
static void *port_idle_thread(void *param)
{
   for (;;) OSSleepTicks(OSSecondsToTicks(1));
   return NULL;
}

int wii_OSCreateThread(void *wiiThread, void *func, void *param, void *stackTop, uint32_t stackSize,
                       int32_t priority, uint16_t attr)
{
   if (*(volatile uint32_t *)func == 0x48000000u) {
      WHBLogPrintf("[thread] game idle thread %p is a spin loop - using a sleeping idle thread", func);
      func = (void *)port_idle_thread;
   }
   ThreadRec *r = thread_new(wiiThread);
   if (!r->cafe) {
      r->cafe = alloc_zero(sizeof(OSThread), 16);
   }
   r->func = func;
   r->param = param;
   memset((uint8_t *)wiiThread + 0x30C, 0, 12); // error, specific[0], specific[1] - as the Wii does
   // Wii is single-core: keep every game thread on the main core so interrupt-disabled
   // critical sections and priority scheduling behave like on the Wii.
   OSThreadAttributes cattr = OS_THREAD_ATTRIB_AFFINITY_CPU1;
   if (attr & WII_THREAD_ATTR_DETACH) cattr |= OS_THREAD_ATTRIB_DETACHED;
   BOOL ok = OSCreateThread(r->cafe, (OSThreadEntryPointFn)game_thread_entry, 0, (void *)r, stackTop,
                            stackSize, priority, cattr);
   if (ok) {
      static char names[MAX_THREADS][16];
      int idx = (int)(r - sThreads);
      snprintf(names[idx], sizeof(names[idx]), "game%d", idx);
      OSSetThreadName(r->cafe, names[idx]);
   }
   WHBLogPrintf("[thread] create wii=%p func=%p prio=%d stack=%u -> %d", wiiThread, func, (int)priority,
                (unsigned)stackSize, ok);
   return ok;
}

int32_t wii_OSResumeThread(void *t)
{
   ThreadRec *r = thread_by_wii(t);
   return r ? OSResumeThread(r->cafe) : 0;
}

int32_t wii_OSSuspendThread(void *t)
{
   ThreadRec *r = thread_by_wii(t);
   return r ? OSSuspendThread(r->cafe) : 0;
}

void wii_OSCancelThread(void *t)
{
   ThreadRec *r = thread_by_wii(t);
   if (r) OSCancelThread(r->cafe);
}

int wii_OSIsThreadTerminated(void *t)
{
   ThreadRec *r = thread_by_wii(t);
   return r ? OSIsThreadTerminated(r->cafe) : 1;
}

void *wii_OSGetCurrentThread(void)
{
   ThreadRec *r = thread_by_cafe(OSGetCurrentThread());
   return r ? r->wii : sBootWiiThread;
}

// Thread-specific values live where the Wii keeps them: OSThread.specific[2] at 0x310 in the
// game's own Wii OSThread struct. The game writes that field directly (ITF::Thread::create
// sets specific[0] between OSCreateThread and OSResumeThread), so a side table won't do.
#define WII_THREAD_SPECIFIC 0x310

static void **thread_specific(void)
{
   return (void **)((uint8_t *)wii_OSGetCurrentThread() + WII_THREAD_SPECIFIC);
}

void wii_OSSetThreadSpecific(int32_t idx, void *ptr)
{
   if (idx >= 0 && idx < 2) thread_specific()[idx] = ptr;
}

void *wii_OSGetThreadSpecific(int32_t idx)
{
   return (idx >= 0 && idx < 2) ? thread_specific()[idx] : NULL;
}

void wii_OSYieldThread(void) { OSYieldThread(); }
void wii_OSSleepTicks(int64_t ticks) { OSSleepTicks(ticks); }

static OSThreadQueue *queue_of(void *wiiQueue)
{
   int created;
   OSThreadQueue *q = map_get(wiiQueue, MAP_QUEUE, sizeof(OSThreadQueue), &created);
   if (created) OSInitThreadQueue(q);
   return q;
}

void wii_OSInitThreadQueue(void *wiiQueue)
{
   memset(wiiQueue, 0, 8);
   OSInitThreadQueue(queue_of(wiiQueue));
}

void wii_OSSleepThread(void *wiiQueue) { OSSleepThread(queue_of(wiiQueue)); }
void wii_OSWakeupThread(void *wiiQueue) { OSWakeupThread(queue_of(wiiQueue)); }

// ---------------------------------------------------------------------------------------
// Mutexes and condition variables
// ---------------------------------------------------------------------------------------
static OSMutex *mutex_of(void *m)
{
   int created;
   OSMutex *cm = map_get(m, MAP_MUTEX, sizeof(OSMutex), &created);
   if (created) OSInitMutex(cm);
   return cm;
}

static OSCondition *cond_of(void *c)
{
   int created;
   OSCondition *cc = map_get(c, MAP_COND, sizeof(OSCondition), &created);
   if (created) OSInitCond(cc);
   return cc;
}

void wii_OSInitMutex(void *m) { OSInitMutex(mutex_of(m)); }
void wii_OSLockMutex(void *m) { OSLockMutex(mutex_of(m)); }
void wii_OSUnlockMutex(void *m) { OSUnlockMutex(mutex_of(m)); }
void wii_OSInitCond(void *c) { OSInitCond(cond_of(c)); }
void wii_OSWaitCond(void *c, void *m) { OSWaitCond(cond_of(c), mutex_of(m)); }
void wii_OSSignalCond(void *c) { OSSignalCond(cond_of(c)); }

// ---------------------------------------------------------------------------------------
// Alarms
// ---------------------------------------------------------------------------------------
typedef struct {
   OSAlarm cafe;
   void *wii;
   void *handler; // void (*)(OSAlarm *wii, OSContext *)
} AlarmRec;

static void alarm_callback(OSAlarm *alarm, OSContext *ctx)
{
   AlarmRec *a = (AlarmRec *)alarm;
   if (a->handler) {
      game_call(a->handler, (uint32_t)a->wii, 0, 0);
   }
}

static AlarmRec *alarm_of(void *wiiAlarm)
{
   int created;
   AlarmRec *a = map_get(wiiAlarm, MAP_ALARM, sizeof(AlarmRec), &created);
   if (created) {
      OSCreateAlarm(&a->cafe);
      a->wii = wiiAlarm;
   }
   return a;
}

void wii_OSSetAlarm(void *wiiAlarm, int64_t ticks, void *handler)
{
   AlarmRec *a = alarm_of(wiiAlarm);
   a->handler = handler;
   OSSetAlarm(&a->cafe, ticks, alarm_callback);
}

void wii_OSSetPeriodicAlarm(void *wiiAlarm, int64_t start, int64_t period, void *handler)
{
   AlarmRec *a = alarm_of(wiiAlarm);
   a->handler = handler;
   OSSetPeriodicAlarm(&a->cafe, start, period, alarm_callback);
}

void wii_OSCancelAlarm(void *wiiAlarm)
{
   OSCancelAlarm(&alarm_of(wiiAlarm)->cafe);
}

// ---------------------------------------------------------------------------------------
// System settings (SC): values a Wii in English, 16:9, 60 Hz, progressive, stereo returns.
// ---------------------------------------------------------------------------------------
uint8_t wii_SCGetLanguage(void) { return 1; }       // SC_LANG_ENGLISH
uint8_t wii_SCGetAspectRatio(void) { return 1; }    // SC_ASPECT_RATIO_16x9
uint8_t wii_SCGetEuRgb60Mode(void) { return 1; }    // 60 Hz
uint8_t wii_SCGetProgressiveMode(void) { return 1; }
uint8_t wii_SCGetSoundMode(void) { return 1; }      // stereo

// For the watchdog: every live Cafe thread that runs game code (boot thread first).
int port_game_threads(OSThread **out, int max)
{
   int n = 0;
   for (int i = 0; i < MAX_THREADS && n < max; i++) {
      if (sThreads[i].wii && sThreads[i].cafe) out[n++] = sThreads[i].cafe;
   }
   return n;
}
