#ifndef LOGGER_H
#define LOGGER_H

#include <stdio.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

// 日志级别
typedef enum {
    LOG_LEVEL_ERROR = 0,
    LOG_LEVEL_WARN  = 1,
    LOG_LEVEL_INFO  = 2,
    LOG_LEVEL_DEBUG = 3,
} log_level_t;

// 输出目标
typedef enum {
    LOG_OUTPUT_CONSOLE = (1 << 0),
    LOG_OUTPUT_SD_CARD = (1 << 1),
    LOG_OUTPUT_NETWORK = (1 << 2),
} log_output_t;

// 初始化日志系统
void logger_init(log_level_t min_level, uint8_t outputs);

// 设置日志级别
void logger_set_level(log_level_t level);

// 设置输出目标
void logger_set_outputs(uint8_t outputs);

// 主日志函数
void logger_write(log_level_t level, const char *tag, const char *fmt, ...);

// 快捷宏
#define LOGE(tag, fmt, ...) logger_write(LOG_LEVEL_ERROR, tag, fmt, ##__VA_ARGS__)
#define LOGW(tag, fmt, ...) logger_write(LOG_LEVEL_WARN,  tag, fmt, ##__VA_ARGS__)
#define LOGI(tag, fmt, ...) logger_write(LOG_LEVEL_INFO,  tag, fmt, ##__VA_ARGS__)
#define LOGD(tag, fmt, ...) logger_write(LOG_LEVEL_DEBUG, tag, fmt, ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif // LOGGER_H
