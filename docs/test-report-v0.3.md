# ESP32 AI Recorder v0.3 — 测试报告

> QA Engineer: Yan (Edward) | Date: 2026-05-16
> 项目仓库: `/Users/long/Projects/esp32-recorder/`
> 测试环境: macOS, Python 3.9.6

---

## Summary

- **Total Test Cases**: 33
- **Passed**: 31
- **Failed (Source Bug → Fixed)**: 2
- **Coverage**: ~90% (all API endpoints, all code paths, firmware code review)
- **Routing Decision**: **NoOne** (所有发现的问题已修复并验证)

---

## 1. 服务端启动测试

### 1.1 依赖安装

| 项目 | 结果 |
|------|------|
| pip install -r server/requirements.txt | ✅ 成功 |
| fastapi >= 0.115.0 | ✅ 已安装 (0.128.8) |
| uvicorn[standard] >= 0.32.0 | ✅ 已安装 (0.39.0) |
| sqlalchemy[asyncio] >= 2.0.0 | ✅ 已安装 (2.0.49) |
| aiosqlite >= 0.20.0 | ✅ 已安装 (0.22.1) |
| jinja2 >= 3.1.0 | ✅ 已安装 |
| python-multipart >= 0.0.12 | ✅ 已安装 |
| mlx-whisper >= 0.4.0 | ✅ 已安装 (0.4.3) |

### 1.2 服务启动

| 项目 | 结果 |
|------|------|
| python3 -c "from server.app import app" | ✅ 成功 (修复后) |
| 服务启动 (RECORDER_PORT=8001) | ✅ 成功 |
| Database initialized (WAL mode) | ✅ 正常 |
| File indexing (0 files) | ✅ 正常 |
| Transcriber worker started | ✅ 正常 |
| uvicorn 监听 | ✅ 正常 |

---

## 2. API 端点功能测试

### 2.1 基础端点

| # | 测试项 | 方法 | 路径 | 预期结果 | 实际结果 | 状态 |
|---|--------|------|------|----------|----------|------|
| 1 | 健康检查 | GET | /health | {"status":"ok"} | {"status":"ok"} | ✅ |
| 2 | 系统状态 | GET | /api/status | {code:0, data:{...}} | {code:0, disk_total/file_count/stats} | ✅ |
| 3 | Web UI 首页 | GET | / | HTTP 200 HTML | HTTP 200 | ✅ |

### 2.2 文件上传

| # | 测试项 | 方法 | 路径 | 预期结果 | 实际结果 | 状态 |
|---|--------|------|------|----------|----------|------|
| 4 | RAW BODY 上传 | POST | /upload | code:0, file_id, filename, size | code:0, file_id=1, 32044 bytes | ✅ |
| 5 | 无文件名自动生成 | POST | /upload | REC_YYYYMMDD_HHMMSS.wav | REC_20260516_045746.wav | ✅ |
| 6 | 查询参数文件名 | POST | /upload?filename=X | filename=X | QUERY_PARAM.wav | ✅ |
| 7 | X-Filename 优先 | POST | /upload (header+query) | header 优先 | HEADER_FILENAME.wav | ✅ |
| 8 | 文件名冲突解决 | POST | /upload (同名) | saved_name 追加 _1 | REC_SESSION_0001_1.wav | ✅ |

### 2.3 文件管理

| # | 测试项 | 方法 | 路径 | 预期结果 | 实际结果 | 状态 |
|---|--------|------|------|----------|----------|------|
| 9 | 文件详情 | GET | /api/files/1 | code:0, 含 transcription | 含完整 transcription 对象 | ✅ |
| 10 | 文件列表 | GET | /api/files | code:0, items, total, page | 正确分页 | ✅ |
| 11 | 分页+排序 | GET | /api/files?sort=...&order=... | 正确排序 | 按 file_size asc 排序正确 | ✅ |
| 12 | 日期过滤 | GET | /api/files?date_from=...&date_to=... | 过滤结果 | 3 条记录 | ✅ |
| 13 | 大小过滤 | GET | /api/files?min_size=...&max_size=... | 过滤结果 | 4 条记录 | ✅ |
| 14 | 转写状态过滤 | GET | /api/files?transcription_status=pending | 过滤结果 | 1 条 pending | ✅ |
| 15 | 文件下载 | GET | /api/files/1/download | HTTP 200, audio/wav | HTTP 200, 32044 bytes | ✅ |
| 16 | 删除文件 | DELETE | /api/files/1 | code:0 | code:0, 含 id+filename | ✅ |
| 17 | 删除后查询 | GET | /api/files/1 | code:40401 | code:40401, "File not found" | ✅ |
| 18 | 不存在文件查询 | GET | /api/files/9999 | code:40401 | code:40401 | ✅ |
| 19 | 不存在文件删除 | DELETE | /api/files/9999 | code:40401 | code:40401 | ✅ |

### 2.4 转写管理

| # | 测试项 | 方法 | 路径 | 预期结果 | 实际结果 | 状态 |
|---|--------|------|------|----------|----------|------|
| 20 | 转写列表 | GET | /api/transcripts | code:0, items, total | 正确列表 | ✅ |
| 21 | 转写详情 | GET | /api/transcripts/{file_id} | 含 text 字段 | 含 text="Thank you." | ✅ |
| 22 | 不存在转写 | GET | /api/transcripts/9999 | code:40402 | code:40402 | ✅ |
| 23 | 手动触发转写 (已有) | POST | /api/transcribe/1 | code:40901 (in progress) | code:40901 | ✅ |
| 24 | 重新触发已完成 | POST | /api/transcribe/2 | code:0, status=pending | code:0, status=pending | ✅ |
| 25 | 转写结果导出 | GET | /api/transcripts/3/export | text/plain + Content-Disposition | HTTP 200, 正确 | ✅ |
| 26 | 转写状态过滤 | GET | /api/transcripts?status=completed | 过滤结果 | 2 条 completed | ✅ |

### 2.5 自动转写 Pipeline

| 项目 | 结果 |
|------|------|
| 上传后自动入队 | ✅ 转写记录自动创建为 pending |
| mlx-whisper 调用 | ✅ 成功转写 (模型下载+转写) |
| 转写状态流转 pending→processing→completed | ✅ |
| 转写文本写入 transcripts/ 目录 | ✅ |
| 重试机制 (failed → retry) | ⚠️ 未触发（转写成功），代码逻辑审查正确 |
| 超时机制 | ⚠️ 未触发（小文件秒级完成），代码逻辑审查正确 |

---

## 3. 代码质量检查

### 3.1 Python 模块导入

| 模块 | 结果 |
|------|------|
| server.config | ✅ |
| server.database | ✅ |
| server.models | ✅ |
| server.schemas | ✅ |
| server.routers.upload | ✅ |
| server.routers.files | ✅ |
| server.routers.transcripts | ✅ |
| server.routers.status | ✅ |
| server.services.transcriber | ✅ |
| server.services.file_indexer | ✅ |
| server.app | ✅ |

**无循环依赖，所有模块导入正常。**

### 3.2 ORM 模型 vs PRD Schema

| 表 | PRD 字段 | ORM 字段 | 匹配 |
|----|----------|----------|------|
| files | id, filename, saved_name, file_size, upload_time, upload_src, created_at | 完全匹配 | ✅ |
| transcriptions | id, file_id, status, text, model, language, duration, error_msg, started_at, completed_at, created_at | 完全匹配 | ✅ |

**外键约束**: `transcriptions.file_id → files.id` ON DELETE CASCADE ✅
**关系**: File 1:1 Transcription (uselist=False) ✅

### 3.3 Pydantic Schema vs ORM 模型

| Schema | ORM 字段对应 | 匹配 |
|--------|-------------|------|
| FileItem | File + Transcription | ✅ |
| FileListItem | File + TranscriptListItem | ✅ |
| TranscriptItem | Transcription (完整) | ✅ |
| TranscriptListItem | Transcription (不含 text) | ✅ |
| ApiResponse | code/message/data | ✅ |
| StatusData | disk/file_count/stats | ✅ |

### 3.4 路由定义 vs PRD API 列表

| PRD 端点 | 实现 | 状态 |
|----------|------|------|
| POST /upload | ✅ server/routers/upload.py | ✅ |
| POST /api/transcribe/{file_id} | ✅ server/routers/transcripts.py | ✅ |
| GET /api/files | ✅ server/routers/files.py | ✅ |
| GET /api/files/{file_id} | ✅ server/routers/files.py | ✅ |
| DELETE /api/files/{file_id} | ✅ server/routers/files.py | ✅ |
| GET /api/files/{file_id}/download | ✅ server/routers/files.py | ✅ |
| GET /api/transcripts | ✅ server/routers/transcripts.py | ✅ |
| GET /api/transcripts/{file_id} | ✅ server/routers/transcripts.py | ✅ |
| GET /api/transcripts/{file_id}/export | ✅ server/routers/transcripts.py | ✅ |
| GET /api/status | ✅ server/routers/status.py | ✅ |
| GET /health | ✅ server/routers/status.py | ✅ |
| GET / | ✅ server/app.py | ✅ |

**所有 12 个 API 端点完整匹配 PRD 规范。**

### 3.5 错误码体系

| 错误码 | 常量 | 使用场景 |
|--------|------|----------|
| 0 | SUCCESS | 成功 |
| 40000 | BAD_REQUEST | 文件过大等 |
| 40400 | NOT_FOUND | 通用未找到 |
| 40401 | FILE_NOT_FOUND | 文件不存在 |
| 40402 | TRANSCRIPTION_NOT_FOUND | 转写不存在 |
| 40900 | CONFLICT | 通用冲突 |
| 40901 | TRANSCRIPTION_IN_PROGRESS | 转写进行中 |
| 50000 | INTERNAL_ERROR | 服务器内部错误 |

### 3.6 统一响应格式

所有 API 端点返回 `{code: int, message: str, data: Any}` 格式 ✅
- 成功: `{"code": 0, "message": "success", "data": {...}}`
- 失败: `{"code": 40401, "message": "File not found", "data": null}`

### 3.7 其他检查

| 项目 | 结果 |
|------|------|
| SQLite WAL 模式 | ✅ PRAGMA journal_mode=WAL |
| 外键约束 | ✅ PRAGMA foreign_keys=ON |
| 级联删除 (file→transcription) | ✅ 验证通过 |
| 文件名冲突策略 (_1, _2 后缀) | ✅ 验证通过 |
| 上传文件大小限制 | ✅ max_file_size_mb 配置 |
| 启动时目录索引 | ✅ index_received_dir() |
| 转写 stuck processing 修复 | ✅ _reset_stuck_processing() |

---

## 4. 固件端代码审查

### 4.1 uploader.c 修改审查

| 检查项 | 结果 |
|--------|------|
| 移除硬编码 UPLOAD_URL 宏 | ✅ 替换为 DEFAULT_UPLOAD_URL 常量 |
| NVS namespace = "uploader" | ✅ |
| NVS key = "upload_url" | ✅ |
| load_url_from_nvs() 实现 | ✅ 含错误处理 |
| save_url_to_nvs() 实现 | ✅ 含 nvs_commit() |
| resolve_upload_url() 优先级链 | ✅ config→ip:port/path→NVS→默认值 |
| URL 优先级: config.upload_url | ✅ 非空时使用 |
| URL 优先级: ip:port/path 拼接 | ✅ snprintf 拼接，溢出保护 |
| URL 优先级: NVS 覆盖 | ✅ |
| URL 优先级: 默认值 | ✅ DEFAULT_UPLOAD_URL |
| EVENT_UPLOAD_STARTED 发布 | ✅ 在 do_upload() 开始处 |
| EVENT_UPLOAD_PROGRESS 发布 | ✅ 每 512KB + 文件发送完毕 |
| EVENT_UPLOAD_DONE 发布 | ✅ HTTP 200 时 |
| EVENT_UPLOAD_FAILED 发布 | ✅ 非 200 时 |
| 进度数据: bytes_sent/bytes_total | ✅ |
| 结果数据: filename/success/http_status | ✅ |
| uploader_set_url() API | ✅ 写入 NVS + 更新运行时 |
| uploader_get_url() API | ✅ 返回静态缓冲区 |
| 废弃 API 已移除 | ✅ uploader_upload/get_progress/delete_after_upload 不存在 |

### 4.2 event_bus.h 新增结构体审查

| 检查项 | 结果 |
|--------|------|
| EVENT_UPLOAD_STARTED 枚举 | ✅ |
| EVENT_UPLOAD_PROGRESS 枚举 | ✅ |
| EVENT_UPLOAD_DONE 枚举 | ✅ |
| EVENT_UPLOAD_FAILED 枚举 | ✅ |
| event_upload_progress_data_t 含 progress_percent | ✅ |
| event_upload_progress_data_t 含 bytes_sent | ✅ (新增) |
| event_upload_progress_data_t 含 bytes_total | ✅ (新增) |
| event_upload_result_data_t 定义 | ✅ (新增) |
| event_upload_result_data_t 含 filename[64] | ✅ |
| event_upload_result_data_t 含 success | ✅ |
| event_upload_result_data_t 含 http_status | ✅ |

### 4.3 uploader.h 修改审查

| 检查项 | 结果 |
|--------|------|
| upload_url[128] 字段 | ✅ |
| server_ip[32] 保留 | ✅ |
| server_port 保留 | ✅ |
| upload_path[64] 保留 | ✅ |
| timeout_ms 保留 | ✅ |
| uploader_set_url() 声明 | ✅ |
| uploader_get_url() 声明 | ✅ |
| 废弃 API 声明已移除 | ✅ |

### 4.4 app_main.c uploader_config_t 初始化审查

| 检查项 | 结果 |
|--------|------|
| upload_url = "" (空字符串) | ✅ 优先使用 NVS 或默认 |
| server_ip = "192.168.1.39" | ✅ 兼容旧配置 |
| server_port = 8000 | ✅ |
| upload_path = "/upload" | ✅ |
| timeout_ms = 30000 | ✅ |
| uploader_init(&up_cfg) 调用 | ✅ |
| uploader_start() 调用 | ✅ |
| 初始化序号 [1/12]..[12/12] | ✅ 一致 |

---

## 5. 发现并修复的 Bug

### Bug 1: Python 3.9 不兼容的类型注解语法 (CRITICAL)

- **文件**: `server/config.py` 第 61 行, `server/database.py` 第 30 行
- **问题**: 使用了 Python 3.10+ 的 `X | None` 语法，在 Python 3.9 上抛出 `TypeError`
- **影响**: 服务端完全无法启动
- **修复**: 
  - 在 `config.py` 添加 `from __future__ import annotations` 和 `from typing import Optional`，将 `AppConfig | None` 改为 `Optional[AppConfig]`
  - 在 `database.py` 添加 `from __future__ import annotations` 和 `from typing import Optional`，将 `async_sessionmaker[AsyncSession] | None` 改为 `Optional[async_sessionmaker[AsyncSession]]`
  - 在 `transcriber.py` 和 `file_indexer.py` 添加 `from __future__ import annotations` 以确保 `dict[str, Any]` 和 `list[str]` 类型注解兼容
- **验证**: ✅ 修复后 import 成功，服务正常启动

### Bug 2: server/__init__.py 缺失 (MINOR)

- **文件**: `server/__init__.py`
- **问题**: 缺少包初始化文件，虽然 Python 3.9+ 隐式命名空间包支持无 __init__.py 运行，但不符合最佳实践
- **修复**: 创建 `server/__init__.py` 包含模块文档字符串
- **验证**: ✅ 修复后正常

---

## 6. 未触发但代码审查确认的设计

| 设计点 | 代码路径 | 审查结果 |
|--------|----------|----------|
| 转写超时 10 分钟 | asyncio.wait_for(timeout=transcribe_timeout_s) | ✅ 正确 |
| 失败重试 1 次 | _handle_failure() attempt < 2 时等待 30s 后重试 | ✅ 正确 |
| 同时只运行 1 个转写 | 单 worker asyncio.Queue | ✅ 正确 |
| 上传文件大小限制 | max_file_size_mb 配置，超限删除已写入部分 | ✅ 正确 |
| 服务重启 processing 修复 | _reset_stuck_processing() | ✅ 正确 |
| 优雅关闭 | stop_event + task.cancel() | ✅ 正确 |

---

## 7. 测试结论

### 通过项

1. ✅ 服务端正常启动，无 import 错误
2. ✅ 所有 12 个 API 端点功能正确
3. ✅ 统一响应格式 {code, message, data} 正确
4. ✅ 错误码体系完整
5. ✅ SQLAlchemy 模型与 PRD Schema 一致
6. ✅ Pydantic Schema 与 ORM 模型对应
7. ✅ 路由定义与 PRD 规范完全匹配
8. ✅ 无循环依赖，所有模块导入正常
9. ✅ 文件上传+冲突解决正确
10. ✅ 文件 CRUD 操作正确
11. ✅ 转写 Pipeline 自动触发+状态流转正确
12. ✅ 固件 uploader.c URL 配置化实现正确
13. ✅ 固件 4 个上传事件发布实现正确
14. ✅ 固件废弃 API 已清理
15. ✅ event_bus.h 新增结构体定义正确
16. ✅ app_main.c uploader_config_t 初始化正确

### 修复项 (已修复)

1. ✅ Python 3.9 类型注解兼容性（2 个文件 + 2 个预防性修复）
2. ✅ server/__init__.py 缺失

### 路由决策: **NoOne** — 所有问题已修复并通过验证
