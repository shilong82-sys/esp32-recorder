"""ESP32 AI Recorder — 转写服务。

asyncio.Queue + 单 worker 后台转写，支持：
- mlx-whisper 调用封装
- 状态机 pending → processing → completed / failed
- 超时 10 分钟，失败重试 1 次（间隔 30 秒）
- 转写结果写文件 transcripts/{saved_name}.txt
- 启动时自动处理 pending 记录 + 修复 stuck processing
- 从 settings 表读取语言和模型配置
"""

from __future__ import annotations

import asyncio
import logging
import os
import time
from datetime import datetime, timezone
from typing import Any, Optional

from sqlalchemy import select, update

from ..config import get_config
from ..models import File, Transcription
from ..services.settings_service import get_setting

logger = logging.getLogger(__name__)


class Transcriber:
    """转写服务单例，管理后台 worker 和任务队列。

    Attributes:
        _queue: 待转写文件 ID 队列。
        _worker_task: 后台 worker asyncio.Task。
        _stop_event: 停止信号。
        _started_at: 服务启动时间戳（用于计算运行时长）。
    """

    def __init__(self) -> None:
        self._queue: asyncio.Queue[int] = asyncio.Queue()
        self._worker_task: Optional[asyncio.Task] = None
        self._stop_event = asyncio.Event()
        self._started_at: float = 0.0

    # ------------------------------------------------------------------
    # 公共接口
    # ------------------------------------------------------------------

    async def start(self) -> None:
        """启动转写 worker，并重新入队所有 pending 记录。"""
        self._started_at = time.time()

        # 修复服务重启时卡在 processing 的记录 → 重置为 pending
        await self._reset_stuck_processing()

        # 将 pending 记录加入队列
        await self._enqueue_pending()

        # 启动后台 worker
        self._worker_task = asyncio.create_task(self._worker_loop())
        logger.info("Transcriber started (queue=%d)", self._queue.qsize())

    async def stop(self) -> None:
        """优雅停止 worker。"""
        self._stop_event.set()
        if self._worker_task is not None:
            self._worker_task.cancel()
            try:
                await self._worker_task
            except asyncio.CancelledError:
                pass
        logger.info("Transcriber stopped")

    async def enqueue(self, file_id: int) -> None:
        """将文件加入转写队列。

        Args:
            file_id: 文件 ID。
        """
        await self._queue.put(file_id)
        logger.info("Enqueued file_id=%d for transcription", file_id)

    def queue_size(self) -> int:
        """获取当前队列大小。"""
        return self._queue.qsize()

    @property
    def started_at(self) -> float:
        """服务启动时间戳。"""
        return self._started_at

    # ------------------------------------------------------------------
    # 启动时修复 & 入队
    # ------------------------------------------------------------------

    async def _reset_stuck_processing(self) -> None:
        """将卡在 processing 状态的记录重置为 pending（服务崩溃后恢复）。"""
        from ..database import _async_session_factory

        if _async_session_factory is None:
            return

        async with _async_session_factory() as session:
            result = await session.execute(
                update(Transcription)
                .where(Transcription.status == "processing")
                .values(status="pending", started_at=None, error_msg=None)
            )
            count = result.rowcount
            if count > 0:
                await session.commit()
                logger.info("Reset %d stuck processing transcriptions to pending", count)

    async def _enqueue_pending(self) -> None:
        """扫描 pending 状态的转写记录，加入队列。"""
        from ..database import _async_session_factory

        if _async_session_factory is None:
            return

        async with _async_session_factory() as session:
            result = await session.execute(
                select(Transcription.file_id).where(Transcription.status == "pending")
            )
            file_ids = [row[0] for row in result.all()]

        for fid in file_ids:
            await self._queue.put(fid)

        if file_ids:
            logger.info("Enqueued %d pending transcriptions", len(file_ids))

    # ------------------------------------------------------------------
    # Worker 主循环
    # ------------------------------------------------------------------

    async def _worker_loop(self) -> None:
        """后台 worker 主循环：从队列取出 file_id 并执行转写。"""
        logger.info("Transcriber worker started")
        while not self._stop_event.is_set():
            try:
                try:
                    file_id = await asyncio.wait_for(
                        self._queue.get(), timeout=1.0
                    )
                except asyncio.TimeoutError:
                    continue

                await self._process_file(file_id, attempt=1)

            except asyncio.CancelledError:
                logger.info("Transcriber worker cancelled")
                break
            except Exception as exc:
                logger.error("Unexpected error in transcriber worker: %s", exc)

        logger.info("Transcriber worker stopped")

    # ------------------------------------------------------------------
    # 转写处理
    # ------------------------------------------------------------------

    async def _process_file(self, file_id: int, attempt: int = 1) -> None:
        """处理单个文件的转写。

        Args:
            file_id: 文件 ID。
            attempt: 当前尝试次数（1=首次，2=重试）。
        """
        from ..database import _async_session_factory

        config = get_config()

        # ---- 读取文件和转写记录 ----
        saved_name: Optional[str] = None
        async with _async_session_factory() as session:
            # 获取文件记录
            file_result = await session.execute(
                select(File).where(File.id == file_id)
            )
            db_file = file_result.scalar_one_or_none()
            if db_file is None:
                logger.warning("File id=%d not found, skipping transcription", file_id)
                return
            saved_name = db_file.saved_name

            # 获取转写记录
            trans_result = await session.execute(
                select(Transcription).where(Transcription.file_id == file_id)
            )
            db_trans = trans_result.scalar_one_or_none()
            if db_trans is None:
                logger.warning("Transcription for file_id=%d not found, skipping", file_id)
                return

            # 检查是否已被处理（避免重复处理）
            if db_trans.status not in ("pending", "processing"):
                logger.info(
                    "Transcription file_id=%d already %s, skipping",
                    file_id, db_trans.status,
                )
                return

            # 从 settings 表读取语言和模型配置
            language = await get_setting("transcribe_language", "zh")
            model = await get_setting("transcribe_model", config.whisper_model)

            # 更新状态为 processing
            db_trans.status = "processing"
            db_trans.started_at = datetime.now(timezone.utc)
            db_trans.model = model
            await session.commit()

        # ---- 执行转写 ----
        audio_path = os.path.join(config.received_dir, saved_name)

        # 将 "auto" 语言转换为 None（mlx-whisper 自动检测）
        whisper_language: Optional[str] = None
        if language and language != "auto":
            whisper_language = language

        try:
            # 使用 asyncio.wait_for 实现超时，asyncio.to_thread 避免阻塞事件循环
            result: dict[str, Any] = await asyncio.wait_for(
                asyncio.to_thread(
                    self._transcribe_audio, audio_path, model, whisper_language
                ),
                timeout=config.transcribe_timeout_s,
            )

            # ---- 转写成功 ----
            text = result.get("text", "")
            detected_language = result.get("language")
            duration = result.get("duration")

            async with _async_session_factory() as session:
                trans_result = await session.execute(
                    select(Transcription).where(Transcription.file_id == file_id)
                )
                db_trans = trans_result.scalar_one_or_none()
                if db_trans is not None:
                    db_trans.status = "completed"
                    db_trans.text = text
                    # 写入实际使用的语言：如果指定了语言则用指定的，否则用检测到的
                    db_trans.language = whisper_language or detected_language
                    db_trans.duration = duration
                    db_trans.completed_at = datetime.now(timezone.utc)
                    await session.commit()

            # 将转写文本写入文件
            await asyncio.to_thread(self._write_transcript_file, saved_name, text)

            logger.info(
                "Transcription completed: file_id=%d lang=%s duration=%.1fs",
                file_id, whisper_language or detected_language, duration or 0,
            )

        except asyncio.TimeoutError:
            error_msg = f"Transcription timed out after {config.transcribe_timeout_s}s"
            logger.error("Transcription timeout: file_id=%d", file_id)
            await self._handle_failure(file_id, error_msg, attempt)

        except ImportError:
            error_msg = (
                "mlx-whisper is not installed. "
                "Install with: pip install mlx-whisper"
            )
            logger.error("mlx-whisper not available: %s", error_msg)
            await self._handle_failure(file_id, error_msg, attempt)

        except Exception as exc:
            error_msg = f"Transcription error: {exc}"
            logger.error("Transcription failed: file_id=%d error=%s", file_id, exc)
            await self._handle_failure(file_id, error_msg, attempt)

    # ------------------------------------------------------------------
    # 失败处理 & 重试
    # ------------------------------------------------------------------

    async def _handle_failure(
        self, file_id: int, error_msg: str, attempt: int
    ) -> None:
        """处理转写失败：首次失败等 30 秒重试，第二次失败则标记 failed。

        Args:
            file_id: 文件 ID。
            error_msg: 错误描述。
            attempt: 当前尝试次数。
        """
        if attempt < 2:
            logger.info(
                "Will retry transcription: file_id=%d attempt=%d in 30s",
                file_id, attempt + 1,
            )
            # 等待 30 秒，但每秒检查停止信号
            for _ in range(30):
                if self._stop_event.is_set():
                    return
                await asyncio.sleep(1.0)
            # 重试
            if not self._stop_event.is_set():
                await self._process_file(file_id, attempt=attempt + 1)
            return

        # 第二次也失败，标记为 failed
        from ..database import _async_session_factory

        async with _async_session_factory() as session:
            trans_result = await session.execute(
                select(Transcription).where(Transcription.file_id == file_id)
            )
            db_trans = trans_result.scalar_one_or_none()
            if db_trans is not None:
                db_trans.status = "failed"
                db_trans.error_msg = error_msg
                db_trans.completed_at = datetime.now(timezone.utc)
                await session.commit()

        logger.error(
            "Transcription failed permanently: file_id=%d error=%s",
            file_id, error_msg,
        )

    # ------------------------------------------------------------------
    # mlx-whisper 调用（同步，在 to_thread 中运行）
    # ------------------------------------------------------------------

    @staticmethod
    def _transcribe_audio(
        audio_path: str, model: str, language: Optional[str] = None
    ) -> dict[str, Any]:
        """调用 mlx-whisper 进行转写（同步函数，在子线程中执行）。

        Args:
            audio_path: 音频文件路径。
            model: 模型名称或 HuggingFace repo ID。
            language: 转写语言，None 时由 mlx-whisper 自动检测。

        Returns:
            转写结果字典，包含 text, language, duration 等字段。
        """
        import mlx_whisper

        kwargs: dict[str, Any] = {
            "path_or_hf_repo": model,
        }
        if language is not None:
            kwargs["language"] = language

        result = mlx_whisper.transcribe(audio_path, **kwargs)
        return result

    # ------------------------------------------------------------------
    # 转写文本写文件
    # ------------------------------------------------------------------

    @staticmethod
    def _write_transcript_file(saved_name: str, text: str) -> None:
        """将转写文本写入 transcripts 目录。

        Args:
            saved_name: 原始磁盘文件名。
            text: 转写文本内容。
        """
        config = get_config()
        os.makedirs(config.transcripts_dir, exist_ok=True)

        txt_filename = os.path.splitext(saved_name)[0] + ".txt"
        txt_path = os.path.join(config.transcripts_dir, txt_filename)

        with open(txt_path, "w", encoding="utf-8") as f:
            f.write(text)

        logger.info("Transcript written: %s", txt_path)


# ----------------------------------------------------------------------
# 模块级单例 + 向后兼容接口
# ----------------------------------------------------------------------

_transcriber: Optional[Transcriber] = None


def get_transcriber() -> Transcriber:
    """获取转写服务单例。

    Returns:
        Transcriber 实例。
    """
    global _transcriber
    if _transcriber is None:
        _transcriber = Transcriber()
    return _transcriber


async def enqueue(file_id: int) -> None:
    """将文件加入转写队列（模块级接口，向后兼容）。

    Args:
        file_id: 文件 ID。
    """
    await get_transcriber().enqueue(file_id)


async def get_queue_size() -> int:
    """获取当前队列大小（模块级接口，向后兼容）。

    Returns:
        队列中待处理的任务数。
    """
    return get_transcriber().queue_size()
