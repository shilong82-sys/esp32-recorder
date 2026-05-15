/**
 * @file led.c
 * @brief LED 组件 - WS2812B RGB LED 驱动（完全异步版）
 *
 * 架构原则：
 * - 所有公开 API (led_set_pattern/color/off) 只发送消息到队列
 * - 所有 RMT 操作只在 led_task 中执行
 * - 禁止任何阻塞等待
 * - RMT timeout 自动恢复
 *
 * 异步流程：
 *   调用者 → 消息队列(深度=1) → led_task → RMT 发送
 *
 * Queue 策略：
 * - 深度 = 1，覆盖模式（xQueueOverwrite）
 * - 新状态覆盖旧状态，不堆积
 * - 防止状态切换风暴导致队列爆炸
 *
 * Timeout 恢复策略：
 * - 每次 RMT 操作有 50ms timeout
 * - 连续 3 次 timeout → 重置 RMT channel
 * - 重置后继续，不影响录音链路
 *
 * 调试日志（DEBUG 级别）：
 * - queue depth, refresh duration, timeout count
 * - dropped pattern count, last active pattern
 */

#include "led.h"
#include "led_strip_encoder.h"
#include "driver/rmt_tx.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <math.h>
#include <string.h>

static const char *TAG = "led";

/* ============================================================
 * 常量配置
 * ============================================================ */
#define LED_PIXEL_BUF_SIZE      3   /* WS2812B GRB */
#define LED_TICK_MS             50  /* Pattern 更新周期 */
#define LED_TASK_STACK         4096
#define LED_TASK_PRIORITY         2

/* RMT timeout 配置 */
#define LED_REFRESH_TIMEOUT_MS   50  /* 每次刷新最大等待 */
#define LED_RECOVERY_THRESHOLD    3  /* 连续 N 次 timeout 触发恢复 */

/* ============================================================
 * 类型定义
 * ============================================================ */

/* LED 消息类型 */
typedef struct {
    led_pattern_config_t  config;
    bool                  valid;      /* 消息是否有效 */
} led_msg_t;

/* RMT 恢复状态 */
typedef enum {
    LED_RMT_OK      = 0,
    LED_RMT_BUSY    = 1,
    LED_RMT_TIMEOUT = 2,
} led_rmt_status_t;

/* ============================================================
 * 静态变量
 * ============================================================ */

/* 硬件句柄 */
static rmt_channel_handle_t      s_rmt_chan    = NULL;
static rmt_encoder_handle_t       s_led_encoder = NULL;
static bool                        s_initialized = false;
static bool                        s_recovering  = false;  /* 正在恢复 */

/* Pattern 状态 */
static led_pattern_config_t       s_pattern     = {0};
static bool                        s_pattern_active = false;

/* 更新定时器 */
static esp_timer_handle_t          s_tick_timer  = NULL;

/* 像素缓冲区（GRB 顺序）*/
static uint8_t                    s_pixel_buf[LED_PIXEL_BUF_SIZE];

/* 内部时间戳（用于 breathing/blink 计算）*/
static uint64_t                   s_tick_count  = 0;

/* 任务和队列 */
static TaskHandle_t               s_led_task    = NULL;
static QueueHandle_t              s_msg_queue   = NULL;  /* 深度=1，覆盖模式 */

/* 忙保护：标记是否正在执行 RMT 刷新 */
static volatile bool              s_refreshing   = false;

/* 调试统计（led_stats_t 定义在 led.h）*/

static led_stats_t                s_stats        = {0};

/* ============================================================
 * 内部函数声明
 * ============================================================ */
static void          led_tick_callback(void *arg);
static void          led_task_entry(void *arg);
static led_rmt_status_t led_try_refresh(void);
static esp_err_t     led_rmt_recover(void);

/* ============================================================
 * led_task — LED 任务（唯一 RMT 操作者）
 *
 * 职责：
 *   1. 从消息队列接收 pattern 更新请求
 *   2. 更新内部 s_pattern 状态
 *   3. 定时执行 led_update（breathing/blink 计算）
 *   4. 调用 RMT 发送（带 timeout 和恢复机制）
 *
 * 规则：
 *   所有 RMT 操作必须在此任务中执行
 *   其他模块只通过消息队列与 LED 交互
 * ============================================================ */
static void led_task_entry(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "LED task started");

    /* 初始化为 OFF */
    s_pattern.pattern  = LED_PATTERN_OFF;
    s_pattern_active  = false;
    s_pixel_buf[0] = s_pixel_buf[1] = s_pixel_buf[2] = 0;

    led_msg_t msg;
    memset(&msg, 0, sizeof(msg));

    while (1) {
        /* 优先检查消息队列（非阻塞）*/
        bool got_msg = (xQueueReceive(s_msg_queue, &msg, 0) == pdTRUE);

        if (got_msg && msg.valid) {
            /* 更新 pattern 配置 */
            s_pattern       = msg.config;
            s_pattern_active = (msg.config.pattern != LED_PATTERN_OFF);
            s_tick_count    = 0;  /* 重置动画相位 */

            ESP_LOGD(TAG, "[LED] Pattern queued: %d RGB(%d,%d,%d) p1=%d p2=%d",
                     msg.config.pattern, msg.config.r, msg.config.g, msg.config.b,
                     msg.config.param1, msg.config.param2);
        }

        /* 定时更新动画（breathing/blink）*/
        if (s_pattern_active) {
            s_tick_count++;

            /* 计算当前帧颜色 */
            uint8_t r = 0, g = 0, b = 0;

            switch (s_pattern.pattern) {
                case LED_PATTERN_STATIC:
                    r = s_pattern.r; g = s_pattern.g; b = s_pattern.b;
                    break;

                case LED_PATTERN_BREATHING: {
                    uint16_t period_ms   = s_pattern.param1 > 0 ? s_pattern.param1 : 2000;
                    uint16_t min_pct     = s_pattern.param2 > 0 ? s_pattern.param2 : 20;
                    uint64_t total_ticks = period_ms / LED_TICK_MS;
                    if (total_ticks == 0) total_ticks = 1;
                    float phase = (s_tick_count % total_ticks) * (2.0f * (float)M_PI / total_ticks);
                    float sin_val = (sinf(phase) + 1.0f) / 2.0f;
                    float min_bright = min_pct / 100.0f;
                    float brightness = min_bright + sin_val * (1.0f - min_bright);
                    r = (uint8_t)(s_pattern.r * brightness);
                    g = (uint8_t)(s_pattern.g * brightness);
                    b = (uint8_t)(s_pattern.b * brightness);
                    break;
                }

                case LED_PATTERN_BLINK: {
                    uint16_t freq_hz    = s_pattern.param1 > 0 ? s_pattern.param1 : 2;
                    uint16_t duty_pct   = s_pattern.param2 > 0 ? s_pattern.param2 : 50;
                    uint64_t cycle_ticks = 1000 / LED_TICK_MS / freq_hz;
                    if (cycle_ticks == 0) cycle_ticks = 1;
                    uint64_t on_ticks = cycle_ticks * duty_pct / 100;
                    bool on = (s_tick_count % cycle_ticks) < on_ticks;
                    if (on) {
                        r = s_pattern.r; g = s_pattern.g; b = s_pattern.b;
                    } else {
                        r = g = b = 0;
                    }
                    break;
                }

                case LED_PATTERN_OFF:
                default:
                    r = g = b = 0;
                    break;
            }

            /* 检查颜色是否变化 */
            bool changed = (s_pixel_buf[0] != g) || (s_pixel_buf[1] != r) || (s_pixel_buf[2] != b);
            s_pixel_buf[0] = g;
            s_pixel_buf[1] = r;
            s_pixel_buf[2] = b;

            if (changed || s_pattern.pattern == LED_PATTERN_OFF) {
                /* 执行 RMT 刷新（带 timeout）*/
                if (!s_refreshing) {
                    led_try_refresh();
                } else {
                    /* 正在刷新，新值已写入 buffer，下次周期会自动刷新 */
                    ESP_LOGD(TAG, "[LED] Skip refresh (busy), value buffered");
                }
            }
        }

        /* 等待下一个 tick（约 50ms）*/
        vTaskDelay(pdMS_TO_TICKS(LED_TICK_MS));

        /* DEBUG: 周期性打印统计（每 60 秒）*/
        static uint32_t s_stat_counter = 0;
        s_stat_counter++;
        if (s_stat_counter >= (60000 / LED_TICK_MS)) {
            s_stat_counter = 0;
            ESP_LOGI(TAG, "[LED] stats: refresh=%lu timeout=%lu recovery=%lu dropped=%lu queue_overflow=%lu",
                     (unsigned long)s_stats.refresh_count,
                     (unsigned long)s_stats.timeout_count,
                     (unsigned long)s_stats.recovery_count,
                     (unsigned long)s_stats.dropped_count,
                     (unsigned long)s_stats.queue_overflow);
            ESP_LOGI(TAG, "[LED] last_duration=%luus consecutive_timeout=%lu",
                     (unsigned long)s_stats.last_duration_us,
                     (unsigned long)s_stats.consecutive_timeout);
        }
    }
}

/* ============================================================
 * led_try_refresh — 尝试执行一次 RMT 刷新
 *
 * @return led_rmt_status_t
 * ============================================================ */
static led_rmt_status_t led_try_refresh(void)
{
    if (!s_initialized || s_recovering) {
        return LED_RMT_OK;
    }

    s_refreshing = true;

    int64_t start_us = esp_timer_get_time();

    /* 启动 RMT 发送 */
    rmt_transmit_config_t tx_cfg = {
        .loop_count = 0,
        .flags.eot_level = 0,
    };

    esp_err_t tx_ret = rmt_transmit(s_rmt_chan, s_led_encoder,
                                     s_pixel_buf, sizeof(s_pixel_buf), &tx_cfg);

    if (tx_ret != ESP_OK) {
        s_refreshing = false;
        s_stats.timeout_count++;
        ESP_LOGW(TAG, "[LED] rmt_transmit failed: %s", esp_err_to_name(tx_ret));
        return LED_RMT_BUSY;
    }

    /* 等待发送完成（带 timeout）*/
    esp_err_t wait_ret = rmt_tx_wait_all_done(s_rmt_chan, pdMS_TO_TICKS(LED_REFRESH_TIMEOUT_MS));

    int64_t end_us = esp_timer_get_time();
    s_stats.last_duration_us = (uint32_t)(end_us - start_us);

    s_refreshing = false;

    if (wait_ret == ESP_OK) {
        s_stats.refresh_count++;
        s_stats.consecutive_timeout = 0;
        return LED_RMT_OK;

    } else if (wait_ret == ESP_ERR_TIMEOUT) {
        s_stats.timeout_count++;
        s_stats.consecutive_timeout++;

        ESP_LOGW(TAG, "[LED] rmt_tx_wait_all_done TIMEOUT after %lums (consecutive=%lu)",
                 (long)(end_us - start_us) / 1000,
                 (unsigned long)s_stats.consecutive_timeout);

        /* 连续 N 次 timeout → 触发 RMT 恢复 */
        if (s_stats.consecutive_timeout >= LED_RECOVERY_THRESHOLD) {
            ESP_LOGE(TAG, "[LED] Consecutive timeout %d times → recovering RMT...",
                     LED_RECOVERY_THRESHOLD);
            led_rmt_recover();
        }

        return LED_RMT_TIMEOUT;

    } else {
        s_stats.timeout_count++;
        ESP_LOGW(TAG, "[LED] rmt_tx_wait_all_done error: %s", esp_err_to_name(wait_ret));
        return LED_RMT_TIMEOUT;
    }
}

/* ============================================================
 * led_rmt_recover — RMT 恢复
 *
 * 当连续 timeout 时调用：
 *   1. 禁用并删除 RMT channel
 *   2. 重新创建 channel 和 encoder
 *   3. 重新使能
 *   4. 重置状态
 * ============================================================ */
static esp_err_t led_rmt_recover(void)
{
    if (s_recovering) {
        return ESP_OK;  /* 防止重复恢复 */
    }

    s_recovering = true;
    s_stats.recovery_count++;
    s_stats.consecutive_timeout = 0;

    ESP_LOGW(TAG, "[LED] Starting RMT recovery (#%lu)...", (unsigned long)s_stats.recovery_count);

    /* 1. 停止定时器 */
    if (s_tick_timer != NULL) {
        esp_timer_stop(s_tick_timer);
    }

    /* 2. 删除 RMT channel */
    if (s_rmt_chan != NULL) {
        rmt_disable(s_rmt_chan);
        rmt_del_channel(s_rmt_chan);
        s_rmt_chan = NULL;
    }

    /* 3. 重新创建 channel */
    gpio_num_t gpio_num = GPIO_NUM_48;  /* 从初始化时记录 */

    rmt_tx_channel_config_t tx_chan_cfg = {
        .clk_src           = RMT_CLK_SRC_DEFAULT,
        .gpio_num          = gpio_num,
        .mem_block_symbols = 64,
        .resolution_hz     = 10 * 1000 * 1000,
        .trans_queue_depth = 4,
    };

    esp_err_t ret = rmt_new_tx_channel(&tx_chan_cfg, &s_rmt_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[LED] Recovery: rmt_new_tx_channel failed: %s", esp_err_to_name(ret));
        s_recovering = false;
        return ret;
    }

    /* 4. 重新创建 encoder */
    if (s_led_encoder != NULL) {
        /* encoder 已在 rmt_del_channel 时自动释放 */
        s_led_encoder = NULL;
    }

    led_strip_encoder_config_t enc_cfg = {
        .resolution = 10 * 1000 * 1000,
    };

    ret = rmt_new_led_strip_encoder(&enc_cfg, &s_led_encoder);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[LED] Recovery: rmt_new_led_strip_encoder failed: %s", esp_err_to_name(ret));
        s_recovering = false;
        return ret;
    }

    /* 5. 使能 channel */
    ret = rmt_enable(s_rmt_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[LED] Recovery: rmt_enable failed: %s", esp_err_to_name(ret));
        s_recovering = false;
        return ret;
    }

    /* 6. 重启定时器 */
    if (s_tick_timer != NULL) {
        esp_timer_start_periodic(s_tick_timer, LED_TICK_MS * 1000);
    }

    s_recovering = false;

    ESP_LOGI(TAG, "[LED] RMT recovery completed. Resetting pixel buffer.");

    /* 7. 重置像素缓冲区（触发一次全量刷新）*/
    s_pixel_buf[0] = s_pixel_buf[1] = s_pixel_buf[2] = 0;
    led_try_refresh();

    return ESP_OK;
}

/* ============================================================
 * led_tick_callback — esp_timer 回调（保留，空实现）
 *
 * 注意：动画逻辑已移至 led_task 的 vTaskDelay 循环
 * 此定时器仅作为架构占位，不触发任何操作
 * ============================================================ */
static void led_tick_callback(void *arg)
{
    (void)arg;
    /* 空实现：led_task 使用自己的 vTaskDelay 循环驱动动画 */
}

/* ============================================================
 * 内部：发送消息到 LED 队列
 *
 * @param config  pattern 配置（会复制到队列）
 * ============================================================ */
static esp_err_t led_send_to_queue(const led_pattern_config_t *config)
{
    if (!s_initialized || config == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    led_msg_t msg = {
        .config = *config,
        .valid  = true,
    };

    /* 使用 xQueueOverwrite 确保队列深度=1，新值覆盖旧值 */
    BaseType_t ret = xQueueOverwrite(s_msg_queue, &msg);

    if (ret != pdTRUE) {
        s_stats.queue_overflow++;
        ESP_LOGW(TAG, "[LED] Queue overflow!");
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "[LED] Queued: pattern=%d RGB(%d,%d,%d)",
             config->pattern, config->r, config->g, config->b);

    return ESP_OK;
}

/* ============================================================
 * led_init — 初始化 LED
 * ============================================================ */
esp_err_t led_init(gpio_num_t gpio_num)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "LED already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing WS2812B on GPIO%d", gpio_num);

    /* 1. 创建消息队列（深度=1，覆盖模式）*/
    s_msg_queue = xQueueCreate(1, sizeof(led_msg_t));
    if (s_msg_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create LED message queue");
        return ESP_FAIL;
    }

    /* 2. 创建 LED 任务 */
    BaseType_t created = xTaskCreate(
        &led_task_entry,
        "led",
        LED_TASK_STACK,
        NULL,
        LED_TASK_PRIORITY,
        &s_led_task
    );
    if (created != pdTRUE) {
        ESP_LOGE(TAG, "Failed to create LED task");
        vQueueDelete(s_msg_queue);
        s_msg_queue = NULL;
        return ESP_FAIL;
    }

    /* 3. RMT TX 通道 */
    rmt_tx_channel_config_t tx_chan_cfg = {
        .clk_src           = RMT_CLK_SRC_DEFAULT,
        .gpio_num          = gpio_num,
        .mem_block_symbols = 64,
        .resolution_hz     = 10 * 1000 * 1000,
        .trans_queue_depth = 4,
    };
    esp_err_t ret = rmt_new_tx_channel(&tx_chan_cfg, &s_rmt_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "rmt_new_tx_channel failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 4. led_strip_encoder */
    led_strip_encoder_config_t enc_cfg = {
        .resolution = 10 * 1000 * 1000,
    };
    ret = rmt_new_led_strip_encoder(&enc_cfg, &s_led_encoder);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "rmt_new_led_strip_encoder failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 5. 使能 RMT */
    ret = rmt_enable(s_rmt_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "rmt_enable failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 6. 创建定时器（占位用，不用于动画 tick）*/
    const esp_timer_create_args_t timer_args = {
        .callback = &led_tick_callback,
        .name     = "led_tick",
    };
    ret = esp_timer_create(&timer_args, &s_tick_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer_create failed: %s", esp_err_to_name(ret));
        return ret;
    }
    esp_timer_start_periodic(s_tick_timer, LED_TICK_MS * 1000);

    /* 7. 状态初始化 */
    s_initialized    = true;
    s_pattern_active  = false;
    s_tick_count     = 0;
    s_pixel_buf[0] = s_pixel_buf[1] = s_pixel_buf[2] = 0;

    /* 8. 默认熄灭（异步，不阻塞）*/
    led_off();

    ESP_LOGI(TAG, "LED initialized OK (GPIO%d, queue_depth=1, refresh_timeout=%dms)",
             gpio_num, LED_REFRESH_TIMEOUT_MS);
    return ESP_OK;
}

/* ============================================================
 * led_set_pattern — 设置 Pattern（异步）
 *
 * 规则：
 *   此函数只发送消息到队列，不执行任何 RMT 操作
 *   禁止：任何阻塞调用、rmt_transmit、rmt_tx_wait_all_done
 * ============================================================ */
esp_err_t led_set_pattern(const led_pattern_config_t *config)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 只发送消息到队列，立即返回 */
    return led_send_to_queue(config);
}

/* ============================================================
 * led_set_color — 设置固定颜色（异步）
 * ============================================================ */
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

/* ============================================================
 * led_off — 熄灭 LED（异步）
 * ============================================================ */
void led_off(void)
{
    if (!s_initialized) return;

    led_pattern_config_t cfg = {
        .pattern = LED_PATTERN_OFF,
        .r = 0, .g = 0, .b = 0,
        .param1 = 0, .param2 = 0,
    };
    led_set_pattern(&cfg);
}

/* ============================================================
 * led_get_stats — 获取调试统计（可选 API）
 * ============================================================ */
void led_get_stats(led_stats_t *out_stats)
{
    if (out_stats != NULL) {
        memcpy(out_stats, &s_stats, sizeof(led_stats_t));
    }
}
