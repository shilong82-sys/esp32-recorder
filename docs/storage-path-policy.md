# Storage Path Policy — ESP32 AI Recorder

> **Version:** v0.4 (VFS-only) | **Updated:** 2026-05-14
> **Scope:** Mandatory filesystem API conventions for ESP-IDF v5.2
> **Principle:** ESP-IDF VFS = 唯一的 filesystem API。禁止直接使用底层 FatFs-native API。

---

## 1. 策略声明（强制）

**本项目统一使用 ESP-IDF VFS/POSIX API。**

| 操作 | 正确 API | 禁止使用 |
|------|---------|---------|
| 创建目录 | `mkdir("/sdcard/recordings", 0755)` | `f_mkdir("0:/recordings")` |
| 验证目录存在 | `opendir("/sdcard/recordings")` | `f_opendir(&dir, "0:/recordings")` |
| 遍历目录 | `readdir()` / `closedir()` | `f_readdir()` / `f_closedir()` |
| 检查文件存在 | `stat("/sdcard/...", &st)` | `f_stat("0:/...", &fno)` |
| 删除文件 | `unlink("/sdcard/...")` | `f_unlink("0:/...")` |
| 打开文件读写 | `fopen("/sdcard/...", "wb")` | `f_open(&fil, "0:/...", FA_WRITE)` |
| 查询剩余空间 | `f_getfree("/sdcard", ...)` | `f_getfree("0:", ...)` |

**底层原因：**

ESP-IDF v5.2 中，`esp_vfs_fat_sdspi_mount()` 将 FatFs 封装进 VFS 层：

```
业务代码 → VFS API (/sdcard/...) → esp_vfs_fat 层 → 底层 FatFs drive (0:/, 1:/)
```

- **drive mapping 不稳定**：SPI Flash FAT 注册顺序决定 drive number，不一定总是 `"0"`
- **FR_INVALID_NAME**：`f_mkdir("0:/recordings")` 因 drive 未被业务层正确探测而失败
- **API 行为不一致**：`fopen("/sdcard/...")` 与 `f_opendir("0:/...")` 状态不同步
- **portability 降低**：业务层依赖底层 drive prefix，换平台/配置会断裂

**结论：业务层永远不应依赖 `"0:/..."`。所有路径操作统一通过 VFS。**

---

## 2. 路径格式规范

所有业务层路径使用 **VFS/POSIX 格式**：

| 路径类型 | 格式 | 示例 |
|---------|------|------|
| 挂载点 | `/sdcard` | 由 `storage_get_vfs_mount()` 统一获取 |
| 目录 | `/sdcard/<dirname>` | `/sdcard/recordings` |
| 文件 | `/sdcard/<dirname>/<filename>` | `/sdcard/recordings/REC_SESSION_0001.wav` |

**获取路径的正确方式（唯一合法）**：

```c
// 通过 enum 获取路径，永远不硬编码
char path[128];
esp_err_t err = storage_build_vfs_path(
    path, sizeof(path),
    STORAGE_PATH_RECORDINGS,    // 枚举，定义在 storage.h
    "REC_SESSION_0001.wav"     // 文件名（可为 NULL 表示仅目录）
);
// err == ESP_OK → path = "/sdcard/recordings/REC_SESSION_0001.wav"
```

---

## 3. 目录管理职责

**storage subsystem 拥有目录生命周期。**

| 职责 | API | 所有者 |
|------|-----|-------|
| SD 卡挂载 | `esp_vfs_fat_sdspi_mount()` | `storage.c` |
| 目录创建 | `mkdir("/sdcard/recordings", 0755)` | `storage.c` — `storage_ensure_directories()` |
| 目录验证 | `opendir("/sdcard/...")` | `storage.c` — `storage_validate_layout()` |
| 录音文件读写 | `fopen()/fwrite()/fclose()` | `recorder.c` |
| 上传文件读取 | `fopen("/sdcard/...", "rb")` | `uploader.c` |
| 剩余空间查询 | `f_getfree("/sdcard", ...)` | `storage.c` — `storage_get_space()` |
| 文件删除 | `unlink("/sdcard/...")` | `storage.c` — `storage_delete_file()` |

**其他模块禁止：**
- `mkdir()` — 除非通过 `storage_ensure_directories()`
- `opendir()` / `readdir()` — 除非在 `storage.c` 内
- `unlink()` / `stat()` — 除非通过 `storage_*` 辅助函数

---

## 4. storage API 概览

```c
// 路径构造（业务层唯一合法获取路径的方式）
esp_err_t storage_build_vfs_path(char *out, size_t out_size,
                                 storage_path_type_t type,
                                 const char *filename);
// → "/sdcard/recordings/test.wav" 或 "/sdcard/recordings"（filename==NULL）

// 目录生命周期
esp_err_t storage_ensure_directories(void);   // 创建所有子目录
void      storage_validate_layout(void);        // 验证（[OK]/[MISSING] 日志）

// 文件操作
int  storage_list_wav_files(const char *dir, char file_list[][64], int max);
bool storage_file_exists(const char *path);
uint32_t storage_get_file_size(const char *path);
esp_err_t storage_delete_file(const char *path);
esp_err_t storage_test_rw(void);
esp_err_t storage_get_space(uint32_t *total_kb, uint32_t *free_kb);

// 辅助
const char* storage_get_vfs_mount(void);   // 返回 "/sdcard"
const char* storage_path_type_to_string(storage_path_type_t type);  // "recordings"
```

**storage_path_type_t 枚举**（定义在 `storage.h`）：

```c
typedef enum {
    STORAGE_PATH_RECORDINGS,   // /sdcard/recordings   — 录音文件
    STORAGE_PATH_UPLOADED,     // /sdcard/uploaded     — 已上传文件
    STORAGE_PATH_UPLOAD_QUEUE, // /sdcard/upload_queue — 待上传队列
    STORAGE_PATH_TEMP,         // /sdcard/temp         — 临时文件
    STORAGE_PATH_LOGS,         // /sdcard/logs         — 日志文件
    STORAGE_PATH_COUNT,        // 枚举计数，不是有效路径类型
} storage_path_type_t;
```

---

## 5. 启动验证日志

`storage_validate_layout()` 输出格式：

```
Storage Layout:
  [OK] recordings/
  [OK] uploaded/
  [OK] upload_queue/
  [OK] temp/
  [OK] logs/
```

**不再出现：**
- `FR_INVALID_NAME`
- `FRESULT`
- `0:/` drive prefix
- `f_mkdir`、`f_opendir`、`f_stat`、`f_unlink`

---

## 6. 录音文件路径保证链

```
storage_mount()
    → mkdir("/sdcard/recordings", 0755)  ← 使用 VFS/POSIX
    → storage_validate_layout()             ← 使用 opendir() 验证
    → [OK] recordings/ 打印
            ↓
recorder_init() → session_init()
    → opendir("/sdcard/recordings")  ✓ 确认可访问
            ↓
on_button_click() → recorder_start(NULL)
    → storage_build_vfs_path(..., STORAGE_PATH_RECORDINGS, "REC_SESSION_0001.wav")
    → fopen("/sdcard/recordings/REC_SESSION_0001.wav", "wb")  ✓ 必然成功
```

---

*Last updated: 2026-05-14（v0.4 — VFS-only 策略，移除所有 FatFs-native API）*
