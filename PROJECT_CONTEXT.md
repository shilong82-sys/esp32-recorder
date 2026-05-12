# PROJECT_CONTEXT.md — ESP32 AI Recorder

> **For any AI agent taking over this project:**
> Read this file first. It tells you everything you need to continue development safely.
> Do NOT modify GPIO assignments or refactor core logic without an architecture review.

---

## 1. Project Goal

Build a **standalone voice recording device** based on ESP32-S3 that:

1. Captures audio via I2S microphone (INMP441)
2. Saves recordings as WAV files to a TF (microSD) card
3. Uploads recordings to a Mac server over WiFi
4. Transcribes audio using mlx-whisper (Apple Silicon, local)
5. Feeds transcripts into a long-term AI memory system (future)

Primary use case: personal voice memo / meeting recording with AI-assisted transcription and memory.

---

## 2. Hardware

| Component       | Part              | Notes                                |
|-----------------|-------------------|--------------------------------------|
| MCU             | ESP32-S3 N16R8    | Xtensa LX7 dual-core, 16MB Flash, 8MB PSRAM |
| Microphone      | INMP441           | I2S digital MEMS mic, mono, Philips standard |
| Storage         | SPI TF Card Module| FAT32, SPI mode, SPI2 bus            |
| LED             | WS2812B           | Single addressable RGB LED           |
| Power           | Li-ion battery    | ADC voltage divider monitoring       |
| Button          | Tactile switch    | Active-low, GPIO0 (boot pin)         |

**Mac-side server:**
- Python 3.x, FastAPI, mlx-whisper
- Receives WAV uploads via HTTP POST /upload
- Transcribes using mlx-whisper (Apple Silicon Metal GPU)

---

## 3. GPIO Freeze Table

> ⚠️ **These GPIOs are FROZEN. Do NOT change without architecture review.**

| Function    | GPIO | Direction | Notes                        |
|-------------|------|-----------|------------------------------|
| WS2812B     | 48   | OUT       | RMT peripheral               |
| Button      | 0    | IN        | Active-low, internal pull-up |
| MIC_BCLK    | 4    | OUT       | I2S bit clock (to INMP441)   |
| MIC_WS      | 5    | OUT       | I2S word select / LRCLK      |
| MIC_DIN     | 6    | IN        | I2S data from INMP441        |
| SD_CS       | 10   | OUT       | SPI chip select              |
| SD_MOSI     | 11   | OUT       | SPI MOSI                     |
| SD_SCK      | 12   | OUT       | SPI clock                    |
| SD_MISO     | 13   | IN        | SPI MISO                     |

Battery ADC channel is ADC_CHANNEL_0 (GPIO1, configurable in `battery_config_t`).

---

## 4. System Architecture

### 4.1 Layer Overview

```
┌─────────────────────────────────────────────────────────────┐
│                        app_main.c                           │
│             (boot sequence + event subscriptions)           │
├─────────────────────────────────────────────────────────────┤
│  Architecture Layer                                         │
│  ┌──────────┐  ┌──────────┐  ┌───────────────┐             │
│  │event_bus │  │  state   │  │ system_monitor│             │
│  └──────────┘  └──────────┘  └───────────────┘             │
├─────────────────────────────────────────────────────────────┤
│  UI Layer                                                   │
│  ┌──────────┐                                               │
│  │    ui    │  (subscribes to events, drives LED patterns) │
│  └──────────┘                                               │
├─────────────────────────────────────────────────────────────┤
│  Hardware Abstraction Layer                                 │
│  ┌───────┐ ┌────────┐ ┌────────┐ ┌──────┐ ┌────────────┐  │
│  │ audio │ │  led   │ │ button │ │  bat │ │   storage  │  │
│  └───────┘ └────────┘ └────────┘ └──────┘ └────────────┘  │
├─────────────────────────────────────────────────────────────┤
│  Business Logic Layer                                       │
│  ┌──────────┐  ┌──────────┐  ┌─────────────┐               │
│  │ recorder │  │ uploader │  │ wifi_manager│               │
│  └──────────┘  └──────────┘  └─────────────┘               │
└─────────────────────────────────────────────────────────────┘
```

### 4.2 Initialization Order (app_main.c)

```
NVS → event_bus → state → led → button → ui → system_monitor
    → audio (task) → storage → wifi_manager → recorder → battery → uploader
    → subscribe events → state_set(IDLE)
```

### 4.3 Key Design Patterns

- **Event-driven**: All inter-module communication via `event_bus`. No direct cross-component calls.
- **State machine**: Device state managed centrally by `state` component. State changes fire `EVENT_STATE_CHANGED`.
- **Task-based**: Long-running operations in FreeRTOS tasks. `app_main` exits to scheduler after init.
- **HAL separation**: Hardware-specific code isolated in components. Business logic does not touch GPIO directly.

---

## 5. What's Done

| Area              | Status | Notes                                               |
|-------------------|--------|-----------------------------------------------------|
| ESP32-S3 bring-up | ✅ Done | Boot log, chip info, heap report working            |
| I2S microphone    | ✅ Done | INMP441, 16kHz mono, RMS monitoring verified        |
| SPI TF card       | ✅ Done | FAT32, mount/unmount, read/write test passed        |
| WiFi manager      | ✅ Done | STA mode, NVS credential restore, event callbacks  |
| LED (WS2812B)     | ✅ Done | RMT-based, patterns: solid/blink/breath             |
| Button            | ✅ Done | Debounce, click/double-click/long-press events      |
| Event bus         | ✅ Done | Pub/sub, 32-slot, synchronous dispatch              |
| State machine     | ✅ Done | INIT/IDLE/RECORDING/UPLOADING/ERROR/SLEEP           |
| UI component      | ✅ Done | Subscribes to state/events, drives LED              |
| System monitor    | ✅ Done | FreeRTOS task stack watermark reporting             |
| Battery monitor   | ✅ Done | ADC voltage → percentage, low/critical events       |
| Recorder          | ⚠️ Stub | WAV header generation done; I2S write pipeline TODO |
| Uploader          | ⚠️ Stub | HTTP POST skeleton done; full upload pipeline TODO  |
| Mac server        | ✅ Done | FastAPI, /upload endpoint, mlx-whisper integration  |

---

## 6. Current Phase Goal

**Phase: firmware architecture stabilization**

We are NOT adding new features. We are:

1. Documenting everything so any AI can take over
2. Establishing clean component boundaries
3. Ensuring all modules compile and run stably
4. Defining coding standards and conventions
5. Building a solid foundation before WAV recording pipeline

Next major milestone: implement real WAV recording pipeline (recorder component).

---

## 7. Engineering Rules

1. **Compile must pass** after every change. Run `idf.py build` before commit.
2. **No GPIO changes** without updating `docs/pinout.md` AND an architecture review.
3. **No business logic in HAL components** (led, button, audio, storage, battery).
4. **All inter-component communication via event_bus**, not direct function calls.
5. **All long-running operations must be FreeRTOS tasks**, not blocking in `app_main`.
6. **Error handling is mandatory**: every `esp_err_t` return must be checked.
7. **Log tags must match component names** (e.g., `TAG = "storage"`, `TAG = "audio"`).
8. **No dynamic memory allocation in interrupt context**.
9. **WAV files on SD card** go to `/sdcard/records/`. Test files go to `/sdcard/`. 
10. **Commit message format**: `[component] short description` (e.g., `[storage] fix capacity calc for SDHC cards`).

---

## 8. AI Collaboration Rules

These rules apply to any AI agent (ChatGPT, Claude, Copilot, Cursor, etc.) working on this project.

### Core Principles

- **Architecture stability first**: Do not refactor working code without a clear reason.
- **No feature creep**: Only implement what is explicitly requested.
- **Small, verified steps**: One change at a time. Confirm compile passes before next step.
- **Preserve existing behavior**: Changes must not break currently working features.
- **ESP-IDF best practices**: Use the v5.x API. Do not use deprecated v4.x APIs.

### Specific Rules

| Rule | Detail |
|------|--------|
| No blocking I/O in tasks | Use `i2s_channel_read` with timeout, not blocking forever |
| No blocking audio architecture | Audio pipeline must be non-blocking (DMA-based) |
| Task naming | Format: `"component_task"` (e.g., `"audio_task"`, `"upload_task"`) |
| Event naming | Format: `EVENT_NOUN_VERB` (e.g., `EVENT_RECORDING_STARTED`) |
| State naming | Format: `DEVICE_STATE_NAME` (e.g., `DEVICE_STATE_RECORDING`) |
| GPIO access | Only in HAL components. Business logic uses events. |
| FreeRTOS queues | Prefer queues over global variables for task communication |
| Heap monitoring | Check `esp_get_free_heap_size()` in system_monitor, not ad-hoc |

### When Taking Over This Project

1. Read `PROJECT_CONTEXT.md` (this file)
2. Read `docs/architecture.md` for system design
3. Read `docs/state-machine.md` for state transitions
4. Read `docs/pinout.md` for frozen GPIO assignments
5. Read `docs/coding-style.md` for naming and style conventions
6. Read `docs/todo.md` for current task list
7. Read `docs/bugs.md` for known issues

### What NOT to Do Without Asking

- Change any GPIO assignment
- Add new FreeRTOS tasks without confirming stack/priority
- Modify the event_bus or state component interfaces
- Add external library dependencies (new components)
- Change the SD card mount point or FAT32 allocation unit size
- Modify `sdkconfig` or `sdkconfig.defaults`

---

## 9. Roadmap

| Version | Name              | Key Features                                  | Status      |
|---------|-------------------|-----------------------------------------------|-------------|
| v0.1    | Hardware Bring-up | All peripherals initialized, basic I/O working | ✅ Complete |
| v0.2    | WAV Recording     | Real I2S→WAV pipeline, file naming, SD write  | 🔨 Next     |
| v0.3    | WiFi Upload       | HTTP upload, server receive, progress events  | Planned     |
| v0.4    | AI Integration    | Whisper transcription, transcript sync        | Planned     |
| v0.5    | Power Optimization| Deep sleep, battery life tuning               | Planned     |
| v0.6    | Production Polish | OTA, BLE provisioning, error LED codes        | Future      |

---

## 10. Known Issues

| ID   | Severity | Component | Description                                    | Workaround                     |
|------|----------|-----------|------------------------------------------------|--------------------------------|
| BUG-001 | Medium | storage | Dupont wire contact instability causes SD card timeout (ESP_ERR_TIMEOUT 0x107) | Firmly reseat card and wires; format as FAT32 |
| BUG-002 | Low    | recorder | Recorder is stub only; `recorder_start()` creates empty WAV files | Expected — real pipeline in v0.2 |
| BUG-003 | Low    | uploader | Uploader does not actually upload; HTTP POST is stub | Expected — real upload in v0.3 |
| BUG-004 | Low    | app_main | Init sequence step numbering is inconsistent (two "[10/10]" entries) | Cosmetic; fix in next cleanup |
| BUG-005 | Info   | audio    | `auto_clear = true` set for TX-only RX channel (harmless but semantically incorrect) | No functional impact |

---

## 11. Repository Structure

```
esp32-recorder/
├── PROJECT_CONTEXT.md          ← YOU ARE HERE (start here)
├── README.md
├── docs/                       ← All project documentation
│   ├── architecture.md
│   ├── hardware.md
│   ├── pinout.md
│   ├── state-machine.md
│   ├── roadmap.md
│   ├── todo.md
│   ├── bugs.md
│   └── coding-style.md
├── firmware/                   ← ESP-IDF firmware project
│   ├── main/app_main.c         ← Entry point
│   ├── components/             ← Custom components (see architecture.md)
│   ├── config/                 ← YAML config files
│   ├── scripts/                ← Build/flash/monitor helper scripts
│   ├── server/                 ← Mac-side FastAPI server
│   └── docs/                   ← (legacy, being migrated to /docs)
├── esp-idf/                    ← ESP-IDF v5.2 submodule (do not modify)
└── hello_world/                ← ESP-IDF sanity test project
```

---

*Last updated: 2026-05-12*
*Maintained by: AI engineering agent + shilong82-sys*
