/*
 * CoexAnchorUdp - UWB 1:N ranging + Wi-Fi UDP on one Portenta C33
 *
 * Needs the patched core (ArduinoCore-renesas, branch uwb-wifi-coexist).
 * Counterpart: sketches/CoexTag (address 0x2222; add 0x3333 for 1:N mode).
 *
 * Every valid ranging result becomes one JSON line on UDP:
 *   {"type":"r","seq":12,"t":53012,"mac":"2222","d":143}
 * Every 5 s a stats line is sent and printed:
 *   {"type":"s","seq":340,"t":...,"acq":..,"defer":..,"tmo":..,"drop":..,"heap":..}
 * Run tools/udp_rx_stats.py on the PC to measure loss and stalls.
 */
#include <PortentaUWBShield.h>
#include <WiFiC3.h>
#include <PortentaUWBWiFiCoex.h>
#include "arduino_secrets.h"

/*
 * ANCHOR_ONE_TO_ONE = 1 (default): UWBRangingController with one peer (0x2222).
 *   Verified on hardware with the CoexTag sketch on the other board.
 * ANCHOR_ONE_TO_ONE = 0: UWBRangingOneToMany with peers 0x2222 and 0x3333.
 *   Not working yet with a Controlee tag (all measurements status 0x21).
 *
 * PortentaUWBShield 1.0.2 bug: UWBRangingController/UWBRangingControlee keep
 * a pointer to the dstAddr copy their constructor received, so PEER_ADDRESS
 * is read from a dead stack frame at init(). The address is re-registered
 * below from a static object.
 */
#ifndef ANCHOR_ONE_TO_ONE
#define ANCHOR_ONE_TO_ONE 1
#endif

/*
 * COEX_SEND_ALL = 1: send every measurement, failed ones too (d=65535,
 * "st" = status). For single-board tests: with no tag the controller still
 * runs a ranging round every 200 ms and the UWB chip reports each one over
 * SPI1, so this gives ~5 datagrams/s that race the UWB SPI traffic.
 * Default 0: only valid distances are sent.
 */
#ifndef COEX_SEND_ALL
#define COEX_SEND_ALL 0
#endif
static volatile uint32_t g_uwbCallbacks = 0;  // ranging notifications seen

static const IPAddress kServer(SERVER_IP);
static const uint16_t kPort = SERVER_PORT;

static volatile uint32_t g_seq = 0;        // ranging results produced
static volatile uint32_t g_enqueueFail = 0;

static void rangingHandler(UWBRangingData& rangingData) {
  if (rangingData.measureType() != (uint8_t)uwb::MeasurementType::TWO_WAY) {
    return;
  }
  g_uwbCallbacks = g_uwbCallbacks + 1;
  RangingMeasures twr = rangingData.twoWayRangingMeasure();
  for (int j = 0; j < rangingData.available(); j++) {
    if (!COEX_SEND_ALL && (twr[j].status != 0 || twr[j].distance == 0xFFFF)) {
      continue;
    }
    char line[UWBCOEX_UDP_MAX_PAYLOAD];
    uint32_t const seq = g_seq;
    g_seq = seq + 1;
    int n = snprintf(line, sizeof(line),
                     "{\"type\":\"r\",\"seq\":%lu,\"t\":%lu,\"mac\":\"%02X%02X\",\"d\":%u,\"st\":%u}\n",
                     (unsigned long)seq, (unsigned long)millis(),
                     twr[j].peer_addr[1], twr[j].peer_addr[0], (unsigned)twr[j].distance,
                     (unsigned)twr[j].status);
    // never block the UWB callback: drop (and count) if the queue is full
    if (n <= 0 || !UWBWiFiCoex.sendUdp(kServer, kPort, (const uint8_t*)line, (size_t)n)) {
      g_enqueueFail = g_enqueueFail + 1;
    }
  }
}

void setup() {
  Serial.begin(115200);
  for (uint32_t t0 = millis(); !Serial && millis() - t0 < 3000;) {
    delay(10);
  }
  // where did the previous run stop? (assert / HardFault / watchdog)
  UWBWiFiCoex.printBootReport(Serial);
  pinMode(LEDR, OUTPUT);
  digitalWrite(LEDR, LOW);

  // 1) Wi-Fi first, in the stock timer-IRQ mode
  Serial.print("Wi-Fi ...");
  while (WiFi.begin(SECRET_SSID, SECRET_PASS) != WL_CONNECTED) {
    Serial.print(".");
    delay(1000);
  }
  Serial.print(" connected, IP ");
  Serial.println(WiFi.localIP());

  // 2) switch lwIP to the net task and arm the SPI1 CS guard
  if (!UWBWiFiCoex.begin()) {
    Serial.println("UWBWiFiCoex.begin() failed (FreeRTOS heap?)");
    while (true) delay(1000);
  }

  // 3) UWB
  uint8_t devAddr[] = {0x11, 0x11};
  uint8_t dst1[] = {0x22, 0x22};
  uint8_t dst2[] = {0x33, 0x33};
  UWBMacAddress srcAddr(UWBMacAddress::Size::SHORT, devAddr);
  UWBMacAddressList dest(UWBMacAddress::Size::SHORT);
  UWBMacAddress dstAddr1(UWBMacAddress::Size::SHORT, dst1);
  UWBMacAddress dstAddr2(UWBMacAddress::Size::SHORT, dst2);
  dest.add(dstAddr1);
  dest.add(dstAddr2);

  UWB.registerRangingCallback(rangingHandler);
  UWB.begin();
  while (UWB.state() != 0) {
    delay(10);
  }

#if ANCHOR_ONE_TO_ONE
  // Order verified on hardware: local address to the constructor, then
  // re-register PEER_ADDRESS from a static copy (dangling-pointer workaround).
  UWBMacAddress peerArg(UWBMacAddress::Size::SHORT, dst1);
  static UWBMacAddress peerKeep(UWBMacAddress::Size::SHORT, dst1);
  static UWBRangingController session(0x11223344, srcAddr, peerArg);
  session.appParams.destinationMacAddr(peerKeep);
#else
  static UWBRangingOneToMany session(0x11223344, srcAddr, dest);
#endif
  UWBSessionManager.addSession(session);
  session.init();
  session.start();
  Serial.println("Ranging started");

  // From here a stuck loop() resets the board after 2 s, and an assert or a
  // HardFault resets it at once; the next boot prints where it happened.
  UWBWiFiCoex.startWatchdog(2000);
}

void loop() {
  // loop() shares priority 4 with the UWB main task: always block here.
  UWBWiFiCoex.loopTick();  // feeds the watchdog
  static uint32_t last = 0;
  if (millis() - last >= 5000) {
    last = millis();
    UWBWiFiCoexStats const s = UWBWiFiCoex.stats();
    char line[UWBCOEX_UDP_MAX_PAYLOAD];
    int n = snprintf(line, sizeof(line),
                     "{\"type\":\"s\",\"seq\":%lu,\"t\":%lu,\"cb\":%lu,\"acq\":%lu,\"defer\":%lu,"
                     "\"tmo\":%lu,\"drop\":%lu,\"heap\":%u}\n",
                     (unsigned long)g_seq, (unsigned long)millis(), (unsigned long)g_uwbCallbacks,
                     (unsigned long)s.espBusAcquired, (unsigned long)s.espBusDeferrals,
                     (unsigned long)s.espBusTimeouts, (unsigned long)g_enqueueFail,
                     (unsigned)s.freeHeapMin);
    if (n > 0) {
      UWBWiFiCoex.sendUdp(kServer, kPort, (const uint8_t*)line, (size_t)n);
    }
    // UWB health without a tag: ranging notifications per 5 s (~25 at 200 ms)
    Serial.print("[uwb] cb=");
    Serial.println(g_uwbCallbacks);
    UWBWiFiCoex.printStats(Serial);
    digitalWrite(LEDR, !digitalRead(LEDR));
  }
  delay(50);
}
