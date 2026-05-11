/**
 * @file ui.h
 * @brief UI 组件 - 头文件
 *
 * 统一管理所有用户反馈（LED pattern），业务模块不直接操作硬件。
 * 通过订阅 EVENT_STATE_CHANGED 驱动 LED 状态。
 *
 * 用法：
 *   1. app_main 中调用 ui_init()
 *   2. 如需临时覆盖：ui_led_override()
 */

#ifndef UI_H
#define UI_H

#include "esp_err.h"
#include "led.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief UI 初始化（创建 UI 任务，订阅事件）
 * @return ESP_OK 成功
 */
esp_err_t ui_init(void);

/**
 * @brief 临时覆盖 LED pattern（覆盖结束后自动恢复状态机模式）
 *
 * @param pattern  要显示的 pattern
 * @param r,g,b   RGB 颜色
 * @param duration_ms  覆盖持续时间（毫秒），0 表示永久覆盖
 * @return ESP_OK
 */
esp_err_t ui_led_override(led_pattern_t pattern, uint8_t r, uint8_t g, uint8_t b, uint32_t duration_ms);

/**
 * @brief 取消当前的 LED 覆盖，立即恢复状态机驱动
 */
void ui_led_override_cancel(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_H */
