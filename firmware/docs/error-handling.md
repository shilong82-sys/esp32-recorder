# 错误处理规范 — ESP32 AI Recorder

> Version: v0.1 | Updated: 2026-05-12
> 本文档定义统一的错误处理策略，确保系统可恢复、可诊断、可维护。

---

## 1. 错误码规则

### 1.1 ESP-IDF 原生错误码

优先使用 `esp_err_t` 标准错误码，不自行定义新错误码（除非必要）。

| 错误码 | 含义 | 使用场景 |
|--------|------|---------|
| `ESP_OK` | 成功 | 所有 API 成功返回 |
| `ESP_FAIL` | 通用失败 | 无更具体错误码可用时 |
| `ESP_ERR_NO_MEM` | 内存不足 | RingBuffer 分配失败、malloc 失败 |
| `ESP_ERR_INVALID_STATE` | 状态错误 | 重复调用 `recorder_start()`、在未初始化时调用 API |
| `ESP_ERR_NOT_FOUND` | 未找到 | 文件不存在、WiFi 未连接 |
| `ESP_ERR_TIMEOUT` | 超时 | SD 卡写入超时、WiFi 连接超时 |
| `ESP_ERR_INVALID_ARG` | 参数非法 | NULL 指针、采样率不支持 |

### 1.2 自定义错误码（必要时）

若标准错误码不够用，在对应组件头文件中定义：

```c
/* recorder.h */
#define RECORDER_ERR_WAV_HEADER  (0x5001)  /* WAV 头写入失败 */
#define RECORDER_ERR_DMA_START   (0x5002)  /* I2S DMA 启动失败 */

/* 使用：返回自定义错误码时，用 esp_err_t 承载 */
esp_err_t ret = RECORDER_ERR_WAV_HEADER;
```

> **原则**：自定义错误码范围应避开 ESP-IDF 已占用的 `0x0000~0x00FF`，建议使用 `0x5000~0x50FF`（recorder）、`0x5100~0x51FF`（uploader）等。

---

## 2. ESP_ERR_xxx 使用规范

### 2.1 返回错误码

```c
/* ✅ 正确：检查返回值，逐级传递错误 */
esp_err_t ret = storage_mount("/sdcard");
if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Mount failed: %s", esp_err_to_name(ret));
    return ret;  /* 向上传递，不 swallow */
}

/* ❌ 错误：忽略返回值 */
storage_mount("/sdcard");  /* 编译不报错，但隐藏问题 */
```

### 2.2 ESP_ERROR_CHECK() 使用场景

`ESP_ERROR_CHECK()` 在错误时打印错误码并重启（abort）。

| 场景 | 是否使用 | 理由 |
|------|---------|------|
| 初始化失败（NVS、event_bus、state）| ✅ 是 | 初始化失败无法继续，重启是合理的 |
| 运行时错误（SD 卡写入失败）| ❌ 否 | 应优雅降级，不重启 |
| `esp_timer_create()` 等系统 API | ✅ 是 | 参数错误是程序 bug，应尽早暴露 |

```c
/* ✅ 正确：初始化用 ESP_ERROR_CHECK */
ESP_ERROR_CHECK(nvs_flash_init());
ESP_ERROR_CHECK(event_bus_init());

/* ✅ 正确：运行时错误自行处理 */
esp_err_t ret = storage_mount("/sdcard");
if (ret != ESP_OK) {
    ESP_LOGW(TAG, "SD card not available, running without storage");
    /* 继续执行，功能降级 */
}
```

---

## 3. Recoverable / Unrecoverable 分类

### 3.1 Recoverable（可恢复错误）

| 错误类型 | 处理策略 | 状态迁移 |
|---------|---------|---------|
| SD 卡写入失败（临时）| 重试 3 次；仍失败 → 停止录音，发布 `EVENT_STORAGE_ERROR` | `RECORDING` → `ERROR` |
| WiFi 断开 | 自动重连（ESP-IDF 内部）；重连失败 → 停止上传，回到 `IDLE` | `UPLOADING` → `IDLE` |
| 上传失败（HTTP 超时）| 重试 3 次；仍失败 → 回到 `IDLE`，保留文件在 `upload_queue/` | `UPLOADING` → `IDLE` |
| 音频 buffer 溢出（drop）| 统计 drop 计数；超过阈值 → 停止录音 | `RECORDING` → `ERROR` |

### 3.2 Unrecoverable（不可恢复错误）

| 错误类型 | 处理策略 | 状态迁移 |
|---------|---------|---------|
| NVS 初始化失败 | `ESP_ERROR_CHECK()` → 重启 | `INIT` → `abort()` |
| event_bus 初始化失败 | 同上 | `INIT` → `abort()` |
| I2S 外设错误（硬件故障）| 发布 `EVENT_RECORDING_ERROR` → 进入 `ERROR`，等待用户重启 | `RECORDING` → `ERROR` |
| Watchdog 触发 | 系统自动重启 | — |

---

## 4. Storage Error 策略

### 4.1 错误分级

| 级别 | 含义 | 处理 |
|------|------|------|
| Warning | SD 卡写入偶尔失败（可重试成功）| 重试 3 次；记录日志 |
| Error | SD 卡卸载（物理移除）| 停止录音；发布 `EVENT_STORAGE_REMOVED`；进入 `ERROR` |
| Critical | SD 卡挂载失败（硬件故障）| 禁止录音功能；LED 错误码闪烁（1 闪）|

### 4.2 处理流程

```
storage_mount() 失败
  → ESP_LOGW(TAG, "SD not available")
  → event_bus_publish(EVENT_STORAGE_ERROR)
  → 功能降级：允许运行，但不录音

录音中 f_write() 失败
  → 重试 3 次（每次间隔 100ms）
  → 仍失败
    → 停止录音（不保存当前文件）
    → event_bus_publish(EVENT_RECORDING_ERROR)
    → state_set(DEVICE_STATE_ERROR)
```

---

## 5. Audio Overflow 策略

### 5.1 检测

Recorder Task 从 RingBuffer 读取时，检查 `xRingbufferGetCurFreeSize()` 或检测数据是否丢失。

### 5.2 处理

```c
/* 在 Recorder Task 中 */
size_t free_space = xRingbufferGetCurFreeSize(s_ringbuffer);
if (free_space < MIN_FREE_THRESHOLD) {
    s_drop_count++;
    ESP_LOGW(TAG, "Audio drop! total=%lu", s_drop_count);
    if (s_drop_count > MAX_DROPS_PER_SESSION) {
        /* 过多 drop，停止录音 */
        event_bus_publish(EVENT_RECORDING_ERROR, &drop_ev, sizeof(drop_ev));
        recorder_stop(NULL);
    }
}
```

| Drop 次数 | 处理 |
|-----------|------|
| 0~5 次/分钟 | 记录日志，继续录音 |
| 5~20 次/分钟 | 发布 `EVENT_RECORDING_ERROR`（warning）|
| > 20 次/分钟 | 停止录音，进入 `ERROR` |

---

## 6. Watchdog 策略

### 6.1 Task Watchdog（ESP-IDF 默认启用）

| Task | 最大阻塞时间 | 风险 |
|------|------------|------|
| `audio_task` | < 10ms（ISR 上下文）| 不直接调用 FATFS，无风险 |
| `recorder_task` | < 100ms（SD 卡写入峰值）| 若 SD 卡写入超过 100ms，触发 watchdog |
| `upload_task` | < 5000ms（HTTP 超时）| 使用 `HTTPC_TIMEOUT_MS`，不阻塞 forever |

### 6.2 防止 Watchdog 触发

```c
/* ✅ 正确：长时间操作中使用 vTaskDelay() 或分段处理 */
for (int i = 0; i < large_buffer_size; i += 512) {
    f_write(f, buffer + i, 512, &written);
    if (i % 4096 == 0) vTaskDelay(1);  /* 让出 CPU，喂狗 */
}

/* ❌ 错误：长时间循环无 vTaskDelay */
for (int i = 0; i < very_large_size; i++) {
    f_write(...);  /* 可能触发 watchdog */
}
```

### 6.3 IDLE 任务 Watchdog

若 main 循环的 `while(1)` 中没有 `vTaskDelay()`，会触发 IDLE watchdog。

```c
/* ✅ 正确 */
while (1) {
    vTaskDelay(pdMS_TO_TICKS(5000));
    /* ... */
}

/* ❌ 错误 */
while (1) {
    /* 无 delay，IDLE watchdog 触发 */
}
```

---

## 7. assert 使用原则

### 7.1 使用场景

| 场景 | 是否使用 assert | 理由 |
|------|----------------|------|
| 参数合法性检查（开发阶段）| ✅ 是 | 尽早发现 bug |
| 运行时错误（外部输入）| ❌ 否 | 用 `if + return error` 处理 |
| 硬件初始化失败 | ❌ 否 | 用 `if + ESP_LOGE + return` 处理 |
| 内存分配失败 | ❌ 否 | 用 `if (ptr == NULL)` 处理 |

### 7.2 示例

```c
/* ✅ 正确：开发阶段用 assert 检查不变量 */
void recorder_start(const char *filename) {
    assert(s_initialized);  /* 未初始化是程序 bug */
    assert(!s_recording);   /* 重复开始是逻辑错误 */
    /* ... */
}

/* ✅ 正确：运行时错误用返回值 */
esp_err_t ret = f_open(...);
if (ret != FR_OK) {
    ESP_LOGE(TAG, "File open failed: %d", ret);
    return ESP_FAIL;
}
```

### 7.3 Release 构建

`NDEBUG` 宏定义时，`assert()` 被定义为空（无运行开销）。

```cmake
/* CMakeLists.txt 中 */
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    $<$<CONFIG:Release>:NDEBUG>
)
```

---

## 8. 错误处理代码模板

### 8.1 初始化函数模板

```c
esp_err_t xxx_init(void) {
    if (s_initialized) return ESP_OK;  /* 幂等 */

    esp_err_t ret = ESP_OK;

    /* 步骤 1 */
    ret = some_setup();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Step 1 failed: %s", esp_err_to_name(ret));
        goto cleanup;
    }

    /* 步骤 2 */
    ret = another_setup();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Step 2 failed: %s", esp_err_to_name(ret));
        goto cleanup;
    }

    s_initialized = true;
    return ESP_OK;

cleanup:
    /* 逆序注销已初始化的部分 */
    return ret;
}
```

### 8.2 运行时 API 模板

```c
esp_err_t recorder_start(const char *filename) {
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (s_recording)    return ESP_ERR_INVALID_STATE;

    esp_err_t ret = ESP_OK;

    /* 操作 1 */
    ret = internal_prepare(filename);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Prepare failed: %s", esp_err_to_name(ret));
        return ret;  /* 快速返回，不执行后续操作 */
    }

    /* 操作 2 */
    ret = internal_start_dma();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "DMA start failed: %s", esp_err_to_name(ret));
        internal_cleanup();  /* 回滚操作 1 */
        return ret;
    }

    s_recording = true;
    return ESP_OK;
}
```

---

## 9. 设计原则总结

1. **错误不 swallow**：检查所有 `esp_err_t` 返回值，不允许忽略。
2. **初始化用 `ESP_ERROR_CHECK()`**：初始化失败无法继续，重启是合理的。
3. **运行时错误优雅降级**：不重启，进入 `ERROR` 状态等待用户恢复。
4. **错误码用标准 `esp_err_t`**：只有标准码不够时才自定义，且要文档化。
5. **Watchdog 不触发**：长时间操作（SD 写入、网络请求）中定期 `vTaskDelay(1)` 喂狗。
6. **assert 只用于开发阶段**：Release 构建中 assert 为空，不影响性能。

---

*文档版本：v0.1 | 作者：AI Assistant | 审核：待定*
