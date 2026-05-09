/**
 * @file recorder.h
 * @brief I2S 麦克风录音模块 - 头文件
 *
 * 功能：
 * - I2S 麦克风驱动（INMP441 / SPH0645 等）
 * - WAV 文件生成（写入 TF 卡）
 * - 录音启停控制
 * - 音频参数配置（采样率、位深、声道）
 */

#ifndef RECORDER_H
#define RECORDER_H

#include "esp_err.h"
#include "driver/i2s_std.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/**
 * @brief 录音参数配置结构体
 */
typedef struct {
    i2s_port_t i2s_port;       /*!< I2S 端口号 */
    uint32_t sample_rate;          /*!< 采样率（16000 / 44100 / 48000）*/
    uint8_t bits_per_sample;       /*!< 位深（16 / 24 / 32）*/
    uint8_t channel_format;        /*!< 声道格式 */
    char file_prefix[32];         /*!< 文件名前缀 */
} recorder_config_t;

/**
 * @brief 初始化录音模块
 * @param config 录音参数配置
 * @return esp_err_t
 */
esp_err_t recorder_init(const recorder_config_t *config);

/**
 * @brief 开始录音
 * @param filename 输出 WAV 文件路径（NULL = 自动生成）
 * @return esp_err_t
 */
esp_err_t recorder_start(const char *filename);

/**
 * @brief 停止录音
 * @param[out] out_duration_ms 录音时长（毫秒）
 * @return esp_err_t
 */
esp_err_t recorder_stop(uint32_t *out_duration_ms);

/**
 * @brief 获取当前录音状态
 * @return true=录音中, false=空闲
 */
bool recorder_is_recording(void);

/**
 * @brief 获取录音文件列表（TF 卡）
 * @param[out] file_list 文件列表缓冲区
 * @param max_files 最大文件数
 * @return 实际文件数
 */
int recorder_list_files(char file_list[][64], int max_files);

#endif // RECORDER_H
