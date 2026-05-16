"""ESP32 AI Recorder — 上传路由。

POST /upload — RAW BODY 流式接收 WAV 文件，兼容现有固件。
"""

import logging
import os
from datetime import datetime, timezone

from fastapi import APIRouter, Depends, Request
from sqlalchemy.ext.asyncio import AsyncSession

from ..config import get_config
from ..database import get_session
from ..models import File, Transcription
from ..schemas import ApiResponse, ErrorCode, UploadResponseData
from ..services.transcriber import enqueue

logger = logging.getLogger(__name__)

router = APIRouter()


def _resolve_filename(request: Request) -> str:
    """从请求中解析文件名。

    优先级：X-Filename header > filename query param > 自动生成时间戳文件名。

    Args:
        request: FastAPI Request 对象。

    Returns:
        解析出的文件名。
    """
    # 优先级 1: X-Filename header
    filename = request.headers.get("X-Filename")
    if filename:
        return filename.strip()

    # 优先级 2: filename query parameter
    filename = request.query_params.get("filename")
    if filename:
        return filename.strip()

    # 优先级 3: 自动生成
    timestamp = datetime.now(timezone.utc).strftime("%Y%m%d_%H%M%S")
    return f"REC_{timestamp}.wav"


def _resolve_conflict_filename(filename: str, directory: str) -> str:
    """解决文件名冲突：检测重名自动追加 _1, _2 后缀。

    Args:
        filename: 原始文件名。
        directory: 目标目录。

    Returns:
        不冲突的文件名。
    """
    target_path = os.path.join(directory, filename)
    if not os.path.exists(target_path):
        return filename

    # 分离文件名和扩展名
    base_name, ext = os.path.splitext(filename)
    counter = 1
    while True:
        new_filename = f"{base_name}_{counter}{ext}"
        new_path = os.path.join(directory, new_filename)
        if not os.path.exists(new_path):
            logger.info("Filename conflict resolved: %s -> %s", filename, new_filename)
            return new_filename
        counter += 1


@router.post("/upload", response_model=ApiResponse)
async def handle_upload(
    request: Request,
    session: AsyncSession = Depends(get_session),
) -> ApiResponse:
    """RAW BODY 接收 WAV 文件。

    ESP32 使用 HTTP POST 发送 raw WAV binary（Content-Type: audio/wav）。
    流式写入磁盘，不占用大量内存。
    """
    config = get_config()

    # 确保存储目录存在
    os.makedirs(config.received_dir, exist_ok=True)

    # 解析文件名
    filename = _resolve_filename(request)
    saved_name = _resolve_conflict_filename(filename, config.received_dir)

    # 流式写入文件
    file_path = os.path.join(config.received_dir, saved_name)
    file_size = 0
    max_bytes = config.max_file_size_mb * 1024 * 1024

    try:
        with open(file_path, "wb") as f:
            async for chunk in request.stream():
                file_size += len(chunk)
                if file_size > max_bytes:
                    # 超出大小限制，删除已写入的部分
                    f.close()
                    os.remove(file_path)
                    logger.warning(
                        "Upload rejected: file too large (%d > %d bytes)",
                        file_size, max_bytes,
                    )
                    return ApiResponse(
                        code=ErrorCode.BAD_REQUEST,
                        message=f"File too large (max {config.max_file_size_mb}MB)",
                        data=None,
                    )
                f.write(chunk)
    except Exception as exc:
        # 写入失败，清理临时文件
        if os.path.exists(file_path):
            os.remove(file_path)
        logger.error("Failed to save uploaded file: %s", exc)
        return ApiResponse(
            code=ErrorCode.INTERNAL_ERROR,
            message="Failed to save file",
            data=None,
        )

    # 获取客户端 IP
    client_ip = "unknown"
    if request.client is not None:
        client_ip = request.client.host

    # 创建数据库记录
    upload_time = datetime.now(timezone.utc)
    db_file = File(
        filename=filename,
        saved_name=saved_name,
        file_size=file_size,
        upload_time=upload_time,
        upload_src=client_ip,
    )
    session.add(db_file)
    await session.flush()  # 获取 db_file.id

    # 创建待转写记录
    db_transcription = Transcription(
        file_id=db_file.id,
        status="pending",
    )
    session.add(db_transcription)
    await session.flush()

    # 触发转写入队（占位实现，T03 才有实际 worker）
    await enqueue(db_file.id)

    logger.info(
        "File received: %s (%d bytes, src=%s) -> id=%d",
        saved_name, file_size, client_ip, db_file.id,
    )

    return ApiResponse(
        data=UploadResponseData(
            file_id=db_file.id,
            filename=filename,
            saved_name=saved_name,
            file_size=file_size,
        ).model_dump(),
    )
