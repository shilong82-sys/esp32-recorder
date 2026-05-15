/**
 * @file app_main.c
 * @brief ESP32 AI Recorder - 固件主入口
 *
 * 架构说明：
 * - event_bus 最早初始化，作为所有模块通信中枢
 * - 硬件模块（led, button, storage, wifi, recorder, uploader, battery）初始化
 * - ui 组件订阅事件，驱动 LED pattern
 * - state 组件管理设备状态机，状态变化自动广播
 *
 * 业务模块不应直接调用 led_set_color()，而应：
 *   1. 订阅 EVENT_BUTTON_* 处理按键
 *   2. 调用 state_set() 改变设备状态
 *   3. ui 组件自动根据状态更新 LED
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"

/* 架构层 */
#include "event_bus.h"
#include "state.h"
#include "ui.h"
#include "system_monitor.h"

/* 硬件抽象层 */
#include "led.h"
#include "button.h"
#include "storage.h"
#include "wifi_manager.h"
#include "recorder.h"
#include "battery.h"
#include "uploader.h"
#include "logger.h"
#include "audio.h"
#include "esp_adc/adc_oneshot.h"

/* 测试硬件引脚定义 */
#ifndef GPIO_LED
#define GPIO_LED        GPIO_NUM_48   /* WS2812B 数据引脚 */
#endif
#ifndef GPIO_BUTTON
#define GPIO_BUTTON     GPIO_NUM_0    /* 用户按键 */
#endif

/*————————————————————————————
 * Audio 任务：唯一 I2S owner，永远运行
 *
 * 架构规则：
 * - audio_task 永远不 suspend/resume
 * - audio_task 永远拥有 I2S 硬件
 * - recording state 仅控制是否向 ringbuf 推送数据
 *
 * 行为：
 * - IDLE:      读取 I2S → 计算 RMS（DEBUG log）→ 不写 ringbuf
 * - RECORDING: 读取 I2S → 计算 RMS → 写入 ringbuf（由 recorder_task 消费）
 *————————————————————————————*/
#define AUDIO_TASK_PERIOD_MS  (100)      /* 打印间隔 */
#define AUDIO_TASK_SAMPLES    (1600)      /* 16000Hz × 100ms = 1600 samples */
#define AUDIO_TASK_STACK      (8192)

static const char *TAG = "app_main";    /* app_main 日志标签（必须在 audio_task 前） */

static void audio_task(void *arg)
{
    (void)arg;

    esp_err_t ret = audio_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Audio init failed, audio task exiting");
        vTaskDelete(NULL);
        return;
    }

    int16_t pcm_buf[AUDIO_TASK_SAMPLES];

    ESP_LOGI(TAG, "Audio task started (RMS period=%dms, samples=%d)",
             AUDIO_TASK_PERIOD_MS, AUDIO_TASK_SAMPLES);

    while (1) {
        int got = audio_read(pcm_buf, AUDIO_TASK_SAMPLES);
        if (got > 0) {
            float rms = audio_calculate_rms(pcm_buf, (size_t)got);
            /* DEBUG level only — don't spam logs during recording */
            ESP_LOGD(TAG, "Audio RMS: %.0f", (double)rms);
        }
        vTaskDelay(pdMS_TO_TICKS(AUDIO_TASK_PERIOD_MS));
    }
}

/*————————————————————————————
 * 示例：按键事件处理
 * 后续业务逻辑在此实现
 *————————————————————————————*/
static void on_button_event(event_type_t type, const void *data, size_t len, void *user_data)
{
    (void)user_data; (void)len;
    const event_button_data_t *ev = (const event_button_data_t *)data;

    switch (type) {
    case EVENT_BUTTON_PRESSED:
        ESP_LOGI(TAG, "[Button] GPIO%d pressed", ev->gpio_num);
        break;
    case EVENT_BUTTON_CLICKED:
        ESP_LOGI(TAG, "[Button] GPIO%d clicked", ev->gpio_num);
        /* TODO: 根据当前状态决定行为 */
        if (state_get() == DEVICE_STATE_IDLE) {
            ESP_LOGI(TAG, "-> Starting recording...");
            state_set(DEVICE_STATE_RECORDING);
        } else if (state_get() == DEVICE_STATE_RECORDING) {
            ESP_LOGI(TAG, "-> Stopping recording...");
            state_set(DEVICE_STATE_IDLE);
        }
        break;
    case EVENT_BUTTON_DOUBLE_CLICKED:
        ESP_LOGI(TAG, "[Button] GPIO%d double-clicked — printing storage layout", ev->gpio_num);
        storage_validate_layout();
        break;
    case EVENT_BUTTON_LONG_PRESSED:
        ESP_LOGI(TAG, "[Button] GPIO%d long pressed", ev->gpio_num);
        break;
    case EVENT_BUTTON_HOLD:
        ESP_LOGI(TAG, "[Button] GPIO%d hold", ev->gpio_num);
        break;
    case EVENT_BUTTON_RELEASED:
        ESP_LOGD(TAG, "[Button] GPIO%d released", ev->gpio_num);
        break;
    default:
        break;
    }
}

/*————————————————————————————
 * 状态变化处理：连接 state machine → recorder
 * RECORDING → recorder_start() + audio_enable_ringbuf(true)
 * IDLE     → recorder_stop()  + audio_enable_ringbuf(false)
 * audio_task 永远运行，永远拥有 I2S
 *————————————————————————————*/
static void on_state_changed(event_type_t type, const void *data, size_t len, void *user_data)
{
    (void)user_data; (void)len;
    const event_state_data_t *ev = (const event_state_data_t *)data;
    ESP_LOGI(TAG, "[State] %s -> %s",
             state_to_string(ev->prev_state),
             state_to_string(ev->curr_state));

    if (ev->curr_state == DEVICE_STATE_RECORDING && ev->prev_state != DEVICE_STATE_RECORDING) {
        /* 进入 RECORDING：启用 ringbuf 推送，启动 recorder
         * audio_task 永远运行，只改变是否向 ringbuf 写数据 */
        audio_enable_ringbuf(true);
        esp_err_t ret = recorder_start(NULL);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "recorder_start failed: %s", esp_err_to_name(ret));
            audio_enable_ringbuf(false);
            state_set(DEVICE_STATE_IDLE);
        }
    }
    else if (ev->prev_state == DEVICE_STATE_RECORDING && ev->curr_state != DEVICE_STATE_RECORDING) {
        /* 退出 RECORDING：停止 recorder，禁用 ringbuf 推送
         * audio_task 继续运行，不受影响 */
        uint32_t duration_ms = 0;
        recorder_stop(&duration_ms);
        audio_enable_ringbuf(false);
        ESP_LOGI(TAG, "Recording complete: %lu ms", (unsigned long)duration_ms);
    }
}

/*————————————————————————————
 * app_main — 固件初始化序列
 *————————————————————————————*/
void app_main(void)
{
    esp_err_t ret;

    /* 1. NVS 初始化 */
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* 2. 打印系统信息 */
    ESP_LOGI(TAG, "==========================================");
    ESP_LOGI(TAG, "  ESP32 AI Recorder — v0.2.1 Fix");
    ESP_LOGI(TAG, "  Phase 1: Recording Closed Loop [FIXED]");
    ESP_LOGI(TAG, "  Architecture: audio_task = sole I2S owner");
    ESP_LOGI(TAG, "==========================================");
    esp_chip_info_t info;
    esp_chip_info(&info);
    ESP_LOGI(TAG, "Chip: ESP32-S3 (rev %d), %d cores, WiFi%s%s",
             info.revision, info.cores,
             (info.features & CHIP_FEATURE_BT) ? "/BT" : "",
             (info.features & CHIP_FEATURE_BLE) ? "/BLE" : "");
    ESP_LOGI(TAG, "Free heap: %lu bytes",
             (unsigned long)esp_get_free_heap_size());

    /* 3. Event Bus（最先，所有模块依赖）*/
    ESP_LOGI(TAG, "[1/12] Event Bus ...");
    event_bus_init();

    /* 4. State 状态机 */
    ESP_LOGI(TAG, "[2/12] State ...");
    ret = state_init();
    ESP_ERROR_CHECK(ret);

    /* 5. LED 硬件初始化 */
    ESP_LOGI(TAG, "[3/12] LED (GPIO%d) ...", GPIO_LED);
    ret = led_init(GPIO_LED);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LED init failed (0x%x)", ret);
    }

    /* 6. Button 硬件初始化 */
    ESP_LOGI(TAG, "[4/12] Button (GPIO%d) ...", GPIO_BUTTON);
    button_set_long_press_time(1500);
    ret = button_init(GPIO_BUTTON);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Button init failed (0x%x)", ret);
    }

    /* 7. UI 组件（订阅事件，启动 UI task）*/
    ESP_LOGI(TAG, "[5/12] UI ...");
    ret = ui_init();
    ESP_ERROR_CHECK(ret);

    /* 6. System Monitor（栈/堆监控）*/
    ESP_LOGI(TAG, "[6/12] System Monitor ...");
    system_monitor_init(10000);  /* 每 10s 打印一次所有任务栈水位线 */

    /* 7. Audio（I2S 麦克风采集验证任务，永远运行，sole I2S owner）*/
    ESP_LOGI(TAG, "[7/12] Audio (INMP441 I2S Mic) ...");
    BaseType_t audio_task_created = xTaskCreatePinnedToCore(
        &audio_task,
        "audio",
        AUDIO_TASK_STACK,
        NULL,
        3,      /* priority */
        NULL,   /* 不需要保存 handle，禁止 suspend/resume */
        0       /* pin to core 0 */
    );
    if (audio_task_created != pdTRUE) {
        ESP_LOGE(TAG, "audio_task create failed");
    }

    /* 8. Storage TF 卡（使用默认挂载点 /sdcard） */
    ESP_LOGI(TAG, "[8/12] Storage (SPI mode) ...");
    ret = storage_mount(NULL);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Storage mount OK, testing read/write...");
        esp_err_t test_ret = storage_test_rw();
        if (test_ret == ESP_OK) {
            ESP_LOGI(TAG, "Storage ready (read/write test passed)");
            event_bus_publish(EVENT_STORAGE_READY, NULL, 0);
        } else {
            ESP_LOGE(TAG, "Storage read/write test FAILED!");
            event_bus_publish(EVENT_STORAGE_ERROR, NULL, 0);
        }
    } else {
        ESP_LOGW(TAG, "Storage mount failed (0x%x) - running without SD", ret);
        event_bus_publish(EVENT_STORAGE_ERROR, NULL, 0);
    }

    /* 9. WiFi */
    ESP_LOGI(TAG, "[9/12] WiFi ...");
    wifi_manager_init();
    ret = wifi_manager_restore_connection();
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "WiFi: connecting to saved network...");
    } else {
        ESP_LOGW(TAG, "WiFi: no saved network");
    }

    /* 10. Recorder */
    ESP_LOGI(TAG, "[10/12] Recorder ...");
    recorder_config_t rec_cfg = {
        .i2s_port        = I2S_NUM_0,
        .sample_rate     = 16000,
        .bits_per_sample = 16,
        .channel_format  = 1,
        .file_prefix     = "REC",
    };
    ret = recorder_init(&rec_cfg);
    ESP_ERROR_CHECK(ret);

    /* 11. Battery */
    ESP_LOGI(TAG, "[11/12] Battery ...");
    battery_config_t bat_cfg = {
        .adc_channel     = ADC_CHANNEL_0,
        .adc_atten       = ADC_ATTEN_DB_12,
        .voltage_divider = 2.0f,
        .full_voltage    = 4.2f,
        .empty_voltage   = 3.3f,
    };
    battery_init(&bat_cfg);
    int pct = battery_get_percentage();
    ESP_LOGI(TAG, "Battery: %d%%", pct);

    /* 12. Uploader (独立任务，队列模式) */
    ESP_LOGI(TAG, "[12/12] Uploader (queue mode) ...");
    uploader_config_t up_cfg = {
        .server_ip   = "192.168.31.185",
        .server_port = 8000,
        .upload_path = "/upload",
        .timeout_ms  = 30000,
    };
    uploader_init(&up_cfg);
    ret = uploader_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "uploader_start failed: %s", esp_err_to_name(ret));
    }

    /* 订阅事件（业务处理层）*/
    event_bus_subscribe(EVENT_BUTTON_PRESSED,       on_button_event, NULL);
    event_bus_subscribe(EVENT_BUTTON_CLICKED,       on_button_event, NULL);
    event_bus_subscribe(EVENT_BUTTON_DOUBLE_CLICKED, on_button_event, NULL);
    event_bus_subscribe(EVENT_BUTTON_LONG_PRESSED,  on_button_event, NULL);
    event_bus_subscribe(EVENT_BUTTON_HOLD,          on_button_event, NULL);
    event_bus_subscribe(EVENT_BUTTON_RELEASED,       on_button_event, NULL);
    event_bus_subscribe(EVENT_STATE_CHANGED,         on_state_changed, NULL);

    /* 进入初始状态 */
    state_set(DEVICE_STATE_IDLE);

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "==========================================");
    ESP_LOGI(TAG, "  Firmware initialized OK!");
    ESP_LOGI(TAG, "  State: %s", state_to_string(state_get()));
    ESP_LOGI(TAG, "  Press button to start recording");
    ESP_LOGI(TAG, "==========================================");
    ESP_LOGI(TAG, "");

    /* 主循环：空循环，事件驱动 */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGD(TAG, "[Idle] Heap: %lu bytes, Battery: %d%%",
                 (unsigned long)esp_get_free_heap_size(),
                 battery_get_percentage());
    }
}
