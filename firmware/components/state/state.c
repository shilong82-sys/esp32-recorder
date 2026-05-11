/**
 * @file state.c
 * @brief 设备状态机 - 实现
 */

#include "state.h"
#include "esp_log.h"

static const char* TAG = "state";

/* 状态名称表 */
static const char* s_state_names[] = {
    [DEVICE_STATE_INIT]       = "INIT",
    [DEVICE_STATE_IDLE]      = "IDLE",
    [DEVICE_STATE_RECORDING] = "RECORDING",
    [DEVICE_STATE_UPLOADING] = "UPLOADING",
    [DEVICE_STATE_ERROR]     = "ERROR",
    [DEVICE_STATE_SLEEP]     = "SLEEP",
};

static device_state_t s_current_state = DEVICE_STATE_INIT;

esp_err_t state_init(void)
{
    s_current_state = DEVICE_STATE_INIT;
    return ESP_OK;
}

device_state_t state_get(void)
{
    return s_current_state;
}

esp_err_t state_set(device_state_t new_state)
{
    if (new_state < 0 || new_state >= DEVICE_STATE_COUNT) {
        ESP_LOGW(TAG, "Invalid state: %d", new_state);
        return ESP_ERR_INVALID_ARG;
    }

    if (s_current_state == new_state) {
        return ESP_OK;  // 状态无变化，不广播
    }

    event_state_data_t data = {
        .prev_state = (int)s_current_state,
        .curr_state = (int)new_state,
    };

    device_state_t prev = s_current_state;
    s_current_state = new_state;

    ESP_LOGI(TAG, "State: %s -> %s",
             state_to_string(prev),
             state_to_string(new_state));

    event_bus_publish(EVENT_STATE_CHANGED, &data, sizeof(data));
    return ESP_OK;
}

const char* state_to_string(device_state_t s)
{
    if (s < 0 || s >= DEVICE_STATE_COUNT) {
        return "UNKNOWN";
    }
    return s_state_names[s] ? s_state_names[s] : "UNKNOWN";
}
