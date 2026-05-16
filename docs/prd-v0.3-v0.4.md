# PRD — ESP32 AI Recorder v0.3 ~ v0.4

> Language: 中文 | Project: `esp32-recorder`
> Version: v0.3-v0.4 PRD | Date: 2026-05-15
> Author: Xu (Product Manager)

---

## 1. 项目信息

| 字段 | 值 |
|------|-----|
| Project Name | `esp32-recorder` |
| 仓库 | `/Users/long/Projects/esp32-recorder/` |
| GitHub | https://github.com/shilong82-sys/esp32-recorder |
| 当前版本 | v0.2.1 |
| 目标设备 | ESP32-S3 N16R8 |
| 框架 | ESP-IDF v5.2.3 |
| 服务端 | Python (Flask → 重构为 FastAPI) |
| 原始需求 | 基于 ESP32-S3 的语音录音设备，通过 WiFi 上传录音到 Mac 服务器，利用 mlx-whisper 进行本地 AI 转写 |

### 原始需求复述

完成 Phase 2 WiFi 上传闭环验证，重构服务端为完整的录音管理+AI转写平台，增强固件上传健壮性，最终实现从录音到转写的端到端自动化 pipeline。

---

## 2. 产品定义

### Product Goals

1. **上传闭环稳定**：固件到服务端的上传链路在局域网和 Tailscale Funnel 两种场景下均可靠工作，支持灵活 URL 配置
2. **服务端可运营**：服务端从"接收脚本"升级为完整的录音管理+AI转写平台，支持文件管理、自动转写、结果查看/导出
3. **端到端自动化**：从 ESP32 按键录音 → WiFi 上传 → 服务端转写 → 结果可查，全流程零人工干预

### User Stories

1. **As a** 设备使用者, **I want** 录音结束后自动上传并转写, **so that** 我无需手动操作即可获得文字记录
2. **As a** 设备使用者, **I want** 在不同网络环境（局域网/Tailscale）下无需重新烧录固件即可切换上传地址, **so that** 我在家和外出都能正常使用
3. **As a** 用户, **I want** 通过 Web 界面查看所有录音文件和转写结果, **so that** 我可以方便地检索和导出历史记录
4. **As a** 开发者, **I want** 服务端提供规范的 REST API, **so that** 我可以集成到其他工作流或前端应用
5. **As a** 设备使用者, **I want** 上传失败后设备自动重试并在网络恢复后继续上传, **so that** 不会因为临时网络问题丢失录音

---

## 3. 当前状态分析

### 已完成

| Phase | 状态 | 内容 |
|-------|------|------|
| Phase 1 | ✅ 完成 | 录音闭环（I2S→ringbuf→SD WAV 写入，按键控制，LED 指示） |
| Phase 2 代码 | ✅ 完成 | WiFi 流式上传（8KB chunk，RAW BODY），WiFi 暂停/恢复，重试 3s→10s→30s |
| Phase 2 验证 | ⏳ 待做 | 局域网大文件上传验证、Tailscale Funnel 上传验证 |

### 关键问题

| 问题 | 影响 | 优先级 |
|------|------|--------|
| `UPLOAD_URL` 硬编码为 `http://record.east-deep.com/upload` | 无法灵活切换局域网/Tailscale/自定义地址 | P0 |
| `uploader_config_t` 传入但被忽略 | 架构不一致，配置机制失效 | P0 |
| 服务端为极简脚本（仅 /upload + /health） | 无文件管理、无转写 pipeline、无 Web UI | P1 |
| EVENT_UPLOAD_PROGRESS 已定义但未发布 | UI 无法显示上传进度 | P1 |
| BUG-004 初始化序号不一致 | 日志混乱（已标记 Fixed 但代码仍需验证） | P2 |
| BUG-005 auto_clear=true on RX-only | 无功能影响但代码不规范 | P2 |
| rgb_led legacy 组件 | 代码冗余 | P2 |
| uploader 兼容接口（uploader_upload 等废弃 API） | 代码噪声 | P2 |

---

## 4. 技术规范

### Requirements Pool

#### P0 — Must Have（v0.3 Phase 2 收尾）

| ID | 需求 | 验收标准 |
|----|------|----------|
| P0-01 | Upload URL 配置化 | 支持 NVS 存储 upload URL；uploader_init() 使用配置而非硬编码；支持 3 种模式：局域网 IP、Tailscale Funnel URL、自定义 URL |
| P0-02 | uploader_config_t 实际生效 | uploader_init() 读取 config 并构造完整 URL；移除硬编码 `UPLOAD_URL` 宏 |
| P0-03 | 局域网上传验证 | 10MB+ WAV 文件通过局域网 IP 上传成功，HTTP 200 返回 |
| P0-04 | Tailscale Funnel 上传验证 | 同一文件通过 Tailscale Funnel URL 上传成功 |

#### P1 — Should Have（v0.3 服务端重构 + v0.3 固件增强）

| ID | 需求 | 验收标准 |
|----|------|----------|
| P1-01 | 服务端重构为 FastAPI | 从 Flask/http.server 迁移到 FastAPI + uvicorn；保留 RAW BODY /upload 接口兼容 |
| P1-02 | 自动转写 pipeline | 上传完成后自动触发 mlx-whisper 转写；异步处理（后台任务）；转写状态可查询 |
| P1-03 | 文件管理 API | GET /api/files（列表+分页+排序）；DELETE /api/files/{id}（删除）；支持按日期/大小过滤 |
| P1-04 | 上传记录持久化 | SQLite 数据库存储文件元数据（文件名、大小、上传时间、转写状态、转写结果）；启动时自动索引 received/ 目录 |
| P1-05 | 转写结果管理 API | GET /api/transcripts（列表）；GET /api/transcripts/{id}（详情含文本）；GET /api/transcripts/{id}/export（下载 .txt） |
| P1-06 | API 规范化 | 统一 JSON 响应格式 `{code, message, data}`；HTTP 状态码语义正确；错误码体系；分页参数统一 |
| P1-07 | Web UI 增强 | 录音文件列表页（含上传时间、大小、转写状态）；转写结果展示页；基础 CSS 美化（无需框架，原生 HTML+CSS） |
| P1-08 | EVENT_UPLOAD_PROGRESS 发布 | uploader 每发送 512KB 发布一次 EVENT_UPLOAD_PROGRESS；附带 event_upload_progress_data_t 数据 |
| P1-09 | 上传完成/失败事件 | 上传成功发布 EVENT_UPLOAD_DONE；失败发布 EVENT_UPLOAD_FAILED；附带文件名信息 |

#### P2 — Nice to Have（v0.3 健壮性 + v0.4 AI 集成 + 固件清理）

| ID | 需求 | 验收标准 |
|----|------|----------|
| P2-01 | WiFi 指数退避重连 | WiFi 断开后按指数退避重连（2s→4s→8s→16s→32s→60s cap） |
| P2-02 | 大文件断点续传评估 | 产出评估文档：是否可行、实现方案、RAM/ROM 开销；不强制实现 |
| P2-03 | 上传队列状态查询 API | 服务端 GET /api/upload-queue 返回待上传文件列表（需 ESP32 端配合） |
| P2-04 | ESP32 接收转写结果 | 上传成功后，服务端返回转写结果（同步）或 ESP32 轮询获取（异步） |
| P2-05 | Transcript 本地存储 | 转写文本保存为 .txt 伴随 WAV 文件；存入 recordings/ 目录 |
| P2-06 | 转写结果同步预留接口 | 定义 webhook/callback 接口规范；不强制实现外部系统对接 |
| P2-07 | BUG-004/005 验证修复 | 确认 app_main 初始化序号 [1/12]..[12/12] 一致；确认 audio.c auto_clear=false |
| P2-08 | 录音前 SD 卡空间检查 | recorder_start() 前检查剩余空间，< 10MB 时拒绝录音并发布 EVENT_STORAGE_ERROR |
| P2-09 | rgb_led 组件清理 | 移除 components/rgb_led/ 目录，确认无其他组件依赖 |
| P2-10 | uploader 兼容接口清理 | 移除 uploader_upload()、uploader_get_progress()、uploader_delete_after_upload() 废弃 API |

---

## 5. UI Design Draft

### 5.1 服务端 Web UI（原生 HTML+CSS，FastAPI 静态页）

```
┌─────────────────────────────────────────────────────────┐
│  ESP32 AI Recorder — 管理面板                            │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  [录音列表]  [转写记录]  [系统状态]                       │
│                                                         │
│  ┌─ 录音列表 ────────────────────────────────────────┐  │
│  │  文件名            大小      上传时间       转写状态│  │
│  │  REC_SESSION_0001  2.3 MB   2026-05-15 10:23  ✅  │  │
│  │  REC_SESSION_0002  5.1 MB   2026-05-15 10:45  ⏳  │  │
│  │  REC_SESSION_0003  1.8 MB   2026-05-15 11:02  ❌  │  │
│  │                                                   │  │
│  │  [上一页]  第 1/3 页  [下一页]     [刷新]          │  │
│  └───────────────────────────────────────────────────┘  │
│                                                         │
│  ┌─ 转写结果 ────────────────────────────────────────┐  │
│  │  REC_SESSION_0001 (2026-05-15 10:23)              │  │
│  │  ─────────────────────────────────────────────    │  │
│  │  今天下午的会议讨论了三个主要议题...               │  │
│  │  第一项是项目进度回顾...                          │  │
│  │                                                   │  │
│  │  [下载 .txt]  [复制]                               │  │
│  └───────────────────────────────────────────────────┘  │
│                                                         │
│  ┌─ 系统状态 ────────────────────────────────────────┐  │
│  │  磁盘: 12.3 GB / 50 GB  |  文件: 47  |  转写: 32  │  │
│  │  mlx-whisper: 就绪  |  服务运行: 2h 15m           │  │
│  └───────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────┘
```

### 5.2 固件 Upload URL 配置流程

```
┌─ NVS 存储策略 ──────────────────────────────────┐
│                                                   │
│  优先级：                                         │
│  1. uploader_config_t 传入的 URL（运行时）        │
│  2. NVS "upload_url" key（持久化）               │
│  3. 默认值：http://record.east-deep.com/upload   │
│                                                   │
│  URL 格式：                                       │
│  - 局域网: http://192.168.1.39:8000/upload       │
│  - Tailscale: http://record.east-deep.com/upload │
│  - 自定义: http://<any-host>:<port>/upload       │
│                                                   │
│  存储位置: NVS namespace "uploader"              │
│  Key: "upload_url" (max 128 bytes)               │
└───────────────────────────────────────────────────┘
```

---

## 6. 服务端 API 设计

### 6.1 统一响应格式

```json
// 成功
{
  "code": 0,
  "message": "success",
  "data": { ... }
}

// 失败
{
  "code": 40001,
  "message": "File not found",
  "data": null
}
```

### 6.2 API Endpoints

| Method | Path | 说明 | 版本 |
|--------|------|------|------|
| POST | /upload | RAW BODY 接收 WAV 文件（兼容现有固件） | v0.3 |
| POST | /api/transcribe/{file_id} | 手动触发转写 | v0.3 |
| GET | /api/files | 文件列表（分页、排序、过滤） | v0.3 |
| GET | /api/files/{file_id} | 文件详情 | v0.3 |
| DELETE | /api/files/{file_id} | 删除文件及关联转写 | v0.3 |
| GET | /api/files/{file_id}/download | 下载 WAV 文件 | v0.3 |
| GET | /api/transcripts | 转写列表 | v0.3 |
| GET | /api/transcripts/{file_id} | 转写详情（含文本） | v0.3 |
| GET | /api/transcripts/{file_id}/export | 导出 .txt | v0.3 |
| GET | /api/status | 系统状态（磁盘、文件数、转写统计） | v0.3 |
| GET | /health | 健康检查 | v0.3 |
| GET | / | Web UI（HTML 页面） | v0.3 |

### 6.3 文件列表 API 参数

```
GET /api/files?page=1&page_size=20&sort=upload_time&order=desc&date_from=2026-05-01&date_to=2026-05-15&min_size=0&max_size=10485760&transcription_status=completed
```

### 6.4 数据库 Schema（SQLite）

```sql
CREATE TABLE files (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    filename    TEXT NOT NULL,           -- 原始文件名
    saved_name  TEXT NOT NULL UNIQUE,    -- 磁盘上的文件名
    file_size   INTEGER NOT NULL,        -- 字节
    upload_time DATETIME NOT NULL,       -- 上传时间
    upload_src  TEXT DEFAULT 'unknown',  -- 上传来源 IP
    created_at  DATETIME DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE transcriptions (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    file_id     INTEGER NOT NULL REFERENCES files(id) ON DELETE CASCADE,
    status      TEXT NOT NULL DEFAULT 'pending',  -- pending/processing/completed/failed
    text        TEXT,                    -- 转写文本
    model       TEXT,                    -- mlx-whisper 模型名
    language    TEXT,                    -- 检测到的语言
    duration    REAL,                    -- 音频时长（秒）
    error_msg   TEXT,                    -- 失败原因
    started_at  DATETIME,
    completed_at DATETIME,
    created_at  DATETIME DEFAULT CURRENT_TIMESTAMP
);
```

---

## 7. 服务端自动转写 Pipeline

```
┌─────────────┐     ┌─────────────┐     ┌──────────────┐     ┌───────────┐
│  ESP32 POST  │────▶│  /upload    │────▶│  后台任务队列 │────▶│ mlx-whisper│
│  audio/wav   │     │  保存文件    │     │  (asyncio)   │     │  转写      │
└─────────────┘     │  写 DB      │     │              │     │           │
                    │  触发转写    │     └──────────────┘     └─────┬─────┘
                    └─────────────┘                                │
                                                                   ▼
                    ┌─────────────┐     ┌──────────────┐     ┌───────────┐
                    │  GET /api/  │◀────│  更新 DB     │◀────│  写 .txt  │
                    │  transcripts│     │  status=done │     │  到磁盘   │
                    └─────────────┘     └──────────────┘     └───────────┘
```

**关键设计决策：**

- 转写采用异步后台任务，不阻塞上传响应（上传立即返回 200）
- 同时只运行 1 个转写任务（Apple Silicon GPU 限制），其余排队
- 转写超时：10 分钟（大文件保护）
- 失败自动重试 1 次，间隔 30 秒

---

## 8. 固件 Upload URL 配置方案

### 现状问题

```c
// uploader.c line 177 — 硬编码
#define UPLOAD_URL  "http://record.east-deep.com/upload"

// app_main.c line 308-313 — 配置传入但被忽略
uploader_config_t up_cfg = {
    .server_ip   = "192.168.1.39",   // ← 被忽略
    .server_port = 8000,             // ← 被忽略
    .upload_path = "/upload",        // ← 被忽略
    .timeout_ms  = 30000,
};
```

### 改造方案

```c
// uploader.h — 扩展配置结构体
typedef struct {
    char upload_url[128];     /*!< 完整上传 URL（优先） */
    char server_ip[32];       /*!< 服务端 IP（兼容旧配置） */
    uint16_t server_port;     /*!< 服务端端口 */
    char upload_path[64];     /*!< 上传路径 */
    uint32_t timeout_ms;      /*!< 超时时间 */
} uploader_config_t;

// uploader_init() 逻辑：
// 1. 若 upload_url 非空 → 使用 upload_url
// 2. 否则 → 从 server_ip + server_port + upload_path 拼接
// 3. 从 NVS 读取 "upload_url" → 若存在则覆盖
// 4. 最终 URL 存入 s_upload_url (static char[128])
```

### NVS 配置接口（预留）

```
// 未来可通过 BLE/WiFi provisioning 修改 NVS
// 当前通过 menuconfig 或代码硬编码默认值
esp_err_t uploader_set_url(const char *url);   // 写入 NVS
const char* uploader_get_url(void);            // 读取当前 URL
```

---

## 9. 版本规划与里程碑

### v0.3 — 上传闭环 + 服务端重构

| 阶段 | 内容 | 预估工作量 | 依赖 |
|------|------|-----------|------|
| v0.3-alpha | P0-01~04: URL 配置化 + 上传验证 | 2 天 | 无 |
| v0.3-beta | P1-01~06: 服务端重构 + API + DB | 3 天 | 无 |
| v0.3-rc | P1-07~09: Web UI + 固件事件 | 2 天 | v0.3-beta |
| v0.3-release | 集成测试 + 文档更新 | 1 天 | v0.3-rc |

### v0.4 — AI 转写集成 + 固件清理

| 阶段 | 内容 | 预估工作量 | 依赖 |
|------|------|-----------|------|
| v0.4-alpha | P2-04~06: ESP32 接收转写 + 本地存储 | 2 天 | v0.3 |
| v0.4-beta | P2-07~10: 固件 BUG 修复 + 清理 | 1 天 | 无 |
| v0.4-rc | P2-01~03: WiFi 重连 + 断点评估 | 2 天 | v0.3 |
| v0.4-release | 端到端测试 | 1 天 | v0.4-alpha + v0.4-rc |

---

## 10. P0 架构规则（不可违反）

| # | 规则 | 说明 |
|---|------|------|
| 1 | `audio_task` 永远运行 | 禁止 suspend/resume，永远拥有 I2S |
| 2 | 路径通过 `storage_build_vfs_path()` 获取 | 禁止硬编码 "/sdcard" 或目录名 |
| 3 | esp_timer callback 只做轻量信号 | `xSemaphoreGiveFromISR()`，禁止重操作 |
| 4 | inter-component 通信通过 event_bus | 禁止直接跨组件函数调用 |
| 5 | 目录生命周期由 `storage.c` 独享 | `recorder.c` 只做 fopen/fwrite/fclose |

---

## 11. Open Questions

| # | 问题 | 影响 | 建议 |
|---|------|------|------|
| 1 | 服务端框架选择：FastAPI vs 继续用 Flask | 开发效率、异步支持 | 推荐 FastAPI（原生 async、自动 OpenAPI 文档、类型校验） |
| 2 | 转写结果回传 ESP32 的方式 | P2-04 实现方案 | 建议 v0.4 先用同步方式（上传响应中返回），后续版本支持异步轮询 |
| 3 | SQLite 并发写入安全 | 多请求同时写入 | FastAPI 单进程 + asyncio 无并发写入问题；若多 worker 则需 WAL 模式 |
| 4 | Web UI 是否需要前端框架 | 开发复杂度 | 建议原生 HTML+CSS+JS，Jinja2 模板渲染；避免引入 React/Vue 增加部署复杂度 |
| 5 | 上传文件名冲突策略 | 并发上传可能同名 | 服务端检测重名自动追加 `_1`, `_2` 后缀（与固件 recorder 策略一致） |
| 6 | 转写模型选择 | 转写质量和速度 | 默认 `mlx-community/whisper-large-v3-turbo`；支持配置切换 |
| 7 | 服务端部署方式 | 便捷性 | 保持 `python upload_server.py` 一键启动；加 `requirements.txt` |

---

## 12. 约束与风险

| 约束/风险 | 影响 | 缓解措施 |
|-----------|------|----------|
| 当前无法操作硬件按键 | 无法验证固件完整流程 | 聚焦纯代码工作；服务端开发先行；固件改动通过代码审查保证 |
| BUG-001 SD 卡杜邦线接触 | 间歇性超时 | 不做固件修复；标注为硬件问题，v0.6 PCB 解决 |
| ESP32-S3 RAM 有限 (~320KB) | 上传 URL 配置不能太长 | URL 限制 128 字节；NVS value 限制处理 |
| mlx-whisper 仅限 Apple Silicon | 服务端必须运行在 Mac | 设计上不依赖特定平台；预留其他 whisper 后端接口 |
| FAT32 单文件 4GB 限制 | 超长录音文件 | 16kHz 16bit mono ≈ 32KB/s，4GB ≈ 36 小时，实际不会触及 |

---

## 附录 A: 固件现有组件状态

| 组件 | 状态 | v0.3~v0.4 变更 |
|------|------|----------------|
| event_bus | ✅ 稳定 | 无变更 |
| state | ✅ 稳定 | 无变更 |
| audio | ✅ 稳定 | BUG-005 修复 |
| recorder | ✅ 稳定 | 新增 SD 卡空间检查 |
| uploader | ⚠️ 需改造 | URL 配置化、事件发布、API 清理 |
| storage | ✅ 稳定 | 无变更 |
| wifi_manager | ✅ 稳定 | 可选：指数退避重连 |
| led | ✅ 稳定 | 无变更 |
| button | ✅ 稳定 | 无变更 |
| ui | ✅ 稳定 | 无变更 |
| battery | ✅ 稳定 | 无变更 |
| system_monitor | ✅ 稳定 | 无变更 |
| rgb_led | ⚠️ Legacy | 移除 |

## 附录 B: 服务端文件结构（规划）

```
server/
├── app.py                  # FastAPI 入口
├── config.py               # 配置（端口、目录、模型名）
├── database.py             # SQLite 初始化 + ORM
├── models.py               # 数据模型（File, Transcription）
├── routers/
│   ├── upload.py           # POST /upload
│   ├── files.py            # /api/files/*
│   ├── transcripts.py      # /api/transcripts/*
│   └── status.py           # /api/status, /health
├── services/
│   ├── transcriber.py      # mlx-whisper 调用封装
│   └── file_indexer.py     # 启动时索引 received/ 目录
├── templates/
│   ├── index.html          # 主页面
│   └── components/         # HTML 片段
├── static/
│   ├── style.css           # 样式
│   └── app.js              # 前端逻辑
├── received/               # 上传文件存储
├── transcripts/            # 转写文件存储
├── requirements.txt        # Python 依赖
└── recorder.db             # SQLite 数据库（运行时生成）
```
