/**
 * @file button.h
 * @brief 按键控制模块 - 头文件
 *
 * 功能：
 * - 短按 / 长按 / 双击检测
 * - 消抖处理（软件定时器）
 * - 按键事件回调
 */

#ifndef BUTTON_H
#define BUTTON_H_

#include "esp_err.h"
#include "driver/gpio.h"

/**
 * @brief 按键事件枚举
 */
typedef enum {
    BUTTON_EVENT_PRESS,      /*!< 短按 */
    BUTTON_EVENT_LONG_PRESS, /*!< 长按（>1.5s）*/
    BUTTON_EVENT_DOUBLE_CLICK, /*!< 双击 */
    BUTTON_EVENT_RELEASE,     /*!< 松开 */
} button_event_t;

/**
 * @brief 按键事件回调函数类型
 */
typedef void (*button_callback_t)(button_event_t event, void *arg);

/**
 * @brief 初始化按键
 * @param gpio_num GPIO 引脚号
 * @param cb 事件回调（可填 NULL）
 * @param arg 回调参数
 * @return esp_err_t
 */
esp_err_t button_init(gpio_num_t gpio_num, button_callback_t cb, void *arg);

/**
 * @brief 获取当前按键事件（非阻塞）
 * @return button_event_t（BUTTON_EVENT_NONE = 无事件）
 */
button_event_t button_get_event(void);

/**
 * @brief 设置长按时间阈值（默认 1500ms）
 * @param long_press_ms 长按时间（毫秒）
 */
void button_set_long_press_time(uint32_t long_press_ms);

#endif // BUTTON_H_
