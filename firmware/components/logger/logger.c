#include "logger.h"
#include <stdarg.h>
#include <time.h>
#include "esp_timer.h"

static log_level_t s_min_level = LOG_LEVEL_INFO;
static uint8_t s_outputs   = LOG_OUTPUT_CONSOLE;

static const char *LEVEL_TAG[] = {
    "ERROR",
    "WARN ",
    "INFO ",
    "DEBUG",
};

void logger_init(log_level_t min_level, uint8_t outputs)
{
    s_min_level = min_level;
    s_outputs   = outputs;
    printf("[LOGGER] 初始化完成，最低级别=%s，输出目标=0x%02X\n",
           LEVEL_TAG[min_level], outputs);
}

void logger_set_level(log_level_t level)
{
    s_min_level = level;
}

void logger_set_outputs(uint8_t outputs)
{
    s_outputs = outputs;
}

void logger_write(log_level_t level, const char *tag, const char *fmt, ...)
{
    if (level > s_min_level) {
        return;
    }

    // 获取时间戳（毫秒）
    int64_t now_ms = esp_timer_get_time() / 1000;
    uint32_t ms  = now_ms % 1000;
    uint32_t sec  = (now_ms / 1000) % 60;
    uint32_t min  = (now_ms / 60000) % 60;
    uint32_t hour = (now_ms / 3600000) % 24;

    char time_str[32];
    snprintf(time_str, sizeof(time_str), "%02lu:%02lu:%02lu.%03lu",
             (unsigned long)hour, (unsigned long)min,
             (unsigned long)sec,  (unsigned long)ms);

    // 格式化消息
    char msg[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    // 输出到控制台
    if (s_outputs & LOG_OUTPUT_CONSOLE) {
        printf("[%s] [%s] %s\n", time_str, tag, msg);
    }

    // 输出到 TF 卡（stub，硬件到货后实现）
    if (s_outputs & LOG_OUTPUT_SD_CARD) {
        // TODO: 写入 /sdcard/logs/xxxx-xx-xx.log
    }

    // 输出到网络（stub，硬件到货后实现）
    if (s_outputs & LOG_OUTPUT_NETWORK) {
        // TODO: 通过 HTTP POST 发送到 Mac 服务器 /log 接口
    }
}
