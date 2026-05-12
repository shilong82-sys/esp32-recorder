# Recorder API 设计 — ESP32 AI Recorder

> Version: v0.1 | Updated: 2026-05-12
> 本文档定义录音模块的 API 接口与职责，不涉及具体实现。

---

## 1. 设计目标

1. **简洁**：对外暴露 5 个核心 API，覆盖 95% 使用场景。
2. **状态安全**：内部维护录音状态机，防止非法调用（如重复 `recorder_start()`）。
3. **线程安全**：所有 API 内部使用 mutex 保护共享状态。
4. **事件驱动**：状态变化通过 `event_bus` 广播，不直接回调。

---

## 2. API 清单

| API | 功能 | 线程安全 | 事件交互 |
|-----|------|---------|---------|
| `recorder_init()` | 初始化模块（配置 I2S、分配资源）| ✅ 是 | — |
| `recorder_start()` | 开始录音 | ✅ 是 | 成功后发布 `EVENT_RECORDING_STARTED` |
| `recorder_stop()` | 停止录音 | ✅ 是 | 完成后发布 `EVENT_RECORDING_STOPPED` |
| `recorder_pause()` | 暂停录音（预留）| ✅ 是 | v0.3+ 实现 |
| `recorder_resume()` | 恢复录音（预留）| ✅ 是 | v0.3+ 实现 |
| `recorder_is_recording()` | 查询录音状态 | ✅ 是（只读，无锁）| — |
| `recorder_get_duration()` | 获取当前录音时长（ms）| ✅ 是 | — |
| `recorder_list_files()` | 列出 `recordings/` 下所有 WAV 文件 | ✅ 是 | — |

---

## 3. API 详细说明

### 3.1 `recorder_init()`

```c
/**
 * @brief 初始化录音模块
 * @param config 录音参数（采样率、位深、声道数等）
 * @return ESP_OK=成功，其他=失败
 *
 * 职责：
 * - 保存 config 到全局变量
 * - 初始化 I2S 外设（但不启动 DMA）
 * - 创建 RingBuffer
 * - 注册 I2S 事件回调
 *
 * 线程安全：✅ 只能调用一次（s_initialized 标志）
 * 事件交互：无
 * 调用时机：app_main() 初始化序列中
 */
esp_err_t recorder_init(const recorder_config_t *config);
```

**状态所有权**：`recorder_init()` 不改变设备状态（`state_get()` 仍为 `DEVICE_STATE_INIT` 或 `DEVICE_STATE_IDLE`）。

---

### 3.2 `recorder_start()`

```c
/**
 * @brief 开始录音
 * @param filename 输出 WAV 文件路径（NULL = 自动生成）
 * @return ESP_OK=成功，ESP_ERR_INVALID_STATE=已在录音，其他=失败
 *
 * 职责：
 * - 检查当前状态（s_recording == false）
 * - 在 recordings/ 下创建 WAV 文件（写 44 bytes 占位头）
 * - 启动 I2S DMA（开始采集）
 * - 创建 Recorder Task（若尚未创建）
 * - 发布 EVENT_RECORDING_STARTED
 *
 * 线程安全：✅ 使用 mutex 保护 s_recording 标志
 * 事件交互：成功后发布 EVENT_RECORDING_STARTED
 * 调用时机：state_set(DEVICE_STATE_RECORDING) 的订阅回调中
 */
esp_err_t recorder_start(const char *filename);
```

**状态检查顺序**：

```
1. s_initialized?  → 否：返回 ESP_ERR_INVALID_STATE
2. s_recording?    → 是：返回 ESP_ERR_INVALID_STATE（不允许重复开始）
3. storage 已挂载？ → 否：返回 ESP_ERR_NOT_FOUND
4. 磁盘空间 > 1MB？→ 否：返回 ESP_ERR_NO_MEM
5. 创建文件成功？
   → 是：启动 I2S DMA，发布 EVENT_RECORDING_STARTED，返回 ESP_OK
   → 否：返回 ESP_FAIL
```

---

### 3.3 `recorder_stop()`

```c
/**
 * @brief 停止录音
 * @param[out] out_duration_ms 录音时长（毫秒），可选（NULL=忽略）
 * @return ESP_OK=成功，ESP_ERR_INVALID_STATE=未在录音
 *
 * 职责：
 * - 设置停止标志（s_stop_requested = true）
 * - 等待 Recorder Task 完成 flush（RingBuffer 中剩余数据写入文件）
 * - 回写 WAV 文件头（正确的 data size）
 * - 关闭文件
 * - 发布 EVENT_RECORDING_STOPPED
 * - 可选：在 upload_queue/ 创建 JSON 任务文件
 *
 * 线程安全：✅ 使用 mutex 保护
 * 事件交互：完成后发布 EVENT_RECORDING_STOPPED
 * 调用时机：state_set(DEVICE_STATE_RECORD_STOPPING) 的订阅回调中
 */
esp_err_t recorder_stop(uint32_t *out_duration_ms);
```

> **注意**：`recorder_stop()` 内部会阻塞等待 Recorder Task 完成（最多等待 2 秒）。若超时，强制关闭文件并发布 `EVENT_RECORDING_ERROR`。

---

### 3.4 `recorder_pause()` / `recorder_resume()`（v0.3+ 预留）

```c
/**
 * @brief 暂停录音（v0.3+）
 * @return ESP_OK=成功
 *
 * 职责：
 * - 停止 I2S DMA（不再采集新数据）
 * - 但保留已采集数据在 RingBuffer 中
 * - 不关闭 WAV 文件
 *
 * 事件交互：发布 EVENT_RECORDING_PAUSED（待定义）
 */
esp_err_t recorder_pause(void);

/**
 * @brief 恢复录音（v0.3+）
 * @return ESP_OK=成功
 *
 * 职责：
 * - 重新启动 I2S DMA
 * - 继续从 RingBuffer 写入 WAV 文件
 */
esp_err_t recorder_resume(void);
```

> **设计说明**：v0.2 不需要暂停/恢复功能。若未来需要（如语音激活录音，VAD），再实现此 API。

---

### 3.5 `recorder_is_recording()`

```c
/**
 * @brief 查询是否在录音中
 * @return true=录音中，false=空闲
 *
 * 职责：
 * - 返回 s_recording 标志（原子读，无需 mutex）
 *
 * 线程安全：✅（s_recording 为 bool，单字节读写在 ESP32 上是原子的）
 * 事件交互：无
 */
bool recorder_is_recording(void);
```

---

### 3.6 `recorder_get_duration()`

```c
/**
 * @brief 获取当前录音时长（毫秒）
 * @return 录音时长（ms），未录音时返回 0
 *
 * 实现：
 * - 录音中：(esp_timer_get_time() - s_start_time) / 1000
 * - 录音停止后：返回最后一次录音的时长（保存在 s_last_duration_ms）
 */
uint32_t recorder_get_duration(void);
```

---

### 3.7 `recorder_list_files()`

```c
/**
 * @brief 列出 recordings/ 下所有 WAV 文件
 * @param[out] file_list 文件名字数组（调用者分配）
 * @param max_files 最大文件数
 * @return 实际文件数
 *
 * 线程安全：✅（只读操作，FATFS 线程安全需外部保证）
 * 事件交互：无
 */
int recorder_list_files(char file_list[][64], int max_files);
```

---

## 4. 内部状态机（recorder 私有）

```
[s_initialized=false] ──recorder_init()──▶ [s_initialized=true, s_recording=false]
                                                           │
                                                recorder_start() 成功
                                                           ▼
                                              [s_recording=true, Task 运行中]
                                                           │
                                                    recorder_stop()
                                                           ▼
                                              [s_recording=false, Task 退出]
```

> **注意**：此状态机是 `recorder.c` 内部状态，与 `device_state_t`（设备级状态机）是**不同层次**的概念。关系如下：

```
设备状态：IDLE ──▶ RECORDING ──▶ RECORD_STOPPING ──▶ IDLE
                │
                └── recorder_start() 在此状态被调用
                     │
                     ▼
                内部状态：s_recording = true
```

---

## 5. 与 event_bus 的交互

| recorder API | 发布的事件 | 时机 |
|-------------|-----------|------|
| `recorder_start()` 成功 | `EVENT_RECORDING_STARTED` | I2S DMA 启动完成，第一个 DMA buffer 已写入 RingBuffer |
| `recorder_stop()` 完成 | `EVENT_RECORDING_STOPPED` | WAV 文件已关闭，data size 已回写 |
| `recorder_stop()` 超时 | `EVENT_RECORDING_ERROR` | Recorder Task 2 秒内未完成 flush |
| RingBuffer 溢出 | `EVENT_RECORDING_ERROR` | drop 计数 > 0（warning 级别）|

---

## 6. 与 device_state_t 的关系

| device_state | recorder 内部状态 | 允许调用的 API |
|-------------|-----------------|---------------|
| `INIT` | `s_initialized=false` | `recorder_init()` |
| `IDLE` | `s_recording=false` | `recorder_start()`、`recorder_list_files()` |
| `RECORDING` | `s_recording=true` | `recorder_stop()`、`recorder_get_duration()` |
| `RECORD_STOPPING` | `s_recording→false`（过渡）| 无（等待完成）|
| 其他状态 | `s_recording=false` | `recorder_list_files()`（只读）|

> **关键原则**：`state_set()` 的订阅回调中调用 `recorder_start()/stop()`，而不是直接在 button 回调中调用。

---

## 7. 错误处理

| 错误场景 | 错误码 | 处理方式 |
|---------|-------|---------|
| 未初始化就调用 `start()` | `ESP_ERR_INVALID_STATE` | 返回错误，不发布事件 |
| 重复调用 `start()` | `ESP_ERR_INVALID_STATE` | 返回错误，不重复启动 |
| SD 卡满（f_write 失败）| `ESP_ERR_NO_MEM` | 自动停止录音，发布 `EVENT_RECORDING_ERROR` |
| I2S DMA 启动失败 | `ESP_ERR_NOT_FOUND` | 返回错误，录音不启动 |

---

## 8. 设计原则总结

1. **API 最简**：只暴露必要的 5~7 个函数，内部复杂性不暴露。
2. **状态由内部维护**：调用者无需关心 `s_recording` 标志，只需调用 API 并检查返回值。
3. **事件由 recorder 发布**：状态变化通过 `event_bus` 广播，其他模块被动响应。
4. **线程安全优先**：所有修改共享状态（如 `s_recording`）的 API 必须使用 mutex。
5. **与 device_state_t 解耦**：recorder 内部状态 ≠ 设备状态，二者通过 `event_bus` 协作。

---

*文档版本：v0.1 | 作者：AI Assistant | 审核：待定*
