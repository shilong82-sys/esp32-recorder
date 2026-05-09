/**
 * @file test_recorder.c
 * @brief 在 Mac 上测试 WAV 生成逻辑（不依赖 ESP-IDF）
 *
 * 编译：
 *   gcc test_recorder.c -o test_recorder -lm
 *
 * 运行：
 *   ./test_recorder
 *   生成 test_output.wav，用音频播放器打开验证
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#define WAV_HEADER_SIZE 44
#define SAMPLE_RATE 16000
#define BITS_PER_SAMPLE 16
#define STUB_RECORD_SECONDS 3
#define STUB_TONE_FREQ 440   // 440 Hz 正弦波

static void wav_header_init(uint8_t *header, uint32_t sample_rate,
                           uint16_t bits_per_sample, uint16_t channels, uint32_t data_size)
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
    header[16] = 16;
    header[17] = 0;
    header[18] = 0;
    header[19] = 0;
    header[20] = 1;   // PCM
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

int main(void)
{
    const char *filename = "test_output.wav";
    uint32_t num_samples = SAMPLE_RATE * STUB_RECORD_SECONDS;
    uint32_t data_size = num_samples * (BITS_PER_SAMPLE / 8);
    uint8_t header[WAV_HEADER_SIZE];

    wav_header_init(header, SAMPLE_RATE, BITS_PER_SAMPLE, 1, data_size);

    FILE *f = fopen(filename, "wb");
    if (!f) {
        printf("❌ 无法创建文件: %s\n", filename);
        return 1;
    }

    // 写 WAV 头
    fwrite(header, 1, WAV_HEADER_SIZE, f);

    // 生成 440Hz 正弦波
    for (uint32_t i = 0; i < num_samples; i++) {
        float t = (float)i / SAMPLE_RATE;
        float sample = 0.5f * sinf(2.0f * M_PI * STUB_TONE_FREQ * t);
        int16_t pcm = (int16_t)(sample * 32767);
        fwrite(&pcm, sizeof(int16_t), 1, f);
    }

    fclose(f);

    printf("✅ 测试 WAV 文件已生成: %s\n", filename);
    printf("   采样率: %d Hz\n", SAMPLE_RATE);
    printf("   时长: %d 秒\n", STUB_RECORD_SECONDS);
    printf("   频率: %d Hz (正弦波)\n", STUB_TONE_FREQ);
    printf("   文件大小: %lu bytes\n", WAV_HEADER_SIZE + data_size);
    printf("\n👉 用音频播放器打开 %s 应该能听到 440Hz  tone\n", filename);

    return 0;
}
