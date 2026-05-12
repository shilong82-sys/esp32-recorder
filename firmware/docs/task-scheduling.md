# Task Scheduling 策略 — ESP32 AI Recorder

> Version: v0.1 | Created: 2026-05-12
> 本文档定义 FreeRTOS Task 优先级策略，避免 priority inversion、audio starvation 等问题。

---

## 1. Task 优先级总览

### 1.1 优先级定义表

| Task | 优先级 | 堆栈大小 | 核心绑定 | 原因 |
|------|--------|---------|---------|------|
| **Audio Task** | 3 | 4096B | Core 0 | 实时音频数据必须持续处理 |
| **Recorder Task** | 3 | 8192B | Core 0 | SD 写入不能阻塞 Audio |
| **UI Task** | 2 | 8192B | Core 1 | LED/按键响应，可容忍少量延迟 |
| **WiFi Manager Task** | 2 | 4096B | 任意 | WiFi 事件处理，可等待 |
| **Upload Task** | 2 | 8192B | 任意 | 网络传输，非实时 |
| **Logger Task** | 1 | 4096B | 任意 | 后台日志，可低优先级 |

### 1.2 优先级层次

```
┌─────────────────────────────────────────────────────────────┐
│                   Priority Level 3 (Highest)                │
│  ┌─────────────────────┐    ┌─────────────────────┐        │
│  │   Audio Task        │    │   Recorder Task    │        │
│  │   (I2S 数据处理)     │    │   (SD 卡写入)       │        │
│  └─────────────────────┘    └─────────────────────┘        │
│  ⚠️ 这两个 task 必须在 Audio 不丢数据的前提下运行            │
└─────────────────────────────────────────────────────────────┘
                            ↓
┌─────────────────────────────────────────────────────────────┐
│                   Priority Level 2                          │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐         │
│  │ UI Task     │  │ WiFi Mgr   │  │ Upload Task │         │
│  │ (LED/按键)  │  │ (网络连接)  │  │ (HTTP上传)  │         │
│  └─────────────┘  └─────────────┘  └─────────────┘         │
│  ⚠️ 绝对不能在 Audio 运行时抢占资源                          │
└─────────────────────────────────────────────────────────────┘
                            ↓
┌─────────────────────────────────────────────────────────────┐
│                   Priority Level 1 (Lowest)                 │
│  ┌─────────────┐                                          │
│  │ Logger Task│  (后台日志，可随时被高优先级任务抢占)         │
│  └─────────────┘                                          │
└─────────────────────────────────────────────────────────────┘
```

---

## 2. 为什么 Audio Task 必须高优先级

### 2.1 实时性约束

```
Audio 数据流（16kHz / 16-bit / mono）：

每秒数据量：16000 samples × 2 bytes = 32KB
每 ms 数据量：16 samples × 2 bytes = 32 bytes
每 16ms 数据量：256 samples × 2 bytes = 512 bytes (一个 DMA buffer)

关键约束：
- I2S DMA buffer 满 → 新数据覆盖旧数据（数据丢失）
- Audio Task 处理延迟 > 16ms → RingBuffer 积压
- Audio Task 处理延迟 > 64ms → RingBuffer 溢出 → audio drop

⚠️ Audio Task 被任何低优先级 task 阻塞 64ms = audio drop
```

### 2.2 阻塞来源分析

| 阻塞场景 | 持续时间 | 风险 |
|---------|---------|------|
| SD 卡写入（f_write）| 5~50ms | 低（有 RingBuffer 缓冲）|
| WiFi 操作（esp_wifi_xxx）| 10~100ms | **高**（可能阻塞）|
| `vTaskDelay()` | 可配置 | 可接受 |
| Mutex 等待 | 不确定 | **高**（可能导致 priority inversion）|

---

## 3. 为什么 Upload Task 不能高优先级

### 3.1 网络操作的性质

```
HTTP 上传特点：
- 非实时：录音文件可以等待几秒再上传
- 不可预测：WiFi 延迟可能波动 100~500ms
- 可能阻塞：TCP 重传、WiFi 切换等情况下可能长时间阻塞

如果 Upload Task 优先级 = 3：
- Upload 可能在 Audio Task 运行时抢占 CPU
- 导致 Audio Task 处理延迟 → RingBuffer 溢出 → audio drop

正确的设计：
Upload Task 优先级 = 2（低于 Audio）
当 Audio 需要 CPU 时，Upload 自动让出
```

### 3.2 Upload Task 与 Audio Task 的关系

```
时间轴：

情况 A：Upload 优先级 = 3（错误）
────────────────────────────────────────────────────────────
Audio:  [====处理====][====处理====][====处理====]
Upload:                    [====HTTP上传====]
                                      ↑
                              Upload 抢占 CPU
                              Audio 延迟处理
                              ⚠️ RingBuffer 积压
────────────────────────────────────────────────────────────

情况 B：Upload 优先级 = 2（正确）
────────────────────────────────────────────────────────────
Audio:  [====处理====][====处理====][====处理====]
Upload:       [==上传==]    [==上传==]    [==上传==]
                  ↑           ↑           ↑
            Audio 运行时 Upload 让出 CPU
            ✅ RingBuffer 保持健康
────────────────────────────────────────────────────────────
```

---

## 4. FreeRTOS Priority Inheritance 风险

### 4.1 什么是 Priority Inheritance

```
FreeRTOS 的 Mutex 使用 priority inheritance 机制：

问题场景：
1. Audio Task（优先级 3）等待 Mutex M
2. Recorder Task（优先级 3）持有 Mutex M，但被 Upload Task（优先级 2）抢占
3. Upload Task 执行耗时操作，Audio Task 一直等待

没有 Priority Inheritance：
Audio Task 优先级 3 > Upload Task 优先级 2
但 Upload Task 正在运行，Audio Task 被阻塞

有了 Priority Inheritance：
Upload Task 临时提升到优先级 3
Upload Task 快速完成，释放 Mutex M
Recorder Task 继续，Audio Task 获得 Mutex
Upload Task 恢复到优先级 2
```

### 4.2 Priority Inheritance 的陷阱

```
⚠️ Priority Inheritance 不是万能的！

陷阱 1：长时间持有 Mutex
────────────────────────────────────────────────────────────
Recorder Task 持有 Mutex M，执行长时间的 SD 卡操作
Upload Task 因 priority inheritance 被提升到优先级 3
Audio Task 等待 Mutex M
结果：整个高优先级链被阻塞

正确做法：Mutex 只保护临界区的"一瞬间"，不要在持有 Mutex 时执行 SD/网络操作

陷阱 2：Multiple Mutex 嵌套
────────────────────────────────────────────────────────────
Task A（优先级 3）等待 Mutex M1（持有者优先级 2）
Task B（优先级 2）等待 Mutex M2（持有者优先级 1）
Task C（优先级 1）持有 M2，等待 M1

优先级链：C→B→A
可能导致"优先级反转"（priority inversion）

正确做法：避免 Mutex 嵌套，或使用替代方案（消息队列）
```

### 4.3 推荐的同步机制

| 场景 | 推荐机制 | 理由 |
|------|---------|------|
| 保护共享变量 | `xSemaphoreHandle` (binary) | 简单，不易出错 |
| 保护 SD 卡文件 | **不要用 Mutex** | SD 卡操作可能阻塞太久 |
| 保护 WiFi 连接 | `xSemaphoreHandle` + timeout | WiFi 操作相对快 |
| Task 间传递数据 | **消息队列**（推荐）| 无阻塞风险 |
| 中断与 Task 通信 | `xQueueSendFromISR()` | ISR-safe |

```
最佳实践：

❌ 不要这样做：
mutex_t mutex;
void recorder_write(uint8_t *data, size_t len) {
    mutex_lock(&mutex);      // 获取锁
    f_write(file, data, len);
    f_sync(file);            // 同步，可能阻塞 50ms！
    mutex_unlock(&mutex);
}

✅ 推荐这样做：
QueueHandle_t write_queue;

void recorder_task(void *arg) {
    uint8_t buffer[4096];
    while (1) {
        xQueueReceive(write_queue, buffer, portMAX_DELAY);
        f_write(file, buffer, sizeof(buffer));  // 不持有锁，不阻塞
    }
}

void isr_or_high_priority_code(uint8_t *data) {
    xQueueSendFromISR(write_queue, data);  // ISR-safe
}
```

---

## 5. Mutex 使用风险清单

### 5.1 禁止事项

```
🚫 绝对禁止：

1. 在持有 Mutex 时调用 f_write()/f_read()
2. 在持有 Mutex 时调用 esp_wifi_xxx()
3. 在持有 Mutex 时调用任何可能阻塞的 API
4. Mutex 持有时间超过 1ms
5. 在 ISR 中使用 Mutex
```

### 5.2 允许事项

```
✅ 允许：

1. 保护简单的共享变量（计数器、标志位）
   mutex_lock(&counter_mutex);
   counter++;
   mutex_unlock(&counter_mutex);

2. 保护快速的配置更新
   mutex_lock(&config_mutex);
   g_sample_rate = 16000;
   mutex_unlock(&config_mutex);

3. 使用带有 timeout 的 mutex_lock
   if (mutex_lock_timeout(&mutex, 100) == ESP_OK) {
       // 保护操作
       mutex_unlock(&mutex);
   } else {
       // 超时处理
   }
```

---

## 6. Task 间通信设计

### 6.1 推荐的消息流

```
┌─────────────────────────────────────────────────────────────┐
│                      Event Bus (推荐)                       │
│                                                              │
│  所有 Task 通过 event_bus 通信，无直接函数调用                │
│  发布者 → Event Bus → 订阅者                                 │
│                                                              │
│  ✅ 优点：解耦、无 mutex 风险、易于调试                      │
└─────────────────────────────────────────────────────────────┘

示例：
Event_Bus_Publish(EVENT_BUTTON_CLICKED, NULL);
    ↓
Event_Bus_Subscribe(EVENT_BUTTON_CLICKED, on_button_clicked);
    ↓
on_button_clicked() {
    if (state_get() == DEVICE_STATE_IDLE) {
        state_set(DEVICE_STATE_RECORDING);
    }
}
```

### 6.2 需要直接通信的场景

| 场景 | 通信方式 | 理由 |
|------|---------|------|
| Audio → Recorder 数据传递 | RingBuffer | 高性能、零拷贝可能 |
| Recorder → Storage 文件操作 | 消息队列（可选）| 避免直接耦合 |
| UI → State | Event Bus | 解耦 |
| Upload → WiFi | Event Bus + 状态查询 | 解耦 |

---

## 7. 调试与监控

### 7.1 优先级相关调试

```c
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* 打印所有任务的优先级和栈使用 */
void dump_task_info(void) {
    UBaseType_t count = uxTaskNumberGet();
    TaskStatus_t *status = malloc(count * sizeof(TaskStatus_t));
    uint32_t total_runtime;

    vTaskGetTasksStatus(status, &count, &total_runtime);

    ESP_LOGI(TAG, "=== Task Info ===");
    for (UBaseType_t i = 0; i < count; i++) {
        ESP_LOGI(TAG, "%s: prio=%d, stack=%d/%d",
                 status[i].pcTaskName,
                 status[i].uxCurrentPriority,
                 status[i].usStackHighWaterMark,
                 configMINIMAL_STACK_SIZE);
    }
    free(status);
}
```

### 7.2 Audio Starvation 检测

```c
static int64_t s_last_audio_process_time = 0;

void audio_task_main(void *arg) {
    while (1) {
        int64_t now = esp_timer_get_time();

        /* 检查是否发生 starvation */
        if (s_last_audio_process_time > 0) {
            int64_t elapsed = now - s_last_audio_process_time;
            if (elapsed > 20 * 1000) {  /* > 20ms */
                ESP_LOGW(TAG, "⚠️ Audio starvation detected: %lld ms", elapsed / 1000);
            }
        }

        s_last_audio_process_time = now;
        /* 处理音频数据... */
    }
}
```

---

## 8. 配置检查清单

```
在 sdkconfig 中确认：

CONFIG_FREERTOS_HZ=1000                    ✅ System tick = 1ms
CONFIG_FREERTOS_UNICODE=1                   ✅ 日志输出可读
CONFIG_FREERTOS_CHECK_STACKOVERFLOW=1        ✅ 栈溢出检测
CONFIG_FREERTOS_WATCHPOINT_STMFLASH=n       ⚠️ 调试用，生产关闭

在代码中确认：

1. Audio Task 优先级 = configMAX_PRIORITIES - 1 (或 3)
2. Recorder Task 优先级 = Audio Task 同级或略低
3. Upload Task 优先级 < Audio Task
4. 任何可能 > 5ms 的操作不在 Mutex 临界区内
5. 使用消息队列替代直接函数调用
```

---

*文档版本：v0.1 | 作者：AI Assistant | 审核：待定*
