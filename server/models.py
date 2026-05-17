"""ESP32 AI Recorder — SQLAlchemy ORM 模型。

对应数据库表 files、transcriptions 和 settings。
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
        duration: 音频时长（秒），从 WAV header 读取。
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
    duration = Column(Float, nullable=True, comment="音频时长（秒）")
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
        is_edited: 是否被手动编辑过（0=原始转写，1=已编辑）。
        edited_at: 最后一次手动编辑的时间戳（UTC）。
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
    is_edited = Column(Integer, default=0, comment="是否被手动编辑过（0/1）")
    edited_at = Column(DateTime, nullable=True, comment="最后一次手动编辑时间 UTC")
    error_msg = Column(Text, nullable=True, comment="失败原因")
    started_at = Column(DateTime, nullable=True, comment="开始转写时间")
    completed_at = Column(DateTime, nullable=True, comment="转写完成时间")
    created_at = Column(DateTime, default=_utcnow, comment="记录创建时间")

    file = relationship("File", back_populates="transcription")

    def __repr__(self) -> str:
        return f"<Transcription id={self.id} file_id={self.file_id} status={self.status!r}>"


class Setting(Base):
    """运行时设置（key-value 表）。

    Attributes:
        key: 设置项名称（主键）。
        value: 设置项值（字符串形式）。
        updated_at: 最后更新时间。
    """

    __tablename__ = "settings"

    key = Column(String, primary_key=True, comment="设置项名称")
    value = Column(String, nullable=False, comment="设置项值")
    updated_at = Column(
        DateTime, default=_utcnow, onupdate=_utcnow, comment="最后更新时间"
    )

    def __repr__(self) -> str:
        return f"<Setting key={self.key!r} value={self.value!r}>"
