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

태그 보드 2대에는 PortentaUWBShield의 `UWB_RangingControlee` 예제를 올린다.
주소를 `devAddr = {0x22,0x22}`, `{0x33,0x33}`으로 한 대씩 바꾸고, `destination`은 둘 다 `{0x11,0x11}`이다.

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

## 비정상 시나리오 (5단계)

- Wi-Fi AP 재부팅: `[coex]` 줄이 계속 나오는지, AP 복귀 후 `sent`가 다시 늘어나는지 본다.
  예제는 재연결 코드를 넣지 않았다. 필요하면 `loop()`에서 `WiFi.status()`를 보고 `WiFi.begin()`을 다시 부른다.
  패치로 DHCP·제어 응답 대기가 `vTaskDelay`로 양보하므로, 재연결 중에도 측거는 계속되어야 한다.
- 태그 1대 전원 차단: 남은 태그의 결과가 계속 송신되는지 본다.
