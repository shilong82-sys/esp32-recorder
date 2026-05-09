#!/bin/bash
# flash.sh - 烧录固件到 ESP32
# 用法：./flash.sh [串口设备] [波特率]

set -e

PORT="${1:-/dev/cu.usbserial-0001}"
BAUD="${2:-921600}"
PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FIRMWARE_DIR="$PROJECT_ROOT/firmware"

echo "=========================================="
echo "  ESP32 AI Recorder - 烧录脚本"
echo "  串口：$PORT"
echo "  波特率：$BAUD"
echo "=========================================="

# 检查 ESP-IDF 环境
if [ -z "$IDF_PATH" ]; then
    echo "[WARN] IDF_PATH 未设置，尝试自动加载..."
    if [ -d "$HOME/Projects/esp32-recorder/esp-idf" ]; then
        export IDF_PATH="$HOME/Projects/esp32-recorder/esp-idf"
        source "$IDF_PATH/export.sh"
    else
        echo "[ERROR] 找不到 ESP-IDF，请设置 IDF_PATH"
        exit 1
    fi
fi

# 检查串口是否存在
if [ ! -e "$PORT" ]; then
    echo "[ERROR] 串口 $PORT 不存在"
    echo "[INFO] 可用串口："
    ls /dev/cu.* 2>/dev/null || echo "  （无可用串口）"
    exit 1
fi

echo "[INFO] 开始烧录..."
cd "$FIRMWARE_DIR"
idf.py -p "$PORT" -b "$BAUD" flash

echo ""
echo "[INFO] 烧录完成！"
echo "[INFO] 建议运行 ./monitor.sh 查看日志"
echo "=========================================="
