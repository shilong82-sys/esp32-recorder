"""ESP32 AI Recorder — 自动清理服务。

asyncio 定时任务，根据 cleanup_days 设置清理超期文件。
提供手动触发接口和状态查询。
"""

from __future__ import annotations

import asyncio
import logging
import os
from datetime import datetime, timedelta, timezone
from typing import Optional

from sqlalchemy import delete, select

from ..config import get_config
from ..models import File, Transcription
from ..services.settings_service import get_setting

logger = logging.getLogger(__name__)


class CleanupService:
    """自动清理服务。

    每 24 小时执行一次清理，删除超过 cleanup_days 天的文件。

    Attributes:
        _task: 后台定时任务。
        _stop_event: 停止信号。
        _last_cleanup: 上次清理时间。
        _next_cleanup: 下次清理时间。
    """

    def __init__(self) -> None:
        self._task: Optional[asyncio.Task] = None
        self._stop_event = asyncio.Event()
        self._last_cleanup: Optional[datetime] = None
        self._next_cleanup: Optional[datetime] = None

    async def start(self) -> None:
        """启动清理定时任务。"""
        self._next_cleanup = datetime.now(timezone.utc) + timedelta(hours=24)
        self._task = asyncio.create_task(self._run_loop())
        logger.info("CleanupService started (next cleanup at %s)", self._next_cleanup)

    async def stop(self) -> None:
        """停止清理定时任务。"""
        self._stop_event.set()
        if self._task is not None:
            self._task.cancel()
            try:
                await self._task
            except asyncio.CancelledError:
                pass
        logger.info("CleanupService stopped")

    async def _run_loop(self) -> None:
        """定时循环：每 24 小时执行一次清理。"""
        while not self._stop_event.is_set():
            try:
                # 等待 24 小时（或直到收到停止信号）
                try:
                    await asyncio.wait_for(
                        self._stop_event.wait(), timeout=86400
                    )
                    # 如果 stop_event 被设置，退出
                    return
                except asyncio.TimeoutError:
                    pass

                if not self._stop_event.is_set():
                    await self._do_cleanup()

            except asyncio.CancelledError:
                logger.info("CleanupService loop cancelled")
                break
            except Exception as exc:
                logger.error("Unexpected error in cleanup loop: %s", exc)

    async def _do_cleanup(self) -> None:
        """执行一次清理。"""
        from ..database import _async_session_factory

        if _async_session_factory is None:
            return

        try:
            days = int(await get_setting("cleanup_days", "90"))
        except (ValueError, TypeError):
            days = 90

        cutoff = datetime.now(timezone.utc) - timedelta(days=days)
        config = get_config()
        deleted_count = 0

        async with _async_session_factory() as session:
            result = await session.execute(
                select(File).where(File.created_at < cutoff)
            )
            old_files = result.scalars().all()

            for db_file in old_files:
                # 删除磁盘文件
                file_path = os.path.join(config.received_dir, db_file.saved_name)
                if os.path.exists(file_path):
                    try:
                        os.remove(file_path)
                    except OSError as exc:
                        logger.warning("Failed to delete file %s: %s", file_path, exc)

                # 删除关联的转写文本文件
                txt_filename = os.path.splitext(db_file.saved_name)[0] + ".txt"
                txt_path = os.path.join(config.transcripts_dir, txt_filename)
                if os.path.exists(txt_path):
                    try:
                        os.remove(txt_path)
                    except OSError as exc:
                        logger.warning("Failed to delete transcript %s: %s", txt_path, exc)

                # 删除数据库记录
                await session.delete(db_file)
                deleted_count += 1

            if deleted_count > 0:
                await session.commit()

        self._last_cleanup = datetime.now(timezone.utc)
        self._next_cleanup = self._last_cleanup + timedelta(hours=24)

        logger.info(
            "Cleanup completed: %d files deleted (cutoff=%s, days=%d)",
            deleted_count, cutoff.isoformat(), days,
        )

    async def run_now(self) -> dict:
        """手动触发清理。

        Returns:
            清理结果信息。
        """
        await self._do_cleanup()
        return await self.get_status()

    async def get_status(self) -> dict:
        """获取清理服务状态。

        Returns:
            状态信息字典。
        """
        try:
            days = int(await get_setting("cleanup_days", "90"))
        except (ValueError, TypeError):
            days = 90

        return {
            "cleanup_days": days,
            "last_cleanup": self._last_cleanup.isoformat() if self._last_cleanup else None,
            "next_cleanup": self._next_cleanup.isoformat() if self._next_cleanup else None,
        }


# 模块级单例
_cleanup_service: Optional[CleanupService] = None


def get_cleanup_service() -> CleanupService:
    """获取清理服务单例。

    Returns:
        CleanupService 实例。
    """
    global _cleanup_service
    if _cleanup_service is None:
        _cleanup_service = CleanupService()
    return _cleanup_service
