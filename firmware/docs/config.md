# 配置系统说明

> ESP32 AI Recorder — 配置系统文档
> 版本：v0.1 | 日期：2026-05-09

---

## 1. 配置系统概述

本项目采用**分层配置**设计：

```
┌─────────────────────────────────────┐
│         config/ (YAML 文件）         │  ← 开发/调试用（人类可读）
├─────────────────────────────────────┤
│         NVS (Non-Volatile Storage)  │  ← 量产用（键值存储）
├─────────────────────────────────────┤
│         menuconfig (sdkconfig)      │  ← ESP-IDF 编译时配置
└─────────────────────────────────────┘
```

**优先级（从高到低）：**
1. 命令行参数（未来扩展）
2. NVS 存储（量产设备）
3. YAML 配置文件（开发阶段）
4. 代码中的默认值

---

## 2. 配置文件结构

### 2.1 `config/server.yaml`

**用途：** 服务器连接参数、WiFi 凭证、日志设置

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `server.host` | string | `"192.168.31.185"` | Mac 服务器 IP |
| `server.port` | int | `8000` | 服务器端口 |
| `server.timeout_ms` | int | `30000` | 上传超时 |
| `wifi.ssid` | string | `"YOUR_WIFI_SSID"` | WiFi 名称 |
| `wifi.password` | string | `"YOUR_WIFI_PASSWORD"` | WiFi 密码 |
| `logging.level` | string | `"INFO"` | 日志级别 |
| `logging.outputs.console` | bool | `true` | 控制台输出 |
| `logging.outputs.sd_card` | bool | `false` | TF 卡输出 |
| `dev.stub_mode` | bool | `true` | 桩模式开关 |

**注意：** `wifi.ssid` 和 `wifi.password` 在量产时**不应**放在 YAML 文件中，而是通过配网（BLE/SmartConfig）写入 NVS。

---

### 2.2 `config/audio.yaml`

**用途：** 音频采集参数、I2S 引脚、WAV 格式

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `i2s.port` | int | `0` | I2S 端口号 |
| `i2s.sample_rate` | int | `16000` | 采样率（Hz） |
| `i2s.bits_per_sample` | int | `16` | 位深 |
| `i2s.channels` | int | `1` | 声道数 |
| `i2s.gpio.bck_io_num` | int | `26` | BCK 引脚（待确认） |
| `i2s.gpio.ws_io_num` | int | `25` | WS 引脚（待确认） |
| `i2s.gpio.data_in_num` | int | `35` | DATA 引脚（待确认） |
| `wav.format_tag` | int | `1` | PCM 格式 |
| `quality.low` | map | ... | 低质量预设 |
| `quality.medium` | map | ... | 中质量预设 |
| `quality.high` | map | ... | 高质量预设 |
| `default_quality` | string | `"low"` | 默认质量 |

**硬件到货后：** 需要根据实际硬件设计填写正确的 GPIO 引脚号。

---

### 2.3 `config/device.yaml`

**用途：** ESP32 型号、引脚分配、电源管理

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `device.model` | string | `"ESP32-S3-N8R8"` | 芯片型号 |
| `pins.led.gpio` | int | `2` | LED GPIO |
| `pins.button.gpio` | int | `0` | 按钮 GPIO |
| `pins.battery.adc_pin` | int | `36` | 电池 ADC 引脚 |
| `power.vbat_full` | float | `4.2` | 满电电压 |
| `power.vbat_empty` | float | `3.3` | 空电电压 |
| `watchdog.timeout_sec` | int | `30` | 看门狗超时 |
| `flash.size` | string | `"8MB"` | Flash 大小 |
| `psram.enabled` | bool | `true` | 是否启用 PSRAM |

---

## 3. ESP32 端加载配置（C 代码）

### 3.1 从 NVS 读取（量产）

```c
#include "nvs_flash.h"
#include "nvs.h"

void config_load_from_nvs() {
    nvs_handle_t nvs;
    nvs_open("config", NVS_READONLY, &nvs);
    
    // 读取 WiFi SSID
    char ssid[32];
    size_t len = sizeof(ssid);
    if (nvs_get_str(nvs, "wifi_ssid", ssid, &len) == ESP_OK) {
        strcpy(wifi_cfg.ssid, ssid);
    }
    
    nvs_close(nvs);
}
```

### 3.2 从 YAML 读取（开发阶段，stub）

```c
// 当前 stub 版本：直接在代码中写死默认值
// 未来扩展：使用 libyaml 解析 YAML 文件（需要额外组件）
```

---

## 4. Mac 服务端配置（Python）

服务端使用 `config/server.yaml`：

```python
import yaml

with open('config/server.yaml', 'r') as f:
    config = yaml.safe_load(f)

SERVER_HOST = config['server']['host']
SERVER_PORT = config['server']['port']
```

---

## 5. 未来扩展

### 5.1 配网模式（WiFi 凭证写入 NVS）

1. 设备进入 BLE 配网模式（LED 快闪）
2. 手机 App 连接 BLE，发送 WiFi SSID/密码
3. 设备尝试连接，成功后保存到 NVS
4. 后续启动自动从 NVS 读取

### 5.2 配置文件热更新

- 监听 HTTP `PUT /config` 接口
- 接收新的 YAML 配置
- 写入 NVS
- 重启生效

### 5.3 配置版本管理

```yaml
version: 1
config:
  server: ...
  wifi: ...
```

设备启动时检查版本号，若 YAML 版本 > NVS 版本，则更新 NVS。

---

## 6. 安全检查清单

- [ ] `wifi.password` 不在代码中硬编码
- [ ] `config/` 目录不提交到公开 Git 仓库（加入 `.gitignore`）
- [ ] 量产设备不从 NVS 读取明文密码（使用 WPA2-PSK）
- [ ] 配置文件中的敏感信息使用环境变量或加密存储

---

## 7. 当前状态

- [x] `config/server.yaml` 创建
- [x] `config/audio.yaml` 创建
- [x] `config/device.yaml` 创建
- [x] `docs/config.md` 创建
- [ ] C 代码解析 YAML（stub 阶段暂不实现）
- [ ] NVS 读写配置（Phase 2 实现）
- [ ] 配网模式（Phase 2 实现）
