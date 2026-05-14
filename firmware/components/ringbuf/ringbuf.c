/**
 * @file ringbuf.c
 * @brief Ring buffer for audio data — lock-free FIFO between audio_task and recorder_task
 *
 * Design:
 * - Uses FreeRTOS ring buffer (esp_ringbuf component)
 * - Byte buffer mode: variable-length items, no item size limit
 * - audio_task calls ringbuf_send() (write side)
 * - recorder_task calls ringbuf_receive() (read side)
 * - NO malloc in send/receive path
 */

#include "ringbuf.h"
#include "esp_log.h"
#include "freertos/ringbuf.h"
#include <string.h>

static const char *TAG = "ringbuf";

/* Ring buffer handle — NULL when not initialized */
static RingbufHandle_t s_handle = NULL;

/* Statistics */
static volatile uint32_t s_overflow_count = 0;
static volatile uint32_t s_total_dropped_samples = 0;
static volatile uint32_t s_total_sent_items = 0;
static volatile uint32_t s_total_received_items = 0;

esp_err_t ringbuf_init(size_t capacity_bytes)
{
    if (s_handle != NULL) {
        ESP_LOGW(TAG, "Ring buffer already initialized");
        return ESP_OK;
    }

    if (capacity_bytes < 4096) {
        ESP_LOGE(TAG, "Capacity too small: %zu bytes (minimum 4096)", capacity_bytes);
        return ESP_ERR_INVALID_SIZE;
    }

    /* Byte buffer mode: allows variable-length items */
    s_handle = xRingbufferCreate(
        capacity_bytes,         /* Total buffer size in bytes */
        RINGBUF_TYPE_BYTEBUF   /* Byte buffer — flexibly sized items */
    );

    if (s_handle == NULL) {
        ESP_LOGE(TAG, "xRingbufferCreate failed");
        return ESP_FAIL;
    }

    ringbuf_reset_stats();

    ESP_LOGI(TAG, "Ring buffer initialized: %zu bytes", capacity_bytes);
    return ESP_OK;
}

void ringbuf_deinit(void)
{
    if (s_handle != NULL) {
        vRingbufferDelete(s_handle);
        s_handle = NULL;
        ESP_LOGI(TAG, "Ring buffer deinitialized");
    }
}

size_t ringbuf_send(const int16_t *samples, size_t count)
{
    if (s_handle == NULL) {
        return 0;
    }
    if (samples == NULL || count == 0) {
        return 0;
    }

    const size_t byte_count = count * sizeof(int16_t);

    /* Non-blocking send (0 ticks to wait) — overflow returns pdFALSE */
    BaseType_t ret = xRingbufferSend(
        s_handle,
        samples,
        byte_count,
        0  /* Don't block — overflow if full */
    );

    if (ret == pdFALSE) {
        /* Ring buffer full — overflow */
        s_overflow_count++;
        s_total_dropped_samples += count;
        return 0;
    }

    s_total_sent_items++;
    return count;
}

size_t ringbuf_receive(int16_t *buf, size_t max_count, uint32_t timeout_ms)
{
    if (s_handle == NULL) {
        return 0;
    }
    if (buf == NULL || max_count == 0) {
        return 0;
    }

    size_t item_size = 0;

    /* Receive an item from the ring buffer */
    void *item = xRingbufferReceive(
        s_handle,
        &item_size,
        pdMS_TO_TICKS(timeout_ms)
    );

    if (item == NULL) {
        /* Timeout — no data available */
        return 0;
    }

    /* Calculate how many samples fit */
    size_t received_samples = item_size / sizeof(int16_t);
    if (received_samples > max_count) {
        /* Truncate if output buffer too small */
        received_samples = max_count;
    }

    /* Copy data to output buffer */
    memcpy(buf, item, received_samples * sizeof(int16_t));

    /* Return item to ring buffer (done reading) */
    vRingbufferReturnItem(s_handle, item);

    s_total_received_items++;
    return received_samples;
}

bool ringbuf_has_data(void)
{
    if (s_handle == NULL) {
        return false;
    }
    UBaseType_t uxFree, uxRead, uxWrite, uxAcquire, uxItemsWaiting;
    vRingbufferGetInfo(s_handle, &uxFree, &uxRead, &uxWrite, &uxAcquire, &uxItemsWaiting);
    /* uxItemsWaiting = bytes waiting to be read (in byte buffer mode) */
    return uxItemsWaiting > 0;
}

void ringbuf_get_stats(ringbuf_stats_t *out_stats)
{
    if (out_stats == NULL) {
        return;
    }
    out_stats->overflow_count = s_overflow_count;
    out_stats->total_dropped_samples = s_total_dropped_samples;
    out_stats->total_sent_items = s_total_sent_items;
    out_stats->total_received_items = s_total_received_items;
}

void ringbuf_reset_stats(void)
{
    s_overflow_count = 0;
    s_total_dropped_samples = 0;
    s_total_sent_items = 0;
    s_total_received_items = 0;
}

size_t ringbuf_available_bytes(void)
{
    if (s_handle == NULL) {
        return 0;
    }
    UBaseType_t uxFree, uxRead, uxWrite, uxAcquire, uxItemsWaiting;
    vRingbufferGetInfo(s_handle, &uxFree, &uxRead, &uxWrite, &uxAcquire, &uxItemsWaiting);
    return (size_t)uxItemsWaiting;
}
