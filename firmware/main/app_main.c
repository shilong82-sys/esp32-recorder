/**
 * @file app_main.c
 * @brief  ESP32 AI Recorder - 主入口（简单版）
 *
 * 硬件未全部到货前的最小可用版本。
 * 功能：初始化各模块 → 按钮控制录音启停 → 循环打印状态。
 *
 * 按钮行为（简化）：
 *   短按 = 开始 / 停止录音
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "wifi_manager.h"
#include "recorder.h"
#include "storage.h"
#include "uploader.h"
#include "led.h"
#include "button.h"
#include "battery.h"

static const char *TAG = "app_main";

/* 录音配置（stub 阶段用默认值即可）*/
static recorder_config_t s_rec_cfg = {
    .i2s_port      = I2S_NUM_0,
    .sample_rate    = 16000,
    .bits_per_sample = 16,
    .channel_format  = 0,
    .file_prefix     = "REC"
};

/* 上传配置 —— 改成你的 Mac IP */
static uploader_config_t s_up_cfg = {
    .server_ip   = "192.168.31.185",   /* ← 改成 Mac 的实际 IP */
    .server_port = 8000,
    .upload_path = "/upload",
    .timeout_ms  = 30000,
};

/*——————————————————————————————
 * 按钮回调（只处理短按）
 *——————————————————————————————*/
static void button_cb(button_event_t event, void *arg)
{
    if (event != BUTTON_EVENT_PRESS) return;

    if (recorder_is_recording()) {
        ESP_LOGI(TAG, ">>> 停止录音");
        recorder_stop(NULL);
        led_set_state(LED_STATE_IDLE);
    } else {
        ESP_LOGI(TAG, ">>> 开始录音");
        recorder_start(NULL);
        led_set_state(LED_STATE_RECORDING);
    }
}

/*——————————————————————————————
 * app_main 入口
 *——————————————————————————————*/
void app_main(void)
{
    ESP_LOGI(TAG, "=== ESP32 AI Recorder v0.2 (simple) ===");
    esp_chip_info_t info;
    esp_chip_info(&info);
    ESP_LOGI(TAG, "Chip %d rev%d %d-core heap=%lu",
             info.model, info.revision, info.cores,
             (unsigned long)esp_get_free_heap_size());

    /* 1. 挂载 SD 卡 */
    if (storage_mount("/sdcard") != ESP_OK) {
        ESP_LOGW(TAG, "SD 卡挂载失败（忽略，继续初始化）");
    }

    /* 2. LED（GPIO2 是大部分 S3 开发板 onboard LED）*/
    led_init(GPIO_NUM_2);
    led_set_state(LED_STATE_IDLE);

    /* 3. 按键（GPIO0 通常是 BOOT 按钮）*/
    button_init(GPIO_NUM_0, button_cb, NULL);

    /* 4. 电池 */
    battery_init(NULL);

    /* 5. WiFi（不阻塞等待）*/
    wifi_manager_init();

    /* 6. 录音 & 上传模块 */
    recorder_init(&s_rec_cfg);
    uploader_init(&s_up_cfg);

    ESP_LOGI(TAG, "✅  初始化完成，等待按钮操作...");

    /* 主循环：每 2 秒打印一次状态 */
    while (1) {
        int bat = battery_get_percentage();
        ESP_LOGI(TAG, "heap=%lu | WiFi=%s | rec=%s | bat=%d%%",
                 (unsigned long)esp_get_free_heap_size(),
                 wifi_manager_is_connected() ? "Y" : "N",
                 recorder_is_recording() ? "Y" : "N",
                 bat > 0 ? bat : 0);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
