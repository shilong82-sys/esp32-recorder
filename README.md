# ESP32 AI Recorder

> 基于 ESP32-S3 的离线录音 + AI 转写原型系统

## 项目目标

构建一个便携式录音设备：
- 通过 I2S 麦克风采集音频，写入 TF 卡（WAV）
- WiFi 联网后将录音文件上传到 Mac 服务端
- 服务端调用 Whisper（MLX/Metal）进行本地 AI 转写
- 未来接入 OpenClaw 实现记忆/对话能力

## 系统架构

```
[ESP32-S3 设备]
    ├─ I2S 麦克风 → WAV 文件（TF 卡）
    ├─ WiFi Manager → HTTP POST → [Mac 服务端]
    ├─ LED / Button → 用户交互
    └─ DeepSleep → 电池优化

[Mac 服务端 ~/Projects/recorder-server]
    ├─ FastAPI（POST /upload）
    ├─ Whisper 转写（mlx-whisper, Metal GPU）
    └─ 结果存储（transcripts/）
```

## 当前开发阶段

- [x] ESP-IDF v5.2.3 环境搭建（hello_world 验证通过）
- [x] FastAPI 服务框架（POST /upload 接口完成）
- [x] Whisper 转写验证（mlx-whisper 0.4.3, Metal GPU, 1.59s）
- [x] ffmpeg 安装（音频解码依赖）
- [ ] ESP32-S3 实体开发板烧录（待你操作）
- [ ] 设备端业务代码（I2S 录音、WiFi 上传）
- [ ] 端到端流程联调

## 后续开发路线

详细见 `docs/roadmap.md`

## 目录说明

| 目录 | 说明 |
|------|------|
| `firmware/` | ESP-IDF 固件工程（main + components） |
| `server/` | Mac 服务端代码（FastAPI） |
| `docs/` | 开发日志、路线图、设计规范 |
| `hardware/` | 硬件原理图、BOM、PCB 文件 |
| `scripts/` | 开发辅助脚本（flash/monitor/clean） |
| `test-audio/` | 音频测试资源（WAV 样本、参考转写） |
| `logs/` | 运行时日志（gitignore） |

## 快速开始

```bash
# 激活 ESP-IDF 环境
source ~/.espressif/idf-python-venv/bin/activate
export IDF_PATH=~/Projects/esp32-recorder/esp-idf
export IDF_SKIP_CHECK_SUBMODULES=1
export PATH="$HOME/.homebrew/bin:$PATH"

# 烧录固件（接上开发板后）
cd ~/Projects/esp32-recorder/firmware
idf.py -p /dev/tty.usbserial-* flash monitor

# 启动 Mac 服务端
cd ~/Projects/recorder-server
uv run uvicorn app.main:app --reload --host 0.0.0.0 --port 8000
```

## 环境要求

- macOS 26.4.1 (Apple Silicon)
- ESP-IDF v5.2.3（目标芯片 ESP32-S3）
- Python 3.11 + uv
- ffmpeg 8.1.1（~/.homebrew/bin/ffmpeg）
- mlx-whisper 0.4.3 + mlx 0.31.2
- 科学上网（HuggingFace 模型下载）
