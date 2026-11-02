// Rayman Origins Wii U port - RAD Game Tools threading layer (rrThread/rrMutex/rrSemaphore)
// used by the Bink video library.
//
// On the Wii these live in wii_rrthreads.c / wii_rratomics.cpp (replaced). Bink asserts
// (trap) when creation fails, so they must really work. Objects live in caller memory of
// unknown size, so each is mapped to a Cafe object. Bink threads run on the game core,
// like everything else that touches game state.
#include <coreinit/interrupts.h>
#include <coreinit/memdefaultheap.h>
#include <coreinit/mutex.h>
#include <coreinit/semaphore.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <whb/log.h>

#include <stdint.h>
#include <string.h>

#include "port.h"

#define RR_WAIT_INFINITE 0xFFFFFFFFu
#define POLL_TICKS OSMicrosecondsToTicks(200)

static int timed_out(OSTime start, uint32_t ms)
{
   return ms != RR_WAIT_INFINITE && OSGetSystemTime() - start >= (OSTime)OSMillisecondsToTicks(ms);
}

// ---------------------------------------------------------------------------------------
// Atomics: every game thread runs on one core, so an interrupts-off section is atomic.
// ---------------------------------------------------------------------------------------
int32_t wii_rrAtomicAddExchange32(volatile int32_t *addr, int32_t add)
{
   int en = OSDisableInterrupts();
   int32_t old = *addr;
   *addr = old + add;
   OSRestoreInterrupts(en);
   return old;
}

// ---------------------------------------------------------------------------------------
// Mutexes (Cafe mutexes are recursive, like RAD's)
// ---------------------------------------------------------------------------------------
static OSMutex *rr_mutex(void *m)
{
   int created;
   OSMutex *cm = port_map_get(m, PORT_MAP_RR_MUTEX, sizeof(OSMutex), &created);
   if (created) OSInitMutex(cm);
   return cm;
}

int wii_rrMutexCreate(void *m, int32_t spin)
{
   OSInitMutex(rr_mutex(m));
   return 1;
}

void wii_rrMutexDestroy(void *m) {}

void wii_rrMutexLock(void *m)
{
   OSMutex *cm = rr_mutex(m);
   int was = port_irq_enable_for_block();
   OSLockMutex(cm);
   port_irq_restore_after_block(was);
}

int wii_rrMutexLockTimeout(void *m, uint32_t ms)
{
   OSMutex *cm = rr_mutex(m);
   if (OSTryLockMutex(cm)) return 1;
   if (ms == 0) return 0;
   int was = port_irq_enable_for_block();
   OSTime start = OSGetSystemTime();
   int ok = 0;
   while (!(ok = OSTryLockMutex(cm)) && !timed_out(start, ms)) OSSleepTicks(POLL_TICKS);
   port_irq_restore_after_block(was);
   return ok;
}

void wii_rrMutexUnlock(void *m) { OSUnlockMutex(rr_mutex(m)); }

// ---------------------------------------------------------------------------------------
// Semaphores
// ---------------------------------------------------------------------------------------
static OSSemaphore *rr_sem(void *s)
{
   int created;
   OSSemaphore *cs = port_map_get(s, PORT_MAP_RR_SEM, sizeof(OSSemaphore), &created);
   if (created) OSInitSemaphore(cs, 0);
   return cs;
}

int wii_rrSemaphoreCreate(void *s, int32_t initial, int32_t max)
{
   OSInitSemaphore(rr_sem(s), initial);
   return 1;
}

void wii_rrSemaphoreDestroy(void *s) {}

void wii_rrSemaphoreIncrement(void *s, int32_t count)
{
   OSSemaphore *cs = rr_sem(s);
   for (int32_t i = 0; i < count; i++) OSSignalSemaphore(cs);
}

int wii_rrSemaphoreDecrementOrWait(void *s, uint32_t ms)
{
   OSSemaphore *cs = rr_sem(s);
   if (OSTryWaitSemaphore(cs) > 0) return 1;
   if (ms == 0) return 0;
   int was = port_irq_enable_for_block();
   OSTime start = OSGetSystemTime();
   int ok = 0;
   while (!(ok = (OSTryWaitSemaphore(cs) > 0)) && !timed_out(start, ms)) OSSleepTicks(POLL_TICKS);
   port_irq_restore_after_block(was);
   return ok;
}

// ---------------------------------------------------------------------------------------
// Threads
// ---------------------------------------------------------------------------------------
typedef struct {
   OSThread thread;     // first: 8-byte aligned via the map allocator (16)
   uint8_t *stack;
   void *func;          // U32 (*)(void *)
   void *data;
   volatile int running;
   volatile int done;
   uint32_t result;
   char name[24];
} RRThread;

#define MAX_RAD_THREADS 8
static RRThread *sRadThreads[MAX_RAD_THREADS];

// For the watchdog: live RAD threads.
int port_rad_threads(OSThread **out, int max)
{
   int n = 0;
   for (int i = 0; i < MAX_RAD_THREADS && n < max; i++) {
      if (sRadThreads[i] && sRadThreads[i]->running && !sRadThreads[i]->done) out[n++] = &sRadThreads[i]->thread;
   }
   return n;
}

static int rr_thread_entry(int argc, const char **argv)
{
   RRThread *t = (RRThread *)argv;
   game_set_sda();
   t->result = game_call(t->func, (uint32_t)t->data, 0, 0);
   t->done = 1;
   return 0;
}

static RRThread *rr_thread(void *t) { return port_map_get(t, PORT_MAP_RR_THREAD, sizeof(RRThread), NULL); }

int wii_rrThreadCreate(void *t, void *func, uint32_t stackSize, void *data, int32_t priority, const char *name)
{
   RRThread *r = rr_thread(t);
   if (r->running && !r->done) return 0;
   if (r->running) OSJoinThread(&r->thread, NULL);
   if (stackSize < 0x10000) stackSize = 0x10000;
   if (r->stack) MEMFreeToDefaultHeap(r->stack);
   r->stack = MEMAllocFromDefaultHeapEx(stackSize, 16);
   if (!r->stack) return 0;
   r->func = func;
   r->data = data;
   r->done = 0;
   r->running = 1;
   if (priority < 0) priority = 0;
   if (priority > 31) priority = 31;
   strncpy(r->name, name ? name : "rad", sizeof(r->name) - 1);
   if (!OSCreateThread(&r->thread, (OSThreadEntryPointFn)rr_thread_entry, 0, (void *)r, r->stack + stackSize,
                       stackSize, priority, OS_THREAD_ATTRIB_AFFINITY_CPU1)) {
      r->running = 0;
      return 0;
   }
   OSSetThreadName(&r->thread, r->name);
   for (int i = 0; i < MAX_RAD_THREADS; i++) {
      if (sRadThreads[i] == r || !sRadThreads[i]) { sRadThreads[i] = r; break; }
   }
   OSResumeThread(&r->thread);
   WHBLogPrintf("[rad] thread '%s' prio %d stack %u", r->name, (int)priority, (unsigned)stackSize);
   return 1;
}

int wii_rrThreadWaitDone(void *t, uint32_t ms, uint32_t *exitCode)
{
   RRThread *r = rr_thread(t);
   if (!r->running) return 1;
   int was = port_irq_enable_for_block();
   OSTime start = OSGetSystemTime();
   while (!r->done && !timed_out(start, ms)) OSSleepTicks(POLL_TICKS);
   port_irq_restore_after_block(was);
   if (r->done && exitCode) *exitCode = r->result;
   return r->done;
}

void wii_rrThreadCleanUp(void *t)
{
   RRThread *r = rr_thread(t);
   if (r->running && r->done) {
      OSJoinThread(&r->thread, NULL);
      r->running = 0;
      MEMFreeToDefaultHeap(r->stack);
      r->stack = NULL;
   }
}

void wii_rrThreadSetPriority(void *t, int32_t priority)
{
   RRThread *r = rr_thread(t);
   if (priority < 0) priority = 0;
   if (priority > 31) priority = 31;
   if (r->running) OSSetThreadPriority(&r->thread, priority);
}

void wii_rrThreadSleep(uint32_t ms)
{
   int was = port_irq_enable_for_block();
   OSSleepTicks(OSMillisecondsToTicks(ms));
   port_irq_restore_after_block(was);
}

void wii_rrThreadYieldToAny(void)
{
   int was = port_irq_enable_for_block();
   OSYieldThread();
   port_irq_restore_after_block(was);
}
