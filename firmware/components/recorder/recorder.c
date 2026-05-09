/**
 * @file recorder.c
 * @brief I2S 麦克风录音模块 - 源文件
 *
 * 功能：
 * - I2S 麦克风驱动（INMP441 / SPH0645 等）
 * - WAV 文件生成（写入 TF 卡）
 * - 录音启停控制
 *
 * 注意：硬件未到，I2S 部分暂为 stub，
 *       但会生成带测试音频的 WAV 文件用于功能验证
 */

#include "recorder.h"
#include "storage.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include <time.h>
#include <math.h>

static const char *TAG = "recorder";

#define DEFAULT_SAMPLE_RATE 16000
#define DEFAULT_BITS_PER_SAMPLE 16
#define WAV_HEADER_SIZE 44

// 测试音频配置（stub 模式）
#define STUB_RECORD_SECONDS 3       // stub 模式生成 3 秒音频
#define STUB_TONE_FREQ 440          // 440 Hz 正弦波（A4 音符）

// 录音状态
static bool s_recording = false;
static bool s_initialized = false;

// 录音配置
static recorder_config_t s_config = {
    .i2s_port = I2S_NUM_0,
    .sample_rate = DEFAULT_SAMPLE_RATE,
    .bits_per_sample = DEFAULT_BITS_PER_SAMPLE,
    .channel_format = 0,
    .file_prefix = "REC"
};

// 文件信息
static char s_current_file[128] = {0};
static uint32_t s_recording_start_time = 0;

/**
 * @brief 生成 WAV 文件头
 */
static void wav_header_init(uint8_t *header, uint32_t sample_rate, uint16_t bits_per_sample, uint16_t channels, uint32_t data_size)
{
    uint32_t file_size = data_size + 36;
    uint32_t byte_rate = sample_rate * channels * bits_per_sample / 8;
    uint16_t block_align = channels * bits_per_sample / 8;

    memcpy(header, "RIFF", 4);
    header[4]  = (uint8_t)(file_size & 0xFF);
    header[5]  = (uint8_t)((file_size >> 8) & 0xFF);
    header[6]  = (uint8_t)((file_size >> 16) & 0xFF);
    header[7]  = (uint8_t)((file_size >> 24) & 0xFF);
    memcpy(header + 8, "WAVE", 4);

    memcpy(header + 12, "fmt ", 4);
    header[16] = 16;  // fmt chunk size
    header[17] = 0;
    header[18] = 0;
    header[19] = 0;
    header[20] = 1;   // PCM format
    header[21] = 0;
    header[22] = (uint8_t)(channels & 0xFF);
    header[23] = 0;
    header[24] = (uint8_t)(sample_rate & 0xFF);
    header[25] = (uint8_t)((sample_rate >> 8) & 0xFF);
    header[26] = (uint8_t)((sample_rate >> 16) & 0xFF);
    header[27] = (uint8_t)((sample_rate >> 24) & 0xFF);
    header[28] = (uint8_t)(byte_rate & 0xFF);
    header[29] = (uint8_t)((byte_rate >> 8) & 0xFF);
    header[30] = (uint8_t)((byte_rate >> 16) & 0xFF);
    header[31] = (uint8_t)((byte_rate >> 24) & 0xFF);
    header[32] = (uint8_t)(block_align & 0xFF);
    header[33] = (uint8_t)((block_align >> 8) & 0xFF);
    header[34] = (uint8_t)(bits_per_sample & 0xFF);
    header[35] = (uint8_t)((bits_per_sample >> 8) & 0xFF);

    memcpy(header + 36, "data", 4);
    header[40] = (uint8_t)(data_size & 0xFF);
    header[41] = (uint8_t)((data_size >> 8) & 0xFF);
    header[42] = (uint8_t)((data_size >> 16) & 0xFF);
    header[43] = (uint8_t)((data_size >> 24) & 0xFF);
}

/**
 * @brief 生成文件名（带时间戳）
 */
static void generate_filename(char *buffer, size_t size, const char *prefix)
{
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    snprintf(buffer, size, "/sdcard/%s_%04d%02d%02d_%02d%02d%02d.wav",
             prefix ? prefix : "REC",
             tm_info->tm_year + 1900,
             tm_info->tm_mon + 1,
             tm_info->tm_mday,
             tm_info->tm_hour,
             tm_info->tm_min,
             tm_info->tm_sec);
}

/**
 * @brief 初始化录音模块
 */
esp_err_t recorder_init(const recorder_config_t *config)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Recorder already initialized");
        return ESP_OK;
    }

    if (config != NULL) {
        memcpy(&s_config, config, sizeof(recorder_config_t));
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Recorder initialized (stub, hardware not yet arrived)");
    ESP_LOGI(TAG, "  Sample rate: %lu Hz", s_config.sample_rate);
    ESP_LOGI(TAG, "  Bits per sample: %d", s_config.bits_per_sample);
    return ESP_OK;
}

/**
 * @brief 开始录音（stub，生成测试音频）
 */
esp_err_t recorder_start(const char *filename)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Recorder not initialized");
        return ESP_FAIL;
    }
    if (s_recording) {
        ESP_LOGW(TAG, "Already recording");
        return ESP_OK;
    }

    if (filename != NULL && strlen(filename) > 0) {
        snprintf(s_current_file, sizeof(s_current_file), "/sdcard/%s", filename);
    } else {
        generate_filename(s_current_file, sizeof(s_current_file), s_config.file_prefix);
    }

    ESP_LOGI(TAG, "Start recording to: %s", s_current_file);
    ESP_LOGW(TAG, "NOTE: generating TEST audio (stub, hardware not yet arrived)");

    // 生成测试音频（正弦波 WAV 文件）
    uint32_t num_samples = s_config.sample_rate * STUB_RECORD_SECONDS;
    uint32_t data_size = num_samples * (s_config.bits_per_sample / 8);
    uint8_t header[WAV_HEADER_SIZE];

    wav_header_init(header, s_config.sample_rate, s_config.bits_per_sample, 1, data_size);

    FILE *f = fopen(s_current_file, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to create file: %s", s_current_file);
        return ESP_FAIL;
    }

    // 写 WAV 头
    fwrite(header, 1, WAV_HEADER_SIZE, f);

    // 生成 440Hz 正弦波音频数据
    for (uint32_t i = 0; i < num_samples; i++) {
        float t = (float)i / s_config.sample_rate;
        float sample = 0.5f * sinf(2.0f * M_PI * STUB_TONE_FREQ * t);
        int16_t pcm = (int16_t)(sample * 32767);
        fwrite(&pcm, sizeof(int16_t), 1, f);
    }

    fclose(f);

    s_recording = true;
    s_recording_start_time = esp_timer_get_time() / 1000;

    ESP_LOGI(TAG, "Test WAV generated: %lu bytes audio (%.1f sec)",
             data_size, (float)STUB_RECORD_SECONDS);
    return ESP_OK;
}

/**
 * @brief 停止录音（stub，更新 WAV 文件头）
 */
esp_err_t recorder_stop(uint32_t *out_duration_ms)
{
    if (!s_recording) {
        ESP_LOGW(TAG, "Not recording");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Stopping recording...");
    s_recording = false;

    uint32_t duration_ms = (esp_timer_get_time() / 1000) - s_recording_start_time;
    if (out_duration_ms) *out_duration_ms = duration_ms;

    // stub 模式下文件已在 recorder_start() 中生成完毕
    // 这里只需日志输出
    ESP_LOGI(TAG, "Recording stopped (stub). Duration: %lu ms", duration_ms);
    ESP_LOGI(TAG, "File saved: %s", s_current_file);

    return ESP_OK;
}

/**
 * @brief 获取当前录音状态
 */
bool recorder_is_recording(void)
{
    return s_recording;
}

/**
 * @brief 获取录音文件列表
 */
int recorder_list_files(char file_list[][64], int max_files)
{
    return storage_list_wav_files("/sdcard", file_list, max_files);
}
