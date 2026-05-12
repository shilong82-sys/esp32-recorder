# Event Bus 规范 — ESP32 AI Recorder

> Version: v0.1 | Updated: 2026-05-12
> 本文档定义全局事件总线（event_bus）的统一规范，所有模块必须遵守。

---

## 1. 设计目标

1. **完全解耦**：模块间不准直接函数调用，统一通过 `event_bus_publish()` + `event_bus_subscribe()` 通信。
2. **可预测**：事件命名、payload、ownership 有明确规则，任意开发者/AI 可无歧义使用。
3. **同步分发**：当前实现为同步调用（订阅者回调在 `publish()` 返回前执行完毕），简化时序分析。
4. **可扩展**：新增事件类型只需在 `event_type_t` 枚举中追加，不影响已有代码。

---

## 2. 事件命名规范

### 2.1 命名格式

```
EVENT_<模块前缀>_<事件名>
```

| 规则 | 说明 |
|------|------|
| 全部大写，单词间用下划线分隔 | 符合 ESP-IDF 宏命名惯例 |
| 过去时（后置 `_ED` / `_DONE` 等）| 表示"事件已发生"，避免与命令（imperative）混淆 |
| 名词优先 | `EVENT_WIFI_CONNECTED`（状态已到达）而非 `EVENT_WIFI_CONNECT`（动作）|

### 2.2 标准事件名清单

#### Button 事件

| 事件名 | 含义 | payload |
|--------|------|---------|
| `EVENT_BUTTON_PRESSED` | 按键刚按下（去抖后） | `event_button_data_t` |
| `EVENT_BUTTON_RELEASED` | 按键刚松开 | `event_button_data_t` |
| `EVENT_BUTTON_CLICKED` | 单击（按下后松开，无长按）| `event_button_data_t` |
| `EVENT_BUTTON_DOUBLE_CLICKED` | 双击 | `event_button_data_t` |
| `EVENT_BUTTON_LONG_PRESSED` | 长按首次触发（达到阈值） | `event_button_data_t` |
| `EVENT_BUTTON_HOLD` | 长按持续中（每 500ms 重复） | `event_button_data_t` |

#### Recorder 事件

| 事件名 | 含义 | payload |
|--------|------|---------|
| `EVENT_RECORDING_STARTED` | 录音已开始（I2S DMA 启动完成） | `event_recording_data_t` |
| `EVENT_RECORDING_STOPPED` | 录音已停止（文件已关闭） | `event_recording_data_t` |
| `EVENT_RECORDING_TIMEOUT` | 录音达到最大时长自动停止 | `event_recording_data_t` |
| `EVENT_RECORDING_ERROR` | 录音过程中发生错误 | `event_error_data_t` |

#### Storage 事件

| 事件名 | 含义 | payload |
|--------|------|---------|
| `EVENT_STORAGE_READY` | TF 卡挂载成功 | `NULL` |
| `EVENT_STORAGE_ERROR` | TF 卡错误（卸载、写入失败等） | `event_error_data_t` |
| `EVENT_STORAGE_REMOVED` | TF 卡被物理移除 | `NULL` |

#### WiFi 事件

| 事件名 | 含义 | payload |
|--------|------|---------|
| `EVENT_WIFI_CONNECTED` | WiFi 已连接，获得 IP | `event_wifi_data_t` |
| `EVENT_WIFI_DISCONNECTED` | WiFi 断开 | `event_wifi_data_t` |

#### Uploader 事件

| 事件名 | 含义 | payload |
|--------|------|---------|
| `EVENT_UPLOAD_STARTED` | 上传已开始 | `event_upload_data_t` |
| `EVENT_UPLOAD_SUCCESS` | 上传成功完成 | `event_upload_data_t` |
| `EVENT_UPLOAD_FAILED` | 上传失败（重试耗尽） | `event_upload_data_t` |
| `EVENT_UPLOAD_PROGRESS` | 上传进度更新 | `event_upload_progress_data_t` |

#### Battery 事件

| 事件名 | 含义 | payload |
|--------|------|---------|
| `EVENT_BATTERY_LOW` | 电量低（< 15%）| `event_battery_data_t` |
| `EVENT_BATTERY_CRITICAL` | 电量极低（< 5%）| `event_battery_data_t` |

#### 状态机事件（由 state 组件统一发布）

| 事件名 | 含义 | payload |
|--------|------|---------|
| `EVENT_STATE_CHANGED` | 设备状态变化 | `event_state_data_t` |

---

## 3. Payload 规范

### 3.1 通用规则

1. **必须使用结构体**，禁止使用裸 `int` / `char*` 作为 payload。
2. 结构体命名格式：`event_<事件名小写>_data_t`
3. payload 指针由发布者在栈上分配，**订阅者回调返回后 payload 内存无效**。若需异步使用，订阅者必须拷贝数据。
4. `data_len` 参数必须等于 `sizeof(struct_type)`，订阅者应首选 `sizeof` 校验。

### 3.2 标准 payload 结构体

```c
/* ---- 按键事件 ---- */
typedef struct {
    int gpio_num;       /* 触发事件的 GPIO 编号 */
    uint32_t press_ms;  /* 按下持续时间（ms），RELEASED/CLICKED 时有意义 */
} event_button_data_t;

/* ---- 录音事件 ---- */
typedef struct {
    char file_path[128];  /* 录音文件路径 */
    uint32_t duration_ms;  /* 录音时长（ms）*/
    size_t file_size;      /* 文件大小（bytes）*/
} event_recording_data_t;

/* ---- 存储事件 ---- */
typedef struct {
    esp_err_t error_code;  /* 错误码（非错误事件时为 ESP_OK）*/
    char mount_point[32];  /* 挂载点，如 "/sdcard" */
} event_storage_data_t;

/* ---- WiFi 事件 ---- */
typedef struct {
    char ip_addr[16];     /* 获得的 IP 地址字符串 */
    esp_err_t disconnect_reason; /* 断开原因（CONNECTED 时为 ESP_OK）*/
} event_wifi_data_t;

/* ---- 上传事件 ---- */
typedef struct {
    char file_path[128];  /* 正在上传的文件路径 */
    int  status_code;      /* HTTP 状态码 */
} event_upload_data_t;

/* ---- 上传进度事件 ---- */
typedef struct {
    int progress_percent;  /* 0~100 */
    size_t bytes_sent;     /* 已发送字节数 */
    size_t total_bytes;    /* 文件总字节数 */
} event_upload_progress_data_t;

/* ---- 电池事件 ---- */
typedef struct {
    int percentage;        /* 电量百分比 0~100 */
    float voltage;         /* 电池电压（V）*/
} event_battery_data_t;

/* ---- 状态变化事件（已有） ---- */
typedef struct {
    int prev_state;        /* 上一状态（device_state_t）*/
    int curr_state;        /* 当前状态（device_state_t）*/
} event_state_data_t;

/* ---- 错误事件（通用）---- */
typedef struct {
    esp_err_t error_code;  /* ESP 错误码 */
    char      message[64]; /* 人类可读错误信息（可选）*/
} event_error_data_t;
```

---

## 4. 事件所有权（Ownership）

每个事件有唯一的**发布者（Owner）**，其他模块只允许订阅，不允许发布该事件。

| 事件 | Owner 模块 | 发布时机 |
|------|-----------|---------|
| `EVENT_BUTTON_*` | `button` | GPIO 中断 + 定时器去抖后 |
| `EVENT_RECORDING_*` | `recorder` | I2S 启动/停止/错误时 |
| `EVENT_STORAGE_*` | `storage` | 挂载/卸载/错误时 |
| `EVENT_WIFI_*` | `wifi_manager` | WiFi 事件回调中 |
| `EVENT_UPLOAD_*` | `uploader` | HTTP 上传状态变化时 |
| `EVENT_BATTERY_*` | `battery` | ADC 采样发现阈值跨越时 |
| `EVENT_STATE_CHANGED` | `state` | `state_set()` 内部，状态实际改变时 |

> **违规检测**：若发现非 Owner 模块调用 `event_bus_publish(EVENT_XXX)`，视为架构错误，应在 code review 中拒绝合并。

---

## 5. 发布 / 订阅规则

### 5.1 发布规则

```c
/* ✅ 正确：带 payload */
event_recording_data_t ev = {
    .file_path = "/sdcard/REC_20260512_123456.wav",
    .duration_ms = 5000,
    .file_size = 160000,
};
event_bus_publish(EVENT_RECORDING_STOPPED, &ev, sizeof(ev));

/* ✅ 正确：无 payload（payload=NULL, len=0）*/
event_bus_publish(EVENT_STORAGE_READY, NULL, 0);

/* ❌ 错误：payload 非空但 len=0 */
event_bus_publish(EVENT_RECORDING_STOPPED, &ev, 0);  // 订阅者无法获取 payload

/* ❌ 错误：payload 为局部变量，但未来若改为异步分发会悬空 */
/* 当前同步分发无此问题，但代码评审应标注警告 */
```

### 5.2 订阅规则

```c
/* ✅ 正确：在模块 init() 中订阅 */
void recorder_init(void) {
    event_bus_subscribe(EVENT_BUTTON_CLICKED, on_button_clicked, NULL);
    event_bus_subscribe(EVENT_STORAGE_ERROR, on_storage_error, NULL);
}

/* ✅ 正确：回调中不调用 event_bus_publish() 发布同类型事件（避免递归）*/
static void on_button_clicked(event_type_t type, const void *data, size_t len, void *user) {
    /* 只做轻量操作，不阻塞 */
    if (recorder_is_recording()) {
        recorder_stop(NULL);
    } else {
        recorder_start(NULL);
    }
}

/* ❌ 错误：回调中执行耗时操作（SD 写入、网络请求等）*/
/* 回调在 publish() 调用者上下文中执行，会阻塞事件分发 */
```

### 5.3 订阅生命周期

| 场景 | 规则 |
|------|------|
| 模块初始化 | 调用 `event_bus_subscribe()`，保存返回的 `event_handler_t` |
| 模块去初始化 | 调用 `event_bus_unsubscribe(handler)` |
| 动态订阅（运行时） | 允许，但必须保证 unsubscribe 与 subscribe 成对调用 |

---

## 6. event_bus.h 头文件（规范版）

```c
/**
 * @file event_bus.h
 * @brief 全局事件总线 - 规范头文件
 *
 * 所有模块间通信必须通过本模块进行。
 * 禁止跨模块直接函数调用。
 */

#ifndef EVENT_BUS_H
#define EVENT_BUS_H

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============ 事件类型枚举 ============ */

typedef enum {
    /* 状态机 */
    EVENT_STATE_CHANGED,

    /* 按键 */
    EVENT_BUTTON_PRESSED,
    EVENT_BUTTON_RELEASED,
    EVENT_BUTTON_CLICKED,
    EVENT_BUTTON_DOUBLE_CLICKED,
    EVENT_BUTTON_LONG_PRESSED,
    EVENT_BUTTON_HOLD,

    /* 录音 */
    EVENT_RECORDING_STARTED,
    EVENT_RECORDING_STOPPED,
    EVENT_RECORDING_TIMEOUT,
    EVENT_RECORDING_ERROR,

    /* 存储 */
    EVENT_STORAGE_READY,
    EVENT_STORAGE_ERROR,
    EVENT_STORAGE_REMOVED,

    /* WiFi */
    EVENT_WIFI_CONNECTED,
    EVENT_WIFI_DISCONNECTED,

    /* 上传 */
    EVENT_UPLOAD_STARTED,
    EVENT_UPLOAD_SUCCESS,
    EVENT_UPLOAD_FAILED,
    EVENT_UPLOAD_PROGRESS,

    /* 电池 */
    EVENT_BATTERY_LOW,
    EVENT_BATTERY_CRITICAL,

    /* 扩展预留 */
    EVENT_TYPE_COUNT,
} event_type_t;

/* ============ Payload 结构体 ============ */

typedef struct { int prev_state; int curr_state; } event_state_data_t;
typedef struct { int gpio_num; uint32_t press_ms; } event_button_data_t;

typedef struct {
    char     file_path[128];
    uint32_t duration_ms;
    size_t   file_size;
} event_recording_data_t;

typedef struct { esp_err_t error_code; char message[64]; } event_error_data_t;
typedef struct { esp_err_t error_code; char mount_point[32]; } event_storage_data_t;
typedef struct { char ip_addr[16]; esp_err_t disconnect_reason; } event_wifi_data_t;
typedef struct { char file_path[128]; int status_code; } event_upload_data_t;
typedef struct { int progress_percent; size_t bytes_sent; size_t total_bytes; } event_upload_progress_data_t;
typedef struct { int percentage; float voltage; } event_battery_data_t;

/* ============ API ============ */

typedef void (*event_cb_t)(event_type_t type, const void *data, size_t data_len, void *user_data);
typedef int event_handler_t;

void     event_bus_init(void);
event_handler_t event_bus_subscribe(event_type_t type, event_cb_t cb, void *user_data);
void     event_bus_unsubscribe(event_handler_t handler);
void     event_bus_publish(event_type_t type, const void *data, size_t len);
const char* event_type_name(event_type_t type);

#ifdef __cplusplus
}
#endif

#endif /* EVENT_BUS_H */
```

---

## 7. 当前实现 vs 规范要求

| 项目 | 当前状态 | 规范目标 | 优先级 |
|------|---------|---------|--------|
| 事件类型枚举 | ✅ 已有，基本覆盖 | 补充 `EVENT_RECORDING_TIMEOUT`、`EVENT_STORAGE_REMOVED`、`EVENT_UPLOAD_SUCCESS` | 高 |
| payload 结构体 | ⚠️ 部分缺失（`event_recording_data_t` 等未定义） | 按第 3 节补全 | 高 |
| 事件所有权 | ❌ 未强制执行 | 文档约定 + code review 检查 | 中 |
| 同步/异步分发 | 当前同步 | v0.4 考虑改为异步（队列）以支持耗时回调 | 低 |
| 最大订阅者数 | 32 | 足够，无需修改 | — |

---

## 8. 设计原则总结

1. **事件 = 已发生的事情**，不是命令。不要发布 `EVENT_START_RECORDING`（命令），而是发布 `EVENT_RECORDING_STARTED`（结果）。
2. **发布者唯一**，订阅者多个。降低耦合度。
3. **回调必须轻量**。耗时操作应触发一个 task 去执行，而非在回调中阻塞。
4. **payload 使用结构体**，禁止裸指针/裸整数。提高类型安全性。
5. **事件名使用过去时**，与命令（imperative）明确区分。

---

*文档版本：v0.1 | 作者：AI Assistant | 审核：待定*
