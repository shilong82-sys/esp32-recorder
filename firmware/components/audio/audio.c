/**
 * @file audio.c
 * @brief Audio component - I2S microphone reader (INMP441)
 *
 * 使用 ESP-IDF v5 I2S STD API (i2s_new_channel + i2s_channel_init_std_mode)
 *
 * INMP441 通信格式：I2S Philips 标准（1 位延迟）
 *   WS LOW  → 左声道数据（INMP441 mono 输出）
 *   WS HIGH → 右声道数据（未使用）
 *   MSB 在 WS 变换后的第 2 个 BCLK 周期开始（1 位延迟）
 *   ESP-IDF v5 中需设置 bit_shift=true 处理此延迟
 *
 * GPIO 配置：
 *   BCLK (SCK) -> GPIO4
 *   WS  (LRCLK) -> GPIO5
 *   SD  (DOUT)  -> GPIO6
 *
 * 采样配置：16kHz, Mono, 16-bit, RX only
 */

#include "audio.h"
#include "esp_log.h"
#include "driver/i2s_std.h"
#include "driver/i2s_common.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>
#include <string.h>

static const char *TAG = "audio";

/*======================================================================
 * 常量定义
 *======================================================================*/
#define I2S_PORT             (I2S_NUM_AUTO)    /* 自动分配 I2S 端口 */
#define I2S_ROLE             (I2S_ROLE_MASTER) /* ESP32-S3 作为主设备 */
#define SAMPLE_RATE_HZ       (16000)           /* INMP441 采样率 */
#define I2S_BIT_WIDTH        (I2S_DATA_BIT_WIDTH_16BIT)
#define I2S_SLOT_MODE        (I2S_SLOT_MODE_MONO)

/* GPIO */
#define GPIO_BCLK            (GPIO_NUM_4)
#define GPIO_WS              (GPIO_NUM_5)
#define GPIO_DIN             (GPIO_NUM_6)

/* DMA 配置
 * - dma_desc_num: DMA 描述符数量（缓冲区数量）
 * - dma_frame_num: 每个 DMA 描述符承载的 I2S 帧数（每次中断的样本数）
 *
 * 计算：
 *   16kHz mono = 16k samples/s = 16k × 2 (32-bit slot) = 32KB/s
 *   每个 DMA frame = 1 sample × 4 bytes = 4 bytes (I2S 32-bit bus)
 *   256 frames × 4 bytes = 1024 bytes/descriptor
 *   6 descriptor × 1024 bytes = 6144 bytes total
 *   延迟：6 × 256 frames / 16000 Hz = 96ms (足够覆盖 100ms 读取周期)
 */
#define DMA_DESC_NUM          (6)
#define DMA_FRAME_NUM         (256)

/* I2S 通道句柄 */
static i2s_chan_handle_t s_rx_handle = NULL;

/* PCM 输出 buffer（每帧 4 字节 I2S 总线数据） */
static uint8_t s_i2s_buf[DMA_DESC_NUM * DMA_FRAME_NUM * sizeof(uint32_t)]
    __attribute__((aligned(4)));

/*======================================================================
 * I2S 总线数据 → Mono PCM 转换
 *
 * I2S STD MSB 模式总线格式（32-bit word）：
 *   [31:16] = Slot 0 data (左声道，INMP441 mono 数据)
 *   [15:0]  = Slot 1 data (右声道，丢弃)
 *
 * 我们的 DMA buffer 格式：每个 uint32_t = 1 个 I2S 帧
 *======================================================================*/
static inline int16_t extract_mono_sample(uint32_t i2s_word)
{
    /* 高 16 位 = 左声道（INMP441 mono） */
    return (int16_t)(i2s_word >> 16);
}

/*======================================================================
 * audio_init
 *======================================================================*/
esp_err_t audio_init(void)
{
    /* Step 1: 分配 I2S 通道（TX=NULL, RX=句柄，simplex RX 模式） */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT, I2S_ROLE);
    chan_cfg.dma_desc_num = DMA_DESC_NUM;
    chan_cfg.dma_frame_num = DMA_FRAME_NUM;
    chan_cfg.auto_clear   = true;  /* 每次读取后自动清零 DMA TX 空数据 */

    esp_err_t ret = i2s_new_channel(&chan_cfg, NULL, &s_rx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Step 2: 配置 STD 模式
     * INMP441 = I2S Philips 标准格式（1 位延迟）
     *   ws_pol = false  → WS LOW 时接收左声道（INMP441 mono 输出到此）
     *   ws_width = 16   → WS 高电平持续 16 个 BCLK 周期
     *   bit_shift = true  → 处理 I2S Philips 的 1 位延迟
     *   left_align = false → 非左对齐（I2S Philips 格式）
     *   slot_mask = I2S_STD_SLOT_LEFT → 仅接收左声道（mono from INMP441）
     */
    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE_HZ),
        .slot_cfg = {
            .data_bit_width = I2S_BIT_WIDTH,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode      = I2S_SLOT_MODE,
            .slot_mask      = I2S_STD_SLOT_LEFT,  /* 仅接收左声道（mono from INMP441） */
            .ws_width       = 16,                 /* WS 脉冲宽度 = 16 BCLK cycles (1 声道) */
            .ws_pol         = false,               /* WS LOW = 左声道，WS HIGH = 右声道 */
            .bit_shift      = true,               /* ✅ I2S Philips 格式（INMP441 有 1 位延迟）*/
            .left_align     = false,              /* 非左对齐（I2S Philips 格式）*/
            .big_endian     = false,              /* 小端序（ESP32 为小端） */
            .bit_order_lsb  = false,              /* MSB first */
        },
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,    /* INMP441 不需要 MCLK */
            .bclk = GPIO_BCLK,
            .ws   = GPIO_WS,
            .dout = I2S_GPIO_UNUSED,     /* RX only */
            .din  = GPIO_DIN,
            .invert_flags = {
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    ret = i2s_channel_init_std_mode(s_rx_handle, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode failed: %s", esp_err_to_name(ret));
        i2s_del_channel(s_rx_handle);
        s_rx_handle = NULL;
        return ret;
    }

    /* Step 3: 启动 RX */
    ret = i2s_channel_enable(s_rx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable failed: %s", esp_err_to_name(ret));
        i2s_channel_disable(s_rx_handle);
        i2s_del_channel(s_rx_handle);
        s_rx_handle = NULL;
        return ret;
    }

    /* 打印配置信息 */
    ESP_LOGI(TAG, "=== Audio I2S Configuration ===");
    ESP_LOGI(TAG, "  Model:        INMP441 (I2S MSB/Left-justified)");
    ESP_LOGI(TAG, "  Sample Rate:  %d Hz", SAMPLE_RATE_HZ);
    ESP_LOGI(TAG, "  Bit Width:    16-bit");
    ESP_LOGI(TAG, "  Mode:         Mono (left channel only)");
    ESP_LOGI(TAG, "  BCLK GPIO:   GPIO%d", GPIO_BCLK);
    ESP_LOGI(TAG, "  WS GPIO:     GPIO%d", GPIO_WS);
    ESP_LOGI(TAG, "  DIN GPIO:    GPIO%d", GPIO_DIN);
    ESP_LOGI(TAG, "  DMA Desc:    %d × %d frames = %d bytes total",
             DMA_DESC_NUM, DMA_FRAME_NUM,
             DMA_DESC_NUM * DMA_FRAME_NUM * 4);
    ESP_LOGI(TAG, "  DMA Latency: ~%d ms",
             (DMA_DESC_NUM * DMA_FRAME_NUM) * 1000 / SAMPLE_RATE_HZ);
    ESP_LOGI(TAG, "  Mode:         ESP-IDF v5 I2S STD API (i2s_new_channel)");
    ESP_LOGI(TAG, "==============================");
    ESP_LOGI(TAG, "Audio init OK");

    return ESP_OK;
}

/*======================================================================
 * audio_read
 *
 * 从 I2S DMA 缓冲区读取 PCM 数据。
 * 返回已转换为 16-bit mono PCM 的样本数。
 *======================================================================*/
int audio_read(int16_t *buffer, size_t samples)
{
    if (s_rx_handle == NULL) {
        ESP_LOGE(TAG, "audio_read called before audio_init()");
        return -1;
    }
    if (buffer == NULL || samples == 0) {
        return 0;
    }

    /* 读取原始 I2S 数据（每个 uint32_t = 1 个 I2S 帧 = 1 mono sample） */
    size_t bytes_read = 0;
    size_t buf_size    = samples * sizeof(uint32_t);

    /* 确保不超过 static buffer 大小 */
    if (buf_size > sizeof(s_i2s_buf)) {
        buf_size = sizeof(s_i2s_buf);
    }

    /* 增加超时到 100ms，确保 DMA 缓冲区有足够时间填充 */
    esp_err_t ret = i2s_channel_read(s_rx_handle,
                                     s_i2s_buf,
                                     buf_size,
                                     &bytes_read,
                                     100);  /* 100ms 超时，确保 DMA 填充完成 */

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "i2s_channel_read error: %s, bytes_read=%d", esp_err_to_name(ret), (int)bytes_read);
        return 0;
    }

    if (bytes_read == 0) {
        ESP_LOGW(TAG, "No data received (timeout), try checking INMP441 wiring");
        return 0;
    }

    ESP_LOGD(TAG, "Read %d bytes (%d I2S words)", (int)bytes_read, (int)(bytes_read / 4));

    /* 将 I2S 32-bit 总线数据解复用为 16-bit mono PCM */
    uint32_t *i2s_words = (uint32_t *)s_i2s_buf;
    size_t num_words    = bytes_read / sizeof(uint32_t);
    size_t copied       = 0;

    for (size_t i = 0; i < num_words && copied < samples; i++) {
        buffer[copied++] = extract_mono_sample(i2s_words[i]);
    }

    return (int)copied;
}

/*======================================================================
 * audio_calculate_rms
 *======================================================================*/
float audio_calculate_rms(const int16_t *buffer, size_t samples)
{
    if (buffer == NULL || samples == 0) {
        return 0.0f;
    }

    double sum = 0.0;
    for (size_t i = 0; i < samples; i++) {
        double v = (double)buffer[i];
        sum += v * v;
    }
    return (float)sqrt(sum / (double)samples);
}
