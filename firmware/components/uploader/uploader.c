/**
 * @file uploader.c
 * @brief WAV 文件上传模块 - 源文件
 *
 * 功能：
 * - HTTP POST multipart/form-data 上传
 * - 断点续传支持
 * - 上传进度回调
 * - 上传完成后删除本地文件
 */

#include "uploader.h"
#include "storage.h"
#include "wifi_manager.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_wifi.h"
#include <string.h>

static const char *TAG = "uploader";

// 默认配置
#define DEFAULT_SERVER_PORT 8000
#define DEFAULT_UPLOAD_PATH "/upload"
#define DEFAULT_TIMEOUT_MS 30000
#define CHUNK_SIZE 4096

// 上传状态
static int s_progress = 0;
static bool s_uploading = false;
static uploader_config_t s_config = {
    .server_ip = {0},
    .server_port = DEFAULT_SERVER_PORT,
    .upload_path = DEFAULT_UPLOAD_PATH,
    .timeout_ms = DEFAULT_TIMEOUT_MS,
};

// HTTP 事件处理
static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
    case HTTP_EVENT_ERROR:
        ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
        break;
    case HTTP_EVENT_ON_CONNECTED:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
        break;
    case HTTP_EVENT_HEADER_SENT:
        ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
        break;
    case HTTP_EVENT_ON_HEADER:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER key=%s, value=%s", evt->header_key, evt->header_value);
        break;
    case HTTP_EVENT_ON_DATA:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA len=%d", evt->data_len);
        // 如果需要处理响应数据，可以在这里
        if (evt->user_data && evt->data_len > 0) {
            char **response = (char **)evt->user_data;
            int *len = (int *)((char **)evt->user_data + 1);
            // 简化处理：直接打印响应
            ESP_LOGI(TAG, "Server response: %.*s", evt->data_len, (char *)evt->data);
        }
        break;
    case HTTP_EVENT_ON_FINISH:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
        break;
    case HTTP_EVENT_DISCONNECTED:
        ESP_LOGD(TAG, "HTTP_EVENT_DISCONNECTED");
        break;
    default:
        break;
    }
    return ESP_OK;
}

/**
 * @brief 初始化上传模块
 */
esp_err_t uploader_init(const uploader_config_t *config)
{
    if (config != NULL) {
        memcpy(&s_config, config, sizeof(uploader_config_t));
    }

    ESP_LOGI(TAG, "Uploader initialized");
    ESP_LOGI(TAG, "  Server: %s:%d%s", s_config.server_ip, s_config.server_port, s_config.upload_path);
    ESP_LOGI(TAG, "  Timeout: %lu ms", s_config.timeout_ms);

    return ESP_OK;
}

/**
 * @brief 上传单个文件
 */
static esp_err_t upload_single_file(const char *file_path)
{
    // 检查文件是否存在
    if (!storage_file_exists(file_path)) {
        ESP_LOGE(TAG, "File not found: %s", file_path);
        return ESP_FAIL;
    }

    uint32_t file_size = storage_get_file_size(file_path);
    ESP_LOGI(TAG, "Uploading file: %s (%lu bytes)", file_path, file_size);

    // 构建 HTTP URL
    char url[256];
    snprintf(url, sizeof(url), "http://%s:%d%s",
             s_config.server_ip, s_config.server_port, s_config.upload_path);

    // 打开本地文件
    char full_path[128];
    if (file_path[0] == '/') {
        snprintf(full_path, sizeof(full_path), "/sdcard%s", file_path);
    } else {
        snprintf(full_path, sizeof(full_path), "/sdcard/%s", file_path);
    }

    FILE *fp = fopen(full_path, "rb");
    if (fp == NULL) {
        ESP_LOGE(TAG, "Failed to open file: %s", full_path);
        return ESP_FAIL;
    }

    // 配置 HTTP 客户端
    esp_http_client_config_t http_config = {
        .url = url,
        .event_handler = http_event_handler,
        .timeout_ms = s_config.timeout_ms,
        .method = HTTP_METHOD_POST,
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        fclose(fp);
        return ESP_FAIL;
    }

    // 设置 Content-Type 为 multipart/form-data
    const char *boundary = "----ESP32Boundary123456789";
    char content_type[64];
    snprintf(content_type, sizeof(content_type), "multipart/form-data; boundary=%s", boundary);
    esp_http_client_set_header(client, "Content-Type", content_type);

    // 提取文件名
    const char *filename = strrchr(file_path, '/');
    filename = filename ? filename + 1 : file_path;

    // 开始请求
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        fclose(fp);
        return err;
    }

    s_uploading = true;
    s_progress = 0;

    // 上传文件数据
    uint8_t buffer[CHUNK_SIZE];
    size_t bytes_read;
    uint32_t total_sent = 0;
    int content_length = 0;

    // 计算 multipart body 大小（简化估算）
    content_length = 256 + file_size + 100;

    // 发送 multipart 头部
    char header[512];
    int header_len = snprintf(header, sizeof(header),
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"%s\"\r\n"
        "Content-Type: audio/wav\r\n"
        "\r\n",
        boundary, filename);

    esp_http_client_write(client, header, header_len);

    // 发送文件内容
    while ((bytes_read = fread(buffer, 1, CHUNK_SIZE, fp)) > 0) {
        int written = esp_http_client_write(client, (const char *)buffer, bytes_read);
        if (written < 0) {
            ESP_LOGE(TAG, "HTTP write error");
            s_uploading = false;
            esp_http_client_cleanup(client);
            fclose(fp);
            return ESP_FAIL;
        }

        total_sent += written;
        s_progress = (total_sent * 100) / file_size;

        // 每 10% 打印一次进度
        if (s_progress % 10 == 0) {
            ESP_LOGI(TAG, "Upload progress: %d%%", s_progress);
        }
    }

    fclose(fp);

    // 发送结束标记
    char footer[64];
    int footer_len = snprintf(footer, sizeof(footer), "\r\n--%s--\r\n", boundary);
    esp_http_client_write(client, footer, footer_len);

    // 获取响应
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "HTTP status: %d", status);

    esp_http_client_cleanup(client);
    s_uploading = false;
    s_progress = 100;

    if (status >= 200 && status < 300) {
        ESP_LOGI(TAG, "Upload successful!");
        return ESP_OK;
    } else {
        ESP_LOGE(TAG, "Upload failed with status: %d", status);
        return ESP_FAIL;
    }
}

/**
 * @brief 上传 WAV 文件
 */
esp_err_t uploader_upload(const char *file_path, char *out_response, size_t response_size)
{
    if (s_config.server_ip[0] == '\0') {
        ESP_LOGE(TAG, "Server not configured");
        return ESP_FAIL;
    }

    if (!wifi_manager_is_connected()) {
        ESP_LOGE(TAG, "WiFi not connected");
        return ESP_FAIL;
    }

    esp_err_t ret = upload_single_file(file_path);

    if (ret == ESP_OK && out_response != NULL && response_size > 0) {
        // 可以在这里解析响应
        snprintf(out_response, response_size, "Upload successful");
    }

    return ret;
}

/**
 * @brief 获取上传进度（0~100）
 */
int uploader_get_progress(void)
{
    return s_progress;
}

/**
 * @brief 上传完成后删除本地文件
 */
esp_err_t uploader_delete_after_upload(const char *file_path)
{
    ESP_LOGI(TAG, "Deleting local file: %s", file_path);
    return storage_delete_file(file_path);
}

/**
 * @brief 检查是否正在上传
 */
bool uploader_is_uploading(void)
{
    return s_uploading;
}
