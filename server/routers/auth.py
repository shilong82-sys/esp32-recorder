"""ESP32 AI Recorder — 认证路由。

POST /api/auth/login  — 密码验证，签发 session cookie
POST /api/auth/logout — 清除 session cookie
"""

import logging

from fastapi import APIRouter, Response
from itsdangerous import URLSafeTimedSerializer

from ..config import get_config
from ..schemas import ApiResponse, LoginRequest

logger = logging.getLogger(__name__)

router = APIRouter(prefix="/api")

# Cookie 最大有效期（秒）：7 天
COOKIE_MAX_AGE: int = 604800


@router.post("/auth/login", response_model=ApiResponse)
async def login(request: LoginRequest, response: Response) -> ApiResponse:
    """密码验证，成功后签发 session cookie。

    Args:
        request: 登录请求（含密码）。
        response: FastAPI Response 对象，用于设置 cookie。

    Returns:
        ApiResponse，成功时 code=0。
    """
    config = get_config()

    if request.password != config.auth_password:
        logger.warning("Login failed: incorrect password")
        return ApiResponse(
            code=40100,
            message="Incorrect password",
            data=None,
        )

    # 签发 session token
    serializer = URLSafeTimedSerializer(config.session_secret)
    token = serializer.dumps({"auth": True})

    # 设置 cookie
    response.set_cookie(
        key="rec_session",
        value=token,
        max_age=COOKIE_MAX_AGE,
        httponly=True,
        samesite="lax",
    )

    logger.info("Login successful")
    return ApiResponse(data={"authenticated": True})


@router.post("/auth/logout", response_model=ApiResponse)
async def logout(response: Response) -> ApiResponse:
    """清除 session cookie，退出登录。

    Args:
        response: FastAPI Response 对象，用于清除 cookie。

    Returns:
        ApiResponse，成功时 code=0。
    """
    response.delete_cookie(key="rec_session")
    logger.info("Logout successful")
    return ApiResponse(data={"authenticated": False})
