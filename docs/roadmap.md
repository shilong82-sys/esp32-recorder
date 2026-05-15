# Roadmap — ESP32 AI Recorder

> Version: v0.2 | Updated: 2026-05-13

---

## Version History

| Version | Status      | Summary                              |
|---------|-------------|--------------------------------------|
| v0.1    | ✅ Complete | Hardware bring-up                    |
| v0.2    | ✅ Complete | WAV recording pipeline               |
| v0.3    | 🔨 Current  | WiFi upload pipeline                 |
| v0.4    | Planned     | AI transcription integration         |
| v0.5    | Planned     | Power optimization                   |
| v0.6    | Future      | Production polish                    |

---

## v0.1 — Hardware Bring-up ✅ Complete

**Goal:** All hardware peripherals initialized and verified working.

- [x] ESP32-S3 first boot, chip info log
- [x] I2S microphone (INMP441) — RMS monitoring confirmed
- [x] SPI TF card — FAT32 mount, read/write test passed
- [x] WS2812B LED — RMT driver, color/blink patterns
- [x] Button — debounce, click/double-click/long-press events
- [x] WiFi manager — STA mode, NVS credential restore
- [x] Battery ADC — voltage → percentage
- [x] Event bus — pub/sub working
- [x] State machine — INIT/IDLE/RECORDING/UPLOADING/ERROR/SLEEP
- [x] UI component — LED driven by state events
- [x] System monitor — FreeRTOS task stack watermarks
- [x] Mac server — FastAPI /upload endpoint
- [x] mlx-whisper — local transcription on Apple Silicon
- [x] Git + GitHub — version control established

---

## v0.2 — WAV Recording Pipeline ✅ Complete

**Goal:** Real audio capture → WAV file on SD card.

- [x] Wire `audio` component into `recorder` component
  - `audio_read()` → ring buffer → WAV write
- [x] Implement WAV file creation with correct RIFF header
- [x] Implement WAV file streaming write (no large RAM buffer)
- [x] Update WAV header (file size, data chunk size) on `recorder_stop()`
- [x] File naming: `REC_SESSION_XXXX.wav` using session counter
- [x] Event integration: publish `EVENT_RECORDING_STARTED` / `STOPPED`
- [x] State integration: IDLE → RECORDING → IDLE via button
- [x] SD card space check before starting recording
- [x] Test: verify WAV file is playable on Mac
- [x] Stable-state directory architecture (recordings/ → upload_queue/)

---

## v0.3 — WiFi Upload Pipeline 🔨 In Progress

**Goal:** Automatically upload recorded WAV files to Mac server.

**Architecture:** Stable-state upload queue (producer-consumer)
- `recordings/` → recorder writes finalized WAV → rename → `upload_queue/`
- `upload_queue/` → uploader_task (独立 FreeRTOS task) scans FIFO → HTTP POST → rename → `uploaded/`
- `uploaded/` → 归档，不 delete

- [x] `storage_rename_file()` — 跨目录 move
- [x] `storage_delete_file_vfs()` — 任意相对路径删除
- [x] recorder_stop flow: flush WAV → finalize header → fclose → rename upload_queue/
- [x] uploader_task 独立任务：周期扫描、FIFO 上传、重试（3s/10s/30s）、归档 rename
- [x] WiFi 断开/恢复事件：自动暂停/继续上传
- [x] WiFi 事件回调系统（wifi_manager_register_callback）
- [ ] Retry 失败文件保留在队列，等待下一轮
- [ ] WiFi 断开期间录音不受影响
- [ ] 固件烧录验证

---

## v0.4 — AI Transcription Integration

**Goal:** Recorded audio automatically transcribed; transcripts accessible.

- [ ] Server: trigger mlx-whisper on new file upload
- [ ] Server: return transcript in upload response JSON
- [ ] ESP32: receive and log transcript from server response
- [ ] Store transcript alongside WAV file on SD card (`.txt`)
- [ ] Optional: sync transcript to cloud / external memory system

---

## v0.5 — Power Optimization

**Goal:** Extend battery life; deep sleep between recordings.

- [ ] Implement `DEVICE_STATE_SLEEP` → `esp_deep_sleep_start()`
- [ ] Wake on GPIO0 (button) interrupt
- [ ] Wake on timer (configurable, default 10 min)
- [ ] Disable WiFi and I2S before sleep
- [ ] Save state to NVS before sleep
- [ ] Measure actual current in each state
- [ ] Target: >8 hours standby on 500mAh

---

## v0.6 — Production Polish

**Goal:** Device ready for daily use; reliable, maintainable.

- [ ] OTA firmware update (esp_https_ota)
- [ ] BLE WiFi provisioning (no hard-coded SSID/password)
- [ ] Error LED codes (blink patterns for error types)
- [ ] Watchdog timer enabled
- [ ] NVS schema versioning
- [ ] SD card wear leveling configuration
- [ ] Hardware revision: replace dupont wires with PCB or soldered connections
- [ ] README with quick-start guide

---

## Long-Term Architecture Roadmap

> ⚠️ **These are future capability phases — not current implementation targets.**
> All items below require architecture reservation before any code is written.
> See `docs/expansion-architecture.md` and `docs/platform-reservation.md` for details.

### Phase 2–5 Overview

| Phase | Name | Target | Status |
|-------|------|--------|--------|
| v0.2–v0.4 | Offline AI Memory | Upload + transcription + local transcript storage | 🔨 In Progress |
| Phase 2 | Audio Output | I2S TX + MAX98357A speaker + TTS | Architecture Reserved |
| Phase 2 | OLED Display | I2C SSD1306 128×64 | Architecture Reserved |
| Phase 2 | Wake Word | Local wake-word detection | Architecture Reserved |
| Phase 3 | PTT AI Assistant | Full duplex audio + push-to-talk AI query | Architecture Reserved |
| Phase 3 | 4G Connectivity | Cellular backup with network abstraction layer | Architecture Reserved |
| Phase 4 | Continuous Voice Agent | Always-on wake word + cellular + GPS | Architecture Reserved |
| Phase 4 | Camera | Daughterboard-based photo capture | Architecture Reserved |
| Phase 4 | IMU / Sensors | Activity detection, environmental sensing | Architecture Reserved |

### Phase 2 — Audio Output & OLED (Future)

**Goal:** Add audio output and a small display without disrupting the recording pipeline.

- [ ] Reserve GPIO1/GPIO2 for I2C OLED (SSD1306)
- [ ] Reserve I2S TX channel for MAX98357A speaker
- [ ] Design 2×10 expansion header on core board
- [ ] Implement `audio_output` component (HAL)
- [ ] Implement `display` component (OLED driver)
- [ ] Integrate TTS: receive transcript text → play via speaker
- [ ] Display recording status and transcript snippets on OLED
- [ ] Add haptic motor feedback (LRA) via GPIO18

**Key resources reserved:**

| Resource | GPIO / Bus | Notes |
|----------|-----------|-------|
| I2C bus | GPIO1 (SDA), GPIO2 (SCL) | SSD1306 OLED, sensors |
| I2S TX | Future routing on I2S0 | MAX98357A speaker |
| Haptic GPIO | GPIO18 | LRA motor driver |
| Power rail | Audio 5V rail | Speaker amp power |

### Phase 3 — 4G Connectivity (Future)

**Goal:** Add cellular connectivity for outdoor/offline-WiFi use with a clean network abstraction.

- [ ] Reserve UART2 (GPIO16/GPIO17) for 4G module
- [ ] Design dedicated 3.8V power rail for 4G (up to 2A peak)
- [ ] Implement `network_manager` abstraction (uploader → network_manager → transport backend)
- [ ] Implement WiFi backend (current)
- [ ] Implement 4G backend (Quectel BC66/BC95 or similar)
- [ ] Implement cellular reconnect logic with exponential backoff
- [ ] Handle UART2 ownership (cellular vs debug — exclusive)
- [ ] Compress audio before cellular upload (reduce data cost)

**Key challenge:** 4G peak current up to 2A. Must use dedicated power rail — never power 4G from main 3.3V rail.

### Phase 4 — Continuous Voice Agent (Future)

**Goal:** Transform device from a button-triggered recorder to an always-on voice agent.

- [ ] Implement wake word engine (TensorFlow Lite Micro or ESP-Skainet)
- [ ] Implement full duplex audio (simultaneous RX + TX)
- [ ] Integrate always-on microphone pipeline with wake word detection
- [ ] Add GNSS module (UART2 shared with 4G or separate)
- [ ] GPS coordinate embedding in WAV file metadata
- [ ] Camera daughterboard: OV2640 + ESP32-C3 co-processor
- [ ] OTA firmware update with rollback on failure

### Long-Term Vision: Platform Ecosystem

- Daughterboard marketplace (third-party modules)
- Cloud sync beyond local Mac
- OTA firmware marketplace
- Companion app for iOS/Android (future, not current)

---

## Non-Goals (Out of Scope)

- Android/iOS companion app (out of scope for v1)
- Multiple microphone support
- Real-time streaming (non-buffered) to server
- Built-in display (Phase 2+)
- USB audio class
- Direct cloud upload in Phase 1–2 (local Mac server only)
