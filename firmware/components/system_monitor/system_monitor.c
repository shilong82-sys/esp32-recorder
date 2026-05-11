/**
 * @file system_monitor.c
 * @brief System Monitor - 运行时任务栈/堆监控
 *
 * 功能：
 * - 定期打印所有 FreeRTOS 任务的栈水位线（watermark）
 * - 打印 heap / min heap，用以检测内存泄漏趋势
 * - 帮助发现栈溢出和内存问题
 *
 * Root cause note（维护者请阅读）：
 * event_bus_publish() 是同步调用，handler 在调用者的任务栈上执行。
 * button 的 polling_callback 运行在 esp_timer_task 中（默认 3.5KB 栈）。
 * 当 button 触发：state_set → EVENT_STATE_CHANGED → on_state_changed
 * → ui_refresh_by_state → led_set_pattern → rmt_tx_wait_all_done()
 * 时，调用链在 timer task 栈上展开，若栈太小则溢出。
 * system_monitor 每 10s 打印所有任务的栈剩余空间，帮助定位问题。
 */

#include "system_monitor.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "sys_mon";

/* 定时器句柄 */
static esp_timer_handle_t s_monitor_timer = NULL;

/* 全局最小 heap */
static size_t s_min_heap = SIZE_MAX;

/*————————————————————————————
 * 内部：打印单个任务信息（简化版）
 *
 * vTaskList() 输出格式：
 * TaskName  State  Priority  Stack  Number
 *
 * - Stack 列 = usStackHighWaterMark（剩余最小空间，单位 words）
 * - 我们用 pxTaskGetStackStart() 估算栈总量
 *
 * 注意：vTaskList() 内部使用 sprintf()，会消耗一些栈空间，
 * 但对于列出 ~20 个任务来说，消耗 < 1KB（在系统 timer task 栈上）
 *————————————————————————————*/
static void dump_all_tasks(void)
{
    /* vTaskList 输出缓冲区。
     * 每行格式: "TaskName          S   3   456  1\n"
     * 按 TaskName(16) + State(1) + Priority(5) + Stack(6) + Number(3) + \n(1) = ~32 字节/任务
     * 20 任务 * 32 = 640 字节，加上格式化开销给 2048 字节足够。
     * 注意：vTaskList 内部会用 ~600 字节的栈来做 sprintf，
     * 所以这个函数总栈消耗 < 3KB，运行在 timer task 的 3.5KB 栈上安全。 */
    static char vlist_buf[2048];

    /* 调用 vTaskList（使用 timer task 栈）*/
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
    ESP_LOGI(TAG, "  ⚠️ 标记: HIGH STACK USAGE (栈使用率高)");
    ESP_LOGI(TAG, "========================================");
}

/*————————————————————————————
 * 内部：获取指定任务的栈 watermark（字节）
 * 通过 xTaskGetHandle 找到任务句柄，再查 watermark
 *————————————————————————————*/
static void dump_task_watermarks(void)
{
    /* 已知任务名称及其栈总量（字节）
     * FreeRTOS 任务名最长为 15 字符（含 \0 共 16），
     * xTaskGetHandle() 内会有 assert(strlen(name) < 16)。
     * 下面在调用前做 strlen 检查，防止触发 assert 崩溃。 */
    static const char *known_tasks[] = {
        "sys_mon",          /* 7 chars  */
        "ui",               /* 2 chars  */
        "main",             /* 4 chars  */
        "esp_timer",        /* 9 chars  */
        "IDLE",             /* 4 chars  */
        "Tmr Svc",         /* 7 chars  */
        "wifi_rec",         /* 8 chars  ← wifi_manager 重连任务 */
        "ipc0",             /* 3 chars  */
        "ipc1",             /* 3 chars  */
    };
    static const uint32_t known_stacks[] = {
        4096,      /* sys_mon: esp_timer 创建，栈大小 = ESP_TIMER_TASK_STACK_SIZE */
        8192,      /* ui: UI_TASK_STACK_SIZE (已增大至 8KB，消除 >70% 警告) */
        5120,      /* main: ESP_MAIN_TASK_STACK_SIZE (已增大至 5KB) */
        3584,      /* esp_timer: ESP_TIMER_TASK_STACK_SIZE */
        1536,      /* IDLE: FREERTOS_IDLE_TASK_STACKSIZE */
        2048,      /* Tmr Svc: FREERTOS_TIMER_TASK_STACK_DEPTH */
        4096,      /* wifi_rec: wifi_manager 重连任务栈 */
        1024,      /* ipc0: CONFIG_ESP_IPC_TASK_STACK_SIZE */
        1024,      /* ipc1 */
    };

    ESP_LOGI(TAG, "----------------------------------------");
    ESP_LOGI(TAG, "  Known Task Watermarks:");
    for (size_t i = 0; i < sizeof(known_tasks) / sizeof(known_tasks[0]); i++) {
        /* 防御：任务名超长时跳过，不调用 xTaskGetHandle，避免 assert 崩溃 */
        if (strlen(known_tasks[i]) >= 16) {
            ESP_LOGW(TAG, "  [%-18s] SKIP - name too long (%u chars, max 15)",
                     known_tasks[i], (unsigned)strlen(known_tasks[i]));
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
 * 定时器回调（运行在 ESP timer task 栈，
 * 不占用任何业务任务栈）
 *————————————————————————————*/
static void monitor_timer_callback(void *arg)
{
    (void)arg;
    dump_all_tasks();
    dump_task_watermarks();
}

/*————————————————————————————
 * system_monitor_init
 * @param interval_ms 监控打印间隔（毫秒），0=仅手动触发
 *————————————————————————————*/
esp_err_t system_monitor_init(uint32_t interval_ms)
{
    if (s_monitor_timer != NULL) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    s_min_heap = esp_get_free_heap_size();

    const esp_timer_create_args_t args = {
        .callback        = &monitor_timer_callback,
        .arg             = NULL,
        .name            = "sys_mon",
        .dispatch_method = ESP_TIMER_TASK,  /* 回调在系统 timer task 栈上执行 */
    };
    ESP_ERROR_CHECK(esp_timer_create(&args, &s_monitor_timer));

    if (interval_ms > 0) {
        ESP_ERROR_CHECK(esp_timer_start_periodic(s_monitor_timer,
                                                  (uint64_t)interval_ms * 1000));
        ESP_LOGI(TAG, "Monitor started (interval=%lu ms)", (unsigned long)interval_ms);
    } else {
        ESP_LOGI(TAG, "Monitor initialized (manual mode)");
    }

    /* 启动时打印一次初始快照 */
    dump_all_tasks();
    dump_task_watermarks();
    return ESP_OK;
}

/*————————————————————————————
 * system_monitor_dump — 手动触发一次打印
 *————————————————————————————*/
void system_monitor_dump(void)
{
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
