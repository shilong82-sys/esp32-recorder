/**
 * @file ringbuf.h
 * @brief Ring buffer for audio data — lock-free FIFO between audio_task and recorder_task
 *
 * Design:
 * - Uses FreeRTOS RingbufHandle_t (no-spin, ISR-safe)
 * - Item mode: byte array (ringbuf item = one audio frame batch)
 * - audio_task calls ringbuf_send() (write side)
 * - recorder_task calls ringbuf_receive() (read side)
 * - NO MALLOC in send/receive path
 */

#ifndef RINGBUF_H
#define RINGBUF_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Statistics for ring buffer health monitoring
 */
typedef struct {
    uint32_t overflow_count;       /**< Times ringbuf was full and samples were dropped */
    uint32_t total_dropped_samples;/**< Total samples dropped due to overflow */
    uint32_t total_sent_items;     /**< Total items successfully sent */
    uint32_t total_received_items; /**< Total items successfully received */
} ringbuf_stats_t;

/**
 * @brief Initialize the ring buffer
 *
 * @param capacity_bytes  Buffer capacity in bytes. Recommended: 32KB (32768 bytes)
 * @return ESP_OK on success, error code on failure
 */
esp_err_t ringbuf_init(size_t capacity_bytes);

/**
 * @brief Deinitialize the ring buffer and free resources
 */
void ringbuf_deinit(void);

/**
 * @brief Send audio samples to the ring buffer (called from audio_task)
 *
 * This is non-blocking. If the ring buffer is full, samples are silently
 * dropped and overflow_count is incremented.
 *
 * @param samples  Pointer to int16_t PCM samples
 * @param count    Number of samples (int16_t count, NOT byte count)
 * @return Number of samples actually written (count on success, < count on overflow)
 */
size_t ringbuf_send(const int16_t *samples, size_t count);

/**
 * @brief Receive audio samples from the ring buffer (called from recorder_task)
 *
 * This blocks with a timeout. Returns 0 if no data available within timeout.
 *
 * @param buf       Destination buffer for PCM samples
 * @param max_count Maximum number of samples to receive
 * @param timeout_ms Timeout in milliseconds (portMAX_DELAY = wait forever)
 * @return Number of samples actually received, 0 if timeout
 */
size_t ringbuf_receive(int16_t *buf, size_t max_count, uint32_t timeout_ms);

/**
 * @brief Check if ring buffer has data waiting
 *
 * @return true if ring buffer has at least one byte
 */
bool ringbuf_has_data(void);

/**
 * @brief Get current ring buffer statistics
 *
 * @param[out] out_stats Pointer to stats struct to fill
 */
void ringbuf_get_stats(ringbuf_stats_t *out_stats);

/**
 * @brief Reset all statistics counters to zero
 */
void ringbuf_reset_stats(void);

/**
 * @brief Get current ring buffer fill level (approximate)
 *
 * @return Number of bytes currently in the ring buffer
 */
size_t ringbuf_available_bytes(void);

#ifdef __cplusplus
}
#endif

#endif // RINGBUF_H
