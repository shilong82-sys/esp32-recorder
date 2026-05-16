/**
 * @file uploader.h
 * @brief WAV 文件上传模块 - 头文件
 *
 * 功能：
 * - 通过 WiFi HTTP POST 上传 WAV 文件到 Mac 服务端
 * - 支持断点续传（可选）
 * - 上传状态回调
 */

#ifndef UPLOADER_H
#define UPLOADER_H_

#include "esp_err.h"
#include "recorder.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief 上传配置结构体
 */
typedef struct {
    char upload_url[128];       /*!< 完整上传 URL（优先级最高） */
    char server_ip[32];         /*!< 服务端 IP 地址 */
    uint16_t server_port;       /*!< 服务端端口（默认 8000） */
    char upload_path[64];       /*!< 上传路径（默认 /upload） */
    uint32_t timeout_ms;        /*!< 超时时间（毫秒）*/
} uploader_config_t;

/**
 * @brief 初始化上传模块
 * @param config 上传配置
 * @return esp_err_t
 */
esp_err_t uploader_init(const uploader_config_t *config);

/**
 * @brief 启动独立上传队列任务
 *
 * 该任务独立运行，不阻塞 recorder。
 * 启动时扫描 upload_queue/ 目录恢复未上传文件。
 * @return esp_err_t
 */
esp_err_t uploader_start(void);

/**
 * @brief 检查是否正在上传
 * @return true=上传中, false=空闲
 */
bool uploader_is_uploading(void);

/**
 * @brief 设置上传 URL（写入 NVS 持久化）
 * @param url 新的上传 URL（最长 127 字节）
 * @return ESP_OK=成功，ESP_FAIL=失败
 */
esp_err_t uploader_set_url(const char *url);

/**
 * @brief 获取当前上传 URL
 * @return URL 字符串指针（内部静态缓冲区，不需要释放）
 */
const char* uploader_get_url(void);

#endif // UPLOADER_H
