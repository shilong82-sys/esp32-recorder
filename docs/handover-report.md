# ESP32 AI Recorder — 项目交接报告

> 日期：2026-05-16 | 版本：v0.3（已联调验证） | 下一阶段：v0.4

---

## 1. 项目概述

**ESP32 AI Recorder** 是一个轻量个人语音记录工具：ESP32-S3 设备录音 → WiFi 上传到 Mac → mlx-whisper 自动转写 → Web UI 管理查看。

| 维度 | 详情 |
|------|------|
| 仓库 | `/Users/long/Projects/esp32-recorder/` |
| GitHub | `https://github.com/shilong82-sys/esp32-recorder.git` |
| 当前版本 | v0.3（commit `e8fd647`） |
| 目标设备 | ESP32-S3 N16R8 |
| 固件框架 | ESP-IDF v5.2.3 |
| 服务端 | FastAPI + SQLAlchemy 2.0 async + aiosqlite，端口 8000 |
| 转写引擎 | mlx-whisper (large-v3-turbo)，单 worker |
| 项目定位 | **轻量个人工具**，原则：**稳定 > 简单 > 实用** |

---

## 2. 当前系统架构

### 2.1 固件端（16 个组件）

```
firmware/
├── main/app_main.c              — 入口，初始化所有组件
└── components/
    ├── audio/                   — I2S 麦克风采集（16kHz 16bit mono）
    ├── recorder/                — WAV 录音闭环（ringbuf → SD 卡）
    ├── storage/                 — VFS 路径管理 + 目录生命周期
    ├── uploader/                — WiFi 流式上传（8KB chunk，RAW BODY POST）
    ├── wifi_manager/            — WiFi 连接管理 + Kconfig
    ├── event_bus/               — 组件间事件通信
    ├── button/                  — 按键控制（录音开始/停止）
    ├── led/                     — LED 指示灯
    ├── rgb_led/                 — RGB LED 状态显示
    ├── battery/                 — 电池监测
    ├── logger/                  — 日志系统
    ├── ringbuf/                 — 环形缓冲区
    ├── state/                   — 全局状态管理
    ├── system_monitor/          — 系统监控（堆栈/堆/任务）
    └── ui/                      — UI 控制
```

**关键设计**：
- `audio_task` 是 I2S 唯一 owner，**永远运行**，禁止 suspend/resume
- Upload URL 优先级链：`config.upload_url` → `ip:port/path` 拼接 → NVS → 默认值
- 启动时 `recover_recordings_dir()` 自动恢复孤儿文件到 upload_queue
- 4 个 event_bus 事件：UPLOAD_STARTED / PROGRESS / DONE / FAILED

### 2.2 服务端（Python FastAPI）

```
server/
├── app.py                      — FastAPI 入口 + 生命周期管理（7 行路由注册）
├── config.py                   — 配置管理（dataclass + 环境变量覆盖）
├── database.py                 — SQLAlchemy async engine + session
├── models.py                   — ORM 模型（File, Transcription）
├── schemas.py                  — Pydantic schema
├── requirements.txt            — 7 个依赖
├── routers/
│   ├── upload.py               — POST /upload（ESP32 文件上传）
│   ├── files.py                — 文件列表/详情/删除/流式下载
│   ├── transcripts.py          — 转写记录/详情/重新转写/下载txt
│   └── status.py               — 系统状态（磁盘/队列/运行时长）
├── services/
│   ├── transcriber.py          — mlx-whisper 单 worker 后台转写
│   └── file_indexer.py         — 启动时索引 received/ 目录已有文件
├── templates/
│   └── index.html              — Web UI 单页模板（177 行）
└── static/
    ├── app.js                  — 前端逻辑（882 行）
    └── style.css               — 样式（603 行）
```

**API 端点清单（12 个）**：

| Method | Path | 说明 |
|--------|------|------|
| POST | /upload | ESP32 上传 WAV 文件 |
| GET | /api/files | 文件列表（分页+排序+日期筛选） |
| GET | /api/files/{id} | 文件详情 |
| DELETE | /api/files/{id} | 删除文件+转写记录 |
| GET | /api/files/{id}/download | 下载 WAV 文件 |
| GET | /api/transcripts | 转写记录列表（分页+状态筛选） |
| GET | /api/transcripts/{file_id} | 转写详情 |
| POST | /api/transcripts/{file_id}/retranscribe | 重新转写 |
| GET | /api/transcripts/{file_id}/download | 下载转写 .txt |
| GET | /api/status | 系统状态 |
| GET | /health | 健康检查 |
| GET | / | Web UI 首页 |

**数据库表（2 个）**：
- `files`: id, filename, saved_name, file_size, upload_time, upload_src, created_at
- `transcriptions`: id, file_id(FK), status, text, model, language, duration, error_msg, started_at, completed_at, created_at

**转写流程**：
1. ESP32 上传 WAV → 服务端存入 `received/`
2. 文件记录入 DB → 转写记录创建（status=pending）→ 入队 asyncio.Queue
3. 单 worker 从队列取任务 → `mlx_whisper.transcribe()` → 结果写入 DB + `transcripts/*.txt`
4. 状态机：pending → processing → completed / failed（失败重试 1 次，30s 后）

### 2.3 Web UI

3-tab 单页应用（原生 HTML+CSS+JS）：
- **录音列表**：分页、大小/上传时间显示、删除（含确认）、转写状态
- **转写记录**：按状态筛选、查看详情、下载 .txt、复制文本、重新转写
- **系统状态**：文件数/磁盘信息/转写队列/运行时长

---

## 3. 已验证的功能

| 功能 | 状态 | 备注 |
|------|------|------|
| I2S 录音 → SD 卡 WAV | ✅ 联调通过 | 16kHz 16bit mono |
| WiFi 连接 + 流式上传 | ✅ 联调通过 | 约 310 KB/s，19.4MB 文件约 62s |
| Upload URL 配置化 | ✅ 联调通过 | NVS 优先级链 |
| 上传事件发布 | ✅ 联调通过 | 4 个 event_bus 事件 |
| 孤儿文件恢复 | ✅ 联调通过 | recover_recordings_dir() |
| FastAPI 服务端 | ✅ 测试通过 | 33 个 API 测试全部通过 |
| mlx-whisper 自动转写 | ✅ 联调通过 | 6/11 首批转写成功 |
| Web UI 3-tab | ✅ 联调通过 | 文件列表/转写/状态正常显示 |
| 文件索引 | ✅ 测试通过 | 启动时自动索引 received/ |
| GitHub 推送 | ✅ 完成 | commit e8fd647, 32 文件, +5361/-52 行 |

---

## 4. P0 架构规则（不可违反）

| # | 规则 | 说明 |
|---|------|------|
| 1 | `audio_task` 永远运行 | 禁止 suspend/resume，永远拥有 I2S |
| 2 | 所有路径通过 `storage_build_vfs_path()` 获取 | 禁止硬编码 "/sdcard" 或目录名 |
| 3 | esp_timer callback 只做轻量信号 | `xSemaphoreGiveFromISR()`，禁止重操作 |
| 4 | 所有 inter-component 通信通过 event_bus | 禁止直接跨组件函数调用 |
| 5 | 目录生命周期由 `storage.c` 独享 | `recorder.c` 只做 fopen/fwrite/fclose |
| 6 | 设置变更通过 settings 表 | 禁止直接修改 config.py 常量（v0.4+） |
| 7 | 转写语言参数透传 | settings → transcriber → mlx-whisper（v0.4+） |
| 8 | 说话人分离为可选 | 依赖 pyannote-audio，安装失败降级为手动标注（v0.5+） |

---

## 5. 下一阶段规划：v0.4+ PRD

**PRD 文档**：`docs/prd-v0.4+.md`（修订版，轻量定位）

### 5.1 v0.4 — 核心增强（预估 8 天）

| ID | 需求 | 复杂度 | 关键实现点 |
|----|------|--------|-----------|
| P0-01 | **转写语言选择（默认中文）** | 低 | config.py 加 `transcribe_language` 字段；transcriber.py 传 `language` 参数给 mlx-whisper；settings 表存语言偏好；前端设置页下拉选择 |
| P0-02 | **音频在线播放** | 中 | 新增 `GET /api/files/{id}/stream`（支持 Range 请求）；前端 HTML5 Audio 播放器；WAV 16kHz mono |
| P0-03 | **转写结果编辑与保存** | 低 | 新增 `PUT /api/transcripts/{file_id}`；前端 textarea 编辑 + 保存按钮；DB 加 is_edited + edited_at 字段 |
| P0-04 | **全文搜索** | 低 | 新增 `GET /api/search?q=keyword`；SQLite LIKE 查询（<5000 条够用）；前端搜索框 + 结果高亮 |
| P0-05 | **简单密码认证** | 低 | 新增 `POST /api/auth/login`；session cookie 签名（itsdangerous）；/health 和 /upload 免认证；.env 配置密码 |
| P0-06 | **系统设置页面** | 中 | 新增 settings 表（key-value）；新增 `GET/PUT /api/settings`；前端"设置"Tab；修改后实时生效无需重启 |
| P0-07 | **录音列表显示时长** | 低 | 从 WAV header 读取时长（或 transcriptions.duration）；files 列表加 duration 列；DB files 表加 duration 字段 |
| P0-08 | **日期范围筛选** | 低 | 后端已有 date_from/date_to 参数；前端补齐日期选择器 |

### 5.2 v0.5 — 转写质量 + 效率（预估 11 天）

| ID | 需求 | 复杂度 | 关键实现点 |
|----|------|--------|-----------|
| P1-01 | **说话人分离** | 高 | pyannote-audio 聚类 + mlx-whisper 时间戳对齐；DB 加 speakers JSON；前端按说话人分段显示；降级方案：手动标注 |
| P1-02 | **转写时间戳分段** | 中 | mlx-whisper 返回 segments；DB 加 segments JSON；前端 `[00:01] 文本` 格式；点击跳转播放位置 |
| P1-03 | **多模型切换** | 低 | 设置页模型下拉；重新转写可选模型；模型列表 API |
| P1-04 | **批量操作** | 中 | 新增 batch-delete/batch-transcribe API；前端 checkbox 多选 + 确认对话框 |
| P1-05 | **自动清理** | 低 | settings 配置保留天数；APScheduler 或 asyncio 定时任务 |
| P1-06 | **标签系统** | 中 | 新增 tags + file_tags 表；CRUD API；前端标签输入/筛选 |
| P1-07 | **SRT/VTT 导出** | 低 | 基于时间戳 segments 生成；新增 export API |
| P1-08 | **移动端适配** | 中 | ≤768px 响应式；表格变卡片；播放器自适应 |
| P1-09 | **暗色模式** | 低 | CSS 变量切换；localStorage 持久化 |
| P1-10 | **数据备份** | 中 | tar.gz 打包 SQLite dump + WAV + transcripts；export/import API |
| P1-11 | **拖拽上传** | 低 | 前端 drag-drop + 上传进度 |

---

## 6. 关键文件索引

### 固件端

| 文件 | 行数 | 说明 |
|------|------|------|
| `firmware/main/app_main.c` | ~200 | 入口，初始化所有组件，配置 upload URL |
| `firmware/components/uploader/uploader.c` | ~600 | WiFi 流式上传 + NVS 优先级链 + 4 事件发布 + 孤儿恢复 |
| `firmware/components/uploader/include/uploader.h` | ~80 | upload_url 字段 + API 声明 |
| `firmware/components/storage/storage.c` | ~300 | VFS 路径管理 + 目录生命周期 |
| `firmware/components/recorder/recorder.c` | ~400 | WAV 录音闭环（ringbuf → fopen/fwrite/fclose） |
| `firmware/components/audio/audio.c` | ~300 | I2S 采集 task |
| `firmware/components/event_bus/include/event_bus.h` | ~60 | 事件定义 + 数据结构 |

### 服务端

| 文件 | 行数 | 说明 |
|------|------|------|
| `server/app.py` | 137 | FastAPI 入口，4 路由挂载，生命周期管理 |
| `server/config.py` | 77 | 配置 dataclass，7 个配置项，支持 .env 覆盖 |
| `server/database.py` | ~80 | async engine + session factory |
| `server/models.py` | 109 | File + Transcription ORM |
| `server/schemas.py` | ~120 | Pydantic 请求/响应 schema |
| `server/routers/upload.py` | 169 | ESP32 上传端点 |
| `server/routers/files.py` | 292 | 文件管理 CRUD |
| `server/routers/transcripts.py` | 235 | 转写管理 |
| `server/routers/status.py` | 84 | 系统状态 |
| `server/services/transcriber.py` | 389 | 转写单 worker，状态机，重试 |
| `server/services/file_indexer.py` | 104 | 启动时索引已有文件 |
| `server/templates/index.html` | 177 | Web UI 模板 |
| `server/static/app.js` | 882 | 前端逻辑 |
| `server/static/style.css` | 603 | 前端样式 |

### 文档

| 文件 | 说明 |
|------|------|
| `docs/prd-v0.4+.md` | **v0.4+ 产品需求文档（修订版，19 项需求）** |
| `docs/prd-v0.3-v0.4.md` | v0.3 PRD（已完成的版本） |
| `docs/architecture-v0.3-v0.4.md` | v0.3 架构设计 |
| `docs/test-report-v0.3.md` | v0.3 测试报告 |
| `docs/coding-style.md` | 编码规范 |
| `docs/realtime-rules.md` | 实时性规则 |
| `docs/storage-layout.md` | 存储布局 |
| `docs/storage-path-policy.md` | 路径策略 |
| `docs/event-system.md` | 事件系统 |
| `docs/roadmap.md` | 旧路线图（已过时，以 prd-v0.4+.md 为准） |

---

## 7. 开发环境与操作

### 服务端启动

```bash
cd /Users/long/Projects/esp32-recorder
python -m server.app
# 或
python -m uvicorn server.app:app --host 0.0.0.0 --port 8000
```

服务启动后：
- Web UI: http://192.168.1.205:8000/
- API docs: http://192.168.1.205:8000/docs
- 健康检查: http://192.168.1.205:8000/health

### 固件编译烧录

```bash
cd /Users/long/Projects/esp32-recorder/firmware
idf.py build
idf.py -p /dev/cu.usbserial-2110 flash monitor
```

### 运行时数据（已 .gitignore）

- `server/received/` — 上传的 WAV 文件
- `server/transcripts/` — 转写文本文件
- `server/recorder.db*` — SQLite 数据库

### 依赖安装

```bash
pip install -r server/requirements.txt
# 当前 7 个依赖：fastapi, uvicorn, sqlalchemy, aiosqlite, jinja2, python-multipart, mlx-whisper
```

---

## 8. 已知问题与注意事项

| # | 问题 | 影响 | 建议 |
|---|------|------|------|
| 1 | 转写语言固定（无 language 参数） | 中文录音识别质量差 | v0.4 P0-01 优先解决 |
| 2 | 服务端 app_main.c 中 server_ip 硬编码 `192.168.1.205` | IP 变更需重新编译 | 可接受（个人设备，IP 比较稳定） |
| 3 | 旧 Flask 服务器 `upload_server.py` 仍存在 | 可能端口冲突 | 启动前确认 8000 端口未被占用 |
| 4 | prd-v0.4+.md 未 git commit | PRD 修订后尚未推送 | 开发前先 commit |
| 5 | 转写 worker 不传 language 参数 | mlx-whisper 默认英文 | 需修改 transcriber.py `_transcribe_audio()` |

---

## 9. 用户偏好

- **沟通语言**：中文
- **风格**：简洁高效，结论前置，偏好直接选择预设方案
- **工作方法**：规划先行，实现与规划分离，逐步演进
- **固件烧录**：倾向自行执行烧录命令（端口问题时）
- **代码修改确认后**：通常会请求 git commit 和 push
- **当前位置**：频繁往返杭州-温州出差

---

## 10. v0.4 开发建议（给下一个 Agent）

### 推荐实现顺序

1. **P0-01 转写语言选择** — 改动最小，收益最大
   - `config.py` 加 `transcribe_language` 字段
   - `transcriber.py` 的 `_transcribe_audio()` 加 `language` 参数
   - `models.py` 新增 `settings` 表
   - 新增 `routers/settings.py`
   - 前端设置页加语言下拉

2. **P0-07 时长显示 + P0-08 日期筛选** — 前端小改动

3. **P0-05 简单密码认证** — 加 session 中间件

4. **P0-06 系统设置页面** — 整合以上设置项

5. **P0-02 音频播放** — 需新增 stream 端点 + 前端播放器

6. **P0-03 转写编辑 + P0-04 搜索** — 功能独立，可并行

### 技术决策速查

| 决策 | 结论 | 理由 |
|------|------|------|
| 认证方案 | session cookie | 个人工具，简单够用 |
| 实时推送 | 轮询 5s | 个人使用不需要 WebSocket |
| 搜索 | SQLite LIKE | <5000 条够用 |
| Web UI | 继续原生 HTML/JS | 4-tab 不复杂 |
| 数据库 | SQLite | 个人工具不需要 PG |
| 说话人分离 | pyannote-audio | v0.5 再做，有降级方案 |

---

_报告结束。接手后建议先读 `docs/prd-v0.4+.md` 了解完整需求，再按上述顺序开始 v0.4 开发。_
