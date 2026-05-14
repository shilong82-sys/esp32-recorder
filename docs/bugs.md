# Bugs — ESP32 AI Recorder

> Version: v0.2.1 | Updated: 2026-05-14

---

## Bug Tracking

| ID      | Severity | Component   | Status     | Summary                                        |
|---------|----------|-------------|------------|------------------------------------------------|
| BUG-001 | Medium   | storage     | Open       | SD card intermittent timeout (dupont wire)     |
| BUG-002 | Low      | recorder    | Expected   | recorder is stub, creates empty WAV            |
| BUG-003 | Low      | uploader    | Expected   | uploader is stub, no actual HTTP upload        |
| BUG-004 | Low      | app_main    | ✅ Fixed   | Init sequence has two "[10/10]" step numbers   |
| BUG-005 | Info     | audio       | ✅ Fixed   | `auto_clear = true` set on RX-only I2S channel |
| BUG-006 | P0       | recorder    | ✅ Fixed   | `recorder_start()` fopen fails: dir missing    |
| BUG-007 | P1       | sys_mon     | ✅ Fixed   | `vTaskList()` in esp_timer callback → crash     |

---

## BUG-001 — SD Card Intermittent Timeout

**Severity:** Medium
**Component:** storage
**Status:** Open (hardware issue)

**Symptom:**
```
E (xxx) sdmmc_common: sdmmc_init_ocr: send_op_cond (1) returned 0x107 (ESP_ERR_TIMEOUT)
```

**Root Cause:**
Dupont wire connections between ESP32-S3 and TF card module have poor contact.
The SPI clock signal degrades at higher frequencies or with wire movement.

**Current Workaround:**
- Reduce SPI frequency to 20MHz (`SDMMC_FREQ_DEFAULT`) — done ✅
- Firmly re-seat dupont wires before power-on
- Reformat TF card as FAT32 if not already

**Proper Fix (v0.6):**
- Replace dupont wires with soldered connections or a PCB breakout
- Hardware fix, not firmware

**Detection:**
`storage_mount()` returns `ESP_ERR_TIMEOUT (0x107)`.
`app_main` logs: `Storage mount failed (0x107) - running without SD`
Device runs in degraded mode (no recording possible).

---

## BUG-002 — Recorder is Stub

**Severity:** Low (Expected)
**Component:** recorder
**Status:** Expected — will be fixed in v0.2

**Symptom:**
`recorder_start()` creates an empty WAV file (44-byte header, 0 samples).
Recording shows no audio data.

**Root Cause:**
I2S → WAV streaming pipeline not yet implemented.
`recorder.c` contains stub implementation.

**Fix:** Implement in v0.2 (see `docs/todo.md`).

---

## BUG-003 — Uploader is Stub

**Severity:** Low (Expected)
**Component:** uploader
**Status:** Expected — will be fixed in v0.3

**Symptom:**
`uploader_upload_file()` returns immediately without sending HTTP request.

**Root Cause:**
HTTP POST multipart upload not yet implemented.

**Fix:** Implement in v0.3 (see `docs/todo.md`).

---

## BUG-004 — Init Sequence Step Number Inconsistency

**Severity:** Low (Cosmetic)
**Component:** app_main
**Status:** Open

**Symptom:**
Boot log shows:
```
[10/10] Recorder ...
[10/11] Battery ...
[11/11] Uploader ...
```
Two steps labeled `[10/10]`.

**Root Cause:**
Steps were added incrementally and numbering was not updated.

**Fix:**
Update `app_main.c` log strings. Trivial fix.
Correct sequence is 11 steps: NVS + event_bus + state + led + button + ui + sysmon + audio + storage + wifi + recorder/battery/uploader.

---

## BUG-005 — `auto_clear = true` on RX-only I2S Channel

**Severity:** Info (No functional impact)
**Component:** audio
**Status:** Open (minor)

**Symptom:**
In `audio.c`:
```c
chan_cfg.auto_clear = true;  // comment says "auto clear TX empty data"
```
But this is an RX-only channel (no TX). `auto_clear` applies to TX channels.

**Root Cause:**
`auto_clear` was set from a template that assumed TX was present.
For RX-only, it is silently ignored by ESP-IDF.

**Fix:**
Set `auto_clear = false` for RX-only channels. No behavioral change.

---

## BUG-006 — `recorder_start()` fopen EINVAL: Directory Missing

**Severity:** P0
**Component:** recorder
**Status:** ✅ Fixed (2026-05-14)

**Symptom:**
```
Failed to open file: /sdcard/recordings/REC_SESSION_0001.wav (errno=22)
```

**Root Cause:**
1. `session_init()` used POSIX `stat()` which may not work reliably on FATFS paths
2. `recorder_start()` called `fopen()` without verifying directory exists
3. Used POSIX `mkdir()` instead of FATFS-native `f_mkdir()`

**Fix:**
- Added `storage_ensure_directories()` to `storage.c` using `f_mkdir()` (FATFS API)
- Called automatically from `storage_mount()` after FATFS mount succeeds
- Added defensive directory check in `recorder_start()` before `fopen()`
- Changed `session_init()` to use `f_opendir()` + `f_mkdir()` (FATFS-native)
- Changed `recorder.c` includes from `<sys/stat.h>/<dirent.h>` to `<ff.h>`

---

## BUG-007 — `system_monitor` Crash: vTaskList() in esp_timer Callback

**Severity:** P1
**Component:** system_monitor
**Status:** ✅ Fixed (2026-05-14)

**Symptom:**
Device crashes during periodic monitor timer callback:
```
vTaskList()
dump_all_tasks()
monitor_timer_callback()
```

**Root Cause:**
`vTaskList()` consumes ~1.5KB of stack. The `esp_timer` task has only ~3.5KB stack.
Combined with existing stack usage, this causes stack overflow → crash.

**Fix:**
Refactored `system_monitor` architecture:
- `monitor_timer_callback()`: now only does `xSemaphoreGiveFromISR()` (lightweight)
- New `monitor_task` (4KB stack): waits on semaphore, calls `vTaskList()` + `dump_task_watermarks()`
- Architecture: Timer → signal → dedicated task → heavy work

---

## Resolved Bugs

| ID      | Date       | Summary                                         | Fix Applied                                         |
|---------|------------|-------------------------------------------------|-----------------------------------------------------|
| —       | 2026-05-09 | `spi_master` component not found in ESP-IDF v5.2| Removed from `CMakeLists.txt`; SPI is in `driver`   |
| —       | 2026-05-11 | SD card capacity reported as 266MB (wrong)      | Fixed CSD capacity calc for SDHC (512B sectors)    |
| —       | 2026-05-11 | SD card timeout at 50MHz                        | Reduced to 20MHz (`SDMMC_FREQ_DEFAULT`)             |
| —       | 2026-05-11 | GitHub push 403 (PAT permissions)               | Regenerated PAT with `repo` write scope             |
| BUG-004 | 2026-05-14 | Init sequence "[10/10]" duplicate             | Fixed to [1/11]..[11/11]                           |
| BUG-005 | 2026-05-14 | `auto_clear=true` on RX-only channel           | Set to `false` for RX-only I2S channel             |
| BUG-006 | 2026-05-14 | P0: `fopen()` EINVAL (dir missing)            | `storage_ensure_directories()` + `f_mkdir()`        |
| BUG-007 | 2026-05-14 | P1: `vTaskList()` crash in esp_timer callback  | Refactored: monitor_task + semaphore signaling      |
