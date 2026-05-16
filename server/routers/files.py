"""ESP32 AI Recorder — 文件管理路由。

GET    /api/files              — 文件列表（分页+排序+过滤）
GET    /api/files/{file_id}    — 文件详情
DELETE /api/files/{file_id}    — 删除文件及关联转写
GET    /api/files/{file_id}/download — 下载 WAV 文件
"""

import logging
import os
from datetime import datetime, timedelta
from typing import Optional

from fastapi import APIRouter, Depends, HTTPException, Query
from fastapi.responses import FileResponse
from sqlalchemy import func, select
from sqlalchemy.ext.asyncio import AsyncSession
from sqlalchemy.orm import selectinload

from ..config import get_config
from ..database import get_session
from ..models import File, Transcription
from ..schemas import (
    ApiResponse,
    ErrorCode,
    FileItem,
    FileListData,
    FileListItem,
)

logger = logging.getLogger(__name__)

router = APIRouter(prefix="/api")


# ---------------------------------------------------------------------------
# 辅助函数
# ---------------------------------------------------------------------------

def _file_to_item(db_file: File) -> FileListItem:
    """将 ORM File 对象转换为 FileListItem schema。"""
    transcription = None
    if db_file.transcription is not None:
        t = db_file.transcription
        transcription = {
            "id": t.id,
            "file_id": t.file_id,
            "status": t.status,
            "model": t.model,
            "language": t.language,
            "duration": t.duration,
            "error_msg": t.error_msg,
            "started_at": t.started_at,
            "completed_at": t.completed_at,
            "created_at": t.created_at,
        }
    return FileListItem(
        id=db_file.id,
        filename=db_file.filename,
        saved_name=db_file.saved_name,
        file_size=db_file.file_size,
        upload_time=db_file.upload_time,
        upload_src=db_file.upload_src,
        created_at=db_file.created_at,
        transcription=transcription,
    )


def _file_to_detail(db_file: File) -> FileItem:
    """将 ORM File 对象转换为 FileItem 详情 schema。"""
    transcription = None
    if db_file.transcription is not None:
        t = db_file.transcription
        transcription = {
            "id": t.id,
            "file_id": t.file_id,
            "status": t.status,
            "text": t.text,
            "model": t.model,
            "language": t.language,
            "duration": t.duration,
            "error_msg": t.error_msg,
            "started_at": t.started_at,
            "completed_at": t.completed_at,
            "created_at": t.created_at,
        }
    return FileItem(
        id=db_file.id,
        filename=db_file.filename,
        saved_name=db_file.saved_name,
        file_size=db_file.file_size,
        upload_time=db_file.upload_time,
        upload_src=db_file.upload_src,
        created_at=db_file.created_at,
        transcription=transcription,
    )


# ---------------------------------------------------------------------------
# 路由
# ---------------------------------------------------------------------------

@router.get("/files", response_model=ApiResponse)
async def list_files(
    page: int = Query(1, ge=1, description="页码"),
    page_size: int = Query(20, ge=1, le=100, description="每页条数"),
    sort: str = Query("upload_time", description="排序字段"),
    order: str = Query("desc", description="排序方向 asc/desc"),
    date_from: Optional[str] = Query(None, description="起始日期 YYYY-MM-DD"),
    date_to: Optional[str] = Query(None, description="结束日期 YYYY-MM-DD"),
    min_size: Optional[int] = Query(None, ge=0, description="最小文件大小（字节）"),
    max_size: Optional[int] = Query(None, ge=0, description="最大文件大小（字节）"),
    transcription_status: Optional[str] = Query(
        None, description="转写状态过滤: pending/processing/completed/failed"
    ),
    session: AsyncSession = Depends(get_session),
) -> ApiResponse:
    """获取文件列表（分页、排序、过滤）。"""
    # 基础查询
    query = select(File).options(selectinload(File.transcription))
    count_query = select(func.count(File.id))

    # 过滤条件
    if date_from is not None:
        try:
            dt_from = datetime.strptime(date_from, "%Y-%m-%d")
            query = query.where(File.upload_time >= dt_from)
            count_query = count_query.where(File.upload_time >= dt_from)
        except ValueError:
            pass

    if date_to is not None:
        try:
            dt_to = datetime.strptime(date_to, "%Y-%m-%d")
            # 包含当天，所以加一天
            dt_to_end = dt_to + timedelta(days=1)
            query = query.where(File.upload_time < dt_to_end)
            count_query = count_query.where(File.upload_time < dt_to_end)
        except ValueError:
            pass

    if min_size is not None:
        query = query.where(File.file_size >= min_size)
        count_query = count_query.where(File.file_size >= min_size)

    if max_size is not None:
        query = query.where(File.file_size <= max_size)
        count_query = count_query.where(File.file_size <= max_size)

    if transcription_status is not None:
        query = query.join(File.transcription).where(
            Transcription.status == transcription_status
        )
        count_query = count_query.join(File.transcription).where(
            Transcription.status == transcription_status
        )

    # 排序
    sort_column = File.upload_time  # 默认
    if sort == "file_size":
        sort_column = File.file_size
    elif sort == "filename":
        sort_column = File.filename
    elif sort == "created_at":
        sort_column = File.created_at
    # else: upload_time (default)

    if order.lower() == "asc":
        query = query.order_by(sort_column.asc())
    else:
        query = query.order_by(sort_column.desc())

    # 总数
    total_result = await session.execute(count_query)
    total = total_result.scalar() or 0

    # 分页
    offset = (page - 1) * page_size
    query = query.offset(offset).limit(page_size)

    result = await session.execute(query)
    db_files = result.scalars().all()

    items = [_file_to_item(f) for f in db_files]

    return ApiResponse(
        data=FileListData(
            items=items,
            total=total,
            page=page,
            page_size=page_size,
        ).model_dump(),
    )


@router.get("/files/{file_id}", response_model=ApiResponse)
async def get_file(
    file_id: int,
    session: AsyncSession = Depends(get_session),
) -> ApiResponse:
    """获取文件详情。"""
    query = select(File).where(File.id == file_id).options(
        selectinload(File.transcription)
    )
    result = await session.execute(query)
    db_file = result.scalar_one_or_none()

    if db_file is None:
        return ApiResponse(
            code=ErrorCode.FILE_NOT_FOUND,
            message="File not found",
            data=None,
        )

    return ApiResponse(data=_file_to_detail(db_file).model_dump())


@router.delete("/files/{file_id}", response_model=ApiResponse)
async def delete_file(
    file_id: int,
    session: AsyncSession = Depends(get_session),
) -> ApiResponse:
    """删除文件及关联转写记录。"""
    config = get_config()

    query = select(File).where(File.id == file_id).options(
        selectinload(File.transcription)
    )
    result = await session.execute(query)
    db_file = result.scalar_one_or_none()

    if db_file is None:
        return ApiResponse(
            code=ErrorCode.FILE_NOT_FOUND,
            message="File not found",
            data=None,
        )

    # 删除磁盘文件
    file_path = os.path.join(config.received_dir, db_file.saved_name)
    if os.path.exists(file_path):
        try:
            os.remove(file_path)
            logger.info("Deleted file: %s", file_path)
        except OSError as exc:
            logger.warning("Failed to delete file %s: %s", file_path, exc)

    # 删除关联的转写文本文件（如果存在）
    if db_file.transcription is not None:
        txt_filename = os.path.splitext(db_file.saved_name)[0] + ".txt"
        txt_path = os.path.join(config.transcripts_dir, txt_filename)
        if os.path.exists(txt_path):
            try:
                os.remove(txt_path)
                logger.info("Deleted transcript: %s", txt_path)
            except OSError as exc:
                logger.warning("Failed to delete transcript %s: %s", txt_path, exc)

    # 删除数据库记录（级联删除 transcription）
    await session.delete(db_file)

    logger.info("Deleted file record: id=%d filename=%s", file_id, db_file.filename)

    return ApiResponse(
        data={"id": file_id, "filename": db_file.filename},
    )


@router.get("/files/{file_id}/download")
async def download_file(
    file_id: int,
    session: AsyncSession = Depends(get_session),
) -> FileResponse:
    """下载 WAV 文件。"""
    config = get_config()

    query = select(File).where(File.id == file_id)
    result = await session.execute(query)
    db_file = result.scalar_one_or_none()

    if db_file is None:
        raise HTTPException(status_code=404, detail="File not found")

    file_path = os.path.join(config.received_dir, db_file.saved_name)
    if not os.path.exists(file_path):
        raise HTTPException(status_code=404, detail="File not found on disk")

    return FileResponse(
        path=file_path,
        media_type="audio/wav",
        filename=db_file.filename,
    )
