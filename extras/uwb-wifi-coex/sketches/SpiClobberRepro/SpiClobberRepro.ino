/*
 * SpiClobberRepro - minimal repro: one SPI1.begin() silently breaks Wi-Fi
 *
 * No UWB Shield code. Sends one JSON line per 200 ms over UDP. After
 * CLOBBER_AFTER packets it calls SPI1.begin() (what UWB.begin() does).
 *
 * Expected on the stock core: packets stop arriving at the PC right after
 * the clobber while endPacket() keeps returning 1 (frames are only queued for
 * the ESP32; the ESPhost FSP transfer on channel 1 never completes because
 * ArduinoSPI::begin() rewrote SPCR without the interrupt enables).
 * Expected with patches/0001: packets keep arriving.
 *
 * Without reverting the core: on the patched core, build with
 *   --build-property "compiler.cpp.extra_flags=-DESPHOST_USE_ARDUINO_SPI=0"
 * ESPhost then uses its original FSP SPI driver (only the SPI1 sharing of
 * patches/0001 is off; the other fixes stay) and should show the failure.
 *
 * PC: python3 tools/udp_rx_stats.py --port 9000 --duration 60 --wait-s 60
 *     -> stall FAIL on the stock core, PASS with 0001.
 */
#include <WiFiC3.h>
#include <SPI.h>
#include "arduino_secrets.h"

#ifndef CLOBBER_AFTER
#define CLOBBER_AFTER 25
#endif

static const IPAddress kServer(SERVER_IP);
static WiFiUDP udp;

void setup() {
  Serial.begin(115200);
  for (uint32_t t0 = millis(); !Serial && millis() - t0 < 3000;) delay(10);
  while (WiFi.begin(SECRET_SSID, SECRET_PASS) != WL_CONNECTED) delay(1000);
  Serial.print("IP ");
  Serial.println(WiFi.localIP());
  udp.begin(0);
}

void loop() {
  static uint32_t seq = 0, okCount = 0, failCount = 0;
  static bool clobbered = false;

  if (!clobbered && seq == CLOBBER_AFTER) {
    SPI1.begin();  // the only change; no transfer, no other SPI1 call
    clobbered = true;
    Serial.println("SPI1.begin() called");
  }

  char line[96];
  int n = snprintf(line, sizeof(line), "{\"type\":\"r\",\"seq\":%lu,\"t\":%lu,\"mac\":\"0000\",\"d\":%u}\n",
                   (unsigned long)seq, (unsigned long)millis(), clobbered ? 1u : 0u);
  bool ok = udp.beginPacket(kServer, SERVER_PORT) == 1;
  ok = ok && udp.write((const uint8_t*)line, n) == (size_t)n;
  ok = ok && udp.endPacket() == 1;
  ok ? okCount++ : failCount++;
  seq++;

  if (seq % 25 == 0) {
    Serial.print("seq=");
    Serial.print(seq);
    Serial.print(" endPacket ok=");
    Serial.print(okCount);
    Serial.print(" fail=");
    Serial.print(failCount);
    Serial.print(clobbered ? " (after SPI1.begin)" : " (before)");
    Serial.print(" wifi=");
    Serial.println(WiFi.status());
  }
  delay(200);
}
