/**
 * @file system_monitor.c
 * @brief System Monitor - 运行时任务栈/堆监控
 *
 * 架构原则：
 * - esp_timer 回调（monitor_timer_callback）必须轻量，只发送信号
 * - vTaskList() / uxTaskGetSystemState() 只在专用 monitor_task 中调用
 * - Timer callback 栈大小受限（esp_timer task 栈 ~3.5KB），不安全
 *
 * Root cause note（维护者请阅读）：
 * event_bus_publish() 是同步调用，handler 在调用者的任务栈上执行。
 * button 的 polling_callback 运行在 esp_timer_task 中（默认 3.5KB 栈）。
 * 当 button 触发：state_set → EVENT_STATE_CHANGED → on_state_changed
 * → ui_refresh_by_state → led_set_pattern → rmt_tx_wait_all_done()
 * 时，调用链在 timer task 栈上展开，若栈太小则溢出。
 * system_monitor 每 10s 打印所有任务的栈水位线（watermark），帮助定位问题。
 *
 * Timer callback rules:
 * - 禁止在 esp_timer 回调中调用：vTaskList(), uxTaskGetSystemState(),
 *   vTaskGetCpuUsage(), heap_caps_print(), 或任何可能阻塞/大量栈的操作
 * - Timer callback 应该：设置标志、发送信号量/队列、更新时间戳
 * - 重操作必须在专用任务中进行
 */

#include "system_monitor.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "sys_mon";

/* 定时器句柄 */
static esp_timer_handle_t s_monitor_timer = NULL;

/* 二值信号量：timer callback 发送，monitor_task 接收 */
static SemaphoreHandle_t s_dump_sem = NULL;

/* 监控任务句柄（用于通知监控已停止） */
static TaskHandle_t s_monitor_task_handle = NULL;

/* 全局最小 heap */
static size_t s_min_heap = SIZE_MAX;

/* 初始化标记 */
static bool s_initialized = false;

/*————————————————————————————
 * dump_all_tasks - 在 monitor_task 栈上打印所有任务状态
 *
 * vTaskList() 栈消耗分析（运行在 monitor_task 4KB 栈，安全）：
 * - vTaskList 内部输出缓冲：~32 字节/任务 × 20 任务 ≈ 640 字节
 * - sprintf() 调用栈：~600 字节
 * - 总计：< 1.5KB → 完全适合 4KB 栈
 *
 * 注意：在 esp_timer task (~3.5KB 栈) 上运行不安全！
 *————————————————————————————*/
static void dump_all_tasks(void)
{
    static char vlist_buf[2048];

    vTaskList(vlist_buf);

    /* Heap */
    size_t free_heap = esp_get_free_heap_size();
    if (free_heap < s_min_heap) {
        s_min_heap = free_heap;
    }

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  System Task Status (via vTaskList)");
    ESP_LOGI(TAG, "  Heap: free=%lu B (%.1f KB), min_ever=%lu B (%.1f KB)",
             (unsigned long)free_heap,
             (double)free_heap / 1024.0,
             (unsigned long)s_min_heap,
             (double)s_min_heap / 1024.0);
    ESP_LOGI(TAG, "----------------------------------------");
    ESP_LOGI(TAG, "  %s", vlist_buf);
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  Stack 列 = remaining words (High Water Mark)");
}

/*————————————————————————————
 * dump_task_watermarks - 打印已知任务的水位线
 *————————————————————————————*/
static void dump_task_watermarks(void)
{
    static const char *known_tasks[] = {
        "sys_mon",          /* 7 chars  */
        "ui",               /* 2 chars  */
        "main",             /* 4 chars  */
        "esp_timer",        /* 9 chars  */
        "IDLE",             /* 4 chars  */
        "Tmr Svc",         /* 7 chars  */
        "wifi_rec",         /* 8 chars  */
        "ipc0",             /* 3 chars  */
        "ipc1",             /* 3 chars  */
        "recorder",         /* 8 chars  */
        "audio",            /* 5 chars  */
    };
    static const uint32_t known_stacks[] = {
        4096,      /* sys_mon: monitor_task 栈 */
        8192,      /* ui: UI_TASK_STACK_SIZE */
        5120,      /* main: ESP_MAIN_TASK_STACK_SIZE */
        3584,      /* esp_timer: ESP_TIMER_TASK_STACK_SIZE */
        1536,      /* IDLE: FREERTOS_IDLE_TASK_STACKSIZE */
        2048,      /* Tmr Svc: FREERTOS_TIMER_TASK_STACK_DEPTH */
        4096,      /* wifi_rec: wifi_manager 重连任务栈 */
        1024,      /* ipc0: CONFIG_ESP_IPC_TASK_STACK_SIZE */
        1024,      /* ipc1 */
        8192,      /* recorder: RECORDER_TASK_STACK */
        8192,      /* audio: AUDIO_TASK_STACK */
    };
    static const size_t n = sizeof(known_tasks) / sizeof(known_tasks[0]);

    ESP_LOGI(TAG, "----------------------------------------");
    ESP_LOGI(TAG, "  Known Task Watermarks:");
    for (size_t i = 0; i < n; i++) {
        /* 防御：任务名超长时跳过，不调用 xTaskGetHandle */
        if (strlen(known_tasks[i]) >= 16) {
            continue;
        }
        TaskHandle_t handle = xTaskGetHandle(known_tasks[i]);
        if (handle != NULL) {
            uint32_t watermark_words = uxTaskGetStackHighWaterMark(handle);
            uint32_t watermark_bytes = watermark_words * sizeof(StackType_t);
            uint32_t total_bytes    = known_stacks[i];
            uint32_t used_bytes     = (total_bytes > watermark_bytes)
                                      ? (total_bytes - watermark_bytes) : 0;
            float usage_pct = (float)used_bytes / (float)total_bytes * 100.0f;
            const char *warn = "";
            if (usage_pct > 85.0f) {
                warn = " ⚠️  OVERFLOW RISK";
            } else if (usage_pct > 70.0f) {
                warn = " ⚡  LOW STACK";
            }
            ESP_LOGI(TAG, "  [%-18s] total=%luKB hwmark=%luKB (%.0f%% used)%s",
                     known_tasks[i],
                     (unsigned long)total_bytes / 1024,
                     (unsigned long)watermark_bytes / 1024,
                     (double)usage_pct,
                     warn);
        }
    }
    ESP_LOGI(TAG, "----------------------------------------");
}

/*————————————————————————————
 * monitor_task - 专用监控任务
 *
 * 等待 s_dump_sem 信号，然后执行重量级诊断操作。
 * 栈大小 4KB，专门为 vTaskList() 分配（普通任务栈 ~1.5KB 不够）。
 *————————————————————————————*/
static void monitor_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "monitor_task started (stack=%u bytes)", (unsigned)(configMINIMAL_STACK_SIZE * sizeof(StackType_t)));

    while (1) {
        /* 等待 timer callback 发来的信号（永久阻塞，安全）*/
        if (xSemaphoreTake(s_dump_sem, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        /* 执行重量级诊断（运行在 monitor_task 4KB 栈，安全）*/
        dump_all_tasks();
        dump_task_watermarks();
    }
}

/*————————————————————————————
 * monitor_timer_callback - 轻量级 timer 回调
 *
 * 规则：timer callback 必须轻量！
 * 此函数只做一件事：发送信号量给 monitor_task。
 * 禁止在此调用：vTaskList(), heap_caps_print(), 任何 blocking 操作。
 *————————————————————————————*/
static void monitor_timer_callback(void *arg)
{
    (void)arg;
    /* 轻量：只发送信号，不做其他任何事 */
    BaseType_t higher_wake = pdFALSE;
    xSemaphoreGiveFromISR(s_dump_sem, &higher_wake);
    if (higher_wake == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

/*————————————————————————————
 * system_monitor_init
 * @param interval_ms 监控打印间隔（毫秒），0=仅手动触发
 *————————————————————————————*/
esp_err_t system_monitor_init(uint32_t interval_ms)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    s_min_heap = esp_get_free_heap_size();

    /* 创建二值信号量 */
    s_dump_sem = xSemaphoreCreateBinary();
    if (s_dump_sem == NULL) {
        ESP_LOGE(TAG, "Failed to create semaphore");
        return ESP_FAIL;
    }

    /* 创建 monitor_task（4KB 栈，专门用于 vTaskList） */
    BaseType_t created = xTaskCreatePinnedToCore(
        &monitor_task,
        "sys_mon",
        configMINIMAL_STACK_SIZE * 2,  /* 4KB 栈：足够 vTaskList() */
        NULL,
        1,      /* 低优先级，让出 CPU 给业务任务 */
        &s_monitor_task_handle,
        tskNO_AFFINITY  /* 可在任意核心运行 */
    );
    if (created != pdTRUE) {
        ESP_LOGE(TAG, "monitor_task create failed");
        vSemaphoreDelete(s_dump_sem);
        s_dump_sem = NULL;
        return ESP_FAIL;
    }

    /* 创建 esp_timer（周期性触发）*/
    const esp_timer_create_args_t args = {
        .callback        = &monitor_timer_callback,
        .arg             = NULL,
        .name            = "sys_mon_t",
        .dispatch_method = ESP_TIMER_TASK,  /* 回调在 esp_timer task 栈上 */
    };
    esp_err_t ret = esp_timer_create(&args, &s_monitor_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer_create failed: %s", esp_err_to_name(ret));
        vTaskDelete(s_monitor_task_handle);
        vSemaphoreDelete(s_dump_sem);
        s_dump_sem = NULL;
        return ret;
    }

    if (interval_ms > 0) {
        ret = esp_timer_start_periodic(s_monitor_timer, (uint64_t)interval_ms * 1000);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "esp_timer_start_periodic failed: %s", esp_err_to_name(ret));
            esp_timer_delete(s_monitor_timer);
            s_monitor_timer = NULL;
            return ret;
        }
        ESP_LOGI(TAG, "Monitor started (interval=%lu ms, timer->task signaling)", (unsigned long)interval_ms);
    } else {
        ESP_LOGI(TAG, "Monitor initialized (manual mode)");
    }

    s_initialized = true;

    /* 启动时手动触发一次打印（直接调用，不走 timer）*/
    ESP_LOGI(TAG, "--- Initial system snapshot ---");
    dump_all_tasks();
    dump_task_watermarks();

    return ESP_OK;
}

/*————————————————————————————
 * system_monitor_dump — 手动触发一次打印（直接调用，安全）
 *————————————————————————————*/
void system_monitor_dump(void)
{
    if (!s_initialized) {
        ESP_LOGW(TAG, "Not initialized, call system_monitor_init first");
        return;
    }
    dump_all_tasks();
    dump_task_watermarks();
}

/*————————————————————————————
 * system_monitor_get_min_heap
 *————————————————————————————*/
size_t system_monitor_get_min_heap(void)
{
    return (s_min_heap == SIZE_MAX) ? esp_get_free_heap_size() : s_min_heap;
}
