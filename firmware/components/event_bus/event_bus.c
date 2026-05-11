/**
 * @file event_bus.c
 * @brief 全局事件总线 - 实现
 */

#include "event_bus.h"
#include "esp_log.h"
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#define MAX_HANDLERS  32

static const char* TAG = "event_bus";

/**
 * 单个监听器条目
 */
typedef struct {
    bool            in_use;
    event_type_t    type;
    event_cb_t      cb;
    void           *user_data;
} listener_entry_t;

static listener_entry_t s_listeners[MAX_HANDLERS];
static bool s_initialized = false;

/* 事件类型名称表（与 event_type_t 顺序严格对应） */
static const char* s_event_names[] = {
    [EVENT_STATE_CHANGED]        = "STATE_CHANGED",
    [EVENT_BUTTON_PRESSED]       = "BUTTON_PRESSED",
    [EVENT_BUTTON_RELEASED]      = "BUTTON_RELEASED",
    [EVENT_BUTTON_CLICKED]       = "BUTTON_CLICKED",
    [EVENT_BUTTON_DOUBLE_CLICKED]= "BUTTON_DOUBLE_CLICKED",
    [EVENT_BUTTON_LONG_PRESSED]  = "BUTTON_LONG_PRESSED",
    [EVENT_BUTTON_HOLD]          = "BUTTON_HOLD",
    [EVENT_WIFI_CONNECTED]       = "WIFI_CONNECTED",
    [EVENT_WIFI_DISCONNECTED]    = "WIFI_DISCONNECTED",
    [EVENT_STORAGE_READY]        = "STORAGE_READY",
    [EVENT_STORAGE_ERROR]        = "STORAGE_ERROR",
    [EVENT_BATTERY_LOW]          = "BATTERY_LOW",
    [EVENT_BATTERY_CRITICAL]     = "BATTERY_CRITICAL",
    [EVENT_RECORDING_STARTED]    = "RECORDING_STARTED",
    [EVENT_RECORDING_STOPPED]    = "RECORDING_STOPPED",
    [EVENT_UPLOAD_STARTED]       = "UPLOAD_STARTED",
    [EVENT_UPLOAD_PROGRESS]      = "UPLOAD_PROGRESS",
    [EVENT_UPLOAD_DONE]          = "UPLOAD_DONE",
    [EVENT_UPLOAD_FAILED]        = "UPLOAD_FAILED",
};

void event_bus_init(void)
{
    if (s_initialized) {
        return;
    }
    memset(s_listeners, 0, sizeof(s_listeners));
    s_initialized = true;
    ESP_LOGI(TAG, "Event bus initialized (max %d handlers)", MAX_HANDLERS);
}

event_handler_t event_bus_subscribe(event_type_t type, event_cb_t cb, void *user_data)
{
    if (!s_initialized) {
        event_bus_init();
    }

    if (cb == NULL || type < 0 || type >= EVENT_TYPE_COUNT) {
        return -1;
    }

    for (int i = 0; i < MAX_HANDLERS; i++) {
        if (!s_listeners[i].in_use) {
            s_listeners[i].in_use    = true;
            s_listeners[i].type      = type;
            s_listeners[i].cb        = cb;
            s_listeners[i].user_data = user_data;
            return i;  // 句柄 = 数组索引
        }
    }

    /* 监听器已满 */
    return -1;
}

void event_bus_unsubscribe(event_handler_t handle)
{
    if (handle < 0 || handle >= MAX_HANDLERS) {
        return;
    }
    s_listeners[handle].in_use = false;
}

void event_bus_publish(event_type_t type, const void *data, size_t len)
{
    if (!s_initialized || type < 0 || type >= EVENT_TYPE_COUNT) {
        return;
    }

    for (int i = 0; i < MAX_HANDLERS; i++) {
        if (s_listeners[i].in_use && s_listeners[i].type == type) {
            s_listeners[i].cb(type, data, len, s_listeners[i].user_data);
        }
    }
}

const char* event_type_name(event_type_t type)
{
    if (type < 0 || type >= EVENT_TYPE_COUNT) {
        return "UNKNOWN";
    }
    return s_event_names[type] ? s_event_names[type] : "UNKNOWN";
}
