# Storage Layout — ESP32 AI Recorder

> **Version:** v0.4 (VFS-only) | **Updated:** 2026-05-14
> **Scope:** SD card directory structure and filesystem initialization guarantees
> **Key change:** 统一使用 ESP-IDF VFS/POSIX API，移除所有 FatFs-native 调用

---

## 1. 目录结构

```
/sdcard/
├── recordings/      ← WAV 录音文件（auto-created at mount）
├── uploaded/        ← 已上传到服务器的文件（auto-created）
├── upload_queue/    ← 待上传队列（auto-created）
├── temp/            ← 临时文件（auto-created）
├── logs/            ← 日志文件（auto-created）
└── test.txt         ← R/W test file (created at boot)
```

**所有子目录在 `storage_mount()` 成功后由 `storage_ensure_directories()` 自动创建，使用 POSIX `mkdir()` API（VFS 封装）。**

---

## 2. 文件系统初始化保证

### Boot Sequence

```
storage_mount()
  → spi_bus_initialize()
  → esp_vfs_fat_sdspi_mount()              // VFS 封装 FatFs 挂载
  → storage_ensure_directories()            // 创建所有子目录
      → mkdir("/sdcard/recordings", 0755)   (errno==EEXIST if exists)
      → mkdir("/sdcard/uploaded", 0755)
      → mkdir("/sdcard/upload_queue", 0755)
      → mkdir("/sdcard/temp", 0755)
      → mkdir("/sdcard/logs", 0755)
  → storage_validate_layout()               // 验证目录存在，打印 [OK] 日志
  → storage_test_rw()                       // 验证读写
```

### Startup Validation Log

启动后 `storage_validate_layout()` 打印：

```
I (xxx) storage: Storage Layout:
I (xxx) storage:   [OK] recordings/
I (xxx) storage:   [OK] uploaded/
I (xxx) storage:   [OK] upload_queue/
I (xxx) storage:   [OK] temp/
I (xxx) storage:   [OK] logs/
```

### Guarantees

| 时机 | 保证 |
|------|------|
| `storage_mount()` 返回 ESP_OK | FATFS 已通过 VFS 挂载，`storage_ensure_directories()` 已创建所有子目录 |
| `storage_validate_layout()` 打印 `[OK]` | 目录已验证存在（使用 `opendir()`） |
| `recorder_init()` | `/sdcard/recordings/` 已存在 |
| `recorder_start()` | 目录保证存在；`fopen()` 失败时 clean return |

---

## 3. 文件系统策略（强制）

**见 `docs/storage-path-policy.md` 获取完整说明。**

统一使用 ESP-IDF VFS/POSIX API：

| 操作 | 正确 API | 说明 |
|------|---------|------|
| 创建目录 | `mkdir("/sdcard/recordings", 0755)` | VFS 封装，errno==EEXIST 视为成功 |
| 验证目录 | `opendir("/sdcard/recordings")` | POSIX，返回 DIR* |
| 遍历文件 | `readdir()` / `closedir()` | POSIX |
| 打开文件 | `fopen("/sdcard/...", "wb")` | VFS 封装 |
| 删除文件 | `unlink("/sdcard/...")` | POSIX，通过 VFS |
| 查询空间 | `f_getfree("/sdcard", ...)` | VFS 封装（内部仍用 FatFs） |

**禁止使用底层 FatFs-native API（`f_mkdir`、`f_opendir`、`f_stat`、`f_unlink` 等）。**

---

## 4. File Naming

### Recordings

格式：`REC_SESSION_XXXX.wav`（4 位零填充序号）

```c
// 示例
/sdcard/recordings/REC_SESSION_0001.wav
/sdcard/recordings/REC_SESSION_0002.wav
```

序号在 `recorder_init()` 时扫描已有文件确定（`session_init()`），使用 POSIX `opendir()`/`readdir()` 遍历。

### Uploaded

已成功上传到服务器的文件可移至 `/sdcard/uploaded/`。

### Upload Queue

待上传的任务描述文件（JSON）存放在此目录。

---

## 5. Storage Capacity

| 参数 | 值 |
|------|---|
| SD 卡支持 | FAT32（推荐），exFAT |
| 推荐容量 | 8GB - 128GB |
| 每分钟 WAV 录音 | ~9.6 MB（16kHz, 16-bit, mono） |
| 32GB SD 卡 | ~55 小时连续录音 |

---

## 6. Error Handling

| 错误 | 处理 |
|------|------|
| SD 卡未插入 | `esp_vfs_fat_sdspi_mount()` 返回 ESP_FAIL，设备进入 ERROR 状态 |
| FAT32 格式化失败 | 提示用户手动在电脑上格式化 |
| `fopen()` 失败 | 日志 errno，clean return，不崩溃 |
| `mkdir()` 失败（除 EEXIST） | `storage_ensure_directories()` 返回 ESP_FAIL |
| `storage_validate_layout()` 发现缺失目录 | 打印 `[FAIL]`，返回错误 |

---

## 7. Directory Ownership

**Storage subsystem 拥有目录生命周期：**

| 责任 | 组件 |
|------|------|
| mount / unmount | `storage.c` |
| 目录创建 (`mkdir`) | `storage.c` — `storage_ensure_directories()` |
| 目录验证 (`opendir`) | `storage.c` — `storage_validate_layout()` |
| 文件读写 (`fopen`) | 任意组件（recorder, logger 等） |
| 文件删除 (`unlink`) | `storage.c` — `storage_delete_file()` |

**禁止**：`recorder.c` 等其他组件直接调用 `mkdir`、`opendir`。

---

*Last updated: 2026-05-14（v0.4 — VFS-only 策略）*
