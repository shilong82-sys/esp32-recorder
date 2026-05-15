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
    char server_ip[32];     /*!< 服务端 IP 地址 */
    uint16_t server_port;       /*!< 服务端端口（默认 800） */
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
 * @brief 上传 WAV 文件
 * @param file_path WAV 文件路径（TF 卡上）
 * @param[out] out_response 服务端响应缓冲区（可选填 NULL）
 * @param response_size 响应缓冲区大小
 * @return esp_err_t
 */
esp_err_t uploader_upload(const char *file_path, char *out_response, size_t response_size);

/**
 * @brief 获取上传进度（0~100）
 * @return 进度百分比
 */
int uploader_get_progress(void);

/**
 * @brief 上传完成后删除本地文件
 * @param file_path 文件路径
 * @return esp_err_t
 */
esp_err_t uploader_delete_after_upload(const char *file_path);

/**
 * @brief 检查是否正在上传
 * @return true=上传中, false=空闲
 */
bool uploader_is_uploading(void);

#endif // UPLOADER_H
