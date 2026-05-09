# 日志规范

> ESP32 AI Recorder 项目 — 统一日志规范
> 版本：v0.1 | 日期：2026-05-09

---

## 1. ESP32 端日志规范

### 1.1 日志宏使用规则

| 宏 | 级别 | 使用场景 |
|----|------|----------|
| `ESP_LOGE(TAG, ...)` | ERROR | 不可恢复错误、初始化失败、硬件故障 |
| `ESP_LOGW(TAG, ...)` | WARN | 可恢复异常、降级处理、配置缺失 |
| `ESP_LOGI(TAG, ...)` | INFO | 关键状态变化、用户操作、模块初始化完成 |
| `ESP_LOGD(TAG, ...)` | DEBUG | 详细调试信息、循环内日志、数据内容 |

### 1.2 TAG 命名规范

统一使用**大写缩写**，长度不超过 10 字符：

| TAG | 模块 | 示例 |
|-----|------|------|
| `MAIN` | 主入口 app_main | `ESP_LOGI("MAIN", "启动完成");` |
| `WIFI` | WiFi 管理器 | `ESP_LOGI("WIFI", "已连接 %s", ssid);` |
| `RECODER` | 录音模块 | `ESP_LOGD("RECODER", "写入 %d bytes", n);` |
| `UPLOAD` | 上传模块 | `ESP_LOGI("UPLOAD", "进度 %d%%", pct);` |
| `STORAGE` | TF 卡存储 | `ESP_LOGW("STORAGE", "空间不足");` |
| `LED` | LED 指示 | `ESP_LOGD("LED", "状态改为 %d", s);` |
| `BTN` | 按钮 | `ESP_LOGI("BTN", "短按事件");` |
| `BAT` | 电池 | `ESP_LOGI("BAT", "电量 %d%%", pct);` |
| `LOGGER` | 日志组件 | `ESP_LOGI("LOGGER", "输出目标 0x%02X", t);` |

### 1.3 日志格式要求

```
[时间戳] [TAG] 消息内容
```

示例：
```
I (123456) MAIN: === ESP32 AI Recorder v0.2 ===
I (123789) WIFI: 正在连接 SSID=MyWiFi...
W (124000) WIFI: 连接超时，重试 1/3
I (125000) RECODER: 开始录音，文件=/sdcard/REC_20260509_001.wav
E (130000) STORAGE: SD 卡挂载失败，errno=12
```

### 1.4 禁止事项

- ❌ 不要在 `ESP_LOG` 宏内做函数调用（副作用）
- ❌ 不要在生产版本中保留 `ESP_LOGD`（用编译开关控制）
- ❌ 不要在三秒内打印超过 10 条日志（防止刷屏）
- ❌ 日志消息不要带换行符（`\n`），让 ESP_LOG 自动处理

---

## 2. Python 端日志规范（Mac 服务端）

### 2.1 使用标准 logging 模块

```python
import logging

logger = logging.getLogger(__name__)

logger.debug("详细调试信息")
logger.info("正常业务流程")
logger.warning("可恢复的异常")
logger.error("错误信息")
logger.critical("严重错误")
```

### 2.2 日志格式

```python
LOG_FORMAT = "[%(asctime)s] [%(levelname)-8s] [%(name)s] %(message)s"
DATE_FORMAT = "%Y-%m-%d %H:%M:%S"
```

输出示例：
```
[2026-05-09 18:30:00] [INFO    ] [server.upload] 收到上传请求，文件=REC_001.wav (1.2MB)
[2026-05-09 18:30:02] [INFO    ] [server.whisper] 转写完成，耗时 1.59s
[2026-05-09 18:30:03] [WARNING ] [server.upload] 文件已存在，跳过
```

### 2.3 模块命名

| logger name | 模块 |
|-------------|------|
| `server.app` | FastAPI 主应用 |
| `server.upload` | 上传接口 |
| `server.whisper` | Whisper 转写服务 |
| `server.config` | 配置加载 |

---

## 3. Whisper 转写日志规范

### 3.1 关键日志点

| 阶段 | 级别 | 内容 |
|------|------|------|
| 接收音频文件 | INFO | 文件名、文件大小 |
| 调用 Whisper | INFO | 模型名称、设备（CPU/Metal） |
| 转写进行中 | DEBUG | 进度（如果支持） |
| 转写完成 | INFO | 耗时、转录字数 |
| 保存 transcript | INFO | 输出文件路径 |
| 转写失败 | ERROR | 错误原因、临时文件位置 |

### 3.2 示例

```python
logger.info(f"开始转写：{wav_path} ({file_size_mb:.1f} MB)")
logger.info(f"模型：{model_name}，设备：{device}")
# ... 转写 ...
logger.info(f"转写完成，耗时 {elapsed:.2f}s，字数 {len(transcript)}")
logger.info(f"transcript 已保存：{output_path}")
```

---

## 4. 上传日志规范（ESP32 → Mac）

### 4.1 ESP32 端上传日志

```c
ESP_LOGI("UPLOAD", "开始上传 %s (%d bytes)", filename, file_size);
ESP_LOGI("UPLOAD", "上传进度 %d/%d bytes (%.1f%%)", sent, total, pct);
ESP_LOGI("UPLOAD", "上传完成，HTTP %d", status_code);
ESP_LOGW("UPLOAD", "重试 %d/%d", retry, max_retry);
ESP_LOGE("UPLOAD", "上传失败：%s", esp_err_to_name(err));
```

### 4.2 Mac 服务端接收日志

```python
logger.info(f"收到上传：{filename}，大小 {content_length} bytes")
logger.info(f"保存路径：{save_path}")
logger.info(f"MD5 校验：{md5_value}（{'通过' if ok else '失败'}）")
```

---

## 5. 错误日志规范

### 5.1 错误信息必须包含

1. **错误码或错误原因**（字符串，非仅数字）
2. **发生位置**（函数名或 TAG）
3. **上下文信息**（相关变量值）
4. **建议处理方法**（如果适用）

### 5.2 好的错误日志 ✅

```c
// Good
ESP_LOGE("STORAGE", "SD卡挂载失败：errno=%d，请检查卡是否插入", err);
ESP_LOGE("WIFI", "连接失败：SSID=%s，错误=%s，请检查密码", ssid, esp_err_to_name(err));
```

### 5.3 不好的错误日志 ❌

```c
// Bad
ESP_LOGE("STORAGE", "failed");   // 没有原因
ESP_LOGE("WIFI", "error %d", err); // 没有上下文，没有建议
```

---

## 6. 推荐：统一日志输出格式（最终目标）

### 6.1 控制台输出（开发阶段）

```
[2026-05-09 18:30:00.123] [INFO ] [MAIN] ESP32 AI Recorder v0.2 启动
[2026-05-09 18:30:00.456] [INFO ] [WIFI] 连接中 SSID=MyWiFi
[2026-05-09 18:30:03.789] [WARN ] [WIFI] 连接超时，重试 1/3
[2026-05-09 18:30:05.012] [INFO ] [WIFI] 连接成功，IP=192.168.1.100
[2026-05-09 18:30:06.345] [INFO ] [RECODER] 开始录音
```

### 6.2 TF 卡日志文件（量产阶段）

路径：`/sdcard/logs/2026-05-09.log`

```
2026-05-09 08:00:00.123 [INFO ] [MAIN] 系统启动
2026-05-09 08:00:01.456 [INFO ] [WIFI] 连接成功
2026-05-09 08:05:23.789 [INFO ] [RECODER] 录音完成，文件=REC_001.wav
2026-05-09 08:05:25.012 [INFO ] [UPLOAD] 上传完成
```

---

## 7. 日志级别动态调整（未来扩展）

通过 NVS 或串口命令动态调整日志级别：

```
# 串口命令示例
log set MAIN INFO
log set WIFI DEBUG
log set RECODER WARN
log set * ERROR   # 所有模块设为 ERROR
```

---

## 8. 检查清单

- [ ] 所有模块使用统一的 TAG 命名
- [ ] ERROR 日志包含错误原因和建议
- [ ] 不在循环中刷日志（加节流）
- [ ] Python 端使用 logging 模块，不用 print
- [ ] 日志格式统一（时间戳 + 级别 + TAG + 消息）
- [ ] 敏感信息不出现在日志中（WiFi 密码等）
