/**
 * @file button.c
 * @brief 按键控制模块 - 源文件（Event 版）
 *
 * 按键事件通过 event_bus 广播，不再依赖直接回调。
 *
 * 事件分发：
 *   按下          → EVENT_BUTTON_PRESSED
 *   松开          → EVENT_BUTTON_RELEASED
 *   单击          → EVENT_BUTTON_CLICKED（消抖后 300ms 内无双击时）
 *   双击          → EVENT_BUTTON_DOUBLE_CLICKED
 *   长按触发      → EVENT_BUTTON_LONG_PRESSED（到达阈值时一次）
 *   持续按住      → EVENT_BUTTON_HOLD（阈值后每 500ms 重复）
 */

#include "button.h"
#include "event_bus.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>

static const char *TAG = "button";

/* 时间参数 */
#define DEBOUNCE_TIME_MS       50   /* 消抖 */
#define DOUBLE_CLICK_TIME_MS  300   /* 双击判定窗口 */
#define HOLD_INTERVAL_MS       500  /* HOLD 重复间隔 */
#define LONG_PRESS_DEFAULT_MS 1500  /* 默认长按阈值 */

/* 最大支持按键数 */
#define MAX_BUTTONS  3

/* 按键内部状态 */
typedef enum {
    BTN_STATE_IDLE      = 0,
    BTN_STATE_DEBOUNCE  = 1,
    BTN_STATE_PRESSED   = 2,
    BTN_STATE_LONG      = 3,
} btn_state_t;

/* 按键信息 */
typedef struct {
    gpio_num_t   gpio;
    btn_state_t  state;
    uint64_t     press_start_ms;  /* 按下时刻（ms） */
    uint64_t     last_long_ms;    /* 上次发送 HOLD 的时刻 */
    bool         last_level;
} button_info_t;

/* 全局状态 */
static button_info_t   s_buttons[MAX_BUTTONS] = {0};
static int             s_button_count         = 0;
static esp_timer_handle_t s_polling_timer     = NULL;
static uint32_t        s_long_press_ms        = LONG_PRESS_DEFAULT_MS;

/* 内部事件队列（兼容 button_get_event 旧接口）*/
#define EVT_QUEUE_SIZE  16
static button_event_t  s_evt_queue[EVT_QUEUE_SIZE];
static int             s_evt_head = 0;
static int             s_evt_tail = 0;

/* 双击等待状态 */
static uint64_t        s_last_click_ms     = 0;
static bool            s_waiting_double    = false;

/*————————————————————————————
 * 内部：向 event_bus 发布按键数据
 *————————————————————————————*/
static void publish_event(event_type_t type, gpio_num_t gpio)
{
    event_button_data_t data = {
        .gpio_num = (int)gpio,
    };
    event_bus_publish(type, &data, sizeof(data));
}

/*————————————————————————————
 * 内部：向内部事件队列推入事件（button_get_event 兼容）
 *————————————————————————————*/
static void push_event(button_event_t ev)
{
    int next = (s_evt_head + 1) % EVT_QUEUE_SIZE;
    if (next != s_evt_tail) {
        s_evt_queue[s_evt_head] = ev;
        s_evt_head = next;
    }
}

/*————————————————————————————
 * 轮询定时器回调
 *————————————————————————————*/
static void polling_callback(void *arg)
{
    (void)arg;
    int64_t now_ms = esp_timer_get_time() / 1000;

    for (int i = 0; i < s_button_count; i++) {
        button_info_t *btn = &s_buttons[i];
        int level = gpio_get_level(btn->gpio);

        switch (btn->state) {
        case BTN_STATE_IDLE:
            if (level == 0) {  // 低电平按下
                btn->state       = BTN_STATE_DEBOUNCE;
                btn->press_start_ms = now_ms;
                btn->last_long_ms  = 0;
            }
            break;

        case BTN_STATE_DEBOUNCE:
            if (level == 0 && (now_ms - btn->press_start_ms) >= DEBOUNCE_TIME_MS) {
                /* 消抖通过，按下生效 */
                btn->state        = BTN_STATE_PRESSED;
                btn->press_start_ms = now_ms;

                ESP_LOGI(TAG, "GPIO%d: PRESS", btn->gpio);
                publish_event(EVENT_BUTTON_PRESSED, btn->gpio);
                push_event(BUTTON_EVENT_PRESS);

            } else if (level == 1) {
                /* 消抖期间松开，忽略 */
                btn->state = BTN_STATE_IDLE;
            }
            break;

        case BTN_STATE_PRESSED: {
            bool released = (level == 1);
            bool long_reached = ((now_ms - btn->press_start_ms) >= s_long_press_ms);

            if (released) {
                /* 在 LONG_PRESS 之前松开 = 普通短按 */
                btn->state = BTN_STATE_IDLE;

                /* 检查双击 */
                if (s_waiting_double && (now_ms - s_last_click_ms) < DOUBLE_CLICK_TIME_MS) {
                    ESP_LOGI(TAG, "GPIO%d: DOUBLE_CLICK", btn->gpio);
                    publish_event(EVENT_BUTTON_DOUBLE_CLICKED, btn->gpio);
                    push_event(BUTTON_EVENT_DOUBLE_CLICK);
                    s_waiting_double = false;
                } else {
                    /* 300ms 后若无双击，发送 CLICK */
                    s_waiting_double = true;
                    s_last_click_ms = now_ms;
                }

                ESP_LOGI(TAG, "GPIO%d: RELEASE", btn->gpio);
                publish_event(EVENT_BUTTON_RELEASED, btn->gpio);
                push_event(BUTTON_EVENT_RELEASE);

            } else if (long_reached) {
                /* 到达长按阈值：LONG_PRESS + 进入 HOLD 状态 */
                btn->state       = BTN_STATE_LONG;
                btn->last_long_ms = now_ms;

                ESP_LOGI(TAG, "GPIO%d: LONG_PRESS", btn->gpio);
                publish_event(EVENT_BUTTON_LONG_PRESSED, btn->gpio);
                push_event(BUTTON_EVENT_LONG_PRESS);

                /* 立即发送第一个 HOLD */
                ESP_LOGD(TAG, "GPIO%d: HOLD", btn->gpio);
                publish_event(EVENT_BUTTON_HOLD, btn->gpio);
            }
            break;
        }

        case BTN_STATE_LONG: {
            bool released = (level == 1);

            if (released) {
                /* 释放 */
                btn->state = BTN_STATE_IDLE;
                ESP_LOGI(TAG, "GPIO%d: RELEASE (after hold)", btn->gpio);
                publish_event(EVENT_BUTTON_RELEASED, btn->gpio);
                push_event(BUTTON_EVENT_RELEASE);

            } else {
                /* 持续按住：每 HOLD_INTERVAL_MS 发送一次 HOLD */
                if (now_ms - btn->last_long_ms >= HOLD_INTERVAL_MS) {
                    btn->last_long_ms = now_ms;
                    ESP_LOGD(TAG, "GPIO%d: HOLD", btn->gpio);
                    publish_event(EVENT_BUTTON_HOLD, btn->gpio);
                }
            }
            break;
        }
        } /* switch */
    } /* for each button */

    /* 处理双击超时：300ms 内无第二次按下，发送 CLICK */
    if (s_waiting_double && (now_ms - s_last_click_ms) >= DOUBLE_CLICK_TIME_MS) {
        s_waiting_double = false;
        ESP_LOGI(TAG, "CLICK (single)");
        publish_event(EVENT_BUTTON_CLICKED, s_buttons[0].gpio);  /* 单按键场景 */
        push_event(BUTTON_EVENT_PRESS);  /* CLICK */
    }
}

/*————————————————————————————
 * button_init
 *————————————————————————————*/
esp_err_t button_init(gpio_num_t gpio_num)
{
    if (s_button_count >= MAX_BUTTONS) {
        ESP_LOGE(TAG, "Max buttons reached (%d)", MAX_BUTTONS);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Initializing button GPIO%d", gpio_num);

    gpio_config_t io_conf = {
        .pin_bit_mask    = (1ULL << gpio_num),
        .mode            = GPIO_MODE_INPUT,
        .pull_up_en      = GPIO_PULLUP_ENABLE,
        .pull_down_en    = GPIO_PULLDOWN_DISABLE,
        .intr_type       = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    button_info_t *btn = &s_buttons[s_button_count];
    memset(btn, 0, sizeof(*btn));
    btn->gpio       = gpio_num;
    btn->state      = BTN_STATE_IDLE;
    btn->last_level = gpio_get_level(gpio_num);
    s_button_count++;

    /* 仅第一个按键创建定时器 */
    if (s_button_count == 1) {
        const esp_timer_create_args_t timer_args = {
            .callback      = &polling_callback,
            .arg           = NULL,
            .name          = "btn_poll",
            .dispatch_method = ESP_TIMER_TASK,
        };
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_polling_timer));
        ESP_ERROR_CHECK(esp_timer_start_periodic(s_polling_timer, 20 * 1000));  // 20ms
        ESP_LOGI(TAG, "Button polling timer started (20ms)");
    }

    ESP_LOGI(TAG, "Button GPIO%d registered (total: %d)", gpio_num, s_button_count);
    return ESP_OK;
}

/*————————————————————————————
 * button_get_event
 *————————————————————————————*/
button_event_t button_get_event(void)
{
    if (s_evt_tail == s_evt_head) {
        return (button_event_t)-1;
    }
    int next = (s_evt_tail + 1) % EVT_QUEUE_SIZE;
    button_event_t ev = s_evt_queue[s_evt_tail];
    s_evt_tail = next;
    return ev;
}

/*————————————————————————————
 * button_set_long_press_time
 *————————————————————————————*/
void button_set_long_press_time(uint32_t long_press_ms)
{
    s_long_press_ms = long_press_ms;
    ESP_LOGI(TAG, "Long press threshold: %lu ms", long_press_ms);
}
