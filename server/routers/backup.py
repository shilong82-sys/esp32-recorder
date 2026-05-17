"""ESP32 AI Recorder — 备份路由。

POST /api/backup/export — 导出备份包（tar.gz）
POST /api/backup/import — 导入备份包（tar.gz）
"""

import io
import json
import logging
import os
import shutil
import subprocess
import tempfile
from datetime import datetime, timezone

from fastapi import APIRouter, Depends, UploadFile, File as FastAPIFile
from fastapi.responses import StreamingResponse
from sqlalchemy import select
from sqlalchemy.ext.asyncio import AsyncSession

from ..config import get_config
from ..database import _async_session_factory, get_session
from ..models import File
from ..schemas import ApiResponse, ErrorCode

logger = logging.getLogger(__name__)

router = APIRouter(prefix="/api")


@router.post("/backup/export")
async def export_backup(
    session: AsyncSession = Depends(get_session),
) -> StreamingResponse:
    """导出备份包（.tar.gz）。

    包含 SQLite dump + WAV 文件 + 转写文本 + meta.json。
    """
    config = get_config()

    # 创建临时目录
    tmp_dir = tempfile.mkdtemp(prefix="recorder-backup-")

    try:
        # 1. 导出 SQLite dump
        dump_path = os.path.join(tmp_dir, "dump.sql")
        await _dump_sqlite(config.db_path, dump_path)

        # 2. 复制 received/ 目录
        received_dst = os.path.join(tmp_dir, "received")
        if os.path.isdir(config.received_dir):
            shutil.copytree(config.received_dir, received_dst)

        # 3. 复制 transcripts/ 目录
        transcripts_dst = os.path.join(tmp_dir, "transcripts")
        if os.path.isdir(config.transcripts_dir):
            shutil.copytree(config.transcripts_dir, transcripts_dst)

        # 4. 生成 meta.json
        file_count = 0
        db_size = 0
        if os.path.exists(config.db_path):
            db_size = os.path.getsize(config.db_path)

        result = await session.execute(select(File))
        file_count = len(result.scalars().all())

        meta = {
            "version": "0.5.0",
            "exported_at": datetime.now(timezone.utc).isoformat(),
            "file_count": file_count,
            "db_size_bytes": db_size,
        }
        meta_path = os.path.join(tmp_dir, "meta.json")
        with open(meta_path, "w", encoding="utf-8") as f:
            json.dump(meta, f, indent=2, ensure_ascii=False)

        # 5. 打包为 tar.gz
        timestamp = datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S")
        archive_name = f"recorder-backup-{timestamp}"

        # 使用流式 tar.gz 输出
        tar_buffer = io.BytesIO()
        _create_tar_gz(tmp_dir, archive_name, tar_buffer)
        tar_buffer.seek(0)

        # 清理临时目录
        shutil.rmtree(tmp_dir, ignore_errors=True)

        filename = f"{archive_name}.tar.gz"
        return StreamingResponse(
            tar_buffer,
            media_type="application/gzip",
            headers={
                "Content-Disposition": f'attachment; filename="{filename}"',
            },
        )

    except Exception as exc:
        # 清理临时目录
        shutil.rmtree(tmp_dir, ignore_errors=True)
        logger.error("Backup export failed: %s", exc)
        raise


@router.post("/backup/import", response_model=ApiResponse)
async def import_backup(
    file: UploadFile = FastAPIFile(..., description="备份文件 (.tar.gz)"),
) -> ApiResponse:
    """导入备份包（.tar.gz）。

    解压 → 恢复数据库 → 复制文件。
    """
    config = get_config()
    tmp_dir = tempfile.mkdtemp(prefix="recorder-import-")

    try:
        # 1. 保存上传的文件到临时位置
        upload_path = os.path.join(tmp_dir, "upload.tar.gz")
        with open(upload_path, "wb") as f:
            content = await file.read()
            f.write(content)

        # 2. 解压 tar.gz
        extract_dir = os.path.join(tmp_dir, "extracted")
        os.makedirs(extract_dir, exist_ok=True)
        _extract_tar_gz(upload_path, extract_dir)

        # 查找解压后的根目录（可能是直接在 extract_dir 下，也可能有一层子目录）
        src_dir = extract_dir
        entries = os.listdir(extract_dir)
        if len(entries) == 1 and os.path.isdir(os.path.join(extract_dir, entries[0])):
            src_dir = os.path.join(extract_dir, entries[0])

        # 3. 恢复数据库
        dump_path = os.path.join(src_dir, "dump.sql")
        if os.path.exists(dump_path):
            await _restore_sqlite(config.db_path, dump_path)

        # 4. 复制文件
        received_src = os.path.join(src_dir, "received")
        if os.path.isdir(received_src):
            os.makedirs(config.received_dir, exist_ok=True)
            for filename in os.listdir(received_src):
                src_file = os.path.join(received_src, filename)
                dst_file = os.path.join(config.received_dir, filename)
                if not os.path.exists(dst_file):
                    shutil.copy2(src_file, dst_file)

        transcripts_src = os.path.join(src_dir, "transcripts")
        if os.path.isdir(transcripts_src):
            os.makedirs(config.transcripts_dir, exist_ok=True)
            for filename in os.listdir(transcripts_src):
                src_file = os.path.join(transcripts_src, filename)
                dst_file = os.path.join(config.transcripts_dir, filename)
                if not os.path.exists(dst_file):
                    shutil.copy2(src_file, dst_file)

        logger.info("Backup import completed successfully")

        # 清理
        shutil.rmtree(tmp_dir, ignore_errors=True)

        return ApiResponse(
            data={"message": "Backup imported successfully. Please restart the server to apply changes."},
        )

    except Exception as exc:
        shutil.rmtree(tmp_dir, ignore_errors=True)
        logger.error("Backup import failed: %s", exc)
        return ApiResponse(
            code=ErrorCode.INTERNAL_ERROR,
            message=f"Backup import failed: {exc}",
            data=None,
        )


# ---------------------------------------------------------------------------
# 辅助函数
# ---------------------------------------------------------------------------

async def _dump_sqlite(db_path: str, dump_path: str) -> None:
    """导出 SQLite 数据库为 SQL dump。

    Args:
        db_path: 数据库文件路径。
        dump_path: dump 文件输出路径。
    """
    import asyncio

    proc = await asyncio.create_subprocess_exec(
        "sqlite3", db_path, ".dump",
        stdout=open(dump_path, "w"),
        stderr=asyncio.subprocess.PIPE,
    )
    await proc.wait()
    if proc.returncode != 0:
        logger.warning("SQLite dump returned non-zero exit code: %d", proc.returncode)


async def _restore_sqlite(db_path: str, dump_path: str) -> None:
    """从 SQL dump 恢复 SQLite 数据库。

    Args:
        db_path: 数据库文件路径。
        dump_path: dump 文件路径。
    """
    import asyncio

    # 备份现有数据库
    if os.path.exists(db_path):
        backup_path = db_path + ".pre-import-bak"
        shutil.copy2(db_path, backup_path)

    # 删除现有数据库
    if os.path.exists(db_path):
        os.remove(db_path)

    proc = await asyncio.create_subprocess_exec(
        "sqlite3", db_path,
        stdin=open(dump_path, "r"),
        stderr=asyncio.subprocess.PIPE,
    )
    await proc.wait()
    if proc.returncode != 0:
        logger.error("SQLite restore failed with exit code: %d", proc.returncode)


def _create_tar_gz(source_dir: str, archive_name: str, buffer: io.BytesIO) -> None:
    """将目录打包为 tar.gz 写入 buffer。

    Args:
        source_dir: 源目录。
        archive_name: 归档内部目录名。
        buffer: 输出 buffer。
    """
    import tarfile

    with tarfile.open(fileobj=buffer, mode="w:gz") as tar:
        for item in os.listdir(source_dir):
            item_path = os.path.join(source_dir, item)
            tar.add(item_path, arcname=os.path.join(archive_name, item))


def _extract_tar_gz(archive_path: str, dest_dir: str) -> None:
    """解压 tar.gz 文件。

    Args:
        archive_path: 压缩文件路径。
        dest_dir: 解压目标目录。
    """
    import tarfile

    with tarfile.open(archive_path, "r:gz") as tar:
        tar.extractall(dest_dir)
