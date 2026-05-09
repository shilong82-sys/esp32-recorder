#!/usr/bin/env python3
"""
mock_upload_test.py - Mock 测试链路脚本

功能：
1. 自动生成测试 WAV 文件（440Hz 正弦波）
2. 自动 POST 到 FastAPI 服务器
3. 自动触发 Whisper 转写
4. 自动生成 transcript
5. 输出测试报告

使用：
    python3 scripts/mock_upload_test.py [--host 192.168.31.185] [--port 8000] [--count 3]
"""

import argparse
import http.client
import json
import os
import struct
import sys
import time
from pathlib import Path

# ── 默认参数 ──────────────────────────────────────────
DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 8000
DEFAULT_COUNT = 1
WAV_DURATION_SEC = 3
WAV_SAMPLE_RATE = 16000
WAV_FILENAME = "mock_test_rec.wav"


# ── WAV 生成 ──────────────────────────────────────────
def generate_wav(path: str, duration_sec: int = WAV_DURATION_SEC) -> int:
    """
    生成 440Hz 正弦波 WAV 文件（16bit PCM，mono，16kHz）
    返回文件大小（bytes）
    """
    import math
    sample_rate = WAV_SAMPLE_RATE
    bits = 16
    channels = 1
    num_samples = sample_rate * duration_sec
    freq = 440  # A4 音高

    # 生成正弦波
    data = b""
    for i in range(num_samples):
        value = int(32767 * math.sin(2 * math.pi * freq * i / sample_rate))
        data += struct.pack("<h", value)

    data_size = len(data)
    file_size = 36 + data_size

    # RIFF header
    wav = b"RIFF"
    wav += struct.pack("<I", file_size)
    wav += b"WAVEfmt "
    wav += struct.pack("<I", 16)
    wav += struct.pack("<H", 1)
    wav += struct.pack("<H", channels)
    wav += struct.pack("<I", sample_rate)
    wav += struct.pack("<I", sample_rate * channels * bits // 8)
    wav += struct.pack("<H", channels * bits // 8)
    wav += struct.pack("<H", bits)
    wav += b"data"
    wav += struct.pack("<I", data_size)
    wav += data

    with open(path, "wb") as f:
        f.write(wav)

    print(f"  [生成] {path} ({data_size} bytes PCM)")
    return data_size + 8


# ── HTTP 上传 ─────────────────────────────────────────
def upload_wav(host: str, port: int, filepath: str) -> tuple:
    """
    上传 WAV 文件到服务器
    返回 (HTTP状态码, 耗时秒, 文件名)
    """
    conn = http.client.HTTPConnection(host, port, timeout=30)
    boundary = b"----FormBoundaryMockTest"
    filename = os.path.basename(filepath)

    with open(filepath, "rb") as f:
        file_data = f.read()

    body = b""
    body += b"--" + boundary + b"\r\n"
    body += b'Content-Disposition: form-data; name="file"; filename="' + filename.encode() + b'"\r\n'
    body += b"Content-Type: audio/wav\r\n\r\n"
    body += file_data
    body += b"\r\n--" + boundary + b"--\r\n"

    headers = {
        "Content-Type": "multipart/form-data; boundary=" + boundary.decode(),
        "Content-Length": str(len(body)),
    }

    start = time.time()
    conn.request("POST", "/upload", body=body, headers=headers)
    resp = conn.getresponse()
    elapsed = time.time() - start
    status = resp.status
    body_text = resp.read().decode("utf-8", errors="replace")
    conn.close()

    # 解析返回的文件名
    saved_name = filename
    try:
        result = json.loads(body_text)
        if result.get("status") == "success":
            saved_name = result.get("filename", filename)
    except Exception:
        pass

    print(f"  [上传] HTTP {status}，耗时 {elapsed:.2f}s，文件={saved_name}")
    return status, elapsed, saved_name


# ── Whisper 转写（HTTP 触发）────────────────────────
def trigger_whisper(host: str, port: int, filename: str) -> tuple:
    """
    通过 HTTP 触发服务端 Whisper 转写
    返回 (transcript文本, 耗时秒, 状态)
    """
    conn = http.client.HTTPConnection(host, port, timeout=60)
    url = f"/whisper?filename={filename}"

    start = time.time()
    conn.request("POST", url)
    resp = conn.getresponse()
    elapsed = time.time() - start
    body_text = resp.read().decode("utf-8", errors="replace")
    conn.close()

    transcript = ""
    if resp.status == 200:
        try:
            result = json.loads(body_text)
            transcript = result.get("transcript", "")
        except Exception:
            transcript = body_text
    else:
        transcript = f"[ERROR] HTTP {resp.status}: {body_text[:200]}"

    print(f"  [转写] HTTP {resp.status}，耗时 {elapsed:.2f}s")
    return transcript, elapsed, resp.status


# ── 主流程 ────────────────────────────────────────────
def run_test(host: str, port: int, count: int) -> list:
    results = []
    script_dir = Path(__file__).parent
    wav_path = script_dir / WAV_FILENAME

    print("=" * 50)
    print("  Mock 测试链路")
    print(f"  目标：{host}:{port}")
    print(f"  次数：{count}")
    print("=" * 50)

    for i in range(1, count + 1):
        print(f"\n── 第 {i}/{count} 次测试 ──────────────────────")

        # 1. 生成 WAV
        t0 = time.time()
        file_size = generate_wav(str(wav_path))
        gen_elapsed = time.time() - t0

        # 2. 上传
        upload_status, upload_elapsed, saved_name = upload_wav(host, port, str(wav_path))

        # 3. 触发 Whisper（如果上传成功）
        transcript = ""
        whisper_elapsed = 0.0
        whisper_status = 0
        if upload_status == 200 and saved_name:
            transcript, whisper_elapsed, whisper_status = trigger_whisper(host, port, saved_name)
        else:
            transcript = f"[跳过] 上传失败，HTTP {upload_status}"

        # 4. 记录结果
        result = {
            "index": i,
            "file_size": file_size,
            "gen_elapsed": round(gen_elapsed, 3),
            "upload_status": upload_status,
            "upload_elapsed": round(upload_elapsed, 3),
            "whisper_status": whisper_status,
            "whisper_elapsed": round(whisper_elapsed, 3),
            "transcript": transcript[:200],
        }
        results.append(result)

        print(f"  [结果] 文件={file_size}B，上传={upload_elapsed:.2f}s，转写={whisper_elapsed:.2f}s")
        print(f"  [transcript] {transcript[:100]}")

        # 清理
        if wav_path.exists():
            wav_path.unlink()

    return results


# ── 输出报告 ─────────────────────────────────────────
def print_report(results: list):
    print("\n" + "=" * 50)
    print("  测试报告")
    print("=" * 50)

    total = len(results)
    ok = sum(1 for r in results if r["upload_status"] == 200 and r["whisper_status"] == 200)
    avg_upload = sum(r["upload_elapsed"] for r in results) / total if total else 0
    avg_whisper = sum(r["whisper_elapsed"] for r in results if r["whisper_status"] == 200) / ok if ok else 0

    print(f"总次数：{total}")
    print(f"成功次数：{ok}")
    print(f"成功率：{ok/total*100:.1f}%")
    print(f"平均上传耗时：{avg_upload:.2f}s")
    if ok:
        print(f"平均转写耗时：{avg_whisper:.2f}s")

    print("\n── 详细信息 ──────────────────────────────")
    for r in results:
        print(f"  #{r['index']}: HTTP={r['upload_status']}/{r['whisper_status']}，"
              f"上传={r['upload_elapsed']:.2f}s，"
              f"转写={r['whisper_elapsed']:.2f}s")
        if r["transcript"]:
            print(f"    transcript: {r['transcript'][:80]}")


# ── 入口 ──────────────────────────────────────────────
def main():
    parser = argparse.ArgumentParser(description="Mock 测试链路")
    parser.add_argument("--host", default=DEFAULT_HOST, help="服务器 IP")
    parser.add_argument("--port", default=DEFAULT_PORT, type=int, help="服务器端口")
    parser.add_argument("--count", default=DEFAULT_COUNT, type=int, help="测试次数")
    args = parser.parse_args()

    results = run_test(args.host, args.port, args.count)
    print_report(results)


if __name__ == "__main__":
    main()
