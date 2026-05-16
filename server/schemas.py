"""ESP32 AI Recorder — Pydantic 请求/响应 Schema。

统一响应格式：{code: int, message: str, data: Any}
"""

from datetime import datetime
from typing import Any, List, Optional

from pydantic import BaseModel, ConfigDict, Field


# ---------------------------------------------------------------------------
# 统一响应
# ---------------------------------------------------------------------------

class ApiResponse(BaseModel):
    """统一 API 响应格式。

    Attributes:
        code: 状态码，0=成功，非0=错误。
        message: 状态描述。
        data: 响应数据。
    """

    code: int = 0
    message: str = "success"
    data: Optional[Any] = None


# ---------------------------------------------------------------------------
# 转写相关
# ---------------------------------------------------------------------------

class TranscriptItem(BaseModel):
    """转写记录详情。"""

    model_config = ConfigDict(from_attributes=True)

    id: int
    file_id: int
    status: str
    text: Optional[str] = None
    model: Optional[str] = None
    language: Optional[str] = None
    duration: Optional[float] = None
    error_msg: Optional[str] = None
    started_at: Optional[datetime] = None
    completed_at: Optional[datetime] = None
    created_at: datetime


class TranscriptListItem(BaseModel):
    """转写列表中的条目（不含长文本）。"""

    model_config = ConfigDict(from_attributes=True)

    id: int
    file_id: int
    status: str
    model: Optional[str] = None
    language: Optional[str] = None
    duration: Optional[float] = None
    error_msg: Optional[str] = None
    started_at: Optional[datetime] = None
    completed_at: Optional[datetime] = None
    created_at: datetime


class TranscriptListData(BaseModel):
    """转写列表响应数据。"""

    items: List[TranscriptListItem]
    total: int
    page: int
    page_size: int


# ---------------------------------------------------------------------------
# 文件相关
# ---------------------------------------------------------------------------

class FileItem(BaseModel):
    """文件详情（含关联转写）。"""

    model_config = ConfigDict(from_attributes=True)

    id: int
    filename: str
    saved_name: str
    file_size: int
    upload_time: datetime
    upload_src: str
    created_at: datetime
    transcription: Optional[TranscriptItem] = None


class FileListItem(BaseModel):
    """文件列表条目（含关联转写摘要）。"""

    model_config = ConfigDict(from_attributes=True)

    id: int
    filename: str
    saved_name: str
    file_size: int
    upload_time: datetime
    upload_src: str
    created_at: datetime
    transcription: Optional[TranscriptListItem] = None


class FileListData(BaseModel):
    """文件列表响应数据。"""

    items: List[FileListItem]
    total: int
    page: int
    page_size: int


class UploadResponseData(BaseModel):
    """上传成功响应数据。"""

    file_id: int
    filename: str
    saved_name: str
    file_size: int


# ---------------------------------------------------------------------------
# 系统状态
# ---------------------------------------------------------------------------

class TranscriptionStats(BaseModel):
    """转写统计。"""

    total: int = 0
    pending: int = 0
    processing: int = 0
    completed: int = 0
    failed: int = 0


class StatusData(BaseModel):
    """系统状态。"""

    disk_total_bytes: int
    disk_used_bytes: int
    disk_free_bytes: int
    file_count: int
    transcription_stats: TranscriptionStats


# ---------------------------------------------------------------------------
# 错误码
# ---------------------------------------------------------------------------

class ErrorCode:
    """统一错误码常量。"""

    SUCCESS = 0
    BAD_REQUEST = 40000
    NOT_FOUND = 40400
    CONFLICT = 40900
    INTERNAL_ERROR = 50000
    FILE_NOT_FOUND = 40401
    TRANSCRIPTION_NOT_FOUND = 40402
    TRANSCRIPTION_IN_PROGRESS = 40901
