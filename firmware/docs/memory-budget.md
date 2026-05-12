# Memory Budget 规划 — ESP32 AI Recorder

> Version: v0.1 | Created: 2026-05-12
> 本文档定义 ESP32-S3 的 RAM 使用预算，提前识别潜在的内存问题。

---

## 1. ESP32-S3 内存架构概述

### 1.1 内存类型

| 类型 | 大小 | 速度 | DMA 支持 | 用途 |
|------|------|------|---------|------|
| **Internal RAM (IRAM)** | 512KB | 最快 | ❌ 不支持 | 代码、ISR、栈 |
| **Internal DRAM** | 512KB | 快 | ❌ 不支持 | 全局变量、堆 |
| **PSRAM** | 8MB | 较慢 | ⚠️ 部分支持 | 大型 buffer、WiFi |
| **DMA-capable SRAM** | 特定区域 | 快 | ✅ 支持 | I2S DMA buffer |

### 1.2 DMA 约束（关键）

```
ESP32-S3 的 DMA 有严格约束：

1. DMA 只能在特定内存区域执行
2. Internal RAM 可以 DMA
3. PSRAM 需要使用特定 API（heap_caps_malloc_prefer）分配才可 DMA
4. 普通 malloc() 分配的 PSRAM 内存不能 DMA

重要：RingBuffer 必须 DMA-capable！
如果 RingBuffer 分配在不可 DMA 的区域，I2S 数据传输会失败。
```

### 1.3 可用内存总结

| 区域 | 大小 | 可用（约）| 说明 |
|------|------|-----------|------|
| IRAM + DRAM | 512KB | ~300KB | 系统运行核心 |
| PSRAM | 8MB | ~7.5MB | 大型 buffer、WiFi heap |
| **DMA-capable 总计** | ~400KB | ~250KB | I2S DMA + RingBuffer |

---

## 2. Memory Budget 详细规划

### 2.1 模块 RAM 预算表

| 模块 | 分配位置 | 大小 | 用途 | 备注 |
|------|---------|------|------|------|
| **I2S DMA Buffers** | DMA SRAM | 4 × 1KB = 4KB | DMA 环形 buffer | ESP-IDF I2S driver 自动管理 |
| **RingBuffer** | DMA SRAM | 64KB | 音频数据缓冲 | **必须 DMA-capable** |
| **WAV Write Buffer** | DRAM | 4KB | f_write() 批量缓冲 | 可选，建议保留 |
| **WiFi Stack** | PSRAM | 50~80KB | WiFi 连接管理 | 动态，连接时分配 |
| **HTTP Upload Buffer** | PSRAM | 32KB | HTTP 请求/响应 | 上传时临时使用 |
| **FreeRTOS Stacks** | DRAM/PSRAM | 见下表 | 各任务栈空间 | 见 Task Stacks 节 |

### 2.2 Task Stack 预算

| Task | 栈大小 | 分配位置 | 典型使用量 | 风险 |
|------|--------|---------|-----------|------|
| Audio Task | 4096B | DRAM | ~2KB | 低 |
| Recorder Task | 8192B | DRAM | ~4KB | 中 |
| Upload Task | 8192B | DRAM | ~3KB | 低 |
| WiFi Manager Task | 4096B | DRAM | ~2KB | 低 |
| UI Task | 8192B | DRAM | ~3KB | 中 |
| Logger Task | 4096B | DRAM | ~1KB | 低 |
| IDLE Task | 1536B | 系统 | ~0.5KB | 无 |
| **总计** | ~39KB | — | ~16KB | — |

### 2.3 系统保留

| 用途 | 大小 | 说明 |
|------|------|------|
| System Heap（DRAM）| ~100KB | malloc 备留 |
| WiFi Internal | ~30KB | ESP32 WiFi 库内部使用 |
| ESP-IDF System | ~50KB | 日志、事件等 |
| **系统保留总计** | ~180KB | DRAM 中 |

---

## 3. 内存分配策略

### 3.1 分配原则

```
优先级：高 → 低

1. 必须 DMA-capable 的 buffer（RingBuffer）
   → 分配在 DMA SRAM 区域

2. 大型但不频繁访问的 buffer（WiFi/HTTP）
   → 分配在 PSRAM

3. 小型高频访问的 buffer（WAV buffer、stack）
   → 分配在 DRAM

4. 通用堆分配
   → DRAM 优先，DRAM 不足时用 PSRAM
```

### 3.2 ESP-IDF 分配示例

```c
#include "esp_heap_caps.h"

/* RingBuffer：必须 DMA-capable */
static RingbufHandle_t s_ringbuf;

void audio_init(void) {
    /* MALLOC_CAP_DMA = DMA-capable SRAM */
    s_ringbuf = xRingbufferCreateWithCaps(
        64 * 1024,           /* 64KB */
        RINGBUF_TYPE_NOSPLIT,
        MALLOC_CAP_DMA       /* 必须 DMA-capable！ */
    );
    assert(s_ringbuf != NULL);
}

/* WAV Write Buffer：普通 DRAM 即可 */
static uint8_t s_wav_buffer[4096];

/* HTTP Upload Buffer：可以使用 PSRAM */
static uint8_t *s_http_buffer;

void uploader_init(void) {
    s_http_buffer = heap_caps_malloc(
        32 * 1024,           /* 32KB */
        MALLOC_CAP_SPIRAM    /* PSRAM */
    );
}
```

---

## 4. 内存安全检查

### 4.1 运行时检查点

```c
#include "esp_heap_caps.h"

/* 在关键节点检查剩余内存 */
void check_memory(const char *context) {
    multi_heap_info_t info;
    heap_caps_get_info(&info, MALLOC_CAP_INTERNAL);

    ESP_LOGI(TAG, "[%s] DRAM free: %u bytes", context, info.total_free_bytes);

    /* 警告阈值：剩余 < 50KB */
    if (info.total_free_bytes < 50 * 1024) {
        ESP_LOGW(TAG, "⚠️ DRAM 内存紧张！");
    }
}
```

### 4.2 内存不足场景处理

| 场景 | 处理策略 |
|------|---------|
| RingBuffer 分配失败 | **致命错误**：重启系统 |
| WiFi 栈分配失败 | 降低 WiFi 配置，不录音 |
| Upload buffer 分配失败 | 减小 buffer 到 16KB |
| Task stack 不足 | 增加对应 task 栈大小 |

---

## 5. PSRAM vs DRAM 决策树

```
内存分配决策：

1. 这个 buffer 是否需要 DMA？
   ├─ 是 → 必须使用 DRAM 或 heap_caps_malloc(MALLOC_CAP_DMA)
   └─ 否 → 继续下一步

2. Buffer 大小是否 > 4KB？
   ├─ 是 → 优先考虑 PSRAM（节省 DRAM）
   └─ 否 → 使用 DRAM

3. 这个 buffer 是否高频访问（每 ms 级）？
   ├─ 是 → DRAM（延迟更低）
   └─ 否 → PSRAM（节省 DRAM）
```

---

## 6. 当前设计总览

```
Memory Budget 汇总（基于 v0.2 设计）

┌─────────────────────────────────────────────────────────────┐
│                     ESP32-S3 Memory                         │
│                                                             │
│  ┌─────────────────────────────────────────────────────┐  │
│  │                  DMA SRAM (~250KB free)              │  │
│  │  ┌─────────────┐  ┌──────────────────────────────┐ │  │
│  │  │ I2S DMA     │  │ RingBuffer (64KB)            │ │  │
│  │  │ 4KB         │  │ ⚠️ Must be DMA-capable        │ │  │
│  │  └─────────────┘  └──────────────────────────────┘ │  │
│  └─────────────────────────────────────────────────────┘  │
│                                                             │
│  ┌─────────────────────────────────────────────────────┐  │
│  │                  DRAM (~300KB free)                 │  │
│  │  ┌────────────┐ ┌──────────┐ ┌────────────────┐   │  │
│  │  │ WAV Buffer │ │Stacks    │ │ System Heap    │   │  │
│  │  │ 4KB        │ │~40KB     │ │~100KB          │   │  │
│  │  └────────────┘ └──────────┘ └────────────────┘   │  │
│  └─────────────────────────────────────────────────────┘  │
│                                                             │
│  ┌─────────────────────────────────────────────────────┐  │
│  │                  PSRAM (~7.5MB free)               │  │
│  │  ┌────────────┐ ┌──────────┐                      │  │
│  │  │ WiFi Stack │ │ HTTP Buf │                      │  │
│  │  │ 50-80KB    │ │ 32KB     │                      │  │
│  │  └────────────┘ └──────────┘                      │  │
│  └─────────────────────────────────────────────────────┘  │
│                                                             │
│  ✅ 预计总使用：~250KB DRAM + ~120KB PSRAM                │
│  ✅ 预计剩余：~50KB DRAM + ~7.4MB PSRAM                   │
└─────────────────────────────────────────────────────────────┘
```

---

## 7. 未来扩展考虑

| 扩展项 | 内存影响 | 缓解策略 |
|--------|---------|---------|
| 双声道录音 | +32KB/s RingBuffer | 使用 PSRAM RingBuffer |
| 固件 OTA | +200KB 临时区 | OTA 时暂停录音 |
| BLE 支持 | +50KB | v0.8+ 再考虑 |
| 语音识别本地化 | +1MB+ | 使用 PSRAM |

---

*文档版本：v0.1 | 作者：AI Assistant | 审核：待定*
