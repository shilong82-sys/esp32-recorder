"""ESP32 AI Recorder — SQLAlchemy ORM 模型。

对应数据库表 files 和 transcriptions。
"""

from datetime import datetime, timezone

from sqlalchemy import (
    Column,
    DateTime,
    Float,
    ForeignKey,
    Integer,
    String,
    Text,
)
from sqlalchemy.orm import relationship

from .database import Base


def _utcnow() -> datetime:
    """返回当前 UTC 时间。"""
    return datetime.now(timezone.utc)


class File(Base):
    """上传文件记录。

    Attributes:
        id: 主键。
        filename: 原始文件名。
        saved_name: 磁盘上的文件名（唯一）。
        file_size: 文件大小（字节）。
        upload_time: 上传时间（UTC）。
        upload_src: 上传来源 IP。
        created_at: 记录创建时间。
        transcription: 关联的转写记录（1:1）。
    """

    __tablename__ = "files"

    id = Column(Integer, primary_key=True, autoincrement=True)
    filename = Column(String, nullable=False, comment="原始文件名")
    saved_name = Column(String, nullable=False, unique=True, comment="磁盘文件名")
    file_size = Column(Integer, nullable=False, comment="字节")
    upload_time = Column(DateTime, nullable=False, comment="上传时间 UTC")
    upload_src = Column(String, default="unknown", comment="上传来源 IP")
    created_at = Column(DateTime, default=_utcnow, comment="记录创建时间")

    transcription = relationship(
        "Transcription",
        back_populates="file",
        uselist=False,
        cascade="all, delete-orphan",
        passive_deletes=True,
    )

    def __repr__(self) -> str:
        return f"<File id={self.id} filename={self.filename!r}>"


class Transcription(Base):
    """转写记录。

    Attributes:
        id: 主键。
        file_id: 关联文件 ID（外键，级联删除）。
        status: 转写状态（pending/processing/completed/failed）。
        text: 转写文本。
        model: 使用的模型名称。
        language: 检测到的语言。
        duration: 音频时长（秒）。
        error_msg: 失败原因。
        started_at: 开始转写时间。
        completed_at: 转写完成时间。
        created_at: 记录创建时间。
        file: 关联的文件记录。
    """

    __tablename__ = "transcriptions"

    id = Column(Integer, primary_key=True, autoincrement=True)
    file_id = Column(
        Integer,
        ForeignKey("files.id", ondelete="CASCADE"),
        nullable=False,
        comment="关联文件 ID",
    )
    status = Column(
        String,
        nullable=False,
        default="pending",
        comment="pending/processing/completed/failed",
    )
    text = Column(Text, nullable=True, comment="转写文本")
    model = Column(String, nullable=True, comment="模型名称")
    language = Column(String, nullable=True, comment="检测到的语言")
    duration = Column(Float, nullable=True, comment="音频时长（秒）")
    error_msg = Column(Text, nullable=True, comment="失败原因")
    started_at = Column(DateTime, nullable=True, comment="开始转写时间")
    completed_at = Column(DateTime, nullable=True, comment="转写完成时间")
    created_at = Column(DateTime, default=_utcnow, comment="记录创建时间")

    file = relationship("File", back_populates="transcription")

    def __repr__(self) -> str:
        return f"<Transcription id={self.id} file_id={self.file_id} status={self.status!r}>"
