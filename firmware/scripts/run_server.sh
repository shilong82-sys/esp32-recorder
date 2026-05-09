#!/bin/bash
# run_server.sh - 启动 Mac 端 FastAPI 接收服务器
# 用法：./run_server.sh [--reload] [--port 端口]

set -e

PORT=8000
RELOAD=""

for arg in "$@"; do
    case $arg in
        --reload)  RELOAD="--reload" ;;
        --port)     PORT="$2"; shift ;;
        *)          echo "[WARN] 未知参数：$arg" ;;
    esac
    shift
done

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SERVER_DIR="$PROJECT_ROOT/firmware/server"
UPLOADS_DIR="$SERVER_DIR/uploads"
LOGS_DIR="$SERVER_DIR/logs"

echo "=========================================="
echo "  ESP32 AI Recorder - 启动服务端"
echo "  目录：$SERVER_DIR"
echo "  端口：$PORT"
echo "=========================================="

# 创建必要目录
mkdir -p "$UPLOADS_DIR" "$LOGS_DIR"

# 检查依赖
if ! command -v python3 &> /dev/null; then
    echo "[ERROR] python3 未找到"
    exit 1
fi

# 检查 flask 是否安装
if ! python3 -c "import flask" 2>/dev/null; then
    echo "[WARN] flask 未安装，正在安装..."
    python3 -m pip install flask>=2.3.0
fi

cd "$SERVER_DIR"
echo "[INFO] 启动服务器...（Ctrl+C 退出）"
echo "[INFO] 访问：http://localhost:$PORT"
echo "=========================================="
python3 app.py
