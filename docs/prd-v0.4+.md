# PRD — ESP32 AI Recorder v0.4+

> Language: 中文 | Project: `esp32-recorder`
> Version: v0.4+ PRD (修订版) | Date: 2026-05-16
> Author: Qi (Delivery Director) — 基于用户反馈修订
> 基线版本: v0.3（已全部完成并通过联调验证）

---

## 1. 项目定位

**轻量个人语音记录工具**，不是企业级产品。

核心原则：**稳定 > 简单 > 实用**

- **稳定**：每个功能上线即可用，不追求大而全
- **简单**：个人工具，不需要多角色权限、审计日志、云同步
- **实用**：解决真实痛点，不做花架子

### 与原版 PRD 的关键差异

| 维度 | 原版（v1） | 修订版（v2） |
|------|-----------|-------------|
| 需求总量 | 47 项 | 19 项 |
| 认证方式 | JWT + refresh token | 简单密码 |
| 实时推送 | WebSocket | 轮询（够用就行） |
| 设备管理 | 独立 Tab + OTA + 远程配置 | 状态页显示基本信息 |
| 数据安全 | HTTPS + 审计 + 加密 | 局域网可信，暂不需要 |
| 版本数 | 3 个版本（v0.4/0.5/0.6） | 2 个版本（v0.4/0.5） |

---

## 2. 当前系统现状

v0.3 已完成端到端闭环：录音 → WiFi 上传 → 服务端接收 → mlx-whisper 转写 → Web UI 查看结果

### 已有能力

- 固件：I2S 录音、WiFi 流式上传、URL 配置化（NVS 优先级链）、event_bus 事件、孤儿文件恢复
- 服务端：FastAPI + SQLAlchemy 2.0 async，12 个 API 端点，mlx-whisper 单 worker 转写
- Web UI：3-tab 页面（录音列表/转写记录/系统状态），文件管理基础功能

### 核心痛点（个人使用视角）

| # | 痛点 | 为什么重要 |
|---|------|-----------|
| 1 | **转写语言固定** | 默认英文模型，中文录音识别差，必须可选语言 |
| 2 | **不能听录音** | 每次都要下载 WAV 才能确认内容，极不方便 |
| 3 | **转写不能改** | AI 转写有误只能接受，无法修正 |
| 4 | **找不到内容** | 录音多了只能翻列表，无法搜索 |
| 5 | **不知道谁在说** | 会议/对话场景无法区分说话人 |
| 6 | **没有基本安全** | 局域网内 API 完全开放 |
| 7 | **设置要改代码** | 换个模型/调个参数得改源码重启 |

---

## 3. 需求池

### P0 — v0.4 核心增强

> 聚焦：Web 界面从"能看"变"好用" + 转写语言选择 + 基本安全

| ID | 需求 | 验收标准 | 复杂度 |
|----|------|----------|--------|
| P0-01 | **转写语言选择（默认中文）** | 设置页可选择转写语言：中文/英文/日文/自动检测；默认中文；传递 `language` 参数给 mlx-whisper；已有转写记录显示所用语言；重新转写时可切换语言 | 低 |
| P0-02 | **音频在线播放** | 录音列表/详情页内嵌播放器，支持播放/暂停/进度条/音量；支持 WAV 16kHz 16bit mono | 中 |
| P0-03 | **转写结果编辑与保存** | 转写文本可编辑（textarea），保存按钮更新内容；显示编辑标记和保存时间 | 低 |
| P0-04 | **全文搜索** | 搜索框输入关键词 → 搜索转写 text 字段 → 返回匹配文件列表；高亮匹配词 | 低 |
| P0-05 | **简单密码认证** | 首次访问需输入密码（配置文件设置）；浏览器记住密码（localStorage）；/health 和 /upload（ESP32）免认证；无 JWT，用 session cookie | 低 |
| P0-06 | **系统设置页面** | 新增"设置"Tab：转写语言（下拉）、转写模型（下拉）、自动转写开关；修改后实时生效无需重启 | 中 |
| P0-07 | **录音列表显示时长** | files 列表增加 duration 列（从 WAV header 计算）；格式 mm:ss | 低 |
| P0-08 | **日期范围筛选** | 录音列表增加日期选择器（开始-结束）；URL 参数 date_from/date_to 前端补齐 | 低 |

### P1 — v0.5 转写质量 + 效率提升

> 聚焦：说话人分离 + 时间戳 + 批量操作 + 实用增强

| ID | 需求 | 验收标准 | 复杂度 |
|----|------|----------|--------|
| P1-01 | **说话人分离** | 转写结果标注说话人（S1/S2 或自定义名字）；设置页开关；数据库增加 speakers JSON 字段；前端按说话人分段显示 | 高 |
| P1-02 | **转写时间戳分段** | 转写结果包含 segment-level 时间戳；格式 [00:01 - 00:05] 文本；数据库增加 segments JSON 字段；点击段落跳转播放位置 | 中 |
| P1-03 | **多模型切换** | 设置页可选择 mlx-whisper 模型（tiny/base/small/medium/large-v3-turbo/large-v3）；重新转写可选模型 | 低 |
| P1-04 | **批量操作** | 录音列表支持多选 → 批量删除、批量重新转写；确认对话框 | 中 |
| P1-05 | **自动清理** | 设置页配置保留天数（默认 90 天）；后台定时任务自动清理超期文件 | 低 |
| P1-06 | **标签系统** | 录音可打标签；按标签筛选；新增 tags + file_tags 表 | 中 |
| P1-07 | **SRT/VTT 导出** | 转写详情页导出 SRT/VTT 字幕文件（基于时间戳分段） | 低 |
| P1-08 | **移动端适配** | ≤768px 响应式：单列布局、表格变卡片、播放器自适应 | 中 |
| P1-09 | **暗色模式** | 亮/暗主题切换；CSS 变量切换；localStorage 持久化 | 低 |
| P1-10 | **数据备份** | 一键导出 SQLite dump + WAV + transcripts（.tar.gz）；一键恢复 | 中 |
| P1-11 | **拖拽上传** | Web 页面拖拽 WAV 文件上传 | 低 |

---

## 4. 技术规范

### 4.1 新增/修改 API

#### P0-01 转写语言选择

| Method | Path | 说明 |
|--------|------|------|
| GET | /api/settings | 获取所有设置（含 transcribe_language） |
| PUT | /api/settings | 更新设置（含 language 字段） |

mlx-whisper 调用变更：
```python
# 之前
result = whisper_model.transcribe(audio_path)
# 之后
result = whisper_model.transcribe(audio_path, language=settings.get("transcribe_language", "zh"))
```

支持的语言值：`zh`（中文，默认）、`en`（英文）、`ja`（日文）、`ko`（韩文）、`auto`（自动检测）

#### P0-02 音频播放

| Method | Path | 说明 |
|--------|------|------|
| GET | /api/files/{id}/stream | 流式返回 WAV 音频（支持 Range 请求） |

#### P0-03 转写编辑

| Method | Path | 说明 |
|--------|------|------|
| PUT | /api/transcripts/{file_id} | 更新转写文本（编辑保存） |

#### P0-04 搜索

| Method | Path | 说明 |
|--------|------|------|
| GET | /api/search?q=keyword&type=transcript | 全文搜索转写文本 |

#### P0-05 简单密码认证

| Method | Path | 说明 |
|--------|------|------|
| POST | /api/auth/login | 密码验证 → session cookie |
| POST | /api/auth/logout | 退出登录 |

认证方案：**session cookie**（非 JWT），服务端内存维护 session，重启后需重新登录。

配置文件新增：
```python
# config.py
AUTH_PASSWORD: str = "changeme"  # .env 覆盖
AUTH_ENABLED: bool = True        # 设为 False 关闭认证
```

免认证端点：`/health`、`/upload`（ESP32 兼容）、`/api/auth/login`

#### P0-06 系统设置

| Method | Path | 说明 |
|--------|------|------|
| GET | /api/settings | 获取所有设置 |
| PUT | /api/settings | 更新设置 |
| GET | /api/settings/models | 获取可用转写模型列表 |

#### P1-01 说话人分离

| Method | Path | 说明 |
|--------|------|------|
| GET | /api/transcripts/{file_id}/speakers | 获取说话人信息 |
| PUT | /api/transcripts/{file_id}/speakers | 更新说话人名称 |

实现方案：使用 `pyannote-audio` 做说话人聚类 + mlx-whisper 时间戳对齐。
降级方案：如 pyannote 安装困难，提供"手动分段标注说话人"模式。

#### P1-04 批量操作

| Method | Path | 说明 |
|--------|------|------|
| POST | /api/files/batch-delete | 批量删除 {file_ids: [1,2,3]} |
| POST | /api/transcribe/batch | 批量触发转写 {file_ids: [1,2,3]} |

#### P1-07 SRT/VTT 导出

| Method | Path | 说明 |
|--------|------|------|
| GET | /api/transcripts/{file_id}/export?format=srt | 导出 SRT 字幕 |
| GET | /api/transcripts/{file_id}/export?format=vtt | 导出 VTT 字幕 |

#### P1-10 数据备份

| Method | Path | 说明 |
|--------|------|------|
| POST | /api/backup/export | 导出备份包 |
| POST | /api/backup/import | 导入备份包 |

### 4.2 数据库 Schema 变更

```sql
-- 新增：设置表（key-value）
CREATE TABLE settings (
    key         TEXT PRIMARY KEY,
    value       TEXT NOT NULL,
    updated_at  DATETIME DEFAULT CURRENT_TIMESTAMP
);

-- 预置设置项：
-- transcribe_language = 'zh'
-- transcribe_model = 'large-v3-turbo'
-- auto_transcribe = 'true'
-- cleanup_days = '90'

-- 新增：标签表
CREATE TABLE tags (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    name        TEXT NOT NULL UNIQUE,
    color       TEXT DEFAULT '#666666',
    created_at  DATETIME DEFAULT CURRENT_TIMESTAMP
);

-- 新增：文件-标签关联表
CREATE TABLE file_tags (
    file_id     INTEGER NOT NULL REFERENCES files(id) ON DELETE CASCADE,
    tag_id      INTEGER NOT NULL REFERENCES tags(id) ON DELETE CASCADE,
    PRIMARY KEY (file_id, tag_id)
);

-- 修改：transcriptions 增加字段
ALTER TABLE transcriptions ADD COLUMN segments TEXT;      -- JSON: [{start, end, text}]
ALTER TABLE transcriptions ADD COLUMN speakers TEXT;      -- JSON: [{id, name, segments}]
ALTER TABLE transcriptions ADD COLUMN language TEXT;      -- 检测/指定的语言
ALTER TABLE transcriptions ADD COLUMN is_edited INTEGER DEFAULT 0;
ALTER TABLE transcriptions ADD COLUMN edited_at DATETIME;

-- 修改：files 增加字段
ALTER TABLE files ADD COLUMN duration REAL;               -- 音频时长（秒）
```

### 4.3 服务端新增依赖

```
# 认证（简单方案）
itsdangerous>=2.1.0              # session cookie 签名

# 说话人分离（P1，可选）
pyannote-audio>=3.1.0           # 说话人聚类（需 HuggingFace token）

# 备份（P1）
# 无额外依赖，使用标准库 tarfile + sqlite3 .dump
```

---

## 5. UI 设计

### 5.1 v0.4 — 4-tab 结构

```
┌──────────────────────────────────────────────────────────┐
│  🎙️ ESP32 AI Recorder         [🔍 搜索]  [⚙] [🔒 退出]  │
├──────────────────────────────────────────────────────────┤
│                                                          │
│  [录音列表]  [转写记录]  [设置]  [系统状态]                │
│                                                          │
│  ┌─ 录音列表 ──────────────────────────────────────────┐ │
│  │  [搜索: ________] [日期范围] [刷新]                  │ │
│  │                                                     │ │
│  │  文件名          时长    大小    上传时间    转写状态  │ │
│  │  REC_0001        3:24   2.3MB   05-16 10:23  ✅ 中文 │ │
│  │  REC_0002        12:05  5.1MB   05-16 10:45  ⏳     │ │
│  │  REC_0003        1:02   1.8MB   05-16 11:02  ❌     │ │
│  │                                                     │ │
│  │  [▶ 播放器] ━━━●━━━━━━━━━ 01:23/03:24 🔊           │ │
│  │                                                     │ │
│  │  [上一页]  第 1/3 页  [下一页]                       │ │
│  └─────────────────────────────────────────────────────┘ │
│                                                          │
│  ┌─ 转写详情（点击录音展开）───────────────────────────┐ │
│  │  REC_0001 | 2026-05-16 10:23 | 语言: 中文 | 3:24   │ │
│  │  ─────────────────────────────────────────────────  │ │
│  │  今天下午的会议讨论了三个主要议题...                   │ │
│  │  第一项是项目进度回顾...                              │ │
│  │  接下来我们讨论了技术方案...                           │ │
│  │                                                     │ │
│  │  [📝 编辑] [📥 下载 .txt] [📋 复制]                 │ │
│  └─────────────────────────────────────────────────────┘ │
│                                                          │
│  ┌─ 设置 ──────────────────────────────────────────────┐ │
│  │                                                     │ │
│  │  转写设置                                           │ │
│  │  ├─ 语言: [中文 ▼]  (中文/英文/日文/韩文/自动检测)   │ │
│  │  ├─ 模型: [large-v3-turbo ▼]                        │ │
│  │  └─ 自动转写: [开/关]                                │ │
│  │                                                     │ │
│  │  安全设置                                           │ │
│  │  └─ 修改密码: [________] [确认]                     │ │
│  │                                                     │ │
│  │  [保存设置]                                         │ │
│  └─────────────────────────────────────────────────────┘ │
└──────────────────────────────────────────────────────────┘
```

### 5.2 v0.5 — 增强转写显示

```
┌─ 转写详情（带说话人 + 时间戳）──────────────────────────┐
│  REC_0001 | 2026-05-16 10:23 | 语言: 中文 | 3:24       │
│  ─────────────────────────────────────────────────────  │
│  [00:00] 🟢 S1(张三): 今天下午的会议讨论了三个议题...    │
│  [00:15] 🔵 S2(李四): 第一项是项目进度回顾...           │
│  [01:42] 🟢 S1(张三): 接下来讨论技术方案...             │
│  [02:30] 🔵 S2(李四): 我觉得方案二更可行...             │
│                                                         │
│  [📝 编辑] [📥 .txt] [📥 SRT] [📥 VTT] [📋 复制]       │
│  [🏷 标签: 会议, 技术方案]  [+ 添加标签]                │
└─────────────────────────────────────────────────────────┘
```

---

## 6. 版本规划

### v0.4 — 核心增强（预估 8 天）

> 主题：**能用 → 好用**

| 阶段 | 内容 | 预估 |
|------|------|------|
| v0.4-a1 | P0-01 转写语言选择 + P0-07 时长显示 | 1 天 |
| v0.4-a2 | P0-02 音频在线播放 | 1.5 天 |
| v0.4-a3 | P0-05 简单密码认证 + session | 1 天 |
| v0.4-a4 | P0-03 转写编辑 + P0-04 全文搜索 | 1.5 天 |
| v0.4-a5 | P0-06 系统设置页面 + settings 表 | 1.5 天 |
| v0.4-a6 | P0-08 日期范围筛选 | 0.5 天 |
| v0.4-rc | 集成测试 + Bug 修复 | 1 天 |
| **总计** | | **~8 天** |

### v0.5 — 转写质量 + 效率（预估 10 天）

> 主题：**好用 → 专业**

| 阶段 | 内容 | 预估 |
|------|------|------|
| v0.5-a1 | P1-01 说话人分离 + P1-02 时间戳分段 | 3 天 |
| v0.5-a2 | P1-03 多模型切换 + P1-04 批量操作 | 1.5 天 |
| v0.5-a3 | P1-05 自动清理 + P1-06 标签系统 | 2 天 |
| v0.5-a4 | P1-07 SRT/VTT 导出 | 1 天 |
| v0.5-a5 | P1-08 移动端适配 + P1-09 暗色模式 | 1.5 天 |
| v0.5-a6 | P1-10 数据备份 + P1-11 拖拽上传 | 1 天 |
| v0.5-rc | 集成测试 + Bug 修复 | 1 天 |
| **总计** | | **~11 天** |

---

## 7. 架构规则

继承 v0.3 全部规则，新增：

| # | 规则 | 说明 |
|---|------|------|
| 6 | **设置变更通过 settings 表** | 禁止直接修改 config.py 常量 |
| 7 | **转写语言参数透传** | settings → transcriber → mlx-whisper，全程 language 参数 |
| 8 | **说话人分离为可选** | 依赖 pyannote-audio，安装失败时降级为手动标注模式 |

---

## 8. 技术决策说明

| 决策 | 选择 | 理由 |
|------|------|------|
| 认证方案 | session cookie | 个人工具不需要 JWT，简单够用 |
| 实时推送 | 轮询（5s） | 个人使用 WebSocket 过重，轮询稳定可靠 |
| 搜索实现 | SQLite LIKE | 数据量 < 5000 条，LIKE 够用；超万条再上 FTS5 |
| 说话人分离 | pyannote-audio | 成熟方案，有降级策略 |
| Web UI 框架 | 继续原生 HTML/JS | 4-tab 不算复杂，无需引入框架 |
| 数据库 | 继续 SQLite | 个人工具不需要 PostgreSQL |

---

## 9. Open Questions

| # | 问题 | 建议 |
|---|------|------|
| 1 | pyannote-audio 安装门槛（需 HuggingFace token） | 做成可选依赖，安装失败时功能降级 |
| 2 | 移动端 WAV 播放兼容性 | 可能需要服务端转码为 MP3/AAC，v0.5 实测后决定 |
| 3 | 说话人名称自定义 | v0.5 先用 S1/S2，后续支持在设置中给常用说话人起名 |

---

## 附录 A: 需求优先级总览

```
P0 (v0.4) ─── 核心增强 ──────────────────────────────
  P0-01  转写语言选择（默认中文）     ★ 用户明确要求
  P0-02  音频在线播放
  P0-03  转写结果编辑与保存
  P0-04  全文搜索
  P0-05  简单密码认证
  P0-06  系统设置页面
  P0-07  录音列表显示时长
  P0-08  日期范围筛选

P1 (v0.5) ─── 转写质量 + 效率 ────────────────────────
  P1-01  说话人分离                  ★ 用户明确要求
  P1-02  转写时间戳分段
  P1-03  多模型切换
  P1-04  批量操作
  P1-05  自动清理
  P1-06  标签系统
  P1-07  SRT/VTT 导出
  P1-08  移动端适配
  P1-09  暗色模式
  P1-10  数据备份
  P1-11  拖拽上传
```

## 附录 B: 被砍功能及理由

| 原需求 | 砍掉理由 |
|--------|---------|
| WebSocket 实时推送 | 个人工具轮询够用，WebSocket 增加复杂度 |
| JWT 认证 + refresh token | 个人工具不需要多角色，简单密码即可 |
| 设备管理面板 | 只有一个设备，不需要管理面板 |
| OTA 固件升级 | 个人设备物理烧录即可，OTA 增加固件端复杂度 |
| 远程配置下发 | 改配置可通过设置页+重启，不需要实时下发 |
| AI 摘要 | 依赖外部 LLM，不够简单 |
| 翻译 | 依赖外部 LLM，不够简单 |
| PDF 导出 | 个人用不需要 PDF，SRT/VTT 足够 |
| API Token 管理 | 个人工具不需要多 token |
| 操作审计日志 | 个人工具不需要 |
| 性能监控面板 | 个人工具不需要 |
| 异常告警 | 个人工具不需要 |
| 云存储同步 | 过度工程 |
| 数据加密存储 | 局域网可信环境不需要 |
| 多 Worker 转写 | 个人录音量不需要并行 |
| PWA 支持 | 过度工程 |
| 键盘无障碍 | 个人工具不需要 |
| 多语言 UI | 中文够用 |
| 录音文件压缩 | ESP32 算力有限，收益不大 |
