/**
 * @file button.c
 * @brief 按键控制模块 - 源文件
 *
 * 功能：
 * - 短按 / 长按 / 双击检测
 * - 消抖处理（软件定时器）
 * - 按键事件回调
 */

#include "button.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "button";

// 消抖参数
#define DEBOUNCE_TIME_MS 50      // 消抖时间
#define DOUBLE_CLICK_TIME_MS 300 // 双击间隔

// 按键 GPIO 配置
#define BUTTON_PIN_SEL ((1ULL << 0) | (1ULL << 1) | (1ULL << 2))  // 最多支持 3 个按键

// 按键状态
typedef enum {
    BTN_STATE_IDLE = 0,
    BTN_STATE_DEBOUNCE,
    BTN_STATE_PRESSED,
    BTN_STATE_LONG_PRESS,
} btn_state_t;

// 按键信息
typedef struct {
    gpio_num_t gpio;
    btn_state_t state;
    uint64_t press_start_time;
    button_callback_t callback;
    void *callback_arg;
    bool last_level;
} button_info_t;

// 全局状态
static button_info_t s_buttons[3] = {0};
static int s_button_count = 0;
static esp_timer_handle_t s_polling_timer = NULL;
static uint32_t s_long_press_ms = 1500;

// 事件队列（用于事件通知）
#define EVENT_QUEUE_SIZE 10
static button_event_t s_event_queue[EVENT_QUEUE_SIZE];
static int s_event_head = 0;
static int s_event_tail = 0;
static int s_last_click_time = 0;
static bool s_waiting_double_click = false;

/**
 * @brief 添加事件到队列
 */
static void push_event(button_event_t event)
{
    int next = (s_event_head + 1) % EVENT_QUEUE_SIZE;
    if (next != s_event_tail) {
        s_event_queue[s_event_head] = event;
        s_event_head = next;
    }
}

/**
 * @brief 从队列获取事件
 */
static button_event_t pop_event(void)
{
    if (s_event_tail == s_event_head) {
        return (button_event_t)-1;  // 队列为空
    }
    int next = (s_event_tail + 1) % EVENT_QUEUE_SIZE;
    button_event_t event = s_event_queue[s_event_tail];
    s_event_tail = next;
    return event;
}

/**
 * @brief 按键轮询定时器回调
 */
static void polling_callback(void *arg)
{
    int64_t now = esp_timer_get_time() / 1000;  // 转换为毫秒

    for (int i = 0; i < s_button_count; i++) {
        button_info_t *btn = &s_buttons[i];
        int level = gpio_get_level(btn->gpio);

        switch (btn->state) {
        case BTN_STATE_IDLE:
            if (level == 0) {  // 按键按下（低电平有效）
                btn->state = BTN_STATE_DEBOUNCE;
                btn->press_start_time = now;
                ESP_LOGD(TAG, "Button %d: DEBOUNCE", btn->gpio);
            }
            break;

        case BTN_STATE_DEBOUNCE:
            if (level == 0 && (now - btn->press_start_time) >= DEBOUNCE_TIME_MS) {
                btn->state = BTN_STATE_PRESSED;
                btn->press_start_time = now;
                ESP_LOGI(TAG, "Button %d: PRESSED", btn->gpio);

                // 检查是否为双击
                if (s_waiting_double_click && (now - s_last_click_time) < DOUBLE_CLICK_TIME_MS) {
                    push_event(BUTTON_EVENT_DOUBLE_CLICK);
                    s_waiting_double_click = false;
                    ESP_LOGI(TAG, "Button %d: DOUBLE CLICK", btn->gpio);
                } else {
                    s_waiting_double_click = true;
                    s_last_click_time = now;
                }

                // 触发回调
                if (btn->callback) {
                    btn->callback(BUTTON_EVENT_PRESS, btn->callback_arg);
                }
            } else if (level == 1) {
                btn->state = BTN_STATE_IDLE;  // 消抖期间释放，忽略
            }
            break;

        case BTN_STATE_PRESSED:
            if (level == 1) {  // 按键释放
                btn->state = BTN_STATE_IDLE;
                push_event(BUTTON_EVENT_RELEASE);
                ESP_LOGI(TAG, "Button %d: RELEASE", btn->gpio);

                if (btn->callback) {
                    btn->callback(BUTTON_EVENT_RELEASE, btn->callback_arg);
                }
            } else if ((now - btn->press_start_time) >= s_long_press_ms) {
                btn->state = BTN_STATE_LONG_PRESS;
                push_event(BUTTON_EVENT_LONG_PRESS);
                ESP_LOGI(TAG, "Button %d: LONG PRESS", btn->gpio);

                if (btn->callback) {
                    btn->callback(BUTTON_EVENT_LONG_PRESS, btn->callback_arg);
                }
            }
            break;

        case BTN_STATE_LONG_PRESS:
            if (level == 1) {  // 按键释放
                btn->state = BTN_STATE_IDLE;
                push_event(BUTTON_EVENT_RELEASE);
                ESP_LOGI(TAG, "Button %d: RELEASE after long press", btn->gpio);

                if (btn->callback) {
                    btn->callback(BUTTON_EVENT_RELEASE, btn->callback_arg);
                }
            }
            break;
        }
    }

    // 处理双击超时
    if (s_waiting_double_click && (now - s_last_click_time) >= DOUBLE_CLICK_TIME_MS) {
        s_waiting_double_click = false;
    }
}

/**
 * @brief 初始化按键模块
 */
esp_err_t button_init(gpio_num_t gpio_num, button_callback_t cb, void *arg)
{
    if (s_button_count >= 3) {
        ESP_LOGE(TAG, "Maximum buttons reached");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Initializing button on GPIO %d", gpio_num);

    // 配置 GPIO
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << gpio_num),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    // 保存按键信息
    button_info_t *btn = &s_buttons[s_button_count];
    btn->gpio = gpio_num;
    btn->state = BTN_STATE_IDLE;
    btn->callback = cb;
    btn->callback_arg = arg;
    btn->last_level = gpio_get_level(gpio_num);
    s_button_count++;

    // 如果是第一个按键，创建轮询定时器
    if (s_button_count == 1) {
        esp_timer_create_args_t timer_args = {
            .callback = polling_callback,
            .arg = NULL,
            .name = "button_polling"
        };
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_polling_timer));
        ESP_ERROR_CHECK(esp_timer_start_periodic(s_polling_timer, 20));  // 20ms 轮询
        ESP_LOGI(TAG, "Button polling timer started (20ms interval)");
    }

    ESP_LOGI(TAG, "Button initialized. Total buttons: %d", s_button_count);
    return ESP_OK;
}

/**
 * @brief 获取当前按键事件（非阻塞）
 */
button_event_t button_get_event(void)
{
    return pop_event();
}

/**
 * @brief 设置长按时间阈值
 */
void button_set_long_press_time(uint32_t long_press_ms)
{
    s_long_press_ms = long_press_ms;
    ESP_LOGI(TAG, "Long press threshold set to %lu ms", long_press_ms);
}
