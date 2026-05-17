"""ESP32 AI Recorder — 文件管理路由。

GET    /api/files              — 文件列表（分页+排序+过滤+标签筛选）
GET    /api/files/{file_id}    — 文件详情
DELETE /api/files/{file_id}    — 删除文件及关联转写
GET    /api/files/{file_id}/download  — 下载 WAV 文件
GET    /api/files/{file_id}/stream   — 流式播放 WAV（支持 HTTP Range 请求）
"""

import logging
import os
from datetime import datetime, timedelta
from typing import Optional

from fastapi import APIRouter, Depends, HTTPException, Query, Request
from fastapi.responses import FileResponse, StreamingResponse
from sqlalchemy import func, select
from sqlalchemy.ext.asyncio import AsyncSession
from sqlalchemy.orm import selectinload

from ..config import get_config
from ..database import get_session
from ..models import File, FileTag, Tag, Transcription
from ..schemas import (
    ApiResponse,
    ErrorCode,
    FileItem,
    FileListData,
    FileListItem,
    TagItem,
)

logger = logging.getLogger(__name__)

router = APIRouter(prefix="/api")

# Range 请求的默认块大小（1 MB）
CHUNK_SIZE = 1024 * 1024


# ---------------------------------------------------------------------------
# 辅助函数
# ---------------------------------------------------------------------------

def _build_tag_items(db_file: File) -> Optional[list[TagItem]]:
    """从 File ORM 对象提取标签列表。"""
    if not db_file.tags:
        return None
    return [
        TagItem(
            id=t.id,
            name=t.name,
            color=t.color,
            created_at=t.created_at,
        )
        for t in db_file.tags
    ]


def _file_to_item(db_file: File) -> FileListItem:
    """将 ORM File 对象转换为 FileListItem schema。"""
    transcription = None
    if db_file.transcription is not None:
        t = db_file.transcription
        transcription = {
            "id": t.id,
            "file_id": t.file_id,
            "status": t.status,
            "segments": t.segments,
            "speakers": t.speakers,
            "model": t.model,
            "language": t.language,
            "duration": t.duration,
            "is_edited": t.is_edited,
            "edited_at": t.edited_at,
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
        duration=db_file.duration,
        created_at=db_file.created_at,
        transcription=transcription,
        tags=_build_tag_items(db_file),
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
            "segments": t.segments,
            "speakers": t.speakers,
            "model": t.model,
            "language": t.language,
            "duration": t.duration,
            "is_edited": t.is_edited,
            "edited_at": t.edited_at,
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
        duration=db_file.duration,
        created_at=db_file.created_at,
        transcription=transcription,
        tags=_build_tag_items(db_file),
    )


def _parse_range(range_header: str, file_size: int) -> tuple[int, int]:
    """解析 HTTP Range 头。

    Args:
        range_header: Range 头值，如 "bytes=0-1023" 或 "bytes=0-"。
        file_size: 文件总大小。

    Returns:
        (start, end) 元组，end 为包含的结束位置。
    """
    # 格式: bytes=start-end 或 bytes=start-
    range_spec = range_header.replace("bytes=", "")
    parts = range_spec.split("-", 1)

    try:
        start = int(parts[0]) if parts[0] else 0
        end = int(parts[1]) if parts[1] else file_size - 1
    except (ValueError, IndexError):
        start = 0
        end = file_size - 1

    # 边界检查
    start = max(0, min(start, file_size - 1))
    end = max(start, min(end, file_size - 1))

    return start, end


def _file_chunk_iterator(file_path: str, start: int, end: int, chunk_size: int):
    """生成文件块的迭代器（用于 StreamingResponse）。

    Args:
        file_path: 文件路径。
        start: 起始字节位置。
        end: 结束字节位置（包含）。
        chunk_size: 每次读取的块大小。
    """
    with open(file_path, "rb") as f:
        f.seek(start)
        remaining = end - start + 1
        while remaining > 0:
            read_size = min(chunk_size, remaining)
            data = f.read(read_size)
            if not data:
                break
            remaining -= len(data)
            yield data


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
    tag_id: Optional[int] = Query(None, description="按标签 ID 筛选"),
    session: AsyncSession = Depends(get_session),
) -> ApiResponse:
    """获取文件列表（分页、排序、过滤、标签筛选）。"""
    # 基础查询
    query = select(File).options(selectinload(File.transcription), selectinload(File.tags))
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

    # 按标签筛选
    if tag_id is not None:
        query = query.join(FileTag, File.id == FileTag.file_id).where(
            FileTag.tag_id == tag_id
        )
        count_query = count_query.join(FileTag, File.id == FileTag.file_id).where(
            FileTag.tag_id == tag_id
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
        selectinload(File.transcription),
        selectinload(File.tags),
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
    """删除文件及关联转写记录和标签关联。"""
    config = get_config()

    query = select(File).where(File.id == file_id).options(
        selectinload(File.transcription),
        selectinload(File.tags),
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

    # 删除数据库记录（级联删除 transcription 和 file_tags）
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


@router.get("/files/{file_id}/stream")
async def stream_file(
    file_id: int,
    request: Request,
    session: AsyncSession = Depends(get_session),
) -> StreamingResponse:
    """流式返回 WAV 文件，支持 HTTP Range 请求。

    用于浏览器 <audio> 标签内嵌播放。
    """
    config = get_config()

    query = select(File).where(File.id == file_id)
    result = await session.execute(query)
    db_file = result.scalar_one_or_none()

    if db_file is None:
        raise HTTPException(status_code=404, detail="File not found")

    file_path = os.path.join(config.received_dir, db_file.saved_name)
    if not os.path.exists(file_path):
        raise HTTPException(status_code=404, detail="File not found on disk")

    file_size = os.path.getsize(file_path)

    # 解析 Range 头
    range_header = request.headers.get("range")

    if range_header:
        start, end = _parse_range(range_header, file_size)
        content_length = end - start + 1

        headers = {
            "Content-Range": f"bytes {start}-{end}/{file_size}",
            "Accept-Ranges": "bytes",
            "Content-Length": str(content_length),
            "Content-Type": "audio/wav",
        }

        return StreamingResponse(
            _file_chunk_iterator(file_path, start, end, CHUNK_SIZE),
            status_code=206,
            headers=headers,
            media_type="audio/wav",
        )
    else:
        # 无 Range 头，返回完整文件
        headers = {
            "Accept-Ranges": "bytes",
            "Content-Length": str(file_size),
            "Content-Type": "audio/wav",
        }

        return StreamingResponse(
            _file_chunk_iterator(file_path, 0, file_size - 1, CHUNK_SIZE),
            status_code=200,
            headers=headers,
            media_type="audio/wav",
        )
