# 实施计划 v0.2 — 稳定 WAV 录音 Pipeline

> Version: v0.1 | Updated: 2026-05-12
> 目标：在真实硬件上实现可工作的 WAV 录音管道，按三个阶段分步验证。

---

## 阶段总览

| 阶段 | 目标 | 关键里程碑 | 风险 |
|------|------|-----------|------|
| **Phase 1** | I2S 真实音频采集 | INMP441 数据在终端可读 | 🔴 高（硬件未到）|
| **Phase 2** | RingBuffer + Recorder Task | 数据在 task 间流动，无丢数 | 🔴 中 |
| **Phase 3** | SD 卡 WAV 写入 + 完整集成 | WAV 文件可播放，上传成功 | 🟡 中 |

---

## Phase 1：I2S 真实音频采集

**预计工作量**：1~2 天（硬件到货后）
**依赖**：ESP32-S3 开发板 + INMP441 麦克风 + 杜邦线

### 目标

替换 `audio.c` 中的 stub I2S 配置，使用真实的 INMP441 I2S 外设采集音频数据。

### 实施步骤

#### Step 1.1：验证 I2S 硬件连接（硬件层）

```
操作：
1. 检查 INMP441 与 ESP32-S3 的接线：
   - INMP441 WS  → ESP32-S3 GPIO5
   - INMP441 SCK → ESP32-S3 GPIO4
   - INMP441 SD  → ESP32-S3 GPIO6
   - INMP441 L/R → GND（左声道模式）
   - INMP441 VCC → 3.3V
   - INMP441 GND → GND

2. 使用逻辑分析仪（或示波器）验证：
   - BCLK 有脉冲输出（ESP32-S3 作为 master）
   - WS 有信号（16kHz 方波）
   - SD 数据线有数据活动（非恒高/恒低）

3. 如果 SD 线无活动 → 检查 INMP441 供电或接线
```

#### Step 1.2：实现 `audio.c` 真实 I2S 读取

```
修改文件：components/audio/audio.c

关键变更：
1. 删除 stub 的 sine wave 生成代码
2. 调用 i2s_new_channel() + i2s_channel_init_std_mode()
3. 配置 I2S STD 模式（bit_shift=true, ws_width=1）
4. 使用 i2s_channel_read() 读取 PCM 数据
5. 返回实际读取的 sample 数量

验证：串口每 100ms 打印一次 RMS 值，观察有真实数据（非零值）
```

#### Step 1.3：添加 I2S DMA 配置

```
修改文件：components/audio/audio.c

关键变更：
1. 配置 DMA（desc_num=4, frame_num=256）
2. 使用 Ring Buffer 模式（而非每次 read）
3. 在 I2S 完成回调中将数据放入 Ring Buffer（预留，为 Phase 2 准备）

验证：
- ESP_LOGD 每秒打印 RMS（数据 > 0）
- 静音时 RMS ~ 0，有声音时 RMS > 100
- 数据持续稳定，无长时间中断
```

### Phase 1 验证方法

| 验证点 | 方法 | 预期结果 |
|--------|------|---------|
| I2S 硬件连接 | 逻辑分析仪观察 WS/BCLK/SD | WS=16kHz, BCLK 有脉冲，SD 有数据 |
| I2S 读取 | ESP_LOGD 每秒打印 RMS | 正常说话 RMS > 100，静音 RMS ~ 0 |
| DMA 配置 | 检查 esp_log 中无 DMA errors | 无 `I2S ERROR` 日志 |
| 数据连续性 | 连续观察 RMS 1 分钟 | 无长时间（> 1s）的 RMS=0 |

### Phase 1 完成标准

- [ ] INMP441 数据可读取（RMS 非零）
- [ ] 无 I2S 初始化错误
- [ ] `audio_read()` 返回正确数量的 samples
- [ ] 数据在 1 分钟内持续稳定（无 DMA 丢失）

---

## Phase 2：RingBuffer + Recorder Task 解耦

**预计工作量**：2~3 天
**依赖**：Phase 1 完成

### 目标

将 `audio.c` 的 I2S 读取与录音逻辑分离，建立独立的 Recorder Task，消除同步阻塞风险。

### 实施步骤

#### Step 2.1：创建 Recorder Task

```
新增文件：components/recorder/recorder_task.c

关键实现：
1. xTaskCreatePinnedToCore(recorder_task, "recorder", 8192, NULL, 3, NULL, 0);
2. Task 主循环：
   while (!s_stop_requested) {
       void *buf = xRingbufferReceive(s_ringbuf, &size, portMAX_DELAY);
       if (buf) {
           f_write(s_wav_file, buf, size, &written);
           vRingbufferReturnItem(s_ringbuf, buf);
       }
   }
3. s_stop_requested 由 recorder_stop() 设置
```

#### Step 2.2：实现 RingBuffer（生产者-消费者）

```
修改文件：components/audio/audio.c

关键变更：
1. audio_init() 中创建 Ring Buffer：
   s_ringbuf = xRingbufferCreate(4096, RINGBUF_TYPE_NOSPLIT);

2. audio_read() 改为填充 Ring Buffer（生产者）
   /* I2S DMA 回调中调用 xRingbufferSendFromISR() */

3. 暴露 audio.h 接口：
   void audio_start_streaming(void);   /* 启动 DMA + Ring Buffer */
   void audio_stop_streaming(void);    /* 停止 DMA */
```

#### Step 2.3：集成到 recorder_start()/stop()

```
修改文件：components/recorder/recorder.c

recorder_start():
  1. 调用 audio_start_streaming()  ← 启动 I2S DMA
  2. 创建 Recorder Task（若未创建）
  3. 发布 EVENT_RECORDING_STARTED

recorder_stop():
  1. audio_stop_streaming()         ← 停止 I2S DMA
  2. 设置 s_stop_requested = true   ← 通知 Recorder Task
  3. 等待 Task 退出（xTaskNotifyWait）
  4. 更新 WAV 头
  5. 发布 EVENT_RECORDING_STOPPED
```

### Phase 2 验证方法

| 验证点 | 方法 | 预期结果 |
|--------|------|---------|
| RingBuffer 不丢数据 | 对比：I2S 读到的 samples vs 写入文件的 samples | 误差 < 1% |
| Task 解耦 | 在 SD 卡写入期间观察 RMS 打印 | RMS 持续（task 未被 SD 卡写入阻塞）|
| WAV 文件完整 | 将录音文件导入电脑，用 Audacity 打开 | 音频可播放，无明显杂音/截断 |
| 停止流程 | 按下按钮停止录音 | WAV 文件头正确（data size 非零）|

### Phase 2 完成标准

- [ ] WAV 文件可播放（用 Audacity/macOS afplay 验证）
- [ ] 文件时长与实际录音时长一致（误差 < 1s）
- [ ] `audio_task`（Phase 1 的测试 task）可正常删除或合并
- [ ] Recorder Task 栈水位线 > 2KB（无溢出风险）

---

## Phase 3：SD 卡写入优化 + 完整状态机集成

**预计工作量**：2~3 天
**依赖**：Phase 2 完成

### 目标

优化 SD 卡写入（批量写入、分段 f_sync），与完整状态机集成，WiFi 上传可工作。

### 实施步骤

#### Step 3.1：实现 WAV 文件批量写入

```
修改文件：components/recorder/recorder.c

关键变更：
1. 不再每 RingBuffer 数据包都调用 f_write()
2. 累积 4KB 后调用一次 f_write()
3. 每 4KB 调用 f_sync()（可选，降低掉电损坏风险）

static uint32_t s_pending_bytes = 0;
static uint8_t  s_write_buf[4096];

void recorder_append_data(const void *data, size_t len) {
    memcpy(s_write_buf + s_pending_bytes, data, len);
    s_pending_bytes += len;
    if (s_pending_bytes >= 4096) {
        f_write(s_wav_file, s_write_buf, s_pending_bytes, NULL);
        s_pending_bytes = 0;
    }
}
```

#### Step 3.2：完善状态机集成

```
修改文件：components/state/state.c

关键变更（根据 docs/state-machine.md v0.3）：
1. 添加新枚举值：RECORD_ARMED, RECORD_STOPPING, PROCESSING, LOW_BATTERY
2. 更新 s_state_names[]（重要！需同步）

修改文件：components/ui/ui.c

关键变更：
1. s_state_map[] 补充新状态的 LED 模式
2. 添加编译期静态断言检查

修改文件：main/app_main.c

关键变更：
1. on_button_event() 中添加 RECORD_ARMED 状态逻辑
2. 订阅 EVENT_RECORDING_STOPPED，触发状态迁移
```

#### Step 3.3：实现 WiFi 上传

```
修改文件：components/uploader/uploader.c

关键变更：
1. 实现 HTTP POST 上传（使用 esp_http_client）
2. 从 upload_queue/ 读取 JSON 任务文件
3. 实现重试逻辑（最多 3 次）

验证：
1. 录制 10 秒音频
2. 检查 recordings/ 下有 WAV 文件
3. 检查 upload_queue/ 下有 JSON 文件
4. WiFi 连接后自动上传
5. uploaded/ 下有归档文件
```

### Phase 3 验证方法

| 验证点 | 方法 | 预期结果 |
|--------|------|---------|
| 批量写入 | 录制 1 分钟，观察 ESP_LOGD 每 4KB 打印一次 | 每 ~256ms 打印一次 |
| 掉电恢复 | 录音中拔电池，检查 WAV 文件 | WAV 文件可修复（data size 回写）|
| 状态机 | 完整流程：IDLE → ARMED → RECORDING → STOPPING → IDLE | LED 正确响应，无状态错乱 |
| WiFi 上传 | 启动 Mac 服务端，录制并触发上传 | macOS `received/` 目录有 WAV 文件 |
| 端到端 | 完整测试：录制 → 自动上传 → Whisper 转写 | transcript.txt 生成，内容正确 |

### Phase 3 完成标准

- [ ] WAV 文件 1 分钟录音，文件大小约 1.92MB（误差 < 5%）
- [ ] 状态机完整流程可正常运行（无非法状态转换）
- [ ] WiFi 上传成功率 > 95%（同一局域网内）
- [ ] `received/` 目录下有正确 WAV 文件
- [ ] mlx-whisper 可成功转写（音频格式正确）
- [ ] 无 audio drop（RingBuffer 无溢出告警）

---

## 风险矩阵

| 风险 | 阶段 | 影响 | 缓解措施 |
|------|------|------|---------|
| INMP441 接线错误 | Phase 1 | 高：无法采集音频 | 先用逻辑分析仪验证信号；参考 docs/hardware.md 接线表 |
| SD 卡写入阻塞 | Phase 2 | 中：audio drop | RingBuffer ≥ 4KB；WAV Writer 批量写入 |
| 杜邦线接触不良（已知 BUG-001）| Phase 2 | 中：SD 卡超时 | 等待硬件焊接方案；或在 Phase 2 测试时用胶带加固 |
| WiFi 不稳定 | Phase 3 | 低：上传失败 | 实现 3 次重试；失败后留在 upload_queue/ |
| FATFS 文件损坏 | Phase 3 | 中：录音文件不可用 | 每 4KB f_sync()；掉电后 wav_repair_header() 恢复 |
| 时间戳冲突（同秒内多次录音）| Phase 3 | 低：文件覆盖 | 使用 `esp_timer_get_time()` 作为唯一时间戳 |

---

## 硬件验证检查清单

每次 Phase 开始前，必须完成硬件验证：

```
□ ESP32-S3 开发板正常供电（LED 亮）
□ USB 连接 Mac，可通过 esptool.py 烧录固件
□ INMP441 接线正确（GPIO 4/5/6）
□ TF 卡插入，卡槽接触良好
□ WS2812B LED 连接在 GPIO48
□ GPIO0 按钮连接正确
□ 电池 ADC 接线正确（GPIO1）
□ Mac 端 FastAPI 服务运行在 :8000
□ Mac 端与 ESP32 在同一局域网（WiFi SSID 配置正确）
□ 手机/电脑开启麦克风测试音频（用手敲麦克风验证 INMP441 有响应）
```

---

## 文档交付清单

| 文档 | Phase | 状态 |
|------|-------|------|
| `docs/recorder-pipeline.md` | Phase 1 之前 | ✅ 已完成 |
| `docs/storage-layout.md` | Phase 2 之前 | ✅ 已完成 |
| `docs/recorder-api.md` | Phase 2 之前 | ✅ 已完成 |
| `docs/event-system.md` | Phase 2 之前 | ✅ 已完成 |
| `docs/state-machine.md` (v0.3) | Phase 3 之前 | ✅ 已完成 |
| `docs/error-handling.md` | Phase 3 之前 | ✅ 已完成 |
| `firmware 测试脚本` | Phase 1 | 待开发 |
| `WiFi 上传测试脚本` | Phase 3 | 待开发 |

---

*文档版本：v0.1 | 作者：AI Assistant | 审核：待定*
