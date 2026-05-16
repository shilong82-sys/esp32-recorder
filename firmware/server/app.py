#!/usr/bin/env python3
"""
ESP32 Recorder - Mac 接收服务器
接收 ESP32 通过 HTTP POST 上传的 WAV 文件，并触发 Whisper 转写

用法：
    python3 app.py
    默认监听 0.0.0.0:8000，保存文件到 ./received/
"""

from flask import Flask, request, jsonify
import os
from datetime import datetime

app = Flask(__name__)

# 接收文件保存目录
SAVE_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "received")
os.makedirs(SAVE_DIR, exist_ok=True)

# transcript 保存目录
TRANSCRIPT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "transcripts")
os.makedirs(TRANSCRIPT_DIR, exist_ok=True)

# 允许的最大文件大小：100MB（RAW BODY 上传，支持大文件）
app.config['MAX_CONTENT_LENGTH'] = 100 * 1024 * 1024


@app.route('/')
def index():
    """首页：显示接收状态和已接收文件列表"""
    files = []
    total_size = 0
    for fname in sorted(os.listdir(SAVE_DIR)):
        fpath = os.path.join(SAVE_DIR, fname)
        if os.path.isfile(fpath):
            size = os.path.getsize(fpath)
            total_size += size
            files.append({
                "name": fname,
                "size_kb": round(size / 1024, 1),
                "time": datetime.fromtimestamp(os.path.getmtime(fpath)).strftime("%Y-%m-%d %H:%M:%S")
            })

    html = f"""
    <!DOCTYPE html>
    <html>
    <head>
        <meta charset="UTF-8">
        <title>ESP32 Recorder 接收服务器</title>
        <style>
            body {{ font-family: monospace; max-width: 800px; margin: 40px auto; padding: 20px; }}
            h1 {{ color: #333; }}
            .status {{ background: #e8f5e9; padding: 15px; border-radius: 8px; margin-bottom: 20px; }}
            .file-list {{ background: #f5f5f5; padding: 15px; border-radius: 8px; }}
            .file {{ padding: 8px; border-bottom: 1px solid #ddd; }}
            .file:last-child {{ border-bottom: none; }}
            .meta {{ color: #666; font-size: 0.9em; }}
            .clear-btn {{ background: #f44336; color: white; border: none;
                          padding: 8px 16px; border-radius: 4px; cursor: pointer; margin-top: 10px; }}
        </style>
    </head>
    <body>
        <h1>🎙️ ESP32 Recorder 接收服务器</h1>
        <div class="status">
            ✅ 服务器运行中<br>
            📡 监听地址：<strong>{request.host}</strong><br>
            💾 保存目录：<strong>{SAVE_DIR}</strong><br>
            📊 已接收 <strong>{len(files)}</strong> 个文件，
            共 <strong>{round(total_size / 1024, 1)} KB</strong>
        </div>
        <div class="file-list">
            <h3>📁 已接收文件</h3>
            {"".join(
                f'<div class="file">📄 <strong>{f["name"]}</strong>'
                f'<span class="meta"> — {f["size_kb"]} KB — {f["time"]}</span></div>'
                for f in files
            ) if files else "<p>暂无文件，等待 ESP32 上传...</p>"}
        </div>
    </body>
    </html>
    """
    return html


@app.route('/upload', methods=['POST'])
def upload():
    """
    接收 ESP32 上传的 WAV 文件（RAW BODY，流式接收）
    不使用 request.get_data()（会一次性读入内存导致超时）
    直接读取 request.stream 流式写入文件
    """
    # 生成文件名
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    save_name = f"{timestamp}_upload.wav"
    save_path = os.path.join(SAVE_DIR, save_name)

    # 流式读取 request body（不加载到内存）
    bytes_received = 0
    try:
        with open(save_path, 'wb') as f:
            # Flask 的 request.stream 是原始 WSGI input
            # 使用 8KB chunk 读取，避免内存占用
            chunk_size = 8192
            while True:
                chunk = request.stream.read(chunk_size)
                if not chunk:
                    break
                f.write(chunk)
                bytes_received += len(chunk)

        file_size = os.path.getsize(save_path)
        print(f"✅ 接收文件成功：{save_name} （{file_size} bytes）")

        return jsonify({
            "status": "success",
            "filename": save_name,
            "size_bytes": file_size
        }), 200

    except Exception as e:
        print(f"❌ 接收文件失败：{e}")
        # 删除不完整的文件
        if os.path.exists(save_path):
            os.remove(save_path)
        return jsonify({"status": "error", "message": str(e)}), 500


@app.route('/whisper', methods=['POST'])
def whisper_transcribe():
    """
    触发 Whisper 转写
    参数（query string）：filename=已上传的文件名
    返回：{ "status": "success", "transcript": "..." }
    """
    filename = request.args.get('filename', '')
    if not filename:
        return jsonify({"status": "error", "message": "Missing filename parameter"}), 400

    wav_path = os.path.join(SAVE_DIR, filename)
    if not os.path.isfile(wav_path):
        return jsonify({"status": "error", "message": f"File not found: {filename}"}), 404

    print(f"🎙️ 开始转写：{filename}")

    # 尝试调用 Whisper（需要 mlx-whisper 环境）
    transcript = ""
    elapsed = 0.0
    try:
        import time
        start = time.time()

        # 尝试导入 mlx-whisper（在正确的 Python 环境中）
        try:
            import subprocess
            result = subprocess.run(
                ["python3", "-c",
                 f"import mlx_whisper; "
                 f"result = mlx_whisper.transcribe('{wav_path}', language='en'); "
                 f"print(result['text'])"],
                capture_output=True, text=True, timeout=60
            )
            if result.returncode == 0:
                transcript = result.stdout.strip()
            else:
                transcript = f"[Whisper Error] {result.stderr[:200]}"
        except Exception as e:
            transcript = f"[Whisper Failed] {str(e)}"

        elapsed = time.time() - start

    except Exception as e:
        return jsonify({"status": "error", "message": str(e)}), 500

    # 保存 transcript
    transcript_filename = filename.replace('.wav', '_transcript.txt')
    transcript_path = os.path.join(TRANSCRIPT_DIR, transcript_filename)
    with open(transcript_path, 'w') as f:
        f.write(transcript)

    print(f"✅ 转写完成：{transcript_filename} （耗时 {elapsed:.2f}s）")

    return jsonify({
        "status": "success",
        "filename": filename,
        "transcript": transcript,
        "transcript_file": transcript_filename,
        "elapsed_sec": round(elapsed, 2)
    }), 200


@app.route('/files', methods=['GET'])
def list_files():
    """API：返回已接收文件列表（JSON格式）"""
    files = []
    for fname in sorted(os.listdir(SAVE_DIR)):
        fpath = os.path.join(SAVE_DIR, fname)
        if os.path.isfile(fpath):
            files.append({
                "name": fname,
                "size_bytes": os.path.getsize(fpath),
                "modified": os.path.getmtime(fpath)
            })
    return jsonify({"count": len(files), "files": files})


@app.route('/health', methods=['GET'])
def health():
    """健康检查接口"""
    return jsonify({"status": "ok", "save_dir": SAVE_DIR}), 200


if __name__ == '__main__':
    print("=" * 60)
    print("  ESP32 Recorder - Mac 接收服务器")
    print("=" * 60)
    print(f"  保存目录：{SAVE_DIR}")
    print(f"  Transcript 目录：{TRANSCRIPT_DIR}")
    print(f"  访问首页：http://localhost:8000")
    print(f"  上传接口：http://localhost:8000/upload")
    print(f"  转写接口：http://localhost:8000/whisper?filename=xxx.wav")
    print(f"  健康检查：http://localhost:8000/health")
    print("=" * 60)
    print("  等待 ESP32 连接...")
    print()

    app.run(host='0.0.0.0', port=8000, debug=False)
