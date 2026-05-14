/**
 * @file recorder.h
 * @brief Recorder component - WAV recording from I2S microphone
 *
 * The recorder reads audio from a ring buffer (populated by audio.c)
 * and writes it to SD card as WAV files.
 *
 * Architecture:
 * - audio.c reads from I2S and sends to ringbuf
 * - recorder.c's recorder_task reads from ringbuf and writes WAV to SD
 * - recorder_start/stop control recording via the state machine
 */

#ifndef RECORDER_H
#define RECORDER_H

#include "esp_err.h"
#include "driver/i2s_std.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/**
 * @brief Recorder configuration (currently uses fixed settings)
 */
typedef struct {
    i2s_port_t i2s_port;        /**< I2S port (unused — audio.c owns I2S) */
    uint32_t sample_rate;        /**< Sample rate in Hz (default: 16000) */
    uint8_t bits_per_sample;     /**< Bits per sample (default: 16) */
    uint8_t channel_format;       /**< Channels (default: 1 = mono) */
    char file_prefix[32];         /**< File prefix (unused in Phase 1 — session-based) */
} recorder_config_t;

/**
 * @brief Initialize the recorder component
 *
 * Creates the recorder task, initializes ring buffer, and sets up session counter.
 *
 * @param config Configuration (NULL = use defaults)
 * @return ESP_OK on success
 */
esp_err_t recorder_init(const recorder_config_t *config);

/**
 * @brief Start recording
 *
 * Opens a new WAV file, writes header, and starts accumulating audio.
 * Filename is auto-generated as REC_SESSION_XXXX.wav.
 *
 * @param filename Optional full path override (NULL = auto-generate)
 * @return ESP_OK on success
 */
esp_err_t recorder_start(const char *filename);

/**
 * @brief Stop recording
 *
 * Flushes remaining ring buffer data, updates WAV header with correct sizes,
 * closes the file, and publishes EVENT_RECORDING_STOPPED.
 *
 * @param[out] out_duration_ms Duration in milliseconds (may be NULL)
 * @return ESP_OK on success
 */
esp_err_t recorder_stop(uint32_t *out_duration_ms);

/**
 * @brief Check if recording is in progress
 *
 * @return true if recording, false otherwise
 */
bool recorder_is_recording(void);

/**
 * @brief Enable or disable ring buffer forwarding
 *
 * Called by app_main before starting/stopping recording to ensure
 * the audio pipeline is in the right mode.
 *
 * @param enable true = enable, false = disable
 * @return ESP_OK
 */
esp_err_t recorder_enable_ringbuf(bool enable);

/**
 * @brief List all WAV files in /sdcard/recordings/
 *
 * @param[out] file_list Array of filename strings (64 chars each)
 * @param max_files Maximum number of filenames to return
 * @return Number of files found
 */
int recorder_list_files(char file_list[][64], int max_files);

#endif /* RECORDER_H */
