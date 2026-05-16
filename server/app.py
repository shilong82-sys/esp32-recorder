"""ESP32 AI Recorder — FastAPI 入口。

挂载所有路由，管理应用生命周期（启动时初始化数据库、索引文件、启动转写 worker）。
"""

import logging
import os
from contextlib import asynccontextmanager
from datetime import datetime, timezone
from pathlib import Path

import uvicorn
from fastapi import FastAPI, Request
from fastapi.responses import HTMLResponse
from fastapi.staticfiles import StaticFiles
from fastapi.templating import Jinja2Templates

from .config import get_config
from .database import close_db, init_db
from .routers import files, status, transcripts, upload
from .services.file_indexer import index_received_dir
from .services.transcriber import get_transcriber

# 配置日志
logging.basicConfig(
    level=logging.INFO,
    format="[%(asctime)s] %(levelname)s %(name)s: %(message)s",
    datefmt="%H:%M:%S",
)
logger = logging.getLogger(__name__)

# 项目根路径（server/ 目录）
BASE_DIR = Path(__file__).resolve().parent

# 服务启动时间（用于前端展示运行时长）
_server_start_time: str = ""


@asynccontextmanager
async def lifespan(app: FastAPI):
    """应用生命周期管理。

    启动时：
    1. 初始化数据库
    2. 确保存储目录存在
    3. 索引 received/ 目录
    4. 启动转写 worker
    """
    global _server_start_time
    config = get_config()

    # 确保存储目录存在
    os.makedirs(config.received_dir, exist_ok=True)
    os.makedirs(config.transcripts_dir, exist_ok=True)

    # 初始化数据库
    await init_db()
    logger.info("Database initialized")

    # 索引 received/ 目录中已有的文件
    indexed = await index_received_dir()
    logger.info("File indexing complete: %d files indexed", indexed)

    # 启动转写 worker
    transcriber = get_transcriber()
    await transcriber.start()
    logger.info("Transcriber worker started")

    # 记录服务启动时间
    _server_start_time = datetime.now(timezone.utc).isoformat()

    logger.info(
        "ESP32 AI Recorder server started — http://%s:%d",
        config.host, config.port,
    )

    yield  # 应用运行中

    # 停止转写 worker
    await transcriber.stop()
    logger.info("Transcriber worker stopped")

    # 关闭数据库
    await close_db()
    logger.info("Server shutting down")


def create_app() -> FastAPI:
    """创建 FastAPI 应用实例。"""
    app = FastAPI(
        title="ESP32 AI Recorder",
        description="录音管理 + AI 转写平台",
        version="0.3.0",
        lifespan=lifespan,
    )

    # 挂载路由
    app.include_router(upload.router, tags=["upload"])
    app.include_router(files.router, tags=["files"])
    app.include_router(transcripts.router, tags=["transcripts"])
    app.include_router(status.router, tags=["status"])

    # 挂载静态文件（CSS、JS 等）
    static_dir = BASE_DIR / "static"
    static_dir.mkdir(exist_ok=True)
    app.mount("/static", StaticFiles(directory=str(static_dir)), name="static")

    # Jinja2 模板
    templates = Jinja2Templates(directory=str(BASE_DIR / "templates"))

    # Web UI 首页
    @app.get("/", response_class=HTMLResponse)
    async def root(request: Request) -> HTMLResponse:
        """Web UI 管理面板首页。"""
        return templates.TemplateResponse(
            "index.html",
            {
                "request": request,
                "server_start_time": _server_start_time,
            },
        )

    return app


app = create_app()


if __name__ == "__main__":
    config = get_config()
    uvicorn.run(
        "server.app:app",
        host=config.host,
        port=config.port,
        reload=False,
    )
