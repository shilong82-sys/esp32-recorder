"""ESP32 AI Recorder — 转写管理路由。

GET  /api/transcripts                — 转写列表
GET  /api/transcripts/{file_id}      — 转写详情（含文本）
GET  /api/transcripts/{file_id}/export — 导出 .txt
POST /api/transcribe/{file_id}       — 手动触发转写
PUT  /api/transcripts/{file_id}      — 编辑转写文本
"""

import logging
import os
from datetime import datetime, timezone
from typing import Optional

from fastapi import APIRouter, Depends, HTTPException, Query
from fastapi.responses import PlainTextResponse
from sqlalchemy import func, select
from sqlalchemy.ext.asyncio import AsyncSession
from sqlalchemy.orm import selectinload

from ..config import get_config
from ..database import get_session
from ..models import File, Transcription
from ..schemas import (
    ApiResponse,
    ErrorCode,
    TranscriptEditRequest,
    TranscriptItem,
    TranscriptListData,
    TranscriptListItem,
)
from ..services.transcriber import enqueue

logger = logging.getLogger(__name__)

router = APIRouter(prefix="/api")


# ---------------------------------------------------------------------------
# 辅助函数
# ---------------------------------------------------------------------------

def _transcription_to_list_item(t: Transcription) -> TranscriptListItem:
    """将 ORM Transcription 对象转换为列表条目 schema。"""
    return TranscriptListItem(
        id=t.id,
        file_id=t.file_id,
        status=t.status,
        model=t.model,
        language=t.language,
        duration=t.duration,
        is_edited=t.is_edited,
        edited_at=t.edited_at,
        error_msg=t.error_msg,
        started_at=t.started_at,
        completed_at=t.completed_at,
        created_at=t.created_at,
    )


def _transcription_to_detail(t: Transcription) -> TranscriptItem:
    """将 ORM Transcription 对象转换为详情 schema。"""
    return TranscriptItem(
        id=t.id,
        file_id=t.file_id,
        status=t.status,
        text=t.text,
        model=t.model,
        language=t.language,
        duration=t.duration,
        is_edited=t.is_edited,
        edited_at=t.edited_at,
        error_msg=t.error_msg,
        started_at=t.started_at,
        completed_at=t.completed_at,
        created_at=t.created_at,
    )


# ---------------------------------------------------------------------------
# 路由
# ---------------------------------------------------------------------------

@router.get("/transcripts", response_model=ApiResponse)
async def list_transcripts(
    page: int = Query(1, ge=1, description="页码"),
    page_size: int = Query(20, ge=1, le=100, description="每页条数"),
    status: Optional[str] = Query(
        None, description="转写状态过滤: pending/processing/completed/failed"
    ),
    session: AsyncSession = Depends(get_session),
) -> ApiResponse:
    """获取转写列表。"""
    # 基础查询
    query = select(Transcription)
    count_query = select(func.count(Transcription.id))

    # 状态过滤
    if status is not None:
        query = query.where(Transcription.status == status)
        count_query = count_query.where(Transcription.status == status)

    # 总数
    total_result = await session.execute(count_query)
    total = total_result.scalar() or 0

    # 分页 + 排序（最新在前）
    offset = (page - 1) * page_size
    query = query.order_by(Transcription.created_at.desc()).offset(offset).limit(page_size)

    result = await session.execute(query)
    db_transcripts = result.scalars().all()

    items = [_transcription_to_list_item(t) for t in db_transcripts]

    return ApiResponse(
        data=TranscriptListData(
            items=items,
            total=total,
            page=page,
            page_size=page_size,
        ).model_dump(),
    )


@router.get("/transcripts/{file_id}", response_model=ApiResponse)
async def get_transcript(
    file_id: int,
    session: AsyncSession = Depends(get_session),
) -> ApiResponse:
    """获取指定文件的转写详情（含文本）。"""
    query = select(Transcription).where(Transcription.file_id == file_id)
    result = await session.execute(query)
    db_transcript = result.scalar_one_or_none()

    if db_transcript is None:
        return ApiResponse(
            code=ErrorCode.TRANSCRIPTION_NOT_FOUND,
            message="Transcription not found for this file",
            data=None,
        )

    return ApiResponse(data=_transcription_to_detail(db_transcript).model_dump())


@router.get("/transcripts/{file_id}/export")
async def export_transcript(
    file_id: int,
    session: AsyncSession = Depends(get_session),
) -> PlainTextResponse:
    """导出转写文本为 .txt 文件。"""
    query = select(Transcription).where(Transcription.file_id == file_id)
    result = await session.execute(query)
    db_transcript = result.scalar_one_or_none()

    if db_transcript is None:
        raise HTTPException(status_code=404, detail="Transcription not found")

    if db_transcript.text is None:
        raise HTTPException(status_code=404, detail="Transcription text not available")

    # 获取文件名用于导出
    file_query = select(File).where(File.id == file_id)
    file_result = await session.execute(file_query)
    db_file = file_result.scalar_one_or_none()
    export_filename = f"{db_file.filename}.txt" if db_file else f"transcript_{file_id}.txt"

    return PlainTextResponse(
        content=db_transcript.text,
        media_type="text/plain; charset=utf-8",
        headers={
            "Content-Disposition": f'attachment; filename="{export_filename}"',
        },
    )


@router.post("/transcribe/{file_id}", response_model=ApiResponse)
async def trigger_transcribe(
    file_id: int,
    session: AsyncSession = Depends(get_session),
) -> ApiResponse:
    """手动触发转写。

    如果文件没有转写记录，创建 pending 记录。
    如果已有记录且状态为 terminal（completed/failed），重置为 pending 重新转写。
    如果正在转写中（pending/processing），返回冲突错误。
    """
    # 检查文件是否存在
    file_query = select(File).where(File.id == file_id)
    file_result = await session.execute(file_query)
    db_file = file_result.scalar_one_or_none()

    if db_file is None:
        return ApiResponse(
            code=ErrorCode.FILE_NOT_FOUND,
            message="File not found",
            data=None,
        )

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
        logger.info("Created transcription record for file_id=%d", file_id)
    elif db_transcript.status in ("completed", "failed"):
        # 重置为 pending 以重新转写
        old_status = db_transcript.status
        db_transcript.status = "pending"
        db_transcript.text = None
        db_transcript.error_msg = None
        db_transcript.started_at = None
        db_transcript.completed_at = None
        # 重置编辑标记
        db_transcript.is_edited = 0
        db_transcript.edited_at = None
        logger.info(
            "Reset transcription %d for file_id=%d (was %s)",
            db_transcript.id, file_id, old_status,
        )
    else:
        # 正在转写中
        return ApiResponse(
            code=ErrorCode.TRANSCRIPTION_IN_PROGRESS,
            message=f"Transcription already in progress (status: {db_transcript.status})",
            data=None,
        )

    # 入队转写
    await enqueue(file_id)

    return ApiResponse(
        data={
            "file_id": file_id,
            "transcription_id": db_transcript.id,
            "status": db_transcript.status,
        },
    )


@router.put("/transcripts/{file_id}", response_model=ApiResponse)
async def edit_transcript(
    file_id: int,
    request: TranscriptEditRequest,
    session: AsyncSession = Depends(get_session),
) -> ApiResponse:
    """编辑转写文本。

    更新 text 字段，设置 is_edited=1 和 edited_at 为当前时间。

    Args:
        file_id: 文件 ID。
        request: 编辑请求体（含新文本）。
        session: 数据库会话。

    Returns:
        更新后的转写详情。
    """
    query = select(Transcription).where(Transcription.file_id == file_id)
    result = await session.execute(query)
    db_transcript = result.scalar_one_or_none()

    if db_transcript is None:
        return ApiResponse(
            code=ErrorCode.TRANSCRIPTION_NOT_FOUND,
            message="Transcription not found for this file",
            data=None,
        )

    if db_transcript.status != "completed":
        return ApiResponse(
            code=ErrorCode.BAD_REQUEST,
            message="Can only edit completed transcriptions",
            data=None,
        )

    # 更新文本和编辑标记
    db_transcript.text = request.text
    db_transcript.is_edited = 1
    db_transcript.edited_at = datetime.now(timezone.utc)

    # 同步写入文本文件
    config = get_config()
    txt_filename = os.path.splitext(
        (await session.execute(
            select(File.saved_name).where(File.id == file_id)
        )).scalar_one_or_none() or f"file_{file_id}"
    )[0] + ".txt"
    txt_path = os.path.join(config.transcripts_dir, txt_filename)

    try:
        os.makedirs(config.transcripts_dir, exist_ok=True)
        with open(txt_path, "w", encoding="utf-8") as f:
            f.write(request.text)
    except OSError as exc:
        logger.warning("Failed to update transcript file %s: %s", txt_path, exc)

    logger.info("Transcript edited: file_id=%d", file_id)

    return ApiResponse(data=_transcription_to_detail(db_transcript).model_dump())
