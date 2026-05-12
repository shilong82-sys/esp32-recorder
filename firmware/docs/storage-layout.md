# SD 卡目录结构规范 — ESP32 AI Recorder

> Version: v0.2 | Updated: 2026-05-12
> 本文档定义 TF 卡（/sdcard/）的目录布局、文件命名规则、生命周期与崩溃恢复策略。

---

## 1. 目录总览

```
/sdcard/
├── recordings/       ← WAV 录音文件（主产物）
├── upload_queue/     ← 待上传任务描述文件（JSON）
├── uploaded/         ← 已成功上传的文件（归档）
├── temp/             ← 临时文件（崩溃恢复用）
└── logs/             ← 设备运行日志（可选，v0.4+）
```

---

## 2. 各目录详细说明

### 2.1 `/sdcard/recordings/`

| 属性 | 说明 |
|------|------|
| **用途** | 存放所有 WAV 录音文件（录音的直接产物）|
| **生命周期** | 创建 → 录音进行中 → 录音停止（文件关闭）→ 等待上传 → 上传成功 → 可选归档或删除 |
| **文件命名规则** | `REC_SESSION_0001.wav`（单调递增序号）|
| **示例** | `REC_SESSION_0001.wav`、`REC_SESSION_0002.wav` |
| **唯一性保证** | 启动时扫描 `recordings/` 目录，找到最大序号 N，下次录音使用 N+1 |
| **时间语义** | **当前阶段（v0.2-v0.5）：仅保证唯一性，不保证真实时间。** 未来联网后升级（见下方 Roadmap）|
| **文件大小** | 16kHz / 16-bit / 单声道：`16000 × 2 = 32KB/s`，每分钟约 1.92MB |

#### 文件命名详细规则

```
格式：REC_SESSION_XXXX.wav

字段说明：
- REC：固定前缀（recording）
- SESSION：会话标记（区分于未来 UTC 命名）
- XXXX：4 位序号，从 0001 开始，不足 4 位前导零
- .wav：固定扩展名（小写）

序号分配逻辑：
1. 启动时扫描 /sdcard/recordings/
2. 解析所有 REC_SESSION_*.wav 文件名
3. 提取序号，找到最大 max_seq
4. 下次录音使用 (max_seq + 1)
5. 若扫描失败（首次使用/目录为空），从 0001 开始

⚠️ 当前阶段的命名目标：保证唯一性，而非真实时间。
```

#### 文件命名 Roadmap（未来升级项）

| 阶段 | 命名格式 | 前提条件 | 降级策略 |
|------|---------|---------|---------|
| v0.2-v0.5（当前） | `REC_SESSION_0001.wav` | 无 | — |
| v0.6+（联网后） | `REC_YYYYMMDD_HHMMSS.wav` | WiFi + SNTP 同步成功 | SNTP 超时 fallback 到 monotonic naming |

> **为什么不用时间戳？**
> - `time(NULL)` 上电后返回 1970 年（无电池 RTC）
> - `esp_timer_get_time()` 是 monotonic uptime，跨重启不连续
> - monotonic uptime 无法排序不同会话的文件
> - 上传后服务端无法按真实时间排列录音

---

### 2.2 `/sdcard/upload_queue/`

| 属性 | 说明 |
|------|------|
| **用途** | 存放待上传任务描述文件（JSON），每个录音文件对应一个 `.json` |
| **生命周期** | 录音停止 → 创建 JSON → 上传成功 → 移动到 `uploaded/` 或删除 |
| **文件命名规则** | 与对应 WAV 文件同名，扩展名改为 `.json` |
| **示例** | `REC_20260512_143052.json` |

#### JSON 任务描述格式

```json
{
  "version": "1.0",
  "wav_file": "/sdcard/recordings/REC_20260512_143052.wav",
  "file_size": 1920000,
  "created_at": "2026-05-12T14:30:52Z",
  "duration_ms": 60000,
  "sample_rate": 16000,
  "bits_per_sample": 16,
  "channels": 1,
  "upload_url": "http://192.168.31.185:8000/upload",
  "retry_count": 0,
  "max_retries": 3
}
```

> **设计说明**：使用 JSON 任务文件（而非数据库）是为了崩溃恢复：若上传中断，重启后扫描 `upload_queue/` 即可恢复待上传任务。

---

### 2.3 `/sdcard/uploaded/`

| 属性 | 说明 |
|------|------|
| **用途** | 存放已成功上传的 WAV 文件（归档），或仅存放 JSON 任务文件作为上传凭证 |
| **生命周期** | 上传成功 → 移入此目录 → 用户手动删除 或 自动清理（超过 N 天）|
| **清理策略** | 见第 4 节 |
| **文件命名规则** | 与 `recordings/` 一致 |

> **可选策略**：v0.3 可以改为"上传成功后直接删除 WAV 文件"，不归档，节省 SD 卡空间。

---

### 2.4 `/sdcard/temp/`

| 属性 | 说明 |
|------|------|
| **用途** | 存放录音过程中的临时数据（如 WAV 头占位文件、未完成的 JSON）|
| **生命周期** | 录音开始 → 创建临时文件 → 录音正常停止 → 移动到 `recordings/`；若崩溃则保留，重启后恢复 |
| **崩溃恢复策略** | 见第 5 节 |

---

### 2.5 `/sdcard/logs/`（v0.4+）

| 属性 | 说明 |
|------|------|
| **用途** | 存放设备运行日志（由 `logger` 组件写入，可选）|
| **文件命名规则** | `LOG_YYYYMMDD.csv` 或 `LOG_YYYYMMDD.txt` |
| **轮换策略** | 每天一个新文件；超过 7 天的自动删除 |

---

## 3. 目录初始化

`storage_mount()` 成功后将自动创建所需目录：

```c
/* storage.c 中在 mount 成功后调用 */
void storage_ensure_dirs(void) {
    mkdir("/sdcard/recordings", 0755);
    mkdir("/sdcard/upload_queue", 0755);
    mkdir("/sdcard/uploaded", 0755);
    mkdir("/sdcard/temp", 0755);
    mkdir("/sdcard/logs", 0755);
}
```

> **注意**：`mkdir()` 在目录已存在时返回错误，可安全调用（需检查 `errno == EEXIST`）。

---

## 4. 清理策略

| 目录 | 清理触发条件 | 清理规则 |
|------|------------|---------|
| `recordings/` | 上传成功 | 可选：移动到 `uploaded/` 或直接删除 |
| `upload_queue/` | 上传成功 | 删除对应 `.json` 文件 |
| `uploaded/` | 文件超过 7 天 或 总大小超过 100MB | 删除最旧的文件 |
| `temp/` | 每次启动时 | 删除所有 `.wav.tmp` 临时文件 |
| `logs/` | 每次启动时 | 删除 7 天前的日志 |

#### 自动清理实现建议

```c
/* 在 recordings/ 目录，上传成功后 */
esp_err_t storage_archive_file(const char *wav_path) {
    char dest[128];
    snprintf(dest, sizeof(dest), "/sdcard/uploaded/%s", basename(wav_path));
    return rename(wav_path, dest);  /* FATFS rename = 移动文件 */
}
```

---

## 5. 崩溃恢复策略

### 5.1 场景：录音中意外掉电/崩溃

**现象**：
- `recordings/` 中的 WAV 文件头不完整（data size 为 0xFFFFFFFF）
- `temp/` 中可能有 `.wav.tmp` 占位文件
- 上次 checkpoint sync 之后的数据可能丢失（最多 30 秒）

**恢复步骤**（在 `recorder_init()` 或 `storage_mount()` 后执行）：

```
1. 扫描 /sdcard/recordings/*.wav
2. 对每个 .wav 文件检查 WAV 头：
   - 若 data chunk size == 0xFFFFFFFF（未正常关闭）
   - 使用 fseek() 到文件末尾，计算实际 data size
   - 回写正确的 WAV 头
3. 删除 /sdcard/temp/*.tmp（残留临时文件）
4. 扫描 /sdcard/upload_queue/*.json，重新加入上传队列
```

### 5.2 崩溃恢复能力分析

| 场景 | 数据丢失量 | 恢复方式 |
|------|-----------|---------|
| 掉电前刚完成 checkpoint sync | 0 | WAV 头完整，无需修复 |
| 掉电前 30s 内（checkpoint 未触发）| 最多 30s | WAV 头需修复（data size 回写） |
| 掉电前最后 4KB 写入中 | 最多 4KB | WAV 头需修复（data size 回写） |
| 掉电前 FAT 表未更新 | 目录可能无条目 | 重启后扫描修复 |

> **设计说明**：v0.2 采用"每 30 秒 checkpoint sync"策略，崩溃时最多丢失 30 秒数据。
> 这是 embedded audio recording 的合理权衡：牺牲少量数据换取写入稳定性。

### 5.3 恢复代码示例（伪代码）

```c
void storage_crash_recovery(void) {
    /* 1. 修复未关闭的 WAV 文件 */
    DIR *dir = opendir("/sdcard/recordings");
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strstr(ent->d_name, ".wav")) {
            char path[128];
            snprintf(path, sizeof(path), "/sdcard/recordings/%s", ent->d_name);
            wav_repair_header(path);  /* 回写正确的 data size */
        }
    }
    closedir(dir);

    /* 2. 清理 temp/ */
    storage_clean_dir("/sdcard/temp");

    /* 3. 恢复上传队列 */
    /* 扫描 upload_queue/，重新发布 EVENT_UPLOAD_STARTED */
}
```

---

## 6. 与现有代码的对接

| 现有接口 | 对接方式 |
|---------|---------|
| `storage_mount()` | 挂载成功后调用 `storage_ensure_dirs()` |
| `recorder_start()` | 在 `recordings/` 下创建 WAV 文件 |
| `recorder_stop()` | 关闭 WAV 文件；创建 `upload_queue/` 下 JSON 任务 |
| `uploader_upload()` | 读取 `upload_queue/` 下 JSON，上传对应 WAV 文件 |
| `EVENT_UPLOAD_DONE` | 触发 `storage_archive_file()` 或 `storage_delete_file()` |

---

## 7. 设计原则

1. **目录即状态**：文件在哪个目录，就代表它处于生命周期的哪个阶段。
2. **JSON 任务文件**：上传队列用文件而非内存队列，崩溃后可恢复。
3. **WAV 头延迟写入**：录音开始时写占位头，停止时回写正确大小（标准做法）。
4. **清理策略保守**：v0.2 建议"上传成功后归档到 `uploaded/`"，v0.4 再考虑自动删除。
5. **崩溃恢复优先**：设备可能随时掉电，恢复逻辑必须在 `main()` 早期执行。

---

*文档版本：v0.1 | 作者：AI Assistant | 审核：待定*
