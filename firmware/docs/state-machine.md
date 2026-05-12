# 状态机设计 — ESP32 AI Recorder

> Version: v0.3 | Updated: 2026-05-12
> 状态机是设备行为的最高权威，所有模块通过 `state_set()` 切换状态，通过订阅 `EVENT_STATE_CHANGED` 响应变化。

---

## 1. 状态总览

| # | 状态 | 枚举值 | 说明 |
|---|-------|--------|------|
| 1 | `INIT` | `DEVICE_STATE_INIT` | 系统初始化，各模块依次启动 |
| 2 | `IDLE` | `DEVICE_STATE_IDLE` | 待机，等待用户操作 |
| 3 | `RECORD_ARMED` | `DEVICE_STATE_RECORD_ARMED` | 已武装，预热身完成，随时开始录音 |
| 4 | `RECORDING` | `DEVICE_STATE_RECORDING` | 正在采集 I2S 音频并写入 SD 卡 |
| 5 | `RECORD_STOPPING` | `DEVICE_STATE_RECORD_STOPPING` | 停止请求已发出，正在 flush buffer、更新 WAV 头 |
| 6 | `PROCESSING` | `DEVICE_STATE_PROCESSING` | 后处理：修剪静音、更新 WAV 头、生成元数据 |
| 7 | `UPLOADING` | `DEVICE_STATE_UPLOADING` | 将 WAV 文件通过 HTTP 上传至 Mac 服务端 |
| 8 | `LOW_BATTERY` | `DEVICE_STATE_LOW_BATTERY` | 电量低警告状态，提示用户充电 |
| 9 | `ERROR` | `DEVICE_STATE_ERROR` | 可恢复错误，停留一段时间后自动恢复 |
| 10 | `SLEEP` | `DEVICE_STATE_SLEEP` | 深度睡眠，仅 GPIO0 中断或定时器可唤醒 |

---

## 2. 各状态详细说明

### 2.1 INIT

| 属性 | 说明 |
|------|------|
| **职责** | 按顺序初始化所有模块：NVS → event_bus → state → LED → button → ui → audio → storage → WiFi → recorder → battery → uploader |
| **进入条件** | 系统上电，自动进入 |
| **退出条件** | 所有模块初始化完成 → `IDLE`；任一关键模块失败 → `ERROR` |
| **可触发事件** | `EVENT_STORAGE_READY` / `EVENT_STORAGE_ERROR`、`EVENT_WIFI_CONNECTED` / `EVENT_WIFI_DISCONNECTED`、`EVENT_BATTERY_LOW` / `EVENT_BATTERY_CRITICAL` |
| **LED 行为** | 白灯常亮 |
| **允许录音** | ❌ 否 |
| **允许 WiFi** | ⚠️ 初始化中（内部） |
| **允许上传** | ❌ 否 |

---

### 2.2 IDLE

| 属性 | 说明 |
|------|------|
| **职责** | 等待用户操作；周期性检查待上传文件；监控电池状态 |
| **进入条件** | `INIT` 完成；或 `RECORD_STOPPING`/`PROCESSING`/`UPLOADING`/`ERROR` 完成恢复 |
| **退出条件** | 单击按钮 → `RECORD_ARMED`；有待上传文件且 WiFi 已连接 → `UPLOADING`；空闲超时 → `SLEEP`；电池低于阈值 → `LOW_BATTERY` |
| **可触发事件** | `EVENT_BUTTON_CLICKED`、`EVENT_WIFI_CONNECTED`、`EVENT_UPLOAD_DONE`、`EVENT_BATTERY_LOW` |
| **LED 行为** | 绿灯慢闪（1 Hz） |
| **允许录音** | ⚠️ 单击后进入 `RECORD_ARMED`，不直接录音 |
| **允许 WiFi** | ✅ 是 |
| **允许上传** | ✅ 是（在有待上传文件时） |

---

### 2.3 RECORD_ARMED

| 属性 | 说明 |
|------|------|
| **职责** | 录音"武装"状态：预分配文件、初始化 I2S DMA、准备好后立即进入 `RECORDING`。此状态存在时间极短（< 100ms），用于平滑过渡 |
| **进入条件** | `IDLE` + `EVENT_BUTTON_CLICKED` |
| **退出条件** | I2S/DMA 准备完成 → `RECORDING`；准备失败 → `ERROR` |
| **可触发事件** | `EVENT_RECORDING_STARTED`（内部）、`EVENT_STORAGE_ERROR` |
| **LED 行为** | 红灯快闪（5 Hz），表示"即将开始" |
| **允许录音** | ⚠️ 准备中，尚未开始实际录音 |
| **允许 WiFi** | ❌ 否（录音期间关闭 WiFi 以降功耗和减少干扰） |
| **允许上传** | ❌ 否 |

> **设计说明**：`RECORD_ARMED` 是一个短暂的中间状态，目的是将"用户触发"与"实际启动 I2S DMA"解耦，避免在 `IDLE` 状态的处理函数中执行耗时操作。

---

### 2.4 RECORDING

| 属性 | 说明 |
|------|------|
| **职责** | I2S 音频持续采集 → RingBuffer → WAV 文件写入 SD 卡；监控存储空间和电池 |
| **进入条件** | `RECORD_ARMED` + I2S/DMA 准备完成 |
| **退出条件** | 单击按钮 → `RECORD_STOPPING`；SD 卡满 → `RECORD_STOPPING`（带错误标记）；电池极低 → `RECORD_STOPPING` → `LOW_BATTERY` |
| **可触发事件** | `EVENT_RECORDING_STOPPED`（内部停止）、`EVENT_STORAGE_ERROR`、`EVENT_BATTERY_CRITICAL` |
| **LED 行为** | 红灯快闪（5 Hz）|
| **允许录音** | ✅ 是（正在录音） |
| **允许 WiFi** | ❌ 否 |
| **允许上传** | ❌ 否 |

---

### 2.5 RECORD_STOPPING

| 属性 | 说明 |
|------|------|
| **职责** | 停止 I2S DMA；flush RingBuffer 中剩余数据；更新 WAV 文件头（data chunk size）；关闭文件 |
| **进入条件** | `RECORDING` + 停止条件触发 |
| **退出条件** | 文件关闭完成 → `PROCESSING`（如果需要后处理）或 `IDLE`（如果不需要） |
| **可触发事件** | `EVENT_RECORDING_STOPPED`（完成） |
| **LED 行为** | 橙灯呼吸（表示"正在停止"）|
| **允许录音** | ❌ 否（正在停止） |
| **允许 WiFi** | ❌ 否 |
| **允许上传** | ❌ 否 |

> **设计说明**：此状态的存在是因为 SD 卡写入有延迟（尤其是 FATFS 更新文件 alloc table 时）。不能直接从 `RECORDING` 跳到 `IDLE`，否则文件可能损坏。

---

### 2.6 PROCESSING

| 属性 | 说明 |
|------|------|
| **职责** | WAV 文件后处理：确认文件头完整、可选修剪开头/结尾静音、生成上传任务描述文件（JSON）|
| **进入条件** | `RECORD_STOPPING` + 文件关闭完成 |
| **退出条件** | 后处理完成 → `IDLE`（如果不需要上传）或 自动触发 → `UPLOADING`；处理失败 → `ERROR` |
| **可触发事件** | `EVENT_UPLOAD_STARTED`（自动触发）|
| **LED 行为** | 蓝灯呼吸 |
| **允许录音** | ❌ 否 |
| **允许 WiFi** | ✅ 是（准备上传） |
| **允许上传** | ⚠️ 即将开始 |

> **设计说明**：v0.2 可简化：后处理为空，直接从 `RECORD_STOPPING` → `IDLE`。此状态保留为 v0.3+ 扩展预留。

---

### 2.7 UPLOADING

| 属性 | 说明 |
|------|------|
| **职责** | HTTP POST WAV 文件到 Mac 服务端；支持进度回调；失败时重试（最多 3 次）|
| **进入条件** | `PROCESSING` 完成 或 `IDLE` 中检测到待上传文件 |
| **退出条件** | 上传成功 → `IDLE`；重试耗尽 → `ERROR`；WiFi 断开 → `IDLE`（暂停上传）|
| **可触发事件** | `EVENT_UPLOAD_DONE`、`EVENT_UPLOAD_FAILED`、`EVENT_WIFI_DISCONNECTED` |
| **LED 行为** | 蓝灯呼吸（与 PROCESSING 区分：频率不同）|
| **允许录音** | ❌ 否 |
| **允许 WiFi** | ✅ 是（正在使用） |
| **允许上传** | ✅ 是（正在上传） |

---

### 2.8 LOW_BATTERY

| 属性 | 说明 |
|------|------|
| **职责** | 提示用户充电；如果正在录音，先安全停止；超时后自动进入 `SLEEP` |
| **进入条件** | 任意状态 + `EVENT_BATTERY_LOW`（电量 < 15%）|
| **退出条件** | 用户充电（ADC 检测电压回升）→ `IDLE`；超时无操作 → `SLEEP` |
| **可触发事件** | `EVENT_BATTERY_LOW`（持续检测）、`EVENT_BUTTON_CLICKED`（用户忽略警告）|
| **LED 行为** | 红灯慢闪（1 Hz，与 IDLE 的绿灯区分）|
| **允许录音** | ❌ 否（禁止新录音） |
| **允许 WiFi** | ❌ 否（省电） |
| **允许上传** | ❌ 否 |

---

### 2.9 ERROR

| 属性 | 说明 |
|------|------|
| **职责** | 可恢复错误停留状态；显示错误类型（通过 LED 闪码）；超时或按钮触发恢复 |
| **进入条件** | 任一状态发生可恢复错误（SD 卡错误、上传失败且重试耗尽、I2S 错误等）|
| **退出条件** | 3s 后自动恢复 → `IDLE`；或 `EVENT_BUTTON_CLICKED` 立即恢复 |
| **可触发事件** | `EVENT_BUTTON_CLICKED`（手动恢复）|
| **LED 行为** | 红灯快闪（10 Hz，错误码通过闪码表示，见下文）|
| **允许录音** | ❌ 否 |
| **允许 WiFi** | ⚠️ 取决于错误类型 |
| **允许上传** | ❌ 否 |

#### 错误码 LED 闪码

| 错误类型 | 闪码（红灯快闪 N 次后暂停 1s） |
|---------|-----------------------------|
| SD 卡卸载/错误 | 1 闪 |
| WiFi 连接失败 | 2 闪 |
| 上传失败（重试耗尽） | 3 闪 |
| I2S / 音频错误 | 4 闪 |
| 未知错误 | 5 闪 |

---

### 2.10 SLEEP

| 属性 | 说明 |
|------|------|
| **职责** | 深度睡眠（deep sleep），仅 GPIO0 中断或定时器可唤醒；功耗 < 20µA |
| **进入条件** | `IDLE` 超时（默认 60s 无操作）；或 `LOW_BATTERY` 超时 |
| **退出条件** | GPIO0 中断（按钮按下）→ `INIT`（唤醒后相当于重启）；定时器唤醒 → `INIT` |
| **可触发事件** | （深度睡眠期间无事件处理，唤醒后重新 INIT） |
| **LED 行为** | 熄灭 |
| **允许录音** | ❌ 否 |
| **允许 WiFi** | ❌ 否 |
| **允许上传** | ❌ 否 |

---

## 3. 完整状态迁移表

| 从 \ 到 | INIT | IDLE | RECORD_ARMED | RECORDING | RECORD_STOPPING | PROCESSING | UPLOADING | LOW_BATTERY | ERROR | SLEEP |
|---------|------|------|--------------|-----------|------------------|------------|-----------|-------------|-------|-------|
| **INIT** | — | ✅ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ✅ | ❌ |
| **IDLE** | ❌ | — | ✅ | ❌ | ❌ | ❌ | ✅ | ✅ | ✅ | ✅ |
| **RECORD_ARMED** | ❌ | ❌ | — | ✅ | ❌ | ❌ | ❌ | ❌ | ✅ | ❌ |
| **RECORDING** | ❌ | ❌ | ❌ | — | ✅ | ❌ | ❌ | ✅ | ✅ | ❌ |
| **RECORD_STOPPING** | ❌ | ❌ | ❌ | ❌ | — | ✅ | ❌ | ❌ | ✅ | ❌ |
| **PROCESSING** | ❌ | ✅ | ❌ | ❌ | ❌ | — | ✅ | ❌ | ✅ | ❌ |
| **UPLOADING** | ❌ | ✅ | ❌ | ❌ | ❌ | ❌ | — | ❌ | ✅ | ❌ |
| **LOW_BATTERY** | ❌ | ✅ | ❌ | ❌ | ❌ | ❌ | ❌ | — | ❌ | ✅ |
| **ERROR** | ❌ | ✅ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | — | ❌ |
| **SLEEP** | ✅ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | — |

> ✅ = 合法迁移；❌ = 非法迁移

---

## 4. 非法状态迁移说明

| 非法迁移 | 原因 |
|---------|------|
| `INIT` → `RECORD_ARMED` / `RECORDING` 等 | 初始化未完成，不允许用户操作 |
| `IDLE` → `RECORDING`（跳过 `RECORD_ARMED`） | 必须经过武装状态完成 I2S 准备 |
| `RECORDING` → `IDLE`（跳过 `RECORD_STOPPING`） | 直接跳回 IDLE 会导致 WAV 文件头未更新，文件损坏 |
| `RECORD_STOPPING` → `RECORDING`（回退） | 停止中不可重启，须先回到 IDLE |
| `PROCESSING` → `RECORDING` | 后处理阶段不允许重新开始录音 |
| `UPLOADING` → `RECORDING` | 上传中不允许录音（资源冲突） |
| `SLEEP` → 任意状态（除 `INIT`） | 深度睡眠只能通过唤醒+重新 INIT 退出 |
| 任意状态 → 任意状态（通过 `state_set()` 强制设相同值） | `state_set()` 内会检查，相同状态直接返回 `ESP_OK`，不触发迁移 |

---

## 5. state.h API 设计（配套更新）

```c
/* 新增状态枚举值 */
typedef enum {
    DEVICE_STATE_INIT = 0,
    DEVICE_STATE_IDLE,
    DEVICE_STATE_RECORD_ARMED,      /* 新增 */
    DEVICE_STATE_RECORDING,
    DEVICE_STATE_RECORD_STOPPING,   /* 新增 */
    DEVICE_STATE_PROCESSING,        /* 新增 */
    DEVICE_STATE_UPLOADING,
    DEVICE_STATE_LOW_BATTERY,       /* 新增 */
    DEVICE_STATE_ERROR,
    DEVICE_STATE_SLEEP,
    DEVICE_STATE_COUNT,
} device_state_t;
```

---

## 6. LED 行为总表

| 状态 | 颜色 | 模式 | 频率/说明 |
|------|------|------|-----------|
| INIT | 白 | 常亮 | — |
| IDLE | 绿 | 慢闪 | 1 Hz |
| RECORD_ARMED | 红 | 快闪 | 5 Hz |
| RECORDING | 红 | 快闪 | 5 Hz |
| RECORD_STOPPING | 橙 | 呼吸 | — |
| PROCESSING | 蓝 | 呼吸（慢） | ~0.5 Hz |
| UPLOADING | 蓝 | 呼吸（快） | ~1 Hz |
| LOW_BATTERY | 红 | 慢闪 | 1 Hz |
| ERROR | 红 | 快闪（错误码） | 10 Hz |
| SLEEP | 灭 | — | — |

> 注：`RECORD_ARMED` 与 `RECORDING` LED 行为相同，实际中 `RECORD_ARMED` 停留时间极短（< 100ms），肉眼不可区分，无实际问题。

---

## 7. 设计原则

1. **单一入口，单一出口**：每个状态只有一个主要出口条件（`state_set()` 调用点），便于排查。
2. **不可跳过中间状态**：`RECORDING` → `IDLE` 必须经过 `RECORD_STOPPING`，由 `state.c` 内的迁移 guard 保证（可选实现）。
3. **ERROR 是唯一"逃生舱"**：任意状态发生异常均可进入 `ERROR`，从 `ERROR` 只能回到 `IDLE`。
4. **SLEEP 是"终点"**：进入 SLEEP 后相当于系统关闭，唤醒后重新 INIT。
5. **事件驱动，禁止轮询**：所有状态迁移由事件触发，模块不得循环调用 `state_get()` 判断状态。

---

## 8. 与 event_bus 的交互

```
[按钮单击]
  → event_bus_publish(EVENT_BUTTON_CLICKED)
    → app_main 订阅回调
      → state_set(DEVICE_STATE_RECORD_ARMED)
        → event_bus_publish(EVENT_STATE_CHANGED, {INIT→RECORD_ARMED})
          → recorder 订阅回调：启动 I2S DMA
          → ui 订阅回调：更新 LED 为红色快闪
```

状态变化永远通过 `state_set()` 触发，所有模块通过 `EVENT_STATE_CHANGED` 被动响应。

---

*文档版本：v0.3 | 作者：AI Assistant | 审核：待定*
