"""ESP32 AI Recorder — 认证中间件。

检查 session cookie（rec_session），对未认证请求返回 401。
豁免路径：/health、/upload、/api/auth/login、/api/auth/logout、/static/、/docs、/openapi.json。
"""

import logging

from itsdangerous import BadSignature, SignatureExpired, URLSafeTimedSerializer
from starlette.middleware.base import BaseHTTPMiddleware
from starlette.requests import Request
from starlette.responses import JSONResponse

from ..config import get_config

logger = logging.getLogger(__name__)

# 豁免认证的路径前缀和精确路径
EXEMPT_PATHS: tuple[str, ...] = (
    "/health",
    "/upload",
    "/api/auth/login",
    "/api/auth/logout",
    "/static/",
    "/docs",
    "/openapi.json",
    "/redoc",
)

# Cookie 最大有效期（秒）：7 天
COOKIE_MAX_AGE: int = 604800


class AuthMiddleware(BaseHTTPMiddleware):
    """认证中间件：检查 rec_session cookie 签名和有效期。

    若 config.auth_enabled == False，直接放行所有请求。
    """

    async def dispatch(self, request: Request, call_next):
        """处理每个请求的认证检查。"""
        config = get_config()

        # 认证未启用，直接放行
        if not config.auth_enabled:
            return await call_next(request)

        # 检查是否为豁免路径
        path = request.url.path
        for exempt in EXEMPT_PATHS:
            if path == exempt or path.startswith(exempt):
                return await call_next(request)

        # 根路径也需要放行（返回 HTML 页面）
        if path == "/":
            return await call_next(request)

        # 读取并验证 session cookie
        session_token = request.cookies.get("rec_session")
        if not session_token:
            return self._unauthorized_response()

        try:
            serializer = URLSafeTimedSerializer(config.session_secret)
            data = serializer.loads(session_token, max_age=COOKIE_MAX_AGE)
            # 验证 data 中包含 auth=True
            if not isinstance(data, dict) or not data.get("auth"):
                return self._unauthorized_response()
        except SignatureExpired:
            logger.info("Session cookie expired for path: %s", path)
            return self._unauthorized_response()
        except BadSignature:
            logger.warning("Invalid session cookie for path: %s", path)
            return self._unauthorized_response()
        except Exception as exc:
            logger.warning("Session cookie validation error: %s", exc)
            return self._unauthorized_response()

        # 认证通过
        return await call_next(request)

    @staticmethod
    def _unauthorized_response() -> JSONResponse:
        """返回 401 未认证响应。"""
        return JSONResponse(
            {"code": 40100, "message": "Unauthorized", "data": None},
            status_code=401,
        )
