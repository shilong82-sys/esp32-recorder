/**
 * @file rgb_led.h
 * @brief RGB LED（WS2812）驱动接口
 *
 * 驱动方式：RMT peripheral + LED Strip Driver
 * 时序符合 WS2812 规范（800kHz，GRB 顺序）
 *
 * 接线参考（ESP32-S3-N16R8 板载 RGB LED）：
 *   WS2812 Data In  →  GPIO48（可按需修改 RGB_LED_GPIO）
 */

#ifndef RGB_LED_H
#define RGB_LED_H

#include "esp_err.h"
#include "driver/gpio.h"

/**
 * @brief 初始化 RGB LED
 * @param gpio_num WS2812 数据引脚
 * @return esp_err_t ESP_OK 表示成功
 */
esp_err_t rgb_led_init(gpio_num_t gpio_num);

/**
 * @brief 设置 RGB LED 颜色
 * @param r 红色分量（0–255）
 * @param g 绿色分量（0–255）
 * @param b 蓝色分量（0–255）
 * @return esp_err_t ESP_OK 表示成功
 */
esp_err_t rgb_led_set_color(uint8_t r, uint8_t g, uint8_t b);

/**
 * @brief 熄灭 RGB LED
 */
void rgb_led_off(void);

#endif // RGB_LED_H
