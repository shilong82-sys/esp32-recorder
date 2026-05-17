"""ESP32 AI Recorder — 批量操作路由。

POST /api/files/batch-delete — 批量删除文件
POST /api/transcribe/batch   — 批量触发转写
"""

import logging
import os

from fastapi import APIRouter, Depends
from sqlalchemy import select
from sqlalchemy.ext.asyncio import AsyncSession
from sqlalchemy.orm import selectinload

from ..config import get_config
from ..database import get_session
from ..models import File, Transcription
from ..schemas import ApiResponse, BatchDeleteRequest, BatchTranscribeRequest
from ..services.transcriber import get_transcriber

logger = logging.getLogger(__name__)

router = APIRouter(prefix="/api")


@router.post("/files/batch-delete", response_model=ApiResponse)
async def batch_delete_files(
    request: BatchDeleteRequest,
    session: AsyncSession = Depends(get_session),
) -> ApiResponse:
    """批量删除文件及其关联数据。

    Args:
        request: 批量删除请求体，包含 file_ids 列表。

    Returns:
        删除结果。
    """
    config = get_config()
    deleted_ids: list[int] = []
    failed_ids: list[int] = []

    for file_id in request.file_ids:
        query = select(File).where(File.id == file_id).options(
            selectinload(File.transcription),
            selectinload(File.tags),
        )
        result = await session.execute(query)
        db_file = result.scalar_one_or_none()

        if db_file is None:
            failed_ids.append(file_id)
            continue

        # 删除磁盘文件
        file_path = os.path.join(config.received_dir, db_file.saved_name)
        if os.path.exists(file_path):
            try:
                os.remove(file_path)
            except OSError as exc:
                logger.warning("Failed to delete file %s: %s", file_path, exc)

        # 删除关联的转写文本文件
        if db_file.transcription is not None:
            txt_filename = os.path.splitext(db_file.saved_name)[0] + ".txt"
            txt_path = os.path.join(config.transcripts_dir, txt_filename)
            if os.path.exists(txt_path):
                try:
                    os.remove(txt_path)
                except OSError as exc:
                    logger.warning("Failed to delete transcript %s: %s", txt_path, exc)

        # 删除数据库记录（级联删除）
        await session.delete(db_file)
        deleted_ids.append(file_id)

    logger.info(
        "Batch delete: %d deleted, %d not found",
        len(deleted_ids), len(failed_ids),
    )

    return ApiResponse(
        data={
            "deleted_count": len(deleted_ids),
            "deleted_ids": deleted_ids,
            "failed_ids": failed_ids,
        },
    )


@router.post("/transcribe/batch", response_model=ApiResponse)
async def batch_transcribe(
    request: BatchTranscribeRequest,
    session: AsyncSession = Depends(get_session),
) -> ApiResponse:
    """批量触发转写。

    Args:
        request: 批量转写请求体，包含 file_ids 列表和可选 model。

    Returns:
        触发结果。
    """
    transcriber = get_transcriber()
    queued_ids: list[int] = []
    failed_ids: list[int] = []

    for file_id in request.file_ids:
        # 检查文件是否存在
        file_query = select(File).where(File.id == file_id)
        file_result = await session.execute(file_query)
        db_file = file_result.scalar_one_or_none()

        if db_file is None:
            failed_ids.append(file_id)
            continue

        # 查找或创建转写记录
        trans_query = select(Transcription).where(Transcription.file_id == file_id)
        trans_result = await session.execute(trans_query)
        db_transcript = trans_result.scalar_one_or_none()

        if db_transcript is None:
            # 创建新的转写记录
            db_transcript = Transcription(
                file_id=file_id,
                status="pending",
            )
            session.add(db_transcript)
            await session.flush()
        elif db_transcript.status in ("completed", "failed"):
            # 重置为 pending
            db_transcript.status = "pending"
            db_transcript.text = None
            db_transcript.segments = None
            db_transcript.speakers = None
            db_transcript.error_msg = None
            db_transcript.started_at = None
            db_transcript.completed_at = None
            db_transcript.is_edited = 0
            db_transcript.edited_at = None
            if request.model:
                db_transcript.model = request.model
        elif db_transcript.status in ("pending", "processing"):
            # 已在队列中或正在处理
            queued_ids.append(file_id)
            continue

        # 入队转写
        await transcriber.enqueue(file_id, model=request.model)
        queued_ids.append(file_id)

    logger.info(
        "Batch transcribe: %d queued, %d failed",
        len(queued_ids), len(failed_ids),
    )

    return ApiResponse(
        data={
            "queued_count": len(queued_ids),
            "queued_ids": queued_ids,
            "failed_ids": failed_ids,
        },
    )
