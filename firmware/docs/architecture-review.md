# 组件边界审查 — ESP32 AI Recorder

> Version: v0.2 | Updated: 2026-05-12
> 本文档分析当前组件边界耦合问题，输出建议（不进行大规模代码改写）。

---

## 1. 总体评估

| 组件 | 当前边界质量 | 风险等级 | 结论 |
|------|------------|---------|------|
| `event_bus` | ✅ 优秀 | — | 最小化依赖，纯粹 pub/sub |
| `state` | ✅ 良好 | 🟡 中 | 状态表硬编码，新增状态需同步修改 |
| `ui` | ✅ 良好 | 🟡 中 | LED 映射表硬编码，新增状态需同步更新 |
| `audio` | ✅ 良好 | 🟢 低 | HAL 边界清晰 |
| `led` | ✅ 良好 | 🟢 低 | HAL 边界清晰 |
| `button` | ✅ 良好 | 🟢 低 | 仅发布事件，无业务逻辑 |
| `storage` | ⚠️ 需扩展 | 🟡 中 | API 够用，但缺少 upload_queue 目录管理 |
| `recorder` | ⚠️ 强耦合 | 🔴 高 | 直接调用 `generate_filename()` + `time()`，时间戳问题 |
| `uploader` | ⚠️ 强耦合 | 🔴 高 | 依赖 `recorder.h`，与 recorder 紧耦合 |
| `wifi_manager` | ⚠️ 接口不清晰 | 🟡 中 | 需明确是否允许其他组件查询连接状态 |
| `app_main` | ⚠️ 业务逻辑过重 | 🟡 中 | `on_button_event()` 直接调用 `state_set()`，业务逻辑分散 |

---

## 2. 耦合问题详细分析

### 2.1 🔴 高风险：`recorder` 时间戳问题

**问题**：`recorder.c` 中 `generate_filename()` 使用 `time(NULL)`：

```c
/* recorder.c 第 99-108 行 */
time_t now = time(NULL);
struct tm *tm_info = localtime(&now);
snprintf(buffer, size, "/sdcard/%s_%04d%02d%02d_%02d%02d%02d.wav",
         prefix, tm_info->tm_year + 1900, tm_info->tm_mon + 1,
         tm_info->tm_mday, tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec);
```

**风险**：
- 系统上电后 `time(NULL)` 返回 1970 年（Unix epoch），录音文件名全是 `REC_19700101_000000.wav`
- 同一秒内多次录音会覆盖同名文件
- 与 WiFi NTP 同步机制未集成

**设计原则（已更新）**：
> **当前阶段目标：保证唯一性，而非真实时间。**

`esp_timer_get_time()` 是 monotonic uptime，**不适合**作为文件命名时间戳：
- 无法跨重启（每次重启从头开始）
- 不是真实 UTC 时间
- 上传后无法正确排序
- 无法作为长期文件时间语义

**正确方案（v0.2 实施）**：
使用 monotonic session naming：每次录音使用递增序号。

```c
/* 正确的 monotonic naming（v0.2）*/
static uint16_t s_session_counter = 0;

void generate_filename(char *buffer, size_t size) {
    s_session_counter++;
    snprintf(buffer, size, "/sdcard/REC_SESSION_%04u.wav", s_session_counter);
}
```

启动时扫描 `recordings/` 目录找到最大序号，初始化 `s_session_counter`。

**未来升级（v0.6+ WiFi 联网后）**：
```c
/* 联网后的 UTC naming（v0.6+）*/
if (wifi_manager_is_connected() && sntp_sync_done()) {
    snprintf(buffer, size, "/sdcard/REC_%04d%02d%02d_%02d%02d%02d.wav", ...);
} else {
    /* Fallback: monotonic naming */
    snprintf(buffer, size, "/sdcard/REC_SESSION_%04u.wav", s_session_counter);
}
```

---

### 2.2 🔴 高风险：`uploader` 与 `recorder` 耦合

**问题**：`uploader.h` 直接 `#include "recorder.h"`：

```c
/* uploader.h 第 15 行 */
#include "recorder.h"
```

**风险**：
- `uploader` 不需要 `recorder` 的任何 API，只是为了获取 `recorder_config_t`（这是 recorder 自己的结构体，不应在 uploader 中使用）
- 若未来 `recorder.h` 变更，`uploader` 必须重新编译
- 违反了"业务组件间禁止直接依赖"原则

**建议**：
1. 删除 `uploader.h` 中的 `#include "recorder.h"`
2. `uploader_config_t` 应独立定义，不依赖 `recorder_config_t`
3. 上传任务描述（JSON 文件路径）由 `recorder_stop()` 创建，上传任务由 `app_main` 的 `IDLE` 状态检查触发

```c
/* uploader.h 不应 include recorder.h */
/* recorder_config_t 应只在 recorder.c/.h 中使用 */
```

---

### 2.3 🟡 中风险：`state` 状态名称硬编码

**问题**：`state.c` 中的 `s_state_names[]` 数组硬编码了状态名称：

```c
/* state.c 第 12-19 行 */
static const char* s_state_names[] = {
    [DEVICE_STATE_INIT]       = "INIT",
    [DEVICE_STATE_IDLE]      = "IDLE",
    [DEVICE_STATE_RECORDING] = "RECORDING",
    /* ... 缺少新增状态（RECORD_ARMED 等）*/
};
```

**风险**：新增 `DEVICE_STATE_RECORD_ARMED` 等枚举值后，若忘记在 `s_state_names[]` 中添加对应项，`state_to_string()` 会返回 `"UNKNOWN"`。

**建议**：
1. 使用 `DEVICE_STATE_COUNT` 的编译期检查：
   ```c
   _Static_assert(sizeof(s_state_names)/sizeof(s_state_names[0]) == DEVICE_STATE_COUNT,
                  "state.c: s_state_names[] must have one entry per device_state_t");
   ```
2. 或使用 `__builtin_available()` 检查宏在编译期验证

---

### 2.4 🟡 中风险：`ui.c` LED 映射表硬编码

**问题**：`ui.c` 中 `s_state_map[]` 硬编码了状态与 LED 模式的对应关系：

```c
/* ui.c 第 42-50 行 */
static const state_led_map_t s_state_map[] = {
    { DEVICE_STATE_INIT,       LED_PATTERN_BREATHING, ... },
    { DEVICE_STATE_IDLE,       LED_PATTERN_STATIC,    ... },
    /* 缺少 DEVICE_STATE_RECORD_ARMED, RECORD_STOPPING, PROCESSING, LOW_BATTERY */
};
```

**风险**：新增状态后若忘记更新此表，`ui_get_map()` 会 fallback 到第一个元素（INIT 的模式），导致 LED 显示错误。

**建议**：
1. 在 `ui_init()` 中添加静态断言（与 state.c 相同策略）
2. 考虑将 LED 映射移到配置系统（NVS 或 `sdkconfig`）中，使 LED 行为可运行时配置

---

### 2.5 🟡 中风险：`app_main.c` 业务逻辑过重

**问题**：`app_main.c` 中的 `on_button_event()` 直接包含所有业务逻辑：

```c
/* app_main.c 第 104-113 行 */
case EVENT_BUTTON_CLICKED:
    if (state_get() == DEVICE_STATE_IDLE) {
        state_set(DEVICE_STATE_RECORDING);
    } else if (state_get() == DEVICE_STATE_RECORDING) {
        state_set(DEVICE_STATE_IDLE);
    }
    break;
```

**风险**：
- 随着业务复杂化，`on_button_event()` 会越来越长
- 按钮行为依赖状态机的具体状态值，耦合紧密
- 测试困难（需要完整初始化所有组件才能测试按钮行为）

**建议**：
1. 将按钮处理逻辑抽象为 `btn_handler.c`：
   ```c
   /* btn_handler.c: 订阅 EVENT_BUTTON_CLICKED，内部根据 state 处理 */
   ```
2. 使用状态表驱动按钮行为，而非 if-else 分支：
   ```c
   static const state_action_t s_btn_actions[DEVICE_STATE_COUNT] = {
       [DEVICE_STATE_IDLE]      = ACTION_START_RECORD,
       [DEVICE_STATE_RECORDING] = ACTION_STOP_RECORD,
       [DEVICE_STATE_ERROR]     = ACTION_RESET,
       /* ... */
   };
   ```
3. v0.2 阶段保持现状（if-else），v0.3+ 再重构

---

### 2.6 🟡 中风险：`wifi_manager` 状态查询接口缺失

**问题**：当前 `wifi_manager.h` 没有提供查询 WiFi 连接状态的公共 API。其他模块（如 `uploader`）需要知道 WiFi 是否已连接，目前只能通过订阅 `EVENT_WIFI_*` 事件来跟踪。

**建议**：
1. 添加 `wifi_manager_is_connected()` 函数：
   ```c
   bool wifi_manager_is_connected(void);
   ```
2. `app_main.c` 的 IDLE 状态检查上传队列时，先调用此函数确认 WiFi 连接

---

### 2.7 🟢 低风险：`audio_task` 高频日志

**问题**：`app_main.c` 中的 `audio_task()` 每 100ms 打印一次 RMS：

```c
/* app_main.c 第 83 行 */
ESP_LOGI(TAG, "Audio RMS: %.0f", (double)rms);  /* 每 100ms = 10 次/秒 */
```

**风险**：日志刷屏，影响调试效率。

**建议**：
1. 将 `ESP_LOGI` 改为 `ESP_LOGD`（默认关闭）
2. 添加计数器，每 10 次（1 秒）打印一次：
   ```c
   static int s_log_count = 0;
   if (++s_log_count >= 10) {
       ESP_LOGD(TAG, "RMS: %.0f", (double)rms);
       s_log_count = 0;
   }
   ```
3. v0.2 实施时在 `audio.c` 中统一处理

---

## 3. 接口抽象建议

### 3.1 当前可行的接口抽象

| 组件 | 建议添加的接口 | 理由 |
|------|--------------|------|
| `wifi_manager` | `wifi_manager_is_connected()` | 允许其他组件查询连接状态 |
| `wifi_manager` | `wifi_manager_get_ip(char *ip_out)` | 上传模块需要 IP（用于日志）|
| `storage` | `storage_mkdir(const char *path)` | `recorder`/`uploader` 需要创建目录 |
| `storage` | `storage_file_rename(src, dst)` | 上传成功后移动文件 |

### 3.2 暂不需要的抽象（v0.2 阶段）

| 抽象 | 原因 |
|------|------|
| `i2s_handle_t` 抽象 | 当前只有 `audio` 使用 I2S，无需抽象 |
| `recorder_interface.h` | recorder 不会在 v0.2 被替换，无需抽象 |
| OTA 接口 | v0.6 才需要 |

---

## 4. 各组件边界健康度评分

| 组件 | 评分（1-5）| 核心问题 |
|------|-----------|---------|
| `event_bus` | 5/5 | 无问题 |
| `state` | 4/5 | 状态名称硬编码（可接受）|
| `ui` | 4/5 | LED 映射表需同步更新 |
| `audio` | 5/5 | 边界清晰 |
| `led` | 5/5 | 边界清晰 |
| `button` | 5/5 | 只发布事件，做得很好 |
| `storage` | 3/5 | 缺少目录管理 API |
| `recorder` | 2/5 | 时间戳问题严重，需要尽快修复 |
| `uploader` | 2/5 | 与 recorder 耦合，需解耦 |
| `wifi_manager` | 3/5 | 缺少查询接口 |

---

## 5. 优先修复清单

| 优先级 | 问题 | 修复方式 | 工作量 | 状态 |
|--------|------|---------|--------|------|
| 🔴 P0 | recorder 时间戳问题 | 修改 `generate_filename()` 使用 monotonic session naming | < 15 分钟 | **已设计** |
| 🔴 P0 | uploader 包含 recorder.h | 删除 `#include "recorder.h"` | < 5 分钟 | 待实施 |
| 🟡 P1 | `s_state_names[]` 未同步更新 | 添加编译期静态断言 | < 10 分钟 | 待实施 |
| 🟡 P1 | `s_state_map[]` 未同步更新 | 添加编译期静态断言 | < 10 分钟 | 待实施 |
| 🟡 P1 | `wifi_manager` 缺少 `is_connected()` | 添加简单 getter 函数 | < 15 分钟 | 待实施 |
| 🟡 P1 | audio_task 高频日志 | 改为 `ESP_LOGD`，加计数器 | < 10 分钟 | 待实施 |
| 🟢 P2 | `app_main` 业务逻辑过重 | v0.3+ 再考虑 `btn_handler.c` | 中期 | 规划中 |

> **注意**：原 architecture-review.md v0.1 建议使用 `esp_timer_get_time()` 作为时间戳，这是**错误的设计**。
> `esp_timer_get_time()` 是 monotonic uptime，无法跨重启、无法排序、上传后无时间语义。
> 已更新为正确的 monotonic session naming 方案（见上方 Task 1 详细说明）。

---

*文档版本：v0.1 | 作者：AI Assistant | 审核：待定*
