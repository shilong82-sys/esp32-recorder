# Architecture — ESP32 AI Recorder

> Version: v0.3 | Updated: 2026-05-14

---

## 1. System Overview

```
┌─────────────────────────────────────────────────────────────────────┐
│                         ESP32-S3 (Firmware)                         │
│                                                                     │
│  ┌──────────────────────────────────────────────────────────────┐  │
│  │                        app_main.c                            │  │
│  │  Boot sequence, module init, event subscriptions, main loop  │  │
│  └───────────────────────────┬──────────────────────────────────┘  │
│                               │                                     │
│  ┌──────────────────────────────────────────────────────────────┐  │
│  │                    Architecture Layer                        │  │
│  │   event_bus  ←──── state ────→  system_monitor              │  │
│  │   (pub/sub)        (FSM)        (task watchdog)              │  │
│  └───────────────────────────┬──────────────────────────────────┘  │
│                               │ events                              │
│  ┌──────────────────────────────────────────────────────────────┐  │
│  │                        UI Layer                              │  │
│  │   ui  (subscribes to state/button events, drives LED)       │  │
│  └───────────────────────────┬──────────────────────────────────┘  │
│                               │                                     │
│  ┌──────────────────────────────────────────────────────────────┐  │
│  │                  Hardware Abstraction Layer                   │  │
│  │   audio    led    button    storage    battery               │  │
│  │   (I2S)   (RMT)  (GPIO)    (SDSPI)    (ADC)                 │  │
│  └───────────────────────────┬──────────────────────────────────┘  │
│                               │                                     │
│  ┌──────────────────────────────────────────────────────────────┐  │
│  │                   Business Logic Layer                       │  │
│  │   recorder    uploader    wifi_manager                       │  │
│  │   (WAV file)  (HTTP POST) (STA mode)                        │  │
│  └──────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────┘
                         │ WiFi HTTP POST
                         ▼
┌─────────────────────────────────────────────────────────────────────┐
│                      Mac Server (server/)                            │
│  FastAPI :8000 → /upload → save WAV → mlx-whisper → transcript.txt │
└─────────────────────────────────────────────────────────────────────┘
```

---

## 2. Component Map

| Component      | Location                            | Role                                      | Status    |
|----------------|-------------------------------------|-------------------------------------------|-----------|
| `event_bus`    | components/event_bus/               | Global pub/sub event broker               | ✅ Done   |
| `state`        | components/state/                   | Device FSM, broadcasts state changes      | ✅ Done   |
| `system_monitor` | components/system_monitor/        | FreeRTOS task stack watermark reporter    | ✅ Done   |
| `ui`           | components/ui/                     | LED pattern controller, event-driven      | ✅ Done   |
| `audio`        | components/audio/                  | INMP441 I2S reader, RMS monitor           | ✅ Done   |
| `led`          | components/led/                    | WS2812B RMT driver, low-level            | ✅ Done   |
| `button`       | components/button/                 | GPIO debounce, click/long-press detection | ✅ Done   |
| `storage`      | components/storage/                | SPI TF card, FAT32 mount, directory lifecycle | ✅ Done |
| `battery`      | components/battery/                | ADC voltage → percentage, events          | ✅ Done   |
| `recorder`     | components/recorder/               | WAV file pipeline (write-only, no dirs)   | ✅ Done   |
| `uploader`     | components/uploader/               | HTTP upload to Mac server (stub → v0.3)  | ⚠️ Stub   |
| `wifi_manager` | components/wifi_manager/           | WiFi STA, NVS credentials, reconnect      | ✅ Done   |
| `logger`       | components/logger/                | Structured log helper                     | ✅ Done   |
| `rgb_led`      | components/rgb_led/               | (Legacy; use `led` component instead)    | ⚠️ Legacy |

---

## 3. Event Bus Design

All inter-module communication flows through `event_bus`. Direct cross-component function calls are avoided.

### Event Types (from `event_bus.h`)

```
STATE:    EVENT_STATE_CHANGED
BUTTON:   EVENT_BUTTON_PRESSED / RELEASED / CLICKED / DOUBLE_CLICKED / LONG_PRESSED / HOLD
WIFI:     EVENT_WIFI_CONNECTED / DISCONNECTED
STORAGE:  EVENT_STORAGE_READY / ERROR
BATTERY:  EVENT_BATTERY_LOW / CRITICAL
RECORDER: EVENT_RECORDING_STARTED / STOPPED
UPLOAD:   EVENT_UPLOAD_STARTED / PROGRESS / DONE / FAILED
```

### Event Flow Example (Button → State → UI)

```
button.c
  → event_bus_publish(EVENT_BUTTON_CLICKED)
      → app_main on_button_event()
          → state_set(DEVICE_STATE_RECORDING)
              → event_bus_publish(EVENT_STATE_CHANGED)
                  → ui.c on_state_changed()
                      → led_set_pattern(LED_PATTERN_RECORDING)
```

---

## 4. Initialization Sequence

```c
// app_main.c — boot order (must not be changed without review)
1.  nvs_flash_init()
2.  event_bus_init()              // MUST be first — all modules depend on it
3.  state_init()                  // INIT state
4.  led_init(GPIO_NUM_48)
5.  button_init(GPIO_NUM_0)
6.  ui_init()
7.  system_monitor_init(10000)
8.  xTaskCreatePinnedToCore(audio_task, ...)   // core 0
9.  storage_mount("/sdcard")    // → storage_ensure_directories() + storage_validate_layout()
10. wifi_manager_init() + wifi_manager_restore_connection()
11. recorder_init()              // sessions scanned from 0:/recordings/
12. battery_init()
13. uploader_init()
14. event_bus_subscribe(...)     // register app-level event handlers
15. state_set(DEVICE_STATE_IDLE)
```

---

## 5. FreeRTOS Task Layout

| Task Name        | Stack  | Priority | Core | Description                                   |
|------------------|--------|----------|------|-----------------------------------------------|
| `audio`          | 8192 B | 3        | 0    | I2S read + RMS + ringbuf (always running)     |
| `recorder`       | 8192 B | 3        | 0    | Ringbuf → WAV file (always running)           |
| `sys_monitor`    | ~2048 B| 1        | any  | Stack watermark every 10s                    |
| `wifi` (internal)| ESP-IDF| varies   | any  | ESP-IDF WiFi stack               |
| `button_task`    | ~2048 B| 5        | any  | Button debounce timer (internal) |

> Priority note: ESP-IDF WiFi tasks run at priority 23. Audio task at 3 is safe.

---

## 6. SD Card File Layout

```
/sdcard/
├── recordings/        ← WAV files (REC_SESSION_XXXX.wav)
│                       Created + validated by storage.c (NOT recorder.c)
├── uploaded/          ← Uploaded files (auto-created)
├── upload_queue/      ← Upload task queue (auto-created)
├── temp/              ← Temporary files (auto-created)
├── logs/              ← On-device logs (auto-created)
└── test.txt           ← R/W test file (created at boot)
```

**Directory ownership: storage.c owns all directory lifecycle operations.
recorder.c only writes WAV files via fopen() — it never calls f_mkdir/f_opendir.
See docs/storage-path-policy.md for path strategy details.**

---

## 7. Mac Server Architecture

```
firmware/server/
├── app.py               ← FastAPI entry point (:8000)
├── received/            ← Uploaded WAV files
├── requirements.txt     ← Python dependencies
└── start.sh             ← Launch script
```

API endpoints:
- `POST /upload` — receive WAV file, save to `received/`
- `GET /` — health check

---

## 8. Confirmed Design Decisions

| Decision | Rationale |
|----------|-----------|
| SPI mode for SD card (not SDMMC 4-bit) | Simpler wiring, sufficient speed for 16kHz WAV |
| 20MHz SPI clock (not 50MHz) | Better compatibility with dupont wire connections |
| FAT32 (not exFAT) | ESP-IDF FATFS supports FAT32 natively |
| INMP441 left channel only | Mono mic, no right channel data |
| I2S `bit_shift=true` | INMP441 uses Philips I2S format (1-bit MSB delay) |
| event_bus synchronous dispatch | Simpler, no inter-task queue overhead for current scale |
| GPIO0 for button | Doubles as ESP32 boot pin; acceptable for dev board |

---

## 9. Future Extensions

| Area | Plan |
|------|------|
| Real WAV recording | v0.2: wire audio → recorder → storage |
| HTTP upload | v0.3: trigger on EVENT_RECORDING_STOPPED |
| BLE provisioning | v0.6: SmartConfig or BluFi for WiFi setup |
| OTA | v0.6: esp_https_ota component |
| Power management | v0.5: Deep sleep, wake on GPIO0 |
| AI memory sync | v0.4: transcript → external API |
