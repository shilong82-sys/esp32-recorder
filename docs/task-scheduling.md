# Task Scheduling — ESP32 AI Recorder

> **Version:** v0.1 | **Updated:** 2026-05-14
> **Scope:** FreeRTOS task design, priority assignment, suspend/resume strategy

---

## 1. Task Overview

| Task Name | Stack (bytes) | Priority | Core | Responsibility |
|-----------|-------------|----------|------|---------------|
| app_main | (exits) | — | — | Init only, then deleted |
| audio | 8192 | 3 | Core 0 | I2S mic read, RMS, ringbuf send |
| recorder | 8192 | 3 | Core 0 | SD write, WAV file management |
| ui | 4096 | 2 | Core 0 | LED pattern animation |
| wifi_manager | 4096 | 4 | Core 0 | WiFi connection management |
| system_monitor | 4096 | 1 | Core 1 | Stack watermark, heap monitoring |

**Priority scale:** Higher number = higher priority (FreeRTOS config).

---

## 2. Priority Design

### Priority 1 (system_monitor)
- Lowest priority
- Periodic monitoring only — never blocks important work
- Runs every 10 seconds

### Priority 2 (ui)
- Medium-low priority
- LED animations must not cause audio glitches
- Uses `vTaskDelay` — cooperative yielding

### Priority 3 (audio, recorder)
- **Critical audio priority**
- Both pinned to core 0 to minimize I2S jitter
- audio_task and recorder_task are **mutually exclusive** (never run simultaneously)

### Priority 4 (wifi_manager)
- Highest priority
- WiFi operations must complete quickly to avoid connection drops
- Network I/O can block

---

## 3. Audio Task Behavior

### Normal Mode (IDLE state)

```
audio_task loop:
  audio_read()      → get RMS
  ESP_LOGD(RMS)     → throttled (every 5s)
  vTaskDelay(100ms) → 10 reads/sec for RMS monitoring
```

- **Purpose:** Verify mic is working, provide RMS feedback
- **CPU load:** Minimal (~1% due to 100ms delay)
- **I2S access:** Exclusive owner of I2S peripheral

### Recording Mode (RECORDING state)

```
audio_task:
  vTaskSuspend(audio_task)  ← Called by app_main

recorder_task loop:
  ringbuf_receive()         ← reads PCM from ringbuf
  batch_accumulate()
  fwrite()                  ← SD card batch write
  vTaskDelay(50ms)         ← prevents tight loop
```

- **Purpose:** audio_task suspended — recorder_task handles all I2S data
- **CPU load:** Mostly SD write latency
- **I2S access:** `audio_read()` is not called — no contention

### Resume on Stop

```
app_main on_state_changed(→IDLE from RECORDING):
  recorder_stop()
  vTaskResume(audio_task)   ← audio_task resumes RMS monitoring
```

---

## 4. Suspend/Resume Mechanism

### Why Suspend Instead of Delete/Create?

| Approach | Pro | Con |
|----------|-----|-----|
| `vTaskSuspend/Resume` | Fast (<1ms), preserves state | Task stack remains allocated |
| `vTaskDelete/Create` | Frees stack memory | ~10ms overhead, stack re-init |
| **Decision** | **Simplicity + speed wins** | **32KB RAM acceptable** |

### Implementation

```c
// app_main.c
static TaskHandle_t s_audio_task_handle = NULL;

// audio_task creation (saves handle)
xTaskCreatePinnedToCore(&audio_task, "audio", ..., &s_audio_task_handle, ...);

// on_state_changed (RECORDING → IDLE transition)
vTaskSuspend(s_audio_task_handle);  // Stop RMS monitoring
recorder_start(NULL);

// on_state_changed (IDLE → RECORDING transition)
recorder_stop();
vTaskResume(s_audio_task_handle);    // Resume RMS monitoring
```

### Safety Rules

1. **Never suspend from ISR** — `vTaskSuspend` is not ISR-safe
2. **Check handle is not NULL** before suspending
3. **Always resume** after stopping recorder — or audio_task will be stuck suspended
4. **Wrap in critical section** if state changes can occur from multiple contexts

---

## 5. Task Watchdog

ESP32 has a Task Watchdog Timer (TWDT) on each core. Tasks that don't yield for too long will trigger a watchdog reset.

### Audio/Recorder Tasks and Watchdog

- `audio_task` has a `vTaskDelay(100ms)` in its loop → yields regularly ✅
- `recorder_task` has a `vTaskDelay(50ms)` in its loop → yields regularly ✅
- Both tasks are subscribed to the TWDT by default (ESP-IDF default)

### Long SD Write Problem

`fwrite()` can take 10–50ms on a slow SD card. During this time, the task holds the CPU and doesn't yield. This could cause the TWDT to fire.

**Solution:**
- Batch writes (32KB) reduce the frequency of SD operations
- 32KB batch write at 10MB/s SD → ~3ms → well within tolerance
- If watchdog fires, increase `configFRTOS_TASK_WATCHDOG_TIMEOUT_TICKS` in sdkconfig

---

## 6. Memory (IRAM) Considerations

### What Goes in IRAM

The I2S ISR (interrupt service routine) must be in IRAM to avoid cache misses:

```c
// In sdkconfig:
// CONFIG_ESP_INTR_ALLOC_FLAGS=ESP_INTR_FLAG_IRAM
// CONFIG_DRIVER_I2S_ISR_IN_IRAM=y
```

### What Doesn't Need IRAM

- `recorder_task` — runs from flash, SD writes don't need IRAM
- `ringbuf.c` — FreeRTOS ring buffer ISR is already in IRAM
- `audio_read()` body — runs from flash, only ISR needs IRAM

---

## 7. Cooperative Multitasking Rules

1. **Every task loop must have a `vTaskDelay`** — tasks that run forever without yielding will starve other tasks
2. **Minimum delay:** 10ms — shorter delays waste CPU on context switching
3. **SD writes in recorder_task** — never in audio_task
4. **No blocking I/O in callbacks** — event callbacks run synchronously

---

## 8. Stack Sizing

| Task | Stack | Usage | Watermark |
|------|-------|-------|-----------|
| audio | 8192 | audio_read, RMS calc, ringbuf send | Monitored by system_monitor |
| recorder | 8192 | ringbuf recv, batch write, WAV header | Monitored by system_monitor |
| ui | 4096 | LED pattern state machine | Monitored by system_monitor |

**Stack overflow detection:** `configCHECK_FOR_STACK_OVERFLOW = 1` in FreeRTOS config. On overflow, `vApplicationStackOverflowHook()` is called.

---

*Last updated: 2026-05-14*
