# 开发日志

> ESP32 AI Recorder 项目 — 开发日志
> 硬件等待期 · 软件工程化阶段

---

## 当前项目状态

| 项目 | 状态 | 备注 |
|------|------|------|
| ESP-IDF 环境 | ✅ 完成 | v5.2.3，所有子模块已安装 |
| 工具链 | ✅ 完成 | Xtensa esp-13.2.0_20230928 |
| Python 环境 | ✅ 完成 | 3.11，mlx-whisper + FastAPI |
| Whisper 转写 | ✅ 完成 | tiny 模型，Metal GPU 加速 |
| 工程模板 | ✅ 完成 | stub 版，可编译通过 |
| logger 组件 | ✅ 完成 | 桩实现，支持多输出目标 |
| 硬件 | ❌ 未到货 | ESP32-S3、TF卡、I2S麦克风 |

---

## 已完成模块

- [x] ESP-IDF v5.2.3 安装与配置
- [x] 所有 ESP-IDF 子模块手动安装（22个）
- [x] Xtensa 交叉编译工具链
- [x] Python venv 环境（idf-python-venv）
- [x] FastAPI 接收服务器（app.py）
- [x] ffmpeg 从源码编译安装
- [x] mlx-whisper 转写环境（Metal GPU）
- [x] whisper-tiny 模型下载与缓存
- [x] ESP-IDF 工程模板（stub 版，编译通过）
- [x] 组件：recorder（stub，生成 440Hz 正弦波 WAV）
- [x] 组件：wifi_manager（桩）
- [x] 组件：storage（桩）
- [x] 组件：uploader（桩）
- [x] 组件：led（桩）
- [x] 组件：button（桩）
- [x] 组件：battery（桩）
- [x] 组件：logger（新创建，本批次）
- [x] app_main.c 简单版（按钮控制录音启停）
- [x] test_recorder.c（Mac 上独立测试 WAV 生成）
- [x] 工程化文档（本批次）

---

## 待完成模块（硬件到货后）

- [ ] I2S 麦克风驱动（真实硬件）
- [ ] 真实录音（I2S → WAV 文件）
- [ ] TF 卡读写（真实挂载与文件操作）
- [ ] WiFi 连接（真实配网）
- [ ] HTTP 上传到 Mac 服务器
- [ ] Whisper 转写集成（ESP32 触发 → Mac 执行）
- [ ] LED 状态指示（真实 GPIO 控制）
- [ ] 按钮事件处理（消抖、长按/短按）
- [ ] 电池电量检测（ADC 读取）
- [ ] DeepSleep 低功耗模式
- [ ] OTA 固件升级
- [ ] BLE 配网
- [ ] AES 加密传输

---

## 当前风险点

1. **硬件未到货**：所有真实功能无法验证，只能做桩实现
2. **I2S 驱动复杂性**：ESP32-S3 的 I2S 外设配置较复杂，需要仔细调试
3. **WiFi 上传稳定性**：大文件上传可能失败，需要断点续传或重试机制
4. **Whisper 转写延迟**：音频文件较大时，转写耗时可能较长
5. **功耗优化**：电池供电场景，需要精细的功耗管理
6. **TF 卡写入速度**：高速录音时，TF 卡写入可能成为瓶颈

---

## 已知技术债

1. **所有组件均为桩实现**：硬件到货后需要逐一替换为真实实现
2. **错误处理不完善**：当前代码很多地方直接忽略错误，正式版需要完善
3. **没有单元测试**：C 语言单元测试在嵌入式项目中比较麻烦，但需要至少做集成测试
4. **日志系统未完全集成**：logger 组件已创建，但各模块还未全面使用
5. **没有配置系统**：WiFi SSID/密码、服务器地址等硬编码，需要改为配置文件
6. **没有状态机设计**：当前 app_main 的主循环比较简单，后续需要正式的状态机
7. **Mac 服务端结构混乱**：当前只有一个 app.py，需要按 router/service/utils 拆分
8. **没有 Mock 测试链路**：硬件未到货，但需要能测试完整的"上传 → 转写"链路

---

## 每日开发记录模板

（后续每天的开发记录按此模板填写）

---

### 日期

YYYY-MM-DD

### 完成内容


### 遇到问题


### 解决方案


### 下一步


---

## 历史记录

### 2026-05-09

#### 完成内容
- ESP-IDF v5.2.3 全部初始化（含子模块）
- Xtensa 工具链安装
- Python venv 环境搭建
- ffmpeg 从源码编译安装
- mlx-whisper + Metal GPU 转写环境
- FastAPI 接收服务器（app.py）
- ESP-IDF 工程模板（stub 版）
- 所有基础组件（recorder/wifi_manager/storage/uploader/led/button/battery）
- app_main.c 简单版
- test_recorder.c（Mac 独立测试）
- logger 组件创建
- 工程化文档（dev-log.md, logging.md, state-machine.md 等）

#### 遇到问题
- ESP-IDF 子模块无法通过 git clone 安装（GitHub SSL 问题）
- 解决：通过 codeload.github.com tarball 手动下载安装
- ffmpeg 无法通过 Homebrew bottle 安装（非标准路径）
- 解决：从源码编译，耗时约 10 分钟

#### 解决方案
- 子模块：写了一个批量下载脚本，通过 curl 下载 tarball
- ffmpeg：使用非标准 Homebrew 路径，从源码编译

#### 下一步
- 等待硬件到货
- 硬件到货前完成工程化基础设施（日志、脚本、文档、配置、状态机）
