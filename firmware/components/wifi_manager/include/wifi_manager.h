/**
 * @file wifi_manager.h
 * @brief WiFi 连接管理模块 - 头文件
 *
 * 功能：
 * - Station 模式连接
 * - 断线自动重连
 * - 配网支持（SmartConfig / BluFi）
 */

#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "esp_err.h"
#include <stdbool.h>

/**
 * @brief 初始化 WiFi Manager
 * @return esp_err_t
 */
esp_err_t wifi_manager_init(void);

/**
 * @brief 连接到指定 SSID/密码
 * @param ssid WiFi 名称
 * @param passwd WiFi 密码（可为 NULL 表示开放网络）
 * @return esp_err_t
 */
esp_err_t wifi_manager_connect(const char *ssid, const char *passwd);

/**
 * @brief 从 NVS 恢复上次的连接
 * @return esp_err_t ESP_OK 表示成功恢复，ESP_FAIL 表示无保存的凭证
 */
esp_err_t wifi_manager_restore_connection(void);

/**
 * @brief 断开当前连接
 */
void wifi_manager_disconnect(void);

/**
 * @brief 获取当前连接状态
 * @return true=已连接, false=未连接
 */
bool wifi_manager_is_connected(void);

#endif // WIFI_MANAGER_H
