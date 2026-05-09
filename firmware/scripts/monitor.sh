#!/bin/bash
# monitor.sh - 监控 ESP32 串口输出
# 用法：./monitor.sh [串口设备] [波特率]

PORT="${1:-/dev/cu.usbserial-0001}"
BAUD="${2:-115200}"

echo "=========================================="
echo "  ESP32 AI Recorder - 串口监控"
echo "  串口：$PORT"
echo "  波特率：$BAUD"
echo "  退出：Ctrl+]"
echo "=========================================="

# 检查 ESP-IDF 环境
if [ -z "$IDF_PATH" ]; then
    if [ -d "$HOME/Projects/esp32-recorder/esp-idf" ]; then
        export IDF_PATH="$HOME/Projects/esp32-recorder/esp-idf"
        source "$IDF_PATH/export.sh"
    fi
fi

# 检查串口是否存在
if [ ! -e "$PORT" ]; then
    echo "[ERROR] 串口 $PORT 不存在"
    echo "[INFO] 可用串口："
    ls /dev/cu.* 2>/dev/null || echo "  （无可用串口）"
    exit 1
fi

idf.py -p "$PORT" monitor
