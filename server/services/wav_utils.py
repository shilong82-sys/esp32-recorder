"""ESP32 AI Recorder — WAV 文件工具。

从 WAV RIFF header 读取音频时长（秒），无需解码音频数据。
使用 Python 标准库 struct，无额外依赖。
"""

import logging
import struct
from typing import Optional

logger = logging.getLogger(__name__)


def read_wav_duration(path: str) -> Optional[float]:
    """从 WAV 文件 header 读取音频时长（秒）。

    仅读取 RIFF header 中的 byte_rate 和 data_chunk_size，
    不解码音频数据，性能极高。

    Args:
        path: WAV 文件的绝对路径。

    Returns:
        时长（秒），读取失败时返回 None。
    """
    try:
        with open(path, "rb") as f:
            # 读取 RIFF header（前 44 字节包含固定格式信息）
            header = f.read(44)

            # 至少需要 44 字节才是合法的 WAV header
            if len(header) < 44:
                logger.warning("WAV file too short: %s (%d bytes)", path, len(header))
                return None

            # 验证 RIFF 标识
            riff_tag = header[0:4]
            if riff_tag != b"RIFF":
                logger.warning("Not a RIFF file: %s", path)
                return None

            # 验证 WAVE 格式
            wave_tag = header[8:12]
            if wave_tag != b"WAVE":
                logger.warning("Not a WAVE file: %s", path)
                return None

            # 查找 fmt 子块和 data 子块
            # WAV 文件的子块可能不是固定的偏移量，需要搜索
            offset = 12
            byte_rate: Optional[int] = None
            data_chunk_size: Optional[int] = None

            while offset < len(header):
                # 不够读取子块头部
                if offset + 8 > len(header):
                    break

                chunk_id = header[offset:offset + 4]
                chunk_size = struct.unpack_from("<I", header, offset + 4)[0]

                if chunk_id == b"fmt ":
                    # 读取 byte_rate（偏移量在 fmt 块内：audio_format(2)+channels(2)+sample_rate(4)+byte_rate(4)）
                    fmt_data_start = offset + 8
                    if fmt_data_start + 16 <= len(header):
                        byte_rate = struct.unpack_from("<I", header, fmt_data_start + 8)[0]

                elif chunk_id == b"data":
                    data_chunk_size = chunk_size

                # 移动到下一个子块（8 字节头部 + chunk_size）
                # 对齐到 2 字节边界
                next_offset = offset + 8 + chunk_size
                if chunk_size % 2 != 0:
                    next_offset += 1
                offset = next_offset

            # 如果固定偏移方案没读到（子块顺序不标准），用固定偏移做 fallback
            if byte_rate is None:
                # 标准 PCM WAV：byte_rate 在 offset 28
                if len(header) >= 32:
                    byte_rate = struct.unpack_from("<I", header, 28)[0]

            if data_chunk_size is None:
                # 标准 PCM WAV：data_chunk_size 在 offset 40
                if len(header) >= 44:
                    # 验证 data 标签
                    data_tag = header[36:40]
                    if data_tag == b"data":
                        data_chunk_size = struct.unpack_from("<I", header, 40)[0]

            if byte_rate is None or data_chunk_size is None:
                logger.warning(
                    "Could not parse WAV header: %s (byte_rate=%s, data_size=%s)",
                    path, byte_rate, data_chunk_size,
                )
                return None

            if byte_rate == 0:
                logger.warning("WAV byte_rate is 0: %s", path)
                return None

            duration = data_chunk_size / byte_rate
            return duration

    except FileNotFoundError:
        logger.warning("WAV file not found: %s", path)
        return None
    except OSError as exc:
        logger.warning("Failed to read WAV file %s: %s", path, exc)
        return None
    except struct.error as exc:
        logger.warning("Failed to parse WAV header %s: %s", path, exc)
        return None
