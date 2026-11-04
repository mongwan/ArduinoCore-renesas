/*
 * PortentaUWBWiFiCoex - diagnostics and heap safety
 *
 * 1. __malloc_lock/__malloc_unlock: newlib's default versions are empty, so
 *    malloc()/free() (used by the UWB stack from priority 2 and 4 tasks and by
 *    ESPhost/lwIpWrapper from the net task) are not protected against
 *    preemption. These versions mask interrupts for the duration of the call
 *    (nesting-safe, ISR-safe).
 * 2. Checkpoints in .noinit RAM, an __assert_func and a HardFault_Handler
 *    that record where the firmware was and reset the board, and a watchdog
 *    fed from loop(). After the reset printBootReport() shows what happened.
 */
#include "PortentaUWBWiFiCoex.h"
#include "coex_diag.h"

#include <WDT.h>
#include <reent.h>

namespace {

constexpr uint32_t kMagic = 0xC0E1D1A6;

struct CoexTrace {
  uint32_t magic;
  uint32_t boots;
  /* last checkpoint, any context */
  uint32_t cp;
  uint32_t cp_ms;
  uint32_t cp_who;       /* 'I' ISR, 'N' net task, 'T' other task, 'S' no scheduler */
  uint32_t cp_prio;
  /* last checkpoint seen in the net task, and the last loop() tick */
  uint32_t net_cp;
  uint32_t net_cp_ms;
  uint32_t loop_ms;
  /* what ended the previous run */
  uint32_t kind;         /* 0 none/power, 1 assert, 2 hardfault */
  uint32_t a_line;
  const char* a_file;
  const char* a_func;
  const char* a_expr;
  uint32_t f_pc, f_lr, f_cfsr, f_hfsr, f_bfar, f_mmfar;
  uint32_t f_task_prio;
  uint32_t f_who;
};

__attribute__((section(".noinit"))) volatile CoexTrace g_trace;
CoexTrace g_prev;           /* copy of the previous run, taken at boot */
bool g_prevValid = false;
bool g_autoReset = false;
void* g_netTask = nullptr;

volatile uint32_t g_mallocDepth = 0;
volatile uint32_t g_mallocPrimask = 0;

inline bool inIsr() { return __get_IPSR() != 0; }

uint32_t who() {
  if (inIsr()) return 'I';
  if (xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED) return 'S';
  return (xTaskGetCurrentTaskHandle() == g_netTask) ? 'N' : 'T';
}

uint32_t prio() {
  if (inIsr() || xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED) return 0xFF;
  return uxTaskPriorityGet(nullptr);
}

const char* cpName(uint32_t cp) {
  switch (cp) {
    case CP_NONE: return "none";
    case CP_LOCK_WAIT: return "lock_wait";
    case CP_LOCK_TAKEN: return "lock_taken";
    case CP_UNLOCK: return "unlock";
    case CP_ACQ_ENTER: return "bus_acquire_enter";
    case CP_ACQ_OK: return "bus_acquired(scheduler suspended)";
    case CP_ACQ_DEFER: return "bus_defer(shield CS low)";
    case CP_ACQ_TIMEOUT: return "bus_timeout";
    case CP_RELEASE: return "bus_release";
    case CP_HS_WAIT: return "handshake_wait";
    case CP_WAIT_1MS: return "ctrl_wait_1ms";
    case CP_POLL_YIELD: return "poll_yield";
    case CP_NET_LWIP: return "net:lwip_task";
    case CP_NET_LWIP_DONE: return "net:lwip_task_done";
    case CP_NET_UDP_BEGIN: return "net:udp_beginPacket";
    case CP_NET_UDP_WRITE: return "net:udp_write";
    case CP_NET_UDP_END: return "net:udp_endPacket";
    case CP_NET_UDP_DONE: return "net:udp_done";
    case CP_NET_SLEEP: return "net:sleep";
    case CP_LOOP: return "loop";
    case CP_SEND_UDP: return "sendUdp";
    default: return "?";
  }
}

[[noreturn]] void resetOrSpin() {
  if (g_autoReset) {
    NVIC_SystemReset();
  }
  for (;;) {
  }
}

}  // namespace

/* ------------------------------------------------------------------------- */
/* checkpoints                                                                */
/* ------------------------------------------------------------------------- */
void coex_cp(CoexCp cp) {
  uint32_t const w = who();
  g_trace.cp = cp;
  g_trace.cp_ms = millis();
  g_trace.cp_who = w;
  g_trace.cp_prio = prio();
  if (w == 'N') {
    g_trace.net_cp = cp;
    g_trace.net_cp_ms = g_trace.cp_ms;
  }
  if (cp == CP_LOOP) {
    g_trace.loop_ms = g_trace.cp_ms;
  }
}

void coex_diag_set_net_task(void* handle) { g_netTask = handle; }

/* ------------------------------------------------------------------------- */
/* heap lock (overrides newlib's empty __malloc_lock / __malloc_unlock)       */
/* ------------------------------------------------------------------------- */
extern "C" void __malloc_lock(struct _reent*) {
  uint32_t const primask = __get_PRIMASK();
  __disable_irq();
  if (g_mallocDepth == 0) {
    g_mallocPrimask = primask;
  }
  g_mallocDepth = g_mallocDepth + 1;
}

extern "C" void __malloc_unlock(struct _reent*) {
  g_mallocDepth = g_mallocDepth - 1;
  if (g_mallocDepth == 0) {
    __set_PRIMASK(g_mallocPrimask);
  }
}

/* ------------------------------------------------------------------------- */
/* assert / HardFault capture                                                 */
/* ------------------------------------------------------------------------- */
extern "C" void __assert_func(const char* file, int line, const char* func, const char* expr) {
  __disable_irq();
  g_trace.kind = 1;
  g_trace.a_file = file;
  g_trace.a_line = (uint32_t)line;
  g_trace.a_func = func;
  g_trace.a_expr = expr;
  g_trace.f_who = who();
  g_trace.f_task_prio = prio();
  resetOrSpin();
}

extern "C" void coex_hardfault_c(uint32_t* sp, uint32_t ipsr) {
  g_trace.kind = 2;
  g_trace.f_pc = sp[6];
  g_trace.f_lr = sp[5];
  g_trace.f_cfsr = SCB->CFSR;
  g_trace.f_hfsr = SCB->HFSR;
  g_trace.f_bfar = SCB->BFAR;
  g_trace.f_mmfar = SCB->MMFAR;
  g_trace.f_who = ipsr;
  resetOrSpin();
}

extern "C" __attribute__((naked)) void HardFault_Handler(void) {
  __asm volatile(
      "tst lr, #4      \n"
      "ite eq          \n"
      "mrseq r0, msp   \n"
      "mrsne r0, psp   \n"
      "mrs r1, ipsr    \n"
      "b coex_hardfault_c \n");
}

/* ------------------------------------------------------------------------- */
/* public API                                                                 */
/* ------------------------------------------------------------------------- */
void UWBWiFiCoex_::captureBootTrace() {
  if (g_trace.magic == kMagic) {
    memcpy(&g_prev, (const void*)&g_trace, sizeof(g_prev));
    g_prevValid = true;
  }
  uint32_t const boots = g_prevValid ? g_prev.boots + 1 : 1;
  memset((void*)&g_trace, 0, sizeof(g_trace));
  g_trace.magic = kMagic;
  g_trace.boots = boots;
}

/* Runs during static initialisation, before setup() and before any
   checkpoint of this boot can overwrite the previous run's trace. */
__attribute__((constructor)) static void coex_capture_at_boot() {
  UWBWiFiCoex_::captureBootTraceOnce();
}

void UWBWiFiCoex_::captureBootTraceOnce() {
  static bool done = false;
  if (!done) {
    done = true;
    captureBootTrace();
  }
}

void UWBWiFiCoex_::printBootReport(Print& out) {
  captureBootTraceOnce();
  uint8_t const rst0 = R_SYSTEM->RSTSR0;
  uint16_t const rst1 = R_SYSTEM->RSTSR1;
  out.print("[coex-boot] boot#");
  out.print(g_trace.boots);
  out.print(" RSTSR0=0x");
  out.print(rst0, HEX);
  out.print(" RSTSR1=0x");
  out.print(rst1, HEX);
  out.print((rst1 & 0x2) ? " (watchdog reset)" : "");
  out.println();
  if (!g_prevValid) {
    out.println("[coex-boot] no previous trace (power-on)");
    return;
  }
  CoexTrace const& p = g_prev;
  out.print("[coex-boot] previous run: last cp=");
  out.print(cpName(p.cp));
  out.print(" @");
  out.print(p.cp_ms);
  out.print("ms by ");
  out.print((char)p.cp_who);
  out.print(" prio=");
  out.print(p.cp_prio);
  out.print(" | net cp=");
  out.print(cpName(p.net_cp));
  out.print(" @");
  out.print(p.net_cp_ms);
  out.print("ms | last loop @");
  out.print(p.loop_ms);
  out.println("ms");
  if (p.kind == 1) {
    out.print("[coex-boot] ASSERT ");
    out.print(p.a_file ? p.a_file : "?");
    out.print(":");
    out.print(p.a_line);
    out.print(" ");
    out.print(p.a_func ? p.a_func : "?");
    out.print(" (");
    out.print(p.a_expr ? p.a_expr : "?");
    out.print(") by ");
    out.print((char)p.f_who);
    out.print(" prio=");
    out.println(p.f_task_prio);
  } else if (p.kind == 2) {
    out.print("[coex-boot] HARDFAULT pc=0x");
    out.print(p.f_pc, HEX);
    out.print(" lr=0x");
    out.print(p.f_lr, HEX);
    out.print(" cfsr=0x");
    out.print(p.f_cfsr, HEX);
    out.print(" hfsr=0x");
    out.print(p.f_hfsr, HEX);
    out.print(" bfar=0x");
    out.print(p.f_bfar, HEX);
    out.print(" ipsr=");
    out.println(p.f_who);
    out.println("[coex-boot] resolve pc/lr with: arm-none-eabi-addr2line -f -C -e <sketch>.elf <pc> <lr>");
  } else {
    out.println("[coex-boot] no assert/fault recorded (hang -> watchdog, or external reset)");
  }
}

bool UWBWiFiCoex_::startWatchdog(uint32_t timeout_ms) {
  g_autoReset = true;
  return WDT.begin(timeout_ms) == 1;
}

/* Set by the core (cores/arduino/usb/SerialUSB.cpp) when the upload tool
   sends DFU detach: the core has written the "stay in bootloader" magic and
   expects a watchdog reset. With our watchdog already running, its own WDT
   setup is ignored (the WDT can only be configured once per reset) and
   feeding it here would keep the sketch alive, so the upload fails with
   "Lost device after RESET". Reset right away instead. */
extern bool is_watchdog_reset_in_progress_for_upload;

void UWBWiFiCoex_::loopTick() {
  coex_cp(CP_LOOP);
  if (is_watchdog_reset_in_progress_for_upload) {
    NVIC_SystemReset();
  }
  WDT.refresh();
}
