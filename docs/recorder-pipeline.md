# Recorder Pipeline — ESP32 AI Recorder

> **Version:** v0.4 (VFS-only) | **Updated:** 2026-05-14
> **Scope:** Recording pipeline architecture from I2S mic to SD card WAV file
> **Architecture Fix:** audio_task = sole I2S owner, no suspend/resume
> **Key change:** Directory lifecycle now owned by storage.c (NOT recorder.c)
> **Path strategy:** VFS/POSIX only (/sdcard/...) — see storage-path-policy.md

---

## 1. Pipeline Overview

The recording pipeline transforms raw I2S audio into playable WAV files on SD card.

```
┌──────────────┐    I2S DMA     ┌──────────────┐   RingBuf   ┌───────────────┐   fwrite    ┌───────────┐
│  INMP441     │ ────────────▶ │  audio.c     │ ─────────▶ │  ringbuf      │ ─────────▶ │  SD card  │
│  (I2S Mic)   │               │  audio_read()│            │  (32KB)       │             │  FAT32    │
│              │               │              │            │               │             │           │
│  BCLK GPIO4  │               │  Extracts    │            │               │             │ /sdcard/  │
│  WS   GPIO5  │               │  mono PCM    │            │               │             │ recordings│
│  DIN  GPIO6  │               │  16kHz mono  │            │               │             │           │
└──────────────┘               └──────────────┘            └───────────────┘             └───────────┘
                                        │
                    ┌───────────────────┴───────────────────┐
                    │         audio_task (永远运行)           │
                    │                                          │
                    │  IDLE:      audio_read → RMS (DEBUG)     │
                    │  RECORDING: audio_read → ringbuf_send() │
                    │                                          │
                    │  规则：audio_task = sole I2S owner      │
                    │        禁止 suspend/resume               │
                    │        ringbuf_enabled 控制数据流向      │
                    └──────────────────────────────────────────┘
```

**Phase 1 scope:** No streaming, no upload, no VAD — just I2S → SD card WAV.

---

## 2. Data Format

| Property | Value |
|----------|-------|
| Sample rate | 16000 Hz |
| Bit depth | 16-bit PCM |
| Channels | 1 (mono) |
| I2S format | Philips standard (1-bit delay), MSB left |
| Output format | WAV (RIFF) on FAT32 SD card |

---

## 3. Data Flow Steps

### Step 1: I2S DMA Capture (audio.c)

- ESP32 I2S peripheral receives audio from INMP441 via GPIO4/5/6
- DMA transfers data into `s_i2s_buf` (6KB, 6 descriptors × 256 frames)
- `audio_read()` pulls from DMA buffer, converts to 16-bit mono PCM
- **audio_task 永远运行**，ringbuf_enabled 控制是否向 ringbuf 推送

### Step 1.5: Directory Initialization

**Storage subsystem owns directory lifecycle — recorder does NOT create directories.**

文件系统目录在 `storage_mount()` 成功后由 `storage_ensure_directories()` 自动创建：

```
/sdcard/
├── recordings/      ← WAV 录音文件（auto-created: mkdir("/sdcard/recordings")）
├── uploaded/        ← 已上传文件（auto-created）
├── upload_queue/    ← 待上传队列（auto-created）
├── temp/           ← 临时文件（auto-created）
└── logs/           ← 日志文件（auto-created）
```

**保证链：**

```
storage_mount()
  → storage_ensure_directories()   // mkdir("/sdcard/recordings"), etc.
  → storage_validate_layout()      // opendir() 验证, 打印 [OK] 日志
  → recorder_init()               // session_init() 扫描 /sdcard/recordings/
  → recorder_start()              // fopen("/sdcard/recordings/REC_SESSION_xxxx.wav")
```

**recorder.c 的职责**：仅 `fopen()` 写文件，不调用 `mkdir`、`opendir`。
**storage.c 的职责**：mount、目录生命周期、布局验证。

See `docs/storage-path-policy.md` for the complete path layer separation rules.

### Step 2: Ring Buffer (ringbuf.c)

- When `ringbuf_enabled == true`, `audio_read()` also sends PCM to ring buffer
- Ring buffer decouples `audio_task` and `recorder_task`
- Size: 32KB (~2 seconds of audio)
- Overflow: old samples dropped, counter incremented

### Step 3: Recorder Task (recorder.c)

- `recorder_task` runs continuously once created
- When recording: reads from ring buffer in 100ms batches (1600 samples)
- Accumulates 32 batches (~3.2 seconds) before SD write
- On stop: flushes all remaining samples, updates WAV header

### Step 4: WAV File Write (recorder.c)

- Batch write: `fwrite()` with 32KB buffer
- Checkpoint: `fflush()` every 30 seconds (data safe on SD)
- On stop: `fflush()` + `fsync()` + close file

---

## 4. WAV File Format

### RIFF Header (44 bytes)

| Offset | Field | Size | Value |
|--------|-------|------|-------|
| 0 | ChunkID | 4 | "RIFF" |
| 4 | ChunkSize | 4 | file_size - 8 |
| 8 | Format | 4 | "WAVE" |
| 12 | Subchunk1ID | 4 | "fmt " |
| 16 | Subchunk1Size | 4 | 16 |
| 20 | AudioFormat | 2 | 1 (PCM) |
| 22 | NumChannels | 2 | 1 (mono) |
| 24 | SampleRate | 4 | 16000 |
| 28 | ByteRate | 4 | 32000 (16000×2) |
| 32 | BlockAlign | 2 | 2 (channels×bits/8) |
| 34 | BitsPerSample | 2 | 16 |
| 36 | Subchunk2ID | 4 | "data" |
| 40 | Subchunk2Size | 4 | num_samples × 2 |

### Header Write Strategy

```
recorder_start():
  1. fopen() file
  2. Write 44-byte placeholder header (data_size = 0)
  3. Start accumulating samples in batch buffer

recorder_stop():
  1. Read remaining samples from ring buffer
  2. fwrite() any remaining batch buffer
  3. fflush() + fsync()
  4. Seek to offset 40 in file
  5. Write real Subchunk2Size
  6. Seek to offset 4
  7. Write real ChunkSize
  8. fflush() + fclose()
```

---

## 5. File Naming

### Phase 1: Session-based naming

Format: `REC_SESSION_XXXX.wav` where XXXX is a 4-digit zero-padded number.

Examples: `REC_SESSION_0001.wav`, `REC_SESSION_0002.wav` ...

### Session Initialization

On `recorder_init()`, scan `/sdcard/recordings/` for existing `REC_SESSION_*.wav` files. Parse the 4-digit number from each filename. Set `s_next_session_id = max(number) + 1`.

If no files found, start at `0001`.

### Directory Structure

```
/sdcard/
└── recordings/
    ├── REC_SESSION_0001.wav
    ├── REC_SESSION_0002.wav
    └── REC_SESSION_0003.wav
```

**Note**: `recordings/` directory is created and validated by `storage.c` (not `recorder.c`).
Session scanning uses `opendir("/sdcard/recordings")` (POSIX/VFS).

---

## 6. Batch Write Strategy

### Why Batch Writes?

SD card writes are slow (~10ms per sector). Writing per sample (16kHz = 16,000 writes/sec) would overwhelm the SD card.

### Batch Parameters

| Parameter | Value | Effect |
|-----------|-------|--------|
| `RECORDER_BUF_SAMPLES` | 1600 | 100ms audio per read |
| `RECORDER_BATCH_SIZE` | 32 | 32 × 1600 = 51,200 samples = ~3.2s per batch |
| `RECORDER_CHECKPOINT_MS` | 30000 | Force fflush() every 30s |

### Write Flow

```
recorder_task loop:
  1. ringbuf_receive(timeout=200ms) → batch buffer
  2. batch_count++
  3. if batch_count >= 32 OR checkpoint_timer >= 30s:
       fwrite(batch_buffer, 1, batch_size, f)
       fflush(f)   // only on checkpoint
       batch_count = 0
       reset checkpoint timer
  4. if stopped:
       write remaining batch
       finalize WAV header
       fflush() + fsync() + fclose()
```

---

## 7. Error Handling

| Condition | Action |
|-----------|--------|
| Ring buffer overflow | Increment `overflow_count`, drop oldest samples, continue |
| `fwrite()` fails | Increment `write_failures`, log error, continue recording |
| SD card full | Stop recording gracefully, log `SD_FULL`, enter ERROR state |
| `recorder_stop()` called while ringbuf empty | Wait up to 500ms for remaining samples |
| WAV header write fails on stop | Log error, file may be corrupt — log warning |

---

## 8. Resource Usage

| Resource | Usage |
|----------|-------|
| CPU | `audio_task`: ~0% (100ms delay between reads) |
| | `recorder_task`: ~5-10% (SD writes are the bottleneck) |
| RAM | Ring buffer: 32KB |
| | Batch buffer: 3.2KB (1600 × 2 bytes) |
| | Task stack: 8KB |
| SD bandwidth | ~32000 bytes/sec (16kHz × 2 bytes) |

---

## 9. What's NOT in Phase 1

| Feature | Why excluded |
|---------|-------------|
| VAD (voice activity detection) | Complexity; continuous recording is fine for MVP |
| Opus / MP3 compression | CPU overhead; raw PCM is acceptable for short recordings |
| Streaming upload | Upload is Phase 2 |
| Multi-file recording | One file per session, restart on each button press |
| Recording pause | Not required for MVP |

---

*Last updated: 2026-05-14*
