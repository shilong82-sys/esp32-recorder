# Realtime Rules — ESP32 AI Recorder

> Version: v0.1 | Created: 2026-05-12
> 本文档定义 ESP32 Recorder 的 realtime system engineering 规则。
> 目标：避免典型 embedded realtime 问题（丢音、SD stall、WiFi 抢占、jitter、fragmentation）。

---

## 1. Realtime Path Definition

### 1.1 层级定义

```
┌─────────────────────────────────────────────────────────────────┐
│                     Realtime Level Spectrum                      │
│                                                                 │
│  HARD REALTIME ────────────────────────────────── NON-REALTIME  │
│       │                                                        │
│       │  时序约束：无妥协空间                                     │
│       │  超过截止时间 = 系统失败                                   │
│       ▼                                                        │
│  ┌─────────────┐                                               │
│  │  I2S DMA    │  ← HARD REALTIME                               │
│  │  RingBuffer │  ← HARD REALTIME                               │
│  └─────────────┘                                               │
│       │                                                        │
│       ▼                                                        │
│  ┌─────────────┐                                               │
│  │Recorder Task│  ← SOFT REALTIME                               │
│  │  (SD 写入)  │  ← SOFT REALTIME                               │
│  └─────────────┘                                               │
│       │                                                        │
│       ▼                                                        │
│  ┌──────────────────────────────────────────────┐               │
│  │  UI Task  │  WiFi Manager  │  Upload Task  │ ← NON-REALTIME │
│  │  Logger    │  (非关键后台)                  │               │
│  └──────────────────────────────────────────────┘               │
└─────────────────────────────────────────────────────────────────┘
```

### 1.2 Realtime Level 详细定义

| 模块 | Realtime Level | Deadline | 超过后果 | 原因 |
|------|---------------|----------|---------|------|
| **I2S DMA refill** | 🔴 HARD | < 1 DMA period (16ms) | 数据覆盖，audio drop | DMA buffer 满则覆盖 |
| **Audio capture task** | 🔴 HARD | < 1 DMA period (16ms) | RingBuffer 积压 | 必须持续消费 DMA 数据 |
| **RingBuffer write** | 🔴 HARD | < 1 DMA period (16ms) | overflow，audio drop | ISR/TASK 生产数据不能停 |
| **Recorder task** | 🟡 SOFT | < RingBuffer 积压阈值 | 触发 backpressure | 可短暂延迟，累积后触发警告 |
| **SD write** | 🟡 SOFT | 无硬性截止 | RingBuffer 积压 | 吞吐量够即可，延迟可接受 |
| **WAV header update** | 🟡 SOFT | 录音停止时 | 文件不完整 | 可事后修复 |
| **UI task** | 🟢 NON | 无 | 无感知影响 | 用户不可见延迟 |
| **WiFi upload** | 🟢 NON | 无 | 重试即可 | 网络本来就不可预测 |
| **Logger** | 🟢 NON | 无 | 日志丢失可接受 | 不影响核心功能 |

### 1.3 关键澄清：Realtime Level ≠ Task Priority

```
⚠️ 常见误解：HARD REALTIME = 高优先级 task

这是错误的。

Realtime Level 描述的是：系统对时序错误的容忍度
Task Priority 描述的是：OS 调度器如何分配 CPU 时间片

关系：
- HARD REALTIME 组件通常需要较高优先级（但不是绝对）
- 高优先级 task 不一定是 HARD REALTIME（例如：高优先级但执行时间长的 task）
- 关键是：HARD REALTIME 路径中的操作必须在 deadline 前完成

示例：
Audio Task（优先级 3）是 HARD REALTIME
→ 必须在每个 DMA period 内完成数据消费
→ 如果被抢占超过 16ms，RingBuffer 开始积压

Upload Task（优先级 2）是 NON-REALTIME
→ 可以随时被 Audio Task 抢占
→ 网络延迟 500ms 完全可以接受
```

---

## 2. HARD REALTIME Rules

### 2.1 绝对禁止事项

```
🚫 HARD REALTIME 路径中严禁执行以下任何操作：

1. 内存分配/释放
   - malloc(), free(), realloc()
   - heap_caps_malloc()（除非确定耗时 < 1μs）
   - 原因：分配时间不确定，可能触发 heap compaction（阻塞数百 μs）

2. 任何日志输出
   - ESP_LOGI(), ESP_LOGW(), ESP_LOGE()
   - printf(), snprintf()
   - 原因：UART/USB 写入可能阻塞 1~10ms

3. 文件系统操作
   - f_open(), f_write(), f_read(), f_sync()
   - 原因：FATFS 可能阻塞 5~200ms

4. 网络操作
   - esp_wifi_xxx(), esp_http_client_xxx()
   - 原因：WiFi stack 可能阻塞 10~100ms

5. 阻塞同步原语
   - xSemaphoreTake()（不带 timeout 或 timeout 过长）
   - 原因：持有者可能执行耗时操作

6. 长临界区
   - 任何持有 mutex 超过 100μs 的代码
   - 原因：可能触发 priority inversion

7. 动态数据处理
   - JSON 序列化/反序列化
   - 动态大小 buffer 处理
   - 原因：处理时间不可预测

8. 动态字符串格式化
   - sprintf(), snprintf()（大 buffer）
   - strdup(), strcat()
   - 原因：strlen() 扫描是 O(n)

9. 浮点运算
   - double/float 运算（部分平台）
   - 原因：软件浮点库可能很慢

10. 递归调用
    - 任何递归
    - 原因：栈深度不可预测
```

### 2.2 HARD REALTIME 核心原则

```
✅ 正确模式：Copy → Queue → Return

┌─────────────────────────────────────────────────────────────────┐
│                     HARD REALTIME 路径                           │
│                                                                 │
│  DMA ISR / Callback                                             │
│       │                                                        │
│       ▼                                                        │
│  ┌─────────────────────────────────────────────────────────┐    │
│  │  1. COPY（必须是最小化操作）                               │    │
│  │     memcpy(dst, src, fixed_size)  ← 固定大小，已预分配     │    │
│  └─────────────────────────────────────────────────────────┘    │
│       │                                                        │
│       ▼                                                        │
│  ┌─────────────────────────────────────────────────────────┐    │
│  │  2. QUEUE（发送到 ringbuffer/message queue）               │    │
│  │     xRingbufferSendFromISR()  ← 非阻塞，O(1)              │    │
│  │     xQueueSendFromISR()      ← 非阻塞，O(1)              │    │
│  └─────────────────────────────────────────────────────────┘    │
│       │                                                        │
│       ▼                                                        │
│  ┌─────────────────────────────────────────────────────────┐    │
│  │  3. RETURN（立即返回）                                     │    │
│  │     释放 CPU，不做任何其他事情                               │    │
│  └─────────────────────────────────────────────────────────┘    │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘

❌ 错误模式：Process → Allocate → Format → Upload

void isr_or_realtime_callback(void) {
    // 错误：这里做了太多事情
    char *buf = malloc(BIG_SIZE);           // ❌ malloc
    process_audio(buf);                      // ❌ 复杂处理
    snprintf(tmp, sizeof(tmp), "%s", buf);  // ❌ 字符串操作
    log_to_sd(buf);                         // ❌ 文件系统
    upload_to_wifi(buf);                     // ❌ 网络
    free(buf);                              // ❌ free
    // ISR 结束
}
```

### 2.3 Realtime Path 清单

以下代码路径必须遵守 HARD REALTIME 规则：

| 代码路径 | 触发频率 | Deadline | 违规后果 |
|---------|---------|----------|---------|
| I2S DMA callback | 每 DMA buffer (~16ms) | < 16ms | audio drop |
| `xRingbufferSendFromISR()` | 每 DMA buffer | < 100μs | audio drop |
| `xQueueSendFromISR()` | 事件触发 | < 100μs | 事件丢失 |
| Audio Task 数据消费循环 | 持续 | < 16ms | RingBuffer 积压 |
| RingBuffer 消费端 | 每 PCM block | < 16ms | overflow |

---

## 3. Logging Rules

### 3.1 按模块的日志策略

| 模块 | 允许的最高等级 | 频率限制 | 理由 |
|------|--------------|---------|------|
| **Audio Task / I2S callback** | ESP_LOGD() | ≤ 1Hz | 高频日志会刷屏 |
| **Recorder Task** | ESP_LOGD() | ≤ 1Hz | SD 操作不需高频日志 |
| **Upload Task** | ESP_LOGI() | ≤ 1/min | 网络事件需可读日志 |
| **WiFi Manager** | ESP_LOGI() | ≤ 1/10s | 连接事件需日志 |
| **UI Task** | ESP_LOGI() | ≤ 1Hz | 用户交互事件 |
| **Logger 自身** | ESP_LOGW() | 无 | 避免递归 |
| **Event Bus** | ESP_LOGD() | ≤ 1Hz | 调试用 |

### 3.2 Audio Path 日志规则

```
🚫 绝对禁止在 audio path 中使用：

ESP_LOGI(TAG, "Audio RMS: %.0f", rms);  // ❌ 每 100ms = 刷屏
ESP_LOGW(TAG, "Buffer almost full");    // ❌ 可能频繁触发
printf("Processing frame %d\n", frame); // ❌ UART 写入极慢

✅ 正确做法：

// 方案 A：降低频率
static uint32_t s_log_counter = 0;
if (++s_log_counter >= 100) {  // 每 100 次（约 1 秒）打印一次
    ESP_LOGD(TAG, "RMS: %.0f", rms);
    s_log_counter = 0;
}

// 方案 B：使用专用日志宏（推荐）
#define LOG_AUDIO_D_THROTTLED(expr, interval_ms) \
    do { \
        static int64_t s_last_log = 0; \
        int64_t now = esp_timer_get_time() / 1000; \
        if (now - s_last_log >= interval_ms) { \
            ESP_LOGD(TAG, expr); \
            s_last_log = now; \
        } \
    } while(0)

// 使用示例
LOG_AUDIO_D_THROTTLED("RMS: %.0f, Buffer: %d", 1000);
```

### 3.3 推荐日志宏设计

```c
// docs/realtime-rules.md 建议的统一日志宏

#ifndef LOG_THROTTLED_H
#define LOG_THROTTLED_H

#include "esp_log.h"

/**
 * LOG_AUDIO_D_THROTTLED - Audio 路径专用节流日志
 * @expr: 日志表达式
 * @ms: 最小间隔（毫秒）
 * 
 * 使用示例：
 *   LOG_AUDIO_D_THROTTLED("RMS: %.0f", 1000);
 *   LOG_AUDIO_D_THROTTLED("Buffer usage: %d%%", 2000);
 * 
 * 特点：
 * - 仅在 Audio Task 中使用
 * - 频率限制防止刷屏
 * - 使用 static 变量，无额外内存开销
 */
#define LOG_AUDIO_D_THROTTLED(expr, ms) \
    do { \
        static int64_t _last = 0; \
        int64_t _now = esp_timer_get_time() / 1000; \
        if (_now - _last >= (ms)) { \
            ESP_LOGD(TAG, expr); \
            _last = _now; \
        } \
    } while (0)

/**
 * LOG_RECORDER_D_THROTTLED - Recorder 路径专用节流日志
 */
#define LOG_RECORDER_D_THROTTLED(expr, ms) \
    do { \
        static int64_t _last = 0; \
        int64_t _now = esp_timer_get_time() / 1000; \
        if (_now - _last >= (ms)) { \
            ESP_LOGD(TAG, expr); \
            _last = _now; \
        } \
    } while (0)

/**
 * LOG_UPLOAD_I_THROTTLED - Upload 路径日志（INFO 级别）
 */
#define LOG_UPLOAD_I_THROTTLED(expr, ms) \
    do { \
        static int64_t _last = 0; \
        int64_t _now = esp_timer_get_time() / 1000; \
        if (_now - _last >= (ms)) { \
            ESP_LOGI(TAG, expr); \
            _last = _now; \
        } \
    } while (0)

#endif // LOG_THROTTLED_H
```

### 3.4 日志级别指南

```
ESP_LOG_NONE  = 0   ← 完全静默（生产环境 Audio Task 可用）
ESP_LOG_ERROR = 1   ← 仅错误（生产环境推荐）
ESP_LOG_WARN  = 2   ← 警告 + 错误
ESP_LOG_INFO  = 3   ← 信息 + 警告 + 错误
ESP_LOG_DEBUG = 4   ← 调试 + 全部（仅开发环境）
ESP_LOG_VERBOSE = 5 ← 全部日志

生产环境推荐配置：
CONFIG_LOG_DEFAULT_LEVEL=ESP_LOG_INFO
Audio Task 代码使用：ESP_LOGD（编译进固件，但运行时由 TAG 级别过滤）
```

---

## 4. Memory Rules

### 4.1 Realtime Path 内存规则

```
🚫 禁止在 HARD/SOFT REALTIME 路径中：

1. 任何 heap 分配
   - malloc(), free()
   - heap_caps_malloc()
   - new, delete (C++)

2. 动态大小 buffer
   - 可变长度数组 (VLA)
   - strdup(), strndup()
   - sprintf() 到未知大小

3. 内存碎片化行为
   - 频繁分配/释放小对象
   - 跨 size class 分配

✅ 正确做法：

1. 静态预分配
   static uint8_t s_buffer[4096];  // 在 .bss 段，编译时分配

2. 固定大小对象池
   typedef struct {
       uint8_t data[512];
       uint32_t size;
       bool in_use;
   } audio_frame_t;
   
   static audio_frame_t s_frame_pool[16];  // 预分配 16 个 frame

3. 固定大小 ringbuffer
   static RingbufHandle_t s_ringbuf;  // 在 init() 时分配一次

4. 零拷贝原则
   - 传递指针而非数据
   - 指针生命周期由 queue ownership 管理
```

### 4.2 DMA Memory 特殊规则

```
⚠️ ESP32 DMA 有严格的内存位置约束

DMA-capable memory：
- Internal SRAM（DRAM）✅
- PSRAM with DMA-capable allocation ✅
- External SPI RAM (普通分配) ❌

RingBuffer 必须使用 DMA-capable memory：

// ❌ 错误：普通 PSRAM 分配
s_ringbuf = xRingbufferCreate(64 * 1024, ...);  // 可能是不可 DMA 的

// ✅ 正确：明确指定 DMA capability
s_ringbuf = xRingbufferCreateWithCaps(
    64 * 1024,
    RINGBUF_TYPE_NOSPLIT,
    MALLOC_CAP_DMA  // 必须是 DMA-capable
);

// ✅ 正确：使用 heap_caps 分配 buffer
uint8_t *dma_buffer = heap_caps_malloc(
    64 * 1024,
    MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL
);
assert(dma_buffer != NULL);
```

### 4.3 内存分配时序

```
┌─────────────────────────────────────────────────────────────────┐
│                   内存分配时机图                                  │
│                                                                 │
│  系统启动 ──────▶ 初始化阶段 ──────▶ 运行时                      │
│                   │                    │                         │
│                   ▼                    ▼                         │
│  ┌─────────────────────────────────────────────────────────┐    │
│  │  允许分配（NON-REALTIME 阶段）                           │    │
│  │                                                          │    │
│  │  - RingBuffer 分配                                       │    │
│  │  - WAV buffer 分配                                      │    │
│  │  - HTTP upload buffer 分配                               │    │
│  │  - Task stack 分配（系统自动）                            │    │
│  │  - Message queue 分配                                   │    │
│  └─────────────────────────────────────────────────────────┘    │
│                                                             │
│  ┌─────────────────────────────────────────────────────────┐    │
│  │  禁止分配（HARD/SOFT REALTIME 阶段）                     │    │
│  │                                                          │    │
│  │  - Audio Task 运行中                                     │    │
│  │  - I2S DMA callback 中                                  │    │
│  │  - Recorder Task 持续写入中                              │    │
│  │                                                          │    │
│  │  ⚠️ SD 卡写入期间仍然禁止（SOFT REALTIME）               │    │
│  └─────────────────────────────────────────────────────────┘    │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

---

## 5. Mutex & Queue Rules

### 5.1 Priority Inversion 分析

```
⚠️ Priority Inversion 是 embedded realtime 系统中最危险的问题之一

什么是 Priority Inversion：

┌─────────────────────────────────────────────────────────────────┐
│  时间轴：                                                        │
│                                                                 │
│  Task L (低优先级) 持有 Mutex M                                  │
│  ─────────────────────────────────────────────────────────      │
│  Task M (中优先级) 运行                                          │
│                      ──────────────────────────────────         │
│  Task H (高优先级) 等待 Mutex M，但被 Task M 阻塞                  │
│                                        ──────────────────        │
│  结果：Task H 无法运行，即使它的优先级最高                          │
│        Task L 被 Task M 抢占了 CPU                               │
│        系统看起来像死锁，但实际上不是                               │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘

FreeRTOS 的解决方案：Priority Inheritance
- 当 Task H 等待 Task L 持有的 mutex 时
- Task L 临时继承 Task H 的优先级
- Task M 无法抢占 Task L
- Task L 快速释放 mutex，Task H 继续

⚠️ 但 Priority Inheritance 有局限性：
- 只对单个 mutex 有效
- 嵌套 mutex 可能失效
- 如果持有 mutex 时间过长，仍然会阻塞高优先级 task
```

### 5.2 Mutex 禁止模式

```
🚫 绝对禁止：

1. 在持有 mutex 时执行 SD/网络操作
   mutex_lock(&file_mutex);
   f_write(file, buf, len);    // ❌ SD 操作可能阻塞 50ms
   f_sync(file);               // ❌ sync 可能阻塞 200ms
   mutex_unlock(&file_mutex);

2. 在持有 mutex 时执行任何 sleep/delay
   mutex_lock(&mutex);
   vTaskDelay(pdMS_TO_TICKS(100));  // ❌ 长时间阻塞
   mutex_unlock(&mutex);

3. 在持有多个 mutex 时持有时间过长
   mutex_lock(&A);
   mutex_lock(&B);
   // 复杂操作...
   mutex_unlock(&B);
   mutex_unlock(&A);
   // ⚠️ 嵌套 mutex 风险极高

4. 在 ISR 中使用 mutex
   xSemaphoreGiveFromISR(sem, ...);  // ✅
   xSemaphoreTake(sem, ...);         // ❌ ISR 中禁止

5. mutex timeout 过长
   xSemaphoreTake(mutex, portMAX_DELAY);  // ❌ 可能永久阻塞
```

### 5.3 推荐替代模式

```
✅ 推荐：Producer-Consumer + Message Queue

┌─────────────────────────────────────────────────────────────────┐
│                    Producer-Consumer 模式                        │
│                                                                 │
│  Producer (HARD REALTIME)                                       │
│       │                                                         │
│       │  xQueueSendFromISR(queue, &item, ...)                   │
│       │  (非阻塞，O(1)，无需 mutex)                              │
│       ▼                                                         │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │               Message Queue                              │   │
│  │    ┌────┬────┬────┬────┐                                │   │
│  │    │item│item│    │    │ → Producer 写入                  │   │
│  │    └────┴────┴────┴────┘                                │   │
│  └─────────────────────────────────────────────────────────┘   │
│       │                                                         │
│       │  xQueueReceive(queue, &item, timeout)                   │
│       ▼                                                         │
│  Consumer (NON/SOFT REALTIME)                                   │
│       │  处理 item（可以 sleep，可以 SD，可以网络）                │
│       ▼                                                         │
│  示例：Audio → Recorder → SD                                     │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘

✅ 推荐：Single Owner Task

每个共享资源（如 SD 卡文件句柄）只有一个 owner task：

┌─────────────────────────────────────────────────────────────────┐
│                      Single Owner 模式                           │
│                                                                 │
│  Storage Owner Task                                              │
│       │                                                         │
│       │  持有 FILE* 文件句柄                                     │
│       │  仅响应来自其他 task 的请求                               │
│       │  任何 task 需要写 SD → 发消息给 Owner                     │
│       │  Owner 执行写操作（持有 mutex 保护 FILE*）                 │
│       │                                                         │
│  优点：                                                          │
│  - FILE* 始终被同一 task 持有，不需 mutex 保护                     │
│  - Owner 内部可以自由执行 SD 操作，无需担心阻塞                     │
│  - 其他 task 无法直接操作文件，避免竞争                             │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘

✅ 推荐：Ownership Transfer

数据通过 queue 传递，ownership 随之转移：

┌─────────────────────────────────────────────────────────────────┐
│                    Ownership Transfer                            │
│                                                                 │
│  Audio Task                                                     │
│       │                                                         │
│       │  分配 audio_frame_t，填充数据                             │
│       │  发送到 RingBuffer                                        │
│       │  发送后不再持有（RingBuffer 持有）                         │
│       ▼                                                         │
│  RingBuffer → Recorder Task                                     │
│       │                                                         │
│       │  Recorder 从 RingBuffer 取出                              │
│       │  写入 SD 卡                                              │
│       │  写入完成后 frame 返回池                                   │
│       ▼                                                         │
│  Object Pool (预分配)                                            │
│       │                                                         │
│       │  所有 frame 预分配，无 malloc                             │
│       │  复用而非创建/销毁                                         │
│       ▼                                                         │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

---

## 6. SD Card Latency Rules

### 6.1 SD 卡 latency 不是常数

```
⚠️ 警告：SPI SD 卡的写入延迟不是稳定值

常见误解：
"SD 卡写入速度是 10MB/s，所以每 1KB 只需 0.1ms"

这是完全错误的！

实际情况：
- 写入 1KB 的时间可能是 0.5ms，也可能是 200ms
- 取决于：FAT 表状态、flash wear leveling、卡内部缓存、碎片程度

延迟来源分析：

┌─────────────────────────────────────────────────────────────────┐
│                  SD 卡写入延迟分解                                │
│                                                                 │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │ 总延迟 = SPI 传输 + CMD 协议 + Flash 写入 + 内部处理       │   │
│  └─────────────────────────────────────────────────────────┘   │
│            │              │            │            │          │
│            ▼              ▼            ▼            ▼          │
│         0.1~1ms       0.5~5ms      5~50ms      1~100ms        │
│                                                                 │
│  SPI 传输    CMD 协议      Flash 擦除   Wear leveling           │
│                             + 写放大                           │
└─────────────────────────────────────────────────────────────────┘

典型延迟分布（ESP32-S3 + 廉价 TF 卡）：

| 场景 | 延迟范围 | 概率 |
|------|---------|------|
| 正常顺序写入（cached）| 1~5ms | 70% |
| 需要分配新 cluster | 10~30ms | 20% |
| Wear leveling 触发 | 50~100ms | 8% |
| 低质量卡 + 坏块重试 | 100~500ms | 2% |

⚠️ 最坏情况：廉价 TF 卡在接近满时可能达到 800ms+ 写入延迟
```

### 6.2 Average Throughput ≠ Realtime Safety

```
⚠️ 核心误区

"SD 卡平均速度 5MB/s，足够 32KB/s 的音频流"

这个推论是错误的！

┌─────────────────────────────────────────────────────────────────┐
│  平均速度 vs 实时安全                                             │
│                                                                 │
│  假设：                                                          │
│  - 音频数据率：32KB/s                                            │
│  - SD 平均速度：5MB/s                                           │
│  - 平均速度 / 数据率 = 156x（看起来很安全）                         │
│                                                                 │
│  实际：                                                          │
│  - 每秒有 156 个 "时间片" 可用于 SD 写入                           │
│  - 但这些 "时间片" 不是均匀分布的                                  │
│  - 可能连续 800ms 无法写入，然后快速写入 10KB                      │
│                                                                 │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │  时序图：                                                │   │
│  │                                                         │   │
│  │  DMA:  [====][====][====][====][====][====][====][====] │   │
│  │           16ms  16ms  16ms  16ms  16ms  16ms  16ms  16ms│   │
│  │                                                         │   │
│  │  SD:    [==============][write][===][======][write]     │   │
│  │         ~500ms gap      5ms   200ms gap  5ms              │   │
│  │                              ↑                           │   │
│  │                         此时 RingBuffer                  │   │
│  │                         被积压 200ms 数据                  │   │
│  │                                                         │   │
│  └─────────────────────────────────────────────────────────┘   │
│                                                                 │
│  结论：                                                          │
│  - 峰值延迟决定了 RingBuffer 需要多大                              │
│  - 不是平均吞吐量                                                │
│  - 安全预算 = Peak latency（而非 Average throughput）             │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘

设计原则：
✅ RingBuffer 必须能吸收 Peak latency
✅ 计算公式：Buffer size > Peak latency × Audio data rate
✅ 示例：Peak 800ms × 32KB/s = 25.6KB，建议 64KB（2x 安全系数）
```

### 6.3 SD 卡安全设计规则

```
✅ 必须遵守的 SD 卡设计规则：

1. RingBuffer 大小 ≥ Peak SD latency × Data rate × 2
   - 示例：800ms × 32KB/s × 2 = 51.2KB → 使用 64KB

2. 绝不假设 SD 写入是同步的
   - f_write() 可能会缓存在 OS/FATFS 层
   - 真正的 flash 写入在 f_sync() 时才触发

3. 录音开始前确认 SD 卡状态
   - 检查剩余空间
   - 检查写入速度（可用性测试）

4. SD 错误处理必须保守
   - SD 卡满 → 立即停止录音（而不是继续积压）
   - 写入错误 → EVENT_STORAGE_ERROR → 状态机进入 ERROR

5. 热插拔必须感知
   - EVENT_STORAGE_REMOVED → 停止录音 → 进入 ERROR
   - 重新 mount 后需用户确认

6. 禁止在 Realtime path 中检测 SD 卡状态
   - 不要在 Audio Task 中检查 SD 卡剩余空间
   - 在 IDLE 状态检查，上传前检查
```

---

## 7. WiFi Coexistence Rules

### 7.1 WiFi 对 Realtime 系统的影响

```
⚠️ WiFi 是 Realtime 系统的天敌之一

WiFi 对系统的干扰来源：

┌─────────────────────────────────────────────────────────────────┐
│                  WiFi 对 ESP32 的影响                           │
│                                                                 │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │ CPU 占用                                                 │   │
│  │  - WiFi stack 运行在高优先级 task                         │   │
│  │  - LWIP、TCP/IP 处理占用大量 CPU                          │   │
│  │  - 每次 radio 事件触发中断 + 处理                          │   │
│  └─────────────────────────────────────────────────────────┘   │
│                                                                 │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │ RAM 占用                                                 │   │
│  │  - WiFi TX/RX buffer：50~100KB                          │   │
│  │  - LWIP heap：30~50KB                                   │   │
│  │  - TLS/SSL heap：50KB+（如果使用 HTTPS）                  │   │
│  └─────────────────────────────────────────────────────────┘   │
│                                                                 │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │ Interrupt 风暴                                            │   │
│  │  - Radio 中断可能每 10~100ms 触发一次                      │   │
│  │  - 中断期间 Audio Task 可能被延迟                          │   │
│  └─────────────────────────────────────────────────────────┘   │
│                                                                 │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │ Flash 访问                                                │   │
│  │  - WiFi calibration 数据存储在 flash                       │   │
│  │  - 某些 WiFi 操作会触发 flash 读写                          │   │
│  └─────────────────────────────────────────────────────────┘   │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘

⚠️ 典型场景：WiFi 影响 Audio

时间轴：
────────────────────────────────────────────────────────────────
Audio Task:  [处理][处理][处理][处理][处理][处理][处理][处理]
                    ↑                    ↑                    ↑
WiFi ISR:                  [=====radio====]
                                ↑ CPU 被 WiFi 抢占
Audio 被延迟 5~15ms，可能导致 RingBuffer 积压
────────────────────────────────────────────────────────────────
```

### 7.2 WiFi 与 Audio 共存规则

```
✅ WiFi 上传任务必须遵守的规则：

1. 优先级必须低于 Audio Task
   - Audio Task: priority 3
   - Upload Task: priority 2
   
2. Upload Task 必须可暂停
   void upload_task(void *arg) {
       while (1) {
           if (audio_is_recording()) {
               // 录音期间减慢上传速度
               vTaskDelay(pdMS_TO_TICKS(100));  // 让出 CPU
           } else {
               // 空闲时可以全速上传
               upload_one_file();
           }
       }
   }

3. 上传 buffer 必须在 Non-realtime 阶段分配
   - upload_init() 中分配 HTTP buffer
   - 运行时不复配

4. WiFi 连接失败不能影响录音
   - WiFi 断开 → 继续录音，队列本地缓存
   - 重新连接后自动上传

5. 避免在上传高峰期执行录音开始
   // ❌ 不好：WiFi 正忙于重传大文件
   if (wifi_is_busy()) {
       delay(100);  // 等待 WiFi 空闲
   }
   recorder_start();
   
   // ✅ 好：录音开始优先级高于上传
   recorder_start();  // 录音总是可以开始
```

### 7.3 WiFi 干扰缓解策略

```c
// 推荐：录音期间降低 WiFi 活动

#include "esp_wifi.h"

void on_audio_state_changed(audio_state_t state) {
    if (state == AUDIO_STATE_RECORDING) {
        // 录音期间降低 WiFi 功率（减少 radio 中断）
        wifi_config_t cfg = {
            .sta = {
                .listen_interval = 10,  // 减少 beacon 监听频率
            }
        };
        esp_wifi_set_config(WIFI_IF_STA, &cfg);
        
        ESP_LOGI(TAG, "WiFi reduced for audio recording");
    } else {
        // 录音停止后恢复 WiFi 性能
        wifi_config_t cfg = {
            .sta = {
                .listen_interval = 3,  // 恢复正常
            }
        };
        esp_wifi_set_config(WIFI_IF_STA, &cfg);
        
        ESP_LOGI(TAG, "WiFi restored to full performance");
    }
}
```

---

## 8. Recommended Design Patterns

### 8.1 推荐模式

```
✅ 推荐在 ESP32 Recorder 项目中使用的模式：

┌─────────────────────────────────────────────────────────────────┐
│  1. Producer-Consumer (强烈推荐)                                  │
│                                                                 │
│  用途：Audio Task → Recorder Task 数据传递                        │
│  优点：零锁、无阻塞、数据所有权清晰                                │
│                                                                 │
│  实现：RingBuffer 或 Message Queue                               │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│  2. RingBuffer (强烈推荐)                                         │
│                                                                 │
│  用途：I2S 数据暂存、主音频流缓冲                                  │
│  优点：O(1) 入/出、预分配、不碎片化                               │
│                                                                 │
│  注意：必须 DMA-capable                                          │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│  3. Object Pool / Preallocated Pool (推荐)                        │
│                                                                 │
│  用途：JSON 任务对象、WAV frame、upload item                      │
│  优点：避免运行时分配、无碎片化                                    │
│                                                                 │
│  示例：                                                          │
│  static upload_task_t s_pool[16];                               │
│  static bool s_in_use[16];                                      │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│  4. Event-Driven Architecture (推荐)                             │
│                                                                 │
│  用途：所有组件间通信                                             │
│  优点：解耦、无直接依赖、易于测试                                 │
│                                                                 │
│  实现：Event Bus (已实现)                                         │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│  5. State Machine (推荐)                                         │
│                                                                 │
│  用途：设备状态管理（已实现 state.c）                              │
│  优点：确定性行为、易于推理、易于调试                             │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│  6. Watchdog分层保护                                             │
│                                                                 │
│  用途：检测 task 卡死                                            │
│  优点：独立于任务、可恢复                                        │
│                                                                 │
│  示例：                                                          │
│  - Audio watchdog：检测 Audio Task 是否持续消费数据              │
│  - Recorder watchdog：检测 SD 写入是否持续进行                    │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

### 8.2 不推荐模式

```
❌ 不推荐或禁止的模式：

┌─────────────────────────────────────────────────────────────────┐
│  1. Global Shared Mutable State (禁止)                           │
│                                                                 │
│  extern volatile bool g_is_recording;  // ❌ 全局可变状态        │
│                                                                 │
│  问题：多 task 访问需要锁、难以推理、难以测试                      │
│  替代：Event Bus 驱动状态变化                                    │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│  2. Nested Mutex (禁止)                                           │
│                                                                 │
│  mutex_lock(A);                                                  │
│  mutex_lock(B);  // ❌ 嵌套                                      │
│  // ...                                                          │
│  mutex_unlock(B);                                                │
│  mutex_unlock(A);                                                │
│                                                                 │
│  问题：可能导致死锁、priority inversion                           │
│  替代：单一 mutex 或消息队列                                      │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│  3. Synchronous Upload in Realtime Path (禁止)                    │
│                                                                 │
│  void recorder_stop() {                                          │
│      f_sync(file);                                               │
│      upload_sync(file);  // ❌ 同步上传会阻塞                     │
│      f_close(file);                                              │
│  }                                                               │
│                                                                 │
│  问题：上传失败会阻塞录音停止、SD 卡写入可能被延迟                  │
│  替代：异步上传（Upload Task 独立运行）                            │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│  4. Cross-Component Direct Function Calls (不推荐)                │
│                                                                 │
│  void button_handler() {                                         │
│      recorder_start();      // ❌ 直接调用                        │
│      wifi_connect("ssid");  // ❌ 直接调用                        │
│  }                                                               │
│                                                                 │
│  问题：组件耦合、难以独立测试                                      │
│  替代：Event Bus（button 发布事件，其他组件订阅处理）               │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│  5. Callback Hell (不推荐)                                       │
│                                                                 │
│  esp_http_client_perform(client, &conf, callback1,              │
│      callback2, callback3, callback4);  // ❌ 回调地狱           │
│                                                                 │
│  问题：难以调试、错误处理复杂                                      │
│  替代：轮询 + 状态机，或使用 async wrapper                        │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

### 8.3 设计模式决策树

```
新功能设计时，用这个决策树选择合适的模式：

1. 需要在两个 task 之间传递数据？
   ├─ 是 → 使用 RingBuffer（音频流）或 Message Queue（离散消息）
   └─ 否 → 继续下一步

2. 需要保护共享资源？
   ├─ 是 → 是否是短暂操作？
   │   ├─ 是 (< 100μs) → Binary Semaphore
   │   └─ 否 → 重新设计，考虑 Single Owner 模式
   └─ 否 → 继续下一步

3. 需要跨组件通信？
   ├─ 是 → 使用 Event Bus
   └─ 否 → 直接函数调用即可

4. 需要预分配固定数量对象？
   ├─ 是 → Object Pool
   └─ 否 → 检查是否可以在 startup 分配
```

---

## 9. Failure Philosophy

### 9.1 系统优先级顺序

```
⚠️ ESP32 Recorder 的失败哲学

当系统面临资源冲突或必须做出取舍时，
按以下优先级顺序决策：

┌─────────────────────────────────────────────────────────────────┐
│                  优先级 1：不丢音                                 │
│                                                                 │
│  这是系统存在的根本原因。                                          │
│  任何情况下，audio pipeline 必须保持运行。                         │
│                                                                 │
│  后果：                                                          │
│  - WiFi 抢占 → 让出 CPU                                          │
│  - Upload 延迟 → 允许                                            │
│  - UI 卡顿 → 允许                                                │
│  - Logger 丢失 → 允许                                            │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│                  优先级 2：不崩溃                                 │
│                                                                 │
│  系统必须在任何情况下保持可用。                                    │
│  即使录音质量下降，也不能完全崩溃。                                │
│                                                                 │
│  后果：                                                          │
│  - OOM → 拒绝新分配，返回错误                                     │
│  - SD 卡满 → 停止录音，但不 panic                                 │
│  - WiFi 断开 → 继续本地录音                                       │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│                  优先级 3：文件可恢复                             │
│                                                                 │
│  已录音的数据必须能恢复。                                          │
│  哪怕设备突然掉电，已写的数据必须有 WAV 头。                        │
│                                                                 │
│  后果：                                                          │
│  - 录音停止时必须 f_sync() + 更新 WAV 头                          │
│  - 每 30s checkpoint sync                                         │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│                  优先级 4：上传成功                               │
│                                                                 │
│  录音最终要上传到服务器。                                          │
│  但上传可以重试，可以延迟。                                        │
│                                                                 │
│  后果：                                                          │
│  - 上传失败 → 重试（最多 N 次）                                   │
│  - 网络断开 → 队列本地缓存，连接后重传                             │
│  - 永久失败 → 保留本地文件，标记错误                               │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│                  优先级 5：UI 完美                                │
│                                                                 │
│  LED 显示、按钮响应等是最低优先级。                                │
│  UI 可以偶尔卡顿，不影响核心功能。                                  │
│                                                                 │
│  后果：                                                          │
│  - 录音期间 UI 更新频率降低                                       │
│  - LED 模式切换略有延迟可接受                                      │
│  - 按钮去抖动时间可以延长                                          │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

### 9.2 失败处理原则

```
核心原则：录音丢失不可恢复，上传失败可以重试

┌─────────────────────────────────────────────────────────────────┐
│  上传失败 vs 录音丢失                                             │
│                                                                 │
│  上传失败：                                                       │
│  - 可以重试（今天失败，明天再试）                                   │
│  - 可以降级（改用其他网络）                                        │
│  - 数据仍在 SD 卡，本地保留                                        │
│  - 用户可以手动触发重传                                           │
│                                                                 │
│  录音丢失：                                                       │
│  - 不可重试（事件已发生，无法回溯）                                 │
│  - 不可降级（没有数据就没有数据）                                  │
│  - 直接影响核心价值                                               │
│                                                                 │
│  结论：任何情况下，audio pipeline 优先级最高                        │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

### 9.3 资源冲突决策表

| 冲突场景 | 决策 | 理由 |
|---------|------|------|
| WiFi 上传占用 CPU | WiFi 暂停 | WiFi 可重试，丢音不可恢复 |
| SD 卡写入延迟 | 允许积压（依赖 RingBuffer）| 延迟可接受，丢音不可接受 |
| Logger 阻塞 | Logger 丢弃日志 | 核心功能不依赖日志 |
| UI 响应慢 | 允许 | UI 延迟不影响录音 |
| malloc 失败 | 拒绝新功能，保持现有 | OOM 比功能缺失更严重 |
| 电池低 | 停止录音，保存文件 | 防止数据丢失 |

### 9.4 Watchdog 策略

```c
// 推荐：分层 Watchdog

┌─────────────────────────────────────────────────────────────────┐
│                    分层 Watchdog 策略                             │
│                                                                 │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │  Audio Watchdog（最高优先级）                             │   │
│  │  - 检测：Audio Task 是否持续消费 RingBuffer               │   │
│  │  - 阈值：> 64ms 无消费 → 警告                             │   │
│  │  - 动作：EVENT_RECORDING_ERROR                           │   │
│  └─────────────────────────────────────────────────────────┘   │
│                                                                 │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │  Recorder Watchdog（高优先级）                             │   │
│  │  - 检测：Recorder Task 是否持续写入 SD                     │   │
│  │  - 阈值：> 5s 无写入 → 警告（但继续）                      │   │
│  │  - 动作：EVENT_STORAGE_SLOW                             │   │
│  └─────────────────────────────────────────────────────────┘   │
│                                                                 │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │  System Watchdog（普通）                                   │   │
│  │  - 检测：各 task 是否响应心跳                              │   │
│  │  - 阈值：各 task 自行定义                                  │   │
│  │  - 动作：重启或进入 ERROR 状态                            │   │
│  └─────────────────────────────────────────────────────────┘   │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

---

## 10. Quick Reference Card

```
╔═══════════════════════════════════════════════════════════════════╗
║              ESP32 Recorder Realtime Rules — Quick Ref            ║
╠═══════════════════════════════════════════════════════════════════╣
║                                                                   ║
║  🔴 HARD REALTIME (I2S/Audio/RingBuffer)                          ║
║     ─────────────────────────────────────────────                ║
║     ❌ malloc / free                                               ║
║     ❌ ESP_LOGI / ESP_LOGW / printf                                ║
║     ❌ FATFS / f_write / f_sync                                   ║
║     ❌ WiFi / 网络操作                                             ║
║     ❌ 阻塞 mutex                                                  ║
║     ❌ 浮点运算                                                   ║
║     ❌ > 100μs 的任何操作                                          ║
║     ✅ memcpy (固定大小)                                           ║
║     ✅ xRingbufferSendFromISR()                                   ║
║     ✅ xQueueSendFromISR()                                        ║
║                                                                   ║
║  🟡 SOFT REALTIME (Recorder/SD write)                            ║
║     ─────────────────────────────────────────────                ║
║     ❌ malloc / free（启动时分配好）                               ║
║     ✅ f_write (批量写入)                                          ║
║     ✅ f_sync (每 30s 或停止时)                                    ║
║     ⚠️ 持有 mutex 时间 < 1ms                                      ║
║                                                                   ║
║  🟢 NON-REALTIME (UI/WiFi/Upload/Logger)                        ║
║     ─────────────────────────────────────────────                ║
║     ✅ 正常编程                                                    ║
║     ❌ 不要抢占 Audio CPU                                          ║
║     ⚠️ Upload 优先级必须低于 Audio Task                           ║
║                                                                   ║
║  📋 Memory Rules                                                  ║
║     ─────────────────────────────────────────────                ║
║     ✅ 静态分配（.bss / .data）                                   ║
║     ✅ Object Pool                                                ║
║     ✅ RingBuffer DMA-capable                                     ║
║     ❌ 运行时分配（除非 Non-realtime 阶段）                        ║
║                                                                   ║
║  📋 SD Card Rules                                                ║
║     ─────────────────────────────────────────────                ║
║     ✅ RingBuffer ≥ 64KB（吸收 peak latency）                    ║
║     ✅ 批量写入（4KB+）                                            ║
║     ✅ Checkpoint sync（每 30s）                                   ║
║     ✅ 停止录音时必须 sync                                          ║
║     ❌ 不要假设 SD write 是同步的                                  ║
║                                                                   ║
║  📋 Failure Priority                                              ║
║     ─────────────────────────────────────────────                ║
║     1. 不丢音 > 2. 不崩溃 > 3. 文件可恢复 > 4. 上传 > 5. UI        ║
║                                                                   ║
╚═══════════════════════════════════════════════════════════════════╝
```

---

*文档版本：v0.1 | 作者：AI Assistant | 审核：待定*
