"""ESP32 AI Recorder — 标签路由。

GET    /api/tags                    — 获取所有标签
POST   /api/tags                    — 创建标签
DELETE /api/tags/{tag_id}           — 删除标签
POST   /api/files/{file_id}/tags    — 为文件添加标签
DELETE /api/files/{file_id}/tags    — 移除文件标签
"""

import logging
from typing import Optional

from fastapi import APIRouter, Depends
from sqlalchemy import delete, select
from sqlalchemy.ext.asyncio import AsyncSession

from ..database import get_session
from ..models import File, FileTag, Tag
from ..schemas import ApiResponse, ErrorCode, FileTagRequest, TagCreateRequest, TagItem

logger = logging.getLogger(__name__)

router = APIRouter(prefix="/api")


def _tag_to_item(t: Tag) -> TagItem:
    """将 ORM Tag 对象转换为 TagItem schema。"""
    return TagItem(
        id=t.id,
        name=t.name,
        color=t.color,
        created_at=t.created_at,
    )


@router.get("/tags", response_model=ApiResponse)
async def list_tags(
    session: AsyncSession = Depends(get_session),
) -> ApiResponse:
    """获取所有标签。"""
    result = await session.execute(select(Tag).order_by(Tag.created_at.asc()))
    tags = result.scalars().all()
    items = [_tag_to_item(t) for t in tags]
    return ApiResponse(data=items)


@router.post("/tags", response_model=ApiResponse)
async def create_tag(
    request: TagCreateRequest,
    session: AsyncSession = Depends(get_session),
) -> ApiResponse:
    """创建标签。

    Args:
        request: 标签创建请求体。

    Returns:
        创建的标签信息。
    """
    # 检查名称是否已存在
    existing = await session.execute(
        select(Tag).where(Tag.name == request.name)
    )
    if existing.scalar_one_or_none() is not None:
        return ApiResponse(
            code=ErrorCode.TAG_ALREADY_EXISTS,
            message=f"Tag '{request.name}' already exists",
            data=None,
        )

    tag = Tag(
        name=request.name,
        color=request.color or "#6366f1",
    )
    session.add(tag)
    await session.flush()

    logger.info("Created tag: id=%d name=%s", tag.id, tag.name)

    return ApiResponse(data=_tag_to_item(tag).model_dump())


@router.delete("/tags/{tag_id}", response_model=ApiResponse)
async def delete_tag(
    tag_id: int,
    session: AsyncSession = Depends(get_session),
) -> ApiResponse:
    """删除标签。"""
    result = await session.execute(select(Tag).where(Tag.id == tag_id))
    tag = result.scalar_one_or_none()

    if tag is None:
        return ApiResponse(
            code=ErrorCode.TAG_NOT_FOUND,
            message="Tag not found",
            data=None,
        )

    # 删除关联的 file_tags 记录（由 CASCADE 自动处理）
    await session.delete(tag)
    logger.info("Deleted tag: id=%d name=%s", tag_id, tag.name)

    return ApiResponse(data={"id": tag_id, "name": tag.name})


@router.post("/files/{file_id}/tags", response_model=ApiResponse)
async def add_file_tags(
    file_id: int,
    request: FileTagRequest,
    session: AsyncSession = Depends(get_session),
) -> ApiResponse:
    """为文件添加标签。

    Args:
        file_id: 文件 ID。
        request: 标签关联请求体。

    Returns:
        操作结果。
    """
    # 检查文件是否存在
    file_result = await session.execute(select(File).where(File.id == file_id))
    if file_result.scalar_one_or_none() is None:
        return ApiResponse(
            code=ErrorCode.FILE_NOT_FOUND,
            message="File not found",
            data=None,
        )

    added_ids: list[int] = []
    for tag_id in request.tag_ids:
        # 检查标签是否存在
        tag_result = await session.execute(select(Tag).where(Tag.id == tag_id))
        if tag_result.scalar_one_or_none() is None:
            continue

        # 检查是否已关联
        existing = await session.execute(
            select(FileTag).where(
                FileTag.file_id == file_id,
                FileTag.tag_id == tag_id,
            )
        )
        if existing.scalar_one_or_none() is not None:
            continue

        file_tag = FileTag(file_id=file_id, tag_id=tag_id)
        session.add(file_tag)
        added_ids.append(tag_id)

    logger.info("Added tags to file_id=%d: %s", file_id, added_ids)

    return ApiResponse(data={"file_id": file_id, "added_tag_ids": added_ids})


@router.delete("/files/{file_id}/tags", response_model=ApiResponse)
async def remove_file_tags(
    file_id: int,
    request: FileTagRequest,
    session: AsyncSession = Depends(get_session),
) -> ApiResponse:
    """移除文件标签。

    Args:
        file_id: 文件 ID。
        request: 标签关联请求体。

    Returns:
        操作结果。
    """
    removed_ids: list[int] = []
    for tag_id in request.tag_ids:
        result = await session.execute(
            select(FileTag).where(
                FileTag.file_id == file_id,
                FileTag.tag_id == tag_id,
            )
        )
        file_tag = result.scalar_one_or_none()
        if file_tag is not None:
            await session.delete(file_tag)
            removed_ids.append(tag_id)

    logger.info("Removed tags from file_id=%d: %s", file_id, removed_ids)

    return ApiResponse(data={"file_id": file_id, "removed_tag_ids": removed_ids})
