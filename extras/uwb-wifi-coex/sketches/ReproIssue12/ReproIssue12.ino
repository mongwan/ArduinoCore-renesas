/*
 * ReproIssue12 - baseline for Truesense-it/PortentaUWBShield issue #12
 *
 * Builds on the STOCK core. Same traffic as CoexAnchorUdp, but the way the
 * issue author did it: the ranging callback only buffers, loop() sends with
 * WiFiUDP. Expected on the stock core: UDP stops shortly after ranging starts
 * while WiFi.status() still says connected.
 *
 * Use it for gate G1 (logic analyzer: D48/P103 = shield CS, P104 = ESP CS,
 * P100-P102 = SPI1, P806 = ESP HANDSHAKE).
 */
#include <PortentaUWBShield.h>
#include <WiFiC3.h>
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

static const IPAddress kServer(SERVER_IP);
static const uint16_t kPort = SERVER_PORT;
static WiFiUDP udp;

struct Result { uint32_t seq, t; uint8_t a0, a1; uint16_t d; };
static Result ring[32];
static volatile uint32_t head = 0, tail = 0, g_seq = 0, g_overflow = 0;

static void rangingHandler(UWBRangingData& rangingData) {
  if (rangingData.measureType() != (uint8_t)uwb::MeasurementType::TWO_WAY) return;
  RangingMeasures twr = rangingData.twoWayRangingMeasure();
  for (int j = 0; j < rangingData.available(); j++) {
    if (twr[j].status != 0 || twr[j].distance == 0xFFFF) continue;
    uint32_t const seq = g_seq; g_seq = seq + 1;
    if (head - tail >= 32) { g_overflow = g_overflow + 1; continue; }
    ring[head % 32] = {seq, millis(), twr[j].peer_addr[1], twr[j].peer_addr[0], twr[j].distance};
    head = head + 1;
  }
}

void setup() {
  Serial.begin(115200);
  for (uint32_t t0 = millis(); !Serial && millis() - t0 < 3000;) delay(10);
  while (WiFi.begin(SECRET_SSID, SECRET_PASS) != WL_CONNECTED) delay(1000);
  Serial.print("IP "); Serial.println(WiFi.localIP());
  udp.begin(0);

  uint8_t devAddr[] = {0x11, 0x11}, dst1[] = {0x22, 0x22}, dst2[] = {0x33, 0x33};
  UWBMacAddress srcAddr(UWBMacAddress::Size::SHORT, devAddr);
  UWBMacAddressList dest(UWBMacAddress::Size::SHORT);
  UWBMacAddress dstAddr1(UWBMacAddress::Size::SHORT, dst1);
  UWBMacAddress dstAddr2(UWBMacAddress::Size::SHORT, dst2);
  dest.add(dstAddr1);
  dest.add(dstAddr2);
  UWB.registerRangingCallback(rangingHandler);
  UWB.begin();
  while (UWB.state() != 0) delay(10);
#if ANCHOR_ONE_TO_ONE
  static UWBMacAddress peerKeep(UWBMacAddress::Size::SHORT, dst1);
  static UWBRangingController session(0x11223344, srcAddr, peerKeep);
  session.appParams.destinationMacAddr(peerKeep);  // dangling-pointer workaround
#else
  static UWBRangingOneToMany session(0x11223344, srcAddr, dest);
#endif
  UWBSessionManager.addSession(session);
  session.init();
  session.start();
}

void loop() {
  static uint32_t sent = 0, fail = 0, last = 0;
  while (tail != head) {
    Result const r = ring[tail % 32];
    tail = tail + 1;
    char line[128];
    int n = snprintf(line, sizeof(line),
                     "{\"type\":\"r\",\"seq\":%lu,\"t\":%lu,\"mac\":\"%02X%02X\",\"d\":%u}\n",
                     (unsigned long)r.seq, (unsigned long)r.t, r.a0, r.a1, (unsigned)r.d);
    bool ok = udp.beginPacket(kServer, kPort) == 1;
    ok = ok && udp.write((const uint8_t*)line, n) == (size_t)n;
    ok = ok && udp.endPacket() == 1;
    ok ? sent++ : fail++;
  }
  if (millis() - last >= 5000) {
    last = millis();
    Serial.print("seq="); Serial.print(g_seq);
    Serial.print(" sent="); Serial.print(sent);
    Serial.print(" fail="); Serial.print(fail);
    Serial.print(" overflow="); Serial.print(g_overflow);
    Serial.print(" wifi="); Serial.println(WiFi.status());
  }
  delay(10);
}
