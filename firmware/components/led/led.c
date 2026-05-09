/**
 * @file led.c
 * @brief LED 指示模块 - 源文件
 *
 * 功能：
 * - 状态指示（未连 WiFi / 录音中 / 上传中 / 错误）
 * - 呼吸灯效果（LEDC PWM）
 * - 电量指示
 */

#include "led.h"
#include "esp_log.h"
#include "driver/ledc.h"
#include "esp_timer.h"
#include <math.h>

static const char *TAG = "led";

// LEDC 配置
#define LEDC_TIMER              LEDC_TIMER_0
#define LEDC_MODE               LEDC_LOW_SPEED_MODE
#define LEDC_DUTY_RES           LEDC_TIMER_8_BIT  // 8-bit resolution (0-255)
#define LEDC_FREQUENCY          5000              // 5 kHz PWM frequency

// 呼吸灯参数
#define BREATH_PERIOD_MS        2000               // 呼吸周期 2 秒

// LED 状态
static gpio_num_t s_gpio_num = GPIO_NUM_NC;
static led_state_t s_current_state = LED_STATE_OFF;
static bool s_initialized = false;
static esp_timer_handle_t s_led_timer = NULL;
static uint32_t s_breathe_duty = 0;
static int64_t s_last_toggle_time = 0;
static int s_blink_count = 0;
static int s_blink_target = 0;

/**
 * @brief LED 定时器回调
 */
static void led_timer_callback(void *arg)
{
    if (!s_initialized) {
        return;
    }

    int64_t now = esp_timer_get_time() / 1000;
    led_state_t state = s_current_state;

    switch (state) {
    case LED_STATE_OFF:
        ledc_set_duty(LEDC_MODE, LEDC_CHANNEL_0, 0);
        ledc_update_duty(LEDC_MODE, LEDC_CHANNEL_0);
        break;

    case LED_STATE_IDLE:
        // 慢闪：1Hz
        if (now - s_last_toggle_time >= 500) {
            s_last_toggle_time = now;
            static bool on = false;
            on = !on;
            ledc_set_duty(LEDC_MODE, LEDC_CHANNEL_0, on ? 128 : 0);
            ledc_update_duty(LEDC_MODE, LEDC_CHANNEL_0);
        }
        break;

    case LED_STATE_WIFI_CONN:
        // 快闪：4Hz
        if (now - s_last_toggle_time >= 125) {
            s_last_toggle_time = now;
            static bool on = false;
            on = !on;
            ledc_set_duty(LEDC_MODE, LEDC_CHANNEL_0, on ? 200 : 0);
            ledc_update_duty(LEDC_MODE, LEDC_CHANNEL_0);
        }
        break;

    case LED_STATE_RECORDING:
        // 常亮
        ledc_set_duty(LEDC_MODE, LEDC_CHANNEL_0, 255);
        ledc_update_duty(LEDC_MODE, LEDC_CHANNEL_0);
        break;

    case LED_STATE_UPLOADING:
        // 呼吸灯
        {
            static int64_t breath_start = 0;
            if (breath_start == 0) breath_start = now;

            uint32_t elapsed = (now - breath_start) % BREATH_PERIOD_MS;
            float progress = (float)elapsed / BREATH_PERIOD_MS;
            // 正弦波呼吸
            float duty = (1.0f - cosf(progress * 2 * 3.14159f)) / 2.0f * 255.0f;
            uint32_t duty_val = (uint32_t)duty;

            ledc_set_duty(LEDC_MODE, LEDC_CHANNEL_0, duty_val);
            ledc_update_duty(LEDC_MODE, LEDC_CHANNEL_0);
        }
        break;

    case LED_STATE_ERROR:
        // 快闪 3 次后熄灭
        if (s_blink_target == 0) {
            s_blink_target = 6;  // 3 次闪烁 = 6 次切换
            s_blink_count = 0;
            s_last_toggle_time = now;
        }
        if (s_blink_count < s_blink_target) {
            if (now - s_last_toggle_time >= 100) {
                s_last_toggle_time = now;
                s_blink_count++;
                static bool on = false;
                on = !on;
                ledc_set_duty(LEDC_MODE, LEDC_CHANNEL_0, on ? 255 : 0);
                ledc_update_duty(LEDC_MODE, LEDC_CHANNEL_0);
            }
        } else {
            ledc_set_duty(LEDC_MODE, LEDC_CHANNEL_0, 0);
            ledc_update_duty(LEDC_MODE, LEDC_CHANNEL_0);
        }
        break;

    case LED_STATE_LOW_BAT:
        // 红闪
        if (now - s_last_toggle_time >= 500) {
            s_last_toggle_time = now;
            static bool on = false;
            on = !on;
            ledc_set_duty(LEDC_MODE, LEDC_CHANNEL_0, on ? 255 : 0);
            ledc_update_duty(LEDC_MODE, LEDC_CHANNEL_0);
        }
        break;

    default:
        break;
    }
}

/**
 * @brief 初始化 LED
 */
esp_err_t led_init(gpio_num_t gpio_num)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "LED already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing LED on GPIO %d", gpio_num);

    // 保存 GPIO
    s_gpio_num = gpio_num;

    // 配置 LEDC 定时器
    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_MODE,
        .timer_num = LEDC_TIMER,
        .duty_resolution = LEDC_DUTY_RES,
        .freq_hz = LEDC_FREQUENCY,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    // 配置 LEDC 通道
    ledc_channel_config_t ledc_channel = {
        .speed_mode = LEDC_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = gpio_num,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));

    // 关闭 LED（初始状态）
    ledc_set_duty(LEDC_MODE, LEDC_CHANNEL_0, 0);
    ledc_update_duty(LEDC_MODE, LEDC_CHANNEL_0);

    // 创建定时器（用于状态控制）
    esp_timer_create_args_t timer_args = {
        .callback = led_timer_callback,
        .arg = NULL,
        .name = "led_timer"
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_led_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_led_timer, 20));  // 20ms 更新周期

    s_initialized = true;
    ESP_LOGI(TAG, "LED initialized successfully");

    return ESP_OK;
}

/**
 * @brief 设置 LED 状态
 */
void led_set_state(led_state_t state)
{
    if (!s_initialized) {
        ESP_LOGW(TAG, "LED not initialized");
        return;
    }

    ESP_LOGI(TAG, "LED state: %d", state);
    s_current_state = state;
    s_blink_target = 0;  // 重置闪烁计数
    s_blink_count = 0;
}

/**
 * @brief 关闭 LED
 */
void led_off(void)
{
    led_set_state(LED_STATE_OFF);
}
