# Portenta C33 UWB + Wi-Fi 공존 패치

Portenta C33에서 UWB Shield 측거와 Wi-Fi UDP 송신을 동시에 돌리기 위한 코어 패치와 공존 라이브러리다.
[이슈 #12](https://github.com/Truesense-it/PortentaUWBShield/issues/12)의 원인 두 가지(SPI1 버스 충돌, lwIP 스레드 안전성)를 코어 쪽에서 막는다.

**상태 (2026-11-04)**: 하드웨어에서 G2(650초, UDP 전달 100%)와 Wi-Fi 끈 기준선 비교(성공 기준 4) 통과, G3(1800초) 진행 중. 절차는 `HARDWARE_TEST.md`.

## 구성

```
patches/0001~0004-...patch            코어 패치: 0001 SPI1 공유·lwIP 잠금, 0002·0003 UDP 버그, 0004 TCP·DNS 대기 중 잠금 해제 (1.5.3, 1.6.0에 그대로 적용됨)
tools/apply_core_patch.sh             설치된 코어에 패치 적용/되돌리기 (백업 생성)
libraries/PortentaUWBWiFiCoex/        공존 라이브러리 + 예제 CoexAnchorUdp
sketches/ReproIssue12/                원본 코어용 재현 스케치 (G1)
tools/udp_rx_stats.py                 PC 수신기: 손실률·공백(stall) 측정, 성공 기준 판정
tools/udp_echo_test.py                PC → 보드 수신 시험: COEX_RX_ECHO 빌드에 보내고 되돌아온 것으로 손실·RTT·공백 측정
tools/http_sink.py                    TCP 시험 수신기: COEX_HTTP_TEST 빌드의 HTTP POST 전달률·공백 측정
tools/flash.sh                        IDE 없이 업로드(보드 패키지의 dfu-util 사용, 여러 대면 USB 시리얼로 선택, --list)
tools/capture.py                      시리얼 기록기: 시각 표시, 리셋·업로드로 포트가 사라지면 다시 열기 (pyserial 필요)
tools/build_c33.py                    arduino-cli 없이 C33 빌드 (이 결과물 검증에 사용)
analysis/uwb_binary_findings.md       UWB 바이너리 디스어셈블 결과 (CS 핀, 태스크 우선순위 등)
HARDWARE_TEST.md                      G1–G3 하드웨어 검증 절차
```

## 동작 방식

| 문제 | 원래 동작 | 패치 후 |
| --- | --- | --- |
| SPI1 소유자가 둘 | ESPhost가 같은 채널을 FSP로 따로 열고 IRQ도 따로 등록 | ESPhost도 `SPI1` 객체를 쓴다. 전송 뒤 직전 `SPISettings`를 복원 |
| ESP 전송이 인터럽트에서 실행 | 100 Hz 타이머 ISR이 `lwip_task()`와 ESP SPI 전송을 실행 | `UWBWiFiCoex.begin()`이 타이머를 끄고 net 태스크(우선순위 1, 10 ms)가 대신 실행 |
| UWB 전송 중 ESP 개입 | 중재 없음 | ESP 전송 직전 스케줄러를 멈추고 실드 CS(D48)가 HIGH일 때만 전송. LOW면 1 ms씩 양보하며 최대 500 ms 대기 |
| lwIP 동시 접근 | 락 없음 (`NO_SYS 1`) | 재귀 락이 `lwip_task()`, UDP/TCP 래퍼, DNS, 동기 요청을 감싼다 |
| 바쁜 대기로 UWB 정지 | ESP 제어 응답(최대 50 s), DHCP, DNS 대기가 호출자 우선순위에서 회전 | 공존 모드에서는 `vTaskDelay(1)`로 양보 |
| 힙 부족 | FreeRTOS 힙 32 KB, UWB 스택만 약 16 KB | C33 힙 48 KB |

코어 쪽 변경은 모두 약한(weak) 훅이라 공존 라이브러리를 쓰지 않으면 원래 동작과 같다.
예외는 ESPhost가 `SPI1` 객체를 거친다는 점 하나다. 클럭(24 MHz 요청)과 모드는 원래와 같다.

## 빠른 시작

```bash
./tools/apply_core_patch.sh
```

1. 위 명령으로 설치된 "Arduino Renesas Portenta Boards" 코어에 패치를 적용한다(IDE를 닫고 실행).
2. `libraries/PortentaUWBWiFiCoex`를 스케치북의 `libraries/`에 복사한다.
3. 예제 `CoexAnchorUdp`의 `arduino_secrets.h`를 채우고 앵커 보드에 올린다.
4. PC에서 수신기를 실행한다.

```bash
python3 tools/udp_rx_stats.py --port 9000 --duration 600
```

자기 스케치에 넣을 때의 순서:

```cpp
#include <PortentaUWBShield.h>
#include <WiFiC3.h>
#include <PortentaUWBWiFiCoex.h>

void setup() {
  WiFi.begin(SSID, PASS);      // 1. Wi-Fi 연결 (원래 방식)
  UWBWiFiCoex.begin();         // 2. 공존 모드 시작
  UWB.begin();                 // 3. UWB 시작
}
// 측거 콜백에서는 UWBWiFiCoex.sendUdp(ip, port, buf, len) 로 큐에 넣기만 한다.
```

## 사용 규칙

- `loop()`는 반드시 블로킹해야 한다(`delay()` 등). `loop()`는 UWB 메인 태스크와 같은 우선순위 4이고 타임 슬라이싱이 꺼져 있다.
- `UWBWiFiCoex.begin()`은 `WiFi.begin()` 뒤, `UWB.begin()` 앞에서 부른다.
- 측거 콜백 안에서는 `sendUdp()`만 쓴다. 큐가 차면 `false`를 돌려주고 기다리지 않는다.
- 다른 태스크에서 `WiFiUDP`를 직접 써도 된다. 여러 호출을 묶어야 하면 `UWBWiFiCoexLock`으로 감싼다. ISR에서는 쓰지 않는다.
- ESP 전송이 실드 CS를 500 ms 넘게 기다리면 그 프레임은 버려진다(원래 HANDSHAKE 타임아웃과 같은 동작). `printStats()`의 `timeout`으로 확인한다.

## 이 환경에서 검증한 것

ARM GCC 13.2(Ubuntu 패키지)로 빌드했다. 공식 IDE는 GCC 7-2017q4를 쓰므로, 아래 결과는 컴파일·링크 검증이지 IDE가 만드는 바이너리 그 자체는 아니다.

| 항목 | 결과 |
| --- | --- |
| 패치 적용 | 1.5.3, 1.6.0 릴리스 트리에 충돌 없음. 적용 → 중복 적용 감지 → 되돌리기 후 원본과 동일 |
| `CoexAnchorUdp` (패치 코어 1.6.0) | 빌드·링크 성공. text 528 KB, bss 133 KB |
| `ReproIssue12` (원본 코어 / 패치 코어) | 둘 다 빌드 성공 |
| 기존 Wi-Fi 예제 3개 (패치 코어, 공존 라이브러리 없음) | `WiFiUdpSendReceiveString`, `WiFiWebClient`, `WiFiPing` 빌드 성공 |
| 경고 | 수정한 파일 전부 `-Wall -Wextra`에서 경고 0 |
| 훅 연결 | ELF 맵에서 약한 훅 7개가 모두 공존 라이브러리 구현으로 대체됨 확인 |
| ESP 드라이버 | 최종 코드에서 `R_SPI_Open`·IRQ 등록 참조 없음. 호출 순서 HANDSHAKE → acquire → beginTransaction → transfer → 설정 복원 → release 확인 |
| 수신기 | `udp_rx_stats.py --selftest` 통과 (손실 3개, 2.5 s 공백 1회를 정확히 검출) |

## 검증 상태 (2026-11-04, 사무실 PC, 공식 Arduino 툴체인 빌드)

하드웨어에서 확인한 것:
- G2(650초)·G3(1800초): UDP 전달률 99.99% 이상, SPI 버스 timeout 0, 리셋 0. Wi-Fi 끈 기준선 대비 측거 성능 저하 없음.
- 원인 단독 증명(`SpiClobberRepro`): 빌드 옵션 `ESPHOST_USE_ARDUINO_SPI` 하나만 바꿔 실패와 정상이 갈린다.
- 수신(PC→보드 에코, 초당 50개까지), TCP(HTTP POST), TCP 서버 장애(패치 0004)에서 손실·드롭 0.
- 1:N(`UWBRangingOneToMany`)은 태그를 `TAG_MULTICAST 1`로 빌드하면 동작한다. 측거 실패 코드 `0x1B`는 보드가 5 cm 이내일 때 나오는 음수 거리 보고다.

아직 확인하지 않은 것:
- 태그 여러 대(실제 1:N)와 실제 사용 세션 설정(채널, 주기, STS)으로의 장시간 시험.
- 앵커가 BLE까지 함께 쓰는 경우(UWB + Wi-Fi + BLE).
- Arduino Stella를 태그로 쓰는 경우. 앵커 쪽 수정과는 무관하지만, 태그 쪽 설정(멀티캐스트 등)은 StellaUWB 라이브러리에서 따로 확인해야 한다.

## 계획서 대비 바뀐 점

- 실드 CS 핀은 캡처 없이 바이너리에서 D48(P103)로 확정했다. 실드 IRQ는 SPI를 쓰지 않는다.
- net 태스크 우선순위를 1로 정했다(UWB 보조 태스크 2, UWB 메인·loop 4).
- 계획에 없던 수정 4가지:
  - `beginTransaction`을 원자화했다.
  - FreeRTOS 힙을 48 KB로 늘렸다.
  - ESP 제어 응답 대기에 양보 훅을 넣었다.
  - DHCP·DNS·TCP 대기 루프에 양보 훅을 넣었다.
