# TODO — ESP32 AI Recorder

> Version: v0.2 | Updated: 2026-05-12
> Format: `[Priority] Task — Component`

---

## 🔴 High Priority (v0.2 Blockers)

- [ ] **[recorder]** Implement real WAV recording pipeline
  - Wire `audio_read()` → internal ring buffer → FAT32 file write
  - Update RIFF header on stop
  - Verify output WAV plays correctly on Mac
  - File: `components/recorder/recorder.c`

- [ ] **[app_main]** Connect button events to recorder start/stop
  - IDLE + button click → `recorder_start()`
  - RECORDING + button click → `recorder_stop()`
  - File: `main/app_main.c` `on_button_event()`

- [ ] **[app_main]** Fix init sequence step numbering bug (two "[10/10]" entries)
  - See BUG-004 in `docs/bugs.md`
  - File: `main/app_main.c`

---

## 🟡 Medium Priority (Polish + Stability)

- [ ] **[storage]** Test SD card reliability with soldered connections
  - Current dupont wire causes intermittent timeouts (BUG-001)
  - May require hardware change rather than firmware fix

- [ ] **[recorder]** Add SD card space check before recording starts
  - Reject start if free space < configurable threshold (e.g., 10MB)

- [ ] **[uploader]** Design upload queue data structure
  - Decide: JSON task files in `/sdcard/pending/` vs. NVS queue
  - Document decision in `docs/architecture.md`

- [ ] **[audio]** Fix `auto_clear = true` on RX-only channel (BUG-005)
  - Harmless but semantically incorrect
  - File: `components/audio/audio.c`

- [ ] **[logger]** Integrate `logger` component into all components
  - Currently `logger.c` exists but most components use `ESP_LOG*` directly
  - Decide: keep logger as structured wrapper or remove it

- [ ] **[rgb_led]** Evaluate and remove or merge `rgb_led` component
  - `rgb_led` appears to be a legacy component superseded by `led`
  - Check if anything depends on it; remove if unused

---

## 🟢 Low Priority (v0.3+)

- [ ] **[uploader]** Implement HTTP POST multipart upload
  - Use `esp_http_client`
  - Handle chunked transfer for large WAV files

- [ ] **[wifi_manager]** Add WiFi reconnect with exponential backoff
  - Currently: tries once, no retry logic

- [ ] **[battery]** Validate battery voltage calibration against real meter
  - Current: calculated from ADC, not hardware-calibrated

- [ ] **[config]** Integrate YAML config files into firmware
  - `config/audio.yaml`, `config/device.yaml`, `config/server.yaml`
  - Currently: values are hard-coded in source files

- [ ] **[docs]** Migrate `firmware/docs/` content into top-level `docs/`
  - `firmware/docs/dev-log.md` → consolidate into `DEVELOPMENT_LOG.md`
  - `firmware/docs/architecture.md` → superseded by `docs/architecture.md`

---

## ✅ Completed

- [x] ESP32-S3 bring-up, boot log
- [x] I2S microphone (INMP441) initialization and RMS monitoring
- [x] SPI TF card — FAT32 mount, read/write test
- [x] WS2812B LED — RMT driver, patterns
- [x] Button — debounce, event types
- [x] Event bus — pub/sub framework
- [x] State machine — FSM with event broadcast
- [x] UI component — state-driven LED
- [x] System monitor — task stack watermarks
- [x] WiFi manager — STA mode, NVS credentials
- [x] Battery ADC — percentage calculation
- [x] Mac server — FastAPI /upload endpoint
- [x] mlx-whisper — local transcription
- [x] GitHub repository — version control
- [x] macOS Keychain — secure Git token storage
- [x] PROJECT_CONTEXT.md — AI handover document
- [x] docs/ — architecture, hardware, pinout, state-machine, roadmap, todo, bugs, coding-style
