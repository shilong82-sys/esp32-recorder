# Roadmap — ESP32 AI Recorder

> Version: v0.2 | Updated: 2026-05-12

---

## Version History

| Version | Status      | Summary                              |
|---------|-------------|--------------------------------------|
| v0.1    | ✅ Complete | Hardware bring-up                    |
| v0.2    | 🔨 Current  | WAV recording pipeline               |
| v0.3    | Planned     | WiFi upload pipeline                 |
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

## v0.2 — WAV Recording Pipeline 🔨 In Progress

**Goal:** Real audio capture → WAV file on SD card.

- [ ] Wire `audio` component into `recorder` component
  - `audio_read()` → ring buffer → WAV write
- [ ] Implement WAV file creation with correct RIFF header
- [ ] Implement WAV file streaming write (no large RAM buffer)
- [ ] Update WAV header (file size, data chunk size) on `recorder_stop()`
- [ ] File naming: `REC_YYYYMMDD_HHMMSS.wav` using `esp_timer`
- [ ] Event integration: publish `EVENT_RECORDING_STARTED` / `STOPPED`
- [ ] State integration: IDLE → RECORDING → IDLE via button
- [ ] SD card space check before starting recording
- [ ] Test: verify WAV file is playable on Mac

**Key files to modify:**
- `firmware/components/recorder/recorder.c`
- `firmware/components/recorder/include/recorder.h`
- `firmware/main/app_main.c` (wire state changes to recorder calls)

---

## v0.3 — WiFi Upload Pipeline

**Goal:** Automatically upload recorded WAV files to Mac server.

- [ ] Implement `uploader_upload_file()` — HTTP POST multipart
- [ ] Upload queue: scan SD card for `.wav` files pending upload
- [ ] Event integration: trigger upload on `EVENT_RECORDING_STOPPED`
- [ ] Progress reporting: `EVENT_UPLOAD_PROGRESS`
- [ ] Retry logic: 3 retries with exponential backoff
- [ ] Move uploaded files to `/sdcard/done/`
- [ ] Handle WiFi disconnect during upload
- [ ] Server: receive file, save, return transcript

**Key files to modify:**
- `firmware/components/uploader/uploader.c`
- `firmware/server/app.py`

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

## Non-Goals (Out of Scope)

- Android/iOS companion app (out of scope for v1)
- Multiple microphone support
- Real-time streaming (non-buffered) to server
- Built-in display
- USB audio class
