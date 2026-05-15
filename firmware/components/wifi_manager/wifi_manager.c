/**
 * @file wifi_manager.c
 * @brief WiFi 连接管理模块 - 源文件
 *
 * 功能：
 * - Station 模式连接
 * - 断线自动重连
 * - NVS 存储 SSID/密码
 */

#include <string.h>
#include <stdbool.h>
#include "wifi_manager.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "wifi_mgr";

// NVS 存储键名
#define NVS_NAMESPACE "wifi_cfg"
#define NVS_KEY_SSID   "ssid"
#define NVS_KEY_PASS   "password"

// 连接状态
static bool s_connected = false;
static bool s_reconnect_enabled = true;
static TaskHandle_t s_reconnect_task = NULL;

// WiFi 状态回调（最多 2 个）
#define WIFI_CB_MAX 2
static wifi_status_callback_t s_callbacks[WIFI_CB_MAX];
static void *s_callback_args[WIFI_CB_MAX];
static int s_callback_count = 0;

// WiFi 配置
static wifi_config_t s_wifi_config = {0};

/**
 * @brief WiFi 事件处理器
 */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WiFi STA started, connecting...");
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGW(TAG, "WiFi disconnected, reason: %d", event->reason);

        if (s_reconnect_enabled) {
            ESP_LOGI(TAG, "Scheduling reconnect in 1 second...");
            xTaskNotifyGive(s_reconnect_task);
        }
        /* 通知所有回调（WiFi 断开）*/
        for (int i = 0; i < s_callback_count; i++) {
            if (s_callbacks[i]) {
                s_callbacks[i](false, s_callback_args[i]);
            }
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_connected = true;
        /* 通知所有回调（WiFi 连接）*/
        for (int i = 0; i < s_callback_count; i++) {
            if (s_callbacks[i]) {
                s_callbacks[i](true, s_callback_args[i]);
            }
        }
    }
}

/**
 * @brief 自动重连任务
 */
static void reconnect_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Reconnect task started");

    while (s_reconnect_enabled) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        if (!s_connected && s_reconnect_enabled) {
            ESP_LOGI(TAG, "Attempting to reconnect...");
            esp_wifi_connect();
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGI(TAG, "Reconnect task exiting");
    s_reconnect_task = NULL;
    vTaskDelete(NULL);
}

/**
 * @brief 初始化 WiFi Manager
 */
esp_err_t wifi_manager_init(void)
{
    esp_err_t ret;

    // 初始化 NVS
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 初始化网络层
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    // WiFi 初始化配置
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // 注册事件处理器
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                &wifi_event_handler, NULL));

    // 设置为 Station 模式
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    // 创建自动重连任务
    xTaskCreatePinnedToCore(reconnect_task, "wifi_rec", 4096, NULL, 3, &s_reconnect_task, 0);

    ESP_LOGI(TAG, "WiFi Manager initialized");
    return ESP_OK;
}

/**
 * @brief 连接到指定 SSID/密码
 */
esp_err_t wifi_manager_connect(const char *ssid, const char *passwd)
{
    if (ssid == NULL || strlen(ssid) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    // 保存到 NVS
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret == ESP_OK) {
        nvs_set_str(nvs_handle, NVS_KEY_SSID, ssid);
        if (passwd != NULL && strlen(passwd) > 0) {
            nvs_set_str(nvs_handle, NVS_KEY_PASS, passwd);
        }
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
        ESP_LOGI(TAG, "WiFi credentials saved to NVS");
    }

    // 配置 WiFi
    memset(&s_wifi_config, 0, sizeof(wifi_config_t));
    strncpy((char *)s_wifi_config.sta.ssid, ssid, sizeof(s_wifi_config.sta.ssid) - 1);
    if (passwd != NULL) {
        strncpy((char *)s_wifi_config.sta.password, passwd, sizeof(s_wifi_config.sta.password) - 1);
    }
    s_wifi_config.sta.threshold.authmode = (passwd && strlen(passwd) > 0) ?
                                            WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &s_wifi_config));

    // 启动 WiFi
    return esp_wifi_start();
}

/**
 * @brief 从 NVS 恢复上次的连接
 */
esp_err_t wifi_manager_restore_connection(void)
{
    char ssid[32] = {0};
    char passwd[64] = {0};
    size_t ssid_len = sizeof(ssid);
    size_t passwd_len = sizeof(passwd);

    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS");
        return ret;
    }

    ret = nvs_get_str(nvs_handle, NVS_KEY_SSID, ssid, &ssid_len);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "No saved SSID found");
        nvs_close(nvs_handle);
        return ret;
    }

    nvs_get_str(nvs_handle, NVS_KEY_PASS, passwd, &passwd_len);
    nvs_close(nvs_handle);

    ESP_LOGI(TAG, "Restoring WiFi connection to: %s", ssid);
    return wifi_manager_connect(ssid, passwd);
}

/**
 * @brief 断开当前连接
 */
void wifi_manager_disconnect(void)
{
    s_reconnect_enabled = false;
    if (s_reconnect_task != NULL) {
        xTaskNotifyGive(s_reconnect_task);
    }
    esp_wifi_disconnect();
    ESP_LOGI(TAG, "WiFi disconnected");
}

/**
 * @brief 获取当前连接状态
 */
bool wifi_manager_is_connected(void)
{
    return s_connected;
}

/*======================================================================
 * WiFi 状态回调
 *======================================================================*/
esp_err_t wifi_manager_register_callback(wifi_status_callback_t callback, void *user_data)
{
    if (callback == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_callback_count >= WIFI_CB_MAX) {
        ESP_LOGE(TAG, "Too many WiFi callbacks (max=%d)", WIFI_CB_MAX);
        return ESP_ERR_NO_MEM;
    }
    s_callbacks[s_callback_count] = callback;
    s_callback_args[s_callback_count] = user_data;
    s_callback_count++;
    ESP_LOGI(TAG, "WiFi callback registered (total=%d)", s_callback_count);
    return ESP_OK;
}
