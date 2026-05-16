"""ESP32 AI Recorder — 数据库初始化与会话管理。

SQLite WAL 模式 + SQLAlchemy async engine。
"""

from __future__ import annotations

import logging
from typing import AsyncGenerator, Optional

from sqlalchemy import event
from sqlalchemy.ext.asyncio import (
    AsyncSession,
    async_sessionmaker,
    create_async_engine,
)
from sqlalchemy.orm import DeclarativeBase

from .config import get_config

logger = logging.getLogger(__name__)


class Base(DeclarativeBase):
    """SQLAlchemy 声明式基类。"""

    pass


# 全局引擎和会话工厂（在 init_db 中初始化）
_engine = None
_async_session_factory: Optional[async_sessionmaker[AsyncSession]] = None


def _set_sqlite_pragma(dbapi_connection, _connection_record) -> None:
    """SQLite 连接建立时设置 PRAGMA。

    启用 WAL 模式（支持并发读）和外键约束。
    """
    cursor = dbapi_connection.cursor()
    cursor.execute("PRAGMA journal_mode=WAL")
    cursor.execute("PRAGMA foreign_keys=ON")
    cursor.close()


async def init_db() -> None:
    """初始化数据库引擎、会话工厂，并创建所有表。

    必须在 FastAPI lifespan 中调用一次。
    """
    global _engine, _async_session_factory

    config = get_config()

    _engine = create_async_engine(
        f"sqlite+aiosqlite:///{config.db_path}",
        echo=False,
    )

    # 在同步连接层设置 SQLite PRAGMA
    event.listen(_engine.sync_engine, "connect", _set_sqlite_pragma)

    # 创建所有表（如果不存在）
    async with _engine.begin() as conn:
        await conn.run_sync(Base.metadata.create_all)

    _async_session_factory = async_sessionmaker(
        _engine,
        class_=AsyncSession,
        expire_on_commit=False,
    )

    logger.info("Database initialized: %s (WAL mode)", config.db_path)


async def close_db() -> None:
    """关闭数据库引擎。"""
    global _engine
    if _engine is not None:
        await _engine.dispose()
        _engine = None
        logger.info("Database engine closed")


async def get_session() -> AsyncGenerator[AsyncSession, None]:
    """FastAPI 依赖注入：获取异步数据库会话。

    Yields:
        AsyncSession 实例。
    """
    if _async_session_factory is None:
        raise RuntimeError("Database not initialized — call init_db() first")
    async with _async_session_factory() as session:
        try:
            yield session
            await session.commit()
        except Exception:
            await session.rollback()
            raise
