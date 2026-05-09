/**
 * @file battery.h
 * @brief 电池监测模块 - 头文件
 *
 * 功能：
 * - ADC 读取电池电压
 * - 电量百分比估算
 * - 低电量报警
 * - 充电状态检测
 */

#ifndef BATTERY_H
#define BATTERY_H

#include "esp_err.h"
#include <stdbool.h>

/**
 * @brief 电池配置结构体
 */
typedef struct {
    int adc_channel;       /*!< ADC 通道 */
    int adc_atten;         /*!< 衰减配置 */
    float voltage_divider;   /*!< 分压比（电池 / ADC）*/
    float full_voltage;      /*!< 满电电压（如 4. 2V）*/
    float empty_voltage;     /*!< 放电信压（如 3. 3V）*/
} battery_config_t;

/**
 * @brief 初始化电池监测
 * @param config 电池配置
 * @return esp_err_t
 */
esp_err_t battery_init(const battery_config_t *config);

/**
 * @brief 读取当前电池电压
 * @param[out] out_voltage 电压值（伏特）
 * @return esp_err_t
 */
esp_err_t battery_get_voltage(float *out_voltage);

/**
 * @brief 读取电量百分比（0~100）
 * @return 电量百分比
 */
int battery_get_percentage(void);

/**
 * @brief 是否正在充电
 * @return true=充电中, false=未充电
 */
bool battery_is_charging(void);

/**
 * @brief 低电量检测
 * @param threshold_pct 阈值百分比（默认 10）
 * @return true=低电量
 */
bool battery_is_low(int threshold_pct);

/**
 * @brief 获取电池状态描述
 * @return 状态字符串（"Critical" / "Low" / "Medium" / "Good" / "Full"）
 */
const char* battery_get_status_string(void);

#endif // BATTERY_H
