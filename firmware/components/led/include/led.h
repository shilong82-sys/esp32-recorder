/**
 * @file led.h
 * @brief LED 组件 - WS2812B RGB LED 驱动接口（Pattern 版）
 *
 * 基于 ESP-IDF RMT + led_strip_encoder
 * 适用于 ESP32-S3-N16R8 板载 WS2812B (GPIO48)
 *
 * Pattern 系统：
 *   LED_PATTERN_OFF       - 熄灭
 *   LED_PATTERN_STATIC    - 常亮（颜色固定）
 *   LED_PATTERN_BREATHING - 呼吸灯（正弦亮度变化）
 *   LED_PATTERN_BLINK     - 闪烁（频率可配）
 */

#ifndef LED_H
#define LED_H

#include "esp_err.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief LED Pattern 类型
 */
typedef enum {
    LED_PATTERN_OFF       = 0,  /*!< 熄灭 */
    LED_PATTERN_STATIC    = 1,  /*!< 常亮（颜色固定亮度 100%） */
    LED_PATTERN_BREATHING = 2,  /*!< 呼吸灯（param1=周期ms, param2=最小亮度%） */
    LED_PATTERN_BLINK     = 3,  /*!< 闪烁（param1=频率Hz） */
} led_pattern_t;

/**
 * @brief LED Pattern 配置
 */
typedef struct {
    led_pattern_t pattern;   /*!< Pattern 类型 */
    uint8_t       r;         /*!< 红色分量 0-255 */
    uint8_t       g;         /*!< 绿色分量 0-255 */
    uint8_t       b;         /*!< 蓝色分量 0-255 */
    uint16_t      param1;    /*!< Pattern 参数1：BREATHING=周期ms, BLINK=频率Hz */
    uint16_t      param2;    /*!< Pattern 参数2：BREATHING=最小亮度%, BLINK=占空比% */
} led_pattern_config_t;

/**
 * @brief 初始化 LED（WS2812B）
 * @param gpio_num WS2812B 数据引脚（通常为 GPIO48）
 * @return ESP_OK 成功
 */
esp_err_t led_init(gpio_num_t gpio_num);

/**
 * @brief 设置 Pattern（推荐方式）
 *
 * Pattern 由内部定时器驱动（50ms 更新间隔），无需外部轮询。
 *
 * @param config Pattern 配置（不可为 NULL）
 * @return ESP_OK 成功
 */
esp_err_t led_set_pattern(const led_pattern_config_t *config);

/**
 * @brief 设置固定颜色（内部转为 LED_PATTERN_STATIC）
 * @param r 红色 0-255
 * @param g 绿色 0-255
 * @param b 蓝色 0-255
 * @return ESP_OK 成功
 */
esp_err_t led_set_color(uint8_t r, uint8_t g, uint8_t b);

/**
 * @brief 熄灭 LED（内部转为 LED_PATTERN_OFF）
 */
void led_off(void);

#ifdef __cplusplus
}
#endif

#endif /* LED_H */
