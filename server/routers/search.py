"""ESP32 AI Recorder — 搜索路由。

GET /api/search?q=keyword&limit=20 — 全文搜索转写文本（SQLite LIKE）。
"""

import logging
from typing import Optional

from fastapi import APIRouter, Depends, Query
from sqlalchemy import select
from sqlalchemy.ext.asyncio import AsyncSession

from ..database import get_session
from ..models import File, Transcription
from ..schemas import ApiResponse

logger = logging.getLogger(__name__)

router = APIRouter(prefix="/api")


@router.get("/search", response_model=ApiResponse)
async def search_transcripts(
    q: str = Query("", description="搜索关键词"),
    limit: int = Query(20, ge=1, le=100, description="最大返回数量"),
    session: AsyncSession = Depends(get_session),
) -> ApiResponse:
    """全文搜索转写文本（SQLite LIKE %keyword%）。

    搜索已完成的转写记录，返回匹配文件信息及文本片段（snippet）。
    snippet 中用 **keyword** 标记匹配词，前端可替换为 <mark> 标签。

    Args:
        q: 搜索关键词。为空时返回空列表。
        limit: 最大返回数量，默认 20，最大 100。
        session: 数据库会话。

    Returns:
        搜索结果列表，每项含 file_id、filename、upload_time、duration、snippet。
    """
    if not q or not q.strip():
        return ApiResponse(data=[])

    keyword = q.strip()

    # 查询：JOIN files 获取文件信息，LIKE 搜索转写文本
    query = (
        select(File, Transcription)
        .join(Transcription, File.id == Transcription.file_id)
        .where(Transcription.text.like(f"%{keyword}%"))
        .where(Transcription.status == "completed")
        .order_by(File.upload_time.desc())
        .limit(limit)
    )

    result = await session.execute(query)
    rows = result.all()

    items = []
    for db_file, db_trans in rows:
        # 生成 snippet：匹配词前后各 50 字符
        snippet = _make_snippet(db_trans.text, keyword)

        items.append({
            "file_id": db_file.id,
            "filename": db_file.filename,
            "upload_time": db_file.upload_time.isoformat() if db_file.upload_time else None,
            "duration": db_file.duration,
            "snippet": snippet,
        })

    return ApiResponse(data=items)


def _make_snippet(text: Optional[str], keyword: str, context_chars: int = 50) -> str:
    """生成搜索结果摘要片段。

    在匹配词前后各取 context_chars 个字符，用 **keyword** 标记匹配词。

    Args:
        text: 完整文本。
        keyword: 搜索关键词。
        context_chars: 匹配词前后的上下文字符数。

    Returns:
        包含高亮标记的摘要片段。
    """
    if not text:
        return ""

    # 查找关键词位置（不区分大小写）
    lower_text = text.lower()
    lower_keyword = keyword.lower()
    pos = lower_text.find(lower_keyword)

    if pos == -1:
        # 理论上不应该发生（查询条件保证存在），做 fallback
        return text[:context_chars * 2] + ("..." if len(text) > context_chars * 2 else "")

    # 计算 snippet 起止位置
    snippet_start = max(0, pos - context_chars)
    snippet_end = min(len(text), pos + len(keyword) + context_chars)

    snippet = text[snippet_start:snippet_end]

    # 添加省略号
    prefix = "..." if snippet_start > 0 else ""
    suffix = "..." if snippet_end < len(text) else ""

    # 在 snippet 中标记匹配词
    # 需要在 snippet 内找到匹配词的位置（考虑大小写）
    local_pos = snippet.lower().find(lower_keyword)
    if local_pos != -1:
        highlighted = (
            snippet[:local_pos]
            + "**" + snippet[local_pos:local_pos + len(keyword)] + "**"
            + snippet[local_pos + len(keyword):]
        )
    else:
        highlighted = snippet

    return prefix + highlighted + suffix
