/**
 * @file button.h
 * @brief 按键控制模块 - 头文件（Event 版）
 *
 * 功能：
 * - 短按 / 长按 / 双击 / HOLD 检测
 * - 消抖处理（软件定时器）
 * - 通过 event_bus 发布按键事件（替代直接回调）
 *
 * 使用方式：
 *   button_init(GPIO_NUM_0);
 *   // 按键事件通过 event_bus 广播，业务模块订阅 EVENT_BUTTON_* 处理
 */

#ifndef BUTTON_H
#define BUTTON_H

#include "esp_err.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 按键事件枚举（内部使用）
 */
typedef enum {
    BUTTON_EVENT_PRESS = 0,      /*!< 短按按下 */
    BUTTON_EVENT_LONG_PRESS,     /*!< 长按触发（到达阈值） */
    BUTTON_EVENT_DOUBLE_CLICK,   /*!< 双击 */
    BUTTON_EVENT_RELEASE,       /*!< 松开 */
} button_event_t;

/**
 * @brief 按键事件回调类型（保留用于 button_get_event 轮询模式）
 */
typedef void (*button_callback_t)(button_event_t event, void *arg);

/**
 * @brief 初始化按键
 *
 * 注意：不再需要传入回调函数，按键事件通过 event_bus 广播。
 *
 * @param gpio_num GPIO 引脚号
 * @return esp_err_t
 */
esp_err_t button_init(gpio_num_t gpio_num);

/**
 * @brief 获取当前按键事件（非阻塞，兼容旧代码）
 * @return button_event_t（BUTTON_EVENT_NONE = 无事件，使用 event_bus 替代）
 */
button_event_t button_get_event(void);

/**
 * @brief 设置长按时间阈值（默认 1500ms）
 * @param long_press_ms 长按时间（毫秒）
 */
void button_set_long_press_time(uint32_t long_press_ms);

#ifdef __cplusplus
}
#endif

#endif /* BUTTON_H */
