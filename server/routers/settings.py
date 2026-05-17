"""ESP32 AI Recorder — 设置路由。

GET  /api/settings        — 获取所有设置
PUT  /api/settings        — 批量更新设置
GET  /api/settings/models — 返回可用转写模型白名单
"""

import logging
from typing import Optional

from fastapi import APIRouter
from pydantic import BaseModel

from ..schemas import ApiResponse
from ..services.settings_service import get_all_settings, set_setting

logger = logging.getLogger(__name__)

router = APIRouter(prefix="/api")

# 可用转写模型白名单
AVAILABLE_MODELS: list[str] = [
    "mlx-community/whisper-large-v3-turbo",
    "mlx-community/whisper-large-v3",
    "mlx-community/whisper-medium",
    "mlx-community/whisper-small",
    "mlx-community/whisper-base",
    "mlx-community/whisper-tiny",
]


class SettingsUpdateRequest(BaseModel):
    """设置更新请求体。"""

    transcribe_language: Optional[str] = None
    transcribe_model: Optional[str] = None
    auto_transcribe: Optional[str] = None


@router.get("/settings", response_model=ApiResponse)
async def get_settings() -> ApiResponse:
    """获取所有设置项。"""
    settings = await get_all_settings()
    return ApiResponse(data=settings)


@router.put("/settings", response_model=ApiResponse)
async def update_settings(request: SettingsUpdateRequest) -> ApiResponse:
    """批量更新设置项（只更新传入的非 None 字段）。

    Args:
        request: 更新请求体，仅非 None 字段会被更新。

    Returns:
        ApiResponse，包含更新后的完整设置。
    """
    updates = request.model_dump(exclude_none=True)

    for key, value in updates.items():
        await set_setting(key, value)
        logger.info("Setting updated: %s = %s", key, value)

    # 返回更新后的完整设置
    settings = await get_all_settings()
    return ApiResponse(data=settings)


@router.get("/settings/models", response_model=ApiResponse)
async def get_available_models() -> ApiResponse:
    """返回可用转写模型白名单。"""
    return ApiResponse(data=AVAILABLE_MODELS)
