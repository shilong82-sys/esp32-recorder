"""ESP32 AI Recorder — 运行时设置服务。

提供 settings 表的 CRUD 操作，所有设置项以 key-value 形式存储。
"""

import logging
from typing import Optional

from sqlalchemy import select
from sqlalchemy.dialects.sqlite import insert as sqlite_insert

from ..models import Setting

logger = logging.getLogger(__name__)

# 默认设置项（仅在 settings 表为空时插入）
DEFAULTS: dict[str, str] = {
    "transcribe_language": "zh",
    "transcribe_model": "mlx-community/whisper-large-v3-turbo",
    "auto_transcribe": "true",
    "cleanup_days": "90",
    "diarize_enabled": "false",
}


async def get_setting(key: str, default: Optional[str] = None) -> Optional[str]:
    """读取单个设置项的值。

    Args:
        key: 设置项名称。
        default: 未找到时的默认返回值。

    Returns:
        设置项的值，未找到则返回 default。
    """
    from ..database import _async_session_factory

    if _async_session_factory is None:
        return default

    async with _async_session_factory() as session:
        result = await session.execute(
            select(Setting.value).where(Setting.key == key)
        )
        value = result.scalar_one_or_none()
        if value is not None:
            return value
        return default


async def set_setting(key: str, value: str) -> None:
    """写入（upsert）单个设置项。

    Args:
        key: 设置项名称。
        value: 设置项值。
    """
    from ..database import _async_session_factory

    if _async_session_factory is None:
        return

    async with _async_session_factory() as session:
        # 尝试查询已有记录
        result = await session.execute(
            select(Setting).where(Setting.key == key)
        )
        existing = result.scalar_one_or_none()

        if existing is not None:
            existing.value = value
        else:
            session.add(Setting(key=key, value=value))

        await session.commit()
        logger.info("Setting updated: %s = %s", key, value)


async def get_all_settings() -> dict:
    """读取所有设置项。

    Returns:
        所有设置项的字典 {key: value}，包含默认值中未覆盖的项。
    """
    from ..database import _async_session_factory

    result_dict: dict[str, str] = {}

    if _async_session_factory is not None:
        async with _async_session_factory() as session:
            result = await session.execute(select(Setting))
            settings = result.scalars().all()
            for s in settings:
                result_dict[s.key] = s.value

    # 补齐默认值中缺失的项
    for key, default_value in DEFAULTS.items():
        if key not in result_dict:
            result_dict[key] = default_value

    return result_dict


async def init_default_settings() -> None:
    """初始化默认设置项（幂等）。

    仅在 settings 表中不存在对应 key 时才插入默认值，
    不会覆盖用户已修改的设置。
    """
    from ..database import _async_session_factory

    if _async_session_factory is None:
        return

    async with _async_session_factory() as session:
        for key, default_value in DEFAULTS.items():
            # 检查是否已存在
            result = await session.execute(
                select(Setting).where(Setting.key == key)
            )
            existing = result.scalar_one_or_none()
            if existing is None:
                session.add(Setting(key=key, value=default_value))
                logger.info("Initialized default setting: %s = %s", key, default_value)

        await session.commit()
