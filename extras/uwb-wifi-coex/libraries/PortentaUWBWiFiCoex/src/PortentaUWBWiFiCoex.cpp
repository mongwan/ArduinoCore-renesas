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
 */
#include "PortentaUWBWiFiCoex.h"

#include <WiFiC3.h>
#include "CNetIf.h"
#include "EspSpiDriver.h"

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
WiFiUDP* s_udp = nullptr;

volatile UWBWiFiCoexStats s_stats = {};

inline bool inIsr() { return __get_IPSR() != 0; }

/* True when blocking FreeRTOS calls are allowed here. */
inline bool rtosContext() {
  return s_active && !inIsr() && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING;
}

inline bool shieldCsIdle() {
  return digitalRead(s_cfg.shieldCsPin) == HIGH;
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
  xSemaphoreTake(s_lockSem, portMAX_DELAY);
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
    xSemaphoreGive(s_lockSem);
  }
}

extern "C" void lwipwrap_poll_yield(void) {
  /* wrapper polling loops (DNS, DHCP, ping, TCP connect/write) would spin at
     the caller's priority; DHCP even waits for the net task to progress */
  if (rtosContext()) {
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

  uint32_t const start = millis();
  for (;;) {
    vTaskSuspendAll();
    /* No task can run from here until release(). The UWB stack only drives
       its CS from tasks (its IRQ handler just gives a semaphore), so a HIGH
       CS now means no UWB transfer is in progress and none can start. */
    if (shieldCsIdle()) {
      s_busSuspended = true;
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
    if ((millis() - start) >= s_cfg.busWaitTimeoutMs) {
      s_stats.espBusTimeouts = s_stats.espBusTimeouts + 1;
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
  (void)xTaskResumeAll();
}

extern "C" void esphost_spi_handshake_wait(void) {
  /* Busy-wait 100 us like the original driver, but give the CPU away every
     ~1 ms so that a waiting caller at priority 4 does not starve the UWB
     tasks for up to 500 ms. */
  static uint8_t spins = 0;
  R_BSP_SoftwareDelay(100, BSP_DELAY_UNITS_MICROSECONDS);
  if (rtosContext() && ++spins >= 10) {
    spins = 0;
    vTaskDelay(1);
  }
}

extern "C" void esphost_wait_1ms(void) {
  /* CEspControl::wait_for_answer() polls every 1 ms for up to timeout_ms
     (50 s for some requests). Busy-waiting there at priority 4 (loop) would
     stop the UWB tasks during a reconnect or an RSSI query. */
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

    CLwipIf::getInstance().lwip_task();

    while (xQueueReceive(s_udpQueue, &msg, 0) == pdTRUE) {
      bool ok;
      {
        UWBWiFiCoexLock lock;
        ok = s_udp->beginPacket(IPAddress(msg.ip), msg.port) == 1;
        ok = ok && s_udp->write(msg.data, msg.len) == msg.len;
        ok = ok && s_udp->endPacket() == 1;
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

  s_udp = new WiFiUDP();
  if (s_udp == nullptr) {
    return false;
  }

  /* Stop the 100 Hz timer IRQ that ran lwip_task() and ESP32 SPI transfers
     in interrupt context (it preempted UWB transfers on the same bus). */
  CLwipIf::getInstance().setExternalTaskMode(true);
  s_active = true;

  s_udp->begin(s_cfg.udpLocalPort);

  if (xTaskCreate(netTask, "uwbcoex-net", s_cfg.netTaskStackWords, nullptr,
                  s_cfg.netTaskPriority, &s_netTask) != pdPASS) {
    s_active = false;
    CLwipIf::getInstance().setExternalTaskMode(false);
    return false;
  }

  _running = true;
  return true;
}

bool UWBWiFiCoex_::sendUdp(const IPAddress& ip, uint16_t port, const uint8_t* data, size_t len) {
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

UWBWiFiCoexStats UWBWiFiCoex_::stats() const {
  UWBWiFiCoexStats s;
  taskENTER_CRITICAL();
  memcpy(&s, (const void*)&s_stats, sizeof(s));
  taskEXIT_CRITICAL();
  s.freeHeapMin = xPortGetMinimumEverFreeHeapSize();
  return s;
}

void UWBWiFiCoex_::printStats(Print& out) const {
  UWBWiFiCoexStats const s = stats();
  out.print("[coex] esp_bus acq=");   out.print(s.espBusAcquired);
  out.print(" defer=");               out.print(s.espBusDeferrals);
  out.print(" timeout=");             out.print(s.espBusTimeouts);
  out.print(" wait_max_ms=");         out.print(s.maxBusWaitMs);
  out.print(" hold_max_us=");         out.print(s.maxBusHoldUs);
  out.print(" | udp q=");             out.print(s.udpQueued);
  out.print(" sent=");                out.print(s.udpSent);
  out.print(" err=");                 out.print(s.udpSendErrors);
  out.print(" drop=");                out.print(s.udpDropped);
  out.print(" | net loops=");         out.print(s.netLoops);
  out.print(" loop_max_us=");         out.print(s.netLoopMaxUs);
  out.print(" heap_min=");            out.println((unsigned long)s.freeHeapMin);
}

UWBWiFiCoex_ UWBWiFiCoex;
