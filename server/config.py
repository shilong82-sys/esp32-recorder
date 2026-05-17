"""ESP32 AI Recorder — 服务端配置管理。

所有可配置项集中管理，支持环境变量覆盖。
"""

from __future__ import annotations

import os
from dataclasses import dataclass, field
from typing import Optional


@dataclass
class AppConfig:
    """服务端全局配置。

    Attributes:
        host: 监听地址。
        port: 监听端口。
        received_dir: 上传文件存储目录。
        transcripts_dir: 转写文本存储目录。
        db_path: SQLite 数据库文件路径。
        whisper_model: mlx-whisper 模型名称。
        max_file_size_mb: 上传文件最大允许大小（MB）。
        transcribe_timeout_s: 转写超时时间（秒）。
    """

    host: str = field(default_factory=lambda: os.getenv("RECORDER_HOST", "0.0.0.0"))
    port: int = field(default_factory=lambda: int(os.getenv("RECORDER_PORT", "8000")))
    received_dir: str = field(default="")
    transcripts_dir: str = field(default="")
    db_path: str = field(default="")
    whisper_model: str = field(
        default_factory=lambda: os.getenv(
            "RECORDER_WHISPER_MODEL",
            "mlx-community/whisper-large-v3-turbo",
        )
    )
    max_file_size_mb: int = field(
        default_factory=lambda: int(os.getenv("RECORDER_MAX_FILE_SIZE_MB", "100"))
    )
    transcribe_timeout_s: int = field(
        default_factory=lambda: int(os.getenv("RECORDER_TRANSCRIBE_TIMEOUT_S", "600"))
    )

    auth_password: str = field(
        default_factory=lambda: os.getenv("RECORDER_AUTH_PASSWORD", "changeme")
    )
    auth_enabled: bool = field(
        default_factory=lambda: os.getenv("RECORDER_AUTH_ENABLED", "true").lower()
        not in ("false", "0", "no", "off")
    )
    session_secret: str = field(
        default_factory=lambda: os.getenv(
            "RECORDER_SESSION_SECRET", "esp32-recorder-secret-key"
        )
    )

    def __post_init__(self) -> None:
        """补全依赖 base_dir 的默认路径。"""
        base_dir = os.path.dirname(os.path.abspath(__file__))
        if not self.received_dir:
            self.received_dir = os.getenv(
                "RECORDER_RECEIVED_DIR", os.path.join(base_dir, "received")
            )
        if not self.transcripts_dir:
            self.transcripts_dir = os.getenv(
                "RECORDER_TRANSCRIPTS_DIR", os.path.join(base_dir, "transcripts")
            )
        if not self.db_path:
            self.db_path = os.getenv(
                "RECORDER_DB_PATH", os.path.join(base_dir, "recorder.db")
            )


# 模块级单例
_config: Optional[AppConfig] = None


def get_config() -> AppConfig:
    """获取全局配置单例。

    Returns:
        AppConfig 实例。
    """
    global _config
    if _config is None:
        _config = AppConfig()
    return _config
