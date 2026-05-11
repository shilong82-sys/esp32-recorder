/**
 * @file audio.h
 * @brief Audio component - Minimal I2S microphone interface
 *
 * 用于验证 INMP441 I2S 麦克风采集正常。
 * 不涉及 SD 卡、录制、上传、VAD 等功能。
 *
 * 硬件连接：
 *   BCLK (SCK) -> GPIO4
 *   WS  (LRCLK) -> GPIO5
 *   SD  (DOUT)  -> GPIO6
 *   L/R         -> GND
 *   VDD         -> 3V3
 *   GND         -> GND
 *
 * 采样配置：16kHz, Mono, 16-bit, RX only
 */

#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

/**
 * @brief 初始化 I2S 外设和 INMP441
 *
 * 使用 ESP-IDF v5 新版 I2S STD API：
 *   i2s_new_channel() + i2s_channel_init_std_mode()
 *
 * DMA 配置：
 *   - DMA buffer 数量：3
 *   - DMA buffer 大小：512 字节（256 samples × 2 channels × 16bit / 8）
 *
 * @return ESP_OK 成功，其他失败
 */
esp_err_t audio_init(void);

/**
 * @brief 从 I2S DMA buffer 读取 PCM 样本
 *
 * 读取的是 I2S 左对齐格式（INMP441 输出格式）。
 * 返回的 buffer 中仅包含左声道数据（mono）。
 *
 * @param buffer  输出 buffer（ caller 分配，16-bit × samples）
 * @param samples 要读取的样本数
 * @return        实际读取的样本数（可能 < samples，当 DMA buffer 不足时）
 *                0 表示无数据，< 0 表示错误
 */
int audio_read(int16_t *buffer, size_t samples);

/**
 * @brief 计算 PCM 数据的 RMS 电平
 *
 * @param buffer PCM 数据（16-bit signed）
 * @param samples 样本数
 * @return RMS 值（0.0 ~ 32767.0）
 */
float audio_calculate_rms(const int16_t *buffer, size_t samples);
