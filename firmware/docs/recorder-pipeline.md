# Recorder Pipeline 架构设计 — ESP32 AI Recorder

> Version: v0.2 | Updated: 2026-05-12
> 本文档定义未来 WAV 录音的数据流架构，不涉及具体代码实现。

---

## 1. 设计目标

1. **实时性**：I2S 音频数据不丢失（no audio drop）。
2. **解耦**：I2S 采集、数据处理、SD 卡写入分属不同 task，互不阻塞。
3. **可维护**：每层职责单一，边界清晰，AI 或新开发者可独立修改某一层。
4. **SD 卡安全**：绝不在 I2S ISR 或高优先级 task 中直接调用 FATFS 写入。

---

## 2. 管道全景（Pipeline Overview）

```
INMP441  →  I2S DMA  →  RingBuffer  →  Recorder Task  →  WAV Writer  →  FATFS  →  SD Card
(Mic)        (硬件 DMA)   (FreeRTOS)    (应用层 task)     (文件封装)     (文件系统)   (物理)
```

对应软件架构：

```
┌──────────────────────────────────────────────────────────────────┐
│                     Recorder Pipeline                           │
│                                                              │
│  ┌─────────┐   ┌──────────┐   ┌──────────┐   ┌─────────┐  │
│  │ I2S     │   │RingBuffer │   │Recorder   │   │WAV      │  │
│  │DMA      ├──▶│(FreeRTOS)├──▶│Task       ├──▶│Writer   │  │
│  │Read     │   │           │   │(priority 3)│   │(同 task)│  │
│  │(ISR)    │   │           │   │           │   │         │  │
│  └─────────┘   └──────────┘   └──────────┘   └─────────┘  │
│       │              │                │                │          │
│       ▼              ▼                ▼                ▼          │
│   DMA Buffer    RingBuffer     应用层处理       FATFS f_write() │
│   (HW)         (RAM)          (可选增益等)     → SD Card      │
└──────────────────────────────────────────────────────────────────┘
```

---

## 3. 各层详细说明

### 3.1 I2S DMA 层（硬件）

| 属性 | 说明 |
|------|------|
| **职责** | 通过 I2S 外设 DMA 将 INMP441 的 PDM/PCM 数据搬入 RAM |
| **数据格式** | I2S Philips 标准，16-bit PCM，左声道有效（INMP441 为单声道 mic）|
| **DMA Buffer** | 由 ESP-IDF I2S driver 管理，默认 2~4 个 buffer 轮转 |
| **数据传递** | I2S driver callback（**不是用户 ISR**）→ Audio Task → RingBuffer |
| **CPU 干预** | ESP-IDF I2S STD API 使用 DMA + driver task，用户不在 ISR 中执行复杂逻辑 |

#### ESP-IDF v5 I2S STD API 工作方式（重要澄清）

```
⚠️ 常见误解："在 I2S ISR 中 memcpy 到 RingBuffer"

这个表述是错误的，会误导未来实现。

ESP-IDF v5 I2S STD API 的实际架构：

┌─────────────────────────────────────────────────────────────┐
│                     ESP-IDF I2S Driver                      │
│                                                             │
│  ┌──────────┐    ┌───────────────┐    ┌───────────────┐  │
│  │  HW I2S  │───▶│  DMA Buffer   │───▶│ Driver Task   │  │
│  │  (硬件)   │    │  (环形 DMA)     │    │ (xTaskNotify) │  │
│  └──────────┘    └───────────────┘    └───────┬───────┘  │
│                                                │           │
│                                                ▼           │
│  ┌──────────────────────────────────────────────────────┐  │
│  │                    User Code                         │  │
│  │                                                      │  │
│  │  i2s_channel_read()  ←── Audio Task 调用此函数        │  │
│  │       │                  读取 DMA buffer 中的数据      │  │
│  │       ▼                                             │  │
│  │  memcpy(dst, src, len)  ←── 拷贝到应用层 buffer       │  │
│  │       │                                             │  │
│  │       ▼                                             │  │
│  │  RingBuffer_Send()  ←── 发送到 RingBuffer            │  │
│  │                                                      │  │
│  └──────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────┘

关键点：
1. 用户代码运行在 Audio Task（应用层），不在 ISR 中
2. DMA 完成中断由 ESP-IDF driver 处理，不暴露给用户
3. 用户通过 i2s_channel_read() 读取数据（同步 API）或注册 callback（异步）
4. memcpy 和 RingBuffer 操作都在 Audio Task 中执行

因此文档中的"ISR memcpy"应理解为：
"Audio Task 从 I2S DMA 读取数据后，尽快转移至 RingBuffer，避免阻塞 DMA refill。"
```

#### 推荐 DMA 配置

```c
i2s_chan_config_t chan_cfg = {
    .id = I2S_NUM_0,
    .role = I2S_ROLE_MASTER,
    .dma_desc_num = 4,          /* DMA 描述符数量：4 个 */
    .dma_frame_num = 256,       /* 每个描述符 256 帧 ~ 512 bytes (16bit) */
    .auto_clear = false,         /* RX 模式：无数据时不自动清零（保留旧数据）*/
};
/* 推荐：dma_desc_num=4, dma_frame_num=256~512 */
/* 理由：平衡延迟与 CPU 占用，避免 audio drop */
```

---

### 3.2 RingBuffer 层（FreeRTOS）

| 属性 | 说明 |
|------|------|
| **职责** | I2S ISR 与 Recorder Task 之间的缓冲，吸收突发 CPU 占用 |
| **实现** | `xRingbufferCreate()`（ESP-IDF 提供，线程安全）|
| **数据单元** | `int16_t pcm[256]`（256 samples = 16ms @ 16kHz）|
| **生产者** | I2S DMA 完成回调（ISR 上下文，必须快速）|
| **消费者** | Recorder Task（应用层，可阻塞等待）|

#### 推荐 RingBuffer 大小

```
⚠️ 警告：4KB RingBuffer 设计已被废弃（v0.1 版本错误）

当前推荐大小：32KB ~ 64KB（约 1~2 秒音频缓冲）

音频数据率计算：
- 采样率：16kHz
- 位深：16-bit（2 bytes/sample）
- 声道：单声道（1 channel）
- 数据率：16000 × 2 × 1 = 32KB/s

64KB RingBuffer ≈ 2 秒缓冲能力
32KB RingBuffer ≈ 1 秒缓冲能力

为什么需要这么大？
─────────────────────────────────────────────────────────────
SPI SD 卡的 worst-case latency spike 可能达到 300~800ms：

延迟来源分析：
1. FATFS metadata update
   - 写入新数据时，FAT 表需要更新
   - FAT32 的 cluster allocation 可能触发多次 flash 操作
   
2. 闪存物理特性
   - NAND flash erase block：10~50ms（视芯片型号）
   - Wear leveling 算法：后台垃圾回收可能阻塞 I/O
   
3. SPI 协议开销
   - 杜邦线连接（非焊接）：信号完整性下降
   - 低质量 TF 卡：内部缓存小，retryl 多
   
4. SD 卡内部缓存
   - 低端卡（Class 4/6）内部缓存仅 512KB~1MB
   - 缓存写满后触发真实写入，期间主机请求排队

实测数据（ESP32-S3 + SPI SD）：
- 正常写入延迟：5~20ms
- FATFS 目录遍历：+10~30ms
- 低质量 TF 卡峰值：50~200ms
- 极端情况（坏块重试）：300~800ms

─────────────────────────────────────────────────────────────
Embedded Audio Recording 的核心约束：
- I2S DMA 必须持续供应数据，不能停顿
- RingBuffer 是唯一吸收 SD 卡延迟波动的手段
- RingBuffer 满 → audio drop → 录音文件有缺口
- Audio drop 无法事后修复，必须预防

设计原则：
宁可浪费 30KB RAM，也不要丢失 1 秒音频。
─────────────────────────────────────────────────────────────

结论：使用 64KB RingBuffer（推荐），最低不低于 32KB。
```

#### Buffer Ownership 规则

| 规则 | 说明 |
|------|------|
| ISR 写入，Task 读取 | 方向固定，不产生竞争 |
| ISR 中只允许 `xRingbufferSendFromISR()` | 不可调用阻塞 API |
| Task 中调用 `xRingbufferReceive()` | 可设 `portMAX_DELAY` 阻塞等待数据 |
| Buffer 用完后必须 `vRingBufferReturnItem()` | 防止内存泄漏 |

---

### 3.3 Recorder Task 层（应用层）

| 属性 | 说明 |
|------|------|
| **职责** | 从 RingBuffer 读取 PCM 数据；封装 WAV 文件头；调用 WAV Writer 写入 SD 卡 |
| **Task 优先级** | `3`（高于上传 task 的 2，低于 I2S ISR）|
| **栈大小** | 推荐 `8192` bytes（含 FATFS 调用开销）|
| **核心绑定** | `xTaskCreatePinnedToCore(..., 0)` — 绑定 core 0（与 I2S 同 core，减少 cache miss）|

#### 为什么必须 Task 解耦？

```
若 I2S ISR 直接调用 f_write()（FATFS → SD 卡）：

1. f_write() 可能阻塞 50~100ms（SD 卡内部写操作）
2. 期间 I2S DMA 继续填充 buffer，RingBuffer 溢出
3. 结果：audio drop，录音文件中有"缺口"
4. 更严重：在 ISR 中调用 FATFS 是危险行为（FATFS 非线程安全，且可能阻塞）

解法：
- ISR 只做一件事：将 DMA buffer 拷贝到 RingBuffer（< 1ms）
- Recorder Task 负责所有 FATFS 调用（可阻塞，不影响 I2S）
```

#### Backpressure 处理

```
RingBuffer 满时（Recorder Task 跟不上 I2S）：

选项 A：丢弃最旧数据，继续运行（audio drop，但文件连续）
选项 B：阻塞 I2S ISR（不可行，会丢失 DMA 数据）
选项 C：降低采样率（运行时不可调）

推荐：选项 A + 统计 drop 计数，通过 event_bus 发布 EVENT_RECORDING_ERROR（warning 级别）
```

---

### 3.4 WAV Writer 层（文件封装）

| 属性 | 说明 |
|------|------|
| **职责** | 维护 WAV 文件头（44 bytes）；将 PCM 数据追加写入文件；停止时回写 data chunk size |
| **文件头** | 录音开始时写入 44 bytes 占位（data size 暂时填 0xFFFFFFFF）；停止时 `fseek()` 回写正确大小 |
| **写入策略** | 累积 4KB 再调用 `f_write()`（减少 FATFS 系统调用次数，提升 SD 卡写入效率）|
| **线程安全** | WAV Writer 只能由 Recorder Task 调用，不共享 |

#### 为什么不能直接同步写 SD？

```
SD 卡写入延迟来源：
1. 闪存块擦除（erase block）：~10~50ms
2. FAT 表更新（FATFS 每写入需更新 alloc table）
3. 写放大（write amplification）：小文件写入效率低

若每次 I2S DMA 完成（每 16ms）都调用 f_write()：
→ FATFS 频繁更新 alloc table
→ SD 卡写入延迟累积
→ RingBuffer 溢出

解法：RingBuffer + Task 缓冲，批量写入（4KB 对齐）。
```

---

### 3.5 FATFS / SD 卡层

| 属性 | 说明 |
|------|------|
| **挂载点** | `/sdcard`（通过 `storage_mount()` 挂载）|
| **文件系统** | FAT32（ESP-IDF 原生支持，兼容 macOS/Windows 直接读取）|
| **SPI 模式** | 使用 `CONFIG_IDF_TARGET_ESP32S3` + `spi_device_handle_t`（当前已配置）|
| **时钟频率** | 20 MHz（兼容杜邦线，稳定优先）|

#### SD 卡写入延迟风险

| 风险 | 影响 | 缓解措施 |
|------|------|---------|
| 突然掉电 | 文件系统损坏（FAT 表未更新）| checkpoint sync（每 30s）+ 停止录音时 sync |
| 写延迟峰值（100ms+）| RingBuffer 溢出 | RingBuffer ≥ 64KB；WAV Writer 批量写入 |
| SD 卡移除（热插拔）| `f_write()` 返回错误 | `EVENT_STORAGE_REMOVED` → 停止录音 → 进入 ERROR |
| FAT 表碎片 | 写入速度下降 | 定期格式化（用户主动）或维护一个 `lost+found` 检查 |

#### f_sync() 策略（v0.2 重大修正）

> ⚠️ **桌面思维陷阱**：原 v0.1 设计建议"每秒 f_sync()"，这是桌面系统的做法，
> 不适合 embedded audio pipeline。

```
桌面思维 vs 嵌入式思维：

桌面系统（PC/Mac）：
- 磁盘速度：100MB/s+
- 写入延迟：< 5ms
- sync() 开销可忽略
- 目标：数据一致性优先

Embedded SPI SD：
- 写入速度：1~5MB/s（SPI 瓶颈）
- 写入延迟：5~200ms（正常），300~800ms（峰值）
- sync() 开销：触发 FAT 表写回 + 可能触发 flash erase
- sync() 越频繁 → 延迟峰值越容易触发 → RingBuffer 溢出风险越大

为什么频繁 sync 是错误的？
─────────────────────────────────────────────────────────────
每次 f_sync() 会：
1. 将 DMA buffer 中的数据写入 flash
2. 更新 FAT 表（记录新的 cluster 分配）
3. 更新 directory entry（文件大小等元数据）
4. 可能触发 wear leveling 后台操作

这些操作加起来可能需要 50~200ms。
在 1 秒内执行多次 sync = 多次延迟峰值叠加
─────────────────────────────────────────────────────────────
```

**推荐 f_sync() 策略**：

| 时机 | 是否 sync | 理由 |
|------|----------|------|
| 正常录音期间 | ❌ 否 | 批量写入即可，不频繁 sync |
| 每 30 秒（checkpoint） | ✅ 可选 | 平衡崩溃恢复与写入稳定性 |
| 录音停止时 | ✅ 必须 | 确保 WAV 头正确写回，文件完整性 |
| 系统进入低功耗前 | ✅ 必须 | 确保所有数据写入 flash |
| SD 卡被移除前 | ✅ 必须 | 同上 |

**Checkpoint Sync 实现建议**：

```c
/* 伪代码：Recorder Task 主循环 */
#define CHECKPOINT_INTERVAL_MS 30000  /* 30 秒 */
static int64_t s_last_sync_time = 0;

void recorder_task_main(void *arg) {
    int64_t now = esp_timer_get_time() / 1000;
    
    /* 检查是否需要 checkpoint sync */
    if (now - s_last_sync_time >= CHECKPOINT_INTERVAL_MS) {
        f_sync(wav_file);  /* 同步，但不停止录音 */
        s_last_sync_time = now;
    }
    
    /* 正常批量写入 */
    if (write_buffer_size >= 4096) {
        f_write(wav_file, buffer, write_buffer_size, &bw);
        write_buffer_size = 0;
    }
}

/* recorder_stop() 中：*/
void recorder_stop(void) {
    /* 最后的 sync：写入所有 pending 数据 */
    f_sync(wav_file);
    
    /* 回写 WAV 头（data chunk size）*/
    wav_finalize_header(wav_file);
    
    f_close(wav_file);
}
```

> **设计总结**：减少 sync 频率是 embedded audio recording 的常见优化策略。
> 在保证崩溃恢复能力的前提下（30s checkpoint），最大化写入稳定性。

---

## 4. 数据流时序图

```
时间轴 →

I2S DMA:  [DMA0 done] [DMA1 done] [DMA2 done] [DMA3 done] [DMA0 done] ...
                      │            │
RingBuffer:            ●─────●─────────────●─────────────●
  (4KB)               │     │             │             │
                       ▼     ▼             ▼             ▼
Recorder Task:         [read] [read]       [read]       [read]
                          │                   │
WAV Writer:               [f_write 4KB]      [f_write 4KB]
                              │                   │
SD Card:                    [SPI write]         [SPI write]
```

> 关键：Recorder Task 的 `[read]` 可等待，不影响 I2S DMA 继续填充 RingBuffer。

---

## 5. 推荐配置汇总

| 参数 | 推荐值 | 理由 |
|------|--------|------|
| I2S DMA 描述符数量 | 4 | 平衡延迟与内存 |
| 每个 DMA buffer 大小 | 256 samples（512 bytes @ 16bit）| 16ms @ 16kHz，低延迟 |
| RingBuffer 总大小 | 64KB（推荐） | 吸收 SPI SD 卡 300~800ms latency spike |
| Recorder Task 优先级 | 3 | 高于上传（2），低于 ISR |
| Recorder Task 栈大小 | 8192 bytes | 含 FATFS 调用开销 |
| WAV 批量写入阈值 | 4096 bytes（4KB）| 减少 FATFS 调用次数 |
| `f_sync()` 频率 | 录音停止时 + 每 30s checkpoint | 详见 Task 3 修正说明 |
| 录音采样率 | 16000 Hz | Whisper 原生支持，文件大小适中 |
| 位深 | 16-bit | INMP441 输出 24-bit，但 16-bit 足够语音识别 |

---

## 6. 与现有代码的对接点

| 现有接口 | 对接方式 |
|---------|---------|
| `recorder_start(filename)` | 内部创建 Recorder Task，初始化 I2S，创建 RingBuffer |
| `recorder_stop()` | 设置停止标志，等待 Recorder Task 完成 flush，更新 WAV 头 |
| `recorder_is_recording()` | 返回 Recorder Task 是否仍在运行 |
| `EVENT_RECORDING_STARTED` | Recorder Task 成功启动 I2S 后发布 |
| `EVENT_RECORDING_STOPPED` | Recorder Task 完成文件写入后发布 |

---

## 7. 未决问题（v0.2 需验证）

| # | 问题 | 验证方法 |
|---|------|---------|
| 1 | ~~RingBuffer 4KB 是否足够？~~ | **已废弃**：RingBuffer 已升级为 64KB，详见 v0.2 设计说明 |
| 2 | I2S `auto_clear` 应设置 true 还是 false？| 查阅 ESP-IDF 文档：RX 模式 `auto_clear=false` 表示 DMA 不会自动清零 buffer（保留旧数据），通常应设 `true` |
| 3 | 是否需要使用 PSRAM 存放 RingBuffer？| v0.2 推荐使用 64KB IRAM RingBuffer；若 IRAM 不足，可将 RingBuffer 分配在 PSRAM |
| 4 | WAV 头回写是否需要在停止时 `fseek()`？| 是，`fseek(f, 0, SEEK_SET)` 回写 RIFF size 和 data size |

---

*文档版本：v0.1 | 作者：AI Assistant | 审核：待定*
