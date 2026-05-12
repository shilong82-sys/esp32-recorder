# Coding Style & 日志规范 — ESP32 AI Recorder

> Version: v0.1 | Updated: 2026-05-12
> Framework: ESP-IDF v5.2 / C99 / FreeRTOS

---

## 1. 日志等级规范（Log Level Specification）

### 1.1 日志等级总览

| Level | Macro | 用途 | 是否允许高频打印 | 默认使能 |
|-------|-------|------|----------------|---------|
| Error | `ESP_LOGE` | 功能阻塞级错误，需要用户干预 | ❌ 否（每次错误只打印 1 次）| ✅ 始终使能 |
| Warning | `ESP_LOGW` | 非预期情况，功能降级但继续运行 | ⚠️ 允许，但需限速（见 1.3）| ✅ 始终使能 |
| Info | `ESP_LOGI` | 正常操作、状态变化、初始化完成 | ❌ 否（状态变化只打印 1 次）| ✅ 始终使能 |
| Debug | `ESP_LOGD` | 周期性低频数据（≥ 1s 间隔）| ⚠️ 允许，默认关闭 | ❌ 默认关闭 |
| Verbose | `ESP_LOGV` | 高频数据（< 100ms 间隔）| ✅ 允许，但必须在 Release 中关闭 | ❌ 默认关闭 |

### 1.2 各级别详细规则

#### `ESP_LOGE` — 错误（必须处理）

```c
/* ✅ 正确：打印错误码和人类可读描述 */
ESP_LOGE(TAG, "Failed to mount SD card: %s", esp_err_to_name(ret));

/* ✅ 正确：错误只打印一次，不放在循环中 */
if (ret != ESP_OK) {
    ESP_LOGE(TAG, "I2S DMA start failed: 0x%x", ret);
    return ret;
}

/* ❌ 错误：在循环或高频上下文中打印 */
while (1) {
    ESP_LOGE(TAG, "retry...");  /* 会刷屏，且不影响错误本身 */
}
```

**使用场景**：
- 初始化失败（NVS、I2S、SD 卡）
- 文件操作失败（f_open 返回错误）
- 内存分配失败（malloc 返回 NULL）
- 不可恢复的状态机错误

---

#### `ESP_LOGW` — 警告（可接受，但需关注）

```c
/* ✅ 正确：限速警告 */
static uint32_t s_last_warn_time = 0;
uint32_t now = esp_timer_get_time() / 1000000;  /* 秒 */
if (now - s_last_warn_time >= 10) {  /* 每 10s 最多打印 1 次 */
    ESP_LOGW(TAG, "WiFi not connected, skip upload");
    s_last_warn_time = now;
}

/* ✅ 正确：一次性警告 */
ESP_LOGW(TAG, "Low battery (%d%%), entering LOW_BATTERY state", pct);
```

**使用场景**：
- WiFi 断开（自动重连中）
- SD 卡剩余空间 < 10MB
- 上传重试（打印当前重试次数）
- 电池电量低

---

#### `ESP_LOGI` — 信息（正常流程）

```c
/* ✅ 正确：状态变化打印（只 1 次）*/
ESP_LOGI(TAG, "State changed: %s -> %s",
         state_to_string(prev), state_to_string(curr));

/* ✅ 正确：初始化完成 */
ESP_LOGI(TAG, "Recorder initialized, sample_rate=%lu", cfg.sample_rate);

/* ❌ 错误：在循环或回调中打印（会刷屏）*/
void on_state_changed(event_type_t type, const void *data, size_t len, void *user) {
    ESP_LOGI(TAG, "state changed");  /* 每次状态变化都打印 → 滥用了 */
    /* 正确做法：状态变化打印应在 state_set() 内部，订阅者不应再打印 */
}
```

**使用场景**：
- 模块初始化完成
- 状态机状态变化（`state_set()` 内部打印）
- 文件创建/关闭
- 上传开始/完成

---

#### `ESP_LOGD` — 调试（开发阶段用，Release 关闭）

```c
/* ✅ 正确：周期性低频数据（≥ 1s）*/
ESP_LOGD(TAG, "Free heap: %lu, battery: %d%%",
         (unsigned long)esp_get_free_heap_size(), battery_get_percentage());

/* ⚠️ 谨慎：在音频回调中使用 */
/* 见第 2 节"Audio Task 特殊限制" */
```

**使用场景**：
- 堆内存监控（system_monitor）
- 电池电压原始值
- 录音文件列表

---

#### `ESP_LOGV` — 冗余（仅深度调试用）

```c
/* ✅ 正确：详细跟踪（默认关闭，开发者手动开启）*/
ESP_LOGV(TAG, "I2S DMA complete, buffer=%p, size=%d", buf, size);

/* ❌ 错误：在 I2S ISR 中调用 */
/* ISR 中调用 ESP_LOG* 会增加延迟，应避免 */
```

**使用场景**：
- I2S DMA 每次完成（高频）
- RingBuffer 每次读写
- FATFS 每次 f_write() 调用

---

### 1.3 高频日志限速模板

```c
/* 模板：每 N 秒最多打印 1 次 */
#define WARN_INTERVAL_S  10

void check_wifi(void) {
    static uint32_t s_last_print_s = 0;
    uint32_t now_s = esp_timer_get_time() / 1000000;

    if (now_s - s_last_print_s >= WARN_INTERVAL_S) {
        if (!wifi_manager_is_connected()) {
            ESP_LOGW(TAG, "WiFi still disconnected after %ds", WARN_INTERVAL_S);
            s_last_print_s = now_s;
        }
    }
}
```

---

## 2. Audio Task 特殊限制（重点）

Audio Task 运行在 core 0，优先级 3，每 100ms 触发一次。在此 task 中打印日志有严格要求：

### 2.1 禁止的日志操作

| 操作 | 原因 |
|------|------|
| `ESP_LOGI()` 在 audio_task 循环中 | 10 次/秒，刷屏且影响性能 |
| `ESP_LOGD()` 在每次 I2S 读取后 | 16000Hz 采样率下 1600 samples/100ms，太高 |
| `printf()` / `ets_printf()` | 绕过 ESP_LOG 等级控制， always 输出 |

### 2.2 允许的日志操作

```c
/* ✅ 正确：audio_task 中只允许 ESP_LOGD，且限速 */
static void audio_task(void *arg) {
    while (1) {
        int got = audio_read(buf, AUDIO_TASK_SAMPLES);
        float rms = audio_calculate_rms(buf, got);

        /* 每 1s 打印 1 次 RMS（debug 级别）*/
        static int s_count = 0;
        if (++s_count >= 10) {  /* 10 * 100ms = 1s */
            ESP_LOGD(TAG, "RMS: %.0f", (double)rms);
            s_count = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* ✅ 正确：错误情况允许 ESP_LOGE（但应是极少数）*/
if (got < 0) {
    ESP_LOGE(TAG, "I2S read error: %d", got);
}
```

### 2.3 Audio Task 日志等级推荐配置

```
开发阶段：
  make menuconfig → Component config → Log output → Default log verbosity → DEBUG
  允许看到 ESP_LOGD 输出，用于验证 I2S 数据正确性

Release 阶段：
  Default log verbosity → INFO
  ESP_LOGD / ESP_LOGV 自动被编译掉（零运行时开销）
```

---

## 3. TAG 命名规范

| 文件 | TAG 值 | 说明 |
|------|---------|------|
| `components/audio/audio.c` | `"audio"` | 组件名，小写 |
| `components/storage/storage.c` | `"storage"` | 同上 |
| `components/wifi_manager/wifi_manager.c` | `"wifi_mgr"` | 多个单词用下划线 |
| `main/app_main.c` | `"app_main"` | 主入口 |
| `components/event_bus/event_bus.c` | `"event_bus"` | 下划线分隔 |

```c
/* 每个 .c 文件顶部定义 */
static const char *TAG = "component_name";
```

---

## 4. 日志消息格式规范

```c
/* ✅ 正确：包含上下文信息 */
ESP_LOGI(TAG, "SD card mounted at %s (%.1f GB free)", mount_point, free_gb);
ESP_LOGE(TAG, "Failed to open file %s: %s", path, strerror(errno));

/* ❌ 错误：无上下文 */
ESP_LOGI(TAG, "done");
ESP_LOGE(TAG, "error");

/* ✅ 正确：错误码用 esp_err_to_name() 转换 */
ESP_LOGE(TAG, "I2S init failed: %s", esp_err_to_name(ret));

/* ✅ 正确：十六进制打印错误码（自定义错误码）*/
ESP_LOGE(TAG, "WAV header write failed: 0x%x", RECORDER_ERR_WAV_HEADER);
```

---

## 5. 命名约定（Naming Conventions）

### 5.1 函数命名

```c
/* 格式：<component>_<verb>[_<noun>]() */
esp_err_t storage_mount(const char *mount_point);
void      storage_unmount(void);
esp_err_t recorder_start(const char *filename);
bool      recorder_is_recording(void);

/* ✅ 返回 esp_err_t（可能失败的操作）*/
/* ❌ 返回 int（错误码不一致）*/

/* 静态函数：无前缀 */
static void IRAM_ATTR i2s_dma_callback(i2s_chan_handle_t chan, i2s_event_data_t *data, void *user_ctx) {
    /* ... */
}
```

### 5.2 类型与枚举

```c
/* 结构体：snake_case + _t 后缀 */
typedef struct {
    int sample_rate;
    int bits_per_sample;
} recorder_config_t;

/* 枚举：UPPER_SNAKE_CASE */
typedef enum {
    DEVICE_STATE_INIT = 0,
    DEVICE_STATE_IDLE,
    DEVICE_STATE_COUNT,   /* 总是最后，用于边界检查 */
} device_state_t;

/* 枚举值：MODULE_PREFIX_NAME */
/* ✅ DEVICE_STATE_IDLE */
/* ❌ IDLE、STATE_IDLE（缺少模块前缀）*/
```

### 5.3 常量与宏

```c
/* ✅ 命名常量：UPPER_SNAKE_CASE */
#define PIN_NUM_MOSI       11
#define RECORDER_STACK_SIZE 8192
#define MAX_WAV_FILES      32

/* ❌ 魔数 */
host.max_freq_khz = 20000;  /* 不知道 20000 代表什么 */
/* ✅ */
host.max_freq_khz = SDMMC_FREQ_DEFAULT;
```

### 5.4 变量

```c
/* 局部变量：snake_case */
int bytes_read = 0;
esp_err_t ret;

/* 模块级静态变量：s_ 前缀 */
static bool s_recording = false;
static SemaphoreHandle_t s_mutex = NULL;

/* 全局变量（避免！如必须使用）：g_ 前缀 */
/* ❌ 尽量避免全局变量 */
```

---

## 6. Task 命名与优先级规范

### 6.1 Task 命名

```c
/* ✅ 正确 */
xTaskCreate(audio_task,     "audio",      8192, NULL, 3, NULL);
xTaskCreate(upload_task,    "upload",     4096, NULL, 2, NULL);
xTaskCreate(monitor_task,   "sys_monitor", 2048, NULL, 1, NULL);

/* ❌ 错误 */
xTaskCreate(task1, "Task1", ...);  /* 无意义名称 */
```

### 6.2 优先级规则

| 优先级 | 用途 | 示例 |
|--------|------|------|
| 1 | 后台任务（监控、日志）| `sys_monitor` |
| 2 | 上传、WiFi 应用任务 | `upload_task` |
| 3 | 音频采集（时间敏感）| `audio_task` |
| 5 | 按钮去抖 | `button` 内部 timer |
| ≥ 23 | ESP-IDF WiFi 栈 | 不可使用（与 WiFi 冲突）|

```c
/* ❌ 错误：优先级 ≥ 23 */
xTaskCreate(task, "name", 4096, NULL, 23, NULL);  /* 可能与 WiFi 栈冲突 */

/* ✅ 正确：最高到 22 */
xTaskCreate(task, "name", 4096, NULL, 22, NULL);
```

---

## 7. FreeRTOS 使用原则

### 7.1 栈大小

```c
/* 最小可行栈大小 */
#define STACK_MIN_SIMPLE    2048   /* 无 printf 的简单任务 */
#define STACK_MIN_LOG       3072   /* 有 ESP_LOG* 的任务 */
#define STACK_AUDIO        8192   /* I2S + 音频处理 */
#define STACK_UPLOAD       6144   /* HTTP 客户端 */

/* 监控栈水位线 */
system_monitor_init(10000);  /* 每 10s 打印一次所有任务水位线 */
/* 若某任务水位线 < 512 bytes，增大其栈大小 */
```

### 7.2 延时

```c
/* ✅ 正确：用 pdMS_TO_TICKS() */
vTaskDelay(pdMS_TO_TICKS(100));  /* 100ms */

/* ❌ 错误：直接写 tick 数（依赖 configTICK_RATE_HZ）*/
vTaskDelay(100);  /* 不清楚是多少 ms */
```

### 7.3 同步原语

```c
/* ✅ 推荐：FreeRTOS queue 用于 task 间数据传递 */
QueueHandle_t q = xQueueCreate(10, sizeof(audio_frame_t));
xQueueSend(q, &frame, portMAX_DELAY);

/* ✅ 可用：Mutex 保护共享资源 */
SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
xSemaphoreTake(mutex, portMAX_DELAY);
/* 访问共享资源 */
xSemaphoreGive(mutex);

/* ❌ 禁止：在 ISR 中调用阻塞 API */
/* I2S DMA 回调（ISR 上下文）中不可调用 xSemaphoreTake() */
```

---

## 8. 模块边界原则

1. **HAL 组件**（`audio`、`led`、`button`、`storage`、`battery`）**不得**：
   - 了解业务逻辑
   - 直接调用 `state_set()` 或 `event_bus_publish()`（除 `storage`/`battery` 可发布硬件事件）
   - 引入业务组件头文件

2. **业务组件**（`recorder`、`uploader`、`wifi_manager`）**不得**：
   - 直接访问 GPIO
   - 直接使用硬件外设 API（应使用 HAL 组件 API）

3. **架构组件**（`event_bus`、`state`）**不得**：
   - 依赖任何其他自定义组件

4. **`app_main.c`** 是唯一允许：
   - 同时引入所有组件
   - 定义初始化顺序
   - 注册顶层事件订阅

---

## 9. 头文件规范

```c
/* ✅ 正确：头文件用 include guard */
#ifndef COMPONENT_NAME_H
#define COMPONENT_NAME_H

#ifdef __cplusplus
extern "C" {
#endif

/* 声明 */

#ifdef __cplusplus
}
#endif

#endif /* COMPONENT_NAME_H */
```

```c
/* ✅ 正确：.c 文件 include 顺序 */
#include "own_header.h"         /* 自身头文件（如 public API）*/
#include "esp_log.h"           /* ESP-IDF 头文件 */
#include "freertos/FreeRTOS.h" /* FreeRTOS 头文件 */
#include <stdint.h>            /* 标准 C 库 */
```

---

*文档版本：v0.1 | 作者：AI Assistant | 审核：待定*
