/**
 * @file system_monitor.h
 * @brief System Monitor - 运行时任务栈/堆监控
 *
 * 功能：
 * - 定期打印所有 FreeRTOS 任务的 stack watermark
 * - 打印 heap 使用情况
 * - 帮助发现栈溢出和内存泄漏
 *
 * 用法：
 *   1. app_main 中调用 system_monitor_init()
 *   2. monitor task 自动周期运行
 */

#ifndef SYSTEM_MONITOR_H
#define SYSTEM_MONITOR_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化系统监控（创建 monitor task）
 * @param interval_ms  监控打印间隔（毫秒），推荐 5000~30000
 * @return ESP_OK 成功
 */
esp_err_t system_monitor_init(uint32_t interval_ms);

/**
 * @brief 立即打印一次所有任务状态（手动触发）
 */
void system_monitor_dump(void);

/**
 * @brief 获取全局最小剩余 heap（自系统启动以来）
 * @return bytes
 */
size_t system_monitor_get_min_heap(void);

#ifdef __cplusplus
}
#endif

#endif /* SYSTEM_MONITOR_H */
