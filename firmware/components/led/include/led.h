/**
 * @file led.h
 * @brief LED 指示模块 - 头文件
 *
 * 功能：
 * - 状态指示（未连 WiFi / 录音中 / 上传中 / 错误）
 * - 呼吸灯效果（PWM）
 * - 电量指示（多色 LED）
 */

#ifndef LED_H
#define LED_H

#include "esp_err.h"
#include "driver/gpio.h"

/**
 * @brief LED 状态枚举
 */
typedef enum {
    LED_STATE_OFF,          /*!< 熄灭 */
    LED_STATE_IDLE,         /*!< 空闲（慢闪）*/
    LED_STATE_WIFI_CONN,    /*!< WiFi 连接中（快闪）*/
    LED_STATE_RECORDING,   /*!< 录音中（常亮）*/
    LED_STATE_UPLOADING,    /*!< 上传中（呼吸）*/
    LED_STATE_ERROR,        /*!< 错误（快闪 3 次）*/
    LED_STATE_LOW_BAT,      /*!< 低电量（红闪）*/
} led_state_t;

/**
 * @brief 初始化 LED
 * @param gpio_num GPIO 引脚号
 * @return esp_err_t
 */
esp_err_t led_init(gpio_num_t gpio_num);

/**
 * @brief 设置 LED 状态
 * @param state LED 状态枚举
 */
void led_set_state(led_state_t state);

/**
 * @brief 关闭 LED
 */
void led_off(void);

#endif // LED_H
