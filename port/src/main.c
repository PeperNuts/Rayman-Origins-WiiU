// Rayman Origins Wii U port - first boot.
//
// Runs the original Wii game code on top of the replacement layer and reports how far it
// gets: live log on the TV/GamePad (and UDP port 4405), and an OSFatal screen with the
// crash location if game code faults.
#include <coreinit/debug.h>
#include <coreinit/exception.h>
#include <coreinit/memdefaultheap.h>
#include <coreinit/systeminfo.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <whb/log.h>
#include <whb/log_console.h>
#include <whb/log_udp.h>
#include <whb/proc.h>

#include <stdio.h>
#include <string.h>

#include "port.h"

#define PORT_VERSION "first-boot 10"
#define GAME_STACK_SIZE (4u << 20)

// Read by the patched game code in place of the Wii's 0x800000F8.
uint32_t wii_OSBusClock = 243000000u;

static OSThread sGameThread __attribute__((aligned(16)));
static volatile int sStage; // 0 start, 1 ctors running, 2 main running, 3 main returned
static volatile uint32_t sCtorIndex, sCtorCount;

static const char *exception_name(OSExceptionType type)
{
   switch (type) {
   case OS_EXCEPTION_TYPE_DSI: return "DSI (bad data access)";
   case OS_EXCEPTION_TYPE_ISI: return "ISI (bad code address)";
   case OS_EXCEPTION_TYPE_PROGRAM: return "PROGRAM (illegal instruction)";
   default: return "exception";
   }
}

static BOOL crash(OSExceptionType type, OSContext *ctx)
{
   static char msg[1024];
   uint32_t gameStart = (uint32_t)__game_text_start, gameEnd = (uint32_t)__game_text_end;
   uint32_t pc = ctx->srr0;
   int n = snprintf(msg, sizeof(msg),
                    "Rayman port %s - crash: %s\n"
                    "PC=%08X %s+%X  LR=%08X (g+%X)  DAR=%08X\n"
                    "r1=%08X r3=%08X r4=%08X r2=%08X r13=%08X\n"
                    "stage=%d ctor=%u/%u  game=%08X\n",
                    PORT_VERSION, exception_name(type), (unsigned)pc,
                    (pc >= gameStart && pc < gameEnd) ? "game" : "port",
                    (unsigned)((pc >= gameStart && pc < gameEnd) ? pc - gameStart : pc),
                    (unsigned)ctx->lr, (unsigned)(ctx->lr - gameStart), (unsigned)ctx->dar, (unsigned)ctx->gpr[1], (unsigned)ctx->gpr[3],
                    (unsigned)ctx->gpr[4], (unsigned)ctx->gpr[2], (unsigned)ctx->gpr[13], sStage,
                    (unsigned)sCtorIndex, (unsigned)sCtorCount, (unsigned)gameStart);
   // Call stack: walk the PowerPC back chain, print return addresses relative to game code.
   n += snprintf(msg + n, sizeof(msg) - n, "stack:");
   uint32_t sp = ctx->gpr[1];
   for (int depth = 0; depth < 6 && n < (int)sizeof(msg); depth++) {
      if (sp < 0x10000000 || sp >= 0x50000000 || (sp & 3)) break;
      sp = *(uint32_t *)sp;
      if (sp < 0x10000000 || sp >= 0x50000000 || (sp & 3)) break;
      uint32_t ra = *(uint32_t *)(sp + 4);
      if (ra >= gameStart && ra < gameEnd) {
         n += snprintf(msg + n, sizeof(msg) - n, " g+%X", (unsigned)(ra - gameStart));
      } else {
         n += snprintf(msg + n, sizeof(msg) - n, " %08X", (unsigned)ra);
      }
   }
   n += snprintf(msg + n, sizeof(msg) - n, "\n");
   trace_summary(msg + n, sizeof(msg) - n, 12, 6);
   OSFatal(msg);
   return FALSE;
}

static BOOL crash_dsi(OSContext *c) { return crash(OS_EXCEPTION_TYPE_DSI, c); }
static BOOL crash_isi(OSContext *c) { return crash(OS_EXCEPTION_TYPE_ISI, c); }
static BOOL crash_program(OSContext *c) { return crash(OS_EXCEPTION_TYPE_PROGRAM, c); }

int port_stage(void) { return sStage; }
const char *port_version(void) { return PORT_VERSION; }
OSThread *port_boot_thread(void) { return &sGameThread; }

static int game_thread(int argc, const char **argv)
{
   port_register_boot_thread(OSGetCurrentThread());
   game_set_sda();

   sStage = 1;
   sCtorCount = (uint32_t)(__game_ctors_end - __game_ctors_start);
   WHBLogPrintf("[boot] running %u game static constructors", (unsigned)sCtorCount);
   for (sCtorIndex = 0; sCtorIndex < sCtorCount; sCtorIndex++) {
      game_call((void *)__game_ctors_start[sCtorIndex], 0, 0, 0);
   }
   WHBLogPrintf("[boot] constructors done");

   sStage = 2;
   WHBLogPrintf("[boot] calling game main()");
   int ret = (int)game_call((void *)rayman_main, 0, 0, 0);
   sStage = 3;
   WHBLogPrintf("[boot] game main() returned %d", ret);
   return ret;
}

int main(int argc, char **argv)
{
   WHBProcInit();
   WHBLogConsoleInit();
   WHBLogUdpInit();
   WHBLogPrintf("== Rayman Origins Wii U port - %s ==", PORT_VERSION);

   wii_OSBusClock = OSGetSystemInfo()->busClockSpeed;
   WHBLogPrintf("[boot] bus clock %u Hz", (unsigned)wii_OSBusClock);
   WHBLogPrintf("[boot] game code %p-%p (%u KiB)", __game_text_start, __game_text_end,
                (unsigned)((__game_text_end - __game_text_start) >> 10));
   WHBLogPrintf("[boot] sda anchors r13=%p r2=%p (os had %08X %08X)", _SDA_BASE_, _SDA2_BASE_,
                (unsigned)read_r13(), (unsigned)read_r2());

   port_mem_init();
   port_dvd_init();
   port_vi_init(); // on the main thread = CPU1, where the game runs

   OSSetExceptionCallbackEx(OS_EXCEPTION_MODE_GLOBAL_ALL_CORES, OS_EXCEPTION_TYPE_DSI, crash_dsi);
   OSSetExceptionCallbackEx(OS_EXCEPTION_MODE_GLOBAL_ALL_CORES, OS_EXCEPTION_TYPE_ISI, crash_isi);
   OSSetExceptionCallbackEx(OS_EXCEPTION_MODE_GLOBAL_ALL_CORES, OS_EXCEPTION_TYPE_PROGRAM, crash_program);

   void *stack = MEMAllocFromDefaultHeapEx(GAME_STACK_SIZE, 16);
   if (!stack || !OSCreateThread(&sGameThread, (OSThreadEntryPointFn)game_thread, 0, NULL,
                                 (uint8_t *)stack + GAME_STACK_SIZE, GAME_STACK_SIZE, 16,
                                 OS_THREAD_ATTRIB_AFFINITY_CPU1)) {
      OSFatal("Rayman port: could not create game thread");
   }
   OSSetThreadName(&sGameThread, "Rayman game");
   OSResumeThread(&sGameThread);

   // Console drawing and progress reporting run on the watchdog (CPU0), so a busy game
   // thread on CPU1 cannot freeze the screen. The main thread only serves ProcUI.
   port_watch_start();
   while (WHBProcIsRunning()) {
      OSSleepTicks(OSMillisecondsToTicks(100));
   }

   WHBLogUdpDeinit();
   WHBLogConsoleFree();
   WHBProcShutdown();
   return 0;
}
