# Realtime Audio Path Rules — ESP32 AI Recorder

> **Version:** v0.3 | **Updated:** 2026-05-14
> **Scope:** Rules governing the audio capture path from I2S mic to recording
> **Architecture Fix:** audio_task = sole I2S owner, no suspend/resume

---

## 1. Core Principle

The audio capture path is a **hard realtime pipeline**. Every rule exists to prevent:
- Audio dropouts (skips, silence, crackling)
- Buffer overflows (samples lost)
- Buffer underruns (gaps in recording)
- Task starvation (recording task blocked)

---

## 2. Hard Rules (Non-Negotiable)

### Rule 1: Audio Task Never Blocks on FATFS

**Do:**
- Audio data flows through a lock-free ring buffer to a separate recorder task
- SD card writes happen only in `recorder_task`, never in `audio_task`

**Don't:**
- No `fopen`, `fwrite`, `fclose` in `audio.c`
- No `vTaskDelay`, `vTaskSuspend`, mutex locks in audio path
- No `ESP_LOG*` with non-DEBUG level in hot path

### Rule 2: No Dynamic Allocation in Audio Path

**Do:**
- All buffers are static (compile-time fixed)
- Ring buffer is pre-allocated at init time

**Don't:**
- No `malloc`, `free`, `pvPortMalloc` in audio read path
- No `strdup`, `snprintf` in audio callback or ISR

### Rule 3: No Mutex in Realtime Path

**Do:**
- Use lock-free FreeRTOS ring buffer (`xRingbufferCreate` with `RINGBUF_MODE_NO_SMP`)
- Use atomic operations for counters

**Don't:**
- No `xSemaphoreTake` in `audio_read()` or audio ISR
- No blocking `xQueueReceive` in audio task

### Rule 4: No Frequent Logging in Audio Path

**Do:**
- `ESP_LOGD` (DEBUG) for audio read metrics, throttled to every ~5 seconds
- `ESP_LOGW` only on error conditions

**Don't:**
- `ESP_LOGI` in per-frame audio path
- `ESP_LOGE` on every I2S timeout (I2S timeout is expected when silent)
- Any logging in audio ISR

### Rule 5: Audio Read Must Be Non-Blocking

**Do:**
- `i2s_channel_read()` with a short timeout (100ms max)
- Return 0 gracefully on timeout

**Don't:**
- `i2s_channel_read()` with portMAX_DELAY
- Infinite loops waiting for audio data

---

## 3. Task Boundaries

```
┌─────────────────────────────────────────────────────┐
│                    audio_task                        │
│  (priority 3, pinned to core 0)                      │
│                                                      │
│  loop:                                               │
│    audio_read() → calculate RMS → ringbuf_send()     │
│    vTaskDelay(100ms)                                 │
│                                                      │
│  ⚠️ NEVER: SD writes, logging, blocking I/O          │
└─────────────────────────────────────────────────────┘
                         │ ringbuf
                         ▼
┌─────────────────────────────────────────────────────┐
│                   recorder_task                      │
│  (priority 3, pinned to core 0)                      │
│                                                      │
│  loop:                                               │
│    ringbuf_receive() → batch_accumulate → fwrite()  │
│    vTaskDelay(50ms)                                 │
│                                                      │
│  ✅ ALLOWED: SD writes, FATFS, logging               │
└─────────────────────────────────────────────────────┘
```

**Core invariant:** `audio_task` 是 I2S 的唯一 owner，永远运行，禁止 suspend/resume。`recording` 状态仅改变 `ringbuf_enabled` 标志（audio → recorder 数据流向开关）。

---

## 4. Buffer Sizing

| Buffer | Size | Rationale |
|--------|------|-----------|
| DMA buffer | 6KB (6 desc × 256 frames) | ~96ms latency at 16kHz |
| Ring buffer | 32KB | Covers ~2s of audio for write bursts |
| Recorder batch | 32KB | Accumulates ~3.2s before SD write |
| Recorder stack | 8KB | Sufficient for WAV header + stats |

**Overflow handling:** When ring buffer is full, new samples are dropped and `overflow_count` is incremented. Recording continues — brief overflows do not stop recording.

---

## 5. I2S Resource Sharing

**规则：audio_task 是 I2S 的唯一 owner，永远运行。**

- I2S 硬件只能由 `audio.c` 调用
- `recorder.c` 永远不调用任何 I2S API
- `vTaskSuspend(audio_task)` 是 P0 禁止项
  - 会导致 DMA timing discontinuity
  - 会导致 I2S ownership 不稳定
  - 会导致后续 VAD/PTT/streaming 无法扩展

**Recording 状态切换逻辑：**

| 状态 | audio_task | ringbuf_enabled | recorder_task |
|------|-----------|-----------------|---------------|
| IDLE | 运行（读 I2S） | false | 等待（空循环） |
| RECORDING | 运行（读 I2S + 写 ringbuf） | true | 运行（消费 ringbuf） |

---

## 6. Error Handling

| Error | Behavior |
|-------|----------|
| I2S timeout (no audio) | `audio_read()` returns 0, no error logged |
| Ring buffer full | Drop samples, increment `overflow_count` |
| SD write fails | Log error, set `write_failures`, continue recording |
| SD card removed | `fwrite()` fails → enter ERROR state via `EVENT_STORAGE_ERROR` |

---

## 7. Monitoring

Audio path health is monitored by `recorder_task` and reported every 5 seconds:

```
I (xxx) recorder: [Audio Stats] total_samples=%lu overflow=%lu dropped=%lu
I (xxx) recorder: [Write Stats] failures=%lu avg_latency_us=%lld max_latency_us=%lld
```

---

## 8. Configuration

| Parameter | Value | Notes |
|-----------|-------|-------|
| Audio sample rate | 16000 Hz | Fixed |
| Audio bit width | 16-bit | Fixed |
| Audio channel | Mono | Left channel only |
| I2S timeout | 100ms | Per-read timeout |
| Ring buffer size | 32KB | ~2 seconds |
| Batch write size | 32 × 1600 samples | ~3.2s per SD write |
| Checkpoint sync | 30s | fflush/sync interval |

---

## 9. Filesystem Ownership (P0 强制)

**Filesystem layout is owned exclusively by `storage.c`.**

### Storage subsystem responsibilities

| Responsibility | Function |
|---------------|----------|
| Mount/unmount | `storage_mount()`, `storage_unmount()` |
| Directory creation | `storage_ensure_directories()` → `f_mkdir("0:/...")` |
| Directory validation | `storage_validate_layout()` → `f_opendir()` + `[OK]` log |
| File delete | `storage_delete_file()` → `f_unlink("0:/...")` |

### Recorder subsystem responsibilities

| Responsibility | Function |
|---------------|----------|
| File write | `fopen("/sdcard/...")` → `fwrite()` → `fclose()` |
| Session scan | `f_opendir("0:/recordings")` → `f_readdir()` (read-only) |
| Directory creation | **FORBIDDEN** |
| Directory check | **FORBIDDEN** |

### Forbidden (P0)

```c
// ❌ recorder.c: must NOT call these
f_mkdir("0:/anything");        // storage.c owns directory lifecycle
f_opendir("0:/recordings");     // only for session scan, not creation

// ❌ Never mix POSIX and FatFs-native paths
f_mkdir("/recordings");         // Wrong: FatFs doesn't understand /sdcard prefix
f_mkdir("/sdcard/recordings");  // Wrong: FatFs interprets as FatFs-root path
f_mkdir("0:/recordings");       // ✅ Correct FatFs-native path
```

### Correct recorder pattern

```c
// ✅ recorder.c: only file I/O
FILE *f = fopen("/sdcard/recordings/REC_SESSION_0001.wav", "wb");
fwrite(pcm_data, 1, size, f);
fflush(f);
fclose(f);
// Directory is guaranteed to exist by storage_ensure_directories().
```

See `docs/storage-path-policy.md` for complete path layer separation rules.

---

## 10. Timer Callback Rules (P1 强制)

esp_timer 回调运行在 `esp_timer_task` 栈上（**默认 3.5KB，是系统最小栈**）。

### 禁止在 esp_timer 回调中执行

| 操作 | 原因 |
|------|------|
| `vTaskList()` | ~1.5KB 栈消耗，超出 esp_timer 栈容量，导致崩溃 |
| `uxTaskGetSystemState()` | 同上 |
| `heap_caps_print()` | 大量格式化输出 |
| `vTaskGetCpuUsage()` | 内部使用 mutex 和动态分配 |
| 任何 blocking 调用 | 可能导致 timer task 饥饿 |
| `ESP_LOGI/ESP_LOGE`（大量） | 日志格式化栈消耗 |

### Timer callback 正确做法

```c
// ❌ 错误：直接调用 vTaskList（崩溃）
static void bad_timer_cb(void *arg) {
    char buf[2048];
    vTaskList(buf);  // P1 CRASH: 栈溢出
    ESP_LOGI(TAG, "%s", buf);
}

// ✅ 正确：只发送信号，重操作在专用任务中
static SemaphoreHandle_t s_dump_sem;

static void timer_callback(void *arg) {
    BaseType_t higher_wake = pdFALSE;
    xSemaphoreGiveFromISR(s_dump_sem, &higher_wake);
    if (higher_wake) portYIELD_FROM_ISR();  // 立即触发 monitor_task
}

static void monitor_task(void *arg) {
    while (1) {
        xSemaphoreTake(s_dump_sem, portMAX_DELAY);
        char buf[2048];
        vTaskList(buf);   // ✅ 安全：monitor_task 栈 = 4KB
        ESP_LOGI(TAG, "%s", buf);
    }
}
```

### 规则总结

- **Timer callback = 轻量信号发射器**，只做：`xSemaphoreGiveFromISR()`, 标志设置, 时间戳更新
- **重操作 = 专用任务**，栈 ≥ 4KB，专门接收 timer 信号后执行诊断
- **system_monitor** 已按此架构重构（monitor_task + timer callback 二元信号量）

---

*Last updated: 2026-05-14*
