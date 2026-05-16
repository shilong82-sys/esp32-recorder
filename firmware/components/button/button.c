/**
 * @file button.c
 * @brief 按键控制模块 - 源文件（Task 分离版）
 *
 * 架构原则：
 * - esp_timer callback (polling_callback) 只做 GPIO 读取和状态更新
 * - 所有业务逻辑（publish_event）都在 button_task 中执行
 * - 两者通过事件队列解耦
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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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

/* ============================================================
 * 任务分离：button 事件队列
 *
 * polling_callback 将检测到的事件放入此队列
 * button_task 从队列中取出并通过 event_bus 发布
 * ============================================================ */
#define BTN_EVT_QUEUE_SIZE  16

typedef enum {
    BTN_EVT_NONE = 0,
    BTN_EVT_PRESSED,
    BTN_EVT_RELEASED,
    BTN_EVT_CLICKED,
    BTN_EVT_DOUBLE_CLICKED,
    BTN_EVT_LONG_PRESSED,
    BTN_EVT_HOLD,
} btn_evt_type_t;

typedef struct {
    btn_evt_type_t type;
    gpio_num_t     gpio;
} btn_evt_t;

static QueueHandle_t s_evt_queue = NULL;
static TaskHandle_t  s_btn_task = NULL;

/* ============================================================
 * 全局状态（供 polling_callback 和 button_task 共享）
 * ============================================================ */
static button_info_t   s_buttons[MAX_BUTTONS] = {0};
static int             s_button_count         = 0;
static esp_timer_handle_t s_polling_timer     = NULL;
static uint32_t        s_long_press_ms        = LONG_PRESS_DEFAULT_MS;

/* 内部事件队列（兼容 button_get_event 旧接口）*/
#define BTN_LEGACY_EVT_QUEUE_SIZE  16
static button_event_t  s_legacy_evt_queue[BTN_LEGACY_EVT_QUEUE_SIZE];
static int             s_evt_head = 0;
static int             s_evt_tail = 0;

/* 双击等待状态 */
static uint64_t        s_last_click_ms     = 0;
static bool            s_waiting_double    = false;

/* Forward declaration for esp_timer callback */
static void polling_callback(void *arg);

/* ============================================================
 * 内部：向 button_task 发送事件
 * ============================================================ */
static void send_evt_to_task(btn_evt_type_t type, gpio_num_t gpio)
{
    if (s_evt_queue == NULL) return;

    btn_evt_t evt = {
        .type = type,
        .gpio = gpio,
    };

    /* 非阻塞发送，超时则丢弃（避免阻塞 esp_timer）*/
    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(s_evt_queue, &evt, &woken);
    if (woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

/* ============================================================
 * 内部：向 event_bus 发布按键数据
 * ============================================================ */
static void publish_event(event_type_t type, gpio_num_t gpio)
{
    event_button_data_t data = {
        .gpio_num = (int)gpio,
    };
    event_bus_publish(type, &data, sizeof(data));
}

/*----------------------------
 * 内部：向内部事件队列推入事件（button_get_event 兼容）
 *----------------------------*/
static void push_event(button_event_t ev)
{
    int next = (s_evt_head + 1) % BTN_LEGACY_EVT_QUEUE_SIZE;
    if (next != s_evt_tail) {
        s_legacy_evt_queue[s_evt_head] = ev;
        s_evt_head = next;
    }
}

/* ============================================================
 * button_task - 处理按钮事件（在独立任务中执行，非 esp_timer）
 *
 * 职责：消费事件队列，发布到 event_bus
 * 栈大小：8192 bytes（足够支持 ESP_LOGI + state 操作）
 * ============================================================ */
static void button_task_entry(void *arg)
{
    (void)arg;
    btn_evt_t evt;
    int64_t last_diag_ms = 0;

    ESP_LOGI(TAG, "Button task started");

    while (1) {
        /* 等待事件队列（阻塞模式）*/
        if (xQueueReceive(s_evt_queue, &evt, pdMS_TO_TICKS(5000)) == pdTRUE) {
            switch (evt.type) {
            case BTN_EVT_PRESSED:
                ESP_LOGI(TAG, "[BTN_DBG] GPIO%d PRESSED (queue recv)", evt.gpio);
                publish_event(EVENT_BUTTON_PRESSED, evt.gpio);
                push_event(BUTTON_EVENT_PRESS);
                break;

            case BTN_EVT_RELEASED:
                ESP_LOGI(TAG, "[BTN_DBG] GPIO%d RELEASED (queue recv)", evt.gpio);
                publish_event(EVENT_BUTTON_RELEASED, evt.gpio);
                push_event(BUTTON_EVENT_RELEASE);
                break;

            case BTN_EVT_CLICKED:
                ESP_LOGI(TAG, "[BTN_DBG] GPIO%d CLICKED (queue recv)", evt.gpio);
                publish_event(EVENT_BUTTON_CLICKED, evt.gpio);
                push_event(BUTTON_EVENT_PRESS);  /* CLICK = 内部事件 */
                break;

            case BTN_EVT_DOUBLE_CLICKED:
                ESP_LOGI(TAG, "[BTN_DBG] GPIO%d DOUBLE_CLICKED (queue recv)", evt.gpio);
                publish_event(EVENT_BUTTON_DOUBLE_CLICKED, evt.gpio);
                push_event(BUTTON_EVENT_DOUBLE_CLICK);
                break;

            case BTN_EVT_LONG_PRESSED:
                ESP_LOGI(TAG, "[BTN_DBG] GPIO%d LONG_PRESSED (queue recv)", evt.gpio);
                publish_event(EVENT_BUTTON_LONG_PRESSED, evt.gpio);
                push_event(BUTTON_EVENT_LONG_PRESS);
                break;

            case BTN_EVT_HOLD:
                ESP_LOGI(TAG, "[BTN_DBG] GPIO%d HOLD (queue recv)", evt.gpio);
                publish_event(EVENT_BUTTON_HOLD, evt.gpio);
                break;

            default:
                break;
            }
        } else {
            /* 5秒超时：打印 GPIO 电平诊断（只在第一次或电平变化时打印） */
            static int last_diag_level = -1;
            int diag_level = gpio_get_level(s_buttons[0].gpio);
            if (diag_level != last_diag_level) {
                last_diag_level = diag_level;
                ESP_LOGI(TAG, "[BTN_DBG] GPIO%d idle-level=%d (no events in 5s)", s_buttons[0].gpio, diag_level);
            }
        }
    }
}

/* ============================================================
 * polling_callback - 轮询定时器回调（esp_timer task context）
 *
 * 规则：esp_timer callback 必须极轻量！
 * 本函数只做：
 *   1. GPIO 读取
 *   2. 状态机更新
 *   3. 事件发送到 button_task 队列
 * 禁止：ESP_LOG、publish_event、任何业务逻辑
 * ============================================================ */
static void polling_callback(void *arg)
{
    (void)arg;
    int64_t now_ms = esp_timer_get_time() / 1000;

    for (int i = 0; i < s_button_count; i++) {
        button_info_t *btn = &s_buttons[i];
        int level = gpio_get_level(btn->gpio);

        switch (btn->state) {
        case BTN_STATE_IDLE:
            if (level == 0) {  /* 低电平按下 */
                btn->state           = BTN_STATE_DEBOUNCE;
                btn->press_start_ms   = now_ms;
                btn->last_long_ms     = 0;
            }
            break;

        case BTN_STATE_DEBOUNCE:
            if (level == 0 && (now_ms - btn->press_start_ms) >= DEBOUNCE_TIME_MS) {
                /* 消抖通过，按下生效 */
                btn->state            = BTN_STATE_PRESSED;
                btn->press_start_ms   = now_ms;

                /* 只发送到队列，不做任何业务处理 */
                send_evt_to_task(BTN_EVT_PRESSED, btn->gpio);

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
                    send_evt_to_task(BTN_EVT_DOUBLE_CLICKED, btn->gpio);
                    s_waiting_double = false;
                } else {
                    /* 300ms 后若无双击，发送 CLICK */
                    s_waiting_double = true;
                    s_last_click_ms = now_ms;
                }

                send_evt_to_task(BTN_EVT_RELEASED, btn->gpio);

            } else if (long_reached) {
                /* 到达长按阈值：LONG_PRESS + 进入 HOLD 状态 */
                btn->state       = BTN_STATE_LONG;
                btn->last_long_ms = now_ms;

                send_evt_to_task(BTN_EVT_LONG_PRESSED, btn->gpio);

                /* 立即发送第一个 HOLD */
                send_evt_to_task(BTN_EVT_HOLD, btn->gpio);
            }
            break;
        }

        case BTN_STATE_LONG: {
            bool released = (level == 1);

            if (released) {
                /* 释放 */
                btn->state = BTN_STATE_IDLE;
                send_evt_to_task(BTN_EVT_RELEASED, btn->gpio);

            } else {
                /* 持续按住：每 HOLD_INTERVAL_MS 发送一次 HOLD */
                if (now_ms - btn->last_long_ms >= HOLD_INTERVAL_MS) {
                    btn->last_long_ms = now_ms;
                    send_evt_to_task(BTN_EVT_HOLD, btn->gpio);
                }
            }
            break;
        }
        } /* switch */
    } /* for each button */

    /* 处理双击超时：300ms 内无第二次按下，发送 CLICK */
    if (s_waiting_double && (now_ms - s_last_click_ms) >= DOUBLE_CLICK_TIME_MS) {
        s_waiting_double = false;
        send_evt_to_task(BTN_EVT_CLICKED, s_buttons[0].gpio);  /* 单按键场景 */
    }
}

/*----------------------------
 * button_init
 *----------------------------*/
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

    /* 仅第一个按键初始化任务和定时器 */
    if (s_button_count == 1) {
        /* 1. 创建事件队列 */
        s_evt_queue = xQueueCreate(BTN_EVT_QUEUE_SIZE, sizeof(btn_evt_t));
        if (s_evt_queue == NULL) {
            ESP_LOGE(TAG, "Failed to create button event queue");
            return ESP_FAIL;
        }

        /* 2. 创建 button_task（栈 8192 bytes，支持 ESP_LOGI + state 操作）*/
        BaseType_t created = xTaskCreate(
            &button_task_entry,
            "button",
            8192,   /* 栈大小 */
            NULL,   /* 参数 */
            3,      /* 优先级 */
            &s_btn_task
        );
        if (created != pdTRUE) {
            ESP_LOGE(TAG, "Failed to create button task");
            vQueueDelete(s_evt_queue);
            s_evt_queue = NULL;
            return ESP_FAIL;
        }

        /* 3. 创建轮询定时器 */
        const esp_timer_create_args_t timer_args = {
            .callback      = &polling_callback,
            .arg           = NULL,
            .name          = "btn_poll",
            .dispatch_method = ESP_TIMER_TASK,
        };
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_polling_timer));
        ESP_ERROR_CHECK(esp_timer_start_periodic(s_polling_timer, 20 * 1000));  /* 20ms */
        ESP_LOGI(TAG, "Button polling timer started (20ms)");
    }

    ESP_LOGI(TAG, "Button GPIO%d registered (total: %d)", gpio_num, s_button_count);
    return ESP_OK;
}

/*----------------------------
 * button_get_event
 *----------------------------*/
button_event_t button_get_event(void)
{
    if (s_evt_tail == s_evt_head) {
        return (button_event_t)-1;
    }
    int next = (s_evt_tail + 1) % BTN_LEGACY_EVT_QUEUE_SIZE;
    button_event_t ev = s_legacy_evt_queue[s_evt_tail];
    s_evt_tail = next;
    return ev;
}

/*----------------------------
 * button_set_long_press_time
 *----------------------------*/
void button_set_long_press_time(uint32_t long_press_ms)
{
    s_long_press_ms = long_press_ms;
    /* 此函数在 app_main 初始化时调用，ESP_LOGI 无栈溢出风险 */
    ESP_LOGI(TAG, "Long press threshold: %lu ms", (unsigned long)long_press_ms);
}
