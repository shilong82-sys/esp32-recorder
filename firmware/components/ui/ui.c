/**
 * @file ui.c
 * @brief UI 组件 - 实现
 *
 * 职责：
 * - 管理 LED 状态机（订阅 EVENT_STATE_CHANGED，驱动 led pattern）
 * - 提供 ui_led_override() 临时覆盖机制
 *
 * 不直接操作 RMT/GPIO，所有硬件通过 led 组件封装。
 */

#include "ui.h"
#include "event_bus.h"
#include "state.h"
#include "led.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "ui";

/* UI 任务栈大小和优先级
 * 栈设大一些：ui task 订阅 event_bus，事件回调链可能很深
 * (event_bus → on_state_changed → ui_refresh_by_state → led_set_pattern → rmt)
 * 增大至 8192 字节：消除 >70% 水位线警告，保持稳定裕量。 */
#define UI_TASK_STACK_SIZE  (8192)
#define UI_TASK_PRIORITY    (2)
#define UI_TASK_NAME        "ui"

/* 状态 → LED Pattern 映射表 */
typedef struct {
    device_state_t  state;
    led_pattern_t   pattern;
    uint8_t         r, g, b;
    uint16_t        param1;   /* BLINK=频率Hz, BREATHING=周期ms */
    uint16_t        param2;   /* BLINK=占空比%, BREATHING=最小亮度% */
} state_led_map_t;

static const state_led_map_t s_state_map[] = {
    /* 状态             Pattern        R    G    B    param1        param2 */
    { DEVICE_STATE_INIT,       LED_PATTERN_BREATHING,  0,   0, 255,  2000,  10 },  /* 蓝色呼吸灯 */
    { DEVICE_STATE_IDLE,       LED_PATTERN_STATIC,    0, 200,   0,     0,   0 },  /* 绿色常亮 */
    { DEVICE_STATE_RECORDING,  LED_PATTERN_BLINK,    255,   0,   0,     4,  50 },  /* 红色快闪 4Hz */
    { DEVICE_STATE_UPLOADING,  LED_PATTERN_BREATHING,100,   0, 255,  1000,  20 },  /* 紫色呼吸 1Hz */
    { DEVICE_STATE_ERROR,      LED_PATTERN_BLINK,    255,   0,   0,     1,  50 },  /* 红色慢闪 1Hz */
    { DEVICE_STATE_SLEEP,      LED_PATTERN_OFF,       0,   0,   0,     0,   0 },  /* 熄灭 */
};

/* UI 内部上下文 */
typedef struct {
    bool                initialized;
    bool                override_active;
    int64_t             override_end_us;   /* 覆盖结束时间（微秒），0=永久 */
    event_handler_t     state_handler;     /* EVENT_STATE_CHANGED 订阅句柄 */
    esp_timer_handle_t  override_timer;   /* 覆盖恢复定时器 */
} ui_context_t;

static ui_context_t s_ctx = {0};

/*————————————————————————————
 * 内部：获取状态对应的 LED 配置
 *————————————————————————————*/
static const state_led_map_t* ui_get_map(device_state_t state)
{
    for (size_t i = 0; i < sizeof(s_state_map) / sizeof(s_state_map[0]); i++) {
        if (s_state_map[i].state == state) {
            return &s_state_map[i];
        }
    }
    return &s_state_map[0]; /* fallback: INIT */
}

/*————————————————————————————
 * 内部：应用一个 LED pattern
 *————————————————————————————*/
static void ui_apply_pattern(const led_pattern_config_t *cfg)
{
    led_set_pattern(cfg);
}

/*————————————————————————————
 * 内部：根据当前设备状态刷新 LED
 *————————————————————————————*/
static void ui_refresh_by_state(void)
{
    if (s_ctx.initialized) {
        const state_led_map_t *map = ui_get_map(state_get());
        led_pattern_config_t cfg = {
            .pattern = map->pattern,
            .r        = map->r,
            .g        = map->g,
            .b        = map->b,
            .param1   = map->param1,
            .param2   = map->param2,
        };
        ui_apply_pattern(&cfg);
    }
}

/*————————————————————————————
 * 覆盖定时器回调
 *————————————————————————————*/
static void override_timer_callback(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Override expired, restoring state-driven LED");
    s_ctx.override_active = false;
    s_ctx.override_end_us = 0;
    ui_refresh_by_state();
}

/*————————————————————————————
 * EVENT_STATE_CHANGED 回调
 *————————————————————————————*/
static void on_state_changed(event_type_t type, const void *data, size_t len, void *user_data)
{
    (void)type; (void)len; (void)user_data;

    /* 忽略覆盖状态，直接恢复到状态机驱动 */
    if (s_ctx.override_active) {
        /* 下次覆盖结束后会恢复 */
        return;
    }

    const event_state_data_t *ev = (const event_state_data_t *)data;
    ESP_LOGI(TAG, "State changed: %s -> %s",
             state_to_string(ev->prev_state),
             state_to_string(ev->curr_state));

    ui_refresh_by_state();
}

/*————————————————————————————
 * UI 任务入口
 *————————————————————————————*/
static void ui_task_entry(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "UI task started");

    /* 注册状态变化监听 */
    s_ctx.state_handler = event_bus_subscribe(EVENT_STATE_CHANGED,
                                               on_state_changed, NULL);
    if (s_ctx.state_handler < 0) {
        ESP_LOGE(TAG, "Failed to subscribe EVENT_STATE_CHANGED");
    }

    /* 初始化时应用一次当前状态 */
    ui_refresh_by_state();

    /* 任务主循环（空循环，等待事件驱动）*/
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));  /* 1s 空循环，刷新由事件触发 */
    }
}

/*————————————————————————————
 * ui_init
 *————————————————————————————*/
esp_err_t ui_init(void)
{
    if (s_ctx.initialized) {
        ESP_LOGW(TAG, "UI already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing UI component");

    memset(&s_ctx, 0, sizeof(s_ctx));

    /* 创建覆盖恢复定时器（单次，不自动启动）*/
    const esp_timer_create_args_t timer_args = {
        .callback      = &override_timer_callback,
        .name          = "ui_override",
        .arg           = &s_ctx,
        .dispatch_method = ESP_TIMER_TASK,
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_ctx.override_timer));

    s_ctx.initialized = true;

    /* 创建 UI FreeRTOS 任务 */
    BaseType_t ret = xTaskCreatePinnedToCore(
        &ui_task_entry,
        UI_TASK_NAME,
        UI_TASK_STACK_SIZE,
        NULL,
        UI_TASK_PRIORITY,
        NULL,
        0  /* 固定 Core 0 */
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create UI task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "UI initialized OK");
    return ESP_OK;
}

/*————————————————————————————
 * ui_led_override
 *————————————————————————————*/
esp_err_t ui_led_override(led_pattern_t pattern, uint8_t r, uint8_t g, uint8_t b, uint32_t duration_ms)
{
    if (!s_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 停止之前的覆盖定时器 */
    if (s_ctx.override_active) {
        esp_timer_stop(s_ctx.override_timer);
    }

    led_pattern_config_t cfg = {
        .pattern = pattern,
        .r       = r,
        .g       = g,
        .b       = b,
        .param1  = 0,
        .param2  = 0,
    };
    led_set_pattern(&cfg);

    if (duration_ms > 0) {
        s_ctx.override_active = true;
        s_ctx.override_end_us = esp_timer_get_time() + (int64_t)duration_ms * 1000;
        esp_timer_start_once(s_ctx.override_timer, (uint64_t)duration_ms * 1000);
        ESP_LOGI(TAG, "LED override: pattern=%d, RGB(%d,%d,%d), duration=%lu ms",
                 pattern, r, g, b, (unsigned long)duration_ms);
    } else {
        s_ctx.override_active = true;
        s_ctx.override_end_us = 0;
        ESP_LOGI(TAG, "LED override (permanent): pattern=%d, RGB(%d,%d,%d)",
                 pattern, r, g, b);
    }

    return ESP_OK;
}

/*————————————————————————————
 * ui_led_override_cancel
 *————————————————————————————*/
void ui_led_override_cancel(void)
{
    if (!s_ctx.initialized) return;

    if (s_ctx.override_active) {
        esp_timer_stop(s_ctx.override_timer);
        s_ctx.override_active = false;
        s_ctx.override_end_us = 0;
        ESP_LOGI(TAG, "Override cancelled, restoring state-driven LED");
        ui_refresh_by_state();
    }
}
