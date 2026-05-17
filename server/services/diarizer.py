"""ESP32 AI Recorder — 说话人分离服务。

封装 pyannote-audio 聚类调用；提供 is_available() 检测；降级时返回空结果。
"""

from __future__ import annotations

import asyncio
import logging
from typing import Any, Optional

logger = logging.getLogger(__name__)


def is_available() -> bool:
    """检测 pyannote-audio 是否可用。

    Returns:
        True 如果 pyannote-audio 可正常导入，否则 False。
    """
    try:
        import pyannote.audio  # noqa: F401
        return True
    except ImportError:
        return False


async def diarize(audio_path: str, num_speakers: int = 2) -> list[dict]:
    """执行说话人分离。

    Args:
        audio_path: 音频文件路径。
        num_speakers: 预期说话人数量，默认 2。

    Returns:
        说话人段落列表: [{"speaker": "S1", "start": 0.0, "end": 5.2}, ...]
        不可用时返回空列表 []。
    """
    if not is_available():
        logger.warning("pyannote-audio not available, skipping diarization")
        return []

    try:
        result = await asyncio.to_thread(
            _run_diarization, audio_path, num_speakers
        )
        return result
    except Exception as exc:
        logger.error("Diarization failed: %s", exc)
        return []


def _run_diarization(audio_path: str, num_speakers: int = 2) -> list[dict]:
    """同步执行 pyannote 说话人分离（在子线程中调用）。

    Args:
        audio_path: 音频文件路径。
        num_speakers: 预期说话人数量。

    Returns:
        说话人段落列表。
    """
    import os
    from pyannote.audio import Pipeline

    hf_token = os.environ.get("HF_TOKEN")
    if not hf_token:
        logger.warning("HF_TOKEN not set, cannot use pyannote pipeline")
        return []

    pipeline = Pipeline.from_pretrained(
        "pyannote/speaker-diarization-3.1",
        use_auth_token=hf_token,
    )

    if pipeline is None:
        logger.error("Failed to load pyannote pipeline")
        return []

    diarization = pipeline(
        audio_path,
        num_speakers=num_speakers,
    )

    # 将 SPEAKER_00 映射为 S1, SPEAKER_01 映射为 S2, ...
    speaker_map: dict[str, str] = {}
    speaker_counter = 1
    segments: list[dict] = []

    for turn, _, speaker in diarization.itertracks(yield_label=True):
        if speaker not in speaker_map:
            speaker_map[speaker] = f"S{speaker_counter}"
            speaker_counter += 1
        segments.append({
            "speaker": speaker_map[speaker],
            "start": round(turn.start, 3),
            "end": round(turn.end, 3),
        })

    return segments


def align_speakers_segments(
    speaker_segments: list[dict],
    whisper_segments: list[dict],
) -> list[dict]:
    """对齐 pyannote 输出与 whisper segments。

    对每个 whisper segment，找到时间重叠最大的说话人，然后生成 speakers JSON。

    Args:
        speaker_segments: pyannote 输出的说话人段落列表，
            格式: [{"speaker": "S1", "start": 0.0, "end": 5.2}, ...]
        whisper_segments: whisper 输出的分段列表，
            格式: [{"start": 0.0, "end": 5.2, "text": "..."}, ...]

    Returns:
        speakers JSON: [{"id": "S1", "name": "S1", "segment_indices": [0, 2, 5]}, ...]
    """
    if not speaker_segments or not whisper_segments:
        return []

    # 为每个 whisper segment 找到最匹配的说话人
    segment_speaker_map: dict[int, str] = {}

    for idx, ws in enumerate(whisper_segments):
        ws_start = ws.get("start", 0.0)
        ws_end = ws.get("end", 0.0)
        ws_duration = ws_end - ws_start
        if ws_duration <= 0:
            continue

        best_speaker: Optional[str] = None
        best_overlap = 0.0

        for ss in speaker_segments:
            ss_start = ss.get("start", 0.0)
            ss_end = ss.get("end", 0.0)
            ss_speaker = ss.get("speaker", "S1")

            # 计算重叠时间
            overlap_start = max(ws_start, ss_start)
            overlap_end = min(ws_end, ss_end)
            overlap = overlap_end - overlap_start

            if overlap > best_overlap:
                best_overlap = overlap
                best_speaker = ss_speaker

        if best_speaker is not None:
            segment_speaker_map[idx] = best_speaker

    # 按 speaker 聚合 segment_indices
    speaker_indices: dict[str, list[int]] = {}
    for idx, speaker in segment_speaker_map.items():
        if speaker not in speaker_indices:
            speaker_indices[speaker] = []
        speaker_indices[speaker].append(idx)

    # 生成 speakers JSON
    speakers: list[dict] = []
    for speaker_id, indices in sorted(speaker_indices.items()):
        speakers.append({
            "id": speaker_id,
            "name": speaker_id,
            "segment_indices": indices,
        })

    return speakers
