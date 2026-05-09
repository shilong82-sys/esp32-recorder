#!/bin/bash
# clean.sh - 清理构建产物（保留 sdkconfig）
# 用法：./clean.sh

set -e

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FIRMWARE_DIR="$PROJECT_ROOT/firmware"

echo "=========================================="
echo "  ESP32 AI Recorder - 清理构建"
echo "=========================================="

# 检查 ESP-IDF 环境
if [ -z "$IDF_PATH" ]; then
    if [ -d "$HOME/Projects/esp32-recorder/esp-idf" ]; then
        export IDF_PATH="$HOME/Projects/esp32-recorder/esp-idf"
        source "$IDF_PATH/export.sh"
    fi
fi

cd "$FIRMWARE_DIR"
idf.py clean

echo ""
echo "[INFO] 清理完成（sdkconfig 已保留）"
echo "=========================================="
