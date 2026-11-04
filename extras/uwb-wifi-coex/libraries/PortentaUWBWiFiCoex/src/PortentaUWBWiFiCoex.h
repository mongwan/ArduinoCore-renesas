/*
 * PortentaUWBWiFiCoex
 *
 * UWB Shield + Wi-Fi coexistence for the Portenta C33.
 * Needs the patched core (ArduinoCore-renesas, branch "uwb-wifi-coexist"):
 *   - ESPhost talks to the ESP32-C3 through the shared SPI1 object
 *   - lwIpWrapper exposes lwipwrap_lock()/unlock() and setExternalTaskMode()
 *   - SPI exposes getSettings() and spi_set_keep_open()
 *
 * Call order in setup():
 *   1. WiFi.begin(...)            (connect, timer IRQ mode as usual)
 *   2. UWBWiFiCoex.begin()        (switch lwIP to the net task)
 *   3. UWB.begin() / sessions
 *
 * loop() runs at the same priority as the UWB main task (4): it must block
 * regularly (delay(), queue wait, ...) or the net task (priority 1) starves.
 */
#pragma once

#include <Arduino.h>
#include <Arduino_FreeRTOS.h>
#include <IPAddress.h>

#ifndef UWBCOEX_UDP_MAX_PAYLOAD
#define UWBCOEX_UDP_MAX_PAYLOAD 128
#endif

struct UWBWiFiCoexConfig {
  /* UWB Shield chip select. D48 = P103, taken from libUWBShieldApi.a
     (uwb_bus_data_tx / uwb_bus_data_rx call digitalWrite(48, ...)). */
  pin_size_t shieldCsPin = 48;
  /* Below the UWB helper tasks (2) and the UWB main / sketch tasks (4). */
  UBaseType_t netTaskPriority = 1;
  uint16_t netTaskStackWords = 1024;
  /* lwip_task() period. The original timer ran at 100 Hz. */
  uint32_t netPeriodMs = 10;
  /* How long an ESP transaction may wait for the shield CS to go idle. */
  uint32_t busWaitTimeoutMs = 500;
  /* UDP send queue, drained by the net task. */
  uint8_t udpQueueDepth = 8;
  /* Local UDP port used by sendUdp(). 0 = any. Set a fixed port when
     receiving, so that the peer knows where to send. */
  uint16_t udpLocalPort = 0;
  /* UDP receive queue for receiveUdp(). 0 = receiving disabled: incoming
     datagrams on the internal socket are left to the wrapper and dropped. */
  uint8_t udpRxQueueDepth = 0;
};

struct UWBWiFiCoexStats {
  uint32_t espBusAcquired;     /* ESP transactions that got the bus           */
  uint32_t espBusDeferrals;    /* attempts that found the shield CS LOW       */
  uint32_t espBusTimeouts;     /* gave up after busWaitTimeoutMs              */
  uint32_t maxBusWaitMs;       /* longest wait for the shield CS              */
  uint32_t maxBusHoldUs;       /* longest scheduler-suspended ESP transfer    */
  uint32_t udpQueued;
  uint32_t udpDropped;         /* queue full or payload too large             */
  uint32_t udpSent;
  uint32_t udpSendErrors;
  uint32_t netLoops;
  uint32_t netLoopMaxUs;       /* longest lwip_task() + queue drain           */
  size_t   freeHeapMin;        /* xPortGetMinimumEverFreeHeapSize()           */
  bool     udpSocketOk;        /* internal UDP socket opened by begin()       */
  uint32_t udpReceived;        /* datagrams put in the receive queue          */
  uint32_t udpRxDropped;       /* receive queue full, payload too large, or
                                  received in an interrupt                    */
};

class UWBWiFiCoex_ {
public:
  /* Returns false if already started, if a FreeRTOS object could not be
     created (heap), or if the internal UDP socket could not be opened
     (Wi-Fi not connected yet, or no free lwIP UDP PCB: MEMP_NUM_UDP_PCB is 3
     in the precompiled liblwIP.a and DHCP/DNS use some of them). */
  bool begin(const UWBWiFiCoexConfig& cfg = UWBWiFiCoexConfig());
  bool isRunning() const { return _running; }

  /* Non-blocking: copies the payload into the queue. Safe from any task
     (not from an ISR). Returns false when the queue is full. */
  bool sendUdp(const IPAddress& ip, uint16_t port, const uint8_t* data, size_t len);
  bool sendUdp(const IPAddress& ip, uint16_t port, const char* text);

  /* Takes one datagram from the receive queue (needs udpRxQueueDepth > 0).
     Waits up to waitMs for one; this counts as blocking for loop(). Returns
     the payload length (datagrams longer than UWBCOEX_UDP_MAX_PAYLOAD are
     dropped and counted, longer than cap are truncated), or -1 if none. */
  int receiveUdp(uint8_t* buf, size_t cap, IPAddress* fromIp = nullptr,
                 uint16_t* fromPort = nullptr, uint32_t waitMs = 0);

  UWBWiFiCoexStats stats() const;
  void printStats(Print& out) const;

  /* Diagnostics (coex_diag.cpp). Call printBootReport() first thing in
     setup(): it shows where the previous run stopped (last checkpoint,
     assert location or HardFault PC/LR, watchdog reset flag).
     startWatchdog() at the end of setup() arms the watchdog (max ~2600 ms)
     and makes asserts/HardFaults reset the board; call loopTick() on every
     loop() pass to feed it. */
  static void printBootReport(Print& out);
  static bool startWatchdog(uint32_t timeout_ms = 2000);
  static void loopTick();
  static void captureBootTraceOnce();  /* internal, runs at boot */

private:
  static void captureBootTrace();
  static void netTask(void* arg);
  bool _running = false;
};

/* RAII lock for direct WiFiUDP / WiFiClient use from your own tasks.
   The socket wrappers in the patched core already lock internally; use this
   to make a multi-call sequence (beginPacket/write/endPacket) atomic. */
class UWBWiFiCoexLock {
public:
  UWBWiFiCoexLock();
  ~UWBWiFiCoexLock();
  UWBWiFiCoexLock(const UWBWiFiCoexLock&) = delete;
  UWBWiFiCoexLock& operator=(const UWBWiFiCoexLock&) = delete;
};

extern UWBWiFiCoex_ UWBWiFiCoex;
