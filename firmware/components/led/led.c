/**
 * @file led.c
 * @brief LED 组件 - WS2812B RGB LED 驱动实现（Pattern 版）
 *
 * 基于 ESP-IDF RMT TX + led_strip_encoder
 * Pattern 由内部 esp_timer 驱动，50ms 更新间隔。
 */

#include "led.h"
#include "led_strip_encoder.h"
#include "driver/rmt_tx.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>

static const char *TAG = "led";

/* WS2812B 只需 3 字节（GRB）*/
#define LED_PIXEL_BUF_SIZE  3

/* Pattern 更新周期（ms）*/
#define LED_TICK_MS         50

/* 静态变量 */
static rmt_channel_handle_t      s_rmt_chan      = NULL;
static rmt_encoder_handle_t      s_led_encoder   = NULL;
static bool                      s_initialized    = false;

/* Pattern 状态 */
static led_pattern_config_t      s_pattern        = {0};
static bool                      s_pattern_active  = false;

/* 更新定时器 */
static esp_timer_handle_t         s_tick_timer    = NULL;

/* LED 像素缓冲区（GRB 顺序，WS2812B 格式）*/
static uint8_t                   s_pixel_buf[LED_PIXEL_BUF_SIZE];

/* 内部时间戳（ms），用于计算 breathing/blink 相位 */
static uint64_t                  s_tick_count     = 0;

/*————————————————————————————
 * 内部：硬件发送一次像素
 *————————————————————————————*/
static void led_refresh(void)
{
    if (!s_initialized) return;

    rmt_transmit_config_t tx_cfg = {
        .loop_count = 0,
        .flags.eot_level = 0,
    };

    rmt_transmit(s_rmt_chan, s_led_encoder,
                 s_pixel_buf, sizeof(s_pixel_buf), &tx_cfg);
    rmt_tx_wait_all_done(s_rmt_chan, pdMS_TO_TICKS(100));
}

/*————————————————————————————
 * 内部：计算当前亮度并刷新
 *————————————————————————————*/
static void led_update(void)
{
    if (!s_initialized || !s_pattern_active) return;

    s_tick_count++;

    uint8_t r = 0, g = 0, b = 0;
    float   brightness = 1.0f;

    switch (s_pattern.pattern) {
        case LED_PATTERN_OFF:
            r = g = b = 0;
            break;

        case LED_PATTERN_STATIC:
            r = s_pattern.r;
            g = s_pattern.g;
            b = s_pattern.b;
            break;

        case LED_PATTERN_BREATHING: {
            uint16_t period_ms  = s_pattern.param1 > 0 ? s_pattern.param1 : 2000; // 默认 2s 周期
            uint16_t min_pct   = s_pattern.param2 > 0 ? s_pattern.param2 : 20;     // 默认最低 20%
            uint64_t total_ticks = period_ms / LED_TICK_MS;
            float   phase = (s_tick_count % total_ticks) * (2.0f * M_PI / total_ticks);
            // sin ∈ [-1, 1] → [0, 1]
            float sin_val = ( sinf((float)phase) + 1.0f ) / 2.0f;
            float min_bright = min_pct / 100.0f;
            brightness = min_bright + sin_val * (1.0f - min_bright);
            r = (uint8_t)(s_pattern.r * brightness);
            g = (uint8_t)(s_pattern.g * brightness);
            b = (uint8_t)(s_pattern.b * brightness);
            break;
        }

        case LED_PATTERN_BLINK: {
            uint16_t freq_hz   = s_pattern.param1 > 0 ? s_pattern.param1 : 2;  // 默认 2Hz
            uint16_t duty_pct  = s_pattern.param2 > 0 ? s_pattern.param2 : 50; // 默认 50% 占空比
            uint64_t cycle_ticks = 1000 / LED_TICK_MS / freq_hz;                // 一周期 tick 数
            uint64_t on_ticks   = cycle_ticks * duty_pct / 100;
            bool on = (s_tick_count % cycle_ticks) < on_ticks;
            if (on) {
                r = s_pattern.r;
                g = s_pattern.g;
                b = s_pattern.b;
            } else {
                r = g = b = 0;
            }
            break;
        }

        default:
            return;
    }

    /* WS2812B 颜色顺序：GRB */
    bool changed = (s_pixel_buf[0] != g) || (s_pixel_buf[1] != r) || (s_pixel_buf[2] != b);
    s_pixel_buf[0] = g;
    s_pixel_buf[1] = r;
    s_pixel_buf[2] = b;

    if (changed || s_pattern.pattern == LED_PATTERN_OFF) {
        led_refresh();
    }
}

/*————————————————————————————
 * 定时器回调（静态，不能传参数）
 *————————————————————————————*/
static void led_tick_callback(void *arg)
{
    (void)arg;
    led_update();
}

/*————————————————————————————
 * led_init — 初始化 LED
 *————————————————————————————*/
esp_err_t led_init(gpio_num_t gpio_num)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "LED already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing WS2812B on GPIO%d", gpio_num);

    /* 1. RMT TX 通道 */
    rmt_tx_channel_config_t tx_chan_cfg = {
        .clk_src           = RMT_CLK_SRC_DEFAULT,
        .gpio_num          = gpio_num,
        .mem_block_symbols = 64,
        .resolution_hz     = 10 * 1000 * 1000,  /* 10 MHz */
        .trans_queue_depth = 4,
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_chan_cfg, &s_rmt_chan));

    /* 2. led_strip_encoder */
    led_strip_encoder_config_t enc_cfg = {
        .resolution = 10 * 1000 * 1000,
    };
    ESP_ERROR_CHECK(rmt_new_led_strip_encoder(&enc_cfg, &s_led_encoder));

    /* 3. 使能 RMT */
    ESP_ERROR_CHECK(rmt_enable(s_rmt_chan));

    /* 4. 创建更新定时器 */
    const esp_timer_create_args_t timer_args = {
        .callback = &led_tick_callback,
        .name     = "led_tick",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_tick_timer, LED_TICK_MS * 1000));  // 微秒

    s_initialized   = true;
    s_pattern_active = false;
    s_tick_count    = 0;

    /* 5. 默认熄灭 */
    led_off();

    ESP_LOGI(TAG, "LED initialized OK (GPIO%d, tick=%dms)", gpio_num, LED_TICK_MS);
    return ESP_OK;
}

/*————————————————————————————
 * led_set_pattern — 设置 Pattern
 *————————————————————————————*/
esp_err_t led_set_pattern(const led_pattern_config_t *config)
{
    if (!s_initialized) {
        ESP_LOGW(TAG, "LED not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    s_pattern       = *config;
    s_pattern_active = (config->pattern != LED_PATTERN_OFF);

    /* 立即刷新一次 */
    led_update();

    ESP_LOGI(TAG, "Pattern: %d, RGB(%d,%d,%d), p1=%d, p2=%d",
             config->pattern, config->r, config->g, config->b,
             config->param1, config->param2);

    return ESP_OK;
}

/*————————————————————————————
 * led_set_color — 设置固定颜色
 *————————————————————————————*/
esp_err_t led_set_color(uint8_t r, uint8_t g, uint8_t b)
{
    led_pattern_config_t cfg = {
        .pattern = LED_PATTERN_STATIC,
        .r       = r,
        .g       = g,
        .b       = b,
        .param1  = 0,
        .param2  = 0,
    };
    return led_set_pattern(&cfg);
}

/*————————————————————————————
 * led_off — 熄灭 LED
 *————————————————————————————*/
void led_off(void)
{
    if (!s_initialized) return;
    led_pattern_config_t cfg = {
        .pattern = LED_PATTERN_OFF,
        .r = 0, .g = 0, .b = 0,
    };
    led_set_pattern(&cfg);
}
