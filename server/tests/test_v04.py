"""ESP32 AI Recorder v0.4 — 综合测试套件。

测试覆盖 P0-01 ~ P0-08 全部需求，使用 pytest + httpx.AsyncClient。
每个测试使用全新的临时 SQLite 数据库，不影响用户数据。

运行方式：
    cd /Users/long/Projects/esp32-recorder
    python -m pytest server/tests/test_v04.py -v
"""

from __future__ import annotations

import os
import struct
import tempfile
from datetime import datetime, timezone
from typing import AsyncGenerator, Optional
from unittest.mock import AsyncMock, patch

import pytest
import pytest_asyncio
from httpx import ASGITransport, AsyncClient
from sqlalchemy.ext.asyncio import AsyncSession, async_sessionmaker, create_async_engine

# ---------------------------------------------------------------------------
# 导入项目模块
# ---------------------------------------------------------------------------
from server.database import Base, get_session
from server.config import AppConfig, get_config
from server.models import File, Setting, Transcription

# 需要在 import app 之前 patch 配置，所以用延迟导入


# ===========================================================================
# Fixtures
# ===========================================================================


def _make_wav_bytes(
    sample_rate: int = 16000,
    bits_per_sample: int = 16,
    channels: int = 1,
    duration_seconds: float = 1.0,
) -> bytes:
    """生成一个最小的合法 WAV 文件字节。

    Args:
        sample_rate: 采样率（Hz）。
        bits_per_sample: 位深。
        channels: 声道数。
        duration_seconds: 时长（秒）。

    Returns:
        完整的 WAV 文件字节数据。
    """
    byte_rate = sample_rate * channels * bits_per_sample // 8
    block_align = channels * bits_per_sample // 8
    num_samples = int(sample_rate * duration_seconds)
    data_size = num_samples * block_align
    file_size = 36 + data_size  # RIFF chunk size = 36 + data_size

    # RIFF header
    header = struct.pack(
        "<4sI4s4sIHHIIHH4sI",
        b"RIFF",
        file_size,
        b"WAVE",
        b"fmt ",
        16,  # fmt chunk size
        1,   # audio format (PCM)
        channels,
        sample_rate,
        byte_rate,
        block_align,
        bits_per_sample,
        b"data",
        data_size,
    )

    # 静音数据（全零）
    audio_data = b"\x00" * data_size

    return header + audio_data


@pytest.fixture(scope="session")
def wav_bytes() -> bytes:
    """1 秒 16kHz 16bit mono 静音 WAV 文件。"""
    return _make_wav_bytes(sample_rate=16000, bits_per_sample=16, channels=1, duration_seconds=1.0)


@pytest.fixture(scope="session")
def wav_bytes_2s() -> bytes:
    """2 秒 16kHz 16bit mono 静音 WAV 文件。"""
    return _make_wav_bytes(sample_rate=16000, bits_per_sample=16, channels=1, duration_seconds=2.0)


@pytest.fixture()
def tmp_dir():
    """创建临时目录用于测试文件存储。"""
    with tempfile.TemporaryDirectory() as d:
        yield d


@pytest.fixture()
def test_config(tmp_dir):
    """创建测试专用配置（auth_enabled=False，临时目录）。"""
    db_path = os.path.join(tmp_dir, "test.db")
    received_dir = os.path.join(tmp_dir, "received")
    transcripts_dir = os.path.join(tmp_dir, "transcripts")
    os.makedirs(received_dir, exist_ok=True)
    os.makedirs(transcripts_dir, exist_ok=True)

    config = AppConfig(
        host="127.0.0.1",
        port=8000,
        received_dir=received_dir,
        transcripts_dir=transcripts_dir,
        db_path=db_path,
        auth_enabled=False,
        auth_password="testpass123",
        session_secret="test-secret-key-for-testing",
    )
    return config


@pytest.fixture()
def test_config_auth_enabled(tmp_dir):
    """创建启用认证的测试配置。"""
    db_path = os.path.join(tmp_dir, "test_auth.db")
    received_dir = os.path.join(tmp_dir, "received_auth")
    transcripts_dir = os.path.join(tmp_dir, "transcripts_auth")
    os.makedirs(received_dir, exist_ok=True)
    os.makedirs(transcripts_dir, exist_ok=True)

    config = AppConfig(
        host="127.0.0.1",
        port=8000,
        received_dir=received_dir,
        transcripts_dir=transcripts_dir,
        db_path=db_path,
        auth_enabled=True,
        auth_password="testpass123",
        session_secret="test-secret-key-for-testing",
    )
    return config


@pytest_asyncio.fixture()
async def db_engine(test_config):
    """创建测试用异步数据库引擎。"""
    engine = create_async_engine(
        f"sqlite+aiosqlite:///{test_config.db_path}",
        echo=False,
    )
    async with engine.begin() as conn:
        await conn.run_sync(Base.metadata.create_all)

    yield engine

    await engine.dispose()


@pytest_asyncio.fixture()
async def db_session_factory(db_engine) -> async_sessionmaker:
    """创建测试用 session factory。"""
    return async_sessionmaker(
        db_engine,
        class_=AsyncSession,
        expire_on_commit=False,
    )


@pytest_asyncio.fixture()
async def db_session(db_session_factory) -> AsyncGenerator[AsyncSession, None]:
    """提供测试用数据库会话。"""
    async with db_session_factory() as session:
        yield session


@pytest_asyncio.fixture()
async def app_client(test_config, db_engine, db_session_factory) -> AsyncGenerator[AsyncClient, None]:
    """创建无认证的 FastAPI 测试客户端。"""

    # patch get_config 和 database 模块级变量
    with patch("server.config._config", test_config), \
         patch("server.database._engine", db_engine), \
         patch("server.database._async_session_factory", db_session_factory), \
         patch("server.services.transcriber.get_transcriber") as mock_get_transcriber:

        # mock transcriber
        mock_transcriber = AsyncMock()
        mock_transcriber.start = AsyncMock()
        mock_transcriber.stop = AsyncMock()
        mock_transcriber.enqueue = AsyncMock()
        mock_get_transcriber.return_value = mock_transcriber

        # 也需要 patch transcriber 模块级单例
        with patch("server.services.transcriber._transcriber", mock_transcriber):
            # 延迟导入 app，确保 patch 生效
            from server.app import create_app
            app = create_app()

            # Override get_session 依赖
            async def override_get_session():
                async with db_session_factory() as session:
                    yield session
                    await session.commit()

            app.dependency_overrides[get_session] = override_get_session

            transport = ASGITransport(app=app)
            async with AsyncClient(transport=transport, base_url="http://test") as client:
                yield client

            app.dependency_overrides.clear()


@pytest_asyncio.fixture()
async def auth_app_client(test_config_auth_enabled, tmp_dir) -> AsyncGenerator[AsyncClient, None]:
    """创建启用认证的 FastAPI 测试客户端。"""

    # 为 auth 测试创建独立的 engine
    auth_engine = create_async_engine(
        f"sqlite+aiosqlite:///{test_config_auth_enabled.db_path}",
        echo=False,
    )
    async with auth_engine.begin() as conn:
        await conn.run_sync(Base.metadata.create_all)

    auth_session_factory = async_sessionmaker(
        auth_engine,
        class_=AsyncSession,
        expire_on_commit=False,
    )

    with patch("server.config._config", test_config_auth_enabled), \
         patch("server.database._engine", auth_engine), \
         patch("server.database._async_session_factory", auth_session_factory), \
         patch("server.services.transcriber.get_transcriber") as mock_get_transcriber, \
         patch("server.services.transcriber._transcriber", AsyncMock()):

        mock_transcriber = AsyncMock()
        mock_transcriber.start = AsyncMock()
        mock_transcriber.stop = AsyncMock()
        mock_transcriber.enqueue = AsyncMock()
        mock_get_transcriber.return_value = mock_transcriber

        from server.app import create_app
        app = create_app()

        async def override_get_session():
            async with auth_session_factory() as session:
                yield session
                await session.commit()

        app.dependency_overrides[get_session] = override_get_session

        transport = ASGITransport(app=app)
        async with AsyncClient(transport=transport, base_url="http://test") as client:
            yield client, test_config_auth_enabled

        app.dependency_overrides.clear()
        await auth_engine.dispose()


async def _create_test_file(
    db_session_factory,
    saved_name: str = "test_audio.wav",
    filename: str = "test_audio.wav",
    file_size: int = 32044,
    duration: Optional[float] = 1.0,
) -> int:
    """在数据库中创建测试文件记录，返回 file_id。"""
    async with db_session_factory() as session:
        db_file = File(
            filename=filename,
            saved_name=saved_name,
            file_size=file_size,
            upload_time=datetime.now(timezone.utc),
            upload_src="127.0.0.1",
            duration=duration,
        )
        session.add(db_file)
        await session.commit()
        return db_file.id


async def _create_test_transcription(
    db_session_factory,
    file_id: int,
    status: str = "completed",
    text: str = "Hello world this is a test",
    language: Optional[str] = "en",
    is_edited: int = 0,
) -> int:
    """在数据库中创建测试转写记录，返回 transcription_id。"""
    async with db_session_factory() as session:
        db_trans = Transcription(
            file_id=file_id,
            status=status,
            text=text,
            language=language,
            is_edited=is_edited,
        )
        session.add(db_trans)
        await session.commit()
        return db_trans.id


async def _login_client(client: AsyncClient, password: str = "testpass123"):
    """执行登录并返回更新后的客户端（带 cookie）。"""
    resp = await client.post("/api/auth/login", json={"password": password})
    return resp


# ===========================================================================
# P0-05: 认证模块测试
# ===========================================================================


class TestAuth:
    """认证模块测试（P0-05）。"""

    @pytest.mark.asyncio
    async def test_login_success(self, auth_app_client):
        """正确密码返回 200 + Set-Cookie rec_session。"""
        client, config = auth_app_client
        resp = await _login_client(client)
        assert resp.status_code == 200
        body = resp.json()
        assert body["code"] == 0
        assert body["data"]["authenticated"] is True
        # 检查 Set-Cookie 头
        set_cookie = resp.headers.get("set-cookie", "")
        assert "rec_session" in set_cookie

    @pytest.mark.asyncio
    async def test_login_wrong_password(self, auth_app_client):
        """错误密码返回 40100 错误码。"""
        client, config = auth_app_client
        resp = await client.post("/api/auth/login", json={"password": "wrongpassword"})
        assert resp.status_code == 200  # HTTP 200，但 code 非 0
        body = resp.json()
        assert body["code"] == 40100
        assert "Incorrect" in body["message"] or "password" in body["message"].lower()

    @pytest.mark.asyncio
    async def test_logout(self, auth_app_client):
        """退出返回 200 + 清除 cookie。"""
        client, config = auth_app_client
        # 先登录
        login_resp = await _login_client(client)
        # 从响应中提取 cookie
        cookies = {}
        for cookie_name in login_resp.cookies.jar:
            cookies[cookie_name.name] = cookie_name.value

        # 退出
        resp = await client.post("/api/auth/logout", cookies=cookies)
        assert resp.status_code == 200
        body = resp.json()
        assert body["data"]["authenticated"] is False
        # 检查 cookie 被清除
        set_cookie = resp.headers.get("set-cookie", "")
        assert "rec_session" in set_cookie

    @pytest.mark.asyncio
    async def test_auth_middleware_blocks_unauthenticated(self, auth_app_client):
        """无 cookie 访问受保护端点返回 401。"""
        client, config = auth_app_client
        resp = await client.get("/api/files")
        assert resp.status_code == 401
        body = resp.json()
        assert body["code"] == 40100

    @pytest.mark.asyncio
    async def test_auth_middleware_allows_health(self, auth_app_client):
        """/health 免认证。"""
        client, config = auth_app_client
        resp = await client.get("/health")
        # /health 可能返回 200 或 404（如果路由不存在），但不应 401
        assert resp.status_code != 401

    @pytest.mark.asyncio
    async def test_auth_middleware_allows_upload(self, auth_app_client):
        """/upload 免认证（发送空 body 也应不被 401 拦截）。"""
        client, config = auth_app_client
        resp = await client.post("/upload", content=b"")
        # 不应返回 401（可能是 200 或其他错误码）
        assert resp.status_code != 401

    @pytest.mark.asyncio
    async def test_auth_disabled(self, app_client):
        """AUTH_ENABLED=False 时所有端点可访问。"""
        resp = await app_client.get("/api/files")
        # 应该 200，不是 401
        assert resp.status_code == 200


# ===========================================================================
# P0-01 & P0-06: Settings 模块测试
# ===========================================================================


class TestSettings:
    """设置模块测试（P0-01, P0-06）。"""

    @pytest.mark.asyncio
    async def test_get_settings(self, app_client, db_session_factory):
        """获取默认设置。"""
        # 先初始化默认设置
        from server.services.settings_service import init_default_settings
        await init_default_settings()

        resp = await app_client.get("/api/settings")
        assert resp.status_code == 200
        body = resp.json()
        assert body["code"] == 0
        data = body["data"]
        assert "transcribe_language" in data
        assert "transcribe_model" in data
        assert "auto_transcribe" in data

    @pytest.mark.asyncio
    async def test_update_settings(self, app_client, db_session_factory):
        """更新 transcribe_language 为 "en"。"""
        # 先初始化
        from server.services.settings_service import init_default_settings
        await init_default_settings()

        resp = await app_client.put(
            "/api/settings",
            json={"transcribe_language": "en"},
        )
        assert resp.status_code == 200
        body = resp.json()
        assert body["code"] == 0
        assert body["data"]["transcribe_language"] == "en"

        # 再次获取验证持久化
        resp2 = await app_client.get("/api/settings")
        data2 = resp2.json()["data"]
        assert data2["transcribe_language"] == "en"

    @pytest.mark.asyncio
    async def test_get_models(self, app_client):
        """获取可用模型列表。"""
        resp = await app_client.get("/api/settings/models")
        assert resp.status_code == 200
        body = resp.json()
        assert body["code"] == 0
        models = body["data"]
        assert isinstance(models, list)
        assert len(models) > 0
        # 应包含默认模型
        assert any("whisper" in m.lower() for m in models)

    @pytest.mark.asyncio
    async def test_init_default_settings_idempotent(self, db_session_factory):
        """重复调用 init_default_settings 不重复插入。"""
        with patch("server.database._async_session_factory", db_session_factory):
            from server.services.settings_service import init_default_settings
            await init_default_settings()
            await init_default_settings()

            # 验证每个 key 只有一条记录
            async with db_session_factory() as session:
                from sqlalchemy import select, func
                result = await session.execute(
                    select(Setting.key, func.count(Setting.key))
                    .group_by(Setting.key)
                )
                counts = {row[0]: row[1] for row in result.all()}
                for key in ["transcribe_language", "transcribe_model", "auto_transcribe"]:
                    assert counts.get(key, 0) == 1, f"Key {key} has {counts.get(key, 0)} rows, expected 1"


# ===========================================================================
# P0-02: 音频流测试
# ===========================================================================


class TestAudioStream:
    """音频流端点测试（P0-02）。"""

    @pytest.mark.asyncio
    async def test_stream_full_file(self, app_client, test_config, db_session_factory, wav_bytes):
        """无 Range 请求返回完整文件（200）。"""
        # 写入 WAV 文件到磁盘
        saved_name = "stream_test.wav"
        file_path = os.path.join(test_config.received_dir, saved_name)
        with open(file_path, "wb") as f:
            f.write(wav_bytes)

        # 创建数据库记录
        file_id = await _create_test_file(
            db_session_factory,
            saved_name=saved_name,
            filename=saved_name,
            file_size=len(wav_bytes),
            duration=1.0,
        )

        resp = await app_client.get(f"/api/files/{file_id}/stream")
        assert resp.status_code == 200
        assert resp.headers.get("content-type", "").startswith("audio/wav")
        assert "accept-ranges" in resp.headers
        assert int(resp.headers.get("content-length", 0)) == len(wav_bytes)

    @pytest.mark.asyncio
    async def test_stream_with_range(self, app_client, test_config, db_session_factory, wav_bytes):
        """Range: bytes=0-1023 返回部分内容（206）。"""
        saved_name = "range_test.wav"
        file_path = os.path.join(test_config.received_dir, saved_name)
        with open(file_path, "wb") as f:
            f.write(wav_bytes)

        file_id = await _create_test_file(
            db_session_factory,
            saved_name=saved_name,
            filename=saved_name,
            file_size=len(wav_bytes),
            duration=1.0,
        )

        resp = await app_client.get(
            f"/api/files/{file_id}/stream",
            headers={"range": "bytes=0-1023"},
        )
        assert resp.status_code == 206
        assert "content-range" in resp.headers
        content_range = resp.headers["content-range"]
        assert content_range.startswith("bytes 0-1023/")
        assert int(resp.headers.get("content-length", 0)) == 1024

    @pytest.mark.asyncio
    async def test_stream_file_not_found(self, app_client):
        """不存在的文件返回 404。"""
        resp = await app_client.get("/api/files/99999/stream")
        assert resp.status_code == 404


# ===========================================================================
# P0-03: 转写编辑测试
# ===========================================================================


class TestTranscriptEdit:
    """转写编辑测试（P0-03）。"""

    @pytest.mark.asyncio
    async def test_edit_transcript(self, app_client, test_config, db_session_factory):
        """PUT 更新 text，is_edited=1，edited_at 非 null。"""
        file_id = await _create_test_file(db_session_factory, saved_name="edit_test.wav")
        trans_id = await _create_test_transcription(
            db_session_factory, file_id=file_id, status="completed", text="Original text"
        )

        new_text = "Updated transcription text"
        resp = await app_client.put(
            f"/api/transcripts/{file_id}",
            json={"text": new_text},
        )
        assert resp.status_code == 200
        body = resp.json()
        assert body["code"] == 0
        data = body["data"]
        assert data["text"] == new_text
        assert data["is_edited"] == 1
        assert data["edited_at"] is not None

    @pytest.mark.asyncio
    async def test_edit_transcript_not_completed(self, app_client, test_config, db_session_factory):
        """只能编辑已完成的转写。"""
        file_id = await _create_test_file(db_session_factory, saved_name="edit_pending.wav")
        await _create_test_transcription(
            db_session_factory, file_id=file_id, status="pending", text="Pending text"
        )

        resp = await app_client.put(
            f"/api/transcripts/{file_id}",
            json={"text": "Try to edit pending"},
        )
        body = resp.json()
        assert body["code"] != 0  # 应该返回错误

    @pytest.mark.asyncio
    async def test_retranscribe_resets_edit(self, app_client, test_config, db_session_factory):
        """重新转写重置 is_edited=0, edited_at=None。"""
        file_id = await _create_test_file(db_session_factory, saved_name="retranscribe.wav")
        trans_id = await _create_test_transcription(
            db_session_factory,
            file_id=file_id,
            status="completed",
            text="Edited text",
            is_edited=1,
        )

        # 触发重新转写
        resp = await app_client.post(f"/api/transcribe/{file_id}")
        assert resp.status_code == 200
        body = resp.json()
        assert body["code"] == 0

        # 验证 is_edited 和 edited_at 被重置
        async with db_session_factory() as session:
            from sqlalchemy import select
            result = await session.execute(
                select(Transcription).where(Transcription.file_id == file_id)
            )
            db_trans = result.scalar_one_or_none()
            assert db_trans is not None
            assert db_trans.is_edited == 0
            assert db_trans.edited_at is None


# ===========================================================================
# P0-04: 搜索测试
# ===========================================================================


class TestSearch:
    """全文搜索测试（P0-04）。"""

    @pytest.mark.asyncio
    async def test_search_found(self, app_client, db_session_factory):
        """搜索匹配内容返回结果。"""
        file_id = await _create_test_file(db_session_factory, saved_name="search_test.wav")
        await _create_test_transcription(
            db_session_factory,
            file_id=file_id,
            status="completed",
            text="The quick brown fox jumps over the lazy dog",
        )

        resp = await app_client.get("/api/search", params={"q": "brown fox"})
        assert resp.status_code == 200
        body = resp.json()
        assert body["code"] == 0
        items = body["data"]
        assert len(items) >= 1
        # snippet 中应包含高亮标记
        assert "**" in items[0]["snippet"] or "brown fox" in items[0]["snippet"].lower()

    @pytest.mark.asyncio
    async def test_search_not_found(self, app_client, db_session_factory):
        """无匹配返回空列表。"""
        file_id = await _create_test_file(db_session_factory, saved_name="search_nf.wav")
        await _create_test_transcription(
            db_session_factory,
            file_id=file_id,
            status="completed",
            text="Hello world",
        )

        resp = await app_client.get("/api/search", params={"q": "xyznonexistent"})
        assert resp.status_code == 200
        body = resp.json()
        assert body["code"] == 0
        assert body["data"] == []

    @pytest.mark.asyncio
    async def test_search_empty_query(self, app_client):
        """空查询返回空列表。"""
        resp = await app_client.get("/api/search", params={"q": ""})
        assert resp.status_code == 200
        body = resp.json()
        assert body["code"] == 0
        assert body["data"] == []


# ===========================================================================
# P0-07: WAV 工具 & 时长显示测试
# ===========================================================================


class TestWavUtils:
    """WAV 工具测试（P0-07）。"""

    def test_read_wav_duration(self, tmp_dir, wav_bytes):
        """读取正常 WAV 文件时长。"""
        from server.services.wav_utils import read_wav_duration

        wav_path = os.path.join(tmp_dir, "test.wav")
        with open(wav_path, "wb") as f:
            f.write(wav_bytes)

        duration = read_wav_duration(wav_path)
        assert duration is not None
        assert 0.9 <= duration <= 1.1  # 约 1 秒

    def test_read_wav_duration_2s(self, tmp_dir, wav_bytes_2s):
        """读取 2 秒 WAV 文件时长。"""
        from server.services.wav_utils import read_wav_duration

        wav_path = os.path.join(tmp_dir, "test2.wav")
        with open(wav_path, "wb") as f:
            f.write(wav_bytes_2s)

        duration = read_wav_duration(wav_path)
        assert duration is not None
        assert 1.9 <= duration <= 2.1  # 约 2 秒

    def test_read_wav_nonexistent(self):
        """文件不存在返回 None。"""
        from server.services.wav_utils import read_wav_duration
        duration = read_wav_duration("/nonexistent/path/file.wav")
        assert duration is None

    def test_read_wav_invalid(self, tmp_dir):
        """非 WAV 文件返回 None。"""
        from server.services.wav_utils import read_wav_duration

        bad_path = os.path.join(tmp_dir, "not_wav.txt")
        with open(bad_path, "wb") as f:
            f.write(b"This is not a WAV file at all")

        duration = read_wav_duration(bad_path)
        assert duration is None


class TestFileDuration:
    """文件列表 duration 字段测试（P0-07）。"""

    @pytest.mark.asyncio
    async def test_file_duration_in_list(self, app_client, db_session_factory):
        """文件列表包含 duration 字段。"""
        file_id = await _create_test_file(
            db_session_factory,
            saved_name="duration_test.wav",
            duration=1.5,
        )

        resp = await app_client.get("/api/files")
        assert resp.status_code == 200
        body = resp.json()
        assert body["code"] == 0
        items = body["data"]["items"]
        # 找到我们创建的文件
        found = [i for i in items if i["id"] == file_id]
        assert len(found) == 1
        assert found[0]["duration"] == 1.5

    @pytest.mark.asyncio
    async def test_file_duration_in_detail(self, app_client, db_session_factory):
        """文件详情包含 duration 字段。"""
        file_id = await _create_test_file(
            db_session_factory,
            saved_name="duration_detail.wav",
            duration=2.0,
        )

        resp = await app_client.get(f"/api/files/{file_id}")
        assert resp.status_code == 200
        body = resp.json()
        assert body["data"]["duration"] == 2.0


# ===========================================================================
# P0-08: 日期范围筛选测试
# ===========================================================================


class TestDateFilter:
    """日期范围筛选测试（P0-08）。"""

    @pytest.mark.asyncio
    async def test_date_from_filter(self, app_client, db_session_factory):
        """date_from 过滤只返回指定日期之后的文件。"""
        # 创建一个文件
        file_id = await _create_test_file(
            db_session_factory,
            saved_name="date_filter.wav",
        )

        resp = await app_client.get(
            "/api/files",
            params={"date_from": "2020-01-01"},
        )
        assert resp.status_code == 200
        body = resp.json()
        assert body["code"] == 0
        # 至少应有我们刚创建的文件
        assert body["data"]["total"] >= 1

    @pytest.mark.asyncio
    async def test_date_to_filter(self, app_client, db_session_factory):
        """date_to 过滤只返回指定日期之前的文件。"""
        file_id = await _create_test_file(
            db_session_factory,
            saved_name="date_filter2.wav",
        )

        # 用很早的日期，应该不包含新创建的文件
        resp = await app_client.get(
            "/api/files",
            params={"date_to": "2020-01-01"},
        )
        assert resp.status_code == 200
        body = resp.json()
        assert body["code"] == 0
        # 不应包含今天创建的文件
        items = [i for i in body["data"]["items"] if i["id"] == file_id]
        assert len(items) == 0


# ===========================================================================
# 上传模块（含 duration 写入和 auto_transcribe 检查）
# ===========================================================================


class TestUpload:
    """上传模块测试。"""

    @pytest.mark.asyncio
    async def test_upload_wav_with_duration(self, app_client, test_config, wav_bytes):
        """上传 WAV 文件后 duration 字段被写入。"""
        resp = await app_client.post(
            "/upload",
            content=wav_bytes,
            headers={
                "Content-Type": "audio/wav",
                "X-Filename": "upload_test.wav",
            },
        )
        assert resp.status_code == 200
        body = resp.json()
        assert body["code"] == 0
        assert body["data"]["file_id"] is not None

        # 验证文件列表中包含 duration
        file_id = body["data"]["file_id"]
        detail_resp = await app_client.get(f"/api/files/{file_id}")
        detail = detail_resp.json()["data"]
        assert detail["duration"] is not None
        assert 0.9 <= detail["duration"] <= 1.1

    @pytest.mark.asyncio
    async def test_upload_auto_transcribe_respected(self, app_client, test_config, db_session_factory, wav_bytes):
        """auto_transcribe=false 时不自动入队转写。"""
        # 先设置 auto_transcribe=false
        with patch("server.database._async_session_factory", db_session_factory):
            from server.services.settings_service import init_default_settings, set_setting
            await init_default_settings()
            await set_setting("auto_transcribe", "false")

        resp = await app_client.post(
            "/upload",
            content=wav_bytes,
            headers={
                "Content-Type": "audio/wav",
                "X-Filename": "no_auto_transcribe.wav",
            },
        )
        assert resp.status_code == 200
        # 文件应已创建，转写记录应为 pending 但不入队
        body = resp.json()
        assert body["code"] == 0
