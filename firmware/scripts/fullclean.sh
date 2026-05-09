#!/bin/bash
# fullclean.sh - 完全清理（删除 build/ 和 sdkconfig）
# 用法：./fullclean.sh
# ⚠️ 警告：会删除 sdkconfig，需要重新配置

set -e

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FIRMWARE_DIR="$PROJECT_ROOT/firmware"

echo "=========================================="
echo "  ESP32 AI Recorder - 完全清理"
echo "  ⚠️  会删除 build/ 和 sdkconfig"
echo "=========================================="

read -p "确认继续？(y/N) " -n 1 -r
echo
if [[ ! $REPLY =~ ^[Yy]$ ]]; then
    echo "[INFO] 已取消"
    exit 0
fi

# 检查 ESP-IDF 环境
if [ -z "$IDF_PATH" ]; then
    if [ -d "$HOME/Projects/esp32-recorder/esp-idf" ]; then
        export IDF_PATH="$HOME/Projects/esp32-recorder/esp-idf"
        source "$IDF_PATH/export.sh"
    fi
fi

cd "$FIRMWARE_DIR"
idf.py fullclean

echo ""
echo "[INFO] 完全清理完成"
echo "=========================================="
