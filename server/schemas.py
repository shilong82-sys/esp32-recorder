"""ESP32 AI Recorder — Pydantic 请求/响应 Schema。

统一响应格式：{code: int, message: str, data: Any}
"""

from datetime import datetime
from enum import Enum
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

class SegmentItem(BaseModel):
    """转写时间戳分段条目。"""

    start: float
    end: float
    text: str


class SpeakerItem(BaseModel):
    """说话人信息条目。"""

    id: str
    name: str
    segment_indices: List[int] = []


class SpeakersUpdateRequest(BaseModel):
    """说话人更新请求体。"""

    speakers: List[SpeakerItem]


class TranscriptItem(BaseModel):
    """转写记录详情。"""

    model_config = ConfigDict(from_attributes=True)

    id: int
    file_id: int
    status: str
    text: Optional[str] = None
    segments: Optional[str] = None
    speakers: Optional[str] = None
    model: Optional[str] = None
    language: Optional[str] = None
    duration: Optional[float] = None
    is_edited: int = 0
    edited_at: Optional[datetime] = None
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
    segments: Optional[str] = None
    speakers: Optional[str] = None
    model: Optional[str] = None
    language: Optional[str] = None
    duration: Optional[float] = None
    is_edited: int = 0
    edited_at: Optional[datetime] = None
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


class TranscriptEditRequest(BaseModel):
    """转写编辑请求体。"""

    text: str


# ---------------------------------------------------------------------------
# 导出格式
# ---------------------------------------------------------------------------

class ExportFormat(str, Enum):
    """导出格式枚举。"""

    txt = "txt"
    srt = "srt"
    vtt = "vtt"


# ---------------------------------------------------------------------------
# 批量操作
# ---------------------------------------------------------------------------

class BatchDeleteRequest(BaseModel):
    """批量删除请求体。"""

    file_ids: List[int]


class BatchTranscribeRequest(BaseModel):
    """批量转写请求体。"""

    file_ids: List[int]
    model: Optional[str] = None


# ---------------------------------------------------------------------------
# 标签相关
# ---------------------------------------------------------------------------

class TagItem(BaseModel):
    """标签条目。"""

    id: int
    name: str
    color: str
    created_at: Optional[datetime] = None


class TagCreateRequest(BaseModel):
    """创建标签请求体。"""

    name: str
    color: Optional[str] = None


class FileTagRequest(BaseModel):
    """文件标签关联请求体。"""

    tag_ids: List[int]


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
    duration: Optional[float] = None
    created_at: datetime
    transcription: Optional[TranscriptItem] = None
    tags: Optional[List[TagItem]] = None


class FileListItem(BaseModel):
    """文件列表条目（含关联转写摘要）。"""

    model_config = ConfigDict(from_attributes=True)

    id: int
    filename: str
    saved_name: str
    file_size: int
    upload_time: datetime
    upload_src: str
    duration: Optional[float] = None
    created_at: datetime
    transcription: Optional[TranscriptListItem] = None
    tags: Optional[List[TagItem]] = None


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
# 搜索相关
# ---------------------------------------------------------------------------

class SearchResultItem(BaseModel):
    """搜索结果条目。"""

    file_id: int
    filename: str
    upload_time: Optional[str] = None
    duration: Optional[float] = None
    snippet: str


# ---------------------------------------------------------------------------
# 认证相关
# ---------------------------------------------------------------------------

class LoginRequest(BaseModel):
    """登录请求体。"""

    password: str


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
    UNAUTHORIZED = 40100
    FORBIDDEN = 40300
    NOT_FOUND = 40400
    CONFLICT = 40900
    INTERNAL_ERROR = 50000
    FILE_NOT_FOUND = 40401
    TRANSCRIPTION_NOT_FOUND = 40402
    TRANSCRIPTION_IN_PROGRESS = 40901
    TAG_NOT_FOUND = 40403
    TAG_ALREADY_EXISTS = 40902
