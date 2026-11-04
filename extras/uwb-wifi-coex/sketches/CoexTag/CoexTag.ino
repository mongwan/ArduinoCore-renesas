/*
 * CoexTag - UWB tag (Controlee) for CoexAnchorUdp / ReproIssue12
 *
 * Same session as the official UWB_RangingControlee example, plus a
 * workaround for a PortentaUWBShield 1.0.2 bug: UWBRangingControlee stores a
 * pointer to the dstAddr copy its constructor received, so PEER_ADDRESS is
 * read from a dead stack frame at init(). Depending on the build this makes
 * the session start fail (reason 0x8D, 0x35, ...) or the peer never answer
 * (status 0x21). The address is re-registered from a static object.
 *
 * TAG_ADDR: 0x2222 for the first tag, 0x3333 for a second one (1:N mode).
 * TAG_MULTICAST = 1: join a one-to-many session (anchor built with
 *   ANCHOR_ONE_TO_ONE=0). UWBRangingOneToMany sets MULTI_NODE_MODE to
 *   multicast on the controller, while UWBRangingControlee always uses
 *   unicast; with that mismatch every measurement times out (0x21).
 *
 * Status codes (from the UCI definitions in libUWBShieldApi.a):
 *   0x00 OK, 0x1B OK but negative distance (devices closer than the antenna
 *   delay calibration, typically a few cm apart), 0x21 RX timeout (no
 *   answer), 0x22-0x27 RX PHY/MAC decode failures.
 * Prints one summary line every 5 s:
 *   tag cb=<callbacks> ok=<valid distances> last_d=<cm> status[...]
 */
#include <PortentaUWBShield.h>

#ifndef TAG_ADDR
#define TAG_ADDR 0x2222
#endif
#ifndef TAG_MULTICAST
#define TAG_MULTICAST 0
#endif

static volatile uint32_t g_cb = 0, g_ok = 0, g_lastD = 0;
static volatile uint32_t g_status[8] = {0};  // 0x00, 0x21, 0x26, 0x81, 0x82, 0x87, other
static volatile uint32_t g_otherCount[256] = {0};  // per-code count of "other"
static volatile uint8_t g_lastOther = 0;

static int statusSlot(uint8_t st) {
  switch (st) {
    case 0x00: return 0;
    case 0x21: return 1;
    case 0x26: return 2;
    case 0x81: return 3;
    case 0x82: return 4;
    case 0x87: return 5;
    default: return 6;
  }
}

static void rangingHandler(UWBRangingData& rangingData) {
  g_cb = g_cb + 1;
  if (rangingData.measureType() != (uint8_t)uwb::MeasurementType::TWO_WAY) return;
  RangingMeasures twr = rangingData.twoWayRangingMeasure();
  for (int j = 0; j < rangingData.available(); j++) {
    int const k = statusSlot(twr[j].status);
    g_status[k] = g_status[k] + 1;
    if (k == 6) {
      g_lastOther = twr[j].status;
      g_otherCount[twr[j].status] = g_otherCount[twr[j].status] + 1;
    }
    if (twr[j].status == 0 && twr[j].distance != 0xFFFF) {
      g_ok = g_ok + 1;
      g_lastD = twr[j].distance;
    }
  }
}

void setup() {
  Serial.begin(115200);
  for (uint32_t t0 = millis(); !Serial && millis() - t0 < 3000;) delay(10);
  pinMode(LEDR, OUTPUT);
  digitalWrite(LEDR, LOW);

  uint8_t devAddr[] = {(uint8_t)(TAG_ADDR >> 8), (uint8_t)(TAG_ADDR & 0xFF)};
  uint8_t anchorAddr[] = {0x11, 0x11};
  UWBMacAddress srcAddr(UWBMacAddress::Size::SHORT, devAddr);

  UWB.registerRangingCallback(rangingHandler);
  UWB.begin();
  while (UWB.state() != 0) delay(10);

  // Order matters (verified on hardware): give the constructor a local
  // address, then re-register PEER_ADDRESS from a static copy. Passing the
  // static object to the constructor made the session start fail on a Mac
  // build ("could not set app params: 2", reason 0x35); cause not known yet.
  UWBMacAddress dstAddr(UWBMacAddress::Size::SHORT, anchorAddr);
  static UWBMacAddress peerKeep(UWBMacAddress::Size::SHORT, anchorAddr);
  static UWBRangingControlee session(0x11223344, srcAddr, dstAddr);
  session.appParams.destinationMacAddr(peerKeep);  // dangling-pointer workaround
#if TAG_MULTICAST
  session.rangingParams.multiNodeMode(uwb::MultiNodeMode::MULTICAST);
#endif
  UWBSessionManager.addSession(session);
  session.init();
  session.start();
  Serial.print("tag 0x");
  Serial.print(TAG_ADDR, HEX);
  Serial.println(TAG_MULTICAST ? " started (multicast)" : " started (unicast)");
}

void loop() {
  static uint32_t last = 0;
  if (millis() - last >= 5000) {
    last = millis();
    Serial.print("tag cb=");     Serial.print(g_cb);
    Serial.print(" ok=");        Serial.print(g_ok);
    Serial.print(" last_d=");    Serial.print(g_lastD);
    Serial.print(" status[00/21/26/81/82/87/other]=");
    for (int i = 0; i < 7; i++) {
      Serial.print(g_status[i]);
      Serial.print(i < 6 ? "/" : "");
    }
    if (g_status[6] > 0) {
      // which "other" codes: last one and the most frequent one
      uint8_t top = 0;
      for (int c = 1; c < 256; c++) {
        if (g_otherCount[c] > g_otherCount[top]) top = (uint8_t)c;
      }
      Serial.print(" other_last=0x");
      Serial.print(g_lastOther, HEX);
      Serial.print(" other_top=0x");
      Serial.print(top, HEX);
      Serial.print("x");
      Serial.print(g_otherCount[top]);
    }
    Serial.println();
    digitalWrite(LEDR, !digitalRead(LEDR));
  }
  delay(50);
}
