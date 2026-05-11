/**
 * @file rgb_led.c
 * @brief RGB LED（WS2812）驱动实现
 *
 * 驱动方式：RMT TX channel + bytes encoder
 *
 * WS2812 时序（800kHz，GRB 顺序，MSB 先发）：
 *   T0H = 0 码：0.35µs 高 + 0.9µs 低
 *   T1H = 1 码：0.9µs  高 + 0.35µs 低
 *   RES = 重置：> 50µs 低电平
 *
 * RMT 配置：10 MHz 分辨率 → 1 tick = 100 ns
 *   bit0: 高 4 tick（0.4µs），低 9 tick（0.9µs）
 *   bit1: 高 9 tick（0.9µs），低 4 tick（0.4µs）
 */

#include "rgb_led.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "rgb_led";

// RMT 分辨率：10 MHz，1 tick = 100 ns
#define RMT_RESOLUTION_HZ     (10 * 1000 * 1000)

// WS2812 时序（tick = 100 ns）
#define WS2812_T0H_TICK       4    // 0.4 µs 高
#define WS2812_T0L_TICK       9    // 0.9 µs 低
#define WS2812_T1H_TICK       9    // 0.9 µs 高
#define WS2812_T1L_TICK       4    // 0.4 µs 低

// ---------- 静态变量 ----------
static rmt_channel_handle_t  s_rmt_chan   = NULL;
static rmt_encoder_handle_t  s_rmt_enc    = NULL;
static bool                 s_initialized = false;

// ---------- 初始化 ----------
esp_err_t rgb_led_init(gpio_num_t gpio_num)
{
    esp_err_t ret = ESP_OK;

    if (s_initialized) {
        ESP_LOGW(TAG, "RGB LED already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing RGB LED on GPIO %d (WS2812)", gpio_num);

    // 1. 配置 RMT TX 通道
    rmt_tx_channel_config_t tx_cfg = {
        .gpio_num        = gpio_num,
        .clk_src         = RMT_CLK_SRC_DEFAULT,
        .resolution_hz   = RMT_RESOLUTION_HZ,
        .mem_block_symbols = 64,
        .trans_queue_depth = 3,
        .flags           = {
            .invert_out = false,
        },
    };
    ret = rmt_new_tx_channel(&tx_cfg, &s_rmt_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "rmt_new_tx_channel failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // 2. 配置 WS2812 bytes 编码器
    // WS2812 MSB 先发
    rmt_bytes_encoder_config_t bytes_enc_cfg = {
        .bit0 = {
            .level0    = 1,
            .duration0 = WS2812_T0H_TICK,
            .level1    = 0,
            .duration1 = WS2812_T0L_TICK,
        },
        .bit1 = {
            .level0    = 1,
            .duration0 = WS2812_T1H_TICK,
            .level1    = 0,
            .duration1 = WS2812_T1L_TICK,
        },
        .flags = { .msb_first = true },
    };
    ret = rmt_new_bytes_encoder(&bytes_enc_cfg, &s_rmt_enc);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "rmt_new_bytes_encoder failed: %s", esp_err_to_name(ret));
        goto err;
    }

    // 3. 使能通道
    ret = rmt_enable(s_rmt_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "rmt_enable failed: %s", esp_err_to_name(ret));
        goto err;
    }

    s_initialized = true;

    // 默认熄灭
    rgb_led_set_color(0, 0, 0);

    ESP_LOGI(TAG, "RGB LED initialized (GPIO %d)", gpio_num);
    return ESP_OK;

err:
    if (s_rmt_enc) {
        rmt_del_encoder(s_rmt_enc);
        s_rmt_enc = NULL;
    }
    if (s_rmt_chan) {
        rmt_del_channel(s_rmt_chan);
        s_rmt_chan = NULL;
    }
    return ret;
}

// ---------- 设置颜色 ----------
esp_err_t rgb_led_set_color(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_initialized) {
        ESP_LOGW(TAG, "RGB LED not initialized, call rgb_led_init() first");
        return ESP_ERR_INVALID_STATE;
    }

    // WS2812 颜色顺序：GRB（绿-红-蓝）
    uint8_t grb[3] = { g, r, b };

    rmt_transmit_config_t tx_cfg = {
        .loop_count = 0,
        .flags       = { .eot_level = 0 },
    };

    ESP_ERROR_CHECK(rmt_transmit(s_rmt_chan, s_rmt_enc, grb, sizeof(grb), &tx_cfg));

    // 等待发送完成
    ESP_ERROR_CHECK(rmt_tx_wait_all_done(s_rmt_chan, pdMS_TO_TICKS(100)));

    ESP_LOGI(TAG, "RGB LED -> R=%-3d G=%-3d B=%-3d", r, g, b);
    return ESP_OK;
}

// ---------- 熄灭 ----------
void rgb_led_off(void)
{
    if (!s_initialized) return;
    rgb_led_set_color(0, 0, 0);
}
