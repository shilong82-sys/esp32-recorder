#!/bin/bash
# ESP32 Recorder Mac 接收服务器 - 启动脚本

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo "============================================"
echo "  ESP32 Recorder - Mac 接收服务器"
echo "============================================"
echo ""

# 检查 Python3
if ! command -v python3 &> /dev/null; then
    echo "❌ 未找到 python3，请先安装 Python 3"
    exit 1
fi

# 检查依赖
if ! python3 -c "import flask" 2>/dev/null; then
    echo "📦 正在安装依赖..."
    pip3 install -r requirements.txt
    if [ $? -ne 0 ]; then
        echo "❌ 依赖安装失败"
        exit 1
    fi
fi

echo "🚀 启动服务器..."
echo "   首页：http://localhost:8000"
echo "   上传接口：http://localhost:8000/upload"
echo "   Ctrl+C 停止服务器"
echo ""

python3 app.py
