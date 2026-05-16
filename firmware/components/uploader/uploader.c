/**
 * @file uploader.c
 * @brief Stable-State Upload Queue — Producer-Consumer Architecture
 *
 * 目录职责：
 * - recordings/   : recorder 唯一写入，WAV finalize 后 rename 到 upload_queue/
 * - upload_queue/ : uploader 唯一输入，只包含 finalized WAV 文件
 * - 上传成功后：删除 upload_queue/ 中的文件（不保留副本）
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
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <errno.h>
#include <string.h>
#include <time.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include <dirent.h>

static const char *TAG = "uploader";

/*======================================================================
 * Log Tags
 *======================================================================*/
#define LOG_QUEUE(...)  ESP_LOGI(TAG, "[QUEUE] " __VA_ARGS__)
#define LOG_ARCHIVE(...) ESP_LOGI(TAG, "[ARCHIVE] " __VA_ARGS__)
#define LOG_RETRY(...)  ESP_LOGW(TAG, "[UPLOAD_RETRY] " __VA_ARGS__)
#define LOG_WIFI(...)   ESP_LOGI(TAG, "[WIFI_WAIT] " __VA_ARGS__)

/*======================================================================
 * Constants
 *======================================================================*/
#define UPLOADER_TASK_STACK   (8192)
#define UPLOADER_TASK_PRIORITY (2)
#define UPLOADER_TASK_CORE     (0)

#define POLL_INTERVAL_MS        (2000)

/* 重试间隔：3s → 10s → 30s */
static const uint32_t RETRY_DELAYS_MS[] = { 3000, 10000, 30000 };
#define RETRY_MAX              (sizeof(RETRY_DELAYS_MS) / sizeof(RETRY_DELAYS_MS[0]))

/*======================================================================
 * State
 *======================================================================*/
static TaskHandle_t s_task_handle = NULL;
static bool s_wifi_ok = false;
static bool s_paused = false;

/*======================================================================
 * HTTP Event State
 *======================================================================*/
static char s_last_err_detail[512] = {0};
static char s_response_body[256]   = {0};
static int  s_response_body_len     = 0;

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
        LOG_QUEUE("opendir failed: errno=%d (%s)", errno, strerror(errno));
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

/*======================================================================
 * HTTP Event Handler — 收集详细错误和响应体
 *======================================================================*/
static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
    case HTTP_EVENT_ERROR:
        ESP_LOGD(TAG, "[HTTP] ERROR");
        if (evt->data && evt->data_len > 0) {
            size_t len = strlen(s_last_err_detail);
            int cap = sizeof(s_last_err_detail) - (int)len - 1;
            if (cap > 0) {
                snprintf(s_last_err_detail + len, (size_t)cap,
                         "%.*s", (int)evt->data_len, (char *)evt->data);
            }
        }
        break;

    case HTTP_EVENT_ON_CONNECTED:
        ESP_LOGI(TAG, "[HTTP] TCP connected");
        break;

    case HTTP_EVENT_HEADERS_SENT:
        ESP_LOGD(TAG, "[HTTP] HEADERS SENT");
        break;

    case HTTP_EVENT_ON_HEADER:
        ESP_LOGD(TAG, "[HTTP] HEADER: %s: %s",
                 evt->header_key ? evt->header_key : "(null)",
                 evt->header_value ? evt->header_value : "(null)");
        break;

    case HTTP_EVENT_ON_DATA: {
        if (evt->data && evt->data_len > 0 &&
            s_response_body_len < (int)(sizeof(s_response_body) - 1)) {
            int cap = (int)(sizeof(s_response_body) - (size_t)s_response_body_len - 1);
            int copy = (evt->data_len < (size_t)cap) ? (int)evt->data_len : cap;
            memcpy(s_response_body + s_response_body_len, evt->data, (size_t)copy);
            s_response_body_len += copy;
            s_response_body[s_response_body_len] = '\0';
        }
        ESP_LOGD(TAG, "[HTTP] ON_DATA len=%d  resp_body_len=%d",
                 evt->data_len, s_response_body_len);
        break;
    }

    case HTTP_EVENT_ON_FINISH:
        ESP_LOGI(TAG, "[HTTP] RESPONSE FINISH");
        break;

    case HTTP_EVENT_DISCONNECTED:
        ESP_LOGD(TAG, "[HTTP] DISCONNECTED");
        break;

    default:
        break;
    }
    return ESP_OK;
}

/*======================================================================
 * Upload — HTTP (Cloudflare Tunnel) — RAW BODY (audio/wav)
 *======================================================================*/

#define UPLOAD_URL            "http://record.east-deep.com/upload"
#define UPLOAD_CHUNK_SIZE     (8 * 1024)   /* 8KB chunk size */

/**
 * 上传单个文件 (HTTP raw body — Content-Type: audio/wav)
 *
 * 实现方式：esp_http_client_open() + esp_http_client_write() 流式发送。
 * - 不 malloc 完整 body，RAM 占用 < 20KB
 * - 支持 100MB+ 录音文件
 * - 无 multipart，直接发送 wav binary
 *
 * @param filename upload_queue/ 中的文件名（不含路径）
 * @return ESP_OK=成功（HTTP 200），ESP_FAIL=失败
 */
static esp_err_t do_upload(const char *filename)
{
    /* ── 计时起点 ── */
    int64_t t_start_ms = (int64_t)xTaskGetTickCount() * portTICK_PERIOD_MS;

    /* ── 文件检查 ── */
    char full_path[128];
    storage_build_vfs_path(full_path, sizeof(full_path),
                             STORAGE_PATH_UPLOAD_QUEUE, filename);

    struct stat st;
    if (stat(full_path, &st) != 0) {
        ESP_LOGE(TAG, "[UPLOAD] file not found: %s errno=%d",
                 full_path, errno);
        return ESP_FAIL;
    }
    uint32_t file_size = (uint32_t)st.st_size;

    ESP_LOGI(TAG, "[UPLOAD] =======================");
    ESP_LOGI(TAG, "[UPLOAD] filename  : %s", filename);
    ESP_LOGI(TAG, "[UPLOAD] file_size : %lu bytes", (unsigned long)file_size);
    ESP_LOGI(TAG, "[UPLOAD] heap_free : %lu bytes",
             (unsigned long)esp_get_free_heap_size());

    /* ── 重置响应状态 ── */
    s_last_err_detail[0] = '\0';
    s_response_body[0]    = '\0';
    s_response_body_len    = 0;

    /* ── total_len = 文件大小（无 multipart 开销） ── */
    size_t total_len = (size_t)file_size;
    ESP_LOGI(TAG, "[HTTP] total_len  : %zu bytes (raw wav)", total_len);

    /* ── 分配 16KB 循环 buffer ── */
    uint8_t *buf = (uint8_t *)heap_caps_malloc(UPLOAD_CHUNK_SIZE, MALLOC_CAP_8BIT);
    if (buf == NULL) {
        ESP_LOGE(TAG, "[UPLOAD] cannot malloc %d bytes for chunk buffer",
                     UPLOAD_CHUNK_SIZE);
        ESP_LOGE(TAG, "[UPLOAD] heap_free=%lu  largest_block=%lu",
                 (unsigned long)esp_get_free_heap_size(),
                 (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        return ESP_FAIL;
    }

    /* ── HTTP 客户端配置 ── */
    esp_http_client_config_t http_config = {
        .url           = UPLOAD_URL,
        .event_handler = http_event_handler,
        .method        = HTTP_METHOD_POST,
        .timeout_ms    = 600000,  /* 10 分钟，支持慢速上传 */
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    if (client == NULL) {
        ESP_LOGE(TAG, "[UPLOAD] esp_http_client_init FAILED");
        free(buf);
        return ESP_FAIL;
    }

    /* ── 设置 Content-Type: audio/wav（无 multipart） ── */
    esp_http_client_set_header(client, "Content-Type", "audio/wav");

    /* ── 打开连接，Content-Length = file_size ── */
    esp_err_t open_err = esp_http_client_open(client, (int)total_len);
    if (open_err != ESP_OK) {
        ESP_LOGE(TAG, "[UPLOAD] open failed: %s",
                 esp_err_to_name(open_err));
        esp_http_client_cleanup(client);
        free(buf);
        return ESP_FAIL;
    }

    /* ── 流式写 wav 文件（无 header/footer） ── */
    FILE *fp = fopen(full_path, "rb");
    if (fp == NULL) {
        ESP_LOGE(TAG, "[UPLOAD] fopen failed: %s errno=%d",
                 full_path, errno);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        free(buf);
        return ESP_FAIL;
    }

    uint32_t total_sent   = 0;
    uint32_t last_log_bytes = 0;
    size_t   nread;

    while ((nread = fread(buf, 1, UPLOAD_CHUNK_SIZE, fp)) > 0) {
        int nw = esp_http_client_write(client, (char *)buf, (int)nread);
        if (nw != (int)nread) {
            ESP_LOGE(TAG, "[UPLOAD] write: expected %zu, got %d",
                     nread, nw);
            fclose(fp);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            free(buf);
            return ESP_FAIL;
        }
        total_sent += (uint32_t)nread;

        /* 每发送 512KB 打印一次进度 */
        if (total_sent - last_log_bytes >= 512 * 1024 ||
            total_sent == file_size) {
            int64_t now_ms     = (int64_t)xTaskGetTickCount() * portTICK_PERIOD_MS;
            int64_t elapsed_ms = now_ms - t_start_ms;
            float   speed_kbps = (elapsed_ms > 0)
                            ? ((float)total_sent / (float)elapsed_ms)
                            : 0.0f;
            ESP_LOGI(TAG, "[UPLOAD] progress : %lu / %lu bytes  (%.1f KB/s)",
                     (unsigned long)total_sent,
                     (unsigned long)file_size, speed_kbps);
            last_log_bytes = total_sent;
        }
    }
    fclose(fp);

    if (total_sent != file_size) {
        ESP_LOGE(TAG, "[UPLOAD] fread mismatch: sent %lu, expected %lu",
                 (unsigned long)total_sent, (unsigned long)file_size);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        free(buf);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "[HTTP] body sent  : %lu bytes",
             (unsigned long)total_sent);
    ESP_LOGI(TAG, "[HTTP] waiting for response...");

    /* ── 接收服务器响应 ── */
    esp_err_t fetch_err = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);

    /* ── 释放循环 buffer ── */
    free(buf);

    /* ── 关闭连接 ── */
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    /* ── 计时结束 ── */
    int64_t t_end_ms   = (int64_t)xTaskGetTickCount() * portTICK_PERIOD_MS;
    int64_t duration_ms = t_end_ms - t_start_ms;

    /* ── 日志报告 ── */
    float speed_kbps = (duration_ms > 0)
                    ? ((float)total_sent / (float)duration_ms)
                    : 0.0f;
    ESP_LOGI(TAG, "[HTTP] HTTP status : %d", status);
    if (s_response_body_len > 0) {
        ESP_LOGI(TAG, "[HTTP] resp_body   : %.*s",
                 s_response_body_len, s_response_body);
    } else {
        ESP_LOGI(TAG, "[HTTP] resp_body   : (empty)");
    }
    ESP_LOGI(TAG, "[UPLOAD] sent       : %lu bytes",
             (unsigned long)total_sent);
    ESP_LOGI(TAG, "[UPLOAD] elapsed    : %lld ms",
             (long long)duration_ms);
    ESP_LOGI(TAG, "[UPLOAD] speed      : %.1f KB/s", speed_kbps);
    ESP_LOGI(TAG, "[UPLOAD] heap_free  : %lu bytes",
             (unsigned long)esp_get_free_heap_size());

    /* ── 判定结果：HTTP 200 = 删除本地文件 ── */
    if (status == 200) {
        ESP_LOGI(TAG, "[UPLOAD_OK] %s HTTP 200 %lldms",
                 filename, (long long)duration_ms);
        if (unlink(full_path) == 0) {
            ESP_LOGI(TAG, "[UPLOAD] deleted: %s", full_path);
        } else {
            ESP_LOGW(TAG, "[UPLOAD] unlink failed: %s errno=%d (%s)",
                     full_path, errno, strerror(errno));
        }
        return ESP_OK;
    } else {
        if (fetch_err != ESP_OK) {
            ESP_LOGE(TAG, "[UPLOAD] fetch_headers: %s",
                     esp_err_to_name(fetch_err));
        }
        ESP_LOGE(TAG, "[UPLOAD_FAIL] %s HTTP %d", filename, status);
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
    ESP_LOGI(TAG, "  HTTP URL: " UPLOAD_URL);

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

        LOG_QUEUE("queued: %s", filename);

        /* 4. 上传 + 重试 */
        esp_err_t upload_err = ESP_FAIL;
        uint32_t retry_count = 0;
        uint32_t delay_ms = 0;

        while (retry_count < RETRY_MAX) {
            if (retry_count > 0) {
                LOG_RETRY("attempt %u for %s — wait %lums",
                          (unsigned)retry_count, filename,
                          (unsigned long)delay_ms);
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
            /* do_upload 已负责删除文件，这里只打印日志 */
            LOG_ARCHIVE("uploaded: %s", filename);
        } else {
            /* 全部重试失败：保留文件，等待下一轮扫描 */
            ESP_LOGW(TAG, "[QUEUE] %s failed all %u retries — will retry next scan cycle",
                     filename, (unsigned)RETRY_MAX);
        }
    }
}

/*======================================================================
 * Public API
 *======================================================================*/

esp_err_t uploader_init(const uploader_config_t *config)
{
    if (config != NULL) {
        /* 不再使用 s_config，URL 硬编码在 UPLOAD_URL */
        (void)config;
    }

    ESP_LOGI(TAG, "Uploader initialized");
    ESP_LOGI(TAG, "  URL: " UPLOAD_URL);
    ESP_LOGI(TAG, "  Retry delays: %lu/%lu/%lu ms",
             (unsigned long)RETRY_DELAYS_MS[0],
             (unsigned long)RETRY_DELAYS_MS[1],
             (unsigned long)RETRY_DELAYS_MS[2]);

    /* 注册 WiFi 状态回调 */
    esp_err_t ret = wifi_manager_register_callback(on_wifi_status, NULL);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "wifi_manager_register_callback failed: %s",
                     esp_err_to_name(ret));
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

esp_err_t uploader_upload(const char *file_path,
                          char *out_response, size_t response_size)
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
    /* 上传成功后直接 unlink，此函数仅作兼容 */
    (void)file_path;
    return ESP_OK;
}

bool uploader_is_uploading(void)
{
    return (s_task_handle != NULL) && !s_paused && s_wifi_ok;
}
