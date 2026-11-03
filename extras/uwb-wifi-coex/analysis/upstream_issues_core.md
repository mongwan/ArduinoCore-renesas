# Draft issues for arduino/ArduinoCore-renesas

Not posted yet. Evidence: Portenta C33 + UWB Shield, core 1.6.0, hardware tests of 2026-11-03
(test logs on the test Mac, `~/c33-test/logs`, summarised in `~/c33-test/STATUS.md`).

---

## 1. lwipUDP::endPacket() leaks one pbuf per datagram

**Status:** confirmed on hardware, fix in `patches/0002`.

`endPacket()` sets `_data = NULL` after a successful `udp_sendto()` without `pbuf_free()`.
`udp_sendto()` does not take ownership of the pbuf, so every datagram leaks one. On a C33 sending
JSON lines, `endPacket()` starts failing after about 110 packets while `WiFi.status()` still reports
`WL_CONNECTED`.

- Before the fix: `sent` stops at 110-113, every later `endPacket()` returns 0.
- After the fix: 526/526 and 2075/2075 sends succeed (no UWB involved in the first run).

Fix: `pbuf_free(_data)` before `_data = NULL` (see `libraries/lwIpWrapper/src/lwipUDP.cpp`).

---

## 2. Any use of SPI1 silently breaks the ESP32-C3 Wi-Fi link on the Portenta C33

**Status:** root cause from source, consistent with hardware results; minimal repro not run yet.

ESPhost opens SPI channel 1 with its own FSP instance (`R_SPI_Open`, TX/RX interrupts registered
through IRQManager) and transfers with `R_SPI_WriteRead()`. `SPI1` (ArduinoSPI, P100-P102) is the
same hardware channel. `ArduinoSPI::begin()` always calls `configSpiSettings(DEFAULT_SPI_SETTINGS)`,
which writes `SPCR = 0` and re-enables the unit without the interrupt enables, at 4 MHz mode 0;
`beginTransaction()` does the same with the user's settings. After that, ESPhost transfers never
complete and are dropped after the 500 ms timeout.

Hardware observation (stock core 1.6.0 + fix 1): five UDP heartbeats sent before `UWB.begin()` all
arrive; after `UWB.begin()` (which calls `SPI1.begin()`), none arrive, with or without a ranging
session, while `endPacket()` keeps returning 1 (it only queues the frame for the ESP32).
Without UWB code every heartbeat arrives.

Expected minimal repro (to be run): Wi-Fi UDP sender, call `SPI1.begin()` after five packets,
no UWB Shield involved; sending should stop.

Fix used here (`patches/0001`): ESPhost transfers through the shared `SPI1` object with its own
`SPISettings`, restores the previous settings afterwards, and the SPI library gets `getSettings()`,
an atomic `beginTransaction()` and a keep-open flag so a user's `end()` cannot close the shared bus.

---

## 3. malloc()/free() are not thread-safe with Arduino_FreeRTOS

**Status:** found by inspection; suspected cause of a full MCU freeze, being verified.

`__malloc_lock`/`__malloc_unlock` resolve to newlib's empty versions (`bx lr`) and
Arduino_FreeRTOS does not provide its own, so `malloc()`/`new` are not protected against
preemption between tasks (or against ISRs that allocate, such as the lwIP timer path).
Libraries that allocate from several tasks (PortentaUWBShield from priority 2 and 4 tasks, ESPhost
and lwIpWrapper from a network task) can corrupt the heap.

Workaround used here (`libraries/PortentaUWBWiFiCoex/src/coex_diag.cpp`): `__malloc_lock` masks
interrupts for the duration of the call (nesting- and ISR-safe). A FreeRTOS-based implementation
(`vTaskSuspendAll`/`xTaskResumeAll` outside ISRs) would also work but does not cover ISR allocation.
