"""ESP32 AI Recorder — 系统状态与健康检查路由。

GET /api/status — 系统状态（磁盘、文件数、转写统计）
GET /health     — 健康检查
"""

import logging
import shutil

from fastapi import APIRouter, Depends
from sqlalchemy import func, select
from sqlalchemy.ext.asyncio import AsyncSession

from ..database import get_session
from ..models import File, Transcription
from ..schemas import ApiResponse, StatusData, TranscriptionStats

logger = logging.getLogger(__name__)

router = APIRouter()


@router.get("/health")
async def health_check() -> dict:
    """健康检查端点。"""
    return {"status": "ok"}


@router.get("/api/status", response_model=ApiResponse)
async def get_status(
    session: AsyncSession = Depends(get_session),
) -> ApiResponse:
    """获取系统状态：磁盘使用、文件数、转写统计。"""
    # 磁盘使用
    disk_usage = shutil.disk_usage("/")
    disk_total = disk_usage.total
    disk_used = disk_usage.used
    disk_free = disk_usage.free

    # 文件数
    file_count_result = await session.execute(select(func.count(File.id)))
    file_count = file_count_result.scalar() or 0

    # 转写统计
    total_result = await session.execute(select(func.count(Transcription.id)))
    total = total_result.scalar() or 0

    pending_result = await session.execute(
        select(func.count(Transcription.id)).where(Transcription.status == "pending")
    )
    pending = pending_result.scalar() or 0

    processing_result = await session.execute(
        select(func.count(Transcription.id)).where(Transcription.status == "processing")
    )
    processing = processing_result.scalar() or 0

    completed_result = await session.execute(
        select(func.count(Transcription.id)).where(Transcription.status == "completed")
    )
    completed = completed_result.scalar() or 0

    failed_result = await session.execute(
        select(func.count(Transcription.id)).where(Transcription.status == "failed")
    )
    failed = failed_result.scalar() or 0

    stats = TranscriptionStats(
        total=total,
        pending=pending,
        processing=processing,
        completed=completed,
        failed=failed,
    )

    status_data = StatusData(
        disk_total_bytes=disk_total,
        disk_used_bytes=disk_used,
        disk_free_bytes=disk_free,
        file_count=file_count,
        transcription_stats=stats,
    )

    return ApiResponse(data=status_data.model_dump())
