/**
 * @file recorder.c
 * @brief Recorder component - Real WAV recording pipeline
 *
 * Responsibilities:
 * - Manages the recorder FreeRTOS task
 * - Reads audio from ring buffer
 * - Writes WAV files to SD card
 * - Handles session naming and WAV header management
 * - Publishes recording events
 *
 * Architecture:
 * - recorder_task: runs continuously, reads from ringbuf, batch writes to SD
 * - recorder_start/stop: called from app_main via state machine events
 * - No I2S calls here — audio flows through ringbuf from audio.c
 *
 * Directory Ownership:
 * - recorder.c does NOT create or check directories
 * - Directory lifecycle is owned by storage.c (storage_ensure_directories)
 * - See docs/storage-path-policy.md for path strategy (POSIX/VFS vs FatFs-native)
 */

#include "recorder.h"
#include "storage.h"
#include "ringbuf.h"
#include "event_bus.h"
#include <dirent.h>       /* POSIX: opendir, readdir, closedir */
#include <sys/stat.h>     /* POSIX: stat */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


/*======================================================================
 * Constants
 *======================================================================*/
static const char *TAG = "recorder";

#define WAV_HEADER_SIZE         (44)       /* Standard WAV RIFF header */
#define SAMPLE_RATE             (16000)    /* Hz */
#define BITS_PER_SAMPLE         (16)       /* PCM 16-bit */
#define CHANNELS                (1)        /* Mono */
#define RECORDER_TASK_STACK     (8192)
#define RECORDER_TASK_PRIORITY  (3)       /* Same as audio_task */
#define RECORDER_TASK_CORE      (0)

/* Read from ring buffer every ~100ms worth of samples */
#define RECORDER_READ_SAMPLES   (1600)     /* 16000 Hz × 0.1s = 1600 samples */
/* Batch 32 reads before SD write (~3.2 seconds) */
#define RECORDER_BATCH_FRAMES   (32)
/* Checkpoint sync every 30 seconds */
#define RECORDER_CHECKPOINT_MS  (30000)
/* Stats report every 5 seconds */
#define RECORDER_STATS_MS       (5000)
/* Ring buffer capacity: 32KB = ~2 seconds of audio */
#define RINGBUF_CAPACITY_BYTES  (32768)

/*======================================================================
 * WAV File Format Helpers
 *======================================================================*/

/**
 * Write a WAV RIFF header with placeholder data size (0).
 * File must be opened before calling.
 */
static void wav_write_header(FILE *f)
{
    uint32_t data_size = 0;
    uint32_t file_size = data_size + 36;
    uint32_t byte_rate = SAMPLE_RATE * CHANNELS * BITS_PER_SAMPLE / 8;
    uint16_t block_align = CHANNELS * BITS_PER_SAMPLE / 8;

    /* RIFF chunk */
    fwrite("RIFF", 1, 4, f);
    fwrite(&file_size, 4, 1, f);
    fwrite("WAVE", 1, 4, f);

    /* fmt subchunk */
    fwrite("fmt ", 1, 4, f);
    uint32_t fmt_size = 16;
    fwrite(&fmt_size, 4, 1, f);
    uint16_t audio_format = 1; /* PCM */
    fwrite(&audio_format, 2, 1, f);
    uint16_t channels = CHANNELS;
    fwrite(&channels, 2, 1, f);
    uint32_t sample_rate = SAMPLE_RATE;
    fwrite(&sample_rate, 4, 1, f);
    fwrite(&byte_rate, 4, 1, f);
    fwrite(&block_align, 2, 1, f);
    uint16_t bits = BITS_PER_SAMPLE;
    fwrite(&bits, 2, 1, f);

    /* data subchunk */
    fwrite("data", 1, 4, f);
    fwrite(&data_size, 4, 1, f);
}

/**
 * Update the WAV header with actual sizes.
 * Seeks to positions 4 and 40 in the file and overwrites the sizes.
 */
static void wav_finalize(FILE *f, uint32_t total_samples)
{
    uint32_t data_size = total_samples * sizeof(int16_t);
    uint32_t file_size = data_size + 36;

    /* Update ChunkSize at offset 4 */
    fseek(f, 4, SEEK_SET);
    fwrite(&file_size, 4, 1, f);

    /* Update Subchunk2Size at offset 40 */
    fseek(f, 40, SEEK_SET);
    fwrite(&data_size, 4, 1, f);
}

/*======================================================================
 * Session Management
 *======================================================================*/

static int s_next_session_id = 0;

/**
 * Scan recordings/ directory for existing REC_SESSION_XXXX.wav files.
 * Set s_next_session_id to max found + 1.
 *
 * Uses opendir()/readdir() (POSIX/VFS) via storage_build_vfs_path().
 * Directory is guaranteed to exist by storage_ensure_directories() at mount time.
 * recorder.c does NOT create directories — storage owns directory lifecycle.
 * See docs/storage-path-policy.md.
 */
static void session_init(void)
{
    /* Build VFS path: "/sdcard/recordings" */
    char vfs_path[64];
    storage_build_vfs_path(vfs_path, sizeof(vfs_path),
                             STORAGE_PATH_RECORDINGS, NULL);

    DIR *dir = opendir(vfs_path);
    if (dir == NULL) {
        /* Directory missing — this should not happen if storage_mount() succeeded.
         * Log error but don't crash; session numbering will start at 0001. */
        ESP_LOGE(TAG, "session_init: opendir(%s) failed: %d", vfs_path, errno);
        ESP_LOGE(TAG, "  Directory may be missing — run storage_validate_layout()");
        s_next_session_id = 1;
        return;
    }

    /* Scan for existing session files */
    int max_id = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_DIR) {
            continue;
        }
        /* Look for REC_SESSION_XXXX.wav pattern */
        unsigned int id = 0;
        if (sscanf(entry->d_name, "REC_SESSION_%4u.wav", &id) == 1) {
            if ((int)id > max_id) {
                max_id = (int)id;
            }
        }
    }
    closedir(dir);

    s_next_session_id = max_id + 1;
    ESP_LOGI(TAG, "Session counter initialized: next = %04d", s_next_session_id);
}

/**
 * Generate the next session VFS path (for fopen).
 * Uses storage_build_vfs_path() — no hardcoded "/sdcard" or "recordings".
 */
static void session_generate_path(char *buf, size_t size)
{
    char filename[64];
    snprintf(filename, sizeof(filename), "REC_SESSION_%04d.wav", s_next_session_id);
    storage_build_vfs_path(buf, size, STORAGE_PATH_RECORDINGS, filename);
    s_next_session_id++;
}

/*======================================================================
 * Recording State
 *======================================================================*/

typedef struct {
    FILE *file;
    bool active;
    uint32_t total_samples;
    int64_t recording_start_us;
    char filepath[256];
    int32_t batch_count;        /* Frames accumulated since last write */
    int64_t last_checkpoint_us; /* Last fflush() timestamp */
    int64_t last_stats_us;      /* Last stats print timestamp */
} recording_state_t;

static recording_state_t s_rec = {
    .file = NULL,
    .active = false,
    .total_samples = 0,
    .batch_count = 0,
};

/*======================================================================
 * Statistics
 *======================================================================*/

typedef struct {
    uint32_t overflow_count;
    uint32_t dropped_samples;
    uint32_t write_failures;
    uint32_t total_writes;
    int64_t max_write_us;
    int64_t total_write_us;
} rec_stats_t;

static rec_stats_t s_stats = {0};

/*======================================================================
 * recorder_task
 *======================================================================*/

static void recorder_task(void *arg)
{
    (void)arg;

    int16_t read_buf[RECORDER_READ_SAMPLES];

    ESP_LOGI(TAG, "recorder_task started");

    while (1) {
        if (!s_rec.active) {
            /* Not recording — just wait and yield */
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        /* Read from ring buffer */
        size_t got = ringbuf_receive(read_buf, RECORDER_READ_SAMPLES, 200);

        if (got == 0) {
            /* No data available yet — yield and retry */
            /* This can happen if audio_read hasn't populated ringbuf yet */
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        /* Write to batch buffer (accumulate) */
        if (s_rec.file != NULL) {
            size_t written = fwrite(read_buf, sizeof(int16_t), got, s_rec.file);
            if (written != got) {
                s_stats.write_failures++;
                ESP_LOGE(TAG, "fwrite failed (errno=%d)", errno);
            } else {
                s_rec.total_samples += got;
                s_rec.batch_count++;
            }
        }

        /* Check for batch flush */
        int64_t now_us = esp_timer_get_time();
        int64_t elapsed_checkpoint = (now_us - s_rec.last_checkpoint_us) / 1000;

        if (s_rec.batch_count >= RECORDER_BATCH_FRAMES || elapsed_checkpoint >= RECORDER_CHECKPOINT_MS) {
            if (s_rec.file != NULL) {
                /* Batch flush — sync to SD but don't close */
                int64_t write_start = esp_timer_get_time();
                int ret = fflush(s_rec.file);
                int64_t write_us = esp_timer_get_time() - write_start;

                if (ret != 0) {
                    s_stats.write_failures++;
                    ESP_LOGE(TAG, "fflush failed");
                } else {
                    s_stats.total_writes++;
                    s_stats.total_write_us += write_us;
                    if (write_us > s_stats.max_write_us) {
                        s_stats.max_write_us = write_us;
                    }
                }

                if (s_rec.batch_count >= RECORDER_BATCH_FRAMES) {
                    s_rec.batch_count = 0;
                }
                if (elapsed_checkpoint >= RECORDER_CHECKPOINT_MS) {
                    s_rec.last_checkpoint_us = now_us;
                }
            }
        }

        /* Check for stats print (every 5 seconds) */
        int64_t elapsed_stats = (now_us - s_rec.last_stats_us) / 1000;
        if (elapsed_stats >= RECORDER_STATS_MS) {
            ringbuf_stats_t rb_stats;
            ringbuf_get_stats(&rb_stats);

            int64_t avg_write_us = s_stats.total_writes > 0
                ? s_stats.total_write_us / s_stats.total_writes : 0;

            ESP_LOGI(TAG, "[Stats %ds] samples=%lu overflow=%lu dropped=%lu "
                     "write_fail=%lu avg_write_us=%lld max_write_us=%lld",
                     (int)(elapsed_stats / 1000),
                     (unsigned long)s_rec.total_samples,
                     (unsigned long)rb_stats.overflow_count,
                     (unsigned long)rb_stats.total_dropped_samples,
                     (unsigned long)s_stats.write_failures,
                     (long long)avg_write_us,
                     (long long)s_stats.max_write_us);

            s_rec.last_stats_us = now_us;
        }
    }
}

/*======================================================================
 * Public API
 *======================================================================*/

esp_err_t recorder_init(const recorder_config_t *config)
{
    (void)config;

    /* Initialize ring buffer */
    esp_err_t ret = ringbuf_init(RINGBUF_CAPACITY_BYTES);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ringbuf_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Initialize session counter */
    session_init();

    /* Create recorder task */
    BaseType_t created = xTaskCreatePinnedToCore(
        &recorder_task,
        "recorder",
        RECORDER_TASK_STACK,
        NULL,
        RECORDER_TASK_PRIORITY,
        NULL,
        RECORDER_TASK_CORE
    );

    if (created != pdTRUE) {
        ESP_LOGE(TAG, "recorder_task create failed");
        return ESP_FAIL;
    }

    /* Subscribe to state changes (optional — currently handled in app_main) */
    /* event_bus_subscribe(EVENT_STATE_CHANGED, on_state_changed, NULL); */

    ESP_LOGI(TAG, "Recorder initialized");
    ESP_LOGI(TAG, "  Sample rate: %d Hz", SAMPLE_RATE);
    ESP_LOGI(TAG, "  Bits/sample: %d", BITS_PER_SAMPLE);
    ESP_LOGI(TAG, "  Channels: %d", CHANNELS);
    ESP_LOGI(TAG, "  Ring buffer: %d bytes", RINGBUF_CAPACITY_BYTES);
    ESP_LOGI(TAG, "  Batch size: %d reads (~%.1fs)", RECORDER_BATCH_FRAMES,
             (float)RECORDER_BATCH_FRAMES * RECORDER_READ_SAMPLES / SAMPLE_RATE);

    return ESP_OK;
}

esp_err_t recorder_start(const char *filename)
{
    if (s_rec.active) {
        ESP_LOGW(TAG, "Already recording");
        return ESP_OK;
    }

    char path[256];
    if (filename != NULL && strlen(filename) > 0) {
        storage_build_vfs_path(path, sizeof(path), STORAGE_PATH_RECORDINGS, filename);
    } else {
        session_generate_path(path, sizeof(path));
    }

    /* Directory (/sdcard/recordings/) is guaranteed to exist by
     * storage_ensure_directories() called during storage_mount().
     * recorder.c does NOT create or check directories — this is storage's job.
     * See docs/storage-path-policy.md for path strategy details. */

    /* Open file for binary write (POSIX/VFS path: /sdcard/...) */
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file: %s", path);
        return ESP_FAIL;
    }

    /* Write placeholder WAV header */
    wav_write_header(f);

    /* Reset state */
    s_rec.file = f;
    s_rec.active = true;
    s_rec.total_samples = 0;
    s_rec.recording_start_us = esp_timer_get_time();
    s_rec.batch_count = 0;
    s_rec.last_checkpoint_us = esp_timer_get_time();
    s_rec.last_stats_us = esp_timer_get_time();
    snprintf(s_rec.filepath, sizeof(s_rec.filepath), "%s", path);

    /* Reset stats */
    memset(&s_stats, 0, sizeof(s_stats));
    ringbuf_reset_stats();

    /* Publish recording started event */
    event_bus_publish(EVENT_RECORDING_STARTED, NULL, 0);

    ESP_LOGI(TAG, "Recording started: %s", path);
    return ESP_OK;
}

esp_err_t recorder_stop(uint32_t *out_duration_ms)
{
    if (!s_rec.active) {
        ESP_LOGW(TAG, "Not recording");
        if (out_duration_ms) *out_duration_ms = 0;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Stopping recording...");

    /* Flush any remaining data in ring buffer */
    int16_t drain_buf[RECORDER_READ_SAMPLES];
    int drain_count = 0;
    while (1) {
        size_t got = ringbuf_receive(drain_buf, RECORDER_READ_SAMPLES, 50);
        if (got == 0) break;
        if (s_rec.file != NULL) {
            fwrite(drain_buf, sizeof(int16_t), got, s_rec.file);
            s_rec.total_samples += got;
        }
        drain_count++;
    }
    if (drain_count > 0) {
        ESP_LOGI(TAG, "Drained %d remaining frames from ringbuf", drain_count);
    }

    /* Finalize WAV header */
    if (s_rec.file != NULL) {
        wav_finalize(s_rec.file, s_rec.total_samples);

        /* Close file */
        int ret = fflush(s_rec.file);
        if (ret != 0) {
            ESP_LOGW(TAG, "fflush warning before close");
        }
        fclose(s_rec.file);
        s_rec.file = NULL;
    }

    /*
     * Step: Move to upload_queue
     * Only after fclose() confirms file is finalized on disk.
     * Rename failure → log error but do NOT crash; file stays in recordings/.
     * The uploader startup scan covers both upload_queue/ and recordings/.
     */
    const char *filename = strrchr(s_rec.filepath, '/');
    filename = filename ? filename + 1 : s_rec.filepath;
    esp_err_t rename_err = storage_rename_file(
        STORAGE_PATH_RECORDINGS, filename,
        STORAGE_PATH_UPLOAD_QUEUE, filename);
    if (rename_err != ESP_OK) {
        ESP_LOGE(TAG, "[QUEUE] rename to upload_queue FAILED (%s) — file stays in recordings/",
                 esp_err_to_name(rename_err));
        ESP_LOGE(TAG, "[QUEUE] filename: %s", filename);
    } else {
        ESP_LOGI(TAG, "[QUEUE] -> upload_queue/%s", filename);
    }

    /* Calculate duration */
    int64_t now_us = esp_timer_get_time();
    uint32_t duration_ms = (uint32_t)((now_us - s_rec.recording_start_us) / 1000);

    /* Final stats */
    ringbuf_stats_t rb_stats;
    ringbuf_get_stats(&rb_stats);

    ESP_LOGI(TAG, "Recording stopped");
    ESP_LOGI(TAG, "  File: %s", s_rec.filepath);
    ESP_LOGI(TAG, "  Duration: %lu ms", (unsigned long)duration_ms);
    ESP_LOGI(TAG, "  Total samples: %lu", (unsigned long)s_rec.total_samples);
    ESP_LOGI(TAG, "  File size: %lu bytes", (unsigned long)(s_rec.total_samples * 2 + WAV_HEADER_SIZE));
    ESP_LOGI(TAG, "  Ringbuf overflows: %lu", (unsigned long)rb_stats.overflow_count);
    ESP_LOGI(TAG, "  Write failures: %lu", (unsigned long)s_stats.write_failures);

    /* Reset state */
    s_rec.active = false;

    if (out_duration_ms) {
        *out_duration_ms = duration_ms;
    }

    /* Publish recording stopped event */
    event_bus_publish(EVENT_RECORDING_STOPPED, NULL, 0);

    return ESP_OK;
}

bool recorder_is_recording(void)
{
    return s_rec.active;
}

esp_err_t recorder_enable_ringbuf(bool enable)
{
    /* This is called before recorder_start to prime the audio pipeline */
    /* The actual ringbuf write is handled by audio.c's audio_read() */
    (void)enable;
    return ESP_OK;
}

int recorder_list_files(char file_list[][64], int max_files)
{
    /* Use "recordings" — storage_list_wav_files normalizes it internally */
    return storage_list_wav_files("recordings", file_list, max_files);
}
