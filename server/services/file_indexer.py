"""ESP32 AI Recorder — 启动时文件索引服务。

扫描 received/ 目录，将尚未入库的 WAV 文件写入 files 和 transcriptions 表。
"""

from __future__ import annotations

import logging
import os
from datetime import datetime, timezone

from sqlalchemy import select

from ..config import get_config
from ..models import File, Transcription
from ..services.wav_utils import read_wav_duration

logger = logging.getLogger(__name__)


async def index_received_dir() -> int:
    """扫描 received/ 目录，索引未入库的 WAV 文件。

    Returns:
        新索引的文件数量。
    """
    config = get_config()

    # 确保目录存在
    os.makedirs(config.received_dir, exist_ok=True)

    # 扫描目录
    wav_files: list[str] = []
    try:
        for entry in os.listdir(config.received_dir):
            if entry.lower().endswith(".wav"):
                wav_files.append(entry)
    except OSError as exc:
        logger.error("Failed to scan received/ directory: %s", exc)
        return 0

    if not wav_files:
        logger.info("No WAV files found in received/")
        return 0

    logger.info("Found %d WAV files in received/", len(wav_files))

    # 获取已入库的 saved_name 集合
    from ..database import _async_session_factory

    if _async_session_factory is None:
        logger.warning("Database not initialized, skipping file indexing")
        return 0

    indexed_count = 0

    async with _async_session_factory() as session:
        # 获取已入库的文件名
        result = await session.execute(select(File.saved_name))
        existing_names = {row[0] for row in result.all()}

        for filename in sorted(wav_files):
            if filename in existing_names:
                continue

            # 获取文件大小
            file_path = os.path.join(config.received_dir, filename)
            try:
                file_size = os.path.getsize(file_path)
            except OSError:
                logger.warning("Cannot stat file: %s, skipping", filename)
                continue

            # 获取文件修改时间作为上传时间
            try:
                mtime = os.path.getmtime(file_path)
                upload_time = datetime.fromtimestamp(mtime, tz=timezone.utc)
            except OSError:
                upload_time = datetime.now(timezone.utc)

            # 从 WAV header 读取时长
            duration = read_wav_duration(file_path)

            # 创建文件记录
            db_file = File(
                filename=filename,
                saved_name=filename,
                file_size=file_size,
                upload_time=upload_time,
                upload_src="indexed",
                duration=duration,
            )
            session.add(db_file)
            await session.flush()

            # 创建待转写记录
            db_transcription = Transcription(
                file_id=db_file.id,
                status="pending",
            )
            session.add(db_transcription)

            indexed_count += 1
            logger.info(
                "Indexed: %s (%d bytes, duration=%s, id=%d)",
                filename, file_size,
                f"{duration:.1f}s" if duration else "unknown",
                db_file.id,
            )

        await session.commit()

    logger.info("File indexing complete: %d new files indexed", indexed_count)
    return indexed_count
