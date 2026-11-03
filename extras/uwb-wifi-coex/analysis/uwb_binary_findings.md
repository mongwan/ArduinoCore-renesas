# libUWBShieldApi.a 분석 결과

대상: PortentaUWBShield v1.0.2, `src/cortex-m33/fpv5-sp-d16-hard/libUWBShieldApi.a`
방법: `arm-none-eabi-objdump -d -C -r` (GNU binutils, Ubuntu 24.04)

계획서에서 "확인할 것"으로 남겨 둔 항목 중 하드웨어 없이 답이 나온 것들이다.

| 질문 | 답 | 근거 (오브젝트 / 함수) |
| --- | --- | --- |
| 실드 CS는 어느 핀인가 | **D48 = P103** | `uwb_bus_io.cpp.o`·`uwb_bus_interface.cpp.o`: `uwb_bus_data_tx`/`uwb_bus_data_rx`가 `digitalWrite(0x30, LOW/HIGH)`로 전송을 감싼다. `variant.cpp`에서 D48 = P103(`CS1`) |
| 실드 SPI 설정 | 4 MHz, SPI_MODE0, MSB first | 두 함수 모두 스택에 `SPISettings{0x003d0900, mode=0, order=1}`를 만들고 `SPI1.beginTransaction()` 호출 |
| 전송마다 `beginTransaction`을 부르는가 | 예 | tx·rx 모두 `beginTransaction` → `digitalWrite(48, LOW)` → `transfer` → `digitalWrite(48, HIGH)` 순서. `endTransaction`은 호출 없음(빈 inline) |
| 실드 IRQ 핸들러에서 SPI를 쓰는가 | 아니오 | `attachInterrupt(digitalPinToInterrupt(28), irq_handler, RISING)`. 핸들러 → `uwb_bus_io_irq_cb` → `xQueueGiveFromISR`만 호출 |
| `SPI1.end()`는 언제 부르는가 | `uwb_bus_deinit` (UWB 스택 종료 시) | `uwb_bus_interface.cpp.o`. `SPI1.begin()`은 `uwb_bus_init`에서 호출 |
| 스케줄러는 누가 시작하는가 | Arduino_FreeRTOS의 early start | `UWB.cpp`가 `EARLY_AUTOSTART_FREERTOS`를 켠다. `setup()`/`loop()`는 "Sketch Thread"(우선순위 4, 스택 4 KB)에서 실행. 바이너리 안의 `phOsalUwb_TaskStartScheduler`는 호출되지 않음 |
| 그 밖의 GPIO | 27, 29, 30, 31 출력 / 28 입력(IRQ) | `uwb_bus_io_init` |

## UWB 스택 태스크

`phOsalUwb_Thread_Create`는 설정 구조체의 우선순위 값에서 1을 빼서 `xTaskCreate`에 넘긴다.
`configMAX_PRIORITIES`는 5(0–4)이고 `configUSE_TIME_SLICING`은 0이다.

| 태스크 (생성 함수) | 우선순위 | 스택 |
| --- | --- | --- |
| UWB 메인 (`StartUwbTask`) | 4 | 1500 words (6000 B) |
| UCI HAL 클라이언트 (`phNxpUciHal_open`) | 2 | 1024 words (4096 B) |
| TML 리더 (`phTmlUwb_StartThread`) | 2 | 1024 words (4096 B) |
| 복구 (`Start_AppRecoveryTask`) | 2 | 300 words (1200 B) |
| Sketch Thread (`setup`/`loop`) | 4 | 1024 words (4096 B) |

UWB 스택만으로 FreeRTOS 힙 약 16 KB를 스택에 쓴다. 원래 힙은 32 KB(`0x8000`)여서
공존 계층(net 태스크 4 KB + UDP 큐 1.1 KB)을 더하면 여유가 거의 없다.
UWB 쪽 태스크 생성이 실패하면 `phOsalUwb_Thread_Create`가 `__assert_func`로 멈추므로
코어 패치에서 C33 힙을 48 KB(`0xC000`)로 늘렸다.

## 설계에 준 영향

- 실드 CS를 D48로 확정했으므로 CS 가드는 `digitalRead(48)`만 보면 된다.
- 실드 IRQ가 SPI를 쓰지 않으므로 ESP 전송 중 막아야 할 인터럽트는 없다.
  `vTaskSuspendAll()`로 태스크 전환만 막으면 충분하다.
- UWB 태스크가 `beginTransaction()`과 `digitalWrite(48, LOW)` 사이에서 선점될 수 있다.
  이 경우 레지스터 설정이 이미 UWB용으로 바뀌어 있으므로, ESP 전송 뒤 직전 `SPISettings`를
  복원해야 한다. 코어 패치는 `SPI1.getSettings()`로 저장했다가 다시 적용한다.
- `beginTransaction()`의 레지스터 쓰기와 `_settings` 갱신 사이에서 선점되면 저장값이 어긋난다.
  코어 패치는 이 구간을 `__disable_irq()`로 원자화했다(수 µs).
- `loop()`와 UWB 메인 태스크가 같은 우선순위 4이고 타임 슬라이싱이 꺼져 있다.
  `loop()`가 블로킹 없이 돌면 UWB 메인 태스크와 그 아래 태스크가 실행되지 않는다.
