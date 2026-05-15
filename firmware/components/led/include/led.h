/**
 * @file led.h
 * @brief LED 组件 - WS2812B RGB LED 驱动接口（完全异步版）
 *
 * 基于 ESP-IDF RMT + led_strip_encoder
 * 适用于 ESP32-S3-N16R8 板载 WS2812B (GPIO48)
 *
 * 架构：
 * - 所有公开 API 异步化，只发送消息到内部队列
 * - LED 任务处理所有 RMT 操作
 * - 支持 RMT timeout 自动恢复
 * - Queue 深度=1，覆盖模式（防状态风暴）
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
 * @brief LED 调试统计
 */
typedef struct {
    uint32_t refresh_count;      /* 成功刷新次数 */
    uint32_t timeout_count;       /* timeout 次数 */
    uint32_t recovery_count;       /* RMT 恢复次数 */
    uint32_t dropped_count;        /* 被覆盖的 pattern 数 */
    uint32_t queue_overflow;       /* 队列溢出次数 */
    uint32_t last_duration_us;     /* 上次刷新耗时（us）*/
    uint32_t consecutive_timeout;  /* 连续 timeout 计数 */
} led_stats_t;

/**
 * @brief 初始化 LED（WS2812B）
 * @param gpio_num WS2812B 数据引脚（通常为 GPIO48）
 * @return ESP_OK 成功
 */
esp_err_t led_init(gpio_num_t gpio_num);

/**
 * @brief 设置 Pattern（异步，推荐方式）
 *
 * 异步特性：此函数立即返回，pattern 更新在 led_task 中执行
 *
 * @param config Pattern 配置（不可为 NULL）
 * @return ESP_OK 成功（异步，不保证立即生效）
 */
esp_err_t led_set_pattern(const led_pattern_config_t *config);

/**
 * @brief 设置固定颜色（内部转为 LED_PATTERN_STATIC，异步）
 * @param r 红色 0-255
 * @param g 绿色 0-255
 * @param b 蓝色 0-255
 * @return ESP_OK 成功（异步）
 */
esp_err_t led_set_color(uint8_t r, uint8_t g, uint8_t b);

/**
 * @brief 熄灭 LED（内部转为 LED_PATTERN_OFF，异步）
 */
void led_off(void);

/**
 * @brief 获取 LED 调试统计
 * @param out_stats 输出统计结构（不可为 NULL）
 */
void led_get_stats(led_stats_t *out_stats);

#ifdef __cplusplus
}
#endif

#endif /* LED_H */
