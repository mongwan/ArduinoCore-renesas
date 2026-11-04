/*
 * PortentaUWBWiFiCoex - implementation
 *
 * Three pieces:
 *  1. lwipwrap_lock()/unlock()   recursive lock built on a binary semaphore
 *                                (configUSE_MUTEXES is 0 in Arduino_FreeRTOS)
 *  2. esphost_spi_bus_*()        "CS guard": an ESP32 transfer only starts
 *                                while the UWB Shield CS (D48) is HIGH, with
 *                                the scheduler suspended for the transfer
 *  3. net task                   calls lwip_task() every netPeriodMs and
 *                                drains the UDP send queue
 *  4. UDP receive (optional)     datagrams for the internal socket go to a
 *                                queue read by receiveUdp()
 */
#include "PortentaUWBWiFiCoex.h"

#include <WiFiC3.h>
#include "CNetIf.h"
#include "EspSpiDriver.h"
#include "lwipTypes.h"
#include "coex_diag.h"

namespace {

struct UdpMsg {
  uint32_t ip;
  uint16_t port;
  uint16_t len;
  uint8_t data[UWBCOEX_UDP_MAX_PAYLOAD];
};

UWBWiFiCoexConfig s_cfg;
volatile bool s_active = false;

/* recursive lock */
SemaphoreHandle_t s_lockSem = nullptr;
volatile TaskHandle_t s_lockOwner = nullptr;
volatile uint32_t s_lockDepth = 0;

/* bus guard */
volatile bool s_busSuspended = false;
volatile uint32_t s_busAcquireUs = 0;

/* net task */
QueueHandle_t s_udpQueue = nullptr;
TaskHandle_t s_netTask = nullptr;
/* Static storage (zero-initialised). A WiFiUDP created with new starts with
   garbage members on cores without patches/0003 and its begin() fails. */
WiFiUDP s_udpObj;
WiFiUDP* const s_udp = &s_udpObj;
volatile bool s_udpReady = false;

/* receive (udpRxQueueDepth > 0) */
QueueHandle_t s_udpRxQueue = nullptr;
struct udp_struct* s_udpRx = nullptr;  /* the wrapper's state for s_udpObj */

volatile UWBWiFiCoexStats s_stats = {};

inline bool inIsr() { return __get_IPSR() != 0; }

/* True when blocking FreeRTOS calls are allowed here. */
inline bool rtosContext() {
  return s_active && !inIsr() && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING;
}

inline bool shieldCsIdle() {
  return digitalRead(s_cfg.shieldCsPin) == HIGH;
}

/* The wrapper keeps one received datagram per socket and frees it when the
   next one arrives, so several datagrams handled by one lwip_task() call
   would be lost. Its receive callback calls onDataArrival right after
   storing a datagram (inside lwip_task(), lwIP lock held): copy it to our
   queue and empty the slot. */
void onUdpData() {
  struct udp_struct* const u = s_udpRx;
  if (u == nullptr || u->data.p == nullptr) {
    return;
  }
  struct pbuf* const p = u->data.p;
  bool queued = false;
  if (!inIsr() && s_udpRxQueue != nullptr && p->tot_len <= UWBCOEX_UDP_MAX_PAYLOAD) {
    UdpMsg msg;
    msg.ip = ip_addr_to_u32(&u->ip);
    msg.port = u->port;
    msg.len = pbuf_copy_partial(p, msg.data, p->tot_len, 0);
    queued = xQueueSend(s_udpRxQueue, &msg, 0) == pdTRUE;
  }
  if (queued) {
    s_stats.udpReceived = s_stats.udpReceived + 1;
  } else {
    s_stats.udpRxDropped = s_stats.udpRxDropped + 1;
  }
  uint32_t const primask = __get_PRIMASK();
  __disable_irq();
  pbuf_free(p);
  u->data.p = nullptr;
  u->data.available = 0;
  __set_PRIMASK(primask);
}

/* lwipUDP keeps its udp_struct private; the pcb's recv_arg points to it. */
struct udp_struct* findWrapperState(const void* obj, size_t size) {
  uintptr_t const lo = (uintptr_t)obj;
  uintptr_t const hi = lo + size;
  for (struct udp_pcb* q = udp_pcbs; q != nullptr; q = q->next) {
    uintptr_t const a = (uintptr_t)q->recv_arg;
    if (a >= lo && a < hi) {
      return (struct udp_struct*)q->recv_arg;
    }
  }
  return nullptr;
}

}  // namespace

/* ------------------------------------------------------------------------- */
/* 1. lwIP lock (overrides the weak no-op in lwIpWrapper/CNetIf.cpp)          */
/* ------------------------------------------------------------------------- */
extern "C" void lwipwrap_lock(void) {
  if (!rtosContext()) {
    return;
  }
  TaskHandle_t const me = xTaskGetCurrentTaskHandle();
  if (s_lockOwner == me) {
    s_lockDepth = s_lockDepth + 1;
    return;
  }
  coex_cp(CP_LOCK_WAIT);
  xSemaphoreTake(s_lockSem, portMAX_DELAY);
  coex_cp(CP_LOCK_TAKEN);
  s_lockOwner = me;
  s_lockDepth = 1;
}

extern "C" void lwipwrap_unlock(void) {
  if (!rtosContext()) {
    return;
  }
  /* A lock taken before begin() was a no-op: ignore its unlock. */
  if (s_lockOwner != xTaskGetCurrentTaskHandle()) {
    return;
  }
  s_lockDepth = s_lockDepth - 1;
  if (s_lockDepth == 0) {
    s_lockOwner = nullptr;
    coex_cp(CP_UNLOCK);
    xSemaphoreGive(s_lockSem);
  }
}

extern "C" void lwipwrap_poll_yield(void) {
  /* wrapper polling loops (DNS, DHCP, ping, TCP connect/write) would spin at
     the caller's priority; DHCP even waits for the net task to progress */
  if (rtosContext()) {
    coex_cp(CP_POLL_YIELD);
    vTaskDelay(1);
  }
}

UWBWiFiCoexLock::UWBWiFiCoexLock() { lwipwrap_lock(); }
UWBWiFiCoexLock::~UWBWiFiCoexLock() { lwipwrap_unlock(); }

/* ------------------------------------------------------------------------- */
/* 2. SPI1 arbitration (overrides the weak hooks in ESPhost/EspSpiDriver.cpp) */
/* ------------------------------------------------------------------------- */
extern "C" bool esphost_spi_bus_acquire(void) {
  if (!rtosContext()) {
    /* before begin() or outside a task: original behaviour */
    return true;
  }

  coex_cp(CP_ACQ_ENTER);
  uint32_t const start = millis();
  for (;;) {
    vTaskSuspendAll();
    /* No task can run from here until release(). The UWB stack only drives
       its CS from tasks (its IRQ handler just gives a semaphore), so a HIGH
       CS now means no UWB transfer is in progress and none can start. */
    if (shieldCsIdle()) {
      s_busSuspended = true;
      coex_cp(CP_ACQ_OK);
      s_busAcquireUs = micros();
      s_stats.espBusAcquired = s_stats.espBusAcquired + 1;
      uint32_t const waited = millis() - start;
      if (waited > s_stats.maxBusWaitMs) {
        s_stats.maxBusWaitMs = waited;
      }
      return true;
    }
    (void)xTaskResumeAll();
    s_stats.espBusDeferrals = s_stats.espBusDeferrals + 1;
    coex_cp(CP_ACQ_DEFER);
    if ((millis() - start) >= s_cfg.busWaitTimeoutMs) {
      s_stats.espBusTimeouts = s_stats.espBusTimeouts + 1;
      coex_cp(CP_ACQ_TIMEOUT);
      return false;
    }
    /* let the (lower priority) UWB task finish its transfer */
    vTaskDelay(1);
  }
}

extern "C" void esphost_spi_bus_release(void) {
  if (!s_busSuspended) {
    return;
  }
  uint32_t const held = micros() - s_busAcquireUs;
  if (held > s_stats.maxBusHoldUs) {
    s_stats.maxBusHoldUs = held;
  }
  s_busSuspended = false;
  coex_cp(CP_RELEASE);
  (void)xTaskResumeAll();
}

extern "C" void esphost_spi_handshake_wait(void) {
  /* Busy-wait 100 us like the original driver, but give the CPU away every
     ~1 ms so that a waiting caller at priority 4 does not starve the UWB
     tasks for up to 500 ms. */
  static uint8_t spins = 0;
  R_BSP_SoftwareDelay(100, BSP_DELAY_UNITS_MICROSECONDS);
  coex_cp(CP_HS_WAIT);
  if (rtosContext() && ++spins >= 10) {
    spins = 0;
    vTaskDelay(1);
  }
}

extern "C" void esphost_wait_1ms(void) {
  /* CEspControl::wait_for_answer() polls every 1 ms for up to timeout_ms
     (50 s for some requests). Busy-waiting there at priority 4 (loop) would
     stop the UWB tasks during a reconnect or an RSSI query. */
  coex_cp(CP_WAIT_1MS);
  if (rtosContext()) {
    vTaskDelay(1);
  } else {
    R_BSP_SoftwareDelay(1000, BSP_DELAY_UNITS_MICROSECONDS);
  }
}

/* ------------------------------------------------------------------------- */
/* 3. net task                                                                */
/* ------------------------------------------------------------------------- */
void UWBWiFiCoex_::netTask(void*) {
  TickType_t last = xTaskGetTickCount();
  UdpMsg msg;
  for (;;) {
    uint32_t const t0 = micros();

    coex_cp(CP_NET_LWIP);
    CLwipIf::getInstance().lwip_task();
    coex_cp(CP_NET_LWIP_DONE);

    while (xQueueReceive(s_udpQueue, &msg, 0) == pdTRUE) {
      bool ok;
      {
        UWBWiFiCoexLock lock;
        coex_cp(CP_NET_UDP_BEGIN);
        ok = s_udp->beginPacket(IPAddress(msg.ip), msg.port) == 1;
        coex_cp(CP_NET_UDP_WRITE);
        ok = ok && s_udp->write(msg.data, msg.len) == msg.len;
        coex_cp(CP_NET_UDP_END);
        ok = ok && s_udp->endPacket() == 1;
        coex_cp(CP_NET_UDP_DONE);
      }
      if (ok) {
        s_stats.udpSent = s_stats.udpSent + 1;
      } else {
        s_stats.udpSendErrors = s_stats.udpSendErrors + 1;
      }
    }

    uint32_t const dt = micros() - t0;
    if (dt > s_stats.netLoopMaxUs) {
      s_stats.netLoopMaxUs = dt;
    }
    s_stats.netLoops = s_stats.netLoops + 1;

    coex_cp(CP_NET_SLEEP);
    vTaskDelayUntil(&last, pdMS_TO_TICKS(s_cfg.netPeriodMs));
    /* do not try to catch up after a long stall */
    TickType_t const now = xTaskGetTickCount();
    if ((now - last) > pdMS_TO_TICKS(s_cfg.netPeriodMs)) {
      last = now;
    }
  }
}

bool UWBWiFiCoex_::begin(const UWBWiFiCoexConfig& cfg) {
  if (_running) {
    return false;
  }
  s_cfg = cfg;

  /* The shield CS idles HIGH. Drive it HIGH now so that the guard does not
     read a floating input before UWB.begin() configures the pin. */
  pinMode(s_cfg.shieldCsPin, OUTPUT);
  digitalWrite(s_cfg.shieldCsPin, HIGH);

  s_lockSem = xSemaphoreCreateBinary();
  if (s_lockSem == nullptr) {
    return false;
  }
  xSemaphoreGive(s_lockSem);

  s_udpQueue = xQueueCreate(s_cfg.udpQueueDepth, sizeof(UdpMsg));
  if (s_udpQueue == nullptr) {
    vSemaphoreDelete(s_lockSem);
    s_lockSem = nullptr;
    return false;
  }

  /* Open the socket while lwIP is still driven by the timer IRQ, so that a
     failure leaves everything as it was. */
  s_udpReady = s_udp->begin(s_cfg.udpLocalPort) == 1;
  if (!s_udpReady) {
    vQueueDelete(s_udpQueue);
    s_udpQueue = nullptr;
    vSemaphoreDelete(s_lockSem);
    s_lockSem = nullptr;
    return false;
  }

  /* Stop the 100 Hz timer IRQ that ran lwip_task() and ESP32 SPI transfers
     in interrupt context (it preempted UWB transfers on the same bus). */
  CLwipIf::getInstance().setExternalTaskMode(true);
  s_active = true;

  auto fail = [&]() {
    s_udpRx = nullptr;
    if (s_udpRxQueue != nullptr) {
      vQueueDelete(s_udpRxQueue);
      s_udpRxQueue = nullptr;
    }
    s_active = false;
    CLwipIf::getInstance().setExternalTaskMode(false);
    return false;
  };

  /* Receiving: no lwip_task() runs between here and the net task start, so
     the pcb list and the callback can be changed without the lock. */
  if (s_cfg.udpRxQueueDepth > 0) {
    s_udpRxQueue = xQueueCreate(s_cfg.udpRxQueueDepth, sizeof(UdpMsg));
    s_udpRx = findWrapperState(&s_udpObj, sizeof(s_udpObj));
    if (s_udpRxQueue == nullptr || s_udpRx == nullptr) {
      return fail();
    }
    s_udp->onDataArrival(onUdpData);
  }

  if (xTaskCreate(netTask, "uwbcoex-net", s_cfg.netTaskStackWords, nullptr,
                  s_cfg.netTaskPriority, &s_netTask) != pdPASS) {
    return fail();
  }

  coex_diag_set_net_task(s_netTask);
  _running = true;
  return true;
}

bool UWBWiFiCoex_::sendUdp(const IPAddress& ip, uint16_t port, const uint8_t* data, size_t len) {
  coex_cp(CP_SEND_UDP);
  if (!_running || len > UWBCOEX_UDP_MAX_PAYLOAD) {
    s_stats.udpDropped = s_stats.udpDropped + 1;
    return false;
  }
  UdpMsg msg;
  msg.ip = (uint32_t)ip;
  msg.port = port;
  msg.len = (uint16_t)len;
  memcpy(msg.data, data, len);
  if (xQueueSend(s_udpQueue, &msg, 0) != pdTRUE) {
    s_stats.udpDropped = s_stats.udpDropped + 1;
    return false;
  }
  s_stats.udpQueued = s_stats.udpQueued + 1;
  return true;
}

bool UWBWiFiCoex_::sendUdp(const IPAddress& ip, uint16_t port, const char* text) {
  return sendUdp(ip, port, (const uint8_t*)text, strlen(text));
}

int UWBWiFiCoex_::receiveUdp(uint8_t* buf, size_t cap, IPAddress* fromIp,
                             uint16_t* fromPort, uint32_t waitMs) {
  if (!_running || s_udpRxQueue == nullptr) {
    if (waitMs > 0) {
      vTaskDelay(pdMS_TO_TICKS(waitMs));
    }
    return -1;
  }
  UdpMsg msg;
  if (xQueueReceive(s_udpRxQueue, &msg, pdMS_TO_TICKS(waitMs)) != pdTRUE) {
    return -1;
  }
  size_t const n = (msg.len < cap) ? msg.len : cap;
  if (buf != nullptr && n > 0) {
    memcpy(buf, msg.data, n);
  }
  if (fromIp != nullptr) {
    *fromIp = IPAddress(msg.ip);
  }
  if (fromPort != nullptr) {
    *fromPort = msg.port;
  }
  return (int)n;
}

UWBWiFiCoexStats UWBWiFiCoex_::stats() const {
  UWBWiFiCoexStats s;
  taskENTER_CRITICAL();
  memcpy(&s, (const void*)&s_stats, sizeof(s));
  taskEXIT_CRITICAL();
  s.freeHeapMin = xPortGetMinimumEverFreeHeapSize();
  s.udpSocketOk = s_udpReady;
  return s;
}

void UWBWiFiCoex_::printStats(Print& out) const {
  UWBWiFiCoexStats const s = stats();
  out.print("[coex] esp_bus acq=");   out.print(s.espBusAcquired);
  out.print(" defer=");               out.print(s.espBusDeferrals);
  out.print(" timeout=");             out.print(s.espBusTimeouts);
  out.print(" wait_max_ms=");         out.print(s.maxBusWaitMs);
  out.print(" hold_max_us=");         out.print(s.maxBusHoldUs);
  out.print(" | udp sock=");         out.print(s.udpSocketOk ? "ok" : "FAIL");
  out.print(" q=");                   out.print(s.udpQueued);
  out.print(" sent=");                out.print(s.udpSent);
  out.print(" err=");                 out.print(s.udpSendErrors);
  out.print(" drop=");                out.print(s.udpDropped);
  out.print(" | net loops=");         out.print(s.netLoops);
  out.print(" loop_max_us=");         out.print(s.netLoopMaxUs);
  out.print(" heap_min=");            out.print((unsigned long)s.freeHeapMin);
  if (s_udpRxQueue != nullptr) {
    /* only with receiving enabled, so the default line stays as before */
    out.print(" | rx=");              out.print(s.udpReceived);
    out.print(" rxdrop=");            out.print(s.udpRxDropped);
  }
  out.println();
}

UWBWiFiCoex_ UWBWiFiCoex;
