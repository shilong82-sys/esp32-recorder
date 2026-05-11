/**
 * @file battery.c
 * @brief 电池监测模块 - 源文件
 *
 * 功能：
 * - ADC 读取电池电压
 * - 电量百分比估算
 * - 低电量报警
 * - 充电状态检测
 */

#include "battery.h"
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include <math.h>
#include <string.h>

static const char *TAG = "battery";

// 默认配置（适用于 3.7V 锂电池）
#define DEFAULT_ADC_CHANNEL    ADC_CHANNEL_0  // GPIO 1
#define DEFAULT_ADC_ATTEN     ADC_ATTEN_DB_12
#define DEFAULT_VOLTAGE_DIV   2.0f          // 分压比
#define DEFAULT_FULL_VOLTAGE  4.20f         // 锂电池满电
#define DEFAULT_EMPTY_VOLTAGE 3.30f         // 锂电池低压保护

// 全局状态
static bool s_initialized = false;
static adc_oneshot_unit_handle_t s_adc_handle = NULL;
static adc_cali_handle_t s_calib_handle = NULL;
static bool s_calibration_success = false;

// 电池配置
static battery_config_t s_config = {
    .adc_channel = DEFAULT_ADC_CHANNEL,
    .adc_atten = DEFAULT_ADC_ATTEN,
    .voltage_divider = DEFAULT_VOLTAGE_DIV,
    .full_voltage = DEFAULT_FULL_VOLTAGE,
    .empty_voltage = DEFAULT_EMPTY_VOLTAGE,
};

/**
 * @brief 初始化电池监测
 */
esp_err_t battery_init(const battery_config_t *config)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Battery monitor already initialized");
        return ESP_OK;
    }

    if (config != NULL) {
        memcpy(&s_config, config, sizeof(battery_config_t));
    }

    ESP_LOGI(TAG, "Initializing battery monitor...");

    // ADC 单元配置
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = ADC_UNIT_1,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &s_adc_handle));

    // ADC 通道配置
    adc_oneshot_chan_cfg_t chan_config = {
        .atten = s_config.adc_atten,
        .bitwidth = ADC_BITWIDTH_12,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc_handle, s_config.adc_channel, &chan_config));

    // 校准初始化（尝试校准）
    adc_cali_curve_fitting_config_t calib_config = {
        .unit_id = ADC_UNIT_1,
        .chan = s_config.adc_channel,
        .atten = s_config.adc_atten,
        .bitwidth = ADC_BITWIDTH_12,
    };

    if (adc_cali_create_scheme_curve_fitting(&calib_config, &s_calib_handle) == ESP_OK) {
        s_calibration_success = true;
        ESP_LOGI(TAG, "ADC calibration enabled");
    } else {
        ESP_LOGW(TAG, "ADC calibration not available, using raw ADC values");
        s_calibration_success = false;
    }

    s_initialized = true;

    ESP_LOGI(TAG, "Battery monitor initialized");
    ESP_LOGI(TAG, "  ADC channel: %d", s_config.adc_channel);
    ESP_LOGI(TAG, "  Voltage divider: %.2f", s_config.voltage_divider);
    ESP_LOGI(TAG, "  Full voltage: %.2f V", s_config.full_voltage);
    ESP_LOGI(TAG, "  Empty voltage: %.2f V", s_config.empty_voltage);

    return ESP_OK;
}

/**
 * @brief 读取 ADC 原始值
 */
static int read_raw_adc(void)
{
    int raw = 0;
    int adc_reading = 0;

    // 多次采样取平均
    for (int i = 0; i < 64; i++) {
        ESP_ERROR_CHECK(adc_oneshot_read(s_adc_handle, s_config.adc_channel, &raw));
        adc_reading += raw;
    }
    adc_reading /= 64;

    return adc_reading;
}

/**
 * @brief 读取电池电压
 */
esp_err_t battery_get_voltage(float *out_voltage)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Battery monitor not initialized");
        return ESP_FAIL;
    }

    int adc_raw = read_raw_adc();
    int adc_voltage_mv = 0;

    if (s_calibration_success) {
        // 使用校准数据
        ESP_ERROR_CHECK(adc_cali_raw_to_voltage(s_calib_handle, adc_raw, &adc_voltage_mv));
    } else {
        // 估算：12-bit ADC，参考电压 1100mV
        adc_voltage_mv = adc_raw * 1100 / 4096;
    }

    // 应用分压比
    float voltage = (float)adc_voltage_mv / 1000.0f * s_config.voltage_divider;

    if (out_voltage != NULL) {
        *out_voltage = voltage;
    }

    ESP_LOGD(TAG, "ADC raw: %d, voltage: %d mV, battery: %.3f V", adc_raw, adc_voltage_mv, voltage);

    return ESP_OK;
}

/**
 * @brief 读取电量百分比
 */
int battery_get_percentage(void)
{
    float voltage = 0;
    if (battery_get_voltage(&voltage) != ESP_OK) {
        return -1;
    }

    // 线性插值计算电量
    float range = s_config.full_voltage - s_config.empty_voltage;
    float percentage = (voltage - s_config.empty_voltage) / range * 100.0f;

    // 限制范围
    if (percentage > 100.0f) percentage = 100.0f;
    if (percentage < 0.0f) percentage = 0.0f;

    return (int)percentage;
}

/**
 * @brief 检测是否正在充电
 *
 * 注意：需要根据实际硬件连接来实现
 * 通常通过 CHG_DET 引脚读取
 */
bool battery_is_charging(void)
{
    // TODO: 实现充电检测
    // 需要连接到充电 IC 的充电状态引脚
    // 例如 TP4056 的 CHRG 引脚

    return false;
}

/**
 * @brief 低电量检测
 */
bool battery_is_low(int threshold_pct)
{
    if (threshold_pct <= 0) {
        threshold_pct = 10;  // 默认 10%
    }

    int percentage = battery_get_percentage();
    if (percentage < 0) {
        return false;  // 读取失败时假设正常
    }

    return (percentage <= threshold_pct);
}

/**
 * @brief 获取电池健康状态描述
 */
const char* battery_get_status_string(void)
{
    int percentage = battery_get_percentage();

    if (percentage < 0) {
        return "Unknown";
    } else if (percentage < 10) {
        return "Critical";
    } else if (percentage < 20) {
        return "Low";
    } else if (percentage < 50) {
        return "Medium";
    } else if (percentage < 80) {
        return "Good";
    } else {
        return "Full";
    }
}
