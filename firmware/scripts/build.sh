#!/bin/bash
# build.sh - 构建 ESP-IDF 项目
# 用法：./build.sh [目标]

set -e

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FIRMWARE_DIR="$PROJECT_ROOT/firmware"

echo "=========================================="
echo "  ESP32 AI Recorder - 构建脚本"
echo "  项目目录：$FIRMWARE_DIR"
echo "=========================================="

# 检查 ESP-IDF 环境
if [ -z "$IDF_PATH" ]; then
    echo "[WARN] IDF_PATH 未设置，尝试自动加载..."
    if [ -f "$HOME/esp/esp-idf/export.sh" ]; then
        source "$HOME/esp/esp-idf/export.sh"
    elif [ -d "$HOME/Projects/esp32-recorder/esp-idf" ]; then
        export IDF_PATH="$HOME/Projects/esp32-recorder/esp-idf"
        source "$IDF_PATH/export.sh"
    else
        echo "[ERROR] 找不到 ESP-IDF，请设置 IDF_PATH"
        exit 1
    fi
fi

echo "[INFO] IDF_PATH=$IDF_PATH"
echo "[INFO] 开始构建..."

cd "$FIRMWARE_DIR"
idf.py build

echo ""
echo "[INFO] 构建完成！"
echo "[INFO] 固件路径：$FIRMWARE_DIR/build/esp32-ai-recorder.bin"
echo "=========================================="
