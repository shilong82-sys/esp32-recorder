/**
 * @file uploader.c
 * @brief Stable-State Upload Queue — Producer-Consumer Architecture
 *
 * 目录职责：
 * - recordings/   : recorder 唯一写入，WAV finalize 后 rename 到 upload_queue/
 * - upload_queue/ : uploader 唯一输入，只包含 finalized WAV 文件
 * - uploaded/     : 上传成功归档（rename，不是 delete）
 *
 * 设计原则：
 * - uploader_task 独立运行，不在 recorder task 内
 * - 不扫描 recordings/（可能有未完成文件）
 * - 启动时仅扫描 upload_queue/ 恢复
 * - 重试策略：3s → 10s → 30s，之后保留文件等待下一轮
 * - WiFi 断开暂停上传，不影响录音
 */

#include "uploader.h"
#include "storage.h"
#include "wifi_manager.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include <string.h>
#include <sys/unistd.h>
#include <dirent.h>

static const char *TAG = "uploader";

/*======================================================================
 * Log Tags (per spec: [UPLOAD][QUEUE][ARCHIVE][RETRY][WIFI_WAIT])
 *======================================================================*/
#define LOG_QUEUE(...)  ESP_LOGI(TAG, "[QUEUE] " __VA_ARGS__)
#define LOG_ARCHIVE(...) ESP_LOGI(TAG, "[ARCHIVE] " __VA_ARGS__)
#define LOG_RETRY(...)  ESP_LOGW(TAG, "[RETRY] " __VA_ARGS__)
#define LOG_WIFI(...)   ESP_LOGI(TAG, "[WIFI_WAIT] " __VA_ARGS__)

/*======================================================================
 * Constants
 *======================================================================*/
#define UPLOADER_TASK_STACK   (8192)
#define UPLOADER_TASK_PRIORITY (2)       /* 低于 audio_task(3) 和 recorder_task(3) */
#define UPLOADER_TASK_CORE     (0)

#define POLL_INTERVAL_MS        (2000)    /* 每 2s 扫描一次队列 */
#define CHUNK_SIZE              (4096)

/* 重试间隔：3s → 10s → 30s */
static const uint32_t RETRY_DELAYS_MS[] = { 3000, 10000, 30000 };
#define RETRY_MAX              (sizeof(RETRY_DELAYS_MS) / sizeof(RETRY_DELAYS_MS[0]))

/*======================================================================
 * State
 *======================================================================*/
static TaskHandle_t s_task_handle = NULL;
static bool s_wifi_ok = false;           /* WiFi 是否可用（由 wifi_manager 回调设置）*/
static bool s_paused = false;           /* 暂停标志（WiFi 断开时置 true）*/
static uploader_config_t s_config = {
    .server_ip   = {0},
    .server_port = 8000,
    .upload_path = "/upload",
    .timeout_ms  = 30000,
};

/*======================================================================
 * Internal Helpers
 *======================================================================*/

/**
 * 获取 upload_queue/ 中第一个 WAV 文件（按字母序 = FIFO）
 * @param[out] buf 文件名缓冲区（必须 >= 64 字节）
 * @param buf_size 缓冲区大小
 * @return true=找到，false=队列为空
 */
static bool get_next_file(char *buf, size_t buf_size)
{
    char queue_dir[64];
    storage_build_vfs_path(queue_dir, sizeof(queue_dir), STORAGE_PATH_UPLOAD_QUEUE, NULL);

    DIR *dir = opendir(queue_dir);
    if (dir == NULL) {
        LOG_QUEUE("opendir failed: errno=%d", errno);
        return false;
    }

    char found[64] = {0};
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_DIR) continue;
        const char *ext = strrchr(entry->d_name, '.');
        if (ext == NULL || strcasecmp(ext, ".wav") != 0) continue;
        /* FIFO: 选字母序最小（最早）的文件 */
        if (found[0] == '\0' || strcmp(entry->d_name, found) < 0) {
            strncpy(found, entry->d_name, sizeof(found) - 1);
            found[sizeof(found) - 1] = '\0';
        }
    }
    closedir(dir);

    if (found[0] != '\0') {
        strncpy(buf, found, buf_size - 1);
        buf[buf_size - 1] = '\0';
        return true;
    }
    return false;
}

/**
 * HTTP 事件处理器（debug 级日志，不打印正常流程）
 */
static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
    case HTTP_EVENT_ERROR:
        ESP_LOGD(TAG, "[UPLOAD] HTTP_ERROR");
        break;
    case HTTP_EVENT_ON_CONNECTED:
        ESP_LOGD(TAG, "[UPLOAD] CONNECTED");
        break;
    case HTTP_EVENT_ON_DATA:
        ESP_LOGD(TAG, "[UPLOAD] ON_DATA len=%d", evt->data_len);
        break;
    case HTTP_EVENT_DISCONNECTED:
        ESP_LOGD(TAG, "[UPLOAD] DISCONNECTED");
        break;
    default:
        break;
    }
    return ESP_OK;
}

/**
 * 上传单个文件
 * @param filename upload_queue/ 中的文件名（不含路径）
 * @return ESP_OK=成功，ESP_FAIL=失败
 */
static esp_err_t do_upload(const char *filename)
{
    char full_path[128];
    storage_build_vfs_path(full_path, sizeof(full_path), STORAGE_PATH_UPLOAD_QUEUE, filename);

    struct stat st;
    if (stat(full_path, &st) != 0) {
        ESP_LOGE(TAG, "[UPLOAD] file not found: %s", full_path);
        return ESP_FAIL;
    }
    uint32_t file_size = (uint32_t)st.st_size;

    ESP_LOGI(TAG, "[UPLOAD] -> %s (%lu bytes)", filename, (unsigned long)file_size);

    /* 构建 URL */
    char url[256];
    snprintf(url, sizeof(url), "http://%s:%d%s",
             s_config.server_ip, s_config.server_port, s_config.upload_path);

    /* 打开文件 */
    FILE *fp = fopen(full_path, "rb");
    if (fp == NULL) {
        ESP_LOGE(TAG, "[UPLOAD] fopen failed: %s", full_path);
        return ESP_FAIL;
    }

    /* HTTP 客户端配置 */
    esp_http_client_config_t http_config = {
        .url          = url,
        .event_handler = http_event_handler,
        .timeout_ms    = s_config.timeout_ms,
        .method        = HTTP_METHOD_POST,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    if (client == NULL) {
        ESP_LOGE(TAG, "[UPLOAD] esp_http_client_init failed");
        fclose(fp);
        return ESP_FAIL;
    }

    /* multipart/form-data */
    const char *boundary = "----ESP32Boundary123456789";
    char content_type[64];
    snprintf(content_type, sizeof(content_type), "multipart/form-data; boundary=%s", boundary);
    esp_http_client_set_header(client, "Content-Type", content_type);

    /* 开始请求 */
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[UPLOAD] open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        fclose(fp);
        return ESP_FAIL;
    }

    /* 发送 multipart 头 */
    char header[512];
    int header_len = snprintf(header, sizeof(header),
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"%s\"\r\n"
        "Content-Type: audio/wav\r\n"
        "\r\n",
        boundary, filename);
    esp_http_client_write(client, header, header_len);

    /* 发送文件内容 */
    uint8_t buf[CHUNK_SIZE];
    size_t bytes_read;
    uint32_t total_sent = 0;
    while ((bytes_read = fread(buf, 1, CHUNK_SIZE, fp)) > 0) {
        int written = esp_http_client_write(client, (const char *)buf, bytes_read);
        if (written < 0) {
            ESP_LOGE(TAG, "[UPLOAD] write error");
            esp_http_client_cleanup(client);
            fclose(fp);
            return ESP_FAIL;
        }
        total_sent += written;
    }
    fclose(fp);

    /* 发送结束标记 */
    char footer[64];
    int footer_len = snprintf(footer, sizeof(footer), "\r\n--%s--\r\n", boundary);
    esp_http_client_write(client, footer, footer_len);

    /* 获取状态码 */
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    ESP_LOGI(TAG, "[UPLOAD] HTTP %d", status);

    if (status >= 200 && status < 300) {
        ESP_LOGI(TAG, "[UPLOAD] SUCCESS: %s", filename);
        return ESP_OK;
    } else {
        ESP_LOGE(TAG, "[UPLOAD] FAIL HTTP %d: %s", status, filename);
        return ESP_FAIL;
    }
}

/*======================================================================
 * WiFi 状态回调（由 wifi_manager 调用）
 *======================================================================*/
static void on_wifi_status(bool connected, void *user_data)
{
    (void)user_data;
    if (connected) {
        if (!s_wifi_ok) {
            s_wifi_ok = true;
            s_paused = false;
            LOG_WIFI("connected — uploader resumed");
        }
    } else {
        if (s_wifi_ok) {
            s_wifi_ok = false;
            s_paused = true;
            LOG_WIFI("disconnected — uploader paused");
        }
    }
}

/*======================================================================
 * Upload Queue Task
 *======================================================================*/
static void uploader_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "uploader_task started (poll=%dms)", POLL_INTERVAL_MS);

    /* 初始状态检查 */
    s_wifi_ok = wifi_manager_is_connected();
    s_paused = !s_wifi_ok;
    if (s_paused) {
        LOG_WIFI("WiFi not connected on startup — waiting");
    }

    while (1) {
        /* 1. 等待一轮扫描间隔 */
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));

        /* 2. WiFi 不可用：跳过本轮 */
        if (!s_wifi_ok || s_paused) {
            if (!s_wifi_ok && s_paused == false) {
                /* 状态不一致（WiFi 断开但没收到回调），主动重查 */
                s_paused = true;
            }
            continue;
        }

        /* 3. 检查是否有文件 */
        char filename[64];
        if (!get_next_file(filename, sizeof(filename))) {
            /* 队列为空 */
            ESP_LOGD(TAG, "[QUEUE] empty — nothing to upload");
            continue;
        }

        LOG_QUEUE("found: %s — starting upload", filename);

        /* 4. 上传 + 重试 */
        esp_err_t upload_err = ESP_FAIL;
        uint32_t retry_count = 0;
        uint32_t delay_ms = 0;

        while (retry_count < RETRY_MAX) {
            /* 首次尝试不等待，后续等待 interval */
            if (retry_count > 0) {
                LOG_RETRY("attempt %u for %s — wait %lums",
                          (unsigned)retry_count, filename, (unsigned long)delay_ms);
                vTaskDelay(pdMS_TO_TICKS(delay_ms));

                /* 等待期间检查 WiFi */
                if (!s_wifi_ok) {
                    LOG_WIFI("WiFi lost during retry wait — abort retry");
                    s_paused = true;
                    break;
                }
            }

            upload_err = do_upload(filename);
            if (upload_err == ESP_OK) {
                break;  /* 上传成功 */
            }

            delay_ms = RETRY_DELAYS_MS[retry_count];
            retry_count++;
        }

        /* 5. 处理结果 */
        if (upload_err == ESP_OK) {
            /* 归档：rename upload_queue/ -> uploaded/ */
            esp_err_t arch_err = storage_rename_file(
                STORAGE_PATH_UPLOAD_QUEUE, filename,
                STORAGE_PATH_UPLOADED, filename);

            if (arch_err != ESP_OK) {
                /* 归档失败不应该删除，保留原文件 */
                ESP_LOGE(TAG, "[ARCHIVE] rename failed: %s — file kept in upload_queue/",
                         filename);
            } else {
                LOG_ARCHIVE("uploaded/%s", filename);
            }
        } else {
            /* 全部重试失败：保留文件，等待下一轮扫描 */
            ESP_LOGW(TAG, "[QUEUE] %s failed all %u retries — will retry next scan cycle",
                     filename, (unsigned)RETRY_MAX);
        }

        /* 循环会自动继续扫描下一个文件 */
    }
}

/*======================================================================
 * Public API
 *======================================================================*/

esp_err_t uploader_init(const uploader_config_t *config)
{
    if (config != NULL) {
        memcpy(&s_config, config, sizeof(uploader_config_t));
    }

    ESP_LOGI(TAG, "Uploader initialized");
    ESP_LOGI(TAG, "  Server: %s:%d%s", s_config.server_ip, s_config.server_port, s_config.upload_path);
    ESP_LOGI(TAG, "  Retry delays: %lu/%lu/%lu ms",
             (unsigned long)RETRY_DELAYS_MS[0],
             (unsigned long)RETRY_DELAYS_MS[1],
             (unsigned long)RETRY_DELAYS_MS[2]);

    /* 注册 WiFi 状态回调 */
    esp_err_t ret = wifi_manager_register_callback(on_wifi_status, NULL);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "wifi_manager_register_callback failed: %s", esp_err_to_name(ret));
    }

    return ESP_OK;
}

esp_err_t uploader_start(void)
{
    if (s_task_handle != NULL) {
        ESP_LOGW(TAG, "uploader already started");
        return ESP_OK;
    }

    BaseType_t created = xTaskCreatePinnedToCore(
        &uploader_task,
        "uploader",
        UPLOADER_TASK_STACK,
        NULL,
        UPLOADER_TASK_PRIORITY,
        &s_task_handle,
        UPLOADER_TASK_CORE
    );

    if (created != pdTRUE) {
        ESP_LOGE(TAG, "uploader_task create failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "uploader task started");
    return ESP_OK;
}

esp_err_t uploader_upload(const char *file_path, char *out_response, size_t response_size)
{
    /* 单次上传接口已废弃（队列模式），保留仅用于兼容 */
    (void)file_path;
    (void)out_response;
    (void)response_size;
    ESP_LOGW(TAG, "uploader_upload() is deprecated — use queue mode");
    return ESP_ERR_NOT_SUPPORTED;
}

int uploader_get_progress(void)
{
    return 0;  /* 队列模式无全局进度 */
}

esp_err_t uploader_delete_after_upload(const char *file_path)
{
    /* 归档改为 rename，不删除。保留此函数仅作兼容。 */
    (void)file_path;
    return ESP_OK;
}

bool uploader_is_uploading(void)
{
    return (s_task_handle != NULL) && !s_paused && s_wifi_ok;
}
