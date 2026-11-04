# 하드웨어 검증 절차

계획서의 관문 G1–G3과 성공 기준 4개를 이 결과물로 측정하는 순서다.
필요한 것: Portenta C33 + UWB Shield 2세트 이상, PC 1대(같은 Wi-Fi), 로직 애널라이저(G1).

## 준비

1. 공식 코어 "Arduino Renesas Portenta Boards" 1.6.0(또는 1.5.3)을 설치한다.
2. PortentaUWBShield 1.0.2와 ArduinoBLE를 라이브러리 매니저로 설치한다.
3. 각 스케치 폴더의 `arduino_secrets.h`에 SSID, 비밀번호, PC의 IP를 넣는다.
4. PC에서 수신기를 띄운다(파이썬 3, 표준 라이브러리만 사용).

```bash
python3 tools/udp_rx_stats.py --port 9000 --duration 600 --csv run.csv
```

태그 보드에는 `sketches/CoexTag`를 올린다(주소 0x2222). 공식 `UWB_RangingControlee` 예제는 쓰지 않는다.
PortentaUWBShield 1.0.2의 `UWBRangingController`·`UWBRangingControlee`는 상대 주소(PEER_ADDRESS)를 사라진 스택 메모리에서 읽는 버그가 있어,
세션 시작이 거부되거나(reason 0x8D, 0x35 등) 상대가 응답하지 않는다(status 0x21). `CoexTag`는 이 버그를 우회한다(`analysis/upstream_issue_peer_address.md`).

앵커 스케치(`CoexAnchorUdp`, `ReproIssue12`)는 기본이 1:1 모드(`ANCHOR_ONE_TO_ONE 1`, 상대 0x2222)다.
1:N 모드(`ANCHOR_ONE_TO_ONE 0`, 상대 0x2222·0x3333)는 아직 Controlee 태그와 측거가 되지 않는다(2026-11-03, 맥에서 확인).
측거 확인의 판정은 앵커 쪽 `seq` 증가(또는 `CoexTag`의 `ok=` 증가)로 한다.

## 멈춤 진단 (공존 라이브러리)

`CoexAnchorUdp`는 `setup()` 맨 앞에서 `UWBWiFiCoex.printBootReport(Serial)`를 부르고, `setup()` 끝에서 `startWatchdog(2000)`을 켠다.

- loop가 2초 넘게 멈추면 워치독이 보드를 리셋한다. assert나 HardFault는 그 자리에서 기록하고 바로 리셋한다. 물리 리셋은 필요 없다.
- 다음 부팅의 `[coex-boot]` 줄이 직전 실행의 마지막 체크포인트(락, 버스 획득·해제, net 태스크 단계, loop)와 assert의 file:line, 또는 HardFault의 PC/LR/CFSR을 보여 준다.
- PC/LR은 같은 빌드의 elf로 해석한다: `arm-none-eabi-addr2line -f -C -e <elf> <pc> <lr>`.
- 워치독 리셋 뒤 USB 시리얼이 다시 열거되므로, 캡처 도구는 포트가 다시 나타나면 다시 열어야 한다.

## G1 – 원인 확정 (원본 코어)

1. 패치하지 않은 코어로 `sketches/ReproIssue12`를 앵커 보드에 올린다.
2. 수신기를 켜고 시리얼 모니터에서 `sent=`가 멈추는 시점을 기록한다.
   이슈 #12와 같다면 측거가 시작된 뒤 `wifi=3`(연결됨)인데 `sent`가 늘지 않는다.
3. 로직 애널라이저를 연결한다. 접근 가능한 핀만 연결해도 된다.

| 채널 | 핀 | 의미 |
| --- | --- | --- |
| 0 | P102 (SCK1) | SPI1 클럭 |
| 1 | P101 (MOSI1) | |
| 2 | P100 (MISO1) | |
| 3 | P103 / D48 | UWB 실드 CS (바이너리에서 확인) |
| 4 | P104 | ESP32 CS |
| 5 | P806 | ESP32 HANDSHAKE |
| 6 | P803 | ESP32 DATA_READY |

4. 판정: D48이 LOW인 구간 안에서 P104가 LOW로 내려가거나, SCK 클럭 속도가 바뀌면 원인 확정이다.
   ESP 전송은 약 1 ms 동안 클럭을 연속으로 내보내므로 구분하기 쉽다.

## G2 – 효과 확인 (패치 코어)

1. 코어에 패치를 적용한다. 되돌릴 때는 `--revert`를 붙인다.

```bash
./tools/apply_core_patch.sh
```

2. `libraries/PortentaUWBWiFiCoex` 폴더를 스케치북의 `libraries/`에 복사한다.
3. 예제 `CoexAnchorUdp`를 앵커 보드에 올리고 수신기로 10분 측정한다.
4. 판정: 수신기 마지막 줄의 `stall`이 PASS면 1번 기준(10분 중단 없음) 통과다.
   G1 결과보다 중단 횟수가 줄었는지도 함께 기록한다.
5. 같은 캡처를 다시 떠서, D48이 LOW인 구간에 P104가 LOW로 겹치지 않는지 확인한다.

시리얼에 5초마다 나오는 `[coex]` 줄의 의미:

| 항목 | 정상 범위(예상) | 벗어나면 |
| --- | --- | --- |
| `defer` | `acq`보다 훨씬 작음 | 실드 CS가 자주 LOW다. `wait_max_ms`도 같이 본다 |
| `timeout` | 0 | 0보다 크면 ESP 프레임이 버려졌다. 리스크 표 1번 행(CS 장시간 점유) |
| `hold_max_us` | 약 1,000 이하 | ESP 1프레임(1600 B) 동안 스케줄러가 멈춘 시간. 크면 UWB 지연 원인 |
| `drop` | 0 | UDP 큐(8개)가 찼다. 송신 속도가 측거 속도를 못 따라간다 |
| `heap_min` | 수 KB 이상 | 0에 가까우면 FreeRTOS 힙 부족 |
| `loop_max_us` | 수십 ms 이하 | net 태스크 한 바퀴가 길다. ESP 응답 지연 또는 CS 대기 |

## G3 – 최종 판정

| # | 기준 | 측정 방법 | 합격선(계획서 제안값) |
| --- | --- | --- | --- |
| 1 | 동시 동작 | `udp_rx_stats.py --duration 600` 의 `stall` | 10분 동안 2초 넘는 공백 0회 |
| 2 | 송신 신뢰성 | 같은 실행의 `delivery` | 99% 이상 |
| 3 | 장시간 안정성 | `--duration 1800`, 수신기 `restarts=0`, 시리얼 끊김 없음 | 30분 행업·리셋 0회 |
| 4 | 측거 성능 | `run.csv`의 측거 간격(t_ms 차이) 중앙값을 Wi-Fi 끈 상태와 비교 | 저하 10% 이내 |

4번 기준용 Wi-Fi 끈 기준선: `CoexAnchorUdp`에서 `WiFi.begin` 루프와 `UWBWiFiCoex.begin()`을 주석 처리하고,
`rangingHandler` 안에서 `Serial.println(line)`으로 같은 JSON을 출력해 시리얼로 수집한다.

`delivery`는 앵커가 매긴 일련번호(`seq`) 범위 대비 수신 개수다.
측거 결과 자체가 안 나온 구간은 `seq`가 늘지 않으므로 손실로 잡히지 않는다. 그 구간은 `stall`로 잡힌다.

## 수신 방향 (PC → 보드)

G1–G3은 보드가 보내기만 한다. 수신은 ESP32-C3가 먼저 "받은 데이터 있음"을 알리는 경로라 송신과 충돌 조건이 다르므로 따로 본다.
보드 1대로 된다. 태그가 없어도 앵커는 200 ms마다 측거를 시도하고 SR150이 결과를 SPI1로 보고하므로, UWB 쪽 SPI 트래픽은 그대로 생긴다.

1. `CoexAnchorUdp.ino` 맨 위에 `#define COEX_RX_ECHO 1`을 넣고 빌드해 앵커 보드에 올린다.
   보드는 UDP 9001번 포트에서 받은 데이터그램을 보낸 쪽으로 그대로 돌려보낸다. 측거와 PC로 가는 기존 UDP 스트림은 그대로 돈다.
2. 시리얼의 `COEX_RX_ECHO: echo on <IP>:9001`에서 보드 IP를 확인한다.
3. PC에서 두 도구를 함께 띄운다.

```bash
python3 tools/udp_rx_stats.py --port 9000 --duration 300 --wait-s 90
```

```bash
python3 tools/udp_echo_test.py --board <보드 IP> --port 9001 --rate 10 --duration 300
```

판정:

| 항목 | 어디서 | 합격선 |
| --- | --- | --- |
| 수신 손실 | `udp_echo_test.py`의 `loss` | 1% 이하 |
| 수신 공백 | 같은 도구의 `stall` | 2초 넘는 공백 0회 |
| 보드 쪽 수신 큐 | 시리얼 `[coex] ... | rx=.. rxdrop=..` | `rxdrop` 0 |
| 기존 송신 | `udp_rx_stats.py` | G2와 같은 기준(stall 0, delivery 99% 이상) |
| 버스 | 시리얼 `timeout` | 0 |

- 부하를 더 주려면 `--rate 20` 또는 `--rate 50`으로 올린다. 보드의 수신 큐는 8칸이고 loop가 최대 50 ms마다 비우므로, 50/s 근처부터 `rxdrop`이 생기는 것은 큐 크기 한계다.
- `COEX_SEND_ALL 1`을 같이 켜면 실패한 측거 결과까지 초당 약 5개가 PC로 나가, 송신·수신·UWB가 동시에 버스를 쓴다.
- `NO ECHO`가 나오면 순서대로 확인한다: 보드 IP·포트, `COEX_RX_ECHO 1` 빌드인지, PC 방화벽(파이썬의 UDP 수신 허용), 공유기의 무선 단말 간 차단.
- 도구 확인: `python3 tools/udp_echo_test.py --selftest` (5% 손실과 2.5초 공백 1회를 검출해야 한다).

라이브러리 쪽 구현: lwIP 래퍼는 소켓마다 받은 데이터그램을 한 칸에만 보관하고 다음 것이 오면 버린다.
`udpRxQueueDepth > 0`이면 래퍼의 수신 콜백(`onDataArrival`)에서 바로 큐로 복사하고 칸을 비우므로, 한 번의 `lwip_task()`에서 여러 개가 들어와도 잃지 않는다.

## TCP (HTTP POST)

보드 1대로 한다. 앵커가 UWB 측거와 UDP 송신을 하는 동안, 별도 태스크(우선순위 1)가 1초마다 PC로 HTTP POST를 한 번씩 보낸다.
요청마다 TCP 연결을 새로 열고, 응답을 끝까지 읽은 뒤 닫는다.

1. 빌드 옵션 `COEX_HTTP_TEST=1`. 태그 없이 할 때는 `COEX_SEND_ALL=1`을 함께 켜서 UDP 송신이 흐르게 한다.
   arduino-cli라면 `--build-property "compiler.cpp.extra_flags=-DCOEX_HTTP_TEST=1 -DCOEX_SEND_ALL=1"`를 붙인다.
2. PC에서 두 도구를 함께 띄운다(`SERVER_IP`가 이 PC여야 한다).

```bash
python3 tools/http_sink.py --port 8080 --duration 300 --csv http.csv
```

```bash
python3 tools/udp_rx_stats.py --port 9000 --duration 300 --wait-s 90
```

판정:

| 항목 | 어디서 | 합격선 |
| --- | --- | --- |
| HTTP 전달 | `http_sink.py`의 `delivery` | 99% 이상 |
| HTTP 공백 | 같은 도구의 `stall` | 3초 넘는 공백 0회 |
| 보드 쪽 실패 | 시리얼 `[http] conn_fail= write_fail= resp_fail=` | 모두 0 |
| 응답 시간 | 시리얼 `[http] max_ms=` | 참고값 (연결부터 응답 수신까지) |
| 기존 송신 | `udp_rx_stats.py` | stall 0, delivery 99% 이상 |
| 메모리 | 시리얼 `[coex] heap_min=` | 시간이 지나도 줄지 않음 |

- 원본 lwIP 래퍼는 `WiFiClient` 객체마다 연결 상태 메모리를 할당하고 해제하지 않으므로, 예제는 객체 하나를 재사용한다.
- 응답을 다 읽지 않고 `stop()`하면 받은 버퍼가 남을 수 있으므로, 서버가 닫을 때까지 읽는다.
- `NOTHING RECEIVED`가 나오면 `SERVER_IP`, 빌드 옵션, PC 방화벽(파이썬의 TCP 8080 수신 허용) 순서로 확인한다.
- 도구 확인: `python3 tools/http_sink.py --selftest` (2개 누락과 3.5초 공백을 검출해야 한다).

## 비정상 시나리오 (5단계)

- Wi-Fi AP 재부팅: `[coex]` 줄이 계속 나오는지, AP 복귀 후 `sent`가 다시 늘어나는지 본다.
  예제는 재연결 코드를 넣지 않았다. 필요하면 `loop()`에서 `WiFi.status()`를 보고 `WiFi.begin()`을 다시 부른다.
  패치로 DHCP·제어 응답 대기가 `vTaskDelay`로 양보하므로, 재연결 중에도 측거는 계속되어야 한다.
- 태그 1대 전원 차단: 남은 태그의 결과가 계속 송신되는지 본다.
