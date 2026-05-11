/**
 * @file state.h
 * @brief 设备状态机 - 头文件
 *
 * 统一管理设备状态，并在状态变化时通过 event_bus 广播。
 * 业务模块应订阅 EVENT_STATE_CHANGED，而非轮询 state_get()。
 */

#ifndef STATE_H
#define STATE_H

#include <stdint.h>
#include "esp_err.h"
#include "event_bus.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 设备状态枚举
 */
typedef enum {
    DEVICE_STATE_INIT = 0,       /*!< 系统初始化中 */
    DEVICE_STATE_IDLE,          /*!< 待机状态 */
    DEVICE_STATE_RECORDING,     /*!< 录音中 */
    DEVICE_STATE_UPLOADING,     /*!< 上传中 */
    DEVICE_STATE_ERROR,         /*!< 错误状态 */
    DEVICE_STATE_SLEEP,         /*!< 深度睡眠 */
    DEVICE_STATE_COUNT,
} device_state_t;

/**
 * @brief 初始化状态机（默认进入 INIT 状态）
 * @return ESP_OK
 */
esp_err_t state_init(void);

/**
 * @brief 获取当前设备状态
 * @return device_state_t
 */
device_state_t state_get(void);

/**
 * @brief 设置设备状态
 *
 * 与当前状态相同时直接返回 ESP_OK，不重复广播。
 * 状态变化时自动通过 event_bus_publish(EVENT_STATE_CHANGED) 广播。
 *
 * @param new_state 目标状态
 * @return ESP_OK 成功，ESP_ERR_INVALID_STATE 已是目标状态
 */
esp_err_t state_set(device_state_t new_state);

/**
 * @brief 将状态枚举转为字符串（用于日志）
 * @param s 状态枚举值
 * @return  字符串，调用方不负责释放
 */
const char* state_to_string(device_state_t s);

/**
 * @brief 获取当前状态对应的事件类型名称
 */
const char* state_event_name(device_state_t s);

#ifdef __cplusplus
}
#endif

#endif /* STATE_H */
